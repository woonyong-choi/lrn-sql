# Part A. 네트워크 하드웨어 & 커널 송신 경로

`docs/question/01-team-question-parts.md` 의 Part A (A-1 ~ A-4) 질문 묶음에 대한 답을 한 문서에 정리한다.
각 섹션은 원 질문 목록을 맨 위에 두고, 그 질문들을 이어서 답하는 설명을 본문으로 제시한다.

## 커버하는 질문 매핑

| 질문 ID | 주제 | 관련 L 노드 |
|--------|------|------------|
| A-1 | 라우팅 심화 — IP/MAC 재포장, TTL | L2-6 |
| A-2 | 커널 송신 파이프라인 전체 (write → wire) | L9 |
| A-3 | I/O Bridge & DMA | L10 |
| A-4 | Echo Server, Datagram, EOF | L15 |

## Part A 를 관통하는 한 문장

앱이 `write()` 한 번 호출하면, 유저 버퍼의 바이트가 커널 소켓 버퍼로 복사되고, TCP/IP 헤더가 붙고, 다음 홉의 MAC 으로 Ethernet frame 이 재포장되고, 드라이버가 DMA descriptor 를 NIC 에 넘긴 뒤, NIC 가 DRAM 에서 프레임을 읽어 실제 선로로 내보낸다.

## 예시 상황 세팅

이 문서 전체가 아래 한 시나리오의 주소/포트 값을 공통 사례로 쓴다.

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

HTTP payload (95 B)
  "GET /home.html HTTP/1.0\r\nHost: www.example.net\r\n..."

크기 누적
  payload    = 95 B
  + TCP hdr  = 20 B  ->  TCP segment  = 115 B
  + IP hdr   = 20 B  ->  IP packet    = 135 B
  + Eth hdr  = 14 B  ->  Eth frame    = 149 B
  + FCS      =  4 B  ->  wire         = 153 B
```

---

## A-1. 라우팅 심화 — IP/MAC 재포장, TTL (L2-6)

### 원 질문

- 왜 Ethernet header 가 가장 바깥 포장지에 있는가? (최현진)
- 라우팅 테이블이 실제 목적지의 모든 세부 위치를 모르면 어떻게 목적지를 찾아가는가? (최현진)
- "다른 네트워크 사이에서 IP 를 보고 전달한다" 는 말이 무슨 뜻인가? (최현진)
- IPv4 header 안에 최종 주소가 있는데, 왜 굳이 MAC 으로 다시 포장해서 보내는가? (최현진)
- router R 을 지날 때 IP header 는 유지되고 MAC header 는 바뀐다는 것은 실제로 어떤 모습인가? (최현진)
- TTL 은 어떤 근거로 처음 값이 정해지는가? (최현진)

### 설명

6 개 질문은 모두 **"IP 와 MAC 은 유효 범위(scope)가 다르다"** 라는 사실 하나에서 나온다.

- IP 는 **end-to-end 주소**. 출발 호스트에서 최종 목적지 호스트까지 유지된다.
- MAC 은 **hop-by-hop 주소**. 바로 옆 기계 하나까지만 유효하다. 링크가 바뀌면 다시 써야 한다.

이 하나의 차이가 나머지를 전부 결정한다.

- **Ethernet header 가 가장 바깥**인 이유: 매 홉에서 벗겨내고 새로 씌워야 하기 때문에 가장 바깥에 있는 게 편하다. 안쪽(IP header)은 건드리지 않는다.
- **"IP 를 보고 전달한다"**: 라우터가 Ethernet 껍질을 벗기고 안쪽 IP header 의 dst IP 를 읽어 "다음 홉" 을 결정한다는 뜻이다.
- **라우팅 테이블이 모든 호스트를 몰라도 되는 이유**: dst IP 전체를 외우지 않고 **네트워크 prefix** 만 매칭한다. "목적지가 208.216.0.0/16 에 들면 en0 으로, 192.168.1.0/24 면 wlan0 으로" 같은 요약된 규칙 수백 개로 전 세계를 커버한다.
- **IP header 에 최종 주소가 있는데 왜 MAC 이 또 필요한가**: MAC 은 "선로 위 이번 프레임을 누가 받아야 하나" 의 답이다. 스위치는 IP 를 모른다. 스위치 · 허브가 동작하려면 이번 링크에서 유효한 MAC 이 필요하다.
- **라우터 한 홉 전/후**: IP header 는 그대로, MAC header 는 새로, TTL 은 1 감소, IP checksum 은 재계산, 그 아래 TCP payload 는 그대로.
- **TTL 초기값 64**: 전 세계 어디든 평균 20 ~ 30 홉이면 도달한다는 경험치에 **2 배 여유**를 둔 값. 리눅스·macOS 기본 64, Windows 128. 루프가 생겨도 패킷이 영원히 돌지 않게 한다.

### 선로 위 전체 프레임 구조

A 가 B 로 보내는 첫 프레임은 다음과 같이 포장된다. 가장 바깥이 Ethernet, 가장 안쪽이 payload 다.

```text
+=========================== Ethernet Frame (153 B on wire) ===========================+
| Ethernet Header (14 B)                                                               |
|   +--------------------------------------------------------------+                   |
|   | dst MAC    6 B   11:11:11:11:11:11    (라우터 R, 서버 B 아님) |                   |
|   | src MAC    6 B   AA:AA:AA:AA:AA:AA    (호스트 A)              |                   |
|   | EtherType  2 B   0x0800               ("안쪽은 IPv4")          |                   |
|   +--------------------------------------------------------------+                   |
+======================================================================================+
| IP Packet (135 B)                                                                    |
|   +--------------------------------------------------------------+                   |
|   | IP Header (20 B)                                              |                   |
|   |   Version    =  4                                             |                   |
|   |   IHL        =  5        (20 B / 4)                           |                   |
|   |   Total Len  =  135                                           |                   |
|   |   TTL        =  64       (hop 마다 -1)                        |                   |
|   |   Protocol   =  6        (TCP)                                |                   |
|   |   Checksum   =  0x????   (TTL 바뀌면 재계산)                   |                   |
|   |   src IP     =  128.2.194.242                                 |                   |
|   |   dst IP     =  208.216.181.15  (최종 목적지, 홉 지나도 유지)  |                   |
|   +--------------------------------------------------------------+                   |
|   | TCP Segment (115 B)                                           |                   |
|   |   TCP Header (20 B)                                           |                   |
|   |     src port = 51213      dst port = 80                       |                   |
|   |     seq, ack, flags (PSH | ACK), window, checksum, ...        |                   |
|   |   Payload (95 B)                                              |                   |
|   |     "GET /home.html HTTP/1.0\r\n..."                          |                   |
|   +--------------------------------------------------------------+                   |
+======================================================================================+
| FCS 4 B                                         (NIC 가 선로에서 붙임)                 |
+======================================================================================+
```

포장의 순서는 바깥부터 벗겨진다: **Ethernet → IP → TCP → Payload**. Ethernet 이 바깥에 있기에 라우터가 그것만 벗겼다 씌웠다 반복할 수 있다.

### IP header 를 비트로

각 필드가 실제 비트 단위 어디에 있는지 보면, "TTL 은 1 바이트, Protocol 은 1 바이트" 같은 감각이 생긴다.

```text
Bit offset |  0                   8                  16                  24                31
Octet 0    | Version(4) | IHL(4)   | TOS(8)           | Total Length (16)                    |
Octet 4    | Identification (16)                       | Flags(3) | Fragment Offset (13)     |
Octet 8    | TTL(8)     | Protocol(8) | Header Checksum (16)                                  |
Octet 12   | Source IP (32)                                                                   |
Octet 16   | Destination IP (32)                                                              |
```

구체적 비트 값:

```text
TTL = 64   = 0100 0000         (1 바이트)
TTL = 63   = 0011 1111         (라우터 1 홉 통과 후 단순 -1 뺄셈)

Protocol = 6 (TCP)  = 0000 0110
Protocol = 17 (UDP) = 0001 0001

EtherType = 0x0800 (IPv4) = 0000 1000  0000 0000
EtherType = 0x86DD (IPv6) = 1000 0110  1101 1101
EtherType = 0x0806 (ARP)  = 0000 1000  0000 0110
```

src IP 128.2.194.242 는 네트워크 바이트 순서(big-endian) 로 4 바이트 정렬된다.

```text
128 . 2 . 194 . 242
  |   |    |    |
 0x80 0x02 0xC2 0xF2
  |   |    |    |
 1000_0000  0000_0010  1100_0010  1111_0010

선로 위 바이트 순서:   80 02 C2 F2   (왼쪽부터)
```

TCP src port 51213 도 같은 방식.

```text
51213 = 0xC82D
      = 1100_1000  0010_1101

선로 위:   C8 2D  (big-endian)
tcpdump 에서: "port 51213 > port 80"
```

### 라우터 한 홉 전/후 비교

같은 IP 패킷이 R 의 LAN1 쪽에서 LAN2 쪽으로 넘어갈 때 바뀌는 필드와 유지되는 필드를 한 화면에 놓는다.

```text
                     LAN1 (R 의 입장)              LAN2 (R 의 출장)
                     ============================= ==============================
Ethernet dst MAC     11:11:11:11:11:11   (R)       BB:BB:BB:BB:BB:BB   (서버 B)
Ethernet src MAC     AA:AA:AA:AA:AA:AA   (A)       22:22:22:22:22:22   (R 의 LAN2)
Ethernet EtherType   0x0800                         0x0800
                     ----------------------------- (Ethernet 껍질은 벗기고 새로) --
IP dst IP            208.216.181.15                208.216.181.15      (유지)
IP src IP            128.2.194.242                 128.2.194.242       (유지)
IP TTL               64                            63                   (1 감소)
IP Checksum          0xABCD                        0x9E12               (재계산)
IP Protocol          6 (TCP)                       6 (TCP)
                     ----------------------------- (IP 아래는 건드리지 않음) ----
TCP segment          unchanged                     unchanged
Payload              unchanged                     unchanged
```

바뀌는 필드는 정확히 **Ethernet 전체 + TTL + IP Checksum** 세 군데. 이게 "IP 는 유지되고 MAC 은 hop 마다 바뀐다" 의 구체적 모습이다.

MAC 이 바뀌는 결정은 R 이 자기 라우팅 테이블에서 dst IP 208.216.181.15 에 대해 "next hop = 서버 B 의 IP" 를 뽑고, 그 IP 에 대응하는 MAC 을 ARP 로 확인한 결과다.

### ARP — IP 를 MAC 으로 바꾸는 순간

A 가 최초 프레임을 쏘기 직전, "R 의 MAC 을 모른다" 면 다음 교환이 선행된다.

```text
[A] ----- ARP Request ----->  (broadcast, dst MAC = FF:FF:FF:FF:FF:FF)
          "who has 192.168.0.1? tell 128.2.194.242"

[R] ----- ARP Reply  ------>  (unicast back to A)
          "192.168.0.1 is-at 11:11:11:11:11:11"
```

ARP 패킷의 EtherType 은 0x0806 이다 (IPv4 의 0x0800 과 다름). L2 에만 살고 IP 층 위로 올라오지 않는다.

이후 A 의 ARP 캐시에 `(192.168.0.1 → 11:11:11:11:11:11)` 가 남고, 같은 링크에서 같은 목적지를 또 부를 때는 캐시만 조회한다.

### 왜 TTL 이 없으면 안 되는가

라우팅 테이블은 dynamic routing protocol (BGP, OSPF 등) 로 분산 갱신된다. 어느 순간 잘못된 수렴이 일어나면 **A → R1 → R2 → R1 → R2 → ...** 루프가 만들어질 수 있다. TTL 없이는 패킷이 루프를 영원히 돈다. TTL 은 일종의 **"확정적 자폭 장치"** 로, 아무리 잘못 라우팅돼도 64 번 홉이면 사라진다.

TTL 이 0 이 된 순간 마지막 라우터는 원발신자에게 ICMP Time Exceeded 를 돌려준다. `traceroute` 는 이 성질을 역이용해서 TTL 을 1, 2, 3 ... 점점 늘리며 각 홉의 주인을 밝혀낸다.

### 직접 검증 — A-1

**검증 1. 내 호스트의 3 층 주소와 첫 홉**

```bash
# IP, MAC
ip -o -4 addr show
ip -o link show                      # link/ether 뒤의 값이 MAC

