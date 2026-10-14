#!/usr/bin/env python3
"""REPL과 HTTP 서버가 출력 버퍼 할당 실패를 전달하고 소유권을 지키는지 검사 (#128).

MINIDB_TEST_HOOKS로 컴파일한 실제 minidb 바이너리를 환경 변수
MINIDB_TEST_FAIL_OUT_MALLOC / MINIDB_TEST_FAIL_OUT_REALLOC(프로세스 시작부터 센 n번째 출력 버퍼
malloc/realloc)으로 실패시킨다. 데이터는 모두 합성 데이터다.

소비 경로의 현행 계약:
  - REPL(src/main.c): out_buf가 NULL이면 출력하지 않고, 있으면 출력 뒤 한 번 free한다.
    message는 항상 출력한다. 따라서 실패한 문장은 오류 문구 한 줄만 보이고 결과 행은 없다.
  - HTTP(src/server/server.c): status -1이면 400과 message 본문. out_buf는 있을 때만 한 번 free한다.
  - db_execute()는 execute() 결과를 그대로 돌려주고 문장이 끝나면 row lock을 모두 푼다.

검사 범위: 실패 문장의 오류 전달, 잘린 결과 없음, 실패 뒤 같은 연결·세션의 다음 문장 성공,
정상 종료, sanitizer 진단 없음. 실행 중 문장 취소 API는 현재 코드에 없으므로 취소 경로는 이 검사의
범위가 아니다(미구현이며 검사하지 않는다).
"""

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
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL: {msg}")


def row_name(i):
    return f"row-{i:04d}-" + "x" * 48


def seed(binary, db):
    text = "CREATE TABLE t (name VARCHAR(64), age INT)\n" + "".join(
        f"INSERT INTO t VALUES ('{row_name(i)}', {i})\n" for i in range(ROWS)) + ".exit\n"
    done = subprocess.run([binary, str(db)], input=text, text=True, capture_output=True,
                          timeout=120, check=False)
    assert done.returncode == 0, done.stderr


def env_with(malloc_at=None, realloc_at=None):
    env = dict(os.environ)
    env.pop("MINIDB_TEST_FAIL_OUT_MALLOC", None)
    env.pop("MINIDB_TEST_FAIL_OUT_REALLOC", None)
    if malloc_at:
        env["MINIDB_TEST_FAIL_OUT_MALLOC"] = str(malloc_at)
    if realloc_at:
        env["MINIDB_TEST_FAIL_OUT_REALLOC"] = str(realloc_at)
    return env


SESSION = (
    "SELECT * FROM t WHERE id = 5\n"        # 출력 버퍼 malloc 1번째
    "SELECT * FROM t WHERE age >= 0\n"      # malloc 2번째, 이어서 realloc 1~3번째
    "SELECT COUNT(*) FROM t\n"              # malloc 3번째
    ".exit\n"
)


def sanitizer_clean(text):
    return "runtime error" not in text and "Sanitizer" not in text


def run_repl(binary, db, malloc_at=None, realloc_at=None):
    return subprocess.run([binary, str(db)], input=SESSION, text=True, capture_output=True,
                          timeout=120, env=env_with(malloc_at, realloc_at), check=False)


