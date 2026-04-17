# Q06. 소켓의 물리/소프트웨어 동작 원리 (I/O bridge 포함)

> CSAPP 11.4 + 9.8(VM) + 6(메모리 계층) | 소켓이 하드웨어까지 어떻게 이어지나 | 중급

## 질문

1. 소켓은 "연결의 끝점" 이라고만 들었다. 실제 물리적으로, 그리고 소프트웨어적으로는 어떻게 구현되어 있는가.
2. I/O bridge 를 통해 비트가 이동한다는데, CPU / DRAM / NIC 사이에서 정확히 어떤 경로로 데이터가 흐르는가.
3. `sockfd` 라는 정수 하나로 커널의 어떤 자료구조가 접근되는가.

## 답변

### 최우녕

> 소켓은 "연결의 끝점" 이라고만 들었다. 실제 물리적으로, 그리고 소프트웨어적으로는 어떻게 구현되어 있는가.

소켓은 **소프트웨어 객체**이지 "하드웨어에 있는 구멍" 이 아니다. 물리적인 것은 **NIC(네트워크 카드)** 뿐이고, 소켓은 그 위에서 OS 커널이 만들어 놓은 **논리적 엔드포인트**다. 이해하기 쉬운 비유는 "파일"이다. 파일이 물리적으로는 디스크 블록인데 유저한텐 `int fd` 로 보이듯, 소켓도 물리적으로는 NIC 와 DRAM 의 버퍼인데 유저한텐 `int sockfd` 로 보인다.

소프트웨어적으로 Linux 기준 객체 체인은 이렇다:

```text
유저 공간
    sockfd (int)
        │
        ▼
[ per-process fd 테이블 ]
    task_struct → files_struct → fdtable[fd] = struct file*
        │
        ▼
struct file
    f_op = socket_file_ops              ← read/write 가 소켓용으로 붙는다
    private_data = struct socket*
        │
        ▼
struct socket                          ← BSD 소켓 레벨 (범용)
    type = SOCK_STREAM | SOCK_DGRAM ...
    ops  = inet_stream_ops             ← 함수 포인터 (connect, bind, sendmsg ...)
    sk   = struct sock*
        │
        ▼
struct sock (또는 tcp_sock / udp_sock)  ← 프로토콜별 상태
    sk_receive_queue   : 수신 sk_buff 체인 (= 수신 버퍼)
    sk_write_queue     : 송신 대기 sk_buff 체인 (= 송신 버퍼)
    sk_state           : TCP 상태 (ESTABLISHED, TIME_WAIT, ...)
    tcp_sock:
       snd_nxt, rcv_nxt, rtt_est, cwnd, rwnd ...
```

그리고 이 소켓의 **물리적 재료**는 아래 둘이다.

- **sk_buff**: 하나의 패킷을 감싸는 커널 자료구조. 실제 바이트는 DRAM 의 페이지에 있고, sk_buff 는 그걸 가리키는 메타데이터와 포인터 집합.
- **NIC**: 물리적 회로. TX/RX ring (descriptor 큐)을 가지고 있고, 커널이 sk_buff 를 descriptor 로 연결해 주면 DMA 로 가져간다.

그래서 "소켓이 물리적으로 어떻게 되어 있냐" 에 대한 정확한 답은 **"DRAM 안의 sk_buff 큐 + NIC ring buffer + 두 개를 이어주는 DMA 경로"** 다.

> I/O bridge 를 통해 비트가 이동한다는데, CPU / DRAM / NIC 사이에서 정확히 어떤 경로로 데이터가 흐르는가.

CSAPP 6장에서 봤던 I/O bridge (요즘 표현으로는 PCH, IOH 칩셋) 를 중심으로 그려 보자.

```text
[ CPU ]──── system bus ────┐
                            │
                     ┌──────┴──────┐
                     │  IO Bridge  │  (= memory controller hub + PCIe root complex)
                     └──────┬──────┘
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
           DRAM         PCIe 링크         ...
                            │
                            ▼
                      [ NIC (PCIe 카드) ]
                          │
                          └── Ethernet PHY ── RJ-45 ── 케이블
```

송신 경로 (유저 → 선로):

