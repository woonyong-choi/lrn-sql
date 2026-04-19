# CSAPP 11장 추가 질문 정리 - choihyunjin1

- 작성일: 2026-04-18
- 기반 자료:
  - CSAPP 11장 본문을 NotebookLM으로 공부하며 나온 질문
  - `woonyong-kr` q01~q11 질문지를 참고하며 추가로 나온 질문
- 업로드 후보 경로: `docs/team/choihyunjin1/`
- 제외 기준:
  - `woonyong-kr` 질문지에 그대로 적힌 대표 질문은 제외했다.
  - 대신 본문 공부 중 내가 추가로 물어본 질문과, 질문지를 보며 꼬리로 이어진 후속 질문을 정리했다.
- 검증 상태:
  - NotebookLM/AI 답변 기반 정리이므로 `검증 전`이다.
  - CSAPP 원문, 실제 코드, 실행 결과로 다시 확인해야 한다.

## 읽는 방법

이 문서는 단순 Q&A 요약본이 아니라, 내가 네트워크를 이해할 때 실제로 사용했던 비유와 사고 흐름을 남기기 위한 기록이다.

1. `1부`는 CSAPP 11장 본문을 처음 읽으면서 생긴 기초 질문이다.
2. `2부`는 팀 질문지 q01~q11을 보며 더 깊게 파고든 후속 질문이다.
3. 답변은 최대한 `짧은 결론 -> 비유 -> 실제 시스템 연결` 순서로 적었다.
4. 답변은 최종 정답이라기보다, 이후 책 원문과 코드로 검증할 학습용 설명이다.

## 문서 표기 규칙

- 각 `##` 제목은 큰 주제다.
- 각 `###` 또는 `####`의 `질문 N`은 바로 위 큰 주제에서 파생된 하부 질문이다.
- `전후 사정`은 그 질문 묶음이 왜 나왔는지 설명한다.
- 각 답변은 `핵심`과 `비유/시스템 연결`로 나누어, 짧은 정의와 실생활 그림을 분리해서 읽을 수 있게 했다.
- 따라서 질문 하나만 따로 보기보다, `전후 사정 -> 질문 -> 답변` 순서로 읽어야 원래 대화 흐름이 살아난다.

## 반복해서 쓰는 비유 지도

이 문서는 기술 용어를 바로 외우기보다, 먼저 실생활 그림으로 잡고 그다음 CSAPP 용어로 내려가는 방식으로 읽는다. 아래 비유들은 여러 질문에서 반복해서 등장한다.

| 실제 개념 | 계속 사용할 비유 | 왜 이 비유가 맞는가 |
| --- | --- | --- |
| Internet 전체 | 전국/전세계 물류망 | 여러 동네 도로, 고속도로, 물류센터가 연결되어 물건을 목적지까지 릴레이한다. |
| LAN | 집 안/건물 안 동네 도로 | 가까운 기기들이 같은 내부 도로 위에서 직접 신호를 주고받는다. |
| WAN | 외부 고속도로/통신사망 | 동네 밖으로 나가 멀리 떨어진 네트워크와 연결되는 긴 도로다. |
| Router | 물류센터/교차로 | 최종 목적지 주소를 보고 다음 물류센터로 넘긴다. 전체 길을 다 외우지 않고 다음 hop만 고른다. |
| Ethernet frame | 이번 구간 트럭 운송장 | 바로 다음 장비까지 가기 위한 source/destination MAC이 붙는다. router를 지날 때마다 바뀐다. |
| IP packet | 상자 안쪽의 최종 주소 송장 | 출발지 IP와 목적지 IP가 들어 있고, 여러 hop을 지나도 대체로 유지된다. |
| TCP | 등기 택배 + 수도관 | 순서 번호와 확인 응답으로 정확히 보내지만, application에는 경계 없는 byte stream처럼 보인다. |
| UDP | 엽서/전단지/퀵서비스 | 가볍게 던지지만 분실, 순서 바뀜, 재전송을 기본 보장하지 않는다. |
| Buffer | 물류창고/물탱크 | 생산자와 소비자 속도가 다를 때 중간에 데이터를 쌓아 둔다. |
| fd | 번호표/리모컨 버튼 | 사용자는 작은 정수만 들고 있고, kernel이 그 번호로 진짜 파일이나 socket을 찾는다. |
| FDT | 개인 전화번호부 | process마다 fd 번호가 무엇을 가리키는지 따로 저장한다. |
| `dup2` | 수도관 방향 바꾸기 | 원래 stdout으로 가던 물길을 socket 쪽으로 꺾는다. |
| CGI | 하청업체/주방장 | web server가 직접 계산하지 않고 child process를 만들어 외부 프로그램에게 맡긴다. |

읽을 때는 "지금 이 질문이 어떤 물류 장면을 묻는가?"를 먼저 생각하면 된다. 예를 들어 MAC/IP 질문은 대부분 "트럭 운송장과 최종 송장이 왜 둘 다 필요한가"이고, fd/socket 질문은 "리모컨 버튼 하나가 kernel 내부의 어떤 창고와 연결되는가"이다.

## 목차

- 1부. CSAPP 11장 본문 읽기 중 나온 질문
  - 11.1 클라이언트-서버 모델
  - 11.2 네트워크와 캡슐화
  - 11.3 글로벌 IP 인터넷
  - 11.4 소켓 인터페이스
  - 11.5 HTTP와 CGI
- 2부. 팀 질문지 기반 후속 질문
  - q01 네트워크 하드웨어와 라우팅
  - q02 호스트 내부 송수신 파이프라인
  - q03 TCP/UDP와 socket syscall
  - q04 IP 주소와 byte order
  - q05 DNS와 Cloudflare
  - q06 socket, fd, FDT, Unix I/O
  - q07 socket interface 조립 순서
  - q08 TCP stream과 UDP datagram
  - q09 HTTP / FTP / MIME / Telnet
  - q10 CPU / memory / kernel / fd 관점
  - q11 CGI / fork / dup2

# 1부. CSAPP 11장 본문 읽기 중 나온 질문

## 11.1 클라이언트-서버 모델

> 전후 사정: 11장에 들어오며 웹, 서버, 클라이언트가 모두 "요청하고 응답받는 구조"로 설명되었다. 여기서 책이 말하는 transaction이 데이터베이스 transaction과 같은 말인지, 아니면 단순한 요청-응답 한 사이클인지 확인하려고 나온 질문이다.

### 질문 1

클라이언트-서버 트랜잭션은 정확히 무엇인가? 요청, 처리, 응답, 완료 네 단계를 통틀어 트랜잭션이라고 부르는가?

### 답변 1

**핵심:** 여기서 트랜잭션은 데이터베이스 트랜잭션처럼 원자성이나 롤백을 보장한다는 뜻이 아니다. CSAPP 11장 네트워크 문맥에서는 클라이언트가 요청을 보내고, 서버가 처리하고, 응답을 돌려주고, 클라이언트가 그 응답을 받는 한 번의 왕복 작업을 말한다.

**비유/시스템 연결:** 비유하면 식당에서 손님이 주문하고, 주방이 요리하고, 직원이 음식을 가져오고, 손님이 받는 한 사이클이 하나의 트랜잭션이다. 웹에서는 브라우저가 `GET /index.html`을 보내고, 서버가 파일을 찾아서 HTTP 응답으로 돌려주는 한 번의 요청-응답 묶음이 여기에 해당한다.

## 11.2 네트워크와 캡슐화

> 전후 사정: 이 구간에서는 네트워크를 "또 하나의 I/O 장치"로 보고, host 내부의 NIC, LAN, Ethernet frame, router, packet encapsulation을 처음 연결했다. 질문들은 대부분 "비트가 실제 장비를 지나갈 때 어떤 포장으로 움직이는가"에서 파생되었다.

> 비유로 먼저 잡기: 이 구간은 "내 컴퓨터에서 나온 작은 택배 상자가 집 안 도로, 건물 출입구, 도시 고속도로를 거쳐 다른 집까지 가는 과정"으로 보면 된다. NIC는 택배를 처음 싣고 내리는 창고 직원이고, Ethernet frame은 지금 동네에서만 통하는 운송장이다. IP packet은 상자 안쪽의 최종 주소표이고, router는 중간 물류센터다. 캡슐화는 상자 안쪽 송장을 유지한 채, 구간마다 다른 운송장과 트럭으로 갈아 태우는 과정이다.

### 질문 2

DMA가 정확히 무엇인가?

### 답변 2

**핵심:** DMA(Direct Memory Access)는 I/O 장치가 CPU를 거치지 않고 메인 메모리(DRAM)에 직접 데이터를 읽고 쓰는 방식이다. 네트워크에서는 NIC가 수신한 frame을 kernel buffer에 직접 올리거나, 송신할 frame을 DRAM에서 직접 읽어 간다.

**비유/시스템 연결:** 비유하면 CPU가 택배 상자를 하나하나 트럭에 실어 나르는 것이 아니라, 창고 직원(NIC)이 창고 위치만 전달받고 직접 물건을 가져가는 방식이다. CPU는 주로 "어느 주소에서 얼마만큼 가져가라"는 지시와 완료 보고 interrupt 처리만 맡고, 실제 대량 데이터 이동은 DMA가 맡는다.

### 질문 3

Ethernet adapter가 주소를 가지는 이유는 무엇인가?

### 답변 3

**핵심:** Ethernet adapter의 MAC 주소는 같은 link 안에서 frame의 수신자를 구분하기 위한 주소다. Ethernet frame의 header에는 source MAC과 destination MAC이 들어가고, NIC는 destination MAC이 자기 주소인지 확인해서 받을지 버릴지 판단한다.

**비유/시스템 연결:** 비유하면 같은 교실에 여러 학생이 있을 때, 선생님이 "현진이에게 전달"이라고 이름을 붙여야 다른 학생들이 자기 것이 아님을 알 수 있는 것과 같다. Ethernet segment 안에서는 IP보다 먼저 MAC 주소가 "이 frame을 누가 받아야 하는가"를 가르는 1차 이름표 역할을 한다.

### 질문 4

frame과 packet은 하는 일이 비슷한데, 둘은 무엇이 다른가?

### 답변 4

**핵심:** 둘 다 데이터를 감싸는 포장지지만 활동 무대가 다르다. frame은 Ethernet/Wi-Fi 같은 link layer에서 바로 다음 장비에게 전달하기 위한 포장이고, packet은 IP layer에서 최종 목적지 IP를 담고 여러 network를 건너가기 위한 포장이다.

