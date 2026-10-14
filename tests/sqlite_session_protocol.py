"""#103 차등 검사의 줄 단위 프로토콜: 값 인코딩, 결과 파서, 어댑터 프로세스 클라이언트.

이 DB 어댑터(tests/session_probe.c)와 SQLite 어댑터(tests/sqlite_session_adapter.py)가
같은 형식을 쓴다. 바이트는 소문자 hex로 보내 NUL·빈 값·개행을 보존한다.
"""

from __future__ import annotations

import contextlib
import queue
import struct
import subprocess
import threading
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

END_LINE = "END"
CELL_LETTERS = "NIRTB"


class EngineError(Exception):
    """어댑터와의 통신 실패의 기준 예외."""


class EngineTimeoutError(EngineError):
    """제한 시간 안에 응답이 끝나지 않았다."""


class EngineExitedError(EngineError):
    """어댑터 프로세스가 응답 도중 또는 응답 전에 끝났다."""


class ProtocolViolationError(EngineError):
    """응답이 프로토콜 형식에 맞지 않는다."""


@dataclass(frozen=True)
class Cell:
    letter: str
    payload: Any  # N: None, I: int, R: 실수 비트(int), T·B: bytes


@dataclass(frozen=True)
class Outcome:
    kind: str  # rows, command, error
    columns: tuple[tuple[bytes, str], ...]
    rows: tuple[tuple[Cell, ...], ...]
    affected: int
    tag: bytes
    error_class: str
    error_code: int
    message: bytes
    txn_active: bool
    txn_failed: bool
    incomplete: str | None


def hex_encode(data: bytes) -> str:
    return data.hex()


def hex_decode(text: str) -> bytes:
    return bytes.fromhex(text)


