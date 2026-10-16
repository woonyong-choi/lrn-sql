"""#103 차등 검사의 엔진 래퍼. 어댑터 프로세스 하나와 그 데이터베이스 파일, 열린 연결을 관리한다."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path
from typing import Any

from sqlite_session_protocol import (
    EngineError,
    EngineProcess,
    Outcome,
    ProtocolViolationError,
    build_param,
    encode_cell,
    hex_encode,
    parse_outcome,
)


class FaultReplyError(EngineError):
    """어댑터가 요청을 수행하지 못했다고 FAULT로 답했다."""


def case_database_name(label: str) -> str:
    return "sqlite.db" if label == "sqlite" else "subject.totedb"


def _expect_ok(frame: list[str], what: str) -> None:
    if frame and frame[0].startswith("FAULT"):
        raise FaultReplyError(f"{what}: {frame[0]}")
    if frame != ["OK"]:
        raise ProtocolViolationError(f"{what}: unexpected reply {frame[:2]!r}")


class EngineSession:
    def __init__(
        self,
        label: str,
        argv: Sequence[str],
        db_path: Path,
        *,
        timeout_s: float,
    ) -> None:
        self.label = label
        self.version = ""
        self._argv = list(argv)
        self._db_path = db_path
        self._timeout_s = timeout_s
        self._process: EngineProcess | None = None

    def start(self) -> None:
        self._process = EngineProcess(self._argv, self._timeout_s)
        frame = self._process.request("HELLO")
        parts = frame[0].split(" ") if frame else []
        if len(parts) < 3 or parts[0] != "HELLO":
            raise ProtocolViolationError(f"{self.label}: unexpected HELLO reply {frame[:1]!r}")
        self.version = parts[2]

    def open_fresh(self, connections: Sequence[str]) -> None:
        self._open(connections, fresh=True)

    def restart(self, connections: Sequence[str], *, crash: bool) -> None:
        """crash면 프로세스를 SIGKILL하고 새로 띄워 복구 경로로 열고, 아니면 정상 종료 뒤 연다."""
        if crash:
            self._require_process().kill()
            self.start()
        else:
            _expect_ok(self._require_process().request("CLOSE"), f"{self.label} close")
        self._open(connections, fresh=False)

    def drop_session(self, conn: str) -> None:
        _expect_ok(self._require_process().request(f"DROP {conn}"), f"{self.label} drop {conn}")

    def execute(self, conn: str, sql: str, params: Sequence[Any]) -> Outcome:
        tokens = [encode_cell(build_param(spec)) for spec in params]
        line = " ".join(["EXEC", conn, hex_encode(sql.encode("utf-8")), str(len(tokens))] + tokens)
        frame = self._require_process().request(line)
        if frame and frame[0].startswith("FAULT"):
            raise FaultReplyError(f"{self.label} exec: {frame[0]}")
        return parse_outcome(frame)

    def stop(self) -> str | None:
        """어댑터를 종료한다. 비정상 종료(sanitizer 보고 포함)나 종료 지연이면 설명을 돌려준다."""
        if self._process is None:
            return None
        process, self._process = self._process, None
        code = process.shutdown()
        if code == 0:
            return None
        detail = "did not exit in time" if code is None else f"exited with code {code}"
        return f"{self.label} {detail}: {process.stderr_tail[-300:]}"

    def _open(self, connections: Sequence[str], *, fresh: bool) -> None:
        process = self._require_process()
        path_hex = hex_encode(str(self._db_path).encode("utf-8"))
        request = "OPEN {} {}".format(path_hex, "new" if fresh else "existing")
        _expect_ok(process.request(request), f"{self.label} open")
        for name in connections:
            _expect_ok(process.request(f"SESSION {name}"), f"{self.label} session {name}")

    def _require_process(self) -> EngineProcess:
        if self._process is None:
            raise EngineError(f"{self.label} adapter is not running")
        return self._process
