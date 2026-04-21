# q20. fd 수명과 디스패치 — close / CLOEXEC / proto 콜백 / 공유 / 버퍼링까지

## 질문

```
 1.  close(fd) 하면 부모/자식이 공유하던 struct file 은 즉시 사라지나? refcount 는 언제 0 이 되나?
 2.  O_CLOEXEC 는 언제, 누구에게 작용하나? 자식이 fd 에 접근하는 순간 닫히는가?
 3.  파일 fd 와 소켓 fd 가 "완전히 똑같은 규칙" 으로 작동한다는 건 어디까지 사실인가?
 4.  소켓 하나의 close() 가 TCP 는 60초 FIN/ACK, UDP 는 즉시 끝 — 같은 함수가 어떻게 다르게 동작하나?
 5.  fork 로 복제된 fd 를 부모/자식이 동시에 read/write 하면 무슨 일이 일어나나?
 6.  fd 로 연결할 수 있는 객체는 뭐가 있나? 전부 나열하고 언제 쓰는지까지 알고 싶다.
 7.  stdin/stdout/stderr 가 "꼭 0/1/2 여야" 하는 이유가 있나? 커널이 fd 번호를 구분하나?
 8.  3 번 fd 에 열린 소켓을 0/1 번에 꽂으려면 어떻게 해야 하나?
 9.  "unbuffered" "line-buffered" "flush" 가 무슨 소리인가? 커널 버퍼? libc 버퍼?
```

## 한 줄 답

```
 (1) close 는 fdtable 슬롯만 즉시 비우고, struct file 은 refcount -- 후 0 이 되면 그제야 해제.
 (2) CLOEXEC 는 execve() 경계에서만 작동. 자식이 exec 하는 순간 해당 fd 만 닫힘 (부모는 그대로).
 (3) "everything is a file" 은 "everything dispatches through file->f_op" 의 의미. 규칙은 같고 구현이 다르다.
 (4) close 의 본체는 file->f_op->release 콜백 디스패치. 소켓은 sock->sk_prot->close (strategy pattern).
 (5) 부모/자식은 같은 struct file 을 공유 → 같은 f_pos, 같은 수신 큐. read 는 경쟁적 소비, write 는 경쟁적 출력.
 (6) 파일/디렉토리/파이프/FIFO/소켓/TTY/디바이스/eventfd/timerfd/signalfd/inotify/epoll/io_uring/pidfd/...
 (7) 커널은 fd 번호를 구분하지 않는다. 0/1/2 는 libc + 셸의 약속 (convention). dup2 로 자유롭게 바꿀 수 있다.
 (8) dup2(src, 0) / dup2(src, 1). 셸의 < > , inetd, CGI 가 전부 이걸로 스테이지 연출.
 (9) libc stdio 가 프로세스 내부에 유지하는 2-level 버퍼. flush = libc 버퍼 → 커널 버퍼. fsync = 커널 → 디스크.
```

## 목차

```
 §1.  close(fd) 의 refcount 메커니즘 — 슬롯과 struct file 의 분리된 수명
 §2.  O_CLOEXEC 와 execve 경계 — 누구에게 언제 닫히는가
 §3.  파일과 소켓 — "똑같다" 는 말의 정확한 의미
 §4.  TCP 와 UDP 의 close — struct proto 와 strategy pattern
 §5.  공유된 fd 의 동시 사용 — 같은 f_pos, 같은 수신 큐
 §6.  fd 로 접근 가능한 객체 총정리 — 11 개 카테고리
 §7.  stdin/stdout/stderr — 커널이 구분하지 않는 "약속"
 §8.  dup2 리다이렉트 — 셸/CGI/inetd 가 쓰는 패턴
 §9.  libc stdio 버퍼링 — unbuffered / line / block / fflush / fsync
 §10. 한 번에 보는 "fd 연산 다이어그램"
```

---

## §1. close(fd) 의 refcount 메커니즘

**핵심 오해 정정.** `close(fd)` 는 "객체를 죽이는 연산" 이 아니라 **"내가 가진 참조 하나를 버리는 연산"** 이다. 객체(struct file)가 실제로 해제되는 건 마지막 참조가 사라질 때다.

### 두 층의 수명

```
 층 A: fdtable 슬롯              (task 당 하나씩, 가벼움)
 층 B: struct file               (전역, 무거움 — inode/socket/page cache 연결)
 층 C: struct inode / sock       (파일/소켓 자체)
```

`close(3)` 이 하는 일은 **층 A 의 슬롯을 비우고, 층 B 의 참조 카운트를 하나 감소시키는 것**. 층 B 가 즉시 해제될지는 **다른 참조가 남아 있는가** 에 달렸다.

### fork 후 close 의 실제 흐름

```
 [1] fork 직전
     Parent P (PID 100)                  struct file                struct inode
     fdtable                             ─────────────              ────────
     [0]→tty  [1]→tty  [2]→tty           "a.txt"                    (inode "a.txt")
     [3]→file──────────────────────────→ refcount=1 ─────────────→  ref=1

 [2] fork() 직후 — fdtable 복제, 각 슬롯이 동일 struct file 을 가리킴 (refcount++)
     Parent P (PID 100)
     [3]→file┐
              ├─────────────────────────→ refcount=2
     Child  C (PID 200)                                              (inode)
     [3]→file┘

 [3] Child 가 close(3) 호출
     Parent P
     [3]→file ─────────────────────────→ refcount=1
     Child  C
     [3]=NULL                            (struct file 은 그대로 살아있음)

 [4] Parent 도 close(3) 호출 → refcount 0 → struct file 해제 → inode 참조 하나 감소
     Parent P
     [3]=NULL
     Child  C
     [3]=NULL
                                         (struct file 해제됨)
```

