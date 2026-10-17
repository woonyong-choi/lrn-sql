<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/totedb-icon.png">
    <img src="docs/assets/totedb-icon.png" alt="ToteDB 로고" width="96">
  </picture>
</p>

<h1 align="center">ToteDB</h1>

<p align="center">
  앱 안에 넣는 SQL 데이터베이스와, 같은 트랜잭션에 들어가는 작업 큐<br>
  <a href="https://github.com/woonyong-choi/totedb/actions/workflows/ci.yml"><img src="https://github.com/woonyong-choi/totedb/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://pypi.org/project/totedb/"><img src="https://img.shields.io/pypi/v/totedb" alt="PyPI 버전"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT 라이선스"></a>
</p>

## 목차

- [빠른 시작](#빠른-시작)
  - [여러 프로세스·언어에서 같은 큐 쓰기](#여러-프로세스언어에서-같은-큐-쓰기)
- [해결하는 문제](#해결하는-문제)
- [작동 방식](#작동-방식)
- [상태](#상태)
- [문서](#문서)
- [개발](#개발)
- [라이선스](#라이선스)

## 빠른 시작

Python 3.9 이상이 필요합니다. 가상 환경에서 설치하세요.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install totedb
```

Linux(x86_64·aarch64)와 macOS(arm64)용 wheel을 제공합니다. 그 밖의 POSIX 환경에서는 C 컴파일러와 GNU Make가 필요합니다.

아래 내용을 `demo.py`로 저장하세요. 주문 저장과 작업 등록을 한 트랜잭션으로 확정한 뒤, 작업을 꺼내 처리합니다. 예제는 임시 DB를 쓰고 실행이 끝나면 지웁니다.

```python
from pathlib import Path
from tempfile import TemporaryDirectory

from totedb import Database, Queue

with TemporaryDirectory() as directory, Database(Path(directory) / "demo.db") as db:
    db.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, status TEXT)")
    queue = Queue(db, "shipping")
    with db.begin() as txn:
        txn.execute("INSERT INTO orders VALUES (?, ?)", [1, "paid"])
        queue.put(b"ship-order-1", txn=txn)

    claim = queue.get(lease_ms=30_000)
    print(db.execute("SELECT * FROM orders").rows)
    print(claim.payload)
    queue.ack(claim)
    print(queue.stats())
```

```sh
python3 demo.py
```

```text
((1, 'paid'),)
b'ship-order-1'
QueueStats(ready=0, leased=0, acked=1, failed=0)
```

실제 데이터를 남기려면 `Database("orders.db")`처럼 파일 경로를 지정하세요. 큐 없이 SQL 데이터베이스로만 쓸 수도 있습니다.

### 여러 프로세스·언어에서 같은 큐 쓰기

Python에서 주문과 작업을 저장하고 C worker 셋이 같은 파일의 큐를 나눠 처리하는 [outbox 예제](examples/outbox/)가 있습니다. 작업을 받은 worker를 강제 종료한 뒤 다른 worker가 작업을 다시 받아 처리하는 과정도 확인합니다.

```sh
make example-outbox
```

## 해결하는 문제

로컬 프로그램이 주문과 출고 작업을 따로 저장하면, 주문만 저장하고 작업 등록 전에 멈출 수 있습니다. ToteDB는 SQL 테이블과 작업 큐를 같은 DB 파일에 두어 둘을 한 트랜잭션으로 확정합니다. 별도 DB 서버나 큐 서버를 띄우지 않고 앱 안에서 사용합니다.

작업은 처리 확인(ACK)을 받기 전까지 다시 전달될 수 있습니다. 메일 발송이나 외부 API 호출처럼 DB 밖에서 하는 일은 중복 실행을 견디도록 작성해야 합니다.

더 넓은 SQL 기능이 필요하면 SQLite를, 여러 writer의 동시 실행과 서버 운영 기능이 필요하면 PostgreSQL을 검토하세요.

## 작동 방식

1. 앱이 Python `Database.execute`나 C `tote_exec`로 SQL을 실행합니다.
2. 같은 트랜잭션 안에서 업무 행을 바꾸고 큐에 작업을 넣습니다.
3. 엔진이 바뀐 페이지를 새 자리에 쓰고, 새 세대를 게시한 뒤 `fsync`를 마쳐 commit을 응답합니다.
4. worker가 작업을 받아 처리하고 ACK합니다. 처리 기한(lease)이 끝나면 다른 worker가 다시 받을 수 있습니다.

![업무 테이블과 큐 테이블은 같은 DB에 저장되고 worker는 그 큐에서 작업을 받아 처리한다](docs/assets/transaction-queue.svg)

여러 프로세스가 같은 파일을 열 수 있지만 쓰기 트랜잭션은 한 번에 하나입니다. 이미 열린 핸들의 조회는 writer가 작업하는 동안 마지막으로 게시된 상태를 읽고, 새 세대를 게시할 때는 잠금을 기다립니다.

Python의 `busy_timeout_s`는 문장 실행과 쓰기 트랜잭션 시작에 적용합니다. 새 핸들을 여는 `Database(...)`는 재시도하지 않으므로 writer와 열기 잠금이 겹치면 `DatabaseOpenError`가 날 수 있습니다.

저장 방식과 잠금 규칙은 [아키텍처](docs/architecture.md)에 있습니다.

## 상태

[0.1.0 릴리스](https://github.com/woonyong-choi/totedb/releases/tag/v0.1.0)와 Python 패키지를 배포했습니다. 1.0 전에는 파일 형식과 API가 예고 없이 바뀔 수 있습니다.

| 있는 것 | 아직 없는 것 | 다음 |
|---|---|---|
| SQL 조회·갱신, JOIN·집계·서브쿼리, 정수 기본 키·TEXT 색인 | ALTER TABLE, 복합 색인, 창 함수 | [기본 키 조회 성능 측정 마무리 #158](https://github.com/woonyong-choi/totedb/issues/158) |
| 단일 writer, 여러 프로세스의 파일 공유, commit 후 복구 | 여러 writer 동시 commit, 네트워크 파일 시스템 지원 | 없음 |
| ACK 큐, 처리 기한·연장, 중복 등록 키, C·Python 바인딩 | C·Python 외 언어 바인딩 | 없음 |

SQL 차등 시험은 SQLite와 대조하며, PostgreSQL 비교는 연결 설정이 있을 때 실행합니다. 강제 종료와 I/O 실패 시험의 결과·조건은 [안전성 시험 결과](docs/safety.md)에 있습니다. 실제 전원 차단은 검증하지 않았습니다.

변경 내용은 [변경 기록](CHANGELOG.md)에 있습니다.

## 문서

- [아키텍처](docs/architecture.md): 구성 요소, 처리 흐름, 지켜야 할 규칙
- [빌드와 시험](docs/build-and-test.md): 빌드 변수, 시험 대상, 다른 엔진과 비교, 패키지 배포
- [안전성 시험 결과](docs/safety.md): 강제 종료·I/O 실패·여러 프로세스 시험과 결과, 확인하지 않은 범위
- [C 코딩 규칙](docs/convention/c-style.md): C 코드 작성 기준
- [Python 코딩 규칙](docs/convention/python-style.md): Python 코드 작성 기준
- [폴더 구조 규칙](docs/convention/project-structure.md): 파일과 폴더를 두는 자리
- [기여 안내](.github/CONTRIBUTING.md): 검사, 커밋·PR 형식
- [전체 문서](docs/README.md)

## 개발

GCC 또는 Clang, GNU Make, Python 3, pytest가 필요합니다. SQL 차등 시험에는 SQLite 개발 라이브러리도 필요합니다.

```sh
git clone https://github.com/woonyong-choi/totedb.git
cd totedb
make all
make test-all test-runner test-fault test-result
make SANITIZE=thread BUILD_DIR=build-tsan test-transaction test-session test-ack-queue
make test-sql-diff
```

소스의 Python 바인딩을 쓰려면 `PYTHONPATH=python`을 지정하세요. 검사 범위와 변경 제출 방법은 [기여 안내](.github/CONTRIBUTING.md)에 있습니다.

## 라이선스

[MIT](LICENSE)
