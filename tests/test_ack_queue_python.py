"""Python 어댑터 시험(#140 완료 조건 1과 2의 Python 쪽). 합성 데이터만 쓴다.

공유 라이브러리가 필요하다: `make test-ack-queue-python`이 빌드하고 TOTEDB_LIB로 알려 준다.
"""

from __future__ import annotations

import threading
from collections.abc import Iterator
from pathlib import Path

import pytest
from totedb import (
    DEFAULT_MAX_PAYLOAD_BYTES,
    BusyError,
    ClaimToken,
    ConflictError,
    Database,
    InvalidArgumentError,
    ItemState,
    LimitError,
    NotInstalledError,
    PutResult,
    Queue,
    SqlError,
    StaleError,
    UncertainError,
)
from totedb.errors import raise_for_status


class FakeClock:
    """결정적 시험을 위한 주입 시계(밀리초)."""

    def __init__(self, now_ms: int = 1000) -> None:
        self.now_ms = now_ms

    def __call__(self) -> int:
        return self.now_ms


@pytest.fixture
def clock() -> FakeClock:
    return FakeClock()


@pytest.fixture
def db(tmp_path: Path) -> Iterator[Database]:
    database = Database(tmp_path / "db.totedb", busy_timeout_s=0.05)
    yield database
    database.close()


def test_database_without_queue_supports_crud_and_creates_no_queue_table(db: Database) -> None:
    db.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT NOT NULL)")
    db.execute("INSERT INTO notes (body) VALUES (?)", ("가나",))
    db.execute("UPDATE notes SET body = ? WHERE id = 1", ("다라",))

    assert db.execute("SELECT id, body FROM notes").rows == ((1, "다라"),)
    with pytest.raises(SqlError):
        db.execute("SELECT id FROM tote_queue_jobs")
    with pytest.raises(NotInstalledError):
        Queue(db, "jobs", install=False)
    db.execute("DELETE FROM notes WHERE id = 1")
    assert db.execute("SELECT id FROM notes").rows == ()


def test_queue_install_creates_table_only_when_requested(db: Database) -> None:
    queue = Queue(db, "jobs")

    assert db.execute("SELECT id FROM tote_queue_jobs").rows == ()
    queue.close()
    Queue(db, "jobs", install=False).close()


def test_queue_put_get_ack_roundtrips_payload_and_fence(db: Database, clock: FakeClock) -> None:
    queue = Queue(db, "jobs", clock=clock)

    first = queue.put(b"alpha")
    claim = queue.get(500)

    assert claim is not None
    assert (claim.id, claim.fence, claim.attempts, claim.payload) == (first.id, 1, 1, b"alpha")
    assert claim.lease_until_ms == 1500
    queue.ack(claim)
    assert queue.stats().acked == 1
    assert queue.get(500) is None


def test_queue_extend_keeps_fence_and_blocks_other_workers(db: Database, clock: FakeClock) -> None:
    """#195: 연장은 fence를 유지하고, 다른 worker가 다시 가져간 뒤의 연장은 StaleError다."""
    queue = Queue(db, "jobs", clock=clock)
    queue.put(b"long")
    claim = queue.get(100)
    assert claim is not None

    clock.now_ms += 90
    assert queue.extend(claim, 100) == clock.now_ms + 100
    clock.now_ms += 50
    assert queue.get(100) is None
    clock.now_ms += 50
    again = queue.get(100)
    assert again is not None and again.fence == claim.fence + 1
    with pytest.raises(StaleError):
        queue.extend(claim, 100)
    queue.ack(again)


def test_queue_retains_idempotency_key_after_purge(db: Database, clock: FakeClock) -> None:
    """#196: purge 뒤에도 보존 기간 안의 같은 key는 지운 작업을 돌려주고, 0이면 남기지 않는다."""
    queue = Queue(db, "jobs", idem_retention_ms=1000, clock=clock)
    first = queue.put(b"mail", "order-1")
    claim = queue.get(100)
    assert claim is not None
    queue.ack(claim)
    assert queue.purge() == 1

    assert queue.put(b"mail", "order-1") == PutResult(first.id, True)
    with pytest.raises(ConflictError):
        queue.put(b"other", "order-1")
    clock.now_ms += 1000
    renewed = queue.put(b"mail", "order-1")
    assert not renewed.duplicate and renewed.id != first.id

    plain = Queue(db, "plain", idem_retention_ms=0, clock=clock)
    kept = plain.put(b"x", "k")
    claim = plain.get(100)
    assert claim is not None
    plain.ack(claim)
    assert plain.purge() == 1
    assert plain.put(b"x", "k").id != kept.id