### 커널 코드 — `close_fd()`

```c
// fs/file.c  (단순화)
int close_fd(unsigned fd)
{
    struct files_struct *files = current->files;
    struct file *file;
    struct fdtable *fdt;

    spin_lock(&files->file_lock);
    fdt = files_fdtable(files);
    if (fd >= fdt->max_fds) { spin_unlock(&files->file_lock); return -EBADF; }

    file = fdt->fd[fd];
    if (!file)            { spin_unlock(&files->file_lock); return -EBADF; }

    // 1) 슬롯 비우기 (RCU 퍼블리시)
    rcu_assign_pointer(fdt->fd[fd], NULL);
    __clear_bit(fd, fdt->open_fds);
    __clear_bit(fd, fdt->close_on_exec);
    spin_unlock(&files->file_lock);

    // 2) struct file 의 refcount 감소. 0 이 되면 __fput() 로 실제 해제
    return filp_close(file, files);
}
```

`filp_close` → `fput` → refcount 0 이면 `__fput` → `file->f_op->release(inode, file)` 콜백.

```c
// fs/file_table.c  (단순화)
void fput(struct file *file)
{
    if (atomic_long_dec_and_test(&file->f_count))
        // 비동기로 __fput 큐에 넣음 (인터럽트 컨텍스트 안전)
        __fput_deferred(file);
}
```

**정리: 두 개의 독립 수명**

```
 ┌─────────────────────────────────────────────────────────────┐
 │ close(fd) 가 즉시 하는 일                                    │
 │   • fdtable[fd] = NULL  (이 프로세스만)                      │
 │   • open_fds, close_on_exec 비트 클리어                     │
 │   • struct file 의 refcount--                               │
 │                                                              │
 │ close(fd) 가 조건부로 하는 일 (refcount == 0 일 때만)        │
 │   • file->f_op->release 콜백 호출                            │
 │   • inode 참조 감소 (inode 도 refcount 기반)                  │
 │   • struct file 메모리 해제                                   │
 └─────────────────────────────────────────────────────────────┘
```

---

## §2. O_CLOEXEC 와 execve 경계

**흔한 오해.** "O_CLOEXEC 로 열면 자식이 그 fd 에 접근할 때 커널이 막아준다."

**사실.** CLOEXEC 는 오직 **execve() 경계에서만** 작동한다. fork 후 자식이 그냥 read/write 하면 다 된다. execve 호출 순간에만 커널이 close_on_exec 비트가 켜진 fd 를 전부 닫는다.

### 타이밍

```
 Parent: open("a.txt", O_RDONLY | O_CLOEXEC)   → fd=3, close_on_exec bit = 1
 Parent: fork()
   ├─ Parent: 계속 실행. fd 3 그대로 사용 가능
   └─ Child : fdtable 복제됨. fd 3, close_on_exec=1 도 같이 복사됨
              Child 가 fd 3 에 read() 하면? → 된다! 아직 exec 안 했으니까
              Child 가 execve("/bin/ls") 호출 → 바로 이 지점에서 fd 3 닫힘
                                                (ls 프로세스는 fd 3 이 없는 상태로 시작)
              Parent 의 fd 3 은 영향 없음
```

### 플래그 비트맵 구조

```
 open("a.txt", O_RDONLY | O_CLOEXEC) 의 2 비트 의미
 ───────────────────────────────────────────────
 O_RDONLY  — 접근 모드 (0, 1, 2 중 하나)
 O_CLOEXEC — struct file 에 FD_CLOEXEC 표시

 bit             의미             저장 위치
 ─────────────── ─────────────── ─────────────────────────────
 O_RDONLY/WR/RW  접근 모드        struct file → f_mode (FMODE_READ / FMODE_WRITE)
 O_APPEND        항상 끝에 쓰기   struct file → f_flags
 O_NONBLOCK      블로킹 안 함     struct file → f_flags
 O_CLOEXEC       exec 시 닫기     fdtable 의 close_on_exec bitmap (file 이 아닌 fd 속성)
```

주의: CLOEXEC 는 **fd 의 속성** 이지 **struct file 의 속성** 이 아니다. 그래서 같은 struct file 을 두 fd 가 공유할 때 한쪽은 CLOEXEC, 한쪽은 아님이 가능하다.

### 커널 코드 — execve 시 CLOEXEC 처리

```c
// fs/exec.c 에서 exec 진행 중
static int do_execveat_common(...)
{
    ...
    // 새 파일 이미지 로드 전/중간에 호출됨
    flush_old_exec(bprm);

    // 여기서 close_on_exec 비트가 켜진 fd 를 전부 닫음
    do_close_on_exec(current->files);
    ...
}

// fs/file.c
void do_close_on_exec(struct files_struct *files)
{
    unsigned i;
    struct fdtable *fdt = files_fdtable(files);

    for (i = 0; ; i++) {
        unsigned long set = fdt->close_on_exec[i];
        while (set) {
            unsigned fd = __ffs(set) + i * BITS_PER_LONG;
            struct file *file = fdt->fd[fd];
            rcu_assign_pointer(fdt->fd[fd], NULL);
            __clear_bit(fd, fdt->open_fds);
            __clear_bit(fd, fdt->close_on_exec);
            filp_close(file, files);
            set &= set - 1;
        }
    }
}
```

