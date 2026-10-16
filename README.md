<h1 align="center">lrn-sql</h1>

<p align="center">
  파일 하나에 SQL 테이블과 선택형 ACK 큐를 담는 C 임베디드 데이터베이스<br>
  <a href="https://github.com/woonyong-choi/lrn-sql/actions/workflows/ci.yml"><img src="https://github.com/woonyong-choi/lrn-sql/actions/workflows/ci.yml/badge.svg" alt="CI"></a><br>
  <a href="#빠른-시작">빠른 시작</a> · <a href="#작동-방식">작동 방식</a> · <a href="#상태">상태</a>
</p>

> [!NOTE]
> 개발 중입니다. 배포판은 없고 소스에서 빌드해 실행합니다.

## 빠른 시작

GCC, GNU Make, Python 3이 필요합니다. 저장소를 받아 공유 라이브러리를 빌드하세요.

```sh
git clone https://github.com/woonyong-choi/lrn-sql.git
cd lrn-sql
make all
```

아래 내용을 `demo.py`로 저장하세요. 테이블을 만들고, 행 갱신과 큐 작업 추가를 한 트랜잭션으로 확정한 뒤 작업을 꺼내 처리합니다.

```python
from lrn_ackq import AckQueue, Database

with Database("demo.db") as db:
    db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)")
    db.execute("INSERT INTO users VALUES (?, ?, ?), (?, ?, ?)", [1, "Alice", 25, 2, "Bob", 31])
    print(db.execute("SELECT name, age FROM users WHERE age > ? ORDER BY age", [20]).rows)

    queue = AckQueue(db, "jobs")
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

## 해결하는 문제

로컬 프로그램이 업무 데이터와 작업 큐를 따로 저장하면 한쪽만 commit되어 작업이 사라지거나 두 번 실행될 수 있습니다. lrn-sql은 큐를 같은 DB 파일의 테이블로 두어 업무 갱신과 큐 변경을 한 트랜잭션으로 확정합니다. 큐를 설치하지 않으면 큐 테이블이나 객체가 생기지 않고 SQL DB로만 동작합니다. 여러 프로세스가 같은 파일에 동시에 써야 하거나 넓은 SQL 호환이 필요하면 SQLite나 PostgreSQL이 더 맞습니다.

## 작동 방식

1. 응용 프로그램이 C API `database_exec`나 Python `Database.execute`로 SQL과 매개변수를 넘깁니다.
2. lexer와 parser가 문장 하나를 AST로 바꾸고, 실행기가 기본 키·색인 조회나 테이블 스캔으로 행을 읽습니다.
3. 변경 문장은 바뀐 페이지를 새 자리에 쓰고, 제어 페이지 한 장으로 새 세대를 게시한 뒤 `fsync`합니다. 제어 페이지에 적은 페이지 체크섬 목록으로 끝나지 않은 commit을 알아보고 이전 세대로 엽니다.
4. 쓰기 트랜잭션은 한 번에 하나이고, 트랜잭션은 시작 때 게시된 상태와 자기 변경만 봅니다. 다른 writer가 있으면 Python 바인딩은 `busy_timeout_s`까지 다시 시도합니다.
5. ACK 큐는 공개 SQL과 트랜잭션 API만 써서 작업을 READY, LEASED, ACKED, FAILED 상태로 옮기고 fence 값으로 오래된 처리 결과를 거절합니다.

전체 구조는 [아키텍처](docs/architecture.md)에 있습니다.

## 상태

| 있는 것 | 아직 없는 것 | 다음 |
|---|---|---|
| CREATE TABLE·INDEX, INSERT·UPDATE·DELETE, DEFAULT, ON CONFLICT, RETURNING | ALTER TABLE, `INSERT ... SELECT`, 복합 색인, 창 함수, WITH RECURSIVE | [연산자·내장 함수 구현](https://github.com/woonyong-choi/lrn-sql/issues/40) |
| SELECT의 WHERE·ORDER BY·LIMIT, JOIN(INNER·LEFT·첫 RIGHT), GROUP BY·HAVING, 상관 서브쿼리, UNION·EXCEPT·INTERSECT, WITH | FULL JOIN, USING, 범위 조건의 색인 조회 | [기본키 범위 조회 구현](https://github.com/woonyong-choi/lrn-sql/issues/180) |
| 단일 writer, snapshot 격리, COW 게시와 `fsync` 뒤 commit 응답 | 여러 writer, 프로세스 간 공유, 저장 장치가 버린 쓰기 탐지 | [파일 I/O 오류 전파 구현](https://github.com/woonyong-choi/lrn-sql/issues/78) |
| at-least-once ACK 큐, lease와 fence, 업무 갱신과 같은 commit | 큐 연산의 색인 사용(claim·ack마다 테이블 전체 스캔) | [선택형 ACK 큐 라이브러리 구현](https://github.com/woonyong-choi/lrn-sql/issues/140) |
| 시험마다 SQLite와 PostgreSQL에 같은 SQL을 실행해 결과 대조 | 혼합 부하와 복구 비용 측정 | [혼합 부하·복구 비용 측정](https://github.com/woonyong-choi/lrn-sql/issues/137) |

1.0 전에는 파일 형식과 API가 예고 없이 바뀔 수 있습니다.

## 문서

- [아키텍처](docs/architecture.md): 구성 요소, 처리 흐름, 지켜야 할 규칙
- [빌드와 시험](docs/build-and-test.md): 빌드 변수, 시험 대상, 다른 엔진과 비교
- [C 코딩 규칙](docs/convention/c-style.md): C 코드 작성 기준
- [Python 코딩 규칙](docs/convention/python-style.md): Python 코드 작성 기준
- [폴더 구조 규칙](docs/convention/project-structure.md): 파일과 폴더를 두는 자리
- [커밋 규칙](docs/convention/commit-convention.md): 커밋·브랜치·PR 형식
- [전체 문서](docs/README.md)

## 개발

```sh
make all
make test-all test-runner test-fault test-result
make SANITIZE=thread BUILD_DIR=build-tsan test-transaction test-session test-ack-queue
make test-sql-diff
```
