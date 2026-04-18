# 최우녕 — 11장 질문 정리

CSAPP 11장 Network Programming을 읽으며 떠오른 질문을 주제별로 그룹화해 답변한 문서 모음입니다.
형식은 `docs/question/q11-page-table-pipeline-example.md`를 따릅니다.

## 🔰 먼저 읽을 것 — Top-down Walkthrough

모든 내용을 "클라이언트 ↔ 서버 한 번의 통신" 이라는 한 줄기로 이어 붙인 탑다운 가이드입니다. q01~q14 를 어떤 순서로 읽고 어떻게 연결지어 이해해야 하는지 전체 그림을 잡아줍니다.

- [00-topdown-walkthrough.md — 전체 선형 읽기 가이드](./00-topdown-walkthrough.md)

## 인덱스

| 번호 | 주제 | 파일 |
| --- | --- | --- |
| 00 | **Top-down Walkthrough — 전체 선형 가이드** | [00-topdown-walkthrough.md](./00-topdown-walkthrough.md) |
| q01 | 네트워크 하드웨어 (Ethernet, Bridge, Router, LAN, WAN) | [q01-network-hardware.md](./q01-network-hardware.md) |
| q02 | 호스트 내부 송신 파이프라인 (프로세스 → 커널 → NIC → 이더넷) | [q02-host-network-pipeline.md](./q02-host-network-pipeline.md) |
| q03 | TCP/UDP, 소켓 시스템콜, host-to-host vs process-to-process | [q03-tcp-udp-socket-syscall.md](./q03-tcp-udp-socket-syscall.md) |
| q04 | IP 주소 체계 (IPv4 32비트, IPv6) 와 네트워크 바이트 순서 | [q04-ip-address-byte-order.md](./q04-ip-address-byte-order.md) |
| q05 | DNS, 도메인 등록, Cloudflare로 도메인을 붙이는 원리 | [q05-dns-domain-cloudflare.md](./q05-dns-domain-cloudflare.md) |
| q06 | 소켓의 물리/소프트웨어 동작 원리 (I/O bridge 포함) | [q06-socket-principle.md](./q06-socket-principle.md) |
| q07 | 11.4 Sockets Interface 함수 전체 정리 + addrinfo 필드 | [q07-ch11-4-sockets-interface.md](./q07-ch11-4-sockets-interface.md) |
| q08 | Echo Server, Datagram, EOF, 파일 I/O와의 유사성 | [q08-echo-server-datagram-eof.md](./q08-echo-server-datagram-eof.md) |
| q09 | HTTP / FTP / MIME / Telnet, HTTP 1.0 vs 1.1 | [q09-http-ftp-mime-telnet.md](./q09-http-ftp-mime-telnet.md) |
| q10 | 네트워크 통신을 CPU / 메모리 / 커널 / 핸들 관점에서 정리 | [q10-network-cpu-kernel-handle.md](./q10-network-cpu-kernel-handle.md) |
| q11 | CGI, fork 로 클라이언트 인자를 서버에 전달하는 과정 | [q11-cgi-fork-args.md](./q11-cgi-fork-args.md) |
| q12 | Tiny Web Server 전체 함수/루틴 (11.6) | [q12-tiny-web-server.md](./q12-tiny-web-server.md) |
| q13 | Proxy — Tiny 를 프록시로 확장하는 관점 | [q13-proxy-extension.md](./q13-proxy-extension.md) |
| q14 | Thread Pool, async I/O, 시스템콜 관점의 concurrent server | [q14-thread-pool-async.md](./q14-thread-pool-async.md) |
| q15 | 동시성과 락 — 스레드 풀에서 thread-safe 하게 일하는 법 | [q15-concurrency-locks.md](./q15-concurrency-locks.md) |

## 읽는 순서

- 처음 한 번 끝까지 훑고 싶으면: **`00-topdown-walkthrough.md`** 부터. 이 문서만 읽어도 뼈대가 잡힙니다.
- 네트워크가 처음이면: q01 → q02 → q04 → q06 → q07 → q08 → q09 → q12
- 서버 관점으로 바로 보고 싶으면: q03 → q07 → q12 → q11 → q13 → q14 → q15
- OS 관점으로 보고 싶으면: q02 → q06 → q10 → q14 → q15
- 동시성/락만 집중해서 보고 싶으면: q14 → q15
