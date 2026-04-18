# 00. Top-down DFS Walkthrough — 네트워크 통신 한 번을 바닥까지

CSAPP 11장 + 본 주 SQL API 서버 과제를 위한 통합 읽기 순서.
q01 ~ q18 문서와 지난 학습 내용을 **탑다운 하나의 흐름**에 DFS(depth-first) 로 묶는다.
위에서 아래로 그대로 읽으면 앞 섹션이 다음 섹션의 전제가 된다.

## 이 문서의 원칙

```
원칙 1.  탑다운            큰 그림 -> 계층 -> 함수 -> 비트
원칙 2.  DFS (깊이 우선)    한 주제는 바닥까지 파고든 뒤 다음 형제로
원칙 3.  한 줄 정의 먼저    용어가 처음 나오면 괄호로 한 줄 정의
원칙 4.  그 다음 다이어그램  ASCII 박스 / 표 / 코드 블록 순
원칙 5.  심화는 q-문서로     각 섹션 끝에 "-> 상세" 로 q0X 연결
```

각 섹션 끝의 `-> 상세: q0X.md` 는 **그 섹션을 바닥까지 더 파고 싶을 때** 읽는다.
본 문서만 읽어도 뼈대가 잡히고, 깊게 보고 싶으면 링크를 타서 내려갔다가 돌아오면 된다.

## 목차 — DFS 순서

```
[A] 큰 그림                        §1
[B] 네트워크 하드웨어                §2   -> q01
[C] 주소 체계                      §3   -> q02
[D] DNS                          §4   -> q03
[E] 유저/커널 경계                  §5
[F] 파일 추상화 (바닥까지)           §6   -> q04
      F-1. VFS 4 객체
      F-2. ext4 디스크 레이아웃
      F-3. path_lookupat / dcache
      F-4. open / read 시스템콜 경로
      F-5. 페이지 캐시 / filemap_read
      F-6. BIO / blk-mq / writeback
      F-7. Pseudo-FS
      F-8. sockfs — 소켓이 파일이 되는 방법
[G] 소켓 3층 구조                   §7   -> q05
[H] 소켓 API                       §8   -> q06
[I] TCP/UDP 시스템콜                §9   -> q07
[J] 송신 파이프라인                  §10  -> q08
      J-1. [1]~[9] 전체
      J-2. sk_buff / slab / 큐
      J-3. TCP 헤더 비트 단위
      J-4. IP 헤더 비트 단위
      J-5. ARP / next-hop / MAC 교체
[K] I/O Bridge (바닥까지)           §11  -> q10
      K-1. IMC + PCH 진화
      K-2. 세 주소 공간
      K-3. PCIe TLP
      K-4. DMA API
      K-5. MSI-X / NAPI
[L] 수신 파이프라인                  §12
[M] 네 개의 렌즈                    §13  -> q09
[N] 응용 계층 HTTP                   §14  -> q11
[O] Tiny Web Server                §15  -> q12
[P] CGI / fork / dup2              §16  -> q13
[Q] Echo Server / EOF              §17  -> q14
[R] Proxy                          §18  -> q15
[S] 스레드 풀 / epoll               §19  -> q16
[T] 락 기본                        §20  -> q17
[U] 스레드 동시성 실패 13선          §21  -> q18
[V] 마무리 — SQL API 서버로 연결     §22
```

---

## §0. 왜 이런 순서인가

네트워크 공부가 어려운 이유는 "전선부터 HTTP 까지" 층이 7~8개로 많고, 각 층에 전용 용어가 있고, **그 용어들이 서로를 전제로** 하기 때문이다. "소켓" 을 이해하려면 "fd" 를, fd 를 이해하려면 "커널 메모리" 를, 커널 메모리를 이해하려면 "syscall" 을 알아야 한다.

본 문서는 그 의존 관계를 한 방향 DFS 로 풀었다. 순서를 따라가면 "이 용어는 아직 모르는데..." 가 생기지 않도록 설계했다.

```
 탑다운                            DFS 깊이
 ────                              ──────
 큰 그림                           얕게
   |                               |
 하드웨어                          |
   |                               |
 주소/DNS                          |
   |                               |
 유저/커널                         |
   |                               |
 파일시스템 ─┐                     |
            |                     |   <- 파일시스템 안쪽을 바닥까지
            | VFS/ext4/page cache |      다 파고 나서 위로 올라옴
            | /blk-mq/writeback   |
            └──────────────────── v
 소켓 3층
   |
 (...계속 깊어지다가 I/O 브릿지에서 또 바닥까지...)
```

---

## §1. 전체 그림 — 한 줄 요청이 일으키는 4계층

`curl http://www.google.com/` 한 줄이 실행되면 내부에서 벌어지는 일의 **3줄 요약**:

```
1. 내 PC 가 "www.google.com" 을 DNS 로 IP (예: 142.251.150.104) 로 변환
2. 내 PC 가 그 IP 의 80 번 포트로 TCP 연결을 맺고 "GET / HTTP/1.1\r\n\r\n" 전송
3. 구글 서버가 HTML 을 TCP 로 돌려줌. 연결 종료
```

뒤에서는 **네 개의 계층**이 동시에 돌아간다.

```
┌─────────────────────────────────────────────────────────┐
│ 응용 계층    HTTP, DNS, SSH, FTP ...                     │  사람이 쓰는 프로토콜
├─────────────────────────────────────────────────────────┤
│ 전송 계층    TCP, UDP                                    │  프로세스 <-> 프로세스
├─────────────────────────────────────────────────────────┤
│ 인터넷 계층  IP, ICMP                                    │  호스트 <-> 호스트
├─────────────────────────────────────────────────────────┤
│ 링크 계층    Ethernet, Wi-Fi, ARP                        │  바로 옆 기계끼리
└─────────────────────────────────────────────────────────┘
```

**핵심 원칙**: 위 계층은 아래 계층을 "모른다". HTTP 는 TCP 덕분에 "바이트 스트림이 순서대로 온다" 고 믿는다. TCP 는 IP 덕분에 "호스트에 도달한다" 고 믿는다. 이 "덕분에" 의 연쇄가 **캡슐화(encapsulation)** 다.

```
유저 데이터    "GET /home.html ..."                       (95B 라 치자)
   | + TCP 헤더 20B
TCP 세그먼트   [ TCP | 데이터 ]                             115B
   | + IP 헤더 20B
IP 패킷        [ IP | TCP | 데이터 ]                        135B
   | + Ethernet 14B + FCS 4B
Ethernet 프레임 [ Eth | IP | TCP | 데이터 | CRC ]            153B
```

이제 바닥 계층부터 하나씩 DFS 로 내려간다.

---

## §2. 네트워크 하드웨어 계층 — 선로, 이더넷, 공유기, 라우터, LAN/WAN

**이더넷(Ethernet)** = "바로 옆에 있는 기계끼리 프레임을 주고받는 방법". 선로는 구리(UTP)/광섬유/무선(Wi-Fi) 어느 쪽이든 된다. 프레임 맨 앞에 **MAC 주소** (48 비트, 예: `AA:BB:CC:DD:EE:FF`) 가 src/dst 로 들어간다.

### 2.1 허브 / 스위치 / 라우터 / 공유기

| 장비 | 계층 | 판단 기준 | 특징 |
| --- | --- | --- | --- |
| 허브 | 물리 | 없음 (모든 포트로 뿌림) | 옛날 물건 |
| 스위치(브릿지) | 링크 | MAC 테이블 | 정확한 포트로만 |
| 라우터 | 인터넷 | 라우팅 테이블 (IP) | 다른 네트워크로 |
| 공유기 | 혼합 | 스위치 + 라우터 + NAT + AP | 가정용 통합 |