### "자식에게 넘겨주지 말자" 용도 — 보안

`O_CLOEXEC` 의 대표 용도는 **유출 방지**. fork+exec 로 외부 프로그램을 실행할 때 내 비밀 파일 fd 가 딸려 들어가지 않게 한다.

```
 시나리오: 웹 서버가 /etc/server.key 를 열어서 TLS 에 쓰는 중
          동시에 CGI 스크립트를 fork+exec 으로 실행
          O_CLOEXEC 없으면 CGI 가 server.key fd 를 상속받아서 읽을 수 있음

 해결:    open("/etc/server.key", O_RDONLY | O_CLOEXEC)
          → CGI (execve 된 프로세스) 에서는 자동으로 닫힘
          → 웹 서버 본체는 계속 사용 가능
```

glibc 2.7+ 에서는 `fopen(..., "re")` 의 `e` 가 O_CLOEXEC. 최근 코드는 기본으로 CLOEXEC 켜는 게 권장 사항.

---

## §3. 파일과 소켓 — "똑같다" 의 정확한 의미

유닉스의 "everything is a file" 은 **의미론적 인터페이스가 같다** 는 뜻이다. 구체적으로:

```
 read(fd, buf, n)    // 파일 / 소켓 / 파이프 / tty / 디바이스 전부 동일한 시그니처
 write(fd, buf, n)
 close(fd)
 fcntl(fd, ...)
 poll([...])
```

이게 가능한 이유는 커널 안에서 **모든 fd 가 `struct file` 을 거치고, 그 안의 `f_op` 함수 포인터 테이블로 실제 구현이 분기** 되기 때문이다.

### 공통 관문: struct file

```c
// include/linux/fs.h  (요약)
struct file {
    struct path            f_path;
    struct inode          *f_inode;
    const struct file_operations *f_op;   // ★ 함수 포인터 테이블
    atomic_long_t          f_count;        // refcount
    unsigned int           f_flags;
    fmode_t                f_mode;
    loff_t                 f_pos;
    void                  *private_data;  // 하위 구현이 쓰는 컨텍스트
    ...
};

struct file_operations {
    ssize_t (*read)     (struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write)    (struct file *, const char __user *, size_t, loff_t *);
    int     (*release)  (struct inode *, struct file *);
    unsigned int (*poll)(struct file *, struct poll_table_struct *);
    long    (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
    int     (*fsync)    (struct file *, loff_t, loff_t, int);
    int     (*mmap)     (struct file *, struct vm_area_struct *);
    ...
};
```

### 각 fd 타입별 f_op 매핑

```
 fd 타입         f_op                     read 구현              close 체인
 ──────────     ──────────────────      ──────────────────     ──────────────────────────
 regular file   ext4_file_operations    ext4_file_read_iter    ext4_release_file
 directory      ext4_dir_operations     generic_read_dir       ext4_release_file
 pipe           pipefifo_fops           pipe_read              pipe_release
 socket         socket_file_ops         sock_read_iter         sock_close
 tty            tty_fops                tty_read               tty_release
 char device    드라이버가 제공         (드라이버별)            (드라이버별)
 block device   def_blk_fops            blkdev_read_iter       blkdev_close
 eventfd        eventfd_fops            eventfd_read           eventfd_release
 epoll          eventpoll_fops          (read 없음)            ep_eventpoll_release
```

### 포인트

```
 ┌─────────────────────────────────────────────────────────────┐
 │ "파일과 소켓이 같다" = 시스템 콜 → struct file 까지는 같다    │
 │ "다르다"            = file->f_op 가 가리키는 구현이 다르다    │
 │                                                              │
 │ 시스템 콜 sys_read 는 fd 가 무엇이든 절대 구현을 몰라야 한다. │
 │ 그게 파일 계층(VFS)의 핵심 설계 원칙.                         │
 └─────────────────────────────────────────────────────────────┘
```

---

## §4. TCP 와 UDP 의 close — struct proto 와 strategy pattern

같은 `close(fd)` 인데 TCP 소켓은 FIN/ACK/TIME_WAIT 60초, UDP 소켓은 "그냥 해제" 로 끝난다. 어떻게 한 함수가 이렇게 다르게 동작하나?

답: **소켓 close 는 4 단계의 함수 포인터 디스패치**.

### close 디스패치 체인

```
 close(fd)  [syscall]
   │
   ▼
 filp_close → fput → __fput → file->f_op->release(inode, file)
   │                           ↓
   │                        socket_file_ops 의 release = sock_close
   ▼
 sock_close                   // net/socket.c
   │
   ▼
 sock->ops->release(sock)     // 프로토콜 패밀리별 (inet, inet6, unix, packet ...)
   │                           ↓
   │                        IPv4 소켓이면 = inet_release
   ▼
 inet_release                 // net/ipv4/af_inet.c
   │
   ▼
 sk->sk_prot->close(sk, timeout)   // 프로토콜별 (tcp, udp, raw ...)
                                     ↓
                                  TCP 면 = tcp_close
                                  UDP 면 = udp_lib_close
```

### 단계별 매핑 표

