# 00. Top-down Walkthrough — 한 번의 네트워크 통신을 끝까지 따라가는 선형 가이드

> CSAPP 11장 + 본 주 SQL API 서버 과제를 위한 통합 읽기 순서
> q01~q14 문서와 지난 대화의 심화 설명을 **탑다운 하나의 흐름**으로 묶는다.
> 위에서 아래로 순서대로 읽으면, 앞 섹션이 다음 섹션의 전제가 된다.

## 읽는 방법

1. 본 문서는 "네트워크 통신 한 번" 을 큰 그림에서 시작해, 점점 깊게 파고들어, 마지막에 실제 서버(Tiny, Proxy, 스레드 풀)까지 연결한다.
2. 각 섹션 끝에 **"→ 상세"** 링크로 대응하는 `q0X.md` 문서를 건다. 본 문서만 읽어도 뼈대가 잡히고, 깊게 보고 싶으면 링크를 타면 된다.
3. 용어가 나올 때마다 **한 줄 정의**부터 주고, 그 다음에 예시로 넘어간다.

## 목차

- §0. 왜 이런 순서로 읽는가
- §1. 전체 그림: 클라이언트 ↔ 서버 한 번의 통신
- §2. 네트워크 하드웨어 계층 — 선로, 이더넷, 공유기, 라우터, LAN/WAN
- §3. 주소 체계 — IP / MAC / 포트, 그리고 byte order
- §4. DNS — 도메인을 IP 로 바꾸는 분산 조회
- §5. 유저와 커널 — ring, syscall, trap, interrupt
- §6. 파일 추상화 — inode, fd, VFS, sockfs
- §7. 소켓의 3층 구조 — struct file → struct socket → struct sock
- §8. 소켓 API 함수들 — socket/bind/listen/accept/connect + getaddrinfo
- §9. 송신 파이프라인 (top-down) — write() 한 번이 NIC 까지 내려가는 길
- §10. sk_buff, sk_write_queue, slab — 커널 쪽 버퍼 모델
- §11. TCP 의 핵심 — seq/ack/flag/window 를 비트 단위로
- §12. IP 의 핵심 — TTL, protocol, checksum, fragmentation
- §13. ARP 와 next-hop — MAC 은 매 홉 바뀌고 IP 는 유지된다
- §14. NIC 와 드라이버 — DMA, MMIO, I/O 브릿지, descriptor ring
- §15. 수신 파이프라인 (bottom-up) — 프레임이 read() 까지 올라오는 길
- §16. 네 개의 렌즈 — CPU / 메모리 / 커널 / 핸들
- §17. 응용 계층 — HTTP, MIME, FTP, Telnet
- §18. 가장 단순한 HTTP 서버 — Tiny 의 구조
- §19. 동적 콘텐츠 — CGI, fork, dup2, execve
- §20. Echo 서버와 EOF — 짧은 read/write, 데이터그램
- §21. Tiny → Proxy — "서버이자 클라이언트"
- §22. Iterative → Concurrent — 스레드 풀, epoll, io_uring
- §23. 마무리 — 이번 주 SQL API 서버로의 연결

---

## §0. 왜 이런 순서로 읽는가

네트워크 공부가 어려운 이유는 "전선부터 HTTP 까지" 층이 7~8개로 많고, 각 층에 전용 용어가 있고, **그 용어들이 서로를 전제로** 하기 때문이다. 예를 들어 "소켓" 을 이해하려면 "fd" 가 무엇인지 알아야 하고, fd 를 이해하려면 "커널 메모리" 를 알아야 하고, 커널 메모리를 이해하려면 "syscall" 이 무엇인지 알아야 한다.

이 문서는 그 의존 관계를 풀어서 **한 방향으로 읽게 재배치**한 것이다. 순서를 따라가면 "이 용어는 아직 모르는데..." 가 안 생기도록 설계했다.

---

## §1. 전체 그림 — 클라이언트 ↔ 서버 한 번의 통신

`curl http://www.google.com/` 한 줄이 일어나면 내부에서 벌어지는 일을 **3줄 요약**하면 이렇다.

1. 내 컴퓨터가 `www.google.com` 을 **DNS** 로 IP(예: `142.251.150.104`)로 바꾼다.
2. 내 컴퓨터가 그 IP 의 80번 포트로 **TCP 연결**을 맺고 `GET / HTTP/1.1\r\n\r\n` 을 보낸다.
3. 구글 서버가 HTML 을 TCP 로 돌려준다. 연결 종료.

이 한 문장 뒤에 실제로는 **네 개의 층**이 동시에 돌아간다.

```text
[ 응용 계층 ]  HTTP, DNS, SSH, FTP ...          ← 사람이 쓰는 프로토콜
[ 전송 계층 ]  TCP, UDP                           ← 프로세스 ↔ 프로세스
[ 인터넷 계층 ] IP, ICMP                          ← 호스트 ↔ 호스트
[ 링크 계층 ]  Ethernet, Wi-Fi, ARP             ← 바로 옆 기계끼리
```

**핵심 원칙**: 위 계층은 아래 계층을 "모른다". HTTP 는 TCP 덕분에 "바이트 스트림이 순서대로 온다" 고만 믿고 있다. TCP 는 IP 덕분에 "호스트에 도달한다" 고만 믿고 있다. IP 는 Ethernet 덕분에 "옆 기계에 프레임이 간다" 고만 믿고 있다. 이 "덕분에" 의 연쇄가 **캡슐화(encapsulation)**다.

```text
유저 데이터       "GET /home.html ..."                        (95B 라 치자)
  ↓ TCP 헤더 20B
TCP 세그먼트      [TCP|데이터]                                  115B
  ↓ IP 헤더 20B
IP 패킷           [IP|TCP|데이터]                               135B
  ↓ Ethernet 헤더 14B + 트레일러 4B
Ethernet 프레임   [Eth|IP|TCP|데이터|CRC]                       149~153B
```

**→ 상세**: [q02-host-network-pipeline.md](./q02-host-network-pipeline.md)

---

## §2. 네트워크 하드웨어 계층 — 선로, 이더넷, 공유기, 라우터, LAN/WAN

**이더넷(Ethernet)** 은 "바로 옆에 있는 기계끼리 프레임을 주고받는 방법" 이다. 선로는 구리(UTP), 광섬유, 무선(Wi-Fi) 어느 쪽이든 된다. 프레임 맨 앞엔 **MAC 주소** (48비트, 예: `AA:BB:CC:DD:EE:FF`)가 src/dst 로 들어간다.

**허브 / 스위치(브릿지) / 라우터 / 공유기** 차이를 한 줄씩:

- **허브** : 신호를 그냥 모든 포트에 뿌린다(물리 계층). 요즘은 거의 없음.
- **스위치(브릿지)** : MAC 주소 테이블을 유지해서 **정확한 포트로만** 프레임을 보낸다(링크 계층).
- **라우터** : IP 주소를 보고 **다른 네트워크로** 패킷을 전달한다(인터넷 계층).
- **공유기** : 스위치 + 라우터 + NAT + Wi-Fi AP 를 한 박스에 넣은 가정용 제품.

**LAN / WAN**:

- **LAN(Local Area Network)** : 한 사무실/집 안. 수 ~ 수백 대 기기. 브로드캐스트 가능. MAC 으로 통신.
- **WAN(Wide Area Network)** : LAN 끼리를 라우터로 연결한 거대한 그물. 인터넷이 대표적. IP 라우팅으로 통신.

이 계층에서 가장 중요한 개념은 **"브로드캐스트 도메인은 LAN 까지"**. 라우터를 넘으면 브로드캐스트가 멈춘다. 그래서 ARP 같은 프로토콜은 같은 LAN 안에서만 돈다(§13).

**→ 상세**: [q01-network-hardware.md](./q01-network-hardware.md)

---

## §3. 주소 체계 — IP / MAC / 포트, 그리고 byte order

세 종류의 주소가 있다.

```text
MAC  48비트   AA:BB:CC:DD:EE:FF         NIC 하나당 하나. 평생 고정(거의).
IP   32비트   192.168.1.10              호스트당. LAN 안에선 공유기가 나눠줌(DHCP).
포트 16비트   80, 443, 51213            프로세스/소켓당. 같은 호스트에서 구분용.
```

**4-tuple** = (src IP, src port, dst IP, dst port) 가 TCP/UDP 연결 하나를 유일하게 식별한다.

