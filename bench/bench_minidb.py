#!/usr/bin/env python3
"""
MiniDB benchmark driver.

Interface: REPL (build-o2/minidb, gcc -O2, NO sanitizers), SQL piped via stdin,
stdout redirected to /dev/null. Single process = single connection, sequential.

Each scenario runs as its own REPL process against the same DB file; wall time
of the whole process is measured, and a no-op baseline (open DB + .exit) is
subtracted to remove process/open/close overhead.

MiniDB parser has no BETWEEN/AND, so the range scenario uses the closest
equivalent:  SELECT * FROM bench WHERE id >= a LIMIT 1000
(ids are dense 1..N and heap order == id order, so the same 1000 rows are
returned as BETWEEN a AND a+999 — but execution is a heap scan with early
stop, not an index range scan; MiniDB only uses its B+tree for id = X.)

Usage: python3 bench_minidb.py [minidb-binary]
  인자를 생략하면 저장소의 build-o2/minidb 를 쓰고, 없으면 cc -O2 로 자동 빌드한다.
"""
import os
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from workload import (N_ROWS, N_POINT, N_RANGE, RANGE_WIDTH, N_FULL,
                      gen_values, gen_point_ids, gen_range_starts)

REPS = 3


def make_sql_files(d):
    values = gen_values()
    point_ids = gen_point_ids()
    range_starts = gen_range_starts()
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
        raise RuntimeError(f"minidb failed: {r.stderr.decode()}")
    return t1 - t0


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = ["storage/pager.c", "storage/schema.c", "storage/table.c",
        "storage/bptree.c", "sql/parser.c", "sql/planner.c", "sql/executor.c",
        "server/http.c", "server/server.c", "server/lock_table.c",
        "db.c", "main.c"]


def ensure_binary():
    """저장소의 build-o2/minidb 를 찾고, 없으면 sanitizer 없이 -O2 로 빌드한다."""
    binary = os.path.join(REPO, "build-o2", "minidb")
    if os.path.exists(binary):
        return binary
    os.makedirs(os.path.dirname(binary), exist_ok=True)
    cmd = ["cc", "-O2", "-Wall", "-I" + os.path.join(REPO, "include"),
           "-o", binary]
    cmd += [os.path.join(REPO, "src", f) for f in SRCS]
    cmd += ["-lpthread"]
    print("building:", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    return binary


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else ensure_binary()
    workdir = tempfile.mkdtemp(prefix="minidb-bench-")
    files = make_sql_files(workdir)

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
        print(f"rep {rep}: " + " ".join(
            f"{k}={results[k][-1]*1000:.1f}ms" for k in results), flush=True)

    base = statistics.median(results["baseline"])
    print("\n== MiniDB results (median of 3, baseline-subtracted) ==")
    print(f"baseline (open+close): {base*1000:.1f} ms")
    ops = {"insert": N_ROWS, "point": N_POINT, "range": N_RANGE, "full": N_FULL}
    for k, n in ops.items():
        med = statistics.median(results[k]) - base
        print(f"{k:8s} raw={sorted(x*1000 for x in results[k])} ms  "
              f"net_median={med*1000:.1f} ms  ops={n}  "
              f"ops/sec={n/med:.1f}" if med > 0 else f"{k}: net<=0 ({med})")


if __name__ == "__main__":
    main()
