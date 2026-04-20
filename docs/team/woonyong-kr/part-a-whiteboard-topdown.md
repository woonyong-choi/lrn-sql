# Part A. 네트워크 하드웨어 & 커널 송신 경로 — 화이트보드 탑다운 발표안

이 문서는 Part A 발표를 위해 만든 **실전용 화이트보드 원고**다.
목표는 단순히 키워드를 나열하는 것이 아니라, `write(sockfd, buf, 95)` 한 번이 **유저 공간 -> 커널 -> TCP/IP 스택 -> 드라이버 -> DMA -> NIC -> Ethernet 선로**로 이어지는 과정을 **실제 숫자와 실제 구조**로 끝까지 설명하는 것이다.

이 문서만 들고 들어가도 발표를 할 수 있어야 하고, 중간에 질문을 받아도 아래 장면 중 하나로 즉시 내려갈 수 있어야 한다.

## Part A 에서 끝까지 밀고 갈 한 문장

```text
앱이 write() 한 번 호출하면
유저 버퍼의 바이트가 커널 소켓 버퍼로 복사되고
TCP/IP 헤더가 붙고
다음 홉의 MAC 으로 Ethernet frame 이 재포장되고
드라이버가 DMA descriptor 를 NIC 에 넘긴 뒤
NIC 가 DRAM 에서 프레임을 읽어 실제 선로로 내보낸다.
```

이 한 문장을 화이트보드 전체에서 계속 반복한다.

## 발표 시작 전에 칠판에 미리 고정할 숫자

이 숫자는 발표 내내 바꾸지 말고 계속 재사용한다.

```text
클라이언트 호스트 A
  IP  = 128.2.194.242
  MAC = AA:AA:AA:AA:AA:AA
  src port = 51213

게이트웨이 라우터 R
  LAN1 쪽 MAC = 11:11:11:11:11:11
  LAN2 쪽 MAC = 22:22:22:22:22:22

서버 호스트 B
  IP  = 208.216.181.15
  MAC = BB:BB:BB:BB:BB:BB
  dst port = 80

HTTP payload
  "GET /home.html HTTP/1.0\r\n"
  "Host: www.example.net\r\n"
  "Connection: close\r\n"
  "Proxy-Connection: close\r\n"
  "\r\n"

payload size = 95B
TCP header   = 20B
IP header    = 20B
Eth header   = 14B
FCS          = 4B

wire size = 95 + 20 + 20 + 14 + 4 = 153B
```

이 숫자를 고정하면 질문이 들어와도 항상 같은 사례로 되돌아갈 수 있다.

## 화이트보드 배치

```text
+--------------------------------------------------------------------------------+
| 상단: 현재 장면                                                               |
| "write(4, GET..., 95) -> 153B Ethernet frame -> next hop"                     |
+--------------------------------------+-----------------------------------------+
| 왼쪽: 소프트웨어 경로                | 오른쪽: 실제 숫자와 헤더                |
| user -> syscall -> file -> socket    | src/dst IP, MAC, port, len, ttl         |
| -> tcp -> ip -> dev_queue_xmit       | TCP 20B, IP 20B, Eth 14B, FCS 4B        |
+--------------------------------------+-----------------------------------------+
| 하단: 끝까지 남길 키워드                                                    |
| MAC / IP / Port / copy_from_user / sk_buff / next hop / TTL / DMA / TX ring  |
+--------------------------------------------------------------------------------+
```

## 발표 흐름 전체 지도

```text
Scene 1   문제를 한 줄로 제시
Scene 2   주소 세 종류와 next hop
Scene 3   write() -> syscall -> fd -> socket 객체 체인
Scene 4   user -> kernel 복사와 소켓 버퍼
Scene 5   TCP 세그먼트 생성
Scene 6   IP 패킷화와 라우팅
Scene 7   Ethernet 재포장과 ARP
Scene 8   드라이버, qdisc, DMA, TX ring
Scene 9   라우터 한 홉 통과 시 실제로 바뀌는 것
Scene 10  write return, IRQ, EOF, echo 로 정리
```

---

## Scene 1. "우리가 열어볼 물음표"

칠판에 제일 먼저 이것만 쓴다.

```text
[ user process ]
    |
    | write(sockfd, payload95B, 95)
    v
  ??????
    v
[ Ethernet wire ]
```

이때 할 말:

`오늘 Part A 는 이 물음표를 열어보는 시간입니다. 네트워크가 추상적으로 느껴지는 이유는 이 안에 운영체제, 프로토콜, 하드웨어가 한꺼번에 숨어 있기 때문입니다.`

바로 다음 줄:

`하지만 실제로는 단계가 정해져 있습니다. 유저 버퍼, 커널 소켓 버퍼, TCP/IP 헤더, Ethernet header, DMA, NIC 순서입니다.`

## Scene 2. 먼저 주소 체계를 분리한다

칠판 왼쪽에 아래 표를 그린다.

```text
Port = 프로세스끼리
IP   = 호스트끼리
MAC  = 바로 옆 기계끼리
```

그리고 숫자를 대입한다.

```text
src port = 51213
dst port = 80

src IP   = 128.2.194.242
dst IP   = 208.216.181.15

이번 홉의 dst MAC = 11:11:11:11:11:11   (게이트웨이 R)
```

여기서 반드시 강조:

- **최종 목적지 IP** 는 서버 B 이다.
- 하지만 **이번 프레임의 목적지 MAC** 은 서버 B 가 아니라 **게이트웨이 라우터 R** 이다.

이때 던질 문장:

`IP 는 end-to-end 주소고, MAC 은 hop-by-hop 주소입니다. IP 는 B 를 가리키지만 첫 번째 Ethernet frame 은 R 에게 갑니다.`

질문이 들어오면 바로 답할 수 있어야 하는 것:

- 왜 MAC 이 서버 MAC 이 아니냐
- 왜 라우터는 MAC 이 두 개냐
- 왜 IP 는 유지되고 MAC 은 바뀌냐

---

## Scene 3. write() 가 들어가면 커널은 무엇을 찾는가

이제 `sockfd` 를 커널 객체로 확장해서 그린다.

```text
user
  sockfd = 4
     |
     v
fdtable[4] -> struct file
               |
               v
           struct socket
               |
               v
        struct sock / tcp_sock
```

이 장면에서 할 말:

`유저는 그냥 정수 4를 들고 있지만, 커널은 이 숫자를 따라가서 file, socket, sock 객체를 찾습니다.`

실제로 어디까지 가는지 한 줄 더 적는다.

```text
write(4, buf, 95)
 -> sys_write
 -> file->f_op->write_iter
 -> sock_write_iter
 -> sock_sendmsg
 -> tcp_sendmsg
```

핵심 메시지:

- 파일과 소켓이 같은 fd 추상화 안에 있다는 것
- 소켓도 결국 VFS 경유로 시작한다는 것
- `tcp_sendmsg` 같은 프로토콜별 함수로 내려간다는 것

초보자가 헷갈리는 포인트:

- 소켓은 "물리 장치"가 아니라 커널 객체다.
- 실제 물리 장치는 NIC 이고, 소켓은 그 NIC 를 쓰기 위한 논리적 엔드포인트다.

---

## Scene 4. user -> kernel 복사는 왜 필요한가

이제 유저/커널 경계를 따로 크게 그린다.

```text
유저 공간                        커널 공간
---------------------------------------------------------------
buf[95B]   -- copy_from_user -->   socket send buffer
                                   sk_buff chain
```

정확히 말할 문장:

`write가 리턴했다고 해서 선로로 나간 게 아닙니다. 일단 커널이 유저 버퍼를 자기 관리 영역으로 복사했다는 뜻입니다.`

여기서 꼭 설명해야 할 것:

1. 유저 메모리는 프로세스가 마음대로 바꿀 수 있다.
2. NIC 가 유저 메모리를 직접 믿고 읽게 둘 수 없다.
3. 그래서 커널이 먼저 안전한 커널 버퍼/`sk_buff` 체인으로 옮긴다.

그리고 이 문장을 꼭 남긴다.

```text
write return = "커널 버퍼에 받아 적음"
wire transmit = 그 이후의 일
```

추가로 말하면 좋은 디테일:

- 일반 `write` 에서는 유저 -> 커널 복사가 한 번 일어난다.
- `sendfile`, `splice`, `MSG_ZEROCOPY` 는 이 복사를 줄이기 위한 최적화다.

---

## Scene 5. TCP 세그먼트가 실제로 어떻게 만들어지나

이제 95B payload 위에 TCP 헤더를 붙인다.

```text
payload 95B
  +
TCP header 20B
  =
TCP segment 115B
```

헤더 필드도 적는다.

