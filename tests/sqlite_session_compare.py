"""#103 차등 검사의 비교 정책과 판정.

단계 판정은 pass, fail, missing, not-run, declared-difference 다섯이다. 통과로 세는 것은 pass뿐이며
declared-difference는 문서화한 지원 차이를 두 엔진이 선언한 그대로 보인 경우로 따로 센다.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any

from sqlite_session_protocol import Cell, Outcome, encode_cell

PASS = "pass"
FAIL = "fail"
MISSING = "missing"
NOT_RUN = "not-run"
DECLARED = "declared-difference"
VERDICTS = (PASS, FAIL, MISSING, NOT_RUN, DECLARED)

DEFAULT_COMPARE = {
    "rows": ("kind", "columns", "rows", "txn_active"),
    "command": ("kind", "affected", "txn_active"),
    "error": ("kind", "error_class", "txn_active"),
}
MAX_SHOWN_ROWS = 3
MAX_SHOWN_BYTES = 24


@dataclass(frozen=True)
class StepVerdict:
    case_id: str
    step_id: str
    verdict: str
    detail: str = ""
    reference: dict[str, Any] = field(default_factory=dict)
    subject: dict[str, Any] = field(default_factory=dict)


def summarize(outcome: Outcome) -> dict[str, Any]:
    """판정과 선언된 차이 대조에 쓰는 결과 요약."""
    return {
        "kind": outcome.kind,
        "error_class": outcome.error_class,
        "affected": outcome.affected,
        "rows": len(outcome.rows),
        "txn_active": outcome.txn_active,
        "txn_failed": outcome.txn_failed,
    }


def _show_cell(cell: Cell) -> str:
    if cell.letter == "N":
        return "NULL"
    if cell.letter == "I":
        return str(cell.payload)
    if cell.letter == "R":
        return f"real:{cell.payload:016x}"
    shown = cell.payload[:MAX_SHOWN_BYTES].hex()
    suffix = "..." if len(cell.payload) > MAX_SHOWN_BYTES else ""
    kind = "text" if cell.letter == "T" else "blob"
    return f"{kind}({len(cell.payload)}):{shown}{suffix}"


def _show_row(row: Sequence[Cell]) -> str:
    return "(" + ", ".join(_show_cell(cell) for cell in row) + ")"


def _rows_difference(
    reference: Outcome, subject: Outcome, *, ordered: bool
) -> str | None:
    if ordered:
        if reference.rows == subject.rows:
            return None
        return (
            "rows differ in order or value: "
            f"reference {len(reference.rows)} rows, subject {len(subject.rows)} rows"
        )
    left, right = Counter(reference.rows), Counter(subject.rows)
    if left == right:
        return None
    only_reference = list((left - right).elements())[:MAX_SHOWN_ROWS]
    only_subject = list((right - left).elements())[:MAX_SHOWN_ROWS]
    shown_reference = [_show_row(row) for row in only_reference]
    shown_subject = [_show_row(row) for row in only_subject]
    return f"rows differ: only in reference {shown_reference}; only in subject {shown_subject}"


def _field_difference(
    name: str, reference: Outcome, subject: Outcome, *, ordered: bool
) -> str | None:
    if name == "rows":
        return _rows_difference(reference, subject, ordered=ordered)
    if name == "columns":
        left = [column[0] for column in reference.columns]
        right = [column[0] for column in subject.columns]
        return None if left == right else f"column names differ: {left} vs {right}"
    left_value, right_value = getattr(reference, name), getattr(subject, name)
    if left_value == right_value:
        return None
    return f"{name} differs: reference {left_value!r}, subject {right_value!r}"


def compare_outcomes(
    reference: Outcome,
    subject: Outcome,
    compare: Sequence[str] | None = None,
    *,
    ordered: bool = False,
) -> list[str]:
    """두 결과의 차이 설명. 비어 있으면 같다. 결과 종류가 다르면 종류 차이만 돌려준다."""
    if reference.kind != subject.kind:
        return [f"kind differs: reference {reference.kind}, subject {subject.kind}"]
    names = compare if compare is not None else DEFAULT_COMPARE[reference.kind]
    differences = [
        _field_difference(name, reference, subject, ordered=ordered) for name in names
    ]
    return [text for text in differences if text is not None]


def unmet_expectation(outcome: Outcome, expect: dict[str, Any]) -> str | None:
    """케이스의 독립 기대가 결과와 다르면 설명. 두 엔진이 같은 오답을 내는 경우를 막는다."""
    actual = summarize(outcome)
    for key, wanted in expect.items():
        if key not in actual:
            return f"unknown expectation key {key}"
        if actual[key] != wanted:
            return f"{key} expected {wanted!r} but was {actual[key]!r}"
    return None


def subset_matches(summary: dict[str, Any], declared: dict[str, Any]) -> bool:
    return all(summary.get(key) == value for key, value in declared.items())


def row_tokens(outcome: Outcome) -> list[list[str]]:
    """결과 행을 프로토콜 토큰으로 바꾼다. 선언된 차이가 두 엔진의 정확한 값을 고정할 때 쓴다."""
    return [[encode_cell(cell) for cell in row] for row in outcome.rows]


def _declared_rows_match(outcome: Outcome, declared: dict[str, Any], key: str) -> bool:
    return key not in declared or row_tokens(outcome) == declared[key]


def judge_difference(
    difference: dict[str, Any],
    reference: Outcome,
    subject: Outcome,
    differences: list[str],
) -> tuple[str, str]:
    """선언된 지원 차이를 확인한다. (판정, 설명)."""
    if not differences:
        return FAIL, "declared difference {} was not observed".format(difference["id"])
    ref_ok = subset_matches(summarize(reference), difference.get("reference", {}))
    sub_ok = subset_matches(summarize(subject), difference.get("subject", {}))
    ref_ok = ref_ok and _declared_rows_match(reference, difference, "reference_rows")
    sub_ok = sub_ok and _declared_rows_match(subject, difference, "subject_rows")
    if ref_ok and sub_ok:
        return DECLARED, "{}: {}".format(difference["id"], difference["reason"])
    return FAIL, "difference {} does not match its declaration: {}".format(
        difference["id"], "; ".join(differences)
    )


@dataclass
class Tally:
    steps: dict[str, int] = field(
        default_factory=lambda: {verdict: 0 for verdict in VERDICTS}
    )

    def add(self, verdict: str) -> None:
        self.steps[verdict] += 1

    def exit_code(self) -> int:
        """0: 실패·누락·미실행 없음, 1: fail·missing 있음, 2: not-run이 있거나 pass가 없음."""
        if self.steps[FAIL] > 0 or self.steps[MISSING] > 0:
            return 1
        if self.steps[NOT_RUN] > 0 or self.steps[PASS] == 0:
            return 2
        return 0

    def format(self) -> str:
        counts = " ".join(f"{verdict}={self.steps[verdict]}" for verdict in VERDICTS)
        return f"steps: {counts} (only pass counts as passed)"
