# Q01. `write(sockfd, data)` 이후 커널 내부 송신 경로

> CSAPP 11장 + Linux kernel 네트워크 스택 | user -> kernel -> TCP/IP -> NIC | 중급

## 질문

나는 `getaddrinfo -> socket -> connect -> write` 흐름까지 이해했다.
그런데 `write(sockfd, data)` 를 했을 때 그 데이터가 커널 내부에서 어떻게 이동하는지 실제 흐름 기준으로 알고 싶다.

특히:

1. 사용자 영역(user space)에서 커널 영역(kernel space)로 데이터가 어떻게 넘어가는지
2. 커널 내부에서 TCP/IP 스택이 어떻게 동작하는지
3. 소켓 버퍼는 어디에 있고 어떻게 관리되는지

이걸 단계별로 설명해 달라.

## 답변

### 희준

> 사용자 영역(user space)에서 커널 영역(kernel space)로 데이터가 어떻게 넘어가는지, 그 뒤에 TCP/IP 스택과 소켓 버퍼가 어떻게 움직이는지 실제 흐름 기준으로 설명해 달라.

핵심만 먼저 한 줄로 쓰면 이렇다.

```text
user buf
-> syscall 진입
-> fd 로 socket 객체 찾기
-> user data 를 kernel socket send buffer 로 복사
-> TCP 가 segment 로 조직
-> IP 가 라우팅 / 헤더 처리
-> Ethernet / qdisc / driver
-> NIC 가 DMA 로 읽어서 선로로 송신
```

단, 중요한 점 하나:

> `write()` 가 리턴했다고 해서 "이미 선로로 다 나갔다" 는 뜻은 아니다.
> 대부분은 **"커널 송신 버퍼에 안전하게 받아 적었다"** 는 뜻에 더 가깝다.

---

## 1. 전체 그림

```text
유저 공간
────────────────────────────────────────────────────────────
app
  buf = "GET / HTTP/1.1\r\n..."
  write(sockfd, buf, n)

커널 공간
────────────────────────────────────────────────────────────
sys_write
  -> vfs_write
  -> sock_write_iter
  -> sock_sendmsg
  -> tcp_sendmsg
  -> sk_write_queue 에 skb 적재
  -> tcp_write_xmit
  -> ip_queue_xmit
  -> dev_queue_xmit
  -> NIC driver

하드웨어
────────────────────────────────────────────────────────────
NIC
  -> DMA 로 DRAM 에서 프레임 읽기
  -> Ethernet 송신
```

Linux 커널 함수 이름은 버전에 따라 조금씩 달라질 수 있지만, 큰 구조는 거의 이 흐름으로 보면 된다.

---

## 2. 1단계 — 유저 공간에서 syscall 진입

애플리케이션 코드:

```c
write(sockfd, data, len);
```

이 호출이 일어나면 사용자 프로그램이 직접 NIC 에 접근하는 게 아니라, CPU 가 **유저 모드(CPL=3)** 에서 **커널 모드(CPL=0)** 로 들어간다.

개념적으로:

```text
user
  write(sockfd, data, len)
    |
    v
glibc wrapper
    |
    v
syscall 명령
    |
    v
kernel entry
```

x86-64 Linux 식으로 쓰면 대략:

```text
user:
  rdi = sockfd
  rsi = data(user pointer)
  rdx = len
  rax = sys_write 번호
  syscall
```

커널은 여기서:

- "몇 번 fd 인가"
- "유저 주소 `data` 가 가리키는 메모리가 유효한가"
- "몇 바이트를 보내려는가"

를 확인한다.

---

## 3. 2단계 — fd 로 socket 객체 찾기

`sockfd` 는 사용자 입장에선 그냥 정수다. 하지만 커널 안에서는:

```text
task_struct
 -> files_struct
 -> fdtable[ sockfd ]
 -> struct file
 -> private_data
 -> struct socket
 -> struct sock (tcp_sock)
```

즉 `sockfd = 4` 같은 숫자는 실제로는 "이 프로세스의 fdtable 4번 칸" 을 의미한다.

소켓이면 그 칸이 결국 아래 객체에 연결된다.

```text
fd
 -> struct file          // VFS 입장에서 열린 파일
 -> struct socket        // BSD socket 계층 객체
 -> struct sock          // 프로토콜(TCP/UDP) 상태 객체
```