```
 단계           함수 포인터 필드              TCP 소켓 해석                 UDP 소켓 해석
 ───────────    ──────────────────────      ────────────────────────     ────────────────────────
 VFS            file->f_op                   socket_file_ops              socket_file_ops
 (공통)         (release = sock_close)       같음                         같음

 프로토콜 패밀리 sock->ops (struct proto_ops) inet_stream_ops              inet_dgram_ops
                (release = inet_release)     같음(inet_release)           같음(inet_release)

 프로토콜       sk->sk_prot (struct proto)   tcp_prot                     udp_prot
                close 필드                   tcp_close                    udp_lib_close

 실제 동작                                    FIN 전송, 상태머신 진행       수신큐 비우기
                                              TIME_WAIT 60초 유지          sock 해제
                                              SO_LINGER 반영               (즉시)
```

### 코드 — inet_release

```c
// net/ipv4/af_inet.c  (단순화)
int inet_release(struct socket *sock)
{
    struct sock *sk = sock->sk;
    if (sk) {
        long timeout = 0;

        // SO_LINGER 가 켜져 있으면 linger 시간만큼 대기
        if (sock_flag(sk, SOCK_LINGER) && !(current->flags & PF_EXITING))
            timeout = sk->sk_lingertime;

        sk->sk_prot->close(sk, timeout);  // ★ 여기서 프로토콜별 분기
        sock->sk = NULL;
    }
    return 0;
}
```

### 코드 — tcp_prot / udp_prot 등록

```c
// net/ipv4/tcp_ipv4.c
struct proto tcp_prot = {
    .name      = "TCP",
    .close     = tcp_close,         // ★
    .connect   = tcp_v4_connect,
    .accept    = inet_csk_accept,
    .sendmsg   = tcp_sendmsg,
    .recvmsg   = tcp_recvmsg,
    ...
};

// net/ipv4/udp.c
struct proto udp_prot = {
    .name      = "UDP",
    .close     = udp_lib_close,     // ★ (실질적으로 sk_common_release)
    .connect   = udp_connect,
    .sendmsg   = udp_sendmsg,
    .recvmsg   = udp_recvmsg,
    ...
};
```

### tcp_close — 왜 오래 걸리는가

```c
// net/ipv4/tcp.c  (요약)
void tcp_close(struct sock *sk, long timeout)
{
    // 1) 수신큐에 데이터 남아 있으면 RST 전송 (정상 FIN 대신)
    //    아니면 FIN 전송
    if (tcp_close_state(sk))
        tcp_send_fin(sk);

    // 2) SO_LINGER timeout 동안 대기하며 상태 머신 진행
    //    FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT
    sk_stream_wait_close(sk, timeout);

    // 3) TIME_WAIT 슬롯으로 옮김 (2*MSL = 60초, struct tcp_timewait_sock)
    tcp_time_wait(sk, TCP_FIN_WAIT2, timeo);
    ...
}
```

TIME_WAIT 상태는 **프로세스가 죽어도 커널 안에 남는다**. `ss -tan | grep TIME-WAIT` 으로 확인 가능.

### "왜 분리했나?" — 설계 이유

```
 • 신뢰 프로토콜(TCP)은 연결 종료 시 양방향 합의가 필요 → 상태머신과 타이머
 • 비신뢰 프로토콜(UDP)은 그런 게 없음 → 버퍼만 비우면 끝
 • 커널 입장에서는 "struct sock 에 close 라는 함수 포인터" 하나만 있으면 됨
 • TCP/UDP/SCTP/ICMP/RAW 가 각자 다른 구현을 등록
 • 상위(inet_release) 는 어떤 프로토콜인지 몰라도 됨  ← 이게 strategy pattern 의 본질
```

---

## §5. 공유된 fd 의 동시 사용

fork() 이후 부모/자식은 **fdtable 슬롯까지 복제되지만, 그 슬롯이 가리키는 struct file 은 공유** 된다. 결과:

```
 공유되는 것 (struct file 에 있는 것)
   • f_pos (파일 읽기 오프셋)
   • f_flags
   • 소켓이면 수신 큐, 송신 큐
   • inode 참조

 공유되지 않는 것 (fdtable 에 있는 것)
   • fd 번호 자체는 같지만 각자 자기 슬롯에서 관리
   • close_on_exec 비트
```

### 시나리오 A — 같은 파일을 fork 후 read

```
 Parent P 가 open("data.txt", O_RDONLY) → fd 3, f_pos=0
 fork()
 Child  C 도 fd 3 을 보며 같은 struct file, 같은 f_pos

 [1] Parent read(3, buf1, 10)   → buf1 에 0~9 바이트, f_pos=10
 [2] Child  read(3, buf2, 10)   → buf2 에 10~19 바이트, f_pos=20
 [3] Parent read(3, buf3, 10)   → buf3 에 20~29 바이트, f_pos=30

 "경쟁적 소비" (cooperative consumption)
   결과가 race condition — 스케줄링 순서에 따라 각 프로세스가 읽는 범위가 바뀜
```

### 시나리오 B — stdin 공유

```
 stdin 이 키보드 TTY 일 때
   Parent P 가 read(0, buf, 1) 로 블로킹
   Child  C 도 read(0, buf, 1) 로 블로킹

 키보드에서 'A' 한 글자 입력
   → TTY 라인 디시플린이 수신 큐에 'A' 을 하나 넣음
   → 대기 중인 두 프로세스 중 하나가 깨어나서 'A' 를 받아감
   → 다른 하나는 계속 블로킹

 "A B 둘 다 출력된다" 는 직관은 틀림. 경쟁적 소비.
```

