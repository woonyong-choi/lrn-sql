# Q07. 11.4 Sockets Interface — 함수 전체 정리 + addrinfo + 호출 순서

> CSAPP 11.4 | Sockets Interface 전체 | 기본~중급

## 질문

1. 11.4 장에 등장하는 함수와 호출 순서를 정리해 달라.
2. `getaddrinfo` 를 먼저 부른 뒤 `socket`, `connect` 를 차례로 실행한다는 말은 구체적으로 어떤 코드인가.
3. `struct addrinfo` 의 각 필드는 무슨 뜻이고 어떤 역할을 하는가.

## 답변

### 최우녕

> 11.4 장에 등장하는 함수와 호출 순서를 정리해 달라.

서버와 클라이언트의 생명주기를 나란히 놓으면 한눈에 보인다.

```text
─── 서버 ──────────────────────    ─── 클라이언트 ────────────────
getaddrinfo(host, port, hints, &r)    getaddrinfo(host, port, hints, &r)
   └ AI_PASSIVE 로 wildcard 요구         └ hints 는 보통 AF_UNSPEC
listenfd = socket(...)                clientfd = socket(...)
setsockopt(SO_REUSEADDR, 1)           (옵션 설정은 선택)
bind(listenfd, addr)                        │
listen(listenfd, backlog)                   │
freeaddrinfo(r)                       connect(clientfd, addr)
while (1) {                           freeaddrinfo(r)
  connfd = accept(listenfd,           read/write(clientfd, buf, n)   <-  HTTP 요청/응답
                  &cli, &clilen)      close(clientfd)
  read/write(connfd, buf, n)
  close(connfd)
}
close(listenfd)   // 서버 종료 시
```

각 함수의 한 줄 요약:

- `socket(AF_INET, SOCK_STREAM, 0)` : fd 만 만듦. 아직 주소도 상대도 모름.
- `bind(fd, addr, len)` : 로컬 주소(IP:port) 를 fd 에 묶음. 주로 서버가 호출.
- `listen(fd, backlog)` : fd 를 **passive listener** 로 전환. 커널이 SYN 큐 + accept 큐를 만든다.
- `accept(fd, &cli, &clilen)` : accept 큐에서 연결 하나 꺼내 **새 fd(connfd)** 반환. listenfd 는 그대로 남음.
- `connect(fd, addr, len)` : 서버로 SYN 보내고 3-way handshake 완료될 때까지 블록.
- `read/write` : 커널 소켓 버퍼에서 읽고 쓴다(파일과 동일).
- `close(fd)` : 참조 카운트 -1, 0 되면 FIN 보내고 소켓 해제.
- `getaddrinfo` / `getnameinfo` : host/service 문자열 <-> `struct sockaddr` 변환 (프로토콜 독립).
- `setsockopt(SO_REUSEADDR, 1)` : 서버 재시작 시 `Address already in use` 방지.
- CSAPP 의 래퍼 `open_clientfd` / `open_listenfd` 는 위 루틴을 각각 한 줄로 묶은 것.

전체 상태 전이는 이렇다.

```text
서버 소켓
  created -> bind -> LISTEN -> (SYN 받음) -> (3WHS 완료, accept 큐에 올림)
          -> accept -> ESTABLISHED connfd 하나 반환

클라 소켓
  created -> connect 호출 -> SYN_SENT -> (SYN-ACK 받음) -> ESTABLISHED
```

> `getaddrinfo` 를 먼저 부른 뒤 `socket`, `connect` 를 차례로 실행한다는 말은 구체적으로 어떤 코드인가.

CSAPP 의 `open_clientfd` 와 거의 똑같은 형태로 쓰면 이렇게 된다.

```c
int open_clientfd(char *hostname, char *port)
{
    int clientfd;
    struct addrinfo hints, *listp, *p;

    /* 1) hints 로 원하는 소켓 모양을 지정 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;      /* TCP */
    hints.ai_flags    = AI_NUMERICSERV;   /* port 는 숫자 문자열 */
    hints.ai_flags   |= AI_ADDRCONFIG;    /* 내 호스트에 설정된 AF 만 */

    /* 2) hostname + port -> addrinfo linked list */
    Getaddrinfo(hostname, port, &hints, &listp);

    /* 3) 리스트를 순회하면서 socket + connect 시도 */
    for (p = listp; p; p = p->ai_next) {
        if ((clientfd = socket(p->ai_family,
                               p->ai_socktype,
                               p->ai_protocol)) < 0)
            continue;   /* 이 조합은 생성 실패 -> 다음 후보 */

        if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
            break;      /* 성공 -> 루프 탈출 */

        Close(clientfd); /* 실패 -> 닫고 다음 후보 */
    }

    /* 4) 반드시 freeaddrinfo */
    Freeaddrinfo(listp);

    if (!p)         /* 모든 후보 실패 */
        return -1;
    return clientfd;
}
```

