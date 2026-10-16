"""새 엔진의 SQL 동작을 파이썬 바인딩으로 시험한다. 합성 데이터만 쓴다.

경쟁 갱신: 조건부 원자 갱신(UPDATE ... WHERE 상태 조건 RETURNING)으로 여러 스레드가 같은 행을 다퉈도
한 행은 한 번만 바뀌고(이중 claim 없음), 더하기 갱신은 하나도 잃지 않는다(분실 갱신 없음).
이 시험의 스레드는 한 핸들을 같이 쓴다. 여러 핸들·프로세스의 공유는 tests/test_multiprocess.py가 시험한다.
"""

from __future__ import annotations

import threading
import time
from pathlib import Path

import pytest
from totedb import Database, SqlError

JOBS = 60
WORKERS = 6


def make_jobs(db: Database) -> None:
    db.execute("CREATE TABLE jobs (id INTEGER PRIMARY KEY, state TEXT NOT NULL, owner INTEGER, hits INTEGER NOT NULL)")
    with db.begin() as tx:
        for i in range(1, JOBS + 1):
            tx.execute("INSERT INTO jobs VALUES (?, 'ready', NULL, 0)", (i,))
        tx.commit()


def run_workers(target, count: int = WORKERS) -> None:
    errors: list[BaseException] = []

    def wrapped(index: int) -> None:
        try:
            target(index)
        except BaseException as err:  # noqa: BLE001 - 스레드 오류를 주 스레드로 넘긴다
            errors.append(err)

    threads = [threading.Thread(target=wrapped, args=(i,)) for i in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]


def test_conditional_claim_gives_each_row_to_one_worker(tmp_path: Path) -> None:
    path = tmp_path / "claim.totedb"
    main = Database(path, busy_timeout_s=10.0)
    make_jobs(main)
    claimed: dict[int, list[int]] = {}
    lock = threading.Lock()

    def worker(index: int) -> None:
        for job in range(1, JOBS + 1):
            rows = main.execute(
                "UPDATE jobs SET state = 'running', owner = ? WHERE id = ? AND state = 'ready' RETURNING id",
                (index, job),
            ).rows
            if rows:
                with lock:
                    claimed.setdefault(job, []).append(index)

    run_workers(worker)
    assert sorted(claimed) == list(range(1, JOBS + 1))
    assert all(len(owners) == 1 for owners in claimed.values()), "한 행을 둘 이상이 가져감"
    owners = {row[0]: row[1] for row in main.execute("SELECT id, owner FROM jobs WHERE state = 'running'").rows}
    assert owners == {job: who[0] for job, who in claimed.items()}
    main.close()


def test_concurrent_increments_lose_nothing(tmp_path: Path) -> None:
    path = tmp_path / "count.totedb"
    main = Database(path, busy_timeout_s=10.0)
    make_jobs(main)
    rounds = 20

    def worker(_index: int) -> None:
        for _ in range(rounds):
            main.execute("UPDATE jobs SET hits = hits + 1 WHERE id <= 10")
            # 읽고 쓰는 트랜잭션도 다른 쓰기와 겹치지 않아야 한다
            with main.begin() as tx:
                before = tx.execute("SELECT hits FROM jobs WHERE id = 20").rows[0][0]
                tx.execute("UPDATE jobs SET hits = ? WHERE id = 20", (before + 1,))
                tx.commit()

    run_workers(worker)
    total = WORKERS * rounds
    rows = main.execute("SELECT id, hits FROM jobs WHERE id <= 10 OR id = 20 ORDER BY id").rows
    assert list(rows) == [(i, total) for i in range(1, 11)] + [(20, total)]
    main.close()


def test_returning_after_failed_statement_leaves_rows(tmp_path: Path) -> None:
    db = Database(tmp_path / "fail.totedb")
    make_jobs(db)
    db.execute("CREATE UNIQUE INDEX jobs_owner ON jobs (owner)")
    db.execute("UPDATE jobs SET owner = 1 WHERE id = 1")
    with pytest.raises(Exception):
        db.execute("UPDATE jobs SET owner = 1 WHERE id = 2 RETURNING id")
    assert db.execute("SELECT id, owner FROM jobs WHERE owner IS NOT NULL").rows == ((1, 1),)
    with pytest.raises(Exception):
        db.execute("DELETE FROM jobs WHERE id = 3 RETURNING id / 0")
    assert db.execute("SELECT COUNT(*) FROM jobs").rows == ((JOBS,),)
    db.close()


