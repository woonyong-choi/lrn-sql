# Part A. 네트워크 하드웨어 & 커널 송신 경로 — 화이트보드 탑다운 발표안

Part A 발표를 화이트보드 한 장의 흐름으로 끝까지 설명하기 위한 문서입니다.
핵심은 "유저가 `write(sockfd, data)` 한 번 호출했을 때 데이터가 어떻게 선로로 나가는가"를 끊기지 않는 한 줄로 설명하는 것입니다.

## 발표 목표

- 하드웨어, 커널, 프로토콜이 따로 노는 개념이 아니라 하나의 송신 경로라는 점을 보여준다.
- `write()` 호출이 `copy_from_user -> socket buffer -> TCP/IP -> driver -> DMA -> NIC -> Ethernet`으로 이어지는 이유를 설명한다.
- 라우팅, TTL, MAC 재포장, DMA, PCIe를 하나의 경로 위에 올려서 설명한다.

## 발표 한 줄 앵커

```text
앱이 write() 한 번 호출하면
그 바이트는 커널 안에서 포장되고 큐잉되고 DMA 로 NIC 에 실려
Ethernet frame 이 되어 다음 hop 으로 나간다.
```

## 화이트보드 첫 장 구성

```text
+--------------------------------------------------------------------------------+
| 상단: 오늘의 한 줄                                                             |
| "write(sockfd, GET / HTTP/1.1) -> wire"                                        |
+--------------------------------------+-----------------------------------------+
| 왼쪽: 유저 -> 커널 -> TCP/IP -> NIC   | 오른쪽: 실제 숫자 예시                  |
|                                      | src IP   192.168.0.10                  |
| [App] -> [syscall] -> [socket] ->    | src Port 51732                         |
| [TCP] -> [IP] -> [Eth] -> [Driver]   | dst IP   142.250.206.68               |
| -> [DMA] -> [NIC] -> [Wire]          | dst Port 80                            |
|                                      | payload  95B                           |
+--------------------------------------+-----------------------------------------+
| 하단: 꼭 남길 용어 사전                                                     |
| MAC / IP / Port / sk_buff / DMA / qdisc / NIC / next hop / TTL               |
+--------------------------------------------------------------------------------+
```

## 숫자 예시를 고정해 두기

발표 내내 아래 숫자를 계속 재사용하면 흐름이 안 끊깁니다.

```text
Client host      192.168.0.10
Gateway          192.168.0.1
Server host      142.250.206.68
Client port      51732
Server port      80
HTTP payload     95B
TCP header       20B
IPv4 header      20B
Ethernet header  14B
FCS              4B

On-wire frame size = 95 + 20 + 20 + 14 + 4 = 153B
```

## 장면 순서

## Scene 1. 문제를 한 줄로 잡기

칠판에 먼저 그릴 것:

```text
[ user process ]
    |
    | write(sockfd, "GET / HTTP/1.1...", 95)
    v
  ??????
    v
[ Ethernet wire ]
```

이때 할 말:

`우리는 지금 이 물음표 안을 여는 겁니다. 네트워크 공부가 어렵게 느껴지는 이유는 이 물음표 안에 OS, TCP/IP, 하드웨어가 한꺼번에 섞여 있기 때문입니다.`

다음 장면 연결 문장:

`먼저 user 에 있던 데이터가 왜 kernel 로 넘어가야 하는지부터 보겠습니다.`

## Scene 2. user space -> kernel space 경계

칠판에 추가할 것:

```text
[ user buf ] --copy_from_user--> [ kernel socket send buffer ]
         CPL3                      CPL0
```

핵심 설명:

- 유저 메모리는 프로세스가 마음대로 바꿀 수 있으므로 NIC 가 직접 참조하게 두지 않는다.
- 커널은 시스템콜 진입 후 유저 버퍼를 검증하고 커널 쪽 버퍼로 복사한다.
- 그래서 `write()` 직후 유저 버퍼를 바꿔도 이미 보낸 데이터가 즉시 망가지지 않는다.

꼭 짚을 오해:

- `소켓 버퍼`는 유저 공간에 있는 것이 아니라 커널 내부의 소켓 자료구조가 관리한다.
- `버퍼`는 "임시 저장 공간"이고, 송신/재전송/흐름 제어를 위해 필요하다.

## Scene 3. fd 하나가 커널의 어디를 가리키는가

칠판에 추가할 것:

```text
fd 4
 |
 v
[ file ] -> [ socket ] -> [ sock / tcp_sock ]
```

핵심 설명:

- 유저는 정수 `fd`만 본다.
- 커널은 `fd -> file -> socket -> sock` 순으로 실제 객체를 찾는다.
- 여기서 `sock` 계층이 TCP 상태, 버퍼, 큐, sequence number 같은 실제 통신 상태를 들고 있다.

다음 장면 연결 문장:

`이제 커널은 이 데이터를 실제 TCP 패킷으로 포장해야 합니다.`

## Scene 4. TCP 계층에서 무슨 일이 일어나는가

칠판에 추가할 것:

```text
payload 95B
   |
   + TCP header 20B
   v
[ TCP segment 115B ]
```

핵심 설명:

- TCP 는 바이트 스트림을 세그먼트로 잘라 보낸다.
- 헤더에는 src/dst port, sequence number, ACK, window, flags 같은 정보가 붙는다.
- 커널은 이 세그먼트를 담기 위해 `sk_buff`를 만들고 송신 큐에 올린다.

숫자 예시:

```text
src port = 51732
dst port = 80
seq      = 1000
payload  = 95B
next seq = 1095
```

꼭 짚을 오해:

- `write()` 한 번이 반드시 패킷 한 개가 되는 것은 아니다.
- TCP 는 stream 이므로 여러 write 가 합쳐질 수도 있고, 하나가 여러 segment 로 쪼개질 수도 있다.

## Scene 5. IP 계층에서 라우팅과 TTL

칠판에 추가할 것:

```text
[ IP header ]
src = 192.168.0.10
dst = 142.250.206.68
ttl = 64
proto = TCP
```

핵심 설명:

- IP 계층은 "어느 호스트로 가야 하는가"를 담당한다.
- 라우팅 테이블을 보고 다음 hop 을 결정한다.
- 목적지가 같은 LAN 밖에 있으면 실제 목적지는 서버지만, 이번 프레임의 Ethernet 목적지는 gateway MAC 이 된다.
- TTL 은 hop 을 지날 때마다 1씩 감소하고, 무한 루프를 막는다.

화이트보드에 반드시 같이 그릴 것:

```text
Server IP       142.250.206.68
Next hop IP     192.168.0.1
```

다음 장면 연결 문장:

`IP 가 다음 hop 을 정했으면, 이제 정말 전선에 싣기 위한 링크 계층 포장을 해야 합니다.`

## Scene 6. Ethernet header 와 MAC 재포장

칠판에 추가할 것:

```text
[ Ethernet ]
dst MAC = gateway MAC
src MAC = my NIC MAC
type    = IPv4
payload = IP packet
```

핵심 설명:

- MAC 은 "바로 옆 기계"를 식별한다.
- IP 는 끝 목적지를 유지하지만, MAC 은 hop 마다 바뀐다.
- 라우터를 하나 지날 때마다 Ethernet header 는 새로 만들어진다.

꼭 짚을 오해:

- `dst IP` 와 `dst MAC` 은 보통 같은 대상이 아니다.
- 같은 LAN 안이면 서버 MAC 이 들어가고, 다른 LAN 이면 gateway MAC 이 들어간다.

## Scene 7. qdisc -> driver -> DMA

칠판에 추가할 것:

```text
sk_buff
  |
  v
[ qdisc ]
  |
  v
[ NIC driver ]
  |
  v
DMA descriptor -> NIC TX ring
```

핵심 설명:

- qdisc 는 송신 큐잉과 스케줄링 지점이다.
- 드라이버는 `sk_buff` 정보를 NIC 가 이해할 수 있는 descriptor 로 바꾼다.
- DMA 는 CPU 가 바이트를 하나하나 복사하지 않고, 장치가 메모리에서 직접 읽어 가게 하는 메커니즘이다.

이때 강조할 문장:

`CPU 는 "이 버퍼를 저기서 읽어 가"라고 지시하고, 실제 대량 전송은 DMA 엔진과 NIC 가 맡습니다.`

## Scene 8. PCIe 와 NIC, 그리고 선로

칠판에 추가할 것:

```text
[ DRAM ] <=PCIe=> [ NIC ]
                   |
                   v
              Ethernet bits on wire
```

핵심 설명:

- 현대 시스템에서는 NIC 가 PCIe 장치로 연결된다.
- NIC 는 DMA 로 DRAM 의 버퍼를 읽고, 헤더와 payload 를 실제 비트 스트림으로 직렬화한다.
- 전송 완료 후 interrupt 또는 polling 기반으로 완료 처리가 일어난다.

꼭 짚을 오해:

- wire 에 나가는 것은 "TCP 패킷" 그 자체가 아니라 결국 Ethernet frame 비트열이다.
- 커널이 직접 전선을 만지는 것이 아니라 드라이버와 NIC 가 마지막 단을 수행한다.

## Scene 9. Echo / EOF 로 마무리

마지막에 되돌아와서 칠판에 작은 루프를 추가할 것:

```text
send -> wire -> peer recv -> peer write back -> recv -> EOF/close
```

핵심 설명:

- 에코 서버는 이 경로가 양방향으로 대칭이라는 점을 보여주는 가장 좋은 예시다.
- 파일 I/O 와 네트워크 I/O 가 닮았다는 말은 `read/write/close` 추상화가 같다는 뜻이다.
- EOF, FIN, close 를 이해하면 "전송"만이 아니라 "언제 끝났는가"까지 설명할 수 있다.

## 발표 10분 버전 압축 순서

```text
1. write() 호출
2. user -> kernel 복사
3. fd -> socket -> tcp_sock
4. TCP header 와 sk_buff
5. IP route 와 TTL
6. Ethernet MAC 재포장
7. qdisc / driver / DMA
8. NIC / PCIe / wire
9. echo / EOF 로 왕복 대칭 정리
```

## 질문 받으면 확장할 위치

- `소켓 버퍼는 어디 있나?` -> Scene 2, Scene 3
- `TCP/IP 스택은 커널 어디서 동작하나?` -> Scene 3, Scene 4, Scene 5
- `DMA 는 정확히 뭐가 직접 읽는가?` -> Scene 7, Scene 8
- `라우터에서 MAC 이 왜 바뀌나?` -> Scene 5, Scene 6
- `EOF 는 왜 중요한가?` -> Scene 9

## 연결 문서

- `q01-network-hardware.md`
- `q08-host-network-pipeline.md`
- `q10-io-bridge.md`
- `q14-echo-server-datagram-eof.md`
- `99-whiteboard-session.md`