**IPv6** 는 128비트. `2001:db8::1` 처럼 8개 16진수 그룹. 당분간 IPv4 와 병행.

**Byte order** (엔디안) 는 네트워크 공부에서 반드시 걸리는 함정이다.

- x86, ARM, Apple Silicon 은 **little-endian**. 메모리에 낮은 자리수가 앞.
- 네트워크 바이트 순서는 **big-endian**. 높은 자리수가 앞.
- 그래서 포트 번호 `80` (= `0x0050`) 을 그대로 소켓 구조체에 넣으면 전선에서 `0x5000` (=20480) 으로 읽힌다. 꼭 `htons(80)` 써야 한다.

```c
serv_addr.sin_port = htons(80);   // host → network short
serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
```

`htons/ntohs/htonl/ntohl` 네 개를 외워두면 모든 엔디안 실수가 줄어든다.

**→ 상세**: [q04-ip-address-byte-order.md](./q04-ip-address-byte-order.md)

---

## §4. DNS — 도메인을 IP 로 바꾸는 분산 조회

사람이 `www.google.com` 을 외우고, 기계는 IP 로 통신한다. 그 사이 다리가 DNS.

```text
www.google.com.
           ^^^  TLD
     ^^^^^^    2차 도메인
 ^^^          서브도메인
              (맨 끝의 '.' 는 root)
```

**재귀 조회 흐름** (내 PC 가 처음 `www.google.com` 을 물을 때):

```text
① 내 PC → OS 리졸버 → ISP/Cloudflare 재귀 리졸버(1.1.1.1)
② 재귀 리졸버 → root NS:    "com 은 누구?"
                  ← "a.gtld-servers.net"
③ 재귀 리졸버 → com NS:      "google.com 는 누구?"
                  ← "ns1.google.com"
④ 재귀 리졸버 → google.com NS: "www.google.com 의 A 는?"
                  ← "142.251.150.104"
⑤ 내 PC 에 IP 돌아옴. TTL 동안 캐싱.
```

레코드 종류: `A` (IPv4), `AAAA` (IPv6), `CNAME` (별칭), `MX` (메일), `NS` (권한 서버), `TXT` (SPF/인증).

**Cloudflare** 같은 서비스는 보통 세 역할을 겸한다.

- Registrar (도메인 등록 대행)
- 권한 NS (실제 A 레코드 보관)
- 프록시 CDN (proxy-on 이면 A 레코드가 Cloudflare 엣지 IP 로 나옴 → 공격 완화, 캐시)

**→ 상세**: [q05-dns-domain-cloudflare.md](./q05-dns-domain-cloudflare.md)

---

## §5. 유저와 커널 — ring, syscall, trap, interrupt

앞으로 나올 모든 소켓/파일 얘기는 **"유저가 커널에 일을 시키는"** 구조다. 그 경계를 먼저 이해해야 한다.

**링 레벨(CPL: Current Privilege Level)**:

- CPU 는 현재 권한을 `CS` 레지스터 하위 2비트에 저장한다.
- `CPL=0` = 커널 모드 (모든 명령, 모든 메모리 접근 가능)
- `CPL=3` = 유저 모드 (제한된 명령만, 커널 메모리 접근 불가)
- 즉 **권한은 "코드" 가 아니라 "CPU 상태"** 가 쥐고 있다.

**주소 공간은 어떻게 나뉘나**:

- 가상 주소 공간 전체를 "유저 영역" + "커널 영역" 으로 나눈다.
- x86_64 리눅스에선 `0x0000_0000_0000_0000 ~ 0x0000_7FFF_FFFF_FFFF` 가 유저, `0xFFFF_8000_0000_0000 ~ 0xFFFF_FFFF_FFFF_FFFF` 가 커널.
- 커널 영역은 **모든 프로세스의 페이지 테이블에 동일하게 매핑**되어 있다. 하지만 CPL=3 일 땐 접근하면 Segfault.
- 그래서 syscall 이 하는 일은 **주소 공간 바꾸기가 아니라 "권한을 CPL=0 으로 올리기"** 다.

**세 가지 경계 넘기**:

| 종류 | 누가 발생 | 동기/비동기 | 예 |
|---|---|---|---|
| syscall | 유저가 의도적으로 (`syscall` 명령) | 동기 | `read()`, `write()`, `socket()` |
| trap(exception) | 유저 코드가 실수로 | 동기 | 0 나누기, page fault, invalid opcode |
| interrupt | 외부 장치가 | 비동기 | NIC "패킷 왔어", 타이머, 키보드 |

syscall 예:

```text
유저 코드:  write(4, buf, 95)
  ↓ glibc 의 wrapper
  mov rax, 1         ; syscall 번호(sys_write)
  mov rdi, 4         ; fd
  mov rsi, buf       ; 유저 포인터
  mov rdx, 95        ; 길이
  syscall            ; CPU: CPL=3 → 0, rip = entry_SYSCALL_64
  ↓
커널: entry_SYSCALL_64 → sys_write → ksys_write → ...
  ↓ 끝나면
  sysret             ; CPU: CPL=0 → 3, rip = 유저 복귀 주소
```

**glibc** 는 이 wrapper 모음(`libc.so.6`)이다. "편하게 C 함수로 쓰자" 를 위한 얇은 래퍼.

---

## §6. 파일 추상화 — inode, fd, VFS, sockfs

리눅스/유닉스의 근본 사상: **"모든 것은 파일이다"**. 그래서 디스크 파일도, 소켓도, 파이프도, 장치도 전부 `read/write/close` 로 다룬다. 그걸 떠받치는 게 **VFS(Virtual File System)**.

**inode — 파일의 주민등록증**:

```text
$ ls -li /home/woonyong/a.txt
132045 -rw-r--r-- 1 woonyong users 1024 Apr 17 10:00 a.txt
  ^^^^^ inode 번호
```

inode 안에는 크기, 주인, 권한, 타입, 데이터 블록 위치가 들어있지만 **이름은 없다**. 이름은 디렉토리 파일 안에 `"a.txt" → 132045` 로 들어있다.

**fd — 프로세스 안의 정수**:

```text
task_struct (PID=1234)
  └── files → files_struct
               └── fdtable.fd[] 배열
                    ├── [0] → struct file (stdin)
                    ├── [1] → struct file (stdout)
                    ├── [2] → struct file (stderr)
                    ├── [3] → struct file (./a.txt)
                    └── [4] → struct file (socket)
```

- fd 는 이 배열의 **인덱스**일 뿐이다.
- 프로세스마다 고유. 같은 fd 번호가 다른 프로세스에선 다른 파일.
- `fork()` 하면 자식이 **현재 열려있는 모든 fd 를 전부** 복사(§19 참조). 몇 개든, 종류가 무엇이든(파일/소켓/파이프/tty) 가리지 않는다. fdtable 은 기본 64 슬롯, 필요시 자동 확장, 상한은 `RLIMIT_NOFILE`.

**struct file vs struct inode vs struct dentry**:

- `struct inode` : 파일 본체 메타데이터 (한 파일당 한 개)
- `struct dentry` : 이름-inode 매핑 (하드 링크 N개, inode 1개, dentry N개)
- `struct file` : **열린 상태** (read offset, 플래그). 같은 파일을 두 번 열면 file 이 2개. inode 는 1개.

**sockfs — 소켓 전용 가상 파일시스템**:

소켓은 디스크에 없으니 **anonymous inode** 를 sockfs 라는 메모리 가상 FS 가 발급해서 붙여준다. 이렇게 해야 VFS 의 `struct file` 체인에 들어갈 수 있다.

```text
sockfd=4 ── fdtable[4] ── struct file ── f_op = socket_file_ops
                             │
                             └── private_data → struct socket
                                                   ├── ops = inet_stream_ops
                                                   └── sk  → struct sock (tcp_sock)
```

그래서 `read(4, ...)` 가 `socket_file_ops.read_iter` 를 거쳐 `tcp_recvmsg` 까지 흐를 수 있다.

**파이프도 같은 모양**:

```c
int fd[2]; pipe(fd);
```

커널이 **작은 링 버퍼(기본 64KB)** 를 하나 만들고, fd[0]/fd[1] 두 개의 file 로 그걸 가리킨다. "이름 없는 파일" 이 바로 파이프.

---

## §7. 소켓의 3층 구조 — struct file → struct socket → struct sock

이제 소켓 하나를 "세 개의 렌즈" 로 본다.

