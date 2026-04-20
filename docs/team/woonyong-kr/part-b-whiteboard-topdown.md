# Part B. 프로토콜 스택 (주소·연결·Handshake) — 화이트보드 탑다운 발표안

이 문서는 Part B 발표를 위해 만든 **실전용 화이트보드 원고**다.
목표는 `www.example.net` 같은 도메인 이름이 어떻게 **IP 주소가 되고**, 그 주소 위에서 어떻게 **TCP 연결이 성립하고**, 왜 TCP 가 **stream** 으로 동작하는지를 **실제 숫자와 실제 함수**로 설명하는 것이다.

Part B 에서는 키워드를 나열하면 안 된다. 반드시 아래 한 줄의 흐름으로 밀고 가야 한다.

## Part B 에서 끝까지 밀고 갈 한 문장

```text
도메인은 DNS 로 IP 로 바뀌고
IP 와 포트는 getaddrinfo 와 socket/connect 로 연결되고
그 연결은 SYN, SYN/ACK, ACK 로 열리며
그 위에서 TCP 는 메시지가 아니라 순서 보장 바이트 스트림을 만든다.
```

## 발표 전에 칠판에 미리 고정할 숫자

이 숫자는 중간에 바꾸지 않는다.

```text
Domain        = www.example.net
Resolved IP   = 208.216.181.15

Client
  IP          = 128.2.194.242
  port        = 51213

Server
  IP          = 208.216.181.15
  port        = 80

Host byte order example
  80 decimal  = 0x0050

Handshake example
  client ISN  = 1000
  server ISN  = 9000
```

## 화이트보드 배치

```text
+--------------------------------------------------------------------------------+
| 상단: "www.example.net -> 208.216.181.15:80 -> TCP stream"                     |
+--------------------------------------+-----------------------------------------+
| 왼쪽: 이름 -> 주소 -> 연결            | 오른쪽: 숫자 예시                      |
| DNS / getaddrinfo / socket/connect   | port, seq, ack, 4-tuple                |
+--------------------------------------+-----------------------------------------+
| 하단: 끝까지 남길 키워드                                                    |
| DNS / A record / htons / sockaddr / SYN / ACK / 4-tuple / stream / FIN        |
+--------------------------------------------------------------------------------+
```

## 발표 흐름 전체 지도

```text
Scene 1   도메인 이름과 주소를 분리
Scene 2   MAC / IP / Port 세 층
Scene 3   IPv4 / IPv6 와 prefix 감각
Scene 4   byte order 와 htons
Scene 5   DNS resolve 실제 경로
Scene 6   getaddrinfo -> socket -> connect
Scene 7   TCP 3-way handshake
Scene 8   stream vs datagram
Scene 9   listen socket / accept socket / ephemeral port
Scene 10  close / FIN / timeout / 연결 실패
```

---

## Scene 1. 사람은 이름을, 기계는 주소를 본다

칠판에 제일 먼저 아래를 적는다.

```text
www.example.net
      |
      v
208.216.181.15:80
```

그리고 이렇게 말한다.

`사람은 도메인을 기억하지만, 실제 TCP connect 는 결국 IP 주소와 포트로 일어납니다. Part B 는 이 변환과 연결 성립 과정을 설명하는 파트입니다.`

이 장면의 목적:

- "도메인 이름"과 "실제 연결 대상"을 분리하기
- DNS 가 왜 필요한지 감각 잡기

바로 이어서:

`그런데 주소도 하나가 아닙니다. MAC, IP, Port 라는 세 가지 주소 층이 있습니다.`

---

## Scene 2. MAC / IP / Port 세 종류의 주소

칠판에 크게 적는다.

```text
MAC   = 링크 계층, 바로 옆 기계
IP    = 인터넷 계층, 호스트
Port  = 전송 계층, 프로세스
```

그리고 이번 예시에 맞게 숫자를 쓴다.

```text
Client IP   = 128.2.194.242
Client port = 51213

Server IP   = 208.216.181.15
Server port = 80
```

꼭 말해야 하는 문장:

