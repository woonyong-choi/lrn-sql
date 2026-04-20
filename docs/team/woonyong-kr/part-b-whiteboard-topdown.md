# Part B. 프로토콜 스택 (주소·연결·Handshake) — 화이트보드 탑다운 발표안

Part B 발표를 "이름으로 상대를 찾고, TCP 연결을 맺고, 스트림으로 데이터를 주고받는다"는 한 줄로 설명하기 위한 문서입니다.
핵심은 주소 체계, DNS, byte order, handshake, stream 을 따로따로가 아니라 한 흐름으로 연결하는 것입니다.

## 발표 목표

- 사람의 이름 같은 도메인이 어떻게 네트워크 주소가 되는지 설명한다.
- MAC / IP / Port 가 각각 무엇을 식별하는지 분리해서 설명한다.
- `getaddrinfo -> socket -> connect` 와 TCP 3-way handshake 를 한 번의 흐름으로 묶는다.
- stream, close, ephemeral port, listen/accept socket 의 오해를 정리한다.

## 발표 한 줄 앵커

```text
도메인을 주소로 바꾸고
주소로 연결을 열고
연결 위에서 순서 보장 바이트 스트림을 주고받는다.
```

## 화이트보드 첫 장 구성

```text
+--------------------------------------------------------------------------------+
| 상단: "www.google.com -> 142.250.206.68:80 -> TCP stream"                      |
+--------------------------------------+-----------------------------------------+
| 왼쪽: 주소 3종                        | 오른쪽: connect 와 handshake            |
| MAC / IP / Port                      | SYN -> SYN/ACK -> ACK                  |
| DNS resolve                          | seq / ack / ephemeral port             |
+--------------------------------------+-----------------------------------------+
| 하단: 용어 사전                                                              |
| DNS / big-endian / htons / 4-tuple / ISN / stream / FIN / ephemeral port     |
+--------------------------------------------------------------------------------+
```

## 숫자 예시를 고정해 두기

```text
Domain           www.google.com
Resolved IP      142.250.206.68
Client IP        192.168.0.10
Client port      51732
Server port      80
Client ISN       1000
Server ISN       9000
```

## 장면 순서

## Scene 1. 사람은 이름을, 컴퓨터는 주소를 본다

칠판에 먼저 그릴 것:

```text
www.google.com
      |
      v
142.250.206.68:80
```

핵심 설명:

- 사람은 도메인을 기억한다.
- 실제 통신은 IP 와 port 를 기준으로 일어난다.
- 그래서 첫 단계는 "이름을 주소로 바꾸는 것"이다.

다음 장면 연결 문장:

`그런데 주소도 하나가 아니라 세 층으로 나뉩니다.`

## Scene 2. 주소는 세 종류다

칠판에 추가할 것:

```text
MAC   = 옆 기계
IP    = 호스트
Port  = 프로세스
```

숫자 예시:

```text
Client MAC   aa:aa:aa:aa:aa:aa
Client IP    192.168.0.10
Client Port  51732

Server IP    142.250.206.68
Server Port  80
```

핵심 설명:

- MAC 은 링크 계층에서 바로 옆 hop 을 식별한다.
- IP 는 네트워크 전체에서 어느 호스트인지를 식별한다.
- Port 는 그 호스트 안에서 어느 프로세스/소켓인지를 식별한다.

꼭 짚을 오해:

- `IP 하나면 다 끝`이 아니다.
- 같은 서버 IP 라도 80, 443, 3306 은 전혀 다른 서비스다.

## Scene 3. byte order 와 문자열 변환

칠판에 추가할 것:

```text
80 decimal = 0x0050
host order    little-endian
network order big-endian

htons(80) 필요
inet_pton("142.250.206.68") 필요
```

핵심 설명:

- CPU 는 little-endian 인 경우가 많지만 네트워크는 big-endian 을 사용한다.
- 그래서 port, IP 를 구조체에 넣을 때 `htons`, `htonl` 이 필요하다.
- `inet_pton` 은 문자열 IP 를 바이너리 주소로 바꾼다.

다음 장면 연결 문장:

`이제 문자열을 주소로 바꾸는 도구와 이름을 주소로 찾는 도구를 구분하겠습니다.`

## Scene 4. DNS resolve

칠판에 추가할 것:

```text
Client
  |
  v
Resolver -> Root -> .com -> google.com NS -> A record
                                         -> 142.250.206.68
```

핵심 설명:

- DNS 는 분산 계층 구조를 가진다.
- 클라이언트는 보통 로컬 리졸버에게 묻고, 리졸버가 재귀 조회를 한다.
- 결과는 `도메인 -> IP` 매핑이다.

꼭 짚을 오해:

- 매 요청마다 인터넷 끝까지 DNS 를 다시 순회하는 것은 아니다.
- 캐시가 있어서 반복 조회 비용을 줄인다.