```text
유저가 보는 것         sockfd = 4 (단순 정수)

VFS 층               struct file
                       - f_op = socket_file_ops  (read/write/poll/close ...)
                       - private_data → socket

BSD socket 층         struct socket
                       - type = SOCK_STREAM
                       - state = SS_CONNECTED
                       - ops = inet_stream_ops  (bind/listen/accept/sendmsg ...)
                       - sk   → sock

프로토콜 층            struct sock  (상위타입 tcp_sock)
                       - sk_family = AF_INET
                       - sk_write_queue  (송신 대기 FIFO)
                       - sk_receive_queue (수신 대기 FIFO)
                       - sk_rcvbuf / sk_sndbuf (버퍼 크기 제한)
                       - tcp_sock 의 seq/ack/window/cwnd/상태머신
```

이 세 층 덕분에:

- VFS 는 "모든 것이 파일" 을 유지하고
- BSD socket 층은 "bind/listen/accept" 같은 공통 API 를 제공하고
- 프로토콜 층은 TCP/UDP/UNIX 도메인 등 **실제 동작의 차이** 를 담는다.

`sendmsg` 한 번 부를 때 흐르는 경로는:

```text
write()/send() → file->f_op->write_iter
              → sock_write_iter
              → sock->ops->sendmsg
              → tcp_sendmsg  (TCP 면)
              → (sk_write_queue 에 skb enqueue + ip_output 호출)
```

**→ 상세**: [q06-socket-principle.md](./q06-socket-principle.md)

---

## §8. 소켓 API 함수들 — socket/bind/listen/accept/connect + getaddrinfo

서버와 클라이언트가 부르는 함수는 대칭이 있다.

```text
서버                           클라이언트
────                           ────────
socket()                       socket()
bind()                         (bind 는 보통 생략, 커널이 랜덤 포트)
listen()
accept()  ← 여기서 block         connect()  ← 3-way handshake
read()/write()                 write()/read()
close()                        close()
```

각 함수가 하는 일 한 줄씩:

- `socket(domain, type, proto)` : struct socket/sock 할당하고 fd 하나 반환.
- `bind(fd, sa, len)` : "이 소켓은 이 IP:port 로 들어오는 패킷 받을래" 를 커널에 등록.
- `listen(fd, backlog)` : 이 소켓을 "수동(passive)" 로 표시, 미완성/완성 큐 준비.
- `accept(fd, ...)` : listen 큐에서 완성된 연결 하나를 꺼내 **새 fd(connfd)** 반환. listenfd 는 그대로 남는다.
- `connect(fd, sa, len)` : 서버에 SYN 보내고 연결 완성까지 대기.

**listenfd vs connfd** 헷갈림 주의:

- 서버는 한 번 `socket + bind + listen` 해서 **listenfd** 하나 만든다.
- 클라이언트가 붙을 때마다 `accept` 가 **새 connfd** 를 만든다.
- listenfd 는 "들을 귀" , connfd 는 "실제 대화".

**getaddrinfo** — 도메인/서비스명을 socket API 에 맞는 struct 배열로 바꿔주는 "DNS + 포트맵" 래퍼.

```c
struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_family = AF_UNSPEC };
struct addrinfo *res;
getaddrinfo("www.google.com", "80", &hints, &res);
// res 는 linked list. IPv4 하나, IPv6 하나 등이 들어옴.
for (p = res; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
}
freeaddrinfo(res);
```

addrinfo 의 주요 필드: `ai_family` (AF_INET/AF_INET6), `ai_socktype` (SOCK_STREAM/DGRAM), `ai_addr` (sockaddr 포인터), `ai_addrlen`, `ai_next`.

**→ 상세**: [q07-ch11-4-sockets-interface.md](./q07-ch11-4-sockets-interface.md), [q03-tcp-udp-socket-syscall.md](./q03-tcp-udp-socket-syscall.md)

---

## §9. 송신 파이프라인 (top-down) — write() 한 번이 NIC 까지

이제 **실제 한 번의 write** 를 처음부터 끝까지. 95B 의 HTTP 요청을 보낸다고 하자.

```text
[1] 유저            buf = "GET /home.html HTTP/1.1\r\n..."  (95B)
                    write(4, buf, 95);
                      ↓ syscall  (§5: CPL 0 진입)

[2] VFS             fdtable[4] → file → sock_write_iter
                      ↓

[3] BSD socket      socket->ops->sendmsg == tcp_sendmsg
                      ↓

[4] TCP 계층        sk_buff 하나 할당 (슬랩에서)
                    copy_from_user: 유저 buf(95B) → skb 데이터 영역(커널 VA)
                    TCP 헤더 20B 붙이기 (seq, ack, flag, window, checksum)
                    sk_write_queue tail 에 enqueue
                    tcp_write_xmit 호출
                      ↓

[5] IP 계층         ip_queue_xmit
                    목적지 IP 로 라우팅 테이블 조회 → next-hop, 출력 인터페이스 결정
                    IP 헤더 20B 붙이기 (TTL=64, proto=TCP, checksum, src/dst IP)
                      ↓

[6] ARP/이웃        next-hop 의 MAC 주소 조회 (없으면 ARP 요청)
                      ↓

[7] 링크 계층        Ethernet 헤더 14B 붙이기 (src MAC=내 NIC, dst MAC=공유기)
                    dev_queue_xmit → qdisc → ndo_start_xmit
                      ↓

[8] NIC 드라이버      TX descriptor ring 에 "이 skb 의 물리주소 + 길이" 기록
                    MMIO 로 doorbell 레지스터 write → "NIC 야, 보내"
                      ↓

[9] NIC 하드웨어      DMA 엔진이 DRAM → NIC 내부 FIFO 로 프레임 복사
                    MAC 컨트롤러가 CRC 4B 계산해서 프레임 끝에 덧붙임
                    PHY 가 전기/광 신호로 선로에 송출
                      ↓
                    TX 완료 → IRQ → 드라이버가 skb 해제
```

각 단계의 번호를 기억해두면, 문제가 났을 때 "몇 단계까지 갔는가" 로 디버깅할 수 있다.

**→ 상세**: [q02-host-network-pipeline.md](./q02-host-network-pipeline.md)

---

## §10. sk_buff, sk_write_queue, slab — 커널 쪽 버퍼 모델

**sk_buff** = 네트워크 스택의 "패킷 하나" 를 표현하는 커널 구조체.

```text
struct sk_buff
  ├── head   ──▶ [ headroom | data .......... | tailroom ]
  ├── data   ──▶           ↑ 현재 payload 시작
  ├── tail   ──▶                               ↑ 끝
  ├── end    ──▶                                          ↑
  ├── len, data_len
  ├── protocol, pkt_type
  └── (prev, next for queue)
```

- **head/end** 는 할당된 전체 메모리 범위
- **data/tail** 은 현재 유효 payload 범위
- `skb_push(len)` 은 data 포인터를 앞으로 이동해서 헤더를 앞에 붙일 공간을 만든다. 그래서 TCP→IP→Ethernet 으로 내려가면서 **헤더를 앞에 "밀어 넣을" 수 있다**.
- `skb_pull(len)` 은 반대. 수신 때 헤더를 벗긴다.

**sk_write_queue / sk_receive_queue — 소켓마다 한 쌍씩**:

중요한 점: 이 두 큐는 전역 단일이 아니라 **각 struct sock 안에 박혀 있는 리스트 헤드**다. 소켓 하나당 한 쌍. 관리 주체는 커널의 TCP/UDP 프로토콜 계층(`net/ipv4/tcp.c`, `net/core/sock.c`).

```text
커널 DRAM 안

struct sock (소켓 A)
  ├── sk_write_queue  = { prev, next, qlen=3 }
  │       │
  │       ▼
  │   [skb1] ⇄ [skb2] ⇄ [skb3]        ← 이중 연결 리스트
  │     ↑                    ↑
  │     오래된 것(head)        새 것(tail)
  └── sk_receive_queue = { prev, next, qlen=1 }
          │
          ▼
      [skb_rx1]

struct sock (소켓 B) ─ 자기만의 write/receive_queue 를 따로
struct sock (소켓 C) ─ ...
```

동작:

- 유저가 `write()` → 새 skb 가 write_queue **tail** 에 추가
- TCP 가 실제로 내보낼 때 → **head** 부터 꺼내 IP 로 넘김
- NIC 에서 수신된 skb → receive_queue tail 에 추가
- 유저가 `read()` → head 에서 꺼내 copy_to_user