# 기본 라우트 = 이 호스트의 "첫 hop" (= R)
ip route show default
# default via 192.168.0.1 dev en0

# 그 R 의 MAC 은 ARP/neigh 테이블에 있다
ip neigh show | grep "$(ip route show default | awk '{print $3}')"
# 192.168.0.1 dev en0 lladdr 11:11:11:11:11:11 REACHABLE
```

`ip neigh` 가 보여준 MAC 이 곧 이번 프레임의 Ethernet dst MAC 이고, 서버 B 의 MAC 이 아니다. macOS 는 `ifconfig` · `netstat -rn` · `arp -an` 으로 동일 값을 뽑는다.

**검증 2. Ethernet header 와 ARP 를 실제 바이트로**

```bash
# Ethernet header 포함 덤프 (-e 가 핵심)
sudo tcpdump -i any -e -nn 'host 192.168.0.1' -c 2
# 11:22:33:44:55:66 > AA:BB:CC:DD:EE:FF, ethertype IPv4 (0x0800), length ...
#  dst MAC         src MAC              EtherType

# ARP 자체 캡처
sudo tcpdump -i any -nn arp -c 4
# ARP, Request who-has 192.168.0.1 tell 192.168.0.42
# ARP, Reply 192.168.0.1 is-at 11:22:33:44:55:66
```

Request/Reply 가 위 "ARP — IP 를 MAC 으로 바꾸는 순간" 블록의 정확한 실물이다.

**검증 3. TTL 이 홉마다 줄어드는 것**

```bash
# 라우팅 테이블 (prefix 규칙 수십 ~ 수백 개가 전 세계 커버)
ip route show

# 특정 목적지에 대해 커널이 고른 next hop
ip route get 208.216.181.15
# 208.216.181.15 via 192.168.0.1 dev en0 src 192.168.0.42

# TTL 감소를 한 홉씩 시각화
traceroute -n -q 1 www.example.net

# 내가 쏜 패킷의 TTL 기본값
sudo tcpdump -i any -v 'host www.example.net' -c 2
# ... IP (tos 0x0, ttl 64, ...)
```

`traceroute` 가 작동하는 원리 자체가 hop-by-hop TTL 감소가 실재한다는 직접 증거다.

**검증 4. 라우터 전/후를 같은 화면에서 (Linux netns)**

```bash
# veth 로 두 네임스페이스 연결
sudo ip netns add ns1
sudo ip netns add ns2
sudo ip link add veth1 type veth peer name veth2
sudo ip link set veth1 netns ns1
sudo ip link set veth2 netns ns2
sudo ip -n ns1 addr add 10.1.0.2/24 dev veth1 && sudo ip -n ns1 link set veth1 up
sudo ip -n ns2 addr add 10.1.0.3/24 dev veth2 && sudo ip -n ns2 link set veth2 up

# ns2 에서 덤프하면서 ns1 에서 ping
sudo ip netns exec ns2 tcpdump -i veth2 -e -nn -v icmp &
sudo ip netns exec ns1 ping -c 1 10.1.0.3
# ns2 프레임:
#   <ns2 MAC> > <ns1 MAC>         <- MAC 이 바뀜
#   src 10.1.0.2 dst 10.1.0.3     <- IP 유지
#   ttl 63                          <- 기본 64 에서 -1
```

위 "라우터 한 홉 전/후 비교" 표에 실험값을 겹치면 **"바뀌는 것 / 유지되는 것"** 이 완벽히 일치한다.

---

## A-2. 커널 송신 파이프라인 전체 (L9)

### 원 질문

- **최상위 흐름**: 프로세스가 `write(sockfd, buf, n)` 을 호출한 순간부터 이더넷 선로로 비트가 나가기까지, 호스트 내부에서 어떤 일이 일어나는가. user buffer → kernel socket buffer → TCP/IP 처리 → NIC driver → NIC → Ethernet 흐름을 쉽게 말하면? `write(sockfd, data)` 시 데이터가 커널 내부에서 어떻게 이동하는가. (최우녕, 최현진, 김희준)
- **user → kernel 버퍼 복사**: user buffer 에서 kernel socket buffer 로 왜 복사하는가. user → kernel 복사의 1 단계 상세 로직. user space → kernel space 의 데이터 이동. (최현진, 김희준)
- **buffer 개념**: buffer 란 무엇인가. (최현진)
- **sk_buff 수명**: 송수신 과정에서 sk_buff 는 어떻게 변하는가. NIC 가 전송 완료 interrupt 를 보낸 후 sk_buff 는 바로 삭제되는가. (최현진)
- **소켓 버퍼 위치·관리**: 소켓 버퍼는 어디에 있고 어떻게 관리되는가. (김희준)
- **VFS / sockfs / socket / TCP layer 내부 흐름**: VFS, sockfs, socket layer, TCP layer 내부 흐름. 커널 내부 TCP/IP 스택의 동작. (최현진, 김희준)
- **RAM 가상/물리 주소**: network data 는 하드웨어적으로 어디에 들어오고, user/kernel memory 와 어떻게 관련되는가. network data 가 RAM 의 가상주소에 들어가는가 물리주소에 들어가는가. (최현진)
- **프로토콜 SW 위치**: "프로토콜 소프트웨어는 결국 프로세스" 라는데 어디에 있는가. 유저 프로세스인가 커널인가. (최우녕)
- **실제 숫자 예시**: IP, 포트, 프레임 크기로 대입해서. (최우녕)

### 설명

9 개 질문은 전부 한 문장의 연속 확장이다: **"유저가 본 정수 sockfd 하나를 타고 커널이 sk_buff 를 만들어 NIC 까지 배달하는 한 사이클."**

1. 유저는 `sockfd = 4` 라는 정수 하나를 들고 있다. `write(4, buf, 95)` 가 시스템콜로 들어가면, 커널은 현재 프로세스의 **fdtable → struct file → struct socket → struct sock** 객체 체인을 타고 내려가 이 소켓의 상태 머신에 도달한다.
2. VFS 는 `sys_write → file->f_op->write_iter` 로 추상화되어 있고, 소켓의 `f_op` 가 `sock_write_iter` 라서 일반 파일 쓰기와 동일 경로로 소켓에 도달한다. 이 덕에 "모든 것이 파일" 이 성립한다.
3. `sock_sendmsg → tcp_sendmsg` 가 실행되며, **copy_from_user** 가 유저 가상 주소 `buf` 에서 바이트를 읽어 커널 슬랩에 할당된 `sk_buff` 로 옮긴다. 복사의 이유: 유저 메모리는 프로세스가 언제든 수정할 수 있고, NIC 가 유저 메모리를 DMA 로 직접 읽게 두면 데이터 일관성이 깨진다.
4. `sk_buff` 들은 `struct sock->sk_write_queue` 에 FIFO 로 붙는다. 이 큐가 "**소켓 송신 버퍼**" 의 정체다. 물리적으로는 **커널 가상 주소 공간의 슬랩 할당자** 위에 있다.
5. TCP 층이 segment 경계를 잘라 seq/ack/flags 를 쓰고, IP 층이 src/dst/TTL 을 쓰고, `dev_queue_xmit` 가 qdisc 를 거쳐 driver 의 `ndo_start_xmit` 로 넘긴다.
6. driver 는 `sk_buff` 의 데이터 위치(가상 주소)를 **dma_map_single** 로 **DMA 주소(bus 주소)** 로 변환한 뒤, NIC 의 TX descriptor ring 에 쓴다. MMIO 를 통해 NIC 에 doorbell 을 울린다.
7. NIC 는 DRAM 에서 **DMA 로** 프레임을 직접 읽어 PHY 로 직렬화한다. 송신이 끝나면 TX completion interrupt 를 올린다.
8. interrupt handler 가 `consume_skb` 로 `sk_buff` 를 해제한다. 즉 **NIC 완료 interrupt 가 와야 비로소 sk_buff 가 사라진다**. 자동 소멸이 아니다.
9. "프로토콜 SW 는 어디 있나" — 위 모든 코드는 **커널 코드**지만 **현재 프로세스의 시스템콜 컨텍스트에서 실행**된다. 유저 프로세스가 시스템콜을 타고 커널로 들어가 "잠시 커널 코드를 실행하는 프로세스" 가 된다. 유저 프로세스가 직접 실행하는 건 아니지만, 프로세스의 이름(`task_struct`)으로 움직인다.

### `write(4, buf, 95)` — 시스템콜 경로

```text
user space
┌─────────────────────────────────────┐
│  char buf[95] = "GET /home.html..." │
│  write(4, buf, 95);                 │
└──────────────┬──────────────────────┘
               │ SYSCALL
               ▼
kernel space
┌─────────────────────────────────────────────────────────────────┐
│  sys_write(fd=4, buf=user_ptr, count=95)                       │
│    └─► fdget(4) → struct file *                                 │
│          └─► file->f_op->write_iter                             │
│                  = sock_write_iter()         (소켓 전용 f_op)    │
│                      └─► sock_sendmsg()                         │
│                              └─► inet_sendmsg()                 │
│                                      └─► tcp_sendmsg()          │
│                                              ├─► alloc sk_buff  │
│                                              ├─► copy_from_user │
│                                              └─► enqueue to     │
│                                                  sk_write_queue │
└─────────────────────────────────────────────────────────────────┘
```

여기까지가 "유저 → 커널 복사" 의 한 사이클이다. 이 시점에서 `write` 는 이미 리턴할 수 있다. **실제 선로 전송은 아직 일어나지 않았다.**

### 커널 객체 체인: fdtable → file → socket → sock

`sockfd = 4` 는 현재 프로세스의 `task_struct->files->fdt->fd[4]` 를 가리키는 정수에 불과하다. 그 한 칸이 객체 포인터 체인의 시작이다.

```text
task_struct (현재 프로세스)
   └─ files : struct files_struct *
         └─ fdt : struct fdtable *
               └─ fd[0]  ─► struct file (stdin,  /dev/ttys...)
                  fd[1]  ─► struct file (stdout)
                  fd[2]  ─► struct file (stderr)
                  fd[3]  ─► struct file (listen socket)
                  fd[4]  ─► struct file   ────────────┐
                  ...                                  │
                                                       ▼
                                            ┌─────────────────────┐
                                            │ struct file         │
                                            │   f_op = &socket_   │
                                            │         file_ops    │
                                            │   private_data ─────┼──┐
                                            └─────────────────────┘  │
                                                                     ▼
                                                         ┌──────────────────┐
                                                         │ struct socket    │
                                                         │   ops = &inet_   │
                                                         │         stream_  │
                                                         │         ops      │
                                                         │   sk  ───────────┼──┐
                                                         └──────────────────┘  │
                                                                               ▼
                                                         ┌──────────────────────┐
                                                         │ struct sock          │
                                                         │   sk_state = ESTAB   │
                                                         │   sk_sndbuf = 2MB    │
                                                         │   sk_write_queue     │
                                                         │     [skb][skb][skb]  │
                                                         │   sk_rmem_alloc      │
                                                         │   sk_wmem_alloc      │
                                                         │   ...                │
                                                         └──────────────────────┘
