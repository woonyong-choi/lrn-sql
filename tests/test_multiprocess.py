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


def _write_forever(path: str) -> None:
    db = Database(path, create=False, busy_timeout_s=60)
    n = 0
    while True:
        with db.begin() as txn:
            txn.execute(
                "INSERT INTO log (worker, n, pad) VALUES (?, ?, ?)",
                (os.getpid(), n, b"x" * 20000),
            )
            txn.execute("UPDATE total SET n = n + 1 WHERE id = 1")
        n += 1


def _wait_for_writer_progress(
    db: Database, writer: multiprocessing.Process, before: int = 0
) -> None:
    deadline = time.monotonic() + 60
    while True:
        assert writer.is_alive(), (
            f"writer {writer.pid}가 종료됨(exitcode={writer.exitcode})"
        )
        after = db.execute(
            "SELECT COUNT(*) FROM log WHERE worker = ?", (writer.pid,)
        ).rows[0][0]
        if after > before:
            return
        assert time.monotonic() < deadline, (
            f"writer {writer.pid}가 60초 안에 쓰지 못함(before={before}, after={after})"
        )
        time.sleep(0.01)


def test_killed_writer_leaves_no_half_applied_transaction(tmp_path: Path) -> None:
    path = tmp_path / "kill.totedb"
    db = Database(path)
    db.execute(
        "CREATE TABLE log (id INTEGER PRIMARY KEY AUTOINCREMENT, worker INTEGER, n INTEGER, pad BLOB)"
    )
    db.execute("CREATE TABLE total (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)")
    db.execute("INSERT INTO total VALUES (1, 0)")
    db.close()
    observer = Database(path, create=False, busy_timeout_s=60)
    writers = [
        SPAWN.Process(target=_write_forever, args=(str(path),)) for _ in range(3)
    ]
    try:
        for writer in writers:
            writer.start()
        for writer in writers:
            _wait_for_writer_progress(observer, writer)
        for writer in writers[:2]:
            time.sleep(0.2)
            writer.kill()
            writer.join(timeout=60)
            assert not writer.is_alive(), (
                f"writer {writer.pid}가 강제 종료 뒤 끝나지 않음"
            )
            assert writer.exitcode == -signal.SIGKILL
        survivor = writers[2]
        before = observer.execute(
            "SELECT COUNT(*) FROM log WHERE worker = ?", (survivor.pid,)
        ).rows[0][0]
        _wait_for_writer_progress(observer, survivor, before)
        survivor.kill()
        survivor.join(timeout=60)
        assert not survivor.is_alive(), (
            f"writer {survivor.pid}가 강제 종료 뒤 끝나지 않음"
        )
        assert survivor.exitcode == -signal.SIGKILL
        rows = observer.execute("SELECT COUNT(*) FROM log").rows[0][0]
        total = observer.execute("SELECT n FROM total").rows[0][0]
        assert rows == total
        observer.execute("INSERT INTO log (worker, n, pad) VALUES (0, 0, X'00')")
    finally:
        for writer in writers:
            if writer.pid is not None and writer.is_alive():
                writer.kill()
        for writer in writers:
            if writer.pid is not None:
                writer.join(timeout=60)
        observer.close()
        assert all(not writer.is_alive() for writer in writers), (
            "강제 종료 뒤 writer가 남음"
        )


def _hold_big_transaction(path: str, started: multiprocessing.Event, release: multiprocessing.Event) -> None:
    db = Database(path, create=False, busy_timeout_s=60)
    with db.begin() as txn:
        for i in range(200):
            txn.execute("INSERT INTO big (id, pad) VALUES (?, ?)", (1000 + i, b"y" * 20000))
        started.set()
        assert release.wait(timeout=60), "writer의 거래 해제 신호가 오지 않음"
    db.close()


def _read_before_commit(path: str, ready: multiprocessing.Event, started: multiprocessing.Event) -> None:
    db = Database(path, create=False)
    try:
        ready.set()
        assert started.wait(timeout=60), "writer의 거래 시작 신호가 오지 않음"
        for _ in range(50):
            assert db.execute("SELECT COUNT(*) FROM big").rows == ((1,),)
    finally:
        db.close()


