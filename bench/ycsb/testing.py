from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

from analysis import analyze, read_records, verify
from runner import process, write_json

from build import BUILD, ROOT, SOURCE, build, java_command


def test(output: Path) -> None:
    build()
    unit_tests = unittest.defaultTestLoader.discover(
        str(SOURCE / "tests"), pattern="test_*.py"
    )
    result = unittest.TextTestRunner(verbosity=2).run(unit_tests)
    if not result.wasSuccessful():
        raise RuntimeError("runner or analyzer unit test failed")
    checks = output / "adapter-checks"
    if checks.exists():
        raise ValueError("test output already exists: " + str(checks))
    checks.mkdir(parents=True)
    for engine in ("totedb", "sqlite"):
        directory = checks / engine
        directory.mkdir()
        database = BUILD / ("adapter-check-" + engine + ".db")
        if database.exists():
            database.unlink()
        command = java_command("org.totedb.ycsb.Tool", "smoke", engine, str(database))
        command.insert(1, "-Xcheck:jni")
        execution = process(command, directory, 120)
        write_json(directory / "process.json", execution)
        if execution["exit_code"] != 0 or execution["timeout"]:
            raise RuntimeError("adapter smoke failed: " + str(directory))
        if (
            "WARNING" in (directory / "stdout.txt").read_text()
            or "WARNING" in (directory / "stderr.txt").read_text()
        ):
            raise RuntimeError("JNI checker warning: " + engine)
    run_id = "checks-" + str(__import__("os").getpid())
    command = [
        sys.executable,
        str(SOURCE / "run.py"),
        "collect",
        "--output",
        str(output.resolve()),
        "--run-id",
        run_id,
        "--sizes",
        "30",
        "--workloads",
        "A,B,C,D,E,F",
        "--threads",
        "1,4",
        "--repetitions",
        "1",
        "--operations",
        "40",
        "--timeout",
        "60",
    ]
    directory = ROOT / "docs" / "experiments" / "ycsb-sqlite"
    with (
        (checks / "collect-stdout.txt").open("w") as stdout,
        (checks / "collect-stderr.txt").open("w") as stderr,
    ):
        execution = subprocess.run(
            command, cwd=directory, stdout=stdout, stderr=stderr, check=False
        )
    write_json(
        checks / "collect-process.json",
        {"command": command, "cwd": str(directory), "exit_code": execution.returncode},
    )
    if execution.returncode != 0:
        raise RuntimeError("official client CLI failed: " + str(checks))
    trials, _ = read_records(output / "data" / "raw")
    summary = analyze(output)
    verify(output)
    if any(trial["problems"] for trial in trials):
        raise RuntimeError("official client smoke failed; see preserved raw output")
    write_json(
        checks / "result.json",
        {
            "adapter": "pass",
            "official_client": "pass",
            "trials": len(trials),
            "measured_runs": summary["measured_runs"],
            "preflight_runs": summary["preflight_runs"],
        },
    )
    print(
        json.dumps(
            {"test": "pass", "output": str(output), "official_trials": len(trials)}
        )
    )
