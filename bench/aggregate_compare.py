"""반복 이벤트 집계를 같은 데이터·같은 SQL로 비교한다(#42). 합성 데이터만 쓴다.

비교 대상: 이 엔진(totedb), SQLite(색인 없음), SQLite + 커버링 색인, DuckDB(설치돼 있을 때).
모든 엔진의 결과가 같아야 시간을 기록한다. 시간은 Python 호출을 포함한 왕복이며 판정에 쓰지 않는다.

    TOTEDB_LIB=build/lib/libtotedb.so PYTHONPATH=python python3 bench/aggregate_compare.py --rows 100000
"""

from __future__ import annotations

import argparse
import json
import random
import sqlite3
import statistics
import tempfile
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from totedb import Database

KINDS = 16
USERS = 1000
BATCH = 500

QUERIES = {
    "kind": "SELECT kind, COUNT(*), SUM(amount), MIN(ts), MAX(ts) FROM events GROUP BY kind",
    "user_having": "SELECT user_id, COUNT(*), MAX(ts) FROM events GROUP BY user_id HAVING COUNT(*) > 100",
    "distinct": "SELECT kind, COUNT(DISTINCT user_id) FROM events GROUP BY kind",
    "total": "SELECT COUNT(*), SUM(amount), AVG(amount) FROM events",
}
CREATE = "CREATE TABLE events (id INTEGER PRIMARY KEY, kind INTEGER, user_id INTEGER, amount INTEGER, ts INTEGER)"


def make_rows(count: int) -> list[tuple[int, int, int, int, int]]:
    rng = random.Random(42)
    return [(i, rng.randrange(KINDS), rng.randrange(USERS), rng.randrange(1, 1000), 1_700_000_000 + i) for i in range(1, count + 1)]


def insert_sql(rows: list[tuple[int, ...]]) -> str:
    return "INSERT INTO events VALUES " + ", ".join("(" + ", ".join(str(v) for v in row) + ")" for row in rows)


def normalize(rows: Any) -> list[tuple[Any, ...]]:
    out = []
    for row in rows:
        out.append(tuple(round(v, 6) if isinstance(v, float) else v for v in row))
    return sorted(out)


def timed(run: Callable[[], Any], repeats: int) -> tuple[Any, float]:
    run()
    samples = []
    result = None
    for _ in range(repeats):
        begin = time.perf_counter()
        result = run()
        samples.append((time.perf_counter() - begin) * 1000)
    return result, statistics.median(samples)


def run_ours(path: Path, rows: list[tuple[int, ...]], repeats: int) -> dict[str, tuple[Any, float]]:
    with Database(path) as db:
        db.execute(CREATE)
        with db.begin() as tx:
            for start in range(0, len(rows), BATCH):
                tx.execute(insert_sql(rows[start : start + BATCH]))
            tx.commit()
        return {name: timed(lambda sql=sql: normalize(db.execute(sql).rows), repeats) for name, sql in QUERIES.items()}


def run_sqlite(path: Path, rows: list[tuple[int, ...]], repeats: int, covering: bool) -> dict[str, tuple[Any, float]]:
    conn = sqlite3.connect(path)
    conn.execute(CREATE)
    conn.executemany("INSERT INTO events VALUES (?, ?, ?, ?, ?)", rows)
    if covering:
        conn.execute("CREATE INDEX events_kind ON events (kind, amount, ts, user_id)")
        conn.execute("CREATE INDEX events_user ON events (user_id, ts)")
    conn.commit()
    try:
        return {name: timed(lambda sql=sql: normalize(conn.execute(sql).fetchall()), repeats) for name, sql in QUERIES.items()}
    finally:
        conn.close()


def run_duckdb(rows: list[tuple[int, ...]], repeats: int) -> dict[str, tuple[Any, float]] | None:
    try:
        import duckdb  # noqa: PLC0415 - 선택 의존성
    except ImportError:
        return None
    conn = duckdb.connect()
    conn.execute(CREATE.replace("INTEGER PRIMARY KEY", "BIGINT PRIMARY KEY").replace("INTEGER", "BIGINT"))
    conn.executemany("INSERT INTO events VALUES (?, ?, ?, ?, ?)", rows)
    try:
        return {name: timed(lambda sql=sql: normalize(conn.execute(sql).fetchall()), repeats) for name, sql in QUERIES.items()}
    finally:
        conn.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=100_000)
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--output", default="build/bench/aggregate_compare.json")
    args = parser.parse_args()
    rows = make_rows(args.rows)
    with tempfile.TemporaryDirectory(prefix="aggcmp-") as tmp:
        engines = {
            "lrn": run_ours(Path(tmp) / "ours.lrdb", rows, args.repeats),
            "sqlite": run_sqlite(Path(tmp) / "plain.db", rows, args.repeats, covering=False),
            "sqlite_covering": run_sqlite(Path(tmp) / "covering.db", rows, args.repeats, covering=True),
        }
        duck = run_duckdb(rows, args.repeats)
        if duck is not None:
            engines["duckdb"] = duck
    report: dict[str, Any] = {"rows": args.rows, "repeats": args.repeats, "median_ms": {}, "same_result": {}}
    for name in QUERIES:
        reference = engines["sqlite"][name][0]
        # DuckDB의 AVG는 같은 값이고 SUM은 HUGEINT라 정수로 맞춰 비교한다
        report["same_result"][name] = {
            engine: [tuple(int(v) if hasattr(v, "__int__") and not isinstance(v, float) else v for v in row) for row in got[name][0]] == reference
            for engine, got in engines.items()
        }
        report["median_ms"][name] = {engine: round(got[name][1], 2) for engine, got in engines.items()}
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if all(all(v.values()) for v in report["same_result"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
