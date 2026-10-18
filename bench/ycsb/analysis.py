from __future__ import annotations

import csv
import hashlib
import io
import itertools
import json
import math
import random
import statistics
from pathlib import Path
from typing import Any

OPERATIONS = ("READ", "UPDATE", "INSERT", "SCAN", "DELETE")


def parse_metrics(stdout: str, stderr: str) -> dict[str, Any]:
    metrics: dict[str, dict[str, float]] = {}
    for row in csv.reader(io.StringIO(stdout)):
        if len(row) != 3 or not row[0].startswith("["):
            continue
        operation, name, value = row[0].strip("[]"), row[1].strip(), row[2].strip()
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError("non-finite YCSB metric")
        metrics.setdefault(operation, {})[name] = parsed
    binding: dict[str, int] = {}
    workers = 0
    for line in stderr.splitlines():
        if not line.startswith("BINDING "):
            continue
        workers += 1
        for pair in line.split()[1:]:
            name, value = pair.split("=", 1)
            binding[name] = binding.get(name, 0) + int(value)
    calls = sum(
        metrics.get(operation, {}).get("Operations", 0) for operation in OPERATIONS
    )
    logical = calls - metrics.get("READ-MODIFY-WRITE", {}).get("Operations", 0)
    non_ok = sum(
        value
        for values in metrics.values()
        for name, value in values.items()
        if name.startswith("Return=") and name != "Return=OK"
    )
    return {
        "ycsb": metrics,
        "binding": binding,
        "workers": workers,
        "sql_calls": calls,
        "logical_operations": logical,
        "non_ok": non_ok,
        "binding_errors": sum(
            line.startswith("BINDING_ERROR ") for line in stderr.splitlines()
        ),
    }


def validate_trial(record: dict[str, Any]) -> list[str]:
    problems = []
    process = record["process"]
    metrics = record["metrics"]
    condition = record["condition"]
    if process["timeout"]:
        problems.append("timeout")
    if process["exit_code"] != 0:
        problems.append("nonzero_exit")
    if metrics["non_ok"] or metrics["binding_errors"]:
        problems.append("operation_error")
    if metrics["logical_operations"] != condition["operations"]:
        problems.append("logical_operation_count")
    if metrics["workers"] != condition["threads"]:
        problems.append("worker_cleanup_count")
    binding = metrics["binding"]
    if binding.get("exhausted", 0):
        problems.append("busy_exhausted")
    if binding.get("attempts", 0) - binding.get("retries", 0) != metrics["sql_calls"]:
        problems.append("native_call_count")
    for operation in (*OPERATIONS, "READ-MODIFY-WRITE"):
        values = metrics["ycsb"].get(operation, {})
        if values.get("Operations", 0):
            for percentile in (50, 95, 99):
                name = f"{percentile}thPercentileLatency(us)"
                if name not in values or not math.isfinite(values[name]):
                    problems.append("missing_percentile_" + operation.lower())
        if operation == "READ-MODIFY-WRITE":
            continue
        if values.get("Operations", 0) != values.get("Return=OK", 0):
            problems.append("status_count_" + operation.lower())
    before, after = record.get("audit_before"), record.get("audit_after")
    if before is None or after is None:
        problems.append("audit_missing")
        return problems
    if before["sha256"] != record["input_identifier"]:
        problems.append("baseline_digest_mismatch")
    inserts = metrics["ycsb"].get("INSERT", {}).get("Return=OK", 0)
    if (
        before["rows"] != condition["records"]
        or after["rows"] != before["rows"] + inserts
    ):
        problems.append("row_count")
    if after["missing_rows"] != 0 or after["added_rows"] != inserts:
        problems.append("key_set")
    if after["payload_bytes"] != after["rows"] * 1000:
        problems.append("stored_payload_bytes")
    if condition["workload"] == "C" and after["sha256"] != before["sha256"]:
        problems.append("read_only_digest_changed")
    updates = metrics["ycsb"].get("UPDATE", {}).get("Return=OK", 0)
    if record["phase"] == "measured" and updates > 0 and after["changed_fields"] == 0:
        problems.append("updates_did_not_change_fields")
    if inserts > 0 and after["sha256"] == before["sha256"]:
        problems.append("inserts_did_not_change_digest")
    reads = metrics["ycsb"].get("READ", {}).get("Return=OK", 0)
    scans = metrics["ycsb"].get("SCAN", {}).get("Return=OK", 0)
    returned = binding.get("result_rows", 0)
    if not reads + scans <= returned <= reads + scans * 100:
        problems.append("returned_row_count")
    if binding.get("output_bytes", 0) != returned * 1000:
        problems.append("returned_payload_bytes")
    return problems


