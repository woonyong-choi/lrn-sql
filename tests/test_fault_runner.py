"""#133: 실제 자식 프로세스의 종료·시간초과와 원출력·재실행 입력 계약."""

from __future__ import annotations

import json
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest

RUNNER = Path(__file__).with_name("fault_runner.py")


def _run(folder: Path, source: str, options: list[str]) -> tuple[int, dict]:
    report = folder / "result.json"
    completed = subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--report",
            str(report),
            *options,
            "--",
            sys.executable,
            "-c",
            source,
        ],
        capture_output=True,
        text=True,
        timeout=5,
    )
    return completed.returncode, json.loads(report.read_text())


@pytest.mark.parametrize(
    ("code", "options", "expected", "matched"),
    [
        (0, [], "success", True),
        (7, ["--expect", "error", "--expect-code", "7"], "error", True),
        (8, ["--expect", "error", "--expect-code", "7"], "error", False),
    ],
)
def test_exit_code_matches_only_the_requested_result(
    tmp_path: Path,
    code: int,
    options: list[str],
    expected: str,
    matched: bool,
) -> None:
    status, result = _run(
        tmp_path,
        f"import sys; print('out'); print('err',file=sys.stderr); sys.exit({code})",
        options,
    )
    assert status == (0 if matched else 1)
    assert result["outcome"] == expected
    assert result["matched"] is matched
    assert Path(result["stdout"]).read_text() == "out\n"
    assert Path(result["stderr"]).read_text() == "err\n"


def test_signal_is_distinct_from_error(tmp_path: Path) -> None:
    source = "import os,signal; os.kill(os.getpid(),signal.SIGTERM)"
    status, result = _run(
        tmp_path, source, ["--expect", "signal", "--expect-code", str(signal.SIGTERM)]
    )
    assert status == 0
    assert result["outcome"] == "signal"
    assert result["code"] == signal.SIGTERM
    assert result["returncode"] == -signal.SIGTERM


def test_timeout_kills_descendants_and_is_not_a_signal_result(tmp_path: Path) -> None:
    marker = tmp_path / "escaped-child"
    child = (
        f"import time,pathlib; time.sleep(0.5); pathlib.Path({str(marker)!r}).touch()"
    )
    source = f"import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',{child!r}]); time.sleep(60)"
    status, result = _run(
        tmp_path, source, ["--expect", "timeout", "--timeout", "0.15"]
    )
    assert status == 0
    assert result["outcome"] == "timeout"
    time.sleep(0.6)
    assert not marker.exists()


def test_seed_site_and_call_are_reproducible_inputs(tmp_path: Path) -> None:
    source = (
        "import os,json,random; random.seed(int(os.environ['FAULT_SEED'])); "
        "print(json.dumps([os.environ['FAULT_SITE'],os.environ['FAULT_CALL'],"
        "random.randrange(1000000)]))"
    )
    options = ["--seed", "9281", "--site", "pager.read", "--call", "2"]
    status, first = _run(tmp_path / "first", source, options)
    assert status == 0
    status, second = _run(tmp_path / "second", source, options)
    assert status == 0
    assert Path(first["stdout"]).read_bytes() == Path(second["stdout"]).read_bytes()
    assert first["seed"] == second["seed"] == 9281
    assert first["site"] == "pager.read" and first["call"] == 2


def test_launch_failure_is_never_an_expected_error(tmp_path: Path) -> None:
    report = tmp_path / "result.json"
    completed = subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--report",
            str(report),
            "--expect",
            "error",
            "--expect-code",
            "2",
            "--",
            str(tmp_path / "missing-command"),
        ],
        capture_output=True,
        timeout=5,
    )
    result = json.loads(report.read_text())
    assert completed.returncode == 1
    assert result["outcome"] == "launch_error"
    assert not result["matched"]


@pytest.mark.parametrize("value", ["nan", "inf", "0", "-1"])
def test_invalid_timeout_is_rejected_without_launch(tmp_path: Path, value: str) -> None:
    report = tmp_path / "result.json"
    completed = subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--report",
            str(report),
            "--timeout",
            value,
            "--",
            sys.executable,
            "-c",
            "raise AssertionError('must not run')",
        ],
        capture_output=True,
        timeout=5,
    )
    assert completed.returncode == 2
    assert not report.exists()