```

TCP 가 붙은 `struct sock` 은 내부적으로 `struct tcp_sock` 으로 캐스팅되어 snd_una / snd_nxt / rcv_nxt 같은 TCP 상태를 들고 있다.

### sk_buff — 커널이 네트워크 데이터를 들고 다니는 그릇

소켓 버퍼의 entry 단위는 `struct sk_buff`. 하나당 한 "메시지/세그먼트/프레임" 을 표현한다.

```text
struct sk_buff (메타데이터)
┌───────────────────────────────┐
│ next / prev    (큐 연결)        │
│ dev            (네트워크 장치)   │
│ sk             (속한 struct sock)│
│ len            (전체 길이)       │
│ data_len       (paged 부분 길이) │
│ truesize       (실제 점유량)     │
│ head ──────────────────┐        │
│ data ─────────────────┐│        │
│ tail ────────────────┐││        │
│ end  ───────────────┐│││        │
└───────────────────────────────┘
                     ││││
                     ▼▼▼▼
          data buffer (실제 바이트)
          ┌──────────────────────┐
          │ <headroom>           │  head
          │ ┌─── Eth hdr ────┐   │  <- data 가 여기를 가리키게 될 수 있다
          │ ├─── IP hdr  ────┤   │
          │ ├─── TCP hdr ────┤   │
          │ ├─── payload ────┤   │
          │ └────────────────┘   │  tail
          │ <tailroom>           │  end
          └──────────────────────┘
```

계층을 내려갈 때마다 `skb_push(skb, hdr_size)` 로 `data` 를 앞당겨서 **같은 버퍼에 제자리에서 헤더를 쌓는다**. 복사 없이 포인터만 민다 — 커널이 네트워크 처리에서 오버헤드를 줄이는 핵심 기법이다.

sk_buff 의 수명:

```text
[1] alloc_skb()             <- tcp_sendmsg() 에서 생성
[2] copy_from_user()        <- 유저 버퍼 → skb->data
[3] skb_queue_tail(sk_write_queue, skb)   <- 소켓 송신 큐에 걸림
[4] tcp_write_xmit() → tcp_transmit_skb() <- IP 로 내려가기
[5] ip_output → dev_queue_xmit → qdisc → driver
[6] dma_map_single(skb->data)             <- bus 주소로 매핑
[7] NIC 가 DMA 로 읽어 선로로 송출
[8] NIC TX completion IRQ
[9] skb_tx_timestamp / napi_consume_skb   <- free
```

즉 `sk_buff` 는 **NIC 의 TX completion 인터럽트가 올라와야 해제**된다. "write 리턴했으니 끝" 이 아니다. 재전송(retransmit) 을 대비해 일부 TCP 구현은 ACK 가 돌아올 때까지 복사본을 더 붙잡고 있기도 한다.

### user → kernel 복사는 왜 필요한가

```text
┌────── user space ──────┐          ┌────── kernel space ──────┐
│                         │          │                           │
│   buf[95]  ────────────╋──────────▶  sk_buff->data (95 B)      │
│   (가상주소 0x7fff...)  │  copy   │  (가상주소 0xffffc90000...) │
│                         │  from_  │                           │
│                         │  user   │                           │
└─────────────────────────┘          └───────────────────────────┘
```

복사하는 이유:

1. 유저 메모리는 프로세스가 언제든 덮어쓸 수 있다. NIC DMA 가 진행 중에 바뀌면 선로 위 바이트가 중간에 깨진다.
2. 유저 메모리는 스왑 아웃될 수 있다. 드라이버가 "지금 여기 읽어" 라고 지시했는데 그 순간 스왑 디스크에 가 있다면 NIC 는 엉뚱한 값을 읽는다.
3. 유저 주소는 MMU 매핑에만 유효하고, NIC 가 쓰는 DMA (bus) 주소 공간과 다르다. 커널 버퍼는 DMA-able 영역에서 할당돼 `dma_map_single` 로 안전하게 매핑된다.

`sendfile` · `splice` · `MSG_ZEROCOPY` 는 정확히 이 복사를 줄이는 특수 경로다. zerocopy 는 유저 페이지를 pinning 해서 DMA 로 직접 읽되, 복사 없는 대가로 페이지 고정과 완료 통지 비용을 감당한다.

"buffer 란 무엇인가" 에 대한 한 줄 답: **생산자 속도와 소비자 속도 사이의 차이를 흡수하는 중간 저장소**. 여기선 "유저의 `write` 속도" 와 "NIC 의 송신 속도" 가 100 배 이상 다를 수 있어 그 사이에 sk_write_queue 가 들어가 있는 것이다.

### 주소 공간 세 종류 — 어디에 data 가 들어 있나

network data 는 어느 주소에 있느냐는 질문의 답은 **세 가지 다른 주소 공간이 동시에 가리키는 같은 물리 바이트** 다.

```text
┌─── 유저 가상 주소 공간 ───┐   ┌─── 커널 가상 주소 공간 ───┐
│                            │   │                            │
│  buf     = 0x7fff_1234_5000│   │  skb->data = 0xffff_c900_...│
│                            │   │                            │
└────────────┬───────────────┘   └────────────┬───────────────┘
             │                                │
             ▼  (MMU 페이지 테이블로 번역)       ▼
       ┌──────────────── 물리 주소 공간 ─────────────────┐
       │                                                  │
       │   물리 주소 0x0000_0001_2345_0000 (DRAM 실주소)  │
       │                                                  │
       └──────────────────────────┬───────────────────────┘
                                  │ (IOMMU 가 있으면 여기서 bus 주소로 재매핑)
                                  ▼
                       ┌─── DMA (bus) 주소 공간 ───┐
                       │                            │
                       │  NIC 가 보는 주소            │
                       │  0xfee0_0000_0000_0000      │
                       │                            │
                       └────────────────────────────┘
```

요점:

- **유저 가상 주소**: 프로세스만 보는 주소. MMU 페이지 테이블로 물리 주소로 번역된다.
- **커널 가상 주소**: 커널이 sk_buff 를 가리킬 때 쓴다. 같은 물리 페이지를 다른 가상 주소로 매핑한 경우가 많다.
- **물리 주소**: DRAM 셀 위치. DDR 채널/랭크/뱅크로 구체화된다.
- **DMA 주소 (bus 주소)**: PCIe NIC 이 보는 주소. IOMMU 가 있으면 물리 주소와 다르고, 없으면 같다.

network data 자체는 **물리 DRAM 셀** 에 올라가 있다. 유저·커널·NIC 이 각자 **자기 좌표계** 로 그 셀을 가리킬 뿐이다. "가상이냐 물리냐" 가 아니라 **"누가 보느냐" 에 따라 주소가 달라진다**.

### TCP / IP / Eth 헤더가 붙는 실제 숫자

`tcp_sendmsg` 이후 아래 순서로 헤더가 쌓인다. sk_buff 의 `head/data/tail` 이 어떻게 움직이는지까지 보이도록 그린다.

```text
Step 0: alloc_skb 직후
  head ──► [ headroom (예: 128 B) ]
  data ──► [ payload 95 B           ]
  tail ──► [                         ]
  end  ──► [                         ]

Step 1: skb_push(skb, 20)  -> TCP 헤더 20 B 만큼 data 를 앞당김
  head ──► [ headroom ]
  data ──► [ TCP 20 B | payload 95 B ]
  tail ──► [                          ]
  end  ──► [                          ]

Step 2: skb_push(skb, 20)  -> IP 헤더
  data ──► [ IP 20 B | TCP 20 B | payload 95 B ]

Step 3: dev_queue_xmit → driver → Ethernet header 붙이기
  data ──► [ Eth 14 B | IP 20 B | TCP 20 B | payload 95 B ] = 149 B

Step 4: NIC 가 FCS 4 B 붙여 선로로 보냄
                                                                → wire 153 B
```

계층마다 **복사 없이** `skb_push` 로 포인터만 민다는 점이 핵심이다.

### 프로토콜 소프트웨어는 어디에 있나

"프로토콜 소프트웨어가 결국 프로세스" 라는 말은 두 가지를 동시에 뜻한다.

1. **코드 위치**: 모든 프로토콜 처리 코드(tcp_sendmsg, ip_output, dev_queue_xmit, driver 등)는 **커널 이미지 안** 에 있다. 유저 프로세스가 별도로 탑재하지 않는다.
2. **실행 컨텍스트**: 그 코드는 **현재 시스템콜을 부른 프로세스의 컨텍스트** 에서 실행된다. `write` 를 부른 프로세스가 CPU 를 잡은 채로 커널 코드를 실행한다. `current` (현재 프로세스 포인터) 가 그대로 유지된다.

예외는 수신 경로의 일부와 NIC TX 완료 처리 — 이쪽은 soft IRQ 나 `ksoftirqd` 커널 스레드 컨텍스트에서 돌아서, 원래 프로세스가 뭘 하고 있었든 상관없이 처리된다.

그래서 "프로토콜은 유저인가 커널인가" 의 답은 "**커널 코드다. 하지만 송신은 주로 유저 프로세스의 시스템콜 컨텍스트에서 돈다**" 가 된다.

### 직접 검증 — A-2

**검증 1. sockfd 를 따라 커널 객체로**

```bash
# 터미널 1: 서버 역할
nc -l 8080

# 터미널 2: 클라이언트 프로세스에서
ls -l /proc/$$/fd               # 0 1 2 외에 socket:[xxxx] 가 뜸

# 그 socket inode 가 실제 TCP 소켓으로 잡혀 있는지
ss -tanpie | grep 8080

# write() 가 어느 syscall 로 내려가는지
strace -e trace=write,sendto,sendmsg -f nc 127.0.0.1 8080
# write(3, "hello\n", 6) = 6      <- fd 3 = struct socket 의 입구
```

`/proc/PID/fd/3` 의 `socket:[inode]` → `ss` 에서 같은 inode → `strace` 의 `write(3, ...)`. 세 지점이 **한 호흡으로** 연결된다.

**검증 2. 소켓 버퍼 크기와 "아직 안 나간 바이트"**

```bash
# 시스템 기본 송신 버퍼 크기
cat /proc/sys/net/core/wmem_default
cat /proc/sys/net/core/wmem_max
cat /proc/sys/net/ipv4/tcp_wmem         # min default max

# 내 연결이 실제로 쓰는 SO_SNDBUF 와 현재 상태
ss -tmi '( dport = :8080 )' | grep -E 'skmem|cwnd|bytes_acked'
# skmem:(r0,rb131072,t0,tb2626560,...)   <- tb = tx buffer size
# bytes_acked:N                           <- peer 가 확인한 바이트

# "write return 했는데 아직 wire 로 안 나간" 증거
ss -tanp | grep 8080
# State   Recv-Q  Send-Q
# ESTAB   0       4096      <- Send-Q 가 곧 sk_write_queue 의 길이
```

`Send-Q > 0` 이 **sk_write_queue 에 아직 소비되지 않은 sk_buff** 가 쌓여 있다는 실물 증거다.

**검증 3. TCP 헤더를 실제 바이트로**

```bash
# loopback 8080 트래픽을 바이트 단위로
sudo tcpdump -i lo -X -nn -vv 'tcp port 8080' -c 4
# ... seq 1000:1095, ack 9001, win 64240, options [...], length 95
# 0x0014:  c82d 0050 000003e8    <- src_port=0xc82d(51213) dst_port=0x0050(80)

# 현재 연결의 MSS / cwnd / rto / rtt
ss -tin '( dport = :8080 )'
# rto:204 rtt:0.1/0.05 mss:1460 cwnd:10 bytes_sent:95 bytes_acked:95
```

`mss:1460` 이 `1500 - 20(IP) - 20(TCP) = 1460` 계산과 정확히 일치한다. 95 B payload 는 이 한계를 넘지 않아 한 segment 에 담긴다.

**검증 4. 시스템콜 경로 전체를 한 화면에**

```bash
# sys_write → sock_sendmsg → tcp_sendmsg 까지의 진입을 동시 추적
sudo bpftrace -e '
  kprobe:ksys_write    { printf("ksys_write pid=%d\n", pid); }
  kprobe:sock_sendmsg  { printf("  sock_sendmsg\n"); }
  kprobe:tcp_sendmsg   { printf("    tcp_sendmsg bytes=%d\n", arg2); }
  kprobe:dev_queue_xmit { printf("      dev_queue_xmit\n"); }
