# Part D. OS 기반 (FD · 프로세스 · 메모리)

`docs/question/01-team-question-parts.md` 의 Part D (D-1 ~ D-4) 질문 묶음에 대한 답을 한 문서에 정리한다.
각 섹션은 원 질문 목록을 맨 위에 두고, 그 질문들을 이어서 답하는 설명을 본문으로 제시한다.

## 커버하는 질문 매핑

| 질문 ID | 주제 | 관련 L 노드 |
|--------|------|------------|
| D-1 | Unix I/O & FD 추상화 — VFS · FDT · v-node · 0/1/2 · dup2 · RIO | L5 |
| D-2 | 프로세스 족보 & fd 수명 — PID 1 · fork · reparent · O_CLOEXEC · TCP vs UDP close | L18 |
| D-3 | 가상 메모리 & 프로세스 레이아웃 — heap · mmap · demand paging · VSZ/RSS · OOM | L19 |
| D-4 | 코드 디테일 — adder.c 의 printf vs sprintf, content 문자열 대입 | L20 |

## Part D 를 관통하는 한 문장

프로세스는 **`task_struct`** 한 덩어리로 태어나 자기만의 **fdtable** 과 **주소 공간(mm_struct)** 을 들고 다니며, `open()` / `socket()` 은 **fd(정수)** 를 주고받고 그 fd 는 커널 heap 의 **`struct file` → `struct inode` / `struct socket`** 로 내려가 디스크 블록이나 TCP 제어 블록에 연결되고, `fork()` 는 이 fdtable 과 VM 을 **CoW 로 복제** 하고, `execve()` 는 `O_CLOEXEC` 가 켜진 fd 만 닫으면서 VM 을 완전히 갈아엎고, `mmap` 과 `brk` 로 할당된 페이지는 처음 접근될 때 비로소 **page fault → 물리 프레임 할당(demand paging)** 으로 진짜 RAM 이 붙는다.

## 예시 상황 세팅

Part A / B / C 에서 썼던 네트워크 시나리오를 **OS 관점** 에서 다시 본다.

```text
호스트 프로세스 트리

  systemd              PID = 1
    └─ agetty          PID = 90
         └─ login      PID = 120
              └─ bash  PID = 500        (셸)
                   └─ ./tiny 8080  PID = 1234, PPID = 500
                        └─ CGI adder  PID = 1300, PPID = 1234

표준 fd 약속

  0 = stdin   1 = stdout   2 = stderr
  3 = listening socket (listenfd)
  4 = connected socket (connfd, accept 결과)

VM 레이아웃 (x86_64 userland, PIE)

  0x0000_5555_5555_4000   .text        (실행 가능, 읽기, 공유)
  0x0000_5555_5555_6000   .data/.bss
  0x0000_5555_5555_7000   heap start (brk) — sbrk 로 위로 확장
           ...
  0x0000_7fff_abcd_0000   mmap 영역 — 아래로 확장
  0x0000_7fff_ffff_e000   stack top — 아래로 확장
  0x0000_7fff_ffff_f000   argv / envp
  ───────────────────────────────────────────────
  0xffff_8000_0000_0000   kernel space 시작 (TASK_SIZE 경계)

프로세스 크기 예시

  VSZ = 24 MB  (매핑된 페이지 전체)
  RSS =  4 MB  (실제 물리 메모리 붙은 페이지)
```

---

## D-1. Unix I/O & FD 추상화 (L5)

### 원 질문

- VFS 네 개의 핵심 객체(superblock / inode / dentry / file)는 어떤 관계이고 커널에서 어떻게 표현되나? (최우녕)
- 디스크 위의 ext4 는 실제로 어떤 레이아웃으로 쓰여 있나? (최우녕)
- `open("/home/woonyong/a.txt")` 는 커널 안에서 몇 단계를 거치나? (최우녕)
- `read()` 한 번이 VFS → FS → page cache → block layer → 디스크로 어떻게 흐르나? (최우녕)
- 소켓·파이프·procfs 가 왜 "파일" 처럼 보이나? (최우녕)
- 모든 외부 장치를 하나의 I/O 로 보고 똑같이 처리하는 시스템을 만든 것인가? 소켓도 결국 파일이라는 말 때문에 모든 장치를 USB 처럼 생각해도 되는가? 소켓은 주소가 저장되는 구조체인가, 파일 같은 것인가? (최현진)
- fd 는 무엇이고 책 어디에서 나오는가? FDT 는 뭐라고 부르면 되고, 정확히 무엇인가? (최현진)
- FD table 에서 file table entry 주소를 어떻게 찾아가는가? file table, v-node 정보는 어디서 가지고 오는가? fd / v-node / i-node 연결 관계는 어떻게 되는가? (정범진, 최현진)
- 식별자 테이블을 프로세스마다 가지는 이유는 무엇이고, 결국 fd 와 FDT 는 무엇을 하려고 있는가? (최현진)
- stdin/stdout/stderr 가 "꼭 0/1/2 여야" 하는 이유가 있나? 커널이 fd 번호를 구분하나? (최우녕)
- 3번 fd 에 열린 소켓을 0/1번에 꽂으려면 어떻게 해야 하나? (최우녕)
- "unbuffered", "line-buffered", "flush" 가 무슨 소리인가? 커널 버퍼? libc 버퍼? (최우녕)
- rio 함수들은 일반적인 file 함수와 어떤 차이점을 가지고 있는가? (정범진)
- 앱을 터미널에서 켠 경우와 GUI 아이콘 클릭으로 켠 경우 FD table 에서 0, 1, 2 번 값이 다른 이유? (정범진)
- V-node 는 왜 필요한가? (정범진)

### 설명

이 모든 질문은 **"UNIX 커널은 모든 I/O 소스를 `struct file` 이라는 하나의 공통 객체로 포장하고, 프로세스는 그 포인터를 담은 프로세스별 배열(FDT)에 정수 인덱스로 접근한다"** 라는 한 가지 설계로 답할 수 있다. 바로 이 설계가 "모든 것은 파일" 이라는 슬로건의 실체다.

- **VFS 4객체** 는 커널의 가상 파일시스템 계층이다.
  - `struct super_block` — 마운트된 파일시스템 한 개당 하나. "이 디스크/파티션의 메타데이터" (블록 크기, 루트 inode, op 테이블).
  - `struct inode` — 파일 하나의 실체(데이터 블록 위치, 권한, 크기, 타임스탬프). **파일 이름은 inode 에 없다.**
  - `struct dentry` — 디렉터리 엔트리 캐시. "name ↔ inode 매핑" 을 캐시해 경로 조회를 빠르게 한다.
  - `struct file` — **열린 상태** (현재 오프셋, 플래그, f_op). 같은 inode 를 두 번 열면 file 은 두 개, inode 는 하나.
- **왜 v-node 가 필요한가**: CSAPP 의 "v-node" 는 실제 Linux 에서는 `struct inode` + 그 위의 VFS 추상화를 뭉친 개념이다. 핵심은 `struct file->f_inode` 가 구체 파일시스템에 **간접적으로** 연결되어 있어서, ext4/btrfs/tmpfs/procfs 가 같은 `read()` 시스템 콜을 각자의 코드로 처리할 수 있게 해준다. 이 간접층이 없으면 `read(fd)` 가 ext4 를 직접 호출해 버려서 다른 파일시스템이 붙을 수 없다.
- **ext4 디스크 레이아웃**: 파티션은 block group 들로 쪼개져 있고, 각 group 은 `superblock (사본) + group descriptor + block bitmap + inode bitmap + inode table + data blocks` 순서로 정렬된다. superblock 은 0번 group 에 정답, 나머지는 백업.
- **open("/home/woonyong/a.txt")** 의 내부 단계:
  1. 유저 `open()` → `sys_openat` → `do_filp_open`.
  2. **fd 할당**: `get_unused_fd_flags` 가 fdtable 의 open_fds 비트맵에서 가장 낮은 비트 0 자리를 찾아 예약.
  3. **경로 해석**: `path_openat` → `link_path_walk` 가 `/`, `home`, `woonyong`, `a.txt` 를 한 컴포넌트씩 dentry 캐시에서 찾고, 캐시 miss 면 해당 디렉터리의 inode 를 읽어 디스크에서 엔트리 조회.
  4. 최종 dentry 에서 inode 확보 → `alloc_file_pseudo` 로 `struct file` 생성, `f_inode = inode`, `f_op = ext4_file_operations`, `f_flags = O_RDONLY | ...`.
  5. 예약한 fd 자리에 `files->fd_array[fd] = file` 로 설치, 비트맵 on.
  6. 유저에게 정수 fd 반환.
- **read(fd, buf, n)** 의 흐름:
  1. `sys_read(fd)` → fdtable 에서 `struct file*` 꺼냄.
  2. `file->f_op->read_iter`. 파일이면 `ext4_file_read_iter` → `generic_file_read_iter`.
  3. `f_pos` 오프셋에 대응하는 page cache 페이지를 찾음 (`find_get_page`).
  4. miss 면 `a_ops->readpage` 호출 → block layer 가 LBA 로 변환해 I/O 요청 큐 (`bio`) 에 올림.
  5. SSD/HDD DMA → 페이지가 채워지면 `copy_to_user` 로 유저 버퍼에 복사 → `f_pos` 증가.
- **소켓도 파일처럼 보이는 이유**: `socket()` 이 만드는 `struct file` 은 `f_op = socket_file_ops` 로 설정되어 `sock_read_iter` / `sock_write_iter` 가 호출된다. VFS 입장에서는 그냥 `struct file` 이므로 `read`/`write`/`close`/`poll` 이 그대로 통한다. 파이프/procfs/epoll/timerfd/eventfd 도 동일.
- **fd / FDT 의 존재 이유**: 커널이 직접 포인터를 유저에게 줄 수 없기 때문이다. 포인터는 커널 주소공간의 값이고 유저가 조작하면 커널이 털린다. 그래서 **정수 인덱스** 를 유저에 주고, 커널은 프로세스별 fdtable 로 그 인덱스를 포인터로 바꾼다. 프로세스별로 따로 두는 이유는 "A 의 fd 3 과 B 의 fd 3 은 전혀 다른 객체" 여야 하기 때문.
- **stdin/stdout/stderr = 0/1/2 의 이유**: 커널은 0/1/2 를 특별히 대우하지 않는다. **libc 와 셸의 관습** 일 뿐이다. 셸이 자식을 fork 하기 직전에 `fd_array[0] = /dev/pts/0` 로 세팅해 놓으면, 자식은 `write(1, ...)` 만 해도 터미널로 찍힌다. 커널 입장에서 1 은 그냥 fdtable 의 1번 슬롯이다.
- **dup2(3, 0)**: fdtable 의 0번 슬롯에 3번이 가리키던 `struct file*` 을 복사하고, refcount 를 +1. 기존 0번이 가리키던 파일은 refcount -1.
- **버퍼링의 계층**: `printf` → libc `stdout` 버퍼(유저 공간 4KB) → flush 시 `write` 시스템 콜 → 커널 page cache/sk_buff → 물리 디바이스. unbuffered 는 libc 버퍼 크기 0, line-buffered 는 `\n` 을 만날 때 flush, fully buffered 는 가득 차야 flush.
- **RIO vs stdio**: RIO (CSAPP 의 `rio_readn` / `rio_readlineb`) 는 **짧은 카운트(short count) 재시도** 를 보장한다. 소켓/파이프에서 `read(10)` 이 3을 주는 일이 흔한데, RIO 는 내부 루프로 10 을 채울 때까지 재시도한다. 또 `rio_readlineb` 는 자체 버퍼를 가지고 라인 단위로 잘라 준다 (libc `fgets` 의 소켓 버전).
- **터미널 vs GUI**: 터미널에서 띄운 프로세스의 0/1/2 는 모두 `/dev/pts/N` (현재 터미널) 에 묶인다. GUI 아이콘으로 띄우면 부모가 launchd/systemd 이고 터미널이 없으므로 0/1/2 는 `/dev/null` 로 설정한다 (그래서 printf 가 사라짐).