### 2.2 LAN 과 WAN

```
LAN  (Local Area Network)   한 사무실/집    수~수백 대    브로드캐스트 가능   MAC 통신
WAN  (Wide Area Network)    LAN 들의 연결   인터넷 전체   라우터가 분리      IP 라우팅
```

### 2.3 브로드캐스트 도메인의 경계

> **브로드캐스트는 LAN 까지만**. 라우터를 넘으면 멈춘다.

이 한 줄이 왜 중요하냐면 — ARP 처럼 브로드캐스트로 도는 프로토콜은 같은 LAN 안에서만 돈다 (§10.5). 라우터가 경계를 끊어 주지 않으면 인터넷 전체에서 ARP 폭풍이 일어난다.

> **-> 상세**: [q01-network-hardware.md](./q01-network-hardware.md)
> Ethernet 프레임 67B 바이트맵, MAC 의 I/G/U/L 비트, IP 헤더 RFC 비트맵, TCP 9-bit 플래그, CRC-32 / 체크섬 연산.

---

## §3. 주소 체계 — MAC / IP / Port, 그리고 byte order

세 종류의 주소가 있다.

```
┌───────┬─────────┬───────────────────────┬─────────────────────────────┐
│ 주소  │ 비트    │ 예시                  │ 소속                          │
├───────┼─────────┼───────────────────────┼─────────────────────────────┤
│ MAC   │ 48      │ AA:BB:CC:DD:EE:FF     │ NIC 하나당 하나. 평생 고정    │
│ IP    │ 32      │ 192.168.1.10          │ 호스트당. DHCP 로 분배        │
│ 포트  │ 16      │ 80, 443, 51213        │ 프로세스/소켓당              │
└───────┴─────────┴───────────────────────┴─────────────────────────────┘
```

**4-tuple** = `(src IP, src port, dst IP, dst port)` 가 TCP/UDP 연결 하나를 유일하게 식별한다.

### 3.1 IPv4 와 IPv6

```
IPv4    32 비트   192.168.1.10           4 바이트, 약 43억 개
IPv6   128 비트   2001:db8::1            8 개의 16진수 그룹, 사실상 무한
```

당분간 둘은 병행된다.

### 3.2 Byte order (엔디안) 함정

```
 x86/ARM/Apple Silicon      little-endian   (낮은 자리수 앞)
 네트워크 바이트 순서        big-endian      (높은 자리수 앞)
```

포트 `80` (= `0x0050`) 을 그대로 소켓 구조체에 넣으면 전선에서 `0x5000` (= 20480) 으로 읽힌다. **반드시 `htons(80)`** 써야 한다.

```c
serv_addr.sin_port = htons(80);            // host -> network short
serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
```

`htons / ntohs / htonl / ntohl` 네 개를 외워두면 모든 엔디안 실수가 줄어든다.

> **-> 상세**: [q02-ip-address-byte-order.md](./q02-ip-address-byte-order.md)

---

## §4. DNS — 도메인을 IP 로 바꾸는 분산 조회

사람은 `www.google.com` 을 외우고, 기계는 IP 로 통신한다. 그 사이 다리가 DNS.

### 4.1 도메인 구조

```
           www.google.com.
           ^^^            서브도메인
               ^^^^^^     2차 도메인
                      ^^^ TLD
                          . (root, 보통 생략)
```

### 4.2 재귀 조회 흐름

```
┌─ 내 PC ─────┐
│             │
│  OS 리졸버  │ ──(①)──> ISP/Cloudflare 재귀 리졸버 (예: 1.1.1.1)
│             │                   │
└─────────────┘                   │ ②  "com 누구?"
                                   ├────────> Root NS
                                   │ <────── "a.gtld-servers.net"
                                   │
                                   │ ③  "google.com 누구?"
                                   ├────────> com NS
                                   │ <────── "ns1.google.com"
                                   │
                                   │ ④  "www.google.com A?"
                                   ├────────> google.com NS
                                   │ <────── "142.251.150.104"
                                   │
         <──────────────────────── ⑤  IP 돌아옴, TTL 동안 캐싱
```

### 4.3 주요 레코드

| 타입 | 의미 |
| --- | --- |
| A | IPv4 주소 |
| AAAA | IPv6 주소 |
| CNAME | 별칭 (다른 도메인으로 위임) |
| MX | 메일 서버 |
| NS | 권한 네임서버 |
| TXT | SPF, 도메인 인증 문자열 |

### 4.4 Cloudflare 의 세 역할

```
Registrar          도메인 등록 대행
권한 NS             A 레코드 실제 보관
프록시 CDN          proxy-on 이면 A = Cloudflare 엣지 IP (공격 완화, 캐시)
```

> **-> 상세**: [q03-dns-domain-cloudflare.md](./q03-dns-domain-cloudflare.md)

---

## §5. 유저와 커널 — ring, syscall, trap, interrupt

앞으로 나올 모든 "소켓/파일" 얘기는 **유저가 커널에 일을 시키는** 구조다. 그 경계를 먼저 이해해야 한다.

### 5.1 링 레벨 (CPL)

```
CPU 내부        CS 레지스터 하위 2비트 = CPL (Current Privilege Level)
CPL = 0         커널 모드 (모든 명령, 모든 메모리)
CPL = 3         유저 모드 (제한된 명령, 유저 영역 메모리만)
```

**권한은 "코드" 가 아니라 "CPU 상태"** 가 쥐고 있다.

### 5.2 주소 공간 배치 (x86_64 Linux)

```
0xFFFF_FFFF_FFFF_FFFF  ┬────────────────────┐
                       │                     │
                       │     커널 영역       │  모든 프로세스가 공유 매핑
                       │                     │  CPL=3 일 때 접근 금지
                       │                     │
0xFFFF_8000_0000_0000  ┼────────────────────┤
                       │                     │  "비매핑 간격"
0x0000_7FFF_FFFF_FFFF  ┼────────────────────┤
                       │                     │
                       │     유저 영역       │  프로세스마다 다름
                       │                     │
0x0000_0000_0000_0000  ┴────────────────────┘
```

syscall 이 하는 일은 **주소 공간을 바꾸는 게 아니라 "CPL 을 3 -> 0 으로 올리는 것"**.

### 5.3 세 가지 경계 넘기

| 종류 | 누가 발생 | 동기/비동기 | 예 |
| --- | --- | --- | --- |
| syscall | 유저가 의도 (`syscall` 명령) | 동기 | `read`, `write`, `socket` |
| trap (exception) | 유저 코드가 실수 | 동기 | 0 나누기, page fault |
| interrupt | 외부 장치 | 비동기 | NIC 패킷, 타이머, 키보드 |

### 5.4 syscall 실제 동작

```
유저 코드:  write(4, buf, 95)
   | glibc wrapper
   v
   mov rax, 1           ; syscall 번호 (sys_write)
   mov rdi, 4           ; fd
   mov rsi, buf         ; 유저 포인터
   mov rdx, 95          ; 길이
   syscall              ; CPU: CPL=3 -> 0, rip = entry_SYSCALL_64
   |
   v
커널:  entry_SYSCALL_64 -> sys_write -> ksys_write -> vfs_write -> ...
   |
   v
   sysret               ; CPU: CPL=0 -> 3, rip = 유저 복귀 주소
```