**비유/시스템 연결:** 비유하면 packet은 택배 상자 안쪽에 붙은 최종 주소 송장이고, frame은 이번 구간에서 물건을 싣고 가는 트럭의 운송장이다. router를 지날 때 최종 주소인 IP packet은 유지되지만, "이번 구간의 트럭"에 해당하는 frame header는 매번 새로 갈아 끼워진다.

### 질문 5

Ethernet과 Internet은 무엇이 다른가?

### 답변 5

**핵심:** Ethernet은 주로 가까운 범위의 LAN을 구성하는 link-layer 기술이고, Internet은 이런 LAN과 WAN을 router로 이어 붙인 거대한 network of networks다. Ethernet이 동네 도로라면 Internet은 동네 도로, 고속도로, 물류센터가 모두 연결된 전국 물류망에 가깝다.

**비유/시스템 연결:** 집에서는 PC, 노트북, 공유기 LAN 포트가 Ethernet 또는 Wi-Fi라는 동네 도로 안에 있다. 그런데 외부 서버로 나가려면 공유기/router를 거쳐 통신사망과 더 큰 Internet으로 나가야 한다. 그래서 "Ethernet은 작은 구간의 전달 방식, Internet은 여러 구간을 이어 붙인 전체 통신망"으로 구분하면 된다.

### 질문 6

LAN과 WAN은 상대적인 말인가, 절대적인 말인가?

### 답변 6

**핵심:** LAN과 WAN은 완전히 수학적으로 고정된 경계가 있는 말은 아니지만, 보통 지리적 범위와 운영 주체로 구분한다. LAN은 집, 건물, 캠퍼스처럼 좁은 범위의 내부망이고, WAN은 도시, 국가, 대륙처럼 넓은 범위를 연결하는 망이다.

**비유/시스템 연결:** 가정용 공유기에서 `LAN` 포트는 내 방과 집 안 기기들이 붙는 내부 도로이고, `WAN` 포트는 통신사 쪽 외부 도로로 나가는 출입구다. 그래서 "더 큰 연결망으로 나가는 방향을 WAN이라고 부른다"는 직관은 실생활 장비를 이해하는 데 유용하다. 다만 기술적으로는 LAN/WAN은 포트 이름만이 아니라 범위와 네트워크 역할의 차이를 말한다.

### 질문 7

TCP/IP가 프로토콜 소프트웨어라면, 예전에 배운 encapsulation은 사라지는 것인가?

### 답변 7

**핵심:** 사라지지 않는다. 달라지는 것은 캡슐화를 누가 직접 다루느냐이다. 응용 프로그래머는 `write`, `connect` 같은 socket interface만 쓰지만, kernel 안의 TCP/IP stack과 NIC는 여전히 TCP header, IP header, Ethernet header를 붙이고 벗긴다.

**비유/시스템 연결:** 비유하면 사용자는 택배 앱에서 주소만 입력하지만, 실제 물류센터 안에서는 박스 포장, 송장 부착, 트럭 배차, 중간 물류센터 재분류가 모두 일어난다. TCP/IP는 이 복잡한 일을 프로그래머 눈앞에서 숨겨 주는 운영체제 내부의 통신 전담반이다.

## 11.3 글로벌 IP 인터넷

> 전후 사정: 앞에서는 hub, bridge, router 같은 물리/링크 장비를 봤고, 여기서는 프로그래머가 보는 추상화로 시야가 바뀌었다. 즉, 복잡한 하드웨어 경로를 직접 다루지 않고 IP 주소, domain name, connection, socket으로 보는 이유를 잡기 위한 질문들이다.

> 비유로 먼저 잡기: 11.2가 물류센터 내부 배선을 보는 장면이라면, 11.3은 택배 앱 화면을 보는 장면이다. 앱 사용자는 트럭이 어느 도로로 가는지, 물류센터에서 몇 번 컨베이어를 타는지 모른다. 대신 최종 주소(IP), 사람에게 익숙한 이름(domain), 연결된 상담 통로(connection)만 본다. TCP/IP와 OS는 이 복잡한 하부 물류를 숨겨 주고, 프로그래머에게는 단순한 주소와 socket interface만 남긴다.

### 질문 8

`111.111...` 같은 IP 주소에 프로토콜 소프트웨어의 핵심 기능인 주소 정보가 들어 있으니, 프로그래머가 복잡한 코드를 짤 필요가 없다는 뜻인가?

### 답변 8

**핵심:** 큰 방향은 맞다. IP 주소는 서로 다른 LAN/WAN의 물리적 차이를 덮고, 전 세계 host를 하나의 통일된 논리 주소 체계로 보게 해 준다. 그래서 프로그래머는 "이 데이터가 어떤 router를 지나고 어떤 frame으로 다시 포장되는가"를 직접 코딩하지 않아도 된다.

**비유/시스템 연결:** 다만 IP 주소 안에 모든 물리적 MAC 경로가 들어 있는 것은 아니다. IP 주소는 최종 목적지 주소이고, 실제로 다음 hop을 고르는 일은 router의 routing table, ARP, link-layer frame 재포장으로 이어진다. 프로그래머 입장에서는 `IP:port`만 알면 kernel이 나머지를 처리해 주기 때문에 단순해지는 것이다.

### 질문 9

big endian과 little endian은 무엇이고, IP 주소와 무슨 관련이 있는가?

### 답변 9

**핵심:** Endian은 여러 byte로 이루어진 정수를 메모리에 어떤 순서로 저장할지 정하는 규칙이다. big endian은 큰 자리 byte를 먼저 놓고, little endian은 작은 자리 byte를 먼저 놓는다. 네트워크 표준 바이트 순서는 big endian이다.

**비유/시스템 연결:** 비유하면 숫자 `1234`를 왼쪽부터 `1,2,3,4`로 적을지, 내부 저장상 `4,3,2,1`처럼 뒤집어 적을지의 약속 차이다. 내 컴퓨터가 little endian이어도 네트워크로 포트 번호나 IPv4 주소를 보낼 때는 big endian으로 맞춰야 상대가 같은 값을 읽는다. 이 변환을 해 주는 함수가 `htons`, `htonl`, `ntohs`, `ntohl`이다.

### 질문 10

`inet_pton`과 `inet_ntop`은 정확히 무엇을 바꾸는 함수인가?

### 답변 10

**핵심:** `inet_pton`은 사람이 읽는 `"127.0.0.1"` 같은 dotted-decimal 문자열을 네트워크가 쓰는 binary IP 주소로 바꾼다. 반대로 `inet_ntop`은 binary IP 주소를 사람이 읽을 수 있는 문자열로 바꾼다.

**비유/시스템 연결:** 여기서 `p`는 presentation, 즉 사람이 보는 표현이고, `n`은 network, 즉 네트워크 내부 표현이다. 쉽게 말하면 `inet_pton`은 주소를 사람이 쓰는 글자에서 컴퓨터용 숫자로 번역하고, `inet_ntop`은 컴퓨터용 숫자를 사람이 읽는 주소표로 다시 번역한다.

### 질문 11

도메인의 계층적 트리 구조는 그냥 `.kr`, `.com`, `amazon`, `tistory` 같은 것을 누가 관리하는지 나타내는 것인가?

### 답변 11

**핵심:** 맞다. 도메인의 계층 구조는 단순히 이름을 예쁘게 나누는 것이 아니라, 관리 권한이 위에서 아래로 위임되는 구조를 나타낸다. `.com`, `.kr` 같은 최상위 도메인은 큰 관리 주체가 있고, 그 아래 `example.com` 같은 2단계 도메인은 등록자가 소유한다.

**비유/시스템 연결:** 비유하면 `대한민국 -> 경기도 -> 성남시 -> 분당구`처럼 주소가 내려갈수록 관리 범위가 좁아지는 것과 비슷하다. `tistory.com`을 가진 회사는 그 아래 `myblog.tistory.com` 같은 하위 이름을 내부 정책에 따라 나눠줄 수 있다. DNS는 이런 계층적 권한 위임을 통해 전 세계 도메인 이름을 관리한다.

### 질문 12

DNS의 1대1, 다대1, 일대다 매핑은 실제로 어떤 상황에 쓰이는가?

### 답변 12

**핵심:** 1대1은 하나의 도메인이 하나의 IP를 가리키는 가장 단순한 경우다. 다대1은 여러 도메인이 같은 서버 IP를 가리키는 경우다. 예를 들어 브랜드 보호나 광고용으로 여러 주소를 사 두고 모두 같은 홈페이지로 보내는 방식이다.

**비유/시스템 연결:** 일대다는 하나의 도메인이 여러 IP를 가리키는 경우다. 대형 서비스는 서버 한 대로 트래픽을 감당하기 어렵기 때문에, `example.com`에 여러 IP를 연결해서 사용자 요청을 여러 서버로 나눈다. 비유하면 한 식당 이름으로 주문이 들어오지만, 실제 조리는 여러 지점이나 주방으로 분산되는 구조다.

### 질문 13

TCP는 지연이 생겨도 순서 인자를 가지고 있어서 데이터를 다시 맞출 수 있는가?

### 답변 13

**핵심:** 맞다. TCP는 byte stream의 각 부분에 sequence number를 붙여서 "이 byte가 전체 흐름에서 몇 번째 위치인가"를 관리한다. 그래서 packet이 늦게 도착하거나 순서가 바뀌어도 kernel의 TCP stack이 순서를 다시 맞춰 application에 넘겨준다.

**비유/시스템 연결:** 비유하면 택배 상자 번호가 아니라 책 페이지 번호가 붙어 있는 것과 같다. 10쪽이 먼저 오고 9쪽이 나중에 와도, TCP는 페이지 번호를 보고 원래 순서대로 다시 꽂아 둔다. UDP는 이런 순서 번호와 재조립 보장이 없으므로 application이 직접 처리하지 않으면 순서 섞임을 감수해야 한다.

## 11.4 소켓 인터페이스

> 전후 사정: 여기부터는 "운영체제가 TCP/IP를 알아서 처리한다"는 말을 실제 C 함수 호출 순서로 바꾸는 단계다. 질문들은 socket이 주소인지 파일인지, server가 왜 listen socket과 connected socket을 따로 가지는지, helper 함수가 어디까지 자동화하는지 확인하는 흐름이다.

