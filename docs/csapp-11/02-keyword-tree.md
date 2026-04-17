# 02. 학습 키워드 트리

모든 키워드를 하나의 종속 관계 트리로 정리한 문서입니다.
위에서 아래로 읽으면 개념이 어떻게 쌓이는지 보입니다.

## 전체 키워드 트리

CSAPP 11: Network Programming

11.1 Client-Server Model

* client: 서비스를 요청하는 프로세스
* server: 자원을 관리하고 서비스하는 프로세스
* transaction
   * 1단계: client sends request
   * 2단계: server processes request
   * 3단계: server sends response
   * 4단계: client processes response

11.2 Network Hardware

* LAN
   * Ethernet segment
      * hub: 모든 포트에 비트 복사
      * frame: 헤더 + payload
      * MAC address: 48-bit 고유 주소
   * bridged Ethernet
      * bridge: 포트별 선택 전달
* WAN
* router
   * 서로 다른 LAN/WAN을 연결
   * routing table로 패킷 전달
* internet = LAN + WAN + router
   * protocol: 이기종 네트워크를 통합하는 규약
      * naming scheme: 균일한 주소 형식
      * delivery mechanism: packet 단위 전달
   * encapsulation
      * data → internet packet → LAN frame

11.3 Global IP Internet

* IP address
   * 32-bit unsigned integer
   * struct in_addr
   * network byte order: big-endian
      * htonl / htons: host → network
      * ntohl / ntohs: network → host
   * dotted-decimal notation
      * inet_pton: 문자열 → binary
      * inet_ntop: binary → 문자열
* DNS
   * domain name: 사람이 읽는 주소
      * 계층 구조: root > com/edu > cmu > cs
   * DNS lookup: domain → IP
      * nslookup / dig 명령
   * localhost = 127.0.0.1
* Internet connection
   * point-to-point
   * full duplex: 양방향 동시 통신
   * reliable: 순서 보장
   * socket: 연결의 끝점
      * socket address = IP:port
      * ephemeral port: 클라이언트에 커널이 자동 할당
      * well-known port: 서버 고정 포트
         * 80 = HTTP, 25 = SMTP, 443 = HTTPS
   * socket pair
      * cliaddr:cliport, servaddr:servport
      * 연결을 유일하게 식별하는 4-tuple

11.4 Sockets Interface

* TCP/IP
   * IP: host-to-host 패킷 전달, 비신뢰
   * UDP: process-to-process, 비신뢰
   * TCP: 신뢰성 있는 양방향 연결
* socket address 구조체
   * sockaddr_in: AF_INET + port + IP
   * sockaddr: 범용 구조체, 캐스팅용
   * sockaddr_storage: 프로토콜 독립적
* 서버 생명주기
   * socket: 소켓 디스크립터 생성
      * AF_INET + SOCK_STREAM + 0
   * bind: 소켓에 주소 바인딩
      * 커널에게 이 주소로 요청 받겠다고 알림
   * listen: passive 소켓으로 전환
      * backlog: 대기 큐 크기 힌트
   * accept: 연결 수락, connfd 반환
      * listenfd: 연결 요청 대기용, 서버 수명 동안 유지
      * connfd: 실제 통신용, 요청 처리 후 close
   * read / write: 데이터 송수신
   * close: 연결 종료
* 클라이언트 생명주기
   * socket: 소켓 디스크립터 생성
   * connect: 서버에 연결 요청
      * 성공 시 clientfd로 읽기/쓰기 가능
   * read / write
   * close
* getaddrinfo
   * hostname + service → addrinfo linked list
   * 프로토콜 독립적: IPv4/IPv6 모두 지원
   * hints로 필터링
      * AI_PASSIVE: 서버용 wildcard 주소
      * AI_ADDRCONFIG: 로컬 설정 기반 필터
      * AI_NUMERICSERV: 포트 번호 강제
   * freeaddrinfo: 메모리 해제 필수
* getnameinfo
   * sockaddr → hostname + service 문자열
   * NI_NUMERICHOST: IP 문자열 반환
* helper 함수
   * open_clientfd: socket + connect 래핑
   * open_listenfd: socket + bind + listen 래핑
      * SO_REUSEADDR: Address already in use 방지
* Echo client/server
   * iterative server: 한 번에 한 클라이언트
   * rio_readlineb / rio_writen: robust I/O
   * EOF: read가 0을 반환하는 조건

11.5 Web Servers

* HTTP
   * text-based application-level protocol
   * request-response 구조
* Web content
   * MIME type
      * text/html, text/plain, image/png, image/gif, image/jpeg
   * static content: 디스크 파일 반환
   * dynamic content: 프로그램 실행 결과 반환
* URL
   * http://host:port/path?args
   * 클라이언트: host:port로 서버 위치 결정
   * 서버: /path로 파일 위치 결정
   * ? 뒤: CGI 인자, & 로 구분
