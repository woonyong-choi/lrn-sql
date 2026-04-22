# 11. Mini DBMS API Server 구현 계획서

> **목표**: wk07 디스크 기반 SQL 엔진(minidb)을 확장하여, TCP 소켓 기반 API 서버 + 스레드 풀 + Level 3 동시성 제어를 갖춘 Mini DBMS를 구현한다.
>
> **발표**: 목요일 오전 (4분 발표 + 1분 Q&A)
>
> **제외 항목**: MVCC, WAL (본문에서 stretch goal로만 언급)

---

## 0단계 — 프로젝트 부트스트랩

### 0-1. wk07 코드 복사 및 빌드 확인

wk07_6 저장소의 핵심 소스를 W08 프로젝트로 복사한다.

```
W08-SQL/
├── include/
│   ├── storage/   ← pager.h, page_format.h, bptree.h, heap.h
│   └── sql/       ← parser.h, executor.h, planner.h, statement.h
├── src/
│   ├── storage/   ← pager.c, bptree.c, heap.c
│   ├── sql/       ← parser.c, executor.c, planner.c
│   └── main.c     ← REPL (유지, 디버그용)
├── Makefile
└── docs/sql/      ← 설계 문서
```

- `make` 로 기존 REPL 빌드 + 기본 동작 확인
- sanitizer 옵션 유지 (`-fsanitize=address,undefined`)

### 0-2. `db_execute()` 경계 함수 도입

현재 `executor.c` 의 `execute()` 는 결과를 `printf()` 로 직접 출력한다.
서버 모드에서는 결과를 버퍼에 담아 클라이언트에 전송해야 하므로, 출력 경로를 분리한다.

**수정 전 (현재):**
```c
exec_result_t execute(pager_t *pager, statement_t *stmt);
// 내부에서 printf("(1, 'Alice', 25)\n") 직접 호출
```

**수정 후:**
```c
// executor.h
typedef struct {
    int status;          // 0=success, -1=error
    char message[512];   // 상태 메시지
    char *out_buf;       // SELECT 결과 등 본문 (동적 할당)
    size_t out_len;      // out_buf 에 쓰인 바이트 수
} exec_result_t;

exec_result_t execute(pager_t *pager, statement_t *stmt);
```

- SELECT 결과를 `out_buf` 에 `snprintf` 로 축적
- `main.c` REPL 에서는 `printf("%s", res.out_buf)` 로 출력
- 서버에서는 `send(client_fd, res.out_buf, res.out_len, 0)` 로 전송
- 호출자가 `free(res.out_buf)` 책임

### 0-3. `db_execute()` 래퍼 작성

서버와 REPL 양쪽에서 호출하는 단일 진입점:

```c
// db.h
exec_result_t db_execute(pager_t *pager, const char *sql);
```

내부 흐름: `parse(sql, &stmt)` → `execute(pager, &stmt)` → 결과 반환

---

## 1단계 — SQL 확장 (P0 + P1)

### P0: 핵심 확장

| 기능 | 설명 | 난이도 |
|------|------|--------|
| **UPDATE** | `UPDATE t SET col=val WHERE ...` | 중 |
| **비교 연산자** | `>`, `<`, `>=`, `<=`, `!=` (WHERE 절) | 중 |

**UPDATE 구현 전략:**
1. parser.c: `STMT_UPDATE` 타입 추가, `SET col=val` 파싱
2. executor.c: WHERE 조건으로 대상 행 탐색 → heap에서 기존 row 읽기 → 필드 수정 → heap에 재기록
3. 인덱스 키(id) 변경 시 B+Tree 업데이트 (id 변경은 미지원으로 단순화 가능)

**비교 연산자 구현 전략:**
1. statement.h: `pred_op` 필드 추가 (`OP_EQ`, `OP_NE`, `OP_LT`, `OP_GT`, `OP_LE`, `OP_GE`)
2. parser.c: `=` 외 연산자 토큰 인식
3. executor.c: `match_predicate()` 함수에 연산자별 비교 로직 추가
4. 비교 연산자는 인덱스 사용 불가 → 항상 table scan (현재와 동일)

### P1: 편의 확장

| 기능 | 설명 | 난이도 |
|------|------|--------|
| **COUNT(*)** | `SELECT COUNT(*) FROM t [WHERE ...]` | 하 |
| **ORDER BY** | `SELECT ... ORDER BY col [ASC\|DESC]` | 중 |
| **LIMIT** | `SELECT ... LIMIT n` | 하 |
| **DROP TABLE** | `DROP TABLE t` (전체 페이지 free) | 중 |

