"""DB 단독 사용: database 열기, SQL 실행, 여러 문장 트랜잭션. 큐 모듈 없이 쓸 수 있다."""

from __future__ import annotations

import ctypes
import os
import struct
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from types import TracebackType
from typing import Protocol, Union

from totedb._native import Param, load_library
from totedb.errors import (
    STATUS_BUSY,
    BusyError,
    InvalidArgumentError,
    SqlError,
    raise_for_status,
)

__all__ = ["Database", "Result", "SqlValue", "Transaction"]

SqlValue = Union[None, int, float, str, bytes]  # noqa: UP007 - 실행 시점에 평가되므로 3.9에서는 `|`를 못 쓴다

DEFAULT_BUSY_TIMEOUT_S = 5.0
_PARAM_NULL, _PARAM_INTEGER, _PARAM_REAL, _PARAM_TEXT, _PARAM_BLOB = 0, 1, 2, 3, 4
_KIND_ROWS, _KIND_COMMAND, _KIND_ERROR = 0, 1, 2
_INT64_MIN, _INT64_MAX = -(2**63), 2**63 - 1
_RESULT_E_BUSY = 7  # result.h의 오류 분류: 다른 트랜잭션이 쓰는 중이라 실행 전에 거절됨
RESULT_E_CANCELLED = 8  # result.h의 오류 분류: 취소된 문장

_lib = load_library()


class Closable(Protocol):
    """DB에 등록해 DB보다 먼저 닫을 수 있는 객체."""

    def close(self) -> None:
        """자원을 놓는다."""


_TYPE_NAMES = {1: "INTEGER", 2: "REAL", 3: "TEXT", 4: "BLOB"}


@dataclass(frozen=True)
class Column:
    """결과 열 하나. type은 INTEGER·REAL·TEXT·BLOB이며 식의 형식을 정하지 못했으면 None이다."""

    name: str
    type: str | None
    nullable: bool


@dataclass(frozen=True)
class Result:
    """문장 실행 결과. 행이 있으면 column_info·rows, 명령이면 tag·affected_rows."""

    column_info: tuple[Column, ...] = ()
    rows: tuple[tuple[SqlValue, ...], ...] = ()
    tag: str = ""
    affected_rows: int = 0

    @property
    def columns(self) -> tuple[str, ...]:
        """열 이름."""
        return tuple(column.name for column in self.column_info)


def status_name(status: int) -> str:
    """상태 값의 이름(메시지용)."""
    return str(_lib.tote_status_name(status).decode("ascii"))


def check(status: int) -> None:
    """C 상태가 성공이 아니면 대응하는 예외를 던진다."""
    raise_for_status(status, status_name(status))


def retry_busy(call: Callable[[], int], timeout_s: float) -> int:
    """BUSY인 동안 잠깐씩 쉬며 다시 부른다. C 호출은 잠금을 쥔 채 기다리지 않으므로 기다림은 여기서 한다."""
    deadline = time.monotonic() + timeout_s
    delay = 0.0005
    while True:
        status = call()
        if status != STATUS_BUSY or time.monotonic() >= deadline:
            return status
        time.sleep(delay)
        delay = min(delay * 2, 0.02)


def _pack_param(value: SqlValue, keep: list[object]) -> Param:
    if value is None:
        return Param(_PARAM_NULL, 0, 0.0, None, 0)
    if isinstance(value, int):
        if not _INT64_MIN <= int(value) <= _INT64_MAX:
            raise InvalidArgumentError(f"integer out of int64 range: {value}")
        return Param(_PARAM_INTEGER, int(value), 0.0, None, 0)
    if isinstance(value, float):
        return Param(_PARAM_REAL, 0, value, None, 0)
    if isinstance(value, str):
        try:
            raw = value.encode("utf-8")
        except UnicodeEncodeError as err:
            raise InvalidArgumentError("text is not valid UTF-8") from err
        kind = _PARAM_TEXT
    else:
        raw = bytes(value)
        kind = _PARAM_BLOB
    keep.append(raw)
    address = ctypes.cast(ctypes.c_char_p(raw), ctypes.c_void_p).value if raw else None
    return Param(kind, 0, 0.0, address, len(raw))


