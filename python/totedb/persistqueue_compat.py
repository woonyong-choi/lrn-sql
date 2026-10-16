"""persist-queue 1.1.0 SQLiteAckQueue 호출을 Queue 위에서 받아 주는 호환 계층.

Markchain이 쓰는 put, get(id, block, raw), ack, nack, update, queue, clear_acked_data, shrink_disk_usage, close를
같은 이름과 반환 모양으로 제공한다. 의미가 같지 않은 부분은 의도한 차이이며 아래에 모두 적었다.

의미 차이(persist-queue -> 이 계층):
1. ack·nack·ack_failed·update는 같은 프로세스에서 get으로 받은 claim(fence)이 있어야 한다. persist-queue는 id만
   주면 무조건 바꾸지만 여기서는 claim을 모르면 UnknownClaimError다(재시작 뒤에는 get으로 다시 claim한다).
2. lease를 잃은 claim(만료 뒤 다른 worker가 가져감)의 ack·nack은 StaleError다. persist-queue는 항상 성공한다.
3. get은 기본 24시간 lease를 건다. persist-queue는 lease가 없어 처리 중(unack)이 재시작 때만 풀린다. auto_resume은 열 때
   모든 처리 중 작업을 대기로 돌린다(release_leases).
4. update는 끝난 작업(acked·ack_failed)을 거절한다(StaleError). persist-queue는 어느 상태든 바꾼다.
5. queue()의 timestamp는 enqueue 시각(초)이고 status는 persist-queue 문자열 코드로 옮긴다(0 새 작업, 1 재시도 대기,
   2 처리 중, 5 ack됨, 9 실패).
6. clear_acked_data는 오래된 ack부터 지운다(persist-queue는 최신 쪽부터 limit만큼). keep_latest=0이면 같은 결과다.
7. shrink_disk_usage는 아무것도 하지 않는다(VACUUM에 해당하는 파일 축소가 없다). size·active_size·qsize는 전체 스캔이다.
8. 큰 payload(큐의 max_payload_bytes, 기본 1 MiB 초과)와 직렬화 결과가 UTF-8이 아닌 값은 LimitError·InvalidArgumentError다.
"""
# ruff: noqa: A002, ANN401, FBT001, FBT002 - persist-queue의 시그니처(id, block, 임의의 직렬화 값)를 그대로 따른다

from __future__ import annotations

import json
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any, Protocol

from totedb.queue import Queue, Claim, ClaimToken, ItemInfo, ItemState
from totedb.database import Database
from totedb.errors import ToteDBError, NotFoundError, StaleError

__all__ = ["Empty", "SQLiteAckQueue", "UnknownClaimError"]

DEFAULT_LEASE_MS = 24 * 3600 * 1000
_POLL_INTERVAL_S = 0.01
_PAGE = 100
_STATUS_CODES = {ItemState.LEASED: "2", ItemState.ACKED: "5", ItemState.FAILED: "9"}


class Empty(Exception):  # noqa: N818 - persistqueue.exceptions.Empty와 같은 이름
    """가져올 작업이 없다."""


class UnknownClaimError(ToteDBError):
    """이 프로세스가 claim한 적 없는 id에 ack·nack·update를 했다(fence를 알 수 없다)."""


class Serializer(Protocol):
    """persist-queue 직렬화 모듈의 dumps·loads."""

    def dumps(self, value: Any) -> bytes:
        """값을 바이트로."""

    def loads(self, data: bytes) -> Any:
        """바이트를 값으로."""


class _JsonSerializer:
    def dumps(self, value: Any) -> bytes:
        return json.dumps(value).encode("utf-8")

    def loads(self, data: bytes) -> Any:
        return json.loads(data.decode("utf-8"))