> 비유로 먼저 잡기: 소켓은 전화기나 상담 창구로 보면 된다. `socket()`은 전화기 하나를 만드는 것이고, `bind()`는 그 전화기에 번호를 붙이는 것이다. `listen()`은 매장 문을 열고 대기줄을 받는 상태이며, `accept()`는 손님 한 명을 실제 상담 창구로 연결하는 동작이다. `connect()`는 클라이언트가 그 번호로 전화를 거는 동작이다. 이 비유를 유지하면 listenfd와 connfd가 왜 다른지 훨씬 덜 헷갈린다.

### 질문 14

소켓은 주소가 저장되는 구조체인가, 파일 같은 것인가?

### 답변 14

**핵심:** 둘 다 관련은 있지만 같은 말은 아니다. 소켓은 연결의 끝점을 나타내는 kernel의 논리적 객체이고, 사용자 프로그램에는 `sockfd`라는 작은 정수 file descriptor로 보인다. 반면 `sockaddr_in`, `sockaddr_storage` 같은 구조체는 소켓에 연결할 IP 주소와 port 정보를 담는 주소 구조체다.

**비유/시스템 연결:** 비유하면 소켓은 전화기 자체이고, 주소 구조체는 전화를 걸 전화번호부 항목에 가깝다. 프로그램은 주소 구조체를 `connect`나 `bind`에 넘겨 "이 번호로 연결해 줘" 또는 "이 번호로 전화 받을게"라고 kernel에 요청한다.

### 질문 15

서버에서 `socket -> bind -> listen -> accept`가 각각 어떤 역할인가?

### 답변 15

**핵심:** `socket`은 전화기를 하나 만드는 단계다. 아직 번호도 없고 전화를 받을 준비도 끝나지 않았다. `bind`는 그 전화기에 특정 IP와 port 번호를 붙이는 단계다. `listen`은 "이제 이 번호로 걸려오는 전화를 받을 준비가 됐다"고 kernel에 알리고 대기열을 만드는 단계다.

**비유/시스템 연결:** `accept`는 실제 손님 전화가 왔을 때 받는 단계다. 중요한 점은 `accept`가 기존 listening socket을 직접 통화용으로 쓰는 것이 아니라, 특정 client와 통신할 새 connected socket fd를 만들어 돌려준다는 것이다. 그래서 서버는 문 앞의 안내 데스크(listenfd)와 실제 손님 상담 창구(connfd)를 구분해서 가진다.

### 질문 16

listening socket과 connected socket은 왜 따로 있는가?

### 답변 16

**핵심:** listening socket은 손님을 받는 문지기 역할만 한다. 특정 client와 데이터를 주고받는 역할은 하지 않고, 새 연결 요청이 오는지 기다린다. connected socket은 accept 이후 특정 client 한 명과 실제로 read/write를 하는 통신 통로다.

**비유/시스템 연결:** 비유하면 식당 입구의 번호표 기계와 손님이 앉은 테이블은 다르다. 번호표 기계는 계속 다음 손님을 받기 위해 남아 있어야 하고, 테이블은 한 손님과 식사를 진행하는 공간이다. 서버가 여러 client를 처리하려면 이 둘이 분리되어야 한다.

### 질문 17

`open_listenfd`는 `bind`, `listen`, `accept`를 모두 한 번에 해 주는가?

### 답변 17

**핵심:** 아니다. `open_listenfd`는 서버가 연결 요청을 받을 준비를 마치는 함수다. 내부적으로 `getaddrinfo`, `socket`, `setsockopt`, `bind`, `listen`까지는 처리하지만, `accept`는 포함하지 않는다.

**비유/시스템 연결:** 이유는 `accept`는 client가 올 때마다 반복해서 호출해야 하기 때문이다. `open_listenfd`는 매장 문을 열고 안내 데스크를 세우는 1회성 준비 작업이고, `accept`는 손님이 올 때마다 번호표를 받아 실제 상담 창구로 연결하는 반복 작업이다.

### 질문 18

`getaddrinfo`는 `inet_pton`처럼 IP 문자열을 32비트 주소로 바꾸는 함수인가?

### 답변 18

**핵심:** `getaddrinfo`는 더 큰 범위의 종합 변환 함수다. `inet_pton`은 `"128.2.194.242"` 같은 IP 문자열을 binary IP 주소로 바꾸는 데 집중한다. 반면 `getaddrinfo`는 domain name, service name, IPv4/IPv6 여부, socket type까지 고려해서 socket 함수가 바로 쓸 수 있는 `addrinfo` list를 만들어 준다.

**비유/시스템 연결:** 비유하면 `inet_pton`은 주소 한 줄을 숫자로 번역하는 계산기이고, `getaddrinfo`는 주소 검색, DNS 질의, IPv4/IPv6 후보 정리, port 설정까지 해 주는 전체 접수창구다. 그래서 현대 socket code에서는 보통 `getaddrinfo`를 먼저 호출하고, 그 결과를 `socket`, `connect`, `bind`에 넘긴다.

## 11.5 HTTP와 CGI

> 전후 사정: socket으로 byte stream을 주고받는 수준에서, 이제 browser와 web server가 HTTP text를 어떻게 해석하고 정적/동적 content를 어떻게 돌려주는지로 올라왔다. 특히 CGI는 앞에서 배운 process, fd, dup2, socket이 한 번에 연결되는 지점이라 추가 질문이 많이 나왔다.

> 비유로 먼저 잡기: HTTP는 손님과 식당 직원이 주고받는 주문서 양식이다. 정적 content는 이미 진열대에 있는 완제품이고, 동적 content는 주문을 받은 뒤 주방장이 즉석에서 만드는 음식이다. CGI는 web server가 직접 요리하지 않고 외부 주방장(child process)에게 주문서를 넘기는 방식이다. `dup2`는 그 주방장의 출력 창구를 손님 테이블(client socket)로 바로 연결하는 수도관 교체 작업이다.

### 질문 19

FTP는 무엇이고, HTTP/HTML의 강력함을 설명하려고 나온 비교 대상인가?

### 답변 19

**핵심:** FTP(File Transfer Protocol)는 이름 그대로 파일을 주고받기 위한 오래된 프로토콜이다. 서버에서 파일을 다운로드하거나 업로드하는 데 초점이 있고, 웹 페이지처럼 문서 안에 링크와 이미지 배치 정보를 담아 브라우저가 화면을 구성하는 방식과는 다르다.

**비유/시스템 연결:** HTTP와 HTML의 차별점은 단순 파일 전송을 넘어서 "문서를 보여 주고, 다른 문서로 연결하고, 이미지와 텍스트를 배치하는" 웹 경험을 만든다는 점이다. 비유하면 FTP는 창고에서 파일 박스를 꺼내 주는 서비스이고, HTTP+HTML은 그 박스를 열어 전시장처럼 배치하고, 다른 전시장으로 가는 안내판까지 제공하는 서비스다.

### 질문 20

정적 콘텐츠와 동적 콘텐츠는 실제로 무엇이 다른가?

### 답변 20

**핵심:** 정적 콘텐츠는 서버 디스크에 이미 완성된 파일이 있고, 요청이 오면 그 파일을 그대로 보내는 방식이다. 예를 들어 로고 이미지, CSS 파일, 고정된 HTML 소개 페이지가 여기에 해당한다. 누가 요청해도 같은 파일을 꺼내 준다.

**비유/시스템 연결:** 동적 콘텐츠는 요청이 올 때 프로그램을 실행해서 그 순간 결과를 만들어 보내는 방식이다. 예를 들어 검색 결과, 로그인 후 내 정보, 장바구니, 계좌 잔액처럼 사용자와 시점에 따라 달라지는 응답이다. 비유하면 정적 콘텐츠는 진열대에 있는 완제품을 주는 것이고, 동적 콘텐츠는 주문을 받은 뒤 주방에서 즉석 조리해 내는 것이다.

### 질문 21

CGI 프로그램은 인자를 어떻게 받는가?

### 답변 21

**핵심:** GET 방식에서는 URI 뒤의 `?` 뒤에 붙은 문자열이 인자가 된다. 예를 들어 `/cgi-bin/adder?15000&213`에서 `15000&213`이 인자이고, Tiny는 이 값을 `QUERY_STRING` 환경변수로 CGI 프로그램에게 넘긴다. CGI 프로그램은 `getenv("QUERY_STRING")`으로 값을 읽는다.

**비유/시스템 연결:** POST 방식은 보통 긴 데이터를 URI에 붙이지 않고 HTTP request body에 넣는다. 이때 서버는 CGI 프로그램의 표준 입력(stdin)을 client socket 쪽으로 연결해 주고, `CONTENT_LENGTH` 같은 환경변수로 길이를 알려준다. 즉 GET은 쪽지를 환경변수로 건네는 느낌이고, POST는 입력 통로 자체를 연결해 주는 느낌이다.

### 질문 22

CGI 프로그램이 `printf`한 데이터는 어떻게 client에게 돌아가는가?

### 답변 22

**핵심:** 핵심은 `dup2`다. 서버가 CGI용 child process를 만든 뒤, child의 표준 출력(fd 1)을 client와 연결된 socket fd로 바꿔치기한다. 그 다음 `execve`로 CGI 프로그램을 실행하면, CGI 프로그램은 자신이 network를 모른 채 평소처럼 `printf`만 호출한다.

**비유/시스템 연결:** 하지만 이미 stdout이 socket으로 꺾여 있기 때문에 `printf` 결과는 모니터가 아니라 client browser로 흘러간다. 비유하면 원래 싱크대로 흘러가던 물길을 호스로 바꿔 연결해서, 같은 수도꼭지를 틀어도 물이 다른 곳으로 나가게 만든 것이다.

### 질문 23

network data는 하드웨어적으로 어디에 들어오고, user memory와 kernel memory는 어떻게 관련되는가?

### 답변 23

**핵심:** network에서 들어온 bit는 NIC가 받고, NIC는 DMA로 DRAM의 kernel buffer 영역에 frame을 올린다. 이때 하드웨어는 실제 물리 주소를 대상으로 DMA를 수행한다. 그 물리 메모리 영역은 운영체제가 관리하는 kernel memory에 해당한다.

**비유/시스템 연결:** 그 뒤 kernel의 network stack이 Ethernet/IP/TCP header를 확인하고, 해당 socket의 receive buffer에 payload를 쌓는다. application이 `read`를 호출하면 kernel은 그 데이터를 user buffer로 복사해 준다. 즉 수신 경로는 "선로 -> NIC -> kernel buffer -> socket receive queue -> user buffer" 순서로 볼 수 있다.

# 2부. 팀 질문지 기반 후속 질문

## q01. 네트워크 하드웨어와 라우팅

