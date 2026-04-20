# 최우녕 — 11장 질문 정리

CSAPP 11장 Network Programming 을 읽으며 떠오른 질문을 주제별로 그룹화해 답변한 문서 모음입니다.
형식은 `docs/question/q11-page-table-pipeline-example.md` 를 따릅니다.

## 먼저 읽을 것 — Top-down DFS Walkthrough

"클라이언트 <-> 서버 한 번의 통신" 이라는 한 줄기로 q01~q21 을 이어 붙인 탑다운 DFS 가이드입니다.
위에서 아래로 읽으면 앞 섹션이 다음 섹션의 전제가 됩니다.

- [00-topdown-walkthrough.md — 전체 DFS 선형 읽기 가이드](./00-topdown-walkthrough.md)
- [99-whiteboard-session.md — 같은 내용을 칠판 앞 90분 세션으로 그려서 설명하는 드로잉 플랜](./99-whiteboard-session.md)

## 발표 자료 — 파트별 화이트보드 탑다운 문서

파트 A, B, C 발표를 각각 하나의 흐름으로 설명하기 위한 화이트보드용 문서입니다.
질문 목록을 나열하는 대신, 어떤 그림을 먼저 그리고 어떤 문장으로 다음 장면으로 넘어갈지 기준으로 정리했습니다.

- [part-a-whiteboard-topdown.md — Part A 네트워크 하드웨어 & 커널 송신 경로 발표안](./part-a-whiteboard-topdown.md)
- [part-b-whiteboard-topdown.md — Part B 주소·연결·Handshake 발표안](./part-b-whiteboard-topdown.md)
- [part-c-whiteboard-topdown.md — Part C 웹 서버 구축 & 동시성 발표안](./part-c-whiteboard-topdown.md)

## 인덱스 (DFS 순서)

| 번호 | 주제 | 파일 |
| --- | --- | --- |
| 00 | **Top-down DFS Walkthrough — 전체 선형 가이드** | [00-topdown-walkthrough.md](./00-topdown-walkthrough.md) |
| q01 | 네트워크 하드웨어 (Ethernet / Bridge / Router / LAN / WAN / 프레임 비트맵) | [q01-network-hardware.md](./q01-network-hardware.md) |
| q02 | IP / MAC / Port 주소 체계, 네트워크 바이트 순서 | [q02-ip-address-byte-order.md](./q02-ip-address-byte-order.md) |
| q03 | DNS, 도메인 등록, Cloudflare 로 도메인을 붙이는 원리 | [q03-dns-domain-cloudflare.md](./q03-dns-domain-cloudflare.md) |
| q04 | 리눅스 파일시스템 완전 해부 (VFS / ext4 / 페이지캐시 / blk-mq) | [q04-filesystem.md](./q04-filesystem.md) |
| q05 | 소켓 3층 구조 — file / socket / sock | [q05-socket-principle.md](./q05-socket-principle.md) |
| q06 | 11.4 Sockets Interface 함수 + addrinfo | [q06-ch11-4-sockets-interface.md](./q06-ch11-4-sockets-interface.md) |
| q07 | TCP / UDP, 소켓 시스템콜, host-to-host vs process-to-process | [q07-tcp-udp-socket-syscall.md](./q07-tcp-udp-socket-syscall.md) |
| q08 | 호스트 내부 송신 파이프라인 (프로세스 -> 커널 -> NIC -> 이더넷) | [q08-host-network-pipeline.md](./q08-host-network-pipeline.md) |
| q09 | CPU / 메모리 / 커널 / 핸들 네 개의 렌즈 | [q09-network-cpu-kernel-handle.md](./q09-network-cpu-kernel-handle.md) |
| q10 | I/O Bridge 완전 해부 — CPU / DRAM / NIC 의 물리 / 커널 경로 | [q10-io-bridge.md](./q10-io-bridge.md) |
| q11 | HTTP / FTP / MIME / Telnet, HTTP 1.0 vs 1.1 | [q11-http-ftp-mime-telnet.md](./q11-http-ftp-mime-telnet.md) |
| q12 | Tiny Web Server 전체 함수 / 루틴 (11.6) | [q12-tiny-web-server.md](./q12-tiny-web-server.md) |
| q13 | CGI, fork 로 클라이언트 인자를 서버에 전달하는 과정 | [q13-cgi-fork-args.md](./q13-cgi-fork-args.md) |
| q14 | Echo Server, Datagram, EOF, 파일 I/O 와의 유사성 | [q14-echo-server-datagram-eof.md](./q14-echo-server-datagram-eof.md) |
| q15 | Proxy — Tiny 를 프록시로 확장하는 관점 | [q15-proxy-extension.md](./q15-proxy-extension.md) |
| q16 | Thread Pool, async I/O, 시스템콜 관점의 concurrent server | [q16-thread-pool-async.md](./q16-thread-pool-async.md) |
| q17 | 동시성과 락 — 스레드 풀에서 thread-safe 하게 일하는 법 | [q17-concurrency-locks.md](./q17-concurrency-locks.md) |
| q18 | 스레드 동시성 — 락 없이 터지는 실전 시나리오 13선 | [q18-thread-concurrency.md](./q18-thread-concurrency.md) |
| q19 | 프로세스 조상과 fd 상속 — PID 0/1/2, O_CLOEXEC, daemonize | [q19-process-ancestry-fd-inheritance.md](./q19-process-ancestry-fd-inheritance.md) |
| q20 | fd 수명과 디스패치 — close/CLOEXEC/proto 콜백/공유/stdio 버퍼링 | [q20-fd-lifecycle-and-dispatch.md](./q20-fd-lifecycle-and-dispatch.md) |
| q21 | 프로세스 부모 결정 · subreaper · 가상 메모리 · heap vs mmap · demand paging | [q21-process-parent-and-memory-deep-dive.md](./q21-process-parent-and-memory-deep-dive.md) |
| 99 | 화이트보드 세션 설계 — curl 한 줄을 바닥까지 그려서 설명하기 (90분 플로우) | [99-whiteboard-session.md](./99-whiteboard-session.md) |
| PA | Part A 화이트보드 발표안 — write 에서 wire 까지 | [part-a-whiteboard-topdown.md](./part-a-whiteboard-topdown.md) |
| PB | Part B 화이트보드 발표안 — 주소, DNS, handshake | [part-b-whiteboard-topdown.md](./part-b-whiteboard-topdown.md) |
| PC | Part C 화이트보드 발표안 — Tiny, CGI, Proxy, 동시성 | [part-c-whiteboard-topdown.md](./part-c-whiteboard-topdown.md) |

