# 99. 화이트보드 세션 설계 — curl 한 줄을 바닥까지 그려서 설명하기

q01~q18 의 DFS 문서 전체를 **칠판 앞 90 분 세션**으로 압축했을 때의 드로잉·진행 계획입니다.
문서로 읽는 것과 같은 뼈대를 유지하되, 사람이 실시간으로 따라올 수 있도록 "지울 곳 / 남길 곳 / 돌아올 곳" 을 미리 설계해 두었습니다.

## 설계 원칙 다섯 가지

```
 1.  앵커 하나를 가운데 고정                "GET / HTTP/1.1" 한 줄이 모든 드로잉의 시작
 2.  칠판을 세 구역으로 분할                상단 네비게이션 / 본판 / 하단 용어 사전
 3.  색은 세 가지만                        검정=구조, 파랑=유저/CPU, 빨강=커널/HW
 4.  지우는 규칙을 미리 합의                본판은 지워도 됨. 상단/하단은 절대 안 지움
 5.  DFS 리듬을 명시적으로 말함             "한 칸 내려갑니다 / 한 칸 올라갑니다"
```

## 칠판 레이아웃

```
 +───────────────────────────────────────────────────────────────+
 |  상단 띠   "현재 어디" 네비게이션 (DFS 트리 미니맵)             |
 +───────────────────────────────────────────────────────────────+
 |                                                                |
 |  본판     현재 장면. 계속 지우고 다시 그림.                    |
 |                                                                |
 +───────────────────────────────────────────────────────────────+
 |  하단 띠   용어 사전 (나올 때마다 한 줄씩 추가)                  |
 +───────────────────────────────────────────────────────────────+
```

## 장면 시퀀스 (Scene 0 ~ Scene 15)

### Scene 0. 오프닝 — "한 줄의 약속"

본판 한가운데 작게:

```
      [ 내 노트북 ]  ----- ??? -----  [ google.com ]

             $ curl http://www.google.com/
```

멘트: **"오늘 우리가 하는 건 이 점선 사이에서 벌어지는 일을 한 꺼풀씩 벗기는 겁니다. 점선 하나가 4개 층, 10개 시스템콜, 2개 DMA, 수십 개 구조체로 분해됩니다."** — 이 문장이 전체 세션의 씨앗.

대응 문서: [00-topdown-walkthrough.md §1](./00-topdown-walkthrough.md)

### Scene 1. "전선부터 시작" — 물리 계층

점선을 실제 케이블로 다시. 중간에 공유기 박스. 라우터 박스에는 **MAC 을 두 개** 찍는 게 포인트.

```
 [ 노트북 ]────(UTP)────[ 공유기 ]────(광)────[ 라우터 ]────[ google ]
   11:11                  22:22                 33:33
                    eth1:44:44 (공유기가 LAN 바깥 쪽에 쓸 MAC)
```

멘트: **"라우터는 인터페이스마다 MAC 이 따로 있습니다. 뒤에서 'hop 마다 MAC 이 바뀐다' 나오면 이 그림 다시 가리킬 겁니다."**

하단 용어 사전에 추가:

```
 MAC   48비트   NIC 한 개당 하나. 바로 옆 기계 식별.
```

대응 문서: [q01-network-hardware.md](./q01-network-hardware.md)

### Scene 2. "주소는 세 종류" — 층별 주소

본판 옆에 세로 스택.

```
            내 노트북                google
            ─────────               ──────
 포트       51213   <-------------->  80
 IP         192.168.1.10   <----->    142.251.150.104
 MAC        11:11   <----------->     33:33
```

그리고 **3 줄 외우기 문장**을 크게:

```
 포트 = 프로세스끼리
 IP   = 호스트끼리
 MAC  = 옆기계끼리
```

옆쪽에 엔디안 함정을 작게:

```
 htons(80) 필수
 0x0050  (little, host)      0x5000  (big, network)
```

대응 문서: [q02-ip-address-byte-order.md](./q02-ip-address-byte-order.md)

### Scene 3. DNS 옆길 — "142.251 은 어디서?"

본판 왼쪽 아래에 5 줄로만.

```
 내 PC -> 1.1.1.1 -> Root -> com NS -> google.com NS -> 142.251.150.104
           재귀     권한     권한       권한
```

멘트: **"DNS 는 분산 조회인데, 오늘은 이 IP 를 얻었다고 치고 넘어갑니다."** — 옆길이 주제가 되지 않게 끊는다.

대응 문서: [q03-dns-domain-cloudflare.md](./q03-dns-domain-cloudflare.md)

### Scene 4. 유저/커널의 큰 벽 [첫 줌인]