> 전후 사정: q01의 원래 대표 질문은 Ethernet, bridge, router, LAN/WAN, bit bundle의 형태를 구분하는 것이었다. 아래 질문들은 그 대표 질문을 읽다가 "그럼 실제 집 공유기/스위치/허브는 책의 무엇에 해당하는가", "router는 IP를 보고 어떻게 다음 길을 고르는가", "DNS는 언제 끼어드는가"로 이어진 하부 질문들이다.

> 비유로 먼저 잡기: q01 전체는 "우리 집 안 네트워크를 하나의 아파트 단지 물류 시스템으로 그리는 연습"이다. hub는 확성기처럼 모두에게 뿌리는 낡은 장비이고, switch/bridge는 어느 집이 어느 포트 뒤에 있는지 기억하는 관리실이다. router는 아파트 단지 바깥으로 나가는 정문이자 외부 물류센터다. LAN은 단지 내부 도로, WAN은 단지 밖 큰 도로다. frame은 단지 안에서 움직이는 배송표, packet은 최종 목적지까지 붙어 있는 송장이다.

### q01-1. 포장과 재포장

> 이 묶음의 초점: Ethernet header, IP header, MAC address가 왜 겹겹이 존재하는지 이해하는 것이다. 핵심 비유는 "최종 주소가 적힌 택배 상자(IP packet)를 구간마다 다른 트럭 운송장(MAC frame)으로 갈아 태운다"이다.

#### 질문 24

왜 Ethernet header가 가장 바깥 포장지에 있는가?

#### 답변 24

**핵심:** Ethernet header는 물리적으로 가장 먼저 데이터를 받는 NIC가 읽어야 하는 정보이기 때문에 가장 바깥에 있다. NIC는 IP나 TCP 내용을 해석하기 전에 Ethernet header의 destination MAC을 보고 이 frame을 받을지 버릴지 먼저 판단한다.

**비유/시스템 연결:** 비유하면 아파트 택배 보관함에 물건이 들어올 때, 제일 바깥 운송장에 "몇 동 경비실로 보낼지"가 적혀 있어야 경비원이 먼저 분류할 수 있는 것과 같다. 안쪽에 최종 목적지 주소가 아무리 정확히 적혀 있어도, 첫 관문인 NIC가 읽는 바깥쪽 이름표가 맞지 않으면 그 frame은 위 계층으로 올라가지 못한다.

#### 질문 25

IP는 인터넷 상위망에서 쓰이는 주소인데, 상위망에서 또 상위망으로 갈 때 따로 감싸는가?

#### 답변 25

**핵심:** IP header를 또 덧씌우는 것이 아니라, router를 지날 때마다 바깥쪽 link-layer header만 새로 씌운다. 안쪽 IP header의 source/destination IP는 최종 출발지와 최종 목적지이므로 계속 유지된다.

**비유/시스템 연결:** 비유하면 택배 상자 안쪽 송장에는 최종 주소가 계속 붙어 있고, 중간 물류센터를 지날 때마다 그 상자를 싣는 트럭만 바뀌는 것이다. 서울 물류센터에서는 서울 트럭, 인천 공항에서는 항공 컨테이너, 미국에 도착하면 현지 트럭으로 바뀌지만, 상자 안의 최종 주소는 그대로 남아 있다. 네트워크에서는 이 트럭 교체가 MAC header 교체다.

### q01-2. 라우팅과 DNS

> 이 묶음의 초점: 목적지 IP를 알고 난 뒤 router들이 어떻게 다음 hop만 보고 packet을 넘기는지, 그리고 애초에 IP를 모를 때 DNS가 어디까지 도와주는지 구분하는 것이다. DNS는 생존 확인 장치가 아니라 이름을 주소로 바꾸는 주소록이다.

#### 질문 26

라우팅 테이블이 실제 목적지의 모든 세부 위치를 모르면 어떻게 목적지를 찾아가는가?

#### 답변 26

**핵심:** 라우터는 전 세계 모든 host의 위치를 외우지 않는다. 대신 IP prefix, 즉 네트워크 대역 단위로 "이 대역은 어느 next hop으로 보내라"는 큰 방향표를 가진다. 그래서 라우팅 테이블은 개별 방 번호 목록이 아니라 지역별 물류센터 안내판에 가깝다.

**비유/시스템 연결:** 예를 들어 택배원이 `경기도 성남시 분당구 정자일로...`의 모든 집을 몰라도, 일단 "경기도 물류센터로 보내라"는 규칙만 알면 다음 단계로 넘길 수 있다. 그 다음 물류센터가 더 세부 지역으로 보내고, 마지막 동네 물류센터가 실제 건물과 호수까지 배달한다. 라우터도 목적지 IP의 앞부분(prefix)을 보고 이런 식으로 단계적으로 전달한다.

#### 질문 27

데이터를 보낼 때 항상 DNS 서버로 먼저 가서 실제 주소가 있는지 확인한 뒤 보내는가?

#### 답변 27

**핵심:** 아니다. DNS는 domain name을 IP address로 바꿔야 할 때만 사용된다. 숫자 IP를 이미 알고 있다면 DNS 질의 없이 바로 그 IP를 목적지로 packet을 만들 수 있다.

**비유/시스템 연결:** DNS는 "목적지가 살아 있는지 검사하는 서버"가 아니라 "이 이름의 숫자 주소가 무엇인지 알려주는 주소록"이다. `google.com`처럼 이름만 알 때는 DNS에 물어보지만, `142.250.x.x` 같은 IP를 이미 알고 있다면 주소록을 펼칠 필요가 없다. 실제 연결 성공 여부는 그 뒤 TCP connect, timeout, 응답 여부로 판단된다.

#### 질문 28

목적지 IP를 아예 모르는 상태에서는 어떻게 통신을 시작하는가?

#### 답변 28

**핵심:** IP를 전혀 모르면 일반적인 Internet 통신을 바로 시작할 수 없다. packet의 IP header에는 destination IP가 들어가야 하므로, 먼저 domain name을 IP로 바꾸는 과정이 필요하다. 이때 OS resolver가 DNS 서버에 질의한다.

**비유/시스템 연결:** 비유하면 친구 이름만 알고 집 주소를 모르는 상태에서는 택배를 보낼 수 없다. 먼저 주소록이나 114 같은 곳에 물어봐서 주소를 알아낸 뒤 택배 송장을 쓸 수 있다. 컴퓨터에서는 이 주소록 역할을 DNS가 하고, C 프로그램에서는 보통 `getaddrinfo`가 그 과정을 감싸 준다.

#### 질문 29

DNS 서버의 주소는 어떻게 아는가?

#### 답변 29

**핵심:** 일반적으로 네트워크에 연결될 때 DHCP가 내 IP, gateway, DNS resolver 주소를 함께 알려준다. 그래서 사용자는 직접 설정하지 않아도 OS가 "이 DNS 서버에게 물어보면 된다"는 정보를 갖게 된다. 사용자가 직접 `8.8.8.8`, `1.1.1.1` 같은 DNS 서버를 수동 설정할 수도 있다.

**비유/시스템 연결:** 비유하면 새 아파트에 입주할 때 관리사무소가 "택배는 이 경비실로, 우편 문의는 이 번호로"라고 기본 안내를 주는 것과 같다. DNS 자체도 IP 주소가 필요하므로, 최소한 내가 처음 물어볼 resolver 주소는 네트워크 설정에 미리 들어와 있어야 한다.

### q01-3. 실생활 장비 대응

> 이 묶음의 초점: 책의 hub/bridge/router/LAN/WAN을 실생활 장비로 다시 매핑하는 것이다. 집 공유기, WAN 포트, LAN 포트, 스위치, 옛 더미 허브를 기준으로 보면 책의 추상적인 장비 구분이 훨씬 선명해진다.

#### 질문 30

실제 생활에서 Ethernet, hub, bridge, router, LAN, WAN은 어떤 장비로 보면 되는가?

#### 답변 30

**핵심:** Ethernet은 장비 이름이라기보다 유선 LAN 통신 규격이다. 실생활에서는 PC의 LAN 포트, 랜선, 공유기 LAN 포트, 사무실 스위치 포트에서 Ethernet을 볼 수 있다. Wi-Fi는 무선 LAN 규격이지만, 역할 면에서는 집 안 기기들을 묶는 "동네 도로"라는 점에서 Ethernet과 같은 층위로 이해할 수 있다.

**비유/시스템 연결:** 옛 hub는 들어온 신호를 모든 포트에 뿌리는 단순 장비다. 현대의 switch는 bridge의 실생활 버전에 가깝고, MAC 주소를 학습해서 필요한 포트로만 보낸다. router는 집 공유기나 통신사 gateway처럼 서로 다른 IP 네트워크를 연결한다. 집 공유기 기준으로 LAN은 안쪽 내부망, WAN은 바깥 통신사망으로 나가는 출구다.

#### 질문 31

더미 허브가 무엇인가?

#### 답변 31

**핵심:** 더미 허브는 들어온 bit를 어느 목적지인지 판단하지 않고 모든 포트로 그대로 복사해 뿌리는 단순한 L1 장비다. PC A가 PC C에게 frame을 보내도 hub는 B, C, D 모든 포트에 신호를 뿌린다. 각 host의 NIC가 destination MAC을 보고 자기 것이 아니면 버린다.

**비유/시스템 연결:** 비유하면 한 사람이 한 명에게만 말하고 싶은데, 확성기로 방 전체에 외치는 것과 같다. 받는 사람은 자기 이름이 불렸는지 확인하고, 아닌 사람은 무시한다. switch는 이보다 똑똑해서 "C는 3번 포트 뒤에 있다"는 MAC address table을 학습하고 필요한 곳으로만 보낸다.

#### 질문 32

"다른 네트워크 사이에서 IP를 보고 전달한다"는 말이 무슨 뜻인가?

#### 답변 32

**핵심:** 같은 네트워크는 보통 같은 IP 대역 안의 기기들이 직접 통신할 수 있는 묶음이다. 예를 들어 집 안 `192.168.0.x` 대역은 같은 LAN으로 볼 수 있다. 목적지 IP가 이 내부망에 없으면 PC는 직접 찾지 않고 gateway인 router로 보낸다.

**비유/시스템 연결:** router는 packet의 destination IP를 보고 routing table에서 다음 경로를 고른다. 즉 "이 packet은 내 집 안 손님이 아니니 바깥 WAN으로 보내야겠다" 또는 "이 대역은 다른 interface로 보내야겠다"를 결정한다. 여기서 핵심은 router가 MAC이 아니라 IP 주소를 기준으로 서로 다른 네트워크 사이의 길을 고른다는 점이다.

