# 07. CSAPP 11장 코드 레퍼런스

이 문서는 CSAPP 11장에 등장하는 모든 구조체, 함수 원형, 전체 소스 코드, Aside 박스, 연습문제, 숙제 문제를 원문 그대로 정리한 레퍼런스입니다.

---

## 1. 구조체 정의

### 1.1 IP 주소 구조체 (Figure 11.9)

```c
/* IP address structure */
struct in_addr {
    uint32_t  s_addr; /* Address in network byte order (big-endian) */
};
```

### 1.2 소켓 주소 구조체 (Figure 11.13)

```c
/* IP socket address structure */
struct sockaddr_in {
    uint16_t        sin_family;   /* Protocol family (always AF_INET) */
    uint16_t        sin_port;     /* Port number in network byte order */
    struct in_addr  sin_addr;     /* IP address in network byte order */
    unsigned char   sin_zero[8];  /* Pad to sizeof(struct sockaddr) */
};

/* Generic socket address structure (for connect, bind, and accept) */
struct sockaddr {
    uint16_t  sa_family;    /* Protocol family */
    char      sa_data[14];  /* Address data */
};
```

편의를 위한 타입 정의:

```c
typedef struct sockaddr SA;
```

`sockaddr_storage`: 프로토콜 독립적, 모든 종류의 소켓 주소를 담을 수 있는 구조체. Echo server에서 `clientaddr` 선언에 사용.

### 1.3 addrinfo 구조체 (Figure 11.16)

```c
struct addrinfo {
    int              ai_flags;      /* Hints argument flags */
    int              ai_family;     /* First arg to socket function */
    int              ai_socktype;   /* Second arg to socket function */
    int              ai_protocol;   /* Third arg to socket function */
    char            *ai_canonname;  /* Canonical hostname */
    size_t           ai_addrlen;    /* Size of ai_addr struct */
    struct sockaddr *ai_addr;       /* Ptr to socket address structure */
    struct addrinfo *ai_next;       /* Ptr to next item in linked list */
};
```

---

## 2. 함수 원형

### 2.1 바이트 순서 변환

```c
#include <arpa/inet.h>

uint32_t htonl(uint32_t hostlong);
uint16_t htons(uint16_t hostshort);
// Returns: value in network byte order

uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);
// Returns: value in host byte order
```

- `htonl`: host → network, 32-bit
- `htons`: host → network, 16-bit
- `ntohl`: network → host, 32-bit
- `ntohs`: network → host, 16-bit
- 64-bit 변환 함수는 없음

### 2.2 IP 주소 변환

```c
#include <arpa/inet.h>

int inet_pton(AF_INET, const char *src, void *dst);
// Returns: 1 if OK, 0 if src is invalid dotted decimal, −1 on error

const char *inet_ntop(AF_INET, const void *src, char *dst, socklen_t size);
// Returns: pointer to a dotted-decimal string if OK, NULL on error
```

- "n" = network, "p" = presentation
- AF_INET (IPv4) 또는 AF_INET6 (IPv6) 지원

### 2.3 소켓 인터페이스 함수

```c
#include <sys/types.h>
#include <sys/socket.h>

int socket(int domain, int type, int protocol);
// Returns: nonnegative descriptor if OK, −1 on error
// 일반적 호출: socket(AF_INET, SOCK_STREAM, 0)

int connect(int clientfd, const struct sockaddr *addr, socklen_t addrlen);
// Returns: 0 if OK, −1 on error

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
// Returns: 0 if OK, −1 on error

int listen(int sockfd, int backlog);
// Returns: 0 if OK, −1 on error
// backlog: 대기 큐 크기 힌트, 보통 1024로 설정

int accept(int listenfd, struct sockaddr *addr, int *addrlen);
// Returns: nonnegative connected descriptor if OK, −1 on error
```

### 2.4 호스트/서비스 변환

```c
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

int getaddrinfo(const char *host, const char *service,
                const struct addrinfo *hints,
                struct addrinfo **result);
// Returns: 0 if OK, nonzero error code on error

void freeaddrinfo(struct addrinfo *result);
// Returns: nothing

const char *gai_strerror(int errcode);
// Returns: error message
```

```c
#include <sys/socket.h>
#include <netdb.h>

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, size_t hostlen,
                char *service, size_t servlen, int flags);
// Returns: 0 if OK, nonzero error code on error
```