이 `struct sock` 안에:

- 송신 버퍼 관련 상태
- 수신 버퍼 관련 상태
- sequence number
- congestion window
- send / receive queue

같은 TCP 핵심 상태가 들어 있다.

---

## 4. 3단계 — user buffer -> kernel buffer 복사

이 단계가 질문의 1번 핵심이다.

`write(sockfd, data, len)` 를 하면 커널은 **유저 공간의 포인터를 그대로 TCP 스택이 들고 있게 두지 않는다.**
보통은 먼저 커널 메모리 쪽으로 복사한다.

개념적으로:

```text
유저 공간
  data -> [ H E L L O ... ]

커널 공간
  sk_buff / page frag 쪽으로 복사
```

이때 흔히 나오는 함수 개념은:

- `copy_from_user`
- 또는 현대 커널에서 `copy_from_iter`

류다.

왜 복사하나:

1. 유저 버퍼는 프로세스가 마음대로 바꿀 수 있다
2. syscall 이 끝난 뒤 유저 버퍼 lifetime 을 커널이 믿을 수 없다
3. TCP 는 나중에 재전송할 수도 있으므로, 자기 쪽 안전한 복사본이 필요하다

즉:

> TCP 송신은 "바로 보내고 끝" 이 아니라, ACK 받을 때까지 커널이 책임져야 하는 데이터다.

그래서 유저 버퍼의 내용을 커널 송신 버퍼에 복사해 둔다.

---

## 5. 4단계 — 소켓 송신 버퍼는 어디에 있나

질문의 3번 핵심이다.

### 5.1 논리적 위치

소켓 버퍼는 **커널 공간의 DRAM** 에 있다.

정확히 말하면 `struct sock` 안에 "버퍼 그 자체" 가 통째로 큰 배열로 들어 있는 건 아니고, 보통은:

```text
struct sock
  ├── sk_write_queue
  ├── sk_receive_queue
  ├── sk_sndbuf
  ├── sk_rcvbuf
  ├── sk_wmem_queued
  └── ...

sk_write_queue
  -> skb1 -> skb2 -> skb3 ...

sk_receive_queue
  -> skbA -> skbB -> ...
```

즉 커널은 `struct sk_buff` 라는 패킷/세그먼트 단위의 객체를 여러 개 연결 리스트나 큐 형태로 관리한다.

### 5.2 `sk_buff` 는 무엇인가

`skb` 는 네트워크 패킷을 표현하는 커널 객체다.

대략:

```text
struct sk_buff
  - next / prev
  - data, head, tail, end
  - len
  - protocol
  - dev
  - header offsets
  - checksum 상태
  - payload 저장 위치
```

중요한 점:

- `skb` 자체는 메타데이터 객체
- 실제 payload 바이트는 별도 data 영역 / page fragment 에 있을 수 있다

즉 "소켓 버퍼"를 한 덩어리 배열로 상상하기보다:

> **커널 DRAM 안에 있는 skb 객체들의 큐**

로 이해하는 게 더 정확하다.

### 5.3 send buffer 크기와 backpressure

TCP 소켓에는 송신 버퍼 한도가 있다.

대표적으로:

- `SO_SNDBUF`
- 내부 필드로는 `sk_sndbuf`

같은 값이 있다.

이 한도를 넘기면:

- **blocking socket** 이면 `write()` 가 잠깐 sleep 할 수 있다
- **non-blocking socket** 이면 `EAGAIN` / `EWOULDBLOCK` 를 반환한다

즉 커널은 "무한정 쌓아두는" 게 아니라 일정 크기까지 큐를 두고, 넘치면 보내는 쪽을 늦춘다.

---

## 6. 5단계 — TCP 계층이 데이터를 segment 로 조직

유저가 `len = 8000` 바이트를 `write()` 했다고 가정하자.

TCP 는 이걸 wire 에 그대로 8000바이트 하나로 내보내지 않는다.
보통 MSS(Maximum Segment Size)에 맞춰 나눈다.

예를 들어:

- MTU = 1500
- IP header = 20B
- TCP header = 20B
- MSS = 1460B

라면:

```text
8000B user data
-> 1460 + 1460 + 1460 + 1460 + 1460 + 700
```

