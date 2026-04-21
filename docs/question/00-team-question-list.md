# WK08-1 팀 질문 트리 (CSAPP Ch.11 기반)

> 화이트보드에 탑다운으로 그려가며 설명하기 위한 계층 구조.
> 원 작성자 표기는 질문 끝 `(이름)`. 중복·동일 질문은 `(이름1, 이름2[, 이름3])`로 병합.
> 원 질문 텍스트는 보존(누락·삭제 없음, 병합으로만 축약).

## 계층 개요

```
[루트] 네트워크 프로그래밍 (CSAPP Ch.11)
├── L1.  클라이언트·서버 & 계층 모델            (11.1 / 복습)
├── L2.  네트워크 하드웨어                       (11.2)
├── L3.  IP 주소 & 바이트 순서                   (11.3.1~11.3.2)
├── L4.  DNS & 도메인                            (11.3.3)
├── L5.  Unix I/O & FD 추상화                    (Ch.10 + 11장)
├── L6.  소켓 원리 & 커널 자료구조               (11.4 전단)
├── L7.  Sockets Interface                       (11.4)
├── L8.  TCP vs UDP & 3-way handshake            (11.3.4 + Ch.11 전반)
├── L9.  커널 송신 파이프라인 (write → NIC)      (11.4 + Ch.10/Ch.9 접합)
├── L10. I/O Bridge & DMA                        (Ch.6 × 11장 심화)
├── L11. 4관점 통합 (CPU·메모리·커널·fd)         (복습/통합)
├── L12. HTTP / FTP / MIME / Telnet              (11.5.1~11.5.3)
├── L13. Tiny Web Server                         (11.6)
├── L14. CGI & fork / execve / dup2              (11.5.4 + Ch.8)
├── L15. Echo Server & EOF                       (11.4 예제)
├── L16. Proxy (Proxy Lab)                       (본문 외 확장)
├── L17. 동시성 (스레드 풀·async I/O·락)         (Ch.12)
├── L18. 프로세스 족보 & fd 수명                 (Ch.8 × 11장)
├── L19. 가상 메모리 & 프로세스 레이아웃         (Ch.9)
└── L20. 코드 디테일 (adder.c)                   (11.5.4 예제)
```

---

## L1. 클라이언트·서버 & 계층 모델

> CSAPP 11.1 · 키워드: `client-server transaction`, `TCP/IP 4계층`, `encapsulation`, `Ethernet ↔ Internet`

- **1-1. 트랜잭션 정의**
  - 클라이언트-서버 트랜잭션은 정확히 무엇인가? 요청, 처리, 응답, 완료 네 단계를 통틀어 트랜잭션이라고 부르는가? (최현진)
- **1-2. TCP/IP 4계층 & Encapsulation**
  - 네트워크는 계층적 구조를 가진다. TCP/IP 4계층을 기준으로 데이터를 보낼 때 어떤 과정을 거쳐서 네트워크에 데이터가 전송되는가? (이호준)
  - 클라이언트 컴퓨터에서 출발한 데이터가 네트워크에서 어떤 과정을 거쳐서 서버 컴퓨터에 도착하는가? (이호준)
  - TCP/IP가 프로토콜 소프트웨어라면, 예전에 배운 encapsulation은 사라지는 것인가? (최현진)
  - IP는 인터넷 상위망에서 쓰이는 주소인데, 상위망에서 또 상위망으로 갈 때 따로 감싸는가? (최현진)
- **1-3. Ethernet vs Internet**
  - Ethernet과 Internet은 무엇이 다른가? (최현진)

---

## L2. 네트워크 하드웨어

> CSAPP 11.2 · 키워드: `Ethernet`, `LAN/WAN`, `hub/bridge/router`, `MAC address`, `frame vs packet`, `routing`, `TTL`

- **2-1. 이더넷 / 브릿지 / 허브 / 라우터**
  - 이더넷, 브릿지, 라우터는 무엇이 다른가. 이더넷과 브릿지는 대역폭만 다른가? (최우녕)
  - 실제 생활에서 Ethernet, hub, bridge, router, LAN, WAN은 어떤 장비로 보면 되는가? (최현진)
  - 더미 허브가 무엇인가? (최현진)
- **2-2. LAN / WAN**
  - LAN과 WAN은 무엇이 다른가. (최우녕)
  - LAN과 WAN은 상대적인 말인가, 절대적인 말인가? (최현진)
- **2-3. MAC 주소 (Ethernet adapter)**
  - Ethernet adapter가 주소를 가지는 이유는 무엇인가? (최현진)
- **2-4. frame vs packet**
  - frame과 packet은 하는 일이 비슷한데, 둘은 무엇이 다른가? (최현진)
