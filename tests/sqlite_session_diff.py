#!/usr/bin/env python3
"""#103: SQLite 3.51.0과 이 DB를 같은 입력의 두 세션 케이스로 비교한다.

케이스(tests/sqlite_session_cases.json)는 연결 ID, 단계별 SQL·매개변수, 재시작, 관찰 질의,
비교 정책을 적는다. 각 케이스는 두 엔진 프로세스(SQLite 어댑터, tests/session_probe)를 새로
띄워 같은 단계를 같은 순서로 보낸다.
단계 판정은 pass, fail, missing, not-run, declared-difference이며 pass만 통과다.
종료 코드: 0 전부 pass 또는 선언된 차이, 1 fail 또는 missing 있음, 2 not-run이 있거나 pass가 없음.

작업 폴더(--workdir)는 아직 없는 경로여야 한다. 이 검사가 만든 폴더만 끝에 지운다
(--keep-workdir로 보존). 데이터는 모두 합성 데이터다.
"""

from __future__ import annotations

import argparse
import json
import logging
import shutil
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

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
    summarize,
    unmet_expectation,
)
from sqlite_session_engine import EngineSession, FaultReplyError, case_database_name
from sqlite_session_protocol import EngineError, Outcome

logger = logging.getLogger(__name__)

EXPECTED_SQLITE_VERSION = "3.51.0"
CASES_PATH = Path(__file__).with_name("sqlite_session_cases.json")
ADAPTER_PATH = Path(__file__).with_name("sqlite_session_adapter.py")
SQLITE_STATEMENT_MAP = {
    "BEGIN": "BEGIN IMMEDIATE",
    "BEGIN DURABLE": "BEGIN IMMEDIATE",
    "BEGIN VOLATILE": "BEGIN IMMEDIATE",
}


class CaseFileError(ValueError):
    """케이스 파일이 형식에 맞지 않는다."""


def validate_cases(cases: Sequence[dict[str, Any]]) -> None:
    seen = set()
    for case in cases:
        case_id = case.get("id", "")
        if not case_id or case_id in seen:
            raise CaseFileError(f"case id missing or duplicated: {case_id!r}")
        seen.add(case_id)
        known = set(case.get("connections", []))
        for index, step in enumerate(case.get("steps", [])):
            step.setdefault("id", f"s{index + 1:02d}")
            if "restart" not in step and "drop" not in step and step.get("conn") not in known:
                raise CaseFileError(f"{case_id} step {index} uses an undeclared connection")
            if _is_unguarded_select(step):
                raise CaseFileError(f"{case_id} step {index} is a SELECT without expect.rows")


def _is_unguarded_select(step: dict[str, Any]) -> bool:
    sql = step.get("sql", "").lstrip().upper()
    expect = step.get("expect", {})
    guarded = "rows" in expect or expect.get("kind") == "error" or "difference" in step
    return sql.startswith("SELECT") and not guarded


# cost: time O(n), heap O(n), stack O(1), io 1 file read
# vars: n = case file bytes
# basis: estimate
def load_cases(path: Path) -> list[dict[str, Any]]:
    with path.open(encoding="utf-8") as handle:
        document = json.load(handle)
    cases = document["cases"]
    validate_cases(cases)
    return cases


def sqlite_statement(step: dict[str, Any]) -> str:
    if "sql_sqlite" in step:
        return step["sql_sqlite"]
    sql = step["sql"]
    return SQLITE_STATEMENT_MAP.get(" ".join(sql.upper().split()), sql)


