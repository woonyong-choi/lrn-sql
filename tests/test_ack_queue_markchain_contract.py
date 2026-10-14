"""Markchain의 persist-queue 1.1.0 SQLiteAckQueue 호출 계약을 합성 데이터로 검증한다(#140 완료 조건 5).

Markchain 저장소 코드는 읽기만 했고 이 시험은 실제 Markchain 데이터나 설정을 쓰지 않는다. 같은 호출 순서(put,
get(id, block, raw), update, nack, ack, queue, clear_acked_data(keep_latest=0), shrink_disk_usage, 재시작 뒤
auto_resume)를 호환 계층과 persist-queue 1.1.0에 각각 돌려 관측값(id, 상태 코드, 데이터, 반환 모양)이 같은지
비교한다. persist-queue를 import할 수 없으면 비교 시험은 건너뛰고(skip) 호환 계층 단독 시험만 실행한다. 의도한
의미 차이는 마지막 시험들에서 양쪽을 따로 확인한다.
"""

# ruff: noqa: ANN401 - persist-queue 객체는 외부 타입이라 Any로 받는다

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest
from lrn_ackq import StaleError
from lrn_ackq.persistqueue_compat import Empty, SQLiteAckQueue, UnknownClaimError

OpenQueue = Callable[[Path], Any]


def request(index: int, **extra: object) -> dict[str, Any]:
    """Markchain 요청 모양의 합성 작업."""
    return {"key": f"k-{index}", "deadline": 4_000_000_000 + index, "cancelled": False, "n": index, **extra}


def open_ours(path: Path) -> SQLiteAckQueue:
    from persistqueue_json import json_serializer  # noqa: PLC0415 - 아래 fixture가 모듈을 제공한다

    return SQLiteAckQueue(path, multithreading=True, auto_resume=True, serializer=json_serializer)


def open_reference(path: Path) -> Any:
    persistqueue = pytest.importorskip("persistqueue")
    from persistqueue.serializers import json as json_serializer  # noqa: PLC0415

    return persistqueue.SQLiteAckQueue(str(path), multithreading=True, auto_resume=True, serializer=json_serializer)


def statuses(queue: Any) -> list[tuple[int, str]]:
    return [(row["id"], str(row["status"])) for row in queue.queue()]


def run_markchain_flow(open_queue: OpenQueue, directory: Path) -> list[object]:
    """Markchain이 runtime에서 부르는 순서를 그대로 밟고 관측값을 모은다."""
    seen: list[object] = []
    queue = open_queue(directory)
    queue.clear_acked_data(keep_latest=0)
    ids = [queue.put(request(index)) for index in range(3)]
    seen.append(("put ids", ids))
    seen.append(("rows after put", [(row["id"], row["data"], str(row["status"])) for row in queue.queue()]))

    item = queue.get(id=ids[0], block=False, raw=True)
    seen.append(("get raw", item["pqid"], item["data"], sorted(item)))
    seen.append(("status after get", statuses(queue)))
    queue.update({**item["data"], "attempts": 1, "retry_at": 5.0}, id=item["pqid"])
    queue.nack(id=item["pqid"])
    seen.append(("status after update+nack", statuses(queue)))
    again = queue.get(id=ids[0], block=False, raw=True)
    seen.append(("get after nack", again["data"]))
    queue.ack(id=again["pqid"])
    queue.clear_acked_data(keep_latest=0)
    seen.append(("rows after ack+clear", statuses(queue)))

    queue.update({**request(1), "cancelled": True}, id=ids[1])  # 취소: 대기 중인 작업의 갱신
    seen.append(("cancelled", [row["data"]["cancelled"] for row in queue.queue() if row["id"] == ids[1]]))
    plain = queue.get(id=ids[2], block=False)  # 만료 정리: raw 없이 가져와 바로 ack
    seen.append(("get plain", plain))
    queue.ack(id=ids[2])
    seen.append(("status after expiry cleanup", statuses(queue)))
    queue.clear_acked_data(keep_latest=0)
    queue.shrink_disk_usage()

    with pytest.raises(Empty if isinstance(queue, SQLiteAckQueue) else Exception) as empty:
        queue.get(block=False, id=ids[0] + 100)
    seen.append(("empty", type(empty.value).__name__))

    left = queue.put(request(9))
    queue.get(id=left, block=False, raw=True)  # 처리 중인 채 프로세스가 끝난다
    queue.close()
    queue = open_queue(directory)
    seen.append(("status after restart", statuses(queue)))
    seen.append(("resumed get", queue.get(id=left, block=False, raw=True)["data"]["key"]))
    queue.close()
    return seen