### 시나리오 C — 소켓 공유 (prefork 웹 서버 패턴)

```
 Parent P 가 listen socket 에 bind/listen
 fork() × N — 자식들 전부 같은 listen socket fd 공유
 모든 자식이 accept() 호출

 클라이언트가 connect → 커널이 한 자식만 깨움 (epoll/thundering herd 회피)
 → 그 자식만 새 conn socket 을 accept
```

### 시나리오 D — 동시 write

```
 두 프로세스가 같은 fd 에 write("hello\n") 를 동시에 호출

 → POSIX 는 "한 번의 write() 호출이 PIPE_BUF 이하면 atomic" 보장
 → PIPE_BUF 초과하면 원자성 없음 → 두 문자열이 섞일 수 있음

 예: A 가 "hello\n", B 가 "world\n" 동시 write
    출력이 "hello\nworld\n" 일 수도, "heworllo\nld\n" 일 수도 있음
```

### 한 눈에 보는 "읽기 vs 쓰기 대칭성"

```
 동작     같은 struct file 공유 시 의미
 ─────   ─────────────────────────────────────────────
 read    파괴적 소비 (destructive) — 누가 먼저 읽느냐가 내용을 결정
 write   누적적 출력 (accumulative) — 둘 다 결과가 나감. 단 atomic 경계 주의
```

---

## §6. fd 로 접근 가능한 객체 총정리

흔히 "파일, 디렉토리, 소켓, 파이프" 넷으로 알고 있지만 실제로는 훨씬 다양하다. 전부 나열하면:

### 카테고리별 카탈로그

```
 [A] 파일시스템 객체
 ────────────────────
  A1. regular file       — /etc/passwd, ./main.c 같은 일반 파일
  A2. directory          — opendir/readdir 또는 getdents64 시스템 콜
  A3. symbolic link      — O_PATH | O_NOFOLLOW 로 링크 자체를 fd 로
  A4. FIFO (named pipe)  — mkfifo 로 생성한 파일시스템에 존재하는 파이프

 [B] IPC 객체 (파일시스템에 없음)
 ─────────────────────────────
  B1. anonymous pipe     — pipe() / pipe2()
  B2. UNIX socket        — AF_UNIX, 같은 호스트 프로세스 간 통신 (stream/dgram/seqpacket)

 [C] 네트워크 소켓
 ───────────────
  C1. TCP                — AF_INET/AF_INET6, SOCK_STREAM
  C2. UDP                — SOCK_DGRAM
  C3. RAW                — SOCK_RAW (ping, 패킷 조작)
  C4. PACKET             — AF_PACKET (tcpdump 처럼 L2 프레임 직접 접근)
  C5. NETLINK            — 커널-유저 통신 (iproute2, udev 등)

 [D] TTY / 터미널
 ───────────────
  D1. 물리 TTY           — /dev/tty1, /dev/ttyS0 (시리얼)
  D2. PTY                — /dev/ptmx + /dev/pts/N (xterm, tmux)

 [E] 디바이스
 ───────────
  E1. character device   — /dev/null, /dev/zero, /dev/random, /dev/urandom, /dev/fb0 ...
  E2. block device       — /dev/sda, /dev/loop0

 [F] 커널 이벤트 객체 (fd 로 추상화된 "상태")
 ──────────────────────────────────────
  F1. eventfd            — 유저 공간 세마포어/카운터. eventfd(0,0)
  F2. timerfd            — 타이머 만료를 read 로 소비. timerfd_create
  F3. signalfd           — 시그널을 read 로 받음 (시그널 핸들러 대신)
  F4. inotify            — 파일/디렉토리 변경 감시 fd
  F5. fanotify           — 시스템 전역 파일 감시 (바이러스 백신 등)
  F6. pidfd              — 특정 프로세스 참조. pidfd_open / pidfd_send_signal

 [G] I/O 멀티플렉싱 엔진
 ─────────────────────
  G1. epoll fd           — epoll_create1 (여러 fd 감시)
  G2. io_uring fd        — io_uring_setup (submission/completion queue)

 [H] 메모리 / 격리 객체
 ────────────────────
  H1. memfd              — 파일시스템에 없는 메모리 기반 파일. memfd_create
  H2. userfaultfd        — 유저 공간 page fault 핸들링
  H3. perf_event          — perf 프로파일링 이벤트
  H4. bpf map/program fd  — eBPF 객체 참조

 [I] cgroup / 네임스페이스
 ─────────────────────
  I1. /proc/PID/ns/*     — open 하면 네임스페이스 fd 획득 (setns 용)
  I2. cgroup fd          — cgroup v2 에서 cgroupfs 열기

 [J] 확장 / 메타
 ──────────
  J1. O_PATH fd          — "경로만" 표현하는 fd. 읽기/쓰기 불가. *at() 함수 family 에 사용
  J2. /proc/self/fd/N    — 현재 프로세스의 fd 를 경로로 재open

 [K] DMA / GPU / 특수
 ──────────────────
  K1. DRM fd             — /dev/dri/card0, GPU 커맨드
  K2. dmabuf fd          — 드라이버 간 버퍼 공유
```

### 용도 치트시트

