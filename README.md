# MiniDB — 디스크 기반 SQL 엔진 + HTTP API 서버

MiniDB는 단일 `.db` 파일 위에서 동작하는 작은 디스크 기반 SQL 엔진이다.  
현재 구현은 **단일 테이블**, **자동 생성 `id` 인덱스(B+ Tree)**, **slotted heap**, **pager 캐시**, **HTTP API 서버**, **다층 동시성 제어**를 한 프로젝트 안에 묶어 두었다.

이 저장소는 "SQL 문법만 파싱하는 과제"가 아니라, 실제로 요청이 들어와서 디스크 페이지까지 내려가는 전체 경로를 구현하고 검증하는 것을 목표로 한다.

---

## 한눈에 보기

- 언어: C11
- 실행 모드:
  - REPL
  - HTTP API 서버
- 저장 구조:
  - Heap table
  - B+ Tree (`id -> row_ref`)
  - Pager frame cache
- SQL 기능:
  - `CREATE TABLE`
  - `INSERT`
  - `SELECT`
  - `UPDATE`
  - `DELETE`
  - `DROP TABLE`
  - `EXPLAIN`
  - `COUNT(*)`
  - `ORDER BY`
  - `LIMIT`
- 동시성 계층:
  - Row / Range Lock
  - Engine RWLock
  - Page latch
  - Pager mutex
  - Header lock
- 서버 구조:
  - `thread-per-connection`
  - keep-alive 지원
  - `GET /stats`
  - `POST /query`

---

## 프로젝트 소개

MiniDB는 다음 질문에 답하는 식으로 만들어졌다.

- SQL을 디스크 페이지 기반으로 저장하려면 구조를 어떻게 나눌까?
- `WHERE id = ...`와 전체 스캔은 어떻게 다르게 처리할까?
- 여러 요청이 동시에 들어오면 어디에서 충돌이 날까?
- 같은 row의 "의미"를 지키는 락과, 페이지 메모리를 지키는 락은 왜 달라야 할까?
- thread pool과 keep-alive를 같이 쓰면 왜 병목이 생길까?

즉 이 프로젝트는 SQL 기능 구현과 동시에,

- 저장 엔진
- 인덱스
- 캐시
- 서버
- 동시성
- 테스트

를 함께 다루는 작은 DBMS 실험 저장소다.

---

## 현재 구조

```text
┌────────────────────────────────────────────────────────────────────┐
│                             Client                                 │
│     curl / browser / stress tool / custom socket client           │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                          HTTP Server                               │
│  accept() -> connection thread -> HTTP parse -> db_execute()       │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                           SQL Layer                                │
│        parser -> planner -> executor -> exec_result_t              │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                    Concurrency Control Layer                       │
│ Row/Range Lock -> Engine RWLock -> Page Latch -> Pager Mutex       │
│                     + Header Lock                                  │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                          Storage Layer                             │
│       Heap Pages + B+ Tree + Pager Cache + Free Page List          │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                            Disk File                               │
│                            single .db                              │
└────────────────────────────────────────────────────────────────────┘
```

---

## 핵심 특징

### 1. 디스크 기반 저장

모든 데이터는 고정 크기 페이지 단위로 저장된다.

- header page: DB 전역 메타데이터
- heap page: 실제 row 데이터
- leaf page: `id -> row_ref` 인덱스 엔트리
- internal page: B+ Tree 탐색 경로
- free page: 재사용 가능한 빈 페이지

### 2. 자동 `id` 시스템 컬럼

사용자가 `CREATE TABLE users (name VARCHAR(32), age INT)`처럼 만들면,
실제로는 내부적으로 `id BIGINT` 시스템 컬럼이 앞에 추가된다.

- 자동 증가
- 자동 인덱싱
- 수정 금지

즉 현재 MiniDB는 `id`를 기본 키처럼 사용하지만, 아직 SQL 문법의 `PRIMARY KEY` 옵션을 일반화한 상태는 아니다.

### 3. 인덱스 + 스캔 실행 경로 분리

- `WHERE id = ...`는 B+ Tree 인덱스 사용
- 그 외 조건은 heap 전체 스캔
- `ORDER BY LIMIT k`는 top-k 최적화 경로 사용
- `COUNT(*)`는 조건이 없을 때 fast path 사용

### 4. 서버 모드 포함

엔진만 있는 것이 아니라 바로 HTTP 서버로 띄울 수 있다.