### VFS 4객체 관계

```text
마운트된 파일시스템 하나
──────────────────────────────────────

        struct super_block            (ext4 한 파티션)
         ├─ s_op  = ext4_sops
         ├─ s_blocksize = 4096
         ├─ s_root (root dentry)
         └─ s_inodes (리스트)

            ▲  ▲
            │  │ 소속
            │  └────────────────────────────────┐
            │                                   │
        struct dentry                       struct inode
         ├─ d_name = "a.txt"                 ├─ i_ino = 123456
         ├─ d_parent ─▶ dentry("woonyong")   ├─ i_op = ext4_file_iops
         ├─ d_inode  ─▶ inode 오른쪽          ├─ i_fop = ext4_file_fops
         └─ d_sb     ─▶ super_block          ├─ i_size = 4096
                                             ├─ i_mode = 0100644
                                             └─ i_mapping (page cache anchor)

            ▲
            │ "열린 상태"
            │
        struct file                    (프로세스가 open() 할 때마다 새로 생성)
         ├─ f_inode ─▶ inode 위
         ├─ f_op    = ext4_file_fops
         ├─ f_flags = O_RDONLY | O_CLOEXEC
         ├─ f_pos   = 0   (다음 read 가 읽을 오프셋)
         └─ f_count = 2   (fd 두 개가 가리키고 있으면)

            ▲
            │ fdtable 에서 포인터
            │
    current->files->fd_array[3] = &file
```

### fd → file → (v-node | socket) 3 계층 체인

```text
유저 프로세스 PID=1234 (./tiny 8080)
───────────────────────────────────────────────────────────

    int fd = 3;                ← 유저가 보는 정수

커널 공간 (slab, kmalloc)
───────────────────────────────────────────────────────────

    task_struct (PID=1234)
     │
     ├─ files ─▶ files_struct            ★ 프로세스별 FDT
     │           ├─ count  = 1 (참조)
     │           ├─ fdt    ─▶ fdtable
     │           │           ├─ max_fds = 64
     │           │           ├─ fd_array[0] ─▶ file("/dev/pts/0")
     │           │           ├─ fd_array[1] ─▶ file("/dev/pts/0")
     │           │           ├─ fd_array[2] ─▶ file("/dev/pts/0")
     │           │           ├─ fd_array[3] ─▶ file("listen sock") ── 아래로
     │           │           ├─ open_fds (bitmap) = 0b0000_1111
     │           │           └─ close_on_exec (bitmap) = 0b0000_0000
     │
     ├─ mm ─▶ mm_struct (주소 공간, D-3 에서)
     │
     └─ sighand, pending, ...

    fd=3 의 file 체인
    ──────────────────────────────────────────────
      struct file
       ├─ f_op = socket_file_ops          ← 소켓이면 이거, 파일이면 ext4_file_fops
       ├─ f_flags = O_RDWR | O_NONBLOCK | O_CLOEXEC
       ├─ private_data ─▶ struct socket
       └─ f_count = 1

      struct socket (fd 가 소켓일 때 private_data)
       ├─ state = SS_UNCONNECTED → SS_CONNECTED
       ├─ type  = SOCK_STREAM
       ├─ ops   = inet_stream_ops
       ├─ sk    ─▶ struct sock (tcp_sock)
       └─ file  (back-pointer)

      struct sock (tcp_sock)
       ├─ sk_family  = AF_INET
       ├─ sk_state   = TCP_ESTABLISHED
       ├─ sk_receive_queue (skb list)
       ├─ sk_write_queue   (skb list)
       └─ snd_una, snd_nxt, rcv_nxt, ...

    fd=5 의 file 체인 (일반 파일일 때)
    ──────────────────────────────────────────────
      struct file
       ├─ f_op = ext4_file_fops
       ├─ f_inode ─▶ inode("/var/log/tiny.log")
       │                ├─ i_mode  = 0100644
       │                ├─ i_size  = 8192
       │                ├─ i_data[15] (extent tree root)
       │                └─ i_mapping (page cache)
       └─ f_pos = 4096
```

**요약**: `fd` 는 **프로세스별** 인덱스. `struct file` 은 **열림 상태** (오프셋, 플래그). `struct inode` / `struct socket` 은 **실제 자원** 이고 여러 `struct file` 이 같은 inode 를 가리킬 수 있다.

### struct file 의 플래그 비트 레이아웃

`open(2)` 에 넘기는 플래그가 그대로 `f_flags` 의 비트로 들어간다. 주요 값 (`/usr/include/asm-generic/fcntl.h` 기준):

```text
f_flags 비트 위치        플래그 이름        8진수          16진수
──────────────────────────────────────────────────────────────
bit 0  (하위 2비트)     O_RDONLY           000000         0x0
bit 0                   O_WRONLY           000001         0x1
bit 1                   O_RDWR             000002         0x2
bit 6                   O_CREAT            000100         0x40
bit 7                   O_EXCL             000200         0x80
bit 8                   O_NOCTTY           000400         0x100
bit 9                   O_TRUNC            001000         0x200
bit 10                  O_APPEND           002000         0x400   ★ log 파일에 자주
bit 11                  O_NONBLOCK         004000         0x800   ★ 소켓에 자주
bit 12                  O_DSYNC            010000         0x1000
bit 15                  O_LARGEFILE        0100000        0x8000
bit 16                  O_DIRECTORY        0200000        0x10000
bit 17                  O_NOFOLLOW         0400000        0x20000
bit 19                  O_CLOEXEC          02000000       0x80000 ★ execve 시 자동 close

예시) tiny 가 소켓을 열 때
  flags = O_RDWR | O_NONBLOCK | O_CLOEXEC
        = 0x2 | 0x800 | 0x80000
        = 0x80802
        = 비트:  0000_0000_0000_1000_0000_1000_0000_0010
                                    ▲         ▲         ▲
                                    │         │         └─ RDWR
                                    │         └─ NONBLOCK
                                    └─ CLOEXEC
```

### fdtable 의 open_fds 비트맵

fd 번호 할당은 "가장 낮은 비트 0 자리" 를 찾는 연산이다.

```text
tiny 가 막 시작한 직후 fdtable open_fds 비트맵

  비트 위치 7 6 5 4 3 2 1 0
  ────────────────────────
  값        0 0 0 0 0 1 1 1        ← 0/1/2 만 사용 중
                        ▲ ▲ ▲
                        │ │ └─ stdin  = /dev/pts/0
                        │ └─── stdout = /dev/pts/0
                        └───── stderr = /dev/pts/0

socket() 호출 → 가장 낮은 0 자리 (bit 3) 할당

  비트 위치 7 6 5 4 3 2 1 0
  ────────────────────────
  값        0 0 0 0 1 1 1 1        ← fd=3 예약
                    ▲
                    └─ listenfd

accept() 호출 → 가장 낮은 0 자리 (bit 4) 할당

  비트 위치 7 6 5 4 3 2 1 0
  ────────────────────────
  값        0 0 0 1 1 1 1 1        ← fd=4 예약
                  ▲
                  └─ connfd
```

**dup2(3, 0)** 이 하는 일도 이 비트맵 위에서 보인다.

```text
dup2(3, 0) 직전
  fd_array[0] ─▶ file("/dev/pts/0")
  fd_array[3] ─▶ file("connfd")

dup2(3, 0) 직후
  fd_array[0] ─▶ file("connfd")     ← 같은 파일을 가리킴
  fd_array[3] ─▶ file("connfd")     ← f_count 가 +1 되어 2

close(3) 이후
  fd_array[0] ─▶ file("connfd")
  fd_array[3] ─▶ NULL               ← f_count 가 1로 줄어듬
  open_fds    = 0b...1111_0001      ← bit 3 꺼짐
```

CGI 에서 `dup2(connfd, STDOUT_FILENO)` 가 왜 마법처럼 보이냐면, **`write(1, ...)` 이 무엇을 가리키는지는 fdtable 의 1번 엔트리가 결정** 하기 때문이다. 자식이 dup2 한 순간부터 자식의 stdout 은 TCP 소켓이 된다.

### ext4 디스크 레이아웃 (한 block group)

```text
파티션 (예: /dev/sda1, 4 KiB block) 의 첫 block group

  offset (byte)         내용
  ───────────────────────────────────────────
  0x00000 ~ 0x003FF     Boot sector (1 KiB, group 0 에만)
  0x00400 ~ 0x00BFF     Superblock (1 KiB, 중복본은 다른 group 에도)
  0x01000 ~ 0x01FFF     Group Descriptor Table
  0x02000 ~ 0x02FFF     Reserved GDT blocks
  0x03000 ~ 0x03FFF     Data block bitmap (4 KiB = 32768 블록 커버)
  0x04000 ~ 0x04FFF     Inode bitmap (4 KiB = 32768 inode 커버)
  0x05000 ~ 0x44FFF     Inode table (typ. 8192 inode × 256 B)
  0x45000 ~           Data blocks (파일 실제 내용)

struct ext4_inode (on-disk, 256 B)
  0x00: i_mode     (u16)  0100644 등
  0x02: i_uid      (u16)
  0x04: i_size_lo  (u32)
  0x08: i_atime    (u32)
  0x0C: i_ctime    (u32)
  0x10: i_mtime    (u32)
  0x14: i_dtime    (u32)
  0x18: i_gid      (u16)
  0x1A: i_links    (u16)
  0x1C: i_blocks_lo(u32)
  0x20: i_flags    (u32)
  0x24: i_osd1     (u32)
  0x28: i_block[15] (60 B, extent tree root 또는 direct/indirect 블록 포인터)
  0x64: i_generation ...
  ...

extent 방식이면 i_block[0..11] 에 struct ext4_extent_header 가 들어감.
  eh_magic = 0xF30A
  eh_entries
  eh_max
  eh_depth
그 뒤에 ext4_extent 배열 (start block, len, physical block high/low)
```

### read("/home/woonyong/a.txt") 의 흐름 (바이트 단위)

```text
유저 프로세스
  buf = malloc(4096);
  read(fd, buf, 4096);
        │
        ▼
커널 진입  sys_read
        ▼
  current->files->fd_array[fd]  =  struct file *f
        ▼
  f->f_op->read_iter(iter, ...)  =  ext4_file_read_iter
        ▼
  generic_file_read_iter
        │
        ├─ loop: index = f_pos >> PAGE_SHIFT (여기선 0)
        │
        ├─ page = find_get_page(inode->i_mapping, index)
        │     │
        │     └─ cache miss → readpage(inode, page)
        │                          │
        │                          ├─ ext4_readpage → extent tree 조회
        │                          ├─ LBA 계산 (physical block × 8)
        │                          ├─ submit_bio(READ, bio)
        │                          │     │
        │                          │     └─ block layer → scheduler → driver
        │                          │                                     │
        │                          │                                     ▼
        │                          │                           NVMe Submission Queue
        │                          │                                     │
        │                          │                                     ▼
        │                          │                               DMA to page buffer
        │                          │                                     │
        │                          │                                     ▼
        │                          │                           NVMe Completion Queue
        │                          │
        │                          └─ end_buffer_async_read → PG_uptodate=1
        │
        ├─ copy_page_to_iter(page, offset_in_page, remaining, iter)
        │     │
        │     └─ copy_to_user(userbuf, page_data, bytes)
        │
        └─ f_pos += bytes,  return bytes

유저 프로세스
  buf 에 데이터 도착
```

