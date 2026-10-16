#!/usr/bin/env python3
"""ACK 큐 시간·메모리 비교(#140 완료 조건 4와 5의 측정 쪽). 합성 데이터만 쓴다.

비교군(모두 같은 파이썬 프로세스에서 같은 호출 모양으로 돌린다):
  persist-queue   persist-queue 1.1.0 SQLiteAckQueue(Markchain 설정: multithreading, auto_resume, JSON).
                  기본 SQLite 보증은 journal_mode=WAL, synchronous=FULL(실제 값을 읽어 결과에 남긴다).
  sqlite-lease    이 스크립트의 표준 라이브러리 sqlite3 기준군. lease·fence·상태 index를 갖고 WAL + synchronous=FULL.
  lrn             lrn_ackq(이 이슈의 큐). COW 게시 commit(fsync 뒤 응답, DURABLE).
같은 payload(결정적 합성 JSON 객체), 같은 내구성 수준(FULL/DURABLE끼리 비교), 같은 스레드 수, 같은
보존 정책(ack된 작업을 끝에서 모두 지움)을 쓴다. 한 측정은 따로 띄운 프로세스에서 하고 `ru_maxrss`의 증가분을 메모리로
보고한다(이전 최고치를 넘은 만큼만 잡히므로 작은 작업은 0일 수 있다). 시간은 벽시계(perf_counter)다.

주의: lrn 큐는 모든 연산이 테이블 전체 스캔이라 행 수에 비례해 느려진다(SQL에 LIMIT·ORDER BY·index가
없다). 이 스크립트는
그 비용을 그대로 드러내며 성능 우위를 주장하지 않는다. 결과는 이 호스트·이 빌드·이 입력에서의 관측이다.

사용법:
  worker  python3 bench/ack_queue_compare.py worker --system lrn --n 200 --payload-bytes 256 --threads 1
  비교    python3 bench/ack_queue_compare.py compare --sizes 200,1000 --payloads 256,16384 --threads 1,4 --repeats 3
  대기열  python3 bench/ack_queue_compare.py pending --n 500 --payload-bytes 65536
필요한 것: LRN_ACKQ_LIB(공유 라이브러리), PYTHONPATH=python, persist-queue 1.1.0이 import되는 파이썬.
"""

# ruff: noqa: ANN401 - 비교군 객체(persist-queue·sqlite·lrn)는 서로 다른 타입이라 Any로 받는다

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import resource
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

SYSTEMS = ("persist-queue", "sqlite-lease", "lrn")
LEASE_MS = 600_000


def synthetic(index: int, payload_bytes: int) -> dict[str, Any]:
    """결정적 합성 작업. 압축이 잘 되지 않도록 sha256 연쇄의 16진 문자열로 본문을 채운다."""
    body, digest = [], hashlib.sha256(f"seed-{index}".encode()).hexdigest()
    while sum(map(len, body)) < payload_bytes:
        body.append(digest)
        digest = hashlib.sha256(digest.encode()).hexdigest()
    return {"key": f"k-{index}", "n": index, "body": "".join(body)[:payload_bytes]}


def rss_peak_bytes() -> int:
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return peak if sys.platform == "darwin" else peak * 1024


# ── 어댑터: put / claim / ack / pending_copy / pending_cursor / purge / close ──


class PersistQueue:
    name = "persist-queue"

    def __init__(self, directory: Path) -> None:
        import persistqueue  # noqa: PLC0415
        from persistqueue.serializers import json as serializer  # noqa: PLC0415

        self.queue = persistqueue.SQLiteAckQueue(
            str(directory / "queue"), multithreading=True, auto_resume=True, serializer=serializer
        )
        self.empty = persistqueue.exceptions.Empty
        self.version = getattr(persistqueue, "__version__", "?")
        self.synchronous = self.queue._putter.execute("PRAGMA synchronous").fetchone()[0]  # noqa: SLF001

    def put(self, item: dict[str, Any]) -> int:
        return int(self.queue.put(item))

    def claim(self) -> Any:
        try:
            return self.queue.get(block=False, raw=True)
        except self.empty:
            return None

    def ack(self, claim: Any) -> None:
        self.queue.ack(id=claim["pqid"])

    def pending_copy(self) -> tuple[float, float, int]:
        """(DB 읽기 초, 복호화·복사 초, 건수): 전체 pending을 payload째 복사하는 비용을 둘로 나눈다."""
        start = time.perf_counter()
        table = self.queue._table_name  # noqa: SLF001
        rows = self.queue._getter.execute(f"SELECT _id, data, timestamp, status FROM {table}").fetchall()  # noqa: SLF001
        read_s = time.perf_counter() - start
        start = time.perf_counter()
        copied = [json.loads(row[1]) for row in rows]
        return read_s, time.perf_counter() - start, len(copied)

    def pending_api_copy(self) -> int:
        return len(self.queue.queue())

    def purge(self, n: int) -> None:
        self.queue.clear_acked_data(max_delete=n, keep_latest=0)

    def close(self) -> None:
        self.queue.close()