**skb 한 개는 "메타데이터 + 데이터" 두 조각**:

```text
skb 하나
  ① struct sk_buff (메타데이터, 약 224B)    ← slab "skbuff_head_cache" 에서 할당
  ② 데이터 영역 (패킷 바이트)                ← page_frag_alloc (페이지 조각)
```

- 작은 패킷(MTU 1500 정도): 한 페이지(4KB) 안에서 조각을 떼서 씀.
- 큰 데이터(TSO 64KB 세그먼트 등): 한 skb 에 **흩어진 페이지 여러 조각**이 `skb_frag[]` 배열로 매달림. scatter-gather DMA 로 NIC 에 전송될 때 하나의 프레임처럼 이어붙여진다. **연속된 물리 메모리일 필요가 없다**.

**sk_sndbuf / sk_rcvbuf — 큐의 상한과 백프레셔**:

큐가 무한정 커지면 DRAM 이 터진다. 커널은 소켓마다 두 상한을 둔다.

```text
/proc/sys/net/ipv4/tcp_wmem
4096   16384    4194304       ← min   default   max
       (16 KB)   (4 MB)

setsockopt(fd, SOL_SOCKET, SO_SNDBUF, ...) 로 개별 설정 가능.
전역 상한은 net.core.wmem_max.
```

**한도를 넘어도 연결을 끊지 않는다. 속도를 줄인다**.

- **송신 쪽이 꽉 찬 경우** (내가 너무 빨리 write):
  - blocking 소켓 → `write()` 가 block, 공간 생기면 깨어남
  - non-blocking → `write()` 가 -1, `errno = EAGAIN/EWOULDBLOCK`
- **수신 쪽이 꽉 찬 경우** (상대가 빨리 보내는데 내가 안 읽음):
  - TCP 가 ACK 의 `Window` 필드를 0 으로 알려줌 (= Zero Window)
  - 상대 TCP 가 송신을 멈춤
  - 내가 `read()` 로 큐를 비우면 window 가 다시 열리고 상대가 재개

"연결 닫힘(RST)" 은 별개 사건이다. 버퍼가 꽉 찼다는 이유만으로 RST 를 보내지 않는다.

**slab allocator — 계란판 모형으로**:

`sk_buff` 는 초당 수백만 번 할당/해제된다. 매번 페이지 받아서 224B 만 쓰고 버리면 낭비라, 커널은 **slab** 이라는 "같은 크기 객체 전용 풀" 을 쓴다.

두 단계로 쌓여 있다.

```text
[ Buddy Allocator ]    ← 페이지(4KB) 단위 관리인
                          2^n 페이지 크기로 반씩 쪼개고, 반납시 짝꿍과 다시 합침(단편화 방지)
       │ "페이지 줘"
       ▼
[ Slab Allocator  ]    ← "같은 크기 객체 N칸짜리 계란판" 만듦
   대표 cache:
     - skbuff_head_cache    224B
     - TCPv4                2304B
     - inode_cache          592B
     - kmalloc-64, -128 ...

계란판 모형:
  buddy 에서 4KB 페이지 한 장 받음
  → 224B 씩 줄 그어 18칸짜리 계란판 완성 (= slab 하나)
  → 요청 올 때마다 빈 칸 하나씩 반환
  → 18칸 다 차면 buddy 에 새 페이지 요청 → 새 계란판 추가 (이제 36칸)

확인:  /proc/slabinfo
  skbuff_head_cache   12340 / 12800   objsize=224  objperslab=36  pagesperslab=2
  (현재 12,340칸 사용 / 총 12,800칸. 2페이지짜리 slab 하나에 36칸.)
```

**요청 경로 요약**:

```text
kmem_cache_alloc(skbuff_head_cache)
  ├─ 현재 CPU 의 per-CPU 캐시에 남는 칸 있음 → 바로 반환 (락 없음, 최고속)
  ├─ 없으면 slab 에서 다음 칸 할당 (가벼운 락)
  └─ slab 도 꽉 찼으면 buddy 에 새 페이지 요청 → 새 slab 만들어 칸 반환
```

**SLAB / SLUB / SLOB** 은 "Slab" 이라는 개념의 **구현 엔진** 이름. API 는 모두 동일.
- SLAB : 옛 구현, 복잡
- SLUB : 현대 리눅스 기본(2.6.23+), 단순하고 빠름
- SLOB : 초소형 임베디드용

커스텀 커널이 아니면 보통 SLUB 가 돌고 있다. 신경 쓸 필요 없음.

---

## §11. TCP 의 핵심 — seq/ack/flag/window 를 비트 단위로

TCP 헤더 (20B, 옵션 제외):

> **비트맵 읽는 법**: 이 다이어그램은 마크다운 표가 아니라 **RFC 스타일 비트 레이아웃**이다.
> - 맨 위 눈금 `0 1 2 … 31` 은 **비트 번호**.
> - 한 줄 = **32비트 = 4바이트**.
> - 줄이 5개면 20바이트, 6개면 24바이트 식으로 읽는다.
> - 한 줄 안에 칸이 두 개면 2+2바이트, 칸이 4개면 1+1+1+1바이트.

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  DO |Rsvd | U A P R S F |           Window                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Options (길이 가변)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

첫 줄 해석: `Source Port` (비트 0~15, 2바이트) + `Destination Port` (비트 16~31, 2바이트). 전체 5행 = **20바이트 고정 헤더**.

**seq / ack — 바이트 번호** (내 대화에서 이미 짚었던 것):

- `seq` : "내가 지금 보내는 첫 바이트의 번호"
- `ack` : "상대가 기대하는 다음 바이트 번호 = 지금까지 받은 바이트의 다음"

```text
클라 초기 seq=1000, 서버 초기 seq=5000 이라고 가정.

[3-way handshake]
  클라 → 서버 : SYN seq=1000              (데이터 0, 하지만 SYN 은 +1)
  서버 → 클라 : SYN+ACK seq=5000 ack=1001
  클라 → 서버 : ACK seq=1001 ack=5001

[데이터]
  클라 → 서버 : seq=1001, 95B
  서버 → 클라 : ack=1096  (1001+95=1096)
  서버 → 클라 : seq=5001, 300B
  클라 → 서버 : ack=5301  (5001+300=5301)

[FIN]
  클라 → 서버 : FIN seq=1096 ack=5301
  서버 → 클라 : ACK ack=1097
  서버 → 클라 : FIN seq=5301 ack=1097
  클라 → 서버 : ACK ack=5302
```

**플래그 6비트**:

- `SYN` : "연결 시작하자"
- `ACK` : "ack 필드가 유효해" (연결 중엔 거의 항상 켜짐)
- `FIN` : "내 쪽은 더 보낼 거 없어"
- `RST` : "비정상 종료, 상태 초기화"
- `PSH` : "받는 즉시 앱에 전달해라"
- `URG` : "긴급 데이터 있음" (거의 안 씀)

**Window** : "나 지금 N 바이트 더 받을 수 있어" 의 크기. 흐름 제어(flow control)용.

**혼잡 제어(congestion control)** : 송신자가 내부적으로 유지하는 `cwnd`. 네트워크 자체의 혼잡을 감지(패킷 손실, RTT 증가)해서 보낼 양을 조절. Reno, Cubic, BBR 등의 알고리즘.

**MSS (Maximum Segment Size)** : 한 TCP 세그먼트에 넣을 수 있는 최대 데이터 바이트. 보통 **1460B** (= Ethernet MTU 1500 - IP 20 - TCP 20). 유저가 아무리 큰 데이터를 줘도 TCP 가 잘라서 여러 세그먼트로 보낸다.

---

## §12. IP 의 핵심 — TTL, protocol, checksum, fragmentation

IP 헤더 (20B, 옵션 제외):

> **읽는 법** (TCP 와 동일): 한 줄 = 32비트 = 4바이트. 5행이면 20바이트.
> 첫 줄은 4비트+4비트+8비트+16비트 식으로 쪼개져 있다. 칸 위에 쓰인 숫자가 해당 필드의 **비트 폭**.

```text
+---+---+---+---+---+---+---+---+
| Ver=4 | IHL=5 |   TOS         |   Total Length (16)           |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|        Identification         | Flags|   Fragment Offset      |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|   TTL (8)     | Protocol (8)  |       Header Checksum (16)    |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|                     Source IP Address (32)                    |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|                  Destination IP Address (32)                  |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
```

