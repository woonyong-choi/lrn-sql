# Q01. 네트워크 하드웨어 — Ethernet / Bridge / Router / LAN / WAN

> CSAPP 11.2 | 네트워크의 물리/링크 계층 | 기본

## 질문

1. 이더넷, 브릿지, 라우터는 무엇이 다른가. 이더넷과 브릿지는 대역폭만 다른가?
2. LAN과 WAN은 무엇이 다른가.
3. 호스트들은 결국 비트를 주고받는다고 하는데, 그 "비트 뭉치"는 어떤 형태이며 어떻게 수신자가 자기 데이터라는 것을 아는가.

## 답변

### 최우녕

> 이더넷, 브릿지, 라우터는 무엇이 다른가. 이더넷과 브릿지는 대역폭만 다른가?

이 세 가지는 "대역폭이 다른 같은 것"이 아니라, **연결의 범위와 연결 단위**가 다르다.

이더넷(Ethernet segment)은 가장 기본 단위다. 여러 호스트가 허브(hub)라는 작은 장치에 꽂혀 있고, 허브는 어떤 포트에서 들어온 비트를 **다른 모든 포트에 그대로 복사**해서 내보낸다. 즉 허브는 지능이 없고, 한 호스트가 말하면 같은 세그먼트의 모든 호스트가 동시에 듣는다(그래서 대역폭을 나눠 쓴다). 이 구간 안에서 프레임을 주고받는 단위는 MAC 주소(48-bit)다.

브릿지(bridged Ethernet)는 여러 이더넷 세그먼트를 묶어 **더 큰 하나의 LAN**으로 만드는 장치다. 브릿지는 "학습"을 한다. 각 포트 뒤에 어떤 MAC 주소가 사는지 테이블로 기억해서, A->B로 가는 프레임이 왔을 때 **B가 있는 쪽 포트로만** 전달하고 나머지 쪽에는 내보내지 않는다. 덕분에 서로 다른 세그먼트끼리 트래픽 간섭이 줄고, 전체 대역폭이 허브에 비해 훨씬 커진다. 하지만 브릿지로 묶인 결과물은 여전히 하나의 "링크 계층(L2) 네트워크"이고, 모두 같은 IP 서브넷, 같은 브로드캐스트 도메인 안에 있다.

라우터(router)는 여기서부터 계층이 바뀐다. 라우터는 **서로 다른 LAN/WAN을 이어주는 장치**이며, L2(MAC)가 아니라 L3(IP) 수준에서 판단한다. 들어온 프레임의 IP 헤더를 보고 "이 패킷이 어느 네트워크로 가야 하는지"를 라우팅 테이블로 찾아서 다음 홉으로 내보낸다. 이때 **IP 주소는 유지되지만 MAC 주소는 홉마다 새로 바뀐다.**

정리하면 (범위가 넓어지는 순):

```text
Ethernet segment   (허브 하나, 물리적으로 같은 선)
   │
   v  여러 세그먼트 묶음
Bridged Ethernet   (= LAN, 같은 IP 서브넷)
   │
   v  서로 다른 LAN/WAN 묶음
Internet           (= LAN + WAN + Router, IP로 이어진 이기종 네트워크)
```

"이더넷과 브릿지는 대역폭만 다르다"는 틀린 표현이다. 브릿지는 **여러 이더넷을 묶어 하나의 LAN으로 만드는 도구**이고, 이더넷은 그 LAN을 구성하는 기본 조각이다.

> LAN과 WAN은 무엇이 다른가.

LAN(Local Area Network)은 지리적으로 작은 범위(보통 건물/캠퍼스)에서 한 조직이 관리하는 네트워크다. 동일한 링크 계층 기술(Ethernet, Wi-Fi)로 묶여 있고, 내부 호스트는 같은 IP 서브넷을 공유한다.

WAN(Wide Area Network)은 도시, 국가, 대륙 단위로 넓게 펼쳐진 네트워크다. 여러 사업자 장비가 얽혀 있고 링크 계층이 제각각이다(광, 위성, 셀룰러 등). WAN은 보통 **여러 LAN을 라우터로 이어 준다**.

정확히 말하면 LAN과 WAN은 "어떤 장비냐"가 아니라 "**범위와 운영 주체**"의 차이다. 공통점은 둘 다 IP 기반으로 라우터가 이어 준다는 것. 인터넷은 LAN과 WAN을 라우터로 계속 붙여 만든 전 세계 네트워크다.

