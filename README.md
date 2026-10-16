<h1 align="center">ToteDB</h1>

<p align="center">
  Transactional Outbox Table Engine<br>
  앱 안에 넣는 SQL 데이터베이스와, 같은 트랜잭션에 들어가는 작업 큐<br>
  <a href="https://github.com/woonyong-choi/totedb/actions/workflows/ci.yml"><img src="https://github.com/woonyong-choi/totedb/actions/workflows/ci.yml/badge.svg" alt="CI"></a><br>
  <a href="#빠른-시작">빠른 시작</a> · <a href="#작동-방식">작동 방식</a> · <a href="#상태">상태</a>
</p>

> [!NOTE]
> 첫 공개판 0.1.0입니다. 소스에서 빌드해 실행합니다. 1.0 전에는 파일 형식과 API가 바뀔 수 있습니다.

## 빠른 시작

GCC, GNU Make, Python 3이 필요합니다. 저장소를 받아 공유 라이브러리를 빌드하세요.

```sh
git clone https://github.com/woonyong-choi/totedb.git
cd totedb
make all
```

아래 내용을 `demo.py`로 저장하세요. 테이블을 만들고, 행 갱신과 큐 작업 추가를 한 트랜잭션으로 확정한 뒤 작업을 꺼내 처리합니다.

```python
from totedb import Database, Queue

with Database("demo.db") as db:
    db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)")
    db.execute("INSERT INTO users VALUES (?, ?, ?), (?, ?, ?)", [1, "Alice", 25, 2, "Bob", 31])
    print(db.execute("SELECT name, age FROM users WHERE age > ? ORDER BY age", [20]).rows)

    queue = Queue(db, "jobs")
    with db.begin() as txn:
        txn.execute("UPDATE users SET age = age + 1 WHERE name = ?", ["Alice"])
        queue.put(b"send-welcome-mail", txn=txn)
    claim = queue.get(lease_ms=30_000)
    print(claim.payload)
    queue.ack(claim)
    print(queue.stats())
```

```sh
PYTHONPATH=python python3 demo.py
```

```text
(('Alice', 25), ('Bob', 31))
b'send-welcome-mail'
QueueStats(ready=0, leased=0, acked=1, failed=0)
```

### 여러 프로세스·언어에서 같은 큐 쓰기

큐의 lease, fence, 멱등 키 규칙은 C 코어에 있어 어느 언어로 붙어도 같게 동작합니다. 아래 예제는 Python 프로세스가 주문 저장과 출고 작업 등록을 한 트랜잭션으로 확정하고, C worker 프로세스 셋이 같은 파일의 같은 큐를 나눠 처리합니다. 작업 하나를 claim한 worker를 강제 종료해도 lease가 끝나면 다른 worker가 그 작업을 받아 처리합니다. 코드는 [`examples/outbox/`](examples/outbox/)에 있습니다.

```sh
make example-outbox
```

```text
[python] 주문 30건과 출고 작업 30건을 같은 트랜잭션으로 넣음
[c-hung] 주문 1을 claim한 뒤 SIGKILL로 강제 종료됨
[c-1] 10건 처리
[c-2] 10건 처리
[c-3] 10건 처리
[검증] 출고 30건, 중복 0, 누락 0, 큐 acked 30, 강제 종료된 worker의 주문 1은 worker c-2에 재전달되어 처리됨
검증 통과
```

어느 worker가 몇 건을 처리하는지는 실행마다 다릅니다.

## 해결하는 문제

로컬 프로그램이 업무 데이터와 작업 큐를 따로 저장하면 한쪽만 commit되어 작업이 사라지거나 두 번 실행될 수 있습니다. ToteDB은 큐를 같은 DB 파일의 테이블로 두어 업무 갱신과 큐 변경을 한 트랜잭션으로 확정합니다. 큐를 설치하지 않으면 큐 테이블이나 객체가 생기지 않고 SQL DB로만 동작합니다. 한 부모 프로세스가 띄운 여러 worker 프로세스가 같은 파일과 같은 큐를 함께 씁니다. 쓰기는 파일 전체에서 한 번에 하나씩 확정합니다. 여러 writer가 동시에 commit해야 하거나 넓은 SQL 호환이 필요하면 SQLite나 PostgreSQL이 더 맞습니다.

## 작동 방식

