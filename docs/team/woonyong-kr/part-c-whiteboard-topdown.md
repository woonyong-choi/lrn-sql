# Part C. 웹 서버 구축 & 동시성

`docs/question/01-team-question-parts.md` 의 Part C (C-1 ~ C-6) 질문 묶음에 대한 답을 한 문서에 정리한다.
각 섹션은 원 질문 목록을 맨 위에 두고, 그 질문들을 이어서 답하는 설명을 본문으로 제시한다.

## 커버하는 질문 매핑

| 질문 ID | 주제 | 관련 L 노드 |
|--------|------|------------|
| C-1 | 소켓 원리 & 커널 자료구조 | L6 |
| C-2 | Sockets Interface 심화 — listen/accept · addrinfo | L7-4 ~ L7-7 |
| C-3 | HTTP/MIME/Telnet 심화 | L12-2 ~ L12-4 |
| C-4 | CGI & fork/execve/dup2 | L14 |
| C-5 | Proxy | L16 |
| C-6 | 동시성 — 스레드 풀 / Async I/O / 락 | L17 |

## Part C 를 관통하는 한 문장

소켓은 fd 한 개에 묶인 커널 객체 3 겹(file → socket → sock)으로, 하나의 listening 소켓이 accept() 로 자식 소켓들을 낳으면, 각 자식 위에서 HTTP 를 파싱해 정적 파일을 그대로 복사하거나 CGI 프로그램을 fork+execve 로 띄워 stdout 을 돌려주거나 다른 서버로 프록시하며, 요청 수가 늘면 동시에 여러 accept 를 처리하기 위해 스레드 풀과 futex 기반 락으로 공유 자원을 보호한다.

## 예시 상황 세팅

Part A, B 와 같은 시나리오를 공유한다. 이번엔 "서버 쪽에서 무슨 일이 벌어지는가" 를 기준으로 본다.

```text
서버 호스트 B
  IP  = 208.216.181.15
  listening port = 80
  CGI directory  = /cgi-bin
  static root    = /var/www

클라이언트 HTTP request (18 B)
  "GET / HTTP/1.0\r\n\r\n"

Tiny 소스 기준 함수:
  main, doit, read_requesthdrs, parse_uri,
  serve_static, get_filetype, serve_dynamic, clienterror
```

---

## C-1. 소켓 원리 & 커널 자료구조 (L6)

### 원 질문

- 소켓은 "연결의 끝점" 이라고만 들었다. 실제 물리적으로, 그리고 소프트웨어적으로는 어떻게 구현되어 있는가? / 소켓은 물리적으로 어디에 있는가? (최우녕, 최현진)
- sockfd 라는 정수 하나로 커널의 어떤 자료구조가 접근되는가? (최우녕)
- socket fd 는 수신할 때 어떻게 작동하는가? (최현진)
- socket fd 는 송신할 때 어떻게 작동하는가? (최현진)

### 설명

네 질문은 모두 **"소켓은 물리 장치가 아니라 커널 heap 에 할당된 3 겹 객체이고, sockfd 는 그 객체를 가리키는 프로세스별 인덱스"** 라는 구조로 답할 수 있다.

소켓은 "연결의 끝점" 이라는 말은 비유일 뿐, 실제로는 커널이 할당한 `struct file` + `struct socket` + `struct sock` 세 개의 객체가 포인터로 엮여 있는 것이다. 물리적으로는 RAM 의 어딘가(slab 할당자가 잡아준 kernel heap)에 있고, 네트워크 카드나 장치 드라이버와는 포인터 체인으로만 연결된다.

- **sockfd 의 정체**: 정수 하나 (예: 4). 이 수는 프로세스의 **파일 디스크립터 테이블(fdtable)** 의 인덱스다. `current->files->fd_array[4]` 에 `struct file *` 포인터가 들어 있다.
- **struct file 이 소켓인지 파일인지 구분**: `file->f_op` 가 가리키는 file_operations 구조체를 본다. 소켓이면 `socket_file_ops` (read 가 `sock_read_iter`, write 가 `sock_write_iter`). 일반 파일이면 ext4 의 op. VFS 가 투명하게 분기.
- **송신할 때**: `write(fd, buf, n)` → `ksys_write` → `vfs_write` → `sock_write_iter` → `sock_sendmsg` → 프로토콜별 `tcp_sendmsg` / `udp_sendmsg` → 커널 sk_buff 생성 후 네트워크 스택.
- **수신할 때**: NIC 인터럽트 → 드라이버 → `ip_rcv` → `tcp_v4_rcv` → 4-tuple 로 struct sock 찾음 → `sk_receive_queue` 에 skb 추가 → `sk_data_ready` 로 대기 중인 `read()` 깨움 → user 버퍼로 copy_to_user.
- **물리적으로 어디 있나**: kernel heap 의 slab 캐시. `/proc/slabinfo` 에 `sock_inode_cache`, `TCP`, `UDP`, `tw_sock_TCP` 같은 이름으로 잡혀 있다.

### 소켓의 3 겹 객체 체인

```text
프로세스 유저 공간                              커널 공간
────────────────────────                       ─────────────────────────────────────────────

int sockfd = 4;
                                               current->files (struct files_struct *)
                                                      │
                                                      ▼
                                               struct fdtable
                                                      │
                                               fd_array[4] ───┐
                                                              │
                                                              ▼
                                               struct file  (f_op = socket_file_ops)
                                                      │
                                                      │  file->private_data
                                                      ▼
                                               struct socket (SOCK_STREAM)
                                                      │
                                                      │  socket->sk
                                                      ▼
                                               struct sock = struct tcp_sock (TCP 제어 블록)
                                                      │
                                       ┌──────────────┼──────────────┐
                                       ▼              ▼              ▼
                          sk_receive_queue  sk_write_queue   inet_sk (src/dst IP/port)
                          (수신 skb 대기열)  (송신 skb 대기열)
```

### struct file 플래그 비트 (file descriptor flags)

```text
open() 의 flags 인자는 비트마스크다.
sockfd 에도 똑같이 적용된다 (fcntl 로 조작).

O_NONBLOCK    = 0x800    = 0000_1000_0000_0000
O_CLOEXEC     = 0x80000  = 1000_0000_0000_0000_0000
O_APPEND      = 0x400    = 0000_0100_0000_0000
O_DIRECT      = 0x4000   = 0100_0000_0000_0000
O_RDWR        = 0x2      = 0000_0000_0000_0010

accept4(sockfd, ..., SOCK_NONBLOCK | SOCK_CLOEXEC) 로 두 비트를 함께 설정 가능.

확인:
  fcntl(fd, F_GETFL)    // O_NONBLOCK 포함 여부
  fcntl(fd, F_GETFD)    // FD_CLOEXEC 여부 (fd table 단의 플래그)
```

### 송신 · 수신 경로 한눈에 보기

```text
송신 (write 관점):

  앱 write(sockfd, buf, n)
       │
       ▼
  ksys_write → vfs_write
       │
       ▼
  socket_file_ops.write_iter = sock_write_iter
       │
       ▼
  sock_sendmsg → tcp_sendmsg
       │
       │  copy_from_user → skb 선형 버퍼
       │  skb 를 sk_write_queue 에 enqueue
       │
       ▼
  tcp_write_xmit → tcp_transmit_skb
       │
       │  skb_push 로 TCP / IP / Eth 헤더 3 겹
       │
       ▼
  dev_queue_xmit → 드라이버 → NIC → 선로


수신 (read 관점):

  NIC PHY 수신 → DMA → skb 생성
       │
       ▼
  ip_rcv → tcp_v4_rcv
       │
       │  4-tuple 로 struct sock 찾음
       │  sk_receive_queue 에 skb enqueue
       │
       ▼
  sk->sk_data_ready(sk) → 대기 중인 read() 깨움
       │
       ▼
  앱 read(sockfd, buf, n)
       │
       │  tcp_recvmsg → skb_copy_datagram_msg → copy_to_user
       │
       ▼
  반환값 n_copied
```