- `POST /query`
- `GET /stats`
- keep-alive
- `thread-per-connection`
- graceful shutdown

### 5. 동시성 계층 분리

한 종류의 락으로 전부 해결하지 않고 역할을 나눠서 설계했다.

- row 의미 보호
- 페이지 메모리 보호
- 캐시 관리 정보 보호
- 작은 전역 카운터 보호

---

## 저장 구조 개요

MiniDB의 디스크 파일은 "페이지 배열"로 구성된다.

```text
page 0   -> header page
page 1   -> first heap page
page 2   -> root index page
page 3   -> heap / leaf / internal / free ...
page 4   -> ...
```

각 페이지는 `page_type`으로 구분된다.

---

## 페이지 레이아웃

### header page

header page는 DB 전체 상태를 담는다.

- magic
- version
- page_size
- root_index_page_id
- first_heap_page_id
- next_page_id
- free_page_head
- next_id
- row_count
- column_count
- row_size
- columns metadata

```text
오프셋 0                                                오프셋 4096
┌───────────────────────────────────────────────────────────────────┐
│ magic / version / page_size / root / heap / next_page / free     │
│ next_id / row_count / column_count / row_size / columns[]         │
│                                                                   │
│                      남는 공간은 예약 영역                        │
└───────────────────────────────────────────────────────────────────┘
```

### heap page

heap page는 실제 row가 들어가는 페이지다.  
슬롯 디렉터리는 앞에서 뒤로 자라고, row 데이터는 뒤에서 앞으로 자란다.

```text
오프셋 0                                                        오프셋 4096
┌────────┬────────┬────────┬────────┬────────────────┬──────┬──────┬──────┐
│ 헤더    │ slot 0 │ slot 1 │ slot 2 │    빈 공간       │ row2 │ row1 │ row0 │
│  16B   │   8B   │   8B   │   8B   │                │      │      │      │
└────────┴────────┴────────┴────────┴────────────────┴──────┴──────┴──────┘
|←──── 슬롯 디렉터리 증가 방향 ────→|             |←── row 데이터 증가 방향 ──→|
```

이 구조 덕분에:

- row를 가변 위치에 넣을 수 있고
- slot 상태만 바꿔 tombstone/free 재사용이 가능하고
- page 내부 공간을 비교적 단순하게 관리할 수 있다

### leaf page

leaf page는 `key -> row_ref` 매핑을 저장한다.

```text
오프셋 0                                                     오프셋 4096
┌────────────┬──────────┬──────────┬──────────┬──────────────────────┐
│  리프 헤더   │ entry 0  │ entry 1  │ entry 2  │      빈 공간           │
│             │ (id,ref) │ (id,ref) │ (id,ref) │                      │
└────────────┴──────────┴──────────┴──────────┴──────────────────────┘
```

### internal page

internal page는 자식 페이지로 내려가는 길만 저장한다.

```text
오프셋 0                                                     오프셋 4096
┌──────────────┬──────────┬──────────┬──────────┬────────────────────┐
│ 내부노드 헤더   │ entry 0  │ entry 1  │ entry 2  │     빈 공간          │
│              │ key+pid  │ key+pid  │ key+pid  │                    │
└──────────────┴──────────┴──────────┴──────────┴────────────────────┘
```

### free page

free page는 삭제된 페이지를 다시 쓰기 위한 연결 리스트 노드다.

```text
오프셋 0              오프셋 8                          오프셋 4096
┌─────────────────────┬────────────────────────────────────────────┐
│ page_type           │                                            │
│ next_free_page      │                 나머지 0                    │
└─────────────────────┴────────────────────────────────────────────┘
```

---

## SQL 실행 경로

MiniDB는 조건에 따라 서로 다른 실행 경로를 탄다.

### `SELECT * FROM users WHERE id = 10`

```text
SQL
  │
  ▼
parser
  │
  ▼
planner -> INDEX_LOOKUP
  │
  ▼
B+ Tree root
  │
  ▼
internal page ...
  │
  ▼
leaf page -> row_ref
  │
  ▼
heap page -> 실제 row 읽기
```

### `SELECT * FROM users WHERE age > 20`

```text
SQL
  │
  ▼
parser
  │
  ▼
planner -> TABLE_SCAN
  │
  ▼
first_heap_page_id부터 전체 heap chain 순회
  │
  ▼
각 row를 역직렬화 후 조건 검사
```

### `INSERT`

