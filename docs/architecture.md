# 아키텍처

lrn-sql은 파일 하나에 SQL 테이블을 저장하는 C11 임베디드 데이터베이스다. 응용 프로그램은 C API(`database`·`database_txn`)나 공유 라이브러리 `liblrnackq`를 거친 Python 패키지 `lrn_ackq`로 부르며, 선택형 ACK 큐는 같은 DB 위에 테이블 하나로 설치된다.

## 외부 연결

| 외부 요소 | 종류 | 주고받는 것 |
|---|---|---|
| 응용 프로그램(C) | 외부 프로그램 | `database_exec`·`database_txn_*` 호출과 결과 버퍼 |
| 응용 프로그램(Python) | 외부 프로그램 | `lrn_ackq.Database`·`AckQueue` 호출, ctypes로 `liblrnackq`의 `lrndb_*`·`lrnq_*` 함수 |
| DB 파일 | 파일 | 8192바이트 페이지. `flock`으로 한 프로세스의 한 핸들만 쓰기로 연다 |
| 운영체제 | 운영체제 | `pread`·`pwrite`·`fsync`, pthread mutex와 조건 변수 |

## 구성 요소

| 구성 요소 | 하는 일 | 기술 | 위치 |
|---|---|---|---|
| `page_file` | 페이지 읽기·쓰기, 체크섬 검사, 제어 페이지 두 장으로 세대 게시, 빈 페이지 목록 보관 | C11, POSIX 파일 I/O | `src/storage/page_file.c` |
| `page_cache` | 읽은 페이지를 메모리에 두고 다시 읽기를 줄인다 | C11 | `src/storage/page_cache.c` |
| `record`·`overflow`·`value` | 행 인코딩, 큰 TEXT·BLOB의 overflow 체인, 값 형식과 비교 | C11 | `src/storage/record.c`, `src/storage/overflow.c`, `src/storage/value.c` |
| `catalog`·`catalog_store` | 테이블 schema·기본값·색인 목록을 직렬화해 파일에 저장 | C11 | `src/storage/catalog.c`, `src/storage/catalog_store.c` |
| `heap_store` | 테이블 하나의 행 저장소, 페이지 디렉터리, 변경을 모은 batch | C11 | `src/storage/heap_store.c` |
| `pk_tree`·`text_tree` | 정수 기본 키와 TEXT 키를 행 위치로 잇는 COW B+Tree | C11 | `src/storage/pk_tree.c`, `src/storage/text_tree.c` |
| `transaction` | 단일 writer 경합, snapshot 순번, commit 결과, timeout·취소 | C11, pthread | `src/storage/transaction.c` |
| `lexer`·`sql_statement` | SQL 문장을 토큰으로 나누고 문 구조(AST)로 바꾼다 | C11 | `src/sql/lexer.c`, `src/sql/sql_statement.c` |
| `sql_eval` | 식을 컴파일하고 행마다 평가한다. 내장 함수와 집계를 포함한다 | C11 | `src/sql/sql_eval.c` |
| `database` | 문장 실행기. 테이블 스캔·색인 조회·JOIN·GROUP BY·서브쿼리·집합 연산·ON CONFLICT·RETURNING | C11 | `src/sql/database.c` |
| `database_txn` | 여러 문장을 한 게시로 확정하는 트랜잭션 핸들 | C11 | `src/sql/database_txn.c` |
| `result` | 결과 집합과 오류를 바이트 버퍼로 인코딩 | C11 | `src/sql/result.c` |
| `session` | 세션별 메모리 예산과 결과 수명 | C11, pthread | `src/sql/session.c` |
| `ack_queue` | 선택형 ACK 큐. 큐 하나가 테이블 `ackq_<name>` 하나다 | C11, 공개 SQL·트랜잭션 API | `src/queue/ack_queue.c`, `src/queue/ack_queue_sql.c` |
| `ack_queue_capi` | 다른 언어용 평탄한 C 인터페이스 `lrndb_*`·`lrnq_*` | C11, 공유 라이브러리 | `src/queue/ack_queue_capi.c` |
| `lrn_ackq` | Python 바인딩. `Database`·`Transaction`·`AckQueue` | Python 3, ctypes | `python/lrn_ackq` |

## 처리 흐름

### 자동 commit 문장

