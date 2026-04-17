# Q14. Thread Pool, async I/O, 시스템콜 관점의 concurrent server

> CSAPP 11 확장(12장 연계) | 동시성 서버 | 심화

## 질문

1. Tiny 는 iterative 서버인데, 실제 서버는 스레드 풀을 쓴다. 각 스레드가 어떻게 네트워크 I/O 를 "동시에" 처리하는가.
2. 이 과정을 CPU / 메모리 / 커널 / 핸들 / 시스템콜 관점에서 다시 설명해 달라.
3. async I/O (epoll, io_uring) 는 스레드 풀과 무엇이 다른가.

## 답변

### 최우녕

> Tiny 는 iterative 서버인데, 실제 서버는 스레드 풀을 쓴다. 각 스레드가 어떻게 네트워크 I/O 를 "동시에" 처리하는가.

동시성 서버의 기본 아이디어는 세 가지다.

- **요청마다 프로세스/스레드를 만든다** (per-request). 단순하지만 생성 비용이 비싸다.
- **스레드 풀** (thread pool). 워커 N개를 미리 만들어 두고 작업 큐에서 꺼내 처리한다.
- **이벤트 루프 + 소수 스레드** (reactor pattern). 한 스레드가 epoll 로 수천 개 소켓을 지켜보며 "준비된 것" 만 처리한다.

이번 주 SQL API 서버에서 쓰는 구조는 **"main 이 accept, worker 풀이 처리"** 하는 전통적 스레드 풀이다.

```text
                   ┌────────────── job queue ───────────────┐
                   │  fd=4   fd=5   fd=6   fd=7   fd=8      │
                   └──┬─────────────────────────────────────┘
                      │
    main thread       │              worker threads (N 개)
    ─────────         │              ─────────────────────
    listenfd = open_listenfd(port)
    while (1) {
        connfd = accept(listenfd, ...);   ── enqueue ──▶  (condvar wait) ── dequeue ─▶
        sbuf_insert(&sbuf, connfd);                         doit(connfd)
    }                                                       close(connfd)
```

- **mutex** 로 큐를 보호하고 **condvar** 로 "비었을 때 기다리고, 채워지면 깨우기" 를 한다.
- 각 worker 는 **독립적으로 `read/write`** 를 호출한다. 하나가 blocking 되어도 다른 worker 가 계속 일한다.

이 구조의 장점은 **"I/O 블로킹을 스레드 개수만큼 병렬화"** 하는 것이고, 단점은 **연결이 스레드 개수를 초과하면 대기** 가 생긴다는 것. 그래서 대규모 서비스는 이벤트 루프로 이동한다.

> 이 과정을 CPU / 메모리 / 커널 / 핸들 / 시스템콜 관점에서 다시 설명해 달라.

**CPU 관점**

- 스레드 개수 = 동시에 RUNNING 가능한 논리 코어 수의 상한(대강). 물리 코어 > 스레드 면 낭비, < 이면 컨텍스트 스위치 비용.
- 블로킹 read 에서 sleep 하는 동안엔 CPU 를 소모하지 않는다 — 그래서 I/O 바운드 서비스에선 스레드 수 > 코어 수가 일반적이다.
- 워커 간 캐시 친화성은 **작업이 어느 스레드로 갈지 제어**할수록 좋아진다 (`SO_REUSEPORT`, affinity).

**메모리 관점**

- 각 스레드는 자기 스택을 가진다(기본 8MB). 스레드 수 × 8MB = 스레드 풀의 최소 메모리 오버헤드.
- 공유 자료구조(캐시, B+Tree, 버퍼)는 **DRAM 의 같은 페이지**를 여러 스레드가 접근 → false sharing, 캐시 무효화 주의.
- `sk_buff` 는 커널이 할당. 연결 폭증 시 slab 단편화.

**커널 관점**

- `pthread_create` 는 Linux 에서 `clone(CLONE_VM | CLONE_FS | CLONE_FILES | ...)` 시스템콜이다. 주소 공간/파일 테이블을 공유하는 경량 프로세스를 만든다.
- 뮤텍스는 대부분 **futex** 시스템콜로 구현된다. 경합 없을 땐 유저 공간에서 atomic 만으로 끝나고, 경합 있을 때만 커널이 깨운다.
- condvar 의 `wait/signal` 도 futex 위의 프리미티브.

**핸들 관점**