def test_queue_payload_limit_option(db: Database, clock: FakeClock) -> None:
    """#197: payload 상한은 큐 옵션이며 넘는 payload는 자르지 않고 LimitError다."""
    queue = Queue(db, "small", max_payload_bytes=4, clock=clock)
    queue.put(b"four")
    with pytest.raises(LimitError):
        queue.put(b"five!")
    with pytest.raises(InvalidArgumentError):
        Queue(db, "bad", max_payload_bytes=0)
    assert DEFAULT_MAX_PAYLOAD_BYTES == 1 << 20


def test_queue_nack_retry_delay_and_terminal_failure(db: Database, clock: FakeClock) -> None:
    queue = Queue(db, "jobs", max_attempts=2, clock=clock)
    queue.put(b"retry")

    claim = queue.get(1000)
    assert claim is not None
    assert queue.nack(claim, retry_delay_ms=200, reason="temporary") is False
    assert queue.get(1000) is None
    clock.now_ms += 200
    second = queue.get(1000)
    assert second is not None and second.attempts == 2

    assert queue.nack(second, reason="give up") is True
    assert queue.stats().failed == 1
    poison = queue.put(b"poison")
    token = queue.get(1000)
    assert token is not None and token.id == poison.id
    queue.fail(token, "malformed")
    assert [item.note for item in queue.list_items() if item.state is ItemState.FAILED][-1] == "malformed"


def test_queue_stale_claim_after_lease_expiry_is_rejected(db: Database, clock: FakeClock) -> None:
    queue = Queue(db, "jobs", clock=clock)
    queue.put(b"work")
    old = queue.get(100)
    assert old is not None

    clock.now_ms += 100
    new = queue.get(100)

    assert new is not None and new.fence == old.fence + 1
    with pytest.raises(StaleError):
        queue.ack(old)
    with pytest.raises(StaleError):
        queue.nack(old)
    with pytest.raises(StaleError):
        queue.update(old.id, b"late", fence=old.fence)
    queue.ack(new)
    queue.ack(new)


def test_queue_update_changes_payload_before_and_during_claim(db: Database, clock: FakeClock) -> None:
    queue = Queue(db, "jobs", clock=clock)
    item = queue.put(b"v1")

    queue.update(item.id, b"v2")
    claim = queue.get(100)
    assert claim is not None and claim.payload == b"v2"
    with pytest.raises(StaleError):
        queue.update(item.id, b"v3")
    queue.update(item.id, b"v3", fence=claim.fence)
    queue.ack(claim)
    with pytest.raises(StaleError):
        queue.update(item.id, b"v4", fence=claim.fence)


def test_queue_put_idempotency_key_same_payload_dedupes_and_different_conflicts(db: Database) -> None:
    queue = Queue(db, "jobs")

    first = queue.put(b"same", "key-1")
    again = queue.put(b"same", "key-1")

    assert again.duplicate is True and again.id == first.id
    with pytest.raises(ConflictError):
        queue.put(b"other", "key-1")
    assert queue.stats().ready == 1


def test_queue_list_items_pages_without_payload_copy(db: Database) -> None:
    queue = Queue(db, "jobs")
    for index in range(7):
        queue.put(f"payload-{index}".encode())

    pages = [queue.list_items(0, 3), queue.list_items(3, 3)]

    assert [len(page) for page in pages] == [3, 3]
    assert all(item.payload is None for page in pages for item in page)
    assert [item.id for item in queue.iter_items(page_size=2)] == list(range(1, 8))
    assert queue.list_items(0, 1, with_payload=True)[0].payload == b"payload-0"