## q02. 호스트 내부 송수신 파이프라인

> 전후 사정: q02의 원래 대표 질문은 `write()`를 호출한 뒤 user buffer의 데이터가 kernel, TCP/IP stack, NIC, Ethernet으로 나가기까지의 내부 흐름이었다. 아래 질문들은 그 흐름을 실제 메모리 위치, DMA, `sk_buff`, VFS/sockfs 같은 kernel 내부 단계와 연결하려고 나온 질문들이다.

> 비유로 먼저 잡기: q02는 "내 컴퓨터 내부의 물류센터 투어"다. application은 손님이고, user buffer는 손님 장바구니다. kernel socket buffer는 직원만 들어갈 수 있는 창고다. TCP/IP stack은 포장 전담반이고, NIC driver는 출고 지시자다. NIC는 실제 트럭이고, DMA는 트럭이 창고에서 직접 물건을 실어 가는 자동 적재 장치다. `write()`가 return했다는 말은 물건이 고객 집에 도착했다는 뜻이 아니라, 창고 접수가 끝났다는 뜻에 가깝다.

### q02-1. User/kernel buffer와 복사

> 이 묶음의 초점: 왜 user process가 만든 데이터를 바로 NIC가 가져가지 않고 kernel socket buffer로 복사하는지 이해하는 것이다. 핵심 비유는 "손님이 창고 안까지 들어가지 못하고 접수창구를 통해 물건을 맡긴다"이다.

#### 질문 33

user buffer에서 kernel socket buffer로 왜 복사하는가?

#### 답변 33

**핵심:** 가장 근본적인 이유는 보안과 메모리 보호다. user process가 kernel memory나 hardware를 직접 건드리면 시스템 전체 안정성이 깨질 수 있다. 그래서 응용 프로그램은 자기 user buffer에 데이터를 만들고, `write` 같은 system call을 통해 kernel에게 "이걸 보내 달라"고 요청한다.

**비유/시스템 연결:** 비유하면 손님이 물류센터 창고 안까지 직접 들어가 트럭에 물건을 싣지 못하게 하는 것과 같다. 손님은 접수창구에 물건을 맡기고, 창고 안의 분류와 포장과 트럭 배차는 직원(kernel)이 한다. 이때 접수창구에서 창고 안으로 물건을 넘기는 과정이 user buffer -> kernel socket buffer 복사다.

#### 질문 34

`user buffer -> kernel socket buffer -> TCP/IP 처리 -> NIC driver -> NIC -> Ethernet` 흐름을 쉽게 말하면 무엇인가?

#### 답변 34

**핵심:** 응용 프로그램이 만든 알맹이 데이터를 kernel 작업장으로 넘기고, kernel의 TCP/IP 전담반이 header를 붙여 포장한다. 그다음 NIC driver가 NIC에게 "이 메모리 위치에 보낼 frame이 있다"고 알려주고, NIC가 DMA로 DRAM에서 frame을 읽어 Ethernet 선로로 내보낸다.

**비유/시스템 연결:** 비유하면 사용자가 택배 물건을 접수하면, 물류센터가 송장을 붙이고 박스를 포장하고, 배차 담당자가 트럭 기사에게 "이 선반의 상자를 가져가라"고 알려주는 구조다. 실제로 트럭에 물건을 싣고 도로로 나가는 것은 NIC가 DMA로 수행한다.

#### 질문 35

user에서 kernel로 복사하는 1단계의 상세 로직은 무엇인가?

#### 답변 35

**핵심:** application이 `write(sockfd, buf, n)`을 호출하면 CPU가 user mode에서 kernel mode로 들어간다. kernel은 현재 process의 fd table에서 `sockfd`가 가리키는 socket object를 찾고, socket send buffer에 빈 공간을 확보한다. 그 뒤 `copy_from_user` 계열 동작으로 user buffer의 bytes를 kernel buffer로 복사한다.

**비유/시스템 연결:** 여기서 중요한 점은 `write`가 return했다고 해서 bit가 이미 랜선을 타고 나갔다는 뜻이 아니라는 것이다. 대부분의 경우 "kernel 창고에 접수 완료"에 가깝다. 실제 Ethernet 전송은 이후 TCP/IP 처리, NIC driver, DMA, NIC 송신 스케줄을 거쳐 일어난다.

#### 질문 36

buffer는 무엇인가?

#### 답변 36

**핵심:** buffer는 데이터를 한 곳에서 다른 곳으로 옮길 때 잠시 쌓아두는 임시 저장 공간이다. CPU, kernel, NIC, network는 모두 처리 속도가 다르기 때문에 중간 창고가 필요하다. user buffer는 application이 들고 있는 공간이고, kernel socket buffer는 OS가 송수신을 위해 관리하는 공간이다.

**비유/시스템 연결:** 비유하면 물류센터의 임시 적재장이다. 손님이 물건을 가져오는 속도, 직원이 포장하는 속도, 트럭이 출발하는 속도가 다르기 때문에 중간에 물건을 잠시 모아두는 공간이 있어야 전체 시스템이 부드럽게 돈다. 네트워크에서 "버퍼링"이라는 말도 결국 이 임시 저장 공간이 비거나 차는 현상과 관련된다.

### q02-2. NIC, DMA, 물리 주소

> 이 묶음의 초점: 네트워크 데이터를 "소프트웨어 변수"가 아니라 실제 DRAM, 물리 주소, NIC DMA, kernel buffer 관점에서 보는 것이다. 하드웨어는 user virtual address를 모르기 때문에 OS driver가 준비한 물리 메모리 영역을 기준으로 움직인다.

#### 질문 37

network data가 들어올 때 RAM의 가상주소에 들어가는가, 물리주소에 들어가는가?

#### 답변 37

**핵심:** NIC 같은 하드웨어는 user process의 가상주소를 직접 이해하지 못한다. DMA는 실제 DRAM의 물리주소를 대상으로 일어난다. OS driver가 NIC에게 "수신 frame을 이 물리 메모리 영역에 써라"는 정보를 미리 알려두고, NIC는 그 위치에 데이터를 직접 쓴다.

**비유/시스템 연결:** 다만 그 물리 메모리는 OS가 관리하는 kernel buffer에 대응된다. 이후 kernel이 TCP/IP 처리를 끝내고 application이 `read`를 호출하면, 데이터가 user process의 가상주소 공간에 있는 user buffer로 복사된다. 즉 처음은 물리 메모리의 kernel 영역, 마지막은 user 가상주소의 buffer라고 이해하면 된다.

#### 질문 38

VFS, sockfs, socket layer, TCP layer 같은 것은 어떤 흐름인가?

#### 답변 38

**핵심:** 이것들은 `write(sockfd, buf, n)` 한 줄이 kernel 안에서 어떤 부서를 지나가는지를 나눈 이름들이다. VFS/sockfs는 "이 fd가 일반 파일인지 socket인지"를 구분하는 안내데스크에 가깝다. socket layer는 BSD socket 수준의 범용 처리 계층이고, TCP/UDP layer는 실제 프로토콜별 전담반이다.

**비유/시스템 연결:** 비유하면 택배 접수창구에서 "이 물건은 국내 택배인가, 해외 항공인가, 냉장 배송인가"를 확인하고 해당 부서로 넘기는 과정이다. 사용자는 `write` 하나만 호출하지만, kernel 내부에서는 fd table 조회, socket 객체 확인, TCP/UDP 분기, IP routing, Ethernet header 작성, driver 호출까지 이어진다.

#### 질문 39

수신 과정에서 `sk_buff`는 어떻게 변하는가?

#### 답변 39

**핵심:** `sk_buff`는 kernel이 packet/frame을 들고 다니기 위해 쓰는 상자 같은 자료구조다. 수신할 때는 NIC가 DMA로 frame bytes를 RX buffer에 넣고, kernel은 이것을 `sk_buff`로 감싸 network stack 위로 올린다. 처음에는 Ethernet header, IP header, TCP/UDP header, payload가 모두 들어 있는 상태다.

**비유/시스템 연결:** stack을 올라가면서 kernel은 실제 bytes를 매번 복사해 자르는 대신, `sk_buff` 내부 포인터를 이동시키며 "이제 Ethernet header는 처리했으니 다음은 IP header부터 보자"는 식으로 해석 위치를 바꾼다. 마지막에는 header들이 처리되고 payload가 해당 socket receive queue에 쌓인다. application이 `read`하면 이 payload가 user buffer로 복사된다.

#### 질문 40

NIC가 전송 완료 interrupt를 보낸 후 `sk_buff`는 바로 삭제되는가?

#### 답변 40

**핵심:** NIC가 직접 `sk_buff`를 삭제하지는 않는다. NIC는 DMA로 DRAM에서 frame을 읽어 선로에 보낸 뒤, 완료 interrupt를 보내 "이제 이 메모리의 데이터는 내가 다 썼다"고 알려준다. 그 신호를 받은 NIC driver/kernel이 해당 TX descriptor와 연결된 `sk_buff`를 반환하거나 재활용한다.

**비유/시스템 연결:** 비유하면 트럭 기사가 물건을 싣고 출발했다고 접수증을 보내기 전까지는 창고 직원이 그 상자를 치우면 안 된다. 물건을 다 실었다는 확인이 온 뒤에야 창고 공간을 비울 수 있다. DMA가 끝나기 전에 `sk_buff`를 지우면 NIC가 읽는 중인 데이터가 깨질 수 있다.

### q02-3. Header, TTL, MAC 재포장

> 이 묶음의 초점: IP header에 최종 목적지가 있는데도 왜 MAC header가 계속 바뀌는지, router를 지날 때 실제로 무엇이 유지되고 무엇이 교체되는지 확인하는 것이다. 핵심 문장은 "IP는 end-to-end, MAC은 hop-to-hop"이다.

#### 질문 41

IPv4 header 안에 최종 주소가 있는데, 왜 굳이 MAC으로 다시 포장해서 보내는가?

#### 답변 41

**핵심:** IP 주소는 최종 목적지를 나타내는 논리 주소지만, 지금 당장 물리적으로 frame을 받을 다음 장비를 지정하지는 못한다. Ethernet/Wi-Fi 같은 link layer에서는 NIC가 MAC 주소를 보고 frame을 받는다. 그래서 같은 구간에서 다음 hop에게 전달하려면 destination MAC이 필요하다.