`TCP 연결 하나는 결국 (src IP, src port, dst IP, dst port)라는 4-tuple 로 식별됩니다.`

칠판에 4-tuple 도 적는다.

```text
(128.2.194.242, 51213, 208.216.181.15, 80)
```

이 장면에서 초보자가 자주 헷갈리는 것:

- IP 하나만 알면 서비스까지 특정되는 줄 안다
- 포트는 단순 부가정보라고 생각한다
- MAC / IP / Port 를 같은 종류의 주소로 본다

한 줄 정리:

`Port 는 프로세스를, IP 는 호스트를, MAC 은 현재 홉의 링크 대상만 식별합니다.`

### 직접 검증 — 4-tuple 을 실제로 뽑아 보기

```bash
# 터미널 1
python3 -m http.server 8080

# 터미널 2
curl -s http://127.0.0.1:8080/ > /dev/null &
ss -tanp '( sport = :8080 or dport = :8080 )'
# ESTAB 0 0 127.0.0.1:8080  127.0.0.1:51213  users:(("python3",pid=1234,fd=4))
# ESTAB 0 0 127.0.0.1:51213 127.0.0.1:8080   users:(("curl",pid=1235,fd=3))
```

두 줄이 **같은 연결의 양쪽 끝** 이다. `(127.0.0.1, 51213, 127.0.0.1, 8080)` 이 칠판에 쓴 4-tuple 이다. macOS 에서는 `lsof -nP -iTCP:8080` 로 같은 정보를 본다.

---

## Scene 3. IPv4 / IPv6 와 prefix 감각

이번 장면은 주소 자체를 해석하는 감각을 주는 용도다.

칠판에 적는다.

```text
IPv4  = 32bit = 4B
       128.2.194.242

IPv6  = 128bit = 16B
       2001:db8::1
```

그리고 prefix 를 같이 적는다.

```text
128.2.194.242
10000000.00000010.11000010.11110010

128.2.0.0/16
128.2.194.0/24
```

핵심 설명:

- IPv4 주소는 `uint32_t` 하나다.
- 사람이 보기 쉽게 `a.b.c.d` 로 끊어 쓰는 것뿐이다.
- 라우팅은 "개별 주소"보다 보통 `/16`, `/24` 같은 prefix 단위로 이뤄진다.

꼭 짚을 포인트:

- "32비트로 어떻게 전 세계를 감당하냐" -> CIDR, prefix, NAT 가 같이 동작한다.
- IPv6 는 128비트라 주소 공간이 사실상 매우 넓다.

이 장면은 Part B 전체에서 주소 체계를 너무 얕게 보지 않게 만들어 준다.

### 직접 검증 — IPv4 = uint32_t 라는 말의 증거

```bash
# (1) 문자열을 32비트 정수로 풀어 본다
python3 - <<'PY'
import socket, struct
ip = "128.2.194.242"
n  = struct.unpack("!I", socket.inet_aton(ip))[0]
print(f"{ip} -> 0x{n:08x} = {n} = {n:032b}")
# 128.2.194.242 -> 0x8002c2f2 = 2147664114 = 10000000 00000010 11000010 11110010
PY

# (2) prefix /24 안에 내 IP 가 들어 있는지 확인
python3 -c "import ipaddress; print(ipaddress.ip_address('128.2.194.242') in ipaddress.ip_network('128.2.194.0/24'))"

# (3) 내 호스트의 IPv4/IPv6 비교
ip -4 addr ; ip -6 addr
```

화이트보드에서 강조: 두 번째 출력 `10000000 ...` 이 Scene 3 에서 미리 써 둔 이진 비트 4조각과 **완벽히 동일** 하다는 걸 보여 준다.

---

## Scene 4. byte order: 왜 htons 가 필요한가

이 장면은 반드시 실제 바이트 배열까지 적어야 한다.

```text
port 80 = 0x0050

host (little-endian) memory
  [0x50, 0x00]

network (big-endian)
  [0x00, 0x50]
```

그리고 아래를 적는다.

```text
htons(80)  필요
htonl(ip)  필요
ntohs(...) / ntohl(...) 필요
```

꼭 말해야 하는 문장:

`x86/ARM 메모리 표현과 네트워크 표준 바이트 순서가 다르기 때문에, 소켓 구조체에 포트와 주소를 넣을 때는 host -> network 변환이 필요합니다.`

실패 사례도 적는다.

```text
sin_port = 80
-> 메모리 바이트 [0x50, 0x00]
-> 네트워크에서 0x5000 = 20480 으로 오해될 수 있음
```

이 장면에서 같이 구분해야 하는 것:

- `htons` / `htonl` 은 바이트 순서 변환
- `inet_pton` / `inet_ntop` 은 문자열 <-> 바이너리 변환

즉:

```text
htons   = 숫자 바이트 순서 변환
inet_pton = 문자열 IP 를 binary address 로 변환
```

### 직접 검증 — host ↔ network byte order 실물 확인

```bash
python3 - <<'PY'
import socket, struct
print("htons(80)   =", hex(socket.htons(80)))        # 0x5000 on little-endian host
print("ntohs(0x5000)=", socket.ntohs(0x5000))        # 80
print("htonl(0x0A00_0001) =", hex(socket.htonl(0x0A000001)))

# sockaddr_in 실제 바이트
addr = socket.inet_aton("128.2.194.242")
port = struct.pack("!H", 80)
print("network bytes =", (port + addr).hex())
# 0050 8002c2f2  <- Scene 4 의 [0x00, 0x50] 과 동일
PY

# inet_pton/ntop 도 바로 확인
python3 -c "import socket; print(socket.inet_pton(socket.AF_INET,'128.2.194.242').hex())"
# 8002c2f2
```

화이트보드에서 강조: `htons(80) = 0x5000` 이 little-endian 머신 메모리에서 `[0x00, 0x50]` 으로 찍히는 것이 **"네트워크 표준으로 정렬됐다"** 의 증거다.

---

## Scene 5. DNS resolve 는 실제로 어떻게 일어나나

이제 이름이 IP 로 바뀌는 실제 경로를 그린다.

```text
브라우저
  |
  v
stub resolver (libc)
  |
  v
recursive resolver (1.1.1.1 / ISP)
  |
  +-> Root
  +-> .net TLD
  +-> authoritative NS
  |
  v
208.216.181.15
```

실제 질의 흐름을 적는다.

```text
getaddrinfo("www.example.net", "80")
 -> stub resolver
 -> recursive resolver
 -> Root nameserver
 -> .net nameserver
 -> Cloudflare authoritative nameserver
 -> A record = 208.216.181.15
```

핵심 설명:

- 도메인 등록과 DNS 조회는 다른 문제다.
- 등록은 registrar/registry/ICANN 의 세계고,
- resolve 는 root/TLD/authoritative NS 를 따라가는 질의의 세계다.

꼭 짚을 오해:

- DNS 는 "그냥 서버 하나"가 아니다.
- 매번 root 부터 끝까지 가는 것도 아니다. 캐시가 있다.
- DNS 가 끝나야 `connect(208.216.181.15:80)` 로 넘어갈 수 있다.

### 직접 검증 — DNS 질의를 루트부터 실제로 쫓기

```bash
# (1) 내 호스트의 resolver 설정
cat /etc/resolv.conf                     # Linux
scutil --dns | head -30                  # macOS

# (2) root -> TLD -> authoritative 를 한 번에 추적
dig +trace +nodnssec www.example.net
# .          NS a.root-servers.net.
# net.       NS a.gtld-servers.net.
# example.net. NS a.iana-servers.net.
# www.example.net. 86400 IN A 93.184.216.34

# (3) getaddrinfo 가 실제로 어디에 쿼리하는지 엿보기
strace -f -e trace=openat,connect,sendto,recvfrom getent hosts www.example.net 2>&1 | grep -E '53|resolv'
# connect(3, {sa_family=AF_INET, sin_port=htons(53), sin_addr=inet_addr("1.1.1.1")}, ...)

# (4) 동일 도메인 두 번 질의해서 캐시 시간 줄어드는 것 확인
dig www.example.net | grep -E 'ANSWER|IN\s+A' ; sleep 1
dig www.example.net | grep -E 'ANSWER|IN\s+A'
# TTL 이 줄어드는 것 = 캐시 증거
```

