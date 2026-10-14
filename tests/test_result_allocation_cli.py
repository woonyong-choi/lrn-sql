"""REPL·HTTP의 출력 버퍼 할당 실패 전달과 소유권 검사 (#128).

MINIDB_TEST_HOOKS로 컴파일한 실제 minidb 바이너리를 환경 변수
MINIDB_TEST_FAIL_OUT_MALLOC / MINIDB_TEST_FAIL_OUT_REALLOC로 실패시킨다.
값은 프로세스 시작부터 센 n번째 출력 버퍼 malloc/realloc이다.
데이터는 모두 합성 데이터다.

소비 경로의 현행 계약:
  - REPL(src/main.c): out_buf가 NULL이면 출력하지 않고, 있으면 출력 뒤
    한 번 free한다. message는 항상 출력한다. 따라서 실패한 문장은 오류
    문구 한 줄만 보이고 결과 행은 없다.
  - HTTP(src/server/server.c): status -1이면 400과 message 본문.
    out_buf는 있을 때만 한 번 free한다.
  - db_execute()는 execute() 결과를 그대로 돌려주고 문장이 끝나면
    row lock을 모두 푼다.

검사 범위: 실패 문장의 오류 전달, 잘린 결과 없음, 실패 뒤 같은 세션·서버의
다음 문장 성공, 정상 종료, sanitizer 진단 없음. 실행 중 문장 취소 API는
현재 코드에 없으므로 취소 경로는 이 검사의 범위가 아니다(미구현이며
검사하지 않는다).

비용: 합성 행 300개를 가진 DB를 REPL 검사에서 한 번, HTTP 검사에서 사례(3개)마다
새로 만든다. 띄우는 minidb 프로세스는 REPL 검사에서 시드 1개와 실행 7개,
HTTP 검사에서 시드 4개와 서버 3개로 모두 15개다. macOS arm64 UBSan 빌드에서
전체가 약 0.6초였다(2026-10-15 한 번 측정).

작업 폴더: --workdir은 아직 없는 경로여야 한다. 이 검사가 새로 만든 폴더만
끝에 지우며, 이미 있는 경로는 내용을 건드리지 않고 거절한다.
"""

from __future__ import annotations

import argparse
import http.client
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROWS = 300
ERR = "결과 출력 버퍼 할당에 실패했습니다"
EXIT_USAGE = 2
failures = 0


def check(msg: str, *, ok: bool) -> None:
    global failures
    if not ok:
        failures += 1
        print(f"FAIL: {msg}")


def row_name(i: int) -> str:
    return f"row-{i:04d}-" + "x" * 48


def seed(binary: str, db: Path) -> None:
    text = (
        "CREATE TABLE t (name VARCHAR(64), age INT)\n"
        + "".join(f"INSERT INTO t VALUES ('{row_name(i)}', {i})\n" for i in range(ROWS))
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


def env_with(
    malloc_at: int | None = None, realloc_at: int | None = None
) -> dict[str, str]:
    env = dict(os.environ)
    env.pop("MINIDB_TEST_FAIL_OUT_MALLOC", None)
    env.pop("MINIDB_TEST_FAIL_OUT_REALLOC", None)
    if malloc_at:
        env["MINIDB_TEST_FAIL_OUT_MALLOC"] = str(malloc_at)
    if realloc_at:
        env["MINIDB_TEST_FAIL_OUT_REALLOC"] = str(realloc_at)
    return env


SESSION = (
    "SELECT * FROM t WHERE id = 5\n"  # 출력 버퍼 malloc 1번째
    "SELECT * FROM t WHERE age >= 0\n"  # malloc 2번째, 이어서 realloc 1~3번째
    "SELECT COUNT(*) FROM t\n"  # malloc 3번째
    ".exit\n"
)


def sanitizer_clean(text: str) -> bool:
    return "runtime error" not in text and "Sanitizer" not in text


def run_repl(
    binary: str,
    db: Path,
    malloc_at: int | None = None,
    realloc_at: int | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, str(db)],
        input=SESSION,
        text=True,
        capture_output=True,
        timeout=120,
        env=env_with(malloc_at, realloc_at),
        check=False,
    )