```text
src port = 51213 = 0xC82D
dst port = 80    = 0x0050
seq      = x
ack      = y
flags    = PSH | ACK
window   = rwnd
```

설명 포인트:

- 이 95B 는 MSS 1460B 보다 작아서 한 세그먼트로 충분하다.
- TCP 는 데이터에 순서를 부여하기 위해 sequence number 를 가진다.
- 송신 큐에는 보통 `sk_buff` 가 걸려 있고, 그 안에 TCP header + payload 가 붙는다.

중요한 보정:

- `write` 한 번 = 패킷 한 개는 아니다.
- TCP 는 stream 이므로 여러 write 가 합쳐지거나 하나가 나뉠 수 있다.

화이트보드에 함께 적을 것:

```text
MSS = 1500 - 20(IP) - 20(TCP) = 1460B
95B < 1460B -> 이번 예시는 분할 없음
```

---

## Scene 6. IP 계층: 라우팅과 TTL

이제 TCP segment 위에 IP header 를 붙인다.

```text
TCP segment 115B
  +
IP header 20B
  =
IP packet 135B
```

IP header 안에는 아래 값을 적는다.

```text
src IP       = 128.2.194.242
dst IP       = 208.216.181.15
TTL          = 64
Protocol     = 6 (TCP)
Total Length = 135
```

설명 포인트:

- IP 계층은 "어느 호스트로 갈 것인가"를 담당한다.
- 라우팅 테이블을 조회해 **다음 홉(next hop)** 을 결정한다.
- TTL 은 라우터를 하나 지날 때마다 1씩 감소한다.
- IP header checksum 은 TTL 이 바뀔 때마다 다시 계산된다.

꼭 남길 문장:

`IP 는 최종 목적지까지 유지되는 논리 주소고, 이 계층에서 라우팅 테이블이 next hop 을 고릅니다.`

이 장면에서 자주 받는 질문:

- 왜 라우팅 테이블이 "최종 목적지 전체"를 몰라도 되는가
- 왜 목적지는 B 인데 실제로는 R 로 보내는가
- TTL 이 왜 필요한가

한 줄 답:

`라우팅은 최종 목적지를 향해 매 홉마다 가장 가까운 다음 홉을 고르는 과정입니다.`

---

## Scene 7. Ethernet framing: MAC 을 다시 붙인다

이제 IP packet 을 Ethernet frame 으로 감싼다.

```text
Ethernet header 14B
  +
IP packet 135B
  =
149B frame

+ FCS 4B on wire
= 153B on wire
```

헤더 값은 꼭 적는다.

```text
dst MAC   = 11:11:11:11:11:11    <- 라우터 R 의 LAN1 쪽 MAC
src MAC   = AA:AA:AA:AA:AA:AA
EtherType = 0x0800 (IPv4)
```

그리고 아래 문장을 꼭 말한다.

`최종 서버 B의 MAC을 붙이지 않습니다. 같은 LAN 밖이면 지금 프레임의 목적지는 무조건 라우터입니다.`

ARP 까지 같이 설명한다.

```text
ARP cache miss -> "192.168.x.x 누구냐?"
ARP reply      -> "11:11:11:11:11:11 내가 그 IP"
ARP cache hit  -> 바로 Ethernet header 작성
```

정리 문장:

`IP 는 유지되지만 Ethernet header 는 hop 단위로 새로 만들어집니다. 그래서 MAC 은 계속 바뀝니다.`

---

## Scene 8. qdisc -> 드라이버 -> DMA -> NIC

이제 소프트웨어에서 하드웨어로 넘어가는 핵심 장면이다.

칠판에 세로 경로를 그린다.

```text
sk_buff
  |
  v
qdisc
  |
  v
dev_queue_xmit
  |
  v
driver->ndo_start_xmit
  |
  v
TX descriptor ring
  |
  v
NIC DMA reads DRAM
```

하나씩 설명:

1. `qdisc`
   - 송신 큐잉과 스케줄링 지점
   - 트래픽 제어나 공정성 정책이 붙을 수 있다

2. `driver`
   - `sk_buff` 를 NIC 가 이해하는 TX descriptor 로 바꾼다
   - MMIO 로 NIC 에 doorbell 을 울린다

3. `DMA`
   - CPU 가 149B 를 한 바이트씩 복사하는 것이 아니다
   - NIC 가 DRAM 에서 직접 읽어 간다

