# Q18. 스레드 · 동시성 — 락 없이 터지는 실전 시나리오 완전 분해

> CSAPP 12장 + 리눅스 커널 | 멀티코어에서 공유 메모리가 왜 폭발하는가, 그리고 무엇으로 막는가 | 심화

## 질문

1. 스레드가 여러 코어에서 동시에 돌 때 하드웨어는 어떻게 캐시 일관성을 유지하나?
2. 캐시 일관성 프로토콜이 있는데도 왜 락 없이는 프로그램이 터지는가?
3. 락 없는 코드가 어떤 방식으로 터지는가? 실제 예시·시나리오·원인·결과는?
4. 리눅스 커널·libc 는 어떤 락·원자연산을 제공하고 언제 써야 하는가?

## 답변

### §1. 스레드 = 주소공간을 공유하는 실행 문맥

```
프로세스 P
 ├── mm_struct (가상 주소공간, 힙, mmap, 파일맵)  <- 스레드 전체가 공유
 ├── files_struct (fd table)                      <- 스레드 전체가 공유
 ├── signal handlers                              <- 공유
 │
 ├── task_struct (TID=1001)   <- 스레드 1: 레지스터·스택·TLS 만 독립
 ├── task_struct (TID=1002)
 └── task_struct (TID=1003)
```

리눅스는 프로세스와 스레드를 같은 `task_struct` 로 구현한다 (차이는 `CLONE_VM` 플래그 유무). 그래서 "스레드는 주소공간을 공유하는 가벼운 프로세스" 라고 불린다.

### §2. 하드웨어 — MESI 캐시 일관성 프로토콜

코어가 N 개면 각자의 L1/L2 캐시가 따로. 그 사이에서 **같은 DRAM 주소** 의 데이터가 정합성을 유지하게 하는 것이 **MESI**.

#### 네 가지 상태

| 상태 | 의미 | DRAM 과 | 다른 코어에 |
| --- | --- | --- | --- |
| **M** Modified | 내가 수정했고 나만 갖고 있음 | 다름 (stale) | 없음 |
| **E** Exclusive | 나만 갖고 있음, 아직 안 씀 | 같음 | 없음 |
| **S** Shared | 여럿이 읽기로 갖고 있음 | 같음 | 있음 |
| **I** Invalid | 이 슬롯 비었음 / 무효 | — | — |

#### 전이 시나리오

```
(A) 둘 다 읽기만
    Core0: X 로드 -> E
    Core1: X 로드 -> bus: "나도 X" -> Core0 snoop, E->S
    둘 다 S. 문제 없음.

(B) Core0 가 쓰려 함
    Core0: S -> bus 에 "RFO(X)" 브로드캐스트
    Core1 snoop: 자기 X 라인을 I 로 전이
    Core0: S -> M, 쓰기 수행
    DRAM 엔 아직 반영 안 됨 (write-back)

(C) 무효화된 Core1 이 다시 쓰려 함
    Core1: I -> bus 에 요청
    Core0 snoop: M 상태인 라인을 cache-to-cache 로 Core1 에 포워딩
    Core0: M -> I (또는 MOESI 의 O 상태)
    Core1: M 으로 최신값 획득
```

#### 코어 사이 "통신" — 버스·Snoop·Directory

MESI 메시지는 코어 간 **캐시 일관성 버스** 로 오간다.

- Intel: Ring/Mesh interconnect + 소켓간 UPI
- AMD: Infinity Fabric
- 대형 서버: Directory-based (DRAM 근처 디렉토리 테이블)

그러므로 **"CPU 들은 통신하지 않는다" 는 오해**다. 통신한다. 수십~수백 cycle 들여서.

### §3. MESI 가 있어도 레지스터 경계는 보호되지 않는다

**이게 락이 필요한 이유의 핵심.**

```c
count++;   // C 한 줄

// 어셈블리로 펼치면 3 단계
   mov eax, [count]    ; load
   inc eax             ; compute
   mov [count], eax    ; store
```

두 코어가 동시에 실행:

```
t=0  Core0: mov eax, [count]  ;  eax = 100, 캐시 S
t=0  Core1: mov eax, [count]  ;  eax = 100, 캐시 S
t=1  Core0: inc eax           ;  eax = 101
t=1  Core1: inc eax           ;  eax = 101
t=2  Core0: mov [count], eax  ;  RFO -> Core1 캐시 I, 라인 101
t=2  Core1: mov [count], eax  ;  RFO -> Core0 캐시 I, 라인 101

결과: count = 101 (102 가 되어야 하는데 한 번 증가 소실)
```