class SQLiteAckQueue:
    """persist-queue SQLiteAckQueue의 일부 호출을 받는 큐. path는 디렉터리이며 그 안에 DB와 WAL을 둔다."""

    def __init__(
        self,
        path: str | Path,
        auto_resume: bool = True,
        multithreading: bool = True,
        serializer: Serializer | None = None,
        *,
        lease_ms: int = DEFAULT_LEASE_MS,
        max_attempts: int = 0,
        clock: Callable[[], int] | None = None,
    ) -> None:
        del multithreading  # 항상 스레드 안전하다
        directory = Path(path)
        directory.mkdir(parents=True, exist_ok=True)
        self._db = Database(directory / "data.totedb")
        self._queue = Queue(self._db, "ack_queue", max_attempts=max_attempts, clock=clock)
        self._serializer: Serializer = serializer or _JsonSerializer()
        self._lease_ms = lease_ms
        self._lock = threading.RLock()
        self._claims: dict[int, tuple[Claim, Any]] = {}
        if auto_resume:
            self.resume_unack_tasks()

    def put(self, item: Any) -> int:
        """작업을 넣고 id를 돌려준다."""
        return self._queue.put(self._serializer.dumps(item)).id

    def get(
        self,
        block: bool = True,
        timeout: float | None = None,
        id: int | None = None,
        next_in_order: bool = False,
        raw: bool = False,
    ) -> Any:
        """작업을 claim한다. raw면 {'pqid', 'data', 'timestamp'}를 돌려준다.

        Raises:
            Empty: 가져올 것이 없거나 id를 claim할 수 없을 때(block이면 timeout까지 기다린 뒤).
        """
        if next_in_order and id is None:
            raise ValueError("'next_in_order' requires the preceding 'id' be specified.")
        if timeout is not None and timeout < 0:
            raise ValueError("'timeout' must be a non-negative number")
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            claim = self._try_claim(id, next_in_order=next_in_order)
            if claim is not None:
                return self._deliver(claim, raw=raw)
            if not block or (deadline is not None and time.monotonic() >= deadline):
                raise Empty
            time.sleep(_POLL_INTERVAL_S)

    def ack(self, item: Any = None, id: int | None = None) -> int:
        """처리를 끝낸다. 옛 claim이면 StaleError다."""
        claim = self._resolve(item, id)
        self._queue.ack(claim)
        self._forget(claim.id)
        return claim.id

    def ack_failed(self, item: Any = None, id: int | None = None) -> int:
        """다시 시도하지 않는 실패로 끝낸다."""
        claim = self._resolve(item, id)
        self._queue.fail(claim, "ack_failed")
        self._forget(claim.id)
        return claim.id

    def nack(self, item: Any = None, id: int | None = None) -> int:
        """다시 가져갈 수 있게 대기로 돌린다."""
        claim = self._resolve(item, id)
        self._queue.nack(claim)
        self._forget(claim.id)
        return claim.id

    def update(self, item: Any, id: int | None = None) -> int:
        """payload를 바꾼다. {'pqid', 'data'} 모양이나 id를 받는다."""
        job_id = id
        if isinstance(item, dict) and "pqid" in item:
            job_id = item["pqid"] if id is None else id
            item = item.get("data")
        if job_id is None:
            raise ValueError("Provide an id or raw item")
        with self._lock:
            held = self._claims.get(job_id)
        fence = held[0].fence if held is not None else 0
        self._queue.update(job_id, self._serializer.dumps(item), fence=fence)
        return job_id

    def queue(self) -> list[dict[str, Any]]:
        """모든 작업을 persist-queue 모양으로 돌려준다(payload를 모두 복사한다)."""
        return [self._row(info) for info in self._queue.iter_items(_PAGE, with_payload=True)]

    def clear_acked_data(self, max_delete: int = 1000, keep_latest: int = 1000, clear_ack_failed: bool = False) -> None:
        """ack된 작업(선택하면 실패한 작업)을 오래된 것부터 지운다."""
        stats = self._queue.stats()
        surplus = stats.acked - max(keep_latest, 0)
        budget = max_delete if max_delete and max_delete > 0 else surplus
        if surplus > 0:
            self._queue.purge(min(surplus, budget))
        if clear_ack_failed:
            self._queue.purge(budget, include_failed=True)

    def shrink_disk_usage(self) -> None:
        """파일 축소를 지원하지 않는다(의미 차이 7). 아무것도 하지 않는다."""

    def resume_unack_tasks(self) -> None:
        """처리 중이던 작업을 모두 대기로 돌린다(재시작 복구)."""
        while self._queue.release_leases() > 0:
            pass
        with self._lock:
            self._claims.clear()

    def task_done(self) -> None:
        """persist-queue의 auto_commit=False용 호출. 이 계층은 항상 commit하므로 하는 일이 없다."""

    @property
    def size(self) -> int:
        """가져갈 수 있는 작업 수(대기)."""
        return self._queue.stats().ready

    @property
    def unack_count(self) -> int:
        """처리 중인 작업 수."""
        return self._queue.stats().leased

    @property
    def acked_count(self) -> int:
        """ack된 작업 수."""
        return self._queue.stats().acked

    @property
    def active_size(self) -> int:
        """대기와 처리 중인 작업 수."""
        stats = self._queue.stats()
        return stats.ready + stats.leased

    def qsize(self) -> int:
        """size와 같다."""
        return self.size

    def empty(self) -> bool:
        """대기 작업이 없다."""
        return self.size == 0

    def __len__(self) -> int:
        return self.size

    def close(self) -> None:
        """큐와 DB를 닫는다."""
        self._queue.close()
        self._db.close()

    def _try_claim(self, item_id: int | None, *, next_in_order: bool) -> Claim | None:
        if item_id is None:
            return self._queue.get(self._lease_ms)
        if not next_in_order:
            return self._claim_id(item_id)
        for info in self._queue.iter_items(_PAGE):
            if info.id > item_id and info.state is ItemState.READY:
                claim = self._claim_id(info.id)
                if claim is not None:
                    return claim
        return None

    def _claim_id(self, item_id: int) -> Claim | None:
        try:
            return self._queue.get(self._lease_ms, item_id=item_id)
        except (StaleError, NotFoundError):
            return None

    def _deliver(self, claim: Claim, *, raw: bool) -> Any:
        data = self._serializer.loads(claim.payload)
        with self._lock:
            self._claims[claim.id] = (claim, data)
        if not raw:
            return data
        created_ms = self._queue.list_items(claim.id - 1, 1)[0].created_at_ms  # 전체 스캔 한 번
        return {"pqid": claim.id, "data": data, "timestamp": created_ms / 1000}

    def _resolve(self, item: Any, item_id: int | None) -> ClaimToken:
        if item_id is not None and item is not None:
            raise ValueError("Specify an id or an item, not both.")
        if item_id is None and item is None:
            raise ValueError("Specify an id or an item.")
        with self._lock:
            if item_id is None and isinstance(item, dict) and "pqid" in item:
                item_id = int(item["pqid"])
            if item_id is None:
                item_id = next((key for key, (_, data) in self._claims.items() if data is item), None)
            held = self._claims.get(item_id) if item_id is not None else None
        if held is None:
            raise UnknownClaimError(f"no claim held for id {item_id}; get it first")
        return ClaimToken(held[0].id, held[0].fence)

    def _forget(self, item_id: int) -> None:
        with self._lock:
            self._claims.pop(item_id, None)

    def _row(self, info: ItemInfo) -> dict[str, Any]:
        data = self._serializer.loads(info.payload or b"")
        return {"id": info.id, "data": data, "timestamp": info.created_at_ms / 1000, "status": _status_code(info)}


def _status_code(info: ItemInfo) -> str:
    if info.state is ItemState.READY:
        return "1" if info.attempts > 0 else "0"
    return _STATUS_CODES[info.state]