### 2.5 헬퍼 함수

```c
#include "csapp.h"

int open_clientfd(char *hostname, char *port);
// Returns: descriptor if OK, −1 on error

int open_listenfd(char *port);
// Returns: descriptor if OK, −1 on error
```

---

## 3. getaddrinfo hints 플래그 정리

| 플래그 | 설명 |
|--------|------|
| `AI_ADDRCONFIG` | 로컬 호스트 설정에 맞는 주소만 반환 (IPv4 설정이면 IPv4만) |
| `AI_CANONNAME` | 첫 번째 addrinfo의 `ai_canonname`을 정식 호스트이름으로 설정 |
| `AI_NUMERICSERV` | service 인자를 포트 번호로만 해석하도록 강제 |
| `AI_PASSIVE` | 서버용 wildcard 주소 반환. host를 NULL로 전달해야 함 |

getnameinfo flags:

| 플래그 | 설명 |
|--------|------|
| `NI_NUMERICHOST` | 도메인 이름 대신 숫자 주소 문자열 반환 |
| `NI_NUMERICSERV` | 서비스 이름 대신 포트 번호 반환 |

---

## 4. 전체 소스 코드

### 4.1 hostinfo.c (Figure 11.17)

도메인 이름을 IP 주소 목록으로 변환하는 프로그램.

```c
#include "csapp.h"

int main(int argc, char **argv)
{
    struct addrinfo *p, *listp, hints;
    char buf[MAXLINE];
    int rc, flags;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <domain name>\n", argv[0]);
        exit(0);
    }

    /* Get a list of addrinfo records */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;       /* IPv4 only */
    hints.ai_socktype = SOCK_STREAM; /* Connections only */
    if ((rc = getaddrinfo(argv[1], NULL, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rc));
        exit(1);
    }

    /* Walk the list and display each IP address */
    flags = NI_NUMERICHOST; /* Display address string instead of domain name */
    for (p = listp; p; p = p->ai_next) {
        Getnameinfo(p->ai_addr, p->ai_addrlen, buf, MAXLINE, NULL, 0, flags);
        printf("%s\n", buf);
    }

    /* Clean up */
    Freeaddrinfo(listp);

    exit(0);
}
```

### 4.2 open_clientfd (Figure 11.18)

서버와 연결을 수립하는 헬퍼 함수. 재진입 가능하고 프로토콜 독립적.

```c
int open_clientfd(char *hostname, char *port) {
    int clientfd;
    struct addrinfo hints, *listp, *p;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;  /* Open a connection */
    hints.ai_flags = AI_NUMERICSERV;  /* ... using a numeric port arg. */
    hints.ai_flags |= AI_ADDRCONFIG; /* Recommended for connections */
    Getaddrinfo(hostname, port, &hints, &listp);

    /* Walk the list for one that we can successfully connect to */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* Socket failed, try the next */

        /* Connect to the server */
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
            break; /* Success */
        Close(clientfd); /* Connect failed, try another */
    }

    /* Clean up */
    Freeaddrinfo(listp);
    if (!p) /* All connects failed */
        return -1;
    else    /* The last connect succeeded */
        return clientfd;
}
```

### 4.3 open_listenfd (Figure 11.19)

리스닝 디스크립터를 열고 반환하는 헬퍼 함수. 재진입 가능하고 프로토콜 독립적.

```c
int open_listenfd(char *port)
{
    struct addrinfo hints, *listp, *p;
    int listenfd, optval=1;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;             /* Accept connections */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... on any IP address */
    hints.ai_flags |= AI_NUMERICSERV;            /* ... using port number */
    Getaddrinfo(NULL, port, &hints, &listp);

    /* Walk the list for one that we can bind to */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;  /* Socket failed, try the next */

        /* Eliminates "Address already in use" error from bind */
        Setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval , sizeof(int));

        /* Bind the descriptor to the address */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* Success */
        Close(listenfd); /* Bind failed, try the next */
    }

    /* Clean up */
    Freeaddrinfo(listp);
    if (!p) /* No address worked */
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0) {
        Close(listenfd);
        return -1;
    }
    return listenfd;
}
```

### 4.4 Echo Client (Figure 11.20)

