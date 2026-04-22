# 10. 동시성 제어 — 기초부터 실제 DB까지

> 작성일: 2026-04-22
> 대상: MiniDB SQL API Server (W08)
> 선행 문서: `09-concurrency-design.md`

이 문서는 "멀티스레드 환경에서 DB 엔진이 왜 깨지는가, 어떻게 보호하는가"를 다룬다.
학습용 Level 1~3 구현을 먼저 설명하고, 실제 DB가 그 한계를 어떻게 넘어섰는지를 이어서 정리한다.

---

## 1. 동기화 도구 기초: Mutex, Semaphore, RWLock

### 1.1 Mutex — "화장실 열쇠가 1개"

한 번에 하나의 스레드만 진입할 수 있다.
잠근 스레드만 열 수 있다.

```c
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_lock(&mutex);     // 열쇠를 가져감
// 임계 구역: 한 스레드만 여기에 있음
pthread_mutex_unlock(&mutex);   // 열쇠를 돌려놓음
```

내부 상태는 단순하다.

```
mutex 내부:
  locked: true/false
  owner: 잠근 스레드
  wait_queue: 대기 중인 스레드 목록
```

lock을 시도했을 때 이미 잠겨 있으면 wait_queue에 들어가서 잠든다.
owner가 unlock하면 wait_queue에서 하나를 깨운다.

### 1.2 Semaphore — "주차장 남은 자리 N개"

동시에 N개 스레드까지 진입할 수 있다.
잠근 스레드가 아닌 다른 스레드도 signal(반납)할 수 있다.

```c
sem_t sem;
sem_init(&sem, 0, 3);  // 최대 3개 동시 진입

sem_wait(&sem);    // 카운터-- (0이면 대기)
// 임계 구역
sem_post(&sem);    // 카운터++ (아무 스레드나 가능)
```

내부 상태:

```
semaphore 내부:
  count: 남은 자리 수
  wait_queue: count가 0일 때 대기하는 스레드들
```

mutex와 다른 점: 소유권 개념이 없다. A가 wait해서 들어갔는데 B가 post해서 자리를 늘릴 수 있다.

MiniDB에서 semaphore가 자연스러운 곳은 스레드 풀의 job queue다.
"큐에 작업이 몇 개 있는지"를 세는 데 적합하다.
DB 엔진 보호에는 적합하지 않다. "카운팅"이 아니라 "읽기/쓰기 구분"이 필요하기 때문이다.

### 1.3 RWLock — "읽기 전용 입구와 수정 전용 입구가 따로 있는 방"

읽기 모드(rdlock)는 여러 스레드가 동시에 잡을 수 있다.
쓰기 모드(wrlock)는 단 하나만 잡을 수 있고, 모든 rdlock이 풀려야 잡힌다.

```c
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;

// 읽기
pthread_rwlock_rdlock(&rw);   // 읽기 모드, 여러 스레드 동시 OK
// 읽기 작업
pthread_rwlock_unlock(&rw);

// 쓰기
pthread_rwlock_wrlock(&rw);   // 쓰기 모드, 혼자만
// 쓰기 작업
pthread_rwlock_unlock(&rw);
```

내부 상태:

```
rwlock 내부:
  reader_count: 현재 읽기 중인 스레드 수
  writer_active: 쓰기 중인 스레드가 있는가 (true/false)
  read_wait_queue: 읽기 대기 스레드들
  write_wait_queue: 쓰기 대기 스레드들
```

진입 판단 로직:

```
rdlock 시도:
  writer_active == false → reader_count++ → 통과
  writer_active == true  → read_wait_queue에서 대기

wrlock 시도:
  writer_active == false && reader_count == 0 → writer_active = true → 통과
  그 외 → write_wait_queue에서 대기

unlock:
  reader였으면 → reader_count--, 0이 되면 write_wait_queue에서 하나 깨움
  writer였으면 → writer_active = false, 대기 중인 reader 전부 깨움 (또는 writer 하나)
```

rwlock은 내부적으로 mutex + 카운터 + 조건변수 2개를 조합해서 만든 것이다.
직접 구현하면 아래 모양이 된다:

```c
typedef struct {
    pthread_mutex_t guard;        // 내부 상태 보호용 mutex
    pthread_cond_t  readers_ok;   // reader 대기
    pthread_cond_t  writer_ok;    // writer 대기
    int             reader_count;
    bool            writer_active;
} my_rwlock_t;
```

OS(pthread)가 이 조합을 최적화해서 `pthread_rwlock_t` 한 줄로 제공하는 것이다.

### 1.4 Mutex vs Semaphore vs RWLock 비교

```
Mutex:       한 번에 1개. 읽기/쓰기 구분 없음.
Semaphore:   한 번에 N개. 읽기/쓰기 구분 없음. 소유권 없음.
RWLock:      읽기끼리 동시 OK, 쓰기는 독점. 읽기/쓰기를 구분함.
```

---

## 2. MiniDB 동시성 보호 — Level 1, 2, 3

클라이언트 3명이 동시에 요청을 보내는 상황을 기준으로 설명한다:

```
클라이언트 A: SELECT * FROM users WHERE id = 1
클라이언트 B: SELECT * FROM users WHERE id = 2
클라이언트 C: INSERT INTO users VALUES ('Dave', 30)
```

### 2.1 Level 1: Global Mutex

모든 요청을 하나의 mutex로 직렬화한다.

```c
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

int db_execute(const char *sql, char *out_buf, size_t cap) {
    pthread_mutex_lock(&db_mutex);
    statement_t stmt;
    parse(sql, &stmt);
    exec_result_t res = execute(&pager, &stmt);
    snprintf(out_buf, cap, "%s", res.message);
    pthread_mutex_unlock(&db_mutex);
}
```

실행 타이밍:

```
Worker A:  [===lock=== SELECT id=1 ===unlock===]
Worker B:                                        [===lock=== SELECT id=2 ===unlock===]
Worker C:                                                                              [===lock=== INSERT ===unlock===]
```

한 번에 하나만 실행된다. B는 A가 끝날 때까지, C는 B가 끝날 때까지 기다린다.
SELECT끼리도 직렬이다.

장점: 구현 5분. 동시성 버그 0.
단점: 스레드가 8개여도 엔진은 1개만 일한다.

### 2.2 Level 2: Engine RW Lock + Pager Mutex

SELECT는 rdlock, INSERT/UPDATE/DELETE는 wrlock으로 구분한다.
SELECT끼리는 동시에 실행되고, 쓰기 요청이 오면 모든 읽기가 끝날 때까지 기다린다.

```c
static pthread_rwlock_t engine_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  pager_lock  = PTHREAD_MUTEX_INITIALIZER;

int db_execute(const char *sql, char *out_buf, size_t cap) {
    statement_t stmt;
    parse(sql, &stmt);  // 파싱은 무상태 → 락 불필요

    bool is_write = (stmt.type == STMT_INSERT ||
                     stmt.type == STMT_DELETE ||
                     stmt.type == STMT_UPDATE);

    if (is_write) {
        pthread_rwlock_wrlock(&engine_lock);
    } else {
        pthread_rwlock_rdlock(&engine_lock);
    }

    exec_result_t res = execute(&pager, &stmt);
    snprintf(out_buf, cap, "%s", res.message);

    pthread_rwlock_unlock(&engine_lock);
}
```

실행 타이밍:

```
Worker A:  [==rdlock== SELECT id=1 ==unlock==]
Worker B:  [==rdlock== SELECT id=2 ==unlock==]     ← A와 동시에 실행
Worker C:                                      [==wrlock== INSERT ==unlock==]
                                                    ↑ A, B 둘 다 끝나야 시작
```

#### 왜 Pager Mutex가 추가로 필요한가

SELECT끼리 동시에 돌면 engine_lock은 rdlock이라 서로 안 막는다.
하지만 두 SELECT가 같은 page를 요청하면 pager 내부에서 충돌한다.

pager_mutex가 없을 때의 문제:

```
Worker A: pager_get_page(page_id=3)
Worker B: pager_get_page(page_id=3)

Worker A                              Worker B
────────                              ────────
frames 탐색: "page 3 없다"
                                      frames 탐색: "page 3 없다"
빈 frame[2] 찾음
                                      빈 frame[2] 찾음    ← 같은 빈 자리!
frame[2]에 page 3 로드
pin_count = 1
                                      frame[2]에 page 3 로드  ← 덮어쓰기!
                                      pin_count = 1         ← 2여야 하는데 1
```