첫 줄 풀어 읽기:
- `Version=4` → IPv4 (4비트)
- `IHL=5` → 헤더 길이 5 × 4바이트 = **20바이트** (4비트)
- `TOS` → 서비스 타입 (8비트)
- `Total Length` → 헤더+본문 총 길이 (16비트)

- **TTL (Time To Live)** : 라우터 한 번 통과할 때마다 1 감소. 0 되면 버리고 ICMP Time Exceeded 로 돌려줌. 루프 방지. 리눅스 기본 64, Windows 128.
- **Protocol** : 상위 계층 식별. 6=TCP, 17=UDP, 1=ICMP.
- **Source/Destination IP** : 네트워크를 건너는 동안 **거의 바뀌지 않음**. NAT 구간에서만 src IP 가 치환됨.

**체크섬 비트 레벨로**:

예) 헤더 두 word `0x4500` + `0x0073` 의 체크섬.

```text
① 16비트 단위로 더한다 (1의 보수 합)
   0x4500 + 0x0073 = 0x4573

② 캐리가 났으면 하위에 돌려 더한다 (이 예엔 없음)

③ 비트 반전
   ~0x4573 = 0xBA8C

④ 이게 체크섬 필드에 들어간다.
```

수신 쪽은 동일 방식으로 합산했을 때 `0xFFFF` 가 나와야 정상. 틀리면 버린다. 요즘은 NIC 가 이걸 하드웨어로 오프로드.

**Fragmentation(단편화)**:

경로 중간 라우터의 MTU 가 작으면 IP 패킷을 쪼갠다. 쪼개진 조각은 같은 ID 를 공유하고 Fragment Offset 이 다르다. 수신 호스트가 다시 조립. 요즘은 Path MTU Discovery 로 송신자가 미리 MSS 를 줄여서 fragment 를 피하는 게 표준.

---

## §13. ARP 와 next-hop — MAC 은 매 홉 바뀌고 IP 는 유지된다

IP 는 "어디로 최종 도착" 이고, MAC 은 "바로 옆 누구에게 던질래" 다. 그래서 프레임이 라우터를 지날 때마다 **MAC 은 매번 바뀌고 IP 는 유지**된다.

```text
내 PC ─────▶ 공유기 ─────▶ ISP 라우터 ─────▶ … ─────▶ 구글 서버

프레임 #1 (내 PC → 공유기)
  src MAC = 내 PC, dst MAC = 공유기
  src IP  = 192.168.0.10 (사설)
  dst IP  = 142.251.150.104

프레임 #2 (공유기 → ISP)
  src MAC = 공유기, dst MAC = ISP 라우터
  src IP  = 공유기 외부 IP (NAT 로 치환됨)   ← 여기만 바뀜
  dst IP  = 142.251.150.104

프레임 #3 (ISP → 다음)
  src MAC = ISP, dst MAC = 다음 라우터
  src IP  = 공유기 외부 IP (그대로)
  dst IP  = 142.251.150.104 (그대로)
```

**라우팅 테이블 조회** (내 PC 의 커널이 하는 일):

```text
$ ip route
default via 192.168.0.1 dev wlan0
192.168.0.0/24 dev wlan0 scope link
10.0.0.0/8 via 192.168.0.254 dev wlan0

목적지가 142.251.150.104 이면:
- /24 매치? 아니
- /8 매치?  아니
- default  매치! → next-hop = 192.168.0.1
```

가장 구체적인 prefix 가 이긴다 = **Longest Prefix Match**. 현대 라우터는 이걸 하드웨어(TCAM)로 한다.

**ARP** (Address Resolution Protocol):

"192.168.0.1 의 MAC 이 뭐니?" 를 LAN 전체에 브로드캐스트.

```text
ARP Request (브로드캐스트)
  "Who has 192.168.0.1? Tell 192.168.0.10"
  dst MAC = FF:FF:FF:FF:FF:FF   (전체 뿌림)

ARP Reply (유니캐스트)
  "192.168.0.1 is at AA:BB:CC:DD:EE:FF"
```

대답을 ARP 캐시에 저장. `ip neigh` 로 볼 수 있다. 타임아웃 지나면 다시 물음.

**hop** 이라는 용어:

- "라우터 한 번 통과" = 1 hop.
- `traceroute` 가 각 홉의 IP 를 보여주는 원리: TTL=1,2,3... 으로 증가시켜 보내면 각 중간 라우터가 `ICMP Time Exceeded` 를 돌려줘서 그 라우터의 IP 가 드러난다.

```text
$ traceroute www.google.com
 1  192.168.0.1   2 ms
 2  10.x.x.x      5 ms   (ISP 첫 라우터)
 ...
 8  142.251.150.119   30 ms   (구글 엣지)
```

---

## §14. NIC 와 드라이버 — DMA, MMIO, I/O 브릿지, descriptor ring

여기가 "CPU-메모리-주변장치" 의 물리 구조를 이해해야 하는 지점.

**CPU ↔ DRAM ↔ 주변장치 구조**:

```text
[CPU 코어] ── [캐시] ── [IMC: 메모리 컨트롤러] ── [DRAM]
                              │
                              └── [PCIe 루트 컴플렉스] ── [NIC, GPU, NVMe ...]
                                   (= I/O 브릿지)
```

- CPU ↔ DRAM 은 **메모리 컨트롤러**를 통한 직통. CPU 칩 안에 IMC 가 들어있음(요즘 Intel/AMD 모두). 빠름.
- CPU ↔ NIC 은 **PCIe 루트 컴플렉스**를 통과. CSAPP 에서 "I/O 브릿지" 라 부르는 그것.
- 즉 "CPU 는 DRAM 과 직결" 이 맞지만, 주변장치는 별도의 버스로 연결된다.

**DMA (Direct Memory Access)**:

NIC 가 데이터를 보내려면 DRAM 의 skb 를 읽어야 하는데, CPU 가 일일이 복사하면 느리다. 대신 NIC 가 **PCIe 를 통해 DRAM 에 직접 읽기/쓰기** 를 한다. CPU 는 "시작해" 신호만 주고 빠진다. 복사는 NIC ↔ 메모리 컨트롤러 ↔ DRAM 사이에서 일어남.

**MMIO (Memory-Mapped I/O)**:

NIC 레지스터를 메모리 주소처럼 접근한다. 예: 주소 `0xFEC00000` 에 쓰는 것 = "NIC 의 doorbell 레지스터에 신호 주기". 이런 주소 영역은 **페이지 테이블에서 non-cacheable** 로 매핑됨 (캐시되면 장치에 안 보이니까).

즉 두 종류의 통신이 있다:

- `CPU → NIC 에게 명령` : MMIO (CPU 가 특정 주소에 write)
- `NIC ↔ DRAM 데이터 이동` : DMA (CPU 는 개입 X)

**TX/RX descriptor ring**:

DRAM 안에 드라이버가 만들어 두는 **링 버퍼**. 한 칸(= descriptor)에는 "이 skb 는 물리주소 0x12345000 에 있고 길이 149B" 가 들어간다.

```text
TX ring (송신)
  [ desc0 | desc1 | desc2 | ... | descN ]  (링처럼 순환)
     ↑                     ↑
     NIC 가 다음 읽을 위치   드라이버가 다음 쓸 위치

프로시저:
  드라이버: desc 에 skb 포인터 쓰기 → tail 증가 → MMIO 로 doorbell
  NIC:     doorbell 감지 → DMA 로 desc 의 skb 를 읽어서 전송 → head 증가
```

**NIC 내부 하드웨어**:

- **DMA 엔진** : PCIe 로 DRAM 과 통신하는 하드웨어 블록
- **MAC 컨트롤러** : Ethernet 프레임 조립, CRC 계산
- **PHY** : 전기/광 신호 ↔ 디지털 비트
- **FIFO SRAM** : 수십~수백 KB 의 내부 버퍼 (프레임 임시 보관)
- **펌웨어** : NIC 안의 작은 CPU 가 돌리는 마이크로코드

**드라이버 vs NIC 내부 차이**:

- **드라이버** = 소프트웨어. 커널 모듈. `drivers/net/ethernet/...` 의 C 코드. descriptor ring 관리, skb 와의 연결, IRQ 처리.
- **NIC 내부** = 하드웨어. PCIe 카드 안의 칩들. 실제 프레임 송수신.

