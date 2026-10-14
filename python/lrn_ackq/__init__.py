"""lrn_ackq: 독립 DB(lrn-sql)와 그 위에 선택 설치하는 ACK 큐의 Python 인터페이스.

Database와 Transaction은 큐 없이 DB 단독으로 쓴다. AckQueue는 설치해야 테이블이 생긴다.
persistqueue_compat은 persist-queue 1.1.0 SQLiteAckQueue의 일부 호출을 감싸며 의미 차이는 그 모듈에 적었다.
"""

from lrn_ackq.ack_queue import AckQueue, Claim, ClaimToken, ItemInfo, ItemState, PutResult, QueueStats
from lrn_ackq.database import Database, Result, SqlValue, Transaction
from lrn_ackq.errors import (
    BusyError,
    ConflictError,
    DatabaseOpenError,
    InternalError,
    InvalidArgumentError,
    LimitError,
    LrnAckqError,
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
    "AckQueue",
    "BusyError",
    "Claim",
    "ClaimToken",
    "ConflictError",
    "Database",
    "DatabaseOpenError",
    "InternalError",
    "InvalidArgumentError",
    "ItemInfo",
    "ItemState",
    "LimitError",
    "LrnAckqError",
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