MESI 는 **"마지막 store 가 이긴다"** 만 보장, `load-compute-store` 시퀀스를 원자적으로 묶어주진 않는다. 사이에 다른 코어가 끼어든다.

### §4. 터지는 시나리오 13선 — 코드 + 원인 + 결과 + 수정

공유 상태를 락 없이 만지면 어떤 식으로 망가지는지 구체 예시로.

---

####  S1. Lost Update — 카운터 경쟁

**상황**: 웹서버가 요청 수를 카운트.

```c
int request_count = 0;   // 전역

void *handler(void *arg) {
    request_count++;     // 1만 번 호출
    return NULL;
}

int main() {
    pthread_t t[100];
    for (int i = 0; i < 100; i++)
        pthread_create(&t[i], NULL, handler, NULL);
    for (int i = 0; i < 100; i++)
        pthread_join(t[i], NULL);
    printf("%d\n", request_count);   // 기대: 100 0000
}
```

**실제 결과**: 실행할 때마다 다름. `987234`, `992011`, `1000000` (운 좋을 때).

**무엇이 일어났나**: §3 에서 본 load-compute-store 분할.

**터지나?**: 크래시 아님. 하지만 **데이터 정합성 파괴**. 과금·통계·재고 시스템에선 치명.

**수정**:
```c
atomic_int request_count = 0;
atomic_fetch_add(&request_count, 1);   // lock prefix 로 RMW 원자화
```

---

####  S2. Torn Write — 찢어진 64비트 값

**상황**: 32비트 시스템에서 64비트 정수를 공유.

```c
uint64_t timestamp = 0;

void *writer(void *arg) {
    while (1) timestamp = get_now_ns();  // 64비트 write
}

void *reader(void *arg) {
    while (1) {
        uint64_t t = timestamp;          // 64비트 read
        if (t < get_now_ns() - 1e12)     // 조건 체크
            handle_timeout(t);
    }
}
```

**실제 결과**: reader 가 **상위 32비트는 새 값 + 하위 32비트는 옛 값** 인 괴상한 timestamp 를 받는다. 논리 오류, 필요시 0 시간 반환 -> handle_timeout 에서 나눗셈 0 -> SIGFPE.

**무엇이 일어났나**: 32비트 CPU 에선 64비트 write 가 두 번의 store. reader 가 그 사이에 읽음. x86-64 도 정렬이 안 된 64비트 값은 같은 문제.

**터지나?**: 종종. `SIGFPE`, `SIGSEGV` (잘못된 포인터 역참조), 논리 오류.

**수정**:
```c
_Atomic uint64_t timestamp;   // C11 atomic
atomic_store(&timestamp, get_now_ns());
uint64_t t = atomic_load(&timestamp);
```

---

####  S3. Use-After-Free — 삭제와 참조 경쟁

**상황**: 세션 테이블에서 한 스레드가 조회하는 사이 다른 스레드가 삭제.

```c
struct session {
    int id;
    char data[1024];
};

struct session *sessions[1000];

void *reader(void *arg) {
    int id = 42;
    struct session *s = sessions[id];       // ① 포인터 load
    if (s) {
        printf("%s\n", s->data);            // ② 역참조
    }
}

void *deleter(void *arg) {
    struct session *s = sessions[42];
    sessions[42] = NULL;
    free(s);                                // ③ 해제
}
```

**실제 결과**: `reader` 가 ① 과 ② 사이에 `deleter` 가 ③ 을 실행하면 이미 해제된 메모리를 읽음.

**무엇이 일어났나**: ①② 사이의 race. `s` 는 reader 의 스택에 있지만 가리키는 객체는 이미 free 되어 heap 의 다른 누군가에게 재할당됨.

**터지나?**: 매우 자주.
- `SIGSEGV`: 해제된 페이지가 unmap 됨
- `SIGABRT`: heap metadata 손상 -> glibc 의 `double free or corruption` 에서 `abort()`
- 은밀한 데이터 오염: 해제 후 재할당된 다른 객체 데이터가 `s->data` 로 읽힘