def _pack_params(params: Sequence[SqlValue]) -> tuple[ctypes.Array[Param] | None, list[object]]:
    keep: list[object] = []
    if not params:
        return None, keep
    array = (Param * len(params))(*[_pack_param(value, keep) for value in params])
    return array, keep


class _Reader:
    """인코딩된 결과 버퍼를 앞에서부터 읽는다(little-endian)."""

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._at = 0

    def unpack(self, fmt: str) -> int | float:
        (value,) = struct.unpack_from(fmt, self._data, self._at)
        self._at += struct.calcsize(fmt)
        return value  # type: ignore[no-any-return]

    def take(self, size: int) -> bytes:
        chunk = self._data[self._at : self._at + size]
        self._at += size
        return chunk

    def text(self) -> str:
        return self.take(int(self.unpack("<I"))).decode("utf-8")


def _decode_cell(reader: _Reader) -> SqlValue:
    kind = int(reader.unpack("<B"))
    if kind == _PARAM_NULL:
        return None
    if kind == _PARAM_INTEGER:
        return int(reader.unpack("<q"))
    if kind == _PARAM_REAL:
        return float(reader.unpack("<d"))
    payload = reader.take(int(reader.unpack("<I")))
    return payload.decode("utf-8") if kind == _PARAM_TEXT else payload


def decode_result(data: bytes) -> Result:
    """tote_exec가 만든 버퍼를 Result로 바꾼다.

    Raises:
        SqlError: 버퍼가 오류 결과일 때.
    """
    reader = _Reader(data)
    kind = int(reader.unpack("<B"))
    if kind == _KIND_COMMAND:
        tag = reader.text()
        return Result(tag=tag, affected_rows=int(reader.unpack("<Q")))
    if kind == _KIND_ERROR:
        error_class = int(reader.unpack("<I"))
        error_code = int(reader.unpack("<i"))
        raise SqlError(
            reader.text() or f"sql error class {error_class}", error_class=error_class, error_code=error_code
        )
    columns = []
    for _ in range(int(reader.unpack("<I"))):
        name = reader.text()
        kind = int(reader.unpack("<B"))
        columns.append(Column(name, _TYPE_NAMES.get(kind), bool(reader.unpack("<B"))))
    rows: list[tuple[SqlValue, ...]] = []
    while int(reader.unpack("<B")) == 1:
        rows.append(tuple(_decode_cell(reader) for _ in columns))
    return Result(column_info=tuple(columns), rows=tuple(rows))


def run_sql(
    db_handle: ctypes.c_void_p, txn_handle: ctypes.c_void_p | None, sql: str, params: Sequence[SqlValue]
) -> Result:
    """문장 하나를 실행한다. 오류 결과는 SqlError로 던진다."""
    array, keep = _pack_params(params)
    out = ctypes.c_void_p()
    size = ctypes.c_size_t()
    status = _lib.tote_exec(
        db_handle, txn_handle, sql.encode("utf-8"), array, len(params), ctypes.byref(out), ctypes.byref(size)
    )
    del keep
    check(status)
    try:
        return decode_result(ctypes.string_at(out, size.value))
    finally:
        _lib.tote_free(out)


