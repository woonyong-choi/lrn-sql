# Part C. 웹 서버 구축 & 동시성 — 화이트보드 탑다운 발표안

이 문서는 Part C 발표를 위해 만든 **실전용 화이트보드 원고**다.
목표는 `socket -> bind -> listen -> accept` 로 시작해서, Tiny Web Server 의 `main -> doit`, 정적/동적 응답, CGI, Proxy, thread pool, SQL API 서버까지를 **하나의 서버가 점점 커지는 과정**으로 설명하는 것이다.

중요한 점은 이 문서가 "서버 키워드 모음"이 되어선 안 된다는 것이다.
발표는 반드시 아래 한 줄의 흐름으로 밀고 가야 한다.

## Part C 에서 끝까지 밀고 갈 한 문장

```text
서버는 먼저 연결을 받을 준비를 하고
들어온 요청 하나를 읽고 해석한 뒤
정적 파일이든 동적 프로그램이든 응답을 만들고
그 구조를 프록시와 스레드 풀로 확장해 실제 서비스로 키운다.
```

## 발표 전에 칠판에 미리 고정할 예시 두 개

Part C 는 요청 예시가 두 개 있으면 설명이 훨씬 쉬워진다.

```text
예시 1. 정적 요청
GET /home.html HTTP/1.0

예시 2. 동적 요청
GET /cgi-bin/adder?15000&213 HTTP/1.0
```

후반부 SQL API 서버 연결용 예시:

```text
POST /api/v1/query HTTP/1.1
Body: SELECT * FROM users WHERE id = 7;
```

## 화이트보드 배치

```text
+--------------------------------------------------------------------------------+
| 상단: "listen -> accept -> doit -> static/dynamic -> proxy -> thread pool"     |
+--------------------------------------+-----------------------------------------+
| 왼쪽: Tiny 함수 호출 트리             | 오른쪽: 요청 하나의 실제 흐름           |
| main / doit / parse_uri / serve_*    | GET /cgi-bin/adder?15000&213           |
+--------------------------------------+-----------------------------------------+
| 하단: 끝까지 남길 키워드                                                    |
| listenfd / connfd / rio / parse_uri / CGI / dup2 / proxy / queue / lock       |
+--------------------------------------------------------------------------------+
```

## 발표 흐름 전체 지도

```text
Scene 1   서버가 연결을 받을 준비를 하는 단계
Scene 2   listenfd 와 connfd
Scene 3   Tiny main -> doit 호출 트리
Scene 4   요청 라인, 헤더, parse_uri
Scene 5   정적 콘텐츠 처리
Scene 6   동적 콘텐츠 처리와 CGI
Scene 7   HTTP/1.0 응답과 MIME
Scene 8   Tiny -> Proxy 확장
Scene 9   iterative -> thread pool / async I/O
Scene 10  SQL API 서버로 연결
```

---

## Scene 1. 서버는 먼저 "문을 만든다"

칠판에 먼저 아래 네 줄을 쓴다.

```text
socket
bind
listen
accept
```

그리고 이렇게 말한다.

`클라이언트는 connect로 문을 두드리지만, 서버는 그 전에 문을 만들어 두어야 합니다. 그 네 단계가 socket, bind, listen, accept 입니다.`

각 함수의 의미를 바로 붙인다.

```text
socket  = 서버용 fd 생성
bind    = 이 fd 에 로컬 IP:port 부여
listen  = passive listener 로 전환
accept  = 실제 연결 하나를 새 fd 로 꺼냄
```

꼭 남길 문장:

`socket은 아직 연결이 아니고, listen은 아직 통신이 아니라 "받을 준비 상태"입니다.`

---

## Scene 2. listenfd 와 connfd 는 왜 다르나

이 장면에서 서버 쪽 객체를 나눈다.

```text
listenfd = 3
  |
  +-- "문 자체"

accept()
  |
  v
connfd = 4
  |
  +-- "특정 클라이언트와의 대화 채널"
```

그리고 실제 흐름을 그린다.

```text
Client ---- connect ----> listenfd
                           |
                           v
                        accept()
                           |
                           v
                         connfd
```