**비유/시스템 연결:** 비유하면 택배 상자에 "미국 뉴욕 최종 목적지"가 적혀 있어도, 지금 이 상자를 인천공항 물류센터까지 실어 갈 트럭을 배정해야 한다. 그 트럭 번호표가 MAC 주소다. router를 지날 때마다 최종 IP 주소는 유지되지만, 다음 구간 트럭에 해당하는 MAC header는 새로 바뀐다.

#### 질문 42

router R을 지날 때 IP header는 유지되고 MAC header는 바뀐다는 것은 실제로 어떤 모습인가?

#### 답변 42

**핵심:** 예를 들어 packet의 IP header가 `src=128.2.194.242`, `dst=208.216.181.15`라면 router R을 지나도 이 두 끝점 주소는 유지된다. 하지만 Ethernet header는 이전 구간에서 쓰던 source/destination MAC을 버리고, 다음 구간 기준의 source/destination MAC으로 새로 만들어진다.

**비유/시스템 연결:** 비유하면 물류센터 R에 택배가 도착했을 때, 상자 안쪽의 최종 주소표는 그대로 두고, 이 상자를 다음 물류센터나 최종 집까지 운반할 새 트럭 운송장만 다시 붙이는 것이다. 그래서 "IP는 end-to-end, MAC은 hop-to-hop"이라는 말이 나온다.

#### 질문 43

TTL은 어떤 근거로 처음 값이 정해지는가?

#### 답변 43

**핵심:** TTL(Time To Live)의 초기값은 보통 출발지 운영체제의 기본 설정으로 정해진다. 흔한 값은 64, 128, 255 등이다. router를 하나 지날 때마다 TTL이 1씩 감소하고, 0이 되면 packet은 폐기된다.

**비유/시스템 연결:** 비유하면 인터넷망 안에서 길을 잃은 택배가 영원히 물류센터를 빙빙 돌지 않도록 붙인 유통기한이다. "64번이나 물류센터를 지났는데도 도착하지 못했다면 이건 정상 경로가 아니다"라고 보고 버리는 것이다. 실제 경로는 보통 수십 hop 안에 끝나므로 TTL은 loop 방지 장치로 충분히 넉넉하게 잡힌다.

## q03. TCP/UDP와 socket syscall

> 전후 사정: q03의 원래 대표 질문은 TCP/UDP가 system call 구조에서 어떻게 나뉘고, host-to-host/process-to-process 표현을 어떻게 이해할지였다. 아래 질문들은 그중 TCP와 UDP의 전송 감각, 순서 보장, protocol 식별, 3-way handshake, byte stream 성질을 구체화한 것이다.

> 비유로 먼저 잡기: TCP와 UDP는 같은 도로(IP)를 쓰지만 배송 계약이 다르다. TCP는 등기 택배라서 사전 연락, 번호표, 확인 서명, 재배송, 속도 조절이 붙는다. UDP는 엽서나 전단지라서 가볍고 빠르지만 분실되거나 순서가 바뀌어도 기본적으로 따지지 않는다. TCP가 application에 byte stream으로 보이는 이유는 kernel이 물탱크에 순서대로 물을 채워 주기 때문이고, UDP가 datagram으로 보이는 이유는 엽서 한 장 한 장의 경계를 유지하기 때문이다.

### q03-1. TCP와 UDP의 전송 감각

> 이 묶음의 초점: TCP와 UDP를 "정확하지만 무거운 등기 택배"와 "빠르지만 보장 없는 퀵서비스"로 나누어 보는 것이다. 여기서 차이는 단순 속도 차이가 아니라 연결, 순서, 재전송, message boundary의 차이다.

#### 질문 44

TCP와 UDP의 전송 방식 차이를 비유하면 무엇인가?

#### 답변 44

**핵심:** TCP는 확인 서명까지 받는 등기 택배에 가깝다. 보내기 전에 연결을 맺고, 보낸 데이터의 순서를 관리하고, 빠진 것이 있으면 다시 보내며, 받는 쪽이 감당할 수 있는 속도도 고려한다. 그래서 느릴 수 있지만 정확하고 순서가 보장된다.

**비유/시스템 연결:** UDP는 전단지나 퀵서비스에 가깝다. 연결을 미리 맺지 않고, 보낸 뒤 잘 도착했는지 직접 확인하지 않는다. 대신 header가 작고 처리 절차가 가볍다. 게임 위치 정보, 음성/영상 실시간 전송처럼 조금 손실되어도 최신성이 중요한 경우에 잘 맞는다.

#### 질문 45

TCP는 몇 번째 데이터인지 인자를 가지고 있고, UDP는 그런 인자가 없어서 지연되면 섞일 수 있는가?

#### 답변 45

**핵심:** 맞다. TCP는 sequence number를 통해 byte stream 안에서 각 byte가 어느 위치인지 관리한다. 그래서 나중에 보낸 segment가 먼저 도착해도 kernel TCP stack이 순서를 맞춘 뒤 application에 넘겨준다.

**비유/시스템 연결:** UDP는 TCP처럼 stream 순서를 보장하는 sequence number가 없다. UDP datagram은 각각 독립된 편지처럼 도착하고, network 상황에 따라 사라지거나 순서가 바뀔 수 있다. 필요하다면 application이 자기 payload 안에 sequence number를 따로 넣어 직접 순서를 맞춰야 한다.

#### 질문 46

기기는 들어온 데이터가 TCP인지 UDP인지 어떻게 알아차리는가?

#### 답변 46

**핵심:** 보낼 때는 application이 `socket`을 만들 때 결정한다. `SOCK_STREAM`이면 보통 TCP이고, `SOCK_DGRAM`이면 보통 UDP다. kernel은 해당 socket object에 어떤 protocol을 쓸지 기억하고, `write`나 `sendto`가 오면 맞는 protocol handler로 보낸다.

**비유/시스템 연결:** 받을 때는 IP header 안의 protocol field를 본다. IPv4에서 protocol 값이 6이면 TCP, 17이면 UDP다. 비유하면 IP packet 겉면에 "안쪽 포장지는 TCP입니다" 또는 "UDP입니다"라는 택배사 코드가 적혀 있고, kernel이 그 코드를 보고 TCP 전담반 또는 UDP 전담반으로 넘기는 것이다.

### q03-2. TCP connection과 stream

> 이 묶음의 초점: TCP가 왜 연결을 먼저 맺고, 왜 application에는 message 묶음이 아니라 byte stream으로 보이는지 이해하는 것이다. sequence number가 있기 때문에 kernel은 segment 경계를 지우고 byte 순서만 맞춘 물탱크처럼 보여줄 수 있다.

#### 질문 47

TCP 3-way handshake의 구체적인 단계는 무엇인가?

#### 답변 47

**핵심:** TCP 연결은 `SYN -> SYN+ACK -> ACK` 세 단계로 열린다. client가 먼저 "연결하고 싶다"는 SYN을 보내고, server는 "요청 받았고 나도 준비됐다"는 SYN+ACK을 돌려준다. client가 마지막으로 "네 응답도 받았다"는 ACK을 보내면 연결이 성립한다.

**비유/시스템 연결:** 비유하면 전화를 걸기 전에 서로 통화 가능한지 확인하는 과정이다. client가 "들리세요?"라고 묻고, server가 "들립니다, 제 말도 들리나요?"라고 답하고, client가 "네, 들립니다"라고 마무리하는 식이다. 이 과정이 끝난 뒤에야 HTTP 요청 같은 실제 데이터가 TCP byte stream 위로 흐른다.

#### 질문 48

TCP는 송신자가 50B씩 두 번 보내도 수신자가 100B를 한 번에 읽을 수 있는 이유가 무엇인가?

#### 답변 48

**핵심:** TCP는 message boundary를 보존하지 않고 byte stream을 제공하기 때문이다. 송신자가 `write(50)`을 두 번 호출해도 TCP는 그것을 "50B짜리 메시지 2개"로 application에 보장하지 않는다. 수신 kernel은 sequence number를 보고 byte 순서만 맞춰 receive buffer에 이어 붙인다.

**비유/시스템 연결:** 비유하면 UDP는 상자 단위 택배라서 상자 경계가 유지되지만, TCP는 수도관에 흐르는 물이다. 보낸 사람이 컵 두 번으로 물을 부어도, 받는 사람은 물탱크에 고인 100ml를 한 번에 퍼갈 수 있다. 그래서 TCP application protocol은 줄바꿈, Content-Length 같은 별도 규칙으로 message 경계를 정해야 한다.

## q04. IP 주소와 byte order

> 전후 사정: q04의 원래 대표 질문은 IPv4/IPv6 주소 크기와 network byte order였다. 아래 질문은 그중 특히 `htons(80)`처럼 값과 메모리 byte 배열이 다르게 보이는 지점에서 나온 하부 질문이다.

### 질문 49

`htons(80)` 결과가 왜 `0x5000`처럼 보이는데 실제 네트워크에서는 80으로 해석되는가?

### 답변 49

**핵심:** 핵심은 값 자체와 메모리 byte 배열을 구분해야 한다는 점이다. 포트 80은 수학적으로 `0x0050`이다. little endian host 메모리에 그냥 저장하면 byte 순서가 `[0x50, 0x00]`이 된다. 그런데 네트워크는 big endian을 표준으로 쓰므로, wire에는 `[0x00, 0x50]` 순서로 나가야 한다.

**비유/시스템 연결:** `htons(80)`은 host byte order를 network byte order에 맞는 byte 배열이 되도록 바꾼다. little endian machine에서 그 결과를 정수로 찍어 보면 `0x5000`처럼 보일 수 있지만, 메모리에 놓인 byte 순서는 네트워크가 기대하는 `[0x00, 0x50]`이 된다. 비유하면 말은 같은데 종이에 적는 방향이 다르기 때문에, 상대가 읽는 방향에 맞춰 미리 뒤집어 적어 주는 것이다.

## q05. DNS와 Cloudflare

> 전후 사정: q05의 원래 대표 질문은 DNS, domain registration, Cloudflare를 통해 domain이 실제 server 접속으로 이어지는 과정이었다. 아래 질문은 그중 Cloudflare의 proxied mode가 왜 origin IP를 숨긴다고 하는지 확인하려고 나온 하부 질문이다.

### 질문 50

Cloudflare의 proxied mode는 왜 실제 origin IP를 숨긴다고 하는가?

### 답변 50

**핵심:** 일반 DNS only 모드에서는 사용자가 domain을 조회하면 origin server의 실제 IP가 응답된다. 반면 Cloudflare proxied mode에서는 DNS 응답으로 Cloudflare edge IP가 나온다. 사용자는 origin으로 직접 가지 않고 Cloudflare edge에 먼저 접속한다.

