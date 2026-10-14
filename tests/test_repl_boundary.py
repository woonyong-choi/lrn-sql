#!/usr/bin/env python3
"""#126: REPL은 한도를 넘는 한 줄을 통째로 거절·폐기하고 뒤 정상 문장은 독립 실행한다."""

import argparse
import os
import re
import subprocess
import sys

DB_PATH = "__test_repl_boundary.db"
LIMIT = 4095  # include/sql/parser.h의 PARSER_MAX_INPUT_LEN
HANGUL = "가"  # UTF-8 3바이트

FAILURES = []


def check(condition: bool, label: str) -> None:
    if not condition:
        FAILURES.append(label)
        print(f"  FAIL: {label}")


def run_repl(binary: str, stdin_bytes: bytes) -> str:
    done = subprocess.run(
        [binary, DB_PATH],
        input=stdin_bytes + b"\n.exit\n",
        capture_output=True,
        timeout=60,
        check=False,
    )
    return done.stdout.decode("utf-8", errors="replace")


def sql_lines(*statements: str) -> bytes:
    return "".join(f"{s}\n" for s in statements).encode("utf-8")


def rows(binary: str) -> list:
    """새 프로세스로 DB를 다시 열어 행을 읽는다."""
    out = run_repl(binary, sql_lines("SELECT * FROM users"))
    return [line for line in out.splitlines() if re.match(r"^\s*\d+\s", line)]


def fresh_db(binary: str) -> None:
    for suffix in ("", "-journal"):
        if os.path.exists(DB_PATH + suffix):
            os.remove(DB_PATH + suffix)
    run_repl(
        binary,
        sql_lines(
            "CREATE TABLE users (name VARCHAR(32), age INT)",
            "INSERT INTO users VALUES ('alice', 20)",
            "INSERT INTO users VALUES ('bob', 30)",
        ),
    )


def pad_to(head: str, total_bytes: int, tail: str = "") -> bytes:
    """head 뒤를 공백으로 채워 tail을 포함한 전체를 total_bytes로 맞춘다."""
    head_b, tail_b = head.encode(), tail.encode()
    gap = total_bytes - len(head_b) - len(tail_b)
    assert gap >= 0
    return head_b + b" " * gap + tail_b


HEADS = {
    "INSERT": "INSERT INTO users VALUES ('mallory', 99)",
    "UPDATE": "UPDATE users SET name = 'hijacked' WHERE id = 1",
    "DELETE": "DELETE FROM users WHERE id = 1",
}
NEXT_STATEMENT = "UPDATE users SET age = 77 WHERE id = 2"


def case_boundary_with_valid_tail(binary: str) -> None:
    """4094·4095바이트의 유효한 문장(끝 공백)은 실행하고 4096바이트 이상은 거절한다."""
    for total, accepted in ((LIMIT - 1, True), (LIMIT, True), (LIMIT + 1, False)):
        fresh_db(binary)
        line = pad_to(HEADS["UPDATE"], total)
        out = run_repl(binary, line + b"\n" + sql_lines(NEXT_STATEMENT))
        after = rows(binary)
        label = f"{total} bytes valid statement"
        # 줄 끝 공백은 REPL이 잘라내므로 한도 안이면 실행한다
        check(("hijacked" in " ".join(after)) == accepted, f"{label}: applied={accepted}")
        check(any("77" in row for row in after), f"{label}: next statement ran whole")
        check("너무 깁니다" in out or accepted, f"{label}: rejection reported")


def case_overlong_line_discarded(binary: str) -> None:
    """한도를 넘는 줄은 앞부분이 유효해도 실행하지 않고, 남은 꼬리가 다음 문장이 되지 않는다."""
    tails = ["DELETE FROM users", "; DELETE FROM users", "ZZZ"]
    for name, head in HEADS.items():
        for total in (LIMIT + 1, LIMIT + 2, LIMIT + 17, 2 * LIMIT + 50):
            for tail in tails:
                fresh_db(binary)
                before = rows(binary)
                # 꼬리는 4095바이트 지점 바로 뒤에서 시작해 옛 분할 지점과 겹친다
                line = pad_to(head, max(total, LIMIT + len(tail)), tail)
                out = run_repl(binary, line + b"\n" + sql_lines(NEXT_STATEMENT))
                after = rows(binary)
                label = f"{name} +{tail!r} at {len(line)} bytes"
                check("너무 깁니다" in out, f"{label}: rejection reported")
                check(
                    not any("mallory" in r or "hijacked" in r for r in after),
                    f"{label}: head not executed",
                )
                check(len(after) == len(before), f"{label}: no row added or deleted")
                check(any("77" in r for r in after), f"{label}: next statement ran whole")


def case_utf8_boundary(binary: str) -> None:
    """다중 바이트 문자가 4095·4096바이트 경계에 걸려도 조각나 실행되지 않는다."""
    head = "UPDATE users SET name = '" + HANGUL + "' WHERE id = 1"
    for end in (LIMIT - 1, LIMIT, LIMIT + 1, LIMIT + 2):
        fresh_db(binary)
        before = rows(binary)
        line = pad_to(head, end, HANGUL)
        out = run_repl(binary, line + b"\n" + sql_lines(NEXT_STATEMENT))
        after = rows(binary)
        label = f"hangul ending at byte {end}"
        check(len(after) == len(before), f"{label}: row count unchanged")
        check(not any(HANGUL in r for r in after), f"{label}: statement not applied")
        check(any("77" in r for r in after), f"{label}: next statement ran whole")
        if end > LIMIT:
            check("너무 깁니다" in out, f"{label}: rejection reported")


def case_nul_byte(binary: str) -> None:
    """NUL 바이트 뒤가 조용히 잘려 앞부분만 실행되지 않는다."""
    fresh_db(binary)
    before = rows(binary)
    line = b"INSERT INTO users VALUES ('nul', 1)\x00; DELETE FROM users"
    out = run_repl(binary, line + b"\n" + sql_lines(NEXT_STATEMENT))
    after = rows(binary)
    check("NUL" in out, "NUL line: rejection reported")
    check(len(after) == len(before), "NUL line: row count unchanged")
    check(any("77" in r for r in after), "NUL line: next statement ran whole")


def case_crlf_and_eof_without_newline(binary: str) -> None:
    """CRLF 입력과 개행 없는 마지막 줄은 한도 안이면 그대로 실행한다."""
    fresh_db(binary)
    run_repl(binary, pad_to(HEADS["INSERT"], LIMIT) + b"\r\n")
    after = rows(binary)
    check(any("mallory" in r for r in after), "4095 bytes with CRLF accepted")
    done = subprocess.run(
        [binary, DB_PATH],
        input=b"UPDATE users SET age = 55 WHERE id = 2",
        capture_output=True,
        timeout=60,
        check=False,
    )
    check(done.returncode == 0, "last line without newline accepted")
    check(any("55" in r for r in rows(binary)), "last line without newline executed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minidb", required=True)
    args = parser.parse_args()
    try:
        for case in (
            case_boundary_with_valid_tail,
            case_overlong_line_discarded,
            case_utf8_boundary,
            case_nul_byte,
            case_crlf_and_eof_without_newline,
        ):
            print(case.__name__)
            case(args.minidb)
    finally:
        if os.path.exists(DB_PATH):
            os.remove(DB_PATH)
    print(f"repl boundary: {len(FAILURES)} failed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
