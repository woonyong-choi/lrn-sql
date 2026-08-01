#!/usr/bin/env python3
"""
PostgreSQL benchmark driver (psycopg2, single connection, sequential).

- autocommit=True: each INSERT is its own transaction (matches psql default
  and MiniDB's per-statement execution).
- Same workload as bench_minidb.py (workload.py, fixed seed).
- Range query uses real BETWEEN (index range scan) — MiniDB side uses
  WHERE id >= a LIMIT 1000 (heap scan), see bench_minidb.py header.
- Run once with fsync=on (default) and once with fsync=off:
    ALTER SYSTEM SET fsync = off; SELECT pg_reload_conf();

Usage: python3 bench_postgres.py <label>   (label: fsync_on / fsync_off)
"""
import os
import statistics
import sys
import time

import psycopg2

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from workload import (N_ROWS, N_POINT, N_RANGE, RANGE_WIDTH, N_FULL,
                      gen_values, gen_point_ids, gen_range_starts)

REPS = 3
DSN = "host=127.0.0.1 dbname=bench user=bench password=bench"


def timed(fn):
    t0 = time.perf_counter()
    fn()
    return time.perf_counter() - t0


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "default"
    values = gen_values()
    point_ids = gen_point_ids()
    range_starts = gen_range_starts()

    conn = psycopg2.connect(DSN)
    conn.autocommit = True
    cur = conn.cursor()

    cur.execute("SELECT version()")
    print("server:", cur.fetchone()[0])
    cur.execute("SHOW fsync")
    fsync = cur.fetchone()[0]
    cur.execute("SHOW synchronous_commit")
    sync_commit = cur.fetchone()[0]
    print(f"fsync={fsync} synchronous_commit={sync_commit} label={label}")

    results = {k: [] for k in ("insert", "point", "range", "full")}

    for rep in range(REPS):
        cur.execute("DROP TABLE IF EXISTS bench")
        cur.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY, value BIGINT)")

        def do_insert():
            for i, v in enumerate(values, start=1):
                cur.execute("INSERT INTO bench VALUES (%s, %s)", (i, v))

        def do_point():
            for pid in point_ids:
                cur.execute("SELECT * FROM bench WHERE id = %s", (pid,))
                cur.fetchall()

        def do_range():
            for a in range_starts:
                cur.execute("SELECT * FROM bench WHERE id BETWEEN %s AND %s",
                            (a, a + RANGE_WIDTH - 1))
                cur.fetchall()

        def do_full():
            for _ in range(N_FULL):
                cur.execute("SELECT * FROM bench")
                cur.fetchall()

        results["insert"].append(timed(do_insert))
        results["point"].append(timed(do_point))
        results["range"].append(timed(do_range))
        results["full"].append(timed(do_full))
        print(f"rep {rep}: " + " ".join(
            f"{k}={results[k][-1]*1000:.1f}ms" for k in results), flush=True)

    print(f"\n== PostgreSQL results ({label}, median of {REPS}) ==")
    ops = {"insert": N_ROWS, "point": N_POINT, "range": N_RANGE, "full": N_FULL}
    for k, n in ops.items():
        med = statistics.median(results[k])
        print(f"{k:8s} raw={sorted(x*1000 for x in results[k])} ms  "
              f"median={med*1000:.1f} ms  ops={n}  ops/sec={n/med:.1f}")

    cur.execute("DROP TABLE IF EXISTS bench")
    conn.close()


if __name__ == "__main__":
    main()