### 직접 검증 ① — 프로세스의 fd 실제 매핑

```bash
pid=$(pgrep -f "tiny 8080")
ls -l /proc/$pid/fd
# lr-x------ 1 root root 64 Apr 17 13:00 0 -> /dev/pts/0
# lrwx------ 1 root root 64 Apr 17 13:00 1 -> /dev/pts/0
# lrwx------ 1 root root 64 Apr 17 13:00 2 -> /dev/pts/0
# lrwx------ 1 root root 64 Apr 17 13:00 3 -> socket:[12345]
# lrwx------ 1 root root 64 Apr 17 13:00 4 -> socket:[12346]
```

### 직접 검증 ② — open() 이 내부적으로 몇 단계를 거치는지

```bash
strace -f -e trace=openat,read,close -o open.trace ./tiny 8080
head open.trace
# openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
# openat(AT_FDCWD, "/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
# openat(AT_FDCWD, "/var/log/tiny.log", O_WRONLY|O_APPEND|O_CREAT, 0644) = 4
```

### 직접 검증 ③ — VFS 가 투명한지 (같은 read 가 다른 파일시스템에 먹힘)

```bash
cat /etc/hostname          # ext4
cat /proc/self/status      # procfs (가상)
cat /sys/devices/system/cpu/online  # sysfs
# 전부 read() 한 번으로 동작. f_op 가 다를 뿐.
```

### 직접 검증 ④ — 소켓도 파일처럼 stat 이 먹힘

```bash
stat /proc/$(pgrep tiny)/fd/3
# 출력: File: 3 -> socket:[12345]
#       Size: 0   Blocks: 0   Device: 0,9   Inode: 12345
#       Access: srwxrwxrwx   Uid/Gid
```

---

## D-2. 프로세스 족보 & fd 수명 (L18)

### 원 질문

- 모든 프로세스가 fork 로 만들어진다면, 그 포크의 "맨 처음" 부모는 누구인가? (최우녕)
- 터미널에서 프로그램을 띄우면 터미널이 부모인가? 셸(bash)이 부모인가? (최우녕)
- "크롬이 떠 있는데 엣지를 열면 엣지는 크롬의 자식인가?" 부모가 되는 기준이 뭔가? (최우녕)
- 자식이 먼저 죽으면 고아가 된 손자는 "할아버지" 로 reparent 되는가? subreaper 플래그는 어디서 언제 켜는가? (최우녕)
- `fork()` 하면 fdtable 까지 복제된다는데, 그럼 stdin/stdout/stderr 도 공유하는 건가? (최우녕)
- 그런데 실제론 어떤 프로세스는 터미널을 물려받고(ls), 어떤 건 안 물려받는 것 같다(systemd 서비스). 기준과 메커니즘이 뭔가? (최우녕)
- `close(fd)` 하면 부모/자식이 공유하던 struct file 은 즉시 사라지나? refcount 는 언제 0이 되나? (최우녕)
- `O_CLOEXEC` 는 언제, 누구에게 작용하나? 자식이 fd 에 접근하는 순간 닫히는가? (최우녕)
- 파일 fd 와 소켓 fd 가 "완전히 똑같은 규칙" 으로 작동한다는 건 어디까지 사실인가? (최우녕)
- 소켓 하나의 close() 가 TCP 는 60초 FIN/ACK, UDP 는 즉시 끝 — 같은 함수가 어떻게 다르게 동작하나? (최우녕)
- fork 로 복제된 fd 를 부모/자식이 동시에 read/write 하면 무슨 일이 일어나나? (최우녕)
- fd 로 연결할 수 있는 객체는 뭐가 있나? 전부 나열하고 언제 쓰는지까지. (최우녕)

### 설명

이 질문들은 **"프로세스는 `task_struct` 한 덩어리의 트리고, fork 는 부모 트리를 얕게 복제하고 execve 는 복제된 이미지를 바꿔치기하고, fd 는 `struct file` 에 대한 참조 카운트 덕분에 공유가 가능하다"** 라는 골격으로 하나로 엮인다.

- **맨 처음 부모 (PID 1)**: 부팅 시 커널이 직접 만드는 `init` 프로세스. 현대 Linux 에서는 `systemd`, 컨테이너/Docker 에서는 `tini`, `pid1`, 또는 그냥 앱 자신. PID 1 은 **fork 없이** 커널의 `rest_init → kernel_thread` 로 탄생한다. 커널이 `/sbin/init` 을 execve 해서 만든다.
- **터미널 vs 셸의 부모 관계**: 터미널 에뮬레이터(gnome-terminal, iTerm2) 가 자식으로 **bash** 를 fork+execve 한다. `ls` 를 치면 **bash** 가 다시 fork+execve 한다. 그래서 `ls` 의 부모는 bash, bash 의 부모는 터미널 에뮬레이터, 에뮬레이터의 부모는 데스크톱 환경(gnome-shell, launchd). 터미널은 **pts (pseudo terminal slave)** 라는 파일을 bash 의 0/1/2 에 매핑해 주고, bash 는 자식에게 그대로 복사.
- **크롬이 떠 있는데 엣지를 열면**: 엣지는 **크롬의 자식이 아니다**. 엣지는 Finder/Explorer/gnome-shell 이 fork+execve 한 자식이다. 부모는 **실제로 fork 한 프로세스** 만 된다. 윈도우를 같이 쓴다는 건 GUI 합성기(compositor) 의 문제고 프로세스 트리와는 무관.
- **Reparent & subreaper**: 자식이 부모보다 먼저 **죽으면**(정확히 말하면 자식 입장에서 자기 부모가 죽으면) 자식은 고아가 된다. 커널은 고아를 기본적으로 **PID 1** 로 reparent 한다. 하지만 프로세스가 `prctl(PR_SET_CHILD_SUBREAPER, 1)` 로 "subreaper" 플래그를 켜면, 그 프로세스 자손들은 중간에 부모가 죽어도 **PID 1 이 아니라 이 subreaper 에게** reparent 된다. 컨테이너 런타임, `systemd --user`, `tini` 가 이걸 쓴다. 그래야 컨테이너 경계를 넘어 PID 1 에 고아가 쌓이지 않는다.
- **fork 의 fdtable 복제**: `fork()` 는 default 로 **fdtable 을 복제** (각 엔트리의 struct file 을 그대로 가리키고 `f_count++`) 한다. `clone()` 에 `CLONE_FILES` 를 주면 fdtable 자체를 **공유** (같은 files_struct) 한다. 그래서 자식이 `dup2(4, 1)` 하면 **부모의 stdout 은 안 바뀐다** (fdtable 은 각자의 것).
- **stdin/stdout/stderr 공유**: fork 직후 0/1/2 는 같은 `struct file*` 을 가리킨다. 그래서 터미널 출력이 부모/자식이 섞여 찍힌다. 이건 "상속" 이고 execve 해도 `O_CLOEXEC` 가 안 켜져 있으면 그대로 남는다.
- **터미널 상속 기준**: `ls` 는 bash 의 자식이고 bash 의 0/1/2 가 pts 이므로 그대로 물려받음. `systemd` 서비스는 `systemd` 가 자식으로 fork 한 뒤 **0/1/2 를 `/dev/null` 또는 journal 소켓으로 재설정** 하고 execve. 그래서 터미널이 안 붙는다. `StandardOutput=journal` 같은 unit 설정이 이 리다이렉션을 제어.
- **close(fd) 와 refcount**:
  - fd_array[fd] 를 NULL 로 만들고 비트맵에서 비트 끔.
  - `struct file` 의 `f_count--`. 아직 다른 fd 나 다른 프로세스가 참조하면 0 이 아니므로 살아있다.
  - 0 이 되면 `__fput` 이 호출되어 inode 참조 해제, 소켓이면 `sock->ops->release` → TCP 는 FIN 전송 시작.
- **O_CLOEXEC 동작 타이밍**: execve 하는 **순간** 커널이 fdtable 을 훑어 `close_on_exec` 비트맵이 켜진 fd 를 전부 close. fork 자체에는 영향이 없다. "자식이 접근하는 순간" 이 아니라 "자식이 execve 하는 순간" 이 정확한 답.
- **파일 fd vs 소켓 fd 차이**:
  - **똑같은 규칙**: read/write/close/poll/dup/fcntl/fork 상속/O_CLOEXEC/O_NONBLOCK.
  - **다른 규칙**: lseek 이 소켓에는 ESPIPE. ioctl 명령어가 다름. mmap 가능 여부 다름. shutdown(2) 은 소켓 전용.
- **TCP vs UDP close**:
  - 같은 `close(fd)` → `__fput` → `sock->ops->release`.
  - TCP 에서는 `tcp_close` 가 호출되어 send queue flush → FIN 전송 → FIN_WAIT_1 / FIN_WAIT_2 / TIME_WAIT. 이 전이는 **커널이 fd 해제와 별도로 들고 있다** (fd 는 이미 닫혔지만 struct sock 은 TIME_WAIT 동안 살아있다).
  - UDP 는 `udp_close` → connection 이 없으므로 즉시 sock 해제.
- **fork 후 동시 read/write**:
  - 같은 `struct file` 을 공유하므로 `f_pos` 가 공유된다. 부모가 100바이트 읽으면 f_pos=100, 자식이 바로 read 하면 100 부터. 동시에 read 하면 커널이 f_pos 접근을 락(inode->i_rwsem) 으로 보호하므로 데이터가 섞이진 않지만 **순서는 비결정적**.
  - 소켓이면 f_pos 가 의미 없고 (stream offset 은 커널 snd_nxt/rcv_nxt), 두 프로세스가 같은 connfd 에 write 하면 segment 경계에서 섞일 수 있다.
  - pipe 의 경우 PIPE_BUF (4KB) 이하 write 는 atomic 보장.
- **fd 로 연결 가능한 객체 전체 목록**:
  - **일반 파일** (regular file) — ext4/btrfs/...
  - **디렉터리** (read 로 getdents, openat 의 anchor)
  - **심볼릭 링크** (보통 O_PATH 로만)
  - **소켓** — TCP/UDP/UNIX domain/netlink/packet
  - **파이프** — `pipe(2)` 한 쌍
  - **FIFO** (named pipe) — `mkfifo`
  - **캐릭터 디바이스** — /dev/tty, /dev/null, /dev/urandom
  - **블록 디바이스** — /dev/sda
  - **/proc, /sys 의 가상 파일**
  - **eventfd** — 프로세스 간 이벤트 카운터
  - **signalfd** — 시그널을 read 로 받기
  - **timerfd** — 타이머 만료를 read 로 받기
  - **epoll fd** — 이벤트 멀티플렉싱
  - **inotify / fanotify fd** — 파일 변화 감지
  - **memfd** — 익명 메모리 파일 (공유 메모리용)
  - **pidfd** — 프로세스를 fd 로 다루기 (`pidfd_open`, kill 안 흘리기)
  - **userfaultfd** — page fault 를 유저에서 처리
  - **seccomp-unotify fd** — 시스템 콜 감사

### 프로세스 트리 구조

```text
PID=1  systemd                 (fork 없이 커널이 직접 execve 한 최초)
  │
  ├─ PID=90   agetty /dev/tty1
  │    └─ PID=120  login       (성공 시 execve)
  │         └─ PID=500  bash   ★ 여기부터 pts/0 을 0/1/2 로 물려줌
  │              └─ PID=1234  ./tiny 8080    ★ Part A/B/C 의 서버
  │                   └─ PID=1300  /cgi-bin/adder  ★ Part C 의 CGI
  │
  ├─ PID=200  sshd              (원격 로그인용)
  │    └─ PID=400  sshd (세션)
  │         └─ PID=450  bash    (원격 셸)
  │
  ├─ PID=300  systemd --user    ★ subreaper 켜진 경우 많음
  │    └─ PID=310  gnome-shell
  │         ├─ PID=600  chrome  (크롬)
  │         └─ PID=700  firefox (엣지/파이어폭스)
  │
  └─ PID=50   kthreadd          (커널 스레드 공장, 유저 x)
```