def test_queue_restart_preserves_state_and_fence_continues(tmp_path: Path, clock: FakeClock) -> None:
    path = tmp_path / "db.totedb"
    with Database(path) as db:
        queue = Queue(db, "jobs", clock=clock)
        queue.put(b"a", "key-a")
        queue.put(b"b")
        claim_a = queue.get(5000)
        assert claim_a is not None
        queue.ack(claim_a)
        lost = queue.get(5000)
        assert lost is not None
        queue.close()

    with Database(path) as db:
        queue = Queue(db, "jobs", install=False, clock=clock)
        assert (queue.stats().acked, queue.stats().leased) == (1, 1)
        assert queue.put(b"a", "key-a").duplicate is True
        clock.now_ms += 5000
        redelivered = queue.get(100)
        assert redelivered is not None and redelivered.id == lost.id and redelivered.fence == lost.fence + 1
        with pytest.raises(StaleError):
            queue.ack(lost)
        queue.ack(redelivered)
        queue.close()


def test_queue_ack_in_same_transaction_commits_or_rolls_back_with_business_update(
    db: Database, clock: FakeClock
) -> None:
    queue = Queue(db, "jobs", clock=clock)
    db.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, status TEXT NOT NULL)")
    db.execute("INSERT INTO orders VALUES (1, 'new')")
    queue.put(b"order-1")
    claim = queue.get(1000)
    assert claim is not None

    txn = db.begin()
    txn.execute("UPDATE orders SET status = 'done' WHERE id = 1")
    queue.ack(claim, txn=txn)
    assert db.execute("SELECT status FROM orders").rows == (("new",),)
    txn.rollback()
    assert (db.execute("SELECT status FROM orders").rows, queue.stats().leased) == ((("new",),), 1)

    with db.begin() as txn:
        txn.execute("UPDATE orders SET status = 'done' WHERE id = 1")
        queue.ack(claim, txn=txn)
    assert (db.execute("SELECT status FROM orders").rows, queue.stats().acked) == ((("done",),), 1)


def test_queue_transaction_from_other_database_is_rejected(db: Database, tmp_path: Path) -> None:
    queue = Queue(db, "jobs")
    queue.put(b"x")
    claim = queue.get(1000)
    assert claim is not None
    with Database(tmp_path / "other.totedb") as other, other.begin() as foreign:
        with pytest.raises(InvalidArgumentError):
            queue.ack(claim, txn=foreign)
        with pytest.raises(InvalidArgumentError):
            queue.put(b"y", txn=foreign)
    assert queue.stats().leased == 1


def test_transaction_begin_while_writer_open_raises_busy_after_timeout(db: Database) -> None:
    with db.begin():
        with pytest.raises(BusyError):
            db.begin()


def test_database_close_closes_its_queues_and_busy_transaction_keeps_it_open(tmp_path: Path) -> None:
    db = Database(tmp_path / "db.totedb", busy_timeout_s=0.05)
    queue = Queue(db, "jobs")
    txn = db.begin()

    with pytest.raises(BusyError):
        db.close()
    assert db.execute("SELECT id FROM tote_queue_jobs").rows == ()
    txn.rollback()
    db.close()

    with pytest.raises(ValueError, match="closed"):
        queue.stats()


def test_queue_concurrent_get_gives_each_item_to_one_worker(db: Database) -> None:
    db.busy_timeout_s = 10.0
    queue = Queue(db, "jobs")
    for index in range(30):
        queue.put(f"p{index}".encode())
    seen: list[int] = []
    lock = threading.Lock()

    def worker() -> None:
        while (claim := queue.get(100000)) is not None:
            with lock:
                seen.append(claim.id)

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    assert sorted(seen) == list(range(1, 31))


@pytest.mark.parametrize(
    ("status", "name", "error_type"),
    [
        (12, "UNCERTAIN", UncertainError),
        (7, "STALE", StaleError),
        (8, "CONFLICT", ConflictError),
        (4, "BUSY", BusyError),
    ],
)
def test_raise_for_status_maps_status_to_exception(status: int, name: str, error_type: type[Exception]) -> None:
    with pytest.raises(error_type):
        raise_for_status(status, name)


def test_claim_token_ack_works_for_explicit_id_and_fence(db: Database, clock: FakeClock) -> None:
    queue = Queue(db, "jobs", clock=clock)
    item = queue.put(b"t")
    claim = queue.get(1000)
    assert claim is not None

    queue.ack(ClaimToken(item.id, claim.fence))

    assert queue.stats().acked == 1
