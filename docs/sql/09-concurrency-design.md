# Mini DBMS API Server — 동시성 설계 보고서

> 작성일: 2026-04-22 · 대상 프로젝트: Krafton Jungle W08 SQL API Server
> 선행 문서: `08-wk07-minidb-to-wk08-sql-api-server.md`
> 본 문서의 목적: **"클라이언트가 TCP 로 접속해서 SQL 을 던지고 결과를 받기까지"** 그 한 왕복 안에 어떤 동시성 문제가 어느 지점에서 끼어드는지, 그리고 현대 DB/서버는 그걸 어떻게 푸는지 — 개념 위주로 정리. 실제 구현에 들어가기 전에 **"왜 이게 필요한지, 적용할지 말지"** 를 판단할 수 있도록.

---

## 0. 개요

### 0.1 이 문서가 다루는 것

요청 하나는 "TCP 연결 → accept → 쓰레드에 배치 → 파싱 → 락 확보 → B+ Tree 조회/수정 → 버퍼 풀 → WAL → 응답" 이라는 긴 파이프라인을 타고 내려간다. 이 파이프라인을 **두 개의 수직축** 으로 쪼갠다.

1. **서버 입장의 상위 레이어 (L1 ~ L5)** — 서버 공통 문제. Tomcat 이든, Nginx 든, 우리 미니 DBMS 든 공통으로 겪는 문제. CSAPP 11, 12 장 내용과 정확히 대응.
2. **DB 엔진 내부 레이어 (D1 ~ D7)** — SQL 서버만의 고유 문제. B+ Tree, Lock Manager, WAL, MVCC 등. 여기가 본 과제의 핵심.

두 축이 **만나는 지점** 이 이번 과제다. API 서버의 Thread Pool (L2) 이 내부 B+ Tree (D2) 를 동시에 건드린다.

### 0.2 범위 / 비범위

- **범위**: 단일 머신 내 멀티쓰레드 동시성. 안 A (단일 프로세스) 를 기본, 안 B (프록시 + SQL 서버 2 프로세스) 를 확장으로.
- **비범위**: 분산 (복제, 샤딩, Paxos/Raft) · MVCC 완전 구현 · WAL 전체 (ARIES).

### 0.3 난이도·효과 표기법

각 해결책 옆에 다음 셋을 붙인다.

- **난이도** ★ 1 ~ 5. 숙련 개발자가 처음부터 설계·구현·디버깅하는 데 걸리는 순공수. ★ = 반나절, ★★★ = 2~3 일, ★★★★★ = 2 주+.
- **효과** `(정/처/지)` — 정합성/처리량/지연 개선을 각 0~10 점. 예: `(9/3/2)` = 버그는 확실히 막지만 처리량·지연엔 조금만 도움.
- **ROI** = (효과 평균) ÷ (난이도 별 수). 2 이상이면 MVP 포함 권장, 1 미만이면 학습용.

### 0.4 개발 전에 빠르게 읽는 순서

문서가 길기 때문에 개발 직전에 전부 처음부터 읽기보단 아래 순서로 보는 게 좋다.

1. **1장, 2장** — 이번 과제 전체 구조를 잡기
2. **3장의 L2, L3** — 쓰레드풀과 공유 메모리 문제 이해
3. **4장의 D2, D3, D4, D5** — B+ Tree, 버퍼 풀, 락 매니저, 데드락
4. 마지막에 **권장안 / 비범위** 만 다시 보고 구현 범위 확정

즉 "모든 이론"보다 먼저
"우리 코드에서 어디가 깨질 수 있는가"를 중심으로 읽는 게 좋다.

---

## 1. 아키텍처 — 안 A / 안 B

### 1.1 안 A — 단일 프로세스 (In-Process)

한 실행 파일 안에 accept 스레드, 워커 쓰레드 풀, B+ Tree 엔진, 락 매니저가 모두 함께 산다. 워커 쓰레드는 엔진 함수를 **직접 호출** 한다. 네트워크 홉이 없고 동시성 이슈가 한 주소 공간에 몰려 있어 **학습/디버깅에 최적** — ThreadSanitizer 같은 도구가 바로 붙는다. 대신 한 쓰레드의 SEGV 가 전체 프로세스를 죽인다.

### 1.2 안 B — 프록시 + SQL 서버 (2 프로세스)

앞단에 **프록시** 가 있고, 뒤에 **SQL 서버** 가 있다. 두 프로세스가 TCP 로 통신. 프록시가 커넥션 풀, TLS 종단, 인증, 레이트 리밋, 서킷 브레이커를 맡고, SQL 서버는 순수 DB 엔진에 집중. **장애 격리와 연결 관리가 핵심 이익**. 대신 IPC 1 홉 비용으로 p99 지연이 수백 µs 증가.

### 1.3 "둘 다 계획에 포함" 의 의미

Step 1~4 에선 안 A 를 빌드한다. Step 5 (선택) 에서 안 A 를 감싸는 얇은 프록시 한 장을 추가해 안 B 로 전환. 이걸 쉽게 만들려면 안 A 단계에서 미리 해둘 장치가 있다.

1. 요청/응답을 **직렬화 가능한 포맷** (고정 헤더 + payload) 으로 정의. 안 A 에선 메모리 안 객체지만 그 레이아웃을 TCP 로 그대로 보내면 안 B.
2. "요청 하나 → 응답 하나" 의 **단일 엔트리 함수** 를 엔진 경계로. 바깥이 이 함수만 호출한다.
3. 요청 헤더에 **세션 ID, 데드라인, 멱등키 자리** 를 처음부터 확보.
4. `fd = 세션` 이라는 가정을 피하고, 세션 ID 로 추상화.

이렇게 만들어두면 안 B 전환은 "그 단일 함수를 recv 루프로 감싸기" + "프록시 앞에 붙이기" 둘 뿐이다.

### 1.4 난이도·효과

| 안 | 난이도 | 얻는 이익 | 잃는 것 |
| --- | --- | --- | --- |
| 안 A | ★★★ (2~3 일) | 동시성 이슈 학습·디버깅 최적 | 장애 격리 없음 |
| 안 B (프록시 추가) | ★★★ (2~3 일 추가) | 격리 · 커넥션 풀 · 서킷 브레이커 | IPC 비용 · 프로토콜 정의 부담 |

---

## 2. 전체 흐름 (End-to-End) — 안 A

"`BEGIN; INSERT; COMMIT;`" 한 세트가 들어왔을 때 안 A 의 서버 안에서 실제로 무슨 일이 일어나는지 따라간다. 괄호 안 `[Lx]` / `[Dy]` 는 Part 3/4 의 어느 레이어에서 동시성 이슈가 끼어드는지 표시.

