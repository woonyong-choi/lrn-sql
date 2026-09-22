#!/usr/bin/env python3
"""
규모를 키우며 INSERT 와 Range 질의의 '기울기'를 잰다.

왜 배수가 아니라 기울기인가:
  "×215 빨라졌다"는 한 대의 기계, 한 번의 실행에서만 참인 숫자다. 실제로
  같은 스크립트를 다른 기계에서 돌리면 ×57 도, ×107 도 나온다. 절대 배수는
  캐시 크기와 메모리 대역폭에 따라 크게 흔들린다.

  반면 "N 이 2배가 될 때 시간이 몇 배가 되는가"는 알고리즘의 성질이라
  기계가 바뀌어도 유지된다. 고친 결함이 O(N^2) 였다는 주장은 이 계수로만
  증명할 수 있다 — 계수가 4 에 가까우면 제곱, 2 에 가까우면 선형이다.

세 가지 빌드를 같은 소스에서 뽑아 같은 워크로드로 잰다:
  after   현재 코드
  prefix  수정 커밋 e640fdd 의 직전 코드 (INSERT O(N^2) 결함 2건이 살아 있음)
  noindex 현재 코드 + -DMINIDB_DISABLE_INDEX_RANGE (Range 가 힙 스캔으로 감)

사용:
  python3 bench/scaling.py                      # N = 25k..200k, 3회 중앙값
  python3 bench/scaling.py --max-rows 400000
  python3 bench/scaling.py --reps 1 --out bench/scaling.md
"""
import argparse
import os
import random
import statistics
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = ["storage/pager.c", "storage/schema.c", "storage/table.c",
        "storage/bptree.c", "sql/parser.c", "sql/planner.c", "sql/executor.c",
        "server/http.c", "server/server.c", "server/lock_table.c",
        "db.c", "main.c"]
PREFIX_COMMIT = "e640fdd"     # INSERT O(N^2) 결함 2건을 고친 커밋
RANGE_WIDTH = 1000
N_RANGE = 100
SEED = 42


def build(variant):
    """build-o2/minidb-<variant> 를 준비한다. prefix 는 git 이력에서 되살린다."""
    out = os.path.join(REPO, "build-o2", f"minidb-{variant}")
    if os.path.exists(out):
        return out
    os.makedirs(os.path.dirname(out), exist_ok=True)

    src_root = REPO
    tmp = None
    if variant == "prefix":
        tmp = tempfile.mkdtemp(prefix="minidb-prefix-")
        tar = subprocess.run(["git", "archive", PREFIX_COMMIT + "^"],
                             cwd=REPO, stdout=subprocess.PIPE, check=True)
        subprocess.run(["tar", "-x", "-C", tmp], input=tar.stdout, check=True)
        src_root = tmp

    cmd = ["cc", "-O2", "-Wall", "-I" + os.path.join(src_root, "include")]
    if variant == "noindex":
        cmd.append("-DMINIDB_DISABLE_INDEX_RANGE")
    cmd += ["-o", out]
    cmd += [os.path.join(src_root, "src", f) for f in SRCS]
    cmd += ["-lpthread"]
    print(f"  building {variant} ...", flush=True)
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return out


def write_sql(path, lines):
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n.exit\n")


def run(binary, db, sql_file):
    with open(sql_file) as fin, open(os.devnull, "wb") as null:
        t0 = time.perf_counter()
        r = subprocess.run([binary, db], stdin=fin, stdout=null,
                           stderr=subprocess.PIPE)
        t1 = time.perf_counter()
    if r.returncode != 0:
        raise RuntimeError(f"{binary} failed: {r.stderr.decode()[:300]}")
    return t1 - t0


