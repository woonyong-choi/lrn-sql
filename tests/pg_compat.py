#!/usr/bin/env python3
"""현재 MiniDB SQL 부분집합을 격리된 PostgreSQL 18 스키마와 비교한다."""

import argparse
import csv
import io
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
from typing import Any


def run_postgres(
    dsn: str, sql: str, schema: str | None = None
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    if schema is not None:
        env["PGOPTIONS"] = f"-c search_path={schema}"
    else:
        env.pop("PGOPTIONS", None)
    return subprocess.run(
        [
            "psql",
            "--no-psqlrc",
            "--quiet",
            "--tuples-only",
            "--csv",
            "--set",
            "ON_ERROR_STOP=1",
            "--dbname",
            dsn,
            "--command",
            sql,
        ],
        text=True,
        capture_output=True,
        env=env,
        timeout=30,
        check=False,
    )


def mini_rows(output: str) -> list[tuple[str, ...]]:
    lines = output.splitlines()
    if len(lines) < 2 or not re.fullmatch(r"[-+]+", lines[1]):
        raise ValueError(f"MiniDB 결과 형식을 해석할 수 없음: {output!r}")
    return [tuple(cell.strip() for cell in line.split(" | ")) for line in lines[2:]]


def pg_rows(output: str) -> list[tuple[str, ...]]:
    return [tuple(row) for row in csv.reader(io.StringIO(output))]


def compare_case(
    case: dict[str, Any], probe: str, db_path: str, dsn: str, schema: str
) -> str | None:
    mini = subprocess.run(
        [probe, db_path, case["sql"]],
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    pg = run_postgres(dsn, case.get("pg_sql", case["sql"]), schema)
    if mini.returncode != 0:
        return f"MiniDB probe 실패: {mini.stderr.strip()}"
    mini_lines = mini.stdout.split("\n", 1)
    mini_status = mini_lines[0]
    pg_status = "OK" if pg.returncode == 0 else "ERROR"
    expected = case.get("expect", "ok").upper()
    if mini_status != expected or pg_status != expected:
        return (
            f"상태 불일치: 예상={expected}, MiniDB={mini_status} "
            f"({mini.stderr.strip()}), PostgreSQL={pg_status} ({pg.stderr.strip()})"
        )
    if expected == "OK" and case.get("rows"):
        try:
            left = mini_rows(mini_lines[1])
            right = pg_rows(pg.stdout)
        except ValueError as exc:
            return str(exc)
        if not case.get("ordered"):
            left.sort()
            right.sort()
        if left != right:
            return f"행 불일치: MiniDB={left!r}, PostgreSQL={right!r}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--require-postgres", action="store_true")
    args = parser.parse_args()
    dsn = os.environ.get("PGCOMPAT_DSN")
    missing = (
        "PGCOMPAT_DSN 미설정"
        if not dsn
        else "psql 명령 없음"
        if not shutil.which("psql")
        else None
    )
    if missing:
        print(f"{'FAIL' if args.require_postgres else 'SKIP'}: {missing}")
        return 2 if args.require_postgres else 0

    version = run_postgres(dsn, "SHOW server_version_num")
    if version.returncode != 0:
        print(
            f"{'FAIL' if args.require_postgres else 'SKIP'}: PostgreSQL 접속 실패: {version.stderr.strip()}"
        )
        return 2 if args.require_postgres else 0
    try:
        version_number = int(version.stdout.strip())
    except ValueError:
        print(f"FAIL: PostgreSQL 버전을 해석할 수 없음: {version.stdout!r}")
        return 2
    if not 180000 <= version_number < 190000:
        print(
            f"{'FAIL' if args.require_postgres else 'SKIP'}: PostgreSQL 18 필요 (현재 {version_number})"
        )
        return 2 if args.require_postgres else 0

    cases = json.loads(Path(__file__).with_name("pg_compat_cases.json").read_text())
    schema = f"lrn_sql_{secrets.token_hex(8)}"
    created = run_postgres(dsn, f"CREATE SCHEMA {schema}")
    if created.returncode != 0:
        print(f"FAIL: 격리 스키마 생성 실패: {created.stderr.strip()}")
        return 2
    failures = 0
    mapped = 0
    try:
        with tempfile.TemporaryDirectory(prefix="lrn_sql_pg_compat_") as tmp:
            db_path = str(Path(tmp) / "minidb.db")
            for case in cases:
                if case.get("pg_sql"):
                    if not case.get("difference"):
                        print(f"FAIL {case['name']}: SQL 변환 근거 없음")
                        failures += 1
                        continue
                    mapped += 1
                error = compare_case(case, args.probe, db_path, dsn, schema)
                if error:
                    failures += 1
                    print(f"FAIL {case['name']}: {error}")
                else:
                    print(
                        f"PASS {case['name']}{' (문법 차이 기록)' if case.get('pg_sql') else ''}"
                    )
    finally:
        dropped = run_postgres(dsn, f"DROP SCHEMA {schema} CASCADE")
        if dropped.returncode != 0:
            failures += 1
            print(f"FAIL: 격리 스키마 정리 실패: {schema}: {dropped.stderr.strip()}")
    print(f"결과: {len(cases) - failures}/{len(cases)} 사례, 문법 차이 {mapped}건")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