```
 [Client]
    │
    │ 1. TCP SYN → 3-way handshake
    │    커널이 accept 큐에 올려둔다                 ← [L1] 소켓
    ▼
 [Main Thread]
    │  accept(listen_fd)       꺼냄
    │  work_queue.push(fd)                           ← [L2] 쓰레드풀 / 큐
    │  (큐 가득 → admission control 로 거절)
    ▼
 [Worker Thread]
    │  recv(fd) → "BEGIN"
    │  parse()                                       ← [D1] 파서 (무상태)
    │  txn_begin() → TxnTable.add                    ← [L3] 공유 메모리
    │    ├ txn_id = 7 발급 (atomic 증가 or mutex)
    │    └ TxnTable 은 여러 스레드가 건드리는 공유 자료구조
    │  send(fd, "OK txn=7")
    │
    │  recv(fd) → "INSERT INTO t VALUES (5, 'a')"
    │  parse()
    │  lm_acquire(row=key5, X)                       ← [D3] Lock Manager
    │    ├ 다른 txn 이 같은 row 에 락 쥐고 있나?
    │    │   └ yes → cond_wait → [D4] Deadlock 탐지 대상
    │    └ no → grant
    │  btree_insert(t, 5, 'a')                       ← [D2] B+ Tree
    │    ├ root → child → leaf 내려가며 래치 이동 (Lock Coupling)
    │    └ 리프 split 발생 시 부모까지 래치 유지
    │  bp_fetch(leaf_page)                           ← [D5] Buffer Pool
    │    ├ hash bucket mtx → 있으면 pin++, 없으면 victim LRU
    │    └ pin 과 latch 는 별개 의미
    │  wal_append(redo: put k=5)                     ← [D6] WAL
    │  send(fd, "OK")
    │
    │  recv(fd) → "COMMIT"
    │  wal_append(commit marker) + fsync
    │  lm_release_all(txn=7)
    │    └ 대기 중인 다른 txn 들 cond_signal 로 깨움
    │  send(fd, "OK")
    ▼
 [Client]
    │ close()
    ▼
 (Worker 는 다음 요청 대기 or 다음 세션으로)
```

## 2.5 전체 흐름 (End-to-End) — 안 B

안 B 에선 프록시가 L1~L4 대부분을 흡수한다. SQL 서버 내부 (D1~D7) 는 안 A 와 동일.

```
 [Client]                       [Proxy Process]                        [SQL Server Process]
    │                                │                                         │
    │--- connect() ---------------->│  accept, TLS, auth                       │
    │                                │  session Sx 등록                          │
    │                                │                                          │
    │--- "BEGIN" ------------------>│  upstream pool_acquire()                 │
    │                                │    ├ 여유 있으면 fd 하나 빼서 Sx 에 pin  │
    │                                │    └ 없으면 cond_wait (풀 상한)           │
    │                                │  frame 으로 감싸 전송  ─── TCP ───▶  accept (내부 포트)
    │                                │                                          │  worker, parse, txn_begin
    │                                │                                  ◀─────  "OK txn=7"
    │<------- "OK txn=7" -----------│                                          │
    │                                │  (Sx 의 upstream 은 계속 pin)             │
    │                                │                                          │
    │--- "INSERT ..." -------------▶│  같은 upstream 재사용                      │
    │                                │                          ─── TCP ───▶  동일 worker, 동일 txn
    │                                │                                  ◀─────  "OK"
    │<------- "OK" -----------------│                                          │
    │                                │                                          │
    │--- "COMMIT" -----------------▶│                          ─── TCP ───▶  release_all, fsync
    │                                │                                  ◀─────  "OK"
    │<------- "OK" -----------------│  pool_release(upstream)                   │
    │                                │   └ 다른 세션이 대기 중이면 signal         │
    │--- close() -----------------▶│  세션 Sx 제거                              │
```

이때 안 B 에서 **새로 생기는 동시성 문제** 는 주로 `upstream pool` 주변이다. Part 3 L4 에서 자세히.

---

## 3. 서버 입장 상위 레이어

### L1. 소켓 / TCP 수준

#### 현상 1 — accept 큐 오버플로

서버가 `listen()` 해두면 커널에 두 개의 큐가 생긴다. SYN 큐 (3-way 진행 중) 와 accept 큐 (완성된 연결). `accept()` 가 꺼내 쓰는 건 후자. 트래픽이 몰리거나 워커가 느려 accept 가 밀리면 accept 큐가 꽉 차고, 신규 연결은 드롭되거나 RST 가 되돌아간다. 클라이언트 입장에선 `connection refused` 나 알 수 없는 타임아웃.

```
SYN 폭주                  accept 큐 (크기 = min(backlog, somaxconn))
 ─────▶  [⬜⬜⬜⬜🟥🟥🟥🟥🟥]  ─────▶  app.accept()
                      가득 참, 이후 드랍
```

- **왜 중요한가** — 서버가 "죽은 건 아닌데 안 받는" 상태. 모니터링으로 잡기 어렵고, 클라이언트는 재시도 폭풍을 만든다.
- **해결 방법 비교**

| 방법 | 어떻게 작동하는가 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| backlog 키우기 (`listen(1024+)`) | 큐 상한을 그냥 늘림 | 즉효, 코드 한 줄 | 근본 해결 아님. 워커가 여전히 느리면 누적만 지연 | ★ | 8/2/4 |
| `SO_REUSEADDR` | 재시작 시 `TIME_WAIT` 를 무시하고 bind | 개발 중 필수 | 프로덕션 이점은 작음 | ★ | 5/0/0 |
| `SO_REUSEPORT` | 여러 스레드/프로세스가 같은 포트에 바인드, 커널이 flow-hash 로 분산 | thundering herd 근본 해결 | 리눅스 3.9+/BSD 만 | ★★ | 6/6/5 |
| 앞단 프록시 (Nginx/ALB) | 프록시가 수많은 커넥션을 받아 뒤에 소수로 전달 | 백엔드 부담 분산, TLS 종단 | 홉 추가 | ★★★ | 9/8/7 |
| SYN cookies | SYN 폭주 시 커널이 상태 없이 응답 | SYN flood 공격 방어 | 정상 트래픽엔 무관 | ★ (설정만) | 9/0/0 |

우리 프로젝트: `SO_REUSEADDR` 는 필수 (재시작 편의). `backlog = 1024` 로 시작. `SO_REUSEPORT` 는 accept 스레드가 여럿일 때만 의미, 지금은 불필요.

#### 현상 2 — TIME_WAIT 고갈