```text
1. header_lock으로 next_id 확보
2. 새 row 직렬화
3. heap_insert()
4. bptree_insert(id -> row_ref)
5. row_count 증가
```

### `UPDATE` / `DELETE`

- `WHERE id = ...`면 인덱스 기반 단건 경로
- 그 외 조건이면 scan 기반 다건 경로
- scan 기반 DML은 대상 `id`를 먼저 수집한 뒤 row lock과 재검증을 거친다

---

## 동시성 제어

MiniDB는 한 종류의 락으로 모든 문제를 풀지 않는다.  
역할이 다르면 락도 다르게 둔다.

```text
┌─────────────────────────────────────────────┐
│  Row / Range Lock                           │  같은 row 의미 보호
├─────────────────────────────────────────────┤
│  Engine RWLock                              │  DDL은 혼자, DML은 같이
├─────────────────────────────────────────────┤
│  Page Latch                                 │  페이지 메모리 보호
├─────────────────────────────────────────────┤
│  Pager Mutex                                │  frame 관리 정보 보호
├─────────────────────────────────────────────┤
│  Header Lock                                │  next_id, row_count 보호
└─────────────────────────────────────────────┘
```

### Row / Range Lock

논리 락이다.

- `WHERE id = X`
- `WHERE id > X`, `id <= X`
- statement 시작 시 잡고 끝에서 푼다
- 3초 timeout

이 락은 "같은 row의 의미가 중간에 바뀌지 않게" 하는 역할이다.

### Engine RWLock

엔진 전체 입장 관리용 락이다.

- `CREATE TABLE`, `DROP TABLE`은 `wrlock`
- DML은 `rdlock`

즉 현재 구조에서 engine lock은 "모든 write를 한 줄로 세우는 락"이 아니라,
"DDL은 혼자 들어가고 DML은 같이 들어가게 하는 락"에 가깝다.

### Page Latch

페이지 메모리 자체를 보호한다.

- heap fetch/scan: `rlatch`
- insert/update/delete: `wlatch`
- B+ Tree: latch coupling + crab protocol

이 락은 "메모리가 깨지지 않게" 하는 락이다.

### Pager Mutex

frame 관리 정보만 보호한다.

- `find_frame`
- `pin_count`
- `dirty`
- `used_tick`
- frame hash index

### Header Lock

작은 전역값을 보호한다.

- `next_id`
- `row_count`
- `last_heap_page_id`

---

## 서버 구조

현재 서버는 `thread-per-connection` 모델이다.

```text
accept()
  │
  ▼
┌───────────────────────────────┐
│         Main Thread           │
│   연결 수 확인 -> 스레드 생성     │
└───────┬─────────┬─────────┬────┘
        │         │         │
        ▼         ▼         ▼
   conn thread conn thread conn thread
        │         │         │
        └──────► handle_client()
                     │
                     ▼
                 db_execute()
```

### 왜 thread-per-connection인가

초기에는 thread pool을 썼지만, keep-alive 연결이 worker를 오래 점유하면서
Head-of-Line Blocking이 발생했다.

즉:

- worker 4개
- keep-alive 연결 16개
- 앞의 4개 연결이 worker를 붙잡음
- 뒤 요청은 큐에서 대기

이 문제를 해결하기 위해 현재는:

- 연결마다 전용 스레드 생성
- `pthread_detach`
- `MAX_CONNECTIONS=128`
- graceful shutdown

구조를 사용한다.

### HTTP 엔드포인트

- `POST /query`
  - SQL 실행
- `GET /stats`
  - 서버 / pager / row lock / DB 통계 반환

### keep-alive

현재 keep-alive는 지원한다.  
성공 응답과 에러 응답 모두 keep-alive 헤더 정합성을 맞춘 상태다.

주의:

- `idle timeout`은 아직 구현되지 않았다.
- 발표나 문서에서는 "다음 서버 개선 포인트"로 보는 것이 정확하다.

---

## 지원 SQL

### DDL

| 명령어 | 예시 |
|------|------|
| `CREATE TABLE` | `CREATE TABLE users (name VARCHAR(32), age INT)` |
| `DROP TABLE` | `DROP TABLE users` |

### DML

