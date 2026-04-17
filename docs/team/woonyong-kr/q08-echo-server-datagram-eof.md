# Q08. Echo Server, Datagram, EOF, 파일 I/O 와의 유사성

> CSAPP 11.4 | 소켓으로 실제 통신해보기 | 기본

## 질문

1. "데이터그램(datagram)" 은 무엇인가. 패킷, 프레임, 세그먼트와 어떻게 다른가.
2. 책에서 말하는 **에코 서버(echo server)** 는 무엇이고, 소켓으로 어떻게 구현되는가.
3. 네트워크 통신은 파일 입출력과 비슷하고, 그래서 EOF 가 중요하다는데, 그 과정을 자세히 설명해 달라.

## 답변

### 최우녕

> "데이터그램(datagram)" 은 무엇인가. 패킷, 프레임, 세그먼트와 어떻게 다른가.

데이터그램은 **한 번에 주소를 달아서 독립적으로 보내는 하나의 메시지 단위**다. 각 계층에서 쓰는 용어는 겹치지만 구분해서 쓰면 이렇다.

```text
L2 (링크)          frame       : 이더넷 프레임. MAC + EtherType + payload + FCS
L3 (네트워크)      packet      : IP 패킷. 좁게 쓰면 IP datagram
                                (IP는 원래 datagram 프로토콜이라 "IP datagram" 이 정식 명칭)
L4 (전송)
    TCP            segment     : TCP 세그먼트. byte stream 을 잘라 붙인 단위
    UDP            datagram    : UDP datagram. **그 자체가 메시지의 최소 단위**
```

정리: **datagram 은 "독립적으로 배달되는 메시지 한 개" 라는 개념**이다. UDP 와 IP 는 연결 없이 datagram 단위로 전달하므로 **경계가 보존**된다(송신자가 100B 한 번 보내면 수신자가 정확히 100B 하나로 받는다). 반면 TCP 는 byte stream 이라 **경계가 없다**(송신자가 50B 두 번 보내도 수신자는 100B 한 번으로 받을 수 있다). 이 차이가 소켓 API 에서 `sendto/recvfrom` (UDP) vs `read/write` (TCP) 의 쓰임을 결정한다.

> 책에서 말하는 **에코 서버(echo server)** 는 무엇이고, 소켓으로 어떻게 구현되는가.

에코 서버는 **클라이언트가 보낸 것을 그대로 돌려보내는** 가장 단순한 네트워크 프로그램이다. 네트워크 프로그래밍을 배울 때 제일 먼저 만드는 예제고, 소켓 호출이 제대로 작동하는지 확인하는 용도로 쓴다.

CSAPP 책에 나오는 에코 서버를 뼈대만 써보면:

```c
/* echoserver.c 의 핵심 루프 */
int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;   /* protocol-independent */
    char client_hostname[MAXLINE], client_port[MAXLINE];

    listenfd = Open_listenfd(argv[1]);    /* = socket + bind + listen */

    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Accept(listenfd, (SA*)&clientaddr, &clientlen);

        Getnameinfo((SA*)&clientaddr, clientlen,
                    client_hostname, MAXLINE,
                    client_port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", client_hostname, client_port);

        echo(connfd);                     /* 진짜 일은 여기서 */
        Close(connfd);
    }
}

void echo(int connfd)
{
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes\n", (int)n);
        Rio_writen(connfd, buf, n);       /* 그대로 돌려보냄 */
    }
}
```

클라이언트 쪽도 대칭이다.

```c
int main(int argc, char **argv)
{
    int clientfd;
    char *host = argv[1], *port = argv[2];
    char buf[MAXLINE];
    rio_t rio;

    clientfd = Open_clientfd(host, port);  /* = socket + connect */
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

포인트:

- `Open_listenfd` / `Open_clientfd` 는 CSAPP 의 래퍼다 (q07 참고).
- 에코 서버는 **iterative server** 다. `while` 루프 한 번에 클라이언트 한 명만 처리한다. 다중 연결을 위해선 fork/pthread/select/epoll 로 확장해야 한다(→ q14).
- `rio_readlineb` 는 "\n 까지 읽는" 버퍼드 I/O. `rio_writen` 은 "원하는 길이를 다 쓸 때까지 반복" 하는 robust writer. 짧은 read/write 를 방지한다.

> 네트워크 통신은 파일 입출력과 비슷하고, 그래서 EOF 가 중요하다는데, 그 과정을 자세히 설명해 달라.

CSAPP 가 10장(Unix I/O) 바로 뒤에 11장(네트워크)을 놓은 이유가 바로 이것이다. 네트워크 소켓은 **파일 디스크립터와 동일한 인터페이스(`read/write/close`)** 를 쓴다. 다만 "EOF" 의 의미가 파일과 미묘하게 다르다.

**파일에서의 EOF**: `read()` 가 0 을 돌려준 순간. 파일 끝에 도달한 것이다.

**소켓에서의 EOF**: `read()` 가 0 을 돌려준 순간. 이건 **상대가 자기 쪽 송신(FIN)을 닫았다** 는 의미이지, "데이터가 영원히 없다" 는 뜻이 아니다. TCP 는 양방향(full duplex) 이므로, 상대가 FIN 을 보내도 내 쪽에서 write 는 가능하다. 그래서 흔히 "half close" 라고 부른다.

```text
클라이언트                                    서버
───────                                      ────
write(connfd, "hello", 5)                    read → "hello"
                                             write("hello", 5)
read → "hello"                               ...
close(clientfd)  ── FIN ──▶                  read → 0  (EOF!)
                                             [서버는 이제 더 읽을 게 없음을 안다]
                                             close(connfd)  ── FIN ──▶
read → 0 (이미 끝)
```

에코 서버의 루프가 `while ((n = rio_readlineb(...)) != 0)` 인 이유가 여기다. `n == 0` 이면 클라이언트가 연결을 닫았다는 뜻이므로 루프를 빠져나와 `close(connfd)` 하고 다음 클라이언트를 받는다.

또 중요한 차이 세 가지:

- **short read / short write 가능성**: 파일에서도 가능하지만 네트워크에서는 흔하다. `write(1000)` 이 400 만 쓰고 돌아올 수 있다. 그래서 `rio_writen` 으로 감싸서 "원하는 만큼 쓸 때까지 반복" 하는 패턴이 필요하다.
- **blocking**: read 가 할 일이 없으면 커널이 프로세스를 sleep 시킨다. non-blocking fd 나 `select/poll/epoll` 로 대기를 제어할 수 있다.
- **에러의 종류**: 파일은 `EIO` 정도지만 소켓은 `ECONNRESET`, `EPIPE`, `ETIMEDOUT` 등 연결 상태에서 오는 에러가 많다. 특히 `EPIPE` 는 "상대가 FIN 을 이미 보냈는데 내가 write 했다" 는 상황이며, SIGPIPE 시그널도 같이 발생한다. 서버에서는 보통 `signal(SIGPIPE, SIG_IGN)` 해두고 `write` 의 errno 로 판단한다(CSAPP 11.13 숙제).

즉 네트워크 I/O 는 "파일 I/O + 프로토콜 에러 + 부분 전송" 을 같이 다뤄야 하고, 그 중심 신호가 EOF(=`read == 0`)다.

## 연결 키워드

- [07-ch11-code-reference.md — Figure 11.20 ~ 11.22 echo 관련](../../csapp-11/07-ch11-code-reference.md)
- q07. Open_listenfd 내부
- q12. Tiny 의 doit 루프가 에코 서버와 구조적으로 같다
- q14. iterative → concurrent 로 확장