class CaseRunner:
    """케이스 하나를 두 엔진에 같은 단계로 실행하고 단계 판정을 모은다."""

    def __init__(self, reference: EngineSession, subject: EngineSession) -> None:
        self._reference = reference
        self._subject = subject

    def run(self, case: dict[str, Any]) -> list[StepVerdict]:
        verdicts = [self._setup(case)]
        aborted: str | None = _abort_reason(verdicts[-1])
        for step in case["steps"]:
            if aborted is not None:
                verdicts.append(StepVerdict(case["id"], step["id"], NOT_RUN, aborted))
                continue
            verdicts.append(self._run_step(case, step))
            aborted = _abort_reason(verdicts[-1])
        return verdicts

    def _setup(self, case: dict[str, Any]) -> StepVerdict:
        try:
            for engine in (self._reference, self._subject):
                engine.open_fresh(case["connections"])
        except FaultReplyError as err:
            return StepVerdict(case["id"], "setup", FAIL, f"adapter fault: {err}")
        except EngineError as err:
            return StepVerdict(case["id"], "setup", MISSING, f"no response during setup: {err}")
        return StepVerdict(case["id"], "setup", PASS)

    def _run_step(self, case: dict[str, Any], step: dict[str, Any]) -> StepVerdict:
        try:
            if "restart" in step:
                return self._restart(case, step)
            if "drop" in step:
                return self._drop(case, step)
            return self._execute(case, step)
        except FaultReplyError as err:
            return StepVerdict(case["id"], step["id"], FAIL, f"adapter fault: {err}")
        except EngineError as err:
            return StepVerdict(case["id"], step["id"], MISSING, f"no response: {err}")

    def _restart(self, case: dict[str, Any], step: dict[str, Any]) -> StepVerdict:
        for engine in (self._reference, self._subject):
            engine.restart(case["connections"], crash=step["restart"] == "crash")
        return StepVerdict(case["id"], step["id"], PASS, "restart {}".format(step["restart"]))

    def _drop(self, case: dict[str, Any], step: dict[str, Any]) -> StepVerdict:
        for engine in (self._reference, self._subject):
            engine.drop_session(step["drop"])
        return StepVerdict(case["id"], step["id"], PASS, "dropped session {}".format(step["drop"]))

    def _execute(self, case: dict[str, Any], step: dict[str, Any]) -> StepVerdict:
        params = step.get("params", [])
        reference = self._reference.execute(step["conn"], sqlite_statement(step), params)
        subject = self._subject.execute(step["conn"], step["sql"], params)
        verdict, detail = judge_step(step, reference, subject)
        return StepVerdict(
            case["id"], step["id"], verdict, detail, summarize(reference), summarize(subject)
        )


def judge_step(step: dict[str, Any], reference: Outcome, subject: Outcome) -> tuple[str, str]:
    """한 단계의 두 결과를 판정한다. (판정, 설명)."""
    differences = compare_outcomes(
        reference, subject, step.get("compare"), ordered=step.get("ordered", False)
    )
    if "difference" in step:
        return judge_difference(step["difference"], reference, subject, differences)
    problems = []
    for label, outcome in (("reference", reference), ("subject", subject)):
        unmet = unmet_expectation(outcome, step.get("expect", {}))
        if unmet is not None:
            problems.append(f"{label}: {unmet}")
    problems += differences
    return (FAIL, "; ".join(problems)) if problems else (PASS, "")


def _abort_reason(verdict: StepVerdict) -> str | None:
    """응답이 없거나 어댑터가 수행을 거절하면 상태를 믿을 수 없어 뒤 단계를 실행하지 않는다."""
    if verdict.verdict == MISSING or verdict.detail.startswith("adapter fault"):
        return f"aborted after {verdict.step_id} {verdict.verdict}"
    return None


def not_run_case(case: dict[str, Any], reason: str) -> list[StepVerdict]:
    ids = ["setup"] + [step["id"] for step in case["steps"]]
    return [StepVerdict(case["id"], step_id, NOT_RUN, reason) for step_id in ids]


def run_case(
    case: dict[str, Any], probe: Path, workdir: Path, timeout_s: float
) -> list[StepVerdict]:
    case_dir = workdir / case["id"]
    case_dir.mkdir()
    wal = bool(case.get("wal", True))
    reference = EngineSession(
        "sqlite",
        [sys.executable, str(ADAPTER_PATH)],
        case_dir / case_database_name("sqlite"),
        wal=wal,
        timeout_s=timeout_s,
    )
    subject = EngineSession(
        "subject",
        [str(probe)],
        case_dir / case_database_name("subject"),
        wal=wal,
        timeout_s=timeout_s,
    )
    try:
        reference.start()
        subject.start()
        verdicts = CaseRunner(reference, subject).run(case)
    except EngineError as err:
        verdicts = not_run_case(case, f"engine could not start: {err}")
    finally:
        problems = [reference.stop(), subject.stop()]
    return verdicts + [
        StepVerdict(case["id"], "shutdown", FAIL, problem) for problem in problems if problem
    ]


def sqlite_version(timeout_s: float) -> str | None:
    engine = EngineSession(
        "sqlite",
        [sys.executable, str(ADAPTER_PATH)],
        Path("unused"),
        wal=False,
        timeout_s=timeout_s,
    )
    try:
        engine.start()
        return engine.version
    except EngineError:
        return None
    finally:
        engine.stop()


