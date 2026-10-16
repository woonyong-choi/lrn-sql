"""totedb: ToteDB(Transactional Outbox Table Engine)의 Python 인터페이스. DB와 그 위에 선택 설치하는 작업 큐.

Database와 Transaction은 큐 없이 DB 단독으로 쓴다. Queue는 설치해야 테이블이 생긴다.
persistqueue_compat은 persist-queue 1.1.0 SQLiteAckQueue의 일부 호출을 감싸며 의미 차이는 그 모듈에 적었다.
"""

from totedb.queue import Queue, Claim, ClaimToken, ItemInfo, ItemState, PutResult, QueueStats
from totedb.database import Column, Database, Result, SqlValue, Transaction
from totedb.errors import (
    BusyError,
    CancelledError,
    ConflictError,
    DatabaseOpenError,
    InternalError,
    InvalidArgumentError,
    LimitError,
    ToteDBError,
    NotAppliedError,
    NotFoundError,
    NotInstalledError,
    PoisonedError,
    SqlError,
    StaleError,
    StorageError,
    TransactionFailedError,
    UncertainError,
    UnsupportedError,
)

__all__ = [
    "Queue",
    "BusyError",
    "CancelledError",
    "Claim",
    "ClaimToken",
    "Column",
    "ConflictError",
    "Database",
    "DatabaseOpenError",
    "InternalError",
    "InvalidArgumentError",
    "ItemInfo",
    "ItemState",
    "LimitError",
    "ToteDBError",
    "NotAppliedError",
    "NotFoundError",
    "NotInstalledError",
    "PoisonedError",
    "PutResult",
    "QueueStats",
    "Result",
    "SqlError",
    "SqlValue",
    "StaleError",
    "StorageError",
    "Transaction",
    "TransactionFailedError",
    "UncertainError",
    "UnsupportedError",
]