- **2-5. 비트 뭉치의 형태와 수신 식별**
  - 호스트들은 결국 비트를 주고받는다고 하는데, 그 "비트 뭉치"는 어떤 형태이며 어떻게 수신자가 자기 데이터라는 것을 아는가. (최우녕)
- **2-6. 라우팅 & 재포장 (IP header, MAC header, TTL)**
  - 왜 Ethernet header가 가장 바깥 포장지에 있는가? (최현진)
  - 라우팅 테이블이 실제 목적지의 모든 세부 위치를 모르면 어떻게 목적지를 찾아가는가? (최현진)
  - "다른 네트워크 사이에서 IP를 보고 전달한다"는 말이 무슨 뜻인가? (최현진)
  - IPv4 header 안에 최종 주소가 있는데, 왜 굳이 MAC으로 다시 포장해서 보내는가? (최현진)
  - router R을 지날 때 IP header는 유지되고 MAC header는 바뀐다는 것은 실제로 어떤 모습인가? (최현진)
  - TTL은 어떤 근거로 처음 값이 정해지는가? (최현진)

---

## L3. IP 주소 & 바이트 순서

> CSAPP 11.3.1 ~ 11.3.2 · 키워드: `IPv4 32bit`, `IPv6 128bit`, `big/little endian`, `htons/htonl/ntohs/ntohl`, `inet_pton/inet_ntop`

- **3-1. IPv4 vs IPv6 형식·크기**
  - IPv4와 IPv6의 주소 형식과 크기는 어떻게 다른가. (최우녕)
- **3-2. Endianness & htons/htonl**
  - htons, ntohs, htonl, ntohl은 왜 써야 하는가. 안 쓰면 무슨 일이 벌어지는가. (최우녕)
  - big endian과 little endian은 무엇이고, IP 주소와 무슨 관련이 있는가? (최현진)
  - htons(80) 결과가 왜 0x5000처럼 보이는데 실제 네트워크에서는 80으로 해석되는가? (최현진)
- **3-3. 문자열 ↔ 바이너리 변환**
  - inet_pton과 inet_ntop은 정확히 무엇을 바꾸는 함수인가? (최현진)
- **3-4. 32비트로 전 세계 주소 표현**
  - IPv4는 32비트 정수 한 개라는데, 어떻게 그것만으로 전 세계 주소를 표현하는가. (최우녕)
  - 111.111... 같은 IP 주소에 프로토콜 소프트웨어의 핵심 기능인 주소 정보가 들어 있으니, 프로그래머가 복잡한 코드를 짤 필요가 없다는 뜻인가? (최현진)

---

## L4. DNS & 도메인

> CSAPP 11.3.3 · 키워드: `DNS`, `domain hierarchy`, `resolve`, `mapping (1:1/N:1/1:N)`, `Cloudflare`

- **4-1. DNS 역할**
  - DNS는 무엇이고 어떤 용도로 쓰이는가. (최우녕)
- **4-2. 도메인 계층 트리 & 관리**
  - 도메인의 계층적 트리 구조는 그냥 .kr, .com, amazon, tistory 같은 것을 누가 관리하는지 나타내는 것인가? (최현진)
- **4-3. 등록(소유) & 해석(resolve) 흐름**
  - 도메인 이름은 어떻게 등록(소유)되고, 어떻게 IP로 해석(resolve)되는가. (최우녕)
  - 데이터를 보낼 때 항상 DNS 서버로 먼저 가서 실제 주소가 있는지 확인한 뒤 보내는가? (최현진)
  - 목적지 IP를 아예 모르는 상태에서는 어떻게 통신을 시작하는가? (최현진)
- **4-4. DNS 매핑 유형**
  - DNS의 1대1, 다대1, 일대다 매핑은 실제로 어떤 상황에 쓰이는가? (최현진)
- **4-5. DNS 서버 주소 부트스트랩**
  - DNS 서버의 주소는 어떻게 아는가? (최현진)
- **4-6. Cloudflare / Proxied Mode**
  - Cloudflare에서 도메인을 구매하고 DNS를 등록하면 접속이 가능해지는데, 그 과정을 단계별로 설명해 달라. (최우녕)
  - Cloudflare의 proxied mode는 왜 실제 origin IP를 숨긴다고 하는가? (최현진)

---

## L5. Unix I/O & FD 추상화

> CSAPP Ch.10 + 11장 · 키워드: `VFS`, `superblock/inode/dentry/file`, `ext4`, `page cache`, `FDT`, `fd`, `v-node`, `0/1/2`, `dup2`, `RIO`

- **5-1. VFS 4객체 관계**
  - VFS 네 개의 핵심 객체(superblock / inode / dentry / file)는 어떤 관계이고 커널에서 어떻게 표현되나? (최우녕)
- **5-2. ext4 디스크 레이아웃**
  - 디스크 위의 ext4는 실제로 어떤 레이아웃으로 쓰여 있나? (최우녕)