def describe(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {"n": 0, "median": None, "min": None, "max": None}
    return {
        "n": len(values),
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
    }


def bootstrap_ratios(ratios: list[float]) -> dict[str, Any]:
    if not ratios:
        return {"n": 0, "median": None, "ci95": None, "seed": 226, "resamples": 10000}
    randomizer = random.Random(226)
    samples = sorted(
        statistics.median(randomizer.choices(ratios, k=len(ratios)))
        for _ in range(10000)
    )

    def percentile(fraction: float) -> float:
        position = fraction * (len(samples) - 1)
        lower = int(position)
        return samples[lower] + (
            samples[min(lower + 1, len(samples) - 1)] - samples[lower]
        ) * (position - lower)

    return {
        "n": len(ratios),
        "median": statistics.median(ratios),
        "ci95": [percentile(0.025), percentile(0.975)],
        "ratios": ratios,
        "seed": 226,
        "resamples": 10000,
    }


def summarize(
    records: list[dict[str, Any]], expected_repetitions: int
) -> dict[str, Any]:
    conditions: dict[tuple[int, str, int], list[dict[str, Any]]] = {}
    for record in records:
        condition = record["condition"]
        key = (condition["records"], condition["workload"], condition["threads"])
        conditions.setdefault(key, []).append(record)
    summaries = []
    for key, trials in sorted(conditions.items()):
        measured = [trial for trial in trials if trial["phase"] == "measured"]
        preflight = [trial for trial in trials if trial["phase"] == "preflight"]
        failures = [
            {
                "trial_id": trial["trial_id"],
                "phase": trial["phase"],
                "reasons": trial["problems"],
            }
            for trial in trials
            if trial["problems"]
        ]
        comparable = (
            not failures
            and len(measured) == 2 * expected_repetitions
            and len(preflight) == 2
        )
        engines = {
            engine: _engine_summary(
                [trial for trial in measured if trial["condition"]["engine"] == engine]
            )
            for engine in ("totedb", "sqlite")
        }
        pairs: dict[int, dict[str, float]] = {}
        for trial in measured:
            if trial["problems"]:
                continue
            pairs.setdefault(trial["repetition"], {})[trial["condition"]["engine"]] = (
                trial["metrics"]["ycsb"]["OVERALL"]["Throughput(ops/sec)"]
            )
        ratios = [
            pair["totedb"] / pair["sqlite"]
            for _, pair in sorted(pairs.items())
            if len(pair) == 2 and pair["sqlite"] > 0
        ]
        ratio = bootstrap_ratios(ratios) if comparable else bootstrap_ratios([])
        h2 = "not_applicable"
        if key[1] == "E":
            h2 = "inconclusive"
            if comparable and ratio["ci95"][1] < 0.8:
                h2 = "supported"
            elif comparable and ratio["ci95"][0] >= 0.8:
                h2 = "rejected"
        summaries.append(
            {
                "records": key[0],
                "workload": key[1],
                "threads": key[2],
                "comparable": comparable,
                "engines": engines,
                "paired_throughput_ratio": ratio,
                "failures": failures,
                "h2": h2,
            }
        )
    return {
        "schema_version": 1,
        "repetitions": expected_repetitions,
        "conditions": summaries,
        "measured_runs": sum(record["phase"] == "measured" for record in records),
        "preflight_runs": sum(record["phase"] == "preflight" for record in records),
        "failed_runs": sum(bool(record["problems"]) for record in records),
        "total_logical_operations": sum(
            record["metrics"]["logical_operations"]
            for record in records
            if record["phase"] == "measured"
        ),
        "total_non_ok": sum(
            record["metrics"]["non_ok"]
            for record in records
            if record["phase"] == "measured"
        ),
        "total_native_attempts": sum(
            record["metrics"]["binding"].get("attempts", 0)
            for record in records
            if record["phase"] == "measured"
        ),
        "total_busy_retries": sum(
            record["metrics"]["binding"].get("retries", 0)
            for record in records
            if record["phase"] == "measured"
        ),
        "bootstrap_note": "paired median ratio; 10000 resamples; seed 226; n=5 intervals are imprecise; no multiplicity correction",
    }


def _engine_summary(selected: list[dict[str, Any]]) -> dict[str, Any]:
    successful = [trial for trial in selected if not trial["problems"]]
    operation_summary = {}
    for operation in (*OPERATIONS, "READ-MODIFY-WRITE"):
        available = [
            trial["metrics"]["ycsb"][operation]
            for trial in successful
            if operation in trial["metrics"]["ycsb"]
        ]
        if available:
            operation_summary[operation] = {
                name: describe([values[name] for values in available if name in values])
                for name in (
                    "Operations",
                    "50thPercentileLatency(us)",
                    "95thPercentileLatency(us)",
                    "99thPercentileLatency(us)",
                )
            }
    return {
        "total": len(selected),
        "successful": len(successful),
        "throughput": describe(
            [
                trial["metrics"]["ycsb"]["OVERALL"]["Throughput(ops/sec)"]
                for trial in successful
            ]
        ),
        "operations": operation_summary,
        "logical_operations": sum(
            trial["metrics"]["logical_operations"] for trial in selected
        ),
        "native_attempts": sum(
            trial["metrics"]["binding"].get("attempts", 0) for trial in selected
        ),
        "audit": {
            name: describe([trial["audit_after"][name] for trial in successful])
            for name in (
                "rows",
                "payload_bytes",
                "changed_fields",
                "added_rows",
                "missing_rows",
            )
        },
        "runtime_seconds": describe(
            [trial["process"]["elapsed_seconds"] for trial in selected]
        ),
        "returned_rows": sum(
            trial["metrics"]["binding"].get("result_rows", 0) for trial in selected
        ),
        "returned_bytes": sum(
            trial["metrics"]["binding"].get("output_bytes", 0) for trial in selected
        ),
        "non_ok": sum(trial["metrics"]["non_ok"] for trial in selected),
        "timeouts": sum(trial["process"]["timeout"] for trial in selected),
        "retries": sum(
            trial["metrics"]["binding"].get("retries", 0) for trial in selected
        ),
        "exhausted": sum(
            trial["metrics"]["binding"].get("exhausted", 0) for trial in selected
        ),
    }


def read_records(raw: Path) -> tuple[list[dict[str, Any]], int]:
    manifests = sorted(raw.glob("*/manifest.json"))
    if len(manifests) != 1:
        raise ValueError("raw input must contain exactly one collection manifest")
    manifest = json.loads(manifests[0].read_text())
    repetitions = manifest["repetitions"]
    expected = {
        (
            size,
            workload,
            threads,
            repetition,
            "preflight" if repetition == 0 else "measured",
            engine,
        )
        for size, workload, threads, repetition, engine in itertools.product(
            manifest["sizes"],
            manifest["workloads"],
            manifest["threads"],
            range(repetitions + 1),
            ("totedb", "sqlite"),
        )
    }
    records = []
    actual = set()
    for path in sorted(raw.glob("*/trials/*/record.json")):
        record = json.loads(path.read_text())
        condition = record["condition"]
        key = (
            condition["records"],
            condition["workload"],
            condition["threads"],
            record["repetition"],
            record["phase"],
            condition["engine"],
        )
        if key in actual:
            raise ValueError("duplicate trial condition: " + str(key))
        actual.add(key)
        if (
            record["run_id"] != manifest["run_id"]
            or condition["operations"] != manifest["operations"]
        ):
            raise ValueError("trial does not match collection manifest")
        parent = path.parent
        record["metrics"] = parse_metrics(
            (parent / "stdout.txt").read_text(), (parent / "stderr.txt").read_text()
        )
        record["process"] = json.loads((parent / "process.json").read_text())
        for name in ("before", "after"):
            audit_path = parent / ("audit-" + name + ".json")
            record["audit_" + name] = (
                json.loads(audit_path.read_text()) if audit_path.exists() else None
            )
        record["problems"] = validate_trial(record)
        records.append(record)
    if actual != expected:
        raise ValueError(
            f"trial matrix mismatch: missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}"
        )
    return records, repetitions


def analyze(experiment: Path) -> dict[str, Any]:
    records, repetitions = read_records(experiment / "data" / "raw")
    summary = summarize(records, repetitions)
    manifest_path = next((experiment / "data" / "raw").glob("*/manifest.json"))
    manifest = json.loads(manifest_path.read_text())
    summary["collection"] = {
        name: manifest[name]
        for name in (
            "run_id",
            "sizes",
            "workloads",
            "threads",
            "operations",
            "timeout_seconds",
            "expected_measured_runs",
            "expected_preflight_runs",
            "fresh_jvm",
            "jit_warmup",
            "filesystem_cache",
            "request_trace",
        )
    }
    summary["native_versions"] = manifest["environment"]["versions"]
    summary["git_sha"] = manifest["environment"]["git_sha"]
    destination = experiment / "results"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    _write_csv(summary, destination)
    return summary


def _write_csv(summary: dict[str, Any], destination: Path) -> None:
    with (destination / "summary.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "records",
                "workload",
                "threads",
                "engine",
                "comparable",
                "successful",
                "total",
                "throughput_median_ops_sec",
                "throughput_min_ops_sec",
                "throughput_max_ops_sec",
                "ratio_median",
                "ratio_ci95_low",
                "ratio_ci95_high",
                "timeouts",
                "non_ok",
                "busy_retries",
            ]
        )
        for condition in summary["conditions"]:
            ratio = condition["paired_throughput_ratio"]
            for engine, result in condition["engines"].items():
                writer.writerow(
                    [
                        condition["records"],
                        condition["workload"],
                        condition["threads"],
                        engine,
                        condition["comparable"],
                        result["successful"],
                        result["total"],
                        result["throughput"]["median"],
                        result["throughput"]["min"],
                        result["throughput"]["max"],
                        ratio["median"],
                        *(ratio["ci95"] or [None, None]),
                        result["timeouts"],
                        result["non_ok"],
                        result["retries"],
                    ]
                )
    with (destination / "operations.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "records",
                "workload",
                "threads",
                "engine",
                "operation",
                "metric",
                "n",
                "median",
                "min",
                "max",
            ]
        )
        for condition in summary["conditions"]:
            for engine, result in condition["engines"].items():
                for operation, values in result["operations"].items():
                    for metric, stats in values.items():
                        writer.writerow(
                            [
                                condition["records"],
                                condition["workload"],
                                condition["threads"],
                                engine,
                                operation,
                                metric,
                                stats["n"],
                                stats["median"],
                                stats["min"],
                                stats["max"],
                            ]
                        )


