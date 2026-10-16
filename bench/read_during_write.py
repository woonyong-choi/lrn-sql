"""다른 프로세스가 계속 쓰는 동안 조회 지연을 SQLite와 비교한다(#200).

writer 프로세스는 거래마다 20KB 행 20개를 넣고 commit한다. reader 프로세스는 같은 시간 동안 `SELECT COUNT(*)`를 반복하며
문장마다 걸린 시간을 잰다. 내구성은 거래마다 디스크 동기화(ToteDB COW+fsync, SQLite synchronous=FULL)다.

사용: TOTEDB_LIB=build/lib/libtotedb.so PYTHONPATH=python python3 bench/read_during_write.py --seconds 3 --repeats 3
"""

from __future__ import annotations

import argparse
import json
import multiprocessing
import sqlite3
import statistics
import tempfile
import time
from pathlib import Path

SPAWN = multiprocessing.get_context("spawn")
ROWS_PER_TXN = 20
PAD = b"w" * 20000


def _open(engine: str, path: str):
    if engine == "totedb":
        from totedb import Database

        return Database(path, create=False, busy_timeout_s=60)
    db = sqlite3.connect(path, timeout=60, isolation_level=None)
    db.execute("PRAGMA synchronous=FULL")
    return db


def _writer(engine: str, path: str, start: multiprocessing.Event, stop: multiprocessing.Event, out: multiprocessing.Queue) -> None:
    db = _open(engine, path)
    start.wait()
    txns = 0
    while not stop.is_set():
        if engine == "totedb":
            with db.begin() as txn:
                for _ in range(ROWS_PER_TXN):
                    txn.execute("INSERT INTO t (pad) VALUES (?)", (PAD,))
        else:
            db.execute("BEGIN IMMEDIATE")
            for _ in range(ROWS_PER_TXN):
                db.execute("INSERT INTO t (pad) VALUES (?)", (PAD,))
            db.execute("COMMIT")
        txns += 1
    db.close()
    out.put(("writer", txns))


def _reader(engine: str, path: str, start: multiprocessing.Event, stop: multiprocessing.Event, out: multiprocessing.Queue) -> None:
    db = _open(engine, path)
    start.wait()
    latencies = []
    while not stop.is_set():
        began = time.perf_counter()
        if engine == "totedb":
            db.execute("SELECT COUNT(*) FROM t")
        else:
            db.execute("SELECT COUNT(*) FROM t").fetchone()
        latencies.append(time.perf_counter() - began)
    db.close()
    out.put(("reader", latencies))


def _prepare(engine: str, path: Path) -> None:
    if engine == "totedb":
        from totedb import Database

        db = Database(path)
        db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, pad BLOB)")
        db.execute("INSERT INTO t (pad) VALUES (X'00')")
        db.close()
        return
    db = sqlite3.connect(path, isolation_level=None)
    db.execute(f"PRAGMA journal_mode={'WAL' if engine == 'sqlite-wal' else 'DELETE'}")
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, pad BLOB)")
    db.execute("INSERT INTO t (pad) VALUES (X'00')")
    db.close()


def run_once(engine: str, seconds: float, workdir: Path) -> dict[str, object]:
    path = workdir / f"{engine}.db"
    _prepare(engine, path)
    start, stop, out = SPAWN.Event(), SPAWN.Event(), SPAWN.Queue()
    workers = [SPAWN.Process(target=f, args=(engine, str(path), start, stop, out)) for f in (_writer, _reader)]
    for worker in workers:
        worker.start()
    time.sleep(0.5)
    start.set()
    time.sleep(seconds)
    stop.set()
    results = dict(out.get(timeout=120) for _ in workers)
    for worker in workers:
        worker.join()
    latencies = sorted(results["reader"])
    ms = [x * 1000 for x in latencies]
    return {
        "engine": engine,
        "writer_txns": results["writer"],
        "reads": len(ms),
        "read_p50_ms": round(ms[len(ms) // 2], 3),
        "read_p99_ms": round(ms[min(len(ms) - 1, int(len(ms) * 0.99))], 3),
        "read_max_ms": round(ms[-1], 3),
        "ok": all(worker.exitcode == 0 for worker in workers),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--engines", default="totedb,sqlite-delete,sqlite-wal")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("build/bench/read_during_write.json"))
    args = parser.parse_args()
    runs = []
    with tempfile.TemporaryDirectory() as tmp:
        for repeat in range(args.repeats):
            engines = args.engines.split(",")
            engines = engines[repeat % len(engines):] + engines[: repeat % len(engines)]
            for engine in engines:
                workdir = Path(tmp) / f"r{repeat}"
                workdir.mkdir(exist_ok=True)
                result = run_once(engine, args.seconds, workdir)
                result["repeat"] = repeat + 1
                runs.append(result)
                print(json.dumps(result, ensure_ascii=False), flush=True)
    medians = {}
    for engine in args.engines.split(","):
        mine = [run for run in runs if run["engine"] == engine]
        medians[engine] = {key: statistics.median(run[key] for run in mine)
                           for key in ("writer_txns", "reads", "read_p50_ms", "read_p99_ms", "read_max_ms")}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"runs": runs, "median": medians}, ensure_ascii=False, indent=2))
    for engine, values in medians.items():
        print(engine, values)


if __name__ == "__main__":
    main()
