# Q11. CGI, fork 로 클라이언트 인자를 서버에 전달하는 과정

> CSAPP 11.5~11.6 | Dynamic content, CGI | 기본~중급

## 질문

1. CGI 는 무엇인가.
2. 클라이언트가 보낸 인자 `15000&213` 이 CGI 프로그램의 `argv` / 환경변수 / stdin / stdout 중 어디로 전달되는가.
3. 서버가 **fork** 한 자식에서 **execve** 로 CGI 프로그램을 띄우고 그 결과가 클라이언트로 돌아가기까지의 전 과정을 자세히 설명해 달라.

## 답변

### 최우녕

> CGI 는 무엇인가.

CGI(Common Gateway Interface)는 **웹 서버가 외부 실행 파일을 실행해서 그 결과를 HTTP 응답으로 돌려주는 표준 약속**이다. 1993년경 정의된 오래된 규약이지만, "요청이 오면 외부 프로세스에게 위임한다" 는 개념은 현대의 FastCGI, WSGI, uWSGI, 서블릿 컨테이너까지 이어진다.

CGI 의 계약은 네 가지다:

1. 서버는 요청이 오면 CGI 프로그램을 `fork + execve` 로 실행한다.
2. 서버는 요청의 메타 정보(메서드, 경로, 쿼리스트링, 원격 IP 등)를 **환경변수**로 넘긴다.
3. POST 본문 같은 긴 입력은 **stdin** 으로 넘긴다.
4. CGI 프로그램은 **stdout 으로 HTTP 응답 헤더 + 빈 줄 + 본문**을 출력한다. 서버는 이것을 소켓에 그대로 전달한다.

이 "stdout 이 곧 응답" 이라는 설계가 **dup2 + fork** 덕분에 가능해진다. 아래에서 본다.

> 클라이언트가 보낸 인자 `15000&213` 이 CGI 프로그램의 `argv` / 환경변수 / stdin / stdout 중 어디로 전달되는가.

GET 요청의 쿼리스트링은 **환경변수 `QUERY_STRING`** 으로 전달된다. CGI 프로그램은 `getenv("QUERY_STRING")` 을 읽어서 파싱한다. argv 는 URI path 쪽에 쓰이는 게 표준이지만 CSAPP 의 Tiny/adder 예제에서는 쓰지 않는다. POST 본문은 stdin 으로 전달된다. CGI 프로그램의 출력은 stdout 으로 가고, 그게 **연결된 소켓으로 바로 흘러**가서 클라이언트에 전달된다.

```text
요청:  GET /cgi-bin/adder?15000&213 HTTP/1.0

서버가 설정하는 환경변수 일부:
  REQUEST_METHOD = GET
  QUERY_STRING   = 15000&213      ← 여기에 들어간다
  CONTENT_LENGTH = 0              (POST 면 값 있음)
  SERVER_PORT    = 80
  REMOTE_HOST    = 128.2.194.242
  SCRIPT_FILENAME = cgi-bin/adder
```

CGI 프로그램(`adder.c`) 은 이렇게 쓴다:

```c
int main(void)
{
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1 = 0, n2 = 0;

    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        *p = '\0';
        strcpy(arg1, buf);
        strcpy(arg2, p + 1);
        n1 = atoi(arg1);
        n2 = atoi(arg2);
    }

    sprintf(content, "Welcome to add.com: THE Internet addition portal.\r\n");
    sprintf(content + strlen(content),
            "<p>The answer is: %d + %d = %d\r\n", n1, n2, n1 + n2);
    sprintf(content + strlen(content), "Thanks for visiting!\r\n");

    /* HTTP 응답 헤더 + 본문을 stdout 으로 뱉는다.
       서버가 이 stdout 을 소켓에 dup2 해놨기 때문에
       이 printf 가 곧 클라이언트로 전송된다. */
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}
```

> 서버가 **fork** 한 자식에서 **execve** 로 CGI 프로그램을 띄우고 그 결과가 클라이언트로 돌아가기까지의 전 과정을 자세히 설명해 달라.

CSAPP 8장 프로세스 제어를 그대로 써서 이해하면 된다. 핵심 도구는 `fork`, `dup2`, `setenv`, `execve`, `wait` 다.

```text
[ 요청 도착 시, 서버 쪽 흐름 ]

accept() → connfd 획득 (예: connfd=4)
Doit(connfd)
  ㄴ 요청 라인 읽고 parse_uri (→ cgiargs = "15000&213")
  ㄴ serve_dynamic(connfd, filename="cgi-bin/adder", cgiargs="15000&213")
       ㄴ 1) 응답 시작 행 전송 (서버가 직접 보냄)
              "HTTP/1.0 200 OK\r\n"
              "Server: Tiny Web Server\r\n"
       ㄴ 2) fork()
              부모(PID=P) / 자식(PID=C) 분기
              (자식은 부모의 fd 테이블을 "그대로 복사" 받는다 → connfd=4 도 자식에게 보인다)
```