TCP 연결을 닫으면 **먼저 닫는 쪽** 이 `TIME_WAIT` 상태로 60 초 정도 소켓을 쥐고 있는다. 이는 "뒤늦게 도착하는 패킷을 다른 새 연결이 오인하지 않게" 하기 위한 안전장치. 짧은 연결을 초당 수천 번 여는 패턴에선 ephemeral port 가 고갈된다 (리눅스 기본 28,000 개 가량). 이게 차면 새 연결 시도가 실패.

- **해결 방법 비교**

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| HTTP Keep-Alive / 장기 커넥션 | 한 연결로 여러 요청 처리 | 근본 해결 | 클라이언트가 협조해야 함 | ★ | 9/8/8 |
| 커넥션 풀 | 클라이언트가 풀로 재사용 | 근본 해결 | 풀 관리 복잡도 | ★★★ | 9/9/8 |
| `tcp_tw_reuse` | 커널이 TIME_WAIT 소켓을 조건부 재사용 | 즉효 | 미세한 오인식 위험, NAT 환경 주의 | ★ | 6/4/3 |
| 포트 범위 확장 | `ip_local_port_range` 늘림 | 즉효 | 근본 해결 아님 | ★ | 5/2/1 |

우리 프로젝트: 커넥션 단위가 "세션" 이라 한 번 열면 여러 요청을 타기 때문에 TIME_WAIT 고갈은 잘 안 난다. 다만 **부하 테스트 도구** 가 짧은 연결을 쏟아내면 테스트 환경에서 고갈 경험할 수 있음.

---

### L2. 애플리케이션 쓰레드 풀

#### 현상 1 — 쓰레드 풀 고갈 (Thread Pool Exhaustion)

쓰레드 풀 크기가 200 이라면 동시 처리 상한도 200. 각 요청이 디스크나 외부 호출에서 오래 블로킹되면 200 개가 전부 대기 상태에 빠진다. 새 요청은 큐에서 기다리다 타임아웃.

```
쓰레드 풀 (200)
[T1: I/O wait] [T2: I/O wait] ... [T200: I/O wait]
                                        │
큐: [req 201, 202, 203, ...]   ← 한 자리도 못 얻음
```

**핵심 통찰** — 느려진 외부 서비스/디스크 하나가 내 서버를 마비시킨다. 이게 **Cascading Failure** 의 출발점. 본 과제에선 "디스크 I/O 가 느릴 때 모든 워커가 blocking read 에 물려 accept 도 뒤에 쌓인다" 는 형태로 발생할 수 있다.

- **해결 방법 비교**

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| 타임아웃 짧게 | blocking call 에 시한 | 쓰레드가 영원히 안 묶임 | 오탐 · 부분 실패 복구 필요 | ★★ | 7/6/7 |
| 비동기 I/O (epoll/io_uring, async/await) | blocking 을 이벤트로 전환 | 쓰레드 수 작게 유지 가능 | 구조 전면 개편, 디버깅 복잡 | ★★★★ | 8/9/9 |
| Bulkhead 패턴 | 외부 호출별/자원별로 풀을 쪼갠다 | 한쪽 장애가 다른 쪽을 안 죽임 | 풀 관리 복잡, 메모리 증가 | ★★★ | 9/7/7 |
| Circuit Breaker | 실패율 높으면 아예 호출 차단 | 장애 확산 방지 | 오픈/하프오픈 상태 관리 필요 | ★★★ | 8/6/7 |
| Queue + admission control | 큐 차면 거절 | 느려지지 말고 거절 | 사용자 경험 저하 | ★ | 7/5/8 |

**Bulkhead 의 개념** — 배의 격벽처럼 풀을 나눈다. "디스크 I/O 전용 풀 30 개, CPU 작업 전용 풀 20 개, 외부 호출 전용 풀 20 개." 디스크가 느려져도 CPU 작업과 외부 호출은 살아 있다. 반대는 "공용 풀" — 하나의 병목이 전체를 집어삼킨다.

**Circuit Breaker 의 세 상태** — CLOSED(정상) / OPEN(차단) / HALF-OPEN(탐색). 실패율이 임계를 넘으면 OPEN 으로 전환, 일정 시간 뒤 HALF-OPEN 에서 몇 건만 시험 호출, 성공하면 CLOSED 복귀. 복구 중인 서비스를 재시도 폭풍으로 다시 때리지 않도록 보호한다.

#### 현상 2 — 이벤트 루프 블로킹

Node.js, Netty 같은 단일 이벤트 루프는 쓰레드 하나가 모든 요청을 번갈아 처리한다. 한 요청이 CPU 를 오래 쓰는 연산 (큰 정렬, 큰 파싱, 암호화) 을 하면 그 순간 서버 전체가 멈춘다.

- **해결** — CPU 작업은 Worker Thread 로 오프로드, 작업을 쪼개어 yield 하기.

본 과제: 우리는 Thread-per-Request 에 가까운 쓰레드 풀 모델이므로 이 문제는 직격이 아니지만, **한 워커가 긴 full-table-scan 을 하는 동안 다른 워커는 여전히 살아있다** 는 점은 쓰레드 풀 모델의 이점.

#### 쓰레드 수 결정 — "많을수록 좋다" 는 착각

- 순수 CPU 바운드: `WORKERS = cores` 근처.
- I/O 바운드: `WORKERS = cores × 2 ~ 8`. 그 이상은 컨텍스트 스위칭 비용이 더 커진다.
- Little's Law: `평균_동시처리 = QPS × 평균지연`. 목표 QPS 1000, 평균지연 10 ms 면 필요 동시처리 10. 초과 여유만 두면 됨.

본 과제: 8 코어 기준 `WORKERS=8` 로 시작. 큐 깊이가 계속 차면 16 으로. 문서화해둘 것 — "워커를 무조건 늘리지 말 것" 이 실수가 가장 많은 포인트.

---

### L3. 공유 메모리 상태

서버가 메모리에 **공유 상태** 를 두는 순간 — 전역 카운터, 캐시, 세션 스토어, Transaction Table, Lock Table — 여러 쓰레드가 동시에 건드린다. CSAPP 12 장의 race condition 이 여기서 일어난다.

#### 현상 1 — Lost Update

```c
// "조회수 +1" 의 순진한 코드
counter++;

// 실제론 3 단계
//   1. counter 를 레지스터로 로드
//   2. +1
//   3. 메모리로 저장
// 두 쓰레드가 1 ~ 2 ~ 3 을 인터리브하면 한쪽의 +1 이 사라짐
```

DB 의 Lost Update 와 **정확히 같은 구조**. 이번엔 공유 자원이 디스크의 row 가 아니라 프로세스의 메모리 한 칸일 뿐.