- **5-3. open() / read() 커널 내부 흐름**
  - open("/home/woonyong/a.txt")는 커널 안에서 몇 단계를 거치나? (최우녕)
  - read() 한 번이 VFS → FS → page cache → block layer → 디스크로 어떻게 흐르나? (최우녕)
- **5-4. "모든 것은 파일" — 소켓·파이프·procfs**
  - 소켓·파이프·procfs가 왜 "파일"처럼 보이나? (최우녕)
  - 모든 외부 장치를 하나의 I/O로 보고 똑같이 처리하는 시스템을 만든 것인가? (최현진)
  - 소켓도 결국 파일이라는 말 때문에, 모든 외부 장치를 내용이 계속 업데이트되는 USB처럼 생각해도 되는가? (최현진)
  - 소켓은 주소가 저장되는 구조체인가, 파일 같은 것인가? (최현진)
- **5-5. FD / FDT / file table / v-node / i-node 구조 — [병합]**
  - fd는 무엇이고 책 어디에서 나오는가? (최현진)
  - FDT는 뭐라고 부르면 되고, 정확히 무엇인가? (최현진)
  - FD table에서 file table entry 주소를 어떻게 찾아가는거지? + file table, v-node에 대한 정보는 어디서 가지고 오는거지? / fd, v-node, i-node의 연결 관계는 어떻게 되는가? (정범진, 최현진)
- **5-6. 프로세스별 FDT 분리 이유**
  - 식별자 테이블을 프로세스마다 가지는 이유는 무엇이고, 결국 fd와 FDT는 무엇을 하려고 있는가? (최현진)
- **5-7. stdin/stdout/stderr ↔ 0/1/2 약속**
  - stdin/stdout/stderr가 "꼭 0/1/2여야" 하는 이유가 있나? 커널이 fd 번호를 구분하나? (최우녕)
- **5-8. dup2로 fd 번호 재배치**
  - 3번 fd에 열린 소켓을 0/1번에 꽂으려면 어떻게 해야 하나? (최우녕)
- **5-9. 버퍼링 (libc vs 커널)**
  - "unbuffered", "line-buffered", "flush"가 무슨 소리인가? 커널 버퍼? libc 버퍼? (최우녕)
- **5-10. RIO vs 일반 file 함수**
  - rio 함수들은 일반적인 file 함수와 어떤 차이점을 가지고 있는거지? (정범진)
- **5-11. 터미널 vs GUI 실행 시 fd 0/1/2**
  - 앱을 터미널에서 켰을 경우와 GUI 아이콘 클릭으로 켰을 경우 FD table에서 0, 1, 2번 값이 다른 이유? (터미널인 경우 0, 1, 2 모두 /dev/ttys000, GUI인 경우 /dev/null) (정범진)
- **5-12. V-node의 존재 이유**
  - V-node는 왜 필요한거지? (정범진)

---

## L6. 소켓 원리 & 커널 자료구조

> CSAPP 11.4 전단 · 키워드: `socket endpoint`, `struct socket`, `struct sock`, `sockfd → kernel object`

- **6-1. 소켓의 물리적/SW 위치 — [병합]**
  - 소켓은 "연결의 끝점"이라고만 들었다. 실제 물리적으로, 그리고 소프트웨어적으로는 어떻게 구현되어 있는가. / 소켓은 물리적으로 어디에 있는가? (최우녕, 최현진)
- **6-2. sockfd → 커널 자료구조**
  - sockfd라는 정수 하나로 커널의 어떤 자료구조가 접근되는가. (최우녕)
- **6-3. 소켓 fd의 수신 동작**
  - socket fd는 수신할 때 어떻게 작동하는가? (최현진)
- **6-4. 소켓 fd의 송신 동작**
  - socket fd는 송신할 때 어떻게 작동하는가? (최현진)

---

## L7. Sockets Interface

> CSAPP 11.4 · 키워드: `socket/bind/listen/accept/connect`, `listening vs connected socket`, `getaddrinfo`, `struct addrinfo`, `open_clientfd/open_listenfd`

- **7-1. 함수 & 호출 순서 — [병합]**
  - 11.4장에 등장하는 함수와 호출 순서를 정리해 달라. / q07의 socket interface 호출 순서는 이미 앞에서 다 배운 내용인가? (최우녕, 최현진)
- **7-2. 클라이언트: getaddrinfo → socket → connect**
  - getaddrinfo를 먼저 부른 뒤 socket, connect를 차례로 실행한다는 말은 구체적으로 어떤 코드인가. (최우녕)
- **7-3. 서버: socket → bind → listen → accept**
  - 서버에서 socket -> bind -> listen -> accept가 각각 어떤 역할인가? (최현진)
- **7-4. Listening vs Connected socket**
  - listening socket과 connected socket은 왜 따로 있는가? (최현진)