본판을 과감히 지우고 노트북 하나를 크게. 내부에 수평선 한 줄.

```
 +───────────────────────────────────────────────────+
 |  유저 영역  (CPL = 3)                              |
 |                                                    |
 |  curl:   buf = "GET / HTTP/1.1..."                 |
 |          write(4, buf, 95);                        |
 |                  |  syscall 명령                   |
 |─────────────────v───── 유저/커널 경계 ───────────|
 |                                                    |
 |  커널 영역  (CPL = 0)                              |
 |  entry_SYSCALL_64 -> sys_write -> ...               |
 +───────────────────────────────────────────────────+
```

멘트 (가장 중요): **"앞으로 나오는 거의 모든 마법은 이 선을 넘나드는 얘기입니다."**

대응 문서: [00-topdown-walkthrough.md §5](./00-topdown-walkthrough.md)

### Scene 5. "모든 것은 파일" — fdtable 지도

노트북 안 유저 영역 쪽에 작게.

```
 curl 프로세스
   task_struct
     |_ files ─┐
               v
      +──────────────+
      | fdtable.fd[] |
      +──────────────+
      | [0] stdin    |
      | [1] stdout   |
      | [2] stderr   |
      | [3] a.txt    |
      | [4] socket ★ |  <- 오늘 끝까지 쫓아갈 fd
      +──────────────+
```

별표 찍은 슬롯 4 가 **북극성**. 이후 모든 드릴다운이 이 fd=4 를 추적하는 것.

대응 문서: [q04-filesystem.md](./q04-filesystem.md) §1, §6

### Scene 6. [DFS 1차 심화] 파일 추상화 바닥까지

본판 오른쪽 절반을 비우고 "잠깐 옆으로 내려갑시다". 상단 미니맵에 굵은 점을 찍는다.

```
  struct file ── f_op ── [ ext4_file_operations    ]  일반 파일
                 │       [ socket_file_ops         ]  소켓
                 │       [ pipe_read               ]  파이프
                 │       [ tty_read                ]  터미널
                 │       [ proc_reg_read_iter      ]  procfs
                 └─ 같은 read() 가 여기서 갈라진다
```

멘트: **"한 번의 read() 가 fd 타입별로 완전히 다른 함수로 빠집니다. 이게 '모든 게 파일' 의 실제 구현 장치예요."**

끝나면 명시적으로: **"파일시스템 드릴다운 끝, 한 칸 올라갑니다."** 상단 미니맵 굵은 점을 지우고 다음 점으로 이동.

대응 문서: [q04-filesystem.md](./q04-filesystem.md) 전체

### Scene 7. 소켓이라는 "세 겹 양파"

본판으로 복귀. 별표 fd=4 를 중심에 두고 3 층 박스.

```
      fd = 4
        │
        v
   +─────────+   VFS 층
   |  file   |   f_op = socket_file_ops
   +─────────+
        │
        v
   +─────────+   BSD socket 층
   | socket  |   ops = inet_stream_ops
   +─────────+
        │
        v
   +─────────+   프로토콜 층
   |  sock   |   sk_write_queue, cwnd, seq/ack ...
   |(tcp_sock)|
   +─────────+
```

양 옆에 손글씨로 **"write() 는 위에서, 패킷은 아래에서"** 대칭 화살표.

대응 문서: [q05-socket-principle.md](./q05-socket-principle.md)

### Scene 8. API 는 얇은 래퍼 — 대칭표

본판 한쪽에 서버/클라 두 컬럼.

```
    서버                  클라
    ────                  ────
    socket                socket
    bind                    .
    listen                  .
    accept  <-- 블록 -->   connect  <-- SYN 송신
        |                      |
        v                      v
     connfd                 여기까지 오면 연결 완성
        |                      |
        v                      v
    read/write             write/read
    close                  close
```

멘트: **"listenfd 하나, connfd 는 클라마다 하나."** — 이 문장이 뒤 스레드 풀 섹션의 핵심 전제.

대응 문서: [q06-ch11-4-sockets-interface.md](./q06-ch11-4-sockets-interface.md), [q07-tcp-udp-socket-syscall.md](./q07-tcp-udp-socket-syscall.md)

### Scene 9. [메인 이벤트] write() 한 번의 9 단 계단

세션 클라이맥스. 본판 세로로 길게 사용.