왜 이런 모양이 되는지 의미를 덧붙이면:

- `getaddrinfo` 는 **host + service 문자열만 알면 알아서 DNS 도 돌리고 `sockaddr` 도 채워주는** "준비물 공장"이다. 결과로 후보가 여러 개(IPv4, IPv6, 여러 DNS 응답) 올 수 있어 **연결 리스트**로 돌려준다.
- `socket()` 은 준비물의 `(ai_family, ai_socktype, ai_protocol)` 을 그대로 받아서 fd 만 만든다.
- `connect()` 는 준비물의 `(ai_addr, ai_addrlen)` 을 그대로 써서 서버로 연결한다.

즉 "getaddrinfo -> socket -> connect" 는 **준비물 생성 -> 도구 만들기 -> 실제 연결** 의 세 단계이고, 서버면 connect 대신 bind/listen/accept 가 들어간다.

> `struct addrinfo` 의 각 필드는 무슨 뜻이고 어떤 역할을 하는가.

Figure 11.16 의 정의를 다시 써보면:

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

필드별 의미와 실제로 어떻게 쓰이는지 정리:

```text
ai_flags     | hints 로 "원하는 결과"를 지정할 때 쓰는 비트 플래그
             | AI_PASSIVE       -> bind 용 wildcard 주소(INADDR_ANY) 반환
             | AI_ADDRCONFIG    -> 로컬에 구성된 AF 만 반환 (IPv6 없으면 IPv6 제외)
             | AI_NUMERICSERV   -> service 인자를 숫자 포트로만 해석
             | AI_NUMERICHOST   -> hostname 을 IP 문자열로만 해석, DNS 안 함
             | AI_V4MAPPED      -> IPv6 소켓에서 IPv4-mapped 주소 반환

ai_family    | 주소 체계
             | AF_INET   = IPv4
             | AF_INET6  = IPv6
             | AF_UNSPEC = 둘 다 허용 (보통 이걸 권장)
             | -> socket() 의 첫 번째 인자로 그대로 넘긴다

ai_socktype  | 소켓 타입
             | SOCK_STREAM = TCP
             | SOCK_DGRAM  = UDP
             | SOCK_RAW    = raw (IP/ICMP 직접)
             | -> socket() 의 두 번째 인자

ai_protocol  | 프로토콜
             | 보통 0 (default: STREAM->TCP, DGRAM->UDP)
             | IPPROTO_TCP, IPPROTO_UDP 로 명시할 수도 있음
             | -> socket() 의 세 번째 인자

ai_canonname | 호스트 이름의 "canonical" 형태 (CNAME 풀어낸 실제 이름)
             | AI_CANONNAME 플래그 줬을 때만 채워지고, 보통 NULL

ai_addrlen   | ai_addr 의 크기 (IPv4 sockaddr_in=16B, IPv6 sockaddr_in6=28B)
             | -> connect/bind 의 addrlen 인자로 그대로 넘긴다

ai_addr      | 실제 소켓 주소 구조체 포인터 (struct sockaddr*)
             | family 에 따라 내부가 sockaddr_in 또는 sockaddr_in6
             | 포트와 IP 가 network byte order 로 이미 채워져 있음
             | -> connect/bind 의 addr 인자

ai_next      | 다음 후보 노드를 가리키는 포인터
             | DNS 가 여러 IP 를 주거나, IPv4/IPv6 둘 다일 때
             | 리스트로 돌려준다
             | -> 순회하면서 첫 번째로 성공하는 조합을 씀
```

"hints 의 값" 과 "결과 addrinfo 의 값" 이 같은 구조체를 쓰는 게 처음엔 혼란스러운데, **호출자가 빈 구조체에 원하는 필터(flags/family/socktype)만 채워서 주면, 커널이 그걸 만족하는 후보들을 **연결 리스트**로 돌려준다**고 생각하면 된다. 그래서 결과를 다 쓰고 나면 반드시 `freeaddrinfo(listp)` 로 리스트를 해제해야 누수가 없다.

## 연결 키워드

- [07-ch11-code-reference.md — 11.4 모든 함수 원형](../../csapp-11/07-ch11-code-reference.md)
- q03. 소켓 시스템콜 내부
- q08. accept 뒤에 read/write 로 이어지는 에코 서버
- q12. Tiny 가 open_listenfd 를 어떻게 쓰는지