pin_count가 1인데 실제로 2개 스레드가 쓰고 있다.
한 스레드가 unpin하면 pin_count=0이 되고, LRU가 이 frame을 내보내버린다.
나머지 스레드가 이미 다른 page로 덮어쓰인 메모리를 읽게 된다.

pager_mutex가 있으면:

```
Worker A                              Worker B
────────                              ────────
pager_mutex lock
  "page 3 없다"
  frame[2]에 로드
  pin_count = 1
pager_mutex unlock
                                      pager_mutex lock
                                        "page 3 있다! frame[2]!"  ← cache hit
                                        pin_count = 2             ← 정확
                                      pager_mutex unlock
```

두 번째 스레드는 디스크를 안 읽는다. 첫 번째가 이미 올려놨으니 cache hit로 처리된다.

pager 내부의 보호 대상:

```c
uint8_t *pager_get_page(pager_t *pager, uint32_t page_id) {
    pthread_mutex_lock(&pager_lock);
    // frame 탐색, pin_count++, used_tick 갱신
    // cache miss면 victim 선택 + 디스크 읽기
    pthread_mutex_unlock(&pager_lock);
    return frame->data;
}
```

정리:

```
engine_rwlock: "이 요청이 읽기냐 쓰기냐" 큰 단위 보호
pager_mutex:   "pager 메타데이터(pin, tick, frame 배열)" 짧은 단위 보호
```

#### Level 2 보호 대상 정리

| 공유 상태 | 위험 | 보호 수단 |
|-----------|------|-----------|
| header.next_id, row_count, next_page_id | 동시 INSERT 시 중복 ID | engine wrlock |
| header.free_page_head | 동시 alloc/free 시 free list 손상 | engine wrlock |
| B+Tree 구조 (split/merge) | 동시 insert 시 트리 파괴 | engine wrlock |
| heap slot (insert/delete) | 동일 slot 동시 재사용 | engine wrlock |
| frame[] (pin_count, used_tick, is_dirty) | SELECT끼리도 경합 | pager_mutex |
| LRU eviction/flush | 사용 중 frame 내보냄 | pager_mutex |
| stats (page_loads, cache_hits 등) | 디버그 값 꼬임 | 요청별 지역 변수로 분리 |

### 2.3 Level 3: Row-Level Lock (Strict 2PL)

Row 단위로 락을 걸어, 서로 다른 row에 대한 요청은 전부 동시에 실행한다.

```
Worker A: SELECT id=1  → S-lock(row 1)
Worker B: SELECT id=2  → S-lock(row 2)    ← 다른 row → 동시
Worker C: INSERT id=3  → X-lock(row 3)    ← 다른 row → 동시
```

세 요청이 전부 동시에 실행된다. 충돌은 같은 row에 접근할 때만 발생한다:

```
Worker A: SELECT id=1  → S-lock(row 1) → 읽는 중...
Worker D: UPDATE id=1  → X-lock(row 1) → ⏳ 대기 (S + X 호환 안 됨)
                                          → A 끝남 → 깨어남 → 수정 → unlock
```

호환성 규칙:

```
           요청하는 모드
            S       X
보유  S    OK      대기     ← 읽기끼리 OK, 읽기+쓰기 NO
중인  X    대기    대기     ← 쓰기 중이면 아무도 못 들어옴
```

Lock Table 구현 개요:

