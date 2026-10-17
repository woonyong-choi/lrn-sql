# 아키텍처

ToteDB는 파일 하나에 SQL 테이블을 저장하는 C11 임베디드 데이터베이스다. 응용 프로그램은 공개 C API(`tote_*`)나 공유 라이브러리 `libtotedb`를 거친 Python 패키지 `totedb`로 부르며, 선택형 ACK 큐는 같은 DB 위에 테이블 하나로 설치된다.

## 외부 연결

| 외부 요소 | 종류 | 주고받는 것 |
|---|---|---|
| 응용 프로그램(C) | 외부 프로그램 | `tote_exec`·`tote_begin`·`tote_commit` 호출과 결과 버퍼 |
| 응용 프로그램(Python) | 외부 프로그램 | `totedb.Database`·`Queue` 호출, ctypes로 `libtotedb`의 `tote_*`·`tote_queue_*` 함수 |
| DB 파일 | 파일 | 8192바이트 페이지. 여러 프로세스가 연다. 읽는 문장은 `flock` 공유, writer는 게시하는 순간만 배타로 쥔다 |
| writer 잠금 파일 | 파일 | DB 파일 옆 `<db>-lock`. 쓰기 트랜잭션 동안 `flock` 배타. 첫 바이트는 게시 대기 표시다. 지우지 않는다 |
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
| `database` | 데이터베이스 핸들 열기·닫기, 메모리 계상, 작업 카탈로그, COW 게시, 문장 실행 진입점 | C11 | `src/sql/database.c` |
| 실행기 | 문장 종류별 실행. 공유 타입은 `database_exec.h` 하나에 둔다 | C11 | `src/sql/database_expr.c`(WHERE·식), `database_subquery.c`(서브쿼리·WITH), `database_ddl.c`(CREATE·DROP), `database_insert.c`(INSERT·ON CONFLICT), `database_select.c`(SELECT·행 공급·집합 연산), `database_group.c`(GROUP BY·집계), `database_join.c`(결합), `database_update.c`(RETURNING·UPDATE·DELETE), `database_check.c`(페이지 회계) |
| `database_txn` | 여러 문장을 한 게시로 확정하는 트랜잭션 핸들 | C11 | `src/sql/database_txn.c` |
| `result` | 결과 집합과 오류를 바이트 버퍼로 인코딩 | C11 | `src/sql/result.c` |
| `session` | 연결 하나. 문장 실행, 트랜잭션, 예산 세 단계(문장·세션·풀), 결과 수명, 취소 | C11, pthread | `src/sql/session.c` |
| `ack_queue` | 선택형 ACK 큐. 큐 하나가 테이블 `tote_queue_<name>` 하나다 | C11, `session`의 SQL·트랜잭션 API | `src/queue/ack_queue.c`, `src/queue/ack_queue_sql.c` |
| `totedb` | 다른 언어용 평탄한 C 인터페이스 `tote_*`·`tote_queue_*`. 연결마다 자동 commit 세션 하나, 트랜잭션마다 세션 하나 | C11, 공유 라이브러리 | `src/capi/totedb.c` |
| `totedb` | Python 바인딩. `Database`·`Transaction`·`Queue` | Python 3, ctypes | `python/totedb` |

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

1. `ack_queue`가 `put`을 호출자가 준 세션의 INSERT로 실행한다. 세션에 트랜잭션이 열려 있으면 그 안에서 실행하고 commit은 호출자가 한다. 없으면 그 세션에 자기 트랜잭션을 열고 commit한다.
2. `get`이 READY 작업이나 lease가 끝난 LEASED 작업을 골라 LEASED로 바꾸고 fence 값을 1 올려 돌려준다.
3. `ack`·`nack`·`fail`·`extend`(lease 연장)는 작업 id와 fence가 지금 값과 같을 때만 적용한다.
4. `purge`는 끝난 작업을 지우고, idempotency 키가 있으면 키·id·payload 요약을 보존 기간 동안 `tote_queue_<name>_keys`에 남긴다. 그 기간의 같은 키 재시도는 지운 작업을 돌려준다.

## 지켜야 할 규칙