@pytest.fixture(autouse=True)
def json_module(monkeypatch: pytest.MonkeyPatch) -> None:
    import json  # noqa: PLC0415
    import sys  # noqa: PLC0415
    import types  # noqa: PLC0415

    module = types.ModuleType("persistqueue_json")
    module.json_serializer = types.SimpleNamespace(  # type: ignore[attr-defined]
        dumps=lambda value: json.dumps(value).encode("utf-8"),
        loads=lambda data: json.loads(data.decode("utf-8")),
    )
    monkeypatch.setitem(sys.modules, "persistqueue_json", module)


def test_markchain_call_sequence_ours_matches_documented_observations(tmp_path: Path) -> None:
    seen = run_markchain_flow(open_ours, tmp_path / "ours")

    assert seen[0] == ("put ids", [1, 2, 3])
    assert seen[1] == ("rows after put", [(i + 1, request(i), "0") for i in range(3)])
    assert seen[3] == ("status after get", [(1, "2"), (2, "0"), (3, "0")])
    assert seen[4] == ("status after update+nack", [(1, "1"), (2, "0"), (3, "0")])
    assert seen[-2][1][-1][1] == "1"
    assert seen[-1] == ("resumed get", "k-9")


def test_markchain_call_sequence_matches_persist_queue_1_1_0(tmp_path: Path) -> None:
    reference = run_markchain_flow(open_reference, tmp_path / "reference")
    ours = run_markchain_flow(open_ours, tmp_path / "ours")

    assert ours == reference


def test_ack_without_claim_is_unconditional_in_persist_queue_but_rejected_here(tmp_path: Path) -> None:
    reference = open_reference(tmp_path / "reference")
    ours = open_ours(tmp_path / "ours")
    reference_id, ours_id = reference.put(request(0)), ours.put(request(0))

    assert reference.ack(id=reference_id) == reference_id
    with pytest.raises(UnknownClaimError):
        ours.ack(id=ours_id)
    reference.close()
    ours.close()


def test_stale_ack_after_lease_expiry_is_accepted_by_persist_queue_but_rejected_here(tmp_path: Path) -> None:
    now = {"ms": 1000}
    ours = SQLiteAckQueue(tmp_path / "ours", lease_ms=100, clock=lambda: now["ms"])
    ours_id = ours.put(request(0))
    ours.get(id=ours_id, block=False)
    now["ms"] += 100
    ours._queue.get(100)  # noqa: SLF001 - 다른 worker가 만료된 lease를 가져간다
    reference = open_reference(tmp_path / "reference")
    reference_id = reference.put(request(0))
    reference.get(id=reference_id, block=False)
    reference.nack(id=reference_id)
    reference.get(id=reference_id, block=False)  # 다른 worker가 가져갔다

    assert reference.ack(id=reference_id) == reference_id  # 옛 worker의 ack도 받아들인다
    with pytest.raises(StaleError):
        ours.ack(id=ours_id)
    reference.close()
    ours.close()


def test_update_of_acked_item_is_allowed_by_persist_queue_but_rejected_here(tmp_path: Path) -> None:
    reference = open_reference(tmp_path / "reference")
    ours = open_ours(tmp_path / "ours")
    for queue in (reference, ours):
        item_id = queue.put(request(0))
        queue.get(id=item_id, block=False)
        queue.ack(id=item_id)

    assert reference.update({"changed": True}, id=1) == 1
    with pytest.raises(StaleError):
        ours.update({"changed": True}, id=1)
    reference.close()
    ours.close()