## DFS 순서 요약

```
[A] 큰 그림                        ->  00 §1
[B] 네트워크 하드웨어               ->  q01
[C] 주소 체계                      ->  q02
[D] DNS                          ->  q03
[E] 유저/커널 경계                  ->  00 §5
[F] 파일 추상화 (바닥까지)          ->  q04
[G] 소켓 3층                       ->  q05
[H] 소켓 API                       ->  q06
[I] TCP/UDP 시스템콜                ->  q07
[J] 송신 파이프라인                  ->  q08
[K] I/O Bridge (바닥까지)           ->  q10
[L] 수신 파이프라인                  ->  00 §12
[M] 네 개의 렌즈                    ->  q09
[N] HTTP 응용                      ->  q11
[O] Tiny Web Server                ->  q12
[P] CGI / fork / dup2              ->  q13
[Q] Echo Server / EOF              ->  q14
[R] Proxy                          ->  q15
[S] Thread Pool / epoll            ->  q16
[T] 락 기본                        ->  q17
[U] 스레드 동시성 실패 13선          ->  q18
[V] 프로세스 조상 / fd 상속          ->  q19
[W] fd 수명 / 디스패치 / stdio        ->  q20
[X] 부모 결정 / 가상메모리 / heap vs mmap -> q21
[Y] 화이트보드 드로잉 플로우         ->  99
```

## 읽는 순서 (트랙별)

```
+─────────────────────────────────────────────────────────────────+
| 트랙                                | 순서                       |
+─────────────────────────────────────────────────────────────────+
| 전체 한 바퀴 (추천)                  | 00 -> q01 -> ... -> q21    |
| 네트워크가 처음                      | q01 q02 q03 q05 q06 q11 q12|
| 서버 관점만                         | q07 q12 q13 q15 q16 q17    |
| OS 관점 (파일 + 커널)                | q04 q05 q09 q10 q16 q17    |
| 동시성/락만                         | q16 q17 q18                |
| 하드웨어 심화 (물리 경로)           | q01 q08 q10                 |
| 파일시스템 심화 (syscall -> block)  | q05 q04                     |
| 동시성 실전 (터지는 시나리오)        | q16 q17 q18                 |
| 커널 코드 레벨 올인원                | q04 q10 q18 q19 q20 q21     |
| 프로세스 조상 / daemonize            | q13 q16 q19 q21             |
| fd / stdio / 디스패치 심화           | q05 q07 q19 q20             |
| 가상 메모리 / heap / mmap            | q04 q21                     |
| 칠판 드로잉 플로우 (발표용)          | 99 -> 00 -> q01..q21        |
+─────────────────────────────────────────────────────────────────+
```
