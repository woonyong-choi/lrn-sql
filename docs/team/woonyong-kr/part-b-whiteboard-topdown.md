# Part B. 프로토콜 스택 — 주소 · 연결 · Handshake

`docs/question/01-team-question-parts.md` 의 Part B (B-1 ~ B-3) 질문 묶음에 대한 답을 한 문서에 정리한다.
각 섹션은 원 질문 목록을 맨 위에 두고, 그 질문들을 이어서 답하는 설명을 본문으로 제시한다.

## 커버하는 질문 매핑

| 질문 ID | 주제 | 관련 L 노드 |
|--------|------|------------|
| B-1 | Endianness & 문자열 ↔ 바이너리 변환 | L3-2, L3-3 |
| B-2 | DNS 심화 — 도메인 · resolve · Cloudflare | L4-2 ~ L4-6 |
| B-3 | TCP 순서 · 스트림 · 3-way handshake 심화 | L8-4 ~ L8-8 |

## Part B 를 관통하는 한 문장

클라이언트가 `connect("www.example.net", 80)` 을 호출하면, 바이너리 포트 번호가 네트워크 바이트 순서로 뒤집히고, DNS 로 도메인이 IP 로 풀리고, 세 번의 패킷 교환(SYN → SYN+ACK → ACK)을 거쳐 양쪽 커널에 동일한 4-tuple 로 ESTABLISHED 상태의 TCP 제어 블록이 세워진 뒤, 그제서야 스트림 바이트를 주고받을 수 있게 된다.

## 예시 상황 세팅

Part A 와 동일한 주소/포트 값을 공통 사례로 쓴다.

```text
클라이언트 호스트 A
  IP  = 128.2.194.242
  MAC = AA:AA:AA:AA:AA:AA
  src port (ephemeral) = 51213

서버 호스트 B
  IP  = 208.216.181.15
  MAC = BB:BB:BB:BB:BB:BB
  dst port = 80

도메인 이름
  www.example.net

로컬 리졸버 (예: Cloudflare 1.1.1.1)
  IP = 1.1.1.1
```

---

## B-1. Endianness & 문자열 ↔ 바이너리 변환 (L3-2, L3-3)

### 원 질문

- htons, ntohs, htonl, ntohl 은 왜 써야 하는가. 안 쓰면 무슨 일이 벌어지는가? (최우녕)
- big endian 과 little endian 은 무엇이고, IP 주소와 무슨 관련이 있는가? (최현진)
- htons(80) 결과가 왜 0x5000 처럼 보이는데 실제 네트워크에서는 80 으로 해석되는가? (최현진)
- inet_pton 과 inet_ntop 은 정확히 무엇을 바꾸는 함수인가? (최현진)

### 설명

네 질문은 모두 **"같은 비트 패턴을 바이트 경계에서 어느 쪽부터 읽느냐"** 라는 하나의 사실에서 나온다.

CPU 는 메모리 안에서 멀티바이트 정수를 **어떤 바이트부터 저장하느냐** 가 아키텍처마다 다르다. x86 / ARM(기본) / RISC-V 는 리틀 엔디안 — 낮은 주소에 LSB(least significant byte) 를 둔다. SPARC / 예전 PowerPC / 네트워크 프로토콜은 빅 엔디안 — 낮은 주소에 MSB 를 둔다. 네트워크 선로 위의 약속은 **"빅 엔디안으로 쓴다"** 이다. 이것을 "network byte order" 라고 부른다.

- 포트 번호 80 을 C 로 `uint16_t p = 80;` 하면, x86 머신 메모리 상에는 `50 00` (LSB 가 낮은 주소) 로 저장된다.
- 하지만 선로 위로 나갈 때는 `00 50` (MSB 가 먼저) 여야 한다.
- 그래서 `htons(80)` (host-to-network short) 가 **바이트 두 개를 뒤집어** `0x0050` 을 주는 것이다.
- x86 에서 `htons(80)` 의 결과 값을 그대로 `printf("%04x", ...)` 로 찍으면 `0x5000` 처럼 보이는데, 이건 여전히 x86 메모리 규칙으로 읽어서 그렇다. 실제로 그 바이트들을 뽑아서 선로에 실어 보내면 `00 50` 이 나가고 상대가 해석하면 80 이다.
- `htonl`, `ntohs`, `ntohl` 은 같은 개념의 32 비트 · 수신 방향 버전이다.
- IP 주소는 32 비트 정수이므로 같은 문제를 겪는다. `in_addr.s_addr` 필드에는 **항상 네트워크 바이트 순서** 가 들어간다.
- `inet_pton("128.2.194.242", ...)` 은 문자열을 네트워크 바이트 순서의 32 비트 정수로 만들어 준다. `inet_ntop` 은 그 역이다.
- 이 함수를 안 쓰면 무엇이 터지나: 같은 머신끼리만 통신할 때는 안 터질 수도 있다. 하지만 다른 엔디안 머신이나, 동일 엔디안이어도 패킷을 선로에서 관찰하면 번호가 뒤집혀 있어 디버깅 시 혼란을 준다. 더 중요한 건 라우터 · 방화벽 · 상대 OS 가 모두 네트워크 바이트 순서로 해석하므로, 호스트 엔디안 그대로 쓰면 상대가 다른 포트 / 다른 IP 로 인식해 연결이 성립하지 않는다.

### 엔디안을 눈으로 보기

```text
uint16_t p = 80;   // 10 진수 80 = 16 진수 0x0050 = 2 진수 0000_0000 0101_0000

x86 (리틀 엔디안) 메모리 배치:
  주소       +0    +1
  값         50    00
  비트       0101_0000  0000_0000

빅 엔디안 머신 메모리 배치:
  주소       +0    +1
  값         00    50
  비트       0000_0000  0101_0000

htons(80) 이 하는 일: "호스트 메모리 표현" 을 "네트워크 표현" 으로 강제로 뒤집어 준다.
  x86 의 htons(80) 반환값을 x86 메모리에 저장하면:
    주소  +0    +1
    값    00    50      ← 이미 네트워크 순서대로 들어가 있다
    비트  0000_0000  0101_0000

  이 값을 printf("%04x", htons(80)) 로 찍으면,
  x86 이 두 바이트를 뒤집어 읽어서 0x5000 으로 보인다.
  하지만 send() 로 선로에 실리면 메모리 순서 그대로 00 50 이 나간다.
```

