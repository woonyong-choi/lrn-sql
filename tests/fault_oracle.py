"""#133: 승인된 커밋의 유실·허용 밖 커밋·남거나 사라진 자원을 판정한다."""

from __future__ import annotations

import argparse
import json
import logging
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)
MAX_INPUT_BYTES = 16 * 1024 * 1024  # 합성 검사 입력의 방어 한도다.


class OracleInputError(ValueError):
    """비교할 수 없는 입력은 정상 비교의 불일치와 구분한다."""


def _object(value: Any, keys: set[str], context: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        raise OracleInputError(f"invalid {context} fields")
    return value


def _ids(value: Any, context: str) -> set[str]:
    if not isinstance(value, list):
        raise OracleInputError(f"invalid {context}: expected an ID array")
    if any(not isinstance(item, str) or not item for item in value):
        raise OracleInputError(f"invalid {context}: IDs must be nonempty strings")
    result = set(value)
    if len(result) != len(value):
        raise OracleInputError(f"invalid {context}: duplicate IDs")
    return result


def _resources(value: Any) -> dict[str, set[str]]:
    if not isinstance(value, dict):
        raise OracleInputError("invalid resources: expected an object")
    result = {}
    for name, ids in value.items():
        if not isinstance(name, str) or not name:
            raise OracleInputError("invalid resource kind")
        result[name] = _ids(ids, f"resource {name}")
    return result


def _resource_diff(
    expected: dict[str, set[str]], actual: dict[str, set[str]]
) -> dict[str, Any]:
    differences = {}
    for kind in sorted(expected.keys() | actual.keys()):
        missing = sorted(expected.get(kind, set()) - actual.get(kind, set()))
        unexpected = sorted(actual.get(kind, set()) - expected.get(kind, set()))
        if missing or unexpected or (kind in expected) != (kind in actual):
            differences[kind] = {
                "missing": missing,
                "unexpected": unexpected,
                "kind_missing": kind not in actual,
                "kind_unexpected": kind not in expected,
            }
    return differences


# cost: time O(n log n), heap O(n), stack O(1)
# vars: n = 모든 커밋·자원 ID의 개수와 문자열 길이 합
# basis: estimate
def compare(expected: Any, recovered: Any) -> dict[str, Any]:
    """acknowledged ⊆ recovered ⊆ possible와 정확한 자원 inventory를 검사한다."""
    contract = _object(expected, {"acknowledged", "possible", "resources"}, "expected")
    observed = _object(recovered, {"committed", "resources"}, "recovered")
    acknowledged = _ids(contract["acknowledged"], "acknowledged")
    possible = _ids(contract["possible"], "possible")
    committed = _ids(observed["committed"], "committed")
    if not acknowledged <= possible:
        raise OracleInputError("acknowledged commits must be possible")
    missing = sorted(acknowledged - committed)
    unexpected = sorted(committed - possible)
    resources = _resource_diff(
        _resources(contract["resources"]), _resources(observed["resources"])
    )
    return {
        "ok": not missing and not unexpected and not resources,
        "missing_acknowledged": missing,
        "unexpected_committed": unexpected,
        "resources": resources,
    }


# cost: time O(b), heap O(b), stack O(1), io 1
# vars: b = 입력 JSON 바이트, 방어 한도 이하
# basis: estimate
def _read(path: Path) -> Any:
    with path.open("rb") as source:
        content = source.read(MAX_INPUT_BYTES + 1)
    if len(content) > MAX_INPUT_BYTES:
        raise OracleInputError(f"input exceeds byte limit: {path}")
    return json.loads(content)


# cost: time O(b + n log n), heap O(b + n), stack O(1), io O(1)
# vars: b = 두 입력의 바이트, n = 커밋·자원 ID의 개수와 문자열 길이 합
# basis: estimate
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expected", type=Path)
    parser.add_argument("recovered", type=Path)
    args = parser.parse_args()
    try:
        report = compare(_read(args.expected), _read(args.recovered))
    except (OSError, ValueError) as error:
        logger.error("failed to compare recovery inventories: %s", error)
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    raise SystemExit(main())