```
 [1] 유저          write(4, buf, 95)
      |
      v  (Scene 4 의 큰 벽 넘기)
 [2] VFS           file->f_op->write_iter
      |
      v
 [3] BSD socket    socket->ops->sendmsg
      |
      v
 [4] TCP           sk_buff 할당 (slab) + TCP 헤더 20B
      |
      v
 [5] IP            라우팅 결정 + IP 헤더 20B
      |
      v
 [6] Neighbor      ARP -> dst MAC + Ethernet 14B
      |
      v
 [7] qdisc         pfifo_fast
      |
      v
 [8] 드라이버      DMA descriptor 작성
                   MMIO tail write  ★
      |
      v
 [9] NIC 하드웨어  선로로 나감
```

**★ 8 단계에서 멈춰서** 오른쪽에 `DMA` 와 `MMIO` 두 글자를 크게. **"이 두 글자가 두 번째 드릴다운 구역입니다."**

대응 문서: [q08-host-network-pipeline.md](./q08-host-network-pipeline.md)

### Scene 10. [DFS 2차 심화] I/O Bridge 바닥까지

본판을 지우고 CPU ~ NIC 물리 토폴로지.

```
 +────────────── CPU die ──────────────+
 |  core0  core1  ...  [IMC]  [PCIe RC]|
 +─────────────────────────────────────+
                   |              |
                   v              v
                 DRAM          PCIe 스위치
                                  |
                                  v
                                 NIC
```

그리고 두 개의 흐름을 다른 색으로:

```
 MMIO (파랑):  CPU ─ MOV ─> Root Complex ─ TLP ─> NIC 레지스터
 DMA  (빨강):  NIC ──── PCIe 버스 마스터 ────> DRAM 직접 R/W
```

두 문장으로 고정:

```
 1.  MMIO = CPU 가 NIC 에게 "일 시켜" 쿡 찌르기
 2.  DMA  = NIC 이 CPU 없이 DRAM 을 빨아 마심
```

그 다음 MSI-X 화살표 추가 — "일 끝났다고 NIC 이 CPU 에게 알리는 방법".

끝내면: **"I/O 브릿지 드릴다운 끝, 한 칸 올라갑니다."**

대응 문서: [q10-io-bridge.md](./q10-io-bridge.md)

### Scene 11. 반대편 — 수신은 거꾸로 오르기

Scene 9 의 9 단 계단을 **화살표만 뒤집어서** 재활용. 빨간 펜으로 라벨만 덧씀.

```
 [9]  NIC 수신 -> DMA 로 DRAM 에 씀
 [8]  MSI-X 인터럽트
 [7]  NAPI poll
 [6]  Ethernet 헤더 벗김
 [5]  IP 헤더 벗김
 [4]  TCP -> 4-tuple 로 sock 찾음
 [3]  sk_receive_queue enqueue
 [2]  잠자던 read() 깨움
 [1]  copy_to_user -> 유저 buf
```

멘트: **"송신 그림과 완벽히 대칭이죠?"** — 두 장을 한 쌍으로 묶어 외우게 한다.

대응 문서: [00-topdown-walkthrough.md §12](./00-topdown-walkthrough.md)

### Scene 12. "네 개의 렌즈" 프레임

같은 이벤트를 네 각도에서 본다. 본판 옆에 4 분할 박스.

```
 +─────────────┬─────────────+
 | CPU 시점     | 메모리 시점  |
 | CPL 3->0     | buf 위치   |
 +─────────────┼─────────────+
 | 커널 시점    | 핸들 시점   |
 | fdtable/sock | fd=4 번호   |
 +─────────────┴─────────────+
```

멘트: **"앞으로 디버깅할 때 이 네 렌즈 중 하나가 맞는 질문을 줍니다."**

대응 문서: [q09-network-cpu-kernel-handle.md](./q09-network-cpu-kernel-handle.md)

### Scene 13. HTTP 를 얹자 — Tiny 의 뼈

본판 오른쪽 구석에 아주 단순한 박스.

```
  while (1) {
    connfd = accept(listenfd);
    doit(connfd);
    close(connfd);
  }
```

멘트 (두 번째 "아하!"): **"방금까지 바닥부터 쌓아올린 그 모든 것이... 이 6 줄입니다."**

그 다음 `doit` 안이 정적/동적 분기임을 가지 두 개로. CGI 의 `fork + dup2` 는 부모/자식 박스.

```
   부모 (Tiny)          자식 (adder)
   ───────             ──────────
    fd[1]=tty           fd[1]=tty
       |  fork             |
       v                   v  dup2(connfd, 1)
    fd[1]=tty           fd[1]=connfd ★
                           |
                           v  execve("adder")
                        printf(...) -> 바로 소켓으로
```