- `accept()` 가 돌려주는 connfd 는 **per-process** 이다. 모든 스레드가 공유한다.
- 그래서 worker 스레드가 `read(connfd, ...)` 를 그대로 호출할 수 있다.
- close 는 **참조 카운트**로 관리되므로 worker 가 close 를 책임진다(main 은 dispatch 만 하고 손을 뗀다).
- fd 제한(`RLIMIT_NOFILE`) 을 넘기면 accept 가 `EMFILE` 로 실패. 대규모 서버는 `ulimit -n` 올리기.

**시스템콜 관점**

```text
main:
   accept  →  sys_accept4  (listen 큐 → 새 sk, 새 file, 새 fd)
   (queue push 는 뮤텍스 덕분에 syscall 없을 수 있음 → pthread 는 futex 만)

worker:
   pthread_cond_wait  →  futex(FUTEX_WAIT) → 블록
   (job 들어오면 signal → futex(FUTEX_WAKE) → worker 깨어남)
   read  →  sys_read  → sock_read_iter → tcp_recvmsg  (큐에 없으면 블록)
   write →  sys_write → sock_write_iter → tcp_sendmsg
   close →  sys_close → sock_release → ... FIN 전송
```

정리하면 **"blocking I/O + 여러 스레드"** 전략은 커널에게 "네가 알아서 block 해줘" 라고 위임하는 방식이다. 간단하지만 스레드 수가 많아지면 커널 스케줄링과 메모리가 부담.

> async I/O (epoll, io_uring) 는 스레드 풀과 무엇이 다른가.

핵심 차이는 "**block 을 유저가 대신 관리하는가, 커널이 알아서 하는가**" 이다.

```text
              스레드 풀 (blocking I/O)              epoll reactor (async I/O)
─────────────────────────────────────────────────────────────────────────────
연결 N개      스레드 N개 (또는 풀 사이즈)            스레드 1~ few
프로그래밍    read/write 그냥 부르면 됨              event 루프 + fd 상태 관리
블록 동안     커널이 스레드를 sleep                   스레드는 "준비된 fd" 만 처리
스케일        수천~만 connections 까지               수만~수십만 connections
대표 예       전통적 Apache, Tomcat                   nginx, Node.js, Netty, Go runtime
구현 시스템콜 read/write/pthread/futex                epoll_create/epoll_ctl/epoll_wait
              (커널이 block 대신)                     + read/write (non-blocking)
복잡도        코드가 단순                             상태 머신 작성이 필요 (콜백/async-await)
```

epoll 의 동작을 간단히:

```c
int ep = epoll_create1(0);
struct epoll_event ev = { .events = EPOLLIN, .data.fd = listenfd };
epoll_ctl(ep, EPOLL_CTL_ADD, listenfd, &ev);

struct epoll_event events[MAX];
while (1) {
    int n = epoll_wait(ep, events, MAX, -1);     /* 블록 */
    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        if (fd == listenfd) {
            int connfd = accept4(listenfd, ..., SOCK_NONBLOCK);
            epoll_ctl(ep, EPOLL_CTL_ADD, connfd, &ev_in);
        } else {
            handle_request(fd);    /* non-blocking read/write */
        }
    }
}
```

`io_uring` 은 더 나아가 **"시스템콜 자체를 배치로 커널에 맡긴다"**. 링 버퍼 두 개(submission/completion)에 "이런 read, 저런 write 해줘" 를 쓰면 커널이 알아서 돌리고 결과만 link 해 놓는다. 컨텍스트 스위치가 거의 사라진다. 단, 프로그래밍 모델이 더 복잡하다.

우리 코드 레벨에서 의사 결정은 이렇게 하면 된다.

- 연결 수 ≤ 수천, 코드 단순성 우선 → **스레드 풀 + blocking I/O** (= Tiny 확장 방식). SQL API 서버는 이걸로 충분.
- 연결 수 수만 이상, 레이턴시 민감 → **epoll/kqueue reactor** 또는 언어 런타임의 async.
- 극한 성능 필요, 커스텀 인프라 → **io_uring / DPDK / 유저공간 TCP**.

결국 "동시성" 은 **커널에게 block 을 위임할지, 유저가 상태 기계로 전부 관리할지** 를 고르는 문제다. 이 선택이 곧 CPU/메모리/커널/핸들 비용의 분포를 결정한다.

## 연결 키워드

- [02-keyword-tree.md — SQL API Server / Proxy Lab concurrent](../../csapp-11/02-keyword-tree.md)
- q10. CPU/메모리/커널/핸들 관점
- q12. Tiny 의 iterative 루프
- q13. Proxy 의 concurrent 확장