'
# 클라이언트에서 nc 127.0.0.1 8080 으로 데이터 한 줄 쏘면
# ksys_write pid=12345
#   sock_sendmsg
#     tcp_sendmsg bytes=6
#       dev_queue_xmit
```

bpftrace 가 실제 커널 함수 진입을 한 줄씩 보여준다 — 위 "시스템콜 경로" 다이어그램의 각 단계에 해당한다.

---

## A-1 · A-2 통합: 비트 버퍼가 NIC 을 나와 상대 소켓 큐에 들어올 때까지

### 이 섹션의 목표

A-1 (라우팅 개념) 과 A-2 (커널 송신 파이프라인) 를 실제 **비트 덤프**로 이어붙인다.
payload 한 줄 `"GET /home.html HTTP/1.0\r\n..."` 이 유저 버퍼에 있는 순간부터, 상대 호스트의 `sk_receive_queue` 에 들어갈 때까지 바이트·비트가 어떻게 변형되는지를 단계별로 추적한다.

이 섹션을 통과하고 나면 아래 질문에 전부 답할 수 있어야 한다.

- 기본 payload 버퍼에 TCP / IP / Ethernet 헤더가 **어떻게 붙어서** 선로로 나가는가
- 그 비트 버퍼에는 **정확히 어떤 데이터**가 **어떤 비트 자리**에 들어 있는가
- 홉에서 **다음 홉의 MAC** 을 어떻게 찾는가
- **무결성**은 각 계층에서 **무엇**으로 체크하는가 (FCS / IP checksum / TCP checksum)
- **TTL** 은 홉마다 어떤 비트 연산으로 바뀌는가
- 도착한 프레임을 어떻게 **역순으로 쪼개어** PF_INET 소켓의 큐에 넣는가

### STEP 0. 출발 직전 — skb 의 메모리 맵

앱이 `write(fd, buf, 95)` 를 호출했다고 하자. 유저 버퍼 `buf` 에 95 바이트 payload 가 담겨 있다.
커널은 `tcp_sendmsg` 에서 `sk_stream_alloc_skb()` 로 새 `struct sk_buff` 를 할당하고, 그 안의 선형 버퍼(linear area)에 payload 를 **`copy_from_user`** 로 옮긴다.

중요한 건 이 skb 를 만들 때 미리 **headroom** 을 예약해 둔다는 점이다. headroom 이 Ethernet(14) + IP(20) + TCP(20) = 54 바이트 이상 확보되어 있어야, 나중에 헤더를 붙일 때 **추가 메모리 복사 없이** `skb->data` 포인터만 뒤로 밀어서 공간을 확보할 수 있다.

```text
skb 의 선형 버퍼 (kernel heap 영역)

  skb->head -------->┐
                     │ headroom (예: 128 B 예약)
                     │    ↑ 이 공간으로 skb_push 가 확장됨
  skb->data -------->┤    ← 지금 여기 (payload 첫 바이트)
                     │ payload 95 B
                     │    "GET /home.html HTTP/1.0\r\n..."
  skb->tail -------->┤    ← payload 끝
                     │ tailroom
  skb->end  -------->┘

포인터 값 예시 (head 를 0 으로 놓고 상대 오프셋)

  head = 0
  data = 128       (headroom 128 B)
  tail = 128 + 95 = 223
  end  = 256
  len  = tail - data = 95
```

이 상태에서 `skb->data` 가 가리키는 첫 16 바이트를 hex 로 뜨면:

```text
offset 0x00:  47 45 54 20 2F 68 6F 6D  65 2E 68 74 6D 6C 20 48
              G  E  T  _  /  h  o  m   e  .  h  t  m  l  _  H
```

비트로는 첫 바이트 `0x47 = 0100_0111` 이 ASCII 'G' 이다. 유저 공간에서 적은 그대로, 단 한 비트도 바뀌지 않고 커널 heap 에 복사되어 있다.

### STEP 1. TCP 헤더 스택 (skb_push 1 차, 20 바이트)

`tcp_transmit_skb()` 가 `skb_push(skb, 20)` 을 호출한다. 내부적으로 이 한 줄이다.

```c
skb->data -= 20;
skb->len  += 20;
return skb->data;    // 새 TCP 헤더의 시작점
```

포인터만 뒤로 밀었을 뿐 메모리 복사는 없다. 그다음 그 20 바이트에 TCP 헤더 비트를 기록한다.

```text
TCP header 20 B 의 비트 레이아웃 (각 줄 = 32 비트 = 4 바이트)

 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgement Number                     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| DOff|Rsv|  Flags  |            Window                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

예시 상황 값으로 채운 실제 비트 덤프:

```text
src port 51213       = 0xC82D  = 1100_1000  0010_1101
dst port    80       = 0x0050  = 0000_0000  0101_0000
seq      0x12345678           = 0001_0010 0011_0100 0101_0110 0111_1000
ack      0x00000000           = 0000_0000 0000_0000 0000_0000 0000_0000
data offset = 5 (×4 = 20 B)
reserved    = 000
flags       = PSH(1) ACK(1) = 0001_1000   → 0x18
window   65535       = 0xFFFF  = 1111_1111  1111_1111
checksum             = 0x???? ← 아직 0, 뒤에서 채움
urgent ptr           = 0x0000

16진 덤프 (20 B):
offset 0x00:  C8 2D 00 50 12 34 56 78  00 00 00 00 50 18 FF FF
offset 0x10:  ?? ?? 00 00

필드별 위치 (byte offset):
  [0..1]  src port
  [2..3]  dst port
  [4..7]  seq
  [8..11] ack
  [12]    DOff|Rsv (상위 4 비트가 DOff=5)
  [13]    flags    (0x18 = PSH|ACK)
  [14..15] window
  [16..17] checksum
  [18..19] urgent
```

**포인터 상태 변화**:

```text
이전:  data = 128,         len = 95
이후:  data = 128 - 20 = 108,  len = 115
```

TCP checksum 은 지금 시점엔 0 으로 두고, IP 헤더까지 붙인 뒤 **pseudo-header 를 포함**해서 최종 계산한다 (STEP 2 끝에서).

### STEP 2. IP 헤더 스택 (skb_push 2 차, 20 바이트)

`ip_queue_xmit()` 가 다시 `skb_push(skb, 20)`. data 포인터가 20 바이트 더 뒤로 밀리고, 그 자리에 IPv4 헤더를 쓴다.

```text
IPv4 header 20 B 의 비트 레이아웃

 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Version|  IHL  |Type of Service|          Total Length         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Identification        |Flags|      Fragment Offset    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Time to Live |    Protocol   |         Header Checksum       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Source Address                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Destination Address                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

예시 값으로 채우기:

```text
Version=4, IHL=5                 → 상위 4비트 0100, 하위 4비트 0101
                                   = 0100_0101 = 0x45

ToS = 0                          = 0000_0000 = 0x00

Total Length = 20(IP) + 115(TCPseg) = 135
                                   = 0x0087 = 0000_0000 1000_0111

Identification = 0x1234          = 0001_0010 0011_0100

