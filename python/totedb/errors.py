"""C 인터페이스의 상태 코드를 호출하는 쪽이 경우별로 처리할 수 있는 예외로 옮긴다."""

from __future__ import annotations

__all__ = [
    "BusyError",
    "CancelledError",
    "ConflictError",
    "DatabaseOpenError",
    "InternalError",
    "InvalidArgumentError",
    "LimitError",
    "ToteDBError",
    "NotAppliedError",
    "NotFoundError",
    "NotInstalledError",
    "PoisonedError",
    "SqlError",
    "StaleError",
    "StorageError",
    "TransactionFailedError",
    "UncertainError",
    "UnsupportedError",
    "raise_for_status",
]

STATUS_OK = 0
STATUS_BUSY = 4
STATUS_EMPTY = 50


class ToteDBError(Exception):
    """이 패키지가 던지는 모든 예외의 기준 예외."""

    def __init__(self, message: str, *, status: int = -1) -> None:
        super().__init__(message)
        self.status = status


class InvalidArgumentError(ToteDBError):
    """인자 오류이거나 다른 database의 트랜잭션을 줬다."""


class LimitError(ToteDBError):
    """payload·결과·메모리 예산 한도를 넘었다. 값을 자르지 않고 거절했다."""


class BusyError(ToteDBError):
    """다른 writer가 있어 기다린 시간이 끝났다. 다시 시도할 수 있다."""


class NotInstalledError(ToteDBError):
    """큐 테이블이 없다. 열기는 만들지 않는다."""


class NotFoundError(ToteDBError):
    """id가 없다."""


class StaleError(ToteDBError):
    """fence가 현재 claim이 아니거나 상태가 맞지 않는다. 아무것도 바꾸지 않았다."""


class ConflictError(ToteDBError):
    """같은 idempotency key에 payload가 다르다."""


class UnsupportedError(ToteDBError):
    """트랜잭션 계층 없이 연 DB에 쓰기를 요청했다."""


class StorageError(ToteDBError):
    """저장소 I/O 오류나 메모리 부족. commit 전이면 상태는 그대로다."""


class NotAppliedError(ToteDBError):
    """commit이 확정적으로 적용되지 않았다. 상태는 그대로다."""


class UncertainError(ToteDBError):
    """commit 결과를 알 수 없다. 성공으로 취급하면 안 되며 다시 열어 확인한다."""


class PoisonedError(ToteDBError):
    """database가 불확실한 commit 뒤 더는 쓸 수 없다. 닫고 다시 연다."""


class TransactionFailedError(ToteDBError):
    """호출자의 트랜잭션이 이미 실패 상태다. rollback 뒤 다시 한다."""


class InternalError(ToteDBError):
    """라이브러리가 기대하지 못한 응답."""


class CancelledError(ToteDBError):
    """Database.cancel로 큐 연산이 취소됐다. 자기 트랜잭션이면 반영하지 않았다."""


class DatabaseOpenError(ToteDBError):
    """database 열기 실패(없음·잠김·손상·형식 미지원)."""


class ForkedHandleError(ToteDBError):
    """fork한 자식 프로세스가 부모가 연 핸들을 썼다. 아무것도 하지 않았다. 자식에서 새로 연다."""


class SqlError(ToteDBError):
    """문장이 오류 결과로 끝났다. 분류와 코드는 속성에 있다."""

    def __init__(self, message: str, *, error_class: int, error_code: int) -> None:
        super().__init__(message)
        self.error_class = error_class
        self.error_code = error_code


_BY_STATUS: dict[int, type[ToteDBError]] = {
    1: InvalidArgumentError,
    2: StorageError,
    3: LimitError,
    4: BusyError,
    5: NotInstalledError,
    6: NotFoundError,
    7: StaleError,
    8: ConflictError,
    9: UnsupportedError,
    10: StorageError,
    11: NotAppliedError,
    12: UncertainError,
    13: PoisonedError,
    14: TransactionFailedError,
    15: InternalError,
    16: CancelledError,
    101: DatabaseOpenError,
    102: DatabaseOpenError,
    103: DatabaseOpenError,
    104: DatabaseOpenError,
    105: DatabaseOpenError,
    106: DatabaseOpenError,
    107: DatabaseOpenError,
    108: ForkedHandleError,
}


def raise_for_status(status: int, name: str) -> None:
    """status가 성공(0)이 아니면 대응하는 예외를 던진다.

    Args:
        status: C 인터페이스가 돌려준 상태 값.
        name: tote_status_name이 돌려준 이름. 메시지에 쓴다.

    Raises:
        ToteDBError: status가 0이 아닐 때, 상태에 맞는 하위 예외.
    """
    if status == STATUS_OK:
        return
    error_type = _BY_STATUS.get(status, InternalError)
    raise error_type(f"{name} (status {status})", status=status)