```
 목적                                  써야 하는 fd
 ────────────────────────────         ─────────────────────────────
 같은 호스트 두 프로세스 간 바이트스트림   pipe or UNIX socket(SOCK_STREAM)
 주고받기
 같은 호스트 두 프로세스 간 메시지 단위    UNIX socket(SOCK_DGRAM)
 네트워크 TCP/UDP                       AF_INET + SOCK_STREAM/SOCK_DGRAM
 L2 패킷 캡처/주입                      AF_PACKET
 파일 변경 감시                        inotify / fanotify
 시그널을 동기적으로 받기                signalfd
 타이머 이벤트를 동기적으로 받기          timerfd
 세마포어/카운터 대체                    eventfd
 여러 fd 대기                           epoll / io_uring / poll / select
 파일시스템 없는 임시 파일                memfd_create
 외부 프로세스에 시그널 안전하게           pidfd_open + pidfd_send_signal
 컨테이너 격리                         /proc/PID/ns/* + setns
```

**요지:** 유닉스/리눅스에서 **"어떤 리소스를 프로세스가 참조한다"** 의 기본 단위가 fd 다. 커널이 새 기능을 추가할 때도 fd 추상화에 맞춰서 제공하는 경향이 계속 강해지고 있다 (eventfd/timerfd/signalfd/pidfd 가 다 2000년대 후반 이후 추가).

---

## §7. stdin/stdout/stderr — 커널이 구분하지 않는 "약속"

### 오해와 사실

```
 오해: 커널이 fd 0/1/2 를 특별히 취급해서 "읽기는 0 에서, 쓰기는 1/2 로만 가능" 하게 한다.
 사실: 커널은 fd 번호를 구분하지 않는다. read/write 가능 여부는 struct file->f_mode 만 본다.
       0/1/2 라는 번호는 libc 매크로와 셸 관행이 유지하는 "소켓 위의 약속".
```

### libc 와 셸의 약속

```c
// /usr/include/stdio.h 부근
extern FILE *stdin;    // 내부적으로 fd 0 을 감싼 FILE*
extern FILE *stdout;   // fd 1
extern FILE *stderr;   // fd 2

// glibc 매크로
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2
```

셸이 새 프로세스를 실행할 때의 관행:

```
 bash 가 "./myprog" 실행 시
   1. fork()                    — 자식은 부모의 fdtable 복제 (0/1/2 그대로 터미널)
   2. 필요하면 리다이렉션 세팅   — dup2(file_fd, 0) 등으로 덮어씌움
   3. execve("./myprog", ...)   — myprog 는 0/1/2 가 뭔지 모른 채 그냥 사용
```

### 커널 관점 — 같은 fd 에 양방향 가능성

```
 struct file 의 f_mode 에 FMODE_READ / FMODE_WRITE 둘 다 켜져 있으면
   read(fd, ...)  — 가능
   write(fd, ...) — 가능

 fd 0 이 O_RDWR 로 열린 TTY 라면
   read(0, ...)   — 키보드 입력 읽기
   write(0, ...)  — 화면 출력 (가능은 함, 관행상 1 을 씀)

 fd 1 과 fd 2 가 같은 struct file 을 가리키면
   두 fd 모두 같은 곳에 써짐
```

### "stdin 에 소켓을 끼면?"

```
 1. 어떤 fd 3 에 소켓을 열어둠
 2. dup2(3, 0) — fd 0 도 그 소켓을 가리키게 됨
 3. close(3)   — 정리
 4. 이제 read(0, ...) = 소켓에서 데이터 수신
            write(0, ...) = 소켓으로 데이터 송신 (f_mode 에 WRITE 있으면)

 이 패턴이 inetd / xinetd / systemd socket activation 의 기본
```

### stderr vs stdout — 단 하나의 차이

```
 커널 수준 차이: 없음. 둘 다 그냥 fd.
 libc 수준 차이: stdout 은 block/line-buffered, stderr 은 unbuffered (§9)
 관행 수준 차이: stdout 은 "프로그램의 정상 출력", stderr 은 "에러/진단".
                파이프 처리 시 "| grep ..." 가 stdout 만 넘기는 이유.
```

```
 # stdout 만 파일로
 ./myprog > out.txt
   → bash: open("out.txt", O_WRONLY|O_CREAT|O_TRUNC) = fd X
           dup2(X, 1); close(X);
           execve("./myprog")

 # stderr 만 파일로
 ./myprog 2> err.txt
   → bash: ... dup2(X, 2) ...

 # 둘 다 같은 파일로
 ./myprog > log.txt 2>&1
   → bash: dup2(X, 1); dup2(1, 2);
   → fd 1 과 fd 2 가 같은 struct file 을 가리킴
```

---

## §8. dup2 리다이렉트 — 셸/CGI/inetd 패턴

`dup2(oldfd, newfd)` 의 의미: "newfd 슬롯이 oldfd 와 같은 struct file 을 가리키게 한다".

### 커널 수준 동작

```
 dup2(3, 1) 호출 시
   1. fd 1 이 이미 열려 있으면 close(1) 을 내부적으로 먼저 수행
   2. fdtable[1] = fdtable[3]  (같은 struct file 포인터)
   3. struct file 의 refcount ++

 결과:
   fdtable                   struct file
   [0] → tty
   [1] → socket ────────────→ refcount=2
   [3] → socket ────────────┘
```

### 패턴 1 — 셸 리다이렉션

```c
// bash 가 "./myprog > out.txt" 을 실행할 때의 간략한 코드
pid_t p = fork();
if (p == 0) {
    int f = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(f, 1);            // stdout 을 파일로
    close(f);              // 원본 fd 는 더 이상 필요 없음
    execve("./myprog", argv, envp);
}
```