```c
typedef struct {
    uint64_t        row_id;
    int             mode;          // LOCK_SHARED or LOCK_EXCLUSIVE
    uint32_t        holder_count;
    pthread_cond_t  wait_queue;
} lock_entry_t;

static lock_entry_t lock_table[LOCK_TABLE_SIZE];
static pthread_mutex_t lock_table_mutex;

int lock_acquire(uint64_t row_id, int mode) {
    pthread_mutex_lock(&lock_table_mutex);
    lock_entry_t *entry = find_or_create(row_id);

    while (!compatible(entry->mode, mode)) {
        struct timespec deadline = now_plus_seconds(3);
        int rc = pthread_cond_timedwait(&entry->wait_queue,
                                         &lock_table_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&lock_table_mutex);
            return -1;  // 타임아웃 → abort (데드락 방지)
        }
    }

    entry->mode = mode;
    entry->holder_count++;
    pthread_mutex_unlock(&lock_table_mutex);
    return 0;
}

void lock_release_all(uint32_t txn_id) {
    // commit/abort 시점에 이 txn이 잡은 락 전부 해제
    // 대기 중인 스레드들을 깨움 (cond_broadcast)
}
```

Strict 2PL 규칙: 락은 commit/abort 시점에 한꺼번에 해제한다. 중간에 절대 안 놓는다.
이 규칙이 직렬화 가능(Serializability)을 보장한다.

### 2.4 세 겹의 락이 함께 동작하는 구조 (Level 3)

Level 3에서는 Row Lock이 **추가**된 것이지, 아래 두 층을 대체하는 것이 아니다.

```
Level 1:  [Global Mutex]
Level 2:  [Engine RWLock] + [Pager Mutex]
Level 3:  [Row Lock] + [Engine RWLock] + [Pager Mutex]
              ↑ 추가           ↑ 유지           ↑ 유지
```

각 층의 역할:

```
┌─────────────────────────────────────────────────┐
│  Row Lock (Lock Manager)                        │
│  보호 대상: 데이터의 논리적 정합성               │
│  유지 시간: 트랜잭션 전체 (수 ms ~ 수 초)        │
│  비유: 도서관 대출 장부                          │
│  핵심 질문: "이 row를 지금 누가 쓰고 있는가?"    │
├─────────────────────────────────────────────────┤
│  Engine RW Lock                                 │
│  보호 대상: B+Tree/heap 자료구조 무결성          │
│  유지 시간: 쿼리 실행 중 (수 ms)                 │
│  비유: 책장 재배치 표지판                        │
│  핵심 질문: "트리/힙 구조가 변하는 중인가?"      │
├─────────────────────────────────────────────────┤
│  Pager Mutex                                    │
│  보호 대상: frame 배열의 메타데이터              │
│  유지 시간: page 접근 한 번 (수 µs)              │
│  비유: 좁은 통로 관리인                          │
│  핵심 질문: "캐시 메타데이터가 꼬이지 않는가?"   │
└─────────────────────────────────────────────────┘
```

INSERT 하나가 실행될 때 락이 잡히고 풀리는 타이밍:

```
INSERT INTO users VALUES ('Eve', 28)

시간 →
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Row Lock ██████████████████████████████████████████████████
         잡음(id=3, X)                              풀림
         "3번 row는 내 거"

Engine   ·····██████████████████████████████████████·····
               잡음(wrlock)                    풀림
               "트리/힙 구조 건드릴 거야"

Pager         ··█··█··█··█··█··
               잡 풀 잡 풀 잡 풀
               매번 page 접근할 때마다 아주 짧게

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

잡는 순서: Row Lock → Engine Lock → Pager Mutex (바깥에서 안으로)
푸는 순서: Pager Mutex → Engine Lock → Row Lock (안에서 바깥으로)

이 순서를 모든 스레드가 동일하게 지켜야 데드락이 발생하지 않는다.

### 2.5 Level 비교 요약

| | Level 1 | Level 2 | Level 3 |
|---|---------|---------|---------|
| 보호 단위 | DB 전체 | 읽기/쓰기 구분 | row 단위 |
| SELECT + SELECT | 직렬 | 동시 | 동시 |
| SELECT + INSERT | 직렬 | 직렬 | 다른 row면 동시 |
| INSERT + INSERT | 직렬 | 직렬 | 다른 row면 동시 |
| 구현 시간 | 5분 | 2~3시간 | 4~6시간 |
| 버그 위험 | 거의 없음 | 낮음 | 중간 |

---

## 3. 실제 DB의 최적화 기법

Level 1~3은 출발점이다. 실제 DB는 각 단계의 병목을 정확히 타겟해서 풀어냈다.

### 3.1 Engine RWLock의 병목 → Lock Coupling (Crabbing)

Level 2~3에서 가장 큰 병목은 Engine RWLock이다.
INSERT 두 개가 서로 다른 row를 대상으로 해도, 둘 다 wrlock을 잡으니 직렬화된다.

실제로 INSERT가 B+Tree에서 하는 일은 "root → internal → leaf로 내려가서 leaf에 삽입"이다.
대부분의 경우 서로 다른 leaf로 가는데, 트리 전체를 잠글 필요가 없다.

Lock Coupling은 트리 전체 대신 **노드 하나씩** 잠그면서 내려간다.

#### 핵심 규칙

자식 노드의 latch를 잡은 뒤에야 부모 노드의 latch를 놓는다.
(게가 한 발씩 옮기듯 내려가서 Crabbing이라 부른다.)

그리고 **"이 자식에 여유가 있으면 부모를 놓는다"**가 판단 기준이다.
"여유가 있다"는 "아래에서 split이 올라와도 이 노드가 흡수할 수 있다"는 뜻이다.

#### 여유가 있는 경우

```
           [root: key 50]              ← 최대 3개 중 1개 → 여유 있음
          /              \
    [internal: key 30]    [internal: key 70]    ← 여유 있음
    /        \