class Database:
    """독립 DB 하나. 큐를 설치하지 않으면 테이블도 객체도 생기지 않는다."""

    def __init__(
        self,
        path: str | os.PathLike[str],
        *,
        create: bool = True,
        busy_timeout_s: float = DEFAULT_BUSY_TIMEOUT_S,
    ) -> None:
        handle = ctypes.c_void_p()
        check(_lib.tote_open(os.fsencode(path), 1 if create else 0, ctypes.byref(handle)))
        self._handle: ctypes.c_void_p | None = handle
        self._queues: set[Closable] = set()
        self.busy_timeout_s = busy_timeout_s

    @property
    def handle(self) -> ctypes.c_void_p:
        """C 핸들. 닫힌 DB면 오류다."""
        if self._handle is None:
            raise InvalidArgumentError("database is closed")
        return self._handle

    def execute(self, sql: str, params: Sequence[SqlValue] = ()) -> Result:
        """문장 하나를 자동 commit으로 실행한다.

        다른 트랜잭션이 쓰는 중이라 변경 문장이 BUSY로 거절되면(실행 전 거절이라 반영된 것이 없다)
        busy_timeout_s까지 다시 시도한다. C 호출은 잠금을 쥔 채 기다리지 않으므로 기다림은 여기서 한다.
        """
        deadline = time.monotonic() + self.busy_timeout_s
        delay = 0.0005
        while True:
            try:
                return run_sql(self.handle, None, sql, params)
            except SqlError as err:
                if err.error_class != _RESULT_E_BUSY or time.monotonic() >= deadline:
                    raise
            time.sleep(delay)
            delay = min(delay * 2, 0.02)

    def begin(self) -> Transaction:
        """쓰기 트랜잭션을 시작한다. 다른 writer가 있으면 busy_timeout_s까지 기다린다.

        Raises:
            BusyError: 기다린 시간 안에 시작하지 못했을 때.
        """
        out = ctypes.c_void_p()
        status = retry_busy(lambda: _lib.tote_begin(self.handle, ctypes.byref(out)), self.busy_timeout_s)
        if status == STATUS_BUSY:
            raise BusyError("another writer holds the database", status=status)
        check(status)
        return Transaction(self, out)

    def cancel(self) -> None:
        """다른 스레드에서 지금 실행 중인 문장이나 큐 연산 하나를 취소한다.

        취소된 문장은 SqlError(error_class=RESULT_E_CANCELLED)이고, 큐 연산은 CancelledError이며 자기 트랜잭션을
        반영하지 않는다. 게시를 시작한 문장은 취소와 무관하게 끝난다. 끝나는 순간과 겹친 요청은 같은 세션의
        다음 문장 하나에 적용될 수 있다.
        """
        _lib.tote_cancel(self.handle)

    def register_queue(self, queue: Closable) -> None:
        """이 DB를 쓰는 큐를 등록한다. DB를 닫을 때 먼저 닫힌다(큐는 DB보다 오래 살 수 없다)."""
        self._queues.add(queue)

    def unregister_queue(self, queue: Closable) -> None:
        """큐가 스스로 닫혔다."""
        self._queues.discard(queue)

    def close(self) -> None:
        """등록된 큐를 닫고 DB를 닫는다. 열린 트랜잭션이 있으면 BusyError이며 DB는 열려 있고 큐만 닫혀 있다."""
        if self._handle is None:
            return
        for queue in list(self._queues):
            queue.close()
        status = _lib.tote_close(ctypes.byref(self._handle))
        check(status)
        self._handle = None

    def __enter__(self) -> Database:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()


class Transaction:
    """여러 문장을 하나로 확정하거나 모두 버린다. with 블록은 예외가 없으면 commit, 있으면 rollback한다."""

    def __init__(self, db: Database, handle: ctypes.c_void_p) -> None:
        self.db = db
        self._handle: ctypes.c_void_p | None = handle

    @property
    def handle(self) -> ctypes.c_void_p:
        """C 핸들. commit·rollback한 뒤에는 오류다."""
        if self._handle is None:
            raise InvalidArgumentError("transaction is finished")
        return self._handle

    @property
    def is_failed(self) -> bool:
        """문장 오류로 실패 상태이면 commit할 수 없고 rollback만 된다."""
        return self._handle is not None and bool(_lib.tote_txn_failed(self._handle))

    def execute(self, sql: str, params: Sequence[SqlValue] = ()) -> Result:
        """이 트랜잭션 안에서 문장 하나를 실행한다."""
        return run_sql(self.db.handle, self.handle, sql, params)

    def commit(self) -> None:
        """확정한다. 결과를 알 수 없으면 UncertainError이며 성공으로 취급하면 안 된다."""
        handle, self._handle = self.handle, None
        check(_lib.tote_commit(ctypes.byref(handle)))

    def rollback(self) -> None:
        """모두 버린다."""
        handle, self._handle = self.handle, None
        check(_lib.tote_rollback(ctypes.byref(handle)))

    def __enter__(self) -> Transaction:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        if self._handle is None:
            return
        if exc_type is None and not self.is_failed:
            self.commit()
        else:
            self.rollback()
