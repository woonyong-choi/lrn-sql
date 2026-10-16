"""설치한 totedb 패키지가 자기 안의 공유 라이브러리로 DB와 큐를 쓰는지 확인한다.

wheel·sdist 설치 직후 저장소 밖에서 실행한다. TOTEDB_LIB이 있으면 패키지 안 라이브러리를 확인할 수 없으므로 실패한다.
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

import totedb
from totedb import Database, Queue
from totedb._native import load_library


def main() -> None:
    if os.environ.get("TOTEDB_LIB"):
        sys.exit("TOTEDB_LIB is set; unset it to check the bundled library")
    package_dir = Path(totedb.__file__).resolve().parent
    bundled = [p for p in package_dir.iterdir() if p.name.startswith("libtotedb.")]
    if len(bundled) != 1:
        sys.exit(f"expected one bundled library in {package_dir}, found {[p.name for p in bundled]}")

    native_version = load_library().tote_version().decode()
    if native_version != totedb.__version__:
        sys.exit(f"version mismatch: library {native_version}, package {totedb.__version__}")

    with tempfile.TemporaryDirectory() as tmp, Database(str(Path(tmp) / "smoke.db")) as db:
        db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
        queue = Queue(db, "jobs")
        with db.begin() as txn:
            txn.execute("INSERT INTO users VALUES (?, ?)", [1, "Alice"])
            queue.put(b"welcome", txn=txn)
        rows = db.execute("SELECT name FROM users").rows
        claim = queue.get(lease_ms=1_000)
        if rows != (("Alice",),) or claim is None or claim.payload != b"welcome":
            sys.exit(f"unexpected result: rows={rows!r} claim={claim!r}")
        queue.ack(claim)

    print(f"totedb {totedb.__version__} ok: {bundled[0].name}")


if __name__ == "__main__":
    main()
