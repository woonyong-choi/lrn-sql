# Q10. 네트워크 통신 과정을 CPU / 메모리 / 커널 / 핸들 관점에서

> CSAPP 6~11 종합 | 네트워크 I/O 를 시스템 관점에서 다시 보기 | 중급

## 질문

1. 네트워크 통신 한 번을 CPU, 메모리, 커널, 파일 핸들(= fd) 의 관점으로 각각 나눠서 설명해 달라.
2. 송신과 수신을 대칭으로 묘사해 달라.
3. 이 관점에서 성능에 영향을 주는 요소는 무엇인가.

## 답변

### 최우녕

> 네트워크 통신 한 번을 CPU, 메모리, 커널, 파일 핸들(= fd) 의 관점으로 각각 나눠서 설명해 달라.

같은 한 번의 통신을 네 개의 렌즈로 보면 같은 장면이 다르게 보인다.

**CPU 관점**: 제어와 복사를 한다. 데이터 이동은 DMA 가 대신 하지만, **유저<->커널 복사**, **체크섬 계산**, **TCP 상태 관리(seq/ack, 혼잡제어 업데이트)**, **syscall 진입/복귀**, **인터럽트 처리** 는 CPU 가 실제로 사이클을 쓴다. 캐시 hit/miss, 브랜치 예측이 그대로 네트워크 성능에 나타난다.

**메모리 관점**: 데이터는 DRAM 의 여러 위치를 오간다. 유저 버퍼 -> 커널 소켓 버퍼(`sk_buff`) -> NIC DMA 버퍼 링 -> NIC 내부 FIFO. 각 구간이 **메모리 대역폭**을 소모하고, **물리 페이지**를 잡고 있다. 커널 `sk_buff` 는 `slab allocator` 가 관리한다.

**커널 관점**: 네트워크 스택은 커널의 서브시스템이다. **socket 계층(BSD API) -> proto_ops(TCP/UDP) -> IP 계층 -> qdisc(큐잉) -> 드라이버** 순서의 함수 체인이 돈다. softirq, NAPI, tasklet 이라는 비-프로세스 컨텍스트도 관여한다.

**핸들(fd) 관점**: 유저는 `int sockfd` 하나만 본다. 이 정수는 `fdtable[sockfd] -> struct file -> struct socket -> struct sock` 로 이어지는 체인을 연다. 파일과 똑같이 `read/write/close` 로 다룰 수 있는 이유가 이 체인 덕분이다.

> 송신과 수신을 대칭으로 묘사해 달라.

**송신 (write)**

```text
CPU       : write(3, buf, 95)  -> syscall 트랩 (~100ns)
핸들      : fd=3 -> file -> socket -> tcp_sock
메모리    : copy_from_user: 유저 buf(95B) -> sk_buff(의 page) (~DRAM R/W)
커널      : tcp_sendmsg -> tcp_write_xmit -> ip_output -> dev_queue_xmit
CPU       : TCP 헤더 적고 체크섬 계산, IP 헤더 적기
핸들      : qdisc -> driver->ndo_start_xmit(skb)
메모리    : NIC TX descriptor 에 sk_buff 물리주소 기록
CPU       : MMIO doorbell 쓰기 (수 ns ~ 수백 ns)
NIC       : DMA 로 DRAM -> NIC 프레임 버퍼 (CPU 개입 없음)
NIC PHY   : 선로에 전기/광 신호 송출
NIC -> CPU : TX 완료 IRQ -> 드라이버가 sk_buff 반환
```

**수신 (read)**

```text
NIC PHY   : 프레임 수신
NIC       : DMA 로 NIC -> DRAM 의 RX 링 버퍼 (CPU 개입 없음)
NIC -> CPU : RX IRQ (또는 NAPI polling 스케줄)
CPU/커널  : softirq NET_RX 에서 sk_buff 를 꺼냄
커널      : __netif_receive_skb -> ip_rcv -> tcp_v4_rcv
CPU       : 체크섬 검증, seq 검사, reordering 큐 처리
핸들      : 4-tuple 로 소켓 검색 -> sock->sk_receive_queue 에 skb enqueue
커널      : 프로세스가 read() 로 대기중이면 wake up
CPU       : read syscall 진입 후 copy_to_user: sk_buff -> 유저 buf
메모리    : DRAM R(sk_buff) -> DRAM W(유저 buf)  == 복사 1회
CPU       : 리턴, 유저 프로세스 재개
```

정확히 대칭이지만 **수신은 "인터럽트->소프트IRQ->유저" 3단계**가 있어 레이턴시가 더 긴 경향이 있다.

> 이 관점에서 성능에 영향을 주는 요소는 무엇인가.

관점별로 정리:

- **CPU**: 시스템콜 횟수, 복사 횟수, 체크섬(NIC offload 유무), 인터럽트 coalescing. 많은 연결 = 많은 컨텍스트 스위치.
- **메모리**: `sk_buff` 할당/해제 빈도(slab 단편화), NUMA (CPU-NIC 가까움), 캐시 정렬, TCP window/send buffer 크기(`tcp_rmem`, `tcp_wmem`).
- **커널**: qdisc 정책(fq_codel 등), backpressure, `SO_REUSEPORT` 로 소켓 분산, epoll/kqueue/io_uring 선택, net namespace 비용.
- **핸들**: 열린 fd 개수(per-process limit), select 는 O(N) 이고 epoll 은 O(1)에 가까움, `accept4()` 로 `SOCK_NONBLOCK`/`SOCK_CLOEXEC` 원샷 설정.

성능 튜닝을 할 때 이 네 관점을 각각 점검하면 병목이 어디에 있는지 바로 드러난다. 예를 들어 `perf top` 에서 `copy_user_enhanced_fast_string` 이 보이면 메모리/CPU 문제, `__netif_receive_skb` 가 보이면 커널 스택 문제, `epoll_wait` 에서 block 되면 handle/이벤트 루프 문제다.

한 줄 결론: 네트워크 I/O 는 "**fd 로 커널에 일을 시키고**, **CPU 가 복사와 제어를 하며**, **메모리에서 sk_buff 가 흘러가고**, **커널 함수 체인이 NIC 까지 밀어준다**" 라는 네 문장의 합이다.

## 연결 키워드

- q02. 호스트 내부 송신 파이프라인 (더 디테일)
- q06. 소켓 객체 구조
- q14. 동시성(스레드 풀/async)에서 이 그림이 어떻게 바뀌는지
