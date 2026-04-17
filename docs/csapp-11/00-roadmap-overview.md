# 00. CSAPP 11 전체 로드맵

이 페이지의 목적은 세 가지입니다.

- `CSAPP` 11장 전체를 한 장에서 본다
- 어디까지 학습했는지 체크한다
- 동료와 화이트보드로 설명할 때 공통 뼈대로 사용한다

## 사용 방법

- 발표 전:
  전체 흐름을 한 번 훑는다
- 학습 중:
  각 노드가 설명 가능한지 체크한다
- 화이트보드 설명 시:
  가운데에서 바깥으로 확장하며 설명한다

## 한 장 로드맵 (세로 키워드 트리)

```mermaid
graph TD
    A["CSAPP 11: Network Programming"]

    A --> B["11.1 Client-Server Model"]
    B --> B1["client / server / resource"]
    B --> B2["transaction: request -> process -> response"]

    B2 --> C["11.2 Networks"]
    C --> C1["LAN: Ethernet, hub, bridge, frame, MAC"]
    C --> C2["WAN / router / internet"]
    C --> C3["protocol: naming + delivery"]
    C --> C4["encapsulation: data -> packet -> frame"]

    C4 --> D["11.3 Global IP Internet"]
    D --> D1["IP address: 32-bit, network byte order"]
    D1 --> D1a["htonl/ntohl, inet_pton/inet_ntop"]
    D --> D2["DNS: domain name -> IP"]
    D --> D3["connection: point-to-point, full duplex, reliable"]
    D3 --> D3a["socket = IP:port"]
    D3a --> D3b["socket pair = 4-tuple로 연결 식별"]

    D3b --> E["11.4 Sockets Interface"]
    E --> E1["서버: socket -> bind -> listen -> accept"]
    E --> E2["클라이언트: socket -> connect"]
    E --> E3["listenfd vs connfd"]
    E --> E4["getaddrinfo / getnameinfo"]
    E --> E5["open_clientfd / open_listenfd"]
    E5 --> E6["Echo client/server: iterative, robust I/O"]

    E6 --> F["11.5 Web Servers"]
    F --> F1["HTTP: text-based request/response"]
    F --> F2["URL: host:port/path?args"]
    F --> F3["static content: 파일 반환"]
    F --> F4["dynamic content: CGI 실행 결과 반환"]
    F4 --> F4a["CGI: fork + execve + dup2 + QUERY_STRING"]

    F4a --> G["11.6 Tiny Web Server"]
    G --> G1["main: listen -> accept loop"]
    G --> G2["doit: request 파싱 -> 분기"]
    G --> G3["parse_uri: static vs dynamic 판별"]
    G --> G4["serve_static: mmap -> write"]
    G --> G5["serve_dynamic: fork -> execve"]

    G5 --> H["이번 주 확장"]
    H --> H1["Echo server: 소켓 기초 확인"]
    H --> H2["Tiny 완성 + 숙제 문제"]
    H --> H3["Proxy Lab"]
    H3 --> H3a["sequential -> concurrent -> cache"]
    H --> H4["SQL API Server"]
    H4 --> H4a["thread pool + DB engine + 병렬 처리"]
```

## 화이트보드 설명 순서

아래 순서대로 설명하면 흐름이 가장 자연스럽습니다.

1. `11.1`
   클라이언트와 서버가 무엇인지
2. `11.2`
   요청 데이터가 네트워크를 어떻게 타는지
3. `11.3`
   이름, 주소, 포트, 연결이 어떻게 이어지는지
4. `11.4`
   코드에서 어떤 함수로 연결을 만드는지
5. `11.5`
   HTTP 서버는 무엇을 주고받는지
6. `11.6`
   Tiny가 이 개념을 어떻게 코드로 합치는지
7. `11.7`
   이 지식을 Proxy와 SQL API 서버로 어떻게 확장하는지

## 완료 체크

아래 항목을 설명할 수 있으면 전체 로드맵을 따라간 것입니다.

- `client`, `server`, `resource`, `transaction`
- `LAN`, `router`, `packet`, `frame`, `encapsulation`
- `IP`, `DNS`, `port`, `socket address`, `connection`
- `socket`, `connect`, `bind`, `listen`, `accept`
- `getaddrinfo`, `open_clientfd`, `open_listenfd`
- `URL`, `HTTP request`, `HTTP response`, `header`, `body`
- `static`, `dynamic`, `CGI`
- `Tiny main`, `doit`, `parse_uri`, `serve_static`, `serve_dynamic`
- `Proxy`, `concurrency`, `cache`
- `SQL API server`로의 연결

## 가장 중요한 한 줄

```text
이름을 주소로 바꾸고
주소로 연결을 만들고
연결 위에 HTTP를 흘리고
그 HTTP를 코드로 처리하면 Tiny / Proxy / API 서버가 된다
```
