# Q02. 호스트 내부 송신 파이프라인 — 프로세스 → 커널 → NIC → 이더넷

> CSAPP 11.2~11.4 | 네트워크의 OS 내부 동작 | 기본~중급

## 질문

1. 프로세스가 `write(sockfd, buf, n)`을 호출한 순간부터 이더넷 선로로 비트가 나가기까지, 호스트 내부에서 어떤 일이 일어나는가.
2. 이 과정을 실제 숫자(IP, 포트, 프레임 크기)로 대입해서 예시를 보여 달라.
3. "프로토콜 소프트웨어는 결국 프로세스"라는데, 그건 어디에 있는가. 유저 프로세스인가 커널인가.

## 답변

### 최우녕

> 프로세스가 `write(sockfd, buf, n)`을 호출한 순간부터 이더넷 선로로 비트가 나가기까지, 호스트 내부에서 어떤 일이 일어나는가.

한 문장으로 쓰면 "유저 버퍼 → 커널 소켓 버퍼 → TCP/IP 처리(헤더 붙이기) → NIC 드라이버 → NIC → 이더넷"이다. 이 각 단계에서 데이터는 복사되고, 헤더가 덧붙고, DMA로 카드에 전달된다.

```text
유저 공간                            커널 공간                              하드웨어
────────────                         ─────────────                         ────────────
process
  │   write(sockfd, buf, n)
  │   ── system call (int 0x80/syscall) ─────▶  트랩, 커널 모드 진입
  │
  │                                 VFS / sockfs
  │                                   ㄴ fd 테이블에서 struct socket 찾기
  │
  │                                 socket layer
  │                                   ㄴ sendmsg() 호출
  │                                   ㄴ 유저 버퍼(buf) 를 커널의
  │                                      소켓 송신 버퍼(sk_buff 체인)로 복사
  │
  │                                 TCP layer
  │                                   ㄴ MSS 기준으로 segment 쪼개기
  │                                   ㄴ seq/ack/flags/포트 적은 TCP 헤더 붙임
  │                                   ㄴ 체크섬 계산
  │
  │                                 IP layer
  │                                   ㄴ 라우팅 테이블 보고 next-hop 결정
  │                                   ㄴ src/dst IP, TTL, proto 적은 IP 헤더 붙임
  │                                   ㄴ 필요 시 fragmentation
  │
  │                                 ARP + Ethernet layer
  │                                   ㄴ next-hop MAC 조회 (ARP cache)
  │                                   ㄴ dst MAC / src MAC / EtherType 붙임
  │
  │                                 Driver (NIC driver)
  │                                   ㄴ sk_buff → TX ring 의 descriptor 에 포인터 기록
  │                                   ㄴ NIC 에 "보내라" MMIO 명령 (doorbell)
  │
  │                                                                 NIC (HW)
  │                                                                   ㄴ DMA 로 DRAM 에서 프레임 읽음
  │                                                                   ㄴ 이더넷 MAC 회로가
  │                                                                      프리앰블 + 프레임 + CRC 송신
  │                                                                   ㄴ 전송 완료 인터럽트(IRQ)
  │
  ◀── 보냈다는 확인 (write 는 커널 버퍼에 복사되면 바로 리턴)
```

**중요한 포인트 네 개**:

첫째, `write`가 리턴한 순간 "선로에 비트가 나갔다"는 뜻이 **아니다**. 리턴은 커널 소켓 버퍼에 복사가 끝난 시점이다. 실제 전송은 TCP 혼잡 제어와 NIC 스케줄에 달려 있다.

둘째, 유저 → 커널 복사는 반드시 한 번 일어난다(`copy_from_user`). 이게 소위 "한 번의 copy"다. 제로카피 최적화(`sendfile`, `splice`)는 이 복사를 없애거나 커널 내부 복사로 대체한다.

셋째, 데이터가 NIC에 넘어가는 방법은 "CPU가 바이트를 하나씩 넣어주는" 방식이 아니라 **DMA**다. 드라이버가 DMA 디스크립터에 "물리 주소 X에서 N바이트 읽어 송신해"라고 써두면 NIC가 스스로 DRAM을 읽어간다. 질문에서 말한 "io 브릿지"는 바로 이 과정에서 CPU ↔ 메모리 ↔ PCIe NIC 를 잇는 I/O 브리지(= 칩셋 PCH/IOH)가 DMA 경로를 만들어 준다.

넷째, **이더넷 계층은 항상 "다음 홉" 까지의 프레임을 만든다.** 최종 목적지 IP 가 다른 서브넷이면 목적지 MAC 은 최종 호스트 MAC 이 아니라 **게이트웨이 라우터의 MAC** 이다.

> 이 과정을 실제 숫자(IP, 포트, 프레임 크기)로 대입해서 예시를 보여 달라.

CSAPP 워크스루와 동일한 시나리오를 쓴다.