- **해결 방법 비교**

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| `mutex` / `pthread_rwlock` | 임계구역을 직렬화 | 보편적, 증명된 | 경합 심하면 처리량 제한 | ★ | 10/3/3 |
| `atomic` / CAS | 하드웨어 원자 연산 | 락보다 빠름 | 복합 연산에 부적합 | ★★ | 9/7/5 |
| 쓰레드 안전 자료구조 | 자료구조 안에서 잠금 분산 | 애플리케이션 코드 단순 | 내부 동작 몰라서 과신 위험 | ★★ | 9/7/6 |
| 공유 자체를 줄이기 | immutable / per-thread 복제 | 락이 아예 필요 없음 | 설계 전환 필요 | ★★★ | 9/9/9 |
| 액터 모델 | 상태를 소유한 액터에게 메시지로만 | 확장성 좋음 | 언어 지원 필요 (C 에선 수동 구현) | ★★★★ | 8/8/8 |

#### 현상 2 — 캐시 Thundering Herd

인기 캐시 키가 만료되는 순간 — 동시에 들어온 수천 요청이 모두 캐시 miss 를 맞고, 전부 DB 로 쏟아진다. DB 는 같은 쿼리를 수천 번 중복 처리.

```
T:     cache["hot"] 만료
T+1ms: 요청 1000개 → cache miss → 1000개가 동시에 DB 쿼리
       💥 DB CPU 100%, 커넥션 풀 고갈
```

- **해결** — Single-Flight (첫 한 건만 DB 에, 나머지는 그 결과를 기다린다. Go 의 `singleflight.Group` 이 유명), TTL jitter (만료 시각에 무작위 분산), stale-while-revalidate (만료됐어도 구 값을 잠시 돌려주면서 백그라운드 갱신).

#### 본 과제의 공유 메모리

우리 미니 DBMS 에서 공유 메모리 상태는 **Transaction Table, Lock Table, Buffer Pool, B+ Tree 의 root 포인터, WAL tail 포인터**. 각각을 누가 어떻게 보호하느냐가 Part 4 의 핵심 주제.

---

### L4. DB 커넥션 풀 (주로 안 B 에서 의미)

#### 현상 1 — 풀 고갈

쓰레드는 200 인데 DB 커넥션 풀이 20 이면, 동시에 쿼리하는 21 번째 쓰레드부터는 커넥션을 기다린다. 타임아웃이 나면 HikariCP 가 유명한 메시지 `Connection is not available, request timed out after 30000ms` 를 던진다.

#### 현상 2 — 커넥션 릭

```c
db_conn_t *conn = pool_acquire(&pool);
do_stuff(conn);
/* pool_release(&pool, conn); 누락! → 풀에 영영 안 돌아옴 */
```

하루 지나서 장애. C 에선 `goto cleanup` 패턴이나 반납 래퍼를 써서,
함수 종료 경로마다 `pool_release()` 가 빠지지 않게 만드는 게 중요하다.

#### 현상 3 — 트랜잭션 점유 (악질 중의 악질)

풀에는 돌아왔는데 `BEGIN` 만 한 채 `COMMIT` 없이 반납. 이 커넥션을 다음 사람이 받으면 이전 트랜잭션 스냅샷이 살아 있어, MVCC 의 vacuum 이 진행 불가. PostgreSQL 의 `idle in transaction` 경고가 이 케이스.

#### 풀 크기는 몇이 적절한가

흔한 착각: "크면 클수록 좋다". 실제론 다음 경험 공식이 회자된다.

```
connections ≈ (core_count × 2) + effective_spindle_count
```

출처는 PostgreSQL wiki / HikariCP 문서. 뒤에 `spindle` 은 "동시에 움직일 수 있는 디스크 헤드 수" — SSD 면 1 또는 RAID stripe 수. 이 공식 뒤 논리:

- 한 쿼리는 "CPU 로 생각" 과 "디스크 대기" 를 번갈아 한다. 디스크 대기 중엔 CPU 가 논다. 그래서 CPU 수보다 2 배쯤 커넥션을 주면 겹쳐서 CPU 를 꽉 채운다.
- 그 이상 늘리면 DB 내부 락·IO 대기 큐·컨텍스트 스위칭 비용이 이득을 집어삼킨다.
- Little's Law 로도 검증: `busy_connections = QPS × 평균_쿼리지연`. 예상 QPS 200, 지연 5 ms → 1.0, 동시성 10 이면 충분.

**즉, 풀 크기는 처리량의 상한이 아니라 경합의 하한을 결정한다.** 너무 크면 경합으로 느려지고, 너무 작으면 대기로 느려진다.

#### 해결 방법 비교

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| cleanup label / 반납 래퍼 | 반납을 종료 경로에서 강제 | 릭 원천 차단 | 코드 규율이 필요 | ★ | 10/3/1 |
| 풀 사이즈 튜닝 공식 | 위 공식 | 근거 있는 출발점 | 워크로드별 미세조정 필요 | ★★ | 6/8/8 |
| 풀러 (PgBouncer 류) | 클라이언트 수천 ↔ DB 수십 중간에서 | 결정적 이득 | 세션 변수·트랜잭션 풀링 제약 | ★★★ | 7/10/9 |
| idle-in-transaction 모니터 | 오래 `BEGIN` 만 된 세션 강제 종료 | 장애 차단 | 정상 장기 트랜잭션 오살 위험 | ★★ | 8/5/5 |

**본 과제 적용** — 안 A 에선 커넥션 = fd = 세션이므로 별도 풀이 없다. 안 B 로 확장 시 프록시 쪽에 upstream pool 을 둔다. 풀 크기는 위 공식에 맞춰 4~8 개로 시작.

---

### L5. 분산 / 외부 호출 (본 과제 거의 해당 없음, 참고만)

Retry Storm, Idempotency-Key, 분산 락 — 분산 환경에서의 과제들이다. 우리는 단일 머신이라 **분산 락이 필요한 지점은 없다**. 그러나 안 B 의 프록시 ↔ SQL 서버 통신에 네트워크가 끼므로, **Retry + Idempotency 의 원리** 는 알고 있어야 한다.

- 재시도는 **멱등성이 보장된 요청만**. `INSERT` 같은 요청을 성급히 재시도하면 중복 삽입.
- **Exponential Backoff + Jitter** — 재시도 간격을 `2^n` 으로 늘리되, 랜덤으로 분산. AWS Marc Brooker 의 "Full Jitter" 가 유명. 동시 재시도가 같은 순간에 몰리는 걸 막는다.
- **Idempotency-Key 헤더** — 클라이언트가 요청에 고유 키를 붙이면, 서버는 키별로 첫 결과를 기록해두고, 같은 키 재요청엔 저장된 결과를 그대로 반환. DB 의 `UNIQUE(idempotency_key)` 제약이 이 패턴의 기반.