핵심 설명:

- listenfd 는 계속 살아 있는 리스너다.
- connfd 는 클라이언트마다 하나씩 생기는 connected socket 이다.
- 실제 `read/write` 는 connfd 에 대해 일어난다.

꼭 짚을 오해:

- accept 가 listenfd 를 바꾸는 것이 아니다.
- connfd 는 "기존 fd 의 상태 변화"가 아니라 **새 fd** 다.

---

## Scene 3. Tiny Web Server 의 중심 호출 트리

이 장면은 Part C 의 뼈대다. 아래 트리를 그대로 적는다.

```text
main
 ├─ Open_listenfd(port)
 └─ while (1)
     ├─ Accept(listenfd, ...)
     ├─ Getnameinfo(...)
     ├─ doit(connfd)
     └─ Close(connfd)

doit
 ├─ Rio_readinitb
 ├─ Rio_readlineb
 ├─ sscanf(method, uri, version)
 ├─ read_requesthdrs
 ├─ parse_uri(uri, filename, cgiargs)
 ├─ stat(filename, &sbuf)
 ├─ serve_static(...)
 └─ serve_dynamic(...)
```

이 장면에서 할 말:

`Tiny의 핵심은 doit입니다. main은 연결을 받고, doit이 요청 하나의 생명주기를 통째로 처리합니다.`

꼭 강조할 부분:

- `main` 은 accept loop
- `doit` 은 요청 파싱 + 분기 + 응답 생성
- Part C 전체는 사실 `doit` 안을 해부하는 것이다

---

## Scene 4. 요청 라인, 헤더, parse_uri

이제 요청 한 개를 실제 문자열로 쓴다.

```text
GET /cgi-bin/adder?15000&213 HTTP/1.0
Host: localhost:8080

```

그리고 Tiny 내부 처리를 적는다.

```text
1) Rio_readlineb -> 요청 라인 읽기
2) sscanf -> method / uri / version 파싱
3) read_requesthdrs -> 빈 줄까지 소비
4) parse_uri ->
      static  or  dynamic
```

정적/동적 분기 기준도 정확히 적는다.

```text
uri 에 "cgi-bin" 이 없으면 static
uri 에 "cgi-bin" 이 있으면 dynamic
```

동적 예시 분해:

```text
uri      = /cgi-bin/adder?15000&213
filename = ./cgi-bin/adder
cgiargs  = 15000&213
```

정적 예시 분해:

```text
uri      = /home.html
filename = ./home.html
cgiargs  = ""
```

꼭 짚을 포인트:

- Tiny 는 헤더를 깊게 파싱하지 않는다. 빈 줄까지 읽고 넘긴다.
- URI 기준으로 static/dynamic 분기를 타는 단순한 구조다.
- 이 단순함이 오히려 "서버 뼈대"를 보기 좋게 해 준다.

---

## Scene 5. 정적 콘텐츠 처리: 파일을 그대로 응답한다

이제 정적 요청을 끝까지 따라간다.

예시:

```text
GET /home.html HTTP/1.0
```

Tiny 흐름:

```text
parse_uri -> static
stat("./home.html")
serve_static(connfd, "./home.html", size)
```

`serve_static` 내부도 칠판에 적는다.

```text
get_filetype(filename, filetype)
sprintf(response headers)
Rio_writen(connfd, headers, ...)
Open(filename, O_RDONLY)
Mmap(...)
Rio_writen(connfd, srcp, size)
Munmap(...)
```

그리고 실제 응답 예시를 적는다.

```text
HTTP/1.0 200 OK
Server: Tiny Web Server
Connection: close
Content-length: 2048
Content-type: text/html

<html>...</html>
```

숫자까지 같이 말한다.

```text
header 약 91B
body   2048B
총 응답 약 2139B
```

핵심 설명:

- 정적 콘텐츠는 "파일 읽기 + HTTP 헤더 붙이기"다.
- `get_filetype` 은 MIME 타입을 결정한다.
- Tiny 는 `mmap` 으로 파일을 메모리에 매핑한 뒤 한 번에 `rio_writen` 한다.