IP 주소 128.2.194.242 도 마찬가지다.

```text
128.2.194.242 의 네트워크 표현 (빅 엔디안):
  주소  +0    +1    +2    +3
  값    80    02    C2    F2
  비트  1000_0000  0000_0010  1100_0010  1111_0010

x86 메모리에 저장된 s_addr (inet_pton 이 채운 뒤):
  주소  +0    +1    +2    +3
  값    80    02    C2    F2     ← 네트워크 순서와 동일 (이 함수의 약속)

이 s_addr 을 x86 이 uint32_t 로 읽으면 0xF2C20280 으로 보인다.
"사람 눈으로 읽는 호스트 정수" 와 "네트워크 선로의 바이트 순서" 는 다르다.
inet_ntop 은 이 바이트들을 다시 문자열 "128.2.194.242" 로 되돌려 준다.
```

### 직접 검증 ① — 같은 값이 엔디안에 따라 어떻게 다르게 찍히는지

```c
#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>

int main(void) {
    uint16_t host   = 80;
    uint16_t net    = htons(80);
    printf("host order  : %04x  (x86 메모리에서는 50 00)\n", host);
    printf("network ord : %04x  (x86 메모리에서는 00 50)\n", net);

    struct in_addr a;
    inet_pton(AF_INET, "128.2.194.242", &a);
    unsigned char *p = (unsigned char *)&a.s_addr;
    printf("s_addr bytes: %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
    // x86 에서도 80 02 C2 F2 로 찍힌다. inet_pton 이 네트워크 순서로 채우기 때문.
}
```

### 직접 검증 ② — 선로의 바이트가 실제로 네트워크 순서인지

```bash
sudo tcpdump -i any -c 1 -X 'port 80 and host example.net'
# 출력 중 IP 헤더 시작부를 보면:
#   0x0000:  4500 0087 1234 4000 4006 590b 8002 c2f2
#                                         ^^^^ ^^^^
#                                         src IP 네트워크 순서
#   0x0010:  d0d8 b50f c82d 0050 ...
#                     ^^^^ ^^^^
#                     src port 51213 = 0xC82D, dst port 80 = 0x0050
# 바이트 순서가 빅 엔디안 그대로 찍혀 있음을 확인할 수 있다.
```

---

## B-2. DNS 심화 — 도메인 · resolve · Cloudflare (L4-2 ~ L4-6)

### 원 질문

- 도메인의 계층적 트리 구조는 그냥 .kr, .com, amazon, tistory 같은 것을 누가 관리하는지 나타내는 것인가? (최현진)
- 도메인 이름은 어떻게 등록(소유)되고, 어떻게 IP 로 해석(resolve)되는가? (최우녕)
- 데이터를 보낼 때 항상 DNS 서버로 먼저 가서 실제 주소가 있는지 확인한 뒤 보내는가? (최현진)
- 목적지 IP 를 아예 모르는 상태에서는 어떻게 통신을 시작하는가? (최현진)
- DNS 의 1 대 1, 다 대 1, 일 대 다 매핑은 실제로 어떤 상황에 쓰이는가? (최현진)
- DNS 서버의 주소는 어떻게 아는가? (최현진)
- Cloudflare 에서 도메인을 구매하고 DNS 를 등록하면 접속이 가능해지는데, 그 과정을 단계별로 설명해 달라. (최우녕)
- Cloudflare 의 proxied mode 는 왜 실제 origin IP 를 숨긴다고 하는가? (최현진)

### 설명

여덟 질문은 **"도메인은 이름, IP 는 주소, 그 둘 사이의 매핑을 관리하는 분산 데이터베이스가 DNS"** 라는 하나의 구조에서 나온다.

도메인 계층은 권한 위임(delegation) 의 트리다. 뿌리(root) 부터 `.`, `.net`, `.example.net`, `www.example.net` 으로 내려가면서 **각 단계의 관리자가 다음 단계의 권한 NS 를 알려주는** 방식이다.

- **누가 관리하나**: root 서버 13 세트(a.root-servers.net ~ m.root-servers.net) 는 ICANN 이 배정한 권한 기관들이 운영한다. `.com`, `.net` 같은 TLD 는 Verisign 이, `.kr` 은 KISA 가, `.example.net` 은 그 도메인을 구매한 사람(=등록자) 이 지정한 권위 서버가 관리한다.
- **어떻게 등록하나**: 등록대행자(registrar, 예: Cloudflare Registrar, Namecheap) 가 레지스트리(registry, 예: Verisign for .net) 에 위임 관계를 기록한다. 구매자는 "이 도메인의 권위 NS 는 ns1.cloudflare.com 이다" 를 등록한다. 이 한 줄이 `.net` 레지스트리에 들어가는 순간부터 `.net` 의 권위 서버가 "example.net 은 ns1.cloudflare.com 에게 물어봐" 라고 답할 수 있게 된다.
- **어떻게 resolve 하나**: 클라이언트는 직접 root 까지 내려가지 않고, 자기가 미리 아는 **로컬 리졸버(recursive resolver)** 한 곳에 물어본다. 리졸버가 root → TLD → 권위 서버 순으로 대신 따라 내려가서(=recursion) 최종 A 레코드를 받아온 뒤 결과를 캐시하고 돌려준다.
- **데이터를 보낼 때 항상 DNS 를 먼저 가나**: 이름으로 접속할 때만 그렇다. 리졸버와 OS 가 캐시를 갖고 있어 반복 호출에서는 네트워크를 타지 않는다. 이미 IP 를 알고 있는 상태면 DNS 를 거치지 않는다. 그래서 IP 로 직접 `curl http://208.216.181.15/` 하면 DNS 통신이 없다.
- **IP 를 모를 때 어떻게 시작하나**: "DNS 서버 IP 는 안다" 라는 전제를 이용한다. 그 DNS 서버 IP 자체는 DHCP (가정용 라우터가 `/etc/resolv.conf` 에 꽂아 주는 값) 나 수동 설정(`1.1.1.1`) 으로 받는다. 이 한 지점만 IP 로 직접 가면, 나머지 모든 도메인은 이 서버에 물어서 풀 수 있다.
- **1 대 1 / 다 대 1 / 일 대 다 매핑**: 1 대 1 은 단순 웹 사이트, 다 대 1 은 하나의 IP 에 여러 도메인 (`example.net` · `cdn.example.net` 가 같은 서버를 가리킴), 일 대 다 는 CDN 이나 로드 밸런싱 (`www.example.net` 이 지역별로 다른 IP 를 내줌).
- **DNS 서버의 주소는 어떻게 아나**: 처음은 DHCP 또는 수동. 그 이후로는 리졸버에 대해서는 캐시가 답한다.
- **Cloudflare 로 도메인 붙이기 단계**:
  1. registrar 에서 도메인 등록 → 레지스트리에 NS 를 Cloudflare 로 위임.
  2. Cloudflare 의 권위 DNS 에 "`www.example.net A 208.216.181.15`" 레코드 추가.
  3. Cloudflare 가 존(zone) 을 권위로 서비스하기 시작.
  4. 사용자가 `www.example.net` 조회 → root → `.net` → `ns1.cloudflare.com` → 리졸버가 `208.216.181.15` 를 받아감.