**수정**:
```c
// 방법 1: mutex + refcount
struct session *s = NULL;
pthread_mutex_lock(&session_lock);
s = sessions[42];
if (s) atomic_fetch_add(&s->refcount, 1);
pthread_mutex_unlock(&session_lock);

if (s) {
    printf("%s\n", s->data);
    if (atomic_fetch_sub(&s->refcount, 1) == 1)
        free(s);
}

// 방법 2: RCU (read-copy-update) — 커널/C 모두 존재
rcu_read_lock();
s = rcu_dereference(sessions[42]);
if (s) printf("%s\n", s->data);
rcu_read_unlock();
// deleter 는 synchronize_rcu() 후 free
```

---

####  S4. Double-Free — 같은 포인터 두 번 free

**상황**: 정리 코드 경쟁.

```c
void *cleanup(void *arg) {
    if (global_buf) {
        free(global_buf);
        global_buf = NULL;
    }
}
```

두 스레드가 동시에 `cleanup` 진입.

**실제 결과**:
```
Thread1: if check -> true
Thread2: if check -> true    (아직 global_buf 그대로)
Thread1: free(global_buf)
Thread1: global_buf = NULL
Thread2: free(global_buf)   <- 이미 NULL?  아니, 자신의 local 변수 안 씀
          실제론 Thread2 도 global_buf 를 다시 읽어 NULL 이 되어 있을 수도 있고
          타이밍에 따라 같은 포인터를 두 번 free
```

**터지나?**: 거의 항상.
- glibc 기본 방어: `double free or corruption (fasttop)` 메시지와 함께 `SIGABRT`
- tcmalloc/jemalloc 도 비슷한 단정 실패

**예상 출력**:
```
*** glibc detected *** ./app: double free or corruption (!prev): 0x00007f8... ***
Aborted (core dumped)
```

**수정**: mutex 로 감싸거나, `atomic_exchange` 로 "한 번만 성공" 을 보장:
```c
void *old = atomic_exchange(&global_buf, NULL);
if (old) free(old);
```

---

####  S5. Null Pointer Dereference — 부분 초기화 공유

**상황**: 싱글톤 게으른 초기화.

```c
struct config *g_cfg = NULL;

void init_config() {
    if (!g_cfg) {
        g_cfg = malloc(sizeof(*g_cfg));   // ①
        g_cfg->db_host = strdup("localhost");   // ②
        g_cfg->db_port = 5432;            // ③
    }
}

void *worker(void *arg) {
    init_config();
    printf("%s:%d\n", g_cfg->db_host, g_cfg->db_port);
}
```

두 스레드 동시에 `worker` 진입.

**실제 결과**:
- Thread1: ① 수행 (`g_cfg` 에 할당된 쓰레기 값 포인터)
- Thread2: `if (!g_cfg)` false, 바로 `printf("%s:%d\n", g_cfg->db_host, ...)`
- `g_cfg->db_host` 는 아직 초기화 전 (쓰레기 값)
- strdup 아직 호출 안 됨 -> `db_host` 가 0 이 아닌 랜덤 값
- printf 가 잘못된 주소 역참조 -> **SIGSEGV**

**무엇이 일어났나**: `g_cfg` 는 "포인터가 non-NULL" 이 되었지만 가리키는 메모리 내부는 초기화 덜 됨. 또 컴파일러/CPU 가 store 를 재배치할 수도 있음 (②③ 보다 ① 이 먼저 "보일" 수도).

**터지나?**: `SIGSEGV` (잘못된 주소), 운좋으면 쓰레기 값 출력.

**수정**:
```c
// C11 once
static pthread_once_t once = PTHREAD_ONCE_INIT;

void init_config() {
    g_cfg = malloc(sizeof(*g_cfg));
    g_cfg->db_host = strdup("localhost");
    g_cfg->db_port = 5432;
}

void *worker(void *arg) {
    pthread_once(&once, init_config);   // 정확히 한 번
    // ...
}
```

---

####  S6. Publish-Before-Init — 메모리 순서 깨짐

**상황**: 생산자-소비자 플래그.

```c
struct item *item = NULL;
int ready = 0;

void producer() {
    item = malloc(sizeof(*item));
    item->x = 42;              // ①
    item->y = 17;              // ②
    ready = 1;                 // ③
}

void *consumer() {
    while (!ready) ;           // spin
    use(item->x, item->y);     // ④
}
```

