#!/usr/bin/env python3
"""
Parameterized PostgreSQL benchmark driver (psycopg2, single connection).

- autocommit=True: each INSERT is its own transaction (matches MiniDB's
  per-statement execution and psql default).
- Range uses real BETWEEN a AND (a+RANGE_WIDTH-1) -> B-tree index range scan.
  MiniDB "after" uses id >= a LIMIT 1000, which returns the identical dense
  block of rows via its own B+tree range scan.
- fsync is whatever the server is currently set to (printed at start).

Usage (인자 전부 생략 가능):
  python3 bench/bench_pg_param.py                    # 100k 행
  python3 bench/bench_pg_param.py --rows 1000000     # 1M 행
  python3 bench/bench_pg_param.py --rows 1000000 --skip-insert   # 읽기만
"""
import argparse
import os
import random
import statistics
import sys
import time

import psycopg2

DSN = os.environ.get("PG_DSN",
                     "host=127.0.0.1 dbname=bench user=bench password=bench")
RANGE_WIDTH = 1000
N_POINT = 1000
N_RANGE = 100
N_FULL = 10
SEED = 42


def gen_values(n):
    rng = random.Random(SEED)
    return [rng.randrange(0, 1_000_000_000) for _ in range(n)]


def gen_point_ids(n):
    rng = random.Random(SEED + 1)
    return [rng.randrange(1, n + 1) for _ in range(N_POINT)]


def gen_range_starts(n):
    rng = random.Random(SEED + 2)
    return [rng.randrange(1, n - RANGE_WIDTH + 2) for _ in range(N_RANGE)]


def timed(fn):
    t0 = time.perf_counter()
    fn()
    return time.perf_counter() - t0


def main():
    ap = argparse.ArgumentParser(description="PostgreSQL parameterized benchmark")
    ap.add_argument("--rows", type=int, default=100_000,
                    help="테이블 행 수 (기본 100000)")
    ap.add_argument("--label", default="pg", help="출력 라벨")
    ap.add_argument("--reps-insert", type=int, default=3)
    ap.add_argument("--reps-read", type=int, default=3)
    ap.add_argument("--skip-insert", action="store_true",
                    help="INSERT 측정 생략 (테이블이 비어 있으면 1회 적재만)")
    args = ap.parse_args()

    n, label = args.rows, args.label
    values = gen_values(n)
    point_ids = gen_point_ids(n)
    range_starts = gen_range_starts(n)

    conn = psycopg2.connect(DSN)
    conn.autocommit = True
    cur = conn.cursor()
    cur.execute("SHOW server_version"); ver = cur.fetchone()[0]
    cur.execute("SHOW fsync"); fsync = cur.fetchone()[0]
    cur.execute("SHOW synchronous_commit"); sc = cur.fetchone()[0]
    print(f"[PG {label}] version={ver} fsync={fsync} "
          f"synchronous_commit={sc} N={n}", flush=True)

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

    ins = []
    if not args.skip_insert:
        for rep in range(args.reps_insert):
            cur.execute("DROP TABLE IF EXISTS bench")
            cur.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY, value BIGINT)")
            t = timed(do_insert)
            ins.append(t)
            print(f"[PG {label}] insert rep {rep}: {t*1000:.1f}ms", flush=True)
    else:
        cur.execute("""SELECT count(*) FROM information_schema.tables
                       WHERE table_name = 'bench'""")
        have = cur.fetchone()[0] == 1
        if have:
            cur.execute("SELECT count(*) FROM bench")
            have = cur.fetchone()[0] == n
        if not have:
            cur.execute("DROP TABLE IF EXISTS bench")
            cur.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY, value BIGINT)")
            do_insert()

    reads = {"point": [], "range": [], "full": []}
    for rep in range(args.reps_read):
        reads["point"].append(timed(do_point))
        reads["range"].append(timed(do_range))
        reads["full"].append(timed(do_full))
        print(f"[PG {label}] read rep {rep}: " +
              " ".join(f"{k}={reads[k][-1]*1000:.1f}ms" for k in reads),
              flush=True)

    print(f"\n== PostgreSQL [{label}] N={n} fsync={fsync} ==", flush=True)
    if ins:
        med = statistics.median(ins)
        print(f"insert   median={med*1000:.1f} ms  ops/sec={n/med:.1f}",
              flush=True)
    ops = {"point": N_POINT, "range": N_RANGE, "full": N_FULL}
    for k, nn in ops.items():
        med = statistics.median(reads[k])
        print(f"{k:8s} median={med*1000:.2f} ms  ops/sec={nn/med:.1f}",
              flush=True)

    conn.close()


if __name__ == "__main__":
    main()
