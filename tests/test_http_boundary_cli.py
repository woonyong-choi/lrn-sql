#!/usr/bin/env python3
"""#126: 실제 minidb 서버가 HTTP 본문 길이 경계에서 부분 실행하지 않고 재시작 뒤에도 원본이 같다.

실제 minidb 바이너리를 --server로 띄우고 raw TCP로 요청을 보낸다. 검사 순서:
  1. 서버를 띄워 합성 테이블과 두 행을 만들고 SIGINT로 정상 종료한다. 이때의 DB 파일 해시와 행을 기준으로 삼는다.
  2. 같은 파일로 서버를 다시 띄워 거절되어야 하는 요청만 보낸다.
       - 4096바이트 이상(413), 한도 안의 나쁜 꼬리(400), 한글이 한도에 걸린 본문(400/413),
         한글 첫 바이트들로 끝나는 본문(400). 앞부분은 유효한 INSERT·UPDATE·DELETE다.
       - 거절된 요청 바로 뒤에 같은 연결로 정상 UPDATE를 이어 보낸다.
         413은 연결을 닫아 그 UPDATE가 실행되지 않고, 400은 연결을 유지해 UPDATE가 한 번 실행된다.
         그래서 정상 UPDATE는 맨 마지막 단계에서 따로 보낸다.
     종료 뒤 DB 파일 해시와 새 프로세스로 읽은 행이 1단계와 같아야 한다(서버 재시작 뒤 원본 동일).
  3. 서버를 다시 띄워 4094·4095바이트의 유효한 문장을 문장 가운데를 공백으로 채워 보낸다(200).
     HTTP 경로는 본문 끝의 공백·세미콜론을 잘라내므로 끝 공백으로는 parser가 한도 길이를 보지 못한다.
     종료 뒤 새 프로세스로 읽은 행에 그 효과가 남아 있어야 한다.
  4. 한도 안의 SQL 오류(400) 뒤 같은 연결의 정상 문장은 한 번 실행되고, 413 뒤 같은 연결의 정상 문장은
     실행되지 않는다. 결과는 재시작 뒤 행으로 확인한다.

작업 폴더: --workdir은 아직 없는 경로여야 한다. 이 검사가 새로 만든 폴더만 끝에 지우며 이미 있는 경로는 거절한다.
모든 데이터는 합성 데이터다. 임시 시스템 경로는 쓰지 않는다.

비용: minidb 서버 4회와 REPL 4회를 띄운다. 요청은 서버당 최대 60개 안팎이며 모두 loopback이다.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

LIMIT = 4095  # include/server/http.h의 HTTP_MAX_BODY_BYTES와 include/sql/parser.h의 PARSER_MAX_INPUT_LEN
HANGUL = "가".encode()  # 3바이트
FAILURES: list[str] = []


def check(ok: bool, label: str) -> None:
    if not ok:
        FAILURES.append(label)
        print(f"  FAIL: {label}")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, binary: str, db: Path) -> None:
        self.port = free_port()
        self.proc = subprocess.Popen(
            [binary, "--server", str(self.port), str(db)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return
            except OSError:
                time.sleep(0.1)
        self.proc.kill()
        raise RuntimeError("서버가 열리지 않음")

    def stop(self) -> None:
        self.proc.send_signal(signal.SIGINT)
        try:
            _, err = self.proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            _, err = self.proc.communicate()
            check(False, "서버가 SIGINT 뒤 종료하지 않음")
            return
        check(self.proc.returncode == 0, f"서버 정상 종료 코드 0 (실제 {self.proc.returncode})")
        check("[server] done" in err, "서버가 종료 완료를 알림")


class Client:
    """raw TCP 연결 하나. 한 번의 recv에 응답이 둘 이상 들어와도 남은 바이트를 버리지 않는다."""

    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, data: bytes) -> None:
        try:
            self.sock.sendall(data)
        except OSError:
            pass  # 서버가 거절하고 먼저 닫은 연결이다. 응답은 read_response에서 읽는다

    def _fill(self) -> bool:
        try:
            chunk = self.sock.recv(65536)
        except OSError:
            return False
        if not chunk:
            return False
        self.buf += chunk
        return True

    def read_response(self) -> int:
        """응답 하나를 읽어 상태를 돌려준다. 읽지 못하면 0."""
        while b"\r\n\r\n" not in self.buf:
            if not self._fill():
                return 0
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        status = int(head.split(b" ", 2)[1])
        match = re.search(rb"(?i)content-length:\s*(\d+)", head)
        length = int(match.group(1)) if match else 0
        while len(rest) < length:
            self.buf = head + b"\r\n\r\n" + rest
            if not self._fill():
                return 0
            head, _, rest = self.buf.partition(b"\r\n\r\n")
        self.buf = rest[length:]
        return status

    def closed(self) -> bool:
        """응답을 모두 읽은 뒤 서버가 연결을 닫았는지 본다."""
        if self.buf:
            return False
        try:
            return self.sock.recv(1) == b""
        except OSError:
            return False

    def close(self) -> None:
        self.sock.close()


def post(sql: bytes) -> bytes:
    return b"POST /query HTTP/1.1\r\nHost: t\r\nContent-Length: " + str(len(sql)).encode() + b"\r\n\r\n" + sql


def request(port: int, sql: bytes) -> int:
    client = Client(port)
    try:
        client.send(post(sql))
        return client.read_response()
    finally:
        client.close()


def pad_tail(head: str, total: int, tail: bytes = b"") -> bytes:
    h = head.encode()
    assert total >= len(h) + len(tail)
    return h + b" " * (total - len(h) - len(tail)) + tail


def pad_inner(before: str, after: str, total: int) -> bytes:
    b, a = before.encode(), after.encode()
    assert total >= len(b) + len(a) + 1
    return b + b" " * (total - len(b) - len(a)) + a


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(binary: str, db: Path) -> list[str]:
    """새 프로세스로 DB를 다시 열어 행을 읽는다."""
    done = subprocess.run(
        [binary, str(db)],
        input=b"SELECT * FROM users\n.exit\n",
        capture_output=True,
        timeout=60,
        check=False,
    )
    check(done.returncode == 0, f"REPL 재열기 종료 코드 0 (실제 {done.returncode})")
    out = done.stdout.decode("utf-8", errors="replace")
    return [line for line in out.splitlines() if re.match(r"^\s*\d+\s", line)]


KINDS = {
    "INSERT": ("INSERT INTO users VALUES ('mallory', 99)", "INSERT INTO users VALUES", "('mallory', 99)"),
    "UPDATE": ("UPDATE users SET name = 'hijacked' WHERE id = 1", "UPDATE users SET name = 'hijacked'", "WHERE id = 1"),
    "DELETE": ("DELETE FROM users WHERE id = 1", "DELETE FROM users", "WHERE id = 1"),
}
FOLLOW_SQL = b"UPDATE users SET age = 77 WHERE id = 2"


def seed(binary: str, db: Path) -> None:
    server = Server(binary, db)
    try:
        for sql in (
            "CREATE TABLE users (name VARCHAR(32), age INT)",
            "INSERT INTO users VALUES ('alice', 20)",
            "INSERT INTO users VALUES ('bob', 30)",
        ):
            check(request(server.port, sql.encode()) == 200, f"시드 {sql}")
    finally:
        server.stop()


def send_rejected(port: int) -> None:
    """한 번도 실행되면 안 되는 요청을 모두 보낸다."""
    for name, (head, _, _) in KINDS.items():
        for total in (LIMIT + 1, LIMIT + 2, LIMIT + 17, 2 * LIMIT + 50):
            for tail in (b"", b"ZZZ", b"; DELETE FROM users"):
                label = f"{name} over limit {total} bytes tail {tail!r}"
                check(request(port, pad_tail(head, total, tail)) == 413, label)
        for total in (LIMIT - 1, LIMIT):
            for tail in (b"ZZZ", b"; DELETE FROM users"):
                label = f"{name} bad tail ending at {total} bytes tail {tail!r}"
                check(request(port, pad_tail(head, total, tail)) == 400, label)

    head = KINDS["UPDATE"][0]
    for end in range(LIMIT - 1, LIMIT + 4):
        label = f"hangul tail ending at byte {end}"
        status = request(port, pad_tail(head, end, HANGUL))
        check(status == (400 if end <= LIMIT else 413), f"{label} (상태 {status})")
    for cut in (HANGUL[:1], HANGUL[:2]):
        check(request(port, pad_tail(head, LIMIT, cut)) == 400, f"hangul cut after {len(cut)} bytes at the limit")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--minidb", required=True)
    ap.add_argument("--workdir", required=True)
    args = ap.parse_args()
    work = Path(args.workdir)
    if work.exists():
        print(f"작업 폴더가 이미 있다: {work}")
        return 2
    work.mkdir(parents=True)
    db = work / "boundary.db"
    try:
        print("[1] 시드와 기준")
        seed(args.minidb, db)
        base_hash = sha256(db)
        base_rows = rows(args.minidb, db)
        check(len(base_rows) == 2, f"기준 행 2개 (실제 {len(base_rows)})")
        check(sha256(db) == base_hash, "REPL 재열기가 파일 바이트를 바꾸지 않음")

        print("[2] 거절 요청만 보낸 뒤 재시작")
        server = Server(args.minidb, db)
        try:
            send_rejected(server.port)
        finally:
            server.stop()
        check(sha256(db) == base_hash, "거절 요청과 서버 종료 뒤 DB 파일 바이트가 기준과 같다")
        check(rows(args.minidb, db) == base_rows, "거절 요청 뒤 재열기 행이 기준과 같다")

        print("[3] 한도 길이의 유효한 문장")
        server = Server(args.minidb, db)
        try:
            for total in (LIMIT - 1, LIMIT):
                check(request(server.port, pad_inner(KINDS["UPDATE"][1], KINDS["UPDATE"][2], total)) == 200,
                      f"UPDATE {total} bytes 실행")
                check(request(server.port, pad_inner(KINDS["INSERT"][1], KINDS["INSERT"][2], total)) == 200,
                      f"INSERT {total} bytes 실행")
        finally:
            server.stop()
        after = rows(args.minidb, db)
        check(any("hijacked" in r for r in after), "4095바이트 UPDATE가 재시작 뒤에도 남음")
        check(sum("mallory" in r for r in after) == 2, "4094·4095바이트 INSERT가 각각 한 번씩 남음")
        check(not any("alice" in r for r in after), "UPDATE가 id 1 행에 적용됨")

        print("[4] 거절 뒤 같은 연결의 정상 문장")
        server = Server(args.minidb, db)
        try:
            # 413: 연결이 닫히고 뒤따르는 UPDATE는 실행되지 않는다
            client = Client(server.port)
            client.send(post(pad_tail(KINDS["DELETE"][0], LIMIT + 1)) + post(FOLLOW_SQL))
            check(client.read_response() == 413, "413 응답")
            check(client.read_response() == 0, "413 뒤 같은 연결의 정상 문장은 응답받지 못함")
            check(client.closed(), "413 뒤 서버가 연결을 닫음")
            client.close()
            # 400: 연결이 유지되고 뒤따르는 UPDATE는 한 번 실행된다
            client = Client(server.port)
            client.send(post(pad_tail(KINDS["DELETE"][0], LIMIT, b"ZZZ")) + post(FOLLOW_SQL))
            check(client.read_response() == 400, "400 응답")
            check(client.read_response() == 200, "400 뒤 같은 연결의 정상 문장은 한 번 실행됨")
            client.close()
        finally:
            server.stop()
        final = rows(args.minidb, db)
        check(len([r for r in final if "77" in r]) == 1, "정상 UPDATE가 정확히 한 행에만, 한 번 적용됨")
        check(len(final) == len(after), "거절된 DELETE·INSERT는 행 수를 바꾸지 않음")
    finally:
        shutil.rmtree(work)

    if FAILURES:
        print(f"\nhttp boundary cli: {len(FAILURES)} failed")
        return 1
    print("\nhttp boundary cli: 0 failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
