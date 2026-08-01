#!/usr/bin/env python3
"""
Parameterized PostgreSQL benchmark driver (psycopg2, single connection).

- autocommit=True: each INSERT is its own transaction (matches MiniDB's
  per-statement execution and psql default).
- Range uses real BETWEEN a AND (a+RANGE_WIDTH-1) -> B-tree index range scan.
  MiniDB "after" uses id >= a LIMIT 1000, which returns the identical dense
  block of rows via its own B+tree range scan.
- fsync is whatever the server is currently set to (printed at start).
- Env vars: PG_N, PG_REPS_INSERT, PG_REPS_READ, PG_DO_INSERT (0/1)

Usage: PG_N=10000 python3 bench_pg_param.py <label>
"""
import os, statistics, sys, time, random
import psycopg2

DSN = "host=127.0.0.1 dbname=bench user=bench password=bench"
RANGE_WIDTH = 1000
N_POINT = 1000
N_RANGE = 100
N_FULL = 10
SEED = 42

N = int(os.environ.get("PG_N", "10000"))
REPS_INSERT = int(os.environ.get("PG_REPS_INSERT", "3"))
REPS_READ = int(os.environ.get("PG_REPS_READ", "3"))
DO_INSERT = os.environ.get("PG_DO_INSERT", "1") == "1"


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
    label = sys.argv[1] if len(sys.argv) > 1 else "default"
    values = gen_values(N)
    point_ids = gen_point_ids(N)
    range_starts = gen_range_starts(N)

    conn = psycopg2.connect(DSN)
    conn.autocommit = True
    cur = conn.cursor()
    cur.execute("SHOW server_version"); ver = cur.fetchone()[0]
    cur.execute("SHOW fsync"); fsync = cur.fetchone()[0]
    cur.execute("SHOW synchronous_commit"); sc = cur.fetchone()[0]
    print(f"[PG {label}] version={ver} fsync={fsync} synchronous_commit={sc} N={N}", flush=True)

    def do_insert():
        for i, v in enumerate(values, start=1):
            cur.execute("INSERT INTO bench VALUES (%s, %s)", (i, v))

    def do_point():
        for pid in point_ids:
            cur.execute("SELECT * FROM bench WHERE id = %s", (pid,)); cur.fetchall()

    def do_range():
        for a in range_starts:
            cur.execute("SELECT * FROM bench WHERE id BETWEEN %s AND %s",
                        (a, a + RANGE_WIDTH - 1)); cur.fetchall()

    def do_full():
        for _ in range(N_FULL):
            cur.execute("SELECT * FROM bench"); cur.fetchall()

    ins = []
    if DO_INSERT:
        for rep in range(REPS_INSERT):
            cur.execute("DROP TABLE IF EXISTS bench")
            cur.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY, value BIGINT)")
            t = timed(do_insert)
            ins.append(t)
            print(f"[PG {label}] insert rep {rep}: {t*1000:.1f}ms", flush=True)
    else:
        # ensure table exists and is populated once
        cur.execute("SELECT count(*) FROM bench")
        if cur.fetchone()[0] != N:
            cur.execute("DROP TABLE IF EXISTS bench")
            cur.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY, value BIGINT)")
            do_insert()

    reads = {"point": [], "range": [], "full": []}
    for rep in range(REPS_READ):
        reads["point"].append(timed(do_point))
        reads["range"].append(timed(do_range))
        reads["full"].append(timed(do_full))
        print(f"[PG {label}] read rep {rep}: " +
              " ".join(f"{k}={reads[k][-1]*1000:.1f}ms" for k in reads), flush=True)

    print(f"\n== PostgreSQL [{label}] N={N} fsync={fsync} ==", flush=True)
    if ins:
        med = statistics.median(ins)
        print(f"insert   median={med*1000:.1f} ms  ops/sec={N/med:.1f}", flush=True)
    ops = {"point": N_POINT, "range": N_RANGE, "full": N_FULL}
    for k, nn in ops.items():
        med = statistics.median(reads[k])
        print(f"{k:8s} median={med*1000:.2f} ms  ops/sec={nn/med:.1f}", flush=True)

    conn.close()


if __name__ == "__main__":
    main()