def real_bits(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def bits_real(bits: int) -> float:
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def encode_cell(cell: Cell) -> str:
    if cell.letter == "N":
        return "N"
    if cell.letter == "I":
        return f"I{cell.payload}"
    if cell.letter == "R":
        return f"R{cell.payload:016x}"
    return cell.letter + hex_encode(cell.payload)


def decode_cell(token: str) -> Cell:
    letter, body = token[:1], token[1:]
    if letter == "N" and body == "":
        return Cell("N", None)
    if letter == "I":
        return Cell("I", int(body))
    if letter == "R" and len(body) == 16:
        return Cell("R", int(body, 16))
    if letter in "TB":
        return Cell(letter, hex_decode(body))
    raise ProtocolViolationError(f"invalid cell token: {token[:40]!r}")


def _parse_error(parts: list[str]) -> tuple[str, int, bytes]:
    if len(parts) != 4:
        raise ProtocolViolationError(
            "invalid error line: {!r}".format(" ".join(parts)[:80])
        )
    return parts[1], int(parts[2]), hex_decode(parts[3])


# cost: time O(n) for n response bytes, heap O(n), stack O(1)
# vars: n = response bytes
# basis: estimate
def parse_outcome(lines: Sequence[str]) -> Outcome:
    """EXEC 응답 한 프레임(END 제외)을 Outcome으로 바꾼다."""
    kind, columns, rows = "", [], []
    affected, tag = 0, b""
    error = ("", 0, b"")
    txn = (False, False)
    incomplete: str | None = None
    for line in lines:
        parts = line.split(" ")
        head = parts[0]
        if head == "ROWS":
            kind = "rows"
        elif head == "COL":
            columns.append((hex_decode(parts[1]), parts[2]))
        elif head == "ROW":
            rows.append(tuple(decode_cell(token) for token in parts[1:]))
        elif head == "CMD":
            kind, tag, affected = "command", hex_decode(parts[1]), int(parts[2])
        elif head == "ERR":
            kind, error = "error", _parse_error(parts)
        elif head == "INCOMPLETE":
            incomplete = " ".join(parts[1:])
        elif head == "TXN":
            txn = (parts[1] == "1", parts[2] == "1")
        elif head == "EXECSTATUS":
            raise ProtocolViolationError(f"exec status {parts[1]}")
        else:
            raise ProtocolViolationError(f"unknown response line: {line[:80]!r}")
    if kind == "":
        raise ProtocolViolationError("response has no result line")
    return Outcome(
        kind,
        tuple(columns),
        tuple(rows),
        affected,
        tag,
        error[0],
        error[1],
        error[2],
        txn[0],
        txn[1],
        incomplete,
    )


def _next_random(state: int) -> int:
    return (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF


def generate_blob(size: int, seed: int) -> bytes:
    """seed가 정하는 결정적 바이트열. NUL과 0xff를 포함한다."""
    state, out = seed, bytearray()
    while len(out) < size:
        state = _next_random(state)
        out += state.to_bytes(8, "little")
    return bytes(out[:size])


def generate_text(size: int, seed: int) -> bytes:
    """seed가 정하는 결정적 UTF-8. 정확히 size바이트이며 2·3·4바이트 문자를 섞는다."""
    pieces = ["a", "Z", "7", " ", "'", "é", "한", "€", "😀"]
    state, out = seed, bytearray()
    while len(out) < size:
        state = _next_random(state)
        piece = pieces[(state >> 33) % len(pieces)].encode("utf-8")
        out += piece if len(out) + len(piece) <= size else b"a"
    return bytes(out)


def build_param(spec: Any) -> Cell:
    """케이스 JSON의 값 표기를 Cell로 바꾼다."""
    if spec is None:
        return Cell("N", None)
    if isinstance(spec, bool):
        raise ProtocolViolationError("bool is not a SQL value")
    if isinstance(spec, int):
        return Cell("I", spec)
    if isinstance(spec, dict) and len(spec) == 1:
        ((key, value),) = spec.items()
        builders = {
            "real": lambda v: Cell("R", real_bits(float(v))),
            "text": lambda v: Cell("T", v.encode("utf-8")),
            "blob_hex": lambda v: Cell("B", hex_decode(v)),
            "text_gen": lambda v: Cell("T", generate_text(v["size"], v["seed"])),
            "blob_gen": lambda v: Cell("B", generate_blob(v["size"], v["seed"])),
        }
        if key in builders:
            return builders[key](value)
    raise ProtocolViolationError(f"unsupported parameter spec: {spec!r}")


class EngineProcess:
    """어댑터 프로세스 하나. 요청 한 줄을 보내고 END까지의 응답 줄을 돌려준다."""

    # cost: time O(1), heap O(1), stack O(1), io 1 process spawn
    # basis: estimate
    def __init__(self, argv: Sequence[str], timeout_s: float) -> None:
        self._timeout_s = timeout_s
        self._proc = subprocess.Popen(
            list(argv),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._stderr_tail = b""
        threading.Thread(target=self._pump_stdout, daemon=True).start()
        threading.Thread(target=self._pump_stderr, daemon=True).start()

    @property
    def stderr_tail(self) -> str:
        return self._stderr_tail.decode("utf-8", errors="replace")

    def request(self, line: str) -> list[str]:
        try:
            assert self._proc.stdin is not None
            self._proc.stdin.write(line.encode("ascii") + b"\n")
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError) as err:
            raise EngineExitedError("adapter closed its input") from err
        return self._read_frame()

    def kill(self) -> None:
        self._proc.kill()
        self._proc.wait()

    # cost: time O(t) waiting for exit, heap O(1), stack O(1), io 1 request + 1 process wait
    # vars: t = timeout_s
    # basis: estimate
    def shutdown(self) -> int | None:
        """QUIT 뒤 종료를 기다려 종료 코드를 돌려준다. 시간 안에 끝나지 않아 kill했으면 None."""
        with contextlib.suppress(EngineError):
            self.request("QUIT")
        try:
            return self._proc.wait(timeout=self._timeout_s)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()
            return None

    def _read_frame(self) -> list[str]:
        frame: list[str] = []
        while True:
            try:
                line = self._lines.get(timeout=self._timeout_s)
            except queue.Empty as err:
                raise EngineTimeoutError(
                    f"no response within {self._timeout_s:.1f}s"
                ) from err
            if line is None:
                raise EngineExitedError(f"adapter exited: {self.stderr_tail[-200:]}")
            if line == END_LINE:
                return frame
            frame.append(line)

    def _pump_stdout(self) -> None:
        assert self._proc.stdout is not None
        for raw in self._proc.stdout:
            self._lines.put(raw.decode("ascii", errors="replace").rstrip("\r\n"))
        self._lines.put(None)

    def _pump_stderr(self) -> None:
        assert self._proc.stderr is not None
        for raw in self._proc.stderr:
            self._stderr_tail = (self._stderr_tail + raw)[-4096:]