**실제 결과**: consumer 가 `ready == 1` 을 봤는데도 `item->x` 가 아직 42 가 아닐 수 있음.

**무엇이 일어났나**: CPU (또는 컴파일러) 가 ①② ③ 의 store 순서를 재배치할 수 있다. 특히 ARM, POWER 처럼 약한 순서 모델에선 일상. x86 TSO 라도 컴파일러 재배치는 언제나 가능.

**터지나?**: 쓰레기 값으로 인한 `SIGSEGV`, 논리 오류.

**수정**:
```c
// 릴리스 / 획득 의미
atomic_store_explicit(&ready, 1, memory_order_release);

while (!atomic_load_explicit(&ready, memory_order_acquire)) ;
// acquire 는 이후의 모든 read 가 release 이전 write 를 본다는 보장
```

---

####  S7. Linked List — 연결 포인터 경쟁

**상황**: 링크드 리스트 head 에 삽입 두 스레드 동시.

```c
struct node *head = NULL;

void insert(struct node *n) {
    n->next = head;       // ①
    head = n;             // ②
}
```

**시나리오**:
```
Thread1: n1->next = head (= NULL)       // ① t1
Thread2: n2->next = head (= NULL)       // ① t2
Thread1: head = n1                      // ② t1, head = n1
Thread2: head = n2                      // ② t2, head = n2

리스트 상태:
  head -> n2 -> NULL
  n1 은 어디에도 연결 안 됨 (누수) 또는
  만약 삽입 후 탐색이 돌고 있으면 n1 을 잡으려던 참조가 깨짐
```

동시 순회 + 삭제 + 삽입이 섞이면 SIGSEGV 거의 보장.

**수정**:
```c
pthread_mutex_t list_lock;
pthread_mutex_lock(&list_lock);
n->next = head;
head = n;
pthread_mutex_unlock(&list_lock);
```

또는 CAS 기반 lock-free (ABA 주의 -> S8):
```c
do {
    struct node *old_head = atomic_load(&head);
    n->next = old_head;
} while (!atomic_compare_exchange_weak(&head, &old_head, n));
```

---

####  S8. ABA Problem — CAS 의 함정

**상황**: 위 lock-free 삽입 + 삭제 혼합.

```
Thread1: head 를 A 로 읽음 (old_head = A, next = B)
        CAS 기다림 ...

Thread2: A 꺼내기 -> head = B
Thread2: B 꺼내기 -> head = C
Thread2: A 다시 push -> head = A, A->next = C

Thread1: CAS(head, old_head=A, new=X)  -> 성공! (head 가 A 이므로)
        X->next = B   이지만 이미 B 는 free / 다른 위치
        리스트 손상
```

**무엇이 일어났나**: head 가 A -> B -> C -> A 로 바뀌어 "같은 값으로 되돌아온" ABA. CAS 는 값만 보니까 바뀌었는지 알 수 없다.

**터지나?**: 포인터 손상 -> 역참조 시 `SIGSEGV`, 리스트가 순환하게 돼 `무한루프`.

**수정**: 포인터에 **tag/version 카운터** 를 덧붙여 double-wide CAS (x86 `cmpxchg16b`). 또는 **hazard pointer**, **RCU** 로 회피.

---

####  S9. Deadlock — 락 순서 불일치

**상황**: 계좌 이체.

```c
void transfer(Account *from, Account *to, int amount) {
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}
```

```
Thread1: transfer(A, B)  -> A 락 획득 -> B 락 대기
Thread2: transfer(B, A)  -> B 락 획득 -> A 락 대기

-> 영원히 서로 기다림 (deadlock)
```

**터지나?**: 프로세스 `hang` (SIGSEGV 아님, 그냥 멈춤). 헬스체크 타임아웃으로 외부에서 SIGKILL 받음.

**수정 — 락 순서 규칙**:
```c
void transfer(Account *a, Account *b, int amount) {
    // 포인터 값이 작은 쪽을 먼저 락
    Account *first  = (a < b) ? a : b;
    Account *second = (a < b) ? b : a;
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);
    // ...
}
```

또는 `pthread_mutex_trylock` + backoff, 아니면 더 큰 단위 락 하나로 단순화.

---

####  S10. errno 레이스 — 비재진입 라이브러리

