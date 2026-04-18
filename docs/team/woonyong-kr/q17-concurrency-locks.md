# Q15. 동시성과 락 — 스레드 풀에서 thread-safe 하게 일하는 법

> CSAPP 12장 + 본 주 SQL API 서버 실전 | 스레드 풀 / 뮤텍스 / 조건 변수 / 데드락 / RWLock | 중급
> [00-topdown-walkthrough.md §22](./00-topdown-walkthrough.md#22-iterative--concurrent--스레드-풀-epoll-io_uring) 의 심화 버전.

## 질문

1. 스레드 풀이 뭔지 카페 비유로 처음부터 설명해 달라.
2. 왜 락이 필요한가. race condition 이 실제로 어떻게 나는가.
3. 뮤텍스와 조건 변수, 세마포어의 차이.
4. thread-safe / reentrant / thread-unsafe 는 무엇이 다른가.
5. 데드락은 어떻게 나고, 어떻게 피하나.
6. 이번 주 SQL API 서버에서 실제로 어디에 락이 필요한가.

## 답변

### 최우녕

> 스레드 풀이 뭔지 카페 비유로 처음부터 설명해 달라.

**동네 카페** 로 시작하자. 손님이 주문하고, 바리스타가 음료 만들고, 손님이 받는다. 서버가 하는 일과 정확히 같은 구조.

```text
[ 방법 A: 혼자 일하는 카페 ]  = iterative server
  바리스타 1명. 손님 올 때마다 처음부터 끝까지. 줄 길어짐.

[ 방법 B: 손님마다 바리스타 고용 ]  = per-request thread
  손님 올 때마다 면접+채용+훈련 -> 음료 만들고 -> 해고.
  낭비 심함 (스레드 생성 비용).

[ 방법 C: 바리스타 팀 N명 상시 대기 ]  = thread pool
  미리 뽑아 놓음. 주문지는 대기판에 쌓임.
  빈 바리스타가 대기판에서 한 장 떼서 만듦.
  한 명이 바쁜 동안 다른 명이 다른 주문 처리.
```

이번 주 과제가 택하는 게 방법 C. 역할 매핑:

- **main 스레드** = 매니저. 손님 맞이(`accept`) 해서 주문지(`connfd`) 쓰고 대기판에 꽂음.
- **worker 스레드** = 바리스타들. 대기판에서 주문지 떼서 음료 만듦(`doit`).
- **대기판** = job queue. main 이 쓰고 worker 가 읽는 **공유 자료구조**.

```text
                   ┌────── 대기판 (job queue) ──────┐
                   │   [fd5] [fd6] [fd7] [fd8]      │
                   └──┬─────────────────────────────┘
                      │
  main (매니저)        │              worker 1..N (바리스타들)
  accept -> enqueue    │              dequeue -> doit -> close
```

> 왜 락이 필요한가. race condition 이 실제로 어떻게 나는가.

단일 스레드면 문제가 없다. **여러 스레드가 같은 자료구조를 동시에 만질 때** 문제가 난다. 두 가지 장면으로 보자.

**장면 1 — 대기판 사고**:

```text
매니저 (main):  위치 5에 주문지 A 꽂기
바리스타 1:     위치 5에서 주문지 떼내기       <- 동시에!
바리스타 2:     위치 5에서 주문지 떼내기       <- 또 동시에!

결과: 절반만 쓴 주문지를 가져가거나, 한 종이를 둘이 찢거나,
      큐의 front/rear 인덱스가 망가짐.
```

**장면 2 — 단순한 `count++` 조차 race**:

C 의 `count++` 은 CPU 에서 **세 개 명령**으로 쪼개진다.

```text
① load   count  -> 레지스터
② add    1       -> 레지스터
③ store  레지스터 -> count

두 스레드가 100 -> 102 로 올리려 할 때:
  A: load  (reg=100)
  B: load  (reg=100)      <- A 가 쓰기 전!
  A: store (count=101)
  B: store (count=101)    <- 한 번의 증가가 사라짐

두 번 ++ 했는데 결과는 +1.
```

이게 **race condition**. 해결: 해당 코드 블록을 한 번에 한 스레드만 지나가게 **직렬화**. 그걸 하는 도구가 락.

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int count = 0;

void *worker(void *arg) {
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&lock);
        count++;                     /* 이 한 줄이 "원자적" 으로 실행 */
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
```

> 뮤텍스와 조건 변수, 세마포어의 차이.

**뮤텍스 (mutex)** = "한 번에 하나만". 카페의 대기판 앞 자물쇠. lock -> 임계 구역 -> unlock.

**조건 변수 (condvar)** = "할 일 없으면 자고 있어, 일 생기면 깨워줘". 뮤텍스와 함께 쓰임.

바리스타가 대기판을 봤는데 주문지가 없으면:

**나쁜 방법 — busy wait**:
```c
while (1) {
    lock();
    if (queue not empty) { take; unlock; work; }
    else                 { unlock; /* 바로 while 재시작 */ }
}
```
-> 일 없어도 CPU 100% 돌림. 전기 낭비.

**좋은 방법 — condvar**:
```c
lock();
while (queue empty)
    cond_wait(&not_empty, &lock);   /* 잠듦. "주문 오면 깨워" */
take_item();
unlock();
```

`cond_wait` 이 하는 세 단계:
1. 락을 **풀고** (다른 사람이 들어올 수 있게)
2. 이 스레드를 **잠재움** (CPU 안 씀)
3. 깨어나면 **다시 락을 잡고** 반환

main 이 주문지를 넣을 때:
```c
lock();
put_item();
cond_signal(&not_empty);   /* 자는 바리스타 하나 깨움 */
unlock();
```

**주의**: `while (조건) cond_wait` — 반드시 `if` 아닌 `while`. spurious wakeup(무작위 깨어남) 이나 다른 스레드가 먼저 집어가는 경우에 조건이 여전히 틀릴 수 있다.

**세마포어 (semaphore)** = "카운트 가능한 락". P(내리기, 0이면 대기), V(올리기, 자는 사람 깨움).

CSAPP 의 `sbuf` 는 세마포어 3개로 대기판을 구현한다.

```c
typedef struct {
    int *buf;          /* 순환 버퍼 */
    int n, front, rear;
    sem_t mutex;       /* 상호 배제용 (0/1) */
    sem_t slots;       /* 빈 슬롯 수 (초기 n) */
    sem_t items;       /* 채워진 슬롯 수 (초기 0) */
} sbuf_t;

void sbuf_insert(sbuf_t *sp, int item) {    /* producer */
    P(&sp->slots);                           /* 빈칸 있나? 없으면 대기 */
    P(&sp->mutex);
    sp->buf[(++sp->rear) % sp->n] = item;
    V(&sp->mutex);
    V(&sp->items);                           /* "아이템 생겼다" */
}

int sbuf_remove(sbuf_t *sp) {                /* consumer */
    int item;
    P(&sp->items);                           /* 아이템 있나? 없으면 여기서 sleep */
    P(&sp->mutex);
    item = sp->buf[(++sp->front) % sp->n];
    V(&sp->mutex);
    V(&sp->slots);                           /* "빈칸 생겼다" */
    return item;
}
```

세 개가 하는 일:
- `mutex` : 버퍼의 front/rear 필드 동시 접근 차단
- `items` : "읽을 게 있다" 통지
- `slots` : "쓸 공간이 있다" 통지

뮤텍스+condvar 로도 같은 걸 할 수 있지만, 세마포어는 "카운터 하나로 끝" 이라 더 간결하다.

> thread-safe / reentrant / thread-unsafe 는 무엇이 다른가.

**thread-unsafe** 함수:
- 내부에 **전역 static 버퍼**를 쓴다. 두 스레드가 동시에 부르면 서로 덮어씀.
- 대표적: `strtok`, `localtime`, `asctime`, `gethostbyname`, `rand` (구형 glibc).
- 스레드 풀에서는 **절대 금지**.

**thread-safe** 함수:
- 내부에 락이 있거나 공유 상태를 안 씀. 동시 호출 안전.
- 대표적: `malloc` (glibc 내부 락), `printf` (FILE 락), `pthread_*`, `getaddrinfo`.

**reentrant** 함수:
- thread-safe 의 더 엄격한 버전. 시그널 핸들러 안에서도 안전.
- 공유 상태를 **전혀 안 쓰고**, 인자로 모든 상태를 받음.
- 대표적: `strtok_r`, `localtime_r`, `readdir_r`.

실전 규칙: **스레드 풀에서 `_r` 접미사 함수만 써라**.

```c
/* [X] 위험 */
char *p = strtok(str, " ");

/* [x] 안전 */
char *save;
char *p = strtok_r(str, " ", &save);
```

다음은 스레드 풀에서 자주 걸리는 지뢰:

| 안 됨 | 안전한 대체 |
|---|---|
| `strtok` | `strtok_r` |
| `localtime`, `gmtime` | `localtime_r`, `gmtime_r` |
| `asctime`, `ctime` | `asctime_r`, `ctime_r` |
| `gethostbyname` | `getaddrinfo` |
| `rand` | `rand_r` 또는 `random_r` |
| `readdir` | `readdir_r` (리눅스에선 readdir 도 안전해짐) |

`errno` 는 걱정 안 해도 됨 — POSIX 에서 **per-thread 변수**로 정의되어 있다.

> 데드락은 어떻게 나고, 어떻게 피하나.

**데드락 = 서로가 서로를 영원히 기다리는 상태**.

카페 예시: 바리스타 둘이 "우유통" 과 "초콜릿통" 을 **둘 다** 써야 하는 메뉴를 만든다. 각 통은 한 명만 쓸 수 있다.

```text
바리스타 1: 우유통 잡음 -> 초콜릿통 기다림
바리스타 2: 초콜릿통 잡음 -> 우유통 기다림
   -> 둘 다 영원히 멈춤
```

**회피법 = 락 획득 순서 고정**:

```text
규칙: 무조건 "우유통 먼저, 초콜릿통 나중" 순서로만 잡는다.

바리스타 1: 우유통 -> 초콜릿통 -> 사용 -> 둘 다 풀기
바리스타 2: (1 이 풀 때까지 대기) -> 우유통 -> 초콜릿통 -> ...
```

코드로는 보통 "주소 순서" 를 규칙으로 쓴다.

```c
void safe_acquire_two(pthread_mutex_t *a, pthread_mutex_t *b) {
    if (a < b) {
        pthread_mutex_lock(a);
        pthread_mutex_lock(b);
    } else {
        pthread_mutex_lock(b);
        pthread_mutex_lock(a);
    }
}
```

**예방 원칙 4가지**:

1. 가능한 한 **락 하나**만 잡아라.
2. 꼭 여러 개 잡아야 하면 **모든 스레드가 동일한 순서**로.
3. 락 안에서 **오래 걸리는 일(네트워크/디스크 I/O)** 금지. 락 지역을 최소화.
4. 락 안에서 **새 락을 잡는 중첩** 피해라 (어쩔 수 없으면 순서 문서화).

**RWLock (읽기-쓰기 락)** — 읽기가 압도적일 때 성능 이득:

```c
pthread_rwlock_t cache_lock;

/* 캐시 조회 (여러 명 동시 OK) */
pthread_rwlock_rdlock(&cache_lock);
item = cache_lookup(url);
pthread_rwlock_unlock(&cache_lock);

/* 캐시 업데이트 (혼자만) */
pthread_rwlock_wrlock(&cache_lock);
cache_insert(url, item);
pthread_rwlock_unlock(&cache_lock);
```

Proxy Lab 의 캐시, 설정 테이블, DNS 캐시처럼 **read : write = 100 : 1** 같은 경우 뮤텍스보다 훨씬 빠르다.

**atomic 연산** — 카운터 같이 단일 변수일 땐 락 없이:

```c
#include <stdatomic.h>
atomic_int request_count = 0;
atomic_fetch_add(&request_count, 1);   /* 락 없이 안전하게 ++ */
```

GCC 내장 `__atomic_fetch_add` 도 같은 역할.

> 이번 주 SQL API 서버에서 실제로 어디에 락이 필요한가.

요청 처리 흐름과 각 지점의 공유 자원:

```text
클라 ──HTTP──> 서버
                │
                ① main 스레드: accept 루프
                │   └─ sbuf_insert(&jobs, connfd)   <── 세마포어 3개로 보호
                v
           ┌─ job queue ─┐
           │  [f1][f2]   │
           └─────┬───────┘
                 │ sbuf_remove
   ┌─────────────┼─────────────┐
   v             v             v
  worker1     worker2       workerN
   │             │             │
   ├─ HTTP 파싱
   ├─ SQL 추출
   ├─ DB 커넥션 풀에서 획득    <── 뮤텍스 + condvar (pool_get)
   ├─ SQL 실행 (blocking)
   ├─ 결과 받음
   ├─ DB 커넥션 반납            <── 뮤텍스 + condvar (pool_put)
   ├─ 통계 카운터 ++            <── atomic
   ├─ (필요시) 캐시 조회/갱신    <── RWLock
   ├─ 로그 기록                <── 뮤텍스 or per-thread 버퍼
   ├─ HTTP 응답 write
   └─ close(connfd)
```

**① Job queue (connfd 큐)** — CSAPP sbuf 그대로:

```c
sbuf_t jobs;
sbuf_init(&jobs, QUEUE_SIZE);

/* main */
sbuf_insert(&jobs, connfd);

/* worker */
int connfd = sbuf_remove(&jobs);   /* 큐 비면 자동 sleep */
```

**② DB 커넥션 풀** — 뮤텍스 + condvar 로 생산자-소비자:

```c
typedef struct {
    DBConn *conns[POOL_SIZE];
    int count;
    pthread_mutex_t lock;
    pthread_cond_t  available;
} ConnPool;

DBConn *pool_get(ConnPool *p) {
    pthread_mutex_lock(&p->lock);
    while (p->count == 0)
        pthread_cond_wait(&p->available, &p->lock);
    DBConn *c = p->conns[--p->count];
    pthread_mutex_unlock(&p->lock);
    return c;
}

void pool_put(ConnPool *p, DBConn *c) {
    pthread_mutex_lock(&p->lock);
    p->conns[p->count++] = c;
    pthread_cond_signal(&p->available);
    pthread_mutex_unlock(&p->lock);
}
```

**③ 로그** — 뮤텍스 한 번 + `fflush`:

```c
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void safe_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&log_lock);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}
```

또는 **라인 단위 atomic write** 트릭: 한 번에 `write()` 시스템콜 하나로 PIPE_BUF(4096B) 이하면 POSIX 상 atomic 보장.

```c
char line[1024];
int n = snprintf(line, sizeof line, "[%ld] %s\n", time(NULL), msg);
write(log_fd, line, n);    /* PIPE_BUF 이하면 atomic */
```

**④ 캐시 (Proxy Lab 스타일)** — RWLock:

```c
pthread_rwlock_t cache_lock;

