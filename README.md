# MiniDB — Mini DBMS API Server

C로 구현한 디스크 기반 관계형 데이터베이스 엔진 + HTTP API 서버.

B+ Tree 인덱스, LRU 페이지 캐시, 3-레이어 동시성 제어(Row Lock → Engine RWLock → Pager Mutex)를 갖추고 있다.

---

## 빌드

### 사전 요구사항

- GCC (C11 이상)
- Make
- pthread 라이브러리

Docker 개발 환경을 사용하는 경우 `.devcontainer/`가 포함되어 있으므로 VSCode에서 **Dev Containers: Reopen in Container**를 실행하면 된다.

### 빌드 명령어

```bash
make          # 빌드 → build/minidb 생성
make clean    # 빌드 산출물 + DB 파일 전체 삭제
```

---

## 실행 방법

### 1. REPL 모드 (대화형)

```bash
./build/minidb mydb.db
```

파일이 없으면 새 DB를 생성하고, 있으면 기존 DB를 연다.

```
minidb> 'mydb.db' 연결됨 (page_size=4096)
minidb> CREATE TABLE users (name VARCHAR(32), age INT);
'users' 테이블 생성 완료 (row_size=44, columns=3)
minidb> INSERT INTO users VALUES ('alice', 25);
1행 삽입 완료 (id=1)
minidb> SELECT * FROM users;
id | name | age
----------+-----------+----------
1 | alice | 25
1행 조회 (INDEX_LOOKUP)
minidb> .exit
종료합니다.
```

### 2. HTTP API 서버 모드

```bash
./build/minidb --server 8080 mydb.db
```

기본 포트는 8080이며, 포트 번호를 생략하면 8080을 사용한다.

```
minidb server: 'mydb.db' (page_size=4096)
[thread_pool] 4 workers started (queue_cap=64)
[server] listening on port 8080
```

서버는 `Ctrl+C`(SIGINT)로 종료하며, 종료 시 모든 dirty 페이지를 디스크에 flush한다.

---

## API 호출 방법

서버 모드 실행 후, 별도 터미널에서 아래와 같이 호출한다.

### curl (커맨드라인)

모든 SQL은 `POST /query`에 body로 전달한다.

```bash
# 테이블 생성
curl -X POST http://localhost:8080/query \
  -d "CREATE TABLE users (name VARCHAR(32), age INT)"

# 데이터 삽입
curl -X POST http://localhost:8080/query \
  -d "INSERT INTO users VALUES ('alice', 25)"

curl -X POST http://localhost:8080/query \
  -d "INSERT INTO users VALUES ('bob', 30)"

curl -X POST http://localhost:8080/query \
  -d "INSERT INTO users VALUES ('charlie', 22)"

# 전체 조회
curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users"

# id로 단건 조회
curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users WHERE id = 1"

# 조건 조회
curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users WHERE age > 24"

# 정렬 + 제한
curl -X POST http://localhost:8080/query \
  -d "SELECT * FROM users ORDER BY age DESC LIMIT 2"

# 건수 조회
curl -X POST http://localhost:8080/query \
  -d "SELECT COUNT(*) FROM users"

# 수정
curl -X POST http://localhost:8080/query \
  -d "UPDATE users SET age = 26 WHERE id = 1"

# 삭제
curl -X POST http://localhost:8080/query \
  -d "DELETE FROM users WHERE id = 2"

# 실행 계획 확인
curl -X POST http://localhost:8080/query \
  -d "EXPLAIN SELECT * FROM users WHERE id = 1"

# 테이블 삭제
curl -X POST http://localhost:8080/query \
  -d "DROP TABLE users"
```

### Postman

1. 새 요청 생성
2. 메서드: **POST**
3. URL: `http://localhost:8080/query`
4. Body 탭 → **raw** 선택 → 타입을 **Text**로 변경
5. 본문에 SQL 입력:
   ```
   SELECT * FROM users WHERE age >= 25
   ```
6. **Send** 클릭

응답 예시:
```
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8

id | name | age
----------+-----------+----------
1 | alice | 25
2 | bob | 30
2행 조회 (TABLE_SCAN)
```

### HTTPie

```bash
http POST http://localhost:8080/query <<< "SELECT * FROM users"
```

### VS Code REST Client (`.http` 파일)

```http
POST http://localhost:8080/query
Content-Type: text/plain

SELECT * FROM users ORDER BY age DESC
```

---

## 지원 SQL 명령어

### DDL (데이터 정의)

| 명령어 | 문법 | 설명 |
|--------|------|------|
| CREATE TABLE | `CREATE TABLE t (col1 TYPE, col2 TYPE)` | 테이블 생성. `id BIGINT` 자동 추가 |
| DROP TABLE | `DROP TABLE t` | 테이블 삭제 + 페이지 반환 |

지원 타입: `INT`, `BIGINT`, `VARCHAR(n)`

### DML (데이터 조작)

| 명령어 | 문법 | 설명 |
|--------|------|------|
| INSERT | `INSERT INTO t VALUES (v1, v2, ...)` | 행 삽입. id는 자동 증가 |
| SELECT | `SELECT * FROM t [WHERE ...] [ORDER BY col [ASC\|DESC]] [LIMIT n]` | 조회 |
| SELECT COUNT | `SELECT COUNT(*) FROM t [WHERE ...]` | 건수 조회 |
| UPDATE | `UPDATE t SET col = val WHERE ...` | 행 수정 |
| DELETE | `DELETE FROM t WHERE ...` | 행 삭제 |
| EXPLAIN | `EXPLAIN SELECT/UPDATE/DELETE ...` | 실행 계획만 출력 (실제 실행 안 함) |