- **Proxied mode (오렌지 구름)**: Cloudflare 의 권위 DNS 가 A 레코드를 실제 origin IP 대신 **Cloudflare 엣지 IP** 로 내려준다. 클라이언트는 엣지와 TLS 를 맺고, 엣지가 origin 에 프록시로 요청한다. 권위 응답 어디에도 origin IP 가 실리지 않으므로 공격자가 평범한 `dig` 로 origin 을 알 수 없다.

### DNS 메시지 비트 레이아웃

DNS 는 UDP/53 (대부분) 또는 TCP/53 (응답이 512 B 초과 시) 위에서 동작한다. 한 메시지의 헤더는 12 바이트 고정이다.

```text
DNS message header (12 B)

 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              ID               | flags                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           QDCOUNT             |            ANCOUNT            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           NSCOUNT             |            ARCOUNT            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

flags (16 비트):
  QR(1)  OPCODE(4)  AA(1)  TC(1)  RD(1)  RA(1)  Z(3)  RCODE(4)
```

### DNS 쿼리 패킷 예시 (www.example.net, A 레코드)

UDP payload 의 실제 비트:

```text
===== DNS query (32 B) =====

offset 0x00:  12 34 01 00 00 01 00 00  00 00 00 00
              ID    flags Qs=1  As=0   NSs=0 ARs=0

  ID        = 0x1234     (트랜잭션 구분)
  flags     = 0x0100
              0000_0001 0000_0000
              └ QR=0 (query)
                 OPCODE=0 (standard query)
                 AA=0, TC=0, RD=1 (recursion desired)
                 RA=0, Z=000, RCODE=0000
  QDCOUNT   = 0x0001  (질문 1 개)
  ANCOUNT   = 0x0000
  NSCOUNT   = 0x0000
  ARCOUNT   = 0x0000

offset 0x0C:  03 77 77 77 07 65 78 61  6D 70 6C 65 03 6E 65 74 00
              len=3 'w' 'w' 'w' len=7 'e' 'x' 'a' 'm' 'p' 'l' 'e' len=3 'n' 'e' 't' 0x00

  QNAME 은 길이-접두 라벨 배열:
    0x03 "www"
    0x07 "example"
    0x03 "net"
    0x00  (루트 라벨, 이름 끝)

offset 0x1D:  00 01 00 01
              QTYPE=A(1)  QCLASS=IN(1)
```

### DNS 응답 패킷 (A 레코드 하나) 비트 덤프

```text
===== DNS response (~ 48 B) =====

offset 0x00:  12 34 81 80 00 01 00 01  00 00 00 00
              ID    flags Qs=1  As=1

  flags = 0x8180
          1000_0001 1000_0000
          └ QR=1 (response)
             OPCODE=0
             AA=0 (non-authoritative, 리졸버가 대신 답)
             TC=0, RD=1, RA=1 (recursion available)
             RCODE=0000 (NOERROR)

offset 0x0C:  03 77 77 77 07 65 78 61  6D 70 6C 65 03 6E 65 74 00
              (쿼리 섹션 그대로 반복)

offset 0x1D:  00 01 00 01

===== Answer 섹션 =====
offset 0x21:  C0 0C                  ← 이름 압축 포인터: "offset 0x0C 에 있는 이름 참조"
              1100_0000 0000_1100    ← 상위 2 비트 11 이 포인터, 하위 14 비트가 오프셋

offset 0x23:  00 01              ← TYPE=A
offset 0x25:  00 01              ← CLASS=IN
offset 0x27:  00 00 01 2C        ← TTL = 300 초
offset 0x2B:  00 04              ← RDLENGTH = 4 B
offset 0x2D:  D0 D8 B5 0F        ← RDATA = 208.216.181.15
                                    1101_0000 1101_1000 1011_0101 0000_1111
```

DNS 응답의 핵심 비트는 마지막 `D0 D8 B5 0F` 네 바이트다. 이 네 바이트가 바로 클라이언트가 `connect()` 에 넣을 destination IP 가 된다. **도메인에서 비트 4 개로 수렴하는 과정** 이 DNS 의 전부다.

### 직접 검증 ① — 권위 위임 사슬을 실제로 따라가기

```bash
dig +trace www.example.net
# root 서버부터 순서대로 .net 의 NS, example.net 의 NS, 그리고 A 레코드까지
# 중간에 누가 누구에게 물어봤는지 전부 출력된다.
```

### 직접 검증 ② — DNS 응답의 비트를 직접 보기

```bash
sudo tcpdump -i any -c 2 -X 'port 53 and host 1.1.1.1'
# 응답 쪽 마지막 4 B 가 우리가 받을 IP 의 네트워크 표현 그대로 찍힌다.
```

### 직접 검증 ③ — 캐시가 왜 두 번째부터 빨라지나

```bash
getent hosts www.example.net    # 첫 호출: 리졸버까지 왕복
getent hosts www.example.net    # 두 번째: 시스템/nscd 캐시에서 즉답

# 캐시 유효 시간은 DNS 응답의 TTL (위 덤프에서 0x0000012C = 300 초) 가 정한다.
```

---

## B-3. TCP 순서 · 스트림 · 3-way handshake 심화 (L8-4 ~ L8-8)

### 원 질문