**TSO (TCP Segmentation Offload) & Scatter-Gather DMA**:

TSO 는 "CPU 가 TCP 를 쪼개지 말고 NIC 가 대신 쪼개" 라고 떠넘기는 기능.

```text
[ TSO 없음 ]
  CPU: 64KB 를 1460B × 45개 세그먼트로 손수 쪼갬
       각 세그먼트마다 TCP 헤더 만들고 체크섬 계산
       → 45회 헤더 작업

[ TSO 있음 ]
  CPU: 64KB 덩어리 1개 + "이걸 MSS=1460 으로 쪼개줘" 만 NIC 에 넘김
  NIC: 내부에서 45개 프레임 만들고 헤더 복제, 체크섬, 송출
       → CPU 는 1번만 작업
```

여기서 핵심: "64KB 덩어리" 가 **연속된 물리 메모리일 필요가 없다**. skb 는 `skb_frag[]` 배열로 흩어진 페이지 조각들을 엮는다.

```text
skb
  ├── head/data/tail  (선형 영역 — 헤더가 들어가는 첫 조각)
  └── frags[]
       ├── { page 물리주소 A, offset 0, 4096 }
       ├── { page 물리주소 B, offset 0, 4096 }
       ├── { page 물리주소 C, offset 0, 4096 }
       └── ... (최대 MAX_SKB_FRAGS, 보통 17)
```

NIC 의 DMA 엔진은 **scatter-gather DMA** 를 지원해서 여러 물리주소를 받아 순서대로 끌어간다. 즉 **메모리는 흩어져 있어도 전선에 나갈 땐 이어진다**.

정리: "페이지" 는 메모리 관리 단위(4KB), "TSO 세그먼트" 는 NIC 에 넘기는 덩어리 크기(최대 ~64KB). 서로 다른 층이다.

**IRQ (인터럽트) 처리**:

```text
NIC 가 프레임 수신 완료 → PCIe 로 MSI-X 인터럽트 발사
  ↓
APIC → LAPIC → CPU 특정 코어로 IRQ 벡터 전달
  ↓
IDT[vector] → 드라이버의 top-half 핸들러 실행 (아주 짧음)
  ↓
napi_schedule → softirq NET_RX 로 예약
  ↓
나중에 softirq context 에서 실제 스택 처리 (bottom half)
```

Top-half 와 bottom-half 를 나누는 이유: 인터럽트는 다른 인터럽트를 막으므로 최대한 짧아야 함. 무거운 처리는 softirq/tasklet 으로 미룬다.

**NAPI** (New API) : IRQ polling 을 섞는 방식. 패킷이 많을 땐 IRQ 꺼두고 polling 으로 빠르게, 드물면 IRQ 로. 수십만 pps 에서 IRQ storm 방지.

---

## §15. 수신 파이프라인 (bottom-up) — 프레임이 read() 까지 올라오는 길

§9 의 정반대 방향. 이번엔 바닥에서 위로.

```text
[1] NIC PHY          선로에서 전기/광 신호 수신
                      ↓
[2] NIC MAC          CRC 검증, 프레임 조립, 내부 FIFO 에 저장
                      ↓
[3] NIC DMA          DMA 로 DRAM 의 RX ring 에 프레임 복사
                      ↓
[4] NIC → CPU        MSI-X 인터럽트 발사
                      ↓
[5] 드라이버 top-half  인터럽트 핸들러 진입 → napi_schedule
                      (IRQ 잠깐 마스크)
                      ↓
[6] softirq NET_RX    bottom-half. sk_buff 꺼내서 스택에 올림
                      ↓
[7] Ethernet         dst MAC 확인, EtherType 보고 상위로 (0x0800=IP)
                      ↓
[8] IP 계층           dst IP = 내 IP 인가, checksum, TTL 검증
                      proto 보고 TCP/UDP/ICMP 로 분기
                      ↓
[9] TCP 계층          4-tuple (src/dst IP/port) 로 소켓 검색
                      seq 순서 검증, 재조립
                      sk->sk_receive_queue 에 skb enqueue
                      프로세스가 read 대기중이면 wake_up
                      ↓
[10] 유저 read()      copy_to_user: skb → 유저 buf
                      skb 해제
```

**포인트**:
- CPU 는 처음엔 **전혀 개입하지 않음** (DMA 가 다 함)
- IRQ 부터 비로소 CPU 가 관여
- softirq 는 "유저 프로세스 컨텍스트도, 인터럽트 컨텍스트도 아닌" 특수 상태. 유저 공간 접근은 못 함.
- copy_to_user 는 **read 를 부른 그 프로세스의 컨텍스트**에서 일어남. 그래야 해당 프로세스의 페이지 테이블로 유저 주소를 해석할 수 있으니까.

---

## §16. 네 개의 렌즈 — CPU / 메모리 / 커널 / 핸들

같은 한 번의 통신을 네 가지 관점으로 다시 보면 성능/디버깅이 보인다.

**CPU 관점**: 제어와 복사. DMA 는 장치가 하지만, `copy_from_user`, 체크섬(오프로드 없으면), TCP 상태 관리, 인터럽트 처리, syscall 진입/복귀는 모두 CPU 사이클. `perf top` 에서 `copy_user_enhanced_fast_string`, `__netif_receive_skb`, `tcp_sendmsg` 같은 함수가 뜨는 이유.

**메모리 관점**: sk_buff 가 DRAM 을 여러 번 오간다. 유저 버퍼 → skb → NIC RX/TX 버퍼 → PHY. 각 복사가 메모리 대역폭을 쓴다. NUMA 환경에선 CPU-NIC 가 같은 노드에 있어야 빠름.

**커널 관점**: 소켓 → proto_ops(TCP/UDP) → IP → qdisc → 드라이버 함수 체인. 튜닝 포인트: qdisc 정책, SO_REUSEPORT, epoll vs select, softirq core 분산.

**핸들(fd) 관점**: 유저가 보는 건 정수. 뒤에는 file → socket → sock 의 체인. `ulimit -n`, select O(N) vs epoll O(1), accept4 의 원샷 플래그.

**→ 상세**: [q10-network-cpu-kernel-handle.md](./q10-network-cpu-kernel-handle.md)

---

## §17. 응용 계층 — HTTP, MIME, FTP, Telnet

이 위로는 순수하게 "바이트 약속". TCP 는 바이트 스트림만 보장하고, 그 안을 어떻게 해석할지는 응용 프로토콜의 일.

**HTTP 요청 한 조각**:

```text
GET /home.html HTTP/1.1\r\n
Host: www.example.com\r\n
User-Agent: curl/8.0\r\n
Accept: */*\r\n
\r\n
```

- 시작 라인 + 헤더 + 빈 줄 + (옵션 본문)
- `\r\n` 으로 라인 구분, 빈 줄로 헤더-본문 경계

**HTTP/1.0 vs 1.1** 핵심 차이:

| 항목 | 1.0 | 1.1 |
|---|---|---|
| 연결 | 요청 1개당 새 TCP | keep-alive 기본 |
| Host 헤더 | 없음(선택) | 필수 (가상 호스팅) |
| chunked encoding | 없음 | 있음 (스트리밍) |
| pipelining | 없음 | 있음 (잘 안 씀) |

Tiny 는 HTTP/1.0, 프록시 랩은 "1.1 요청을 받아도 1.0 으로 변환해서 보낸다".

**MIME type**: Content-Type 에 들어가는 "이 본문은 어떤 타입인가" 의 표준 표기. `text/html`, `image/png`, `application/json`. `get_filetype` 이 확장자 → MIME 매핑.

**FTP**: 파일 전송용. **제어 연결(21 포트)** + **데이터 연결(20 또는 동적)** 두 개를 사용. 프로토콜이 두 소켓을 쓰는 대표 예.

**Telnet (23 포트)**: 초기엔 원격 셸용이었지만 현대엔 **평문 TCP 디버깅 도구**로 쓴다.

```bash
$ telnet www.example.com 80
GET / HTTP/1.0
<빈 줄>
<응답 HTML 이 뜬다>
```

HTTP/SMTP/POP3 같은 텍스트 프로토콜은 telnet 으로 직접 칠 수 있다.

**→ 상세**: [q09-http-ftp-mime-telnet.md](./q09-http-ftp-mime-telnet.md)

---

## §18. 가장 단순한 HTTP 서버 — Tiny 의 구조