4. `NIC`
   - 읽어 온 프레임에 FCS 를 붙이고
   - PHY 를 통해 전기/광 신호로 직렬화한다

칠판에 반드시 적을 디테일:

```text
CPU job = 메타데이터 설정
DMA job = 실제 bulk data 이동
```

그리고 I/O bridge 도 여기서 연결한다.

```text
CPU / DRAM / PCIe NIC 사이 경로를
IO bridge / memory controller / PCIe root complex 가 중재
```

---

## Scene 9. 라우터를 한 번 지나면 실제로 무엇이 바뀌나

이 장면은 Part A 의 킬 포인트다. 실제로 숫자를 바꿔 보여 준다.

라우터 전:

```text
Frame on LAN1
  dst MAC = 11:11:11:11:11:11
  src MAC = AA:AA:AA:AA:AA:AA
  src IP  = 128.2.194.242
  dst IP  = 208.216.181.15
  TTL     = 64
```

라우터 후:

```text
Frame on LAN2
  dst MAC = BB:BB:BB:BB:BB:BB
  src MAC = 22:22:22:22:22:22
  src IP  = 128.2.194.242      (유지)
  dst IP  = 208.216.181.15     (유지)
  TTL     = 63                 (1 감소)
```

꼭 말할 문장:

`라우터는 IP 패킷을 그대로 꺼내 들고, Ethernet 바깥 껍데기만 새로 싸서 다음 LAN 으로 내보냅니다.`

이 장면에서 정리되는 것:

- IP 는 end-to-end
- MAC 은 hop-by-hop
- TTL 은 hop-by-hop 감소
- IP checksum 은 재계산
- TCP checksum 은 보통 end-to-end 유지

---

## Scene 10. write 의 리턴, IRQ, echo, EOF 로 마무리

마지막 장면은 처음 질문으로 되돌아온다.

```text
write return
   !=
wire complete

TX complete
 -> NIC interrupt / polling
 -> driver frees skb

peer receives
 -> peer read()
 -> peer write() back (echo)
 -> close / FIN / EOF
```

정리 멘트:

`write가 리턴한 시점은 커널이 송신 데이터를 자기 쪽으로 받아 적은 시점입니다. 실제 선로 송신은 그 뒤에 TCP 스케줄링, 드라이버, DMA, NIC를 거쳐 일어납니다.`

그리고 echo/EOF 를 연결해서 마무리:

`이 과정을 반대로 뒤집으면 수신 경로가 되고, 그 왕복 대칭을 가장 단순하게 보여 주는 예가 echo 서버입니다. close와 EOF를 붙이면 "보내는 것"뿐 아니라 "끝났다는 사실"까지 설명할 수 있습니다.`

---

## 발표 10분 압축 버전

```text
1. 문제 제시: write() 한 번이 어떻게 wire 까지 가나
2. Port / IP / MAC 역할 구분
3. fd -> file -> socket -> tcp_sock
4. copy_from_user 와 소켓 버퍼
5. TCP segment 115B
6. IP packet 135B 와 TTL
7. Ethernet frame 149B + FCS 4B = 153B
8. qdisc -> driver -> DMA -> NIC
9. 라우터에서 MAC 교체, TTL 감소
10. write return 과 실제 전송 완료는 다르다
```

## 질문 받으면 어디까지 내려갈지

- `소켓 버퍼는 어디 있나요?`
  - Scene 3, Scene 4 로 간다
  - `struct sock`, `sk_write_queue`, `sk_buff` 를 그린다

- `TCP/IP 스택은 유저 프로세스인가요 커널인가요?`
  - Scene 3, Scene 5, Scene 6 으로 간다
  - `tcp_sendmsg -> ip_output -> dev_queue_xmit` 라인을 다시 적는다

- `DMA 는 누가 누구 메모리를 읽는 건가요?`
  - Scene 8 로 간다
  - `NIC 가 DRAM 을 직접 읽는다`는 문장을 다시 강조한다

- `왜 목적지 IP 는 서버인데 목적지 MAC 은 라우터인가요?`
  - Scene 2, Scene 7, Scene 9 로 간다

- `write가 리턴했는데 왜 아직 안 나갔다고 하나요?`
  - Scene 4, Scene 10 으로 간다

## 연결 문서

- `q01-network-hardware.md`
- `q05-socket-principle.md`
- `q08-host-network-pipeline.md`
- `q10-io-bridge.md`
- `q14-echo-server-datagram-eof.md`