- TCP 는 지연이 생겨도 순서 인자를 가지고 있어서 데이터를 다시 맞출 수 있는가? (최현진)
- TCP 는 몇 번째 데이터인지 인자를 가지고 있고, UDP 는 그런 인자가 없어서 지연되면 섞일 수 있는가? (최현진)
- 기기는 들어온 데이터가 TCP 인지 UDP 인지 어떻게 알아차리는가? (최현진)
- TCP 는 송신자가 50 B 씩 두 번 보내도 수신자가 100 B 를 한 번에 읽을 수 있는 이유가 무엇인가? (최현진)
- 더 까다로운 TCP 가 어떻게 자유롭게 100 이라는 물을 퍼다 주는가? (최현진)
- TCP 3-way handshake 의 구체적인 단계는 무엇인가? (홍윤기)
- 3-way handshake 는 SYN, SYN/ACK, ACK 로 이루어진다. 이때 정확히 어떤 정보를 전달하는가? 정보의 형태와 무슨 내용이 들어있는가? (최현진)
- socket close 할 때 3-way handshake 를 하나? (홍윤기)
- 만약 인터넷이 예상치 못하게 끊겨서 3-way handshake 를 못하면 어떻게 되나? 클라이언트 측, 서버 측 둘 다 설명하시오. (홍윤기)
- SOCK_STREAM 은 소켓이 인터넷 연결의 끝점이 될 것이라는 뜻인데, 다른 타입은 어떤 게 있고, 역할은 무엇인가? (홍윤기)
- client 의 소켓의 포트가 임시 포트라면, 이 임시 포트는 방화벽에 대해서 열려 있어야 서버로부터 recv 할 수 있는 것 아닌가? 어떻게 이루어지는가? (홍윤기)
- server 측 listen socket 과 accept socket 은 포트 번호가 서로 다를 것 같다. 식별을 위해 accept socket 도 서로 다를 텐데, client 는 이 port 를 전부 알아야 하는가? (홍윤기)

### 설명

이 질문 묶음은 **"TCP 는 두 호스트의 커널이 각자 seq 번호로 추적하는 단일 바이트 스트림이고, 그 스트림을 열고 닫는 약속이 handshake"** 라는 하나의 얼개에서 모두 나온다.

- **TCP 가 순서를 맞출 수 있는 이유**: TCP 세그먼트마다 32 비트 `seq` 필드가 들어 있다. seq 는 바이트 번호다. 수신 측 커널은 이 번호로 중복을 걸러내고 재배열한다. 중간 패킷이 늦게 오면 먼저 도착한 것들을 버퍼에 잡아두고 빈 구간이 채워질 때까지 유저에게 데이터를 내주지 않는다.
- **UDP 는 왜 순서를 못 맞추나**: UDP 헤더에는 seq 필드 자체가 없다. 각 데이터그램이 독립이다. 순서는 앱이 필요하면 직접 (예: RTP 의 sequence number) 심어야 한다.
- **TCP / UDP 구분**: IP 헤더 `Protocol` 필드 한 바이트가 답이다. TCP = 6, UDP = 17. 수신 호스트의 `ip_rcv()` 가 이 값을 보고 `tcp_v4_rcv()` 또는 `udp_rcv()` 로 분기한다.
- **50 B + 50 B 를 100 B 로 읽을 수 있는 이유**: TCP 는 **바이트 스트림** 이다. 앱이 `write(sock, buf1, 50)` 두 번 해도 커널이 묶어 하나의 세그먼트로 보낼 수도 있고, 수신 측은 소켓 버퍼(`sk_receive_queue`) 에 모아둔 뒤 `read(sock, buf, 100)` 한 번에 100 B 를 내줄 수도 있다. 메시지 경계를 보존하지 않기 때문이다. UDP 는 반대로 메시지 경계를 보존한다.
- **까다로운데 어떻게 자유로운가**: 까다로운 건 양 커널이다. seq / ack / window / retransmit / checksum 을 전부 관리한다. 앱 입장에서는 그게 다 가려져서 "100 B 를 원할 때 100 B 를 꺼낸다" 로만 보인다.
- **3-way handshake 가 전달하는 정보**: 초기 시퀀스 번호(ISN) 두 개와 window size, MSS, SACK 지원 여부, window scale, timestamp 등의 **옵션**. 양쪽이 이 값을 맞춰서 같은 4-tuple 로 같은 상태머신을 돌린다.
- **socket close 의 handshake**: close 는 "열 때의 3-way" 와 다른 **4-way (FIN / ACK / FIN / ACK)** 이다. 한쪽이 FIN 을 보내면 상대 read() 가 EOF(0) 를 얻는다. 상대도 FIN 을 보내고 상대의 FIN 에 ACK 를 보내면 양방향 닫힘. 마지막으로 능동 종료 쪽이 TIME_WAIT 를 2*MSL 만큼 유지한다.
- **인터넷이 끊겨 handshake 실패 시**: 클라이언트는 SYN 을 몇 차례 재전송하다가 (리눅스 기본 `tcp_syn_retries=6`, 약 127 초) 포기하고 `connect()` 가 `ETIMEDOUT` 로 반환한다. 서버는 SYN 을 본 적 없으므로 아무 일도 일어나지 않는다. SYN 은 갔는데 SYN+ACK 이 못 돌아온 경우엔 서버가 `tcp_synack_retries` 만큼 재전송하다 SYN queue 에서 삭제한다.
- **SOCK_STREAM / SOCK_DGRAM / SOCK_RAW / SOCK_SEQPACKET**: 각각 TCP 스트림, UDP 데이터그램, 원시 IP 패킷 접근, 메시지 경계를 보존하되 신뢰성 있는 스트림(주로 SCTP).
- **클라이언트 임시 포트와 방화벽**: 클라이언트의 ephemeral 포트(예: 51213)는 **방화벽에 "외부에서 들어오는 SYN 을 허용" 할 필요가 없다**. stateful 방화벽이 outbound SYN 을 기억해 두고, 돌아오는 SYN+ACK 은 "이 세션에 대한 응답" 으로 자동 허용한다 (=connection tracking). 서버의 80 포트만 외부에 열려 있으면 된다.
- **listen vs accept 소켓 포트**: **포트 번호는 똑같다**. 서버 `accept()` 가 반환하는 소켓은 새로운 `struct socket` 이지만 dst port 는 여전히 80 이다. 커널은 4-tuple `(src IP, src port, dst IP, dst port)` 전체로 세션을 구분하기 때문이다. 클라이언트가 서버의 80 만 알면 충분하다.