**상황**: 여러 스레드가 `strtol` 사용.

```c
errno = 0;
long v = strtol(s, NULL, 10);
if (errno == ERANGE) { /* ... */ }
```

옛날 `errno` 가 전역 int 였을 때, 한 스레드의 `strtol` 실패가 다른 스레드의 errno 를 덮어써서 엉뚱한 에러 처리.

**현재 상태**: glibc 는 `errno` 를 **per-thread** 로 구현해 이 문제는 자동 해결. 하지만 원리를 알아두는 게 중요. 비재진입 함수(`strtok`, `gethostbyname` 등) 는 내부에 static 버퍼를 공유해서 여전히 위험.

**수정**: `strtok_r`, `gethostbyname_r` 같은 `_r` 접미사 버전 사용.

---

####  S11. malloc 경합 — 성능 병목

**상황**: 수천 스레드가 동시에 `malloc`/`free`.

glibc `ptmalloc` 은 메인 아레나 + 서브 아레나 락 경쟁으로 병목 발생. jemalloc, tcmalloc 는 per-thread cache 로 완화.

**터지나?**: 데드락 아니고 성능만 저하. 하지만 `malloc` 자체가 라이브러리 락을 잡은 상태에서 다른 경로(시그널, fork) 로 재진입하면 데드락 가능 -> `fork()` 뒤 `async-signal-safe` 함수만 써야 하는 이유.

---

####  S12. Signal + 스레드 — 비동기 안전성

**상황**: `malloc` 중에 SIGALRM 발생, 핸들러가 다시 `malloc`.

```c
void handler(int sig) {
    char *buf = malloc(1024);   // async-signal-unsafe!
    // ...
}
```

**실제 결과**: heap 락이 걸린 상태에서 같은 스레드가 재진입 -> 데드락 또는 heap 손상.

**수정**: 핸들러는 `async-signal-safe` 함수만 사용 (`write`, `_exit` 등). 또는 `sigwait` 로 전용 스레드에서 처리.

---

####  S13. False Sharing — 크래시 아닌 성능 지옥

```c
struct stats {
    long counter_a;   // Thread A 가 계속 쓰기
    long counter_b;   // Thread B 가 계속 쓰기
} s;                   // 둘 다 같은 64B 캐시 라인
```

논리적으로 독립인데 매 쓰기가 상대 라인을 invalidate -> 성능이 1/10 이하로 떨어짐.

**수정**:
```c
struct stats {
    long counter_a;
    char pad[64 - sizeof(long)];   // 패딩
    long counter_b;
} __attribute__((aligned(64)));
```

또는 `cacheline_aligned` 매크로, per-CPU 변수 사용.

---

### §5. 리눅스 커널 locking 카탈로그

커널이 실제로 제공하는 동기화 수단.

| 종류 | 헤더 | 특성 | 언제 쓰나 |
| --- | --- | --- | --- |
| `atomic_t`, `atomic64_t` | `<linux/atomic.h>` | 하나의 정수 RMW 원자 | 카운터, 플래그 |
| `spinlock_t` | `<linux/spinlock.h>` | busy-wait, 짧은 임계구역, 인터럽트 안전 | IRQ handler, softirq |
| `rwlock_t` | `<linux/rwlock.h>` | reader 다수 허용 / writer 단일 | 읽기 주도 공유 자료구조 |
| `seqlock_t` | `<linux/seqlock.h>` | writer 우선, reader 재시도 | jiffies, 시간 |
| `mutex` | `<linux/mutex.h>` | sleep 가능, 공정성 | 프로세스 컨텍스트, 긴 임계구역 |
| `semaphore` | `<linux/semaphore.h>` | counting, sleep | 자원 개수 제한 |
| `rw_semaphore` | `<linux/rwsem.h>` | sleep 가능 reader/writer | mmap_sem 등 |
| `completion` | `<linux/completion.h>` | 한 번의 이벤트 대기 | I/O 완료 |
| RCU | `<linux/rcu*.h>` | 읽기 lock-free, 쓰기 copy-update | 라우팅 테이블, dentry cache |
| `qspinlock` | 내부 | MCS 기반 공정 spinlock | 현대 spinlock 기본 구현 |

#### 어떤 락을 고르나 — 의사결정 트리