**COUNT(*) 구현:**
- 결과 행을 세는 카운터만 반환, row 내용은 출력하지 않음

**ORDER BY 구현:**
- SELECT 결과를 임시 배열에 수집 → `qsort()` 로 정렬 → 출력
- 메모리 내 정렬 (외부 정렬은 미구현)

**LIMIT 구현:**
- 출력 카운터가 N 에 도달하면 조기 종료

**DROP TABLE 구현:**
- 모든 heap 페이지를 free list 로 반환
- B+Tree 페이지도 free list 로 반환
- header의 스키마 초기화

---

## 2단계 — TCP 소켓 서버 + 스레드 풀

### 2-1. 서버 아키텍처

```
┌─────────────────────────────────────────────────┐
│                  Main Thread                    │
│  socket() → bind() → listen() → accept() loop   │
│                     │                           │
│         ┌───────────┴───────────┐               │
│         ▼                       ▼               │
│   ┌──────────┐           ┌──────────┐           │
│   │ Worker 1 │           │ Worker N │           │
│   │ (pthread)│           │ (pthread)│           │
│   └────┬─────┘           └────┬─────┘           │
│        │ recv → db_execute → send               │
│        ▼                       ▼                │
│   ┌─────────────────────────────────┐           │
│   │         pager_t (공유)           │           │
│   │   Level 1~3 동시성 제어 적용        │           │
│   └─────────────────────────────────┘           │
└─────────────────────────────────────────────────┘
```

### 2-2. 프로토콜 (HTTP/1.1 최소 서브셋)

클라이언트 요청:
```http
POST /query HTTP/1.1
Content-Length: 42

SELECT * FROM users WHERE age > 20
```

서버 응답:
```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 55

(1, 'Alice', 25)
(3, 'Charlie', 30)
OK: 2 rows
```

에러 응답:
```http
HTTP/1.1 400 Bad Request
Content-Type: text/plain
Content-Length: 30

오류: SQL 구문을 해석할 수 없습니다
```

- `curl` 로 테스트 가능: `curl -X POST -d "SELECT * FROM users" http://localhost:8080/query`

### 2-3. 스레드 풀 구현

```c
// thread_pool.h
typedef struct {
    int client_fd;
} job_t;

typedef struct {
    pthread_t *threads;     // worker 스레드 배열
    int thread_count;       // worker 수 (기본 4)
    job_t *queue;           // bounded 원형 큐
    int queue_cap;          // 큐 용량 (기본 64)
    int queue_size;         // 현재 큐에 대기 중인 job 수
    int head, tail;         // 큐 인덱스
    pthread_mutex_t mutex;  // 큐 접근 보호
    pthread_cond_t not_empty; // worker가 대기하는 조건
    pthread_cond_t not_full;  // main이 대기하는 조건
    bool shutdown;          // 종료 플래그
    pager_t *pager;         // 공유 DB (모든 worker가 동일 pager 사용)
} thread_pool_t;
```

**Worker 루프:**
```c
void *worker_func(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    while (true) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->queue_size == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        if (pool->shutdown) { unlock; return; }
        job_t job = dequeue(pool);
        pthread_mutex_unlock(&pool->mutex);

        handle_client(pool->pager, job.client_fd);
        close(job.client_fd);
    }
}
```

### 2-4. 구현 순서

1. `server.c`: socket/bind/listen/accept 기본 루프 (단일 스레드 먼저)
2. `http.c`: HTTP 요청 파싱 (POST /query 만), 응답 포매팅
3. `thread_pool.c`: 스레드 풀 + job 큐
4. 단일 스레드 서버 → 스레드 풀 서버로 전환
5. `main.c` 에 `--server` 플래그 추가 (REPL / 서버 모드 선택)

---

## 3단계 — 동시성 제어 (Level 1 → 2 → 3)

### Level 1: Global Mutex (모든 쿼리 직렬화)

```c
// db.c
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

exec_result_t db_execute(pager_t *pager, const char *sql) {
    pthread_mutex_lock(&db_mutex);
    // parse → execute
    pthread_mutex_unlock(&db_mutex);
    return result;
}
```

- 가장 단순, 정확성 보장
- 동시 요청이 들어와도 한 번에 하나만 실행
- **이 단계에서 서버 전체 통합 테스트 통과시키기**

### Level 2: Engine RWLock + Pager Mutex