처럼 여러 TCP segment 로 분할된다.

이 단계에서 TCP 는 각 skb 에 대해:

- source port / destination port
- sequence number
- ACK number
- flags(ACK, PSH 등)
- receive window

같은 TCP 정보를 붙일 준비를 한다.

### sequence number 가 중요한 이유

TCP 는 전송 후 바로 버리지 않는다.

- 아직 ACK 안 받은 데이터는 send queue 에 남아 있다
- 손실되면 재전송해야 한다

그래서 `skb` 별로 "어느 byte range 를 담당하는지"를 계속 추적한다.

예:

```text
skb1 -> seq 1000 ~ 2459
skb2 -> seq 2460 ~ 3919
skb3 -> seq 3920 ~ ...
```

ACK 가 오면 이미 확인된 skb 를 큐에서 제거하고 메모리를 해제한다.

---

## 7. 6단계 — TCP 가 실제로 내보낼지 판단

커널에 복사됐다고 바로 즉시 NIC 로 가는 것은 아니다.

TCP 는 아래 요소를 보고 "지금 몇 개를 실제로 보내도 되는지" 판단한다.

- congestion window (`cwnd`)
- receiver window (`rwnd`)
- Nagle 알고리즘 여부
- 아직 ACK 안 받은 데이터 양
- 재전송 타이머 상태

즉 TCP 는 단순 복사기가 아니라 **전송 스케줄러**다.

대표적 흐름은:

```text
tcp_sendmsg
 -> tcp_push
 -> tcp_write_xmit
 -> tcp_transmit_skb
```

정도로 이해하면 된다.

---

## 8. 7단계 — IP 계층으로 내려감

TCP 가 "이 skb 는 지금 나가도 된다"고 결정하면 다음은 IP 계층이다.

대략:

```text
tcp_transmit_skb
 -> ip_queue_xmit
```

IP 계층은 다음 일을 한다.

1. 목적지 IP 확인
2. 라우팅 테이블 조회
3. 어느 인터페이스(`eth0`, `wlan0`)로 내보낼지 결정
4. next-hop 결정
5. IP header 구성

IP header 에는:

- source IP
- destination IP
- TTL
- protocol = TCP(6)
- total length

등이 들어간다.

중요한 점:

> 여기서 커널은 "최종 목적지 IP"와 "이번 홉에서 누구에게 넘길지"를 구분한다.

즉 destination IP 는 원격 서버 그대로지만, Ethernet 단계에서 쓸 destination MAC 은 next-hop MAC 일 수 있다.

---

## 9. 8단계 — 링크 계층(Ethernet)과 이웃(neighbor) 조회

이제 IP 패킷을 실제 LAN 에서 보낼 프레임으로 싸야 한다.

여기서 필요한 게:

- source MAC
- destination MAC

이다.

destination MAC 은 보통:

- 같은 subnet 이면 상대 호스트 MAC
- 다른 subnet 이면 default gateway MAC

이다.

커널은 neighbor table / ARP cache 를 보고 MAC 을 찾는다.

없으면:

1. ARP request 를 보낸다
2. MAC 응답을 받는다
3. 그 뒤 실제 데이터 프레임을 보낸다

그 다음 패킷은:

```text
ip_queue_xmit
 -> __ip_local_out
 -> ip_output
 -> dev_queue_xmit
```

처럼 네트워크 디바이스 전송 경로로 내려간다.

---

## 10. 9단계 — qdisc 와 NIC driver

`dev_queue_xmit()` 이후에는 바로 하드웨어로 던지는 게 아니라, 보통 네트워크 디바이스 송신 큐(qdisc)를 거친다.

개념적으로:

```text
skb
 -> qdisc enqueue
 -> driver dequeue
 -> TX ring descriptor 등록
```

여기서 드라이버는:

1. `skb` 가 가리키는 데이터의 DMA 가능 주소를 준비하고
2. NIC 의 TX descriptor ring 에 그 포인터와 길이를 써 넣고
3. doorbell/MMIO 레지스터를 건드려
4. "이 엔트리 보내라"고 NIC 에 알린다

---

## 11. 10단계 — NIC 가 DMA 로 읽어 가서 선로로 보냄

이 단계부터는 하드웨어 중심이다.