**glibc** 는 이 wrapper 모음(`libc.so.6`) 이다. "편하게 C 함수로 쓰자" 를 위한 얇은 래퍼일 뿐이다.

### 5.5 함수 주소 해석시에도 CPL 이 체크되나?

- MMU 는 **모든 메모리 접근 때마다** CPL 과 페이지의 U/S 비트를 비교한다.
- CPL=3 인 유저가 커널 영역(U/S=S) 주소로 점프하려고 하면 -> Page Fault (Segfault).
- SMEP (Supervisor Mode Execution Prevention) / SMAP 은 하드웨어가 **커널 모드가 유저 영역을 실수로 실행/접근하는 것** 도 막는 추가 방어.

---

## §6. 파일 추상화 — 바닥까지 파고들기 [DFS 심화 구역]

리눅스/유닉스의 근본 사상: **모든 것은 파일이다**. 디스크 파일, 소켓, 파이프, 장치, 심지어 메모리 공유도 전부 `read/write/close` 로 다룬다. 그걸 떠받치는 게 VFS.

이 섹션은 **DFS 깊이 우선**: 위로 다시 올라가기 전에 파일 시스템 안쪽을 바닥까지 판다.

### 6.1 VFS 의 네 객체

```
┌─────────────┐   한 FS 인스턴스(마운트)당 1개. 전체 구조 메타데이터
│ super_block │   ext4 superblock, tmpfs, sockfs 마다 각각
└─────────────┘

┌─────────────┐   파일 본체. 크기, 주인, 권한, 데이터 블록 위치
│   inode     │   이름은 없다. 한 파일 = 한 inode
└─────────────┘

┌─────────────┐   이름-inode 매핑. 하드 링크 N개 = dentry N개
│   dentry    │   최근 조회된 건 dcache 에 캐시
└─────────────┘

┌─────────────┐   "열린 상태". 같은 파일을 두 번 열면 file 이 2 개
│    file     │   read offset, 플래그, f_op (read/write 핸들러)
└─────────────┘
```

### 6.2 fd 와 fdtable

```
task_struct (PID=1234)
  └── files -> files_struct
                 └── fdtable.fd[]  (배열)
                      ├── [0] -> file (stdin)
                      ├── [1] -> file (stdout)
                      ├── [2] -> file (stderr)
                      ├── [3] -> file (./a.txt)
                      └── [4] -> file (socket)
```

- fd 는 이 배열의 **인덱스**일 뿐.
- 프로세스마다 고유. 같은 fd 번호도 다른 프로세스에선 다른 파일.
- `fork()` 는 이 fdtable 을 **통째로** 복사 (§16).
- 기본 64 슬롯, 필요시 자동 확장, 상한은 `RLIMIT_NOFILE`.

### 6.3 ext4 디스크 레이아웃

ext4 는 디스크를 **블록 그룹** 으로 나눈다.

```
디스크 선두
┌────────────────────────────────────────────────────────────────┐
│ [Block Group 0] [Block Group 1] [Block Group 2] ... [Group N]  │
└────────────────────────────────────────────────────────────────┘

한 블록 그룹 내부:
┌────────────────────────────────────────────────────────────────┐
│ Super │  GDT  │ Block  │ Inode  │ Inode │      Data           │
│ block │       │ Bitmap │ Bitmap │ Table │      Blocks         │
└────────────────────────────────────────────────────────────────┘
   ^       ^        ^        ^         ^          ^
   1블록   N블록    1블록    1블록     수십블록    나머지 전부
```

inode 한 개 (`struct ext4_inode`) 는 **128~256 B**. 데이터 블록 위치는 **extent tree** 로 저장한다 (예전 inode 12 개 직접 + 3 단계 간접 포인터 방식에서 진화).

```c
// fs/ext4/ext4.h  (단순화)
struct ext4_extent {
    __le32  ee_block;   // 논리 블록 번호 (파일 안에서)
    __le16  ee_len;     // 연속 길이
    __le16  ee_start_hi;
    __le32  ee_start_lo;// 물리 블록 번호 (디스크 위치)
};
```

**Extent** = "이 논리 블록 K 개는 디스크의 물리 블록 M 번부터 연속되어 있다". 큰 파일을 효율적으로 저장한다.

### 6.4 path_lookupat — "/home/u/a.txt" 해석

```c
// fs/namei.c  (초단순화)
int path_lookupat(...) {
    // 1. 시작점 (루트 또는 cwd) 의 dentry 잡기
    // 2. "/" 단위로 잘라서 한 조각씩
    for (each component)
        link_path_walk(...)  // dcache 에서 찾음
    // 3. 없으면 디스크에서 inode 읽어옴 (ext4_lookup)
    // 4. 심볼릭 링크면 재귀
    // 5. 최종 dentry 반환
}
```

**dcache** 덕에 반복 조회는 거의 메모리 연산이다.

### 6.5 open / read 시스템콜 경로

```
 open("/home/u/a.txt", O_RDONLY)
   | syscall
   v
 do_sys_open -> getname (유저 경로 커널로 복사)
             -> get_unused_fd_flags (fd 번호 예약)
             -> path_openat        (path_lookupat + alloc_empty_file)
             -> vfs_open           (f_op = ext4_file_operations 고정)
             -> fd_install         (fdtable 에 꽂기)
   |
   v
 유저: fd 반환
```

```
 read(fd, buf, 4096)
   | syscall
   v
 ksys_read -> vfs_read -> f_op->read_iter
                           | ext4 면 ext4_file_read_iter
                           v
                           generic_file_read_iter
                           v
                           filemap_read           (§6.6 페이지 캐시)
```

### 6.6 페이지 캐시 (filemap_read)

```
 [유저 buf 4096B]
       ^
       | copy_to_user
       |
 ┌──────────────────────────────────────┐
 │ 페이지 캐시 (DRAM 안, inode 별로 관리) │
 │ ┌───┬───┬───┬───┬───┬───┐           │
 │ │ 0 │ 1 │ 2 │ 3 │ 4 │ 5 │ ...       │  (4KB 단위)
 │ └───┴───┴───┴───┴───┴───┘           │
 └──────────────────────────────────────┘
       ^
       | 페이지가 없으면 "page fault" -> 디스크에서 올림
       |
 ┌──────────────────────────────────────┐
 │ 블록 레이어 (BIO)                    │
 └──────────────────────────────────────┘
       |
       v
 디스크
```

한 번 읽은 페이지는 캐시에 남아 다음 read 를 빠르게 한다.

### 6.7 BIO / blk-mq / writeback

```
 struct bio   "이 페이지를 이 디스크 섹터와 주고받아줘" 요청 단위
 struct request  드라이버가 실제로 디스크에 보내는 명령

 [파일 쓰기]
   filemap_write -> 페이지에 마킹 (dirty)
   ...
   writeback 데몬 (bdflush) 이 주기적으로 dirty 페이지 수집
   -> bio 만들어 blk-mq 제출
   -> 다중 큐 (CPU 별) 로 드라이버에게 전달
   -> 디스크 I/O 완료시 인터럽트
```

**fsync(fd)** 는 이 dirty 페이지가 **디스크에 실제로 기록될 때까지** 블록한다.

### 6.8 Pseudo-filesystem 들

실제 디스크에 없는 가상 FS 들. 전부 **같은 VFS 인터페이스**로 다룬다.