[leaf A: 꽉 참]  [leaf B]

INSERT key=25 (leaf A로 갈 예정):

① lock(root)
② lock(internal 왼쪽)
③ internal에 여유 있나? → key 1개, 최대 3개 → 있음
   → "아래에서 split이 올라와도 여기서 받을 수 있다"
   → root 풀어줌
④ lock(leaf A)
⑤ leaf A 꽉 참 → split 필요
   → 부모(internal)는 아직 잡고 있으므로 안전하게 split
   → leaf를 둘로 나누고, 부모에 key 추가
   → internal에 여유가 있으므로 여기서 끝
```

이 동안 다른 INSERT가 오른쪽 subtree로 내려가면, root는 이미 풀렸으므로 동시에 진행된다:

```
Worker A: lock(root) → lock(internal L) → unlock(root) → lock(leaf A)
Worker B:              대기...           → lock(root) → lock(internal R) → unlock(root) → lock(leaf D)
```

root를 지나가는 순간만 직렬이고, 서로 다른 subtree에서는 동시에 실행된다.

#### 여유가 없는 경우 (연쇄 split)

```
           [root: 꽉 참]
          /          \
    [internal: 꽉 참]    [internal: 70]
    /        \
[leaf A: 꽉 참]  [leaf B]

INSERT key=25:

① lock(root)
② lock(internal 왼쪽)
③ internal에 여유 있나? → 꽉 참!
   → "아래에서 split 올라오면 나도 split 필요"
   → "그러면 위(root)도 건드려야 함"
   → root 안 풀어줌
④ lock(leaf A)
⑤ leaf A 꽉 참 → split
   → internal에 key 올림 → internal도 꽉 참 → internal split
   → root에 key 올림 → root도 잡고 있으므로 안전하게 처리
```

최악의 경우 root부터 leaf까지 전부 잡은 채로 내려가지만,
이건 트리의 모든 노드가 꽉 찬 극단적 상황에서만 발생한다.

#### 핵심 요약

```
규칙: 내려가면서, 여유가 있는 노드를 만나면 그 위의 락을 전부 풀어준다.
효과: 서로 다른 subtree로 가는 요청은 동시에 실행된다.
사용처: PostgreSQL의 nbtree.
```

### 3.2 Pessimistic → Optimistic Crabbing

#### Latch란 무엇인가

Lock Coupling에서 노드마다 "잠금"을 건다고 했는데, 이것의 정확한 이름이 latch다.
Lock과 Latch는 다른 개념이다:

```
Lock (Row Lock):
  누가 잡나: 트랜잭션
  얼마나 오래: 트랜잭션 끝날 때까지 (수 ms ~ 수 초)
  왜 잡나: "이 데이터는 내가 쓰고 있으니 건드리지 마"
  데드락 탐지: 필요함