| 명령어 | 예시 |
|------|------|
| `INSERT` | `INSERT INTO users VALUES ('alice', 25)` |
| `SELECT` | `SELECT * FROM users WHERE id = 1` |
| `COUNT(*)` | `SELECT COUNT(*) FROM users WHERE age > 20` |
| `UPDATE` | `UPDATE users SET age = 26 WHERE id = 1` |
| `DELETE` | `DELETE FROM users WHERE id = 2` |
| `EXPLAIN` | `EXPLAIN SELECT * FROM users WHERE id = 1` |

### WHERE 비교 연산자

- `=`
- `!=`
- `>`
- `<`
- `>=`
- `<=`

### 정렬 / 제한

```sql
SELECT * FROM users ORDER BY age DESC LIMIT 10;
SELECT * FROM users WHERE age >= 20 ORDER BY age ASC LIMIT 5;
```

### 현재 제약

- 단일 테이블만 지원
- `id`는 시스템 컬럼
- `id` 수정 불가
- 명시적 `PRIMARY KEY`, `UNIQUE`, `NOT NULL` 미지원
- `id` 외 2차 인덱스 미지원

---

## REPL 모드

```bash
make
./build/minidb mydb.db
```

예시:

```text
minidb> 'mydb.db' 연결됨 (page_size=4096)
minidb> CREATE TABLE users (name VARCHAR(32), age INT);
'users' 테이블 생성 완료 (row_size=44, columns=3)
minidb> INSERT INTO users VALUES ('alice', 25);
1행 삽입 완료 (id=1)
minidb> SELECT * FROM users;
id | name | age
----------+-----------+----------
1 | alice | 25
1행 조회 (TABLE_SCAN)
```

### REPL 메타 명령어

| 명령어 | 설명 |
|------|------|
| `.exit`, `.quit` | 종료 |
| `.btree` | B+ Tree 구조 출력 |
| `.pages` | 페이지 유형별 통계 |
| `.stats` | DB 통계 |
| `.log` | pager flush 로그 토글 |
| `.flush` | dirty 페이지 수동 flush |
| `.debug` | 쿼리 디버그 모드 토글 |

---

## 서버 모드

```bash
make
./build/minidb --server 8080 mydb.db
```

예시:

```text
minidb server: 'mydb.db' (page_size=4096)
[server] listening on port 8080 (thread-per-connection, max=128)
```

---

## API 호출 예시

### 테이블 생성

```bash
curl -X POST http://localhost:8080/query \
  -d "CREATE TABLE users (name VARCHAR(32), age INT)"
```

### 데이터 삽입

```bash
curl -X POST http://localhost:8080/query \
  -d "INSERT INTO users VALUES ('alice', 25)"

curl -X POST http://localhost:8080/query \
  -d "INSERT INTO users VALUES ('bob', 30)"
```

### 조회

```bash
curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users"

curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users WHERE id = 1"

curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users WHERE age >= 25 ORDER BY age DESC LIMIT 2"
```

### 수정 / 삭제

```bash
curl -X POST http://localhost:8080/query \
  -d "UPDATE users SET age = 26 WHERE id = 1"

curl -X POST http://localhost:8080/query \
  -d "DELETE FROM users WHERE id = 2"
```

### 실행 계획 확인

```bash
curl -X POST http://localhost:8080/query \
  -d "EXPLAIN SELECT * FROM users WHERE id = 1"
```

### 서버 통계

```bash
curl http://localhost:8080/stats
```

응답 필드 예시:

- server model
- active / total connections
- total processed
- row lock 통계
- pager frame 사용량
- row_count / next_id

---

## 빌드

### 요구사항

- GCC
- Make
- pthread

기본 빌드는 sanitizer가 켜져 있다.

- AddressSanitizer
- UndefinedBehaviorSanitizer

### 기본 빌드

```bash
make
```

생성물:

- `build/minidb`

### 정리

```bash
make clean
```

---

## 테스트

### 전체 테스트

```bash
make test-all
```

2026-04-22 기준 결과:

| 타깃 | 결과 |
|------|------|
| `test_all` | 76 / 76 |
| `test_step0` | 24 / 24 |
| `test_step1` | 72 / 72 |
| `test_step2` | 44 / 44 |

### 개별 테스트

```bash
make test
make test-step0
make test-step1
make test-step2
```

### 별도 빌드 디렉터리 권장

```bash
make -j4 BUILD_DIR=build_mac test-all
```

### 이 테스트들이 보는 것

- pager / schema / heap / B+ Tree 회귀
- `db_execute()` 경계 함수
- SQL 확장 기능
- `id` 수정 금지
- row lock timeout
- gap conflict
- scan DELETE lock conflict
- keep-alive 에러 응답 헤더 정합성