| FS | 마운트 지점 | 용도 |
| --- | --- | --- |
| procfs | `/proc` | 프로세스/커널 상태 조회 |
| sysfs | `/sys` | 장치 트리/드라이버 속성 |
| tmpfs | `/tmp`, `/dev/shm` | RAM 에 있는 파일 (스왑 가능) |
| devpts | `/dev/pts` | PTY 터미널 |
| pipefs | 내부 | 파이프 버퍼의 anonymous inode |
| sockfs | 내부 | 소켓의 anonymous inode |

### 6.9 sockfs — 소켓이 파일이 되는 방법

소켓은 디스크에 없으니 **anonymous inode** 를 sockfs 가 발급한다. 이래야 VFS 체인에 들어갈 수 있다.

```
sockfd=4 ── fdtable[4] ── struct file ── f_op = socket_file_ops
                            │
                            └── private_data -> struct socket
                                                  ├── ops = inet_stream_ops
                                                  └── sk  -> struct sock (tcp_sock)
```

그래서 `read(4, ...)` 가 `socket_file_ops.read_iter` -> `tcp_recvmsg` 까지 흐를 수 있다 (§7, §9).

### 6.10 f_op 디스패치 — 같은 read() 가 fd 타입별로 다르게

```
 read(fd, ...) ──> file->f_op->read_iter
                     ├── ext4_file_read_iter  (일반 파일)
                     ├── sock_read_iter       (소켓)   -> tcp_recvmsg
                     ├── pipe_read            (파이프)
                     ├── tty_read             (터미널)
                     └── proc_reg_read_iter   (procfs)
```

하나의 syscall 이 fd 타입에 따라 완전히 다른 구현으로 빠진다. 이게 "모든 것이 파일" 의 실제 구현 장치다.

> **-> 상세**: [q04-filesystem.md](./q04-filesystem.md)
> syscall -> VFS -> ext4 -> 페이지캐시 -> blk-mq -> 드라이버 전체 경로를 커널 소스 코드 레벨로.

---

## §7. 소켓의 3층 구조 — file -> socket -> sock

소켓 하나를 **세 개의 렌즈**로 본다.

```
─────────────────────────────────────────────────────────────
 유저가 보는 것     sockfd = 4  (단순 정수)
─────────────────────────────────────────────────────────────

 VFS 층            struct file
                    · f_op = socket_file_ops  (read/write/poll/close ...)
                    · private_data -> socket

─────────────────────────────────────────────────────────────

 BSD socket 층      struct socket
                    · type  = SOCK_STREAM
                    · state = SS_CONNECTED
                    · ops   = inet_stream_ops  (bind/listen/accept/sendmsg ...)
                    · sk    -> sock

─────────────────────────────────────────────────────────────

 프로토콜 층        struct sock  (상위 타입: tcp_sock)
                    · sk_family = AF_INET
                    · sk_write_queue    (송신 대기 FIFO)
                    · sk_receive_queue  (수신 대기 FIFO)
                    · sk_rcvbuf / sk_sndbuf
                    · tcp_sock 의 seq / ack / window / cwnd / 상태 머신
─────────────────────────────────────────────────────────────
```

이 세 층 덕분에:

- VFS 층 -> "모든 것이 파일" 유지
- BSD 층 -> bind/listen/accept 같은 **공통 API**
- 프로토콜 층 -> TCP/UDP/UNIX 도메인 등 **실제 동작의 차이**

`sendmsg` 호출 시 흐름:

```
write() / send()
   v  file->f_op->write_iter
sock_write_iter
   v  sock->ops->sendmsg
inet_sendmsg -> tcp_sendmsg     (TCP 일 때)
   v  sk_write_queue 에 skb enqueue + ip_output 호출
IP 계층으로
```

> **-> 상세**: [q05-socket-principle.md](./q05-socket-principle.md)

---

## §8. 소켓 API — socket/bind/listen/accept/connect + getaddrinfo

### 8.1 서버/클라이언트 대칭

```
 서버                          클라이언트
 ────                          ────────
 socket()                      socket()
 bind()                        (bind 보통 생략 -> 커널이 랜덤 포트)
 listen()
 accept()   <- 여기서 block      connect()  <- 3-way handshake
 read()/write()                write()/read()
 close()                       close()
```

### 8.2 각 함수 한 줄씩

| 함수 | 역할 |
| --- | --- |
| `socket(domain, type, proto)` | struct socket/sock 할당 + fd 반환 |
| `bind(fd, sa, len)` | "이 IP:port 로 오는 패킷 받을래" 등록 |
| `listen(fd, backlog)` | 수동(passive) 로 전환, SYN/ACCEPT 큐 준비 |
| `accept(fd, ...)` | 완성된 연결 하나 꺼내 새 fd(connfd) 반환. listenfd 그대로 |
| `connect(fd, sa, len)` | 서버에 SYN 보내고 연결 완성까지 대기 |

### 8.3 listenfd vs connfd

```
 서버 프로세스
 ┌──────────────────────────┐
 │ listenfd (3)  <- 듣기용  │
 │ connfd_1 (4)  <- 클라 A │
 │ connfd_2 (5)  <- 클라 B │
 │ connfd_3 (6)  <- 클라 C │
 └──────────────────────────┘
```

listenfd = "들을 귀", connfd = "실제 대화".

### 8.4 getaddrinfo

도메인 + 서비스명을 socket API 용 struct 배열로 바꿔 주는 "DNS + 포트 맵" 래퍼.

```c
struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_family = AF_UNSPEC };
struct addrinfo *res;
getaddrinfo("www.google.com", "80", &hints, &res);
for (p = res; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
}
freeaddrinfo(res);
```

addrinfo 주요 필드: `ai_family` (AF_INET/AF_INET6), `ai_socktype`, `ai_addr`, `ai_addrlen`, `ai_next`.

> **-> 상세**: [q06-ch11-4-sockets-interface.md](./q06-ch11-4-sockets-interface.md)

---

## §9. TCP/UDP 시스템콜 — process-to-process 추상화

### 9.1 host-to-host vs process-to-process

```
IP 계층       호스트 -> 호스트        (IP 주소로 구분)
TCP/UDP      프로세스 -> 프로세스     (IP+Port 로 구분)
```

### 9.2 TCP 와 UDP 차이

| 관점 | TCP | UDP |
| --- | --- | --- |
| 연결 | 3-way handshake 필요 | 없음 |
| 순서 | 보장 | 안 함 |
| 손실 | 재전송 | 그냥 날림 |
| 혼잡 제어 | cwnd/slow start | 없음 |
| API | stream (`SOCK_STREAM`) | datagram (`SOCK_DGRAM`) |
| 헤더 | 20 B 기본 | 8 B 고정 |
| 용도 | HTTP, SSH, DB | DNS 쿼리, 게임, 동영상 스트리밍 |

### 9.3 TCP 3-way handshake

```
 클라이언트              서버
 ──────                 ────
   |   SYN seq=x          |
   |─────────────────────>|   listen 큐(incomplete) 에 추가
   |                      |
   |   SYN+ACK seq=y,ack=x+1
   |<─────────────────────|   listen 큐(accept) 준비
   |                      |
   |   ACK ack=y+1        |
   |─────────────────────>|   accept() 가 이거 꺼내감
   |                      |
   |   ───── 데이터 ─────  |
```

> **-> 상세**: [q07-tcp-udp-socket-syscall.md](./q07-tcp-udp-socket-syscall.md)

---

## §10. 송신 파이프라인 — write() 한 번이 NIC 까지

### 10.1 [1]~[9] 전체 경로

