# Q12. Tiny Web Server — 11.6장 함수와 루틴 상세

> CSAPP 11.6 | Tiny 의 전체 구조 | 중급

## 질문

1. 책에서 말하는 **Tiny 서버**는 무엇인가. 최소한 무엇을 할 수 있어야 Tiny 인가.
2. Tiny 를 구성하는 함수(`main`, `doit`, `read_requesthdrs`, `parse_uri`, `serve_static`, `get_filetype`, `serve_dynamic`, `clienterror`)가 각각 어떤 역할을 하고 어떻게 호출되는가.
3. Tiny 의 정적 / 동적 처리 흐름을 코드와 함께 따라가 달라.

## 답변

### 최우녕

> 책에서 말하는 Tiny 서버는 무엇인가. 최소한 무엇을 할 수 있어야 Tiny 인가.

Tiny 는 CSAPP 가 제공하는 **가장 단순한 완결 HTTP/1.0 서버**다. 약 300줄 정도의 C 코드로, 아래 세 가지를 한다.

1. TCP 로 80(또는 지정 포트)에서 listen 하고 순차적으로 연결을 받는다. **iterative server** 다.
2. `GET` 메서드만 지원해서 디스크 파일을 돌려준다(**정적 콘텐츠**).
3. URI 에 `cgi-bin` 이 포함되면 해당 프로그램을 fork/execve 로 실행해 결과를 돌려준다(**동적 콘텐츠**).

Tiny 는 "교재용 참조 구현" 이라 의도적으로 단순하다. HTTPS, keep-alive, POST, SIGCHLD 처리 등은 숙제로 맡긴다. 하지만 **"진짜 웹 서버의 뼈대"** 가 여기 다 들어있다. Proxy Lab 과 이번 주 SQL API 서버도 Tiny 의 `main` 루프 모양을 재사용한다.

> Tiny 를 구성하는 함수가 각각 어떤 역할을 하고 어떻게 호출되는가.

호출 트리를 먼저 그린다.

```text
main
 ├─ Open_listenfd(port)          ← socket + bind + listen 래퍼
 └─ while (1)
     ├─ Accept(listenfd, ...)    ← 연결 하나 받기, connfd 반환
     ├─ Getnameinfo(...)          ← 클라 이름/포트 출력(로깅)
     ├─ doit(connfd)             ← 한 요청 처리
     └─ Close(connfd)

doit
 ├─ Rio_readinitb + Rio_readlineb   ← 요청 라인 읽기
 ├─ sscanf → method, uri, version
 ├─ method != GET → clienterror(501)
 ├─ read_requesthdrs(&rio)           ← 나머지 헤더 소비(파싱하지 않음)
 ├─ parse_uri(uri, filename, cgiargs)
 │    → 반환값 is_static
 ├─ stat(filename, &sbuf)             ← 파일 존재/권한 확인
 │    └─ 실패 → clienterror(404)
 ├─ is_static
 │    ├─ S_ISREG && S_IRUSR         ← 일반 파일 + 읽기 가능인지
 │    │    └─ 아니면 clienterror(403)
 │    └─ serve_static(fd, filename, size)
 └─ dynamic
      ├─ S_ISREG && S_IXUSR         ← 일반 파일 + 실행 가능인지
      │    └─ 아니면 clienterror(403)
      └─ serve_dynamic(fd, filename, cgiargs)

parse_uri
 ├─ strstr(uri, "cgi-bin") == NULL
 │    ├─ strcpy(cgiargs, "")
 │    ├─ strcpy(filename, ".")
 │    ├─ strcat(filename, uri)
 │    └─ 마지막이 '/' 이면 home.html 을 덧붙임
 │    └─ 반환 1 (static)
 └─ else (dynamic)
      ├─ '?' 찾아서 쿼리스트링 분리 → cgiargs
      └─ 반환 0 (dynamic)

serve_static
 ├─ get_filetype(filename, filetype)
 ├─ sprintf 로 응답 라인/헤더 구성
 │    HTTP/1.0 200 OK
 │    Server: Tiny Web Server
 │    Connection: close
 │    Content-length: size
 │    Content-type: filetype
 │    \r\n
 ├─ Rio_writen(fd, headerbuf, ...)
 ├─ Open(filename, O_RDONLY) → srcfd
 ├─ Mmap(0, size, PROT_READ, MAP_PRIVATE, srcfd, 0) → srcp
 ├─ Close(srcfd)
 ├─ Rio_writen(fd, srcp, size)      ← 본문 전송
 └─ Munmap(srcp, size)

serve_dynamic
 ├─ HTTP/1.0 200 OK + Server 헤더 전송
 ├─ Fork()
 │    └─ 자식:
 │         setenv("QUERY_STRING", cgiargs, 1)
 │         Dup2(fd, STDOUT_FILENO)
 │         Execve(filename, emptylist, environ)
 └─ Wait(NULL)                      ← 좀비 회수

clienterror
 ├─ body 는 작은 HTML 문자열
 └─ 응답 라인/헤더/본문을 바로 write
```

