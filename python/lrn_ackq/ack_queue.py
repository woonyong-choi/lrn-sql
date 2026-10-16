"""선택형 ACK 큐의 Python 인터페이스. enqueue, bounded claim, lease와 fence, nack, 명시적 terminal failure."""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from enum import IntEnum

from lrn_ackq._native import CLOCK_FN, LIST_NOTE_BYTES, Item, ListItem, load_library
from lrn_ackq.database import Database, Transaction, check, retry_busy
from lrn_ackq.errors import STATUS_BUSY, STATUS_EMPTY, BusyError

__all__ = ["AckQueue", "Claim", "ClaimToken", "ItemInfo", "ItemState", "PutResult", "QueueStats"]

_lib = load_library()

_MAX_LIST_PAGE = 1024


class ItemState(IntEnum):
    """작업 상태. 값은 저장된 상태 값과 같다."""

    READY = 0
    LEASED = 1
    ACKED = 2
    FAILED = 3


@dataclass(frozen=True)
class ClaimToken:
    """claim을 가리키는 값. ack·nack·fail은 이 둘만 있으면 된다."""

    id: int
    fence: int


@dataclass(frozen=True)
class Claim(ClaimToken):
    """claim한 작업. payload는 복사본이다."""

    attempts: int = 0
    lease_until_ms: int = 0
    payload: bytes = b""


@dataclass(frozen=True)
class PutResult:
    """enqueue 결과. duplicate이면 같은 idempotency key의 기존 작업이다."""

    id: int
    duplicate: bool


@dataclass(frozen=True)
class QueueStats:
    """상태별 작업 수."""

    ready: int
    leased: int
    acked: int
    failed: int


@dataclass(frozen=True)
class ItemInfo:
    """목록의 작업 한 건. payload는 요청했을 때만 있다."""

    id: int
    state: ItemState
    fence: int
    attempts: int
    ready_at_ms: int
    lease_until_ms: int
    created_at_ms: int
    note: str
    payload: bytes | None = None


def _txn_handle(txn: Transaction | None) -> ctypes.c_void_p | None:
    return txn.handle if txn is not None else None


