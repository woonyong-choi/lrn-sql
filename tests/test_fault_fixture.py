"""#133: 최소 C 입력의 정상·오류·kill과 동일 seed의 trace 재현을 검사한다."""

from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from typing import Any

import pytest

RUNNER = Path(__file__).with_name("fault_runner.py")
FIXTURE = Path(os.environ.get("FAULT_FIXTURE", "build/fault_fixture")).resolve()


def run_fixture(
    directory: Path, scenario: str, site: str | None = None, call: int = 1
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    report_path = directory / "report.json"
    command = [
        sys.executable,
        str(RUNNER),
        "--report",
        str(report_path),
        "--timeout",
        "5",
        "--seed",
        "103",
        "--call",
        str(call),
    ]
    if site:
        command += [
            "--site",
            site,
            "--expect",
            "signal" if scenario == "kill" else "error",
            "--expect-code",
            str(int(signal.SIGKILL)) if scenario == "kill" else "42",
        ]
    command += ["--", str(FIXTURE), scenario]
    if scenario == "file":
        command.append(str(directory / "exclusive.db"))
    run = subprocess.run(command, capture_output=True, text=True, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
    report = json.loads(report_path.read_text())
    trace = [
        json.loads(line) for line in Path(report["stderr"]).read_text().splitlines()
    ]
    return report, trace


@pytest.mark.parametrize(
    ("scenario", "site", "call"),
    [
        ("alloc", None, 1),
        ("alloc", "alloc.malloc", 1),
        ("alloc", "alloc.realloc", 1),
        ("alloc", "alloc.realloc", 2),
        ("lock", None, 1),
        ("lock", "lock.wait", 1),
        ("file", None, 1),
        ("file", "file.pwrite", 1),
        ("file", "file.fsync", 1),
        ("kill", None, 1),
        ("kill", "process.kill", 1),
    ],
)
def test_fixture_outcome_and_trace(
    tmp_path: Path, scenario: str, site: str | None, call: int
) -> None:
    report, trace = run_fixture(tmp_path, scenario, site, call)
    injections = [event for event in trace if event["inject"]]
    assert len(injections) == (1 if site else 0)
    assert [event["sequence"] for event in trace] == list(range(1, len(trace) + 1))
    if site:
        assert injections[0]["site"] == site and injections[0]["call"] == call
    if scenario != "kill" or not site:
        observed = json.loads(Path(report["stdout"]).read_text())
        assert observed["status"] == (42 if site else 0)
        assert observed["resources"] == {"allocations": [], "locks": [], "fd": []}
    assert not (tmp_path / "exclusive.db").exists()


def test_same_seed_and_failure_replays_identical_origin(tmp_path: Path) -> None:
    _, first = run_fixture(tmp_path, "alloc", "alloc.realloc", 2)
    _, second = run_fixture(tmp_path, "alloc", "alloc.realloc", 2)
    assert first == second
    assert all(event["seed"] == 103 for event in first)


def test_existing_file_is_never_truncated(tmp_path: Path) -> None:
    existing = tmp_path / "existing.db"
    existing.write_bytes(b"preserve")
    env = {k: v for k, v in os.environ.items() if not k.startswith("FAULT_")}
    run = subprocess.run(
        [str(FIXTURE), "file", str(existing)], env=env, check=False, capture_output=True
    )
    assert run.returncode == 70
    assert existing.read_bytes() == b"preserve"