1. 응용 프로그램이 C API `database_exec`나 Python `Database.execute`로 SQL과 매개변수를 넘깁니다.
2. lexer와 parser가 문장 하나를 AST로 바꾸고, 실행기가 기본 키·색인 조회나 테이블 스캔으로 행을 읽습니다.
3. 변경 문장은 바뀐 페이지를 새 자리에 쓰고, 제어 페이지 한 장으로 새 세대를 게시한 뒤 `fsync`합니다. 제어 페이지에 적은 페이지 체크섬 목록으로 끝나지 않은 commit을 알아보고 이전 세대로 엽니다.
4. 쓰기 트랜잭션은 같은 파일을 연 모든 프로세스에서 한 번에 하나이고, 트랜잭션은 시작 때 게시된 상태와 자기 변경만 봅니다. 다른 프로세스가 commit하면 다음 문장이 새 상태를 다시 읽습니다. 다른 프로세스가 쓰는 동안에도 조회는 기다리지 않고 마지막으로 게시된 상태를 읽습니다. 다른 writer가 있으면 Python 바인딩은 `busy_timeout_s`까지 다시 시도합니다.
5. ACK 큐는 공개 SQL과 트랜잭션 API만 써서 작업을 READY, LEASED, ACKED, FAILED 상태로 옮기고 fence 값으로 오래된 처리 결과를 거절합니다.

전체 구조는 [아키텍처](docs/architecture.md)에 있습니다.

## 상태

| 있는 것 | 아직 없는 것 | 다음 |
|---|---|---|
| CREATE TABLE·INDEX, INSERT·UPDATE·DELETE, DEFAULT, ON CONFLICT, RETURNING | ALTER TABLE, `INSERT ... SELECT`, 복합 색인, 창 함수, WITH RECURSIVE | — |
| SELECT의 WHERE·ORDER BY·LIMIT, JOIN(INNER·LEFT·첫 RIGHT), GROUP BY·HAVING, 상관 서브쿼리, UNION·EXCEPT·INTERSECT, WITH, 정수 기본 키 범위 조회 | FULL JOIN, USING, 보조 색인 범위 조회, 보조 색인 순서 정렬 | — |
| 여러 프로세스가 같은 파일 공유(쓰기는 한 번에 하나, 쓰는 동안에도 조회는 기다리지 않음), snapshot 격리, COW 게시와 `fsync` 뒤 commit 응답, fork한 자식의 부모 핸들 사용 거절 | 여러 writer 동시 commit, 네트워크 파일 시스템, 저장 장치가 버린 쓰기 탐지 | — |
| at-least-once ACK 큐, lease와 fence, lease 연장, 업무 갱신과 같은 commit, purge 뒤에도 보존 기간(기본 7일) 동안 유지되는 idempotency 키, 큐별 payload 상한(기본 1 MiB), 여러 프로세스·언어(Python, C)가 같은 큐 사용 | Python·C 밖의 언어 바인딩 | — |
| 시험마다 SQLite와 PostgreSQL에 같은 SQL을 실행해 결과 대조, commit 중 모든 쓰기 지점의 종료·I/O 실패 시험([안전성 시험 결과](docs/safety.md)), SQLite와 같은 입력의 CRUD·혼합 부하·강제 종료 뒤 복구 측정 | 전원 차단 검증, SQLite 수준의 파일 크기(같은 1만 행에서 약 1.6배) | — |

1.0 전에는 파일 형식과 API가 예고 없이 바뀔 수 있습니다. 바뀐 내용은 [변경 기록](CHANGELOG.md)에 있습니다.

## 문서

- [아키텍처](docs/architecture.md): 구성 요소, 처리 흐름, 지켜야 할 규칙
- [빌드와 시험](docs/build-and-test.md): 빌드 변수, 시험 대상, 다른 엔진과 비교
- [안전성 시험 결과](docs/safety.md): 강제 종료·I/O 실패·여러 프로세스 시험과 결과, 확인하지 않은 범위
- [C 코딩 규칙](docs/convention/c-style.md): C 코드 작성 기준
- [Python 코딩 규칙](docs/convention/python-style.md): Python 코드 작성 기준
- [폴더 구조 규칙](docs/convention/project-structure.md): 파일과 폴더를 두는 자리
- [기여 안내](.github/CONTRIBUTING.md): 검사, 커밋·PR 형식
- [전체 문서](docs/README.md)

## 개발

```sh
make all
make test-all test-runner test-fault test-result
make SANITIZE=thread BUILD_DIR=build-tsan test-transaction test-session test-ack-queue
make test-sql-diff
```

## 라이선스

[MIT](LICENSE)
