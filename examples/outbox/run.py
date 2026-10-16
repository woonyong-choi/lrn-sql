"""Python 생산자와 C 소비자가 같은 DB 파일의 같은 큐를 함께 쓰는 예제(#201).

1. Python(웹 앱 역할)이 주문 저장과 출고 작업 등록을 한 트랜잭션으로 확정한다.
2. C worker 하나가 작업을 claim한 뒤 멈추고, 이 스크립트가 그 프로세스를 SIGKILL로 죽인다.
3. 다른 C worker들이 나머지 작업과, lease가 끝난 그 작업을 처리한다. 출고 기록과 ack는 한 트랜잭션이다.
4. 모든 주문이 정확히 한 번 출고됐는지 확인한다.

사용: make example-outbox
      또는 TOTEDB_LIB=build/lib/libtotedb.so PYTHONPATH=python python3 examples/outbox/run.py --consumer build/outbox_consumer
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

from totedb import Database, Queue


def place_orders(path: Path, orders: int) -> None:
    db = Database(path)
    db.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, item TEXT NOT NULL, qty INTEGER NOT NULL)")
    db.execute("CREATE TABLE shipments (order_id INTEGER PRIMARY KEY, worker TEXT NOT NULL)")
    queue = Queue(db, "shipping")
    for order_id in range(1, orders + 1):
        # 주문과 출고 작업이 함께 남거나 함께 사라진다. 한쪽만 남는 일이 없다
        with db.begin() as txn:
            txn.execute("INSERT INTO orders VALUES (?, ?, ?)", (order_id, f"item-{order_id % 7}", order_id % 3 + 1))
            queue.put(f"order:{order_id}".encode(), idem_key=f"order-{order_id}", txn=txn)
    queue.close()
    db.close()
    print(f"[python] 주문 {orders}건과 출고 작업 {orders}건을 같은 트랜잭션으로 넣음")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--consumer", required=True, help="examples/outbox/consumer.c를 빌드한 실행 파일")
    parser.add_argument("--orders", type=int, default=30)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--lease-ms", type=int, default=800)
    parser.add_argument("--dir", type=Path, default=None, help="DB를 둘 폴더. 없으면 임시 폴더")
    args = parser.parse_args()
    workdir = args.dir or Path(tempfile.mkdtemp(prefix="totedb-outbox-"))
    workdir.mkdir(parents=True, exist_ok=True)
    path = workdir / "shop.totedb"
    if path.exists():
        path.unlink()
    place_orders(path, args.orders)

    lease = str(args.lease_ms)
    hung = subprocess.Popen([args.consumer, str(path), "c-hung", lease, "--hang-after-claim"],
                            stdout=subprocess.PIPE, text=True)
    first = hung.stdout.readline().split()
    assert first[0] == "CLAIMED", first
    lost_order = int(first[1])
    os.kill(hung.pid, signal.SIGKILL)
    hung.wait()
    print(f"[c-hung] 주문 {lost_order}을 claim한 뒤 SIGKILL로 강제 종료됨")

    workers = [subprocess.Popen([args.consumer, str(path), f"c-{i + 1}", lease], stdout=subprocess.PIPE, text=True)
               for i in range(args.workers)]
    shipped_by: dict[int, str] = {}
    for worker in workers:
        out, _ = worker.communicate(timeout=120)
        if worker.returncode != 0:
            print(f"worker 종료 코드 {worker.returncode}", file=sys.stderr)
            return 1
        for line in out.splitlines():
            parts = line.split()
            if parts[0] == "SHIPPED":
                assert int(parts[1]) not in shipped_by, f"주문 {parts[1]} 두 번 출고"
                shipped_by[int(parts[1])] = parts[2]
    for name in sorted({w for w in shipped_by.values()}):
        print(f"[{name}] {sum(1 for w in shipped_by.values() if w == name)}건 처리")

    db = Database(path, create=False)
    rows = db.execute("SELECT COUNT(*), COUNT(DISTINCT order_id) FROM shipments").rows[0]
    missing = db.execute(
        "SELECT COUNT(*) FROM orders WHERE id NOT IN (SELECT order_id FROM shipments)").rows[0][0]
    stats = Queue(db, "shipping", install=False).stats()
    redelivered_to = shipped_by.get(lost_order)
    db.close()
    ok = (rows == (args.orders, args.orders) and missing == 0 and stats.acked == args.orders and
          stats.ready == stats.leased == 0 and redelivered_to is not None)
    print(f"[검증] 출고 {rows[0]}건, 중복 0, 누락 {missing}, 큐 acked {stats.acked}, "
          f"강제 종료된 worker의 주문 {lost_order}은 worker {redelivered_to}에 재전달되어 처리됨")
    print("검증 통과" if ok else "검증 실패")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