def measure(binary, workdir, n, reps, do_range):
    """(insert 초, range 초) 중앙값. 프로세스 기동 비용은 no-op 실행으로 뺀다."""
    rng = random.Random(SEED)
    values = [rng.randrange(0, 1_000_000_000) for _ in range(n)]
    rng2 = random.Random(SEED + 2)
    starts = [rng2.randrange(1, max(2, n - RANGE_WIDTH + 2))
              for _ in range(N_RANGE)]

    ins = os.path.join(workdir, "insert.sql")
    rng_sql = os.path.join(workdir, "range.sql")
    noop = os.path.join(workdir, "noop.sql")
    write_sql(ins, ["CREATE TABLE bench (value BIGINT)"] +
              [f"INSERT INTO bench VALUES ({v})" for v in values])
    write_sql(rng_sql, [f"SELECT * FROM bench WHERE id >= {a} LIMIT {RANGE_WIDTH}"
                        for a in starts])
    write_sql(noop, [])

    ins_t, rng_t, base_t = [], [], []
    for rep in range(reps):
        db = os.path.join(workdir, f"s{n}_{rep}.db")
        if os.path.exists(db):
            os.remove(db)
        ins_t.append(run(binary, db, ins))
        base_t.append(run(binary, db, noop))
        if do_range:
            rng_t.append(run(binary, db, rng_sql))
        os.remove(db)

    base = statistics.median(base_t)
    i = max(statistics.median(ins_t) - base, 1e-9)
    r = max(statistics.median(rng_t) - base, 1e-9) if do_range else None
    return i, r


# ── SVG 그리기 (외부 의존성 없이) ───────────────────────────────────────
# GitHub README 에서 바로 보이고, 다시 측정하면 같은 명령으로 갱신된다.
# 라이트/다크 어느 배경에서도 읽히도록 배경은 비우고 중간 회색 축을 쓴다.

AXIS = "#8b949e"
TEXT = "#8b949e"
BEFORE = "#d1495b"
AFTER = "#2a9d8f"


def _panel(x0, y0, w, h, sizes, series, title, note):
    """log-log 선그래프 한 장. series = [(label, color, [초, ...]), ...]"""
    import math
    xs = [math.log10(n) for n in sizes]
    ys = [math.log10(v * 1000) for _, _, vals in series for v in vals]
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    if ymax - ymin < 0.5:
        mid = (ymax + ymin) / 2
        ymin, ymax = mid - 0.25, mid + 0.25
    pad = (ymax - ymin) * 0.12
    ymin, ymax = ymin - pad, ymax + pad

    def px(v):
        return x0 + (v - xmin) / (xmax - xmin) * w
    def py(v):
        return y0 + h - (v - ymin) / (ymax - ymin) * h

    out = [f'<text x="{x0}" y="{y0 - 24}" font-size="15" font-weight="600"'
           f' fill="{TEXT}">{title}</text>',
           f'<text x="{x0}" y="{y0 - 7}" font-size="11" fill="{TEXT}">{note}</text>',
           f'<line x1="{x0}" y1="{y0 + h}" x2="{x0 + w}" y2="{y0 + h}"'
           f' stroke="{AXIS}" stroke-width="1"/>',
           f'<line x1="{x0}" y1="{y0}" x2="{x0}" y2="{y0 + h}"'
           f' stroke="{AXIS}" stroke-width="1"/>']

    for n, xv in zip(sizes, xs):
        lab = f"{n // 1000}k"
        out.append(f'<text x="{px(xv):.1f}" y="{y0 + h + 16}" font-size="11"'
                   f' text-anchor="middle" fill="{TEXT}">{lab}</text>')
    for dec in range(int(ymin), int(ymax) + 2):
        if not (ymin <= dec <= ymax):
            continue
        yy = py(dec)
        ms = 10 ** dec
        lab = f"{ms:g}ms" if ms < 1000 else f"{ms / 1000:g}s"
        out.append(f'<line x1="{x0}" y1="{yy:.1f}" x2="{x0 + w}" y2="{yy:.1f}"'
                   f' stroke="{AXIS}" stroke-width="0.5" stroke-dasharray="3 4"'
                   f' opacity="0.5"/>')
        out.append(f'<text x="{x0 - 6}" y="{yy + 4:.1f}" font-size="11"'
                   f' text-anchor="end" fill="{TEXT}">{lab}</text>')

    for li, (label, color, vals) in enumerate(series):
        pts = " ".join(f"{px(x):.1f},{py(math.log10(v * 1000)):.1f}"
                       for x, v in zip(xs, vals))
        out.append(f'<polyline points="{pts}" fill="none" stroke="{color}"'
                   f' stroke-width="2.5" stroke-linejoin="round"/>')
        for x, v in zip(xs, vals):
            out.append(f'<circle cx="{px(x):.1f}" cy="{py(math.log10(v*1000)):.1f}"'
                       f' r="3.5" fill="{color}"/>')
        ly = y0 + 13 + li * 17
        out.append(f'<line x1="{x0 + w - 142}" y1="{ly - 4}"'
                   f' x2="{x0 + w - 124}" y2="{ly - 4}" stroke="{color}"'
                   f' stroke-width="2.5"/>')
        out.append(f'<text x="{x0 + w - 118}" y="{ly}" font-size="12"'
                   f' fill="{color}">{label}</text>')
    return out


