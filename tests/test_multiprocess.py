"""여러 프로세스가 같은 파일을 쓰고 읽는 동작을 시험한다(#199, #200). 합성 데이터만 쓴다.

worker는 spawn으로 띄운 별도 프로세스이고 각자 파일을 연다. 작업 하나를 claim하고, 처리 기록을 업무 테이블에 넣고,
ack하는 것을 한 트랜잭션으로 확정한다. 처리 기록의 기본 키가 작업 id이므로 같은 작업을 두 번 처리하면 commit이
실패해 worker가 비정상 종료한다.
"""

from __future__ import annotations

import multiprocessing
import os
import signal
import time
from pathlib import Path

import pytest

from totedb import Database, ForkedHandleError, Queue

SPAWN = multiprocessing.get_context("spawn")
LEASE_MS = 60_000


def _process_jobs(path: str, worker: int, report: multiprocessing.Queue) -> None:
    db = Database(path, create=False, busy_timeout_s=60)
    queue = Queue(db, "jobs", install=False)
    done = 0
    while True:
        with db.begin() as txn:
            claim = queue.get(LEASE_MS, txn=txn)
            if claim is None:
                break
            txn.execute("INSERT INTO done (id, worker, payload) VALUES (?, ?, ?)", (claim.id, worker, claim.payload))
            queue.ack(claim, txn=txn)
        done += 1
    queue.close()
    db.close()
    report.put((worker, done))


def _make_jobs(path: Path, jobs: int) -> None:
    db = Database(path)
    db.execute("CREATE TABLE done (id INTEGER PRIMARY KEY, worker INTEGER NOT NULL, payload BLOB NOT NULL)")
    queue = Queue(db, "jobs")
    for i in range(jobs):
        queue.put(f"job-{i}".encode())
    queue.close()
    db.close()


def _run_workers(path: Path, workers: int) -> list[tuple[int, int]]:
    report = SPAWN.Queue()
    processes = [SPAWN.Process(target=_process_jobs, args=(str(path), w, report)) for w in range(workers)]
    for process in processes:
        process.start()
    for process in processes:
        process.join(timeout=60)
    stuck = [process for process in processes if process.is_alive()]
    for process in stuck:
        process.kill()
        process.join()
    assert not stuck, f"{len(stuck)}개 worker가 60초 안에 끝나지 않음"
    assert [process.exitcode for process in processes] == [0] * workers
    return [report.get(timeout=10) for _ in processes]


@pytest.mark.parametrize("workers", [4, 16])
def test_workers_share_one_queue_without_loss_or_duplicates(tmp_path: Path, workers: int) -> None:
    path = tmp_path / "jobs.totedb"
    jobs = 40 * workers
    _make_jobs(path, jobs)

    reports = _run_workers(path, workers)

    assert sum(count for _, count in reports) == jobs
    db = Database(path, create=False)
    assert db.execute("SELECT COUNT(*), COUNT(DISTINCT id) FROM done").rows == ((jobs, jobs),)
    queue = Queue(db, "jobs", install=False)
    stats = queue.stats()
    assert (stats.ready, stats.leased, stats.acked, stats.failed) == (0, 0, jobs, 0)
    # 처리 기록의 payload는 그 작업의 payload다(다른 작업과 섞이지 않음)
    rows = db.execute("SELECT id, payload FROM done ORDER BY id").rows
    assert [payload for _, payload in rows] == [f"job-{i}".encode() for i in range(jobs)]
    queue.close()
    db.close()


def test_commit_in_one_process_is_seen_by_the_next_statement_of_another(tmp_path: Path) -> None:
    path = tmp_path / "seen.totedb"
    first = Database(path)
    first.execute("CREATE TABLE kv (k TEXT PRIMARY KEY, v INTEGER)")
    second = Database(path, create=False)
    first.execute("INSERT INTO kv VALUES ('a', 1)")
    assert second.execute("SELECT v FROM kv WHERE k = 'a'").rows == ((1,),)
    with second.begin() as txn:
        txn.execute("UPDATE kv SET v = 2 WHERE k = 'a'")
    assert first.execute("SELECT v FROM kv WHERE k = 'a'").rows == ((2,),)
    second.close()
    first.close()


def test_forked_child_cannot_use_the_parent_handle(tmp_path: Path) -> None:
    path = tmp_path / "fork.totedb"
    db = Database(path)
    db.execute("CREATE TABLE t (x INTEGER)")
    db.execute("INSERT INTO t VALUES (1)")
    before = path.read_bytes()
    read_end, write_end = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_end)
        outcome = b"none"
        try:
            db.execute("INSERT INTO t VALUES (2)")
        except ForkedHandleError:
            outcome = b"forked"
        except BaseException:  # noqa: BLE001 - 자식은 어떤 예외든 부모에게 알리고 끝난다
            outcome = b"other"
        os.write(write_end, outcome)
        os._exit(0)
    os.close(write_end)
    _, status = os.waitpid(pid, 0)
    outcome = os.read(read_end, 16)
    os.close(read_end)
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
    assert outcome == b"forked"
    assert path.read_bytes() == before
    assert db.execute("SELECT x FROM t").rows == ((1,),)
    db.execute("INSERT INTO t VALUES (3)")
    assert db.execute("SELECT x FROM t ORDER BY x").rows == ((1,), (3,))
    db.close()