- **7-5. open_listenfd / open_clientfd 래퍼**
  - open_listenfd는 bind, listen, accept를 모두 한 번에 해 주는가? (최현진)
- **7-6. struct addrinfo 필드 & memset**
  - struct addrinfo의 각 필드는 무슨 뜻이고 어떤 역할을 하는가. (최우녕)
  - addrinfo 구조체를 memset으로 0 초기화하지 않으면, 초기화되지 않은 필드의 쓰레기값 때문에 getaddrinfo()가 어떤 문제를 일으킬 수 있나요? (+ 쓰레기값은 무슨 값이 들어가게 되는지) (이우진)
- **7-7. getaddrinfo vs inet_pton**
  - getaddrinfo는 inet_pton처럼 IP 문자열을 32비트 주소로 바꾸는 함수인가? (최현진)

---

## L8. TCP vs UDP & 3-way handshake

> CSAPP 11.3.4 + Ch.11 전반 · 키워드: `SOCK_STREAM/SOCK_DGRAM`, `seq/ack`, `port`, `3-way handshake`, `ephemeral port`, `listen/accept port`

- **8-1. 소켓 함수의 시스템 콜 구조**
  - TCP와 UDP의 소켓 함수는 "시스템 콜로 구현되어 있다"는데, 이게 어떤 구조인가. (최우녕)
- **8-2. host-to-host vs process-to-process**
  - "TCP는 host-to-host, UDP는 process-to-process"라는 표현은 어떤 의미인가. 보통은 반대로 들리는데 왜 그렇게 말하는가. (최우녕)
- **8-3. TCP/UDP 공통·차이·비유**
  - TCP와 UDP의 공통점과 차이를 한 번에 정리해 달라. (최우녕)
  - TCP와 UDP의 전송 방식 차이를 비유하면 무엇인가? (최현진)
- **8-4. TCP 순서 인자(seq)로 재조합**
  - TCP는 지연이 생겨도 순서 인자를 가지고 있어서 데이터를 다시 맞출 수 있는가? (최현진)
- **8-5. UDP 순서 없음 → 섞임**
  - TCP는 몇 번째 데이터인지 인자를 가지고 있고, UDP는 그런 인자가 없어서 지연되면 섞일 수 있는가? (최현진)
- **8-6. 기기의 TCP/UDP 구분**
  - 기기는 들어온 데이터가 TCP인지 UDP인지 어떻게 알아차리는가? (최현진)
- **8-7. TCP stream (50B × 2 → 100B × 1)**
  - TCP는 송신자가 50B씩 두 번 보내도 수신자가 100B를 한 번에 읽을 수 있는 이유가 무엇인가? (최현진)
  - 더 까다로운 TCP가 어떻게 자유롭게 100이라는 물을 퍼다 주는가? (최현진)
- **8-8. 3-way handshake 단계·정보·종료·포트**
  - TCP 3-way handshake의 구체적인 단계는 무엇인가? / 3-way handshake는 SYN, SYN/ACK, ACK로 이루어진다. 이 때 정확히 어떤 정보를 전달하는가? 정보의 형태와 무슨 내용이 들어있는가? (홍윤기, 최현진)
  - socket close할 때 3-way handshake를 하나? (홍윤기)
  - 만약 인터넷이 예상치 못하게 끊겨서 3-way handshake를 못하면 어떻게 되나? 클라이언트측, 서버측 둘 다 설명하시오. (홍윤기)
  - SOCK_STREAM은 소켓이 인터넷 연결의 끝점이 될 것이라는 뜻인데, 다른 타입은 어떤 게 있고, 역할은 무엇인가? (홍윤기)
  - client의 소켓의 포트가 임시포트라면, 이 임시포트는 방화벽에 대해서 열려있어야 서버로부터 recv할 수 있는 것 아닌가? 어떻게 이루어지는가? (홍윤기)
  - server측 listen socket과 accept socket은 포트 번호가 서로 다를 것 같다. 식별을 위해 accept socket도 서로 다를텐데, client는 이 port를 전부 알아야하는가? (홍윤기)

---

## L9. 커널 송신 파이프라인 (write → NIC)

> CSAPP 11.4 + Ch.10/Ch.9 접합 · 키워드: `write()`, `user→kernel copy`, `sk_buff`, `TCP segmentation`, `IP routing`, `qdisc`, `NIC driver`, `DMA`

- **9-1. write() → 이더넷 전체 흐름 (최상위) — [병합]**
  - 프로세스가 write(sockfd, buf, n)을 호출한 순간부터 이더넷 선로로 비트가 나가기까지, 호스트 내부에서 어떤 일이 일어나는가. / `user buffer → kernel socket buffer → TCP/IP 처리 → NIC driver → NIC → Ethernet` 흐름을 쉽게 말하면 무엇인가? / write(sockfd, data)를 했을 때 그 데이터가 커널 내부에서 어떻게 이동하는지 실제 흐름 기준으로 알고 싶다. (최우녕, 최현진, 김희준)