def run_all(
    cases: Sequence[dict[str, Any]],
    probe: Path,
    workdir: Path,
    timeout_s: float,
    expected_version: str,
) -> dict[str, list[StepVerdict]]:
    version = sqlite_version(timeout_s)
    results: dict[str, list[StepVerdict]] = {}
    for case in cases:
        if version != expected_version:
            reason = f"sqlite {version} found, {expected_version} required"
            results[case["id"]] = not_run_case(case, reason)
        elif not probe.exists():
            results[case["id"]] = not_run_case(case, f"subject adapter missing: {probe}")
        else:
            results[case["id"]] = run_case(case, probe, workdir, timeout_s)
    return results


def tally_results(results: dict[str, list[StepVerdict]]) -> Tally:
    tally = Tally()
    for verdicts in results.values():
        for verdict in verdicts:
            tally.add(verdict.verdict)
    return tally


def format_results(results: dict[str, list[StepVerdict]]) -> str:
    lines: list[str] = []
    for case_id, verdicts in results.items():
        bad = [v for v in verdicts if v.verdict not in (PASS, DECLARED)]
        lines.append(f"case {case_id}: {len(verdicts)} steps, {len(bad)} not passed")
        lines += [f"  {v.verdict.upper()} {v.step_id}: {v.detail}" for v in bad]
    declared = [v for verdicts in results.values() for v in verdicts if v.verdict == DECLARED]
    lines += [f"declared difference {v.step_id}: {v.detail}" for v in declared]
    lines.append(tally_results(results).format())
    return "\n".join(lines)


def write_report(path: Path, results: dict[str, list[StepVerdict]], version: str) -> None:
    document = {
        "expected_sqlite": version,
        "tally": tally_results(results).steps,
        "cases": {
            case_id: [verdict.__dict__ for verdict in verdicts]
            for case_id, verdicts in results.items()
        },
    }
    path.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding="utf-8")


def parse_arguments(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--workdir", required=True, type=Path)
    parser.add_argument("--cases", type=Path, default=CASES_PATH)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument(
        "--expected-sqlite-version",
        default=EXPECTED_SQLITE_VERSION,
        help="진단 전용. 기본값이 아닌 버전의 결과는 3.51.0과의 비교로 주장하지 않는다",
    )
    parser.add_argument("--report", type=Path)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--keep-workdir", action="store_true")
    return parser.parse_args(argv)


def select_cases(cases: list[dict[str, Any]], only: Sequence[str]) -> list[dict[str, Any]]:
    if not only:
        return cases
    unknown = set(only) - {case["id"] for case in cases}
    if unknown:
        raise CaseFileError("unknown case ids: {}".format(", ".join(sorted(unknown))))
    return [case for case in cases if case["id"] in only]


# cost: time O(c·s·b) bounded by engine round trips, heap O(b), stack O(1),
# cost: io 2 process spawns per case
# vars: c = cases, s = steps per case, b = largest request bytes
# basis: estimate
def main(argv: Sequence[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    args = parse_arguments(argv)
    if args.workdir.exists():
        logger.error("workdir already exists: %s", args.workdir)
        return 2
    try:
        cases = load_cases(args.cases)
        selected = select_cases(cases, args.only)
    except (CaseFileError, OSError, KeyError, json.JSONDecodeError) as err:
        logger.error("invalid case file %s: %s", args.cases, err)
        return 2
    args.workdir.mkdir(parents=True)
    try:
        results = run_all_selected(cases, selected, args)
    finally:
        if not args.keep_workdir:
            shutil.rmtree(args.workdir)
    print(f"sqlite version required: {args.expected_sqlite_version}")
    print(format_results(results))
    if args.report is not None:
        write_report(args.report, results, args.expected_sqlite_version)
    return tally_results(results).exit_code()


def run_all_selected(
    cases: list[dict[str, Any]], selected: list[dict[str, Any]], args: argparse.Namespace
) -> dict[str, list[StepVerdict]]:
    """--only로 고르지 않은 케이스는 통과로 세지 않도록 not-run으로 남긴다."""
    chosen = {case["id"] for case in selected}
    results = run_all(
        selected, args.probe, args.workdir, args.timeout, args.expected_sqlite_version
    )
    for case in cases:
        if case["id"] not in chosen:
            results[case["id"]] = not_run_case(case, "not selected")
    return results


if __name__ == "__main__":
    sys.exit(main())