**해결 방법 비교 요약**

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| Exponential backoff + jitter | 재시도 간격 확장 + 랜덤화 | 재시도 폭풍 완화 | 최종 응답이 오래 걸릴 수 있음 | ★★ | 8/7/6 |
| 재시도 횟수 상한 | 무한 재시도 방지 | 장애 확산 방지 | 실패 노출 | ★ | 8/4/4 |
| Idempotency-Key | 같은 키 재요청엔 같은 결과 | 중복 처리 방지 | 키 저장소·TTL 관리 | ★★★ | 10/3/3 |

본 과제: Step 5 에서 프록시가 SQL 서버에 전달할 때 `deadline` 과 `req_id` 정도만 헤더에 포함. 분산 락은 해당 없음.

---

## 4. DB 엔진 내부 레이어

이제 요청이 엔진 안으로 들어온 뒤다. 여기부턴 SQL 서버만의 고유 동시성 문제.

### D1. SQL 파서 / 플래너

파서와 플래너 자체는 **무상태**. 각 요청이 자기 AST 를 만들어 쓰고 끝나면, 동시성 이슈 없음. 단 함정 하나가 있다 — C 표준 라이브러리의 `strtok` 같은 함수는 내부 상태가 있다. `strtok_r` 로 써야 한다. 전역 `static` 버퍼도 금지.

플랜 캐시를 공유하면 거기서 경합이 생긴다. Oracle 의 `library cache latch` 가 유명한 병목. MySQL 은 그래서 query cache 를 아예 제거. 본 과제는 **플랜 캐시 없이** 시작한다 — 공수 대비 이득이 없다.

- **해결 방법 비교**

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| 무상태 파서 (재진입) | static 금지, 모든 상태를 지역/파라미터로 | 경합 제로 | 주의력 필요 | ★ | 9/5/3 |
| Arena 할당 | 요청당 큰 블록 하나, bump-pointer | malloc 경합 회피 | 메모리 상한 관리 | ★★ | 5/6/4 |
| 세션 로컬 prepared | 세션별 plan 저장 | 재파싱 회피 | 세션 메모리 증가 | ★★★ | 4/7/5 |
| 공유 plan cache | 전역 plan 공유 | 재사용 최대 | latch 경합, 코드 복잡 | ★★★★ | 3/6/3 |

본 과제: 무상태 파서 + Arena 까지만. 나머지는 스킵.

---

### D2. B+ Tree 동시 접근 — **본 과제의 핵심**

#### 현상 — traverse 중 split

스레드 A 가 루트에서 리프로 내려가는 도중, 스레드 B 가 중간 노드를 split 한다. A 는 "있어야 할 키" 를 놓치거나, 심하면 이미 해제된 페이지를 읽는다.

```
 [A: root 에서 child 로 이동 중]
                │
                ▼
    ┌─────────────────────────┐
    │     root                │
    ├────┬────────┬───────────┤
    │ c1 │   c2   │    c3     │   ← A 는 c2 로 갈 예정
    └────┴────┬───┴───────────┘
              │   이 순간 B 가 c2 를 split
              ▼   → c2, c2' 로 갈라짐
         c2 / c2'
       A 가 보는 c2 는 이미 split 전 기준의 포인터.
       키가 c2' 로 이동했다면 A 는 놓친다.
```

#### 해결 방법 비교

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| 1. 트리 전체 단일 rwlock | 트리를 하나의 락으로 감쌈 | 30 분에 끝. 버그 없음 | 쓰기 한 건이 전부 막음. 코어 여러 개 무의미 | ★ | 10/1/1 |
| 2. Lock Coupling (Crabbing) | 부모→자식 래치를 "손에 쥐고 넘어간다" | 업계 표준. 증명 완료 | split 이 부모로 전파되는 경우 역추적 필요 | ★★★ | 9/7/7 |
| 3. Optimistic Crabbing | 처음엔 read latch 로 내려가다 split 없음이 확인되면 write | 읽기 많은 워크로드에 극적 | 틀리면 restart | ★★★★ | 9/9/8 |
| 4. B-link Tree (Lehman-Yao) | 각 노드에 우측 형제 link. split 중에도 링크 따라가서 답 찾음 | 이론상 최고 동시성 | 1978 년 논문 + 수정 다수 필요 | ★★★★★ | 9/10/9 |

#### Lock Coupling 의 동작 개념 (말로만)

"**자식의 래치를 잡은 뒤에야 부모의 래치를 놓는다**" — 이 한 줄이 불변식. 그리고 연산별로 "언제 부모 래치를 놓아도 안전한가" 를 다르게 판정한다.

- **읽기** — 자식 래치를 잡는 순간 항상 안전. 부모를 바로 놓는다. 게(crab) 가 한 발씩 옮기듯 내려가서 "crabbing" 이라 부름.
- **INSERT** — 자식에 여유 공간이 충분해서 **split 이 부모로 전파되지 않을** 것이 분명하면 부모를 놓는다. 그렇지 않으면 부모를 쥔 채로 계속 내려간다. 리프에서 실제 split 이 발생하면, 이미 쥐고 있던 부모에서 바로 split 을 이어간다.
- **DELETE** — 마찬가지로 자식이 삭제 후에도 언더플로 안 날 것이 분명할 때 부모를 놓는다.

왜 이게 정합성을 보장하는가 — 어떤 노드든 "누가 쓰기 중이면 다른 스레드는 읽기/쓰기 둘 다 못 시작한다" 를 rwlock 으로 보장한다. Split 이 진행되는 동안 split 중인 노드는 write-locked 상태. 다른 스레드는 기다린다. Split 이 끝나고 부모의 자식 배열이 업데이트된 뒤에야 락이 풀리고, 뒤따라 내려가는 스레드는 "새 구조" 를 본다. 중간 상태를 아무도 보지 못한다.

**Optimistic Crabbing 과의 차이** — 쓰기도 처음엔 read latch 로 내려간다. 리프에 도달해 "여기 여유 있음, split 불필요" 이면 그 리프만 write-latch 로 업그레이드. 틀리면 전체를 restart (pessimistic 모드로). 읽기 많은 워크로드에선 극적 이득 — PostgreSQL 의 `nbtree` 가 이 방향.