- **9-2. user → kernel buffer 복사 — [병합]**
  - user buffer에서 kernel socket buffer로 왜 복사하는가? (최현진)
  - user에서 kernel로 복사하는 1단계의 상세 로직은 무엇인가? / 사용자 영역(user space)에서 커널 영역(kernel space)로 데이터가 어떻게 넘어가는지 (최현진, 김희준)
- **9-3. buffer 개념**
  - buffer는 무엇인가? (최현진)
- **9-4. sk_buff 수명 (interrupt 전후)**
  - 수신 과정에서 sk_buff는 어떻게 변하는가? (최현진)
  - NIC가 전송 완료 interrupt를 보낸 후 sk_buff는 바로 삭제되는가? (최현진)
- **9-5. 소켓 버퍼 위치·관리**
  - 소켓 버퍼는 어디에 있고 어떻게 관리되는지 (김희준)
- **9-6. VFS / sockfs / socket layer / TCP layer 내부 흐름 — [병합]**
  - VFS, sockfs, socket layer, TCP layer 같은 것은 어떤 흐름인가? / 커널 내부에서 TCP/IP 스택이 어떻게 동작하는지 (최현진, 김희준)
- **9-7. RAM 가상주소 vs 물리주소**
  - network data는 하드웨어적으로 어디에 들어오고, user memory와 kernel memory는 어떻게 관련되는가? (최현진)
  - network data가 들어올 때 RAM의 가상주소에 들어가는가, 물리주소에 들어가는가? (최현진)
- **9-8. 프로토콜 SW — 유저/커널?**
  - "프로토콜 소프트웨어는 결국 프로세스"라는데, 그건 어디에 있는가. 유저 프로세스인가 커널인가. (최우녕)
- **9-9. 실제 숫자(IP/포트/프레임) 예시**
  - 이 과정을 실제 숫자(IP, 포트, 프레임 크기)로 대입해서 예시를 보여 달라. (최우녕)

---

## L10. I/O Bridge & DMA

> CSAPP Ch.6 × 11장 심화 · 키워드: `I/O bridge`, `DMA`, `MMIO`, `IRQ`, `PCIe`, `TLP`

- **10-1. I/O bridge 위치 & CPU·DRAM·NIC 경로**
  - I/O bridge를 통해 비트가 이동한다는데, CPU / DRAM / NIC 사이에서 정확히 어떤 경로로 데이터가 흐르는가. (최우녕)
  - CSAPP가 말하는 "I/O bridge"는 현대 하드웨어에서 정확히 어디 있는가? (최우녕)
- **10-2. DMA 정의**
  - DMA가 정확히 무엇인가? (최현진)
- **10-3. 세 종류의 주소 공간 (CPU·DRAM·주변장치)**
  - CPU·DRAM·주변장치가 얽히는 세 종류의 주소 공간은 어떻게 구분되는가? (최우녕)
- **10-4. 리눅스 커널의 DMA·MMIO·IRQ API**
  - 리눅스 커널은 DMA·MMIO·IRQ를 어떤 API·자료구조로 다루는가? (최우녕)
- **10-5. PCIe TLP**
  - PCIe 위에서 비트는 어떤 패킷으로 움직이는가? (최우녕)
- **10-6. write() → PCIe TLP 경로 추적**
  - write() 한 번이 실제로 PCIe TLP까지 내려가는 경로를 추적하면? (최우녕)

---

## L11. 4관점 통합 (CPU / 메모리 / 커널 / fd)

> 복습·통합 · 키워드: `mental model`, `송수신 대칭`, `성능 요소`

- **11-1. 4관점으로 분해 — [병합]**
  - 네트워크 통신 한 번을 CPU, 메모리, 커널, 파일 핸들(= fd)의 관점으로 각각 나눠서 설명해 달라. / network 통신 한 번을 CPU, memory, kernel, fd 관점으로 나누어 본다는 것은 무슨 의미인가? (최우녕, 최현진)
- **11-2. 송수신 대칭 묘사**
  - 송신과 수신을 대칭으로 묘사해 달라. (최우녕)
- **11-3. 성능 영향 요소**
  - 이 관점에서 성능에 영향을 주는 요소는 무엇인가. (최우녕)

---

## L12. HTTP / FTP / MIME / Telnet

> CSAPP 11.5.1 ~ 11.5.3 · 키워드: `HTTP/1.0 vs 1.1`, `FTP`, `MIME`, `Telnet`, `정적 vs 동적 콘텐츠`

