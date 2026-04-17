# Q09. HTTP / FTP / MIME / Telnet, HTTP 1.0 vs 1.1

> CSAPP 11.5 | 애플리케이션 프로토콜 | 기본

## 질문

1. FTP 는 무엇이고 HTTP 와 무엇이 다른가.
2. MIME 타입이란 무엇이며 왜 필요한가.
3. Telnet 은 무엇이며, 왜 "모든 인터넷 프로토콜의 트랜잭션을 실행해볼 수 있다" 고 말하는가.
4. HTTP/1.0 과 HTTP/1.1 의 차이는 무엇인가.

## 답변

### 최우녕

> FTP 는 무엇이고 HTTP 와 무엇이 다른가.

FTP(File Transfer Protocol, RFC 959)는 **파일 전송 전용** 프로토콜이다. HTTP(HyperText Transfer Protocol, RFC 2616 등)는 하이퍼텍스트 문서를 요청/응답으로 주고받는 **범용 애플리케이션 프로토콜**이다. 둘 다 TCP 위에서 텍스트 기반 명령으로 동작하지만 설계 철학이 다르다.

```text
                   FTP                                HTTP
-----------------------------------------------------------------------
포트              제어 21, 데이터 20 (active)          80, 443(TLS)
                  또는 passive 동적 포트
연결 수           두 개 (control + data)              기본 한 개
상태              세션 기반 (USER, PASS, CWD...)      stateless (쿠키로 보완)
명령              ASCII 명령: USER, RETR, STOR, LIST  METHOD URI VERSION + 헤더
응답              3자리 상태코드 + 텍스트 (예: 220)   3자리 상태코드 + 헤더 + body
용도              파일 업/다운로드 전용                웹 전반(HTML, 이미지, API)
특수              Active/Passive 모드 (NAT 어려움)    CONNECT, WebSocket 등 확장 많음
```

핵심 차이:

- FTP 는 **제어 연결**(명령/응답) 과 **데이터 연결**(실제 파일) 이 분리되어 있어 NAT/방화벽 친화적이지 않다. HTTP 는 한 연결에 다 담아서 단순하다.
- FTP 는 **stateful**. "지금 어떤 디렉터리인지", "로그인 됐는지" 를 서버가 기억한다. HTTP 는 매 요청 독립적이다.
- HTTP 는 **MIME 타입** 으로 다양한 종류의 콘텐츠를 실어 나를 수 있어서, 하이퍼텍스트 뿐 아니라 API, 파일 다운로드까지 다 커버한다. 오늘날 FTP 자리는 SFTP/SCP/Object Storage 로 거의 대체됐다.

> MIME 타입이란 무엇이며 왜 필요한가.

MIME(Multipurpose Internet Mail Extensions) 타입은 원래 이메일이 텍스트뿐 아니라 이미지, 첨부 파일을 실어 보내도록 만든 표준인데, HTTP 가 그대로 이어받았다. **"이 바이트 덩어리가 어떤 종류의 콘텐츠인가"** 를 서버가 명시하는 레이블이다.

HTTP 응답에서는 `Content-Type` 헤더로 전달한다.

```text
Content-Type: text/html; charset=utf-8
Content-Type: image/png
Content-Type: application/json
Content-Type: video/mpeg
Content-Type: application/octet-stream
```

형식은 `type/subtype` 이다. `text`, `image`, `audio`, `video`, `application`, `multipart` 등의 대분류가 있다.

왜 필요한가:

- **브라우저가 어떻게 렌더링할지 결정**한다. `text/html` 이면 HTML 파서로 보여주고, `image/png` 면 이미지로 그리고, `application/octet-stream` 이면 저장 다이얼로그를 띄운다.
- **파일 확장자에만 의존하지 않는다**. URL 이 `.html` 로 끝나도 `Content-Type: text/plain` 이면 브라우저는 텍스트로 보여준다.
- **보안**. `X-Content-Type-Options: nosniff` 와 결합해서 브라우저가 임의로 타입을 추측하지 않도록 막는다.