class SqliteLease:
    name = "sqlite-lease"

    def __init__(self, directory: Path) -> None:
        self.path = str(directory / "lease.sqlite")
        self.local = threading.local()
        conn = self._conn()
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=FULL")
        conn.execute(
            "CREATE TABLE q (id INTEGER PRIMARY KEY AUTOINCREMENT, state INTEGER NOT NULL, ready_at INTEGER NOT NULL,"
            " lease_until INTEGER NOT NULL, fence INTEGER NOT NULL, attempts INTEGER NOT NULL, payload BLOB NOT NULL)"
        )
        conn.execute("CREATE INDEX q_state ON q (state, id)")
        self.synchronous = conn.execute("PRAGMA synchronous").fetchone()[0]

    def _conn(self) -> sqlite3.Connection:
        conn = getattr(self.local, "conn", None)
        if conn is None:
            conn = sqlite3.connect(self.path, timeout=60, isolation_level=None, check_same_thread=False)
            conn.execute("PRAGMA synchronous=FULL")
            self.local.conn = conn
        return conn

    def put(self, item: dict[str, Any]) -> int:
        conn = self._conn()
        cursor = conn.execute(
            "INSERT INTO q (state, ready_at, lease_until, fence, attempts, payload) VALUES (0, 0, 0, 0, 0, ?)",
            (json.dumps(item).encode(),),
        )
        return int(cursor.lastrowid or 0)

    def claim(self) -> Any:
        conn, now = self._conn(), int(time.time() * 1000)
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            "SELECT id, payload, fence FROM q WHERE (state = 0 AND ready_at <= ?1) OR (state = 1 AND lease_until <= ?1)"
            " ORDER BY id LIMIT 1",
            (now,),
        ).fetchone()
        if row is None:
            conn.execute("COMMIT")
            return None
        conn.execute(
            "UPDATE q SET state = 1, lease_until = ?, fence = fence + 1, attempts = attempts + 1 WHERE id = ?",
            (now + LEASE_MS, row[0]),
        )
        conn.execute("COMMIT")
        return (row[0], row[2] + 1, row[1])

    def ack(self, claim: Any) -> None:
        cursor = self._conn().execute(
            "UPDATE q SET state = 2 WHERE id = ? AND state = 1 AND fence = ?", (claim[0], claim[1])
        )
        if cursor.rowcount != 1:
            raise RuntimeError("stale ack")

    def pending_copy(self) -> tuple[float, float, int]:
        start = time.perf_counter()
        rows = self._conn().execute("SELECT id, payload FROM q").fetchall()
        read_s = time.perf_counter() - start
        start = time.perf_counter()
        copied = [json.loads(row[1]) for row in rows]
        return read_s, time.perf_counter() - start, len(copied)

    def pending_api_copy(self) -> int:
        return self.pending_copy()[2]

    def purge(self, n: int) -> None:
        self._conn().execute("DELETE FROM q WHERE state = 2")

    def close(self) -> None:
        self._conn().close()


