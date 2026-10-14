"""#133: 외부 제한시간과 종료 종류를 구분하고 재실행 입력·원출력을 보존한다."""

from __future__ import annotations

import argparse
import json
import logging
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
from typing import Any

logger = logging.getLogger(__name__)


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--site")
    parser.add_argument("--call", type=int, default=1)
    parser.add_argument(
        "--expect", choices=("success", "error", "signal", "timeout"), default="success"
    )
    parser.add_argument("--expect-code", type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("command and a finite positive timeout are required")
    if not 0 <= args.seed <= 2**64 - 1 or args.call < 1:
        parser.error("seed must fit uint64 and call must be positive")
    if args.expect in ("error", "signal"):
        if args.expect_code is None or args.expect_code <= 0:
            parser.error("expected error or signal requires a positive expect-code")
    elif args.expect_code is not None:
        parser.error("expect-code only applies to error or signal")
    return args


# cost: time O(1), heap O(1), stack O(1), io 1
# basis: estimate
def _kill_group(process: subprocess.Popen[bytes]) -> dict[str, Any] | None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return None  # 자식과 그 프로세스 그룹이 이미 끝난 경우다.
    except OSError as error:
        return {"operation": "kill_group", "code": error.errno, "message": str(error)}
    return None


# cost: time O(t), heap O(1), stack O(1), io O(1)
# vars: t = 정리 제한시간; 그룹 종료가 거절되면 직접 자식만 별도로 종료 시도
# basis: estimate
def _cleanup(
    process: subprocess.Popen[bytes], timeout_seconds: float
) -> list[dict[str, Any]]:
    errors = []
    group_error = _kill_group(process)
    if group_error is not None:
        errors.append(group_error)
        if process.poll() is None:
            try:
                process.kill()
            except OSError as error:
                errors.append(
                    {
                        "operation": "kill_child",
                        "code": error.errno,
                        "message": str(error),
                    }
                )
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        errors.append(
            {"operation": "wait", "code": None, "message": "child cleanup timed out"}
        )
    except OSError as error:
        errors.append({"operation": "wait", "code": error.errno, "message": str(error)})
    return errors


def _classify(returncode: int) -> tuple[str, int | None]:
    if returncode < 0:
        return "signal", -returncode
    if returncode > 0:
        return "error", returncode
    return "success", None


# cost: time O(t), heap O(e + c), stack O(1), io O(1)
# vars: t = 제한시간, e = 환경 변수 바이트, c = 명령 바이트; 자식 비용은 별도
# basis: estimate
def _execute(
    args: argparse.Namespace, stdout_path: Path, stderr_path: Path
) -> dict[str, Any]:
    environment = {
        **os.environ,
        "FAULT_SEED": str(args.seed),
        "FAULT_CALL": str(args.call),
    }
    if args.site is not None:
        environment["FAULT_SITE"] = args.site
    else:
        environment.pop("FAULT_SITE", None)
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.Popen(
                args.command,
                env=environment,
                stdout=stdout,
                stderr=stderr,
                start_new_session=True,
            )
        except OSError as error:
            return {
                "outcome": "launch_error",
                "code": error.errno,
                "message": str(error),
                "returncode": None,
            }
        try:
            try:
                returncode = process.wait(timeout=args.timeout)
                outcome, code = _classify(returncode)
            except subprocess.TimeoutExpired:
                returncode = None
                outcome, code = "timeout", None
            result = {"outcome": outcome, "code": code, "returncode": returncode}
        finally:
            cleanup_errors = _cleanup(process, args.timeout)
        if cleanup_errors:
            return {
                "outcome": "cleanup_error",
                "code": cleanup_errors[0]["code"],
                "returncode": process.returncode,
                "pid": process.pid,
                "execution": result,
                "cleanup_errors": cleanup_errors,
            }
        result["returncode"] = process.returncode
        return result


# cost: time O(t + n), heap O(n + e + c), stack O(1), io O(1)
# vars: t = 제한시간, n = 결과 JSON, e = 환경 변수, c = 명령 바이트; 자식 비용은 별도
# basis: estimate
def _run(args: argparse.Namespace) -> dict[str, Any]:
    report_path = args.report.resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    stdout_path = report_path.with_suffix(report_path.suffix + ".stdout")
    stderr_path = report_path.with_suffix(report_path.suffix + ".stderr")
    result = _execute(args, stdout_path, stderr_path)
    matched = result["outcome"] == args.expect
    if args.expect_code is not None:
        matched = matched and result["code"] == args.expect_code
    result.update(
        schema=1,
        command=args.command,
        cwd=str(Path.cwd()),
        seed=args.seed,
        site=args.site,
        call=args.call,
        timeout_seconds=args.timeout,
        expected=args.expect,
        expected_code=args.expect_code,
        matched=matched,
        stdout=str(stdout_path),
        stderr=str(stderr_path),
    )
    report_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return result


# cost: time O(t + n), heap O(n + e + c), stack O(1), io O(1)
# vars: t = 제한시간, n = 결과 JSON, e = 환경 변수, c = 명령 바이트; 자식 비용은 별도
# basis: estimate
def main() -> int:
    logging.basicConfig(level=logging.WARNING)
    args = _arguments()
    try:
        result = _run(args)
    except OSError as error:
        logger.error("failed to write test evidence: %s", error)
        return 1
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["matched"] else 1


if __name__ == "__main__":
    sys.exit(main())