## Scene 5. getaddrinfo -> socket -> connect

칠판에 추가할 것:

```text
getaddrinfo("www.google.com", "80")
        |
        v
  sockaddr list
        |
        v
socket(AF_INET, SOCK_STREAM, 0)
        |
        v
connect(...)
```

핵심 설명:

- `getaddrinfo` 는 이름을 주소 후보 리스트로 바꾸는 고수준 인터페이스다.
- `socket` 은 아직 연결이 아니라 "통신 엔드포인트 객체 생성"이다.
- `connect` 가 실제 연결 성립 절차를 시작한다.

## Scene 6. TCP 3-way handshake

칠판에 크게 그릴 것:

```text
Client 192.168.0.10:51732                  Server 142.250.206.68:80

SYN   seq=1000   ---------------------->
                     <------------------  SYN,ACK seq=9000 ack=1001
ACK   seq=1001 ack=9001  --------------->
```

핵심 설명:

- SYN 은 "연결을 열고 싶다 + 내 초기 sequence 는 이것이다"라는 뜻이다.
- SYN/ACK 는 "좋다 + 네 sequence 는 확인했다 + 내 sequence 는 이것이다"라는 뜻이다.
- 마지막 ACK 로 양쪽이 서로의 초기 상태를 맞춘다.

꼭 짚을 오해:

- handshake 는 단순한 인사말이 아니라 sequence space, 양방향 송수신 준비, 옵션 협상을 맞추는 절차다.
- 연결이 맺어진 뒤부터는 4-tuple 이 연결을 식별한다.

## Scene 7. stream 이라는 감각

칠판에 추가할 것:

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

핵심 설명:

- TCP 는 메시지 경계를 보존하지 않는다.
- TCP 가 보장하는 것은 "순서와 신뢰성"이지 "write 단위 그대로 read"가 아니다.
- UDP 와 비교하면 UDP 는 datagram 경계가 살아 있고, TCP 는 stream 이다.

꼭 짚을 오해:

- `두 번 보냈으니 두 번 읽혀야 한다`는 생각은 TCP 에서는 틀리다.

## Scene 8. listen socket 과 accept socket

칠판에 추가할 것:

```text
listen socket   0.0.0.0:80
accept socket   142.250.206.68:80 <-> 192.168.0.10:51732
```

핵심 설명:

- listen socket 은 "받아들일 준비가 된 문"이다.
- accept 후 생기는 connected socket 은 특정 클라이언트와 묶인 별도 객체다.
- 서버 포트 80 은 그대로 유지되고, 클라이언트 쪽 ephemeral port 가 연결을 구분하는 데 쓰인다.

꼭 짚을 오해:

- accept socket 이라 해서 서버 포트가 매번 새로 생기는 것이 아니다.
- 연결 구분은 `src IP, src port, dst IP, dst port` 조합으로 한다.

## Scene 9. close, FIN, 그리고 인터넷이 끊기면

칠판에 추가할 것:

```text
FIN -> ACK
FIN <- ACK
```

핵심 설명:

- 종료는 보통 3-way 가 아니라 FIN/ACK 를 포함한 4-step 에 가깝다.
- 한쪽이 write 를 끝냈다는 사실과 양방향 종료는 분리해서 봐야 한다.
- 인터넷이 갑자기 끊기면 정석적인 종료 handshake 없이 timeout, retransmission, reset 처리로 넘어갈 수 있다.

꼭 짚을 오해:

- `close 하면 항상 예쁘게 종료 패킷이 왕복한다`는 보장은 없다.
- 네트워크 장애가 나면 커널은 timeout 과 재전송 정책으로 상태를 정리한다.

## 발표 10분 버전 압축 순서

```text
1. 이름과 주소 구분
2. MAC / IP / Port 세 층
3. htons / inet_pton
4. DNS resolve
5. getaddrinfo / socket / connect
6. SYN / SYN-ACK / ACK
7. TCP stream 감각
8. listen socket vs accept socket
9. FIN / close / timeout
```

## 질문 받으면 확장할 위치

- `왜 htons 가 필요한가?` -> Scene 3
- `DNS 서버 주소는 어떻게 아나?` -> Scene 4
- `getaddrinfo 와 inet_pton 차이?` -> Scene 3, Scene 5
- `임시 포트가 방화벽에 막히면?` -> Scene 8, Scene 9
- `왜 TCP 는 stream 이고 UDP 는 datagram 인가?` -> Scene 7

## 연결 문서

- `q02-ip-address-byte-order.md`
- `q03-dns-domain-cloudflare.md`
- `q06-ch11-4-sockets-interface.md`
- `q07-tcp-udp-socket-syscall.md`
- `docs/question/q12-socket-connection-lifecycle.md`