### 직접 검증 ① — 한 프로세스의 소켓 객체 체인 보기

```bash
# 특정 PID 의 fd 목록
ls -l /proc/<pid>/fd
# 출력에 "socket:[12345]" 같은 항목이 보인다. 12345 는 inode 번호.

# 그 inode 로 다시 어떤 소켓인지 조회
ss -tnp | grep 12345
# 프로세스 이름, 4-tuple, 상태를 보여줌.
```

### 직접 검증 ② — slab 캐시에 소켓 객체가 있는지

```bash
sudo cat /proc/slabinfo | grep -E 'TCP|sock_inode|udp'
# 예:
#   TCP       2154   2154   2432  ...
#   sock_inode_cache  43210  43210  1024 ...
# 숫자가 평소보다 늘면 소켓 leak 의심 지점.
```

### 직접 검증 ③ — bpftrace 로 write 한 번의 경로 따라가기

```bash
sudo bpftrace -e '
  kprobe:ksys_write    { printf("ksys_write fd=%d\n", arg0); }
  kprobe:sock_write_iter { printf("  sock_write_iter\n"); }
  kprobe:tcp_sendmsg   { printf("    tcp_sendmsg bytes=%d\n", arg2); }
'
# 다른 터미널에서 nc 에 한 줄 치면 세 단계 출력이 정확한 순서로 나온다.
```

---

## C-2. Sockets Interface 심화 — listen/accept · addrinfo (L7-4 ~ L7-7)

### 원 질문

- listening socket 과 connected socket 은 왜 따로 있는가? (최현진)
- open_listenfd 는 bind, listen, accept 를 모두 한 번에 해 주는가? (최현진)
- struct addrinfo 의 각 필드는 무슨 뜻이고 어떤 역할을 하는가? (최우녕)
- addrinfo 구조체를 memset 으로 0 초기화하지 않으면, 초기화되지 않은 필드의 쓰레기값 때문에 getaddrinfo() 가 어떤 문제를 일으킬 수 있나요? (+ 쓰레기값은 무슨 값이 들어가게 되는지) (이우진)
- getaddrinfo 는 inet_pton 처럼 IP 문자열을 32 비트 주소로 바꾸는 함수인가? (최현진)

### 설명

다섯 질문은 **"하나의 listening 소켓이 accept() 로 자식 소켓을 낳는 구조, 그리고 addrinfo 가 그 소켓을 만들 재료 명세서"** 라는 얼개에서 나온다.

- **listening 소켓이 따로 있는 이유**: listen 소켓은 "어느 포트에서 새 연결을 기다린다" 는 상태만 갖는다. 연결된 소켓은 "누구와 실제로 데이터를 주고받는다" 는 4-tuple 을 갖는다. 둘을 합치면 accept() 라는 개념 자체가 성립하지 않는다. 하나는 "수락 중", 다른 하나는 "연결 중".
- **open_listenfd 가 accept 까지 하는가**: **아니다**. open_listenfd 는 socket → bind → listen 까지만 한다. accept() 는 main 루프 안에서 요청마다 호출한다. 이유는 accept 가 **새 fd 를 낳는** 연산이므로 "준비" 와 "수락" 을 분리해야 하기 때문.
- **addrinfo 필드**:
  - `ai_flags` — AI_PASSIVE (bind 용), AI_NUMERICHOST (IP 문자열 전용), AI_CANONNAME (정규화된 호스트명 저장) 등.
  - `ai_family` — AF_INET / AF_INET6 / AF_UNSPEC.
  - `ai_socktype` — SOCK_STREAM / SOCK_DGRAM.
  - `ai_protocol` — 보통 0 (자동).
  - `ai_addrlen` — ai_addr 의 길이.
  - `ai_addr` — sockaddr 포인터 (실제 주소).
  - `ai_canonname` — 정규 호스트명 (AI_CANONNAME 시).
  - `ai_next` — 다음 결과 연결 리스트.
- **memset 안 하면 왜 터지나**: `ai_flags`, `ai_family`, `ai_socktype` 이 **스택에 남아 있던 쓰레기값** 이 된다. 쓰레기값의 정체는 보통 바로 전 함수가 그 스택 자리에 써 둔 값 — 다른 변수의 잔재, 반환 주소의 일부, 이전 구조체의 필드 등. 이게 우연히 3 이거나 5 같은 값이면 getaddrinfo 가 지원하지 않는 socktype 으로 해석돼 에러가 나거나, 더 나쁘게는 "엉뚱한 주소군으로 해석해서 성공" 해서 조용히 이상 동작.
- **getaddrinfo vs inet_pton**: getaddrinfo 는 **이름 → 주소 구조체** 변환. DNS 호출도 포함. inet_pton 은 **IP 문자열 → 32 비트 숫자** 변환. DNS 없음. 둘은 역할이 다르다.

### listen / accept 의 내부 큐 구조

```text
listen 호출 직후 커널이 만드는 두 큐 (리눅스 2.2 이후):

  (1) SYN queue     — SYN 을 받았지만 3-way 가 끝나지 않은 half-open 세션
                     tcp_max_syn_backlog 로 크기 제한.
  (2) accept queue  — 3-way 완료, accept() 를 기다리는 완성 세션
                     listen(backlog) 의 backlog 값이 크기 제한.

accept() 는 (2) 에서 하나를 pop.

listen socket 메모리 구조
  struct sock  (state = LISTEN, port = 80)
     │
     ├── icsk_accept_queue.rskq_accept_head  ───> req_sock ─> req_sock ─> ...
     │                                           (accept queue)
     └── inet_csk_reqsk_queue_hash            ───> half-open 세션 해시
                                                  (SYN queue)
```

accept() 한 번은 정확히 아래 작업을 한다.

```text
(1) accept queue 에서 첫 req_sock 을 꺼냄.
(2) 새 struct sock (child) 생성, state = ESTABLISHED.
(3) 새 struct socket 생성, socket->sk = child.
(4) 새 struct file 생성, file->private_data = socket.
(5) 새 fd 번호를 fdtable 에 할당, fd_array[newfd] = file.
(6) 사용자에게 newfd 반환.

그 결과 listen fd 는 그대로, 새 fd 는 4-tuple 세션 하나에 바인딩된 독립 객체.
```

### addrinfo 힌트를 비트로 채우기

```text
struct addrinfo hints;
memset(&hints, 0, sizeof(hints));         // 모든 필드 0
hints.ai_family   = AF_INET;              // 2
hints.ai_socktype = SOCK_STREAM;          // 1
hints.ai_flags    = AI_PASSIVE;           // 0x0001

힌트 구조체의 바이트 배치 (예: x86_64):

  offset  size  field           값 (비트)
  ──────  ────  ──────────────  ────────────────
  0x00    4     ai_flags        0x00000001
  0x04    4     ai_family       0x00000002
  0x08    4     ai_socktype     0x00000001
  0x0C    4     ai_protocol     0x00000000
  0x10    4     ai_addrlen      0x00000000
  0x14    4     (pad)
  0x18    8     ai_addr         NULL
  0x20    8     ai_canonname    NULL
  0x28    8     ai_next         NULL

memset(&hints, 0, sizeof(hints)) 을 안 하면:

  offset 0x04 의 ai_family 에 스택 쓰레기가 남는다. 예를 들어
  이전 함수가 이 스택 슬롯에 0x00000003 을 남겼다면,
  ai_family = 3 으로 해석 → getaddrinfo 가 "AF_NETLINK" 로 오해하고
  EAI_FAMILY 에러를 반환. 최악은 ai_family=0 (AF_UNSPEC) + ai_socktype 도
  쓰레기면 결과가 "왜 가끔 동작하고 가끔 안 하는지 모를" 버그.
```

### 직접 검증 ① — listen / accept 가 낳는 fd 번호 변화 관찰