95 B 의 HTTP 요청을 보낸다고 하자.

```
[1] 유저            buf = "GET /home.html HTTP/1.1\r\n..."  (95B)
                    write(4, buf, 95);
                      | syscall  (§5: CPL 3 -> 0)
                      v
[2] VFS             fdtable[4] -> file -> sock_write_iter
                      v
[3] BSD socket      socket->ops->sendmsg == tcp_sendmsg
                      v
[4] TCP 계층        sk_buff 하나 할당 (slab)
                    copy_from_user: 유저 95B -> skb 데이터영역 (커널 VA)
                    TCP 헤더 20B 붙임 (seq, ack, flag, window, checksum)
                    sk_write_queue tail 에 enqueue
                    tcp_write_xmit 호출
                      v
[5] IP 계층         ip_queue_xmit
                    라우팅 테이블 조회 -> next-hop, 출력 인터페이스
                    IP 헤더 20B 붙임 (TTL=64, proto=TCP, checksum, src/dst IP)
                      v
[6] Neighbor (ARP)  next-hop IP 로 dst MAC 조회 (dcache 유사한 neighbor cache)
                    없으면 ARP request 브로드캐스트
                    Ethernet 헤더 14B 붙임 (dst MAC, src MAC, EtherType=0x0800)
                      v
[7] qdisc/TC        큐잉 디스크립터에 삽입 (pfifo_fast 가 기본)
                      v
[8] 드라이버        igb_xmit_frame (예: Intel 드라이버)
                    skb 의 linear/frag 영역을 DMA descriptor 로 변환
                    descriptor ring 에 기록
                    MMIO write 로 "tail pointer 움직임" 을 NIC 에 통지 (§11)
                      v
[9] NIC 하드웨어    DMA 로 DRAM 에서 프레임 읽음
                    FCS (CRC-32) 4B 추가
                    선로로 전기/광 신호 전송
```

### 10.2 sk_buff, sk_write_queue, slab

```c
// include/linux/skbuff.h 단순화
struct sk_buff {
    unsigned char *head, *data, *tail, *end;   // 프레임 버퍼 포인터들
    unsigned int len, data_len;
    __u16 protocol;
    struct sk_buff *next, *prev;               // 큐 연결용
    struct sock *sk;                           // 소유 소켓
    ...
};
```

**sk_buff 버퍼 구조** (메모리상):

```
 head      data                   tail      end
  v          v                      v        v
  +──────────+──────────────────────+────────+
  │ headroom │      payload         │ tailroom│
  +──────────+──────────────────────+────────+
             ^                      ^
             여기에 headers 를 앞쪽으로 "push" 한다
             (TCP -> IP -> Ethernet 추가되며 data 포인터가 앞으로 이동)
```

sk_buff 는 **slab** (`kmem_cache_alloc`) 에서 할당된다. slab 은 **같은 크기 객체의 전용 캐시**이다.

### 10.3 TCP 헤더 비트 단위

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Data |   Reserved|N|C|E|U|A|P|R|S|F|          Window           |
|Offset|          |S|W|C|R|C|S|S|Y|I|                           |
|      |          | |R|E|G|K|H|T|N|N|                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Options (가변)                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Data (payload)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

9-bit 플래그: NS / CWR / ECE / URG / ACK / PSH / RST / SYN / FIN.

### 10.4 IP 헤더 비트 단위

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Version|  IHL  |Type of Service|          Total Length         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Identification        |Flags|      Fragment Offset    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Time to Live |    Protocol   |         Header Checksum       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Source Address                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Destination Address                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Options (있을 때)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

중요 필드:

| 필드 | 기본값 | 의미 |
| --- | --- | --- |
| Version | 4 | IPv4 |
| IHL | 5 | 헤더 길이(4B 단위). 5=20B |
| TTL | 64 | hop 마다 -1. 0 되면 drop |
| Protocol | 6(TCP)/17(UDP)/1(ICMP) | 다음 계층 |
| Checksum | 헤더만 | hop 마다 재계산 (TTL 바뀜) |

### 10.5 ARP 와 next-hop — MAC 은 매 홉 바뀌고 IP 는 유지된다

라우터는 **인터페이스마다 MAC 이 따로** 있다.

```
  호스트 A          라우터          호스트 B
  11:11            (두 개!)          33:33
    |           eth0:22:22            |
    |           eth1:44:44            |
    |─────────────( LAN1 )            |
    |                 |               |
    |                 |─────────( LAN2 )──|
    |                                 |

 프레임 A->라우터:   src MAC 11:11, dst MAC 22:22   (IP: A->B)
 프레임 라우터->B:   src MAC 44:44, dst MAC 33:33   (IP: A->B 그대로)
```

| hop 마다 | 바뀜 | 그대로 |
| --- | --- | --- |
| Ethernet header (src/dst MAC) | 교체 | - |
| IP header (src/dst IP) | - | 유지 |
| TTL | -1 | - |
| IP checksum | 재계산 | - |
| TCP header / payload | - | 유지 |

비유: **IP 는 편지의 최종 주소, MAC 은 "다음 우체국" 주소.** 매 우체국(라우터) 마다 "다음 우체국" 만 갱신된다.

> **-> 상세**: [q08-host-network-pipeline.md](./q08-host-network-pipeline.md)
> 라우터 다중 인터페이스 MAC 교체 규칙, frame ① -> ② 재작성 표.

---

## §11. I/O Bridge — NIC / DMA / MMIO 바닥까지 [DFS 심화 구역]

§10.1 의 [8]~[9] 단계에서 "MMIO write", "DMA", "인터럽트" 라는 단어가 등장했다. 여기서 바닥까지 판다.

### 11.1 Northbridge/Southbridge -> IMC + PCH 진화

```
 옛날 (2008 이전)                   현재 (Intel/AMD 2014+)
 ──────────────                     ──────────────────────
                                     ┌──────────────────────┐
  CPU ─── FSB ─── Northbridge ─── DRAM   CPU die 안에 IMC  │
                     │                   │   (메모리 컨트롤러)│
                     │                   │                    │
                    PCIe / AGP            │   PCIe Root Complex│
                     │                   └──────────────────────┘
                  Southbridge                      │
                  (SATA, USB ...)                  │ DMI (QPI 유사)
                                                   v
                                                  PCH (구 South)
                                                  (SATA, USB, SPI, ...)
```

**IMC (Integrated Memory Controller)** 가 CPU 다이에 들어오면서 메모리 접근이 거의 "다이 내부 배선" 이 되었다.

### 11.2 세 주소 공간

```
                     ┌────────────────────────────────────┐
   DRAM 주소 공간    │  0x0000_0000 ~ 물리 메모리 끝        │  WB (writeback 캐시)
                     └────────────────────────────────────┘

                     ┌────────────────────────────────────┐
   MMIO 주소 공간    │  예: 0xF000_0000 ~ 0xFEFF_FFFF      │  UC/WC (비캐시)
                     │  NIC/GPU/디스크의 레지스터가 이 영역│
                     └────────────────────────────────────┘

                     ┌────────────────────────────────────┐
   Port I/O          │  16 비트, 0x0000 ~ 0xFFFF            │  in/out 명령 전용
                     │  레거시 (PS/2 키보드 등)            │
                     └────────────────────────────────────┘
```

커널은 `ioremap(phys, size)` 로 MMIO 영역을 커널 가상 주소로 매핑하고, 그 포인터에 **보통 MOV 명령**으로 읽고 쓴다. 하지만 페이지 속성이 UC/WC 라서 캐시에 들어가지 않고 바로 PCIe TLP 로 나간다 (§11.3).