```c
#include "csapp.h"

int main(int argc, char **argv)
{
    int clientfd;
    char *host, *port, buf[MAXLINE];
    rio_t rio;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }
    host = argv[1];
    port = argv[2];

    clientfd = Open_clientfd(host, port);
    Rio_readinitb(&rio, clientfd);

    while (Fgets(buf, MAXLINE, stdin) != NULL) {
        Rio_writen(clientfd, buf, strlen(buf));
        Rio_readlineb(&rio, buf, MAXLINE);
        Fputs(buf, stdout);
    }
    Close(clientfd);
    exit(0);
}
```

### 4.5 Echo Server (Figure 11.21)

iterative server: 한 번에 한 클라이언트만 처리.

```c
#include "csapp.h"

void echo(int connfd);

int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;  /* Enough space for any address */
    char client_hostname[MAXLINE], client_port[MAXLINE];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE,
                    client_port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", client_hostname, client_port);
        echo(connfd);
        Close(connfd);
    }
    exit(0);
}
```

### 4.6 echo 함수 (Figure 11.22)

```c
#include "csapp.h"

void echo(int connfd)
{
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes\n", (int)n);
        Rio_writen(connfd, buf, n);
    }
}
```

### 4.7 Tiny Web Server - main (Figure 11.29)

```c
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *          GET method to serve static and dynamic content
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg);

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* Check command-line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE,
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        Close(connfd);
    }
}
```

### 4.8 Tiny - doit (Figure 11.30)

```c
void doit(int fd)
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not implemented",
                    "Tiny does not implement this method");
        return;
    }
    read_requesthdrs(&rio);

    /* Parse URI from GET request */
    is_static = parse_uri(uri, filename, cgiargs);
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found",
                    "Tiny couldn't find this file");
        return;
    }

    if (is_static) { /* Serve static content */
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, sbuf.st_size);
    }
    else { /* Serve dynamic content */
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden",
                        "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs);
    }
}
```

### 4.9 Tiny - clienterror (Figure 11.31)

```c
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
```

### 4.10 Tiny - read_requesthdrs (Figure 11.32)

```c
void read_requesthdrs(rio_t *rp)
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return;
}
```

### 4.11 Tiny - parse_uri (Figure 11.33)

```c
int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {  /* Static content */
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri)-1] == '/')
            strcat(filename, "home.html");
        return 1;
    }
    else {  /* Dynamic content */
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        }
        else
            strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}
```

### 4.12 Tiny - serve_static + get_filetype (Figure 11.34)

```c
void serve_static(int fd, char *filename, int filesize)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */
    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));
    printf("Response headers:\n");
    printf("%s", buf);

    /* Send response body to client */
    srcfd = Open(filename, O_RDONLY, 0);
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    Close(srcfd);
    Rio_writen(fd, srcp, filesize);
    Munmap(srcp, filesize);
}

/*
 * get_filetype - Derive file type from filename
 */
void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else
        strcpy(filetype, "text/plain");
}
```

### 4.13 Tiny - serve_dynamic (Figure 11.35)

```c
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) { /* Child */
        /* Real server would set all CGI vars here */
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);       /* Redirect stdout to client */
        Execve(filename, emptylist, environ); /* Run CGI program */
    }
    Wait(NULL); /* Parent waits for and reaps child */
}
```

### 4.14 CGI adder 프로그램 (Figure 11.27)

```c
#include "csapp.h"

int main(void) {
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1=0, n2=0;

    /* Extract the two arguments */
    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        *p = '\0';
        strcpy(arg1, buf);
        strcpy(arg2, p+1);
        n1 = atoi(arg1);
        n2 = atoi(arg2);
    }

    /* Make the response body */
    sprintf(content, "QUERY_STRING=%s", buf);
    sprintf(content, "Welcome to add.com: ");
    sprintf(content, "%sTHE Internet addition portal.\r\n<p>", content);
    sprintf(content, "%sThe answer is: %d + %d = %d\r\n<p>",
            content, n1, n2, n1 + n2);
    sprintf(content, "%sThanks for visiting!\r\n", content);

    /* Generate the HTTP response */
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);

    exit(0);
}
```

---

## 5. 연습문제와 풀이

### Practice Problem 11.1

다음 표를 완성하라:

| Dotted-decimal | Hex |
|----------------|-----|
| 107.212.122.205 | ? |
| 64.12.149.13 | ? |
| 107.212.96.29 | ? |
| ? | 0x00000080 |
| ? | 0xFFFFFF00 |
| ? | 0x0A010140 |

풀이:

| Dotted-decimal | Hex |
|----------------|-----|
| 107.212.122.205 | 0x6BD47ACD |
| 64.12.149.13 | 0x400C950D |
| 107.212.96.29 | 0x6BD4601D |
| 0.0.0.128 | 0x00000080 |
| 255.255.255.0 | 0xFFFFFF00 |
| 10.1.1.64 | 0x0A010140 |

### Practice Problem 11.2

hex2dd.c: 16-bit hex를 네트워크 바이트 순서로 변환하여 출력.

```c
#include "csapp.h"

int main(int argc, char **argv)
{
    struct in_addr inaddr;       /* Address in network byte order */
    uint16_t addr;               /* Address in host byte order */
    char buf[MAXBUF];            /* Buffer for dotted-decimal string */

    if (argc != 2) {
        fprintf(stderr, "usage: %s <hex number>\n", argv[0]);
        exit(0);
    }
    sscanf(argv[1], "%x", &addr);
    inaddr.s_addr = htons(addr);

    if (!inet_ntop(AF_INET, &inaddr, buf, MAXBUF))
        unix_error("inet_ntop");
    printf("%s\n", buf);

    exit(0);
}
```

### Practice Problem 11.3

dd2hex.c: 네트워크 바이트 순서를 16-bit hex로 변환하여 출력.

```c
#include "csapp.h"

int main(int argc, char **argv)
{
    struct in_addr inaddr;       /* Address in network byte order */
    int rc;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <network byte order>\n", argv[0]);
        exit(0);
    }

    rc = inet_pton(AF_INET, argv[1], &inaddr);
    if (rc == 0)
        app_error("inet_pton error: invalid network byte order");
    // (이후 ntohs로 변환하여 출력)
}
```

### Practice Problem 11.4

hostinfo의 getnameinfo 대신 inet_ntop을 사용하는 버전을 작성하라.

### Practice Problem 11.5

CGI 프로그램이 stdout에 쓴 내용이 어떻게 클라이언트에 전달되는지 설명하라.

답: 서버의 `serve_dynamic`에서 자식 프로세스가 `Dup2(fd, STDOUT_FILENO)`로 표준 출력을 연결 디스크립터로 리다이렉트한다. 따라서 CGI 프로그램이 `printf`나 `fwrite`로 stdout에 쓰는 모든 내용은 클라이언트의 연결 디스크립터로 직접 전송된다.

---

## 6. 숙제 문제 (전문)

### 11.6 ◆◆

A. Tiny가 모든 요청 라인과 요청 헤더를 에코하도록 수정하라.

B. 선호하는 브라우저를 사용하여 Tiny에 정적 콘텐츠를 요청하라. Tiny의 출력을 파일로 캡처하라.

C. Tiny의 출력을 검사하여 브라우저가 사용하는 HTTP 버전을 확인하라.

D. RFC 2616의 HTTP/1.1 표준을 참조하여 브라우저의 HTTP 요청에 포함된 각 헤더의 의미를 확인하라.

### 11.7 ◆◆

Tiny를 확장하여 MPG 비디오 파일을 서빙하도록 하라. 실제 브라우저를 사용하여 확인하라.

힌트: `get_filetype`에 `video/mpeg` MIME 타입 추가.

### 11.8 ◆◆

Tiny를 수정하여 CGI 자식 프로세스를 명시적으로 기다리는 대신 SIGCHLD 핸들러 안에서 회수하도록 하라.

힌트: `serve_dynamic`에서 `Wait(NULL)` 제거, `main`에서 `Signal(SIGCHLD, sigchld_handler)` 등록.

### 11.9 ◆◆

Tiny를 수정하여 정적 콘텐츠를 서빙할 때 mmap과 rio_writen 대신 malloc, rio_readn, rio_writen을 사용하도록 하라.

수정 대상: `serve_static` 함수

변경 전:
```c
srcfd = Open(filename, O_RDONLY, 0);
srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
Close(srcfd);
Rio_writen(fd, srcp, filesize);
Munmap(srcp, filesize);
```

변경 후 (개념):
```c
srcfd = Open(filename, O_RDONLY, 0);
srcp = (char *)Malloc(filesize);
Rio_readn(srcfd, srcp, filesize);
Close(srcfd);
Rio_writen(fd, srcp, filesize);
Free(srcp);
```

### 11.10 ◆◆