Latch (Node Latch):
  누가 잡나: 코드 한 줄 (함수 안에서)
  얼마나 오래: 아주 짧게 (수 µs)
  왜 잡나: "이 메모리를 지금 수정 중이니 잠깐만"
  데드락 탐지: 필요 없음 (순서 규칙으로 예방)
```

latch는 rwlock을 노드마다 하나씩 붙인 것이다:

```c
// B+Tree 노드마다 latch가 있다
typedef struct {
    pthread_rwlock_t latch;   // 이 노드의 latch
    uint32_t page_type;
    uint32_t key_count;
    // ...
} btree_node_t;

// read latch = "이 노드 읽을 거야"
pthread_rwlock_rdlock(&node->latch);

// write latch = "이 노드 수정할 거야"
pthread_rwlock_wrlock(&node->latch);
```

#### Pessimistic의 문제

INSERT는 leaf에서 뭔가를 쓸 것이다.
혹시 split이 나면 부모도 수정해야 한다.
그래서 내려가는 모든 노드에 **write latch**를 잡는다:

```
INSERT key=25 (Pessimistic):

           [root]
              │ write latch ← 수정할 수도 있으니까
              ▼
         [internal]
              │ write latch ← 수정할 수도 있으니까
              ▼
          [leaf A]
              │ write latch ← 여기서 실제로 삽입
              ▼
           삽입 완료. split 필요 없었음.

→ root, internal에 write latch를 잡았지만 실제로 수정한 건 leaf뿐
→ 지나가는 동안 다른 모든 스레드가 그 노드에 접근 못 함
→ 낭비
```

write latch가 비싼 이유: 잡는 순간 그 노드에 읽기 스레드도 못 들어온다.
INSERT가 root를 지나갈 때 SELECT도 root에서 멈춘다.

실제로 split이 필요한 INSERT는 극소수다. 대부분은 leaf에 빈 자리가 있다.
하지만 Pessimistic은 "혹시 모르니까" 항상 write latch로 내려간다.

#### Optimistic의 해결

"아마 split 안 할 거야" — 그래서 **read latch**로 내려간다.
INSERT인데도 내려가는 동안은 SELECT처럼 가볍게 간다:

```
INSERT key=25 (Optimistic):

           [root]
              │ read latch ← 그냥 읽기처럼
              ▼
         [internal]
              │ read latch ← 그냥 읽기처럼
              ▼
          [leaf A]
              │ 도착해서 확인: 빈 자리 있나?
              │
              ├─ 있으면 (99%):
              │    leaf만 write latch로 업그레이드
              │    삽입
              │    끝
              │
              └─ 없으면 (1%):
                   모든 latch 풀기
                   처음부터 pessimistic 방식으로 재시도
```

read latch끼리는 서로 안 막으므로, INSERT가 내려가는 동안 SELECT가 동시에 진행된다:

```
Worker A (INSERT):  root(R) → internal(R) → leaf 확인 중
Worker B (SELECT):  root(R) → internal(R) → 동시 실행
                    ↑ read latch끼리 OK
```

비유:

```
Pessimistic = 복도를 걸을 때 "공사 중" 표지판을 세우면서 감
              공사를 안 해도 다른 사람이 복도를 못 지나감

Optimistic  = 그냥 걸어감. 목적지에서 "여기 공사해야겠다" 싶으면 그때만 표지판
              대부분은 표지판 없이 끝남
```

1% 확률로 재시도하는 비용보다, 99% 확률로 아무도 안 막는 이득이 압도적으로 크다.

### 3.3 트랜잭션 락 → MVCC

#### Level 3 (Strict 2PL)의 한계

SELECT도 S-lock을 잡는다. 읽기가 쓰기를 막고, 쓰기가 읽기를 막는다:

```
2PL:
  A: SELECT id=1 → S-lock(id=1) → 읽기
  B: UPDATE id=1 → X-lock(id=1) → 대기 (A가 끝날 때까지)
```

#### MVCC: 읽기에 락이 없다

row마다 여러 버전을 보관한다. 각 트랜잭션은 자기 시작 시점의 스냅샷을 본다.

```
MVCC:
  A: SELECT id=1 → 락 없음. 시작 시점의 버전을 읽음
  B: UPDATE id=1 → 새 버전을 만듦. A가 보는 구 버전은 건드리지 않음
  → 둘 다 동시에 실행. 아무도 안 기다림