def write_checksums(experiment: Path) -> None:
    base = experiment / "data"
    lines = [
        hashlib.sha256(path.read_bytes()).hexdigest()
        + "  "
        + path.relative_to(base).as_posix()
        for path in sorted((base / "raw").rglob("*"))
        if path.is_file()
    ]
    (base / "SHA256SUMS").write_text("\n".join(lines) + "\n")


def verify(experiment: Path) -> dict[str, Any]:
    base = experiment / "data"
    listed = set()
    for line in (base / "SHA256SUMS").read_text().splitlines():
        expected, relative = line.split("  ", 1)
        if relative in listed:
            raise ValueError("duplicate checksum entry: " + relative)
        listed.add(relative)
        path = (base / relative).resolve()
        if not path.is_relative_to((base / "raw").resolve()):
            raise ValueError("checksum path outside raw directory")
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError("checksum mismatch: " + relative)
    actual = {
        path.relative_to(base).as_posix()
        for path in (base / "raw").rglob("*")
        if path.is_file()
    }
    if listed != actual:
        raise ValueError("checksum file set differs from raw files")
    results = [
        experiment / "results" / name
        for name in ("summary.json", "summary.csv", "operations.csv")
    ]
    previous = {path: path.read_bytes() for path in results if path.exists()}
    summary = analyze(experiment)
    for path, content in previous.items():
        if path.read_bytes() != content:
            raise ValueError("summary did not reproduce from raw inputs: " + path.name)
    return summary