def test_repl(binary: str, work: Path) -> None:
    db = work / "repl.db"
    seed(binary, db)

    ok = run_repl(binary, db)
    check("REPL 대조군 정상 종료", ok=ok.returncode == 0 and "종료합니다." in ok.stdout)
    check(
        f"대조군 행 수 {1 + ROWS} (실제 {ok.stdout.count('row-')})",
        ok=ok.stdout.count("row-") == 1 + ROWS,
    )
    check("대조군에는 할당 오류가 없음", ok=ERR not in ok.stdout)
    check("대조군 COUNT 300", ok="300" in ok.stdout.split("COUNT(*)")[-1])

    cases: list[tuple[str, int | None, int | None]] = [
        ("malloc#2", 2, None),
        ("realloc#1", None, 1),
        ("realloc#2", None, 2),
        ("realloc#3", None, 3),
    ]
    for name, malloc_at, realloc_at in cases:
        res = run_repl(binary, db, malloc_at, realloc_at)
        check(
            f"REPL {name}: 오류 뒤에도 정상 종료",
            ok=res.returncode == 0 and "종료합니다." in res.stdout,
        )
        check(
            f"REPL {name}: 오류 문구가 한 번 출력 (실제 {res.stdout.count(ERR)})",
            ok=res.stdout.count(ERR) == 1,
        )
        check(
            f"REPL {name}: 실패한 SELECT의 결과 행이 없다 "
            f"(point SELECT 한 행만, 실제 {res.stdout.count('row-')})",
            ok=res.stdout.count("row-") == 1,
        )
        check(
            f"REPL {name}: 실패 뒤 다음 문장이 성공",
            ok="COUNT(*)" in res.stdout and "300" in res.stdout.split("COUNT(*)")[-1],
        )
        check(
            f"REPL {name}: sanitizer 진단 없음",
            ok=sanitizer_clean(res.stdout + res.stderr),
        )

    # 점 조회·COUNT의 첫 malloc 실패도 세션을 깨뜨리지 않는다
    for name, at in (("malloc#1", 1), ("malloc#3", 3)):
        res = run_repl(binary, db, malloc_at=at)
        check(
            f"REPL {name}: 오류 한 번, 정상 종료",
            ok=res.returncode == 0 and res.stdout.count(ERR) == 1,
        )
        check(
            f"REPL {name}: sanitizer 진단 없음",
            ok=sanitizer_clean(res.stdout + res.stderr),
        )


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port: int = s.getsockname()[1]
        return port


def post(port: int, sql: str) -> tuple[int, str]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    conn.request("POST", "/query", body=sql)
    resp = conn.getresponse()
    body = resp.read().decode("utf-8", "replace")
    conn.close()
    return resp.status, body


def serve(
    binary: str,
    db: Path,
    malloc_at: int | None = None,
    realloc_at: int | None = None,
) -> tuple[subprocess.Popen[str], int]:
    port = free_port()
    proc = subprocess.Popen(
        [binary, "--server", str(port), str(db)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env_with(malloc_at, realloc_at),
    )
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc, port
        except OSError:
            time.sleep(0.1)
    proc.kill()
    raise AssertionError("서버가 열리지 않음")


def stop(proc: subprocess.Popen[str]) -> tuple[str, str]:
    proc.send_signal(signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        check("서버가 SIGINT 뒤 종료하지 않음", ok=False)
    return out, err


def test_http(binary: str, work: Path) -> None:
    db = work / "http.db"
    seed(binary, db)
    scan = "SELECT * FROM t WHERE age >= 0"

    cases: list[tuple[str, int | None, int | None]] = [
        ("malloc#1", 1, None),
        ("realloc#1", None, 1),
        ("realloc#3", None, 3),
    ]
    for name, malloc_at, realloc_at in cases:
        proc, port = serve(binary, db, malloc_at, realloc_at)
        try:
            status, body = post(port, scan)
            check(f"HTTP {name}: 오류는 400 (실제 {status})", ok=status == 400)
            check(f"HTTP {name}: 본문에 오류 메시지", ok=ERR in body)
            check(f"HTTP {name}: 잘린 결과 행을 보내지 않는다", ok="row-" not in body)
            status, body = post(port, scan)
            check(
                f"HTTP {name}: 같은 서버의 다음 요청 성공 (실제 {status})",
                ok=status == 200 and "row-0000" in body,
            )
            status, body = post(port, "SELECT COUNT(*) FROM t")
            check(f"HTTP {name}: 이후 COUNT 성공", ok=status == 200 and "300" in body)
            status, body = post(port, "INSERT INTO t VALUES ('after', 1)")
            check(f"HTTP {name}: 이후 쓰기 성공 (실제 {status})", ok=status == 200)
            status, body = post(port, "SELECT COUNT(*) FROM t")
            check(
                f"HTTP {name}: row lock이 남지 않아 쓰기 뒤 읽기 성공",
                ok=status == 200 and "301" in body,
            )
        finally:
            out, err = stop(proc)
        check(
            f"HTTP {name}: 서버 정상 종료 (실제 {proc.returncode})",
            ok=proc.returncode == 0,
        )
        check(f"HTTP {name}: sanitizer 진단 없음", ok=sanitizer_clean(out + err))
        db.unlink(missing_ok=True)
        seed(binary, db)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--minidb", required=True, help="MINIDB_TEST_HOOKS로 빌드한 minidb"
    )
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
        test_repl(binary, work)
        test_http(binary, work)
    finally:
        # 이 실행이 방금 만든 폴더만 지운다
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("PASS: REPL·HTTP의 출력 버퍼 할당 실패 전달")
    return 0


if __name__ == "__main__":
    sys.exit(main())