### 패턴 2 — 파이프라인

```c
// "cmd1 | cmd2" 의 대략적 구현
int pfd[2];
pipe(pfd);  // pfd[0]=read, pfd[1]=write

if (fork() == 0) {
    // cmd1
    dup2(pfd[1], 1);   // stdout 을 파이프 쓰기 끝으로
    close(pfd[0]); close(pfd[1]);
    execve("cmd1", ...);
}
if (fork() == 0) {
    // cmd2
    dup2(pfd[0], 0);   // stdin 을 파이프 읽기 끝으로
    close(pfd[0]); close(pfd[1]);
    execve("cmd2", ...);
}
close(pfd[0]); close(pfd[1]);
wait(NULL); wait(NULL);
```

### 패턴 3 — inetd / xinetd / systemd socket activation

```c
// 클라이언트가 connect 해 올 때마다 새 프로세스를 띄워서 처리
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
// bind, listen ...
for (;;) {
    int conn = accept(listen_fd, NULL, NULL);
    if (fork() == 0) {
        // conn 소켓을 stdin/stdout/stderr 에 연결
        dup2(conn, 0);
        dup2(conn, 1);
        dup2(conn, 2);
        close(listen_fd);
        close(conn);

        // 이 프로그램은 stdin/stdout 으로 "그냥" 읽고 쓰면 네트워크가 된다
        execve("/usr/sbin/my-service", argv, envp);
    }
    close(conn);
}
```

이 패턴이 강력한 이유: **my-service 는 소켓 API 를 전혀 몰라도 된다**. 그냥 `scanf`/`printf` 로 동작하는 프로그램이 inetd 아래에서 돌면 네트워크 데몬이 된다. 초기 유닉스 시대에 **"대화형 CLI 도구를 그대로 네트워크 데몬화"** 하기 위해 설계된 규약.

### 패턴 4 — CGI

```c
// 웹 서버가 CGI 스크립트를 실행할 때
int pfd[2];
pipe(pfd);

if (fork() == 0) {
    // CGI 프로그램은 stdin 으로 요청 body, stdout 으로 응답 body
    dup2(client_conn_fd, 0);      // 요청 읽기는 클라이언트 소켓에서
    dup2(pfd[1], 1);              // 응답은 파이프로 (부모가 읽어서 HTTP 헤더 붙임)
    close(pfd[0]); close(pfd[1]);

    // 환경변수 세팅 (CGI 스펙: QUERY_STRING, CONTENT_LENGTH, ...)
    setenv("QUERY_STRING", query, 1);
    execve(cgi_path, argv, envp);
}
```

### "왜 dup2 인가" — fd 번호 고정의 필요성

```
 1. execve 후에도 프로그램은 stdin/stdout 을 찾아야 함 (fd 0/1/2 라는 약속)
 2. fork 후 close 만 하면 fd 번호가 재사용되지 않을 수 있음
    (open 이 제일 낮은 빈 fd 를 선택하므로, close(1) → open 하면 1 이 아닐 수도)
 3. dup2 는 "정확히 이 번호로 복제" 를 원자적으로 보장 → 리다이렉트의 유일한 안전책
```

---

## §9. libc stdio 버퍼링 — unbuffered / line / block

"stdout 이 line-buffered" "stderr 이 unbuffered" 같은 말은 **커널이 아니라 libc 의 FILE* 레벨** 이야기다.

### 2-레벨 버퍼링

```
    유저 프로그램                    libc FILE*                     커널
  ──────────────────             ──────────────────             ──────────────────
    printf("hi\n")         ──→   libc 버퍼에 누적                 syscall 미발생
                                  (4KB buf 예)                   syscall 미발생

    fflush(stdout)         ──→   write(1, buf, n)            ──→ 커널 파이프/소켓/
                                                                 디스크 버퍼 큐잉

    fsync(1)               ──→                               ──→ 블록 디바이스로
                                                                 실제 기록 (FS 에만 유효)
```

libc 버퍼를 쓰는 이유: **syscall 은 비싸다**. 한 바이트씩 write(1, ..., 1) 을 하면 syscall 이 매번 발생해서 성능이 수십 배 느려진다. libc 가 내부 버퍼에 모아뒀다가 한 번에 큰 write() 로 넘긴다.

### 버퍼 모드 세 가지

```
 모드                  정책                              기본으로 쓰이는 곳
 ──────────────       ──────────────────────────────   ──────────────────────────────
 unbuffered           쓰는 즉시 write() 호출           stderr
                     (버퍼 안 씀)

 line-buffered        '\n' 이 나오거나 버퍼가 가득     stdout (TTY 에 연결될 때)
 (_IOLBF)            차면 flush

 block-buffered       버퍼가 가득 차야 flush           stdout (파일/파이프에 연결될 때)
 (_IOFBF)            (보통 BUFSIZ = 8192 바이트)       stdin 도 보통 여기
```

### 결과의 함정