각 함수의 역할을 한 줄로 다시 요약:

- `main` : 리스닝과 accept 루프. **요청 1개 = 순회 1회.**
- `doit` : 한 요청 생명주기를 통째로 관리. 파싱 + 라우팅 + 응답.
- `read_requesthdrs` : 1.0 은 헤더를 거의 쓰지 않기 때문에 **빈 줄(`\r\n`) 전까지 소비만** 한다. Host 같은 값을 쓰고 싶으면 여기서 파싱을 추가해야 한다.
- `parse_uri` : URI 만 보고 **정적/동적** 을 결정하고 `filename`, `cgiargs` 로 분리. `cgi-bin` 이라는 규칙이 전부다.
- `serve_static` : `mmap` 으로 파일을 메모리에 매핑한 뒤 통째로 `rio_writen`.
- `get_filetype` : 확장자 → MIME 타입 (`.html → text/html`, `.png → image/png` 등).
- `serve_dynamic` : `fork + dup2 + execve` 로 CGI 실행 (q11 상세).
- `clienterror` : 에러 응답 HTML 을 조립해 바로 보낸다.

> Tiny 의 정적 / 동적 처리 흐름을 코드와 함께 따라가 달라.

**정적 요청** 예시: `GET /home.html HTTP/1.0`

```text
1) Accept → connfd
2) Rio_readlineb → "GET /home.html HTTP/1.0\r\n"
3) sscanf → method="GET", uri="/home.html", version="HTTP/1.0"
4) read_requesthdrs → 빈 줄까지 소비
5) parse_uri:
     uri 에 "cgi-bin" 없음 → static
     filename = "./home.html"
     cgiargs  = ""
     returns 1
6) stat("./home.html") → sbuf.st_size = 2048
   접근 권한 OK
7) serve_static(connfd, "./home.html", 2048)
     ㄴ get_filetype → "text/html"
     ㄴ 응답 헤더 구성
        HTTP/1.0 200 OK
        Server: Tiny Web Server
        Connection: close
        Content-length: 2048
        Content-type: text/html
        \r\n
     ㄴ Rio_writen(connfd, headers, ~91B)
     ㄴ open + mmap → srcp 포인터
     ㄴ Rio_writen(connfd, srcp, 2048)
     ㄴ munmap
8) Close(connfd)
```

총 응답 크기 91B + 2048B = 2139B. 이 값이 `05-ch11-sequential-numeric-walkthrough.md` 에서 쓰는 숫자와 동일한 이유다.

**동적 요청** 예시: `GET /cgi-bin/adder?15000&213 HTTP/1.0`

```text
1) Accept → connfd
2) 요청 라인 파싱
3) read_requesthdrs → 빈 줄까지 소비
4) parse_uri:
     "cgi-bin" 포함 → dynamic
     '?' 기준으로 path = "./cgi-bin/adder", cgiargs = "15000&213"
     returns 0
5) stat → 실행 가능 확인
6) serve_dynamic(connfd, "./cgi-bin/adder", "15000&213")
     ㄴ "HTTP/1.0 200 OK\r\nServer: Tiny Web Server\r\n" 전송
     ㄴ Fork() == 0 (자식)
          setenv("QUERY_STRING", "15000&213", 1)
          Dup2(connfd, 1)                         ← stdout → connfd
          Execve("./cgi-bin/adder", [NULL], environ)
            adder 가 getenv("QUERY_STRING") = "15000&213"
            → 15000 + 213 = 15213 계산
            → stdout 에 Content-* 헤더 + 본문 출력
            → 소켓으로 바로 나감
     ㄴ Wait(NULL)
7) Close(connfd)
```

Tiny 코드를 보면 **"내가 Proxy Lab / SQL API 서버를 만들 때 어디를 바꿔야 하는지"** 가 바로 보인다.

- 프록시: `parse_uri` 대신 URL 파서가 들어가고, `serve_*` 대신 **상위 서버로의 connect + relay** 가 들어간다. `main` 루프의 모양은 같다.
- 동시성: `doit(connfd)` 를 그대로 호출하는 대신 `pthread_create` 로 스레드에 태우거나 큐에 넣는다(q14).
- SQL API 서버: `parse_uri` 를 "요청 본문에서 SQL 문자열 추출" 로 바꾸고, `serve_static/serve_dynamic` 을 "DB 실행 + 결과 직렬화" 로 바꾼다.

## 연결 키워드

- [02-keyword-tree.md — 11.6 Tiny Web Server](../../csapp-11/02-keyword-tree.md)
- [07-ch11-code-reference.md — Tiny 관련 소스 전체](../../csapp-11/07-ch11-code-reference.md)
- q11. CGI 와 fork 동작
- q13. Proxy 로 확장
- q14. Concurrent server 로 확장