A. CGI adder 함수(Figure 11.27)를 위한 HTML form을 작성하라. form에는 사용자가 더할 두 숫자를 입력하는 텍스트 박스 두 개가 포함되어야 한다. form은 GET 메서드를 사용하여 콘텐츠를 요청해야 한다.

B. 실제 브라우저를 사용하여 Tiny에서 form을 요청하고, 작성된 form을 Tiny에 제출한 후, adder가 생성한 동적 콘텐츠를 표시하여 작동을 확인하라.

### 11.11 ◆◆

Tiny를 확장하여 HTTP HEAD 메서드를 지원하라. telnet을 웹 클라이언트로 사용하여 작동을 확인하라.

힌트: `doit`에서 HEAD 메서드 분기 추가, 헤더만 반환하고 body는 생략.

### 11.12 ◆◆◆

Tiny를 확장하여 HTTP POST 메서드로 요청된 동적 콘텐츠를 서빙하라. 선호하는 웹 브라우저를 사용하여 작동을 확인하라.

힌트: POST body에서 인자를 읽어야 함. `Content-Length` 헤더로 body 크기 파악. CGI 프로그램에 `CONTENT_LENGTH` 환경변수 전달. 자식 프로세스에서 stdin을 connfd로 리다이렉트.

### 11.13 ◆◆◆

Tiny를 수정하여 write 함수가 조기에 닫힌 연결에 쓰려 할 때 발생하는 SIGPIPE 시그널과 EPIPE 에러를 깔끔하게 (종료 없이) 처리하라.

힌트: SIGPIPE를 SIG_IGN으로 설정하거나 핸들러로 잡고, `rio_writen` 반환값에서 EPIPE 확인.

---

## 7. Aside 박스 모음

### Client-server transactions vs database transactions (p.941)

클라이언트-서버 트랜잭션은 데이터베이스 트랜잭션이 아니며, 원자성 같은 속성을 공유하지 않는다. 이 문맥에서 트랜잭션은 클라이언트와 서버가 수행하는 단계의 시퀀스일 뿐이다.

### Internet vs internet (p.943)

소문자 internet은 일반적 개념, 대문자 Internet은 특정 구현 (글로벌 IP 인터넷)을 지칭한다.

### IPv4 and IPv6 (p.947)

원래 인터넷 프로토콜은 32-bit 주소를 사용하며 IPv4로 불린다. 1996년 IETF가 128-bit 주소를 사용하는 IPv6를 제안했지만, 2015년 기준 인터넷 트래픽의 대다수가 여전히 IPv4. 이 책은 IPv4에 집중하되, 프로토콜 독립적인 인터페이스를 사용하여 프로그래밍한다.

### How many Internet hosts are there? (p.952)

1987년 이후 매년 2회 Internet Domain Survey 실시. 1987년 약 20,000개 호스트에서 2015년 10억 개 이상으로 기하급수적 증가.

### Origins of the Internet (p.953)

1957년 Sputnik → ARPA 설립 → 1967년 ARPANET 계획 → 1969년 첫 노드 → 1972년 인터네트워킹 원칙 → 1974년 TCP/IP 발표 → 1983년 1월 1일 모든 ARPANET 노드 TCP/IP 전환 (글로벌 IP Internet 탄생) → 1985년 DNS 발명 → 1995년 NSFNET 은퇴, 현대 인터넷 아키텍처.

### Origins of the sockets interface (p.954)

소켓 인터페이스는 1980년대 초 UC Berkeley에서 개발. "Berkeley sockets"로도 불림. Unix 4.2BSD 커널에 포함되어 배포. TCP/IP 소스 코드가 대학과 연구소에 확산되며 네트워킹 연구 폭발적 증가.

### What does the _in suffix mean? (p.955)

`_in` 접미사는 input이 아니라 internet의 약자.

### Why the distinction between listening and connected descriptors? (p.959)

`listenfd`와 `connfd`를 구분하면 여러 클라이언트 연결을 동시에 처리하는 concurrent server를 만들 수 있다. 연결 요청이 올 때마다 새 프로세스를 fork하여 connfd로 통신.

### What does EOF on a connection mean? (p.970)

EOF 문자는 존재하지 않는다. EOF는 커널이 감지하는 조건이다. 디스크 파일에서는 현재 위치가 파일 길이를 초과하면 EOF. 인터넷 연결에서는 프로세스가 자신의 연결 끝을 닫으면 EOF. 상대편 프로세스가 스트림의 마지막 바이트를 지나 읽으려 할 때 EOF를 감지한다.