대응 문서: [q11-http-ftp-mime-telnet.md](./q11-http-ftp-mime-telnet.md), [q12-tiny-web-server.md](./q12-tiny-web-server.md), [q13-cgi-fork-args.md](./q13-cgi-fork-args.md), [q14-echo-server-datagram-eof.md](./q14-echo-server-datagram-eof.md), [q15-proxy-extension.md](./q15-proxy-extension.md)

### Scene 14. 한 번 -> 여러 번 — Concurrent

Tiny 6 줄 옆에 "이 서버 초당 2 명 밖에 못 받아요" 표기. 세 확장을 비교 박스.

```
 +──────────────┬──────────────┬──────────────+
 | fork per conn| thread per  | epoll 이벤트  |
 |              | conn         |              |
 +──────────────┼──────────────┼──────────────+
 | 메모리 비쌈  | 10k 스레드= | 1 스레드로    |
 | 격리 강함    | 스택 10GB   | 수만 fd       |
 +──────────────┴──────────────┴──────────────+
```

중간 답으로 **스레드 풀** 을 선택. Producer/Consumer 두 박스.

대응 문서: [q16-thread-pool-async.md](./q16-thread-pool-async.md)

### Scene 15. [DFS 3차 심화] 주소 공간 공유의 폭탄

아주 단순한 한 줄 코드.

```
   int counter = 0;
   // 두 스레드가 동시에
   counter++;
```

멘트: **"이게 한 방에 증가할까요?"** 이어서 바로 3 줄 어셈블리.

```
   load  [counter] -> eax
   add   1
   store eax -> [counter]
```

두 스레드 시간선 나란히 그려서 중간 끼어들기 표시 — **Lost Update** 가 저절로 보인다.

이어서 MESI 상태를 오른쪽에:

```
  Core A: M (수정됨)
  Core B: I (무효)

  A 가 쓰기 -> B 로 Invalidate 브로드캐스트
  B 가 읽으려면 -> A 에서 당겨감
```

"13 개 시나리오" 표는 하단에만 남긴다. 하나하나 그릴 시간 없음. q18 로 위임.

대응 문서: [q17-concurrency-locks.md](./q17-concurrency-locks.md), [q18-thread-concurrency.md](./q18-thread-concurrency.md)

### Scene 16. 통합 — 한 판에 전부 [마지막 10 분]

새 빈 칠판에 지금까지의 뼈대를 하나로.

```
 +────────────────────────────────────────────────────────────────+
 |  [유저]   curl ──> write(4, ...)                                 |
 |             |         |                                          |
 |             |       (CPL 벽)                                     |
 |             |         |                                          |
 |  [커널]   VFS ──> sock ──> TCP ──> IP ──> Neigh ──> qdisc         |
 |                                                     |           |
 |                                                     v           |
 |                                                    DMA          |
 |  [하드]   NIC ────── 선로 ─────── 반대쪽 NIC                     |
 |                                    |                            |
 |                                    v  (거꾸로 오르기)             |
 |                                   ...                            |
 |                                   accept + thread pool            |
 |                                   doit -> Tiny                    |
 |                                    |                            |
 |                                    v                            |
 |                                   HTTP 응답 back                 |
 +────────────────────────────────────────────────────────────────+
```

학습자가 노트에 찍어 가져가는 **유일한 한 장**. 각 층의 드릴다운은 노트에 이미 있으니 이 그림에선 생략.

## 진행 규칙

```
 규칙                                         왜
 ───────────────────────────────────────      ───────────────────────
 매 Scene 끝에 "한 줄로 말하면?" 질문         답 못하면 Scene 반복
 용어 처음 나오면 무조건 하단 사전에 추가     다음 등장 때 포인팅만으로 끝
 "한 칸 내려갑니다 / 올라갑니다" 명시        DFS 방향을 잃지 않게
 Scene 9, 11, 16 은 사진으로 보존            세션 이후 복기용
 본판 외에는 90 분 동안 절대 안 지움           미니맵/용어가 유일한 길잡이
```

## 세션 뼈대가 문서와 어떻게 대응되는가

```
 상단 미니맵  =  00-topdown-walkthrough.md 목차
 본판 Scene   =  개별 q 문서 (q01 ~ q18)
 하단 사전    =  용어 인덱스 (README 역할)
 Scene 16     =  00 §22 통합 그림
```

칠판으로 듣는 사람과 문서로 읽는 사람이 **같은 뼈대를 공유** 하도록 맞춰 두었습니다. 혼자 복습할 땐 Scene 16 을 다시 그려 보면 전체가 살아납니다.

## 참고 연결

- [00-topdown-walkthrough.md](./00-topdown-walkthrough.md) — 같은 내용의 글 버전 (DFS 선형)
- [README.md](./README.md) — q 문서 인덱스
