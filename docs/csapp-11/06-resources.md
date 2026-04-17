# 06. 학습 자료 및 참고 링크

이 문서는 CSAPP 11장 학습과 이번 주 과제를 진행하는 데 필요한 참고 자료를 한 곳에 모은 문서입니다.

## 과제 레포지토리

- webproxy-lab: https://github.com/krafton-jungle/webproxy-lab
- webproxy_lab_docker: https://github.com/krafton-jungle/webproxy_lab_docker
- Proxy Lab 과제 PDF (CMU 원본): http://csapp.cs.cmu.edu/3e/proxylab.pdf

## CSAPP 공식 자료

- CSAPP 공식 사이트: http://csapp.cs.cmu.edu/3e/home.html
- CSAPP 코드 모음: http://csapp.cs.cmu.edu/3e/ics3/code/netp/
- csapp.h 헤더: http://csapp.cs.cmu.edu/3e/ics3/code/include/csapp.h
- csapp.c 구현: http://csapp.cs.cmu.edu/3e/ics3/code/src/csapp.c
- Tiny 웹서버 코드: http://csapp.cs.cmu.edu/3e/ics3/code/netp/tiny/

## 영상 자료

### 소켓 프로그래밍 기초

- Beej's Guide to Network Programming (웹 문서, 영문): https://beej.us/guide/bgnet/
  소켓 프로그래밍의 바이블. 11.4절과 함께 보면 getaddrinfo, bind, listen, accept 흐름이 명확해짐

### HTTP 프로토콜

- MDN HTTP 개요 (한글): https://developer.mozilla.org/ko/docs/Web/HTTP/Overview
  HTTP 기초를 빠르게 잡고 싶을 때. 11.5절 보충용
- HTTP/1.1 RFC 2616 요약: https://developer.mozilla.org/ko/docs/Web/HTTP/Messages
  request line, header, body 구조를 시각적으로 볼 수 있음

### 네트워크 기초

- 널널한 개발자 - 네트워크 기초 시리즈 (한글, YouTube): https://www.youtube.com/playlist?list=PLXvgR_grOs1BFH-TuqFsfHqbh-cpMbFbM
  TCP/IP, 소켓, 패킷 흐름을 한글로 이해하기 좋은 시리즈
- Computer Networking: A Top-Down Approach 강의 (영문): https://gaia.cs.umass.edu/kurose_ross/online_lectures.htm
  네트워크를 더 깊이 이해하고 싶을 때

### DNS

- How DNS Works (시각적 설명): https://howdns.works/
  도메인 이름이 IP로 변환되는 과정을 만화로 설명

## man 페이지 (Linux)

11장에서 다루는 핵심 시스템 콜과 함수의 man 페이지 목록입니다.

- `man 2 socket` - 소켓 생성
- `man 2 bind` - 소켓에 주소 할당
- `man 2 listen` - 연결 대기 시작
- `man 2 accept` - 연결 수락
- `man 2 connect` - 서버에 연결
- `man 3 getaddrinfo` - 호스트/서비스 이름을 소켓 주소로 변환
- `man 3 getnameinfo` - 소켓 주소를 호스트/서비스 이름으로 변환
- `man 3 inet_pton` - 문자열 IP를 바이너리로 변환
- `man 3 inet_ntop` - 바이너리 IP를 문자열로 변환
- `man 7 ip` - IP 프로토콜 개요
- `man 7 tcp` - TCP 프로토콜 개요

## C 언어 보충

- C 포인터와 구조체 복습: https://www.learn-c.org/
- fork, execve, dup2 복습: CSAPP 8장 (Exceptional Control Flow)
- mmap 복습: CSAPP 9장 (Virtual Memory)

## 숙제 문제 참고

### 필수 숙제 범위

과제에서 요구하는 숙제 문제는 11.6c, 7, 9, 10, 11입니다. 최소 세 문제 이상 풀어야 합니다.

