"""#103 차등 검사 하네스 자체의 시험.

가짜 엔진으로 각 판정 경로를 일으켜 완료 조건 3(missing·failed·not-run을 통과로 세지 않는다)을
확인한다. 실제 SQLite·이 DB와의 비교 결과는 tests/sqlite_session_diff.py의 실행 결과이며
여기서 검증하지 않는다.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import pytest
import sqlite_session_diff as diff
from sqlite_session_compare import (
    DECLARED,
    FAIL,
    MISSING,
    NOT_RUN,
    PASS,
    StepVerdict,
    Tally,
    compare_outcomes,
    judge_difference,
)
from sqlite_session_engine import EngineSession
from sqlite_session_protocol import (
    Cell,
    EngineExitedError,
    EngineTimeoutError,
    Outcome,
    build_param,
    decode_cell,
    encode_cell,
    generate_blob,
    generate_text,
    parse_outcome,
)


def make_outcome(
    kind: str = "rows", rows: list[list[Cell]] | None = None, **fields: Any
) -> Outcome:
    base: dict[str, Any] = {
        "kind": kind,
        "columns": ((b"v", "T"),),
        "rows": tuple(tuple(r) for r in rows or []),
        "affected": 0,
        "tag": b"",
        "error_class": "",
        "error_code": 0,
        "message": b"",
        "txn_active": False,
        "txn_failed": False,
        "incomplete": None,
    }
    base.update(fields)
    return Outcome(**base)


def text(value: str) -> Cell:
    return Cell("T", value.encode("utf-8"))


class FakeEngine:
    """EngineSession의 대역. 단계마다 미리 정한 결과나 예외를 돌려준다."""

    def __init__(self, script: list[Any]) -> None:
        self._script = list(script)
        self.calls: list[str] = []

    def open_fresh(self, _connections: list[str]) -> None:
        self.calls.append("open")

    def execute(self, _conn: str, sql: str, _params: list[Any]) -> Outcome:
        self.calls.append(sql)
        item = self._script.pop(0)
        if isinstance(item, Exception):
            raise item
        return item

    def restart(self, _connections: list[str], *, crash: bool) -> None:
        self.calls.append("crash" if crash else "restart")

    def drop_session(self, conn: str) -> None:
        self.calls.append("drop " + conn)


def make_case(steps: list[dict[str, Any]]) -> dict[str, Any]:
    return {"id": "fake", "connections": ["A"], "steps": steps}


def select_step(step_id: str, **extra: Any) -> dict[str, Any]:
    step = {
        "id": step_id,
        "conn": "A",
        "sql": "SELECT v FROM t",
        "expect": {"kind": "rows", "rows": 1},
    }
    step.update(extra)
    return step


def run_fake(
    case: dict[str, Any], reference: FakeEngine, subject: FakeEngine
) -> list[StepVerdict]:
    return diff.CaseRunner(reference, subject).run(case)  # type: ignore[arg-type]


def one_row(value: str = "a") -> Outcome:
    return make_outcome(rows=[[text(value)]])


def verdicts_of(results: list[StepVerdict]) -> list[str]:
    return [result.verdict for result in results]


DECLARATION = {
    "id": "D-X",
    "reason": "documented support difference",
    "reference": {"rows": 1},
    "subject": {"rows": 0},
}


# #103 완료 조건 3: 같은 결과만 pass로 센다
def test_run_equal_results_pass() -> None:
    results = run_fake(
        make_case([select_step("s1")]), FakeEngine([one_row()]), FakeEngine([one_row()])
    )

    assert verdicts_of(results) == [PASS, PASS]


# #103 완료 조건 3: 값이 다르면 fail이며 차이 설명이 남는다
def test_run_different_rows_fail() -> None:
    results = run_fake(
        make_case([select_step("s1")]),
        FakeEngine([one_row("a")]),
        FakeEngine([one_row("b")]),
    )

    assert verdicts_of(results) == [PASS, FAIL]
    assert "rows differ" in results[1].detail


# #103 완료 조건 3: 두 엔진이 같은 오답을 내도 독립 기대로 fail이 된다
def test_run_both_engines_wrong_the_same_way_fails_expectation() -> None:
    empty = make_outcome(rows=[])

    results = run_fake(
        make_case([select_step("s1")]), FakeEngine([empty]), FakeEngine([empty])
    )

    assert results[1].verdict == FAIL
    assert "rows expected 1 but was 0" in results[1].detail


# #103 완료 조건 3: 응답이 없으면 missing이고 뒤 단계는 not-run이다
def test_run_engine_without_response_is_missing_and_rest_not_run() -> None:
    steps = [select_step("s1"), select_step("s2"), select_step("s3")]
    subject = FakeEngine([EngineTimeoutError("no response within 1.0s")])

    results = run_fake(make_case(steps), FakeEngine([one_row()]), subject)

    assert verdicts_of(results) == [PASS, MISSING, NOT_RUN, NOT_RUN]
    assert results[2].detail == "aborted after s1 missing"


# #103 완료 조건 3: 엔진이 응답 전에 끝나도 missing이다
def test_run_engine_exit_is_missing() -> None:
    results = run_fake(
        make_case([select_step("s1")]),
        FakeEngine([one_row()]),
        FakeEngine([EngineExitedError("gone")]),
    )

    assert results[1].verdict == MISSING


# #103 완료 조건 3: 값 불일치는 뒤 단계 실행을 막지 않는다
def test_run_mismatch_does_not_stop_later_steps() -> None:
    steps = [select_step("s1"), select_step("s2")]
    reference = FakeEngine([one_row("a"), one_row("a")])
    subject = FakeEngine([one_row("b"), one_row("a")])

    results = run_fake(make_case(steps), reference, subject)

    assert verdicts_of(results) == [PASS, FAIL, PASS]


# #103 완료 조건 1: 재시작과 세션 닫기 단계가 두 엔진에 같은 순서로 전달된다
def test_run_restart_and_drop_steps_pass_without_comparison() -> None:
    steps = [{"id": "r1", "restart": "crash"}, {"id": "d1", "drop": "A"}]
    reference, subject = FakeEngine([]), FakeEngine([])

    results = run_fake(make_case(steps), reference, subject)

    assert verdicts_of(results) == [PASS, PASS, PASS]
    assert reference.calls == ["open", "crash", "drop A"]
    assert subject.calls == reference.calls


# #103 완료 조건 3: 선언된 지원 차이는 pass와 따로 센다
def test_run_declared_difference_is_counted_separately_from_pass() -> None:
    step = select_step("s1", difference=DECLARATION)
    step.pop("expect")

    results = run_fake(
        make_case([step]), FakeEngine([one_row()]), FakeEngine([make_outcome(rows=[])])
    )

    assert verdicts_of(results) == [PASS, DECLARED]


# #103 완료 조건 3: 선언했는데 차이가 사라지면 낡은 선언으로 fail이다
def test_run_declared_difference_not_observed_fails() -> None:
    step = select_step("s1", difference=DECLARATION)
    step.pop("expect")

    results = run_fake(
        make_case([step]), FakeEngine([one_row()]), FakeEngine([one_row()])
    )

    assert results[1].verdict == FAIL
    assert "was not observed" in results[1].detail


# #103 완료 조건 3: 선언과 다른 방식으로 어긋나면 fail이다
def test_run_declared_difference_with_other_subject_behavior_fails() -> None:
    step = select_step("s1", difference=DECLARATION)
    step.pop("expect")
    subject = make_outcome(rows=[[text("a")], [text("b")]])

    results = run_fake(
        make_case([step]), FakeEngine([one_row()]), FakeEngine([subject])
    )

    assert results[1].verdict == FAIL
    assert "does not match its declaration" in results[1].detail


# #103 완료 조건 3: 선언은 두 엔진의 정확한 셀 값을 고정한다
def test_judge_difference_pins_exact_cells_of_both_engines() -> None:
    difference = {
        "id": "D-X",
        "reason": "documented",
        "reference_rows": [["T61"]],
        "subject_rows": [["T62"]],
    }
    subject = one_row("b")

    as_declared = judge_difference(
        difference, one_row("a"), subject, compare_outcomes(one_row("a"), subject)
    )
    other_reference = judge_difference(
        difference, one_row("c"), subject, compare_outcomes(one_row("c"), subject)
    )

    assert (as_declared[0], other_reference[0]) == (DECLARED, FAIL)


# #103 계약: 순서 없는 결과는 multiset으로 비교한다
def test_compare_multiset_ignores_row_order_unless_ordered() -> None:
    first = make_outcome(rows=[[text("a")], [text("b")]])
    second = make_outcome(rows=[[text("b")], [text("a")]])

    assert compare_outcomes(first, second) == []
    assert compare_outcomes(first, second, ordered=True) != []


# #103 계약: NULL과 빈 값을 보존한다
def test_compare_distinguishes_null_empty_text_and_empty_blob() -> None:
    null, empty_text, empty_blob = Cell("N", None), Cell("T", b""), Cell("B", b"")

    outcomes = [make_outcome(rows=[[cell]]) for cell in (null, empty_text, empty_blob)]

    assert all(
        compare_outcomes(left, right) != []
        for left in outcomes
        for right in outcomes
        if left is not right
    )


# #103 계약: 중복 행을 보존한다
def test_compare_counts_duplicate_rows() -> None:
    once = make_outcome(rows=[[text("a")]])
    twice = make_outcome(rows=[[text("a")], [text("a")]])

    assert compare_outcomes(once, twice) != []


# #103 완료 조건 3: 끝나지 않은 일과 실패를 성공 종료 코드로 만들지 않는다
def test_tally_exit_code_never_counts_unfinished_work_as_success() -> None:
    cases = {
        "all pass": ([PASS, PASS], 0),
        "declared only with pass": ([PASS, DECLARED], 0),
        "fail": ([PASS, FAIL], 1),
        "missing": ([PASS, MISSING], 1),
        "not-run": ([PASS, NOT_RUN], 2),
        "nothing passed": ([DECLARED], 2),
        "empty": ([], 2),
    }
    for name, (verdicts, expected) in cases.items():
        tally = Tally()
        for verdict in verdicts:
            tally.add(verdict)
        assert tally.exit_code() == expected, name


# #103 완료 조건 3: 요구 버전이 아닌 SQLite는 실행 없이 not-run이다
def test_run_all_with_wrong_sqlite_version_marks_everything_not_run(
    tmp_path: Path,
) -> None:
    case = make_case([select_step("s1")])
    probe = tmp_path / "probe"
    probe.write_text("")

    results = diff.run_all([case], probe, tmp_path / "work", 5.0, "0.0.0")

    assert verdicts_of(results["fake"]) == [NOT_RUN, NOT_RUN]
    assert "required" in results["fake"][0].detail
    assert diff.tally_results(results).exit_code() == 2


# #103 완료 조건 3: 이 DB 어댑터가 없으면 실행 없이 not-run이다
def test_run_all_with_missing_probe_marks_everything_not_run(tmp_path: Path) -> None:
    version = diff.sqlite_version(10.0)
    assert version is not None

    results = diff.run_all(
        [make_case([select_step("s1")])],
        tmp_path / "absent",
        tmp_path / "work",
        5.0,
        version,
    )

    assert verdicts_of(results["fake"]) == [NOT_RUN, NOT_RUN]
    assert "subject adapter missing" in results["fake"][0].detail


# #103 완료 조건 3: 기대 행 수 없는 SELECT는 공허한 통과라 케이스 파일에서 거절한다
def test_validate_cases_rejects_select_without_expected_rows() -> None:
    case = make_case([{"id": "s1", "conn": "A", "sql": "SELECT * FROM t"}])

    with pytest.raises(diff.CaseFileError):
        diff.validate_cases([case])


# #103 완료 조건 3: 선언하지 않은 연결과 중복 케이스 ID는 케이스 파일에서 거절한다
def test_validate_cases_rejects_undeclared_connection_and_duplicate_ids() -> None:
    with pytest.raises(diff.CaseFileError):
        diff.validate_cases([make_case([{"id": "s1", "conn": "Z", "sql": "BEGIN"}])])
    with pytest.raises(diff.CaseFileError):
        diff.validate_cases([make_case([]), make_case([])])


# #103 완료 조건 1·2: 배포하는 케이스 파일이 검증을 통과하고 케이스마다 근거를 적는다
def test_shipped_cases_are_valid_and_each_case_names_its_basis() -> None:
    cases = diff.load_cases(diff.CASES_PATH)

    assert len(cases) > 0
    assert all(case["basis"].startswith("#103") for case in cases)


# #103 완료 조건 2: BEGIN은 SQLite에서 쓰기 잠금을 즉시 잡는 BEGIN IMMEDIATE로 맞춘다
def test_sqlite_statement_maps_transaction_start_but_keeps_other_sql() -> None:
    assert diff.sqlite_statement({"sql": "begin  durable"}) == "BEGIN IMMEDIATE"
    assert diff.sqlite_statement({"sql": "COMMIT"}) == "COMMIT"
    assert (
        diff.sqlite_statement({"sql": "BEGIN", "sql_sqlite": "BEGIN DEFERRED"})
        == "BEGIN DEFERRED"
    )


# #103 계약: NULL·NUL 바이트·빈 값·실수 비트를 프로토콜이 보존한다
def test_protocol_cell_round_trip_keeps_nul_empty_and_real_bits() -> None:
    cells = [
        Cell("N", None),
        Cell("I", -9223372036854775808),
        Cell("R", 0x8000000000000000),
        Cell("T", b""),
        Cell("B", b"\x00\xff\x00"),
    ]

    assert [decode_cell(encode_cell(cell)) for cell in cells] == cells


# #103 완료 조건 1: 큰 값 생성이 결정적이고 정확한 바이트 수의 UTF-8이다
def test_protocol_generated_values_are_deterministic_exact_size_utf8() -> None:
    assert generate_text(8160, 5) == generate_text(8160, 5)
    assert generate_text(8160, 5) != generate_text(8160, 6)
    assert len(generate_text(8161, 7)) == 8161 and generate_text(8161, 7).decode(
        "utf-8"
    )
    assert len(generate_blob(300000, 12)) == 300000 and b"\x00" in generate_blob(
        300000, 12
    )
    assert build_param({"real": -0.0}).payload == 0x8000000000000000


# #103 계약: 결과 프레임에서 행·컬럼·트랜잭션 상태를 읽는다
def test_parse_outcome_reads_rows_columns_and_transaction_state() -> None:
    outcome = parse_outcome(["ROWS 1", "COL 76 T", "ROW T61", "TXN 1 0"])

    assert outcome.rows == ((Cell("T", b"a"),),)
    assert outcome.txn_active
    assert not outcome.txn_failed


QUITTING_ADAPTER = (
    "import sys\n"
    "for line in sys.stdin:\n"
    "    verb = line.split()[0]\n"
    "    if verb == 'HELLO':\n"
    "        print('HELLO fake 1'); print('END'); sys.stdout.flush()\n"
    "    elif verb == 'QUIT':\n"
    "        print('OK'); print('END'); sys.stdout.flush(); sys.exit(CODE)\n"
)


def fake_session(exit_code: int, tmp_path: Path) -> EngineSession:
    argv = [sys.executable, "-c", f"CODE = {exit_code}\n" + QUITTING_ADAPTER]
    return EngineSession("subject", argv, tmp_path / "db", wal=False, timeout_s=10.0)


# #103 완료 조건 3: sanitizer 보고처럼 종료 코드가 0이 아니면 종료 시점 실패로 남긴다
def test_stop_reports_nonzero_exit_code_of_adapter(tmp_path: Path) -> None:
    session = fake_session(23, tmp_path)
    session.start()

    problem = session.stop()

    assert problem is not None
    assert "exited with code 23" in problem


# #103 완료 조건 3: 정상 종료(코드 0)는 문제로 세지 않는다
def test_stop_returns_nothing_for_clean_adapter_exit(tmp_path: Path) -> None:
    session = fake_session(0, tmp_path)
    session.start()

    assert session.stop() is None
