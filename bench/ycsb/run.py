from __future__ import annotations

import argparse
import json
import logging
import re
from pathlib import Path

from analysis import analyze, verify
from runner import CollectionOptions, collect

from build import BUILD, ROOT, build

logger = logging.getLogger(__name__)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Official YCSB 0.17.0 ToteDB/SQLite shared JNI comparison"
    )
    parser.add_argument(
        "command", choices=["build", "test", "collect", "analyze", "verify", "profile"]
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--run-id", default="default")
    parser.add_argument("--sizes", default="1000,10000")
    parser.add_argument("--workloads", default=None)
    parser.add_argument("--threads", default="1,4")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--operations", type=int, default=1000)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.run_id):
        parser.error(
            "run-id must contain only letters, digits, dots, underscores, and hyphens"
        )
    sizes = [int(value) for value in args.sizes.split(",")]
    threads = [int(value) for value in args.threads.split(",")]
    workloads = (
        (args.workloads or ("C,E" if args.command == "profile" else "A,B,C,D,E,F"))
        .upper()
        .split(",")
    )
    if any(
        value <= 0
        for value in [*sizes, *threads, args.repetitions, args.operations, args.timeout]
    ):
        parser.error(
            "sizes, threads, repetitions, operations and timeout must be positive"
        )
    if any(value not in "ABCDEF" or len(value) != 1 for value in workloads):
        parser.error("workloads must be comma-separated A through F")
    output = (args.output or ROOT / "docs/experiments/ycsb-sqlite").resolve()
    if args.command == "build":
        build()
        print(BUILD / "build.json")
    elif args.command == "test":
        from testing import test

        test(output if args.output is not None else BUILD / "checks")
    elif args.command in ("collect", "profile"):
        if not (BUILD / "build.json").exists():
            raise RuntimeError("run build before collection")
        if args.command == "profile":
            output = output / "profile"
        trials = collect(
            output,
            args.run_id,
            CollectionOptions(
                sizes,
                workloads,
                threads,
                args.repetitions,
                args.operations,
                args.timeout,
                args.command == "profile",
            ),
        )
        analyze(output)
        return int(any(trial["problems"] for trial in trials))
    else:
        summary = analyze(output) if args.command == "analyze" else verify(output)
        print(
            json.dumps(
                {
                    "measured_runs": summary["measured_runs"],
                    "failed_runs": summary["failed_runs"],
                }
            )
        )
    return 0


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    try:
        raise SystemExit(main())
    except Exception as error:
        logger.error("%s", error)
        raise SystemExit(1) from error