def write_svg(path, sizes, res):
    W, H = 960, 320
    body = []
    ki_b = exponent(sizes, res["prefix"]["insert"])
    ki_a = exponent(sizes, res["after"]["insert"])
    kr_b = exponent(sizes, res["noindex"]["range"])
    kr_a = exponent(sizes, res["after"]["range"])
    body += _panel(66, 52, 340, 196, sizes,
                   [(f"개선 전  O(N^{ki_b:.2f})", BEFORE, res["prefix"]["insert"]),
                    (f"개선 후  O(N^{ki_a:.2f})", AFTER, res["after"]["insert"])],
                   "INSERT 전체 소요", "양축 로그 — 기울기가 그대로 지수다")
    body += _panel(556, 52, 340, 196, sizes,
                   [(f"힙 스캔  O(N^{kr_b:.2f})", BEFORE, res["noindex"]["range"]),
                    (f"B+Tree  O(N^{kr_a:.2f})", AFTER, res["after"]["range"])],
                   "Range 질의 100회 (각 1,000행)", "반환 행 수는 고정, 테이블만 커진다")
    body.append(f'<text x="66" y="{H - 12}" font-size="11" fill="{TEXT}">'
                f'bench/scaling.py — 규모별 중앙값, 프로세스 기동 시간 차감, -O2</text>')
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,sans-serif">\n'
           + "\n".join(body) + "\n</svg>\n")
    with open(path, "w") as f:
        f.write(svg)


def exponent(sizes, times):
    """log(시간) = k·log(N) + c 의 기울기 k (최소제곱).

    쌍별 배가 계수는 한 번의 잡음에 통째로 흔들리지만, 전 구간을 한 직선으로
    맞춘 기울기는 훨씬 덜 움직인다. k≈2 면 O(N^2), k≈1 이면 O(N) 이다.
    """
    import math
    xs = [math.log(n) for n in sizes]
    ys = [math.log(t) for t in times]
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


def growth(series):
    """연속한 두 규모 사이의 시간 증가 계수 (N 이 2배가 될 때 몇 배)."""
    out = [None]
    for a, b in zip(series, series[1:]):
        out.append(b / a if a > 0 else None)
    return out


def fmt(x, unit="s"):
    return "-" if x is None else f"{x:.3f}{unit}"