### fork() 가 fdtable 을 복제하는 순간

```text
부모 PID=500 bash
───────────────────────────────
  files_struct(count=1)
   └─ fdtable
      ├─ fd_array[0] ─▶ struct file (pts/0)   f_count=3
      ├─ fd_array[1] ─▶        "               (같은 파일)
      ├─ fd_array[2] ─▶        "
      └─ fd_array[3] ─▶ struct file (history)  f_count=1

    fork() 호출
    ───────────────
      커널이:
        1) 새 task_struct 할당 (PID=1234 예약)
        2) CLONE_FILES 안 주었으므로 files_struct 를 복제
           → 새 files_struct(count=1), 새 fdtable
           → 각 fd_array[i] 는 부모와 같은 struct file * 을 가리킴
           → 각 struct file 의 f_count 를 +1

자식 PID=1234 직후
───────────────────────────────
  files_struct(count=1) ★ 부모와 다른 struct
   └─ fdtable        ★ 부모와 다른 struct
      ├─ fd_array[0] ─▶ struct file (pts/0)   f_count=6  (부모 3 + 자식 3)
      ├─ fd_array[1] ─▶        "
      ├─ fd_array[2] ─▶        "
      └─ fd_array[3] ─▶ struct file (history)  f_count=2

포인트: fd 번호 공간과 fdtable 은 각자, struct file 은 공유.
```

### O_CLOEXEC 비트 동작 타임라인

```text
시각 T0: tiny 가 listenfd 를 열 때
  fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    → 커널: alloc fd=3, struct file 생성, f_flags |= O_CLOEXEC
    → fdtable->close_on_exec 비트 3 = 1
       close_on_exec 비트맵:  0b0000_1000

시각 T1: accept() → connfd=4, O_CLOEXEC 전파 안 함 (기본)
       close_on_exec 비트맵:  0b0000_1000 (bit 4 는 0)

시각 T2: CGI 처리 위해 fork() 호출
  자식 fdtable 의 close_on_exec 도 그대로 복제됨.
       자식 close_on_exec:    0b0000_1000

시각 T3: 자식이 dup2(4, 1) 실행
  dup2 는 기본적으로 대상의 close_on_exec 비트를 끈다
       자식 close_on_exec:    0b0000_1000 (bit 1 은 0)
       자식 fd_array[1] ─▶ connfd

시각 T4: 자식이 execve("/cgi-bin/adder", argv, envp)
  커널이 close_on_exec 비트맵을 훑음:
    bit 0: 0 → 그대로
    bit 1: 0 → 그대로   ★ dup2 된 connfd 가 자식 stdout 으로 살아남음
    bit 2: 0 → 그대로
    bit 3: 1 → close!   ★ listenfd 는 닫힘 (CGI 프로세스가 가져가면 안 되니까)
    bit 4: 0 → 그대로  (connfd 원본, 하지만 자식은 이걸 안 씀)

시각 T5: adder.c 가 printf
  printf → libc buffer → write(1, ...) → TCP 소켓으로 나감 (클라이언트 응답)
```

이 시나리오가 **Part C 의 CGI 동작의 핵심** 이다. `O_CLOEXEC` 가 없으면 listenfd 가 CGI 자식에 누출되어 CGI 가 다음 클라이언트 연결을 훔쳐갈 수 있다.

### close() 시 refcount 소멸 경로

```text
부모 close(3)  ──────────────▶  자식 close(3)
                                        │
  f_count: 2 → 1                        ▼
  struct file 은 아직 살아있음    f_count: 1 → 0
                                         │
                                         ▼
                                   __fput(struct file *)
                                         │
                  ┌──────────────────────┼──────────────────────┐
                  ▼                      ▼                      ▼
              일반 파일                소켓                  파이프
              ext4_release       sock_close                pipe_release
                  │                      │                      │
                  ▼                      ▼                      ▼
              inode 참조 -1          sock->ops->release      read/write end 해제
                                    └─ inet_release
                                         │
                                         ▼
                                    tcp_close      (TCP)    udp_destroy_sock (UDP)
                                         │                         │
                                         ▼                         ▼
                                    FIN 전송            ★ 즉시 sock 해제
                                    FIN_WAIT_1
                                    └─ FIN_WAIT_2
                                         └─ TIME_WAIT  (2×MSL = 60s)
                                              └─ sock 진짜 해제
```

TCP 에서 `close()` 는 fd 는 바로 풀리지만 **커널이 FIN/ACK 교환과 TIME_WAIT 을 혼자 이어 나간다**. 그래서 `ss -tn` 으로 보면 프로세스가 이미 죽었는데도 TIME_WAIT 연결이 남아있는 것이다.

### 직접 검증 ① — 프로세스 족보

```bash
pstree -p
ps --forest -eo pid,ppid,user,cmd
# init/systemd 아래 agetty → login → bash → tiny → adder 계보가 보임
```

### 직접 검증 ② — reparent 관찰

```bash
# 1. bash 에서 sleep 1000 & 돌려놓고
sleep 1000 &
# 2. 부모(bash) 를 강제 종료
kill -9 $$
# 3. 새 터미널에서
ps -o pid,ppid,cmd -p $(pgrep sleep)
# PPID 가 1 (systemd) 또는 subreaper PID 로 바뀌어 있음
```

### 직접 검증 ③ — O_CLOEXEC 가 execve 에서 작동

```bash
# open 시 O_CLOEXEC 안 주기
python3 -c '
import os, fcntl
fd = os.open("/etc/hosts", os.O_RDONLY)
print("cloexec?", bool(fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC))
os.execlp("ls", "ls", f"/proc/self/fd/{fd}")
'
# /proc/self/fd/3 이 보임 → execve 에서도 살아남음

# O_CLOEXEC 주기
python3 -c '
import os, fcntl
fd = os.open("/etc/hosts", os.O_RDONLY | os.O_CLOEXEC)
os.execlp("ls", "ls", f"/proc/self/fd/{fd}")
'
# /proc/self/fd/3 이 없음 → execve 에서 닫혔음
```

### 직접 검증 ④ — TIME_WAIT 관찰

```bash
# 서버 쪽에서 close 직후
ss -tn state time-wait
# State  Recv-Q Send-Q Local Address:Port Peer Address:Port
# TIME-WAIT 0    0     127.0.0.1:8080    127.0.0.1:51234

# 60초 뒤 다시
ss -tn state time-wait
# 사라짐
```

---

## D-3. 가상 메모리 & 프로세스 레이아웃 (L19)

### 원 질문

- libc 는 가상 메모리의 어디에 있는가? 힙 영역인가? 커널 메모리인가? (최우녕)
- 가상 메모리 레이아웃 다이어그램의 방향 — 스택이 위에서 아래로, 힙이 아래에서 위로 — 맞는가? (최우녕)
- 힙(brk/sbrk)과 mmap 은 뭐가 다른가? (최우녕)
- mmap 을 호출하면 즉시 물리 메모리가 할당되는가? 힙은? (demand paging 의 실체) (최우녕)
- VSZ 와 RSS 의 차이, overcommit, OOM killer 와 이들의 관계는? (최우녕)

### 설명

이 질문들은 **"모든 프로세스는 자기 전용의 가상 주소 공간을 가지는데, 커널은 그 공간의 일부만 VMA 로 매핑해 두고, 매핑된 곳도 실제로 접근되기 전에는 물리 RAM 을 안 붙인다"** 라는 한 가지 원리로 답할 수 있다.

- **libc 는 어디에**: 유저 주소 공간의 **mmap 영역** 에 **공유 매핑** 으로 올라간다. 커널 메모리 아니다. 모든 프로세스가 **같은 물리 페이지** 를 공유한다 (read-only 로). `cat /proc/self/maps` 에 `.../libc.so.6` 라인이 보인다.
- **레이아웃 방향**: 맞다. 스택은 높은 주소에서 낮은 주소로, 힙은 낮은 주소에서 높은 주소로 확장. 그래서 "stack overflow" 가 힙을 침범하기 전에 스택의 하한 guard page 를 건드린다.
- **heap(brk) vs mmap**:
  - **heap**: `brk(addr)` 으로 "현재 힙의 끝" 포인터를 이동. 힙은 **하나의 연속된 VMA**. `malloc` 이 작은 할당은 brk 로, 큰 할당(>128KB default)은 `mmap` 으로 분기.
  - **mmap**: 아무 곳에나 별도의 VMA 를 생성. 파일 매핑(`MAP_SHARED|MAP_PRIVATE`), 익명 매핑(`MAP_ANONYMOUS`), 공유 메모리 등. `munmap` 으로 개별 해제 가능.