> 호스트들은 결국 비트를 주고받는다고 하는데, 그 "비트 뭉치"는 어떤 형태이며 어떻게 수신자가 자기 데이터라는 것을 아는가.

비트 뭉치는 한 번에 하나의 덩어리(frame/packet)로 포장된다. 각 계층은 자기 **헤더**를 붙이고, 맨 뒤 또는 가운데에 **데이터(payload)** 를 놓는다. 수신자는 각 계층의 헤더에 쓰여 있는 주소와 길이, 타입 정보를 보고 자기 것인지 판단한다.

실제로 이더넷 프레임 하나를 최소 수치로 풀면 이렇다.

```text
[ 프레임 전체 (Ethernet frame) = ~67B 예시 ]

┌──────────────────────── Ethernet Header (14B) ─────────────────────┐
│ 목적지 MAC 6B  │ 출발지 MAC 6B  │ EtherType 2B (0x0800 = IPv4)     │
└────────────────────────────────────────────────────────────────────┘
┌──────────────────────── IP Header (20B) ───────────────────────────┐
│ version/len │ total-length 2B │ TTL │ proto=6(TCP) │ src IP │ dst IP │
└────────────────────────────────────────────────────────────────────┘
┌──────────────────────── TCP Header (20B) ──────────────────────────┐
│ src port 2B │ dst port 2B │ seq 4B │ ack 4B │ flags │ window ...   │
└────────────────────────────────────────────────────────────────────┘
┌──────────────────────── Payload (N B) ─────────────────────────────┐
│  "GET /home.html HTTP/1.0\r\n..."   <- 실제 데이터                  │
└────────────────────────────────────────────────────────────────────┘
                                       + Ethernet FCS 4B (CRC)
```

### 각 필드 상세 해부

위 박스를 "몇 번째 바이트에 무엇이 있는지" 완전히 풀어서 본다.

#### ① Ethernet Header (14B) — 링크 계층

```
오프셋   0             6            12   14
        ┌─────────────┬─────────────┬────┐
        │ Dst MAC 6B  │ Src MAC 6B  │Type│
        └─────────────┴─────────────┴────┘
```

**MAC 주소 (6B씩)** — 48비트를 16진수 2자리씩 `:`로 구분해서 표기한다.

```
예)  00:1A:2B:3C:4D:5E
     └──OUI──┘ └──NIC──┘
     3B 제조사  3B 디바이스
```

- **앞 3B (OUI)** — IEEE가 제조사에 할당. Apple, Intel, Cisco 등을 식별.
- **뒤 3B** — 제조사가 자기 NIC마다 부여하는 고유번호.
- **첫 바이트의 최하위 비트(I/G bit)**: `0` = Unicast, `1` = Multicast.
- **첫 바이트의 2번째 최하위 비트(U/L bit)**: `0` = Globally unique(공장 출하값), `1` = Locally administered(VM/컨테이너가 자주 사용).

**특수 MAC 주소**

| 주소 | 용도 |
| --- | --- |
| `FF:FF:FF:FF:FF:FF` | Broadcast — 같은 LAN의 모두에게 |
| `01:00:5E:xx:xx:xx` | IPv4 Multicast |
| `33:33:xx:xx:xx:xx` | IPv6 Multicast |
| `01:80:C2:xx:xx:xx` | Bridge/Switch 제어(STP 등) |

**EtherType (2B)** — 상위 계층이 누구인지 알려주는 "다음은 누구 헤더" 플래그. 2바이트 빅엔디언.

| 값 | 의미 |
| --- | --- |
| `0x0800` | IPv4 |
| `0x86DD` | IPv6 |
| `0x0806` | ARP (MAC 찾기) |
| `0x8100` | VLAN Tag (802.1Q) — 있으면 뒤에 4B가 더 붙고 실제 EtherType은 그 뒤 |
| `0x8847` | MPLS |
| `0x88CC` | LLDP (장비 발견) |

> 즉 14B = 6(목적지) + 6(출발지) + 2(타입)로 확정.

#### ② IP Header (20B) — RFC 791 표준

