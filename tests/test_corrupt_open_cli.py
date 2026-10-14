#!/usr/bin/env python3
"""손상된 DB 파일을 실제 minidb(REPL·--adopt-table·서버)로 열 때의 계약 검사 (#130).

정상 합성 DB를 만든 뒤 헤더·힙 체인·슬롯 등을 하나씩 망가뜨린 사본을 연다.
  - 제한 시간 안에 끝난다(정지하지 않는다).
  - 종료 코드 1, 출력에 손상 사유(strerror의 "Invalid argument"와 손상 문구)가 있다.
    읽기 자체의 실패("Input/output error")로 보고하지 않는다.
  - 파일 바이트가 그대로다. --adopt-table도 손상 파일을 고치거나 등록하지 않는다.
  - 손상 파일을 연 직후에도 정상 파일은 열리고 같은 행 수를 낸다.
  - sanitizer 진단이 없다.
프로세스 내부에서 fd·프레임 반환을 보는 검사는 tests/test_corrupt_open.c가 맡는다.

비용: minidb 프로세스 31개(시드 1, 사례 12개의 손상 파일 REPL 12, --adopt-table 3,
서버 시작 2, 정상 파일 확인 REPL 13). macOS arm64 UBSan 빌드에서 약 0.5초
(2026-10-15 한 번 측정).

작업 폴더: --workdir은 아직 없는 경로여야 한다. 이 검사가 새로 만든 폴더만 끝에 지우며
이미 있는 경로는 건드리지 않고 거절한다.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import socket
import struct
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

ROWS = 1500
EXIT_USAGE = 2
TIMEOUT_EXIT = 124
TIMEOUT_SECONDS = 20
PAGE_TYPE_HEAP = 2
PAGE_TYPE_LEAF = 3
failures = 0


def check(msg: str, *, ok: bool) -> None:
    global failures
    if not ok:
        failures += 1
        print(f"FAIL: {msg}")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sanitizer_clean(text: str) -> bool:
    return "runtime error" not in text and "Sanitizer" not in text


def seed(binary: str, db: Path) -> None:
    text = (
        "CREATE TABLE t (name VARCHAR(16), age INT)\n"
        + "".join(f"INSERT INTO t VALUES ('n{i}', {i})\n" for i in range(ROWS))
        + ".exit\n"
    )
    done = subprocess.run(
        [binary, str(db)],
        input=text,
        text=True,
        capture_output=True,
        timeout=120,
        check=False,
    )
    assert done.returncode == 0, done.stderr


def run_limited(
    cmd: list[str], text: str | None = None
) -> subprocess.CompletedProcess[str]:
    """제한 시간 안에 끝나지 않으면 종료 코드 TIMEOUT_EXIT를 돌려준다(정지는 실패)."""
    try:
        return subprocess.run(
            cmd,
            input=text,
            text=True,
            capture_output=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, TIMEOUT_EXIT, "", "시간 초과: 정지")


def run_repl(binary: str, db: Path, text: str) -> subprocess.CompletedProcess[str]:
    return run_limited([binary, str(db)], text)


class Layout:
    """정상 파일에서 읽은 페이지 배치."""

    def __init__(self, data: bytes) -> None:
        self.page_size: int = struct.unpack_from("<I", data, 12)[0]
        count = len(data) // self.page_size
        types = {
            i: struct.unpack_from("<I", data, i * self.page_size)[0]
            for i in range(1, count)
        }
        self.heap = [i for i, t in types.items() if t == PAGE_TYPE_HEAP]
        self.leaf = [i for i, t in types.items() if t == PAGE_TYPE_LEAF]


Mutator = Callable[[bytearray, Layout], None]


def put32(b: bytearray, off: int, v: int) -> None:
    struct.pack_into("<I", b, off, v)


def put16(b: bytearray, off: int, v: int) -> None:
    struct.pack_into("<H", b, off, v)


def self_cycle(b: bytearray, lay: Layout) -> None:
    put32(b, lay.heap[0] * lay.page_size + 4, lay.heap[0])


def two_page_cycle(b: bytearray, lay: Layout) -> None:
    put32(b, lay.heap[0] * lay.page_size + 4, lay.heap[1])
    put32(b, lay.heap[1] * lay.page_size + 4, lay.heap[0])


def next_out_of_range(b: bytearray, lay: Layout) -> None:
    put32(b, lay.heap[0] * lay.page_size + 4, 99999)


def truncated_header(b: bytearray, lay: Layout) -> None:
    del lay
    del b[100:]


def header_page_only(b: bytearray, lay: Layout) -> None:
    del b[lay.page_size :]


def page_size_zero(b: bytearray, lay: Layout) -> None:
    del lay
    put32(b, 12, 0)


def page_size_half(b: bytearray, lay: Layout) -> None:
    put32(b, 12, lay.page_size // 2)


def column_count_40000(b: bytearray, lay: Layout) -> None:
    del lay
    put16(b, 48, 40000)


def column_count_17(b: bytearray, lay: Layout) -> None:
    del lay
    put16(b, 48, 17)


def slot_offset_out_of_page(b: bytearray, lay: Layout) -> None:
    put16(b, lay.heap[0] * lay.page_size + 16, 60000)


def heap_type_leaf(b: bytearray, lay: Layout) -> None:
    put32(b, lay.heap[0] * lay.page_size, PAGE_TYPE_LEAF)


def root_out_of_range(b: bytearray, lay: Layout) -> None:
    del lay
    put32(b, 16, 99999)


CASES: list[tuple[str, Mutator]] = [
    ("self_cycle", self_cycle),
    ("two_page_cycle", two_page_cycle),
    ("next_out_of_range", next_out_of_range),
    ("truncated_header", truncated_header),
    ("header_page_only", header_page_only),
    ("page_size_zero", page_size_zero),
    ("page_size_half", page_size_half),
    ("column_count_40000", column_count_40000),
    ("column_count_17", column_count_17),
    ("slot_offset_out_of_page", slot_offset_out_of_page),
    ("heap_type_leaf", heap_type_leaf),
    ("root_out_of_range", root_out_of_range),
]
ADOPT_CASES = {"self_cycle", "column_count_40000", "heap_type_leaf"}
SERVER_CASES = {"self_cycle", "two_page_cycle"}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port: int = s.getsockname()[1]
        return port


def check_good(binary: str, good: Path, label: str) -> None:
    res = run_repl(binary, good, "SELECT COUNT(*) FROM t\n.exit\n")
    check(
        f"{label}: 정상 파일이 열리고 행 수가 {ROWS}",
        ok=res.returncode == 0 and str(ROWS) in res.stdout.split("COUNT(*)")[-1],
    )


def test_corrupt_cases(binary: str, work: Path, good: Path) -> None:
    pristine = good.read_bytes()
    lay = Layout(pristine)
    check(
        "정상 파일에 힙 페이지가 둘 이상", ok=len(lay.heap) >= 2 and len(lay.leaf) >= 1
    )
    for name, mutate in CASES:
        bad = work / f"{name}.db"
        data = bytearray(pristine)
        mutate(data, lay)
        bad.write_bytes(bytes(data))
        before = digest(bad)

        res = run_repl(binary, bad, ".exit\n")
        out = res.stdout + res.stderr
        check(f"{name}: 종료 코드 1 (실제 {res.returncode})", ok=res.returncode == 1)
        check(f"{name}: EINVAL로 보고", ok="Invalid argument" in out)
        check(f"{name}: 읽기 실패로 보고하지 않음", ok="Input/output error" not in out)
        check(
            f"{name}: 손상 또는 미지원 사유를 출력",
            ok="손상된 데이터베이스 파일" in out or "지원하지 않는" in out,
        )
        check(f"{name}: sanitizer 진단 없음", ok=sanitizer_clean(out))
        check(f"{name}: 파일 바이트가 그대로", ok=digest(bad) == before)

        if name in ADOPT_CASES:
            adopt = run_limited([binary, "--adopt-table", "t", str(bad)])
            check(f"{name}: --adopt-table도 거절", ok=adopt.returncode == 1)
            check(
                f"{name}: --adopt-table이 파일을 바꾸지 않음", ok=digest(bad) == before
            )

        if name in SERVER_CASES:
            srv = run_limited([binary, "--server", str(free_port()), str(bad)])
            check(
                f"{name}: 서버가 시작하지 않고 종료 (실제 {srv.returncode})",
                ok=srv.returncode == 1,
            )
            check(
                f"{name}: 서버 시작 실패가 파일을 바꾸지 않음", ok=digest(bad) == before
            )

        check_good(binary, good, f"{name} 뒤")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minidb", required=True, help="minidb 바이너리")
    parser.add_argument(
        "--workdir",
        required=True,
        help="DB 파일을 둘 새 작업 폴더(아직 없어야 함, 빌드 폴더 아래 권장)",
    )
    args = parser.parse_args()
    binary = os.path.abspath(args.minidb)
    work = Path(args.workdir)
    try:
        # exist_ok=False: 이미 있는 경로(폴더·파일·심볼릭 링크)는 건드리지 않고 거절한다
        work.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print(f"거절: --workdir {work} 이(가) 이미 있습니다. 새 경로를 지정하세요.")
        return EXIT_USAGE
    work = work.resolve()
    try:
        good = work / "good.db"
        seed(binary, good)
        check_good(binary, good, "시작")
        test_corrupt_cases(binary, work, good)
    finally:
        # 이 실행이 방금 만든 폴더만 지운다
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("PASS: 손상 파일 열기 계약(REPL·--adopt-table·서버)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