Tiny 서버의 `get_filetype()` 함수가 하는 일이 정확히 이 매핑이다: 확장자를 보고 `Content-Type` 값을 결정해서 응답 헤더에 넣는다.

> Telnet 은 무엇이며, 왜 "모든 인터넷 프로토콜의 트랜잭션을 실행해볼 수 있다" 고 말하는가.

Telnet 은 원래 **원격 로그인 프로토콜** (포트 23)이다. 지금은 보안 문제로 거의 쓰지 않지만 (대신 SSH), **`telnet` 이라는 CLI 도구는 여전히 유용**하다. 이유는 이 도구가 "지정한 호스트:포트로 TCP 소켓을 열고, stdin 을 그 소켓에 그대로 밀어넣고, 소켓에서 읽은 걸 stdout 에 그대로 찍어주는" 동작을 하기 때문이다.

즉 **TCP 기반 텍스트 프로토콜이면 뭐든 사람 손으로 칠 수 있는 범용 클라이언트**가 된다. HTTP, SMTP, POP3, IMAP, Redis 의 RESP 같은 것들 전부.

예를 들어 HTTP 를 손으로 쳐보면:

```text
$ telnet www.example.net 80
Trying 208.216.181.15...
Connected to www.example.net.
Escape character is '^]'.
GET /home.html HTTP/1.0
Host: www.example.net

HTTP/1.0 200 OK
Server: Tiny Web Server
Content-length: 2048
Content-type: text/html

<html>...
```

SMTP 도 같은 방식으로 된다:

```text
$ telnet mail.example.com 25
220 mail.example.com ESMTP ready
HELO test
250 Hello
MAIL FROM:<a@x.com>
250 OK
...
```

그래서 CSAPP 11.5 가 "Telnet 으로 HTTP 요청을 직접 쳐 보라" 고 하는 것이다. 소켓 한 개로 양방향 텍스트를 주고받는 구조이기에 프로토콜 디버깅 도구로 완벽하다. 요즘은 `nc`(netcat), `curl -v`, `openssl s_client` (TLS 용) 가 같은 역할을 한다.

> HTTP/1.0 과 HTTP/1.1 의 차이는 무엇인가.

핵심만 뽑으면:

```text
                    HTTP/1.0                        HTTP/1.1
--------------------------------------------------------------------------
기본 연결           요청마다 새 TCP (close)          persistent (keep-alive)
Host 헤더           선택                             필수 (가상 호스팅)
파이프라이닝        X                                O (순서 보장 필요)
chunked encoding    X                                O (Transfer-Encoding: chunked)
캐시 제어           Expires 위주                     Cache-Control 세분화
range 요청          제한적                           Range/Content-Range
추가 메서드         GET/HEAD/POST                    + PUT, DELETE, OPTIONS, TRACE
호스트당 연결 수    보통 1                           2~6 병렬 (브라우저마다 상이)
```

CSAPP 가 11.6 Tiny 에서 다루는 과제는 **1.1 요청을 받아도 1.0 으로 응답**하는 형태다. 이유는 구현 단순성 때문이다. 1.0 으로 응답하면 매 요청마다 연결이 끊기므로, `Content-Length` 만 정확히 쓰고 `Connection: close` 만 지키면 된다. 1.1 을 완벽히 구현하려면 keep-alive 상태 관리, chunked encoding, 파이프라이닝 에러 처리가 추가로 필요하다.

참고로 HTTP/2 는 바이너리 프레이밍 + 멀티플렉싱, HTTP/3 는 UDP 기반 QUIC 위로 옮긴 것이다. 실무에서는 오리진 서버가 HTTP/1.1 로 말해도 Cloudflare 같은 프록시가 클라이언트와는 HTTP/2/3 으로 말해주는 식이 흔하다.

## 연결 키워드

- [02-keyword-tree.md — 11.5 Web Servers](../../csapp-11/02-keyword-tree.md)
- q11. CGI (동적 콘텐츠)
- q12. Tiny 의 응답 헤더/Content-Type 구현
- q13. Proxy 의 HTTP/1.1 → 1.0 변환