RFC 비트맵으로 보면 한 줄이 32비트 = 4바이트. 총 5줄 = 20바이트.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Ver|IHL|   TOS   |         Total Length (2B)                   |  <- 0~3B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Identification (2B)     |Flags|   Fragment Offset       |  <- 4~7B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   TTL (1B)   | Protocol (1B) |     Header Checksum (2B)       |  <- 8~11B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Source IP Address (4B)                     |  <- 12~15B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Destination IP Address (4B)                   |  <- 16~19B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 오프셋 | 크기 | 필드 | 의미 |
| --- | --- | --- | --- |
| 0 | 4비트 | **Version** | 4 = IPv4, 6 = IPv6 |
| 0.5 | 4비트 | **IHL** (Internet Header Length) | 헤더 길이를 4바이트 단위로 표현. 옵션 없으면 `5` -> 20B. 합치면 첫 바이트가 `0x45` |
| 1 | 1B | **TOS / DSCP + ECN** | 서비스 품질(우선순위, 혼잡 표시 ECN) |
| 2 | 2B | **Total Length** | IP 헤더 + 페이로드 전체 길이(바이트). 최대 65535B |
| 4 | 2B | **Identification** | 패킷 ID. 프래그먼트 재조합용 같은 ID끼리 묶음 |
| 6 | 3비트 | **Flags** | `DF`(Don't Fragment), `MF`(More Fragments) |
| 6.375 | 13비트 | **Fragment Offset** | 분할된 경우 원본 중 내가 몇 번째 바이트부터인지 |
| 8 | 1B | **TTL** | 라우터 하나 지날 때마다 **-1**. 0 되면 drop하고 ICMP 에러 반환. 초기값: Linux 64, Windows 128 |
| 9 | 1B | **Protocol** | 상위 계층 식별자(아래 표 참조) |
| 10 | 2B | **Header Checksum** | IP 헤더만의 체크섬. TTL이 홉마다 바뀌므로 라우터가 **재계산해서 덮어씀** |
| 12 | 4B | **Source IP** | 보내는 호스트 IP |
| 16 | 4B | **Destination IP** | 받는 호스트 IP |

**Protocol 주요 값**

| 값 | 프로토콜 |
| --- | --- |
| 1 | ICMP (ping) |
| 2 | IGMP (멀티캐스트 그룹 관리) |
| 6 | **TCP** |
| 17 | **UDP** |
| 41 | IPv6-in-IPv4 (터널링) |
| 47 | GRE (터널링) |
| 50 | ESP (IPsec) |
| 89 | OSPF (라우팅 프로토콜) |

**`Version/IHL` 1바이트 풀어쓰기**

```
첫 바이트 0x45 = 0100 0101
                ───── ─────
                Ver=4 IHL=5 -> 5 × 4B = 20B 헤더
```

#### ③ TCP Header (20B) — RFC 793 표준

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Src Port (2B)         |         Dst Port (2B)         |  <- 0~3B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Sequence Number (4B)                        |  <- 4~7B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Acknowledgment Number (4B)                    |  <- 8~11B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Offset|Rsv| Flags |        Window Size (2B)                    |  <- 12~15B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Checksum (2B)         |      Urgent Pointer (2B)      |  <- 16~19B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 오프셋 | 크기 | 필드 | 의미 |
| --- | --- | --- | --- |
| 0 | 2B | **Source Port** | 송신 프로세스 포트(1~65535). 클라이언트는 보통 커널이 할당하는 임시 포트(ephemeral, 32768~60999) |
| 2 | 2B | **Destination Port** | 수신 프로세스 포트. 서버는 80, 443, 22 등 well-known |
| 4 | 4B | **Sequence Number** | 이 세그먼트 첫 바이트의 스트림 내 순번. 32비트라 4GB마다 wrap |
| 8 | 4B | **Acknowledgment Number** | "내가 다음에 받을 순번" — 여기까지는 잘 받았다는 의미 |
| 12 | 4비트 | **Data Offset** | TCP 헤더 길이(4B 단위). 보통 5 -> 20B, 옵션 있으면 더 큼 |
| 12.5 | 3비트 | **Reserved** | 예약(0) |
| 12.875 | 9비트 | **Flags** | 아래 상세 |
| 14 | 2B | **Window Size** | "내가 더 받을 수 있는 버퍼 바이트 수" — flow control의 핵심. `0`이면 Zero Window (잠시 멈춰달라) |
| 16 | 2B | **Checksum** | TCP 헤더 + 페이로드 + pseudo-header 체크섬 (end-to-end 검증) |
| 18 | 2B | **Urgent Pointer** | URG 플래그 켰을 때만 유효. 긴급 데이터 끝 위치 |

**Flags 9비트 전체**

| 비트 | 플래그 | 의미 |
| --- | --- | --- |
| 1 | NS | ECN nonce (거의 안 씀) |
| 2 | CWR | Congestion Window Reduced (혼잡 신호 수신 확인) |
| 3 | ECE | ECN-Echo (혼잡 발생 통보) |
| 4 | **URG** | Urgent Pointer 유효 |
| 5 | **ACK** | Acknowledgment 필드 유효. 핸드셰이크 후 모든 세그먼트에 ON |
| 6 | **PSH** | 버퍼에 쌓지 말고 즉시 상위 계층에 올려라 |
| 7 | **RST** | 강제 연결 종료(비정상) |
| 8 | **SYN** | 연결 시작(3-way handshake 첫 단계) |
| 9 | **FIN** | 정상 연결 종료 요청 |

**전형적 핸드셰이크와 플래그 흐름**

```
Client  SYN           ─────>           Server  SYN=1, seq=x
Client  <───          SYN+ACK                   SYN=1, ACK=1, seq=y, ack=x+1
Client  ACK           ─────>                    ACK=1, seq=x+1, ack=y+1
(연결 완료 -> 이후 모든 세그먼트 ACK=1)

[데이터 전송 중 에러]
        RST           ─────>           "연결 박살, 버려"

[정상 종료]
        FIN           ─────>           "내 쪽 다 보냄"
        <───          ACK
        <───          FIN              "나도 다 보냈음"
        ACK           ─────>
```

#### ④ Payload (N B) — 실제 데이터

상위 계층이 뭘 실었느냐에 따라 다르다.

```
예) HTTP 요청
    "GET /home.html HTTP/1.0\r\n"
    "Host: example.com\r\n"
    "\r\n"
```

- 길이 계산: **`IP Total Length − IP 헤더 − TCP 헤더 = Payload`**.
- 이더넷 프레임 최소는 64B(FCS 포함), 모자라면 Padding으로 채운다.
- 이더넷 MTU 1500B 기준, TCP MSS = 1500 − 20(IP) − 20(TCP) = **1460B**.

#### ⑤ Ethernet FCS (Frame Check Sequence) 4B

프레임의 앞 전체(dst MAC 부터 payload 끝까지)를 **CRC-32**로 돌려서 맨 끝에 붙이는 4바이트 체크섬.

**누가 만들고 누가 검사하나**

```
[송신 NIC 하드웨어]
   프레임 만듦 -> CRC-32 계산 -> 끝에 4B 붙여서 전송
                                     │
                                     v 케이블/Wi-Fi/광섬유 (비트 에러 가능)
                                     │
[수신 NIC 하드웨어]
   프레임 도착 -> 앞부분 CRC-32 다시 계산 -> 뒤에 붙은 FCS와 비교
       같음  -> OK, 커널로 올림
       다름  -> DROP (rx_crc_errors 카운터 +1, 커널/앱은 아예 모름)
```

- **커널 CPU가 아니라 NIC 칩이 하드웨어 수준에서** 계산/검증 -> 10G·100G 라인 속도에서도 공짜.
- `ethtool -S eth0`에서 `rx_crc_errors`로 물리 계층 장애를 감지할 수 있다.

**TCP Checksum과 어떻게 다른가**

| 항목 | Ethernet FCS | TCP Checksum |
| --- | --- | --- |
| 계층 | 링크 (L2) | 전송 (L4) |
| 범위 | **1홉** (나 <-> 바로 다음 스위치/라우터) | **end-to-end** (송신 프로세스 <-> 수신 프로세스) |
| 위치 | 프레임 **끝** 4B | TCP 헤더 안 2B |
| 계산 | NIC 하드웨어 (CRC-32) | 커널 SW 또는 NIC 오프로드 (1의 보수 합) |
| 홉마다 | 매 구간 **재계산** (MAC 주소 바뀌므로) | **변하지 않음** (IP src/dst 고정) |
| 검출력 | 강함 (CRC) | 약함 (간단한 합) |

**왜 둘 다 필요한가**

- **FCS**: 케이블 간섭, Wi-Fi 노이즈 같은 **물리적 비트 에러**를 잡는다.
- **TCP Checksum**: 라우터/스위치 내부 메모리 버그, 잘못된 NAT 동작 같은 **경로상 SW 에러**를 잡는다.

FCS로 지키지 못한 에러가 TCP Checksum에 걸리는 경우가 드물지만 실제로 있다.

#### ⑥ 실전 기본값·옵션 표 (헤더별 자주 쓰이는 값)

위 필드들을 실제 트래픽에서 "무슨 값이 흔히 들어가는지" 기준으로 묶은 표.

**Ethernet Header**

| 필드 | 크기 | 전형적인 값 | 다른 선택지 |
| --- | --- | --- | --- |
| Dst MAC | 6B | 다음 홉 MAC | Broadcast `FF:FF:..`, Multicast `01:00:5E:..` / `33:33:..` |
| Src MAC | 6B | 송신 NIC MAC | 가상 NIC은 locally-admin (U/L=1) |
| EtherType | 2B | `0x0800` IPv4 | `0x86DD`(IPv6), `0x0806`(ARP), `0x8100`(VLAN), `0x8847`(MPLS) |

**IP Header**

| 필드 | 크기 | 전형적인 값 | 범위/의미 |
| --- | --- | --- | --- |
| Version | 4비트 | `4` | 4 또는 6 |
| IHL | 4비트 | `5` -> 20B | 5~15 (최대 60B 헤더, 옵션 포함) |
| TOS/DSCP | 1B | `0x00` | DSCP 6비트 (AF11~EF 등 QoS), ECN 2비트 |
| Total Length | 2B | 40~1500 | 20~65535 |
| Identification | 2B | 단조증가 16비트 | 프래그먼트 재조합 키 |
| Flags (3비트) | — | `0b010` (DF=1) | DF=1이면 분할 금지, MF=1이면 뒤에 더 있음 |
| Fragment Offset | 13비트 | 0 | 8바이트 단위 오프셋 |
| TTL | 1B | **Linux=64**, **Windows=128**, **라우터=255** | 0~255. traceroute는 1,2,3…씩 늘림 |
| Protocol | 1B | 6(TCP) / 17(UDP) / 1(ICMP) | IANA 표 전체 |
| Header Checksum | 2B | 계산값 | 홉마다 재계산 |
| Src/Dst IP | 4B씩 | — | `0.0.0.0` 미지정, `127.0.0.1` loopback, `224.x` 멀티캐스트, `255.255.255.255` 브로드캐스트 |

**TCP Header**

| 필드 | 크기 | 전형적인 값 | 범위/의미 |
| --- | --- | --- | --- |
| Src Port | 2B | **Ephemeral 32768~60999** (Linux) | 1~1023 well-known, 1024~49151 registered |
| Dst Port | 2B | 80/443/22/3306 등 | 위와 동일 |
| Sequence Number | 4B | 초기값 랜덤 (ISN) | 2^32 wrap |
| Ack Number | 4B | 예상 다음 seq | 2^32 wrap |
| Data Offset | 4비트 | `5` -> 20B | 5~15 (옵션 있으면 ^) |
| Flags | 9비트 | SYN/ACK/PSH/FIN 조합 | (앞 Flags 표 참고) |
| Window Size | 2B | 8192~65535 | **Window Scale 옵션**으로 최대 1GB까지 확장 |
| Checksum | 2B | 계산값 | pseudo-header 포함 |
| Urgent Pointer | 2B | 0 | URG=1일 때만 유효 |

**주요 TCP 옵션 (Data Offset > 5일 때 헤더 뒤에 붙음)**

| 종류 | 길이 | 용도 |
| --- | --- | --- |
| **MSS** (kind=2) | 4B | 3-way handshake 때 "내가 받을 수 있는 최대 세그먼트" 광고. 이더넷이면 1460B |
| **Window Scale** (kind=3) | 3B | Window 필드에 `<< N` 배수 적용. 고속 장거리용 |
| **SACK Permitted** (kind=4) | 2B | SACK 지원 협상 |
| **SACK** (kind=5) | 가변 | 수신 중 빈 구간 지정 재전송 요청 |
| **Timestamp** (kind=8) | 10B | RTT 측정 + 순번 wrap 방지 (PAWS) |
| NOP (kind=1) | 1B | 4바이트 정렬 패딩 |

**호스트별 송/수신 버퍼 상한 (Linux sysctl)**

| 설정 | 기본값 | 뜻 |
| --- | --- | --- |
| `net.ipv4.tcp_wmem` | `4096 16384 4194304` (min/default/max B) | TCP 송신 버퍼 자동 조절 범위 |
| `net.ipv4.tcp_rmem` | `4096 131072 6291456` | TCP 수신 버퍼 자동 조절 범위 |
| `net.core.rmem_max` / `wmem_max` | `212992` | SO_RCVBUF/SO_SNDBUF로 앱이 지정할 수 있는 상한 |
| `net.core.somaxconn` | `4096` (최신 커널) | listen backlog 큐 최대 |

#### ⑦ 체크섬·FCS 실제 연산

세 가지가 **다른 알고리즘**을 쓴다. 한 번에 모아본다.

**(1) Ethernet FCS — CRC-32 (NIC 하드웨어)**

다항식 비트 나눗셈.

```
사용 다항식:
  G(x) = x^32 + x^26 + x^23 + x^22 + x^16 + x^12
       + x^11 + x^10 + x^8  + x^7  + x^5  + x^4
       + x^2  + x    + 1
  16진수 표기: 0x04C11DB7  (역순 0xEDB88320)
```

*송신*

```
1. 프레임 전체 비트열 M(x) 에 32비트 0 을 뒤에 패딩 -> M'(x) = M(x) * 2^32
2. M'(x) 를 G(x) 로 나눈 나머지 R(x) 계산
3. R(x) 의 32비트를 그대로 프레임 끝에 붙여 전송
```

*수신 검증*

```
받은 (프레임 + FCS) 전체를 G(x) 로 나눔
  -> 나머지가 0 이면 OK
  -> 아니면 비트 에러 감지 -> drop
```

소프트웨어 구현:

```c
uint32_t crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}
```

실제로는 NIC 칩 안의 **XOR 시프트 회로**가 바이트가 들어오는 동안 스트리밍으로 계산한다. CRC-32는 32비트 미만의 모든 버스트 에러와 대부분의 2비트·홀수비트 에러를 잡는다.

**(2) IP Header Checksum — 1의 보수 16비트 합**

매우 단순. CPU/커널 또는 NIC 오프로드가 계산.

```c
uint16_t ip_checksum(const void *vdata, size_t length) {
    const uint16_t *data = vdata;
    uint32_t acc = 0;
    for (size_t i = 0; i < length / 2; i++) {
        acc += ntohs(data[i]);         // 16비트씩 누적
        if (acc & 0x10000)              // carry 발생 시
            acc = (acc & 0xFFFF) + 1;   // 1 더함 (end-around carry)
    }
    return htons(~acc & 0xFFFF);         // 최종 1의 보수
}
```

*송신*

```
1. Checksum 필드를 0 으로 둔 상태에서 IP 헤더 20B (10개 16비트 워드) 합산
2. end-around carry 접어 넣고 1의 보수 -> Checksum 필드 값
```

*수신 검증*

```
헤더 전체 20B 를 똑같이 1의 보수 합 -> 결과가 0xFFFF 이면 OK
```

**라우터는 매 홉마다** TTL 감소로 헤더가 바뀌므로 이 Checksum 을 **재계산**한다. 실제로는 RFC 1624 의 incremental checksum 으로 "바뀐 1바이트만 증감식으로" 갱신한다.

**(3) TCP/UDP Checksum — Pseudo-header 포함 1의 보수 합**

알고리즘은 IP 와 **같은** 1의 보수 16비트 합. 다른 점은 앞에 **pseudo-header**(가짜 헤더)를 이어붙여 계산한다는 것.

```
Pseudo-header (12B)
┌──────────────────────────────────────────────┐
│ Source IP (4B)                               │
│ Destination IP (4B)                          │
│ Zero (1B) │ Protocol (1B) │ TCP Length (2B) │
└──────────────────────────────────────────────┘
+ TCP Header (Checksum=0 으로 놓고)
+ TCP Payload
를 전부 1의 보수 합 -> 결과의 1의 보수를 Checksum 필드에 기록
```

*왜 Pseudo-header?* IP 주소가 변조되거나 프로토콜 번호가 바뀌면 TCP 도 실패하도록 묶어서 end-to-end 무결성을 보장하기 위함.

수신 검증도 같은 방식으로 합해서 `0xFFFF` 이면 OK.

참고로 TCP Checksum 은 약해서 (단순 덧셈) 커널은 **NIC 오프로드**(TSO/GRO/Checksum Offload) 로 넘기는 경우가 많다. 그래서 `tcpdump` 에 `bad cksum` 경고가 뜨더라도 실제론 NIC 가 나중에 채워서 내보낸 경우가 대부분이다.

**세 알고리즘 비교**

| 체크섬 | 알고리즘 | 범위 | 계산 주체 | 강도 |
| --- | --- | --- | --- | --- |
| Ethernet FCS | CRC-32 (다항식 나눗셈) | 1홉 링크 | NIC 하드웨어 | 강함 |
| IP Header Checksum | 1의 보수 16비트 합 | IP 헤더만 | CPU/커널 (홉마다) | 약함 |
| TCP/UDP Checksum | 1의 보수 16비트 합 + pseudo-header | 헤더 + 페이로드 | CPU/NIC 오프로드 (end-to-end) | 약함 |

####  종합: 67B 예시 바이트맵

```
오프셋  내용                                크기
──────────────────────────────────────────────
0  ~ 5   Dst MAC                           6B   ┐
6  ~ 11  Src MAC                           6B   │ Ethernet Header 14B
12 ~ 13  EtherType (0x0800)                2B   ┘
14       Ver=4/IHL=5 (0x45)                1B   ┐
15       TOS                               1B   │
16 ~ 17  Total Length                      2B   │
18 ~ 19  Identification                    2B   │
20 ~ 21  Flags + Frag Offset               2B   │ IP Header 20B
22       TTL                               1B   │
23       Protocol (6=TCP)                  1B   │
24 ~ 25  Header Checksum                   2B   │
26 ~ 29  Src IP                            4B   │
30 ~ 33  Dst IP                            4B   ┘
34 ~ 35  Src Port                          2B   ┐
36 ~ 37  Dst Port                          2B   │
38 ~ 41  Seq Number                        4B   │
42 ~ 45  Ack Number                        4B   │ TCP Header 20B
46       Data Offset + Reserved            1B   │
47       Flags                             1B   │
48 ~ 49  Window                            2B   │
50 ~ 51  Checksum                          2B   │
52 ~ 53  Urgent Pointer                    2B   ┘
54 ~ 66  Payload "GET /home.html...\r\n"   13B    Payload
67 ~ 70  FCS (CRC-32)                      4B    NIC이 끝에 붙임
──────────────────────────────────────────────
                                 총 ~71B (FCS 포함)
```

수신자가 "내 것"이라고 판단하는 과정:

```text
NIC(MAC 레벨)
  ㄴ 프레임의 목적지 MAC 이 내 MAC 인가? 아니면 버린다(브로드캐스트는 예외)
  ㄴ EtherType=0x0800 이면 IP 계층으로 올린다

IP 계층
  ㄴ 목적지 IP 가 내 IP 인가? 아니면(라우터면) forward, (호스트면) drop
  ㄴ proto=6 이면 TCP, 17이면 UDP 로 올린다

TCP/UDP 계층
  ㄴ 목적지 포트 번호에 해당하는 소켓이 있는가?
  ㄴ 있으면 그 소켓의 수신 큐에 payload 복사

애플리케이션
  ㄴ read() / recv() 로 자기 버퍼에 가져간다
```

즉 **데이터 길이는 각 헤더의 "길이 필드"에** 들어 있고, **누구 것인지는 MAC / IP / 포트** 로 계층마다 판단한다. 그래서 같은 비트 뭉치여도 각 계층이 자기 헤더만 떼어내면서 올라간다. 이 구조를 CSAPP는 "encapsulation"으로 설명한다.

## 연결 키워드

- [02-keyword-tree.md — 11.2 Network Hardware](../../csapp-11/02-keyword-tree.md)
- [05-ch11-sequential-numeric-walkthrough.md — 프레임 크기 계산 예시](../../csapp-11/05-ch11-sequential-numeric-walkthrough.md)
- q02. 호스트 내부 송신 파이프라인
- q03. TCP/UDP 가 이 프레임 위에서 무엇을 하는가