### 11.3 PCIe TLP (Transaction Layer Packet)

```
 CPU core
   |
   | MOV  [mmio_addr], value
   v
 IMC / Root Complex
   |
   | TLP  (packet 형태)
   v
 ┌──────────────────────┐
 │ TLP 헤더             │   Type: MRd, MWr, CfgRd/Wr, Cpl, Msg
 │ Requester ID         │
 │ Tag                  │
 │ Address              │
 │ ─────────────────    │
 │ Payload (데이터)     │
 └──────────────────────┘
   |
   v
 PCIe 스위치/링크 -> NIC 의 레지스터
```

### 11.4 DMA — 커널이 "이 영역을 장치가 직접 건드리게 허락"

```c
// 커널 드라이버 (간단화)
dma_addr_t dma_handle;
void *cpu_vaddr = dma_alloc_coherent(dev, PAGE_SIZE, &dma_handle, GFP_KERNEL);

// cpu_vaddr   = 커널 VA (CPU 가 쓰는 주소)
// dma_handle  = 버스 주소 (NIC 이 DMA 에 쓰는 주소)
```

```
 CPU      ┌────────────┐        ┌────────────┐
 ────────>│ page cache │        │  NIC 내부  │
          └────────────┘        └────────────┘
              ^                        |
              | DMA 를 통한 장치 쓰기 (읽기도 가능)
              +────────────────────────+
              (CPU 안 거침)
```

**dma_map_single** 은 기존 버퍼를 "DMA 가능" 하게 준비 (IOMMU 가 있으면 이때 주소 번역 테이블 만듦).

### 11.5 MMIO vs DMA 역할 구분

| 동작 | MMIO | DMA |
| --- | --- | --- |
| 누가 | CPU | 장치 (NIC) |
| 크기 | 보통 1~4B 레지스터 | 큰 버퍼 전체 |
| 용도 | "작업 시작해" / "상태 봐" | 실제 데이터 운반 |
| 예 | tail pointer write | skb 데이터 -> NIC |

### 11.6 MSI-X 와 NAPI

**MSI-X** = PCIe 장치가 CPU 에 인터럽트를 **특정 메모리 주소에 write 해서** 통지하는 방식. 레거시 INTx pin 이 아니다.

**NAPI** = "인터럽트 받으면 한 번만 깨고, 이후엔 폴링으로 배치 처리". 고속 NIC 이 매 패킷마다 인터럽트를 쏘면 CPU 가 마비되니까.

```c
// drivers/net/ethernet/intel/e1000e/netdev.c (단순화)
static irqreturn_t e1000_msix_rx(int irq, void *data) {
    struct e1000_adapter *adapter = data;
    // 인터럽트 마스크 off, napi 스케줄
    napi_schedule_irqoff(&adapter->napi);
    return IRQ_HANDLED;
}

static int e1000_clean(struct napi_struct *napi, int budget) {
    int work_done = e1000_clean_rx_irq(adapter, budget);
    if (work_done < budget) {
        napi_complete_done(napi, work_done);
        // 인터럽트 다시 활성화
    }
    return work_done;
}
```

> **-> 상세**: [q10-io-bridge.md](./q10-io-bridge.md)
> IMC/PCH 진화, PCIe TLP 필드, DMA coherent vs streaming, IOMMU(VT-d), cache coherent DMA, 메모리 배리어 (mb/wmb/dma_wmb), MSI-X 설정, NAPI 예산, 프레임 송신 [1]~[9] 와 커널 레이어 매핑.

---

## §12. 수신 파이프라인 — 프레임이 read() 까지 올라오는 길

```
[1] NIC 하드웨어   프레임 수신 -> FCS 검사 OK -> DMA 로 수신 descriptor 의 buf 로 쓰기
[2] NIC           MSI-X 인터럽트 발사
[3] 커널          irq handler -> napi_schedule
[4] NAPI          softirq 에서 rx ring 폴링 -> skb 만듦
[5] Ethernet      헤더 벗김 (EtherType 판단) -> IP 계층
[6] IP            헤더 벗김 -> TCP 계층
[7] TCP           소켓 찾음 (4-tuple 매칭)
                  sk_receive_queue 에 skb enqueue
                  process_task 를 깨움 (wait_queue 에서)
[8] 유저          잠자던 read() 가 깨어나 copy_to_user 로 데이터 받음
```

**accept 큐와 receive 큐는 다름에 주의**:

```
 listen 소켓
   ├── SYN queue        (incomplete, SYN 만 받은 상태)
   └── accept queue     (ESTABLISHED, 3-way 완료, accept() 가 꺼낼 것)

 connected 소켓
   └── sk_receive_queue (실제 데이터 skb)
```

---

## §13. 네 개의 렌즈 — CPU / 메모리 / 커널 / 핸들

같은 통신 이벤트를 보는 네 시점.

```
 CPU 시점       "어떤 CPU 코어에서, CPL 몇에서, 어떤 명령이 실행 중인가"
 메모리 시점    "데이터가 유저 VA / 커널 VA / DMA 버스 어디에 있나"
 커널 시점      "어떤 struct 가 어떤 큐/해시/RB 트리에 들어있나"
 핸들 시점      "유저가 쥐고 있는 fd / pid / tid 가 어떤 커널 객체를 가리키나"
```

예: `read(4, buf, 95)` 를 네 렌즈로.

| 렌즈 | 보이는 것 |
| --- | --- |
| CPU | CPL 3 -> 0 전환, syscall 진입, 이후 vfs_read 분기 |
| 메모리 | 커널의 sk_receive_queue -> skb 데이터 -> copy_to_user -> 유저 buf |
| 커널 | fdtable[4] -> file -> socket -> sock -> sk_receive_queue |
| 핸들 | fd=4 (유저 쪽 번호), struct file * (커널 포인터) |

> **-> 상세**: [q09-network-cpu-kernel-handle.md](./q09-network-cpu-kernel-handle.md)

---

## §14. 응용 계층 — HTTP / MIME / FTP / Telnet

### 14.1 HTTP 요청/응답 예

```
 요청 (클라 -> 서버)
 ─────────────────
 GET /home.html HTTP/1.1
 Host: www.google.com
 User-Agent: curl/8.0
 Accept: */*
 \r\n

 응답 (서버 -> 클라)
 ─────────────────
 HTTP/1.1 200 OK
 Content-Type: text/html
 Content-Length: 137
 \r\n
 <html>...</html>
```

### 14.2 HTTP/1.0 vs 1.1

| 관점 | 1.0 | 1.1 |
| --- | --- | --- |
| 연결 | 요청마다 새로 | keep-alive 기본 |
| Host 헤더 | 없어도 됨 | 필수 (가상 호스팅) |
| 파이프라이닝 | 불가 | 가능 (실제론 잘 안 씀) |
| chunked encoding | 없음 | 있음 |

### 14.3 MIME 과 Content-Type

| MIME 타입 | 용도 |
| --- | --- |
| text/html | HTML |
| text/plain | 순수 텍스트 |
| image/png | PNG 이미지 |
| application/json | JSON |
| application/octet-stream | 바이너리 기본 |
| multipart/form-data | 파일 업로드 |

### 14.4 FTP / Telnet

- **FTP** : 제어 연결(21) + 데이터 연결(20 / passive 랜덤). HTTP 이전의 파일 전송.
- **Telnet** : 평문 원격 쉘. 보안이 없어서 SSH 로 대체됨.

