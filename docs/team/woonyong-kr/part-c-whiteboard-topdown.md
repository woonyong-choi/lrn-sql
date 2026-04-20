# Part C. 웹 서버 구축 & 동시성 — 화이트보드 탑다운 발표안

Part C 발표를 "소켓을 열고, 요청을 받고, 응답을 만들고, 동시성으로 확장한다"는 한 흐름으로 설명하기 위한 문서입니다.
핵심은 Tiny Web Server, CGI, Proxy, Thread Pool 을 서로 별개 기능이 아니라 한 서버가 성장하는 단계로 보여주는 것입니다.

## 발표 목표

- 서버가 `listen -> accept -> read -> parse -> respond` 로 굴러간다는 공통 뼈대를 잡는다.
- Tiny Web Server 의 정적/동적 처리 흐름을 끊기지 않게 설명한다.
- CGI 와 `fork/dup2/execve` 가 응답 생성과 어떻게 연결되는지 보여준다.
- iterative 서버에서 thread pool / async I/O 로 확장되는 이유를 설명한다.
- 이번 주 SQL API 서버 구현과 바로 이어지게 만든다.

## 발표 한 줄 앵커

```text
서버는 연결을 받을 준비를 해 두고
요청을 읽고 해석해서
정적이든 동적이든 응답을 만들고
동시성 구조를 붙여 여러 요청을 처리한다.
```

## 화이트보드 첫 장 구성

```text
+--------------------------------------------------------------------------------+
| 상단: "listen -> accept -> HTTP -> Tiny -> CGI/Proxy -> Thread Pool"           |
+--------------------------------------+-----------------------------------------+
| 왼쪽: 서버 생명주기                   | 오른쪽: 요청 한 개의 경로               |
| socket / bind / listen / accept      | GET /cgi-bin/adder?15000&213           |
| iterative -> concurrent              | parse -> static or dynamic             |
+--------------------------------------+-----------------------------------------+
| 하단: 용어 사전                                                              |
| listenfd / connfd / request line / CGI / dup2 / proxy / worker / lock         |
+--------------------------------------------------------------------------------+
```

## 고정 예시 요청

발표 내내 같은 요청을 쓰면 흐름이 자연스럽습니다.

```text
GET /cgi-bin/adder?15000&213 HTTP/1.1
Host: localhost:8080
```

후반부 SQL API 연결용 예시:

```text
POST /api/v1/query HTTP/1.1
Body: SELECT * FROM users WHERE id = 7;
```

## 장면 순서

## Scene 1. 서버는 먼저 "받을 준비"를 한다

칠판에 먼저 그릴 것:

```text
socket -> bind -> listen -> accept
```

핵심 설명:

- `socket` 은 서버용 통신 엔드포인트를 만든다.
- `bind` 는 포트 번호를 붙인다.
- `listen` 은 이 소켓을 passive socket 으로 바꾼다.
- `accept` 는 실제 클라이언트 연결마다 새 `connfd` 를 만든다.

꼭 짚을 문장:

`listenfd 는 문 자체이고, connfd 는 들어온 손님 한 명과의 대화 채널입니다.`

## Scene 2. 요청 한 개가 들어오면 무슨 일이 일어나는가

칠판에 추가할 것:

```text
client ---- connect ----> listenfd
                           |
                           v
                         accept
                           |
                           v
                         connfd
                           |
                           v
                        read request
```

핵심 설명:

- 클라이언트 연결 요청은 listenfd 에 도착한다.
- accept 후에는 실제 읽고 쓰는 대상이 connfd 다.
- 이 시점부터 서버는 HTTP 요청 라인과 헤더를 읽는다.

## Scene 3. Tiny Web Server 의 중심 함수

칠판에 추가할 것:

```text
main
  -> open_listenfd
  -> accept
  -> doit(connfd)

doit
  -> read_requesthdrs
  -> parse_uri
  -> serve_static or serve_dynamic
```

핵심 설명:

- Tiny 의 핵심은 `doit()` 이다.
- `doit()` 안에서 요청을 읽고 URI 를 파싱하고 분기를 탄다.
- Tiny 는 작은 서버지만, 실제 웹 서버의 최소 골격을 다 보여준다.

다음 장면 연결 문장:

`그럼 파싱 결과에 따라 서버는 두 갈래로 갈라집니다.`

## Scene 4. 정적 콘텐츠 vs 동적 콘텐츠

칠판에 추가할 것:

```text
parse_uri
   |
   +-- static  -> file read -> send
   |
   +-- dynamic -> CGI program execute
```

핵심 설명:

- 정적 콘텐츠는 파일 내용을 그대로 응답한다.
- 동적 콘텐츠는 프로그램을 실행해서 그 출력이 응답이 된다.
- Tiny 는 이 둘을 분리해 보여 줘서 서버의 역할을 명확히 드러낸다.

꼭 짚을 오해:

- 동적 콘텐츠는 "메모리에 있는 문자열"만 뜻하지 않는다.
- 핵심은 서버가 계산을 거쳐 응답을 만들어 낸다는 점이다.

## Scene 5. CGI: fork -> dup2 -> execve

칠판에 크게 그릴 것:

```text
parent(Tiny)
  |
  +-- fork --> child
                 |
                 +-- setenv / 준비
                 +-- dup2(connfd, STDOUT_FILENO)
                 +-- execve("./adder", ...)
```

