# Q13. Proxy — Tiny 를 프록시로 확장하는 관점

> CSAPP 11 확장(Proxy Lab) | 프록시 서버 | 중급

## 질문

1. 프록시는 CSAPP 11장의 본문에는 거의 등장하지 않는데, Tiny 와 어떻게 연결되는가.
2. 프록시의 역할과 배치 방식은 어떤가.
3. Proxy Lab 관점에서 Tiny 를 프록시로 바꾸려면 무엇을 어떻게 추가/변경해야 하는가.

## 답변

### 최우녕

> 프록시는 CSAPP 11장의 본문에는 거의 등장하지 않는데, Tiny 와 어떻게 연결되는가.

프록시(proxy)는 **클라이언트와 오리진 서버 사이에 끼어서 요청을 대신 받고, 상위 서버로 요청을 대신 보내는 중계 서버**다. 본문엔 구조만 슬쩍 소개되지만, Proxy Lab 은 Tiny 를 기반으로 "**요청을 읽고 -> 파싱하고 -> 다른 서버에 다시 연결해 보내고 -> 응답을 받아 클라이언트에 돌려주는**" 서버를 직접 만든다. 즉 **Tiny 의 `main` 루프 + `doit` 구조를 그대로 쓰되, `serve_static/serve_dynamic` 대신 "위쪽 서버에 연결해서 릴레이"** 로 바꾸는 일이다.

```text
클라이언트                 프록시 (내가 만드는 서버)           오리진 서버 (예: Tiny)
    │        HTTP 요청        │                                          │
    ├───────────────────────>│                                          │
    │                         │  URL 파싱 -> host:port 추출               │
    │                         │  open_clientfd(host, port)               │
    │                         │  (HTTP/1.1 -> HTTP/1.0 변환,               │
    │                         │   Connection: close 강제 등)              │
    │                         │                                          │
    │                         │         HTTP 요청 (변환본)               │
    │                         ├─────────────────────────────────────────>│
    │                         │                                          │
    │                         │         HTTP 응답                         │
    │                         │<─────────────────────────────────────────┤
    │                         │                                          │
    │        HTTP 응답         │                                          │
    │<────────────────────────┤                                          │
```

이 구조를 보면 **프록시는 "동시에 서버이자 클라이언트"** 라는 점이 드러난다. Tiny 에서 배운 `open_listenfd` 와 `open_clientfd` 를 한 프로세스 안에서 같이 쓰는 것이 핵심.

> 프록시의 역할과 배치 방식은 어떤가.

역할은 대표적으로 네 가지다.

- **포워드 프록시 (Forward proxy)**: 사내망에서 바깥 인터넷으로 나갈 때 통과하는 프록시. 필터링, 감사, 캐싱. 클라이언트가 프록시 주소를 명시적으로 설정한다.
- **리버스 프록시 (Reverse proxy)**: 외부에서 들어오는 요청을 내부 서버들로 분배. nginx, Cloudflare, AWS ALB 등이 대표. 클라이언트는 프록시가 진짜 서버인 줄 안다.
- **캐싱 프록시**: 이전 응답을 저장했다가 같은 요청에 다시 쓴다. Squid, Varnish, 브라우저 앞단의 CDN 이 이것.
- **터널 프록시**: `CONNECT` 메서드로 TCP 파이프를 만들어 TLS 트래픽을 통째로 중계(HTTPS 프록시).

배치 방식:

```text
[ 포워드 ]   사내 PC ──> 포워드 프록시 ──> 인터넷 ──> 서버
[ 리버스 ]   인터넷 ──> 리버스 프록시 ──> 내부 서버 (여러 대)
[ CDN ]      클라이언트 ──> CDN 엣지(캐싱 프록시) ──> 오리진
```

CSAPP Proxy Lab 은 주로 **포워드 + 캐싱 프록시** 를 만든다. 이유는 학습 목적상 "HTTP 요청을 만들고 받는 양쪽 모두를 구현" 하는 게 핵심이기 때문이다.

> Proxy Lab 관점에서 Tiny 를 프록시로 바꾸려면 무엇을 어떻게 추가/변경해야 하는가.

Tiny 와 비교해서 바뀌는 부분만 정리하면 이렇다.

```text
main               <- Tiny 와 동일. open_listenfd + while(accept, 처리, close)

doit
  ┌─ Rio_readlineb 로 요청 라인 읽기         <- 동일
  │
  ├─ 요청 파싱
  │     요청 라인: GET <URL> HTTP/1.1
  │     여기서 URL 이 절대 URL 이라는 점이 Tiny 와 다르다:
  │     예) GET http://www.example.net:80/home.html HTTP/1.1
  │     -> host, port, path 로 쪼갠다
  │
  ├─ 헤더 읽기 + 변환
  │     - HTTP/1.1 -> HTTP/1.0 으로 버전 교체
  │     - Connection: close 강제
  │     - Proxy-Connection: close 강제
  │     - Host: 없으면 추가
  │     - User-Agent 를 과제에서 고정값으로 덮는 경우도 있음
  │
  ├─ open_clientfd(host, port) 로 오리진에 연결
  │
  ├─ 변환된 요청을 오리진에 Rio_writen
  │
  ├─ 오리진 응답을 Rio_readnb 로 읽으면서 connfd 에 그대로 Rio_writen
  │     (캐시 있는 버전은 이때 객체를 메모리에 함께 복사)
  │
  └─ 양쪽 fd 모두 close
```

캐시를 붙이면:

```text
- 요청 파싱 후 캐시 lookup (key = URL 또는 host+path)
- HIT 이면 바로 클라이언트에 응답 본문을 쏴주고 끝
- MISS 이면 오리진에서 가져와 응답을 전달하면서 동시에 메모리에 저장
- 크기 초과면 MAX_OBJECT_SIZE 기준으로 건너뜀
- 가득 차면 LRU 로 eviction
- 공유 자료구조이므로 readers-writers lock 필요
```

동시성을 붙이면 (Proxy Lab 2단계):

```text
- main: accept 만 담당, connfd 를 작업 큐에 넣거나 pthread_create 로 바로 분사
- worker: detached thread (join 필요 없음)
- 각 worker 가 doit(connfd) 를 호출하고 close
```

즉 Proxy 는 Tiny 에서 **"응답을 어떻게 만드느냐" 부분만 "상위 서버로부터 가져와라"** 로 바뀐 것이고, 나머지 뼈대는 그대로 재활용된다. 이 점이 CSAPP 가 11장을 이렇게 쌓은 이유이기도 하다. 이번 주 SQL API 서버도 이 패턴을 정확히 반대로 적용한다(상위 서버 대신 "DB 엔진이 응답을 만든다").

## 연결 키워드

- [02-keyword-tree.md — Proxy Lab](../../csapp-11/02-keyword-tree.md)
- q07. open_clientfd / open_listenfd 내부
- q09. HTTP/1.1 -> HTTP/1.0 변환 이유
- q12. Tiny 의 doit 루프 구조
- q14. 프록시의 스레드 풀 구조