1. `database`가 `database_exec`로 SQL 바이트와 매개변수 binding을 받는다.
2. `lexer`가 입력 전체를 토큰으로 나누고 문장이 둘 이상이면 실행 전에 거절한다.
3. `sql_statement`가 토큰을 AST로 바꾸고 매개변수 값을 리터럴로 복제한다.
4. `sql_eval`이 WHERE·SELECT 목록·ORDER BY의 식을 컴파일하고 컬럼 이름을 행 위치로 해석한다.
5. `database`는 조건이 기본 키나 색인 컬럼의 같음 비교이면 `pk_tree`·`text_tree`를 조회하고, 정수 기본 키의 대소 비교이면 `pk_tree`에서 그 구간만 순회한다. 둘 다 아니면 `heap_store`를 스캔한다. 색인으로 찾은 행에도 전체 조건을 다시 평가한다.
6. 변경 문장이면 `heap_store` batch가 바뀐 행 페이지와 색인 페이지를 새 자리에 쓴다.
7. `page_file`이 제어 페이지 한 장에 새 세대와 이번에 쓴 페이지의 체크섬 목록(manifest)을 쓰고 `fsync`한다. 게시된 페이지를 덮어썼거나 manifest가 넘치면 제어 쓰기 전에 데이터 페이지를 먼저 `fsync`한다.
8. `result`가 행이나 오류를 버퍼로 인코딩해 호출자에게 넘긴다.

### 명시적 트랜잭션

1. `database_txn`이 `transaction` manager에서 writer 자리를 얻는다. 다른 writer가 있으면 timeout까지 기다린 뒤 `TXN_BUSY`를 돌려준다.
2. 트랜잭션 안의 문장은 시작 때 게시된 상태와 자기 변경만 본다.
3. 한 문장이 오류로 끝나면 트랜잭션은 실패 상태가 되고 이후 문장과 commit을 거절한다.
4. commit은 자동 commit 문장과 같은 COW 게시를 한 번 한다.

### 큐 작업

1. `ack_queue`가 `put`을 테이블 `ackq_<name>`의 INSERT로 실행한다. 호출자가 트랜잭션을 주면 그 안에서 실행하고 commit은 호출자가 한다.
2. `get`이 READY 작업이나 lease가 끝난 LEASED 작업을 골라 LEASED로 바꾸고 fence 값을 1 올려 돌려준다.
3. `ack`·`nack`·`fail`은 작업 id와 fence가 지금 값과 같을 때만 적용한다.

## 지켜야 할 규칙

- 쓰기 트랜잭션은 한 번에 하나다. 페이지 단위 잠금 없이 COW 게시만으로 원자성을 지키기 위해서다.
- `heap_store`·`pk_tree`·`text_tree`·`catalog_store`는 게시된 페이지를 제자리에서 고치지 않고 새 페이지에 쓴다. commit 전에 멈춰도 다시 열면 이전 세대가 그대로 남게 하기 위해서다.
- 다시 열 때 최신 제어 페이지의 manifest와 실제 페이지가 다르면 이전 세대로 연다. `fsync` 한 번짜리 commit이 끝나지 않은 채 멈춘 상태를 게시된 상태로 읽지 않기 위해서다.
- 놓은 페이지는 commit이 두 번 성공한 뒤에만 다시 쓴다. 제어 페이지 하나를 잃어 한 세대 전으로 열려도 그 세대의 페이지가 덮어쓰이지 않게 하기 위해서다.
- commit 성공은 제어 쓰기 뒤의 `fsync`가 끝난 뒤에만 돌려준다. 성공을 받은 변경이 프로세스나 OS가 끝난 뒤에도 남게 하기 위해서다.
- 제어 페이지 쓰기를 시도한 뒤 실패하면 결과는 불확실이고 핸들은 poison 상태가 된다. 성공인지 실패인지 모르는 상태에서 다음 쓰기를 쌓지 않기 위해서다.
- DB 코어(`database`·`storage`)는 큐 모듈을 참조하지 않는다. 큐를 설치하지 않은 DB에 큐 객체나 테이블이 생기지 않게 하기 위해서다.
- 큐는 공개 SQL과 트랜잭션 API만 쓴다. 업무 갱신과 큐 변경을 한 commit으로 확정하기 위해서다.
- 한 DB 파일은 한 프로세스의 한 핸들만 쓰기로 연다. 같은 파일에 두 writer가 세대를 따로 게시하지 않게 하기 위해서다.
- 매개변수 값은 SQL 문자열에 이어 붙이지 않고 binding으로만 받는다. 값이 문법으로 해석되지 않게 하기 위해서다.

## 배포

| 프로세스 | 시작 주체 | 수명 |
|---|---|---|
| 응용 프로그램 | 사용자 | DB 핸들을 연 동안 파일 잠금을 쥔다. 별도 서버 프로세스는 없다 |

| 경로 | 내용 | 쓰는 구성 요소 |
|---|---|---|
| 응용 프로그램이 정한 DB 파일 경로 | 제어 페이지, 카탈로그, 행·색인 페이지, 큐 테이블 | `page_file` |
| `build/queue-shared/liblrnackq.so` | Python 바인딩이 읽는 공유 라이브러리. macOS에서는 `.dylib` | `lrn_ackq` |