자식 프로세스가 하는 일:

```c
/* CSAPP serve_dynamic 발췌 (Figure 11.31) */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) {
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);           /* stdout 을 connfd 에 연결 */
        Execve(filename, emptylist, environ);  /* CGI 프로그램 로드 */
    }
    Wait(NULL);                            /* 자식 종료 대기 (좀비 수거) */
}
```

여기서 중요한 포인트 네 개:

**① fork 가 fd 테이블을 복사하는 것**: 부모가 연 `connfd=4` 가 **자식에게도 같은 번호로** 보인다. 이건 POSIX `fork` 의 핵심 속성이다. 그래서 자식이 아무 설정 없이도 그 소켓에 접근할 수 있다. (다만 참조 카운트가 +1 되므로 양쪽 다 close 해야 실제 소켓이 해제된다.)

**② dup2 로 stdout 리다이렉트**: `dup2(connfd, 1)` 은 "fd 1(stdout)을 connfd 의 복사본으로 만들어라" 라는 뜻이다. 결과적으로 자식이 `printf`, `fputs`, `write(1, ...)` 로 뭘 쓰든 **소켓으로 직접 나간다**. 이게 CGI 규약의 핵심 트릭이다. 셸에서 `./adder > /dev/tcp/...` 같은 걸 수동으로 하는 것과 같은 효과.

**③ setenv 로 환경변수 전달**: `setenv("QUERY_STRING", "15000&213", 1)` 은 자식의 환경변수 테이블에 값을 꽂는다. `execve` 는 "환경변수 테이블은 유지하고 코드만 교체" 하므로, CGI 프로그램이 시작되면 `getenv("QUERY_STRING")` 으로 즉시 읽을 수 있다.

**④ execve 뒤의 응답 구성**: 서버는 **응답 시작 행과 Server 헤더만** 직접 썼고, 나머지 `Content-type`, `Content-length`, 빈 줄, 본문은 **CGI 프로그램이 stdout 으로 직접 출력**한다. 이게 "서버와 CGI 가 헤더를 나눠 쓰는" 이상한 모양의 이유다. (RFC CGI 명세에서는 이 부분을 "parsed-header vs non-parsed-header(NPH)" 로 구분한다.)

전체 흐름을 그림으로:

```text
클라이언트 ── TCP ──▶ 서버 프로세스 P
                        │ accept → connfd=4
                        │ write "HTTP/1.0 200 OK\r\n"
                        │ write "Server: Tiny Web Server\r\n"
                        │ fork ────────────────┐
                        │                       ▼
                        │                  자식 프로세스 C
                        │                  connfd=4 (공유)
                        │                  setenv QUERY_STRING = "15000&213"
                        │                  dup2(4, 1)  → stdout → connfd
                        │                  execve("cgi-bin/adder", ...)
                        │                           │
                        │                           │ adder 가 QUERY_STRING 을 파싱
                        │                           │ printf 로
                        │                           │   Content-length, Content-type,
                        │                           │   빈 줄, 본문(<p>The answer is ...</p>)
                        │                           │ → 소켓으로 나감
                        │                           │
                        │                  exit(0) ─┘
                        │ wait(...) 로 자식 회수 (좀비 방지)
                        │ close(connfd)
```

이 구조의 의미:

- 서버 본체는 CGI 프로그램이 어떤 언어/어떤 로직인지 전혀 몰라도 된다. 오직 **fork/dup2/execve** 라는 OS 프리미티브로 분리되어 있다.
- 반대로 **매 요청마다 fork/execve 비용이 든다**. 이걸 없앤 게 FastCGI(영구 워커), WSGI(Python 프로세스), 서블릿(JVM 내부) 같은 후속 기술이다.
- CSAPP 의 이 예제는 **프로세스 제어(8장)와 Unix I/O(10장)와 네트워크(11장)를 한 줄로 꿰어주는** 대표 예제라서 교재에 들어가 있다.

## 연결 키워드

- [02-keyword-tree.md — 11.5 CGI](../../csapp-11/02-keyword-tree.md)
- [07-ch11-code-reference.md — Figure 11.30, 11.31](../../csapp-11/07-ch11-code-reference.md)
- q09. HTTP 응답 헤더 구성
- q12. Tiny 의 serve_dynamic 전체
- q14. CGI 를 스레드로 대체하는 흐름