- 11.6c: Tiny 웹서버에서 HTML 파일과 이미지를 모두 올바르게 서빙하도록 수정
- 11.7: Tiny를 확장하여 HTTP HEAD 메서드 지원
- 11.9: Tiny에서 정적 콘텐츠 서빙 시 mmap 대신 malloc + rio_readn + rio_writen 사용
- 11.10: Tiny에서 CGI adder가 HTML form으로 두 숫자를 입력받도록 수정
- 11.11: Tiny를 확장하여 HTTP POST 메서드로 동적 콘텐츠 서빙

### 숙제 문제 풀이 전략

1. 먼저 Tiny 코드를 완성하고 정상 동작을 확인한다
2. 각 문제를 읽고 Tiny의 어느 함수를 수정해야 하는지 파악한다
3. 한 문제씩 수정 -> 테스트 -> 커밋 사이클로 진행한다

### 숙제와 Tiny 코드 대응

| 숙제 | 수정 대상 함수 | 핵심 변경점 |
|------|---------------|-----------|
| 11.6c | serve_static, get_filetype | MIME 타입 매핑 추가, Content-type 헤더 정확히 반환 |
| 11.7 | doit | HEAD 메서드 분기 추가, body 없이 헤더만 반환 |
| 11.9 | serve_static | mmap 제거, malloc + read + write로 교체 |
| 11.10 | serve_dynamic, adder.c | HTML form 생성, GET 파라미터 파싱 |
| 11.11 | doit, serve_dynamic | POST body 읽기, CONTENT_LENGTH 환경변수 전달 |

## Proxy Lab 참고

- Proxy Lab 과제 PDF: http://csapp.cs.cmu.edu/3e/proxylab.pdf
- CSAPP 12장 (Concurrent Programming) - thread, semaphore, mutex 기초
- POSIX Threads 튜토리얼: https://computing.llnl.gov/tutorials/pthreads/

### Proxy Lab 3단계

| 단계 | 목표 | 핵심 개념 |
|------|------|----------|
| 1단계 | Sequential proxy | 요청 수신 -> 파싱 -> 서버 전달 -> 응답 반환 |
| 2단계 | Concurrent proxy | pthread_create로 요청별 스레드 생성 |
| 3단계 | Cache | thread-safe LRU 캐시, readers-writers lock |

## 수요 코딩회 (SQL API 서버) 참고

- POSIX Threads 가이드: https://computing.llnl.gov/tutorials/pthreads/
- Thread Pool 패턴: https://en.wikipedia.org/wiki/Thread_pool
- C에서 간단한 HTTP 서버 만들기: Tiny 코드가 가장 좋은 참고
- REST API 설계 기초: https://restfulapi.net/

### SQL API 서버와 네트워크 학습의 연결

```
CSAPP 11장 학습       ->  수요 코딩회 구현
---                       ---
socket/bind/listen/accept -> 서버 리스닝 소켓 구성
HTTP request 파싱         -> SQL 요청 파싱
serve_static/dynamic      -> SQL 엔진 호출 + 응답 반환
Proxy의 스레드 처리        -> thread pool로 병렬 SQL 처리
Proxy의 캐시              -> 쿼리 결과 캐시 (선택)
```

## 디버깅 도구

- telnet: HTTP 요청을 직접 보내고 응답 확인
- curl: 명령줄에서 HTTP 요청 테스트
- netstat / ss: 포트 상태 확인
- strace: 시스템 콜 추적
- gdb: C 프로그램 디버깅
- Wireshark: 패킷 캡처 및 분석 (선택)

### 자주 쓰는 디버깅 명령

```bash
# 서버가 포트를 열고 있는지 확인
ss -tlnp | grep <port>

# HTTP GET 요청 보내기
curl -v http://localhost:<port>/

# telnet으로 직접 HTTP 요청
telnet localhost <port>
# 이후 입력:
# GET / HTTP/1.0
# Host: localhost
# (빈 줄)

# 시스템 콜 추적
strace -f -e trace=network ./tiny <port>
```