- **12-1. HTTP vs FTP — [병합]**
  - FTP는 무엇이고 HTTP와 무엇이 다른가. / HTTP와 FTP는 무엇이 다르고, 왜 HTTP가 웹에 더 잘 맞는가? (최우녕, 최현진)
  - FTP는 무엇이고, HTTP/HTML의 강력함을 설명하려고 나온 비교 대상인가? (최현진)
- **12-2. MIME 타입**
  - MIME 타입이란 무엇이며 왜 필요한가. (최우녕)
- **12-3. Telnet으로 프로토콜 트랜잭션 테스트 — [병합]**
  - Telnet은 무엇이며, 왜 "모든 인터넷 프로토콜의 트랜잭션을 실행해볼 수 있다"고 말하는가. / Telnet으로 HTTP를 테스트할 수 있다는 말은 무슨 뜻인가? (최우녕, 최현진)
- **12-4. HTTP/1.0 vs HTTP/1.1**
  - HTTP/1.0과 HTTP/1.1의 차이는 무엇인가. (최우녕)
- **12-5. 정적 vs 동적 콘텐츠**
  - 정적 콘텐츠와 동적 콘텐츠는 실제로 무엇이 다른가? (최현진)

---

## L13. Tiny Web Server

> CSAPP 11.6 · 키워드: `Tiny`, `main/doit/parse_uri`, `serve_static/serve_dynamic`, `get_filetype`, `clienterror`

- **13-1. Tiny 정의 (최소 기능)**
  - 책에서 말하는 Tiny 서버는 무엇인가. 최소한 무엇을 할 수 있어야 Tiny인가. (최우녕)
- **13-2. Tiny 함수별 역할**
  - Tiny를 구성하는 함수(main, doit, read_requesthdrs, parse_uri, serve_static, get_filetype, serve_dynamic, clienterror)가 각각 어떤 역할을 하고 어떻게 호출되는가. (최우녕)
- **13-3. 정적 / 동적 처리 흐름**
  - Tiny의 정적 / 동적 처리 흐름을 코드와 함께 따라가 달라. (최우녕)

---

## L14. CGI & fork / execve / dup2

> CSAPP 11.5.4 + Ch.8 · 키워드: `CGI`, `argv/env/stdin/stdout`, `fork`, `execve`, `dup2`, `GET/POST`

- **14-1. CGI 정의**
  - CGI는 무엇인가. (최우녕)
- **14-2. 인자 전달 (argv / env / stdin / stdout) — [병합]**
  - 클라이언트가 보낸 인자 "15000&213"이 CGI 프로그램의 argv / 환경변수 / stdin / stdout 중 어디로 전달되는가. / CGI 프로그램은 인자를 어떻게 받는가? (최우녕, 최현진)
  - GET 요청의 인자는 CGI 프로그램에게 어떻게 전달되는가? (최현진)
  - POST 요청의 인자는 CGI 프로그램에게 어떻게 전달되는가? (최현진)
- **14-3. fork → execve → CGI → client 전 과정**
  - 서버가 fork한 자식에서 execve로 CGI 프로그램을 띄우고 그 결과가 클라이언트로 돌아가기까지의 전 과정을 자세히 설명해 달라. (최우녕)
  - CGI 프로그램이 printf한 데이터는 어떻게 client에게 돌아가는가? (최현진)
- **14-4. dup2의 역할 (CGI 핵심)**
  - dup2는 무엇이고 CGI에서 왜 중요한가? (최현진)
- **14-5. fork 일반 용도**
  - fork는 무엇이고 어디서 쓰이는가? (최현진)

---

## L15. Echo Server & EOF

> CSAPP 11.4 예제 · 키워드: `datagram vs packet/frame/segment`, `echo server`, `EOF`, `half-close`

- **15-1. 데이터그램 vs 패킷 vs 프레임 vs 세그먼트**
  - "데이터그램(datagram)"은 무엇인가. 패킷, 프레임, 세그먼트와 어떻게 다른가. (최우녕)
- **15-2. Echo Server 구현**
  - 책에서 말하는 에코 서버(echo server)는 무엇이고, 소켓으로 어떻게 구현되는가. (최우녕)
- **15-3. 네트워크 I/O에서 EOF**
  - 네트워크 통신은 파일 입출력과 비슷하고, 그래서 EOF가 중요하다는데, 그 과정을 자세히 설명해 달라. (최우녕)

---

## L16. Proxy (Proxy Lab)

> 본문 외 확장 · 키워드: `forward/reverse proxy`, `Proxy Lab`, `Tiny → Proxy`

- **16-1. Proxy와 Tiny의 연결**
  - 프록시는 CSAPP 11장의 본문에는 거의 등장하지 않는데, Tiny와 어떻게 연결되는가. (최우녕)
- **16-2. Proxy의 역할·배치**
  - 프록시의 역할과 배치 방식은 어떤가. (최우녕)
- **16-3. Tiny → Proxy 변경 사항**
  - Proxy Lab 관점에서 Tiny를 프록시로 바꾸려면 무엇을 어떻게 추가/변경해야 하는가. (최우녕)

