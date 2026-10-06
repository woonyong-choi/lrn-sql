#!/usr/bin/env python3
"""v1 파일은 명시적 이름 등록 전 변경을 막고 등록 뒤 재열린다."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def run_minidb(
    binary: str, db_path: Path, sql: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, str(db_path)],
        input=f"{sql}\n.exit\n",
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--minidb", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lrn_sql_legacy_") as tmp:
        path = Path(tmp) / "legacy.db"
        created = run_minidb(
            args.minidb,
            path,
            "CREATE TABLE users (name VARCHAR(32), age INT)",
        )
        if created.returncode != 0 or "테이블 생성 완료" not in created.stdout:
            raise RuntimeError(
                f"fixture creation failed: {created.stderr} {created.stdout}"
            )
        inserted = run_minidb(
            args.minidb, path, "INSERT INTO users VALUES ('Alice', 25)"
        )
        if inserted.returncode != 0 or "id=1" not in inserted.stdout:
            raise RuntimeError(
                f"fixture insert failed: {inserted.stderr} {inserted.stdout}"
            )

        data = bytearray(path.read_bytes())
        struct.pack_into("<I", data, 8, 1)
        data[676:708] = bytes(32)
        path.write_bytes(data)

        blocked = run_minidb(args.minidb, path, "DROP TABLE other")
        if "--adopt-table" not in blocked.stdout:
            raise RuntimeError(f"v1 write was not blocked: {blocked.stdout}")
        if struct.unpack_from("<I", path.read_bytes(), 8)[0] != 1:
            raise RuntimeError("v1 file changed without explicit adoption")

        adopted = subprocess.run(
            [args.minidb, "--adopt-table", "users", str(path)],
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        if adopted.returncode != 0:
            raise RuntimeError(f"adoption failed: {adopted.stderr}")
        reopened = run_minidb(args.minidb, path, "SELECT * FROM users WHERE id = 1")
        if "Alice | 25" not in reopened.stdout:
            raise RuntimeError(f"row missing after adoption: {reopened.stdout}")
        rejected = run_minidb(args.minidb, path, "DROP TABLE other")
        if "존재하지 않습니다" not in rejected.stdout:
            raise RuntimeError(f"wrong table DROP succeeded: {rejected.stdout}")
        preserved = run_minidb(args.minidb, path, "SELECT * FROM users WHERE id = 1")
        if "Alice | 25" not in preserved.stdout:
            raise RuntimeError(f"row missing after rejected DROP: {preserved.stdout}")
    print("PASS: v1 이름 등록 전 변경 차단, 등록 뒤 재열기와 이름 검증")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