Flags|FragOff:
  Flags = DF (Don't Fragment) = 010
  FragOff = 0
  합치면 16비트 = 0100_0000 0000_0000 = 0x4000

TTL = 64                         = 0x40 = 0100_0000
Protocol = TCP (6)               = 0x06 = 0000_0110
Header Checksum = 0x????         ← 아직 0, 이 헤더 자체로 계산

src IP 128.2.194.242:
  128 = 1000_0000
    2 = 0000_0010
  194 = 1100_0010
  242 = 1111_0010
  → 1000_0000 0000_0010 1100_0010 1111_0010
    = 0x8002 C2F2

dst IP 208.216.181.15:
  208 = 1101_0000
  216 = 1101_1000
  181 = 1011_0101
   15 = 0000_1111
  → 1101_0000 1101_1000 1011_0101 0000_1111
    = 0xD0D8 B50F
```

**IP header checksum 비트 연산** (헤더 20 바이트, 16 비트 단위, 1 의 보수 합):

```text
16 비트 워드로 자르기 (checksum 필드는 0 으로 두고 합산):

  0x4500   (Ver|IHL|ToS)
  0x0087   (Total Length)
  0x1234   (Identification)
  0x4000   (Flags|FragOff)
  0x4006   (TTL|Protocol)
  0x0000   (Checksum 필드 = 0)
  0x8002   (src IP high)
  0xC2F2   (src IP low)
  0xD0D8   (dst IP high)
  0xB50F   (dst IP low)
  ──────
  sum = 0x4500 + 0x0087 + 0x1234 + 0x4000 + 0x4006
      + 0x0000 + 0x8002 + 0xC2F2 + 0xD0D8 + 0xB50F
      = 0x2_A6F2     (carry 2 가 32비트 상위에)

carry fold (상위 16 비트를 하위에 더함):
      0xA6F2 + 0x0002 = 0xA6F4

1 의 보수 (비트 반전):
      ~0xA6F4 = 0x590B

→ Header Checksum = 0x590B
  비트로 0101_1001 0000_1011
```

최종 IP 헤더 20 바이트 덤프:

```text
offset 0x00:  45 00 00 87 12 34 40 00  40 06 59 0B 80 02 C2 F2
offset 0x10:  D0 D8 B5 0F
```

**이제 TCP checksum 을 채운다**. TCP checksum 은 pseudo-header (src IP 4 + dst IP 4 + zero 1 + proto 1 + TCP length 2 = 12 B) + TCP 세그먼트 전체 (115 B) 에 대해 계산한다.

```text
pseudo-header 비트:
  src IP  = 80 02 C2 F2
  dst IP  = D0 D8 B5 0F
  zero    = 00
  proto   = 06           (TCP)
  TCP len = 00 73        (115 = 0x0073)

→ 이 12 B + TCP header 20 B (checksum 필드 0 으로) + payload 95 B
  을 모두 16 비트 단위로 1 의 보수 합, carry fold, 비트 반전.

  예시 결과: TCP checksum = 0x7A1C
  비트 0111_1010 0001_1100
```

TCP 헤더 16~17 바이트 자리에 `7A 1C` 를 써 넣는다. **여기까지 오면 L3/L4 헤더는 전부 고정**되었다.

**포인터 상태 변화**:

```text
이전:  data = 108,         len = 115
이후:  data = 108 - 20 = 88,   len = 135
```

### STEP 3. Ethernet 헤더 스택 (skb_push 3 차, 14 바이트)

드라이버 진입 직전, `eth_header()` 가 `skb_push(skb, 14)` 로 L2 헤더 자리를 연다. 단, dst MAC 을 채우려면 **다음 홉의 MAC** 을 알아야 한다. 이건 ARP 로 해결한다.

```text
호스트 A 가 src IP 128.2.194.242 에 있고, dst IP 208.216.181.15 은
같은 서브넷이 아니다 (라우팅 테이블이 "default → gateway 128.2.194.1"
을 가리킨다).

→ next-hop IP = 128.2.194.1 (게이트웨이 R)
→ 그 IP 의 MAC 이 neighbour table 에 없으면 ARP 를 쏜다:

ARP request 프레임 42 B (브로드캐스트):
  dst MAC = FF:FF:FF:FF:FF:FF        (broadcast)
  src MAC = AA:AA:AA:AA:AA:AA        (A 의 MAC)
  EtherType = 0x0806                  (ARP)
  HType=1 (Eth), PType=0x0800 (IPv4)
  HLen=6, PLen=4
  Operation=1 (request)
  sender MAC = AA:AA:AA:AA:AA:AA
  sender IP  = 128.2.194.242
  target MAC = 00:00:00:00:00:00
  target IP  = 128.2.194.1

→ 게이트웨이 R 이 reply:
  Operation=2 (reply)
  sender MAC = 11:11:11:11:11:11    ← 이게 다음 홉 MAC
  sender IP  = 128.2.194.1

→ A 의 neighbour table 에 (128.2.194.1, 11:11:11:11:11:11) 캐시.
```

이제 Ethernet 헤더 14 바이트를 채운다.

```text
Ethernet header 14 B 의 비트 레이아웃

 0           1           2           3
 0...15      16...31     32...47     48...63
+-----+-----+-----+-----+-----+-----+-----+-----+
|     Destination MAC (6 B = 48 bits)           |
+-----+-----+-----+-----+-----+-----+-----+-----+
|     Source MAC      (6 B = 48 bits)           |
+-----+-----+-----+-----+-----+-----+
|   EtherType   |
+-----+-----+

dst MAC = 11:11:11:11:11:11  (게이트웨이 R 의 LAN1 MAC)
src MAC = AA:AA:AA:AA:AA:AA
EtherType = 0x0800 (IPv4)
            = 0000_1000  0000_0000

16진 덤프 (14 B):
  11 11 11 11 11 11  AA AA AA AA AA AA  08 00
```

**포인터 상태 변화**:

```text
이전:  data = 88,          len = 135
이후:  data = 88 - 14 = 74,    len = 149
```

### STEP 4. NIC DMA 직전 — 프레임 전체 149 B 비트 덤프

이 상태의 skb 는 드라이버 TX 링에 들어간다. DMA descriptor 가 `skb->data` 의 물리 주소와 길이 149 를 NIC 에 알려준다. NIC 가 DRAM 에서 149 B 를 읽어 PHY 에 실어 보낸다. FCS 4 B 는 NIC 의 MAC 블록이 CRC-32 로 계산해서 프레임 뒤에 덧붙인다.

선로로 나가는 전체 153 B (149 프레임 + 4 FCS) 의 앞/뒤 덤프:

```text
===== Ethernet frame (149 B) =====

offset 0x00:  11 11 11 11 11 11 AA AA  AA AA AA AA 08 00 45 00  [L2 + IP 시작]
offset 0x10:  00 87 12 34 40 00 40 06  59 0B 80 02 C2 F2 D0 D8  [IP 나머지]
offset 0x20:  B5 0F C8 2D 00 50 12 34  56 78 00 00 00 00 50 18  [IP 끝 + TCP 시작]
offset 0x30:  FF FF 7A 1C 00 00 47 45  54 20 2F 68 6F 6D 65 2E  [TCP 끝 + payload]
offset 0x40:  68 74 6D 6C 20 48 54 54  50 2F 31 2E 30 0D 0A 48  [payload]
   ...
offset 0x90:  6E 65 74 0D 0A 0D 0A                              [payload 끝]

===== FCS (NIC 가 추가, 4 B) =====

offset 0x94:  XX XX XX XX   (CRC-32, NIC 하드웨어가 채움)
```

계층별로 읽는 법:

```text
[0x00 ~ 0x0D] = Ethernet header (14 B)
     11 11 11 11 11 11 | AA AA AA AA AA AA | 08 00
     dst MAC           | src MAC            | Eth type (IPv4)

[0x0E ~ 0x21] = IP header (20 B)
     45 = Ver4 IHL5
     00 = ToS
     00 87 = Total Length 135
     12 34 = Id
     40 00 = Flags|FragOff (DF=1)
     40 = TTL 64
     06 = Protocol TCP
     59 0B = Header checksum
     80 02 C2 F2 = src IP
     D0 D8 B5 0F = dst IP

[0x22 ~ 0x35] = TCP header (20 B)
     C8 2D = src port 51213
     00 50 = dst port 80
     12 34 56 78 = seq
     00 00 00 00 = ack
     50 = DOff 5
     18 = flags PSH|ACK
     FF FF = window
     7A 1C = checksum
     00 00 = urgent

[0x36 ~ 0x94] = payload (95 B)
     "GET /home.html HTTP/1.0\r\nHost: www.example.net\r\n\r\n"
```

**CRC-32 (Ethernet FCS) 비트 연산 요약**:

```text
다항식 G(x) = 0x104C11DB7
                = 1_0000_0100_1100_0001_0001_1101_1011_0111 (33 비트)

입력: 149 B 프레임 전체의 비트 스트림 M(x)
계산: FCS = M(x) * x^32 를 G(x) 로 나눈 나머지의 비트 반전

→ NIC 하드웨어가 선로에 실기 직전 32 비트 FCS 를 덧붙이고,
  수신 NIC 는 같은 방식으로 다시 계산해서 일치 여부를 본다.
```

### STEP 5. Hop 1 — 게이트웨이 라우터 R 이 하는 일 (비트 단위)

프레임이 R 의 LAN1 포트로 들어온다. R 이 하는 일을 **비트가 무엇으로 바뀌는가** 기준으로 순서대로 본다.

```text
(1) PHY 수신 + FCS 검증
    ─────────────────────────────
    NIC 하드웨어가 153 B 를 받고 마지막 32 비트 FCS 를 떼어
    앞 149 B 로 다시 CRC-32 계산 → 일치하면 통과, 불일치면 drop.
    (무결성 체크 ①: 프레임 전체의 Ethernet FCS, 링크 레벨)

(2) Ethernet 헤더 파싱
    ─────────────────────────────
    dst MAC = 11:11:11:11:11:11 ← R 자기 MAC 이므로 통과.
    EtherType = 0x0800 → IPv4 큐로 분배 (ip_rcv).
    Ethernet 헤더 14 B 는 여기서 "개념적으로 벗겨진다"
    (실제 커널은 skb_pull 로 data 포인터를 14 뒤로 민다).

(3) IP header checksum 검증
    ─────────────────────────────
    IP 헤더 20 B (offset 0x0E ~ 0x21) 의 16 비트 워드 10 개의 1 의 보수 합.
    현재 값 그대로 계산하면:
      0x4500 + 0x0087 + 0x1234 + 0x4000 + 0x4006 + 0x590B
    + 0x8002 + 0xC2F2 + 0xD0D8 + 0xB50F = 0x2_FFFF
      carry fold → 0xFFFF + 0x0002 = 0x1_0001 → 0x0002
      → 검증식이 0xFFFF (모든 비트가 1) 이 되어야 pass.
      계산 결과 실제로 0xFFFF 이면 통과.
    (무결성 체크 ②: IP 헤더만 대상, 라우터가 헤더를 고칠 때마다 재검증)

(4) 라우팅 테이블 longest-prefix-match
    ─────────────────────────────
    dst IP = 0xD0D8B50F = 208.216.181.15
    R 의 테이블 (예):
      208.216.0.0/16  → out=eth1, next-hop = 22:22:22:22:22:22
      default         → out=wan0, next-hop = ...
    /16 이면 상위 16 비트만 매칭:
      목적지 1101_0000 1101_1000  ← 이 16 비트가 "208.216"
      규칙   1101_0000 1101_1000 / 16 prefix
    → 매칭! out = eth1, 다음 홉 MAC = 22:22:22:22:22:22.

(5) TTL 감소
    ─────────────────────────────
    TTL 바이트 (offset 0x16):
       old:  0x40 = 0100_0000   (64)
       new:  0x3F = 0011_0000 → 이건 잘못이고
             0x40 - 1 = 0x3F = 0011_1111  (63)
    한 바이트 빼기 연산이 이뤄지고, 그 바이트가 즉시 skb 의 IP 헤더에 기록된다.
    TTL = 0 이 되면 drop + ICMP Time Exceeded 응답.

(6) IP header checksum 증분 재계산 (RFC 1624)
    ─────────────────────────────
    헤더 전체를 다시 합하지 않고, 변한 16 비트 워드만 반영:
       old word = TTL|Proto = 0x4006
       new word = TTL|Proto = 0x3F06
       HC' = ~(~HC + ~old + new)
           = ~(~0x590B + ~0x4006 + 0x3F06)
           = ~(0xA6F4 + 0xBFF9 + 0x3F06)
           = ~(0x1_A5F3) → carry fold → ~(0xA5F4)
           = 0x5A0B
    → 새 Header Checksum = 0x5A0B 를 offset 0x18~0x19 에 쓴다.

(7) 새 Ethernet 헤더로 재래핑
    ─────────────────────────────
    skb_push(14) 로 L2 헤더 자리를 다시 확보하고:
       dst MAC = 22:22:22:22:22:22  (다음 홉, R 의 neighbour 에서 조회)
       src MAC = R 의 eth1 MAC = 33:33:33:33:33:33  (R 자신으로 갱신)
       EtherType = 0x0800
    IP 와 TCP 와 payload 는 **한 비트도 바뀌지 않는다**.

(8) eth1 드라이버로 전달 → PHY → 선로
    ─────────────────────────────
    프레임 구조 그대로 149 B, 새 FCS 4 B 를 하드웨어가 채워서 송신.
```

hop 전후 비교표:

```text
          | Ethernet dst MAC | Ethernet src MAC | TTL  | IP checksum | TCP / payload
  ────────┼──────────────────┼──────────────────┼──────┼─────────────┼──────────────
  A → R   | 11:11:11:11:11:11| AA:AA:AA:AA:AA:AA| 0x40 | 0x590B      | 그대로
  R → B   | BB:BB:BB:BB:BB:BB| 33:33:33:33:33:33| 0x3F | 0x5A0B      | 그대로
```

TCP checksum 은 **hop 에서 건드리지 않는다** — end-to-end 로 A 가 계산해 채워 넣은 값이 B 까지 그대로 도달한다.

### STEP 6. Hop N — 최종 링크에서 B 의 NIC 수신

프레임이 B 의 NIC 포트에 도착한다.

```text
(1) PHY → MAC 필터
    dst MAC = BB:BB:BB:BB:BB:BB 가 B 자기 MAC 이면 받아들이고,
    아니면 버린다 (promiscuous 아닐 때).

(2) FCS 검증 (NIC 하드웨어)
    CRC-32 재계산 → 일치하면 통과.

(3) DMA write
    NIC 는 드라이버가 미리 준 RX ring 의 descriptor 를 보고
    DRAM 의 해당 물리 주소에 프레임 149 B 를 DMA 로 쓴다.
      RX ring[i].addr = 0x<phys>
      RX ring[i].len  = 2048  (버퍼 크기)
    쓰기가 끝나면 descriptor 의 status 비트가 1 로 올라간다.

(4) MSI-X 인터럽트
    PCIe 위에서 "memory write to APIC address" TLP 가 한 발 더 튄다.
    APIC 는 이것을 CPU 의 IRQ 벡터로 해석 → 드라이버 ISR 진입.

(5) NAPI poll
    ISR 은 짧게 끝내고 napi_schedule 만 걸어둔다.
    softirq 컨텍스트에서 드라이버의 poll() 이 RX ring 을 훑어
    완성된 프레임마다 skb 를 생성:
       skb = napi_alloc_skb(...);
       memcpy(skb->data, RX buf, 149);
       skb->len = 149;
       skb->protocol = eth_type_trans(skb, dev);  // 0x0800 → ETH_P_IP
```

`eth_type_trans()` 가 내부적으로 `skb_pull(skb, 14)` 를 수행해서, 리턴 시점에는 `skb->data` 가 이미 IP 헤더 첫 바이트를 가리킨다. 14 바이트의 Ethernet 헤더는 **버려진 게 아니라 skb->head 쪽으로 숨어 있을 뿐** — 그래서 `eth_hdr(skb)` 로 다시 접근할 수 있다.

### STEP 7. 수신 skb 역방향 언래핑 (skb_pull 연쇄)

커널 수신 경로는 L2 → L3 → L4 순서로 각 계층의 핸들러가 `skb_pull` 로 헤더를 한 겹씩 벗긴다.

```text
[a] ip_rcv(skb, dev, ...) 진입
    ─────────────────────────────
    iph = (struct iphdr *)skb->data;
    (1) IHL 비트 확인: (iph->ihl == 5) 이면 헤더 20 B.
    (2) 버전 비트 확인: ((iph->version) == 4).
    (3) IP header checksum 재검증 (무결성 체크 ②)
        ip_fast_csum(iph, iph->ihl) → 0xFFFF 이어야 통과.
    (4) dst IP 확인: iph->daddr == 0xD0D8B50F ?
        일치 → 로컬 처리, 불일치 → 포워딩 or drop.
    (5) TTL 검사: iph->ttl > 0 (포워딩 시 감소, 로컬 수신은 그대로 둠).
    (6) Protocol 디스패치:
        iph->protocol = 0x06 → tcp_v4_rcv() 로 넘어감.

    skb_pull(skb, 20)  → data 가 TCP 헤더 첫 바이트를 가리킴.

[b] tcp_v4_rcv(skb) 진입
    ─────────────────────────────
    th = (struct tcphdr *)skb->data;
    (1) TCP checksum 재검증 (무결성 체크 ③, 유일하게 end-to-end)
        pseudo-header + TCP 세그먼트 전체 합이 0xFFFF 이어야 통과.
    (2) 4-tuple 로 established socket 조회:
        key = (src IP=0x8002C2F2, src port=0xC82D,
               dst IP=0xD0D8B50F, dst port=0x0050)
        hash → inet_hashtables 에서 struct sock * 찾음.
    (3) TCP 상태 머신에 따라 처리:
         - ESTABLISHED 라면 → tcp_rcv_established()
         - LISTEN 이라면   → tcp_conn_request() (SYN 수신)

    skb_pull(skb, 20)  → data 가 payload 첫 바이트 'G' 를 가리킴.
```

이 시점 skb 의 메모리 상태:

```text
  skb->head ------>┐
                   │ ex-headroom + 벗겨진 헤더들 (Eth 14 + IP 20 + TCP 20 = 54 B)
  skb->data ------>┤  ← 지금 여기 (payload 'G' = 0x47)
                   │ payload 95 B
  skb->tail ------>┤
                   │ tailroom
  skb->end  ------>┘

  skb->len = 95     (헤더 3 겹이 전부 skb_pull 로 data 뒤로 숨은 상태)
```

### STEP 8. 소켓 큐 진입 — 유저 `read()` 가 받을 수 있게 되기까지

```text
(1) tcp_rcv_established(sk, skb) 안에서
    ─────────────────────────────
    - seq 번호 순서 검증 (재정렬 / 중복 제거)
    - TCP 옵션 처리 (SACK, timestamp 등)
    - receive window 관리
    → 정상 도착이면 payload 를 sk->sk_receive_queue 에 enqueue:
         __skb_queue_tail(&sk->sk_receive_queue, skb);

(2) 대기 중인 유저 task 깨우기
    ─────────────────────────────
    sk->sk_data_ready(sk);
      → sock_def_readable(sk);
        → wake_up_interruptible_sync_poll(&sk->sk_wq->wait, ...);
    유저가 read() / recv() / epoll_wait() 에서 자고 있었으면
    여기서 RUNNING 으로 전환된다.

(3) 유저 read(fd, buf, n) 재개
    ─────────────────────────────
    tcp_recvmsg():
      - sk_receive_queue 에서 skb 를 dequeue
      - skb_copy_datagram_msg() → copy_to_user 로 유저 버퍼에 바이트 복사
      - 복사한 만큼 skb 를 pop / 앞부분 소비
    복사가 끝나면 syscall 이 n_copied 를 반환.
    유저 프로세스가 그 즉시 buf[0]..buf[94] 에서
    'G','E','T',' ', ... 을 다시 보게 된다.
```

이렇게 유저가 `write` 했던 95 바이트의 비트 스트림이, 세 겹의 헤더를 붙였다가 다시 벗기는 과정을 통과해 **완전히 동일한 비트**로 상대 프로세스의 버퍼에 도착한다.

### STEP 9. 무결성 체크 총정리

```text
┌─────────────────────┬─────────────┬────────────────┬──────────────────────────┐
│ 레이어              │ 수단        │ 범위           │ hop 마다 어떻게 되는가    │
├─────────────────────┼─────────────┼────────────────┼──────────────────────────┤
│ Ethernet (L2)       │ FCS         │ 프레임 전체     │ 매 링크마다 NIC 하드웨어  │
│                     │ CRC-32 32b  │ (페이로드 포함) │ 가 새로 계산·재검증        │
├─────────────────────┼─────────────┼────────────────┼──────────────────────────┤
│ IP (L3)             │ Header      │ IP 헤더 20 B   │ 라우터가 TTL 을 바꾸면서   │
│                     │ Checksum    │ (payload 제외) │ 증분 재계산 (RFC 1624)     │
│                     │ 16b 1's comp│                │                          │
├─────────────────────┼─────────────┼────────────────┼──────────────────────────┤
│ TCP (L4)            │ Segment     │ pseudo-header  │ **한 번만** 계산 (A 에서), │
│                     │ Checksum    │ + TCP 전체     │ 중간 hop 은 건드리지 않음  │
│                     │ 16b 1's comp│ + payload      │ → end-to-end 무결성        │
├─────────────────────┼─────────────┼────────────────┼──────────────────────────┤
│ TCP 신뢰성           │ seq / ack    │ 바이트 스트림   │ 재전송 / 재정렬 / 중복 제거 │
│ (추가 레이어)         │ 번호         │ 전체            │ — checksum 과 별개          │
└─────────────────────┴─────────────┴────────────────┴──────────────────────────┘
```

핵심 비대칭:

- **Ethernet FCS** 는 hop-by-hop. 매 링크의 물리 계층에서 계산된다.
- **IP checksum** 은 hop-by-hop. 라우터가 TTL 을 1 줄일 때마다 새로 쓴다.
- **TCP checksum** 은 end-to-end. A 가 채우고 B 가 검증한다. 중간 홉이 손대면 안 된다.

이 차이는 TCP checksum 이 **pseudo-header (src IP / dst IP) 를 포함해서** 계산된다는 사실에서 나온다. 중간에 누가 TCP 헤더를 살짝 고쳐도, 양 끝단의 IP 쌍이 맞지 않으면 검증이 깨진다. NAT 처럼 IP 를 바꾸는 장치는 따라서 **TCP checksum 도 같이 고쳐야** 한다.

### 직접 검증 ① — hexdump 로 프레임을 실제로 들여다보기

```bash
# 터미널 1: 로컬에서 HTTP 를 한 번 쏜다
curl -v http://example.net/ >/dev/null

# 터미널 2: 같은 호스트에서 tcpdump 로 그 프레임을 비트 단위로 찍는다
sudo tcpdump -i any -c 1 -X -vv 'port 80 and host example.net'

# 출력에서 각 바이트가 위 덤프와 같은 위치에 있는지 비교:
#   Eth dst / src / type
#   IPv4 ver|ihl, ToS, total length, id, flags, TTL, proto, checksum,
#        src IP, dst IP
#   TCP src port, dst port, seq, ack, flags, window, checksum
#   payload "GET ... HTTP/1.1 ..."
```

### 직접 검증 ② — TTL 과 IP checksum 이 실제로 hop 마다 바뀌는지 확인

```bash
# 한 홉 뒤 TTL 이 줄어드는지:
traceroute -n -q 1 example.net

# 또는 tcpdump 로 연속된 홉을 봐야 할 때:
sudo tcpdump -i eth0 -vv -n -c 5 'host example.net'
#   "ttl 64" 였던 것이 반대 방향 패킷에서 "ttl 54" 처럼 다른 값으로 보임.

# ICMP echo 로 상대의 초기 TTL 을 추측:
ping -c 1 example.net
#   "ttl=54" 이면 상대가 64 로 시작해서 10 홉 떨어져 있다는 뜻.
```

### 직접 검증 ③ — ARP 테이블에서 다음 홉 MAC 을 직접 보기

```bash
ip neigh show
# 출력 예:
#   128.2.194.1 dev eth0 lladdr 11:11:11:11:11:11 REACHABLE
# 이 한 줄이 "다음 홉 IP 는 128.2.194.1, 그 MAC 은 11:11:... 로 캐시 중"
# 이라는 뜻. STEP 3 에서 Ethernet 헤더의 dst MAC 필드에 들어가는 값이다.

ip route
# default via 128.2.194.1 dev eth0
# 이 줄이 "어디로 가든 모르면 게이트웨이 128.2.194.1 에게 맡긴다" 라는 규칙.
```

### 직접 검증 ④ — checksum 재계산을 NIC 가 대신해 주고 있는지

```bash
ethtool -k eth0 | grep -E 'tx-checksumming|rx-checksumming'
#   tx-checksumming: on
#   rx-checksumming: on
# 대부분 현대 NIC 는 TCP/IP checksum 을 하드웨어 오프로드로 처리한다.
# 이 경우 tcpdump 로 로컬 인터페이스에서 프레임을 찍으면
# 송신 패킷의 checksum 필드가 0x0000 으로 보일 수 있는데,
# 이는 실제 선로로 나갈 때 NIC 가 그 자리에 올바른 값을 채워 넣기 때문이다.

# 끄고 싶다면:
sudo ethtool -K eth0 tx off rx off
# → 이제 tcpdump 에도 실제 계산된 checksum 이 찍힌다.
```

### 직접 검증 ⑤ — 수신 경로가 skb_pull 로 헤더를 벗기는 것을 bpftrace 로 확인

```bash
sudo bpftrace -e '
  kprobe:ip_rcv       { printf("ip_rcv     len=%d\n", ((struct sk_buff *)arg0)->len); }
  kprobe:tcp_v4_rcv   { printf("tcp_v4_rcv len=%d\n", ((struct sk_buff *)arg0)->len); }
  kprobe:tcp_recvmsg  { printf("tcp_recvmsg\n"); }
'

# 다른 터미널에서 nc 로 한 줄 받으면:
#   ip_rcv     len=135   ← IP 헤더는 아직 포함 (20 B 벗기기 전)
#   tcp_v4_rcv len=115   ← 20 B 줄어 있음 (ip_rcv 에서 skb_pull(20))
#   tcp_recvmsg          ← 이 시점에 payload 만 소켓 큐에 enqueue 끝
```

---

## A-3. I/O Bridge & DMA (L10)

### 원 질문

- I/O bridge 를 통해 비트가 이동한다는데, CPU / DRAM / NIC 사이에서 정확히 어떤 경로로 데이터가 흐르는가. (최우녕)
- CSAPP 가 말하는 "I/O bridge" 는 현대 하드웨어에서 정확히 어디 있는가? (최우녕)
- DMA 가 정확히 무엇인가? (최현진)
- CPU · DRAM · 주변장치가 얽히는 세 종류의 주소 공간은 어떻게 구분되는가? (최우녕)
- 리눅스 커널은 DMA · MMIO · IRQ 를 어떤 API · 자료구조로 다루는가? (최우녕)
- PCIe 위에서 비트는 어떤 패킷으로 움직이는가? (최우녕)
- write() 한 번이 실제로 PCIe TLP 까지 내려가는 경로를 추적하면? (최우녕)

### 설명

A-3 의 7 개 질문은 한 관찰로 묶인다: **CPU 는 NIC 에 "일해라" 라고 메타데이터만 알려주고, 실제 bulk data 이동은 NIC 가 DRAM 을 직접 DMA 로 읽는다.** 이게 DMA 의 본질이고, 이 협력이 일어나는 배선이 "I/O bridge" 다.

- **I/O bridge 의 현대 위치**: 과거에는 north/southbridge 라는 별도 칩이었지만 현대 x86 에서는 **CPU 다이 내부의 integrated memory controller (IMC) + PCIe root complex** 가 그 역할을 대체한다. 즉 "I/O bridge" 는 사실상 CPU 의 일부다.
- **DMA**: "CPU 를 거치지 않고 장치가 메모리에 직접 접근하는 기능". CPU 는 descriptor (어디서 몇 바이트) 만 준비하고, NIC 이 DRAM 을 읽어간다. CPU cycle 이 실제 bulk copy 에 소모되지 않는다.
- **세 주소 공간**: CPU 가상 주소 / 물리 주소 / DMA(bus) 주소. MMU 와 IOMMU 가 각 경계를 번역한다.
- **MMIO**: "메모리처럼 접근하는 장치 레지스터". CPU 가 NIC 에 명령을 내릴 때 `writel(value, nic_doorbell_addr)` 같은 식으로 쓴다. NIC 는 이 쓰기를 명령으로 해석한다.
- **IRQ / MSI / MSI-X**: NIC 이 CPU 에게 "일이 끝났다" 를 알리는 방식. 전통 IRQ 는 전용 선을 썼고 현대 PCIe 는 **memory write 를 인터럽트로 해석하는 MSI/MSI-X** 를 쓴다.
- **PCIe 위의 패킷 = TLP**: Memory Read Request, Memory Write Request, Completion 같은 종류가 있다. 물리적으로는 직렬 차동 쌍 위의 비트지만, 논리 단위는 TLP.
- **write() 한 번이 TLP 까지 내려가는 경로**: 아래 다이어그램.

### 하드웨어 경로 — 한 장으로

```text
                        ┌───────────────┐
                        │     CPU       │
                        │  core 0 .. N  │
                        └───────┬───────┘
                                │
        ┌───────── uncore / IMC ┼─ PCIe root complex ─────────┐
        │                       │                              │
        │  (1) CPU writes        (2) CPU doorbell write (MMIO)
        │      TX descriptor     │
        │      to DRAM           │
        │                       │                              │
        ▼                       ▼                              ▼
  ┌───────────┐          ┌────────────┐                ┌────────────┐
  │   DRAM    │◀──(3)────┤  NIC       │                │ NIC regs    │
  │           │  MemRd   │ TX engine  │◀──(2')─────────┤ (MMIO BAR)  │
  │ sk_buff   │  TLP     │            │                └────────────┘
  │ data      │          │            │
  │           │          │            │── (4) PHY ──► wire
  │           │──(5)────▶│            │    직렬화
  │ TX comp   │  MemWr   │            │
  │ ring      │  TLP     │            │
  └───────────┘          └─────┬──────┘
                               │
                               │ (6) MSI/MSI-X
                               ▼   (memory write 를
                        ┌───────────┐ interrupt 로 해석)
                        │  CPU      │
                        │  IRQ vec  │── interrupt handler
                        └───────────┘
```

순서:

1. **CPU → DRAM**: CPU 가 송신할 sk_buff 의 포인터/길이/flags 를 TX descriptor 로 써 둔다.
2. **CPU → NIC (MMIO)**: CPU 가 NIC 의 doorbell 레지스터에 "새 descriptor N 개 준비됨" 이라고 쓴다. 이건 MMIO — 물리 주소가 DRAM 이 아니라 NIC 의 BAR 영역에 매핑된 쓰기.
3. **NIC → DRAM (DMA MemRd TLP)**: NIC 이 descriptor 를 읽고, 그 안의 포인터를 따라 실제 데이터를 DRAM 에서 **DMA 로** 읽어온다. PCIe 위에서는 Memory Read Request TLP 가 흘러나가고, DRAM → NIC 방향으로 Completion TLP 들이 돌아온다.
4. **NIC → PHY → wire**: NIC 이 프레임에 FCS 를 붙이고, PHY 가 비트를 전기/광 신호로 직렬화해서 선로로 내보낸다.
5. **NIC → DRAM (DMA MemWr TLP)**: 송신 완료 상태를 TX completion ring 에 기록한다.
6. **NIC → CPU (MSI/MSI-X)**: NIC 이 미리 약속된 메모리 주소에 특정 값을 쓴다(MemWr TLP). CPU 의 APIC 가 이 쓰기를 interrupt 로 해석해서 IRQ 핸들러를 호출한다.

CPU 가 한 일은 "descriptor 작성 + doorbell 쓰기 + IRQ 응답" 뿐. **153 B 의 실제 바이트는 한 번도 CPU 를 거치지 않는다**. 이것이 DMA 의 핵심이다.

### 주소 공간 세 종류 — 실제로 어떻게 다른가

```text
A. CPU 가상 주소 공간       B. 물리 주소 공간          C. DMA (bus) 주소 공간
                                                        (PCIe 장치가 보는)
┌─ user ────────┐
│ buf 0x7fff... ├─┐         ┌──────────────────┐       ┌──────────────────┐
└───────────────┘ │ MMU     │ DRAM 0x1_2345_000│       │ NIC view          │
                  ├────────▶│                   │       │                   │
┌─ kernel ──────┐ │         │                   │       │                   │
│ skb 0xffff... ├─┘         │ NIC BAR 0xfee0..  │       │                   │
└───────────────┘           │                   │       │                   │
                            └──────────────────┘       │                   │
                                     │                  │                   │
                                     │ IOMMU (있으면)    │                   │
                                     └─────────────────▶│ bus addr 0x...    │
                                                        └──────────────────┘
```

| 주소 공간 | 누가 보는가 | 번역 주체 | 리눅스 커널 API |
|-----------|-----------|-----------|-----------------|
| CPU 가상 | 유저 프로세스, 커널 코드 | MMU (페이지 테이블) | `virt_to_phys`, `kmap` |
| 물리 | 하드웨어적 진실 | — | `phys_addr_t` |
| DMA (bus) | PCIe 장치 (NIC 등) | IOMMU 있으면 별도 매핑, 없으면 물리와 동일 | `dma_map_single`, `dma_alloc_coherent` |

IOMMU 가 있는 이유: 보안과 고립. IOMMU 없이는 NIC 이 악의적으로 DRAM 어디든 읽을 수 있다. IOMMU 는 NIC 이 오직 드라이버가 명시적으로 매핑한 영역만 접근할 수 있게 한다.

### PCIe TLP — 선로 위의 논리 단위

PCIe 는 직렬 차동 쌍(lane) 여러 개를 쓰는 물리 계층 위에, TLP (Transaction Layer Packet) 라는 논리 단위를 얹는다.

```text
TLP Header (12 or 16 Bytes)
┌───────────────────────────────────────────────────────────┐
│ Fmt (3 bit) │ Type (5 bit) │ TC │ ... │ Length (10 bit)     │
│ Requester ID (16)          │ Tag (8) │ Last/First BE (8)    │
│ Address (32 or 64)                                          │
└───────────────────────────────────────────────────────────┘
+ Data Payload (0 ~ MPS bytes, 보통 MPS = 128 ~ 4096 B)
+ Digest (ECRC, optional)
```

자주 보는 TLP 종류:

```text
Memory Read Request   (MRd)    <- NIC 이 DRAM 을 읽을 때
Memory Write Request  (MWr)    <- NIC 이 DRAM 에 쓸 때 (TX 완료, MSI)
Completion           (CplD)    <- Memory Read 에 대한 응답 (데이터 동반)
Config Read / Write   (CfgRd/Wr) <- 설정 공간 접근
```

`write()` 한 번이 TLP 로 내려가는 경로 요약:

```text
tcp_sendmsg
  → ip_output
    → dev_queue_xmit
      → driver->ndo_start_xmit
        → TX descriptor 작성 (CPU → DRAM)
        → MMIO doorbell 쓰기 (CPU → NIC BAR)
                    │
                    ▼ PCIe 위:
          NIC 가 MRd TLP 여러 개 발사 (descriptor + data)
          DRAM 이 CplD TLP 로 응답 (bulk data)
          NIC 가 PHY 로 비트 출력 → wire
          NIC 가 MWr TLP (TX completion)
          NIC 가 MWr TLP (MSI → interrupt)
```

이 경로의 각 단계에 리눅스 커널 API 가 있다.

- **TX descriptor 작성**: `struct xxx_tx_desc` 를 드라이버 고유 포맷으로 채움
- **sk_buff → DMA 매핑**: `dma_map_single(dev, skb->data, len, DMA_TO_DEVICE)`
- **MMIO 쓰기**: `writel(value, base + TX_DOORBELL_OFFSET)` — `ioremap` 된 BAR 영역
- **IRQ 처리**: `request_irq()` / `napi_schedule()` / NAPI poll loop
- **완료 후 unmap**: `dma_unmap_single(dev, addr, len, DMA_TO_DEVICE)`

더 깊은 바닥(PCIe configuration space, IOMMU 상세, DMA coherency, zero-copy) 은 [q10-io-bridge.md](./q10-io-bridge.md) 에서 다룬다.

### 직접 검증 — A-3

**검증 1. qdisc, TX ring, IRQ 의 실재**

```bash
# 현재 인터페이스에 달린 qdisc
tc qdisc show dev <iface>
# qdisc fq_codel 0: root refcnt 2 limit 10240p ...

# NIC 의 TX descriptor ring 크기
ethtool -g <iface>
# Current hardware settings:
# TX:     1024                  <- descriptor ring 엔트리 수

# NIC 통계 — TX 바이트 / 패킷 / 에러
ethtool -S <iface> | grep -Ei 'tx_(packets|bytes|errors|dropped)'

# NIC IRQ 가 어느 CPU 에 뜨는가
cat /proc/interrupts | awk 'NR==1 || /<iface>/'
```

`ethtool -g` 의 `TX: 1024` 가 곧 **"descriptor ring 엔트리 1024 개"**. 이 링이 다 차면 qdisc 에 backpressure 가 걸린다.

**검증 2. PCIe 장치 정보와 BAR**

```bash
# NIC 의 PCI 주소
lspci | grep -i 'eth\|ether'
# 02:00.0 Ethernet controller: Intel Corporation ...

# 상세 — BAR 주소, IRQ, capability
sudo lspci -vvv -s 02:00.0
# Region 0: Memory at 00000000fb400000 (64-bit, prefetchable) [size=128K]  <- MMIO BAR
# Interrupt: pin A routed to IRQ 19

# MSI-X capability
sudo lspci -vvv -s 02:00.0 | grep -A5 'MSI-X'
# Capabilities: [50] MSI-X: Enable+ Count=256 Masked-
```

`Region 0` 이 NIC 의 MMIO BAR — CPU 가 doorbell 을 쓰는 실제 물리 주소 범위다. `MSI-X: Count=256` 은 이 NIC 이 256 개의 서로 다른 인터럽트 벡터를 쓸 수 있다는 뜻 (큐 per-core 분산용).

**검증 3. DMA 매핑 확인**

```bash
# 현재 DMA 매핑 현황
sudo cat /sys/kernel/debug/dma_api/map_errors   # 매핑 실패 카운터
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null
ls /sys/kernel/debug/iommu/                     # IOMMU 존재 시

# 드라이버가 쓰는 dma mask
cat /sys/bus/pci/devices/0000:02:00.0/dma_mask_bits
# 64        <- 64-bit DMA 가능 (32-bit only 장치는 bounce buffer 씀)
```

**검증 4. MSI-X 인터럽트 분포**

```bash
# MSI-X 다중 벡터가 여러 CPU 에 분산되어 뜨는지
cat /proc/interrupts | grep -E "<iface>" | head
# 128: 123 456 ... eth0-TxRx-0
# 129: 234 567 ... eth0-TxRx-1
#      ^CPU0 카운트
#          ^CPU1 카운트

# IRQ affinity (각 벡터가 고정된 CPU 마스크)
cat /proc/irq/128/smp_affinity_list
```

RSS / RPS 가 잘 설정된 시스템은 TX/RX 큐 별 MSI-X 벡터가 서로 다른 CPU 에 박혀서 NIC 인터럽트가 병렬로 처리된다.

---

## A-4. Echo Server, Datagram, EOF — wire-level (L15)

### 원 질문

- "데이터그램(datagram)" 은 무엇인가. 패킷, 프레임, 세그먼트와 어떻게 다른가. (최우녕)
- 책에서 말하는 에코 서버(echo server) 는 무엇이고, 소켓으로 어떻게 구현되는가. (최우녕)
- 네트워크 통신은 파일 입출력과 비슷하고, 그래서 EOF 가 중요하다는데, 그 과정을 자세히 설명해 달라. (최우녕)

### 설명

세 질문은 **"네트워크 한 덩어리 = 파일 한 덩어리"** 라는 UNIX 추상화에서 하나의 그림이 된다.

- **한 덩어리의 이름이 층마다 다르다**. application 메시지 → TCP segment / UDP datagram → IP packet → Ethernet frame. 같은 바이트지만 껍질을 씌우면서 이름이 바뀐다. 그래서 "datagram" 은 UDP 층의 단위 이름이고, "segment" 는 TCP 층의 이름, "packet" 은 IP 층의 이름, "frame" 은 link 층의 이름이다.
- **Echo server 는 가장 얇은 TCP 서버**: `socket → bind → listen → accept → read → write → close` 6 단계로 끝난다. 클라가 보낸 그대로 돌려주는 것뿐이지만, **송신 파이프라인 + 수신 파이프라인이 대칭** 이라는 걸 보여주기에 교과서적 예제다.
- **EOF 는 close/FIN 이 파일 EOF 와 같은 인터페이스가 되는 것**. 상대가 `close()` (또는 `shutdown(WR)`) 하면 FIN 이 선로에 실려 오고, 내 쪽 `read` 가 **0** 을 반환한다. 일반 파일에서 파일 끝에 도달했을 때 `read` 가 0 을 반환하는 것과 같은 규칙이다. 그래서 소켓을 파일처럼 다룰 수 있다.
- UDP 는 connection 이 없어서 EOF 개념이 없다. 한 번에 하나의 datagram, 끝.

### 층별 단위 이름 — 한 바이트가 4 번 이름을 바꾼다

```text
층                    단위 이름        붙는 헤더        오늘 예시
============================================================================
application          message         HTTP line        payload 95 B
transport (TCP)      segment          TCP 20 B        segment 115 B
transport (UDP)      datagram         UDP 8 B         datagram (예시 없음)
network (IP)         packet           IP 20 B         packet 135 B
link (Ethernet)      frame            Eth 14 B + FCS  frame 149 B + FCS 4 B = wire 153 B
physical             bits/symbols     -                  153 B × 8 = 1224 bits
```

요점:

- **segment (TCP) vs datagram (UDP)**: 둘 다 transport 층이지만 이름이 다르다. TCP 는 바이트 stream 이라 경계가 없고, UDP 는 **메시지 경계가 살아 있어서 "하나의 datagram = 하나의 메시지"**.
- **packet**: 일반적으로 IP packet 을 뜻한다. "packet" 이라는 단어는 느슨하게 쓰이지만, 층을 특정할 때는 IP 층이 기본.
- **frame**: link 층의 단위. Ethernet frame, Wi-Fi frame 등 물리 매체에 따라 종류가 다르다.
- 선로 위에서 한 덩어리로 움직이는 건 frame. 라우터를 지나면 frame 은 벗겨지고, packet 은 유지되며, 새 frame 이 입혀진다 (A-1 의 라우터 전/후 비교와 동일).

### TCP stream vs UDP datagram — 경계가 있느냐 없느냐

```text
TCP (SOCK_STREAM)
  클라가 send(4B) 두 번 호출                서버가 recv 하면
  +---+---+---+---+---+---+---+---+      +---+---+---+---+---+---+---+---+
  | A | A | A | A | B | B | B | B |  →  | A | A | A | A | B | B | B | B |
  +---+---+---+---+---+---+---+---+      +---+---+---+---+---+---+---+---+
  (두 번 send 했어도)                     (한 번에 8 B 읽을 수 있다)

UDP (SOCK_DGRAM)
  클라가 sendto(4B) 두 번 호출             서버가 recvfrom 하면
  +---+---+---+---+   +---+---+---+---+   +---+---+---+---+
  | A | A | A | A |   | B | B | B | B | → | A | A | A | A |   (한 번의 recvfrom)
  +---+---+---+---+   +---+---+---+---+   +---+---+---+---+
                                          +---+---+---+---+
                                          | B | B | B | B |   (다음 recvfrom)
                                          +---+---+---+---+
  (두 datagram 은 절대 합쳐지지 않고
   각각 한 번의 recvfrom 호출에 매칭)
```

"stream" 은 수도관처럼 연속 흐름이라 원저자가 쓴 경계가 소비자에 전달되지 않는다. "datagram" 은 편지 한 장 단위라 경계가 유지된다.

### TCP flag 비트 — SYN / FIN / RST / ACK

TCP 헤더의 flag 필드(9 bit) 중 자주 쓰는 6 개는 아래 비트에 있다.

```text
TCP header offset 13 번째 바이트 (data offset 바이트 아래쪽)
   bit:    8   7   6   5   4   3   2   1   0
           CWR ECE URG ACK PSH RST SYN FIN
                       ^^^ ^^^ ^^^ ^^^ ^^^
                       자주 보는 5 개

SYN  = 0000 0010   (bit 1)   "연결 시작. 내 초기 seq 는 이 값"
SYN+ACK = 0001 0010          "연결 수락. 내 초기 seq 는 이 값, 네 SYN 확인"
ACK  = 0001 0000   (bit 4)   "여기까지 받았다"
PSH  = 0000 1000   (bit 3)   "버퍼링 말고 앱에 바로 올려"
RST  = 0000 0100   (bit 2)   "연결 즉시 파괴"
FIN  = 0000 0001   (bit 0)   "내 방향은 더 보낼 게 없다"

tcpdump 약어
  [S]    = SYN
  [S.]   = SYN | ACK       (. 은 ACK)
  [.]    = ACK only
  [P.]   = PSH | ACK
  [F.]   = FIN | ACK
  [R]    = RST
```

### close / FIN / EOF — 네 방향으로 읽으면

TCP 는 양방향 연결이지만 **종료는 각 방향이 독립적으로** 일어난다.

```text
  Client                                   Server
  |                                        |
  |--- data (PSH|ACK) -------------------->|
  |<-- echo (PSH|ACK) ---------------------|
  |                                        |
  |--- close() 호출                         |
  |                                        |
  |--- FIN (F.) --------------------------▶|
  |                                        | (server 의 read 가 0 반환 == EOF)
  |<-- ACK ( . ) -------------------------|
  |                                        |
  |                                        |--- close() 호출 (또는 shutdown(WR))
  |<-- FIN (F.) --------------------------|
  | (client 의 read 가 0 반환 == EOF)       |
  |--- ACK ( . ) -------------------------▶|
  |                                        |
  |  [양쪽 다 TIME_WAIT / CLOSED]           |
```

요점:

- **FIN 이 선로에 실려 오면 반대편 `read` 가 0 을 반환**. 이게 EOF 의 구체적 메커니즘이다.
- 파일 I/O 에서 파일 끝에 도달했을 때 `read` 가 0 을 반환하는 것과 **정확히 같은 규칙**이다. 그래서 응용 코드는 소켓과 파일을 구분하지 않고 동일한 패턴(`while (n = read(fd, buf, sz)) { ... }`) 으로 다룰 수 있다.
- **RST 는 EOF 가 아니다**. RST 를 받으면 `read` 가 `-1 + errno=ECONNRESET` 을 반환한다. 정상 종료(FIN → 0) 와 비정상 종료(RST → 에러) 를 구분할 수 있다.
- UDP 는 연결이 없고 FIN 도 없으므로 EOF 개념이 없다. 한 번에 한 datagram, 끝.

### Echo server 최소 골격

```c
// 서버
int lfd = socket(AF_INET, SOCK_STREAM, 0);
bind(lfd, ...);
listen(lfd, 10);
for (;;) {
    int cfd = accept(lfd, ...);
    char buf[1024];
    ssize_t n;
    while ((n = read(cfd, buf, sizeof buf)) > 0) {
        write(cfd, buf, n);                 // echo back
    }
    // n == 0  -> EOF (peer 가 close 해서 FIN 받음)
    // n == -1 -> error
    close(cfd);
}
```

왜 이 서버가 송수신 파이프라인의 대칭성을 보여주는가:

- `read` 한 번은 NIC → driver → sk_read_queue → sock_recvmsg → copy_to_user 의 수신 경로를 거친다.
- 곧바로 `write(cfd, buf, n)` 은 copy_from_user → tcp_sendmsg → sk_write_queue → driver → NIC 의 송신 경로를 거친다.
- 수신과 송신이 **같은 객체 체인(struct sock)** 위에서 **반대 방향**으로 흐른다 — 이게 "대칭" 이다.

### 직접 검증 — A-4

**검증 1. write return 시점 vs 실제 완료 시점**

```bash
# echo 대상: nc -l 8080 이 loopback 에 붙어 있다고 가정
python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1', 8080))
t0 = time.time()
n  = s.send(b'x' * (4*1024*1024))
t1 = time.time()
print(f'write returned in {t1-t0:.4f}s, returned bytes = {n}')
"

# 옆 터미널에서 동시에
watch -n 0.1 "ss -tanpi '( dport = :8080 )' | grep -E 'Send-Q|bytes_acked|retrans'"
```

`write returned in 0.0001s` 와 `Send-Q: 3932160` 이 동시에 뜨면 **"return ≠ wire 송신 완료"** 의 살아 있는 증거.

**검증 2. FIN 과 EOF 를 한 화면에**

```bash
# 터미널 1
nc -l 8080

# 터미널 2 — tcpdump
sudo tcpdump -i lo -nn 'tcp port 8080' -c 20

# 터미널 3 — 클라
nc 127.0.0.1 8080
# 메시지 입력 후 Ctrl-D (EOF 전송)

# tcpdump 출력
# Flags [S]    seq N            <- SYN
# Flags [S.]  seq M, ack N+1   <- SYN+ACK
# Flags [.]   ack M+1           <- ACK
# ...
# Flags [F.]                    <- client 가 Ctrl-D -> close() -> FIN
# Flags [.]                     <- server ACK
# Flags [F.]                    <- server close() -> FIN
# Flags [.]                     <- client ACK
```

`[F.]` 가 **FIN 의 선로 위 모습**, 이 플래그를 받은 쪽에서 `read` 가 0 을 반환한다.

**검증 3. UDP datagram 경계**

```bash
# 서버
python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 9090))
while True:
    data, addr = s.recvfrom(4096)
    print('recv', len(data), 'from', addr)
