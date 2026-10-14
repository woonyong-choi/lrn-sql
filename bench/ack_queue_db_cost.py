#!/usr/bin/env python3
"""ACK 큐가 쓰는 SQL 문장 하나하나의 DB 비용(#140 완료 조건 5: 전체 pending 복사와 DB 비용 분리). 합성 데이터만 쓴다.

큐 코드를 거치지 않고 큐 테이블과 같은 모양의 테이블에 같은 종류의 문장을 직접 실행해, 행 수(n)와 payload 크기(s)에 따라
문장 한 번의 시간이 어떻게 변하는지 잰다.
  meta_scan   SELECT 메타데이터 컬럼 WHERE state = 0   claim이 후보를 찾는 전체 스캔(모든 행이 후보)
  id_scan     SELECT id FROM t                          enqueue 뒤 새 id를 찾는 스캔
  update_one  UPDATE ... WHERE id = ?                   claim·ack가 한 행을 바꾸는 문장
  payload_one SELECT payload WHERE id = ?               claim이 payload 하나를 읽는 문장
payload 크기에 따라 meta_scan이 늘어나면 SQL 계층이 projection 없이 행 전체를 읽는다는 뜻이다
(이 스크립트가 그 여부를 보인다).

사용법: LRN_ACKQ_LIB=... PYTHONPATH=python python3 bench/ack_queue_db_cost.py --sizes 100,200 --payloads 256,4096,16384
"""

from __future__ import annotations

import argparse
import json
import statistics
import tempfile
import time
from pathlib import Path

from lrn_ackq import Database

SCHEMA = (
    "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, state INTEGER NOT NULL, ready_at INTEGER NOT NULL,"
    " lease_until INTEGER NOT NULL, fence INTEGER NOT NULL, attempts INTEGER NOT NULL, payload BLOB NOT NULL)"
)
STATEMENTS = {
    "meta_scan": ("SELECT id, state, ready_at, lease_until, fence, attempts FROM t WHERE state = ?", (0,)),
    "id_scan": ("SELECT id FROM t", ()),
    "update_one": ("UPDATE t SET state = 0, lease_until = ?, fence = ?, attempts = ? WHERE id = ?", (0, 1, 1, 1)),
    "payload_one": ("SELECT payload FROM t WHERE id = ?", (1,)),
}


# cost: time O(k·n·s), heap O(n·s), stack O(1), io O(n)
# vars: k = 반복 수, n = 행 수, s = payload 바이트
# basis: estimate
def measure(rows: int, payload_bytes: int, repeats: int) -> dict[str, float]:
    """한 (n, s) 조합에서 문장별 한 번의 중앙값 시간(밀리초)."""
    result: dict[str, float] = {"n": rows, "payload_bytes": payload_bytes}
    with tempfile.TemporaryDirectory(prefix="ackq-dbcost-") as tmp:
        db = Database(Path(tmp) / "db.lrdb", Path(tmp) / "db.wal", busy_timeout_s=60.0)
        db.execute(SCHEMA)
        body = bytes(index % 251 for index in range(payload_bytes))
        for _ in range(rows):
            db.execute(
                "INSERT INTO t (state, ready_at, lease_until, fence, attempts, payload) VALUES (0, 0, 0, 0, 0, ?)",
                (body,),
            )
        for name, (sql, params) in STATEMENTS.items():
            samples = []
            for _ in range(repeats):
                start = time.perf_counter()
                db.execute(sql, params)
                samples.append((time.perf_counter() - start) * 1000)
            result[name] = round(statistics.median(samples), 3)
        db.close()
    return result


# cost: time O(m), heap O(1), stack O(1), io O(m)
# vars: m = 측정 조합 수와 한 조합의 측정 시간
# basis: estimate
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sizes", default="100,200")
    parser.add_argument("--payloads", default="256,4096,16384")
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    for rows in map(int, args.sizes.split(",")):
        for payload in map(int, args.payloads.split(",")):
            print(json.dumps(measure(rows, payload, args.repeats), ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
