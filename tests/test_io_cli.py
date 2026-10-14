#!/usr/bin/env python3
"""REPL과 서버의 디스크 I/O 오류 종료 계약 검사 (#78).

실제 minidb 바이너리를 합성 DB 파일로 실행한다. 쓰기 실패는 RLIMIT_FSIZE로 만든다.
파일 크기 한도를 넘는 pwrite는 EFBIG로 실패하므로 flush·종료가 실제로 실패한다.

계약:
  - `.flush` 실패는 성공 문구를 출력하지 않고 오류를 출력한다.
  - `.exit`/EOF 종료에서 pager_close가 실패하면 종료 코드 1, "종료합니다."를 출력하지 않는다.
  - 서버 종료에서 pager_close가 실패하면 종료 코드 1, "[server] done"을 출력하지 않는다.
  - 다른 프로세스가 열고 있는 파일은 열기 오류와 종료 코드 1로 거부한다.
  - 마지막 페이지가 잘린 파일은 열 때 손상으로 거절되며 죽지 않는다(#130).

한계: 이 검사는 쓰기 실패와 EOF만 다룬다. 읽기 EIO 주입은 tests/test_io.c가, 손상 파일 열기는
tests/test_corrupt_open.c와 test_corrupt_open_cli.py가 맡는다.
"""

import argparse
import http.client
import os
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROWS = 1000  # 행 28바이트 기준 힙 페이지 하나(16KB 약 450행)를 넘겨 새 페이지 할당을 일으킨다
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL: {msg}")


def limited(limit):
    """RLIMIT_FSIZE를 걸고 SIGXFSZ는 무시해 EFBIG가 반환값으로 나오게 한다."""
    def apply():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        if limit is not None:
            resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
    return apply


def run(binary, db, text, limit=None):
    return subprocess.run(
        [binary, str(db)], input=text, text=True, capture_output=True,
        timeout=60, preexec_fn=limited(limit), check=False)


def build_db(binary, path):
    seed = "CREATE TABLE t (name VARCHAR(16), age INT)\n" + "".join(
        f"INSERT INTO t VALUES ('s{i}', {i})\n" for i in range(5)) + ".exit\n"
    done = run(binary, path, seed)
    assert done.returncode == 0, done.stderr


def inserts():
    return "".join(f"INSERT INTO t VALUES ('x{i}', {i})\n" for i in range(ROWS))


def test_repl_flush_and_exit(binary, tmp):
    db = tmp / "repl.db"
    build_db(binary, db)

    control = tmp / "repl_control.db"
    build_db(binary, control)
    ok = run(binary, control, inserts() + ".flush\n.exit\n")
    check(ok.returncode == 0 and "종료합니다." in ok.stdout
          and "모든 dirty 페이지를 디스크에 기록했습니다." in ok.stdout,
          "한도가 없으면 flush·종료 성공 (대조군)")

    size = db.stat().st_size
    head = db.read_bytes()[:256]
    bad = run(binary, db, inserts() + ".flush\n.exit\n", limit=size)
    check("오류: 디스크에 기록하지 못했습니다" in bad.stdout, ".flush 실패를 오류로 출력")
    check("모든 dirty 페이지를 디스크에 기록했습니다." not in bad.stdout,
          ".flush 실패에서 성공 문구를 출력하지 않음")
    check(bad.returncode == 1, f"종료 flush 실패는 종료 코드 1 (실제 {bad.returncode})")
    check("변경 사항을 디스크에 기록하지 못했습니다" in bad.stderr, "종료 실패를 stderr에 알림")
    check("종료합니다." not in bad.stdout, "종료 실패에서 정상 종료 문구를 출력하지 않음")
    check(db.read_bytes()[:256] == head, "쓰기 실패 시 디스크 헤더는 그대로")



def test_second_open_refused(binary, tmp):
    db = tmp / "lock.db"
    build_db(binary, db)
    holder = subprocess.Popen([binary, str(db)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True)
    try:
        assert "연결됨" in holder.stdout.readline()
        other = run(binary, db, ".exit\n")
        check(other.returncode == 1, f"열려 있는 파일은 종료 코드 1로 거부 (실제 {other.returncode})")
        check("열 수 없습니다" in other.stderr, "열기 거부를 stderr에 알림")
    finally:
        holder.stdin.write(".exit\n")
        holder.stdin.close()
        holder.wait(timeout=30)
    after = run(binary, db, "SELECT * FROM t WHERE age >= 0\n.exit\n")
    check(after.returncode == 0, "첫 프로세스가 끝난 뒤에는 열림")


def test_pages_survives_unreadable_page(binary, tmp):
    """마지막 페이지가 잘린 파일은 열 때 손상(#130)으로 거절되고 죽지 않는다.

    열린 뒤 읽을 수 없게 되는 페이지는 tests/test_io.c의 읽기 오류 주입이 맡는다.
    """
    db = tmp / "pages.db"
    build_db(binary, db)
    size = db.stat().st_size
    with open(db, "r+b") as f:  # 3페이지(헤더·힙·리프) 중 마지막 리프를 잘라 낸다
        f.truncate(size // 3 * 2)
    head = db.read_bytes()
    res = run(binary, db, ".pages\n.exit\n")
    check("손상된 데이터베이스 파일입니다" in res.stderr, "잘린 파일을 손상으로 거절")
    check(res.returncode == 1, f"손상 거절은 종료 코드 1, 시그널로 죽지 않음 (실제 {res.returncode})")
    check(db.read_bytes() == head, "거절한 파일은 바이트가 그대로")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_server_shutdown(binary, tmp):
    db = tmp / "server.db"
    build_db(binary, db)
    size = db.stat().st_size
    port = free_port()
    proc = subprocess.Popen([binary, "--server", str(port), str(db)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            preexec_fn=limited(size))
    try:
        conn = None
        for _ in range(100):
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                conn.connect()
                break
            except OSError:
                time.sleep(0.1)
        assert conn is not None, "서버가 열리지 않음"
        for i in range(ROWS):
            conn.request("POST", "/query", body=f"INSERT INTO t VALUES ('h{i}', {i})")
            conn.getresponse().read()
        conn.close()
    finally:
        proc.send_signal(signal.SIGINT)
    try:
        _, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, err = proc.communicate()
        check(False, "서버가 SIGINT 뒤 종료하지 않음")
    check(proc.returncode == 1, f"서버 종료 flush 실패는 종료 코드 1 (실제 {proc.returncode})")
    check("디스크에 기록하지 못했습니다" in err, "서버 종료 실패를 stderr에 알림")
    check("[server] done" not in err, "서버 종료 실패에서 done을 출력하지 않음")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minidb", required=True)
    args = parser.parse_args()
    binary = os.path.abspath(args.minidb)
    with tempfile.TemporaryDirectory(prefix="lrn_sql_io_cli_") as t:
        tmp = Path(t)
        test_repl_flush_and_exit(binary, tmp)
        test_second_open_refused(binary, tmp)
        test_pages_survives_unreadable_page(binary, tmp)
        test_server_shutdown(binary, tmp)
    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("PASS: REPL·서버의 I/O 오류 종료 계약")
    return 0


if __name__ == "__main__":
    sys.exit(main())