### TCP 플래그 비트 (handshake 의 핵심)

```text
TCP 헤더의 flags 필드 (9 비트, offset 0x0C ~ 0x0D)

       NS CWR ECE URG ACK PSH RST SYN FIN
        0   0   0   0   0   0   0   0   0

각 비트의 역할:
  NS   Nonce Sum (거의 안 씀)
  CWR  Congestion Window Reduced
  ECE  ECN-Echo
  URG  urgent pointer 사용
  ACK  acknowledgement 번호 유효
  PSH  수신 앱에게 즉시 전달 요청
  RST  연결 리셋
  SYN  시퀀스 번호 동기화 (연결 시작)
  FIN  송신 종료 (연결 끝)

handshake 에서 자주 쓰이는 조합의 하위 8 비트 = 1 B 의 값:

  SYN        = 0000_0010 = 0x02
  SYN | ACK  = 0001_0010 = 0x12
  ACK        = 0001_0000 = 0x10
  PSH | ACK  = 0001_1000 = 0x18
  FIN | ACK  = 0001_0001 = 0x11
  RST        = 0000_0100 = 0x04
  RST | ACK  = 0001_0100 = 0x14
```

### 3-way handshake 상태 머신

```text
CLIENT                                       SERVER
─────────────────                            ─────────────────
CLOSED                                       LISTEN

socket() + connect()
    │
    │  ──── [1] SYN  ──────────────────────>
    │       seq = C0                         (SYN queue 에 추가)
SYN_SENT                                     SYN_RECEIVED

    │  <── [2] SYN + ACK ──────────────────
    │       seq = S0, ack = C0 + 1           (half-open)

ESTABLISHED
    │
    │  ──── [3] ACK ─────────────────────>
    │       seq = C0 + 1, ack = S0 + 1       accept queue 로 이동
                                             accept() 가 반환
                                             ESTABLISHED

── 이제 양쪽 다 ESTABLISHED, 스트림 송수신 가능 ──
```

### 세그먼트 ① SYN — 클라이언트가 보내는 첫 패킷 비트 덤프

SYN 에는 payload 가 없다. TCP 옵션이 대신 붙어 있어 세그먼트 길이는 보통 40 B (헤더 20 + 옵션 20).

```text
===== IP + TCP SYN =====

IP 헤더 (20 B)
  45 00 00 3C 12 34 40 00 40 06 59 0B 80 02 C2 F2 D0 D8 B5 0F
  ├ 45         Ver4 IHL5
  ├ 00         ToS
  ├ 00 3C      Total length = 60 (IP 20 + TCP 40)
  ├ 12 34      Id
  ├ 40 00      Flags DF
  ├ 40         TTL 64
  ├ 06         Proto TCP
  ├ 59 0B      checksum
  ├ 80 02 C2 F2  src IP
  └ D0 D8 B5 0F  dst IP

TCP 헤더 + 옵션 (40 B)
  C8 2D 00 50 00 00 0A 00 00 00 00 00 A0 02 FA F0 ...
  ├ C8 2D      src port 51213
  ├ 00 50      dst port 80
  ├ 00 00 0A 00   seq = 0x00000A00  (= ISN_C, 예시)
  ├ 00 00 00 00   ack = 0 (아직 받은 게 없음)
  ├ A0         data offset = 10 × 4 = 40 B (헤더 + 옵션)
               = 1010_0000 상위 4 비트가 data offset
  ├ 02         flags = SYN
               = 0000_0010
  ├ FA F0      window = 64240
  ├ checksum, urgent

  옵션 20 B (SYN 에서 자주 붙는 묶음):
    02 04 05 B4          MSS = 1460 (0x05B4)
    01                   NOP (정렬용)
    03 03 07             Window scale = 7 (×128)
    01 01                NOP NOP
    04 02                SACK permitted
    08 0A xx xx xx xx xx xx xx xx   Timestamp (옵션 코드 08, len 10)

  window 가 0xFAF0 = 64240 으로 고정된 이유는 아직 scale 이 협상되기 전이기
  때문이다. 이 시점에는 상대가 window scale 옵션을 보냈을 때만 × 128 로 계산한다.
```

### 세그먼트 ② SYN+ACK — 서버가 보내는 응답 비트 덤프

```text
===== IP + TCP SYN+ACK =====

IP 헤더 (20 B, 방향 반대)
  45 00 00 3C ... 40 06 ... D0 D8 B5 0F 80 02 C2 F2
                   ^^^^^^^          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                   TTL / proto 동일   src / dst IP 자리 뒤집힘

TCP 헤더 + 옵션 (40 B)
  00 50 C8 2D 00 00 B0 00 00 00 0A 01 A0 12 FF FF ...
  ├ 00 50      src port 80
  ├ C8 2D      dst port 51213
  ├ 00 00 B0 00   seq = 0x0000B000  (= ISN_S, 예시)
  ├ 00 00 0A 01   ack = 0x00000A01  (= ISN_C + 1, SYN 자체가 1 바이트로 계산됨)
  ├ A0         data offset = 40 B
  ├ 12         flags = SYN | ACK
               = 0001_0010
  ├ FF FF      window
```

여기서 핵심은 `ack = ISN_C + 1` 이다. 서버는 "네가 보낸 seq 0x0A00 까지 잘 받았고, 다음엔 0x0A01 을 기대한다" 라고 말한다. SYN 은 payload 가 0 바이트인데도 **seq 를 1 소모** 한다 (FIN 도 동일). 그래서 +1 이다.

### 세그먼트 ③ ACK — 클라이언트가 마무리 비트 덤프

```text
===== IP + TCP ACK =====

TCP 헤더 20 B (옵션 없음, 40 → 20 으로 축소)
  C8 2D 00 50 00 00 0A 01 00 00 B0 01 50 10 FF FF ...
  ├ C8 2D      src port 51213
  ├ 00 50      dst port 80
  ├ 00 00 0A 01   seq = ISN_C + 1
  ├ 00 00 B0 01   ack = ISN_S + 1
  ├ 50         data offset = 5 × 4 = 20 B
  ├ 10         flags = ACK
               = 0001_0000
  ├ FF FF      window
```