- 클라이언트 호스트 A: `128.2.194.242`, MAC `AA:AA:AA:AA:AA:AA`, 임시 포트 `51213`
- 게이트웨이 라우터 R 의 A 쪽 MAC: `11:11:11:11:11:11`
- 서버 호스트 B: `208.216.181.15`, MAC `BB:BB:BB:BB:BB:BB`, 포트 `80`
- 유저 페이로드: `GET /home.html HTTP/1.0\r\nHost: www.example.net\r\nConnection: close\r\nProxy-Connection: close\r\n\r\n` → **95B**

호스트 A 내부에서 일어나는 일을 수치로 쓰면:

```text
[1] write(sockfd, buf95B, 95)           → syscall 트랩

[2] socket 버퍼로 복사
    커널 소켓 송신 버퍼(사이즈 예: 87,380B, tcp_wmem 중간값)
    에 95B 페이로드 복사

[3] TCP 세그먼트화
    MSS = 1460B 이므로 95B 는 그대로 한 세그먼트
    TCP 헤더 20B 붙임
      src port = 51213 (0xC82D)
      dst port = 80    (0x0050)
      seq, ack, flags(PSH|ACK), window, checksum
    → 95 + 20 = 115B

[4] IP 패킷화
    IP 헤더 20B 붙임
      version=4, IHL=5, TTL=64
      proto=6 (TCP)
      src IP = 128.2.194.242
      dst IP = 208.216.181.15
      total-length = 115 + 20 = 135B
    → 115 + 20 = 135B

[5] 이더넷 프레임화
    라우팅 결과: dst IP 가 같은 서브넷이 아니므로 next-hop = 라우터 R
    ARP 캐시에서 R 의 MAC 조회 → 11:11:11:11:11:11
    Ethernet 헤더 14B 붙임
      dst MAC = 11:11:11:11:11:11   ← 라우터 R (최종 서버가 아님!)
      src MAC = AA:AA:AA:AA:AA:AA
      EtherType = 0x0800
    프레임 = 14 + 135 = 149B
    (+ FCS 4B 는 NIC 가 자동 부착 → 실제 선로엔 153B)

[6] NIC 드라이버
    sk_buff 의 물리 주소를 TX descriptor 에 기록
    MMIO 로 doorbell 레지스터에 값 쓰기

[7] NIC (HW)
    DMA 로 149B 를 DRAM → NIC 로 가져옴
    프리앰블 + 프레임 + FCS 를 Ethernet PHY 로 송출
    TX 완료 인터럽트 → 드라이버가 sk_buff 반환
```

라우터 R 을 지날 때의 변화:

```text
IP 헤더:      src=128.2.194.242, dst=208.216.181.15    (유지)
TTL:          64 → 63                                   (1 감소)
Ethernet:     src MAC, dst MAC 모두 교체
              ㄴ src MAC = R 의 LAN2 쪽 MAC (22:22:22:22:22:22)
              ㄴ dst MAC = 서버 B (BB:BB:BB:BB:BB:BB)
```

이게 바로 "**IP 는 끝점이 바뀌지 않고, MAC 은 홉마다 바뀐다**"는 문장의 실제 모습이다.

> "프로토콜 소프트웨어는 결국 프로세스"라는데, 그건 어디에 있는가. 유저 프로세스인가 커널인가.

정확히 말하면 대부분은 **커널 안**에 있다. Linux 기준으로 TCP/IP 스택은 커널의 서브시스템이다(net/ipv4/, net/core/, drivers/net/). 유저 프로세스는 소켓 파일 디스크립터를 통해서 이 서브시스템에 "일을 시키는" 것이지, 프로토콜 자체를 돌리지 않는다.

"프로세스"라는 표현을 엄격히 해석하자면:

- **유저 프로세스**: 브라우저, Tiny 서버, 내 클라이언트. HTTP 메시지를 만들고 `write` 를 호출한다.
- **커널 스레드 / softirq / NAPI**: 실제로 TCP 재전송, 혼잡 제어, ACK 생성, 체크섬 검증을 돌리는 주체. 이들은 유저 공간에 보이지 않지만 "프로세스/스레드"라는 점에선 맞는 표현이다.
- **유저 공간 프로토콜 스택**: 예외적으로 DPDK, QUIC(HTTP/3)의 일부, user-space TCP 같은 구현체는 유저 프로세스 안에서 돌아간다. 고성능이 필요할 때 쓰인다.

즉 CSAPP 관점에서는 "**TCP/IP = 커널의 서브시스템, 애플리케이션은 소켓으로만 접근**"이 정답이고, 그 위에서 사용자가 만드는 프로그램(Tiny, 프록시)이 유저 프로세스다. 그래서 "TCP 소켓 함수는 시스템콜" 이라는 말이 성립한다(자세한 건 q03).

## 연결 키워드

- [05-ch11-sequential-numeric-walkthrough.md — 프레임 단위 수치 예시](../../csapp-11/05-ch11-sequential-numeric-walkthrough.md)
- q01. 헤더·프레임이 어떻게 생겼나
- q03. socket/read/write 가 시스템콜이라는 사실
- q06. I/O bridge 와 소켓의 커널 구조
- q10. CPU/메모리/커널/핸들 관점에서의 전체 요약
