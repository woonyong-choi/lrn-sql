# Q03. TCP/UDP, 소켓 시스템콜 구조, host-to-host vs process-to-process

> CSAPP 11.3~11.4 | 전송 계층과 소켓 | 기본

## 질문

1. TCP 와 UDP 의 소켓 함수는 "시스템 콜로 구현되어 있다"는데, 이게 어떤 구조인가.
2. "TCP 는 host-to-host, UDP 는 process-to-process" 라는 표현은 어떤 의미인가. 보통은 반대로 들리는데 왜 그렇게 말하는가.
3. TCP 와 UDP 의 공통점과 차이를 한 번에 정리해 달라.

## 답변

### 최우녕

> TCP/UDP 의 소켓 함수는 "시스템 콜로 구현되어 있다"는데, 이게 어떤 구조인가.

`socket(), bind(), listen(), accept(), connect(), read(), write(), recvfrom(), sendto(), close()` 는 전부 유저 공간에서 호출하면 **glibc 래퍼 → syscall 명령 → 커널 진입 → 커널 내부 구현 실행 → 리턴** 구조다. CSAPP 가 이 함수들을 "Unix I/O 의 확장"으로 소개하는 것도 이 때문이다.

구체적으로 한 호출이 어떻게 동작하는지 쓰면:

```text
[ 유저 공간 ]
    connect(sockfd, (SA*)&servaddr, sizeof(servaddr));
        │
        ▼  glibc 내부 (libc.so) 의 connect 래퍼
             ㄴ 인자 세팅 (rdi, rsi, rdx 레지스터에)
             ㄴ syscall 명령 실행  (x86-64: syscall; 번호 = 42)
        │
        ▼
[ 커널 진입, ring 3 → ring 0 ]
        │
        ▼  syscall_64 entry point
             ㄴ sys_call_table[__NR_connect] 디스패치
             ㄴ __sys_connect()
                  ㄴ sockfd_lookup_light(fd) : fd 테이블 → struct socket
                  ㄴ sock->ops->connect(...)
                        ㄴ TCP 면 tcp_v4_connect()
                             ㄴ 3-way handshake 시작 (SYN 보냄, ACK 기다림)
                        ㄴ UDP 면 udp_connect() (주소만 기록, 핸드셰이크 없음)
        │
        ▼  커널이 완료되면 결과를 rax 에 넣고 sysret
[ 유저 공간으로 복귀 ]
    return 값 → errno
```

핵심은 세 가지다.

첫째, **파일 디스크립터는 "정수 핸들"일 뿐**이고, 커널 내부에서는 `struct file → struct socket → struct sock(TCP or UDP)` 로 이어지는 객체다. `read/write` 가 소켓에서도 동작하는 이유는 `struct file` 의 `file_operations` 에 소켓용 함수 포인터가 박혀 있기 때문이다.

둘째, **프로토콜별 동작은 함수 포인터 테이블(`proto_ops`)로 분기**된다. 같은 `send()` 시스템콜이어도 TCP 소켓이면 `tcp_sendmsg`, UDP 소켓이면 `udp_sendmsg` 로 간다. 이게 "소켓은 프로토콜 독립적인 범용 인터페이스" 라는 표현의 실제 구현이다.

셋째, **시스템콜은 컨텍스트 스위치 비용이 크기 때문에** 네트워크 성능이 문제되면 `sendfile`, `splice`, `io_uring`, 유저 공간 TCP 등으로 이 비용을 줄이는 방향으로 최적화가 들어간다.

> "TCP 는 host-to-host, UDP 는 process-to-process" 라는 표현은 어떤 의미인가.

CSAPP 키워드 트리에도 그대로 쓰여 있는 이 문구는 **책의 축약 표현**이라 오해하기 쉽다. 의도는 이렇다.

CSAPP 는 이 세 계층을 이렇게 본다:

```text
IP   : host-to-host    (호스트 → 호스트까지 비신뢰 배달)
UDP  : process-to-process (IP 위에 "포트 → 프로세스" 디먹싱만 더함, 여전히 비신뢰)
TCP  : process-to-process, 신뢰성 있는 full-duplex connection
```

책에서 "TCP 는 host-to-host" 라고 읽혔다면 그건 **비교의 강조점이 다르기 때문**이다. TCP 가 해주는 일의 핵심은 "두 호스트 사이에 신뢰성 있는 바이트 스트림 연결을 만드는 것"이라서 "host-to-host connection" 이라는 표현을 쓰는 것이고, UDP 는 연결 개념이 없어서 그냥 "포트로 프로세스를 찾아 데이터그램 하나를 던진다"는 의미로 "process-to-process" 를 강조한다.

정확한 이해:

- **IP** 는 "호스트(=IP 주소) → 호스트" 까지만 책임진다. 어느 프로세스에게 줄지 모른다.
- **UDP/TCP** 는 IP 위에 "포트 번호" 라는 **다중화 키** 를 얹어서 프로세스를 구분한다. 그러므로 UDP, TCP 모두 엄밀히 말하면 process-to-process 다.
- **차이는 "연결과 신뢰성"** 에 있다.

즉 CSAPP 의 문구는 "TCP 는 연결을 중심에 둔다 (host-to-host connection 의 이미지), UDP 는 연결 없이 포트로 던지는 것 (process-to-process messaging 의 이미지)" 정도로 읽는 것이 낫다.

> TCP 와 UDP 의 공통점과 차이를 한 번에 정리해 달라.

공통점:

- 둘 다 IP 위에서 돈다. IP 헤더 다음에 각자의 헤더가 붙는다.
- 둘 다 **포트 번호** 로 프로세스를 구분한다.
- 둘 다 유저 입장에서는 소켓 인터페이스(`socket/read/write/recvfrom/sendto`) 로 다룬다.
- 둘 다 체크섬을 계산한다. (IP 헤더는 IP 가, 그 위 segment 는 각자.)

차이:

```text
                 TCP                         UDP
-------------------------------------------------------------
연결            3-way handshake 로 연결 수립   없음 (연결리스 datagram)
신뢰성          seq/ack, 재전송, 순서 보장     없음 (losses, 순서 섞임 허용)
흐름제어        window 기반                   없음
혼잡제어        cwnd, slow start 등            없음
전송 단위       byte stream (경계 없음)        datagram (메시지 경계 보존)
헤더 크기       20B 이상                       8B
API             socket/connect/listen/accept   socket/sendto/recvfrom
                read/write                     연결형으로도 쓸 수 있으나
                                               보통 비연결형
사용 예         HTTP, SSH, TLS, DB 커넥션      DNS(기본), VoIP, 게임 실시간, QUIC의 하부
```

두 프로토콜을 고르는 기준을 한 줄로 적으면 **"손실돼도 되니 빠르게" = UDP, "느려도 되니 정확하게 + 순서대로" = TCP** 다. 이게 네트워크 프로그래밍에서의 큰 결정 지점이다.

## 연결 키워드

- [07-ch11-code-reference.md — 2.3 소켓 인터페이스 함수](../../csapp-11/07-ch11-code-reference.md)
- q02. write 가 syscall 로 어떻게 들어가는지
- q07. 11.4 장 소켓 함수 전체 정리
- q06. 소켓 객체의 커널 구조