class AckQueue:
    """이름 붙인 큐 하나. 같은 database의 트랜잭션을 txn으로 주면 업무 갱신과 같은 commit으로 확정된다."""

    def __init__(
        self,
        db: Database,
        name: str,
        *,
        install: bool = True,
        max_attempts: int = 0,
        clock: Callable[[], int] | None = None,
    ) -> None:
        self.db = db
        self.name = name
        self._clock = clock
        self._clock_callback = CLOCK_FN(lambda _ctx: int(clock())) if clock is not None else CLOCK_FN()
        handle = ctypes.c_void_p()
        status = _lib.lrnq_open(
            db.handle,
            name.encode("utf-8"),
            1 if install else 0,
            max_attempts,
            self._clock_callback,
            None,
            ctypes.byref(handle),
        )
        check(status)
        self._handle: ctypes.c_void_p | None = handle
        db.register_queue(self)

    @property
    def handle(self) -> ctypes.c_void_p:
        """C 핸들. 닫은 큐면 오류다."""
        if self._handle is None:
            raise ValueError("queue is closed")
        return self._handle

    def put(self, payload: bytes, idem_key: str | None = None, *, txn: Transaction | None = None) -> PutResult:
        """작업을 넣는다. 같은 key·같은 payload는 기존 작업을 돌려준다(ConflictError: payload가 다름)."""
        job_id = ctypes.c_uint64()
        duplicate = ctypes.c_int()
        key = idem_key.encode("utf-8") if idem_key is not None else None
        check(
            self._call(
                lambda: _lib.lrnq_put(
                    self.handle,
                    _txn_handle(txn),
                    payload,
                    len(payload),
                    key,
                    ctypes.byref(job_id),
                    ctypes.byref(duplicate),
                ),
                txn,
            )
        )
        return PutResult(job_id.value, bool(duplicate.value))

    def get(self, lease_ms: int, *, item_id: int | None = None, txn: Transaction | None = None) -> Claim | None:
        """다음 작업(또는 item_id 하나)을 lease_ms 동안 claim한다. 가져올 것이 없으면 None이다.

        Raises:
            StaleError: item_id가 claim할 수 없는 상태(처리 중이거나 끝남)일 때.
        """
        item = Item()
        status = self._call(
            lambda: _lib.lrnq_get(self.handle, _txn_handle(txn), item_id or 0, lease_ms, ctypes.byref(item)), txn
        )
        if status == STATUS_EMPTY:
            return None
        check(status)
        try:
            payload = ctypes.string_at(item.payload, item.payload_size) if item.payload else b""
            return Claim(item.id, item.fence, item.attempts, item.lease_until_ms, payload)
        finally:
            _lib.lrnq_item_free(ctypes.byref(item))

    def ack(self, claim: ClaimToken, *, txn: Transaction | None = None) -> None:
        """처리를 끝낸다. 옛 claim이면 StaleError이며 아무것도 바꾸지 않는다."""
        check(self._call(lambda: _lib.lrnq_ack(self.handle, _txn_handle(txn), claim.id, claim.fence), txn))

    def fail(self, claim: ClaimToken, reason: str = "", *, txn: Transaction | None = None) -> None:
        """다시 시도하지 않는 terminal failure로 끝낸다."""
        text = reason.encode("utf-8") if reason else None
        check(self._call(lambda: _lib.lrnq_fail(self.handle, _txn_handle(txn), claim.id, claim.fence, text), txn))

    def nack(
        self, claim: ClaimToken, retry_delay_ms: int = 0, reason: str = "", *, txn: Transaction | None = None
    ) -> bool:
        """다시 claim할 수 있게 돌린다. 시도 횟수가 다 되어 terminal failure가 되면 True다."""
        text = reason.encode("utf-8") if reason else None
        terminal = ctypes.c_int()
        check(
            self._call(
                lambda: _lib.lrnq_nack(
                    self.handle, _txn_handle(txn), claim.id, claim.fence, retry_delay_ms, text, ctypes.byref(terminal)
                ),
                txn,
            )
        )
        return bool(terminal.value)

    def update(self, item_id: int, payload: bytes, *, fence: int = 0, txn: Transaction | None = None) -> None:
        """payload를 바꾼다. fence 0은 아직 claim하지 않은 작업, 아니면 그 claim이 유효한 작업만 된다."""
        check(
            self._call(
                lambda: _lib.lrnq_update(self.handle, _txn_handle(txn), item_id, fence, payload, len(payload)), txn
            )
        )

    def recover(self, limit: int = _MAX_LIST_PAGE) -> int:
        """lease가 만료된 작업을 대기로(시도가 다 되었으면 terminal로) 돌리고 처리한 수를 돌려준다."""
        return self._count_call(_lib.lrnq_recover, limit)

    def release_leases(self, limit: int = _MAX_LIST_PAGE) -> int:
        """worker가 모두 죽었다고 아는 호출자가 모든 처리 중 작업을 만료와 무관하게 대기로 돌린다."""
        return self._count_call(_lib.lrnq_release_leases, limit)

    def purge(self, limit: int = _MAX_LIST_PAGE, *, include_failed: bool = False) -> int:
        """끝난 작업(ACKED, 선택하면 FAILED)을 id가 작은 것부터 최대 limit개 지우고 지운 수를 돌려준다."""
        count = ctypes.c_size_t()
        check(
            self._call(
                lambda: _lib.lrnq_purge(self.handle, None, 1 if include_failed else 0, limit, ctypes.byref(count)), None
            )
        )
        return count.value

    def stats(self, *, txn: Transaction | None = None) -> QueueStats:
        """상태별 작업 수. payload를 복사하지 않는다."""
        counts = (ctypes.c_uint64 * 4)()
        check(self._call(lambda: _lib.lrnq_stats(self.handle, _txn_handle(txn), counts), txn))
        return QueueStats(counts[0], counts[1], counts[2], counts[3])

    def list_items(
        self, after_id: int = 0, limit: int = 100, *, with_payload: bool = False, txn: Transaction | None = None
    ) -> list[ItemInfo]:
        """after_id보다 큰 id를 오름차순으로 최대 limit개 읽는다(bounded cursor).

        다음 쪽은 마지막 id를 after_id로 준다.
        """
        page = min(limit, _MAX_LIST_PAGE)
        items = (ListItem * page)()
        count = ctypes.c_size_t()
        check(
            self._call(
                lambda: _lib.lrnq_list(
                    self.handle, _txn_handle(txn), after_id, page, 1 if with_payload else 0, items, ctypes.byref(count)
                ),
                txn,
            )
        )
        try:
            return [_to_info(items[i], with_payload=with_payload) for i in range(count.value)]
        finally:
            _lib.lrnq_list_free(items, count.value)

    def iter_items(self, page_size: int = 100, *, with_payload: bool = False) -> Iterator[ItemInfo]:
        """모든 작업을 page_size씩 끊어 읽는다. 한 번에 한 쪽만 메모리에 둔다."""
        after = 0
        while True:
            page = self.list_items(after, page_size, with_payload=with_payload)
            if not page:
                return
            yield from page
            after = page[-1].id

    def close(self) -> None:
        """큐 핸들을 닫는다. DB를 닫기 전에 먼저 닫는다."""
        if self._handle is not None:
            _lib.lrnq_close(ctypes.byref(self._handle))
            self._handle = None
            self.db.unregister_queue(self)

    def _count_call(self, function: Callable[..., int], limit: int) -> int:
        count = ctypes.c_size_t()
        check(self._call(lambda: function(self.handle, None, limit, ctypes.byref(count)), None))
        return count.value

    def _call(self, call: Callable[[], int], txn: Transaction | None) -> int:
        """자기 트랜잭션을 여는 호출만 BUSY를 기다려 다시 시도한다. 호출자의 트랜잭션 안에서는 기다리지 않는다."""
        if txn is not None:
            return call()
        status = retry_busy(call, self.db.busy_timeout_s)
        if status == STATUS_BUSY:
            raise BusyError("another writer holds the database", status=status)
        return status


def _to_info(raw: ListItem, *, with_payload: bool) -> ItemInfo:
    payload = None
    if with_payload:
        payload = ctypes.string_at(raw.payload, raw.payload_size) if raw.payload else b""
    note = bytes(raw.note[:LIST_NOTE_BYTES]).split(b"\0", 1)[0].decode("utf-8", "replace")
    return ItemInfo(
        raw.id,
        ItemState(raw.state),
        raw.fence,
        raw.attempts,
        raw.ready_at_ms,
        raw.lease_until_ms,
        raw.created_at_ms,
        note,
        payload,
    )