> **-> 상세**: [q11-http-ftp-mime-telnet.md](./q11-http-ftp-mime-telnet.md)

---

## §15. Tiny Web Server — 가장 단순한 HTTP 서버

Tiny (CSAPP 11.6) 의 뼈대:

```c
int main(int argc, char **argv) {
    int listenfd = Open_listenfd(argv[1]);   // socket + bind + listen
    while (1) {
        int connfd = Accept(listenfd, ...);
        doit(connfd);
        Close(connfd);
    }
}

void doit(int fd) {
    parse_request(fd, ...);
    if (static)  serve_static(fd, ...);
    else         serve_dynamic(fd, ...);
}
```

**루틴 구조**:

```
 Tiny
  ├── main          : listen 루프
  ├── doit          : 요청 한 건 처리 (정적/동적 분기)
  ├── read_requesthdrs: HTTP 헤더 읽기/버리기
  ├── parse_uri     : URI 를 파일명/CGI 인자로 분리
  ├── serve_static  : 파일 mmap 해서 write
  ├── get_filetype  : 확장자 -> MIME
  ├── serve_dynamic : fork + dup2 + execve (§16)
  └── clienterror   : 4xx/5xx 응답
```

> **-> 상세**: [q12-tiny-web-server.md](./q12-tiny-web-server.md)

---

## §16. 동적 콘텐츠 — CGI, fork, dup2, execve

```
                     부모(Tiny)
                        │
               fork()  ─┤
                        ├─── 자식 프로세스
                        │        │
                        │        │ dup2(connfd, STDOUT_FILENO)
                        │        │ execve("/cgi-bin/adder", argv, envp)
                        │        │
                        │        └── adder 의 printf 가
                        │            곧바로 클라이언트에게 감
                        │
                  wait() 으로 자식 회수
```

**왜 dup2?** CGI 스크립트는 `printf` 로 쓸 뿐인데, 이 `printf` 가 소켓으로 나가게 하려면 `stdout (fd=1)` 을 `connfd` 로 바꿔 줘야 한다.

**왜 fork?** 자식 프로세스가 독립 주소 공간/환경을 가져야 execve 로 완전히 다른 프로그램이 될 수 있다.

**QUERY_STRING 과 argv**:

```
 /cgi-bin/adder?x=15&y=27

 커널: fork -> 자식
       dup2(connfd, 1)
       envp["QUERY_STRING"] = "x=15&y=27"
       execve("/cgi-bin/adder", ["adder"], envp)

 adder 내부:
   getenv("QUERY_STRING")  -> "x=15&y=27"
   sscanf 로 파싱 -> 계산 -> printf("%d\n", x+y)
```

> **-> 상세**: [q13-cgi-fork-args.md](./q13-cgi-fork-args.md)

---

## §17. Echo Server, EOF, Datagram

### 17.1 Echo 서버 루프

```c
while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
    Rio_writen(connfd, buf, n);   // 받은 만큼 그대로 보냄
}
```

### 17.2 EOF 는 "0 바이트 리턴"

| 상황 | read() 리턴 |
| --- | --- |
| 정상 데이터 | 양의 정수 |
| 상대가 close -> FIN | 0 (EOF) |
| 에러 | -1 (errno 확인) |
| non-blocking 데이터 없음 | -1 + EAGAIN |

### 17.3 TCP vs UDP echo

```
 TCP               bytes stream. boundary 없음. 여러 번 write 한 게 한 번 read 에 올 수 있음
 UDP               datagram. 한 번 send = 한 번 recv (여럿 뭉치지 않음)
```

> **-> 상세**: [q14-echo-server-datagram-eof.md](./q14-echo-server-datagram-eof.md)

---

## §18. Proxy — 서버이자 클라이언트

```
  [클라이언트] <-- TCP --> [Proxy] <-- TCP --> [Origin 서버]
                             |
                             ├─ accept()  쪽 = 서버처럼
                             └─ connect() 쪽 = 클라이언트처럼
```

한 프로세스 안에 **두 소켓**이 있다.

```c
int connfd  = Accept(listenfd, ...);              // 클라 -> proxy
int backfd  = Open_clientfd(origin_host, 80);     // proxy -> origin

// 클라가 보낸 것을 읽어 origin 에 쓰기
n = Rio_readnb(&rio_client, buf, ...);
Rio_writen(backfd, buf, n);

// origin 응답을 읽어 클라에 쓰기
n = Rio_readnb(&rio_back, buf, ...);
Rio_writen(connfd, buf, n);
```

Proxy 는 동시에 여러 클라를 처리해야 하므로 §19 의 스레드 풀/epoll 과 필연적으로 합쳐진다.

> **-> 상세**: [q15-proxy-extension.md](./q15-proxy-extension.md)

---

## §19. Iterative -> Concurrent — 스레드 풀, epoll, io_uring

### 19.1 Iterative 서버의 한계

```c
while (1) {
    connfd = accept(...);
    doit(connfd);       // 이거 끝날 때까지 다음 클라가 못 들어옴
    close(connfd);
}
```

### 19.2 세 가지 동시성 모델

| 모델 | 방식 | 장점 | 단점 |
| --- | --- | --- | --- |
| Process per connection | accept 후 fork | 격리 강함 | fork 비싸고 메모리 |
| Thread per connection | accept 후 pthread_create | 메모리 공유, 저렴 | 10k 스레드면 스택만 10GB |
| Event-driven (epoll) | 한 스레드가 수천 fd | 스레드 수 적음 | 복잡. blocking syscall 하나면 망함 |

### 19.3 Thread Pool

```
 ┌── listen accept 루프 (1 스레드) ──┐
 │   while(1) {                       │
 │     connfd = accept();             │
 │     queue.push(connfd);            │   <- Producer
 │     signal(cond);                  │
 │   }                                │
 └────────────────────────────────────┘
                 │
                 v   (mutex / cond)
 ┌── worker 스레드들 (N 개) ──────────┐
 │   while(1) {                       │
 │     connfd = queue.pop();           │   <- Consumer
 │     doit(connfd);                  │
 │   }                                │
 └────────────────────────────────────┘
```

### 19.4 epoll 과 io_uring

```
 select   : O(n), fd 최대 1024, 커널-유저 메모리 복사 매번
 poll     : O(n), 사이즈 제한 없음
 epoll    : O(1), 인터레스트 세트 커널이 유지
 io_uring : 비동기 ring 버퍼. syscall 도 전부 큐에 제출
```

> **-> 상세**: [q16-thread-pool-async.md](./q16-thread-pool-async.md)

---

## §20. 동시성과 락 — 기본

### 20.1 race condition 이 발생하는 순간

```c
int counter = 0;
// 두 스레드가 동시에
counter++;   // 이건 원자적이지 않다
```

기계 수준:

```
 load  [counter], %eax
 add   $1, %eax
 store %eax, [counter]
```

이 세 명령 사이에 context switch 가 일어나면 다른 스레드가 끼어들 수 있다.

### 20.2 기본 도구

| 도구 | 용도 | 비용 |
| --- | --- | --- |
| `atomic` | 단일 변수 증감/교환 | 싸다 (CAS 1회) |
| `mutex` | 임계 구역 보호 | 중 (futex, 슬립) |
| `rwlock` | 읽기 N, 쓰기 1 | 중 |
| `spinlock` | 짧은 임계, 인터럽트 컨텍스트 | 경우에 따라 매우 쌈 |
| `cond var` | "어떤 조건 될 때까지 대기" | mutex 와 세트 |
| `semaphore` | N 개 자원 카운트 | 중 |