이 ACK 가 서버에 도착한 순간, 서버의 `accept queue` 에 해당 세션이 올라가고 서버 앱의 `accept()` 가 반환된다. **비로소 양쪽이 같은 4-tuple 로 ESTABLISHED 가 되었고**, 이 시점부터 실제 HTTP request 같은 payload 가 PSH|ACK 세그먼트로 실려 나갈 수 있다.

### close 의 4-way

```text
CLIENT                                 SERVER
ESTABLISHED                            ESTABLISHED

close() 호출
    │  ──── FIN ─────────────────────>
    │       seq = X, flags = 0x11
FIN_WAIT_1                             CLOSE_WAIT
    │  <── ACK ──────────────────────
    │       ack = X + 1
FIN_WAIT_2                             (서버 앱이 read() 에서 0 을 받음)

                                       서버 앱이 close() 호출
    │  <── FIN ──────────────────────
    │       seq = Y, flags = 0x11
(ack 를 먼저 받았으면 FIN_WAIT_2)        LAST_ACK
    │  ──── ACK ─────────────────────>
    │       ack = Y + 1
TIME_WAIT                              CLOSED

(2 × MSL 대기, 보통 60 초)
CLOSED
```

TIME_WAIT 의 이유는 마지막 ACK 이 손실되어 서버가 FIN 을 재전송할 수 있는데, 그때까지 **소켓 자원(4-tuple)** 을 보존해서 뒷 세션이 엉키지 않게 하려는 것이다.

### 직접 검증 ① — 3-way handshake 패킷 순서 확인

```bash
sudo tcpdump -i any -nn 'host example.net and port 80'
# 실제 출력 (요약):
# IP 128.2.194.242.51213 > 208.216.181.15.80: Flags [S],  seq 2560,                 win 64240, options [mss 1460, ...]
# IP 208.216.181.15.80 > 128.2.194.242.51213: Flags [S.], seq 45056, ack 2561,      win 65535, options [mss 1460, ...]
# IP 128.2.194.242.51213 > 208.216.181.15.80: Flags [.],                 ack 45057, win 502
#
# [S]  = SYN
# [S.] = SYN + ACK
# [.]  = ACK (순수 ACK, 아무 payload 없음)
```

### 직접 검증 ② — 소켓 상태 전이 보기

```bash
ss -tno 'state all' sport = :51213 or dport = :80
# State        Recv-Q  Send-Q   Local Address:Port   Peer Address:Port
# SYN-SENT     0       1        128.2.194.242:51213  208.216.181.15:80
# ... handshake 후:
# ESTAB        0       0        128.2.194.242:51213  208.216.181.15:80
# close 후:
# TIME-WAIT    0       0        128.2.194.242:51213  208.216.181.15:80
```

### 직접 검증 ③ — IP 프로토콜 필드가 실제 TCP (6) 인지

```bash
sudo tcpdump -i any -c 1 -X 'port 80 and host example.net'
# 0x0000:  4500 003c 1234 4000 4006 590b ...
#                                 ^^
#                                 0x06 = TCP
```

---

## B-1 · B-2 · B-3 통합: 비트 버퍼가 이름부터 연결까지 열리는 전체 여정

### 이 섹션의 목표

Part B 의 세 질문 묶음을 실제 비트 덤프로 이어붙인다. 클라이언트가 `curl http://www.example.net/` 한 줄을 치는 순간부터 TCP 가 ESTABLISHED 가 되어 첫 HTTP request 가 나가기 직전까지의 **모든 바이트**를 추적한다.

이 섹션을 통과하면 아래 질문에 전부 답할 수 있어야 한다.

- 도메인 이름이 어떤 UDP 패킷 비트로 바뀌어 DNS 서버에 전달되는가
- 돌아온 응답 비트에서 어떤 4 바이트가 `connect()` 에 들어가는가
- `connect()` 가 낳는 SYN 세그먼트의 실제 비트가 어떻게 구성되는가
- SYN+ACK 의 ack 번호가 왜 ISN_C + 1 인가를 비트 덤프로 확인
- ACK 가 서버 커널의 어느 큐를 건드려 accept() 를 깨우는가
- 인터넷이 끊어지면 이 비트 스트림이 어느 단계에서 멈추고 에러가 어디서 나는가

### STEP 0. 앱 시점 — `curl http://www.example.net/`

앱 입장에서는 한 줄이지만, 내부는 아래 순서다.

```text
(1) getaddrinfo("www.example.net", "80", ...)   → DNS 해석
(2) socket(AF_INET, SOCK_STREAM, 0)              → sockfd 생성
(3) connect(sockfd, &sockaddr, ...)              → TCP 3-way handshake
(4) write(sockfd, "GET / HTTP/1.0\r\n\r\n", 18)  → HTTP request
(5) read(sockfd, ...)                            → HTTP response
(6) close(sockfd)                                → FIN 4-way
```

이 섹션은 (1) ~ (3) 을 비트 단위로 본다. (4) 이후는 Part A 와 Part C 에서 다룬다.

### STEP 1. DNS 쿼리 UDP 패킷의 비트

`getaddrinfo` 는 내부적으로 libc 의 resolver 가 `/etc/resolv.conf` 를 읽어 첫 번째 `nameserver 1.1.1.1` 로 UDP/53 에 쿼리를 쏜다.

```text
UDP payload = DNS query = 32 B (위 B-2 에서 이미 본 것과 동일)

  12 34 01 00 00 01 00 00 00 00 00 00
  03 77 77 77 07 65 78 61 6D 70 6C 65 03 6E 65 74 00
  00 01 00 01

UDP 헤더 8 B (이 앞에 붙음)
  E8 1C 00 35 00 28 XX XX
  ├ E8 1C      src port = 59420 (ephemeral)
  ├ 00 35      dst port = 53
  ├ 00 28      length = 40 (UDP hdr 8 + payload 32)
  ├ XX XX      checksum

IP 헤더 20 B
  45 00 00 3C 56 78 40 00 40 11 XX XX 80 02 C2 F2 01 01 01 01
  ├ 40 11      TTL 64, Proto UDP (0x11 = 17)
  └ 01 01 01 01  dst IP = 1.1.1.1

Ethernet 14 B
  ├ dst MAC = 게이트웨이 R 의 MAC
  ├ src MAC = 호스트 A
  └ 0x0800

총 82 B 프레임.
```

