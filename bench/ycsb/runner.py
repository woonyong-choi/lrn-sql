from __future__ import annotations

import datetime
import hashlib
import itertools
import json
import os
import platform
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from analysis import parse_metrics, validate_trial, write_checksums

from build import BUILD, ROOT, SOURCE, java_command


@dataclass(frozen=True)
class CollectionOptions:
    sizes: list[int]
    workloads: list[str]
    threads: list[int]
    repetitions: int
    operations: int
    timeout: float
    profile: bool = False


@dataclass(frozen=True)
class Trial:
    run_id: str
    trial_id: str
    condition: dict[str, Any]
    repetition: int
    phase: str
    git_sha: str


def timestamp() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def process(
    command: list[str], directory: Path, timeout: float, profile: bool = False
) -> dict[str, Any]:
    started = time.monotonic()
    started_utc = timestamp()
    before = os.getloadavg()
    profiler = None
    with (
        (directory / "stdout.txt").open("w") as stdout,
        (directory / "stderr.txt").open("w") as stderr,
    ):
        child = subprocess.Popen(
            command, cwd=ROOT, stdout=stdout, stderr=stderr, start_new_session=True
        )
        timed_out = False
        sampled = False
        while True:
            pid, status, usage = os.wait4(child.pid, os.WNOHANG)
            if pid:
                child.returncode = os.waitstatus_to_exitcode(status)
                break
            elapsed = time.monotonic() - started
            if (
                profile
                and not sampled
                and elapsed >= 0.1
                and platform.system() == "Darwin"
            ):
                sampled = True
                profiler = subprocess.Popen(
                    [
                        "/usr/bin/sample",
                        str(child.pid),
                        "2",
                        "1",
                        "-file",
                        str(directory / "sample.txt"),
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            if elapsed >= timeout and not timed_out:
                timed_out = True
                os.killpg(child.pid, signal.SIGKILL)
            time.sleep(0.02)
    process_elapsed = time.monotonic() - started
    profile_wait_started = time.monotonic()
    if profiler is not None:
        try:
            profiler.wait(timeout=10)
        except subprocess.TimeoutExpired:
            profiler.kill()
            profiler.wait()
    sample = directory / "sample.txt"
    return {
        "command": command,
        "ts_utc": started_utc,
        "exit_code": child.returncode,
        "timeout": timed_out,
        "elapsed_seconds": process_elapsed,
        "profiler_wait_seconds": time.monotonic() - profile_wait_started,
        "cpu_user_seconds": usage.ru_utime,
        "cpu_system_seconds": usage.ru_stime,
        "max_rss_bytes": usage.ru_maxrss
        * (1 if platform.system() == "Darwin" else 1024),
        "major_faults": usage.ru_majflt,
        "minor_faults": usage.ru_minflt,
        "load_before": before,
        "load_after": os.getloadavg(),
        "sample_exit_code": None if profiler is None else profiler.returncode,
        "sample_available": sample.exists() and sample.stat().st_size > 0,
    }


def environment() -> dict[str, Any]:
    config = json.loads((BUILD / "build.json").read_text())
    result = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "git_sha": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "git_status": subprocess.check_output(
            ["git", "status", "--short"], cwd=ROOT, text=True
        ),
        "build": config,
        "disk_free_bytes": shutil.disk_usage(ROOT).free,
        "ts_utc": timestamp(),
        "versions": json.loads(
            subprocess.check_output(
                java_command("org.totedb.ycsb.Tool", "version"), cwd=ROOT, text=True
            )
        ),
    }
    if platform.system() == "Darwin":
        result["hardware"] = subprocess.check_output(
            ["sysctl", "hw.model", "hw.ncpu", "hw.memsize", "machdep.cpu.brand_string"],
            text=True,
        )
    sources = {}
    for path in sorted(SOURCE.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            sources[path.relative_to(ROOT).as_posix()] = hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
    result["source_sha256"] = sources
    return result


def properties(
    condition: dict[str, Any], database: Path, integrity: bool
) -> dict[str, str]:
    return dict(
        engine=condition["engine"],
        dbpath=str(database),
        recordcount=str(condition["records"]),
        operationcount=str(condition["operations"]),
        fieldcount="10",
        fieldlength="100",
        fieldlengthdistribution="constant",
        dataintegrity=str(integrity).lower(),
        readallfields="true",
        writeallfields="false",
        busyretryms="60000",
        measurementtype="hdrhistogram",
        **{
            "hdrhistogram.percentiles": "50,95,99",
            "exporter": "site.ycsb.measurements.exporter.TextMeasurementsExporter",
        },
    )


def client(
    condition: dict[str, Any],
    database: Path,
    directory: Path,
    phase: str,
    timeout: float,
    profile: bool = False,
) -> tuple[dict[str, Any], dict[str, Any]]:
    values = properties(condition, database, phase in ("load", "preflight"))
    property_path = directory / "run.properties"
    property_path.write_text(
        "".join(key + "=" + value + "\n" for key, value in sorted(values.items()))
    )
    command = java_command(
        "site.ycsb.Client",
        "-load" if phase == "load" else "-t",
        "-db",
        "org.totedb.ycsb.SqlBinding",
        "-P",
        str(SOURCE / "workloads" / ("workload" + condition["workload"].lower())),
        "-P",
        str(property_path),
        "-threads",
        str(condition["threads"]),
    )
    result = process(command, directory, timeout, profile)
    write_json(directory / "process.json", result)
    metrics = parse_metrics(
        (directory / "stdout.txt").read_text(), (directory / "stderr.txt").read_text()
    )
    return result, metrics


def audit(
    engine: str,
    database: Path,
    snapshot: Path,
    baseline: Path | None,
    directory: Path,
    label: str,
    timeout: float,
) -> dict[str, Any] | None:
    arguments = ["audit", engine, str(database), str(snapshot)]
    if baseline is not None:
        arguments.append(str(baseline))
    audit_dir = directory / ("audit-" + label)
    audit_dir.mkdir()
    result = process(
        java_command("org.totedb.ycsb.Tool", *arguments), audit_dir, timeout
    )
    write_json(audit_dir / "process.json", result)
    if result["exit_code"] != 0 or result["timeout"]:
        return None
    parsed = json.loads((audit_dir / "stdout.txt").read_text())
    write_json(directory / ("audit-" + label + ".json"), parsed)
    return parsed


def create_baseline(
    engine: str, records: int, work: Path, raw: Path, timeout: float
) -> dict[str, Any]:
    label = f"{records}-{engine}"
    directory = raw / "baselines" / label
    directory.mkdir(parents=True)
    database = work / (label + ".db")
    setup = directory / "setup"
    setup.mkdir()
    setup_result = process(
        java_command("org.totedb.ycsb.Tool", "setup", engine, str(database)),
        setup,
        timeout,
    )
    write_json(setup / "process.json", setup_result)
    if setup_result["exit_code"] != 0:
        raise RuntimeError("baseline setup failed: " + label)
    condition = {
        "records": records,
        "operations": records,
        "workload": "A",
        "threads": 1,
        "engine": engine,
    }
    execution, metrics = client(condition, database, directory, "load", timeout)
    snapshot = work / (label + ".snapshot")
    checked = audit(engine, database, snapshot, None, directory, "loaded", timeout)
    record = {
        "condition": condition,
        "process": execution,
        "metrics": metrics,
        "audit": checked,
        "database_size_bytes": database.stat().st_size,
    }
    write_json(directory / "record.json", record)
    if (
        execution["exit_code"] != 0
        or execution["timeout"]
        or metrics["non_ok"]
        or metrics["logical_operations"] != records
    ):
        raise RuntimeError("official baseline load failed: " + label)
    if (
        checked is None
        or checked["rows"] != records
        or checked["payload_bytes"] != records * 1000
    ):
        raise RuntimeError("baseline audit failed: " + label)
    return {"database": database, "snapshot": snapshot, "audit": checked}


def execute_trial(
    trial: Trial,
    baseline: dict[str, Any],
    raw: Path,
    work: Path,
    timeout: float,
    profile: bool = False,
) -> dict[str, Any]:
    run_id, trial_id = trial.run_id, trial.trial_id
    condition, repetition, phase = trial.condition, trial.repetition, trial.phase
    git_sha = trial.git_sha
    directory = raw / "trials" / trial_id
    directory.mkdir(parents=True)
    database = work / (trial_id + ".db")
    shutil.copyfile(baseline["database"], database)
    snapshot = work / (trial_id + ".snapshot")
    before = audit(
        condition["engine"],
        database,
        snapshot,
        baseline["snapshot"],
        directory,
        "before",
        timeout,
    )
    execution, metrics = client(condition, database, directory, phase, timeout, profile)
    after = audit(
        condition["engine"],
        database,
        snapshot,
        baseline["snapshot"],
        directory,
        "after",
        timeout,
    )
    record = {
        "schema_version": 1,
        "run_id": run_id,
        "trial_id": trial_id,
        "phase": phase,
        "repetition": repetition,
        "condition": condition,
        "ts_utc": execution["ts_utc"],
        "input_identifier": baseline["audit"]["sha256"],
        "git_sha": git_sha,
        "process": execution,
        "metrics": metrics,
        "audit_before": before,
        "audit_after": after,
        "database_size_bytes": database.stat().st_size,
    }
    record["problems"] = validate_trial(record)
    write_json(directory / "record.json", record)
    with (raw / "records.jsonl").open("a") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    if not record["problems"]:
        database.unlink()
        if snapshot.exists():
            snapshot.unlink()
    print(
        json.dumps(
            {
                "trial_id": trial_id,
                "problems": record["problems"],
                "elapsed_seconds": execution["elapsed_seconds"],
            }
        ),
        flush=True,
    )
    return record


def collect(
    experiment: Path, run_id: str, options: CollectionOptions
) -> list[dict[str, Any]]:
    sizes, workloads, threads = options.sizes, options.workloads, options.threads
    repetitions, operations = options.repetitions, options.operations
    timeout, profile = options.timeout, options.profile
    raw = experiment / "data" / "raw" / run_id
    if raw.exists():
        raise ValueError("raw output already exists: " + str(raw))
    if any((experiment / "data" / "raw").glob("*/manifest.json")):
        raise ValueError(
            "experiment output already contains a collection: "
            + str(experiment)
            + "; existing raw data is preserved, use a new --output directory"
        )
    work = BUILD / "runs" / run_id
    if work.exists():
        raise ValueError("generated database directory already exists: " + str(work))
    raw.mkdir(parents=True)
    work.mkdir(parents=True)
    env = environment()
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "ts_utc": timestamp(),
        "sizes": sizes,
        "workloads": workloads,
        "threads": threads,
        "repetitions": repetitions,
        "operations": operations,
        "timeout_seconds": timeout,
        "profile": profile,
        "expected_measured_runs": len(sizes)
        * len(workloads)
        * len(threads)
        * 2
        * repetitions,
        "expected_preflight_runs": len(sizes) * len(workloads) * len(threads) * 2,
        "fresh_jvm": True,
        "jit_warmup": False,
        "filesystem_cache": "uncontrolled; full audit read immediately before each measured process",
        "request_trace": "independent ThreadLocalRandom",
        "environment": env,
    }
    write_json(raw / "manifest.json", manifest)
    if not (experiment / "env.json").exists():
        write_json(experiment / "env.json", env)
    trials = []
    try:
        baselines = {}
        for records in sizes:
            for engine in ("totedb", "sqlite"):
                baselines[(records, engine)] = create_baseline(
                    engine, records, work, raw, timeout
                )
            if (
                baselines[(records, "totedb")]["audit"]["sha256"]
                != baselines[(records, "sqlite")]["audit"]["sha256"]
            ):
                raise RuntimeError("baseline canonical digest differs between engines")
        for records, workload, workers in itertools.product(sizes, workloads, threads):
            for repetition in range(repetitions + 1):
                phase = "preflight" if repetition == 0 else "measured"
                engines = (
                    ("totedb", "sqlite")
                    if repetition == 0 or repetition % 2 == 1
                    else ("sqlite", "totedb")
                )
                for engine in engines:
                    condition = {
                        "records": records,
                        "workload": workload,
                        "threads": workers,
                        "engine": engine,
                        "operations": operations,
                    }
                    trial_id = (
                        f"{records}-{workload}-t{workers}-{phase}-{repetition}-{engine}"
                    )
                    trials.append(
                        execute_trial(
                            Trial(
                                run_id,
                                trial_id,
                                condition,
                                repetition,
                                phase,
                                env["git_sha"],
                            ),
                            baselines[(records, engine)],
                            raw,
                            work,
                            timeout,
                            profile,
                        )
                    )
    finally:
        write_checksums(experiment)
    return trials