### Origins of the World Wide Web (p.971)

1989년 Tim Berners-Lee가 CERN에서 분산 하이퍼텍스트 시스템 제안 → 1993년 Marc Andreesen이 NCSA에서 Mosaic 그래픽 브라우저 발표 → 웹 폭발적 성장 → 2015년 기준 9억 7,500만 개 이상의 웹사이트.

### Passing arguments in HTTP POST requests (p.975)

HTTP POST 요청의 인자는 URI가 아닌 request body에 전달된다.

### Passing arguments in HTTP POST requests to CGI programs (p.977)

POST 요청에서 자식 프로세스는 표준 입력도 연결 디스크립터로 리다이렉트해야 한다. CGI 프로그램은 request body의 인자를 표준 입력에서 읽는다.

### Dealing with prematurely closed connections (p.986)

클라이언트가 이미 닫은 연결에 서버가 쓰면, 첫 번째 write는 정상 반환하지만 두 번째 write는 SIGPIPE 시그널을 발생시키고 프로세스를 종료한다. SIGPIPE를 잡거나 무시하면 두 번째 write는 errno=EPIPE와 함께 -1을 반환한다. 견고한 서버는 SIGPIPE를 잡고 write의 EPIPE 에러를 확인해야 한다.

---

## 8. HTTP 상태 코드 (Figure 11.25)

| 상태 코드 | 메시지 | 설명 |
|-----------|--------|------|
| 200 | OK | 요청이 에러 없이 처리됨 |
| 301 | Moved permanently | 콘텐츠가 Location 헤더의 호스트로 이동 |
| 400 | Bad request | 서버가 요청을 이해하지 못함 |
| 403 | Forbidden | 서버에 요청된 파일 접근 권한 없음 |
| 404 | Not found | 서버가 요청된 파일을 찾을 수 없음 |
| 501 | Not implemented | 서버가 요청 메서드를 지원하지 않음 |
| 505 | HTTP version not supported | 서버가 요청의 HTTP 버전을 지원하지 않음 |

## 9. CGI 환경변수 (Figure 11.26)

| 환경변수 | 설명 |
|----------|------|
| `QUERY_STRING` | 프로그램 인자 |
| `SERVER_PORT` | 부모가 리스닝하는 포트 |
| `REQUEST_METHOD` | GET 또는 POST |
| `REMOTE_HOST` | 클라이언트의 도메인 이름 |
| `REMOTE_ADDR` | 클라이언트의 dotted-decimal IP 주소 |
| `CONTENT_TYPE` | POST 전용: request body의 MIME 타입 |
| `CONTENT_LENGTH` | POST 전용: request body의 바이트 크기 |

## 10. MIME 타입 (Figure 11.23)

| MIME 타입 | 설명 |
|-----------|------|
| text/html | HTML 페이지 |
| text/plain | 형식 없는 텍스트 |
| application/postscript | Postscript 문서 |
| image/gif | GIF 형식 바이너리 이미지 |
| image/png | PNG 형식 바이너리 이미지 |
| image/jpeg | JPEG 형식 바이너리 이미지 |

---

## 11. 소켓 인터페이스 흐름도 (Figure 11.12)

```
Client                              Server
------                              ------
                                    getaddrinfo
                                    socket
                                    bind
                                    listen
getaddrinfo
socket
              Connection request
connect  ─────────────────────────> accept

rio_writen ────────────────────────> rio_readlineb
rio_readlineb <──────────────────── rio_writen
              ...
close                               rio_readlineb (EOF)
                                    close
```

클라이언트 래퍼: `open_clientfd` = socket + connect
서버 래퍼: `open_listenfd` = socket + bind + listen

## 12. listenfd vs connfd (Figure 11.14)

```
1단계: 서버가 accept에서 블록, listenfd(3)에서 연결 요청 대기
2단계: 클라이언트가 connect 호출하여 연결 요청 전송
3단계: 서버가 accept에서 connfd(4) 반환
       클라이언트가 connect에서 반환
       clientfd와 connfd 사이에 연결 수립
```

- `listenfd`: 클라이언트 연결 요청의 끝점. 한 번 생성되어 서버 수명 동안 유지.
- `connfd`: 클라이언트-서버 연결의 끝점. 매 accept마다 새로 생성되어 요청 처리 후 close.
