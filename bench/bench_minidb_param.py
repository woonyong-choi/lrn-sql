#!/usr/bin/env python3
"""
Parameterized MiniDB benchmark driver (before/after, multiple scales).

Interface: REPL binary, SQL piped via stdin, stdout -> /dev/null.
Single process = single connection, sequential. Each scenario runs as its own
REPL process against the same DB file; wall time of the whole process is
measured and a no-op baseline (open DB + .exit) is subtracted.

Range scenario:  SELECT * FROM bench WHERE id >= a LIMIT 1000
  - "before" binary (-DMINIDB_DISABLE_INDEX_RANGE): heap scan + early stop
  - "after"  binary (default):                      B+tree index range scan
  Same rows returned in the same (ascending-id) order; only the access path
  differs, so this is an apples-to-apples algorithm comparison.

Usage: python3 bench_minidb_param.py <binary> <label> <N_ROWS>
"""
import os, statistics, subprocess, sys, tempfile, time, random

REPS = 3
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


def make_sql_files(d, n):
    values = gen_values(n)
    point_ids = gen_point_ids(n)
    range_starts = gen_range_starts(n)
    files = {}

    def write(name, lines):
        p = os.path.join(d, name)
        with open(p, "w") as f:
            f.write("\n".join(lines) + "\n.exit\n")
        files[name] = p

    write("noop.sql", [])
    write("insert.sql", ["CREATE TABLE bench (value BIGINT)"] +
          [f"INSERT INTO bench VALUES ({v})" for v in values])
    write("point.sql", [f"SELECT * FROM bench WHERE id = {i}" for i in point_ids])
    write("range.sql", [f"SELECT * FROM bench WHERE id >= {a} LIMIT {RANGE_WIDTH}"
                        for a in range_starts])
    write("full.sql", ["SELECT * FROM bench" for _ in range(N_FULL)])
    return files


def run_phase(binary, db, sql_file):
    with open(sql_file) as fin, open(os.devnull, "wb") as devnull:
        t0 = time.perf_counter()
        r = subprocess.run([binary, db], stdin=fin, stdout=devnull,
                           stderr=subprocess.PIPE)
        t1 = time.perf_counter()
    if r.returncode != 0:
        raise RuntimeError(f"minidb failed: {r.stderr.decode()[:400]}")
    return t1 - t0


def main():
    binary, label, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    workdir = tempfile.mkdtemp(prefix="minidb-bench-")
    files = make_sql_files(workdir, n)
    results = {k: [] for k in ("baseline", "insert", "point", "range", "full")}

    for rep in range(REPS):
        db = os.path.join(workdir, f"bench_rep{rep}.db")
        if os.path.exists(db):
            os.remove(db)
        results["insert"].append(run_phase(binary, db, files["insert.sql"]))
        results["baseline"].append(run_phase(binary, db, files["noop.sql"]))
        results["point"].append(run_phase(binary, db, files["point.sql"]))
        results["range"].append(run_phase(binary, db, files["range.sql"]))
        results["full"].append(run_phase(binary, db, files["full.sql"]))
        print(f"[{label} N={n}] rep {rep}: " + " ".join(
            f"{k}={results[k][-1]*1000:.1f}ms" for k in results), flush=True)

    base = statistics.median(results["baseline"])
    ops = {"insert": n, "point": N_POINT, "range": N_RANGE, "full": N_FULL}
    print(f"\n== MiniDB [{label}] N={n} (median of {REPS}, baseline={base*1000:.1f}ms subtracted) ==")
    for k, nn in ops.items():
        med = statistics.median(results[k]) - base
        line = (f"{k:8s} net_median={med*1000:.2f} ms  ops/sec={nn/med:.1f}"
                if med > 0 else f"{k:8s} net<=0 ({med*1000:.3f}ms) raw={sorted(round(x*1000,2) for x in results[k])}")
        print(line, flush=True)


if __name__ == "__main__":
    main()