```c
// db.c
static pthread_rwlock_t engine_lock = PTHREAD_RWLOCK_INITIALIZER;

exec_result_t db_execute(pager_t *pager, const char *sql) {
    parse(sql, &stmt);
    if (stmt.type == STMT_SELECT) {
        pthread_rwlock_rdlock(&engine_lock);   // 읽기: 공유
    } else {
        pthread_rwlock_wrlock(&engine_lock);   // 쓰기: 독점
    }
    exec_result_t res = execute(pager, &stmt);
    pthread_rwlock_unlock(&engine_lock);
    return res;
}
```

```c
// pager.c (내부)
static pthread_mutex_t pager_mutex = PTHREAD_MUTEX_INITIALIZER;

uint8_t *pager_get_page(pager_t *p, uint32_t page_id) {
    pthread_mutex_lock(&pager_mutex);
    // frame 탐색, LRU eviction, pin_count++
    pthread_mutex_unlock(&pager_mutex);
    return frame->data;   // data 자체는 engine_lock이 보호
}
```

**이 단계의 효과:**
- SELECT 끼리 동시 실행 가능
- INSERT/UPDATE/DELETE 는 독점 (스플릿 포함 안전)
- Pager 내부 메타데이터(pin_count, used_tick)는 별도 mutex로 보호

### Level 3: Row Lock (Strict 2PL + Timeout)

```c
// lock_table.h
typedef enum { LOCK_S, LOCK_X } lock_mode_t;

typedef struct lock_entry {
    uint64_t row_id;
    lock_mode_t mode;
    pthread_t owner;
    struct lock_entry *next;   // 같은 row의 다음 lock
} lock_entry_t;

typedef struct {
    lock_entry_t *buckets[256];  // hash table
    pthread_mutex_t mutex;       // lock table 자체 보호
} lock_table_t;

// API
int  lock_acquire(lock_table_t *lt, uint64_t row_id, lock_mode_t mode);
                  // 반환: 0=성공, -1=timeout(deadlock 방지)
void lock_release_all(lock_table_t *lt, pthread_t owner);
```

**Lock 호환성 행렬:**

|         | S (읽기) | X (쓰기) |
|---------|----------|----------|
| S (읽기) | O 호환  | X 대기  |
| X (쓰기) | X 대기  | X 대기  |

**Strict 2PL 규칙:**
- 트랜잭션 중 필요한 lock을 순차적으로 획득
- **모든 lock은 트랜잭션 종료(commit/abort) 시 한꺼번에 해제**
- 중간에 lock을 놓지 않음 → cascading abort 방지

**Deadlock 방지 (Timeout 방식):**
```c
int lock_acquire(lock_table_t *lt, uint64_t row_id, lock_mode_t mode) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;  // 3초 타임아웃

    pthread_mutex_lock(&lt->mutex);
    while (conflict_exists(lt, row_id, mode)) {
        int rc = pthread_cond_timedwait(&lt->cond, &lt->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&lt->mutex);
            return -1;  // timeout → 호출자가 abort 처리
        }
    }
    add_lock(lt, row_id, mode);
    pthread_mutex_unlock(&lt->mutex);
    return 0;
}
```

**Level 3 실행 흐름 (SELECT WHERE id = 5):**
```
1. Engine RWLock  → rdlock
2. B+Tree 탐색   → row_id=5 의 위치 확인
3. Row Lock      → lock_acquire(5, LOCK_S)  ← 3초 내 획득 못하면 abort
4. Heap 읽기     → row 데이터 반환
5. 결과 전송     → 클라이언트에 send
6. Row Lock      → lock_release_all(현재 스레드)
7. Engine RWLock  → unlock
```

**Level 3 실행 흐름 (UPDATE WHERE id = 5):**
```
1. Engine RWLock  → wrlock
2. B+Tree 탐색   → row_id=5 의 위치 확인
3. Row Lock      → lock_acquire(5, LOCK_X)
4. Heap 읽기     → 기존 row
5. 필드 수정     → heap 재기록
6. Engine RWLock  → unlock
7. (응답 전송 후) Row Lock → lock_release_all
```

> **Q: Level 3에서 스플릿이 필요하면?**
> Engine RWLock이 wrlock 상태이므로 다른 writer가 동시에 트리를 수정할 수 없다.
> 따라서 별도의 per-page latch 없이도 스플릿은 안전하다.
> per-page latch(Lock Coupling)는 여러 writer 동시 허용 시 필요한 Level 4+ 최적화이다.

---

## 4단계 — 테스트 및 데모 준비

### 4-1. 단위 테스트

| 테스트 | 검증 항목 |
|--------|----------|
| SQL 파서 | UPDATE, 비교 연산자 파싱 정확성 |
| executor | UPDATE 실행, COUNT/ORDER BY/LIMIT 결과 정확성 |
| lock_table | S/X 호환성, timeout 동작, release_all |
| thread_pool | worker 생성/종료, job 큐 동작 |