---

## L17. 동시성 (Thread Pool · Async I/O · 락)

> CSAPP Ch.12 · 키워드: `thread pool`, `race condition`, `mutex/semaphore/condvar`, `deadlock`, `cache coherence`, `epoll/io_uring`

- **17-1. 스레드 풀 — 카페 비유로 개념 잡기**
  - 스레드 풀이 뭔지 카페 비유로 처음부터 설명해 달라. (최우녕)
- **17-2. Iterative Tiny → 스레드 풀 전환**
  - Tiny는 iterative 서버인데, 실제 서버는 스레드 풀을 쓴다. 각 스레드가 어떻게 네트워크 I/O를 "동시에" 처리하는가. (최우녕)
- **17-3. 4관점(CPU/메모리/커널/핸들/syscall) 재설명**
  - 이 과정을 CPU / 메모리 / 커널 / 핸들 / 시스템콜 관점에서 다시 설명해 달라. (최우녕)
- **17-4. Async I/O (epoll, io_uring) vs 스레드 풀**
  - async I/O(epoll, io_uring)는 스레드 풀과 무엇이 다른가. (최우녕)
- **17-5. 락 필요성 & race condition**
  - 왜 락이 필요한가. race condition이 실제로 어떻게 나는가. (최우녕)
- **17-6. Mutex / Condition Variable / Semaphore**
  - 뮤텍스와 조건 변수, 세마포어의 차이. (최우녕)
- **17-7. thread-safe / reentrant / thread-unsafe**
  - thread-safe / reentrant / thread-unsafe는 무엇이 다른가. (최우녕)
- **17-8. 데드락 발생·회피**
  - 데드락은 어떻게 나고, 어떻게 피하나. (최우녕)
- **17-9. 캐시 일관성 프로토콜**
  - 스레드가 여러 코어에서 동시에 돌 때 하드웨어는 어떻게 캐시 일관성을 유지하나? (최우녕)
- **17-10. 일관성 있어도 락이 필요한 이유**
  - 캐시 일관성 프로토콜이 있는데도 왜 락 없이는 프로그램이 터지는가? (최우녕)
- **17-11. 락 없는 코드가 터지는 시나리오**
  - 락 없는 코드가 어떤 방식으로 터지는가? 실제 예시·시나리오·원인·결과는? (최우녕)
- **17-12. 리눅스 커널 / libc 락·원자연산**
  - 리눅스 커널·libc는 어떤 락·원자연산을 제공하고 언제 써야 하는가? (최우녕)
- **17-13. 실전: SQL API 서버 락 배치**
  - 이번 주 SQL API 서버에서 실제로 어디에 락이 필요한가. (최우녕)

---

## L18. 프로세스 족보 & fd 수명

> CSAPP Ch.8 × 11장 · 키워드: `fork`, `PID 1/systemd`, `reparent`, `subreaper`, `O_CLOEXEC`, `refcount`, `TCP TIME_WAIT`

- **18-1. fork의 "맨 처음" 부모 (PID 1)**
  - 모든 프로세스가 fork로 만들어진다면, 그 포크의 "맨 처음" 부모는 누구인가? (최우녕)
- **18-2. 터미널 vs 셸 — 실제 부모**
  - 터미널에서 프로그램을 띄우면 터미널이 부모인가? 셸(bash)이 부모인가? (최우녕)
- **18-3. 크롬/엣지 — 부모 판정 기준**
  - "크롬이 떠 있는데 엣지를 열면 엣지는 크롬의 자식인가?" 부모가 되는 기준이 뭔가? (최우녕)
- **18-4. 고아 reparent & subreaper**
  - 자식이 먼저 죽으면 고아가 된 손자는 "할아버지"로 reparent되는가? (최우녕)
  - subreaper 플래그는 어디서 언제 켜는가? systemd --user, tini 같은 게 이걸 쓰나? (최우녕)
- **18-5. fork → fdtable 복제 & stdin/out/err 공유**
  - fork하면 fdtable까지 복제된다는데, 그럼 stdin/stdout/stderr도 공유하는 건가? (최우녕)
- **18-6. 터미널 상속 기준 (systemd 서비스가 안 뜨는 이유)**
  - 그런데 실제론 어떤 프로세스는 터미널을 물려받고(예: ls), 어떤 건 안 물려받는 것 같다(예: systemd로 띄운 서비스는 터미널에 출력 안 뜸). 기준과 메커니즘이 뭔가? (최우녕)
- **18-7. close(fd) & struct file refcount**
  - close(fd) 하면 부모/자식이 공유하던 struct file은 즉시 사라지나? refcount는 언제 0이 되나? (최우녕)