지금까지의 모든 층을 합치면 "가장 단순한 서버" 가 만들어진다. CSAPP 의 Tiny 다.

```text
main
 ├─ Open_listenfd(port)           ← socket+bind+listen
 └─ while (1)
     ├─ Accept → connfd
     └─ doit(connfd); Close(connfd)

doit
 ├─ 요청 라인 읽기 (Rio_readlineb)
 ├─ sscanf → method / uri / version
 ├─ read_requesthdrs  ← 빈 줄까지 헤더 소비(쓰진 않음)
 ├─ parse_uri → is_static, filename, cgiargs
 ├─ stat(filename) → 존재/권한 확인
 ├─ static 이면 serve_static
 └─ dynamic 이면 serve_dynamic

serve_static
 ├─ get_filetype → "text/html" 등
 ├─ 응답 헤더 구성 + Rio_writen
 ├─ open + mmap 으로 파일 매핑
 ├─ Rio_writen 으로 본문 전송
 └─ munmap
```

- **iterative** 서버. 요청 1개씩 순서대로. 수십 qps 까지는 문제 없음.
- GET 만 지원. cgi-bin 포함이면 dynamic.
- HTTP/1.0. Connection: close. 매 요청마다 새 TCP.

Tiny 는 "진짜 서버의 뼈대는 이 정도" 를 보여준다. 이후 모든 서버(프록시, 동시성, SQL API 서버)는 이 뼈대를 변형한 것이다.

**→ 상세**: [q12-tiny-web-server.md](./q12-tiny-web-server.md)

---

## §19. 동적 콘텐츠 — CGI, fork, dup2, execve

정적 파일만 돌려주면 밋밋하다. **CGI** 는 "요청이 오면 외부 프로그램을 실행해서 그 출력을 응답으로" 쓰는 표준.

```text
요청:  GET /cgi-bin/adder?15000&213 HTTP/1.0

서버 환경변수 설정:
  QUERY_STRING = "15000&213"
  REQUEST_METHOD = "GET"
  ...
```

**serve_dynamic 코드** (Tiny Figure 11.31):

```c
void serve_dynamic(int fd, char *filename, char *cgiargs) {
    char buf[MAXLINE], *emptylist[] = { NULL };

    sprintf(buf, "HTTP/1.0 200 OK\r\n");     Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n"); Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) {                        /* child */
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);              /* stdout → socket */
        Execve(filename, emptylist, environ);
    }
    Wait(NULL);                               /* reap zombie */
}
```

네 개의 마법:

- `fork()` : 자식은 부모의 fd 테이블 통째로 복사 → connfd=4 가 자식에게도 4 로 보임
- `dup2(fd, 1)` : 자식의 stdout 을 socket 에 연결 → 자식이 `printf` 하면 소켓으로 나감
- `setenv("QUERY_STRING", ...)` : CGI 프로그램이 `getenv` 로 읽을 수 있게 전달
- `execve(filename, ...)` : 코드만 CGI 프로그램으로 교체(환경변수/fd 는 유지)

**fork 시 fd 테이블이 "통째로 복사된다" 의 진짜 의미**:

부모가 연 **모든 fd** 가 복사된다. 세 개든 천 개든 전부. 파일, 소켓, 파이프, tty, eventfd 등 종류 구분 없이 `struct file` 포인터가 복사되고 참조 카운트가 +1 된다.

```text
부모 PID=100                    자식 PID=200 (fork 직후)
fdtable (1024 슬롯)             fdtable (동일 크기, 새 배열)
  [0]  stdin                      [0]  stdin         ◀─ 같은 struct file
  [1]  stdout                     [1]  stdout        ◀─ 같은 struct file
  [2]  stderr                     [2]  stderr        ◀─ 같은 struct file
  [3]  listenfd (소켓)            [3]  listenfd      ◀─ 복사
  [4]  connfd #1                  [4]  connfd #1     ◀─ 복사
  [5]  connfd #2                  [5]  connfd #2     ◀─ 복사
  [6]  /var/log/server.log        [6]  /var/log/...  ◀─ 복사
  [7]  DB 커넥션 #1               [7]  DB 커넥션     ◀─ 복사
   ...                             ...                  ...
  [997] eventfd                   [997] eventfd     ◀─ 복사

fdtable 크기: 기본 64 슬롯, 필요하면 자동 2배씩 확장.
상한은 RLIMIT_NOFILE (`ulimit -n`). 리눅스 기본 1024, 서버는 보통 올림.
```

얕은 복사 (struct file 포인터 공유) 이므로 **파일 offset 이 공유된다**. 자식이 read 로 진행한 위치가 부모에게도 반영.

**CLOEXEC — execve 시 자동 close**:

자식이 `execve` 로 다른 프로그램이 되는 순간, 들고 있던 fd 가 의도치 않게 그쪽에 노출되면 곤란하다. `O_CLOEXEC` (또는 `fcntl(fd, F_SETFD, FD_CLOEXEC)`) 를 걸어두면 fork 때는 복사되지만 execve 때 자동 close 된다. fork+exec 패턴에서는 거의 모든 fd 에 걸어두는 게 안전.

```c
int fd = open("log.txt", O_RDONLY | O_CLOEXEC);
int s  = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
```

**→ 상세**: [q11-cgi-fork-args.md](./q11-cgi-fork-args.md)

---

## §20. Echo 서버와 EOF — 짧은 read/write, 데이터그램

**Echo 서버** = 받은 걸 그대로 되돌려주는 가장 단순한 TCP 서버. CSAPP 11.4 의 예제.

```c
void echo(int connfd) {
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        /* n == 0 이면 EOF = 상대가 close(FIN) 함 */
        printf("server received %d bytes\n", (int)n);
        Rio_writen(connfd, buf, n);
    }
}
```

**EOF 의 의미** (TCP 에서):

- `read` 가 `0` 을 리턴 = 상대가 close/shutdown 해서 이쪽으로 보낼 게 더 없음.
- 시그널 아니고 에러 아님. 정상 종료 신호.

**short read / short write 와 Rio 래퍼**:

기본 `read/write` 는 "요청한 만큼 꼭 다 읽어/써준다" 가 아니다. 일부만 처리하고 돌아올 수 있다.

왜 short 가 생기나:
- `read` : 소켓 버퍼에 **지금 들어있는 만큼만** 돌려줌. 4KB 요청해도 1KB 만 올 수 있음.
- `write` : 송신 버퍼(`sk_sndbuf`)에 들어갈 만큼만 씀. 남으면 호출자가 다시 보내야 함.
- 시그널 도착으로 `EINTR` 중단되는 경우도 있음.

그래서 유저가 매번 루프를 돌아야 하는데, 이걸 감싼 게 CSAPP 의 **Rio (Robust I/O)**:

```c
/* 원하는 n 바이트를 다 쓸 때까지 반복 */
ssize_t rio_writen(int fd, void *usrbuf, size_t n) {
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR) nwritten = 0;   /* 시그널 → 재시도 */
            else return -1;                      /* 진짜 에러 */
        }
        nleft   -= nwritten;
        bufp    += nwritten;
    }
    return n;
}
```

주요 함수:

- `rio_readn / rio_writen` : n 바이트 다 채울 때까지 루프 (EOF 면 일찍 반환)
- `rio_readinitb` : 내부 버퍼를 초기화 — 아래 두 버퍼드 함수의 전제
- `rio_readlineb` : **한 줄씩**(`\n` 까지) 읽는 버퍼드 버전. HTTP 요청 라인 읽기에 쓰임
- `rio_readnb` : 버퍼드 n바이트 읽기

왜 **버퍼드** 가 필요한가: 라인 단위로 읽으려면 1바이트씩 read 하는 게 가장 단순하지만, 매번 syscall 해서 너무 느리다. 내부적으로 큰 청크를 한 번에 읽어두고, 유저에겐 요청한 모양으로 쪼개 돌려준다.

한 줄 요약: **`rio_*` = "short read/write 와 EINTR 을 숨긴 안전한 I/O 래퍼"**.

**UDP 의 데이터그램**:

- TCP 는 바이트 스트림 → 경계 없음.
- UDP 는 메시지 단위 → **sendto 한 번 = recvfrom 한 번** 이 데이터그램 1개. 경계가 유지됨.
- 단 100B 보냈는데 상대 recvfrom 버퍼가 50B 면 잘림(+ `MSG_TRUNC`).