* HTTP request
   * request line: method URI version
      * GET /index.html HTTP/1.1
   * request headers: key: value
      * Host: 필수 (HTTP/1.1)
      * User-Agent, Connection, Proxy-Connection
   * 빈 줄 (CRLF): 헤더 종료
   * request body: POST에서 사용
* HTTP response
   * response line: version status-code status-message
      * HTTP/1.0 200 OK
   * response headers
      * Content-Type: 응답 MIME 타입
      * Content-Length: 응답 body 바이트 수
   * 빈 줄: 헤더 종료
   * response body: 실제 콘텐츠
* CGI
   * 서버가 fork → execve로 프로그램 실행
   * QUERY_STRING 환경변수로 인자 전달
   * dup2로 stdout을 connfd에 연결
   * CGI 프로그램이 Content-type, Content-length 직접 출력
   * 환경변수: SERVER_PORT, REQUEST_METHOD, REMOTE_HOST 등

11.6 Tiny Web Server

* main
   * open_listenfd로 리스닝 시작
   * while(1): accept → doit → close
* doit
   * request line 읽기 + 파싱
   * GET만 지원, 나머지는 501 에러
   * read_requesthdrs: 헤더 소비
   * parse_uri 호출
   * static이면 serve_static
   * dynamic이면 serve_dynamic
* parse_uri
   * URI에 cgi-bin 포함 → dynamic
   * 아니면 → static
   * filename과 cgiargs 분리
* serve_static
   * stat으로 파일 존재/권한 확인
   * get_filetype으로 MIME 결정
   * 응답 헤더 전송
   * mmap으로 파일을 메모리에 매핑
   * rio_writen으로 클라이언트에 전송
   * munmap으로 매핑 해제
* serve_dynamic
   * 응답 라인 + Server 헤더 전송
   * fork로 자식 프로세스 생성
   * setenv로 QUERY_STRING 설정
   * dup2로 stdout → connfd 리다이렉트
   * execve로 CGI 프로그램 실행
   * 부모는 wait로 자식 종료 대기

이번 주 확장

* Echo server
   * 소켓 기초 확인
* Tiny 완성
   * static + dynamic 서빙 동작 확인
* 숙제 문제
   * 11.6c: MIME 처리
   * 11.7: MPG 비디오 서빙
   * 11.8: SIGCHLD 핸들러로 CGI 자식 회수
   * 11.9: mmap 대신 malloc + rio_readn + rio_writen
   * 11.10: HTML form으로 adder 입력
   * 11.11: HEAD 메서드 지원
   * 11.12: POST 메서드 지원
   * 11.13: SIGPIPE / EPIPE 처리
* Proxy Lab
   * sequential proxy
      * 요청 수신 → 파싱 → 서버 전달 → 응답 반환
   * concurrent proxy
      * pthread_create: 요청별 스레드
      * detached thread: join 불필요
   * cache
      * MAX_CACHE_SIZE / MAX_OBJECT_SIZE
      * LRU eviction
      * readers-writers lock
   * HTTP 변환
      * HTTP/1.1 → HTTP/1.0
      * Connection: close 강제
      * Host 헤더 유지
* SQL API Server (수요 코딩회)
   * listen socket: Tiny와 동일
   * request parser: SQL 문자열 추출
   * thread pool
      * main thread: accept만 담당
      * worker thread: 큐에서 작업 꺼냄
      * mutex + condvar: 작업 큐 동기화
   * DB engine bridge
      * 기존 SQL 처리기 + B+Tree 인덱스 호출
   * response encoder: 결과 문자열 반환
   * 동시성 주의
      * 공유 자원: B+Tree, 버퍼, 전역 상태
      * race condition 방지

## 트리 읽는 법

1. 최상위 항목에서 시작해 관심 있는 가지를 타고 내려갑니다
2. 같은 깊이의 항목은 같은 수준의 개념입니다
3. 부모 항목을 모르면 자식 항목이 이해되지 않습니다
4. 학습은 위에서 아래로, 복습은 아래에서 위로 합니다

## 학습 완료 자가 점검

트리의 각 항목을 보고 아래 질문에 답할 수 있으면 해당 개념을 이해한 것입니다.

- 이 항목이 뭔지 한 문장으로 말할 수 있는가
- 부모 항목과 어떤 관계인지 설명할 수 있는가
- 자식 항목이 왜 필요한지 설명할 수 있는가
- 코드에서 이 개념이 어디에 나타나는지 가리킬 수 있는가

## 질문으로 확장하는 포인트

- 왜 `listenfd`와 `connfd`를 구분해야 할까
- 왜 `HTTP/1.1` 요청을 받아 `HTTP/1.0`으로 보내도 과제가 성립할까
- 왜 Proxy의 캐시는 단순 배열보다 동기화 전략이 더 중요할까
- 왜 Tiny를 이해하면 SQL API 서버의 뼈대가 빨라질까
- 왜 네트워크는 결국 "메모리 밖으로 나간 file descriptor I/O"처럼 볼 수 있을까