화이트보드에서 강조: `dig +trace` 의 다섯 줄 (root → .net → authoritative → A 레코드) 이 Scene 5 에서 그린 DNS 피라미드와 **그대로 1:1 대응** 한다.

---

## Scene 6. getaddrinfo -> socket -> connect

이제 실제 함수 호출을 붙인다.

```text
getaddrinfo(host, port, hints, &listp)
   |
   v
addrinfo linked list
   |
   v
socket(p->ai_family, p->ai_socktype, p->ai_protocol)
   |
   v
connect(clientfd, p->ai_addr, p->ai_addrlen)
```

이 장면에서 칠판에 같이 적을 코드 뼈대:

```c
memset(&hints, 0, sizeof(hints));
hints.ai_socktype = SOCK_STREAM;
hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG;

Getaddrinfo("www.example.net", "80", &hints, &listp);

for (p = listp; p; p = p->ai_next) {
    clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
        break;
    Close(clientfd);
}
```

핵심 설명:

- `getaddrinfo` 는 준비물 공장이다.
- `socket` 은 아직 연결이 아니다. fd 만 만든다.
- `connect` 가 실제로 SYN 을 보내며 연결 성립 절차를 시작한다.

그리고 `addrinfo` 필드도 간단히 적는다.

```text
ai_family    -> AF_INET / AF_INET6
ai_socktype  -> SOCK_STREAM / SOCK_DGRAM
ai_protocol  -> TCP / UDP
ai_addr      -> 실제 sockaddr*
ai_addrlen   -> 그 길이
ai_next      -> 다음 후보
```

꼭 남길 문장:

`getaddrinfo -> socket -> connect 는 준비물 생성 -> 엔드포인트 생성 -> 연결 시도의 세 단계입니다.`

### 직접 검증 — 세 개의 시스템콜이 순서대로 나오는지

```bash
# curl 한 번에 3-단계가 전부 찍힌다
strace -e trace=socket,connect,sendto,recvfrom,close -f -tt \
  curl -s http://127.0.0.1:8080/ -o /dev/null 2>&1 | head -20

# 주요 라인 예시
# 00:00.000001 socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, IPPROTO_TCP) = 3
# 00:00.000012 connect(3, {sa_family=AF_INET, sin_port=htons(8080),
#                          sin_addr=inet_addr("127.0.0.1")}, 16) = 0
# 00:00.000020 sendto(3, "GET / HTTP/1.1\r\n...", ...) = ...

# addrinfo 자체를 직접 찍고 싶으면 C 예제로
cat > /tmp/gai.c <<'C'
#include <netdb.h>
#include <stdio.h>
int main(void){
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *list, *p;
    getaddrinfo("www.example.net","80",&hints,&list);
    for (p = list; p; p = p->ai_next) {
        char host[64];
        getnameinfo(p->ai_addr, p->ai_addrlen, host, sizeof host, 0, 0, NI_NUMERICHOST);
        printf("family=%d socktype=%d proto=%d addr=%s\n",
               p->ai_family, p->ai_socktype, p->ai_protocol, host);
    }
}
C
cc /tmp/gai.c -o /tmp/gai && /tmp/gai
```

화이트보드에서 강조: `socket` 의 리턴값 3 이 바로 다음 `connect` 의 첫 인자 3 으로 이어진다는 점. Scene 6 에 그린 체인이 그대로 시스템콜 로그에 찍힌다.

---

## Scene 7. TCP 3-way handshake

이 장면은 반드시 숫자로 그린다.

```text
Client 128.2.194.242:51213                Server 208.216.181.15:80

SYN      seq=1000  --------------------------------------------->
                    <--------------------------------------------  SYN,ACK seq=9000 ack=1001
ACK seq=1001 ack=9001 ----------------------------------------->
```

그리고 헤더 안에 무엇이 있는지 말한다.

SYN 패킷에 들어가는 것:

- src port = 51213
- dst port = 80
- seq = 1000
- SYN = 1
- window
- MSS 등 옵션