**B-link Tree 와의 차이** — 부모 래치를 아예 거의 안 쓴다. 각 노드가 "오른쪽 형제" 포인터를 갖고 있어서, split 이 막 일어난 순간 부모가 아직 모르더라도 sibling link 를 따라가 답을 찾을 수 있다. 대신 구현이 복잡해서 버그 나면 데이터 손상.

#### 본 과제 적용

- **Step 2: Global rwlock MVP** — 반나절 완성. 이후 벤치의 기준선.
- **Step 3: Lock Coupling 업그레이드** — 2~3 일. MVP 와 비교 벤치해서 "처리량 N 배, 지연 M 배" 를 발표에 담는다. 이 대비가 발표의 핵심 스토리가 될 것.
- Optimistic/B-link 는 본 과제 공수 밖. 확장 아이디어로 문서 말미에만 언급.

---

### D3. Buffer Pool · Page Latch

디스크 기반 DB 에선 B+ Tree 노드가 "필요할 때 디스크에서 읽어 오는 페이지" 다. 여기 Buffer Pool 이라는 중간층이 끼는데, 이 층에서만 따로 생기는 동시성 문제가 있다.

#### 현상들

1. **Eviction race** — 스레드 A 가 페이지 P 를 쓰는 중에 LRU 정책이 P 를 내쫓으려 한다. 쓰는 중 evict = 물리 손상.
2. **Pin/Unpin 불일치** — "pin 을 올리는 순간" 과 "evict 판단" 의 순서가 뒤집히면 pin 이 무효.
3. **중복 로드** — 두 스레드가 "P 없다" 를 동시에 확인하고 각각 디스크에서 로드. 풀에 같은 페이지가 두 번 존재 = 불일치.
4. **Torn page** — 페이지를 디스크로 내리는 도중 다른 스레드가 수정하면, 디스크엔 절반만 기록된 페이지가 남는다.

#### 해결 방법 비교

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| 1. 풀 전체 단일 mutex | 모든 접근 직렬화 | 30 분 완성 | 풀 자체가 글로벌 병목 | ★ | 10/1/1 |
| 2. Hash bucket mutex | page_id 해시 버킷별 mutex | 경합 분산, 보편적 | hot page 가 한 버킷에 몰리면 무의미 | ★★★ | 9/6/6 |
| 3. Per-frame latch + atomic pin | 프레임마다 rwlock, pin 은 atomic | 업계 표준, D2 와 자연 연동 | 자료구조 복잡 | ★★★★ | 9/8/8 |
| 4. Multi-instance pool | 풀을 N 개로 샤딩 (InnoDB 방식) | 락 경합 근본 1/N | 메모리 오버헤드 | ★★★★ | 9/9/8 |
| 5. Lock-free hash + epoch GC | 완전 비차단 | 이론상 최고 | 검증 어려움, 버그 → 손상 | ★★★★★ | 8/10/9 |

#### 동작 개념

buffer pool 의 fetch 로직은 "**버킷 mutex 안에서 pin 을 올리는 순서** 가 핵심". 그래야 버킷 lookup 과 pin 올리기 사이에 evict 가 끼어들 수 없다. latch (읽기/쓰기 중 보호) 와 pin (풀에서 내보내지 말 것) 은 의미가 다르므로 따로 관리한다. pin 이 0 이어도 누군가 latch 를 잡고 있을 수 있고, pin 이 1 이어도 아직 latch 를 잡지 않은 순간이 있다.

Torn page 는 **WAL + fsync 순서** 로 해결한다 — 페이지를 디스크로 내리기 전에 redo 로그가 먼저 디스크에 가 있어야 한다(Write-Ahead 규칙). 페이지가 찢어져도 로그로 복원 가능.

#### 본 과제 적용

wk07 레포가 이미 페이지 기반이면 `Hash bucket mutex + atomic pin` (2안) 까지 구현. 모두 메모리 상주라면 이 레이어 자체를 스킵하고 D2 에 바로 연결.

---

### D4. Lock Manager — 2PL / MVCC / OCC

D2 의 latch 는 **수 µs ~ ms** 짜리 단기 락 (자료구조 보호). 여기 D4 의 lock 은 **수 ms ~ 수 초** 짜리 장기 락 (트랜잭션 의미). 두 층이 섞이면 락이 너무 오래 잡히거나 너무 짧게 풀려 직렬성이 깨진다.

#### 현상

- 두 트랜잭션이 같은 row 수정 → lost update.
- 읽기-쓰기 상호 간섭 → dirty read, non-repeatable read, phantom.
- 2PL 의 growing/shrinking 위반 → 직렬화 깨짐.
- Lock Table 자체의 동시성 (여러 스레드가 동시 insert/delete) → 자료구조 손상.

#### 해결 방법론 비교

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| Strict 2PL (row lock) | 모든 읽기 S, 쓰기 X, commit 까지 보유 | 증명된 직렬화. 업계 표준 | 쓰기 핫스팟에서 대기 폭증 | ★★★ | 10/5/5 |
| Table-level 2PL | 테이블 단위 락 | 매우 간단 | 동시성 거의 없음 | ★★ | 10/2/2 |
| Multi-Granularity Lock (MGL) | IS/IX/S/SIX/X 계층 | 테이블/행 잠금 조정 | 호환 매트릭스 복잡 | ★★★★ | 10/7/7 |
| MVCC | 읽기는 버전 체인에서 "그 시점" 본다 | 읽기-쓰기 대기 없음 | 버저닝/GC 오버헤드 | ★★★★★ | 9/10/10 |
| OCC (Optimistic) | 읽을 땐 락 없음, commit 시 검증 | 충돌 드문 워크로드에 최적 | 충돌 많으면 abort 폭풍 | ★★★★ | 9/8/8 |

#### 현대 DB 의 선택

- **PostgreSQL** — MVCC 중심. 읽기는 사실상 잠금 없음. 쓰기끼리만 row X-lock 이 `xmax` 로 표현. 명시적 `SELECT FOR UPDATE` 만 heavyweight lock.
- **MySQL InnoDB** — MVCC + 2PL 혼합. 읽기는 snapshot, 쓰기는 row X-lock. Gap lock 으로 phantom 도 막음.
- **Oracle** — undo segment 로 consistent read, 쓰기는 row lock + enqueue 대기 큐.
- **SQL Server** — 기본 Strict 2PL, `READ_COMMITTED_SNAPSHOT` 옵션 시 MVCC (tempdb 버전 스토어).

공통 교훈: **"읽기 잠금을 어떻게 없애는가"** 가 현대 OLTP 의 차별화 지점. MVCC 가 답이지만 본 과제 공수 밖 (2 주+). 그래서 우리는 **Strict 2PL + row lock** 을 목표로.