```bash
# 한 터미널에서 간단한 서버
python3 -c "
import socket
s = socket.socket()
s.bind(('0.0.0.0', 9999))
s.listen(5)
print('listen fd =', s.fileno())
c, _ = s.accept()
print('accept fd =', c.fileno())
c.close()
s.close()
" &

sleep 1
# 다른 터미널에서
nc 127.0.0.1 9999

# 첫 터미널 출력:
#   listen fd = 3
#   accept fd = 4
# listen 과 accept 는 전혀 다른 fd.
```

### 직접 검증 ② — SYN queue / accept queue 가 차는 상황

```bash
# 강제로 backlog 를 꽉 채우기 (SYN 만 쏘기)
sudo hping3 -S -p 80 --flood example.net   # 위험, 로컬에서만

ss -lnt
# 출력:
# State  Recv-Q  Send-Q  Local Address:Port
# LISTEN 0       5        *:80
#         ^^^    ^^^
#         현재    backlog
# Recv-Q 가 backlog 에 다다르면 커널이 새 SYN 을 drop 한다.
```

### 직접 검증 ③ — addrinfo 쓰레기값 재현

```c
// memset 을 생략한 재현 코드
#include <netdb.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct addrinfo hints;
    // memset(&hints, 0, sizeof(hints));  // 의도적으로 뺌
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res;
    int r = getaddrinfo("example.net", "80", &hints, &res);
    printf("rv=%d msg=%s\n", r, gai_strerror(r));
    // 스택 상태에 따라 EAI_BADFLAGS (-1) 이나 EAI_FAMILY (-6) 이 찍힐 수 있다.
}
```

---

## C-3. HTTP/MIME/Telnet 심화 (L12-2 ~ L12-4)

### 원 질문

- MIME 타입이란 무엇이며 왜 필요한가? (최우녕)
- Telnet 은 무엇이며, 왜 "모든 인터넷 프로토콜의 트랜잭션을 실행해볼 수 있다" 고 말하는가? / Telnet 으로 HTTP 를 테스트할 수 있다는 말은 무슨 뜻인가? (최우녕, 최현진)
- HTTP/1.0 과 HTTP/1.1 의 차이는 무엇인가? (최우녕)

### 설명

세 질문은 **"HTTP 는 TCP 위에 얹은 텍스트 라인 프로토콜이고, MIME 은 본문의 타입을 명시하는 헤더 한 줄, Telnet 은 TCP 소켓에 키보드 입력을 그대로 흘려 넣는 최소 도구"** 라는 하나의 이해로 답할 수 있다.

- **MIME 타입**: `Content-Type: text/html; charset=utf-8` 같은 헤더 한 줄. 본문이 무슨 종류의 바이트인지 알려주는 라벨. 브라우저가 렌더링 방식을 결정한다. 없으면 브라우저가 내용을 추측(snif)해야 해서 보안/호환 문제.
- **Telnet 이 HTTP 를 테스트할 수 있는 이유**: Telnet 은 TCP 연결 한 번을 연 뒤 키보드 입력을 그대로 소켓에 실어 보낸다. HTTP 는 텍스트 라인 프로토콜이므로 키보드로 `GET / HTTP/1.0\r\n\r\n` 을 치면 서버가 응답을 준다. 즉 Telnet = "TCP 위에 키보드 붙인 CLI 도구". HTTP · SMTP · POP3 같은 텍스트 프로토콜 전부 같은 식으로 테스트 가능.
- **HTTP/1.0 vs 1.1**:
  - 1.0: 요청마다 TCP 를 열고 닫음.
  - 1.1: **keep-alive 기본** (한 연결에서 여러 요청), `Host` 헤더 **필수** (한 IP 에 여러 가상호스트), chunked transfer encoding, pipelining.

### HTTP request · response 바이트 레이아웃

```text
HTTP request 의 구조 (GET)

  GET /home.html HTTP/1.1\r\n        ← request line
  Host: www.example.net\r\n          ← 필수 (HTTP/1.1)
  User-Agent: curl/7.88\r\n          ← 선택
  Accept: */*\r\n                    ← 선택
  \r\n                               ← 헤더 끝 표시 (빈 줄)
  [body, GET 은 보통 없음]

각 바이트가 TCP payload 에 그대로 실린다. 개행은 반드시 CRLF (\r\n = 0x0D 0x0A).

첫 줄의 비트:
  G  E  T  _  /  h  o  m  e  .  h  t  m  l  _  H  T  T  P  /  1  .  1  \r \n
  47 45 54 20 2F 68 6F 6D 65 2E 68 74 6D 6C 20 48 54 54 50 2F 31 2E 31 0D 0A
```

response 도 구조가 같다:

```text
HTTP/1.1 200 OK\r\n                            ← status line
Date: Mon, 20 Apr 2026 01:00:00 GMT\r\n
Content-Type: text/html; charset=utf-8\r\n     ← MIME
Content-Length: 42\r\n
\r\n
<html><body>Hello</body></html>                ← body (42 B)
```

수신 측은 `\r\n\r\n` 을 찾으면 헤더 끝임을 알고, `Content-Length` 를 보고 그 바이트만큼 읽는다. chunked 인 경우엔 다른 종단 규칙을 쓴다.

### MIME 타입 일부의 실제 바이트

```text
Content-Type: text/html; charset=utf-8\r\n
  → C  o  n  t  e  n  t  -  T  y  p  e  :  _  t  e  x  t  /  h  t  m  l  ...
     43 6F 6E 74 65 6E 74 2D 54 79 70 65 3A 20 74 65 78 74 2F 68 74 6D 6C ...

Content-Type: application/json\r\n
Content-Type: image/png\r\n
Content-Type: multipart/form-data; boundary=----WebKitForm...\r\n
```

### 직접 검증 ① — Telnet 으로 HTTP 쏘기

```bash
# Telnet 이 없으면 nc (netcat) 로 대체
nc example.net 80
# 프롬프트가 뜨면 키보드로 아래를 그대로 입력 (마지막 빈 줄 필수):
GET / HTTP/1.0
Host: example.net

# 서버가 응답으로 status line + 헤더 + body 를 그대로 내려준다.
```

### 직접 검증 ② — HTTP/1.0 vs 1.1 의 connection 차이

```bash
# HTTP/1.0: 요청마다 연결이 바로 닫힘
curl -v --http1.0 http://example.net/
# → 마지막에 "Connection #0 to host example.net closed" 즉시

# HTTP/1.1: keep-alive 로 연결 유지
curl -v --http1.1 -Z -Z http://example.net/ http://example.net/
# -Z: 다음 요청 재활용
# → 두 번째 요청은 같은 TCP 연결을 씀.
```

### 직접 검증 ③ — MIME 이 없을 때 브라우저 동작

```bash
# 간단한 서버에서 Content-Type 을 안 보내면:
echo -e 'HTTP/1.0 200 OK\r\n\r\n<h1>test</h1>' | nc -l 8888 &
curl -i http://127.0.0.1:8888/
# Content-Type 헤더 없이 그대로 도착. 브라우저가 sniff 로 HTML 로 추측함.
# 이게 보안 문제로 이어질 수 있음 (X-Content-Type-Options: nosniff).
```

---

## C-4. CGI & fork/execve/dup2 (L14)

### 원 질문

- CGI 는 무엇인가? (최우녕)
- 클라이언트가 보낸 인자 "15000&213" 이 CGI 프로그램의 argv / 환경변수 / stdin / stdout 중 어디로 전달되는가? / CGI 프로그램은 인자를 어떻게 받는가? (최우녕, 최현진)
- GET 요청의 인자는 CGI 프로그램에게 어떻게 전달되는가? (최현진)
- POST 요청의 인자는 CGI 프로그램에게 어떻게 전달되는가? (최현진)
- 서버가 fork 한 자식에서 execve 로 CGI 프로그램을 띄우고 그 결과가 클라이언트로 돌아가기까지의 전 과정을 자세히 설명해 달라. (최우녕)
- CGI 프로그램이 printf 한 데이터는 어떻게 client 에게 돌아가는가? (최현진)
- dup2 는 무엇이고 CGI 에서 왜 중요한가? (최현진)
- fork 는 무엇이고 어디서 쓰이는가? (최현진)

### 설명

