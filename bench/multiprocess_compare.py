"""여러 프로세스가 같은 파일의 작업을 나눠 처리하는 처리량을 SQLite와 비교한다(#199).

거래 하나: 상태가 0인 가장 작은 id 하나를 고르고, 상태를 1로 바꾸고, 처리 기록을 넣는다. 두 엔진이 같은 SQL을 쓴다.
내구성: 거래마다 디스크 동기화(ToteDB COW+fsync, SQLite synchronous=FULL). SQLite는 DELETE(rollback)와 WAL 두 모드.
다른 프로세스가 쓰는 중이면 두 엔진 모두 짧게 쉬며 다시 시도한다(ToteDB 바인딩의 BUSY 재시도, SQLite busy_timeout).

사용: TOTEDB_LIB=build/lib/libtotedb.so PYTHONPATH=python python3 bench/multiprocess_compare.py --jobs 800 --repeats 3
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
SCHEMA = (
    "CREATE TABLE jobs (id INTEGER PRIMARY KEY, state INTEGER NOT NULL, payload BLOB NOT NULL)",
    "CREATE INDEX jobs_state ON jobs (state)",
    "CREATE TABLE done (id INTEGER PRIMARY KEY, worker INTEGER NOT NULL)",
)
PICK = "SELECT id FROM jobs WHERE state = 0 ORDER BY id LIMIT 1"
TAKE = "UPDATE jobs SET state = 1 WHERE id = ? AND state = 0"
RECORD = "INSERT INTO done (id, worker) VALUES (?, ?)"


def _tote_worker(path: str, worker: int, start: multiprocessing.Event, out: multiprocessing.Queue) -> None:
    from totedb import Database

    db = Database(path, create=False, busy_timeout_s=120)
    start.wait()
    done = 0
    while True:
        with db.begin() as txn:
            rows = txn.execute(PICK).rows
            if not rows:
                break
            job = rows[0][0]
            txn.execute(TAKE, (job,))
            txn.execute(RECORD, (job, worker))
        done += 1
    db.close()
    out.put(done)


def _sqlite_worker(path: str, worker: int, start: multiprocessing.Event, out: multiprocessing.Queue) -> None:
    db = sqlite3.connect(path, timeout=120, isolation_level=None)
    db.execute("PRAGMA synchronous=FULL")
    start.wait()
    done = 0
    while True:
        db.execute("BEGIN IMMEDIATE")
        row = db.execute(PICK).fetchone()
        if row is None:
            db.execute("COMMIT")
            break
        db.execute(TAKE, (row[0],))
        db.execute(RECORD, (row[0], worker))
        db.execute("COMMIT")
        done += 1
    db.close()
    out.put(done)


def _prepare(engine: str, path: Path, jobs: int) -> None:
    payload = b"p" * 256
    if engine == "totedb":
        from totedb import Database

        db = Database(path)
        for sql in SCHEMA:
            db.execute(sql)
        with db.begin() as txn:
            for i in range(1, jobs + 1):
                txn.execute("INSERT INTO jobs VALUES (?, 0, ?)", (i, payload))
        db.close()
        return
    db = sqlite3.connect(path, isolation_level=None)
    db.execute(f"PRAGMA journal_mode={'WAL' if engine == 'sqlite-wal' else 'DELETE'}")
    db.execute("PRAGMA synchronous=FULL")
    for sql in SCHEMA:
        db.execute(sql)
    db.execute("BEGIN")
    db.executemany("INSERT INTO jobs VALUES (?, 0, ?)", [(i, payload) for i in range(1, jobs + 1)])
    db.execute("COMMIT")
    db.close()


def _verify(engine: str, path: Path, jobs: int) -> bool:
    if engine == "totedb":
        from totedb import Database

        db = Database(path, create=False)
        done = db.execute("SELECT COUNT(*), COUNT(DISTINCT id) FROM done").rows[0]
        left = db.execute("SELECT COUNT(*) FROM jobs WHERE state = 0").rows[0][0]
        db.close()
    else:
        db = sqlite3.connect(path)
        done = db.execute("SELECT COUNT(*), COUNT(DISTINCT id) FROM done").fetchone()
        left = db.execute("SELECT COUNT(*) FROM jobs WHERE state = 0").fetchone()[0]
        db.close()
    return tuple(done) == (jobs, jobs) and left == 0


def run_once(engine: str, processes: int, jobs: int, workdir: Path) -> dict[str, object]:
    path = workdir / f"{engine}-{processes}.db"
    _prepare(engine, path, jobs)
    start = SPAWN.Event()
    out = SPAWN.Queue()
    target = _tote_worker if engine == "totedb" else _sqlite_worker
    workers = [SPAWN.Process(target=target, args=(str(path), w, start, out)) for w in range(processes)]
    for worker in workers:
        worker.start()
    time.sleep(0.5)  # 모든 worker가 파일을 연 뒤 같이 시작한다
    began = time.perf_counter()
    start.set()
    for worker in workers:
        worker.join()
    elapsed = time.perf_counter() - began
    counts = [out.get(timeout=10) for _ in workers]
    ok = all(worker.exitcode == 0 for worker in workers) and sum(counts) == jobs and _verify(engine, path, jobs)
    return {"engine": engine, "processes": processes, "jobs": jobs, "seconds": round(elapsed, 4),
            "jobs_per_s": round(jobs / elapsed, 1), "correct": ok, "per_worker": counts}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--jobs", type=int, default=800)
    parser.add_argument("--processes", default="1,4,16")
    parser.add_argument("--engines", default="totedb,sqlite-delete,sqlite-wal")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("build/bench/multiprocess_compare.json"))
    args = parser.parse_args()
    runs = []
    with tempfile.TemporaryDirectory() as tmp:
        for repeat in range(args.repeats):
            for processes in [int(p) for p in args.processes.split(",")]:
                # 회마다 엔진 순서를 돌려 순서 효과를 줄인다
                engines = args.engines.split(",")
                engines = engines[repeat % len(engines):] + engines[: repeat % len(engines)]
                for engine in engines:
                    workdir = Path(tmp) / f"r{repeat}"
                    workdir.mkdir(exist_ok=True)
                    result = run_once(engine, processes, args.jobs, workdir)
                    result["repeat"] = repeat + 1
                    runs.append(result)
                    print(json.dumps(result, ensure_ascii=False), flush=True)
    summary = {}
    for run in runs:
        key = f"{run['engine']}/{run['processes']}"
        summary.setdefault(key, []).append(run["jobs_per_s"])
    medians = {key: statistics.median(values) for key, values in summary.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"runs": runs, "median_jobs_per_s": medians}, ensure_ascii=False, indent=2))
    for key, value in medians.items():
        print(f"{key:22s} {value:10.1f} jobs/s")


if __name__ == "__main__":
    main()