def _write_forever(path: str, ready: multiprocessing.Event) -> None:
    db = Database(path, create=False, busy_timeout_s=60)
    n = 0
    while True:
        with db.begin() as txn:
            txn.execute("INSERT INTO log (worker, n, pad) VALUES (?, ?, ?)", (os.getpid(), n, b"x" * 20000))
            txn.execute("UPDATE total SET n = n + 1 WHERE id = 1")
        n += 1
        ready.set()


def test_killed_writer_leaves_no_half_applied_transaction(tmp_path: Path) -> None:
    path = tmp_path / "kill.totedb"
    db = Database(path)
    db.execute("CREATE TABLE log (id INTEGER PRIMARY KEY AUTOINCREMENT, worker INTEGER, n INTEGER, pad BLOB)")
    db.execute("CREATE TABLE total (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)")
    db.execute("INSERT INTO total VALUES (1, 0)")
    db.close()
    ready = SPAWN.Event()
    writers = [SPAWN.Process(target=_write_forever, args=(str(path), ready)) for _ in range(3)]
    for writer in writers:
        writer.start()
    assert ready.wait(timeout=60)
    # 쓰는 도중의 writer를 차례로 강제 종료한다. 남은 writer는 계속 쓴다
    for writer in writers[:2]:
        time.sleep(0.2)
        os.kill(writer.pid, signal.SIGKILL)
        writer.join()
    survivor = Database(path, create=False, busy_timeout_s=60)
    before = survivor.execute("SELECT n FROM total").rows[0][0]
    time.sleep(0.3)
    after = survivor.execute("SELECT n FROM total").rows[0][0]
    assert after > before, "남은 writer가 계속 쓴다"
    os.kill(writers[2].pid, signal.SIGKILL)
    writers[2].join()
    # 모든 거래는 log 한 행과 total 1 증가를 함께 확정했다. 반만 적용된 거래가 있으면 두 값이 다르다
    rows = survivor.execute("SELECT COUNT(*) FROM log").rows[0][0]
    total = survivor.execute("SELECT n FROM total").rows[0][0]
    assert rows == total
    survivor.execute("INSERT INTO log (worker, n, pad) VALUES (0, 0, X'00')")
    survivor.close()


def _hold_big_transaction(path: str, started: multiprocessing.Event, release: multiprocessing.Event) -> None:
    db = Database(path, create=False, busy_timeout_s=60)
    with db.begin() as txn:
        for i in range(200):
            txn.execute("INSERT INTO big (id, pad) VALUES (?, ?)", (1000 + i, b"y" * 20000))
        started.set()
        release.wait(timeout=60)
    db.close()


def test_reads_do_not_wait_for_another_process_writing(tmp_path: Path) -> None:
    path = tmp_path / "read.totedb"
    db = Database(path)
    db.execute("CREATE TABLE big (id INTEGER PRIMARY KEY, pad BLOB)")
    db.execute("INSERT INTO big VALUES (1, X'00')")
    started, release = SPAWN.Event(), SPAWN.Event()
    writer = SPAWN.Process(target=_hold_big_transaction, args=(str(path), started, release))
    writer.start()
    try:
        assert started.wait(timeout=60)
        # writer가 4MB를 쓴 거래를 쥐고 있는 동안 조회는 기다리지 않고 commit 전 상태를 읽는다
        slowest = 0.0
        for _ in range(50):
            began = time.perf_counter()
            assert db.execute("SELECT COUNT(*) FROM big").rows == ((1,),)
            slowest = max(slowest, time.perf_counter() - began)
        assert slowest < 0.5, f"가장 느린 조회 {slowest:.3f}s"
    finally:
        release.set()
        writer.join(timeout=60)
    assert writer.exitcode == 0
    assert db.execute("SELECT COUNT(*) FROM big").rows == ((201,),)
    db.close()


def _read_until(path: str, stop: multiprocessing.Event) -> None:
    db = Database(path, create=False)
    while not stop.is_set():
        db.execute("SELECT COUNT(*) FROM t")
    db.close()


def _commit_many(db: Database, start: int, count: int) -> float:
    began = time.perf_counter()
    for i in range(start, start + count):
        db.execute("INSERT INTO t VALUES (?, ?)", (i, b"z" * 200))
    return time.perf_counter() - began


def test_writer_keeps_committing_while_readers_read_continuously(tmp_path: Path) -> None:
    path = tmp_path / "starve.totedb"
    db = Database(path)
    db.execute("CREATE TABLE t (x INTEGER, pad BLOB)")
    with db.begin() as txn:
        for i in range(3000):
            txn.execute("INSERT INTO t VALUES (?, ?)", (-i - 1, b"z" * 200))
    alone = _commit_many(db, 0, 40)
    stop = SPAWN.Event()
    readers = [SPAWN.Process(target=_read_until, args=(str(path), stop)) for _ in range(4)]
    for reader in readers:
        reader.start()
    try:
        time.sleep(0.5)
        shared = _commit_many(db, 40, 40)
    finally:
        stop.set()
        for reader in readers:
            reader.join(timeout=60)
    assert [reader.exitcode for reader in readers] == [0, 0, 0, 0]
    assert db.execute("SELECT COUNT(*) FROM t WHERE x >= 0").rows == ((80,),)
    # 게시 대기 표시가 새 reader를 멈춰 writer가 틈을 얻는다. 표시가 없으면 쉬지 않고 읽는 reader 사이에서 굶는다
    assert shared < alone * 6 + 1.0, f"혼자 {alone:.2f}s, reader 4개와 함께 {shared:.2f}s"
    db.close()