여덟 질문은 **"CGI 는 자식 프로세스의 stdout 을 소켓 fd 로 dup2 해서, CGI 프로그램이 printf 하면 그 바이트가 TCP 소켓을 통해 클라이언트로 흐르게 하는 OS 수준 트릭"** 이라는 한 얼개로 답할 수 있다.

- **CGI 가 무엇인가**: Common Gateway Interface. 서버가 요청마다 별도 프로세스를 띄워서, 그 프로세스의 표준 출력을 응답 본문으로 사용하게 하는 규약.
- **인자 전달 4 가지 채널**:
  - **URL query** (GET): `?arg1=15000&arg2=213` 이 서버에 도달하면 환경변수 `QUERY_STRING="arg1=15000&arg2=213"` 으로 설정된다. CGI 는 `getenv("QUERY_STRING")` 으로 읽는다.
  - **POST body**: body 바이트가 자식의 **stdin(fd 0)** 으로 흐른다. 크기는 `CONTENT_LENGTH` 환경변수.
  - **argv**: CGI 는 보통 argv 는 안 쓴다 (일부 서버가 요청 path 에서 뽑아 줌).
  - **헤더 → 환경변수**: `HTTP_USER_AGENT`, `HTTP_HOST`, `REMOTE_ADDR` 같은 것들.
- **fork → execve → 결과 반환의 전 과정**: 부모 서버가 accept() 로 얻은 connfd 를 가지고, (1) fork() → 자식이 부모의 fd 테이블 사본을 가짐, (2) 자식이 `dup2(connfd, STDOUT_FILENO)` → fd 1 이 connfd 와 같은 struct file 을 가리킴, (3) execve("/cgi-bin/adder", ...) → 자식 프로세스 이미지가 CGI 바이너리로 교체, 그런데 fd 1 은 그대로 connfd 를 가리킴, (4) CGI 가 printf → fd 1 이 소켓이므로 TCP 로 흘러감, (5) CGI exit → 소켓이 parent 쪽에선 열려 있고 자식 쪽에선 닫힘.
- **printf 가 클라이언트에 도달하는 이유**: printf 는 libc 버퍼 → fd 1 → kernel 의 struct file → struct socket → tcp_sendmsg → 네트워크. stdout 이 소켓으로 redirect 된 상태이기 때문.
- **dup2 의 역할**: fd 번호를 강제로 재배치. `dup2(connfd, 1)` 은 "fd 1 이 무엇을 가리키든 상관없이, 지금부터 fd 1 이 connfd 와 같은 파일을 가리키게 해라" 라는 뜻. 원래 fd 1 이 가리키던 파일의 refcount 가 1 줄어들고, connfd 의 refcount 가 1 늘어난다.
- **fork 의 용도**: 자식 프로세스 생성. 주로 서버가 요청마다 독립 프로세스를 만들 때, exec 계열로 다른 프로그램을 실행할 때 쓴다.

### CGI 실행 전후의 fdtable 과 struct file refcount

```text
부모 프로세스 (Tiny 서버) — accept() 직후

  fdtable
    fd 0 ──> struct file (tty)           refcount = 1
    fd 1 ──> struct file (tty)           refcount = 1
    fd 2 ──> struct file (tty)           refcount = 1
    fd 3 ──> struct file (listenfd)      refcount = 1
    fd 4 ──> struct file (connfd, socket) refcount = 1

fork() 후 — 자식은 부모의 fdtable "내용" 을 복제하고 각 struct file 의 refcount 가 +1

  부모 fdtable                           자식 fdtable (내용 복제)
    fd 0 ──> tty                           fd 0 ──> tty
    fd 1 ──> tty                           fd 1 ──> tty
    fd 2 ──> tty                           fd 2 ──> tty
    fd 3 ──> listenfd                      fd 3 ──> listenfd
    fd 4 ──> connfd  (refcount=2)          fd 4 ──> connfd

  struct file (connfd 의) refcount = 2  ← 부모와 자식이 같은 객체 공유

자식이 dup2(4, 1) 실행

  자식 fdtable
    fd 0 ──> tty                    (변경 없음)
    fd 1 ──> connfd                 (원래 tty 가리키던 거 해제 → refcount -1)
    fd 2 ──> tty
    fd 3 ──> listenfd
    fd 4 ──> connfd

  struct file (connfd) refcount = 3  ← fd 1 과 fd 4 가 모두 가리킴
                                       + 부모도 fd 4 로 가리킴
  struct file (tty stdout) refcount 는 dup2 가 자식 쪽에서 하나 잡고 있던 것을
  릴리스하므로 유지.

자식이 close(4) 실행 (fd 4 는 이제 필요 없음)

  자식 fdtable
    fd 0 ──> tty
    fd 1 ──> connfd    ← 이제 fd 1 만 connfd 가리킴
    fd 2 ──> tty
    fd 3 ──> listenfd   ← listenfd 는 자식엔 쓸모 없어서 닫는 게 관습

  struct file (connfd) refcount = 2  ← 부모 fd 4 + 자식 fd 1

execve("/cgi-bin/adder", ...) 실행

  자식 프로세스 이미지가 adder 로 교체. fdtable 은 유지.
  FD_CLOEXEC 플래그가 꽂힌 fd 는 자동으로 닫힘 (flags & FD_CLOEXEC).
  fd 1 은 CLOEXEC 가 없으므로 그대로 connfd 를 가리킴.

  → adder 의 printf("Result: %d\n", x+y) 는 fd 1 로 write →
     connfd 로 → TCP payload → 클라이언트.
```

### CGI 환경변수의 실제 메모리 배치

```text
execve("/cgi-bin/adder", argv, envp) 호출 시 envp 의 모양

  envp 는 char** — NULL 로 끝나는 포인터 배열

  envp[0] ──> "QUERY_STRING=arg1=15000&arg2=213\0"
  envp[1] ──> "CONTENT_LENGTH=0\0"
  envp[2] ──> "REQUEST_METHOD=GET\0"
  envp[3] ──> "HTTP_HOST=www.example.net\0"
  envp[4] ──> "REMOTE_ADDR=128.2.194.242\0"
  envp[5] ──> "SERVER_PORT=80\0"
  envp[6] ──> NULL

execve 시 커널이 이 문자열들을 자식 프로세스의 새 스택에 복사.
CGI 바이너리의 main 진입 시 스택 최상단 근처에 재배치됨.

  ┌───────────────┐  높은 주소
  │ envp strings  │  "QUERY_STRING=..." "CONTENT_LENGTH=0" ...
  ├───────────────┤
  │ argv strings  │  "/cgi-bin/adder" ...
  ├───────────────┤
  │ envp pointers │  envp[0..N] NULL
  ├───────────────┤
  │ argv pointers │  argv[0..M] NULL
  ├───────────────┤
  │ argc          │
  └───────────────┘  낮은 주소 (stack top, rsp)

CGI 는 getenv("QUERY_STRING") 으로 envp 선형 탐색 → "arg1=15000&arg2=213" 획득.
```

### GET vs POST 의 인자 경로 (비트 관점)

```text
GET /cgi-bin/adder?arg1=15000&arg2=213 HTTP/1.1\r\n\r\n

서버 파싱 후:
  REQUEST_METHOD = "GET"
  QUERY_STRING   = "arg1=15000&arg2=213"
  CONTENT_LENGTH = "0"
  stdin          = empty

CGI:
  getenv("QUERY_STRING") → "arg1=15000&arg2=213"

──────────────────────────

POST /cgi-bin/adder HTTP/1.1\r\n
Content-Length: 19\r\n
Content-Type: application/x-www-form-urlencoded\r\n
\r\n
arg1=15000&arg2=213

서버 파싱 후:
  REQUEST_METHOD = "POST"
  QUERY_STRING   = ""
  CONTENT_LENGTH = "19"
  stdin          = "arg1=15000&arg2=213"  (19 B)

CGI:
  len = atoi(getenv("CONTENT_LENGTH"));
  read(STDIN_FILENO, buf, len);  // 그대로 바디 바이트 19 개 읽기
```