---

## 도구

`tools/`에는 개발과 검증을 위한 보조 프로그램이 들어 있다.

### `tools/bench.c`

간단한 TPS 측정용 벤치마크 도구.

```bash
make bench
./build/bench 127.0.0.1 8080 4 100
```

### `tools/stress.c`

혼합 부하 + 실시간 모니터링용 스트레스 테스트 도구.

```bash
make stress
./build/stress 127.0.0.1 8080 8 10000 0
```

### `tools/gen_data.c`

대량 테스트 데이터를 생성하는 도구.

```bash
make gen N=100000
```

### `tools/deadlock_diag.c`

네트워크 없이 `db_execute()`를 직접 호출해 hang / deadlock 성격 문제를 진단하는 멀티스레드 테스트 도구.

이 도구는:

- 여러 스레드가 INSERT / SELECT / UPDATE / DELETE를 섞어 수행하고
- 일정 시간 안에 스레드가 끝나지 않으면
- pinned frame 상태를 덤프해 디버깅에 도움을 준다

---

## 저장소 구조

```text
.
├── include/
│   ├── db.h
│   ├── server/
│   │   ├── http.h
│   │   ├── lock_table.h
│   │   └── server.h
│   ├── sql/
│   │   ├── executor.h
│   │   ├── parser.h
│   │   ├── planner.h
│   │   └── statement.h
│   └── storage/
│       ├── bptree.h
│       ├── page_format.h
│       ├── pager.h
│       ├── schema.h
│       └── table.h
├── src/
│   ├── db.c
│   ├── main.c
│   ├── server/
│   │   ├── http.c
│   │   ├── lock_table.c
│   │   └── server.c
│   ├── sql/
│   │   ├── executor.c
│   │   ├── parser.c
│   │   └── planner.c
│   └── storage/
│       ├── bptree.c
│       ├── pager.c
│       ├── schema.c
│       └── table.c
├── tests/
│   ├── test_all.c
│   ├── test_step0_db_execute.c
│   ├── test_step1_sql_ext.c
│   └── test_step2_concurrency.c
├── tools/
│   ├── bench.c
│   ├── deadlock_diag.c
│   ├── gen_data.c
│   └── stress.c
└── docs/sql/
    ├── README.md
    ├── 11-implementation-plan.md
    ├── 12-test-harness-plan.md
    ├── 13-concurrency-issues.md
    ├── 14-code-audit.md
    └── 15-presentation-deck.md
```

---

## 문서

`docs/sql/`에는 현재 코드 상태를 설명하는 문서가 정리되어 있다.

- `docs/sql/README.md`
  - 문서 모음 개요
- `docs/sql/11-implementation-plan.md`
  - 현재 구현 현황과 다음 백로그
- `docs/sql/12-test-harness-plan.md`
  - 테스트 스위트 구성과 운영 가이드
- `docs/sql/13-concurrency-issues.md`
  - 동시성 문제와 해결 기록
- `docs/sql/14-code-audit.md`
  - 코드 감사 결과와 남은 리스크
- `docs/sql/15-presentation-deck.md`
  - 발표자료 및 발표 원고

---

## 현재 한계

현재 MiniDB는 꽤 많은 부분이 구현되어 있지만, 아직 아래 제약이 있다.

- 단일 테이블만 지원
- `id` 외 2차 인덱스 없음
- 명시적 PK / UNIQUE / NOT NULL 미지원
- `id`가 아닌 조건에 대한 더 강한 조건 기반 락 미지원
- writer starvation 가능성 존재
- idle timeout 미구현
- WAL / recovery / MVCC는 범위 밖

즉 현재 상태는 "교육용 MiniDB를 넘어 실제 서버/동시성 문제까지 다뤄본 구현"이라고 보는 편이 가장 정확하다.

---

## 요약

MiniDB는 다음을 한 저장소 안에서 보여준다.

- 디스크 기반 페이지 저장
- slotted heap
- B+ Tree 인덱스
- pager 캐시
- SQL 실행 경로 분기
- HTTP API 서버
- thread-per-connection
- 다층 동시성 제어
- sanitizer + 테스트 + 스트레스 도구

단순 CRUD를 넘어서, 실제로 요청이 들어오고 경쟁이 생기고 병목과 race를 발견하고 고치는 과정까지 포함한 작은 DBMS 프로젝트다.
