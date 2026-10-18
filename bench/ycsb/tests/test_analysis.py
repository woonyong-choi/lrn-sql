from __future__ import annotations

import copy
import json
import shutil
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analysis import (
    bootstrap_ratios,
    parse_metrics,
    read_records,
    summarize,
    validate_trial,
    verify,
)
from run import main
from runner import CollectionOptions, collect, process

from build import BUILD


class AnalysisTests(unittest.TestCase):
    def setUp(self) -> None:
        self.record = {
            "process": {"timeout": False, "exit_code": 0, "elapsed_seconds": 0.2},
            "condition": {
                "records": 10,
                "operations": 20,
                "threads": 1,
                "workload": "C",
                "engine": "totedb",
            },
            "metrics": {
                "non_ok": 0,
                "binding_errors": 0,
                "logical_operations": 20,
                "workers": 1,
                "sql_calls": 20,
                "binding": {
                    "attempts": 20,
                    "retries": 0,
                    "exhausted": 0,
                    "result_rows": 20,
                    "output_bytes": 20000,
                },
                "ycsb": {
                    "READ": {
                        "Operations": 20,
                        "Return=OK": 20,
                        "50thPercentileLatency(us)": 1,
                        "95thPercentileLatency(us)": 2,
                        "99thPercentileLatency(us)": 3,
                    },
                    "OVERALL": {"Throughput(ops/sec)": 100},
                },
            },
            "audit_before": {"rows": 10, "sha256": "same"},
            "audit_after": {
                "rows": 10,
                "sha256": "same",
                "missing_rows": 0,
                "added_rows": 0,
                "payload_bytes": 10000,
                "changed_fields": 0,
            },
            "phase": "measured",
            "repetition": 1,
            "input_identifier": "same",
            "trial_id": "one",
        }

    def test_successful_read_only_run_has_no_problems(self) -> None:
        self.assertEqual(validate_trial(self.record), [])

    def test_exit_zero_does_not_hide_operation_error_or_timeout(self) -> None:
        self.record["metrics"]["non_ok"] = 1
        self.record["process"]["timeout"] = True
        self.assertIn("operation_error", validate_trial(self.record))
        self.assertIn("timeout", validate_trial(self.record))

    def test_read_only_mutation_and_missing_fields_are_rejected(self) -> None:
        self.record["audit_after"]["sha256"] = "different"
        self.record["audit_after"]["payload_bytes"] = 9000
        problems = validate_trial(self.record)
        self.assertIn("read_only_digest_changed", problems)
        self.assertIn("stored_payload_bytes", problems)

    def test_successful_update_must_change_a_persisted_field(self) -> None:
        self.record["condition"]["workload"] = "A"
        self.record["metrics"]["ycsb"]["UPDATE"] = {
            "Operations": 1,
            "Return=OK": 1,
            "50thPercentileLatency(us)": 1,
            "95thPercentileLatency(us)": 2,
            "99thPercentileLatency(us)": 3,
        }
        self.assertIn("updates_did_not_change_fields", validate_trial(self.record))

    def test_missing_percentile_and_wrong_native_count_are_rejected(self) -> None:
        del self.record["metrics"]["ycsb"]["READ"]["50thPercentileLatency(us)"]
        self.record["metrics"]["binding"]["attempts"] = 21
        self.assertIn("missing_percentile_read", validate_trial(self.record))
        self.assertIn("native_call_count", validate_trial(self.record))

    def test_official_f_logical_operation_count_avoids_double_count(self) -> None:
        parsed = parse_metrics(
            "[READ], Operations, 10\n[UPDATE], Operations, 4\n[READ-MODIFY-WRITE], Operations, 4\n[UPDATE], Return=ERROR, 1\n",
            "BINDING attempts=15 retries=1 exhausted=0\n",
        )
        self.assertEqual(parsed["logical_operations"], 10)
        self.assertEqual(parsed["sql_calls"], 14)
        self.assertEqual(parsed["non_ok"], 1)

    def test_successful_official_scans_require_at_least_one_row_each(self) -> None:
        self.record["condition"]["workload"] = "E"
        self.record["metrics"]["ycsb"]["SCAN"] = self.record["metrics"]["ycsb"].pop(
            "READ"
        )
        for returned in (0, 19, 20, 2000, 2001):
            with self.subTest(returned=returned):
                self.record["metrics"]["binding"]["result_rows"] = returned
                self.record["metrics"]["binding"]["output_bytes"] = returned * 1000
                problems = validate_trial(self.record)
                if 20 <= returned <= 2000:
                    self.assertEqual(problems, [])
                else:
                    self.assertEqual(problems, ["returned_row_count"])

    def test_collection_existing_output_rejects_before_side_effects(self) -> None:
        directory = BUILD / "unit-tests" / "recollection"
        directory.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, directory)
        for profile in (False, True):
            options = CollectionOptions([10], ["C"], [1], 1, 20, 1, profile)
            experiment = directory / ("profile" if profile else "collection")
            old_raw = experiment / "data" / "raw" / "existing"
            old_raw.mkdir(parents=True)
            manifest = old_raw / "manifest.json"
            manifest.write_bytes(b'{"run_id": "existing"}\n')
            environment = experiment / "env.json"
            environment.write_bytes(b'{"preserve": true}\n')
            original = {
                path: path.read_bytes()
                for path in experiment.rglob("*")
                if path.is_file()
            }
            for run_id in ("existing", "recollection-guard-new"):
                with (
                    self.subTest(profile=profile, run_id=run_id),
                    patch("runner.environment") as inspect_environment,
                    patch("runner.subprocess.Popen") as child,
                    patch("runner.BUILD", directory),
                ):
                    with self.assertRaises(ValueError) as failure:
                        collect(experiment, run_id, options)
                    if run_id != "existing":
                        self.assertIn("--output", str(failure.exception))
                    inspect_environment.assert_not_called()
                    child.assert_not_called()
                    self.assertFalse((directory / "runs" / run_id).exists())
                    if run_id != "existing":
                        self.assertFalse(
                            (experiment / "data" / "raw" / run_id).exists()
                        )
                    self.assertEqual(
                        {
                            path: path.read_bytes()
                            for path in experiment.rglob("*")
                            if path.is_file()
                        },
                        original,
                    )

    def test_test_command_respects_both_output_forms_and_default(self) -> None:
        explicit = BUILD / "unit-tests" / "explicit-checks"
        for arguments, expected in (
            (["--output", str(explicit)], explicit),
            (["--output=" + str(explicit)], explicit),
            ([], BUILD / "checks"),
        ):
            with (
                self.subTest(arguments=arguments),
                patch("sys.argv", ["run.py", "test", *arguments]),
                patch("testing.test") as adapter_test,
            ):
                self.assertEqual(main(), 0)
                adapter_test.assert_called_once_with(expected)

    def test_bootstrap_is_paired_and_deterministic(self) -> None:
        result = bootstrap_ratios([0.5] * 5)
        self.assertEqual(result["median"], 0.5)
        self.assertEqual(result["ci95"], [0.5, 0.5])
        self.assertEqual(
            bootstrap_ratios([0.1, 0.5, 1, 2, 4]), bootstrap_ratios([0.1, 0.5, 1, 2, 4])
        )

    def test_failure_makes_condition_incomparable(self) -> None:
        trials = []
        for phase in ("preflight", "measured"):
            for engine in ("totedb", "sqlite"):
                record = copy.deepcopy(self.record)
                record["phase"] = phase
                record["condition"]["engine"] = engine
                record["problems"] = (
                    ["timeout"] if engine == "totedb" and phase == "measured" else []
                )
                trials.append(record)
        summary = summarize(trials, 1)
        self.assertFalse(summary["conditions"][0]["comparable"])
        self.assertIsNone(summary["conditions"][0]["paired_throughput_ratio"]["median"])
        self.assertEqual(summary["failed_runs"], 1)

    def test_missing_entire_condition_is_rejected(self) -> None:
        directory = BUILD / "unit-tests" / "missing-condition"
        directory.mkdir(parents=True, exist_ok=True)
        run = directory / "run"
        run.mkdir(exist_ok=True)
        (run / "manifest.json").write_text(
            json.dumps(
                {"repetitions": 1, "sizes": [10], "workloads": ["C"], "threads": [1]}
            )
        )
        with self.assertRaisesRegex(ValueError, "trial matrix mismatch"):
            read_records(directory)

    def test_checksum_does_not_ignore_unlisted_raw_file(self) -> None:
        directory = BUILD / "unit-tests" / "checksum"
        raw = directory / "data" / "raw"
        raw.mkdir(parents=True, exist_ok=True)
        (directory / "data" / "SHA256SUMS").write_text("")
        (raw / "unlisted.txt").write_text("not in manifest")
        with self.assertRaisesRegex(ValueError, "checksum file set"):
            verify(directory)

    def test_process_timeout_is_preserved_with_partial_output(self) -> None:
        directory = BUILD / "unit-tests" / "timeout"
        directory.mkdir(parents=True, exist_ok=True)
        result = process(
            [
                sys.executable,
                "-c",
                'import time; print("started", flush=True); time.sleep(20)',
            ],
            directory,
            0.2,
        )
        self.assertTrue(result["timeout"])
        self.assertLess(result["exit_code"], 0)
        self.assertEqual((directory / "stdout.txt").read_text(), "started\n")


if __name__ == "__main__":
    unittest.main()