### 직접 검증 ① — 서버에 fork 한 자식이 생기는지

```bash
# Tiny 같은 fork 기반 서버를 하나 띄운 뒤
ps --forest | grep tiny
# CGI 요청 중에는 자식 프로세스가 보였다가 종료 후 사라진다.

# 그 자식의 fd 테이블을 실시간으로 관찰:
ls -l /proc/$(pgrep -f adder)/fd
# fd 1 이 "socket:[...]" 을 가리키는 것을 확인.
```

### 직접 검증 ② — dup2 전후의 fd 변화를 strace 로 확인

```bash
strace -f -e trace=dup2,close,execve,write ./tiny 8080 &

# 다른 터미널에서
curl 'http://127.0.0.1:8080/cgi-bin/adder?arg1=15000&arg2=213'

# 출력 요약:
#   [pid 12345] dup2(4, 1)     = 1
#   [pid 12345] close(4)       = 0
#   [pid 12345] execve("/cgi-bin/adder", ...)
#   [pid 12345] write(1, "Content-Type: text/html...", ...)
# write(1, ...) 의 fd 1 이 실제로는 소켓이라는 점이 포인트.
```

### 직접 검증 ③ — strace 로 envp 가 CGI 에 전달되는지

```bash
strace -e trace=execve -f -s 200 ./tiny 8080
# execve 호출 라인에 QUERY_STRING=... 같은 환경변수가 길게 찍힌다.
```

---

## C-5. Proxy (L16)

### 원 질문

- 프록시는 CSAPP 11 장의 본문에는 거의 등장하지 않는데, Tiny 와 어떻게 연결되는가? (최우녕)
- 프록시의 역할과 배치 방식은 어떤가? (최우녕)
- Proxy Lab 관점에서 Tiny 를 프록시로 바꾸려면 무엇을 어떻게 추가/변경해야 하는가? (최우녕)

### 설명

세 질문은 **"프록시는 `accept()` 로 받은 요청을 파싱한 뒤, 자기가 다시 `connect()` 로 upstream 에 붙어 요청을 전달하고 응답을 그대로 relay 하는 이중 소켓 서버"** 라는 한 줄 그림으로 답할 수 있다.