**비유/시스템 연결:** 비유하면 내 집 주소를 공개하지 않고, 대형 경비실 주소만 공개하는 것과 같다. 방문자는 먼저 경비실(Cloudflare edge)에 도착하고, 경비실이 정상 손님인지 확인한 뒤 내부적으로 내 집(origin)으로 전달한다. 이 과정에서 DDoS 완화, TLS 종료, cache 같은 기능도 함께 제공될 수 있다.

## q06. Socket, fd, FDT, Unix I/O

> 전후 사정: q06의 원래 대표 질문은 socket이 단순한 "연결 끝점"이라는 설명을 넘어, kernel 자료구조와 하드웨어 관점에서 어떻게 구현되는지였다. 아래 질문들은 fd, FDT, v-node, i-node, socket buffer, Unix I/O 추상화를 한 그림으로 연결하려고 나온 질문들이다.

> 비유로 먼저 잡기: q06은 "리모컨 버튼 하나가 실제 장비까지 어떻게 이어지는가"를 보는 장면이다. user program은 fd라는 번호표만 본다. process의 FDT는 개인 전화번호부이고, open file table은 실제 통화 기록 또는 열린 창구 기록이며, v-node/i-node는 파일의 주민등록증과 보관 위치다. socket도 같은 fd 체계에 들어오므로, 네트워크 통신도 파일처럼 `read/write/close`로 다룰 수 있다. 다만 socket은 되감기 가능한 USB 파일이 아니라 흐르는 파이프다.

### q06-1. Socket과 kernel object

> 이 묶음의 초점: user program이 보는 `sockfd` 정수 하나가 kernel 내부의 어떤 객체 체인으로 이어지는지 이해하는 것이다. 핵심 비유는 "사용자는 계좌번호만 보지만, 은행 내부에는 계좌 상태와 거래 기록이 따로 있다"이다.

#### 질문 51

소켓은 물리적으로 어디에 있는가?

#### 답변 51

**핵심:** 소켓은 랜선 구멍 같은 물리 장치가 아니라 kernel이 만든 논리적 endpoint다. 사용자 프로그램에는 `sockfd`라는 정수 하나로 보이지만, kernel 안에서는 `struct file -> struct socket -> struct sock` 같은 자료구조 체인으로 이어진다. 실제 data는 DRAM의 socket buffer, `sk_buff` queue, NIC ring buffer를 통해 흐른다.

**비유/시스템 연결:** 비유하면 "은행 계좌"와 비슷하다. 사용자는 계좌번호만 보지만, 은행 내부에는 잔액, 거래내역, 상태, 연결된 카드 같은 많은 데이터가 있다. 소켓도 사용자는 fd 번호만 보지만, kernel 내부에는 protocol 상태, receive queue, send queue, TCP 상태 등이 붙어 있다.

#### 질문 52

fd는 무엇이고 책 어디에서 나오는가?

#### 답변 52

**핵심:** fd(file descriptor)는 kernel이 열린 파일, socket, terminal 같은 I/O 대상을 user process에게 대신 가리키라고 주는 작은 정수 handle이다. CSAPP에서는 10장 Unix I/O에서 본격적으로 등장하고, 11장 network programming에서는 socket도 fd로 다룬다는 점이 이어진다.

**비유/시스템 연결:** 비유하면 fd는 리모컨 버튼 번호다. 사용자는 "3번 버튼에 써 줘"라고 `write(3, ...)`를 호출하지만, 3번이 실제로 디스크 파일인지 socket인지 terminal인지는 kernel이 fd table을 보고 찾아낸다. 그래서 같은 `read/write/close` 인터페이스로 다양한 I/O 대상을 다룰 수 있다.

#### 질문 53

FDT는 뭐라고 부르면 되고, 정확히 무엇인가?

#### 답변 53

**핵심:** FDT는 file descriptor table, CSAPP 번역으로는 식별자 테이블이라고 부르면 된다. 각 process가 자기만의 FDT를 가지고 있고, fd 숫자는 이 table의 index다. FDT의 각 칸은 open file table의 항목을 가리킨다.

**비유/시스템 연결:** 비유하면 각 프로그램이 자기만의 개인 전화번호부를 들고 있는 것이다. 카카오톡의 fd 3번과 브라우저의 fd 3번은 숫자는 같아도, 각자의 전화번호부에서 전혀 다른 대상을 가리킬 수 있다. 이 구조 덕분에 process끼리 fd 번호가 충돌하지 않고 독립적으로 I/O를 할 수 있다.

#### 질문 54

fd, v-node, i-node의 연결 관계는 어떻게 되는가?

#### 답변 54

**핵심:** 흐름은 `process의 fd -> descriptor table -> open file table -> v-node table -> i-node/file metadata -> disk`로 이어진다. fd는 process가 가진 작은 번호이고, descriptor table은 그 번호를 open file table entry로 연결한다. open file table은 현재 file position과 reference count를 갖고, v-node는 파일의 실제 metadata를 대표한다.

**비유/시스템 연결:** 비유하면 fd는 사물함 번호표, descriptor table은 "이 번호표가 어느 사물함 줄로 가는지" 적힌 안내판, open file table은 현재 사용 중인 사물함 기록, v-node/i-node는 그 사물함 안 물건의 진짜 신분증과 보관 위치에 가깝다. 사용자는 번호표만 들지만, kernel은 여러 table을 따라 실제 파일까지 찾아간다.

### q06-2. Unix I/O 추상화

> 이 묶음의 초점: "소켓도 파일이다"라는 말을 제대로 이해하는 것이다. 모든 외부 장치를 진짜 저장 파일처럼 생각하라는 뜻이 아니라, kernel이 fd/read/write라는 같은 인터페이스로 I/O 대상을 추상화했다는 뜻이다.

#### 질문 55

식별자 테이블을 프로세스마다 가지는 이유는 무엇이고, 결국 fd와 FDT는 무엇을 하려고 있는가?

#### 답변 55

**핵심:** 목적은 복잡한 I/O 대상을 process별로 안전하게 추상화하기 위해서다. 각 process가 자기 FDT를 가지면 같은 fd 번호라도 서로 다른 파일이나 socket을 가리킬 수 있다. 그래서 process 간 충돌을 막고, 각자 독립적인 I/O 세계를 가진 것처럼 동작할 수 있다.

**비유/시스템 연결:** 더 큰 목적은 "모든 것은 파일이다"라는 Unix I/O 철학이다. disk file, terminal, pipe, socket처럼 실제 구현은 완전히 달라도 user program은 fd와 `read/write/close`로 같은 방식으로 다룬다. FDT는 이 단순한 번호를 kernel의 실제 object로 이어 주는 번역표다.

#### 질문 56

모든 외부 장치를 하나의 I/O로 보고 똑같이 처리하는 시스템을 만든 것인가?

#### 답변 56

**핵심:** 맞다. Unix/Linux는 가능한 많은 I/O 대상을 "연속된 byte를 읽고 쓰는 파일"처럼 모델링한다. 그래서 keyboard, monitor, disk file, network socket이 모두 fd를 통해 다뤄진다. 실제 장치별 차이는 kernel과 driver가 감춘다.

**비유/시스템 연결:** 비유하면 다양한 가전제품을 모두 같은 모양의 콘센트에 꽂게 만든 것과 같다. 내부에서 전자레인지와 노트북이 쓰는 방식은 다르지만, 사용자는 일단 플러그와 콘센트라는 통일된 interface를 쓴다. 프로그래머에게는 fd와 read/write가 그 콘센트 역할을 한다.

#### 질문 57

소켓도 결국 파일이라는 말 때문에, 모든 외부 장치를 내용이 계속 업데이트되는 USB처럼 생각해도 되는가?

#### 답변 57

**핵심:** 초기 비유로는 꽤 좋다. fd를 통해 byte를 읽고 쓴다는 점에서는 socket도 USB 파일처럼 "byte가 있는 대상"으로 볼 수 있다. 하지만 socket은 저장 파일이 아니라 되감기 없는 stream/queue에 가깝다는 점을 꼭 구분해야 한다.

**비유/시스템 연결:** USB 파일은 `lseek`으로 앞뒤 위치를 옮겨 다시 읽을 수 있지만, TCP socket은 흐르는 물이나 컨베이어 벨트처럼 지나간 byte를 되감아 읽을 수 없다. 그래서 socket은 "계속 갱신되는 USB"보다는 "양방향으로 byte가 흐르는 파이프"라고 생각하면 더 정확하다.

#### 질문 58

socket fd는 수신할 때 어떻게 작동하는가?

#### 답변 58

**핵심:** network에서 들어온 데이터는 NIC가 DMA로 kernel receive buffer에 올린다. kernel TCP/IP stack은 header를 처리하고, 해당 socket의 receive queue에 payload를 쌓는다. application이 `read(sockfd, buf, n)`을 호출하면 kernel은 fd table을 통해 이 socket을 찾고, receive queue의 데이터를 user buffer로 복사한다.

**비유/시스템 연결:** 비유하면 NIC가 계속 물류센터 창고에 택배를 넣어 두고, application은 fd라는 번호표를 들고 창구에 가서 "내 택배 n바이트만큼 주세요"라고 요청하는 구조다. 물건이 없으면 기다릴 수 있고, 물건이 일부만 있으면 short read가 발생할 수 있다.

#### 질문 59

socket fd는 송신할 때 어떻게 작동하는가?

#### 답변 59

**핵심:** application이 `write(sockfd, buf, n)`을 호출하면 kernel은 fd table을 따라 socket object를 찾고, user buffer의 데이터를 kernel send buffer로 복사한다. 그 뒤 TCP/IP stack이 header를 붙이고, NIC driver가 NIC의 TX ring에 descriptor를 등록한다. NIC는 DMA로 DRAM에서 frame을 읽어 선로로 내보낸다.

**비유/시스템 연결:** 비유하면 user buffer는 손님의 장바구니이고, kernel send buffer는 물류센터 접수 창고다. `write`는 장바구니 물건을 창고에 맡기는 행위이고, 실제 배송은 그 뒤 직원(kernel)과 트럭(NIC)이 자기 속도에 맞춰 진행한다. 그래서 `write` return은 보통 "접수 완료"이지 "상대가 읽음"이 아니다.

## q07. Socket interface 조립 순서

> 전후 사정: q07의 원래 대표 질문은 socket interface 함수들을 client/server에서 어떤 순서로 호출하는지였다. 아래 질문은 q01~q06에서 이미 배운 개별 부품들이 q07에서 새로 등장한 것인지, 아니면 조립 순서로 다시 나온 것인지 확인하려고 나온 질문이다.