SYN/ACK 에 들어가는 것:

- src port = 80
- dst port = 51213
- seq = 9000
- ack = 1001
- SYN = 1, ACK = 1

마지막 ACK:

- seq = 1001
- ack = 9001
- ACK = 1

꼭 설명할 문장:

`handshake 는 단순 인사말이 아니라, 양쪽이 서로의 초기 sequence 공간과 수신 가능 상태를 맞추는 절차입니다.`

질문이 들어오면 바로 답해야 하는 것:

- 왜 ack 가 `seq + 1` 인가
- SYN 은 왜 데이터 1바이트처럼 취급되나
- handshake 는 정확히 무엇을 합의하나

짧은 답:

`SYN 과 FIN 은 sequence 공간을 1칸 소비하기 때문에, 상대는 다음에 기대하는 번호를 ack 로 보냅니다.`

### 직접 검증 — 세 번의 handshake 실제 캡처

```bash
# (1) 더미 서버
python3 -m http.server 8080 >/dev/null &

# (2) SYN/SYN-ACK/ACK 세 줄만 뽑기
sudo tcpdump -i lo -nn -S -tttt 'tcp port 8080 and (tcp[tcpflags] & (tcp-syn|tcp-ack) != 0)' -c 3 &
sleep 0.3
curl -s http://127.0.0.1:8080/ >/dev/null
wait

# 출력 예시
# IP 127.0.0.1.51213 > 127.0.0.1.8080: Flags [S],  seq 1000, win 65535, options [mss 65495 ...]
# IP 127.0.0.1.8080  > 127.0.0.1.51213: Flags [S.], seq 9000, ack 1001, win 65535, ...
# IP 127.0.0.1.51213 > 127.0.0.1.8080: Flags [.],  seq 1001, ack 9001, win 512, length 0

# (3) 커널이 본 연결 상태 천이
ss -tanpio '( dport = :8080 or sport = :8080 )'
# ESTAB ... rto:204 rtt:0.04/0.02 ... bytes_sent:... bytes_acked:...
```

화이트보드에서 강조: `Flags [S]` → `Flags [S.]` → `Flags [.]` 와 seq/ack 쌍이 Scene 7 의 `seq=1000, seq=9000, ack=1001, ack=9001` 과 **완벽히 같은 모양** 이다.

---

## Scene 8. TCP stream vs UDP datagram

이 장면은 아주 단순하게 그리면 된다.

```text
write(50B)
write(50B)
   |
   v
TCP stream
   |
   v
read(100B) 가능
```

그리고 UDP 와 비교한다.

```text
UDP sendto(50B)
UDP sendto(50B)
 -> datagram 2개
 -> 경계 유지
```

핵심 설명:

- TCP 는 메시지 경계를 보존하지 않는다.
- TCP 가 보장하는 것은 순서와 신뢰성이다.
- UDP 는 datagram 경계가 살아 있다.

꼭 말해야 하는 문장:

`두 번 write 했으니 두 번 read 될 것이라는 기대는 TCP 에서는 틀립니다.`

### 직접 검증 — stream vs datagram 차이 실물 재현

```bash
# (1) TCP: 두 write 가 한 read 로 합쳐질 수 있다
( python3 -c "
import socket
s = socket.socket(); s.bind(('127.0.0.1',9001)); s.listen(1)
c,_ = s.accept()
print('read1=', c.recv(4096))   # 보통 b'AAA...BBB...' 로 합쳐짐
" & ) && sleep 0.2 && python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1',9001))
s.send(b'A'*50); time.sleep(0.05); s.send(b'B'*50)
s.close()
"

# (2) UDP: 경계 그대로 두 번
( python3 -c "
import socket
s = socket.socket(type=socket.SOCK_DGRAM); s.bind(('127.0.0.1',9002))
print('dgram1=', len(s.recvfrom(4096)[0]))
print('dgram2=', len(s.recvfrom(4096)[0]))
" & ) && sleep 0.2 && python3 -c "
import socket
s = socket.socket(type=socket.SOCK_DGRAM)
s.sendto(b'A'*50, ('127.0.0.1',9002))
s.sendto(b'B'*50, ('127.0.0.1',9002))
"
# 결과: TCP 는 read1 = 100, UDP 는 dgram1=50 dgram2=50
```