- 쓰기 트랜잭션은 한 번에 하나다. 페이지 단위 잠금 없이 COW 게시만으로 원자성을 지키기 위해서다.
- `heap_store`·`pk_tree`·`text_tree`·`catalog_store`는 게시된 페이지를 제자리에서 고치지 않고 새 페이지에 쓴다. commit 전에 멈춰도 다시 열면 이전 세대가 그대로 남게 하기 위해서다.
- 다시 열 때 최신 제어 페이지의 manifest와 실제 페이지가 다르면 이전 세대로 연다. `fsync` 한 번짜리 commit이 끝나지 않은 채 멈춘 상태를 게시된 상태로 읽지 않기 위해서다.
- 놓은 페이지는 commit이 두 번 성공한 뒤에만 다시 쓴다. 제어 페이지 하나를 잃어 한 세대 전으로 열려도 그 세대의 페이지가 덮어쓰이지 않게 하기 위해서다.
- commit 성공은 제어 쓰기 뒤의 `fsync`가 끝난 뒤에만 돌려준다. 성공을 받은 변경이 프로세스나 OS가 끝난 뒤에도 남게 하기 위해서다.
- 제어 페이지 쓰기를 시도한 뒤 실패하면 결과는 불확실이고 핸들은 poison 상태가 된다. 성공인지 실패인지 모르는 상태에서 다음 쓰기를 쌓지 않기 위해서다.
- DB 코어(`database`·`storage`)는 큐 모듈을 참조하지 않는다. 큐를 설치하지 않은 DB에 큐 객체나 테이블이 생기지 않게 하기 위해서다.
- 큐는 `session`의 SQL과 트랜잭션 API만 쓴다. 업무 갱신과 큐 변경을 한 commit으로 확정하고, DB 단독 사용과 같은 예산·결과 계상·취소 계약을 받기 위해서다.
- 한 세션은 한 번에 한 스레드만 쓴다. 트랜잭션은 세션에 속하므로 여러 스레드가 한 세션을 나눠 쓰면 다른 스레드의 문장이 남의 트랜잭션에 섞이기 때문이다.
- 쓰기 트랜잭션은 시작부터 끝까지 writer 잠금(`<db>-lock` 배타)을 쥔다. 같은 파일에 두 writer가 세대를 따로 게시하지 않게 하기 위해서다.
- writer는 제어 페이지를 쓰고 `fsync`하는 동안만 DB 파일 배타 잠금을 쥐고, 읽는 문장은 DB 파일 공유 잠금을 쥔다. 읽는 도중 세대가 바뀌지 않아, 그 세대의 페이지가 두 세대 지연 뒤 다시 쓰이는 일이 읽는 문장과 겹치지 않게 하기 위해서다.
- writer는 게시 잠금을 기다리기 전에 게시 대기 표시를 쓰고, 표시가 있으면 새 reader가 기다린다. 쉬지 않고 읽는 reader 사이에서 writer가 굶지 않게 하기 위해서다.
- fork 직후 자식은 물려받은 모든 page_file fd 사본을 닫는다. 부모가 죽거나 닫은 뒤에도 자식의 사본이 잠금을 붙잡지 않게 하기 위해서다.
- 잠금을 얻으면 디스크의 게시 세대를 확인하고, 바뀌었으면 제어 상태·빈 목록·캐시·카탈로그를 다시 읽는다. 다른 프로세스가 게시한 뒤 낡은 빈 목록으로 그 세대의 페이지를 덮어쓰지 않게 하기 위해서다.
- 잠금 밖에서는 페이지를 읽지 않는다. 다른 프로세스의 commit이 놓은 페이지를 두 세대 뒤 다시 쓸 수 있기 때문이다.
- 핸들은 연 프로세스에서만 쓴다. fork한 자식이 물려받은 핸들은 모든 연산을 거절한다. 자식이 부모의 잠금이나 쓰던 작업을 이어받아 파일을 바꾸지 않게 하기 위해서다.
- 매개변수 값은 SQL 문자열에 이어 붙이지 않고 binding으로만 받는다. 값이 문법으로 해석되지 않게 하기 위해서다.

## 배포

| 프로세스 | 시작 주체 | 수명 |
|---|---|---|
| 응용 프로그램 | 사용자 | 읽는 문장과 쓰기 트랜잭션의 실행 구간에 파일 잠금을 쥔다. 별도 서버 프로세스는 없다 |

| 경로 | 내용 | 쓰는 구성 요소 |
|---|---|---|
| 응용 프로그램이 정한 DB 파일 경로 | 제어 페이지, 카탈로그, 행·색인 페이지, 큐 테이블 | `page_file` |
| `build/lib/libtotedb.so` | Python 바인딩이 읽는 공유 라이브러리. macOS에서는 `.dylib` | `totedb` |