class Lrn:
    def __init__(self, directory: Path) -> None:
        from lrn_ackq import AckQueue, Database  # noqa: PLC0415

        self.name = "lrn"
        self.db = Database(directory / "db.lrdb", busy_timeout_s=120.0)
        self.queue = AckQueue(self.db, "jobs")
        self.synchronous = "cow-fsync"

    def put(self, item: dict[str, Any]) -> int:
        return self.queue.put(json.dumps(item).encode()).id

    def claim(self) -> Any:
        return self.queue.get(LEASE_MS)

    def ack(self, claim: Any) -> None:
        self.queue.ack(claim)

    def pending_copy(self) -> tuple[float, float, int]:
        """bounded cursor: 메타데이터만 쪽 단위로 읽고(DB 비용), 복호화는 쪽마다 payload를 요청해 따로 잰다."""
        start = time.perf_counter()
        count = sum(1 for _ in self.queue.iter_items(100))
        read_s = time.perf_counter() - start
        start = time.perf_counter()
        copied = sum(1 for item in self.queue.iter_items(100, with_payload=True) if json.loads(item.payload or b"{}"))
        return read_s, time.perf_counter() - start, count if copied == count else -1

    def pending_api_copy(self) -> int:
        return sum(1 for _ in self.queue.iter_items(100))

    def purge(self, n: int) -> None:
        while self.queue.purge(1024) > 0:
            pass

    def close(self) -> None:
        self.db.close()


def make_adapter(system: str, directory: Path) -> Any:
    if system == "persist-queue":
        return PersistQueue(directory)
    if system == "sqlite-lease":
        return SqliteLease(directory)
    return Lrn(directory)


# ── 한 측정(worker) ──


def drain(adapter: Any, threads: int) -> int:
    counts = [0] * threads

    def run(slot: int) -> None:
        while (claim := adapter.claim()) is not None:
            adapter.ack(claim)
            counts[slot] += 1

    workers = [threading.Thread(target=run, args=(slot,)) for slot in range(threads)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    return sum(counts)


def run_worker(args: argparse.Namespace) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="ackq-bench-") as tmp:
        adapter = make_adapter(args.system, Path(tmp))
        base_rss = rss_peak_bytes()
        result: dict[str, Any] = {
            "system": args.system,
            "n": args.n,
            "payload_bytes": args.payload_bytes,
            "threads": args.threads,
            "sync": str(adapter.synchronous),
        }
        start = time.perf_counter()
        for index in range(args.n):
            adapter.put(synthetic(index, args.payload_bytes))
        result["put_s"] = time.perf_counter() - start
        start = time.perf_counter()
        done = drain(adapter, args.threads)
        result["drain_s"] = time.perf_counter() - start
        start = time.perf_counter()
        adapter.purge(args.n)
        result["purge_s"] = time.perf_counter() - start
        result["drained"] = done
        if done != args.n:
            raise RuntimeError(f"{args.system}: drained {done} of {args.n}")
        result["rss_delta_mb"] = (rss_peak_bytes() - base_rss) / 2**20
        result["rss_base_mb"] = base_rss / 2**20
        result["rss_peak_mb"] = rss_peak_bytes() / 2**20
        adapter.close()
    return result


def run_pending(args: argparse.Namespace) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="ackq-bench-") as tmp:
        adapter = make_adapter(args.system, Path(tmp))
        for index in range(args.n):
            adapter.put(synthetic(index, args.payload_bytes))
        base_rss = rss_peak_bytes()
        start = time.perf_counter()
        copied = adapter.pending_api_copy()
        api_s = time.perf_counter() - start
        api_rss = (rss_peak_bytes() - base_rss) / 2**20
        read_s, decode_s, count = adapter.pending_copy()
        peak_mb = rss_peak_bytes() / 2**20
        adapter.close()
    if copied != args.n or count != args.n:
        raise RuntimeError(f"{args.system}: pending copied {copied}, counted {count}, expected {args.n}")
    return {
        "system": args.system,
        "n": args.n,
        "payload_bytes": args.payload_bytes,
        "scan_items": copied,
        "api_s": api_s,
        "api_rss_delta_mb": api_rss,
        "rss_peak_mb": peak_mb,
        "db_read_s": read_s,
        "decode_copy_s": decode_s,
        "counted": count,
    }


# ── 비교(orchestrator) ──


# cost: time O(t), heap O(o), stack O(1), io 1
# vars: t = worker 한 번의 실행 시간, o = worker 출력 바이트
# basis: estimate
def spawn(mode: str, **flags: Any) -> dict[str, Any]:
    command = [sys.executable, os.path.abspath(__file__), mode]
    for key, value in flags.items():
        command += [f"--{key.replace('_', '-')}", str(value)]
    completed = subprocess.run(command, capture_output=True, text=True, check=False, env=os.environ)
    if completed.returncode != 0:
        raise RuntimeError(f"worker failed: {' '.join(command)}\n{completed.stderr[-2000:]}")
    return json.loads(completed.stdout.strip().splitlines()[-1])