IP 헤더 `Protocol = 0x11` 이 이 패킷을 TCP 아닌 UDP 로 식별하게 한다. TCP (=0x06) 와 단 한 비트 차이로 완전히 다른 경로가 갈린다.

### STEP 2. DNS 응답의 핵심 4 바이트

UDP 의 return payload 끝에서 `D0 D8 B5 0F` 가 나오는 순간, libc 는 이 값을 `struct in_addr` 에 채운다. 이 구조체가 `addrinfo` 에 실려 앱으로 돌아오고, `connect()` 호출 시 `sockaddr_in.sin_addr` 로 그대로 쓰인다.

```c
// libc 내부에서 일어나는 동작의 모양
struct sockaddr_in sa;
sa.sin_family = AF_INET;
sa.sin_port   = htons(80);           // 0x0050 (네트워크 순서)
sa.sin_addr.s_addr = htonl(0xD0D8B50F);   // DNS 응답에서 추출한 4 B

// 메모리 상 sa 의 바이트 배치 (x86)
// family(2) = 02 00
// port(2)   = 00 50
// addr(4)   = D0 D8 B5 0F
// zero(8)   = 00 * 8
```

### STEP 3. connect() — 커널이 SYN 을 만들기까지

```text
(1) sys_connect()
    └ inet_stream_connect() 가 소켓 상태를 TCPF_CLOSED → TCPF_SYN_SENT.
      ephemeral port 51213 을 `inet_bind_bucket` 에서 할당.

(2) tcp_v4_connect()
    └ 라우팅 테이블 lookup: dst = 208.216.181.15 → next-hop = 게이트웨이 R
    └ inet_sk(sk)->inet_sport = htons(51213);
       inet_sk(sk)->inet_dport = htons(80);
       inet_sk(sk)->inet_saddr = htonl(호스트 A 의 src IP);
       inet_sk(sk)->inet_daddr = htonl(0xD0D8B50F);

(3) tcp_connect()
    └ 초기 시퀀스 번호 ISN_C 생성 (secure_tcp_seq() 가 4-tuple + 시각 기반 해시)
    └ skb = tcp_send_syn();
       skb 안에 TCP 헤더 + SYN 옵션 20 B 를 채움.

(4) tcp_transmit_skb()
    └ skb_push(20) 으로 IP 헤더 자리 확보
       IP 헤더의 Protocol 필드에 0x06 (TCP) 기록.
       IP checksum 계산.
    └ dev_queue_xmit()
       드라이버가 Ethernet 헤더 14 B 를 붙이고 DMA 로 NIC 에 넘김.
    └ NIC 가 FCS 4 B 를 계산 → 선로로 송신.

(5) 앱의 connect() 는 syscall 안에서 자고 있다.
    ── wait_for_completion(sk_stream) ──
    응답(SYN+ACK) 을 받고 ACK 를 보낸 뒤에야 ETIMEDOUT 또는 0 을 반환.
```

SYN 프레임의 완전한 비트 덤프 (B-3 의 세그먼트 ① 과 동일한 내용을 한 줄에):

```text
Eth 14 + IP 20 + TCP 40 = 74 B

offset 0x00:  11 11 11 11 11 11 AA AA AA AA AA AA 08 00
offset 0x0E:  45 00 00 3C 12 34 40 00 40 06 59 0B 80 02 C2 F2
offset 0x1E:  D0 D8 B5 0F
offset 0x22:  C8 2D 00 50 00 00 0A 00 00 00 00 00 A0 02 FA F0 XX XX 00 00
              (TCP 기본 20 B)
offset 0x36:  02 04 05 B4 01 03 03 07 01 01 04 02 08 0A XX XX XX XX XX XX
              (옵션 20 B)
```

### STEP 4. 서버 커널 측 — SYN 수신 → SYN queue → SYN+ACK

서버 호스트 B 의 NIC 에 도착한 74 B 프레임이 커널을 뚫고 올라가는 경로.

```text
(1) NIC → DMA → NAPI → skb 생성
    eth_type_trans → 0x0800 → ip_rcv.

(2) ip_rcv(skb)
    Protocol=0x06 → tcp_v4_rcv(skb)
    skb_pull(20) 으로 IP 헤더 벗김.

(3) tcp_v4_rcv(skb)
    4-tuple 조회:
      (src=C2F2, sport=C82D, dst=B50F, dport=0050)
    → ESTABLISHED 소켓 해시에 없음
    → LISTEN 소켓 해시 조회 → 80 포트 LISTEN 발견
    → flags 확인: SYN only
    → tcp_conn_request()
       └ SYN cookie 또는 SYN queue 에 request_sock 생성
       └ 응답 SYN+ACK skb 생성
       └ ISN_S = secure_tcp_seq(서버 쪽 4-tuple 뒤집어서)
       └ ack = ISN_C + 1

(4) SYN+ACK skb 를 IP → Ethernet → NIC 경로로 송신.
```

이 시점 서버 측에는 아직 `accept()` 가 반환되지 않았다. 세션은 **SYN_RECV** 상태로 SYN queue 에 올라 있을 뿐이다.

### STEP 5. 클라이언트 측 — SYN+ACK 수신 → ACK 송신 → ESTABLISHED

```text
(1) 호스트 A 의 NIC 가 응답을 수신 → ip_rcv → tcp_v4_rcv.

(2) tcp_v4_rcv 는 이번엔 4-tuple 로 ESTABLISHED 해시 조회 전에
    SYN_SENT 상태의 소켓을 찾아야 하므로 `inet_lookup_listener` 가 아닌
    `__inet_lookup_established` 로 직행 (이미 ephemeral port 51213 가 소켓에 바인딩됨).

(3) tcp_rcv_synsent_state_process()
    └ flags = SYN|ACK 확인
    └ ack 가 우리가 보낸 ISN_C + 1 인지 검증
    └ 상대 ISN_S 를 저장
    └ 옵션 협상 결과 확정 (MSS, window scale, SACK, timestamp)
    └ 상태 전이: SYN_SENT → ESTABLISHED
    └ 응답 ACK skb 생성 (payload 없음, 헤더 20 B)
    └ NIC 경로로 송신.

(4) connect() syscall 깨어남 → 0 반환.
```

### STEP 6. 서버 측 — ACK 수신 → accept queue → accept() 반환