def main():
    ap = argparse.ArgumentParser(description="MiniDB 규모별 기울기 측정")
    ap.add_argument("--min-rows", type=int, default=50_000)
    ap.add_argument("--max-rows", type=int, default=400_000)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(REPO, "bench", "scaling.md"))
    args = ap.parse_args()

    sizes = []
    n = args.min_rows
    while n <= args.max_rows:
        sizes.append(n)
        n *= 2
    if not sizes:
        print("규모 범위가 비었습니다", file=sys.stderr)
        return 1

    bins = {v: build(v) for v in ("after", "prefix", "noindex")}
    workdir = tempfile.mkdtemp(prefix="minidb-scaling-")

    res = {v: {"insert": [], "range": []} for v in bins}
    for n in sizes:
        for v, b in bins.items():
            do_range = (v != "prefix")   # prefix 는 INSERT 결함만 본다
            i, r = measure(b, workdir, n, args.reps, do_range)
            res[v]["insert"].append(i)
            if do_range:
                res[v]["range"].append(r)
            print(f"  N={n:>7} {v:<8} insert={i*1000:9.1f}ms"
                  f" range={'-' if r is None else format(r*1000, '8.1f') + 'ms'}",
                  flush=True)

    g_after = growth(res["after"]["insert"])
    g_prefix = growth(res["prefix"]["insert"])
    gr_after = growth(res["after"]["range"])
    gr_noidx = growth(res["noindex"]["range"])

    lines = []
    lines.append("# 규모별 기울기 (scaling)")
    lines.append("")
    lines.append(f"`python3 bench/scaling.py` 출력. 규모별 {args.reps}회 중앙값, "
                 "프로세스 기동 시간은 no-op 실행으로 차감. -O2, sanitizer 없음.")
    lines.append("")
    lines.append("## INSERT — 힙 재탐색 힌트 (`heap_may_have_free_slots`)")
    lines.append("")
    lines.append("`prefix` 는 수정 커밋 `e640fdd` 의 직전 코드다. N 이 2배가 될 때 "
                 "시간이 약 4배가 되면 O(N^2), 약 2배면 O(N) 이다.")
    lines.append("")
    k_prefix = exponent(sizes, res["prefix"]["insert"])
    k_after = exponent(sizes, res["after"]["insert"])
    lines.append(f"**전 구간 기울기: 개선 전 O(N^{k_prefix:.2f}) → 개선 후 "
                 f"O(N^{k_after:.2f}).** 쌍별 배가 계수는 한 번의 잡음에 흔들리지만, "
                 "전 구간을 한 직선으로 맞춘 이 지수는 훨씬 덜 움직인다.")
    lines.append("")
    lines.append("| 행 수 | 개선 전 (prefix) | 배가 계수 | 개선 후 (after) | 배가 계수 |")
    lines.append("|---:|---:|---:|---:|---:|")
    for k, n in enumerate(sizes):
        lines.append(f"| {n:,} | {fmt(res['prefix']['insert'][k])} | "
                     f"{'-' if g_prefix[k] is None else f'{g_prefix[k]:.2f}x'} | "
                     f"{fmt(res['after']['insert'][k])} | "
                     f"{'-' if g_after[k] is None else f'{g_after[k]:.2f}x'} |")
    lines.append("")
    lines.append("## Range 질의 — B+Tree 리프 순회 (`INDEX_RANGE`)")
    lines.append("")
    lines.append("`noindex` 는 같은 소스를 `-DMINIDB_DISABLE_INDEX_RANGE` 로 빌드해 "
                 "힙 스캔 경로를 되살린 것이다. 반환 행 수는 양쪽 다 1,000 으로 "
                 "고정이므로, 시간이 N 을 따라 늘면 접근 경로가 테이블 크기에 "
                 "비례한다는 뜻이다.")
    lines.append("")
    kr_noidx = exponent(sizes, res["noindex"]["range"])
    kr_after = exponent(sizes, res["after"]["range"])
    lines.append(f"**전 구간 기울기: 힙 스캔 O(N^{kr_noidx:.2f}) → 인덱스 "
                 f"O(N^{kr_after:.2f}).**")
    lines.append("")
    lines.append("| 행 수 | 힙 스캔 (noindex) | 배가 계수 | 인덱스 (after) | 배가 계수 |")
    lines.append("|---:|---:|---:|---:|---:|")
    for k, n in enumerate(sizes):
        lines.append(f"| {n:,} | {fmt(res['noindex']['range'][k])} | "
                     f"{'-' if gr_noidx[k] is None else f'{gr_noidx[k]:.2f}x'} | "
                     f"{fmt(res['after']['range'][k])} | "
                     f"{'-' if gr_after[k] is None else f'{gr_after[k]:.2f}x'} |")
    lines.append("")
    body = "\n".join(lines) + "\n"
    with open(args.out, "w") as f:
        f.write(body)
    svg_path = os.path.join(REPO, "docs", "scaling.svg")
    write_svg(svg_path, sizes, res)
    print("\n" + body)
    print(f"→ {args.out}")
    print(f"→ {svg_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