### 20.3 올바른 패턴: lock ordering

```c
// 데드락 방지: 항상 주소가 작은 쪽을 먼저 잠금
Account *first  = (a < b) ? a : b;
Account *second = (a < b) ? b : a;
pthread_mutex_lock(&first->lock);
pthread_mutex_lock(&second->lock);
// ... 작업 ...
pthread_mutex_unlock(&second->lock);
pthread_mutex_unlock(&first->lock);
```

> **-> 상세**: [q17-concurrency-locks.md](./q17-concurrency-locks.md)

---

## §21. 스레드 동시성 실패 13선 [DFS 심화 구역]

이 섹션은 "락을 안 걸면 실제로 어떻게 죽는가" 의 실전. 각 시나리오의 종료 신호와 원인.

### 21.1 스레드는 주소 공간 공유 실행 흐름

```
 task_struct (thread 1) ─┐
 task_struct (thread 2) ─┼── 같은 mm_struct (페이지 테이블) 공유
 task_struct (thread 3) ─┘      같은 files_struct 공유
                               각자 stack / registers 만 다름
```

그래서 **한 스레드의 잘못이 프로세스 전체를 죽인다**.

### 21.2 MESI 캐시 일관성

```
 상태      의미                          설명
 M (Modified)   나만 가짐, 수정됨          메모리는 stale
 E (Exclusive)  나만 가짐, 수정 안 됨      깨끗
 S (Shared)     여럿이 읽기만 함
 I (Invalid)    버려짐

 코어 A 가 쓰기 -> 다른 코어에 "Invalidate" 브로드캐스트 (RFO)
 이 때 버스/링 트래픽이 생기고, 읽는 쪽이 stall 걸린다.
```

**False Sharing** : 서로 다른 변수가 같은 캐시 라인(64B) 에 있으면 서로 무관한데도 invalidate 가 튀어서 느려진다.

### 21.3 13개 실패 시나리오 요약

| # | 시나리오 | 증상 | 신호/종료 |
| --- | --- | --- | --- |
| S1 | Lost Update (count++) | 카운터 손실 | 데이터 오염 (크래시 없음) |
| S2 | Torn Write | 절반만 갱신된 값 | 데이터 오염 |
| S3 | Use-After-Free | 이미 free 된 포인터 역참조 | SIGSEGV |
| S4 | Double-Free | glibc 감지 -> abort | SIGABRT |
| S5 | Null Deref | NULL 역참조 | SIGSEGV |
| S6 | Publish-Before-Init | 초기화 끝나기 전 객체 노출 | SIGSEGV / 논리 오류 |
| S7 | Linked List 경쟁 | 노드 손실 / 순환 | 무한 루프 또는 SIGSEGV |
| S8 | ABA | CAS 가 속음 | 데이터 오염 |
| S9 | Deadlock | 두 스레드 영구 대기 | 프로세스 hang |
| S10 | errno 경쟁 | 잘못된 에러 처리 | 논리 오류 |
| S11 | malloc 경합 | 힙 손상 | SIGABRT |
| S12 | Signal 안전성 위반 | 핸들러에서 malloc | 데드락 또는 SIGSEGV |
| S13 | False Sharing | 성능 급락 | 크래시 없음, 처리량 저하 |

각 시나리오에 대해 ① 코드 ② 실제 실행 트레이스 ③ 증상 ④ 수정 을 q18 에서 상세히 다룬다.

> **-> 상세**: [q18-thread-concurrency.md](./q18-thread-concurrency.md)
> 13 개 시나리오 코드/어셈블리/수정안 + Linux 커널 락 결정 트리 (atomic_t / spinlock / mutex / rwsem / seqlock / RCU) + TSan/Helgrind/KCSAN.

---

## §22. 마무리 — 이번 주 SQL API 서버로의 연결

이번 주 과제의 골격은 결국 아래 흐름의 구현이다.

```
[클라] ── HTTP(GET/POST) ──> [API 서버]
                                │
                                ├─ accept (§8, §15)
                                ├─ parse request  (§14)
                                ├─ thread pool 에서 실행 (§19)
                                │     ├─ mutex 로 DB connection 풀 보호 (§20, §21)
                                │     └─ SQL 실행 -> JSON 직렬화
                                │
                                └─ write response (§10 송신 파이프라인)
```

읽기 순서 권장:

```
 1일차   §1 ~ §5                큰 그림 + 유저/커널 경계
 2일차   §6 (-> q04 전체)        파일시스템 바닥
 3일차   §7 ~ §10 (+ q05,q06,q07,q08)    소켓 + 송신 파이프라인
 4일차   §11 (-> q10 전체)       I/O 브릿지 바닥
 5일차   §12 ~ §18 (+ q09,q11~q15)        수신 + 응용 + Tiny/CGI/Echo/Proxy
 6일차   §19 ~ §21 (-> q16,q17,q18 전체)  동시성 + 실패 시나리오 바닥
 7일차   과제 구현 + 리뷰
```

---

## 참고 연결

### q 문서 전체 링크 (DFS 순서)

1. [q01-network-hardware.md](./q01-network-hardware.md) — 네트워크 하드웨어 (Ethernet/Bridge/Router/LAN/WAN/프레임 비트맵)
2. [q02-ip-address-byte-order.md](./q02-ip-address-byte-order.md) — IP 주소 체계 / 바이트 순서
3. [q03-dns-domain-cloudflare.md](./q03-dns-domain-cloudflare.md) — DNS / Cloudflare
4. [q04-filesystem.md](./q04-filesystem.md) — 파일시스템 완전 해부 (VFS/ext4/페이지캐시/blk-mq)
5. [q05-socket-principle.md](./q05-socket-principle.md) — 소켓 3층 구조
6. [q06-ch11-4-sockets-interface.md](./q06-ch11-4-sockets-interface.md) — 11.4 Sockets Interface 함수 + addrinfo
7. [q07-tcp-udp-socket-syscall.md](./q07-tcp-udp-socket-syscall.md) — TCP/UDP, 소켓 시스템콜
8. [q08-host-network-pipeline.md](./q08-host-network-pipeline.md) — 호스트 송신 파이프라인
9. [q09-network-cpu-kernel-handle.md](./q09-network-cpu-kernel-handle.md) — CPU/메모리/커널/핸들 네 렌즈
10. [q10-io-bridge.md](./q10-io-bridge.md) — I/O Bridge 완전 해부 (IMC/PCH/PCIe/DMA/MSI-X)
11. [q11-http-ftp-mime-telnet.md](./q11-http-ftp-mime-telnet.md) — HTTP/FTP/MIME/Telnet
12. [q12-tiny-web-server.md](./q12-tiny-web-server.md) — Tiny Web Server
13. [q13-cgi-fork-args.md](./q13-cgi-fork-args.md) — CGI / fork / dup2
14. [q14-echo-server-datagram-eof.md](./q14-echo-server-datagram-eof.md) — Echo Server / EOF / Datagram
15. [q15-proxy-extension.md](./q15-proxy-extension.md) — Proxy
16. [q16-thread-pool-async.md](./q16-thread-pool-async.md) — Thread Pool / epoll / io_uring
17. [q17-concurrency-locks.md](./q17-concurrency-locks.md) — 동시성과 락 기본
18. [q18-thread-concurrency.md](./q18-thread-concurrency.md) — 스레드 동시성 실패 13선