def _finish_processes(processes: list[multiprocessing.Process]) -> None:
    for process in processes:
        if process.pid is None:
            continue
        process.join(timeout=60)
        if process.is_alive():
            process.kill()
            process.join(timeout=60)


def test_reads_do_not_wait_for_another_process_writing(tmp_path: Path) -> None:
    path = tmp_path / "read.totedb"
    db = Database(path)
    db.execute("CREATE TABLE big (id INTEGER PRIMARY KEY, pad BLOB)")
    db.execute("INSERT INTO big VALUES (1, X'00')")
    ready, started, release = SPAWN.Event(), SPAWN.Event(), SPAWN.Event()
    writer = SPAWN.Process(target=_hold_big_transaction, args=(str(path), started, release))
    reader = SPAWN.Process(target=_read_before_commit, args=(str(path), ready, started))
    try:
        reader.start()
        assert ready.wait(timeout=60), "reader가 데이터베이스를 열지 못함"
        writer.start()
        assert started.wait(timeout=60)
        reader.join(timeout=60)
        assert not reader.is_alive(), "writer의 거래 해제 전에 reader가 끝나지 않음"
        assert reader.exitcode == 0
        assert writer.is_alive(), "거래 해제 신호 전에 writer가 종료됨"
        release.set()
        writer.join(timeout=60)
        assert not writer.is_alive(), "거래 해제 뒤 writer가 끝나지 않음"
        assert writer.exitcode == 0
        assert db.execute("SELECT COUNT(*) FROM big").rows == ((201,),)
    finally:
        release.set()
        _finish_processes([writer, reader])
        db.close()


def _read_until(path: str, ready: multiprocessing.Event, stop: multiprocessing.Event) -> None:
    db = Database(path, create=False)
    try:
        db.execute("SELECT COUNT(*) FROM t")
        ready.set()
        while not stop.is_set():
            db.execute("SELECT COUNT(*) FROM t")
    finally:
        db.close()


def _commit_many(db: Database, start: int, count: int) -> None:
    for i in range(start, start + count):
        db.execute("INSERT INTO t VALUES (?, ?)", (i, b"z" * 200))


def _commit_in_process(path: str, start: int, count: int) -> None:
    db = Database(path, create=False, busy_timeout_s=60)
    try:
        _commit_many(db, start, count)
    finally:
        db.close()


def test_writer_keeps_committing_while_readers_read_continuously(tmp_path: Path) -> None:
    path = tmp_path / "starve.totedb"
    db = Database(path)
    db.execute("CREATE TABLE t (x INTEGER, pad BLOB)")
    with db.begin() as txn:
        for i in range(3000):
            txn.execute("INSERT INTO t VALUES (?, ?)", (-i - 1, b"z" * 200))
    _commit_many(db, 0, 40)
    stop = SPAWN.Event()
    ready = [SPAWN.Event() for _ in range(4)]
    readers = [SPAWN.Process(target=_read_until, args=(str(path), event, stop)) for event in ready]
    writer = SPAWN.Process(target=_commit_in_process, args=(str(path), 40, 40))
    try:
        for reader in readers:
            reader.start()
        for event in ready:
            assert event.wait(timeout=60), "reader의 첫 조회가 끝나지 않음"
        writer.start()
        writer.join(timeout=60)
        assert not writer.is_alive(), "reader가 읽는 동안 writer가 끝나지 않음"
        assert writer.exitcode == 0
        assert all(reader.is_alive() for reader in readers), "중지 신호 전에 reader가 종료됨"
        stop.set()
        for reader in readers:
            reader.join(timeout=60)
        assert all(not reader.is_alive() for reader in readers), "중지 신호 뒤 reader가 끝나지 않음"
        assert [reader.exitcode for reader in readers] == [0, 0, 0, 0]
        assert db.execute("SELECT COUNT(*) FROM t WHERE x >= 0").rows == ((80,),)
    finally:
        stop.set()
        _finish_processes([writer, *readers])
        db.close()