def test_repl(binary, work):
    db = work / "repl.db"
    seed(binary, db)

    ok = run_repl(binary, db)
    check(ok.returncode == 0 and "종료합니다." in ok.stdout, "REPL 대조군 정상 종료")
    check(ok.stdout.count("row-") == 1 + ROWS, f"대조군 행 수 {1 + ROWS} (실제 {ok.stdout.count('row-')})")
    check(ERR not in ok.stdout, "대조군에는 할당 오류가 없음")
    check("300" in ok.stdout.split("COUNT(*)")[-1], "대조군 COUNT 300")

    cases = [("malloc#2", dict(malloc_at=2))] + [(f"realloc#{k}", dict(realloc_at=k)) for k in (1, 2, 3)]
    for name, kw in cases:
        res = run_repl(binary, db, **kw)
        check(res.returncode == 0 and "종료합니다." in res.stdout, f"REPL {name}: 오류 뒤에도 정상 종료")
        check(res.stdout.count(ERR) == 1, f"REPL {name}: 오류 문구가 한 번 출력 (실제 {res.stdout.count(ERR)})")
        check(res.stdout.count("row-") == 1,
              f"REPL {name}: 실패한 SELECT의 결과 행이 없다 (point SELECT 한 행만, 실제 {res.stdout.count('row-')})")
        check("COUNT(*)" in res.stdout and "300" in res.stdout.split("COUNT(*)")[-1],
              f"REPL {name}: 실패 뒤 다음 문장이 성공")
        check(sanitizer_clean(res.stdout + res.stderr), f"REPL {name}: sanitizer 진단 없음")

    # 점 조회·COUNT의 첫 malloc 실패도 세션을 깨뜨리지 않는다
    for name, at in (("malloc#1", 1), ("malloc#3", 3)):
        res = run_repl(binary, db, malloc_at=at)
        check(res.returncode == 0 and res.stdout.count(ERR) == 1, f"REPL {name}: 오류 한 번, 정상 종료")
        check(sanitizer_clean(res.stdout + res.stderr), f"REPL {name}: sanitizer 진단 없음")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def post(port, sql):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    conn.request("POST", "/query", body=sql)
    resp = conn.getresponse()
    body = resp.read().decode("utf-8", "replace")
    conn.close()
    return resp.status, body


def serve(binary, db, **kw):
    port = free_port()
    proc = subprocess.Popen([binary, "--server", str(port), str(db)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, env=env_with(**kw))
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc, port
        except OSError:
            time.sleep(0.1)
    proc.kill()
    raise AssertionError("서버가 열리지 않음")


def stop(proc):
    proc.send_signal(signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        check(False, "서버가 SIGINT 뒤 종료하지 않음")
    return out, err


def test_http(binary, work):
    db = work / "http.db"
    seed(binary, db)
    scan = "SELECT * FROM t WHERE age >= 0"

    cases = [("malloc#1", dict(malloc_at=1)), ("realloc#1", dict(realloc_at=1)),
             ("realloc#3", dict(realloc_at=3))]
    for name, kw in cases:
        proc, port = serve(binary, db, **kw)
        try:
            status, body = post(port, scan)
            check(status == 400, f"HTTP {name}: 오류는 400 (실제 {status})")
            check(ERR in body, f"HTTP {name}: 본문에 오류 메시지")
            check("row-" not in body, f"HTTP {name}: 잘린 결과 행을 보내지 않는다")
            status, body = post(port, scan)
            check(status == 200 and "row-0000" in body, f"HTTP {name}: 같은 서버의 다음 요청 성공 (실제 {status})")
            status, body = post(port, "SELECT COUNT(*) FROM t")
            check(status == 200 and "300" in body, f"HTTP {name}: 이후 COUNT 성공")
            status, body = post(port, "INSERT INTO t VALUES ('after', 1)")
            check(status == 200, f"HTTP {name}: 이후 쓰기 성공 (실제 {status})")
            status, body = post(port, "SELECT COUNT(*) FROM t")
            check(status == 200 and "301" in body, f"HTTP {name}: row lock이 남지 않아 쓰기 뒤 읽기 성공")
        finally:
            out, err = stop(proc)
        check(proc.returncode == 0, f"HTTP {name}: 서버 정상 종료 (실제 {proc.returncode})")
        check(sanitizer_clean(out + err), f"HTTP {name}: sanitizer 진단 없음")
        db.unlink(missing_ok=True)
        seed(binary, db)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minidb", required=True, help="MINIDB_TEST_HOOKS로 빌드한 minidb")
    parser.add_argument("--workdir", required=True, help="DB 파일을 둘 작업 폴더(빌드 폴더 아래)")
    args = parser.parse_args()
    binary = os.path.abspath(args.minidb)
    work = Path(args.workdir).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        test_repl(binary, work)
        test_http(binary, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("PASS: REPL·HTTP의 출력 버퍼 할당 실패 전달")
    return 0


if __name__ == "__main__":
    sys.exit(main())