```
임계구역에서 sleep 가능한가?
  ├─ Yes -> mutex / rwsem / semaphore
  │        └ 읽기가 훨씬 많다면 rwsem 또는 RCU
  └─ No (IRQ/softirq)
           ├─ 짧고 단순: spinlock (IRQ-safe: spin_lock_irqsave)
           ├─ 매우 읽기 위주: seqlock 또는 RCU
           └─ 단일 정수: atomic_t
```

#### 커널 spinlock 구현 조각

```c
// include/linux/spinlock.h
static inline void spin_lock(spinlock_t *lock)
{
    raw_spin_lock(&lock->rlock);
}

// kernel/locking/qspinlock.c (요지)
void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
    // MCS queue 에 자신의 노드 추가
    // 자기 노드의 locked 필드를 spin
    // 앞 노드가 완료되면 자기 차례 signal
}
```

#### RCU 최소 예시

```c
// 읽기
rcu_read_lock();
p = rcu_dereference(shared_ptr);
use(p);
rcu_read_unlock();

// 쓰기
new = kmalloc(...);
new->... = ...;
old = shared_ptr;
rcu_assign_pointer(shared_ptr, new);
synchronize_rcu();             // 모든 reader 가 빠질 때까지 대기
kfree(old);
```

리눅스 커널이 dentry cache, routing table, IP route 에서 쓰는 방식. 읽기가 압도적으로 많고 쓰기는 드문 경우 최고 성능.

### §6. 유저 공간 도구 모음

| 도구 | 용도 |
| --- | --- |
| `pthread_mutex_t` / `pthread_rwlock_t` | POSIX 기본 |
| C11 `_Atomic`, `<stdatomic.h>` | 이식성 있는 원자연산 |
| `std::mutex`, `std::atomic` (C++) | C++ |
| `java.util.concurrent` | JVM |
| Go `sync.Mutex`, `atomic` | Go |
| Python `threading.Lock` + **GIL** | GIL 이 있어 대부분의 Python 코드는 바이트코드 단위 원자. 하지만 `count += 1` 같은 복합 연산은 여전히 race (C-extension 해제 구간) |

### §7. 검증 도구 — 동시성 버그는 재현이 어렵다

| 도구 | 발견 가능한 것 |
| --- | --- |
| **ThreadSanitizer** (TSan) | data race 정적·동적 검출. `-fsanitize=thread` |
| **Helgrind** (Valgrind) | race, lock order 위반 |
| **DRD** (Valgrind) | race, deadlock |
| **AddressSanitizer** (ASan) | use-after-free, double-free (race 아닌 것도) |
| **lockdep** (커널) | 커널에서 lock order 자동 검증 |
| **KCSAN** (커널) | kernel concurrency sanitizer |

실전 팁: 스트레스 테스트 (높은 동시성), 긴 실행, `sleep()` 을 일부러 삽입해 타이밍 창 확대. 그리고 무엇보다 **TSan 은 반드시 CI 에 넣기**.

### §8. 체크리스트

1. 공유 자료구조 <-> 락을 1:1 로 연결했는가? (어떤 락이 어떤 필드를 지키는지 주석)
2. 여러 락을 잡는 순서가 코드 전체에서 일관된가?
3. RMW 연산은 atomic 또는 락으로 감쌌는가?
4. 오래 걸리는 작업(I/O) 은 락 밖으로 빼냈는가?
5. `malloc`, 로깅 같은 큰 락이 깊이 중첩되지 않는가?
6. 핸들러에서 async-signal-unsafe 함수를 부르지 않는가?
7. `fork()` 뒤 child 에서 락을 붙잡은 뮤텍스 상태가 정리되어 있는가?
8. TSan 이 녹색인가?
9. 읽기 압도적이면 RWLock 또는 RCU 로 바꿀 수 있는가?
10. 핫 변수는 cacheline 분리했는가 (false sharing 회피)?

## 연결 키워드

- [q16-thread-pool-async.md](./q16-thread-pool-async.md) — 스레드 풀 구조, prethreading
- [q17-concurrency-locks.md](./q17-concurrency-locks.md) — 카페 비유 기반 실무 패턴
- [q10-io-bridge.md](./q10-io-bridge.md) — MESI 가 동작하는 하드웨어 버스
- [q04-filesystem.md](./q04-filesystem.md) — VFS/페이지 캐시 내부 락