- **Tiny 와의 관계**: Tiny 는 "파일시스템을 읽어 클라이언트에 돌려주는" 서버. Proxy 는 "다른 서버에 물어봐서 돌려주는" 서버. 본문 파싱 · 헤더 처리 코드는 공유, 차이는 **응답의 출처가 파일이냐 다른 소켓이냐**.
- **역할**: 캐싱, 인증 검사, 보안(origin IP 숨기기), 로드 밸런싱, 프로토콜 변환.
- **배치**: 포워드 프록시(클라이언트 쪽에 붙어서 outbound 트래픽 대행), 리버스 프록시(서버 쪽에 붙어서 inbound 트래픽 분배, nginx / Cloudflare 엣지).
- **Tiny 를 프록시로 바꾸는 최소 변경**:
  1. `parse_uri` 를 host + path 를 뽑게 수정 (http://host:port/path → host, port, path).
  2. `serve_static` / `serve_dynamic` 대신 upstream 소켓 open → 요청 전달 → 응답을 clientfd 로 relay.
  3. 헤더 전달: `Host:` 를 upstream 에 맞게 재작성, `Connection: close` 로 단순화.
  4. 요청당 fork 또는 thread 로 동시성 확보.

### Proxy 의 두 소켓 타임라인

```text
Client                  Proxy                    Upstream Server
────                    ────                     ───────────────
                        listen(80)
connect(proxy:80) ───>  accept() ──────────────> (존재함, 아직 접속 전)
                        → clientfd 할당

GET http://www.example.net/ HTTP/1.0\r\n
Host: www.example.net\r\n\r\n
                  ───> clientfd 로 도착
                        │
                        │ parse_uri →
                        │   host = "www.example.net"
                        │   port = 80
                        │   path = "/"
                        │
                        │ open_clientfd(host, port)
                        │    └ DNS resolve → connect()
                        │
                        │                       SYN ─────────────>
                        │                       <──── SYN+ACK ────
                        │                       ACK ─────────────>
                        │
                        │                         ESTABLISHED
                        │
                        │ 요청 재작성:
                        │   GET / HTTP/1.0\r\n
                        │   Host: www.example.net\r\n\r\n
                        │   → upstreamfd 로 write

                                                   ← 응답 바이트 스트림
                        │
                        │ while (n = read(upstreamfd, buf))
                        │   write(clientfd, buf, n);
                        │                                  (단순 relay)
                  <───  응답 바이트가 그대로 clientfd 로 흐름
받음
```

proxy 는 동시에 **두 개의 TCP 소켓** 을 갖는다. 하나는 클라이언트 쪽 연결 (clientfd), 다른 하나는 upstream 쪽 연결 (upstreamfd). 각각 독립된 4-tuple.

### 요청 재작성의 바이트 변화

```text
원래 클라이언트가 proxy 에 보낸 첫 줄:
  "GET http://www.example.net/ HTTP/1.0\r\n"
   47 45 54 20 68 74 74 70 3A 2F 2F 77 77 77 2E 65 78 61 6D 70 6C 65 2E 6E 65 74 2F 20 48 54 54 50 2F 31 2E 30 0D 0A
   └ "GET "   └ "http://www.example.net/"                                                    └ " HTTP/1.0\r\n"

Proxy 가 upstream 에 다시 쓰는 첫 줄:
  "GET / HTTP/1.0\r\n"
   47 45 54 20 2F 20 48 54 54 50 2F 31 2E 30 0D 0A
   └ "GET "   └ "/"   └ " HTTP/1.0\r\n"

host 부분은 Host: 헤더로 이동.
```

### 직접 검증 ① — 프록시 두 소켓 모두 관찰

```bash
ss -tnp '( sport = :80 or dport = :80 )' | grep proxy
# 프록시 PID 하나에 붙은 소켓이 두 개:
#   ESTAB 0 0 proxy_ip:51213 client_ip:<ephem>   ← clientfd
#   ESTAB 0 0 proxy_ip:<ephem> upstream_ip:80    ← upstreamfd
```

### 직접 검증 ② — 요청 재작성을 tcpdump 로 보기

```bash
sudo tcpdump -i any -A -s 0 'port 80'
# clientfd 쪽에는 "GET http://..." 로 full URL,
# upstreamfd 쪽에는 "GET /" 로 path 만 남아 있는 것을 볼 수 있다.
```

---

## C-6. 동시성 — 스레드 풀 / Async I/O / 락 (L17)

### 원 질문

- 스레드 풀이 뭔지 카페 비유로 처음부터 설명해 달라. (최우녕)
- Tiny 는 iterative 서버인데, 실제 서버는 스레드 풀을 쓴다. 각 스레드가 어떻게 네트워크 I/O 를 "동시에" 처리하는가? (최우녕)
- 이 과정을 CPU / 메모리 / 커널 / 핸들 / 시스템콜 관점에서 다시 설명해 달라. (최우녕)
- async I/O (epoll, io_uring) 는 스레드 풀과 무엇이 다른가? (최우녕)
- 왜 락이 필요한가. race condition 이 실제로 어떻게 나는가? (최우녕)
- 뮤텍스와 조건 변수, 세마포어의 차이. (최우녕)
- thread-safe / reentrant / thread-unsafe 는 무엇이 다른가? (최우녕)
- 데드락은 어떻게 나고, 어떻게 피하나? (최우녕)
- 스레드가 여러 코어에서 동시에 돌 때 하드웨어는 어떻게 캐시 일관성을 유지하나? (최우녕)
- 캐시 일관성 프로토콜이 있는데도 왜 락 없이는 프로그램이 터지는가? (최우녕)
- 락 없는 코드가 어떤 방식으로 터지는가? 실제 예시 · 시나리오 · 원인 · 결과는? (최우녕)
- 리눅스 커널 · libc 는 어떤 락 · 원자연산을 제공하고 언제 써야 하는가? (최우녕)
- 이번 주 SQL API 서버에서 실제로 어디에 락이 필요한가? (최우녕)

### 설명

열세 질문 전체가 **"여러 실행 흐름이 공유 데이터를 만질 때, 연산의 원자성이 깨지면 데이터가 일관성을 잃고, 이걸 막기 위해 락과 원자연산이 필요하다"** 라는 한 줄 문제에서 나온다.

- **스레드 풀 = 카페의 바리스타 여럿**: 손님(요청)이 오면 매니저(main thread) 가 주문 티켓(job queue) 에 넣고, 비어 있는 바리스타(worker thread) 가 티켓을 집어 처리. 주문이 몰려도 바리스타 수만큼만 동시에 처리.
- **스레드가 동시에 네트워크 I/O 를 처리하는 원리**: 각 스레드가 자기 accept()/read()/write() 를 블로킹으로 호출. 한 스레드가 read() 에서 대기 중이어도 다른 스레드는 CPU 를 받아서 다른 connfd 를 처리. 커널 스케줄러가 스레드를 core 에 배정한다.
- **관점별 설명**:
  - **CPU**: 스레드는 커널 스케줄러가 스케줄링하는 최소 단위. core 수 만큼 실제 동시 실행.
  - **메모리**: 모든 스레드가 같은 가상 주소 공간을 공유. 스택만 개별. heap · bss · text 공유.
  - **커널**: task_struct 하나 = 스레드 하나. files_struct (fdtable) 는 같은 프로세스 내에서 공유.
  - **fd**: listenfd 는 모든 worker 가 공유, connfd 는 accept 한 worker 가 자기 것으로 소유.
  - **시스템콜**: 각 스레드가 독립적으로 read/write. 커널은 per-thread entry 로 syscall 진입.
- **async I/O vs 스레드 풀**:
  - 스레드 풀: 각 요청 = 전용 스레드. 간결하지만 스레드 수가 커지면 컨텍스트 스위치 · 메모리 비용 증가.
  - epoll: 하나의 스레드가 많은 fd 를 감시. "어떤 fd 가 준비됐나" 를 한 번의 syscall 로 받아 처리. 수만 동시 연결.
  - io_uring: syscall 없이 링 버퍼로 커널에 I/O 명령을 제출 · 완료 통지를 받음. 가장 최신.
- **race condition**: 공유 데이터를 두 스레드가 동시에 읽고 쓰고, 그 **읽기-수정-쓰기** 사이에 다른 스레드가 끼어들 수 있을 때 발생. 예: `counter++` 은 load + add + store 3 단계인데 그 사이에 다른 스레드가 끼어들면 한 증가가 유실.
- **뮤텍스 / 조건 변수 / 세마포어**:
  - 뮤텍스 = 한 스레드만 들어가는 방 (binary).
  - 조건 변수 = "조건이 맞을 때까지 잔다" + "조건 맞추면 깨운다". 뮤텍스와 짝.
  - 세마포어 = 카운팅 자원 토큰. N 개까지 들어갈 수 있음. 슬롯 수 제한.
- **thread-safe / reentrant / unsafe**:
  - thread-safe: 락으로 보호되어 여러 스레드 동시 호출 가능.
  - reentrant: 전역 상태를 안 써서 자기 호출이 중첩돼도 안전 (시그널 핸들러 안에서도 호출 가능).
  - thread-unsafe: 공유 데이터를 보호 없이 만짐. strtok, localtime, strerror 등.
- **데드락**: A→B 순으로 락 잡는 스레드와 B→A 순으로 락 잡는 스레드가 서로 상대의 락을 기다림. 피하는 법: **항상 정해진 순서로 락 획득**, timeout 사용, 락 hierarchy 지정.
- **캐시 일관성**: MESI 프로토콜. 한 코어가 캐시 라인을 고치면 다른 코어의 같은 라인을 invalidate 해서 다음 읽기 때 재로드하게 함.
- **캐시 일관성이 있어도 락이 필요한 이유**: 일관성은 "한 메모리 연산 이후의 관찰 가능성" 이지, "복수 연산의 원자성" 을 보장하지 않는다. `x = x + 1` 같은 복합 연산은 load/add/store 3 단계의 원자성이 별도로 필요하고, CPU 재정렬 (memory reorder) 까지 포함하면 락 / 원자연산 / 메모리 배리어가 반드시 필요.
- **락 없는 코드가 터지는 예시**: 카운터, 연결 리스트, 해시 맵, fd 할당, 재귀 잠금 요구.
- **리눅스 커널 / libc 의 제공물**: pthread_mutex_t, pthread_rwlock_t, pthread_cond_t, sem_t, __atomic_* (GCC built-in), C11 stdatomic.h, futex syscall, 커널의 spinlock / RCU.
- **SQL API 서버의 락 지점**: 커넥션 풀의 free list 조작, 인메모리 세션 테이블 업데이트, 공유 카운터/메트릭, 캐시 접근, 로그 파일 writer (append 는 O_APPEND 가 커널에서 원자성 줌).

### race condition 비트 레벨 재현

```text
counter 변수가 0 에서 시작. 두 스레드가 동시에 counter++ 100 번씩.

예상: counter == 200.
실제: 가끔 100, 150, 170 등 더 작은 수.

이유 — counter++ 의 어셈블리:

  mov   eax, [counter]    ; load
  add   eax, 1            ; increment
  mov   [counter], eax    ; store

두 스레드의 interleaving:

  Thread A: mov eax, [counter]   ; eax = 0
  Thread B: mov eax, [counter]   ; eax = 0 (아직 A 가 store 안 함)
  Thread A: add eax, 1           ; eax = 1
  Thread B: add eax, 1           ; eax = 1
  Thread A: mov [counter], eax   ; counter = 1
  Thread B: mov [counter], eax   ; counter = 1   ← 증가 하나 유실

이걸 막으려면:
  LOCK XADD [counter], 1
  또는 pthread_mutex_lock + counter++ + unlock
```

### futex 비트 연산 — pthread_mutex 의 내부

```text
pthread_mutex 는 사용자 공간에서 원자 연산으로 시도하고,
경합이 있을 때만 커널의 futex syscall 로 대기.

pthread_mutex_t 내부 상태 (단순화):
  state (32 비트)
    0  = unlocked
    1  = locked, no waiters
    2  = locked, has waiters

LOCK 시도:
  __atomic_compare_exchange_n(&state, &expected=0, desired=1, ...)
    → state 가 0 이면 1 로 바꿈 (성공)
    → 아니면 실패, state 를 2 로 바꾼 뒤 futex(FUTEX_WAIT) 로 잠.

UNLOCK 시도:
  old = __atomic_exchange_n(&state, 0, ...)
  if (old == 2) futex(FUTEX_WAKE, 1) 로 한 스레드 깨움.

비트로 본 state:
  0 = 0000_0000_0000_0000_0000_0000_0000_0000
  1 = 0000_0000_0000_0000_0000_0000_0000_0001
  2 = 0000_0000_0000_0000_0000_0000_0000_0010

가장 하위 2 비트가 뮤텍스 의미를 담는다.
```

### 스레드 풀 구조

```text
main thread
    │
    ├── job queue (linked list, mutex 보호)
    │       ├─ job (connfd=5)
    │       ├─ job (connfd=6)
    │       └─ job (connfd=7)
    │
    ├── worker 0 ──┐
    ├── worker 1 ──┼── 모두 같은 queue 의 mutex 로 경쟁, cond_var 로 대기
    ├── worker 2 ──┘
    ...

worker 루프:
  pthread_mutex_lock(&queue_mutex)
  while (queue.empty()) pthread_cond_wait(&queue_cond, &queue_mutex)
  job = queue.pop()
  pthread_mutex_unlock(&queue_mutex)

  doit(job.connfd)   // 실제 HTTP 처리는 락 없이
  close(job.connfd)
```

main thread 는 accept() → queue.push() → cond_signal. 락이 걸리는 구간은 짧다.

### 직접 검증 ① — 스레드 풀이 실제로 병렬 처리하는지

```bash
# 워커 2 개짜리 풀 서버를 띄운 뒤
ab -n 10 -c 4 http://127.0.0.1:8080/
# apache bench 로 동시 4 개 요청.
# 서버의 sleep(1) 을 넣어두면, 4 개가 2 개씩 1 초 간격으로 처리되는 걸 볼 수 있다.
```

### 직접 검증 ② — race condition 직접 보기

```c
// gcc -pthread race.c && ./a.out
#include <pthread.h>
#include <stdio.h>

long counter = 0;
void *inc(void *x) { for (int i=0;i<1000000;i++) counter++; return NULL; }

int main(void) {
    pthread_t a, b;
    pthread_create(&a, NULL, inc, NULL);
    pthread_create(&b, NULL, inc, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL);
    printf("%ld\n", counter);   // 2000000 이 나와야 할 것 같지만 매번 다름
}
```

### 직접 검증 ③ — futex syscall 이 실제로 호출되는지

```bash
strace -e trace=futex -f ./multithreaded_program
# pthread_mutex 경합 시 FUTEX_WAIT / FUTEX_WAKE 가 다량 찍힘.
# 경합이 없으면 futex syscall 이 거의 없다 — fast path 가 사용자 공간에서 끝나기 때문.
```

---

## C-1 ~ C-6 통합: 비트 버퍼가 서버 안에서 요청마다 겪는 전체 여정

### 이 섹션의 목표

Part C 의 여섯 질문 묶음을 실제 비트 · 메모리 수준으로 이어붙인다. 서버 프로세스가 listen 소켓 하나로 떠 있는 상태에서, 요청 한 개가 도착해서 응답 바이트가 나가기까지 — 그리고 수백 개가 동시에 밀려올 때 — 서버 내부 구조가 어떻게 변하는지를 추적한다.

이 섹션을 통과하면 아래 질문에 답할 수 있어야 한다.

- listen fd 하나가 accept() 로 connected fd 를 어떻게 낳는가
- HTTP request 바이트가 어느 버퍼에서 어느 버퍼로 복사되는가
- 정적 파일 vs CGI vs Proxy 에서 응답 바이트의 출처가 어떻게 달라지는가
- 스레드 풀이 동시 요청을 처리할 때 job queue 의 mutex 는 어느 비트를 건드리는가
- 락이 깨질 때 공유 변수의 비트가 어떻게 유실되는가

### STEP 0. 서버 부팅 직후의 메모리 스냅샷

```text
Tiny (또는 스레드 풀 서버) 프로세스 PID 12345

  fdtable
    fd 0, 1, 2 ──> tty
    fd 3       ──> listenfd (struct file → struct socket(LISTEN, port=80))

  heap
    ├ worker job queue (mutex=0, cond=0, list=empty)
    └ 로그 버퍼, 통계 카운터 등

  코드 영역: main, doit, serve_static, serve_dynamic, ...
```

`listen` 상태의 struct sock 은 `inet_connection_sock.icsk_accept_queue` 가 비어 있는 상태.

### STEP 1. 클라이언트 SYN 도착 → 3-way → accept queue 에 완성 세션

Part B 에서 본 handshake 가 완료되면, 서버 커널의 accept queue 에 request_sock (이제는 full child sock) 이 enqueue 된다. 이 시점 아직 `accept()` 은 반환되지 않았다.

```text
listen sock
  │
  └── icsk_accept_queue
         ├─ child_sock (4-tuple: src=C2F2 sport=C82D dst=B50F dport=0050)
         │             state = ESTABLISHED
         └─ ...
```

main thread (또는 worker) 가 accept() 를 호출하면:

```text
(1) child_sock 을 pop.
(2) 새 struct socket 생성, socket->sk = child_sock.
(3) 새 struct file 생성, file->f_op = socket_file_ops.
(4) 새 fd (예: 4) 를 fdtable 에 할당, fd_array[4] = file.
(5) user space 에 fd 값 4 를 반환.

fdtable
  fd 0,1,2 ── tty
  fd 3    ── listenfd
  fd 4    ── connfd (4-tuple 세션)
```

### STEP 2. HTTP request 바이트가 커널로 → 소켓 큐 → read() 로 복사

클라이언트가 `GET / HTTP/1.0\r\n\r\n` 18 B 를 PSH|ACK 세그먼트로 쏘면:

```text
Ethernet 14 + IP 20 + TCP 20 + payload 18 = 72 B 프레임

payload 의 비트:
  47 45 54 20 2F 20 48 54 54 50 2F 31 2E 30 0D 0A 0D 0A
   G  E  T  _  /  _  H  T  T  P  /  1  .  0 \r \n \r \n
```

서버 커널은 수신 경로(Part A STEP 7-8) 를 따라 payload 18 B 를 `child_sock->sk_receive_queue` 에 enqueue 하고 `sk_data_ready()` 를 호출. 대기 중이던 `read(fd=4, buf, 8192)` 가 깨어나 `tcp_recvmsg → copy_to_user` 로 유저 버퍼에 18 B 를 복사.

### STEP 3. 서버가 request 파싱 (fdtable 은 그대로, heap 에서만 동작)

```text
서버 주요 유저 공간 동작:

  // Tiny 의 doit 함수 요약
  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);    // "GET / HTTP/1.0\r\n" 읽기
  sscanf(buf, "%s %s %s", method, uri, version);
  read_requesthdrs(&rio);               // "\r\n" 까지 읽기
  parse_uri(uri, filename, cgiargs);    // "/" → "./home.html", isStatic
  if (static) serve_static(connfd, filename, size);
  else        serve_dynamic(connfd, filename, cgiargs);
```

여기까지는 모두 **프로세스 heap / stack 안에서만 바이트가 움직인다**. 소켓은 read() 로 18 B 가져온 뒤 해당 세션에서 더 읽을 게 없을 뿐, 여전히 연결된 상태.

### STEP 4a. 정적 응답 — 파일 → 소켓 (Tiny serve_static)

```text
(1) fd_file = open(filename, O_RDONLY)   ← 새 fd, ext4 파일
(2) mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd_file, 0)
(3) close(fd_file)    ← mmap 은 fd 와 독립적
(4) write(connfd, mmap_ptr, filesize)

write 가 하는 일:
  유저 공간 mmap 영역 (page cache 매핑) 에서 바이트를 읽어
  TCP sk_write_queue 로 복사 → sk_buff 조립 → NIC.

비트 경로 (간단화):

  Disk block ─┐
              ├── page cache (kernel RAM) ← mmap 으로 유저에 보임
              │
              ▼
  write(connfd, ptr, n)
              │
              │ copy_from_user (실제로는 zero-copy 가능: sendfile, splice)
              ▼
  sk_write_queue → tcp_sendmsg → skb_push(TCP) → skb_push(IP) → skb_push(Eth)
              ▼
  NIC → 선로
```

### STEP 4b. 동적 응답 — CGI fork/exec (Tiny serve_dynamic)

C-4 에서 본 fork → dup2 → execve 흐름이 여기 들어간다. 요약:

```text
(1) fork()
    → 자식은 fd 0..4 를 모두 상속, struct file 들의 refcount +1.

(2) 자식:
      setenv("QUERY_STRING", "arg1=15000&arg2=213", 1);
      dup2(connfd=4, STDOUT_FILENO=1);
      close(4);                 ← fd 4 는 이제 필요 없음
      execve("/cgi-bin/adder", argv, environ);

(3) adder 가 printf("Content-Type: text/plain\r\n\r\n%d\n", 15213);
    → fd 1 은 connfd 를 가리키므로 TCP 선로로 송신.

(4) adder exit → 자식 소멸 → SIGCHLD → 부모가 waitpid 로 수거.
```

부모 입장에서 connfd (fd 4) 는 그대로. struct file refcount 가 2 였다가 자식 종료로 1 로 돌아옴.

### STEP 4c. 프록시 응답 — upstream 소켓을 통한 relay

```text
(1) parse_uri 로 host, port, path 뽑기.
(2) upstreamfd = connect(host:port)       ← 새 TCP handshake (Part B 반복)
(3) write(upstreamfd, "GET / HTTP/1.0\r\nHost: ...\r\n\r\n")
(4) while ((n = read(upstreamfd, buf, 8192)) > 0)
        write(connfd, buf, n);
(5) close(upstreamfd);
(6) close(connfd);

fdtable 한때 모습:
  fd 3 ── listenfd
  fd 4 ── connfd      (client-side)
  fd 5 ── upstreamfd  (server-side)

프로세스 메모리 안에서는 buf 하나를 사이에 두고 두 소켓의 바이트가
왕복한다 — 8192 B 짜리 heap 버퍼가 릴레이 버퍼 역할.
```

### STEP 5. 스레드 풀 동시 처리 — job queue 비트 흐름

```text
main thread (한 개):
  while (1) {
      connfd = accept(listenfd, ...);
      pthread_mutex_lock(&q_mutex);
      queue.push_back({connfd});
      pthread_mutex_unlock(&q_mutex);
      pthread_cond_signal(&q_cond);
  }

worker thread (N 개):
  while (1) {
      pthread_mutex_lock(&q_mutex);
      while (queue.empty())
          pthread_cond_wait(&q_cond, &q_mutex);
      connfd = queue.pop_front();
      pthread_mutex_unlock(&q_mutex);

      doit(connfd);
      close(connfd);
  }
```

mutex 의 비트 레벨 동작 (futex state 재확인):

```text
초기 state = 0000_0000 ... 0000_0000 (unlocked)

main thread 가 lock 시도:
  CAS(state, 0, 1) → state = 0000_0000 ... 0000_0001 (locked, no waiter)

동시에 worker 가 lock 시도:
  CAS(state, 0, 1) 실패 (state=1)
  __atomic_exchange(&state, 2) → state = 0000_0000 ... 0000_0010 (locked, waiter)
  syscall futex(&state, FUTEX_WAIT, 2)  → 커널이 worker 를 재우기

main 이 unlock:
  __atomic_exchange(&state, 0)  → old 가 2
  syscall futex(&state, FUTEX_WAKE, 1) → worker 한 개 깨움

worker 재개 → CAS 성공 → state = 1
```

락이 빠졌을 때 터지는 장면:

```text
두 worker 가 동시에 queue.pop_front() 를 호출하고 mutex 가 없으면:

  Thread A: read head pointer → P
  Thread B: read head pointer → P    (같은 걸 읽음)
  Thread A: queue.head = P->next
  Thread B: queue.head = P->next     (이미 A 가 갱신한 head 를 또 덮어씀)
  Thread A: return P                 (P 를 꺼내 처리)
  Thread B: return P                 (P 를 똑같이 꺼냄)

→ 하나의 connfd 를 두 스레드가 동시에 처리.
   read() 경쟁, write() 경쟁, 응답이 엉키고 close() 가 두 번 호출돼
   double-close → 다른 요청의 새 fd 에 엉뚱한 close 가 떨어져 치명적.
```

### STEP 6. epoll 로의 전환 — "스레드 대신 fd 이벤트 기반"

```text
스레드 풀:
  각 worker 가 자기 connfd 에서 read/write 블로킹.
  동시 연결 수 N 이면 스레드 N 개 필요.

epoll:
  epfd = epoll_create1(0);
  epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, ...);
  while (1) {
      n = epoll_wait(epfd, events, MAXEV, -1);
      for (i=0; i<n; i++) {
          if (events[i].data.fd == listenfd) {
              connfd = accept(listenfd, ...);
              set_nonblocking(connfd);
              epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, ...);
          } else {
              handle_read_or_write(events[i].data.fd);
          }
      }
  }

하나의 스레드가 10,000 개 fd 를 감시 가능.
epoll 은 내부적으로 red-black tree (fd 집합) + ready list (준비된 fd) 를 관리.
fd 하나의 이벤트는 sk_data_ready 시점에 epoll 의 ready list 에 추가됨.
```

### STEP 7. SQL API 서버에서 실제 락 지점

```text
예: SQLite 기반 미니 API 서버

공유 자원:
  (1) 커넥션 풀 free list        ← pthread_mutex 필수
  (2) in-memory 세션 맵          ← pthread_rwlock (다독 일씀)
  (3) 요청 카운터 / 메트릭        ← __atomic_fetch_add 로 원자연산
  (4) 로그 파일                  ← O_APPEND 로 커널이 원자성 보장, 락 불필요
  (5) 환경설정 캐시 (읽기 전용)   ← 락 불필요

공통 규칙:
  - 공유 상태를 만지는 최소 시간만 락
  - 락 안에서 syscall · malloc · I/O 피하기
  - 여러 락이면 항상 동일 순서로 획득
```

### 직접 검증 ① — 동시 요청 N 개의 처리 시간

```bash
ab -n 100 -c 10 http://127.0.0.1:8080/static.html
# iterative 서버: 시간이 선형 증가
# thread pool (10 workers): 거의 plateau
# epoll: 동시 1000 까지도 부드러움
```

### 직접 검증 ② — race condition 이 있을 때 응답 mismatch

```bash
# 카운터 없이 req_id 를 공유 전역 변수로 쓰는 서버라면
ab -n 1000 -c 50 http://127.0.0.1:8080/status
# 응답에 찍힌 req_id 가 중복되거나 건너뛰는 것이 관찰됨.
```

### 직접 검증 ③ — epoll 이 실제로 한 스레드에서 다수 fd 를 처리하는지

```bash
strace -e trace=epoll_wait,accept4,read,write -p <pid>
# 한 스레드에서 epoll_wait → 여러 fd 에 대한 read/write 가 교차로 찍힌다.
```

---

## 전체 검증 명령 모음

```bash
# 서버 기본 상태
ss -lntp                            # listen 소켓
ss -tnp state established            # 연결된 소켓과 PID

# fd 와 소켓 매핑
ls -l /proc/$(pgrep tiny)/fd

# 요청 한 번의 syscall 경로
strace -f -e trace=accept4,read,write,dup2,execve ./tiny 8080

# CGI 자식 프로세스 관찰
ps --forest | grep tiny

# 스레드 / 경합 관찰
ps -T -p <pid>                       # 스레드 목록
strace -f -e trace=futex -p <pid>    # futex 호출

# HTTP 요청을 텔넷처럼 직접 쏘기
nc 127.0.0.1 8080
GET / HTTP/1.0

# 부하 테스트
ab -n 1000 -c 50 http://127.0.0.1:8080/

# bpftrace 로 doit 진입 / 종료 시각
sudo bpftrace -e 'uprobe:./tiny:doit { @[pid, tid] = nsecs; }'
```

## 연결 문서

- [q05-socket-principle.md](./q05-socket-principle.md) — 소켓 3 층 구조 (file/socket/sock)
- [q06-ch11-4-sockets-interface.md](./q06-ch11-4-sockets-interface.md) — Sockets Interface 함수 + addrinfo
- [q11-http-ftp-mime-telnet.md](./q11-http-ftp-mime-telnet.md) — HTTP / FTP / MIME / Telnet, HTTP 1.0 vs 1.1
- [q12-tiny-web-server.md](./q12-tiny-web-server.md) — Tiny Web Server 전체 함수
- [q13-cgi-fork-args.md](./q13-cgi-fork-args.md) — CGI, fork 로 인자 전달
- [q15-proxy-extension.md](./q15-proxy-extension.md) — Proxy 확장
- [q16-thread-pool-async.md](./q16-thread-pool-async.md) — Thread Pool, async I/O
- [q17-concurrency-locks.md](./q17-concurrency-locks.md) — 락의 기본
- [q18-thread-concurrency.md](./q18-thread-concurrency.md) — 스레드 동시성 실패 시나리오
- [part-a-whiteboard-topdown.md](./part-a-whiteboard-topdown.md) — Part A, write 이후 선로까지
- [part-b-whiteboard-topdown.md](./part-b-whiteboard-topdown.md) — Part B, 연결이 열리기까지