**→ 상세**: [q08-echo-server-datagram-eof.md](./q08-echo-server-datagram-eof.md)

---

## §21. Tiny → Proxy — "서버이자 클라이언트"

프록시는 **중간 서버**다. 클라이언트에게는 서버이고, 오리진 서버에게는 클라이언트다.

```text
클라 ──HTTP──▶ 프록시 ──HTTP(변환)──▶ 오리진
             │
             │ Tiny 의 main 루프 + doit 을 그대로
             │ 다만 "응답을 만들어라" 대신 "상위 서버에 연결해 받아라"
```

Tiny 대비 바뀌는 것:

1. `parse_uri` → **절대 URL 파서** (host/port/path 로 쪼개기)
2. 헤더 변환 → HTTP/1.1 → 1.0, Connection: close, Proxy-Connection: close
3. `serve_static/serve_dynamic` → `open_clientfd(host, port)` + `Rio_writen` + `Rio_readnb` + 릴레이

**프록시 종류**:
- **Forward** : 사내 → 바깥 나갈 때 통과 (클라가 명시적으로 설정)
- **Reverse** : 외부 → 내부 서버 분배 (nginx, Cloudflare, ALB)
- **Caching** : 응답 저장 재사용 (Squid, Varnish, CDN)
- **Tunnel** : CONNECT 로 TCP 파이프 (HTTPS 프록시)

**캐시를 붙이면** (Proxy Lab Part B):
- key = URL
- HIT → 바로 반환, MISS → 오리진 가서 받고 저장
- MAX_OBJECT_SIZE 초과는 패스, LRU eviction
- **readers-writers lock** 필수 (동시성)

**→ 상세**: [q13-proxy-extension.md](./q13-proxy-extension.md)

---

## §22. Iterative → Concurrent — 스레드 풀, epoll, io_uring

Tiny 는 iterative. 연결 한 번에 하나씩. 실전은 동시에 수천~수십만 처리해야 한다. 세 가지 전략.

**(1) 요청마다 스레드 생성**: 단순, 하지만 생성 비용.

**(2) 스레드 풀 + blocking I/O** (이번 주 SQL API 서버가 택하는 방식):

```text
main 스레드: accept 만
         → connfd 를 작업 큐에 enqueue (sbuf_insert)

worker N 개: 큐에서 dequeue (sbuf_remove, condvar wait)
           → doit(connfd) → close
```

- mutex + condvar 로 큐 동기화
- 각 worker 는 독립적으로 blocking read/write
- 하나가 block 되어도 다른 worker 가 계속 일함
- 장점: 코드 단순, "I/O 블로킹을 스레드 개수만큼 병렬화"
- 단점: 연결 수 > 스레드 수 면 대기, 스레드당 8MB 스택

**(3) 이벤트 루프 + epoll (Reactor)**:

```c
int ep = epoll_create1(0);
epoll_ctl(ep, EPOLL_CTL_ADD, listenfd, &ev);
while (1) {
    int n = epoll_wait(ep, events, MAX, -1);
    for (i = 0; i < n; i++) {
        if (events[i].data.fd == listenfd) {
            int cfd = accept4(listenfd, ..., SOCK_NONBLOCK);
            epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev_in);
        } else {
            handle_request(events[i].data.fd);
        }
    }
}
```

- 한 스레드가 수만 fd 를 관리. 커널이 "준비된 fd" 만 알려줌.
- 스케일: nginx, Node.js, Netty, Go 런타임.
- 단점: 상태 머신 작성 필요. 콜백 지옥 → async/await 으로 완화.

**(4) io_uring** (리눅스 5.1+) :

- submission/completion 링 버퍼 두 개로 "시스템콜 자체를 배치"
- 컨텍스트 스위치 거의 사라짐
- 극한 성능 인프라용

**선택 기준 요약**:

| 상황 | 선택 |
|---|---|
| 수천 연결, 단순성 우선 | 스레드 풀 (= 이번 주) |
| 수만~수십만 연결, 레이턴시 민감 | epoll/kqueue |
| 극한 성능, 커스텀 | io_uring, DPDK |

> **동시성/락 심화**: 스레드 풀에서 실제로 필요한 **뮤텍스/조건 변수/데드락/thread-safe/RWLock/SQL API 서버 실전 락 설계** 는 별도 문서 [q15-concurrency-locks.md](./q15-concurrency-locks.md) 에서 카페 비유로 상세 다룬다.

**→ 상세**: [q14-thread-pool-async.md](./q14-thread-pool-async.md), [q15-concurrency-locks.md](./q15-concurrency-locks.md)

---

## §23. 마무리 — 이번 주 SQL API 서버로의 연결

지금까지 쌓은 모든 것이 이번 주 과제의 그림으로 수렴한다.

```text
[클라이언트]
   │ HTTP 요청 (본문에 SQL)
   ▼
[SQL API 서버]  ← Tiny 의 "main + accept 루프" + "스레드 풀 worker"
   │
   ├─ 요청 파싱     (Tiny 의 doit 와 같은 자리)
   ├─ SQL 추출      (parse_uri 대신 "본문에서 SQL 꺼내기")
   ├─ DB 실행       (serve_static 대신 "DB 엔진에 실행")
   ├─ 결과 직렬화    (HTML/JSON 으로)
   └─ HTTP 응답

[DB 엔진]
```

매핑하면:

| Tiny 구성 요소 | SQL API 서버의 역할 |
|---|---|
| main + accept | 동일 |
| Rio_readlineb / read_requesthdrs | HTTP 요청 파싱 |
| parse_uri | SQL 본문 추출 |
| serve_static/dynamic | DB 쿼리 실행 + 결과 직렬화 |
| (iterative) | **스레드 풀로 동시성 확장** |

이번 문서 순서대로 이해했다면:

- "왜 sockfd 를 read/write 로 다룰 수 있나?" → §6 VFS + sockfs
- "write 한 번이 어떻게 NIC 까지 가나?" → §9 파이프라인
- "TCP 가 왜 순서 보장이 되나?" → §11 seq/ack
- "프레임이 라우터를 지날 때 뭐가 바뀌나?" → §13 MAC/IP
- "왜 스레드 풀을 쓰나?" → §22 concurrent

각 의문이 이 문서의 한 섹션으로 매핑된다.

---

## 참고 연결

- [README.md — 문서 전체 인덱스](./README.md)
- [csapp-11/02-keyword-tree.md](../../csapp-11/02-keyword-tree.md) — CSAPP 11장 키워드 트리
- [csapp-11/05-ch11-sequential-numeric-walkthrough.md](../../csapp-11/05-ch11-sequential-numeric-walkthrough.md) — 95B HTTP 요청의 수치적 전 과정
- [csapp-11/07-ch11-code-reference.md](../../csapp-11/07-ch11-code-reference.md) — 주요 구조체/함수 소스 레퍼런스

### q 문서 전체 링크

- [q01-network-hardware.md](./q01-network-hardware.md) — §2 상세
- [q02-host-network-pipeline.md](./q02-host-network-pipeline.md) — §9, §15 상세
- [q03-tcp-udp-socket-syscall.md](./q03-tcp-udp-socket-syscall.md) — §8, §11 상세
- [q04-ip-address-byte-order.md](./q04-ip-address-byte-order.md) — §3 상세
- [q05-dns-domain-cloudflare.md](./q05-dns-domain-cloudflare.md) — §4 상세
- [q06-socket-principle.md](./q06-socket-principle.md) — §7 상세
- [q07-ch11-4-sockets-interface.md](./q07-ch11-4-sockets-interface.md) — §8 상세
- [q08-echo-server-datagram-eof.md](./q08-echo-server-datagram-eof.md) — §20 상세
- [q09-http-ftp-mime-telnet.md](./q09-http-ftp-mime-telnet.md) — §17 상세
- [q10-network-cpu-kernel-handle.md](./q10-network-cpu-kernel-handle.md) — §16 상세
- [q11-cgi-fork-args.md](./q11-cgi-fork-args.md) — §19 상세
- [q12-tiny-web-server.md](./q12-tiny-web-server.md) — §18 상세
- [q13-proxy-extension.md](./q13-proxy-extension.md) — §21 상세
- [q14-thread-pool-async.md](./q14-thread-pool-async.md) — §22 상세
- [q15-concurrency-locks.md](./q15-concurrency-locks.md) — §22 동시성/락 심화 (카페 비유, SQL API 서버 실전)