"

# 클라
python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b'AAAA', ('127.0.0.1', 9090))
s.sendto(b'BBBB', ('127.0.0.1', 9090))
"
# 서버 출력:
# recv 4 from ('127.0.0.1', ...)
# recv 4 from ('127.0.0.1', ...)
```

TCP 는 4 + 4 = 8 이 한 번에 `read` 될 수 있지만, UDP 는 항상 4, 4 로 분리된다. **stream vs datagram** 의 실물 증거다.

**검증 4. RST vs FIN 구분**

```bash
# 서버
nc -l 8080 &
SERVER_PID=$!

# 클라
nc 127.0.0.1 8080 &

# 서버를 강제 종료 (FIN 없이)
kill -9 $SERVER_PID

# tcpdump 에는
# Flags [R]                      <- RST
# 클라 read() 는 errno=ECONNRESET 을 반환
```

정상 종료는 FIN, 강제 종료는 RST. 응용 레벨에서 EOF 와 에러를 구분하는 근거다.

---

## 전체 검증 명령 모음

```bash
# 3 층 주소 (A-1)
ip -o -4 addr ; ip -o link ; ip route show default ; ip neigh

# fd → socket (A-2)
ls -l /proc/$$/fd ; ss -tanpie

# 버퍼 / MSS / cwnd / send-Q (A-2)
ss -tmi ; ss -tanp ; ss -tin

# wire 덤프 + ARP + TTL (A-1)
sudo tcpdump -i any -e -X -vv -c 4 'tcp port 8080'
sudo tcpdump -i any -nn arp -c 4
traceroute -n <host>

# 하드웨어 (A-3)
tc qdisc show dev <iface> ; ethtool -g <iface>
ethtool -S <iface> | grep tx_
cat /proc/interrupts | grep <iface>
sudo lspci -vvv -s <pci-id>

# FIN / UDP / RST (A-4)
sudo tcpdump -i lo -nn 'tcp port 8080' -c 20
```

## 연결 문서 (같은 저자 내 심화 자료)

- [q01-network-hardware.md](./q01-network-hardware.md)
- [q05-socket-principle.md](./q05-socket-principle.md)
- [q08-host-network-pipeline.md](./q08-host-network-pipeline.md)
- [q10-io-bridge.md](./q10-io-bridge.md)
- [q14-echo-server-datagram-eof.md](./q14-echo-server-datagram-eof.md)