CacheItem *cache_find(const char *url) {
    pthread_rwlock_rdlock(&cache_lock);
    CacheItem *it = lookup(url);
    if (it) it->lru_time = time(NULL);  /* 갱신은 atomic 으로 */
    pthread_rwlock_unlock(&cache_lock);
    return it;
}

void cache_insert(const char *url, void *data, size_t size) {
    pthread_rwlock_wrlock(&cache_lock);
    evict_lru_if_full(size);
    add_entry(url, data, size);
    pthread_rwlock_unlock(&cache_lock);
}
```

**⑤ 통계 카운터** — atomic:

```c
atomic_long total_requests = 0;
atomic_long total_errors   = 0;

atomic_fetch_add(&total_requests, 1);
```

**⑥ 시그널 처리** — 메인 스레드만 받게:

```c
/* main 에서, worker 들 만들기 전에 */
sigset_t mask;
sigemptyset(&mask);
sigaddset(&mask, SIGINT);
sigaddset(&mask, SIGTERM);
pthread_sigmask(SIG_BLOCK, &mask, NULL);
/* 이제 만든 worker 들은 이 시그널 무시 */

/* main 만 시그널 받음 — sigwait 로 동기 처리 */
```

추가로 `SIGPIPE` 는 서버에서는 반드시 무시:
```c
signal(SIGPIPE, SIG_IGN);     /* 끊긴 소켓에 write -> EPIPE 로 받기 */
```

> 전체 코드 스케치를 보여달라.

```c
#define NWORKERS    8
#define QUEUE_SIZE  64