### WHERE 절 비교 연산자

`=`, `!=`, `>`, `<`, `>=`, `<=`

```sql
SELECT * FROM users WHERE age >= 25
DELETE FROM users WHERE name != 'alice'
UPDATE users SET age = 99 WHERE id = 1
```

### REPL 전용 메타 명령어

| 명령어 | 설명 |
|--------|------|
| `.exit` / `.quit` | 프로그램 종료 |
| `.btree` | B+ Tree 내부 구조 출력 |
| `.pages` | 페이지 유형별 통계 |
| `.stats` | DB 통계 (행 수, 페이지 크기, 트리 높이 등) |
| `.log` | pager flush 로그 ON/OFF 토글 |
| `.flush` | dirty 페이지를 수동으로 디스크에 기록 |
| `.debug` | 쿼리 디버그 모드 ON/OFF (페이지 로드/히트/미스/소요시간 출력) |

---

## 테스트

### 전체 테스트 실행

```bash
make test-all
```

총 **178 assertions**이 4개 테스트 스위트로 실행된다:

| 타겟 | 파일 | 내용 | assertions |
|------|------|------|-----------|
| `make test` | `tests/test_all.c` | wk07 회귀 (pager, heap, bptree, parser, planner, persistence) | 73 |
| `make test-step0` | `tests/test_step0_db_execute.c` | db_execute 경계 함수 (CRUD, EXPLAIN, persistence) | 24 |
| `make test-step1` | `tests/test_step1_sql_ext.c` | SQL 확장 (UPDATE, 비교연산자, COUNT, ORDER BY, LIMIT, DROP) | 63 |
| `make test-step2` | `tests/test_step2_concurrency.c` | 동시성 제어 (lock S/X, timeout, 멀티스레드 INSERT) | 18 |

### 개별 테스트 실행

```bash
make test        # wk07 회귀만
make test-step0  # db_execute만
make test-step1  # SQL 확장만
make test-step2  # 동시성만
```

### 동시성 테스트가 검증하는 항목

- **S-S 호환**: 같은 row에 S lock 2개 동시 획득 가능
- **S-X 비호환**: S lock이 해제된 후에만 X lock 획득
- **X-X timeout**: 3초 내 획득 실패 시 -1 반환
- **다른 row 독립**: row 400에 X lock + row 401에 X lock 동시 가능
- **멀티스레드 INSERT**: 4스레드 × 25건 = 100건 동시 삽입 → COUNT == 100
- **동시 SELECT + UPDATE**: 다른 row에 대한 읽기/쓰기 동시 실행

### 벤치마크 (TPS 측정)

```bash
# 터미널 1: 서버 시작
./build/minidb --server 8080 bench.db

# 터미널 2: 벤치마크 실행
make bench                                       # 기본: 4스레드 × 100건
./build/bench 127.0.0.1 8080 4 1000             # 커스텀: 4스레드 × 1000건
./build/bench 127.0.0.1 8080 8 500              # 8스레드 × 500건
```

출력 예시:
```
=== MiniDB Benchmark ===
Target: 127.0.0.1:8080
Threads: 4, Requests/thread: 250, Total: 1000

=== Results ===
Elapsed: 0.03 sec
Success: 1000, Fail: 0
TPS: 39894.0 transactions/sec
Rows in DB: 1000
```

---

## 아키텍처

### 프로젝트 구조

```
SW_AI-W08-SQL/
├── include/
│   ├── storage/          # 저장소 계층 (pager, page_format, bptree, table, schema)
│   ├── sql/              # SQL 계층 (parser, planner, executor, statement)
│   ├── server/           # 서버 계층 (http, server, thread_pool, lock_table)
│   └── db.h              # db_execute() 경계 함수
├── src/
│   ├── storage/          # pager.c, bptree.c, table.c, schema.c
│   ├── sql/              # parser.c, planner.c, executor.c
│   ├── server/           # http.c, server.c, thread_pool.c, lock_table.c
│   ├── db.c              # 3-레이어 동시성 제어 통합
│   └── main.c            # REPL + --server 모드 분기
├── tests/                # 4개 테스트 스위트 (178 assertions)
├── tools/                # gen_data.c (대량 데이터 생성), bench.c (TPS 벤치마크)
├── .devcontainer/        # Docker 개발 환경 (Ubuntu 22.04)
├── .vscode/              # GDB 디버그 + 빌드 태스크
└── Makefile
```

### 3-레이어 동시성 제어

```
요청 → db_execute()
         │
         ├─ L3  Row Lock        id 단건 연산에 S/X lock (3초 timeout)
         │
         ├─ L2  Engine RWLock   SELECT=공유, 쓰기=독점
         │
         └─ L1  Pager Mutex     프레임 메타데이터 보호 (recursive)
```

### SQL 실행 흐름

```
curl -d "SELECT * FROM users WHERE id = 3"
  │
  ▼
HTTP Parser (POST /query → body 추출)
  │
  ▼
db_execute(pager, sql)
  ├─ parse(sql) → statement_t
  ├─ lock_acquire(S, row_id=3)     ← L3 Row Lock
  ├─ pthread_rwlock_rdlock()        ← L2 Engine RWLock
  ├─ planner → ACCESS_PATH_INDEX_LOOKUP
  ├─ bptree_search(key=3) → ref    ← B+ Tree O(log n)
  ├─ heap_fetch(ref) → row_data    ← Pager L1 Mutex 내부 보호
  ├─ pthread_rwlock_unlock()
  └─ lock_release_all()            ← Strict 2PL 해제
  │
  ▼
HTTP 200 OK + 결과 텍스트
```