#### Strict 2PL 의 동작 개념

- 연산 시작 시점에 필요한 row 에 **해당 모드의 락** 을 요청한다 (SELECT → S, INSERT/UPDATE/DELETE → X).
- 이미 다른 트랜잭션이 호환 안 되는 락을 쥐고 있으면 **대기 큐에 등록** 되어 `cond_wait`.
- 쥐고 있던 트랜잭션이 commit/abort 시점에 **자기가 쥔 락 전부를 한꺼번에 해제**. 해제 순간 대기 큐 앞쪽에서 grant 가능한 것을 깨운다.
- 락은 중간에 절대 안 놓는다 (shrinking 단계가 commit/abort 의 순간점). 그래야 **직렬화 가능** 이 보장된다.

**왜 이 규칙이 정합성을 주는가** — 어떤 트랜잭션 T1 이 row R 에 쓰기 락을 commit 까지 쥐고 있으면, 그 사이 T2 가 R 을 읽거나 쓰는 것은 불가능. T1 이 commit 한 뒤에야 T2 가 R 을 본다. 결과적으로 "T1 전부 실행 → T2 전부 실행" 이라는 **직렬 스케줄과 동등** 한 결과. 이게 Serializability 의 증명 아이디어.

#### 본 과제 적용

Step 4 에서 row-level Strict 2PL 을 구현. SELECT → S, INSERT/UPDATE/DELETE → X. 업그레이드 (S → X) 는 생략하고, 처음부터 X 로 잡아도 MVP 로 충분.

---

### D5. Deadlock

#### 현상

T1 이 row A 를 쥐고 row B 를 기다리는데, T2 가 row B 를 쥐고 row A 를 기다린다. 둘 다 영원히 못 진행. Lock Manager 의 대기 큐에 그대로 방치하면 그 두 트랜잭션뿐 아니라 그들이 쥔 락에 걸려 대기 중이던 제 3, 4 의 트랜잭션까지 줄줄이 hang.

```
T1:  hold(A)  →  wait(B)
T2:  hold(B)  →  wait(A)
        ↑──────────┘
        순환 대기 = deadlock
```