sbuf_t jobs;

void *worker_thread(void *arg) {
    while (1) {
        int connfd = sbuf_remove(&jobs);   /* 큐 비면 여기서 sleep */
        doit(connfd);                       /* HTTP 처리, DB 쿼리 등 */
        close(connfd);
    }
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int listenfd = Open_listenfd(argv[1]);
    sbuf_init(&jobs, QUEUE_SIZE);

    for (int i = 0; i < NWORKERS; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, worker_thread, NULL);
        pthread_detach(tid);               /* 자동 회수, join 불필요 */
    }

    while (1) {
        struct sockaddr_storage cliaddr;
        socklen_t len = sizeof cliaddr;
        int connfd = Accept(listenfd, (SA *)&cliaddr, &len);
        sbuf_insert(&jobs, connfd);
    }
}
```

**실전 체크리스트**:

- [ ] 공유 자료구조(큐, 풀, 캐시, 로그)마다 **어떤 락**을 쓸지 결정했나
- [ ] 락 안에서 blocking I/O 안 하나 (network/disk)
- [ ] 두 락 잡는 코드 있으면 **순서 규칙** 정했나
- [ ] `strtok`, `localtime` 등 **unsafe 함수** 안 쓰나
- [ ] worker 는 `pthread_detach` 해서 리소스 자동 회수하나
- [ ] `SIGPIPE` 무시했나
- [ ] `connfd` 를 정확히 한 번만 close 하나 (worker 가 책임)
- [ ] `FD_CLOEXEC` 플래그 고려했나 (exec 안 해도 안전장치)

> 스레드 풀을 더 키우면 무한정 빨라지나.

아니다. 세 가지 상한이 있다.

1. **CPU 코어 수** : CPU 바운드면 코어 수 이상은 스왑만 늘어남.
2. **락 경합** : 워커 늘수록 큐/풀 락 경쟁 증가. 처리량이 정체 후 오히려 감소.
3. **메모리** : 스레드당 기본 스택 8MB. 1000개 = 8GB. `pthread_attr_setstacksize` 로 줄일 수 있음.

경험적으로 I/O 바운드 서버는 **CPU 코어 수 × (2~4)** 정도가 sweet spot. DB 쿼리가 길면 더 늘려도 됨. 수만 연결이면 스레드 풀 포기하고 epoll 로.

## 연결 키워드

- [00-topdown-walkthrough.md §22 — Iterative -> Concurrent](./00-topdown-walkthrough.md#22-iterative--concurrent--스레드-풀-epoll-io_uring)
- [q16-thread-pool-async.md — 스레드 풀 vs epoll vs io_uring](./q16-thread-pool-async.md)
- [q09-network-cpu-kernel-handle.md — CPU/메모리/커널/핸들 관점](./q09-network-cpu-kernel-handle.md)
- CSAPP 12장 — Concurrent Programming
- `man 3 pthread_mutex_lock`, `man 3 pthread_cond_wait`, `man 7 pthreads`