```

#### 버전 가시성: "내 트랜잭션 시작 시점"이 기준이다

```
시간 순서:

t=100  txn_A 시작 (SELECT)
t=110  txn_B 시작 (UPDATE id=1, age 25→99)
t=120  txn_B COMMIT → 새 버전 생성됨
t=130  txn_A가 id=1을 읽음
```

txn_A는 age=25를 본다. 99가 아니다.

txn_A는 t=100에 시작했다. txn_B의 commit(t=120)은 txn_A의 시작 이후이므로 안 보인다.

```
버전 체인:
  id=1, version 2: {age=99, created_by=txn_B, commit_time=120}
  id=1, version 1: {age=25, created_by=txn_X, commit_time=50}

txn_A (시작 t=100)가 읽을 때:
  version 2: commit_time=120 > 시작 100 → 안 보임
  version 1: commit_time=50  < 시작 100 → 이거 보임
  → age=25 반환

txn_C (시작 t=140)가 읽을 때:
  version 2: commit_time=120 < 시작 140 → 이거 보임
  → age=99 반환
```

"락이 풀렸으니 자동으로 최신을 본다"가 아니다.
"내 트랜잭션 시작 시점 기준으로 그때까지 commit된 버전 중 최신을 본다"이다.

쓰기끼리는 여전히 락이 필요하다 (같은 row를 동시에 수정하면 안 되므로).
하지만 읽기에 락이 전혀 없으므로 OLTP 워크로드에서 처리량이 극적으로 좋아진다.

대가: 구 버전을 주기적으로 청소해야 한다.
PostgreSQL은 VACUUM, InnoDB는 purge thread가 이 역할을 한다.

현대 DB에서 MVCC를 쓰는 곳:
PostgreSQL, MySQL InnoDB, Oracle, SQL Server (READ_COMMITTED_SNAPSHOT 모드).

### 3.4 디스크 보호 → WAL + Group Commit

#### WAL (Write-Ahead Logging)

MiniDB는 dirty page를 바로 디스크에 쓴다.
쓰는 도중 서버가 죽으면 page가 반쪽만 기록된다(torn page).

WAL은 "로그를 먼저 쓰고, 페이지는 나중에"라는 규칙이다:

```
MiniDB (현재):
  INSERT 실행 → page 수정 → 언젠가 flush
  → 죽으면 날아감

WAL:
  INSERT 실행 → redo log에 "id=3, age=28 넣었음" 기록 → fsync(로그)
  → 이 시점에 commit 완료 (page는 아직 안 씀)
  → 나중에 여유 있을 때 page를 디스크에 씀
  → 중간에 죽어도 로그를 재생(replay)하면 복구 가능
```

크래시 후 복구:

```
정상 실행 중 기록된 로그:
  [1] redo: page 5, offset 200에 row 쓰기
  [2] redo: page 3, offset 100의 age를 99로
  [3] txn_7 commit

크래시 후:
  로그를 처음부터 재생:
    [1] 실행, [2] 실행, [3] commit 확인 → 이 트랜잭션은 유효

  만약 commit 로그가 없는 트랜잭션:
    → 그 변경을 되돌림 (undo)
```

#### Group Commit

fsync는 "디스크에 확실히 기록해라"라는 명령이다.
한 번에 약 5~10ms가 걸리고, 이 비용이 commit 성능의 병목이 된다.

Group Commit은 **개수 기준이 아니라 시간 기준**이다.
"N개 모일 때까지 기다린다"가 아니라, "fsync가 진행되는 동안 온 요청을 묶는다"이다:

```
txn_1: commit → fsync 시작 (5ms 걸림)
                  ↑ 이 5ms 동안:
txn_2: commit → "이미 누가 fsync 하고 있네, 같이 태워줘"
txn_3: commit → "나도 같이"
txn_4: commit → "나도"
                  ↓ 5ms 후 fsync 완료