- **demand paging 실체**:
  - `mmap` 호출 순간: 커널은 `struct vm_area_struct` 만 생성. page table 은 비어 있고 PTE 는 `Present=0`. 실제 물리 프레임은 0 개.
  - 첫 접근 순간: MMU 가 PTE 를 보고 `Present=0` → **page fault** (예외 #14). 커널의 `do_page_fault` → `handle_mm_fault` 가 물리 프레임 하나를 할당하고 PTE 에 채움 → `Present=1, A=1`.
  - `brk` 도 같다. `brk(new_top)` 은 VMA 만 확장하고, 실제 RAM 은 첫 접근 때 붙음.
  - `MAP_POPULATE` 플래그를 주면 mmap 시점에 미리 채운다.
- **VSZ vs RSS**:
  - **VSZ (virtual size)**: 이 프로세스가 **매핑해 둔** 가상 주소 공간 전체 크기. VMA 들 크기의 합. 물리 RAM 과 무관.
  - **RSS (resident set size)**: 그중 **실제로 물리 RAM 에 붙어 있는** 페이지의 크기. demand paging 때문에 초기에는 RSS << VSZ.
  - 예: `malloc(1GB)` 하고 안 쓰면 VSZ +1GB, RSS +0.
- **overcommit**: 커널이 RAM+swap 용량보다 큰 VMA 를 허용할지. `/proc/sys/vm/overcommit_memory` (0=heuristic/기본, 1=always, 2=never). 기본은 적당히 넘쳐도 허용 — 어차피 쓰지 않을 가능성이 있으니까.
- **OOM killer**: 실제 접근이 몰려 물리 RAM 이 바닥나면 커널은 `oom_killer` 를 돌려 `oom_score` 가 높은 프로세스를 SIGKILL. 서버 운영에서 악명 높음. `/proc/PID/oom_score_adj` 로 조정.

### x86_64 userland VM 레이아웃 (PIE 기준)

```text
가상 주소 (48-bit 유효, canonical form)
  높음   0xFFFF_FFFF_FFFF_FFFF ┐
                               │ 커널 공간 (TASK_SIZE 이상)
         0xFFFF_8000_0000_0000 ┘
                               ▒  정통 canonical hole (사용 불가)
         0x0000_7FFF_FFFF_FFFF ┐
         0x0000_7FFF_FFFF_F000 │ argv, envp, aux vector
         0x0000_7FFF_FFFF_E000 │ Stack top  ──────── VM_GROWSDOWN
                               │   │
                               │   ▼ (자라는 방향)
         0x0000_7FFF_FA00_0000 │ Stack bottom (현재)
                               │ guard page
                               │
                               │ (mmap 영역, 아래로)
         0x0000_7F12_3456_0000 │ libc.so.6                 ──── file mapping (shared, RO)
         0x0000_7F12_3411_0000 │ ld-linux.so                    file mapping
         0x0000_7F11_0000_0000 │ anonymous mmap (malloc 128KB+)  MAP_ANONYMOUS
                               │
                               │     ... 넓은 빈 공간 ...
                               │
         0x0000_5555_555B_0000 │ Heap top (brk)           ──────── VMA anon
                               │   ▲
                               │   │ (자라는 방향)
         0x0000_5555_5556_7000 │ Heap start (brk 초기)
         0x0000_5555_5555_6000 │ .bss
         0x0000_5555_5555_5000 │ .data
         0x0000_5555_5555_4000 │ .rodata
         0x0000_5555_5555_3000 │ .text (exec + RO)
         0x0000_5555_5555_0000 │ ELF header
                               │
         0x0000_0000_0040_0000 │ (non-PIE 의 경우 .text 여기)
  낮음   0x0000_0000_0000_0000 ┘

확인 명령:
  cat /proc/$(pgrep tiny)/maps
```

`/proc/PID/maps` 예시:

```text
555555554000-555555556000 r-xp 00000000 08:02 12345 /home/wy/tiny/tiny
555555755000-555555756000 r--p 00001000 08:02 12345 /home/wy/tiny/tiny
555555756000-555555757000 rw-p 00002000 08:02 12345 /home/wy/tiny/tiny
555555757000-555555778000 rw-p 00000000 00:00 0     [heap]
7f1234000000-7f1234200000 rw-p 00000000 00:00 0
7f12340b1000-7f12340d3000 r-xp 00000000 08:02 222   /lib/.../libc.so.6
...
7ffdabcd0000-7ffdabcf1000 rw-p 00000000 00:00 0     [stack]
7ffdabcff000-7ffdabd00000 r-xp 00000000 00:00 0     [vdso]
ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0 [vsyscall]
```

### x86_64 PTE 64-bit 비트 레이아웃 (4KB page)

```text
비트:  63   62..52   51 .......... 12 11..9  8 7 6 5 4 3 2 1 0
       │   │         │              │  │    │ │ │ │ │ │ │ │ │
       NX  Available │              │  AVL  G PAT D A PCD PWT U/S R/W P
                     │   Physical   │
                     │   Frame #    │
                     │   (PFN,40b)  │

의미 요약
  bit  0  P     Present         1 이면 이 매핑 유효. 0 이면 page fault.
  bit  1  R/W   Read/Write      0=읽기전용, 1=쓰기가능. .rodata 는 0.
  bit  2  U/S   User/Supervisor 0=커널만, 1=유저도. 유저 페이지는 1.
  bit  3  PWT   Write-Through   캐시 정책
  bit  4  PCD   Cache Disable   캐시 정책
  bit  5  A     Accessed        HW 가 접근 시 1 (LRU 힌트)
  bit  6  D     Dirty           HW 가 쓰기 시 1 (swap-out 판단)
  bit  7  PAT   (4KB 에서 PAT)  페이지 속성 테이블 선택
  bit  8  G     Global          컨텍스트 스위치 시 TLB 유지 (커널용)
  bit  9-11    Available        OS 자유
  bit 12-51    PFN              4KB 정렬 물리 프레임 번호 (52-bit 물리 주소까지)
  bit 52-62    Available        (MPK 등)
  bit 63  NX    No-Execute      1 이면 실행 금지 (W^X 보호)

예시) tiny 의 .text 페이지 한 장의 PTE
  물리 프레임 PFN = 0x87654
  R/W = 0 (읽기전용)
  U/S = 1 (유저)
  NX  = 0 (실행 가능)
  P   = 1 (유효)
  A   = 1 (접근됨)
  D   = 0

  하위 9비트 (bit 8..0):  0_0_0_0_1_0_0_1_0_1 = ...
  실제 조립 후 PTE 값 ≈ 0x0000_0000_0876_5025
```

### VMA flags 비트 (vm_flags)

`struct vm_area_struct->vm_flags` 의 주요 비트:

```text
bit  이름              의미
────────────────────────────────────────────────────────
0    VM_READ           현재 읽기 허용
1    VM_WRITE          현재 쓰기 허용
2    VM_EXEC           현재 실행 허용
3    VM_SHARED         공유 매핑 (fork 후 양쪽이 같은 물리 페이지)
4    VM_MAYREAD        mprotect 으로 VM_READ 켤 수 있는지
5    VM_MAYWRITE       mprotect 으로 VM_WRITE 켤 수 있는지
6    VM_MAYEXEC        mprotect 으로 VM_EXEC 켤 수 있는지
7    VM_MAYSHARE       MAP_SHARED 로 변경 가능
8    VM_GROWSDOWN      스택처럼 아래로 자람
9    VM_UFFD_MISSING   userfaultfd 대상
10   VM_PFNMAP         물리 페이지에 직접 매핑 (/dev/mem)
11   VM_DENYWRITE      파일 쓰기 금지 (구버전 실행파일)
12   VM_LOCKED         mlock 됨 (스왑 아웃 금지)
13   VM_IO             MMIO 영역
14   VM_SEQ_READ       순차 읽기 힌트 (readahead 강)
15   VM_RAND_READ      랜덤 읽기 힌트 (readahead 약)
16   VM_DONTCOPY       fork 시 복제 안 함
17   VM_DONTEXPAND     mremap 으로 확장 금지

예시) [heap] VMA 의 flags
  VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC
  = 0x1 | 0x2 | 0x10 | 0x20 | 0x40
  = 0x73
  = 비트:  0000_0000_0111_0011

예시) [stack] VMA
  VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE | VM_GROWSDOWN
  = 0x1 | 0x2 | 0x10 | 0x20 | 0x100
  = 0x133

예시) libc.so.6 의 .text VMA
  VM_READ | VM_EXEC | VM_MAYREAD | VM_MAYEXEC | VM_MAYSHARE
  = 0x1 | 0x4 | 0x10 | 0x40 | 0x80
  = 0xD5
```

### heap(brk) vs mmap 비교 다이어그램

```text
시나리오 A — malloc(64) (작은 크기)
────────────────────────────────────

시점 T0: brk = 0x5555_5556_7000,  heap VMA 하나
         [0x5555_5556_7000 ~ 0x5555_5556_7000] 크기 0

시점 T1: malloc(64) 호출
  ptmalloc 내부: 힙에 공간 없음 → sbrk(0x21000) 호출
    커널: 기존 heap VMA 를 0x21000 확장 → end = 0x5555_5558_8000
          page table 은 안 건드림 (demand paging)

시점 T2: *ptr = 'A' 쓰기
  MMU: PTE Present=0 → #PF
  커널: 물리 프레임 0x9ABC0 할당, PTE 에 PFN+P+RW+U 세팅
  이제 VSZ +132KB, RSS +4KB


시나리오 B — malloc(256 KB) (큰 크기)
────────────────────────────────────

시점 T0: (위와 같은 상태)

시점 T1: malloc(256*1024) 호출
  ptmalloc: 128KB 초과 → mmap(256*1024, MAP_ANONYMOUS | MAP_PRIVATE)
    커널: 새 VMA 생성 (heap VMA 와 분리)
          [0x7F11_2345_0000 ~ 0x7F11_2349_0000]

시점 T2: 쓰기 시작
  #PF 마다 PFN 하나씩 붙음 → 256KB 전부 접근 시 RSS +256KB

시점 T3: free(ptr)
  ptmalloc: 이 할당은 mmap 으로 온 것 (chunk header 보고 판단)
            → munmap 으로 VMA 자체를 커널에 돌려줌
  이제 VSZ -256KB, RSS -256KB
```

`free` 가 **작은 할당에는 brk 축소를 안 한다** 는 점이 중요하다. 그래서 오래 돌린 서버는 RSS 는 줄어도 VSZ 는 그대로 남는다.

### demand paging 바이트 레벨 시퀀스

```text
유저 코드
  char *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
  // 이 시점 VSZ +4KB, RSS +0, PTE=0 (없음)

  *(p + 1000) = 'X';
          │
          ▼  (MMU)
  PTE 조회: 비어 있음  →  exception #PF
          │
          ▼
  커널  do_page_fault(addr=p+1000, err_code=write+user+not_present)
          │
          ├─ find_vma(addr) → VMA (ok)
          ├─ VMA->vm_flags & VM_WRITE ? yes
          ├─ anon page? yes → alloc_page(GFP_HIGHUSER_MOVABLE)
          │     → 예: struct page 인덱스 0x9ABC0
          │     → 가상↔물리  PFN = 0x9ABC0
          ├─ zero page 로 먼저 세팅 후, COW 트리거 가능
          ├─ PTE 작성:  PFN<<12 | P=1 | R/W=1 | U/S=1 | A=1 | D=0
          └─ 리턴, 유저 코드 재실행
          │
          ▼
  이제 *(p+1000) 쓰기 성공, PTE.D=1 (하드웨어가 세팅)
  RSS +4KB
```

### 직접 검증 ① — VM 레이아웃 확인

```bash
pid=$(pgrep tiny)
cat /proc/$pid/maps
# [heap], [stack], libc, mmap 영역 전부 보임

# libc 가 공유되는지 확인 — 여러 프로세스의 libc 주소
for p in $(pgrep -f 'bash|sshd|systemd'); do
  grep 'libc' /proc/$p/maps | head -1
done
# ASLR 이 켜져 있으면 주소는 다르지만, 물리 페이지는 공유
```

### 직접 검증 ② — VSZ vs RSS 실감

```bash
cat > vszdemo.c <<'EOF'
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    void *p = malloc(100 * 1024 * 1024);   /* 100 MB */
    sleep(5);
    /* 이 시점 VSZ +100MB, RSS 거의 0 */
    memset(p, 1, 100 * 1024 * 1024);
    sleep(60);
    /* 이 시점 VSZ +100MB, RSS +100MB */
    return 0;
}
EOF
gcc vszdemo.c -o vszdemo && ./vszdemo &
sleep 1; ps -o pid,vsz,rss,cmd -p $!
sleep 10; ps -o pid,vsz,rss,cmd -p $!
```

### 직접 검증 ③ — page fault 카운팅

```bash
pid=$(pgrep vszdemo)
grep -E '(VmPeak|VmSize|VmRSS|VmData)' /proc/$pid/status

# fault 수
awk '{print "minflt="$10, "majflt="$12}' /proc/$pid/stat
# minflt: minor fault (물리 할당만)
# majflt: major fault (디스크까지 내려감)
```

### 직접 검증 ④ — overcommit 관찰

```bash
cat /proc/sys/vm/overcommit_memory
# 0  heuristic
# 1  always 허용 (malloc 1 TB 해도 성공)
# 2  strict

# overcommit 전체 한도
cat /proc/sys/vm/overcommit_ratio
cat /proc/meminfo | grep -E 'MemTotal|MemAvailable|CommitLimit|Committed_AS'
```

### 직접 검증 ⑤ — OOM killer 로그

```bash
dmesg -T | grep -i 'killed process'
# [Wed Apr 17 14:22:01 2026] Out of memory: Killed process 1234 (vszdemo)
#   total-vm:10240000kB, anon-rss:8192000kB, ...
```

---

## D-4. 코드 디테일 — adder.c (L20)

### 원 질문

- adder.c 에서 왜 그냥 `printf()` 로 바로 출력하지 않고, 굳이 `sprintf()` 로 content 문자열을 먼저 만들어야 하나요? 그리고 content 에 `=` 로 문자열을 넣으면 안 되나요? (CSAPP 3판 920페이지) (이우진)

### 설명

이 질문은 두 개가 한 뭉치다.
하나는 **"CGI 응답은 왜 sprintf 로 한 번 모아서 보내야 하는가"** — HTTP 응답 헤더의 `Content-Length` 문제.
다른 하나는 **"C 에서 `char content[] = "..."` 는 되는데 왜 `content = "..."` 는 안 되는가"** — 배열과 포인터의 차이, 그리고 문자열이 `.rodata` 에 들어간다는 사실.

- **Content-Length 문제**:
  - HTTP/1.0 응답은 `Content-Length: N\r\n\r\n` 뒤에 정확히 N 바이트가 와야 한다. 중간에 모자라거나 남으면 클라이언트가 파싱에 실패하거나 connection 유지 모드에서 다음 요청을 오염시킴.
  - `printf` 를 곧바로 쏘면, 본문을 찍기 전에는 길이를 모른다. 그래서 헤더에 `Content-Length` 를 못 쓴다.
  - 해법: **먼저 본문을 `sprintf` 로 C 문자열 버퍼에 만들어 놓고**, 그 버퍼의 `strlen` 을 `Content-Length` 헤더에 써서 함께 출력.
- **content 문자열 대입**:
  - C 에서 `char content[100]` 는 **배열 객체**. 이름 `content` 는 "배열 자체" 이지 포인터가 아님. 대입의 왼쪽에 올 수 없다 (modifiable lvalue 가 아님).
  - `char content[] = "hello"` 는 선언 시 **초기화 구문** 특수 처리. 컴파일러가 스택에 'h','e','l','l','o','\0' 6 바이트를 직접 펼쳐 담음.
  - 선언이 끝난 뒤 `content = "world"` 는 **대입문** 이고, 이건 에러다.
  - 포인터는 다르다. `char *content = "hello"; content = "world";` 는 컴파일됨. 단, `"hello"` 는 `.rodata` 에 있는 불변 메모리라 `content[0] = 'H'` 는 세그폴트.

### adder.c 의 핵심 (CSAPP p.920)

```c
/* adder.c — 간략화 */
int main(void) {
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1=0, n2=0;

    /* QUERY_STRING 에서 두 수 파싱 */
    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        *p = '\0';
        strcpy(arg1, buf);
        strcpy(arg2, p+1);
        n1 = atoi(arg1);
        n2 = atoi(arg2);
    }

    /* 본문을 먼저 만든다  ★ 핵심 */
    sprintf(content, "Welcome to add.com: ");
    sprintf(content + strlen(content),
            "THE Internet addition portal.\r\n<p>");
    sprintf(content + strlen(content),
            "The answer is: %d + %d = %d\r\n<p>", n1, n2, n1+n2);

    /* 이제 strlen(content) 가 Content-Length */
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}
```

### sprintf → printf 흐름의 메모리 레이아웃

```text
실행 중인 CGI (adder) 프로세스의 스택

        높은 주소
        ┌─────────────────────────────┐
        │ envp 영역                    │ ★ tiny 가 setenv 한 환경변수들
        │   QUERY_STRING=15&27          │
        │   CONTENT_LENGTH=...          │
        │   REQUEST_METHOD=GET          │
        └─────────────────────────────┘
        │ argv                         │
        └─────────────────────────────┘
          ...
        ┌─────────────────────────────┐
        │ main 의 스택 프레임          │
        │   char arg1[MAXLINE]          │  ┐
        │   char arg2[MAXLINE]          │  │ 지역 배열 (스택)
        │   char content[MAXLINE]  ★   │  ┘
        │     [0] = 'W'                 │
        │     [1] = 'e'                 │
        │     [2] = 'l'                 │
        │     ...                       │
        │     [80] = ' '                │
        │     [81] = '=                '│  sprintf 가 계속 덧붙임
        │     [99] = '\0'               │
        └─────────────────────────────┘
          ...
        낮은 주소

포맷 문자열 "Welcome to add.com: " 은 어디 있나?
  → .rodata 세그먼트. 읽기전용 페이지.
     sprintf 는 이 문자열을 스택의 content 배열에 복사.

printf("%s", content) 가 실제로 하는 일
  → libc 내부 stdout 버퍼(힙 또는 .bss) 에 content 내용을 복사
  → flush 시 write(1, stdout->_IO_write_base, n)
  → 1 은 CGI 입장에서 dup2 된 connfd → TCP 소켓
  → Part A 의 write() 경로 그대로 NIC 까지
```

### content 에 `=` 로 문자열을 넣지 못하는 이유

```text
C 선언문:  char content[MAXLINE] = "Welcome";   ★ 초기화, OK
  의미: 스택에 MAXLINE 바이트 공간을 잡고, 맨 앞에 'W','e','l','c','o','m','e','\0' 를 복사.

C 대입문:  char content[MAXLINE];
           content = "Welcome";                ★ 컴파일 에러
  의미: "배열 이름 content" 는 "decay" 되면 주소 상수 &content[0] 인데,
        이건 modifiable lvalue 가 아님. 대입의 왼쪽에 올 수 없다.

반면 포인터:
           char *content;
           content = "Welcome";                ★ OK, "Welcome" 은 .rodata 의 주소

하지만 이 경우:
           content[0] = 'H';                   ★ 런타임 세그폴트
  의미: "Welcome" 은 .rodata 페이지 (PTE R/W=0) 라서 쓰면 #PF 후 SIGSEGV.
```

어셈블리로 보면 `=` 가 왜 배열과 포인터에서 다른지 명확하다.

```text
char content[16] = "hello";   의 어셈블리 (x86_64, GCC -O0)

  sub  $0x10, %rsp             ; 스택에 16바이트 공간
  movl $0x6C6C6568, (%rsp)     ; 'h','e','l','l'
  movl $0x006F, 4(%rsp)        ; 'o','\0'
  ; ... 나머지 0 패딩
  ; content 는 %rsp 위치 자체 (배열 실체)

char *content = "hello";  의 어셈블리

  lea  .LC0(%rip), %rax        ; .rodata 에 있는 "hello" 주소
  mov  %rax, -8(%rbp)          ; 포인터 변수(8바이트) 에 저장

content = "world"  (포인터 대입)  의 어셈블리

  lea  .LC1(%rip), %rax
  mov  %rax, -8(%rbp)          ; 그냥 포인터만 바꿈

content = "world"  (배열의 경우)  는 컴파일러가 거부:
  error: assignment to expression with array type
```

### 직접 검증 ① — CGI 응답의 Content-Length 일치

```bash
curl -v "http://127.0.0.1:8080/cgi-bin/adder?15&27" > /tmp/resp
# 로그에서 Content-length: 88 정도
wc -c /tmp/resp
# 응답 바디 크기가 정확히 헤더 값과 일치해야 함
```

### 직접 검증 ② — sprintf 대신 printf 로만 짜면 어떻게 깨지는지

```c
/* 잘못된 버전 */
printf("Content-length: ???\r\n\r\n");   // 이 시점 길이를 모름
printf("Welcome to add...\n");
printf("The answer is: %d + %d = %d\n", n1, n2, n1+n2);
/* Content-Length 가 거짓말 → HTTP/1.1 keepalive 에서 연결 오염 */
```

### 직접 검증 ③ — 문자열 대입 에러 확인

```bash
cat > badassign.c <<'EOF'
int main(void) {
    char content[10];
    content = "hello";   /* 배열 대입 */
    return 0;
}
EOF
gcc badassign.c -o /dev/null 2>&1 | head -3
# badassign.c:3:13: error: assignment to expression with array type
```

```bash
cat > rodata.c <<'EOF'
int main(void) {
    char *p = "hello";
    p[0] = 'H';          /* .rodata 쓰기 → SIGSEGV */
    return 0;
}
EOF
gcc rodata.c -o rodata && ./rodata
# Segmentation fault
```

---

## D-1 ~ D-4 통합: 프로세스 한 개가 태어나서 파일·소켓·CGI 까지 모두 다루고 종료하기까지

이 섹션이 **Part D 에서 가장 중요한 내용** 이다. 앞 네 절에서 본 개별 개념들(VFS 4객체, fdtable, fork/execve, O_CLOEXEC, struct file refcount, PTE 비트, VMA flags, demand paging, adder.c 의 sprintf) 이 **한 번의 CGI 요청 처리 중에 실제 비트와 주소 값으로 어떻게 연결되는지** 를 끝까지 따라간다. 모든 숫자는 앞에서 쓴 예시 (PID 500 bash, PID 1234 tiny, PID 1300 adder, fd 3/4) 와 동일하다.

### STEP 1. 부모 bash(PID 500) 가 준비된 순간의 스냅샷

bash 가 이미 터미널 `/dev/pts/0` 아래서 돌고 있다. tiny 를 실행하기 직전의 bash 내부.

```text
task_struct (PID=500, comm="bash")
 ├─ files ─▶ files_struct(count=1)
 │           └─ fdtable(max_fds=64)
 │              ├─ fd_array[0] ─▶ struct file (pts/0)  f_count=3
 │              ├─ fd_array[1] ─▶                  "
 │              ├─ fd_array[2] ─▶                  "
 │              ├─ open_fds     비트맵: 0b0000_0111
 │              └─ close_on_exec 비트맵: 0b0000_0000
 │
 ├─ mm ─▶ mm_struct
 │        ├─ mmap_base    = 0x7FFF_8000_0000
 │        ├─ start_brk    = 0x5555_5556_0000
 │        ├─ brk          = 0x5555_5556_D000
 │        ├─ total_vm     = 6144   (페이지 수, 24 MB)
 │        ├─ rss_stat     = 1024   (4 MB)
 │        └─ mmap list (VMA 사슬)
 │             [0x5555_5555_4000, 0x5555_5555_6000, r-xp] bash .text
 │             [0x5555_5555_6000, 0x5555_5555_7000, r--p] bash .rodata
 │             [0x5555_5555_7000, 0x5555_5555_8000, rw-p] bash .data
 │             [0x5555_5556_0000, 0x5555_5556_D000, rw-p] [heap]
 │             [0x7F12_3400_0000, 0x7F12_3420_0000, r-xp] libc.so
 │             [0x7FFD_abcd_0000, 0x7FFD_abce_0000, rw-p] [stack]
 │
 ├─ parent ─▶ task_struct (PID=120, login)
 └─ children ─▶ []  (아직 tiny 를 못 띄움)
```

`cat /proc/500/status` 로 보이는 값:

```text
Name:   bash
Pid:    500
PPid:   120
State:  S (sleeping)
VmPeak: 24576 kB
VmSize: 24576 kB
VmRSS:   4096 kB
```

### STEP 2. bash 가 fork() 를 호출하는 순간

bash 내부:

```c
pid_t pid = fork();
if (pid == 0) { execve("./tiny", argv, envp); }
```

커널 진입 — `sys_clone(flags = SIGCHLD)` (기본 fork).

커널이 새 task_struct 를 할당하고 **files_struct 를 복제** 한다. 비트 관점에서 일어나는 일:

```text
부모 fdtable (PID=500)                     자식 fdtable (PID=1234 직후, 아직 execve 전)
─────────────────────────────              ─────────────────────────────────────────
count=1                                    count=1             ← 새로 할당된 files_struct
fd_array:                                  fd_array:           ← 새로 할당된 fdtable
  [0] ─▶ file_pts0 (f_count+1 → 4)          [0] ─▶ file_pts0   같은 struct file
  [1] ─▶ file_pts0 (f_count+1 → 5)          [1] ─▶ file_pts0
  [2] ─▶ file_pts0 (f_count+1 → 6)          [2] ─▶ file_pts0
open_fds      = 0b...0111                  open_fds      = 0b...0111
close_on_exec = 0b...0000                  close_on_exec = 0b...0000


부모 mm_struct (PID=500)                   자식 mm_struct (PID=1234 직후)
─────────────────────────                  ───────────────────────────────
  같은 VMA 리스트 복제 (각 VMA struct 새로)
  모든 PTE 를 "쓰기 불가" 로 표시 (CoW)     PTE 도 복사 (PFN 은 같은 물리 프레임)
                                            쓰기 발생 시 #PF → 페이지 복제
```

이 시점 커널은 자식의 PID 를 **부모 반환 값** 으로, **0** 을 자식 반환 값으로 돌려준다. 자식은 `if (pid == 0)` 로 분기해서 바로 execve.

### STEP 3. 자식이 execve("./tiny", ..., envp) 를 호출하는 순간

이 한 콜이 **엄청난 일** 을 한다. **O_CLOEXEC 비트 청소 + VM 완전 교체 + fdtable 유지** 가 전부 한 번에 일어난다.

```text
execve 이전 자식 (PID=1234) 상태:
  comm = "bash"
  fdtable = (STEP 2 와 동일)
  mm      = bash 의 VMA 복제본
  VmSize  = 24 MB

커널 do_execveat_common 흐름:
  1) ELF 헤더 검사 — "./tiny" 가 ELF64
  2) bprm_mm_init — 새 mm_struct 준비
  3) 기존 mm 내려놓기 (refcount -1, CoW PTE 들 자식 전용이면 해제)
  4) 새 mm 에 tiny 의 .text/.rodata/.data VMA 매핑
  5) stack VMA 생성 (argv, envp 쌓음)
  6) **fdtable 의 close_on_exec 비트맵 스캔**:
       for (fd=0; fd<max_fds; fd++)
         if (close_on_exec & (1<<fd))  close(fd)
     이 시점엔 아직 아무것도 켜져 있지 않음 (bash 가 켠 건 없음)
     → 0/1/2 모두 살아남음
  7) entry point 로 점프 (tiny 의 _start)

execve 이후 자식 상태:
  comm = "tiny"
  PID  = 1234  (유지, 새 PID 아님!)
  fdtable:
    [0] ─▶ file_pts0
    [1] ─▶ file_pts0    ← 그대로! 그래서 printf 가 터미널에 찍힌다
    [2] ─▶ file_pts0
  mm: tiny 의 ELF 에 따라 완전히 새 VMA 들
  VmSize  = 2 MB       (tiny 는 작으니까)
```

### STEP 4. tiny 가 listening socket(fd=3) 을 연다

tiny main() 에서 (CSAPP Tiny 기준):

```c
int listenfd = Open_listenfd(8080);
```

내부 구현은 `socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)` + `bind` + `listen`.

커널에서 일어나는 일:

```text
1) alloc_fd():
   fdtable->open_fds 비트맵 스캔 → bit 3 이 가장 낮은 0 자리
   open_fds   = 0b0000_1111
   close_on_exec 도 SOCK_CLOEXEC 이므로 bit 3 on
   close_on_exec = 0b0000_1000

2) struct file 할당 (slab "filp"):
   f_op      = socket_file_ops
   f_flags   = O_RDWR | O_NONBLOCK | O_CLOEXEC
             = 0x2 | 0x800 | 0x80000
             = 0x80802
   f_count   = 1
   private_data = &struct socket

3) struct socket + struct sock 할당:
   socket.type = SOCK_STREAM
   socket.ops  = inet_stream_ops
   sock.sk_state = TCP_CLOSE (지금)

4) bind(3, {AF_INET, 0.0.0.0:8080}):
   sock 의 src_addr 세팅, port 8080 해시 테이블 등록

5) listen(3, backlog=10):
   sock.sk_state = TCP_LISTEN
   accept queue, SYN queue 초기화

fdtable 최종 상태:
  [0] ─▶ pts/0
  [1] ─▶ pts/0
  [2] ─▶ pts/0
  [3] ─▶ struct file (listen sock)   ★ O_CLOEXEC on
  open_fds      = 0b0000_1111
  close_on_exec = 0b0000_1000
                        │
                        └─ listenfd 는 자식 CGI 에 누출되면 안 됨
```

### STEP 5. accept() 로 connfd=4 가 태어난다

클라이언트의 SYN 이 도착해 3-way handshake 가 끝나면 accept queue 에 새 sock 이 쌓인다. tiny 는 루프에서:

```c
int connfd = accept(listenfd, NULL, NULL);
```

를 호출.

```text
커널 sys_accept4:
  1) listenfd 의 sock 에서 accept queue pop → 이미 ESTABLISHED 된 새 sock
  2) alloc_fd() → bit 4 할당, 새 struct file 생성
     f_flags = O_RDWR  (기본)
     close_on_exec 비트 4 = 0   ★ 기본으로 전파 안 함
  3) connfd=4 유저 반환

fdtable:
  [0..2] 유지
  [3] listen sock (여전히 살아있음)
  [4] connected sock  ★ 새로 태어남
  open_fds      = 0b0001_1111
  close_on_exec = 0b0000_1000
```

### STEP 6. GET /cgi-bin/adder?15&27 → fork + dup2(4, 1) + execve("adder", ..., envp)

tiny 의 `serve_dynamic` 내부에서 일어나는 일. 비트 관점에서 **가장 중요한 순간** 이다.

```c
if (Fork() == 0) {
    setenv("QUERY_STRING", "15&27", 1);
    setenv("CONTENT_LENGTH", "0", 1);
    setenv("REQUEST_METHOD", "GET", 1);
    Dup2(fd, STDOUT_FILENO);      // fd = connfd = 4
    Execve(filename, emptylist, environ);
}
Wait(NULL);
```

#### 6-1. Fork() — tiny(1234) 가 adder(1300) 를 낳는다

```text
adder 자식(PID=1300) 초기 상태 (execve 전):

  fdtable (자식용으로 새로 할당):
    [0] ─▶ pts/0
    [1] ─▶ pts/0          ← 아직 dup2 안 함
    [2] ─▶ pts/0
    [3] ─▶ listen sock    ★ 복제됨
    [4] ─▶ connected sock ★ 복제됨
    open_fds      = 0b0001_1111
    close_on_exec = 0b0000_1000   ★ bit 3 (listen) 만 CLOEXEC
                                   bit 4 (conn) 는 꺼져있음

  각 struct file 의 f_count 는 +1
    pts/0 f_count:       3 → 6    (부모 tiny 가 3개 가지고 있었으므로 +3 → 6)
    listen sock f_count: 1 → 2
    conn sock   f_count: 1 → 2
```

#### 6-2. Dup2(fd=4, 1) — 자식의 stdout 을 connfd 로 리다이렉트

이 한 줄이 **CGI 전체 설계의 핵심** 이다. dup2 는 fdtable 을 물리적으로 바꾼다.

```text
dup2(4, 1) 직전 자식 fdtable:
  [1] ─▶ pts/0 (f_count=6)
  [4] ─▶ conn_sock (f_count=2)

커널 sys_dup3 (dup2 래퍼):
  a) old_file = fd_array[4]            ; conn_sock
  b) new_file = fd_array[1]            ; pts/0
  c) fd_array[1] = old_file            ; [1] = conn_sock
     f_count++  → conn_sock f_count = 3
  d) fput(new_file)                    ; pts/0 f_count = 5
  e) close_on_exec bit 1 = 0           ; dup2 는 CLOEXEC 기본 꺼줌

dup2(4, 1) 직후 자식 fdtable:
  [0] ─▶ pts/0          (f_count=5)
  [1] ─▶ conn_sock      ★ stdout 이 소켓이 됨 (f_count=3)
  [2] ─▶ pts/0
  [3] ─▶ listen sock    (CLOEXEC 켜짐)
  [4] ─▶ conn_sock      (복사본)
  close_on_exec = 0b0000_1000   ★ bit 1 꺼져 있음
```

만약 `Close(fd)` (==`close(4)`) 를 dup2 뒤에 호출하면:

```text
close(4) 후:
  [4] = NULL, f_count-- → conn_sock f_count = 2
  open_fds bit 4 = 0
  하지만 [1] 이 여전히 가리키므로 sock 은 살아있음.
```

#### 6-3. Execve("/cgi-bin/adder", argv, envp) — adder 의 VM 이 탄생

```text
execve 시 커널이 하는 일:

  A) close_on_exec 스캔:
     bit 3 (listen sock) = 1 → close(3)
       listen_sock f_count: 2 → 1   ★ 부모 tiny 에서만 살아있음
       open_fds bit 3 = 0

     bit 1 (stdout) = 0 → 유지        ★ 여기가 결정적!
     bit 4 (conn original) = 0 → 유지
     → 자식 adder 의 fdtable:
        [0] ─▶ pts/0     (1)
        [1] ─▶ conn_sock (stdout!)
        [2] ─▶ pts/0     (stderr)
        [4] ─▶ conn_sock (tiny 에서도 갖고 있음, 자식 adder 는 안 씀)

  B) mm_struct 완전 교체:
     기존 tiny-복제 VMA 전부 해제, tiny 메모리 이미지 버림
     adder 바이너리 ELF 파싱해서 새 VMA 생성:

        VMA list of adder (PID=1300):
        ────────────────────────────────────
        0x4000_0000_4000 .text       (VM_READ|VM_EXEC|VM_MAYREAD|VM_MAYEXEC)
                                      flags 비트 = 0x1 | 0x4 | 0x10 | 0x40
                                                 = 0x55
        0x4000_0020_0000 .rodata     (VM_READ|VM_MAYREAD)
                                      "Welcome to add.com: "  ← STEP 7 에서 쓰임
        0x4000_0021_0000 .data/.bss  (VM_READ|VM_WRITE|VM_MAYREAD|VM_MAYWRITE)
        0x4000_0022_0000 [heap]      (처음엔 빈 VMA)
        0x7F12_3400_0000 libc.so     (공유)
        0x7FFD_1234_0000 [stack]     (VM_GROWSDOWN|VM_READ|VM_WRITE|VM_MAYREAD|VM_MAYWRITE)
                                      flags 비트 = 0x100 | 0x1 | 0x2 | 0x10 | 0x20
                                                 = 0x133

     PTE 는 전부 Present=0 (demand paging).
     VSZ 는 약 2 MB, RSS 는 거의 0.

  C) 스택 셋업:
     argv = ["adder"]  (Tiny 는 empty argv 사용)
     envp = ["QUERY_STRING=15&27", "CONTENT_LENGTH=0", "REQUEST_METHOD=GET", NULL]

     envp 가 스택 상단에 쌓이는 모습:
     ┌─────────────────────────────┐   0x7FFD_1234_FFF0
     │ "QUERY_STRING=15&27\0"       │  (rodata 아님, execve 가 복사해준 스택)
     ├─────────────────────────────┤
     │ "CONTENT_LENGTH=0\0"         │
     ├─────────────────────────────┤
     │ "REQUEST_METHOD=GET\0"       │
     ├─────────────────────────────┤
     │ envp[3] = NULL               │
     │ envp[2] = ptr → "REQUEST_..."│
     │ envp[1] = ptr → "CONTENT_..."│
     │ envp[0] = ptr → "QUERY_..."  │  ← %rsi 가 이걸 가리킴
     └─────────────────────────────┘

  D) entry point 로 점프 (adder 의 _start)
```

### STEP 7. adder 가 sprintf 로 content 를 만들고 printf 로 출력하는 순간

D-4 에서 설명한 바로 그 루틴이 **지금 PID 1300 의 스택 위에서** 돌아간다.

```text
adder 의 스택 프레임 (sprintf 직후)

  0x7FFD_1234_E000  ─ 스택 top ─────────────
  ...
  0x7FFD_1234_DB00  main() frame:
                    char arg1[100]       offset -0x150
                    char arg2[100]       offset -0xE0
                    char content[100]    offset -0x70
                      [0..19]  = 'W','e','l','c','o','m','e',' ',
                                 't','o',' ','a','d','d','.','c',
                                 'o','m',':',' '
                      [20..51] = "THE Internet addition portal.\r\n<p>"
                      [52..87] = "The answer is: 15 + 27 = 42\r\n<p>"
                      [88]     = '\0'
                    int n1 = 15        offset -0x4
                    int n2 = 27        offset -0x8

  이 content 의 처음 20바이트는 .rodata 에서 복사됐다:
    sprintf(content, "Welcome to add.com: ");
      내부에서 libc 의 memcpy 가
        src = 0x4000_0020_0008 (.rodata 안의 "Welcome..." 주소)
        dst = 0x7FFD_1234_DB90 (스택 위 content[0])
        n   = 20
```

첫 접근 때 **stack 페이지가 Present=0 인 경우**:

```text
*(content+0) = 'W'  쓰기
  MMU PTE:  Present=0  →  #PF
  커널:
    do_page_fault(addr=0x7FFD_1234_DB90, err=write+user+not_present)
    find_vma → VM_GROWSDOWN 있는 stack VMA
    expand_downwards ok
    alloc_page → PFN = 0x12345
    PTE 세팅:  PFN<<12 | P | R/W | U/S | A
               = 0x12345000 | 0x1 | 0x2 | 0x4 | 0x20
               = 0x12345027
    TLB 무효화 후 재실행
  쓰기 성공, PTE.D = 1 (하드웨어 갱신)
  RSS += 4KB
```

이제 printf 가 호출된다.

```c
printf("Content-length: %d\r\n", (int)strlen(content));
```

libc 내부:

```text
1) vsnprintf 로 포맷 전개 → stdout (FILE *) 의 buffer 에 채움
   stdout->_IO_buf_base  = 힙 어딘가 (0x4000_0022_0100)
   stdout->_IO_write_base= 위
   stdout->_IO_write_ptr = 채운 만큼 전진

2) stdout 은 line buffered 인가 fully buffered 인가?
   → CGI 환경에서 stdout 은 **소켓** (isatty 가 아님)
     libc 는 기본 fully buffered 로 둠 (잘못된 경우가 있어
     CGI 코드는 보통 마지막에 fflush(stdout))

3) fflush → write(1, buf, n)
   system call 번호 = 1
   fd=1 (자식 adder 의 dup2 로 conn_sock 에 연결)
   → sock_write_iter → tcp_sendmsg
   → sk_write_queue 에 skb 쌓음
   → dev_queue_xmit → NIC ...
   (Part A 의 write 경로 그대로)
```

클라이언트는 이 write 의 바이트 스트림을 SYN/SYN+ACK/ACK 후의 **데이터 세그먼트** 로 받는다.

### STEP 8. adder 종료 → tiny 가 wait → conn_sock refcount 0

```text
adder 의 exit(0):
  커널이 모든 VMA 해제, fdtable 해제
  close_all_files() 호출:
    fd_array[0] pts/0  f_count-- (5→4)
    fd_array[1] conn_sock f_count-- (3→2)
    fd_array[2] pts/0  f_count-- (4→3)
    fd_array[4] conn_sock f_count-- (2→1)
  (listenfd 는 execve 때 닫혔으므로 이미 없음)

  adder 가 zombie 상태 진입 (exit_code 저장)

tiny 의 Wait(NULL):
  waitpid 가 zombie 를 거둠
  zombie 의 task_struct 해제
  SIGCHLD 처리

tiny 가 Close(connfd=4):
  fd_array[4] = NULL, f_count-- (1→0)
  __fput(conn_sock):
    sock->ops->release = inet_release
      → tcp_close
        → send queue flush
        → FIN 세그먼트 생성 및 전송
        → sk_state: ESTABLISHED → FIN_WAIT_1
```

### STEP 9. TCP close 의 뒷처리 (TIME_WAIT 60초)

fd 는 이미 없지만 struct sock 은 살아있다.

```text
커널 TCP state machine (이 소켓 전용)

시간 0s:     FIN 전송 (TCP flags = FIN|ACK = 0x11)
             tiny 의 sk_state = FIN_WAIT_1

시간 0.01s:  상대(클라이언트) 의 ACK 수신
             sk_state = FIN_WAIT_2

시간 0.02s:  상대의 FIN 수신
             ACK 전송
             sk_state = TIME_WAIT   ★
             struct tcp_timewait_sock 으로 전환
               (더 가벼운 구조, 메모리 절약)

시간 60s:    2*MSL 만료
             timewait sock 해제, 포트 번호 재사용 가능

ss -tn state time-wait
  TIME-WAIT 0 0 [server:8080] [client:51234]
```

### STEP 10. tiny 가 다음 accept() 로 돌아가기 직전

```text
tiny (PID=1234) 의 fdtable 최종 상태:
  [0] ─▶ pts/0
  [1] ─▶ pts/0
  [2] ─▶ pts/0
  [3] ─▶ listen sock  (여전히 살아있음, f_count=1)
  [4] 없음             (방금 close)
  open_fds      = 0b0000_1111
  close_on_exec = 0b0000_1000

tiny 의 mm:
  STEP 1 에서 tiny 를 막 띄웠을 때와 거의 동일.
  CGI 요청 처리 중 heap/stack 쓴 만큼 RSS 는 소폭 증가.

루프 복귀:
  while(1) {
    connfd = accept(listenfd, ...);    ← 다음 요청
    if (Fork() == 0) ...               ← 다시 STEP 6 부터
  }
```

### 위 10 STEP 을 하나의 timeline 으로

```text
시간 → → → → → → → → → → → → → → → → → → → → → → → → → → → → → → → → →

bash(500)
  │  fork ─┐
  │        ▼
  │      child(1234, was bash)
  │        │  execve("./tiny")
  │        │   - close_on_exec 스캔 (지금은 다 0)
  │        │   - mm 교체
  │        │
  │      tiny(1234)
  │        │  socket() → fd=3, O_CLOEXEC on
  │        │  bind/listen
  │        │  accept() → fd=4
  │        │
  │        │  요청 도착 "GET /cgi-bin/adder?15&27"
  │        │
  │        │  fork ─┐
  │        │        ▼
  │        │      child(1300, was tiny)
  │        │        │  dup2(4, 1)    ★ stdout=소켓
  │        │        │  execve("adder")
  │        │        │   - close_on_exec: bit 3 on → close listen fd
  │        │        │   - mm 교체: tiny VM 전부 폐기
  │        │        │   - envp=QUERY_STRING=15&27 등 스택에 복사
  │        │        │
  │        │      adder(1300)
  │        │        │  QUERY_STRING 파싱 (getenv)
  │        │        │  sprintf(content, ...)  ★ 스택 page fault 발생
  │        │        │  printf("Content-length: ...")
  │        │        │  printf("%s", content)
  │        │        │  fflush → write(1, ...) → tcp_sendmsg
  │        │        │                            ↓ Part A 의 skb/IP/Eth
  │        │        │  exit(0)
  │        │        ▼ (zombie)
  │        │  wait()  → reap, conn_sock f_count--
  │        │  close(4)  → f_count → 0 → __fput
  │        │                            → tcp_close → FIN
  │        │                                sk_state: ESTABLISHED → FIN_WAIT_1
  │        │                                         → FIN_WAIT_2 → TIME_WAIT
  │        │                                         → 60s 뒤 해제
  │        │  accept() 다시 대기
  ▼        ▼
```

### 이 10 STEP 에서 답하는 원 질문 매핑

```text
D-1  VFS 4객체 + FDT + 0/1/2 + dup2 + RIO + 터미널 vs GUI
     → STEP 1, 4, 6-2 에서 실제 fdtable/struct file/close_on_exec 비트로 답함

D-2  PID 1, fork 상속, O_CLOEXEC, refcount, TCP vs UDP close, 공유 fd
     → STEP 2, 3, 6-1, 6-3 에서 fdtable 복제와 close_on_exec 비트
     → STEP 8, 9 에서 TCP close 의 이중성 (fd 해제 vs FIN/TIME_WAIT)

D-3  libc 위치, VM 방향, heap vs mmap, demand paging, VSZ/RSS, OOM
     → STEP 6-3 에서 execve 후 VMA 리스트
     → STEP 7 에서 sprintf 의 stack page fault 로 실제 물리 프레임 할당

D-4  adder.c 의 sprintf/printf 순서와 content 대입 문제
     → STEP 7 에서 스택 위 content[100] 의 바이트 덤프와
       .rodata 에서 복사되는 순간까지
```

## 전체 검증 명령 모음

```bash
# 프로세스 족보
pstree -p $(pgrep systemd | head -1)
ps --forest -eo pid,ppid,comm,stat

# fd 매핑
pid=$(pgrep -f 'tiny 8080')
ls -l /proc/$pid/fd
cat /proc/$pid/fdinfo/3         # pos, flags (8진수), mnt_id

# fd 플래그 비트 (실제 값)
grep flags /proc/$pid/fdinfo/3
# flags: 02000002   ← O_CLOEXEC (02000000) | O_RDWR (2)

# VM 레이아웃
cat /proc/$pid/maps
head -30 /proc/$pid/smaps        # VMA 별 Rss, Pss, Anonymous
grep -E 'Vm|Threads' /proc/$pid/status

# page fault 카운트
awk '{print "minflt="$10, "majflt="$12}' /proc/$pid/stat

# execve 관찰 (fork + dup2 + execve 전부)
strace -f -e trace=clone,execve,dup2,dup3,close,openat -p $pid

# CGI 요청 한 번 돌리면서 O_CLOEXEC 동작 관찰
strace -f -e trace=execve,close -o cgi.trace ./tiny 8080 &
curl "http://127.0.0.1:8080/cgi-bin/adder?1&2"
grep 'close(3)' cgi.trace         # 자식 execve 직후 listenfd 가 닫히는 라인이 보임

# 메모리 overcommit 상태
cat /proc/sys/vm/overcommit_memory
grep -E 'MemTotal|MemAvailable|CommitLimit|Committed_AS' /proc/meminfo

# TCP close 후 TIME_WAIT 꼬리
ss -tn state time-wait
ss -tni                           # RTT, cwnd 까지 전부

# OOM 이력
dmesg -T | grep -i 'killed process\|oom'

# VFS 분기 확인 — 같은 read 가 다른 f_op 로
cat /proc/self/status             # procfs
cat /etc/hostname                 # ext4
cat /sys/kernel/ostype            # sysfs

# ext4 실제 레이아웃
dumpe2fs -h /dev/sda1 | head -30  # superblock, group 수, inode 수
tune2fs -l /dev/sda1              # feature flags

# bpftrace 로 page fault 실시간
sudo bpftrace -e 'tracepoint:exceptions:page_fault_user { @[pid, comm] = count(); }'
```

## 연결 문서

- [q04-filesystem.md](./q04-filesystem.md) — 파일시스템 · VFS 4객체 · ext4 레이아웃
- [q20-fd-lifecycle-and-dispatch.md](./q20-fd-lifecycle-and-dispatch.md) — fd 수명과 f_op 분기
- [q19-process-ancestry-fd-inheritance.md](./q19-process-ancestry-fd-inheritance.md) — 프로세스 족보 · fd 상속 · reparent
- [q21-process-parent-and-memory-deep-dive.md](./q21-process-parent-and-memory-deep-dive.md) — VM 레이아웃 · heap/mmap/demand paging
- [q13-cgi-fork-args.md](./q13-cgi-fork-args.md) — CGI · fork · dup2 · envp
- [part-a-whiteboard-topdown.md](./part-a-whiteboard-topdown.md) — STEP 7 의 write 뒤 네트워크 전체
- [part-b-whiteboard-topdown.md](./part-b-whiteboard-topdown.md) — 연결이 열리기 전 주소/DNS/3-way
- [part-c-whiteboard-topdown.md](./part-c-whiteboard-topdown.md) — 서버 측 요청 전체 처리와 동시성