#### 해결 방법론 비교

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| 탐지 (wait-for graph + cycle detection) | 주기적으로 그래프를 그려 사이클이 있으면 한 트랜잭션 abort | 범용적. 코드가 특정 락 순서를 강제 안 해도 됨 | 주기마다 CPU. 탐지 지연만큼 hang | ★★★ | 10/5/5 |
| 예방 (Lock Ordering) | 모든 트랜잭션이 리소스를 정렬된 순서로만 획득 | 데드락 원천 제거 | 개발자가 규약 어기면 끝. 동적 접근 시 곤란 | ★★ | 10/4/4 |
| Wait-Die / Wound-Wait | 타임스탬프로 "누가 양보할지" 정적 결정 | 탐지 불필요 | starvation 위험 | ★★★ | 8/4/4 |
| Timeout 기반 | "N 초 이상 기다리면 abort" | 가장 단순 | 긴 쿼리 오탐, 탐지 지연 큼 | ★ | 6/4/4 |
| 회피 (Banker's algorithm) | 허가 전에 안전성 검증 | 이론적 | 실용성 낮음 | ★★★★ | 5/2/2 |

#### 현대 DB 의 선택

- **MySQL InnoDB** — wait-for graph 주기 탐지 (1 초 주기). cycle 발견 시 youngest txn 을 victim.
- **PostgreSQL** — `deadlock_timeout` (기본 1 초) 경과 후 wait-for graph 탐지.
- **MySQL (비 InnoDB 엔진)** — timeout only.

#### 탐지 로직의 개념

각 트랜잭션이 **락을 기다리기 시작할 때** wait-for 그래프에 엣지를 추가한다. `T_wait → T_hold`. 락을 얻으면 엣지 제거. 주기적으로 또는 새 엣지 추가 시점에 그래프에서 사이클을 DFS 로 찾는다. 사이클 발견 시 **가장 어린 트랜잭션** (롤백 비용이 작은 쪽) 을 abort 하고, 그 트랜잭션이 쥔 락을 풀어 나머지가 진행하게 한다. abort 된 쪽은 자동 재시도 또는 클라이언트에 에러.

**왜 "탐지 + abort" 가 "예방" 보다 보편적인가** — SQL 의 UPDATE 는 조건절에 따라 어떤 row 가 락 대상이 될지 동적으로 결정된다. 개발자가 모든 트랜잭션에 정적 락 순서를 강제하기 어렵다. 그래서 엔진이 탐지-해소 하는 모델이 현실적.

#### 본 과제 적용

Step 4 후반에 wait-for graph + 단순 DFS cycle detection 추가. 주기 탐지보다 "새 edge 추가 시 탐지" 가 코드 단순하고 반응 빠름. Victim 은 youngest txn.

---

### D6. WAL · Recovery (스켈레톤만)

#### 왜 필요한가

"커밋된 트랜잭션의 변경은 크래시 후에도 남아야 한다(durability). 커밋 안 된 변경은 남아선 안 된다(atomicity)." 이 두 불변식을 동시에 지키는 장치.

페이지를 디스크에 쓰는 순간은 길고 원자적이지 않다 — 전원이 튀면 **torn page** 가 생긴다. 그래서 **로그 먼저, 페이지 나중** 이라는 규칙 (Write-Ahead Logging) 을 둔다. 크래시 후 복구는 로그를 재생 (redo) 해서 커밋된 변경을 다시 적용하고, 커밋 안 된 변경은 로그를 거꾸로 읽어 취소 (undo) 한다. 이게 **ARIES** 알고리즘의 뼈대.

#### 동시성 문제

- 여러 스레드가 WAL 로그에 append 하려 할 때 순서가 섞이면 복구 시 뜻을 잃는다 → **단일 writer 직렬화** 또는 LSN (Log Sequence Number) 으로 순서 부여.
- `fsync` 가 무거우므로 **group commit** — 여러 트랜잭션의 commit 을 묶어 한 번의 `fsync`.

#### 해결 방법 비교

| 방법 | 개념 | 장점 | 단점 | 난이도 | 효과 |
| --- | --- | --- | --- | --- | --- |
| 로그 없음 (메모리만) | 크래시 시 전부 날아감 | 구현 제로 | 내구성 없음 | ★ | 0/10/10 |
| append-only log + LSN | WAL 기본 | 업계 표준 | 구현 복잡 | ★★★★★ | 10/5/5 |
| group commit | 여러 커밋의 fsync 묶음 | fsync 횟수 1/N | 미세 지연 | ★★★ | 5/9/5 |
| 체크포인트 + ARIES | 전체 복구 알고리즘 | 완전 복구 | 2 주+ 공수 | ★★★★★ | 10/6/6 |

#### 본 과제 적용

스켈레톤만 — "WAL 자리를 비워두고, redo entry 형식만 정의, fsync 는 생략 or 버퍼드". 발표 때 "여기가 WAL 이 들어갈 자리" 로 보여주기.

---

### D7. MVCC (참고만)

본 과제엔 구현하지 않지만 개념은 알아야 한다. 이유는 "현대 DB 가 왜 빠른가" 의 답이 거의 MVCC 이기 때문.

- **개념** — 각 row 마다 여러 버전을 보관. 트랜잭션은 시작 시점의 스냅샷을 보므로 **읽기가 다른 트랜잭션의 쓰기를 기다리지 않는다**.
- **구현 방식** — PG 는 heap 안에 구버전을 두고 `xmin/xmax` 로 가시성 판단. InnoDB 는 undo log 에서 역으로 구성. Oracle 은 SCN 과 rollback segment, SQL Server 는 tempdb 의 version store.
- **대가** — 버전 GC (PG 는 VACUUM, InnoDB 는 purge thread). 이걸 게을리하면 "bloat" — 디스크가 부풀고 스캔이 느려짐.
- **교훈** — "읽기 잠금" 을 없애는 것이 현대 OLTP 의 정체성. 하지만 그 대가로 **쓰기 경로가 복잡해지고, 버전 공간·GC 라는 새로운 관심사** 가 생긴다.

본 과제는 Strict 2PL 로 끝. MVCC 는 발표 Q&A 대비 개념만 숙지.

---

## 5. 구현 로드맵

| Step | 내용 | 레이어 | 공수 | 얻는 것 |
| --- | --- | --- | --- | --- |
| 1 | 소켓 + 단일 accept + work queue + 워커 풀 | L1, L2 | 1 일 | 요청 파이프라인 뼈대 |
| 2 | 기존 B+ Tree 에 **Global rwlock** 붙여 MVP | D2 | 반일 | 첫 벤치 기준선 |
| 3 | **Lock Coupling** 업그레이드 | D2 | 2~3 일 | 쓰기 동시성 이득 측정 |
| 4 | Lock Manager (Strict 2PL) + Wait-for graph deadlock | D4, D5 | 3~4 일 | 트랜잭션 의미 확보 |
| 5 (선택) | 프록시 추가 → 안 B | L4, L5 | 2~3 일 | 커넥션 풀·서킷브레이커 학습 |

**수요일 오전**: Step 1, Step 2 완료. 오후: Step 3 시작. **수요일 밤**: Step 3 완료하고 벤치 돌려 수치 확보. **목요일 오전**: Step 4 핵심 부분, 발표 슬라이드 정리. Step 5 는 시간 남으면.

---

## 6. 체크리스트

- `SO_REUSEADDR` 켰는가.
- `SIGPIPE` 무시했는가.
- `accept()` 의 `EINTR` / `EMFILE` 처리했는가.
- Work queue 는 **bounded**, 가득 차면 **명시적 거절**.
- `pthread_cond_wait` 는 반드시 `while (predicate) wait` 형태인가 (spurious wakeup 방어).
- 파서에 `static` 변수 없는가.
- 모든 B+ Tree 노드에 latch 붙였고, 순서는 "부모 → 자식" 인가.
- Strict 2PL 에서 commit/abort 전에 락을 **절대** 중간에 안 놓는가.
- Wait-for graph 에 edge 추가/제거를 **매번** 하는가.
- ThreadSanitizer(`-fsanitize=thread`) 로 통과했는가.
- 부하 테스트에서 "큐 깊이, 락 대기 시간, p99 지연" 세 메트릭 측정했는가.

---

## 7. 발표 4 분 스크립트 초안

1. **문제 진술 (30 s)** — "미니 DBMS 에 클라이언트 N 명이 동시에 접속하면 어떤 동시성 문제가 생기는가. 두 축 — 서버 공통 (쓰레드풀, accept 큐) + DB 고유 (B+ Tree, Lock)."
2. **아키텍처 (30 s)** — 안 A (단일 프로세스) 와 안 B (프록시 + 서버) 의 그림. 이번 구현은 안 A, 안 B 는 확장 경로.
3. **핵심 선택 (1 m)** — B+ Tree 동시 접근: Global rwlock vs Lock Coupling vs B-link. 우린 Coupling 을 골랐다. 왜? 장단점 수치.
4. **결과 (1 m)** — 벤치 수치. "Global rwlock 대비 처리량 N 배, p99 지연 M 배." 라이브 데모 or 그래프 한 장.
5. **확장 계획 (1 m)** — 안 B 의 프록시, Optimistic Crabbing, MVCC, WAL. "여기까지 왔다, 여기서 더 가려면 이만큼 남았다" 스토리로.

---

## 부록 A. 용어

- **accept 큐** — 3-way 완료된 커넥션이 대기하는 커널 큐.
- **backlog** — accept 큐의 상한 (실제론 min(backlog, somaxconn)).
- **Bulkhead** — 격벽. 자원 풀을 분리해 한쪽 장애가 다른 쪽을 안 죽이게.
- **Circuit Breaker** — 장애 감지 시 호출을 차단하는 상태 머신.
- **latch** — 자료구조 보호용 단기 락 (µs ~ ms).
- **lock** — 트랜잭션 의미의 장기 락 (ms ~ s).
- **2PL** — 락을 먼저 모두 얻은 뒤 해제만 하는 프로토콜. Strict 2PL 은 commit/abort 시점에 한꺼번에 해제.
- **WAL** — Write-Ahead Logging. 로그 먼저, 페이지 나중.
- **MVCC** — Multi-Version Concurrency Control. 읽기가 다른 트랜잭션의 쓰기를 기다리지 않음.
- **Lock Coupling / Crabbing** — 트리 탐색 시 자식 래치를 잡은 뒤 부모 래치를 놓는 규칙.

## 부록 B. 참고

- CSAPP 11 장 (Network Programming), 12 장 (Concurrent Programming).
- PostgreSQL wiki — "Tuning Your PostgreSQL Server".
- HikariCP wiki — "About Pool Sizing".
- Marc Brooker — "Exponential Backoff and Jitter" (AWS Architecture Blog).
- Lehman & Yao (1981) — "Efficient Locking for Concurrent Operations on B-Trees".
- Mohan et al. (1992) — "ARIES: A Transaction Recovery Method".
- Kleppmann vs Antirez — Redlock 논쟁 (분산 락 함정 사례).