### 4-2. 동시성 스트레스 테스트

```bash
# 10개 동시 INSERT
for i in $(seq 1 10); do
  curl -s -X POST -d "INSERT INTO users VALUES ('user$i', $((20+i)))" \
    http://localhost:8080/query &
done
wait

# 결과 확인: 10행이 모두 존재하는지
curl -X POST -d "SELECT COUNT(*) FROM users" http://localhost:8080/query
# 기대: OK: count = 10
```

```bash
# 동시 읽기 + 쓰기
curl -X POST -d "SELECT * FROM users" http://localhost:8080/query &
curl -X POST -d "INSERT INTO users VALUES ('conflict', 99)" http://localhost:8080/query &
wait
```

### 4-3. 데모 시나리오 (4분)

1. **(30초)** 아키텍처 소개: wk07 REPL → wk08 API 서버 전환, 3계층 락 구조 다이어그램
2. **(60초)** 라이브 데모 1: CREATE TABLE → INSERT 여러 건 → SELECT 확인
3. **(60초)** 라이브 데모 2: 동시성 — 터미널 2개로 동시 INSERT, COUNT로 정합성 확인
4. **(30초)** 라이브 데모 3: UPDATE + WHERE 비교 연산자, ORDER BY + LIMIT
5. **(30초)** 동시성 제어 설명: Level 1→2→3 진화 과정, 왜 Row Lock까지 갔는지
6. **(30초)** 한계 및 개선 방향: MVCC, WAL, Lock Coupling 등

---

## 5단계 — Stretch Goals (시간 여유 시)

| 항목 | 설명 | 우선순위 |
|------|------|---------|
| WAL 스켈레톤 | append-only 로그 파일에 변경 기록, 실제 recovery는 미구현 | 낮음 |
| 벤치마크 | 동시 요청 수 대비 처리량(TPS) 측정 | 중간 |
| CLI 클라이언트 | readline 기반 `minidb-cli` (curl 대체) | 낮음 |
| GROUP BY | 단순 집계 (COUNT, SUM) + 그룹핑 | 낮음 |

---

## 파일 구조 (최종 예상)

```
W08-SQL/
├── include/
│   ├── storage/
│   │   ├── pager.h
│   │   ├── page_format.h
│   │   ├── bptree.h
│   │   └── heap.h
│   ├── sql/
│   │   ├── parser.h
│   │   ├── executor.h
│   │   ├── planner.h
│   │   └── statement.h
│   └── server/
│       ├── server.h          ← socket accept loop
│       ├── http.h            ← HTTP 파싱/포매팅
│       ├── thread_pool.h     ← 스레드 풀
│       └── lock_table.h      ← Row Lock (Level 3)
├── src/
│   ├── storage/              ← pager.c, bptree.c, heap.c
│   ├── sql/                  ← parser.c, executor.c, planner.c
│   ├── server/
│   │   ├── server.c
│   │   ├── http.c
│   │   ├── thread_pool.c
│   │   └── lock_table.c
│   ├── db.c                  ← db_execute() 경계 함수 + 동시성 제어
│   └── main.c                ← REPL + --server 모드 분기
├── Makefile
├── docs/sql/
└── test/
```

---

## 구현 순서 요약

```
  0단계           1단계              2단계              3단계           4단계
┌─────────┐   ┌───────────┐   ┌──────────────┐   ┌────────────┐   ┌──────────┐
│ 부트스트랩 │ → │ SQL 확장   │ → │ 소켓 서버      │ → │ 동시성 제어   │ → │ 테스트     │
│         │   │           │   │ + 스레드 풀    │   │ L1→L2→L3   │   │ + 데모    │
│ db_execute│ │ UPDATE    │   │              │   │            │   │          │
│ out_buf │   │ 비교연산자   │   │ POST /query  │   │ Global Mut │   │ 스트레스   │
│ 빌드확인  │   │ COUNT/SORT│   │ worker pool  │   │ → RWLock   │   │ 시나리오   │
│         │   │ LIMIT/DROP│   │              │   │ → Row Lock │   │ 발표준비   │
└─────────┘   └───────────┘   └──────────────┘   └────────────┘   └──────────┘
   ~2h           ~4h              ~4h              ~4h              ~2h
                                                               총 약 16시간
```

**핵심 원칙**: 각 단계가 끝날 때마다 빌드 + 동작 확인. Level 1부터 서버가 돌아가는 상태를 유지하고, Level 2→3으로 점진적으로 개선한다.