NIC 는 CPU 가 바이트를 하나씩 밀어 넣어 주길 기다리지 않는다.
대신 **DMA** 를 쓴다.

즉:

```text
DRAM (kernel skb payload)
   --DMA-->
NIC onboard buffer
   -> Ethernet frame 송신
```

CPU 역할:

- descriptor 준비
- MMIO 로 전송 시작 알림
- 완료 interrupt 처리

NIC 역할:

- DRAM 에서 payload 읽기
- MAC header / CRC 처리
- PHY 통해 비트 송신

따라서 실제 물리적 데이터 이동의 주체는 상당 부분 NIC 와 DMA 엔진이다.

---

## 12. 언제 `write()` 가 끝나는가

이건 많이 헷갈린다.

대부분의 경우 `write()` 가 끝나는 시점은:

```text
"유저 데이터가 커널 송신 버퍼에 받아들여진 시점"
```

이지,

```text
"상대방이 이미 받았거나, NIC 가 이미 다 보낸 시점"
```

이 아니다.

그러므로:

- `write()` 성공
- 그런데 실제 wire 송신은 아직 조금 뒤
- 손실되면 TCP 가 재전송
- ACK 가 와야 진짜로 송신 완료 방향으로 간다

라고 이해하면 된다.

---

## 13. ACK 가 오면 무슨 일이 일어나나

송신된 skb 는 ACK 를 받을 때까지 커널이 기억하고 있다.

ACK 가 돌아오면:

1. TCP 가 `snd_una` 를 앞으로 이동
2. 이미 확인된 바이트 범위의 skb 제거
3. `sk_wmem_queued` 감소
4. send buffer 공간 회수
5. 필요하면 잠들어 있던 writer 깨움

즉 send buffer 는 "보내기 전 임시 보관소"가 아니라:

> **보낸 뒤에도 ACK 받을 때까지 책임지고 붙들고 있는 재전송 버퍼**

의 성격을 가진다.

---

## 14. 실제 함수 이름까지 포함한 한 줄 경로

커널 버전에 따라 조금 다르지만, 개념적으로 가장 많이 보는 길은 아래처럼 적을 수 있다.

```text
write
-> __x64_sys_write
-> ksys_write
-> vfs_write
-> sock_write_iter
-> __sock_sendmsg / sock_sendmsg
-> inet_sendmsg
-> tcp_sendmsg
-> tcp_push / tcp_write_xmit
-> tcp_transmit_skb
-> ip_queue_xmit
-> __ip_local_out
-> ip_output
-> dev_queue_xmit
-> driver
-> NIC DMA
-> wire
```

이 전체를 "유저 데이터가 커널 네트워크 스택을 통해 선로로 나가는 길"이라고 보면 된다.

---

## 15. 질문 3개에 대한 압축 답

### 1. user -> kernel 은 어떻게 넘어가나

- `write()` 가 syscall 로 커널 진입
- fdtable 에서 socket 객체 찾기
- `copy_from_user` / `copy_from_iter` 계열로 user buffer 를 kernel socket send buffer 로 복사

### 2. kernel 내부에서 TCP/IP 스택은 어떻게 동작하나

- TCP 가 데이터를 MSS 단위로 나누고 seq/ack/flags 를 관리
- ACK 전까지 send queue 에 보관
- IP 가 route / next-hop / TTL / header 처리
- Ethernet/qdisc/driver 를 거쳐 NIC 로 전달

### 3. 소켓 버퍼는 어디 있고 어떻게 관리되나

- 커널 DRAM 안, `struct sock` 기준으로 관리
- 실제 데이터는 `sk_buff` 객체들의 queue 로 관리
- send 쪽은 `sk_write_queue`, receive 쪽은 `sk_receive_queue`
- ACK 가 오면 send queue 에서 제거되고 메모리가 회수됨
- 버퍼 한도는 `sk_sndbuf`, `sk_rcvbuf` 같은 값으로 제한됨

---

## 16. 한 문장 결론

`write(sockfd, data)` 는 "유저 버퍼의 바이트를 커널의 TCP 송신 큐로 넘기고, 커널이 그 뒤를 책임지게 만드는 호출"이다.

즉 진짜 핵심은:

> **유저가 직접 네트워크로 보내는 게 아니라, 커널 TCP/IP 스택에게 전송 책임을 위임하는 것**

이다.