화이트보드에서 강조: TCP 쪽 "한 번에 100B 가 왔다" 가 Scene 8 의 `read(100B) 가능` 을 **코드로 증명** 한다.

---

## Scene 9. listen socket, accept socket, ephemeral port

이 장면은 서버 쪽 구조를 그린다.

```text
listen socket
  0.0.0.0:80

accept 후 connected socket
  (208.216.181.15:80) <-> (128.2.194.242:51213)
```

핵심 설명:

- listen socket 은 "받을 준비가 된 문"이다.
- accept socket 은 특정 클라이언트와 연결된 실제 대화 채널이다.
- 서버 포트는 계속 80 이다.
- 연결 구분은 클라이언트의 ephemeral port 를 포함한 4-tuple 이 한다.

임시 포트도 같이 설명한다.

```text
client port 51213
 -> 커널이 ephemeral port range 에서 할당
```

방화벽 질문이 나오면 이렇게 답한다.

`클라이언트는 자기 쪽에서 바깥으로 연결을 열었기 때문에, 상태 기반 방화벽은 그 연결에 대응하는 응답 패킷을 같은 세션의 반환 트래픽으로 허용합니다.`

꼭 짚을 오해:

- accept socket 이라 해서 서버 포트가 매번 바뀌지 않는다.
- 클라이언트가 서버의 모든 내부 소켓 포트를 알아야 하는 것도 아니다.

### 직접 검증 — listen fd 와 accept fd 가 "다른 소켓" 임을 보이기

```bash
# (1) 서버 포트 범위 확인
cat /proc/sys/net/ipv4/ip_local_port_range
# 32768  60999     <- 여기서 ephemeral 이 뽑힌다

# (2) 서버 프로세스의 fd 테이블 관찰
python3 -m http.server 8080 >/dev/null &
SRV=$!
ls -l /proc/$SRV/fd | grep socket
# lrwx... 3 -> socket:[123]    <- listen socket

# (3) 동시 접속 3개를 열고 다시 본다
for i in 1 2 3; do (curl -s http://127.0.0.1:8080/ >/dev/null & ) ; done ; sleep 0.2
ls -l /proc/$SRV/fd | grep socket
# lrwx... 3 -> socket:[123]    <- listen socket (항상 존재)
# lrwx... 4 -> socket:[456]    <- accept socket #1
# lrwx... 5 -> socket:[457]    <- accept socket #2
# lrwx... 6 -> socket:[458]    <- accept socket #3

# (4) ss 로 4-tuple 확인
ss -tanp '( sport = :8080 )'
# LISTEN 0 5 *:8080         *:*
# ESTAB  0 0 127.0.0.1:8080 127.0.0.1:51213
# ESTAB  0 0 127.0.0.1:8080 127.0.0.1:51214
# ESTAB  0 0 127.0.0.1:8080 127.0.0.1:51215
# ^-- 서버 포트는 계속 8080, 클라 포트만 ephemeral 로 다름
```

화이트보드에서 강조: listen socket 은 계속 한 개(fd 3), accept 는 연결 수만큼 늘어난다. 서버 포트는 고정 80/8080. 구분은 클라의 ephemeral port.

---

## Scene 10. close, FIN, timeout, 인터넷이 갑자기 끊기면

마지막 장면은 종료와 예외 처리다.

```text
정상 종료
FIN  ------------>
     <----------- ACK
     <----------- FIN
ACK  ------------>
```

핵심 설명:

- 종료는 보통 시작 handshake 와 다르게 FIN/ACK 흐름으로 본다.
- 한쪽이 close 했다고 해서 즉시 양방향이 동시에 끝나는 것은 아니다.
- 네트워크가 갑자기 끊기면 정석적인 종료 없이 timeout, retransmission, RST 등으로 상태가 정리될 수 있다.

꼭 말할 문장:

`connect와 close는 둘 다 TCP 상태 머신 위에서 동작합니다. 연결 성립과 종료는 모두 커널이 상태를 기억하며 관리합니다.`

### 직접 검증 — FIN / TIME_WAIT / RST 관찰

```bash
# (1) 정상 close 시 FIN 캡처
python3 -m http.server 8080 >/dev/null &
sudo tcpdump -i lo -nn -S 'tcp port 8080 and (tcp[tcpflags] & tcp-fin != 0)' -c 4 &
sleep 0.3
curl -s http://127.0.0.1:8080/ -o /dev/null
# IP ...: Flags [F.], seq X, ack Y    <- client -> server
# IP ...: Flags [F.], seq Y, ack X+1  <- server -> client

# (2) TIME_WAIT 상태 확인
ss -tan state time-wait '( sport = :8080 or dport = :8080 )'

# (3) 강제 종료 시 RST 실험
python3 - <<'PY' &
import socket, os
s = socket.socket(); s.connect(('127.0.0.1',8080))
# linger 0 -> close 가 FIN 대신 RST 를 보내게 함
import struct; s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii',1,0))
s.close()
PY
sudo tcpdump -i lo -nn -c 2 'tcp port 8080 and tcp[tcpflags] & tcp-rst != 0'
# Flags [R.], seq ..., ack ...  <- "그냥 끊어진" 경우
```

화이트보드에서 강조: FIN 은 양방향이 따로 닫힌다 (active/passive close), RST 는 커널이 강제로 정리한다. `ss state time-wait` 가 남아 있는 게 TCP 상태 머신이 "살아 있다" 는 증거.

---

## 발표 10분 압축 버전

```text
1. 도메인 이름과 IP:port 분리
2. MAC / IP / Port 세 층
3. IPv4 / prefix / NAT 감각
4. htons 와 byte order
5. DNS resolve
6. getaddrinfo -> socket -> connect
7. SYN / SYN-ACK / ACK
8. TCP stream vs UDP datagram
9. listen / accept / ephemeral port
10. FIN / timeout / failure
```

## 질문 받으면 어디까지 내려갈지

- `왜 htons 가 필요한가요?`
  - Scene 4 로 내려간다
  - `0x0050 -> [0x50,0x00] -> 20480` 사례를 다시 적는다

- `DNS 서버 주소는 누가 알려주나요?`
  - Scene 5 로 내려간다
  - `/etc/resolv.conf`, systemd-resolved, ISP resolver 를 설명한다

- `getaddrinfo 와 inet_pton 차이가 뭐죠?`
  - Scene 4 와 Scene 6 을 같이 본다
  - `inet_pton = 문자열->주소`, `getaddrinfo = 이름/service -> 후보 리스트`

- `왜 TCP 는 stream 이고 UDP 는 datagram 인가요?`
  - Scene 8 로 내려간다

- `listen socket 과 accept socket 이 왜 따로 있나요?`
  - Scene 9 로 내려간다

- `인터넷이 갑자기 끊기면 close 는 어떻게 되나요?`
  - Scene 10 으로 내려간다

## 발표 중 한 화면에 띄울 검증 치트시트

```bash
# 이름 -> 주소
dig +trace <host> ; cat /etc/resolv.conf ; getent hosts <host>

# 바이트 배열
python3 -c "import socket,struct;print(hex(socket.htons(80)));print(socket.inet_aton('1.2.3.4').hex())"

# 시스템콜 순서
strace -e trace=socket,connect,sendto,recvfrom,close curl -s http://<host>/

# 핸드셰이크 / stream / close
sudo tcpdump -i lo -nn -S 'tcp port 8080' -c 6
ss -tanpi '( sport = :8080 or dport = :8080 )'
ss -tan state time-wait

# 4-tuple 과 listen/accept
ss -tanp '( sport = :8080 )'
ls -l /proc/$(pgrep -n python3)/fd | grep socket
```

## 연결 문서

- `q02-ip-address-byte-order.md`
- `q03-dns-domain-cloudflare.md`
- `q06-ch11-4-sockets-interface.md`
- `q07-tcp-udp-socket-syscall.md`
