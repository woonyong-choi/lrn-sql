"""liblrnackq 공유 라이브러리를 ctypes로 연결한다(선언은 include/queue/ack_queue_capi.h와 같다)."""

from __future__ import annotations

import ctypes
import os
import sys
from collections.abc import Iterator
from pathlib import Path

__all__ = ["CLOCK_FN", "LIST_NOTE_BYTES", "Item", "ListItem", "Param", "load_library"]

CLOCK_FN = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p)

LIST_NOTE_BYTES = 201


class Param(ctypes.Structure):
    """SQL 매개변수 하나(lrn_param_t)."""

    _fields_ = [
        ("type", ctypes.c_int32),
        ("integer", ctypes.c_int64),
        ("real", ctypes.c_double),
        ("bytes", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
    ]


class Item(ctypes.Structure):
    """claim한 작업(lrnq_item_t)."""

    _fields_ = [
        ("id", ctypes.c_uint64),
        ("fence", ctypes.c_uint64),
        ("attempts", ctypes.c_uint32),
        ("lease_until_ms", ctypes.c_uint64),
        ("payload", ctypes.c_void_p),
        ("payload_size", ctypes.c_size_t),
    ]


class ListItem(ctypes.Structure):
    """목록의 작업 한 건(lrnq_listitem_t)."""

    _fields_ = [
        ("id", ctypes.c_uint64),
        ("state", ctypes.c_int32),
        ("fence", ctypes.c_uint64),
        ("attempts", ctypes.c_uint32),
        ("ready_at_ms", ctypes.c_uint64),
        ("lease_until_ms", ctypes.c_uint64),
        ("created_at_ms", ctypes.c_uint64),
        ("note", ctypes.c_char * LIST_NOTE_BYTES),
        ("payload", ctypes.c_void_p),
        ("payload_size", ctypes.c_size_t),
    ]


def load_library() -> ctypes.CDLL:
    """공유 라이브러리를 찾아 열고 함수 시그니처를 선언한다.

    Raises:
        OSError: LRN_ACKQ_LIB과 기본 빌드 위치 어디에도 라이브러리가 없을 때.
    """
    tried: list[str] = []
    for path in _candidates():
        tried.append(str(path))
        if path.exists():
            return _declare(ctypes.CDLL(str(path)))
    raise OSError("liblrnackq not found (set LRN_ACKQ_LIB or run make); tried: " + ", ".join(tried))


def _candidates() -> Iterator[Path]:
    env = os.environ.get("LRN_ACKQ_LIB")
    if env:
        yield Path(env)
    suffix = "dylib" if sys.platform == "darwin" else "so"
    root = Path(__file__).resolve().parents[2]
    yield root / "build" / "queue-shared" / f"liblrnackq.{suffix}"


_INT_FUNCTIONS = (
    "lrndb_open",
    "lrndb_close",
    "lrndb_exec",
    "lrndb_begin",
    "lrndb_commit",
    "lrndb_rollback",
    "lrndb_txn_failed",
    "lrnq_open",
    "lrnq_put",
    "lrnq_get",
    "lrnq_ack",
    "lrnq_fail",
    "lrnq_nack",
    "lrnq_update",
    "lrnq_recover",
    "lrnq_release_leases",
    "lrnq_purge",
    "lrnq_stats",
    "lrnq_list",
)


def _declare(lib: ctypes.CDLL) -> ctypes.CDLL:
    c = ctypes
    ptr, u64, size = c.c_void_p, c.c_uint64, c.c_size_t
    pptr = c.POINTER(c.c_void_p)
    lib.lrn_status_name.restype = c.c_char_p
    lib.lrn_status_name.argtypes = [c.c_int]
    lib.lrn_free.argtypes = [ptr]
    lib.lrn_set_max_result_bytes.argtypes = [size]
    lib.lrn_max_result_bytes.restype = size
    lib.lrndb_open.argtypes = [c.c_char_p, c.c_int, pptr]
    lib.lrndb_close.argtypes = [pptr]
    lib.lrndb_cancel.argtypes = [ptr]
    lib.lrndb_cancel.restype = None
    lib.lrndb_exec.argtypes = [ptr, ptr, c.c_char_p, c.POINTER(Param), size, pptr, c.POINTER(size)]
    lib.lrndb_begin.argtypes = [ptr, pptr]
    lib.lrndb_commit.argtypes = [pptr]
    lib.lrndb_rollback.argtypes = [pptr]
    lib.lrndb_txn_failed.argtypes = [ptr]
    lib.lrnq_open.argtypes = [ptr, c.c_char_p, c.c_int, c.c_uint32, CLOCK_FN, ptr, pptr]
    lib.lrnq_close.argtypes = [pptr]
    lib.lrnq_put.argtypes = [ptr, ptr, c.c_char_p, size, c.c_char_p, c.POINTER(u64), c.POINTER(c.c_int)]
    lib.lrnq_get.argtypes = [ptr, ptr, u64, u64, c.POINTER(Item)]
    lib.lrnq_item_free.argtypes = [c.POINTER(Item)]
    lib.lrnq_ack.argtypes = [ptr, ptr, u64, u64]
    lib.lrnq_fail.argtypes = [ptr, ptr, u64, u64, c.c_char_p]
    lib.lrnq_nack.argtypes = [ptr, ptr, u64, u64, u64, c.c_char_p, c.POINTER(c.c_int)]
    lib.lrnq_update.argtypes = [ptr, ptr, u64, u64, c.c_char_p, size]
    lib.lrnq_recover.argtypes = [ptr, ptr, size, c.POINTER(size)]
    lib.lrnq_release_leases.argtypes = [ptr, ptr, size, c.POINTER(size)]
    lib.lrnq_purge.argtypes = [ptr, ptr, c.c_int, size, c.POINTER(size)]
    lib.lrnq_stats.argtypes = [ptr, ptr, c.POINTER(u64)]
    lib.lrnq_list.argtypes = [ptr, ptr, u64, size, c.c_int, c.POINTER(ListItem), c.POINTER(size)]
    lib.lrnq_list_free.argtypes = [c.POINTER(ListItem), size]
    for name in _INT_FUNCTIONS:
        getattr(lib, name).restype = c.c_int
    return lib
