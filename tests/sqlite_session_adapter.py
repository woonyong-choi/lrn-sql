#!/usr/bin/env python3
"""#103 차등 검사의 SQLite 쪽 어댑터. 한 줄 명령으로 연결을 유지하며 실행하고 결과를 줄로 돌려준다.

프로토콜은 tests/session_probe.c와 같다(HELLO, OPEN, SESSION, EXEC, DROP, CLOSE, QUIT).
연결마다 자동 commit 모드(isolation_level=None)로 열어 BEGIN·COMMIT·ROLLBACK을 문장 그대로
실행하고 busy timeout은 0이다. SQLite 버전은 HELLO로 알리며 허용 판정은 하네스가 한다.
"""

from __future__ import annotations

import os
import sqlite3
import sys
from typing import Any

from sqlite_session_protocol import (
    Cell,
    bits_real,
    decode_cell,
    encode_cell,
    hex_decode,
    hex_encode,
    real_bits,
)

DML_VERBS = ("INSERT", "UPDATE", "DELETE")
SYNTAX_MARKERS = (
    "syntax error",
    "incomplete input",
    "unrecognized token",
    "You can only execute one statement",
)
SEMANTIC_MARKERS = (
    "no such table",
    "no such column",
    "already exists",
    "has no column",
    "values for",
    "columns but",
    "no transaction is active",
    "within a transaction",
)
BUSY_MARKERS = ("database is locked", "database table is locked")


class SqliteAdapter:
    def __init__(self) -> None:
        self._path = ""
        self._use_wal = False
        self._connections: dict[str, sqlite3.Connection] = {}

    def handle(self, line: str) -> list[str] | None:
        """요청 한 줄의 응답 줄들. QUIT이면 None."""
        parts = line.split(" ")
        verb, args = parts[0], parts[1:]
        if verb == "QUIT":
            return None
        handlers = {
            "HELLO": self._hello,
            "OPEN": self._open,
            "SESSION": self._session,
            "EXEC": self._exec,
            "DROP": self._drop,
            "CLOSE": self._close,
        }
        if verb not in handlers:
            return ["FAULT unknown command 0 0"]
        try:
            return handlers[verb](args)
        except (ValueError, IndexError, KeyError) as err:
            return ["FAULT {} 0 0".format(str(err).replace(" ", "_")[:60])]

    def _hello(self, _args: list[str]) -> list[str]:
        return [f"HELLO sqlite {sqlite3.sqlite_version}"]

    def _open(self, args: list[str]) -> list[str]:
        path_hex, wal, fresh = args
        self._path = hex_decode(path_hex).decode("utf-8")
        self._use_wal = wal == "wal"
        if fresh == "new" and os.path.exists(self._path):
            return ["FAULT database_exists 0 0"]
        return ["OK"]

    def _session(self, args: list[str]) -> list[str]:
        name = args[0]
        if name in self._connections:
            return ["FAULT session_exists 0 0"]
        connection = sqlite3.connect(
            self._path, timeout=0, isolation_level=None, check_same_thread=False
        )
        if self._use_wal:
            connection.execute("PRAGMA journal_mode=WAL")
        self._connections[name] = connection
        return ["OK"]

    def _drop(self, args: list[str]) -> list[str]:
        self._connections.pop(args[0]).close()
        return ["OK"]

    def _close(self, _args: list[str]) -> list[str]:
        for connection in self._connections.values():
            connection.close()
        self._connections.clear()
        return ["OK"]

    # cost: time O(n) for statement and rows, heap O(n), io 1 statement execution
    # vars: n = bytes of values and result rows
    # basis: estimate
    def _exec(self, args: list[str]) -> list[str]:
        name, sql_hex, count = args[0], args[1], int(args[2])
        connection = self._connections[name]
        sql = hex_decode(sql_hex).decode("utf-8")
        params = [_to_python(decode_cell(token)) for token in args[3 : 3 + count]]
        try:
            cursor = connection.execute(sql, params)
            frame = _frame_for_cursor(cursor, sql)
        except sqlite3.Warning as err:
            frame = [_error_line("syntax", err)]
        except sqlite3.Error as err:
            frame = [_error_line(_classify(err), err)]
        return [*frame, f"TXN {int(connection.in_transaction)} 0"]


def _to_python(cell: Cell) -> Any:
    if cell.letter == "N":
        return None
    if cell.letter == "R":
        return bits_real(cell.payload)
    if cell.letter == "T":
        return cell.payload.decode("utf-8")
    return cell.payload


def _from_python(value: Any) -> str:
    if value is None:
        return encode_cell(Cell("N", None))
    if isinstance(value, int):
        return encode_cell(Cell("I", value))
    if isinstance(value, float):
        return encode_cell(Cell("R", real_bits(value)))
    if isinstance(value, str):
        return encode_cell(Cell("T", value.encode("utf-8")))
    return encode_cell(Cell("B", bytes(value)))


def _frame_for_cursor(cursor: sqlite3.Cursor, sql: str) -> list[str]:
    if cursor.description is not None:
        names = [column[0] for column in cursor.description]
        frame = [f"ROWS {len(names)}"]
        frame += [f"COL {hex_encode(name.encode('utf-8'))} D" for name in names]
        frame += [
            "ROW " + " ".join(_from_python(value) for value in row)
            for row in cursor.fetchall()
        ]
        return frame
    words = sql.strip().split()
    verb = words[0].upper() if words else ""
    affected = cursor.rowcount if verb in DML_VERBS else 0
    return [f"CMD {hex_encode(verb.encode('ascii'))} {affected}"]


def _classify(err: sqlite3.Error) -> str:
    message = str(err)
    if isinstance(err, sqlite3.IntegrityError):
        return "constraint"
    if any(marker in message for marker in BUSY_MARKERS):
        return "busy"
    if any(marker in message for marker in SYNTAX_MARKERS):
        return "syntax"
    if any(marker in message for marker in SEMANTIC_MARKERS):
        return "semantic"
    return "other"


def _error_line(error_class: str, err: Exception) -> str:
    return "ERR {} 0 {}".format(error_class, hex_encode(str(err).encode("utf-8")))


def main() -> int:
    adapter = SqliteAdapter()
    for raw in sys.stdin:
        reply = adapter.handle(raw.rstrip("\r\n"))
        if reply is None:
            break
        sys.stdout.write("\n".join(reply + ["END"]) + "\n")
        sys.stdout.flush()
    adapter.handle("CLOSE")
    sys.stdout.write("OK\nEND\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