### 질문 60

q07의 socket interface 호출 순서는 이미 앞에서 다 배운 내용인가?

### 답변 60

**핵심:** 부품은 이미 배웠고, q07은 그 부품을 조립하는 순서도에 가깝다. 앞에서는 `socket`, `bind`, `listen`, `accept`, `connect`, `getaddrinfo` 각각의 역할을 따로 배웠다. q07에서는 client와 server가 실제 코드에서 어떤 순서로 이 함수들을 호출하는지 전체 lifecycle로 본다.

**비유/시스템 연결:** 비유하면 엔진, 바퀴, 핸들, 브레이크가 무엇인지는 앞에서 배웠고, q07은 자동차 조립 설명서다. 서버는 `getaddrinfo -> socket -> bind -> listen -> accept`, 클라이언트는 `getaddrinfo -> socket -> connect` 흐름으로 움직인다. 그래서 q07은 새로운 이론보다는 실전 코드 연결 단계라고 보면 된다.

## q08. TCP stream과 UDP datagram

> 전후 사정: q08의 원래 대표 질문은 echo server와 TCP byte stream/UDP datagram의 차이였다. 아래 질문은 "더 까다로운 TCP가 왜 오히려 자유롭게 read 크기를 정할 수 있게 해 주는가"라는 역설을 이해하려고 나온 하부 질문이다.

### 질문 61

더 까다로운 TCP가 어떻게 자유롭게 100이라는 물을 퍼다 주는가?

### 답변 61

**핵심:** 오히려 TCP가 더 까다롭게 sequence number를 관리하기 때문에 application에는 자유로운 byte stream을 제공할 수 있다. TCP는 "몇 번째 packet인가"보다 "전체 stream에서 몇 번째 byte인가"를 관리한다. 그래서 여러 segment로 쪼개져 와도 kernel이 순서를 맞춰 receive buffer에 이어 붙인다.

**비유/시스템 연결:** 비유하면 TCP는 상자 경계가 아니라 물방울 번호를 관리하는 물탱크다. 송신자가 50ml씩 두 번 부어도, 받는 쪽 물탱크에는 100ml가 순서대로 고인다. application은 그 물탱크에서 10ml씩 퍼도 되고 100ml를 한 번에 퍼도 된다. 그래서 TCP는 정확하지만 message 경계는 application protocol이 직접 정해야 한다.

## q09. HTTP / FTP / MIME / Telnet

> 전후 사정: q09의 원래 대표 질문은 HTTP와 FTP, MIME type, Telnet, HTTP/1.0과 1.1의 차이를 정리하는 것이었다. 아래 질문들은 "HTTP가 단순 파일 전송과 무엇이 다른지", "Telnet으로 HTTP를 직접 말해볼 수 있다는 게 무슨 뜻인지"를 확인하는 흐름이다.

### 질문 62

HTTP와 FTP는 무엇이 다르고, 왜 HTTP가 웹에 더 잘 맞는가?

### 답변 62

**핵심:** FTP는 파일 전송에 초점을 둔 protocol이고, control connection과 data connection을 따로 사용하는 구조다. 서버가 로그인 상태나 현재 디렉터리 같은 상태도 기억한다. 반면 HTTP는 request/response 중심이고, 기본적으로 각 요청이 독립적인 stateless 구조다.

**비유/시스템 연결:** 웹에는 HTTP가 더 잘 맞는다. HTML, image, CSS, JavaScript, API 응답처럼 다양한 content를 MIME type으로 실어 보낼 수 있고, browser가 이를 해석해 화면을 만든다. 비유하면 FTP는 창고에서 파일 상자를 꺼내 주는 서비스이고, HTTP는 문서, 이미지, 링크를 조합해 웹 페이지라는 전시장을 구성하는 운송 규칙이다.

### 질문 63

Telnet으로 HTTP를 테스트할 수 있다는 말은 무슨 뜻인가?

### 답변 63

**핵심:** HTTP/1.0, HTTP/1.1의 기본 request/response는 사람이 읽을 수 있는 text line으로 구성된다. Telnet은 특정 host:port에 TCP 연결을 열고 사용자가 직접 text를 입력할 수 있게 해 주는 도구다. 그래서 browser 없이도 Telnet으로 80번 port에 연결해 `GET / HTTP/1.0` 같은 HTTP request line을 직접 보낼 수 있다.

**비유/시스템 연결:** 비유하면 browser가 평소 대신 전화해서 정해진 문장을 말해 주던 것을, 내가 직접 전화기를 들고 서버에게 같은 문장을 말해 보는 것이다. 이 실험을 통해 HTTP가 결국 TCP 연결 위에 정해진 text protocol을 주고받는 방식임을 확인할 수 있다.

## q10. CPU / memory / kernel / fd 관점

> 전후 사정: q10의 원래 대표 질문은 network I/O 한 번을 CPU, memory, kernel, fd라는 네 관점으로 나누어 다시 보는 것이었다. 아래 질문은 이 네 관점이 왜 필요한지, 같은 통신 장면이 관점마다 어떻게 다르게 보이는지 정리하기 위해 나온 질문이다.

### 질문 64

network 통신 한 번을 CPU, memory, kernel, fd 관점으로 나누어 본다는 것은 무슨 의미인가?

### 답변 64

**핵심:** 같은 network I/O도 어떤 렌즈로 보느냐에 따라 다르게 보인다. CPU 관점에서는 system call, copy, checksum, interrupt 처리처럼 cycle을 쓰는 일이 보인다. memory 관점에서는 user buffer, kernel socket buffer, `sk_buff`, NIC DMA buffer 사이로 byte가 이동하는 모습이 보인다.

**비유/시스템 연결:** kernel 관점에서는 socket layer, TCP/UDP layer, IP layer, driver가 이어지는 함수 체인이 보인다. fd 관점에서는 user가 가진 정수 하나가 fd table을 통해 `struct file`, `struct socket`, `struct sock`으로 이어지는 handle 구조가 보인다. 비유하면 같은 택배 배송을 고객, 창고 직원, 물류 시스템, 운송장 번호 관점에서 각각 보는 것이다.

## q11. CGI / fork / dup2

> 전후 사정: q11의 원래 대표 질문은 CGI가 client 인자를 어떻게 받고, fork/execve/dup2를 이용해 동적 content를 어떻게 만드는지였다. 아래 질문들은 `fork`, `dup2`, GET/POST 인자 전달이 각각 어떤 역할을 하는지 다시 쪼개서 확인한 것이다.

> 비유로 먼저 잡기: CGI는 식당 본점(web server)이 주문을 받으면, 주방 보조(child process)를 하나 만들고, 그 보조를 특정 요리사 프로그램(CGI executable)으로 갈아입히는 구조다. GET 인자는 주문서 위쪽에 적힌 짧은 메모(`QUERY_STRING`)이고, POST body는 별도 박스로 들어오는 긴 재료(stdin)다. `dup2`는 요리사가 접시에 담아 내는 출구(stdout)를 손님 테이블(client socket)로 직접 연결하는 작업이다.

### 질문 65

`fork`는 무엇이고 어디서 쓰이는가?

### 답변 65

**핵심:** `fork`는 현재 process를 복제해서 child process를 만드는 system call이다. 부모와 자식은 거의 같은 주소 공간과 열린 fd들을 가진 상태로 시작하지만, `fork` return 값이 다르기 때문에 서로 다른 흐름으로 나뉠 수 있다. CSAPP에서는 8장 process 제어에서 배우고, 11장 CGI와 동시성 server에서 다시 중요해진다.

**비유/시스템 연결:** 비유하면 식당 사장이 자기와 같은 작업 환경을 가진 직원을 한 명 복제해 내는 것과 같다. Tiny server는 동적 content를 처리할 때 child를 만들고, 그 child가 CGI program으로 변신해서 계산 결과를 client에게 보낸다.

### 질문 66

`dup2`는 무엇이고 CGI에서 왜 중요한가?

### 답변 66

**핵심:** `dup2(oldfd, newfd)`는 `newfd`가 가리키던 대상을 닫고, `oldfd`가 가리키는 열린 파일 또는 socket을 `newfd`도 가리키게 만든다. CGI에서는 child process의 stdout(fd 1)을 client socket fd로 바꾸기 위해 사용한다.

**비유/시스템 연결:** 비유하면 원래 모니터로 나가던 수도관을 client socket으로 연결된 호스로 갈아끼우는 것이다. 그 뒤 CGI program은 아무것도 모르고 `printf`를 호출하지만, 출력은 화면이 아니라 browser로 흘러간다. 이게 CGI가 network를 직접 몰라도 HTTP 응답을 만들 수 있는 핵심이다.

### 질문 67

GET 요청의 인자는 CGI 프로그램에게 어떻게 전달되는가?

### 답변 67

**핵심:** GET 요청에서는 URI의 `?` 뒤 문자열이 인자다. Tiny는 `/cgi-bin/adder?15000&213` 같은 요청을 받으면 `?` 뒤의 `15000&213`을 잘라내고, child process에서 `QUERY_STRING` 환경변수로 설정한다. CGI program은 `getenv("QUERY_STRING")`으로 이 값을 읽는다.

**비유/시스템 연결:** 비유하면 손님이 주문서 본문에 "15000&213"이라는 메모를 붙여 보냈고, 서버가 그 메모를 하청업체(CGI program)의 작업 지시서 칸에 옮겨 적어 주는 것이다. CGI program은 네트워크를 직접 보지 않고도 환경변수라는 전달 쪽지를 읽어 계산할 수 있다.

### 질문 68

POST 요청의 인자는 CGI 프로그램에게 어떻게 전달되는가?

### 답변 68

**핵심:** POST 요청의 인자는 보통 URI가 아니라 HTTP request body에 들어간다. 서버는 CGI child의 stdin(fd 0)을 client socket 쪽으로 연결하거나, request body를 CGI의 표준 입력으로 읽을 수 있게 구성한다. 또한 `CONTENT_LENGTH`, `CONTENT_TYPE` 같은 환경변수로 body의 길이와 형식을 알려준다.

**비유/시스템 연결:** 비유하면 GET은 짧은 메모를 작업 지시서 위쪽에 붙여 주는 방식이고, POST는 별도의 박스에 담긴 긴 서류를 입력 창구로 밀어 넣는 방식이다. CGI program 입장에서는 keyboard에서 입력을 읽듯 stdin을 읽으면 client가 보낸 body를 받는 셈이다.