꼭 짚을 오해:

- 정적 콘텐츠는 "그냥 문자열"이 아니다.
- 서버는 파일 권한 확인, 길이 계산, MIME 타입 결정, 헤더 조립까지 한다.

---

## Scene 6. 동적 콘텐츠 처리: CGI

이 장면이 Part C 의 핵심 드릴다운이다.

예시 요청:

```text
GET /cgi-bin/adder?15000&213 HTTP/1.0
```

분해 결과:

```text
filename = ./cgi-bin/adder
cgiargs  = 15000&213
```

이제 `serve_dynamic` 내부를 그린다.

```text
serve_dynamic(fd, filename, cgiargs)
  |
  +-- write "HTTP/1.0 200 OK\r\n"
  +-- write "Server: Tiny Web Server\r\n"
  +-- Fork()
        |
        +-- child
              setenv("QUERY_STRING", "15000&213", 1)
              Dup2(fd, STDOUT_FILENO)
              Execve("./cgi-bin/adder", ...)
  +-- Wait(NULL)
```

그리고 이 문장을 꼭 말한다.

`CGI의 핵심은 stdout 이 곧 응답이라는 점입니다. dup2(connfd, 1)을 해 두면 CGI 프로그램이 printf 한 내용이 그대로 클라이언트에게 갑니다.`

고정 예시를 실제로 끝까지 적는다.

```text
QUERY_STRING = "15000&213"
adder 가 getenv("QUERY_STRING") 로 읽음
15000 + 213 = 15213 계산
stdout 에
  Connection: close
  Content-length: ...
  Content-type: text/html
  <p>The answer is ...</p>
를 출력
-> stdout 이 connfd 로 연결되어 있으므로 소켓으로 전달
```

꼭 짚을 포인트:

- GET 쿼리스트링은 `QUERY_STRING`
- POST body 는 stdin 으로도 전달 가능
- 부모는 서버 루프 유지, 자식이 CGI 실행
- 매 요청마다 `fork/execve` 비용이 있다

---

## Scene 7. HTTP 응답, MIME, 1.0 vs 1.1

Part C 에서는 네트워크보다 응용 계층을 다루므로, HTTP 형식도 반드시 짚어야 한다.

칠판에 아래 형식을 쓴다.

```text
Request
  METHOD URI VERSION
  headers...
  blank line
  body (optional)

Response
  VERSION STATUS
  headers...
  blank line
  body
```

그리고 Tiny 기준 응답도 다시 적는다.

```text
HTTP/1.0 200 OK
Server: Tiny Web Server
Connection: close
Content-length: 2048
Content-type: text/html
```

MIME 타입 예시:

```text
text/html
image/png
application/json
application/octet-stream
```

HTTP/1.0 과 1.1 차이도 짧게 붙인다.

```text
HTTP/1.0 = 기본 close
HTTP/1.1 = 기본 keep-alive + Host 필수
```

꼭 말할 문장:

`Tiny는 단순화를 위해 1.0 방식으로 응답합니다. 그래야 Content-Length와 close만 지키면 구현이 훨씬 쉬워집니다.`

---

## Scene 8. Tiny 에서 Proxy 로 확장

이제 서버가 "직접 응답을 만드는" 경우에서 "상위 서버에게 대신 물어보는" 경우로 확장한다.

칠판에 아래 그림을 그린다.

```text
Client -> Proxy -> Origin server
```

그리고 Tiny 와 Proxy 의 대응을 적는다.

```text
Tiny
  parse_uri
  -> serve_static / serve_dynamic

Proxy
  parse absolute URL
  -> open_clientfd(host, port)
  -> write request to origin
  -> read response from origin
  -> write response back to client
```

핵심 설명:

- 프록시는 **동시에 서버이자 클라이언트**다.
- 클라이언트에게는 서버처럼 보이고,
- 오리진 서버에게는 클라이언트처럼 보인다.

꼭 짚을 포인트:

- Tiny 의 `main` 루프 구조는 거의 그대로 쓴다
- 바뀌는 건 `serve_*` 부분이다
- 정적/동적 응답을 직접 만들던 자리에, "상위 서버로 connect 해서 relay"가 들어간다

캐시까지 붙이면 한 줄 더:

```text
cache hit  -> 바로 응답
cache miss -> origin 에서 받아오며 동시에 저장
```

---

## Scene 9. iterative 서버의 한계와 thread pool

이제 concurrency 로 넘어간다.

먼저 iterative 구조를 적는다.

```text
while (1) {
    connfd = accept(listenfd, ...);
    doit(connfd);
    close(connfd);
}
```

그리고 문제를 말한다.

- 한 번에 하나의 connfd 만 깊게 처리한다
- 느린 클라이언트, 긴 CGI, 긴 DB 작업이 있으면 뒤 요청이 기다린다

이제 thread pool 구조를 그린다.

```text
main thread
  accept(listenfd)
  -> queue push(connfd)

worker threads
  queue pop(connfd)
  -> doit(connfd)
  -> close(connfd)
```

핵심 설명:

- main 은 accept 와 dispatch 만 담당한다
- worker 는 read / parse / execute / respond 를 담당한다
- queue 는 mutex / condvar 로 보호한다

그리고 async I/O 와도 비교한다.

```text
thread pool = blocking I/O + 여러 worker
epoll       = non-blocking I/O + readiness event loop
```

꼭 말해야 하는 문장:

`동시성의 핵심은 CPU를 마법처럼 늘리는 것이 아니라, I/O 때문에 기다리는 시간을 겹치게 만드는 것입니다.`

---

## Scene 10. SQL API 서버로 연결

마지막 장면은 이번 주 구현과 직접 연결한다.

칠판에 아래처럼 적는다.

```text
Tiny / Proxy / SQL API server

공통
  listen
  accept
  parse request
  make response
  write response

차이
  Tiny   -> file / CGI
  Proxy  -> origin relay
  SQL API -> DB engine execution
```

그리고 SQL API 서버 쪽으로 더 구체화한다.

```text
POST /api/v1/query
 -> parse SQL string
 -> DB engine execute
 -> rows / status serialize
 -> HTTP response
```

락이 필요한 위치도 적는다.

```text
shared queue
buffer pool
B+ tree
transaction / lock table
cache
```

마무리 멘트:

`그래서 CSAPP 11장을 배우는 목적은 웹서버 하나 만들고 끝이 아니라, 요청을 받고 처리하고 응답하는 모든 네트워크 서버를 구현할 수 있는 뼈대를 손에 넣는 데 있습니다.`

---

## 발표 10분 압축 버전

```text
1. socket / bind / listen / accept
2. listenfd 와 connfd
3. main -> doit
4. 요청 라인 / 헤더 / parse_uri
5. serve_static
6. serve_dynamic + CGI
7. HTTP header + MIME + 1.0/1.1
8. Tiny -> Proxy
9. iterative -> thread pool / epoll
10. SQL API 서버 연결
```

## 질문 받으면 어디까지 내려갈지

- `accept 는 뭘 반환하나요?`
  - Scene 2 로 내려가서 listenfd / connfd 를 다시 그린다

- `CGI 출력이 왜 클라이언트로 가나요?`
  - Scene 6 으로 내려가 `dup2(connfd, 1)` 를 다시 적는다

- `정적과 동적의 차이는 뭔가요?`
  - Scene 5, Scene 6 을 비교한다

- `Proxy 와 웹서버는 뭐가 다른가요?`
  - Scene 8 로 내려간다

- `왜 thread pool 이 필요한가요?`
  - Scene 9 로 내려간다

- `SQL API 서버에서 락은 어디에 필요한가요?`
  - Scene 9, Scene 10 을 연결한다

## 연결 문서

- `q05-socket-principle.md`
- `q06-ch11-4-sockets-interface.md`
- `q11-http-ftp-mime-telnet.md`
- `q12-tiny-web-server.md`
- `q13-cgi-fork-args.md`
- `q15-proxy-extension.md`
- `q16-thread-pool-async.md`
- `q17-concurrency-locks.md`