```text
1) CPU 가 write() 로 유저 버퍼(buf) 를 sk_buff 에 copy_from_user
     ㄴ 이때 CPU ↔ DRAM 사이의 버스(시스템 버스 + memory controller) 를 씀
     ㄴ 이게 CSAPP 6장에서 말하는 "IO bridge 를 통한 DRAM 접근"

2) TCP/IP 처리 후 sk_buff 를 NIC 의 TX descriptor 에 등록
     ㄴ 드라이버가 MMIO (Memory-Mapped I/O) 로 NIC 레지스터에 "doorbell" 기록
     ㄴ MMIO 도 IO bridge 를 거쳐 PCIe 로 간다

3) NIC 가 DMA 로 DRAM 에서 프레임 바이트를 직접 가져옴
     ㄴ CPU 개입 없이 IO bridge ↔ DRAM ↔ NIC 가 데이터 전송
     ㄴ 끝나면 NIC 가 IRQ 를 발생시켜 CPU 에 알림

4) NIC MAC 블록이 프리앰블 + 프레임 + CRC 를 Ethernet PHY 에 실어 전기/광 신호로 송신
```

수신 경로 (선로 → 유저):

```text
1) Ethernet PHY → NIC MAC 수신 → DMA 로 DRAM 의 RX 버퍼에 기록
2) NIC 가 IRQ (또는 NAPI polling) 로 커널을 깨움
3) softirq 에서 프로토콜 스택이 sk_buff 를 TCP/UDP 소켓 수신 큐로 넣음
4) read() 를 호출한 프로세스가 깨어나고, copy_to_user 로 유저 버퍼에 복사
```

여기서 CSAPP 스러운 중요 관찰:

- **CPU는 많은 경우 데이터를 손으로 만지지 않는다.** DMA 가 해준다. CPU 가 하는 일은 주로 "어디에서 어디로 얼마를" 이라는 메타데이터를 설정하는 것.
- **유저↔커널 복사는 반드시 CPU 가 한다.** 그래서 고성능 서버는 이 복사를 줄이기 위한 기법(`sendfile`, `splice`, `MSG_ZEROCOPY`, `io_uring`) 을 쓴다.
- **I/O bridge 는 메모리 대역폭과 PCIe 대역폭을 모두 중재한다.** 네트워크 트래픽이 늘면 DRAM 대역폭 경합이 CPU 성능에도 영향을 준다.

> `sockfd` 라는 정수 하나로 커널의 어떤 자료구조가 접근되는가.

위에서 그린 체인을 "접근 단계" 로 바꿔 쓰면:

```text
int sockfd = socket(AF_INET, SOCK_STREAM, 0);    // 예: sockfd = 3

커널 동작:
  1) alloc_inode → alloc_file → fd_install(fd=3, file)
  2) sock = sock_alloc()                ← struct socket
  3) sock->ops = &inet_stream_ops
  4) sk = sk_alloc(...)                 ← struct sock (TCP 이면 tcp_sock)
  5) file->private_data = sock
  6) current->files->fdtable->fd[3] = file

이후 user-level write(3, buf, n):
  fdget(3) → struct file*
     → file->f_op->write_iter
         → sock_write_iter
             → sock_sendmsg
                 → sock->ops->sendmsg        (= tcp_sendmsg)
                     → sk_stream_alloc_skb
                     → skb_copy_to_kernel (copy_from_user)
                     → tcp_push_one / tcp_write_xmit
                         → ip_output → ip_finish_output
                             → dev_queue_xmit
                                 → driver->ndo_start_xmit
                                     → NIC TX ring 에 등록, doorbell
```

그래서 "소켓 하나" 라는 것은 사실 위 전체 체인이 합쳐진 그림이고, `sockfd` 는 그 모든 것에 들어가는 **핸들(열쇠)** 역할만 한다. 파일 디스크립터와 같은 원리여서 CSAPP 가 굳이 10장 I/O 뒤에 11장을 붙인 것이다.

## 연결 키워드

- [02-keyword-tree.md — 11.4 Sockets Interface](../../csapp-11/02-keyword-tree.md)
- q02. 송신 파이프라인의 수치 예시
- q03. 시스템콜 구조와 sock->ops 디스패치
- q10. CPU/메모리/커널/핸들 관점 종합