```
 # 터미널에서 직접 실행 — stdout 이 line-buffered
 ./myprog
   printf("progress..."); sleep(1); printf(" done\n");
   → 터미널에 "progress... done\n" 이 한꺼번에 안 뜰 수도 있음 (\n 전까지 버퍼링)
   → 근데 "progress..." 가 1 초 뒤에 'done' 과 같이 나타남

 # 파이프로 넘기면 — stdout 이 block-buffered
 ./myprog | cat
   printf("progress...");  // cat 에서 안 보임
   printf("done");          // 안 보임
   ...프로그램 종료 시점(= libc exit handler 가 fflush) 에 한꺼번에 출력

 대응:
   fflush(stdout) 명시 호출
   setvbuf(stdout, NULL, _IOLBF, 0)  — 프로그램 시작에 line-buffered 강제
   setvbuf(stdout, NULL, _IONBF, 0)  — unbuffered 강제
   환경변수 BUFSIZ 등으로 크기 조정
```

### stderr 이 unbuffered 인 이유

```
 에러는 "즉시" 나타나야 디버깅에 유용하다.
 프로그램이 크래시 직전 fprintf(stderr, "crashing!\n") 만 남기고 죽어도 메시지가 나와야 함.
 buffered 면 fflush 전에 죽으면 메시지 소실.
```

### fflush vs fsync

```
 fflush(FILE*)           — libc 버퍼 → 커널 버퍼  (write() 시스템콜 호출)
 fsync(int fd)           — 커널 버퍼 → 물리 디스크 (FS 에만 유효)
 fdatasync(int fd)       — 메타데이터는 제외하고 데이터만 fsync
 sync()                  — 시스템 전역 커널 버퍼 → 디스크
```

### 사이즈

```
 glibc 기본값
   BUFSIZ                 = 8192 (8KB)       — stdio FILE* 의 기본 버퍼
   PIPE_BUF               = 4096 (보통)       — atomic write 경계
   PAGE_SIZE              = 4096             — 가상 메모리 페이지
```

### 요지

```
 ┌─────────────────────────────────────────────────────────────┐
 │ write(2) 는 항상 syscall 을 호출 (버퍼링 없음, 커널은 비싸다) │
 │ fwrite/printf 는 libc 내부 버퍼를 거친다 (flush 정책 있음)   │
 │                                                              │
 │ "왜 안 찍혀?"  → 십중팔구 libc 버퍼링 미flush                 │
 │ "왜 디스크에 안 적혔어?" → fflush 만 했고 fsync 안 함         │
 └─────────────────────────────────────────────────────────────┘
```

---

## §10. 한 번에 보는 "fd 연산 다이어그램"

```
 유저 프로그램                                        커널
 ────────────────                                    ───────────────────────────

 int fd = open("a.txt", O_RDONLY|O_CLOEXEC);
                        │
                        ▼
                 sys_open                      ─→  do_sys_open
                                                     ├ path lookup (dcache)
                                                     ├ struct file 할당
                                                     ├ file->f_op = ext4_file_operations
                                                     ├ inode 참조 ++
                                                     ├ fdtable 빈 슬롯 찾기 (여기선 3)
                                                     └ fdtable[3] = file
                                                       close_on_exec bit[3] = 1
                                                fd = 3

 read(fd, buf, 4096)
                        │
                        ▼
                 sys_read                      ─→  vfs_read
                                                     ├ file = fdtable[3]
                                                     ├ file->f_op->read_iter (ext4_file_read_iter)
                                                     │   ├ page cache 에서 읽기 시도
                                                     │   └ 없으면 디스크에서 page in
                                                     └ copy_to_user(buf, ...)

 fork()               ─→  do_fork  ─→  copy_files (fdtable 복제, struct file refcount++)
                                       copy_fs (fs context)
                                       copy_mm (VM 복제)
                                       ...
 // 이 시점 Child 의 fdtable[3] 도 같은 struct file 을 가리킴
 // struct file 의 refcount = 2

 Child: execve("/bin/ls", ...)
                        │
                        ▼
                 sys_execve                    ─→  do_execveat_common
                                                     ├ flush_old_exec
                                                     ├ do_close_on_exec (★)
                                                     │   └ fdtable[3] 가 close_on_exec 이므로
                                                     │     close(3) 과 동일 처리
                                                     │     → struct file refcount--  (=1)
                                                     └ 새 ELF 이미지 로드

 Parent: close(fd)
                        │
                        ▼
                 sys_close                     ─→  close_fd
                                                     ├ fdtable[3] = NULL
                                                     ├ struct file refcount--  (=0)
                                                     └ __fput 지연 실행
                                                         └ file->f_op->release (ext4_release_file)
                                                             └ inode 참조 --
                                                             └ struct file 메모리 해제
```

---

## 한 줄 정리

```
 fd           = "이 프로세스가 어떤 커널 객체를 참조한다" 의 handle
 struct file  = 실제 객체 컨텍스트 (f_op 로 동작 분기, refcount 로 수명 관리)
 close(fd)    = 이 프로세스의 참조 하나 버리기. refcount 0 일 때만 객체 해제.
 O_CLOEXEC    = execve 경계에서만 자동 close. fork 후 실행 중엔 자식도 쓸 수 있음.
 파일=소켓    = VFS 관문까지는 같고, file->f_op 가 가리키는 구현이 다름.
 TCP close    = sock->ops->release → sk->sk_prot->close (strategy pattern 4단 디스패치)
 공유된 fd    = 같은 struct file → 같은 f_pos/수신큐 → 경쟁적 소비, 경쟁적 출력
 stdin/out/err= 커널이 구분 안 함. libc + 셸의 약속. dup2 로 자유롭게 교체.
 stdio 버퍼링 = libc 내부 버퍼. fflush = 커널로. fsync = 디스크로.
```