- **18-8. O_CLOEXEC 타이밍**
  - O_CLOEXEC는 언제, 누구에게 작용하나? 자식이 fd에 접근하는 순간 닫히는가? (최우녕)
- **18-9. 파일 fd vs 소켓 fd — 규칙이 정말 같나**
  - 파일 fd와 소켓 fd가 "완전히 똑같은 규칙"으로 작동한다는 건 어디까지 사실인가? (최우녕)
- **18-10. socket close: TCP 60초 FIN/ACK vs UDP 즉시**
  - 소켓 하나의 close()가 TCP는 60초 FIN/ACK, UDP는 즉시 끝 — 같은 함수가 어떻게 다르게 동작하나? (최우녕)
- **18-11. fork 복제 fd — 동시 read/write**
  - fork로 복제된 fd를 부모/자식이 동시에 read/write하면 무슨 일이 일어나나? (최우녕)
- **18-12. fd로 연결 가능한 객체 전체**
  - fd로 연결할 수 있는 객체는 뭐가 있나? 전부 나열하고 언제 쓰는지까지 알고 싶다. (최우녕)

---

## L19. 가상 메모리 & 프로세스 메모리 레이아웃

> CSAPP Ch.9 · 키워드: `libc`, `heap (brk/sbrk)`, `mmap`, `demand paging`, `VSZ/RSS`, `overcommit`, `OOM killer`

- **19-1. libc의 가상 메모리 위치**
  - libc는 가상 메모리의 어디에 있는가? 힙 영역인가? 커널 메모리인가? (최우녕)
- **19-2. 스택 ↓ / 힙 ↑ 방향 맞나**
  - 가상 메모리 레이아웃 다이어그램의 방향 — 스택이 위에서 아래로, 힙이 아래에서 위로 — 맞는가? (최우녕)
- **19-3. Heap (brk/sbrk) vs mmap**
  - 힙(brk/sbrk)과 mmap은 뭐가 다른가? (최우녕)
- **19-4. Demand Paging (mmap 즉시 물리 할당?)**
  - mmap을 호출하면 즉시 물리 메모리가 할당되는가? 힙은? (demand paging의 실체) (최우녕)
- **19-5. VSZ / RSS / overcommit / OOM killer**
  - VSZ와 RSS의 차이, overcommit, OOM killer와 이들의 관계는? (최우녕)

---

## L20. 코드 디테일 (adder.c 예제)

> CSAPP 11.5.4 예제 · 키워드: `sprintf`, `Content-Length`, `배열 vs 포인터`

- **20-1. sprintf vs printf & Content-Length**
  - adder.c에서 왜 그냥 printf()로 바로 출력하지 않고, 굳이 sprintf()로 content 문자열을 먼저 만들어야 하나요? 그리고 content에 =로 문자열을 넣으면 안 되나요? (CSAPP 3판 920페이지) (이우진)

---

## 부록 A. 병합 내역 (중복 제거 11건)

| #   | 노드    | 병합된 작성자                    | 병합 근거                                      |
|-----|---------|---------------------------------|-----------------------------------------------|
| M1  | L5-5    | 정범진, 최현진                   | FD→file table→v-node 연결 관계 동일           |
| M2  | L6-1    | 최우녕, 최현진                   | "소켓은 물리적으로 어디에 있는가" 동일 축       |
| M3  | L7-1    | 최우녕, 최현진                   | 11.4 함수 호출 순서 메타 질문 동일              |
| M4  | L8-8    | 홍윤기, 최현진                   | 3-way handshake 단계·정보 동일                |
| M5  | L9-1    | 최우녕, 최현진, 김희준           | write() → NIC 전체 흐름 동일                  |
| M6  | L9-2    | 최현진, 김희준                   | user → kernel 복사 로직 동일                  |
| M7  | L9-6    | 최현진, 김희준                   | VFS/sockfs/socket/TCP layer 흐름 동일         |
| M8  | L11-1   | 최우녕, 최현진                   | 4관점(CPU/메모리/커널/fd) 분해 동일            |
| M9  | L12-1   | 최우녕, 최현진                   | HTTP vs FTP 차이 동일                         |
| M10 | L12-3   | 최우녕, 최현진                   | Telnet 프로토콜 테스트 동일                   |
| M11 | L14-2   | 최우녕, 최현진                   | CGI 인자 전달 경로 동일                        |

## 부록 B. 작성자별 원 질문 수 (사전 분포)

| 작성자   | 원 질문 수 |
|---------|-----------|
| 홍윤기   | 6         |
| 정범진   | 4         |
| 최우녕   | 84        |
| 최현진   | 68        |
| 이우진   | 2         |
| 김희준   | 4 (1 main + 3 sub) |
| 이호준   | 2         |
| **합계** | **170**   |

> 병합 후 트리 노드 수: 약 159 (원 질문 텍스트는 전부 보존, 중복 11건을 병합만 수행).