```text
(1) 서버 tcp_v4_rcv 가 이번 ACK 를 받음.
    4-tuple 이 SYN queue 의 request_sock 과 매칭.

(2) tcp_check_req()
    └ ACK 의 ack 번호가 ISN_S + 1 인지 확인.
    └ 통과하면 full socket 생성:
         child = inet_csk_clone_lock(listen_sk, ...);
         child 의 상태를 ESTABLISHED 로.
    └ SYN queue 에서 꺼내 accept queue 에 enqueue.

(3) sk_data_ready(listen_sk) 호출
    └ listen 소켓에서 wait 하던 accept() syscall 을 깨움.

(4) sys_accept()
    └ accept queue 에서 child 를 pop.
    └ 새 파일 디스크립터를 할당해 child 와 엮음.
    └ syscall 반환값으로 새 fd 를 앱에게 돌려줌.
```

이 순간 **양쪽 커널에 같은 4-tuple 의 TCP 제어 블록이 ESTABLISHED 로 서 있다**. 이제 HTTP request 를 실어 보낼 수 있다 — 그게 Part A 의 `write()` 로 이어진다.

### STEP 7. 실패 경로 — 핸드셰이크가 깨지면 비트가 어디서 멈추는가

```text
(a) SYN 이 목적지에 닿지 못함 (경로 단절)
    ── 호스트 A 의 커널이 SYN 을 tcp_syn_retries=6 만큼 재전송.
       시간 간격: 1, 2, 4, 8, 16, 32, 64 초 (총 약 127 초)
    ── 포기 후 connect() 가 -ETIMEDOUT 반환.
    ── 호스트 B 는 SYN 자체를 본 적 없음. 아무 상태 없음.

(b) SYN 은 받았는데 SYN+ACK 이 돌아오지 못함
    ── 호스트 B 커널이 SYN queue 에 유지, tcp_synack_retries 만큼 재전송.
    ── 양쪽 모두 타임아웃으로 포기. 호스트 A 의 connect 도 -ETIMEDOUT.

(c) 방화벽이 RST 를 내려줌 (서버가 80 포트 닫혀 있음)
    ── 호스트 A 의 SYN 에 호스트 B (또는 게이트웨이) 가 RST 를 응답.
    ── tcp_rcv_state_process → 상태 CLOSED 로 리셋.
    ── connect() 가 -ECONNREFUSED 반환.

(d) ACK 를 먼저 보냈는데 서버가 못 받음
    ── 서버는 여전히 SYN_RECV 에서 SYN+ACK 을 재전송.
    ── 클라이언트는 ESTABLISHED 로 착각하고 write() 를 시도.
       나가는 패킷에 PSH|ACK 로 payload 가 실리면 서버가 그 ACK 를 보고
       핸드셰이크가 "implicit" 하게 마무리되거나, 아니면 RST.
```

### 직접 검증 ① — 한 줄로 전체 여정 보기

```bash
sudo tcpdump -i any -nn 'host example.net' &

# 다른 터미널에서
curl -s http://www.example.net/ >/dev/null

# 출력에서 DNS UDP 쿼리 → UDP 응답 → TCP SYN → SYN+ACK → ACK → HTTP → FIN ... 이
# 순서대로 관찰된다.
```

### 직접 검증 ② — 각 단계의 상태 전이 보기

```bash
ss -tno 'state all' '( dport = :80 or sport = :80 )'
# handshake 중에 SYN-SENT → ESTAB 으로 넘어가고,
# close 뒤에는 TIME-WAIT 로 남는 것을 실시간 관찰 가능.
```

### 직접 검증 ③ — 실패 시나리오 재현

```bash
# (c) 닫힌 포트에 SYN 쏘기
curl -v --max-time 5 http://127.0.0.1:9       # 9 번 포트 대부분 닫혀 있음
# → connect() 가 ECONNREFUSED 로 즉시 반환

# (a) 라우팅 불가 IP 로 SYN 쏘기
curl -v --max-time 10 http://10.255.255.1/
# → tcp_syn_retries 만큼 기다렸다가 ETIMEDOUT
```

### 직접 검증 ④ — bpftrace 로 커널 함수 진입 순서 확인

```bash
sudo bpftrace -e '
  kprobe:tcp_v4_connect    { printf("connect()\n"); }
  kprobe:tcp_connect       { printf("  send SYN\n"); }
  kprobe:tcp_rcv_synsent_state_process { printf("  got SYN+ACK, sending ACK\n"); }
  kprobe:inet_csk_complete_hashdance   { printf("  ESTABLISHED\n"); }
'

# 다른 터미널에서 curl 을 치면:
#   connect()
#     send SYN
#     got SYN+ACK, sending ACK
#     ESTABLISHED
```

---

## 전체 검증 명령 모음

```bash
# 엔디안 확인
printf '%04x\n' "$(python3 -c 'import socket; print(socket.htons(80))')"

# DNS resolve 과정 따라가기
dig +trace www.example.net
dig @1.1.1.1 www.example.net

# TCP handshake 패킷 살펴보기
sudo tcpdump -i any -nn -c 10 'host example.net and port 80'
sudo tcpdump -i any -nn -c 10 -X 'host example.net and port 80'

# 소켓 상태 전이 관찰
watch -n 0.5 "ss -tno 'state all' sport = :51213 or dport = :80"

# 옵션 협상 결과 확인
ss -tnoi 'dport = :80'   # 'i' 플래그: MSS, cwnd, rtt, window scale

# proto 필드 확인
sudo tcpdump -i any -c 1 -X 'port 80 and host example.net'
```

## 연결 문서

- [q02-ip-address-byte-order.md](./q02-ip-address-byte-order.md) — IP/MAC/Port 주소 체계, 네트워크 바이트 순서
- [q03-dns-domain-cloudflare.md](./q03-dns-domain-cloudflare.md) — DNS, 도메인 등록, Cloudflare
- [q06-ch11-4-sockets-interface.md](./q06-ch11-4-sockets-interface.md) — Sockets Interface 함수 + addrinfo
- [q07-tcp-udp-socket-syscall.md](./q07-tcp-udp-socket-syscall.md) — TCP/UDP 소켓 시스템콜, host-to-host vs process-to-process
- [part-a-whiteboard-topdown.md](./part-a-whiteboard-topdown.md) — Part A, 핸드셰이크 이후의 write/wire 경로
