"""여러 언어 예제(#201)를 실행해 결과를 확인한다. Python 생산자와 C worker들이 같은 큐를 쓰고, worker 하나를 강제 종료한다."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def test_python_producer_and_c_workers_share_one_queue(tmp_path: Path) -> None:
    consumer = os.environ["OUTBOX_CONSUMER"]
    done = subprocess.run(
        [sys.executable, str(ROOT / "examples/outbox/run.py"), "--consumer", consumer, "--dir", str(tmp_path)],
        capture_output=True, text=True, timeout=300, env={**os.environ, "PYTHONPATH": str(ROOT / "python")},
    )
    assert done.returncode == 0, done.stdout + done.stderr
    assert "SIGKILL로 강제 종료됨" in done.stdout
    assert "재전달되어 처리됨" in done.stdout
    assert done.stdout.rstrip().endswith("검증 통과")