def median(runs: list[dict[str, Any]], key: str) -> float:
    return statistics.median(run[key] for run in runs)


# cost: time O(s·p·h·r·t), heap O(s·p·h·r), stack O(1), io O(s·p·h·r)
# vars: s = 크기 수, p = payload 수, h = 스레드 수, r = 반복 수, t = worker 한 번의 실행 시간
# basis: estimate
def run_compare(args: argparse.Namespace) -> None:
    rows = []
    for n in map(int, args.sizes.split(",")):
        for payload in map(int, args.payloads.split(",")):
            for threads in map(int, args.threads.split(",")):
                for system in args.systems.split(","):
                    runs = [
                        spawn("worker", system=system, n=n, payload_bytes=payload, threads=threads)
                        for _ in range(args.repeats)
                    ]
                    row = {
                        "system": system,
                        "n": n,
                        "payload_bytes": payload,
                        "threads": threads,
                        "sync": runs[0]["sync"],
                        "repeats": args.repeats,
                        **{key: round(median(runs, key), 4) for key in ("put_s", "drain_s", "purge_s", "rss_delta_mb")},
                        "rss_base_mb": round(median(runs, "rss_base_mb"), 2),
                        "rss_peak_mb": round(median(runs, "rss_peak_mb"), 2),
                        "rss_peak_max_mb": round(max(r["rss_peak_mb"] for r in runs), 2),
                        "drained_equals_n": all(r["drained"] == n for r in runs),
                        "drain_min_s": round(min(r["drain_s"] for r in runs), 4),
                        "drain_max_s": round(max(r["drain_s"] for r in runs), 4),
                    }
                    rows.append(row)
                    print(json.dumps(row, ensure_ascii=False), flush=True)
    meta = {"python": sys.version.split()[0], "platform": platform.platform(), "sqlite": sqlite3.sqlite_version}
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(json.dumps({"meta": meta, "rows": rows}, ensure_ascii=False, indent=1))
    print(f"wrote {args.output}", file=sys.stderr)


# cost: time O(y·r·t), heap O(y·r), stack O(1), io O(y·r)
# vars: y = 비교 시스템 수, r = 반복 수, t = worker 한 번의 실행 시간
# basis: estimate
def run_pending_compare(args: argparse.Namespace) -> None:
    for system in SYSTEMS:
        runs = [
            spawn("pending-worker", system=system, n=args.n, payload_bytes=args.payload_bytes)
            for _ in range(args.repeats)
        ]
        row = {
            "system": system,
            "n": args.n,
            "payload_bytes": args.payload_bytes,
            **{
                key: round(median(runs, key), 4)
                for key in ("api_s", "api_rss_delta_mb", "rss_peak_mb", "db_read_s", "decode_copy_s")
            },
        }
        print(json.dumps(row, ensure_ascii=False), flush=True)


# cost: time O(m), heap O(1), stack O(1), io 1
# vars: m = 선택한 모드의 실행 시간
# basis: estimate
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    for name in ("worker", "pending-worker"):
        worker = sub.add_parser(name)
        worker.add_argument("--system", choices=SYSTEMS, required=True)
        worker.add_argument("--n", type=int, required=True)
        worker.add_argument("--payload-bytes", type=int, default=256)
        worker.add_argument("--threads", type=int, default=1)
    compare = sub.add_parser("compare")
    compare.add_argument("--sizes", default="200")
    compare.add_argument("--payloads", default="256")
    compare.add_argument("--threads", default="1")
    compare.add_argument("--repeats", type=int, default=3)
    compare.add_argument("--systems", default=",".join(SYSTEMS))
    compare.add_argument("--output", default="build/bench/ack_queue_compare.json")
    pending = sub.add_parser("pending")
    pending.add_argument("--n", type=int, default=500)
    pending.add_argument("--payload-bytes", type=int, default=65536)
    pending.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.mode == "worker":
        print(json.dumps(run_worker(args)))
    elif args.mode == "pending-worker":
        print(json.dumps(run_pending(args)))
    elif args.mode == "compare":
        run_compare(args)
    else:
        run_pending_compare(args)


if __name__ == "__main__":
    main()