핵심 설명:

- 부모는 서버 루프를 유지하고, 자식이 CGI 프로그램을 실행한다.
- `dup2(connfd, 1)` 을 해 두면 CGI 프로그램이 `printf()` 하는 출력이 소켓으로 간다.
- 그래서 프로그램 입장에서는 표준출력에 썼을 뿐인데, 클라이언트는 HTTP 응답 바디로 받는다.

고정 예시 설명:

```text
/cgi-bin/adder?15000&213
-> QUERY_STRING="15000&213"
-> adder 가 15213 계산
-> stdout 으로 출력
-> connfd 로 전달
```

## Scene 6. HTTP 레벨에서 서버가 실제로 하는 일

칠판에 추가할 것:

```text
Request line   GET /cgi-bin/adder?15000&213 HTTP/1.1
Headers        Host: ...
Body           (GET 은 보통 없음)

Response line  HTTP/1.0 200 OK
Headers        Content-length / Content-type
Body           계산 결과 또는 파일 내용
```

핵심 설명:

- 서버는 요청 라인, 헤더, 바디를 읽는다.
- 응답도 상태 줄, 헤더, 바디로 만든다.
- MIME 타입, Content-Length, persistent connection 여부를 이해하면 HTTP 규칙이 보인다.

## Scene 7. Tiny 에서 Proxy 로 확장

칠판에 추가할 것:

```text
Client -> Proxy -> Origin server
```

핵심 설명:

- Tiny 는 "요청을 직접 처리하는 서버"다.
- Proxy 는 중간에 서서 요청을 대신 보내고 응답을 다시 전달한다.
- 그래서 Proxy Lab 은 Tiny 의 소켓/HTTP 감각 위에 "중계" 개념을 추가한 확장판으로 볼 수 있다.

꼭 짚을 포인트:

- 클라이언트는 프록시에 접속하고,
- 프록시는 원서버에 새 클라이언트처럼 접속한다.
- 즉 프록시는 서버이면서 동시에 클라이언트다.

## Scene 8. iterative 서버의 한계

칠판에 추가할 것:

```text
while (1) {
  connfd = accept(...)
  doit(connfd)
  close(connfd)
}
```

핵심 설명:

- 이 구조는 단순하지만 한 번에 하나의 연결만 깊게 처리한다.
- 느린 클라이언트, 블로킹 I/O, 긴 CGI 작업이 생기면 뒤 요청이 기다린다.
- 그래서 실제 서비스는 concurrent 구조가 필요하다.

## Scene 9. thread pool / async I/O 로 확장

칠판에 추가할 것:

```text
main thread
  -> accept
  -> queue push(connfd)

worker threads
  -> queue pop
  -> read / parse / execute / respond
```

핵심 설명:

- thread pool 은 worker 를 미리 만들어 두고 요청마다 재사용한다.
- accept 와 작업 수행을 분리하면 여러 요청을 병렬 처리할 수 있다.
- async I/O 는 스레드를 늘리는 대신 readiness 이벤트 중심으로 처리하는 방식이다.

꼭 짚을 오해:

- 동시성은 "CPU 가 동시에 다 계산한다"보다 "기다림을 겹친다"는 관점에서 먼저 봐야 한다.
- 네트워크 I/O, 파일 I/O, CGI 실행은 기다림이 크기 때문에 concurrent 구조의 이득이 크다.

## Scene 10. 락과 SQL API 서버로 연결

마지막 장면에서 칠판에 아래를 적을 것:

```text
SQL API server
  = network server
  + request parser
  + DB engine
  + thread pool
  + lock / condition / queue
```

핵심 설명:

- 이번 주 구현물은 결국 Tiny 의 구조를 더 큰 형태로 다시 만드는 일이다.
- 차이는 응답 본문 대신 SQL 실행 결과가 나온다는 점이다.
- thread pool 을 붙이면 shared queue, buffer, cache, B+ tree, transaction state 에 락이 필요해진다.

마무리 문장:

`그래서 CSAPP 11장을 배우는 목적은 웹서버 하나 만들고 끝이 아니라, 네트워크 서버를 DBMS API 서버까지 확장할 수 있는 뼈대를 손에 넣는 데 있습니다.`

## 발표 10분 버전 압축 순서

```text
1. socket / bind / listen / accept
2. listenfd 와 connfd 구분
3. main -> doit
4. read_requesthdrs / parse_uri
5. static vs dynamic
6. fork / dup2 / execve
7. Tiny -> Proxy
8. iterative 한계
9. thread pool / async I/O
10. SQL API 서버 연결
```

## 질문 받으면 확장할 위치

- `addrinfo 는 어디서 쓰나?` -> Scene 1
- `CGI 출력이 왜 클라이언트로 가나?` -> Scene 5
- `HTTP/1.0 vs 1.1 차이?` -> Scene 6
- `프록시와 웹서버 차이?` -> Scene 7
- `락은 실제 어디에 필요한가?` -> Scene 9, Scene 10

## 연결 문서

- `q05-socket-principle.md`
- `q06-ch11-4-sockets-interface.md`
- `q11-http-ftp-mime-telnet.md`
- `q12-tiny-web-server.md`
- `q13-cgi-fork-args.md`
- `q15-proxy-extension.md`
- `q16-thread-pool-async.md`
- `q17-concurrency-locks.md`