# #47 완료 조건 2: 동시 충돌 삽입에서 유일성 유지, 부분 성공 금지
def test_concurrent_upserts_lose_nothing(tmp_path: Path) -> None:
    db = Database(tmp_path / "upsert.totedb", busy_timeout_s=10.0)
    db.execute("CREATE TABLE counters (k INTEGER PRIMARY KEY, n INTEGER NOT NULL)")
    rounds = 30

    def worker(index: int) -> None:
        for r in range(rounds):
            key = r % 5
            rows = db.execute(
                "INSERT INTO counters VALUES (?, 1) ON CONFLICT (k) DO UPDATE SET n = counters.n + 1 RETURNING n",
                (key,),
            ).rows
            assert len(rows) == 1 and rows[0][0] >= 1
            db.execute("INSERT INTO counters VALUES (?, 0) ON CONFLICT DO NOTHING", (100 + index,))

    run_workers(worker)
    rows = db.execute("SELECT k, n FROM counters ORDER BY k").rows
    assert list(rows[:5]) == [(k, WORKERS * rounds // 5) for k in range(5)]
    assert [row[0] for row in rows[5:]] == [100 + i for i in range(WORKERS)]
    db.close()


# #40 완료 조건 2: 집계 이름의 잘못된 인자 꼴은 모르는 함수(43)가 아니라 인자 오류(44)로 거절
@pytest.mark.parametrize(
    ("sql", "code"),
    [
        ("SELECT SUM(*) FROM t", 44),
        ("SELECT MAX(x, x) FROM t", 44),
        ("SELECT COUNT(x, x) FROM t", 44),
        ("SELECT ABS(1, 2)", 44),
        ("SELECT nosuchfunc(1)", 43),
    ],
)
def test_function_signature_errors_are_distinct(tmp_path: Path, sql: str, code: int) -> None:
    db = Database(tmp_path / "fn.totedb")
    db.execute("CREATE TABLE t (x INTEGER)")
    with pytest.raises(SqlError) as raised:
        db.execute(sql)
    assert raised.value.error_code == code
    db.close()


# #79·#64: 다른 스레드의 취소는 실행 중인 문장을 성공과 구별되는 오류로 끝내고, 다음 문장은 정상 실행된다
def test_cancel_from_another_thread_is_distinct_from_success(tmp_path: Path) -> None:
    from totedb.database import RESULT_E_CANCELLED

    db = Database(tmp_path / "cancel.totedb")
    db.execute("CREATE TABLE n (id INTEGER PRIMARY KEY)")
    with db.begin() as tx:
        for start in range(1, 3001, 500):
            tx.execute("INSERT INTO n VALUES " + ", ".join(f"({i})" for i in range(start, start + 500)))
        tx.commit()
    slow = "SELECT COUNT(*) FROM n a, n b, n c WHERE a.id + b.id + c.id = -1"
    caught: list[BaseException] = []

    def run() -> None:
        try:
            db.execute(slow)
        except BaseException as err:  # noqa: BLE001 - 주 스레드에서 판정한다
            caught.append(err)

    # 고장 난 취소에서도 시험 프로세스가 끝나도록 daemon 스레드로 돌린다
    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    time.sleep(0.2)
    # 실행 중이 아닐 때 건 취소는 다음 문장에 남을 수 있으므로 끝나지 않았을 때만 다시 건다
    for _ in range(10):
        db.cancel()
        worker.join(timeout=1)
        if not worker.is_alive():
            break
    assert not worker.is_alive()
    assert len(caught) == 1 and isinstance(caught[0], SqlError) and caught[0].error_class == RESULT_E_CANCELLED
    assert db.execute("SELECT COUNT(*) FROM n").rows == ((3000,),)
    db.close()


# #64 완료 조건 1: NULL·빈 값·NUL 포함 값·페이지보다 큰 값과 열 정보가 바인딩까지 손실 없이 온다
def test_values_and_column_info_round_trip(tmp_path: Path) -> None:
    from totedb import Column

    db = Database(tmp_path / "values.totedb")
    db.execute("CREATE TABLE v (id INTEGER PRIMARY KEY, t TEXT, b BLOB, r REAL, n INTEGER NOT NULL)")
    big_text = "한글" * 60_000 + "\x00끝"
    big_blob = bytes(range(256)) * 1200
    values = [
        (1, None, None, None, 0),
        (2, "", b"", 0.0, -1),
        (3, "a\x00b", b"\x00\x01\x00", -0.5, 2**63 - 1),
        (4, big_text, big_blob, 1e308, -(2**63)),
    ]
    for row in values:
        db.execute("INSERT INTO v VALUES (?, ?, ?, ?, ?)", row)
    result = db.execute("SELECT id, t, b, r, n FROM v ORDER BY id")
    assert result.rows == tuple(values)
    assert result.column_info == (
        Column("id", "INTEGER", False),
        Column("t", "TEXT", True),
        Column("b", "BLOB", True),
        Column("r", "REAL", True),
        Column("n", "INTEGER", False),
    )
    assert result.columns == ("id", "t", "b", "r", "n")
    db.close()


# #64 완료 조건 2: 결과 상한을 넘으면 부분 행을 주지 않고 LimitError, 문장 오류는 분류·코드가 있는 SqlError
def test_limit_and_errors_are_distinct_from_success(tmp_path: Path) -> None:
    from totedb import LimitError
    from totedb.database import _lib

    db = Database(tmp_path / "limits.totedb")
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)")
    for i in range(1, 21):
        db.execute("INSERT INTO t VALUES (?, ?)", (i, "x" * 100))
    previous = _lib.tote_max_result_bytes()
    try:
        _lib.tote_set_max_result_bytes(1000)
        with pytest.raises(LimitError):
            db.execute("SELECT s FROM t")
        assert len(db.execute("SELECT s FROM t WHERE id = 1").rows) == 1
    finally:
        _lib.tote_set_max_result_bytes(previous)
    with pytest.raises(SqlError) as raised:
        db.execute("SELECT * FROM missing")
    assert raised.value.error_class != 0 and raised.value.error_code != 0
    db.close()


# #64 완료 조건 3: 디코딩이 실패해도 C 버퍼는 해제된다
def test_result_buffer_is_freed_when_decoding_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    from totedb import database as module

    freed: list[int] = []
    real_lib = module._lib

    class CountingLib:
        def __getattr__(self, name: str) -> object:
            return getattr(real_lib, name)

        def tote_free(self, pointer: object) -> None:
            freed.append(1)
            real_lib.tote_free(pointer)

    def broken_decode(data: bytes) -> module.Result:
        raise ValueError("decoder failure")

    db = Database(tmp_path / "free.totedb")
    monkeypatch.setattr(module, "_lib", CountingLib())
    monkeypatch.setattr(module, "decode_result", broken_decode)
    with pytest.raises(ValueError):
        db.execute("SELECT 1")
    assert freed == [1]
    monkeypatch.undo()
    db.close()