txn_1, 2, 3, 4: 전부 commit 완료
```

비유:

```
개별 fsync = 택시. 사람마다 한 대씩 탐.
Group Commit = 버스. 버스가 한 바퀴 도는 동안(fsync 시간) 온 사람을 다 태움.
```

부하가 높을수록 한 번의 fsync에 더 많은 commit이 묶인다:

```
부하 낮음: 5ms에 commit 2개   → fsync 1번으로 2개 처리
부하 높음: 5ms에 commit 100개 → fsync 1번으로 100개 처리
```

부하가 높아질수록 1 commit당 비용이 줄어드는 구조다.

### 3.5 Buffer Pool 최적화

MiniDB의 pager_mutex는 page 접근마다 잡았다 풀었다 한다.
한 쿼리가 page를 10번 읽으면 mutex를 10번 잡는다.

실제 DB는 이걸 hash bucket별 mutex로 쪼갠다:

```
MiniDB:
  pager_mutex 1개 → 모든 page 접근이 직렬화

InnoDB:
  hash bucket 1024개 → page_id % 1024 = bucket 번호
  page 3 접근 → bucket[3]만 잠금
  page 5 접근 → bucket[5] 잠금 (다른 bucket이니 동시 OK)
```

InnoDB는 Buffer Pool 자체를 여러 인스턴스로 샤딩한다:

```
Buffer Pool Instance 0: page_id % 8 == 0인 page 담당
Buffer Pool Instance 1: page_id % 8 == 1인 page 담당
...
Buffer Pool Instance 7: page_id % 8 == 7인 page 담당

→ 8개 인스턴스가 완전히 독립된 mutex를 가짐
→ 경합이 1/8로 줄어듦
```

---

## 4. 전체 진화 경로

```
MiniDB (학습용)                         실제 DB (PostgreSQL/InnoDB)
──────────────                          ─────────────────────────

Engine 전체 RWLock                      Lock Coupling / Optimistic Crabbing
"트리 전체를 잠근다"                     "노드 하나씩만 잠근다"
        │                                       │
        ▼                                       ▼
Row Lock (Strict 2PL)                   MVCC
"읽기도 잠근다"                          "읽기는 락 없이 옛 버전을 본다"
        │                                       │
        ▼                                       ▼
Pager 단일 Mutex                        Hash Bucket Mutex + Pool 샤딩
"page 접근마다 전체 잠금"                "bucket 단위로 분산"
        │                                       │
        ▼                                       ▼
dirty page 직접 flush                   WAL + Group Commit + Checkpoint
"죽으면 날아감"                          "죽어도 로그로 복구"
```

각 단계가 이전 단계의 병목을 정확히 타겟해서 풀어낸 것이다.
Level 1~3은 이 진화 경로의 첫 번째 칸이다.
여기서부터 "왜 Lock Coupling이 필요한가", "왜 MVCC가 나왔는가"를 이해할 수 있는 출발점이 된다.

---

## 5. MiniDB 구현 시 권장 적용 범위

| 기법 | 구현 여부 | 이유 |
|------|-----------|------|
| Level 1 (Global Mutex) | 필수 | 첫 동작 검증. 5분이면 끝남 |
| Level 2 (Engine RWLock + Pager Mutex) | 강력 권장 | read-read 병렬성 확보. 벤치마크 스토리 |
| Level 3 (Row Lock + Strict 2PL) | 시간 남으면 | row 단위 병렬성. 차별화 포인트 |
| Lock Coupling | 하지 않음 | split 경로 버그 위험. 하루 안에 안정적 완성 어려움 |
| Optimistic Crabbing | 하지 않음 | Lock Coupling 위에 추가 최적화. 공수 과다 |
| MVCC | 하지 않음 | 2주+ 공수. 버전 관리/GC까지 필요 |
| WAL | 스켈레톤만 | redo entry 형식 정의, 실제 fsync는 생략. 발표 시 "여기가 WAL 자리"로 설명 |
| Buffer Pool 샤딩 | 하지 않음 | pager 구조 전면 개편 필요 |

구현 순서: Level 1 → Level 2 → (검증 통과 후) → Level 3

각 단계를 넘어가기 전에 반드시 동시성 smoke test를 통과시킨다.
Level 2까지만으로도 "SELECT끼리는 동시에 돈다"를 벤치마크로 보여줄 수 있어서 발표에 충분하다.
