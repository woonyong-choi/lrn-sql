"""#133의 커밋 승인 경계와 자원 유실·누수 반례."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
from typing import Any

import pytest

from fault_oracle import OracleInputError, compare


def contract() -> dict[str, Any]:
    return {
        "acknowledged": ["tx1"],
        "possible": ["tx1", "tx2"],
        "resources": {"fd": [], "lock": [], "page": ["catalog", "row1"]},
    }


@pytest.mark.parametrize("committed", [["tx1"], ["tx2", "tx1"]])
def test_unacknowledged_possible_commit_is_optional(committed: list[str]) -> None:
    expected = contract()
    assert compare(
        expected, {"committed": committed, "resources": expected["resources"]}
    )["ok"]


@pytest.mark.parametrize(
    ("committed", "missing", "unexpected"),
    [([], ["tx1"], []), (["tx1", "tx3"], [], ["tx3"])],
)
def test_lost_ack_and_unintended_commit_fail(
    committed: list[str], missing: list[str], unexpected: list[str]
) -> None:
    expected = contract()
    report = compare(
        expected, {"committed": committed, "resources": expected["resources"]}
    )
    assert not report["ok"]
    assert report["missing_acknowledged"] == missing
    assert report["unexpected_committed"] == unexpected


def test_missing_page_and_leaked_lock_fail() -> None:
    recovered = {
        "committed": ["tx1"],
        "resources": {"fd": [], "lock": ["writer"], "page": []},
    }
    report = compare(contract(), recovered)
    assert not report["ok"]
    assert report["resources"]["lock"]["unexpected"] == ["writer"]
    assert report["resources"]["page"]["missing"] == ["catalog", "row1"]


def test_omitted_empty_resource_kind_is_not_proof_of_cleanup() -> None:
    expected = contract()
    resources = dict(expected["resources"])
    del resources["fd"]
    report = compare(expected, {"committed": ["tx1"], "resources": resources})
    assert not report["ok"]
    assert report["resources"]["fd"]["kind_missing"]


@pytest.mark.parametrize("ids", [["tx1", "tx1"], [1], [""], "tx1"])
def test_invalid_commit_inventory_rejected(ids: Any) -> None:
    with pytest.raises(OracleInputError):
        compare(contract(), {"committed": ids, "resources": {}})


def test_impossible_acknowledgement_rejected() -> None:
    expected = contract()
    expected["possible"] = []
    with pytest.raises(OracleInputError):
        compare(expected, {"committed": [], "resources": {}})


@pytest.mark.parametrize("outcome", ["match", "mismatch", "invalid"])
def test_cli_distinguishes_comparison_from_bad_input(
    tmp_path: Path, outcome: str
) -> None:
    expected = contract()
    observed = {"committed": ["tx1"], "resources": expected["resources"]}
    if outcome == "mismatch":
        observed["committed"] = []
    expected_path, observed_path = (
        tmp_path / "expected.json",
        tmp_path / "observed.json",
    )
    expected_path.write_text(json.dumps(expected))
    observed_path.write_text("{" if outcome == "invalid" else json.dumps(observed))

    run = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).with_name("fault_oracle.py")),
            str(expected_path),
            str(observed_path),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == {"match": 0, "mismatch": 1, "invalid": 2}[outcome]
    if outcome == "invalid":
        assert "failed to compare" in run.stderr
    else:
        assert json.loads(run.stdout)["ok"] == (outcome == "match")
