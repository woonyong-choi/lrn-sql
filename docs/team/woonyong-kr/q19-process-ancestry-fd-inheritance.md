# q19. 프로세스의 조상과 fd 상속 — "누가 포크의 끝이고 왜 어떤 건 상속 안 되는가"

## 질문

```
 1.  모든 프로세스가 fork 로 만들어진다면, 그 포크의 "맨 처음" 부모는 누구인가?
 2.  fork 하면 fdtable 까지 복제된다는데, 그럼 stdin/stdout/stderr 도 공유하는 건가?
 3.  그런데 실제론 어떤 프로세스는 터미널을 물려받고(예: ls) 어떤 건 안 물려받는 것 같다.
     (예: systemd 로 띄운 서비스는 터미널에 출력 안 뜸) 기준과 메커니즘이 뭔가?
```

## 한 줄 답

```
 (1) 부팅 시 커널이 "직접" 만든 세 프로세스 — PID 0 / 1 / 2 — 가 fork 의 베이스 케이스.
 (2) 맞다. fork() 는 fdtable 전체를 복제한다. 자식은 기본적으로 부모의 모든 fd 를 받는다.
 (3) "상속 안 함" 이 아니라 "상속받았다가 자식이 명시적으로 버린 것".
     도구는 O_CLOEXEC / explicit close / setsid / double-fork.
```

## 목차

```
 §1.  "fork 로 만든다" 의 끝 — PID 0, 1, 2 가 어떻게 태어나는가
 §2.  부팅 시퀀스 — start_kernel 에서 /sbin/init 까지
 §3.  fork 의 fd 복제 메커니즘 — 커널 코드 따라가기
 §4.  공유된 fd 를 끊는 네 가지 방법
 §5.  컨트롤링 TTY — "터미널에 붙어 있다" 의 정체
 §6.  daemonize 실전 — double-fork + setsid
 §7.  실무 도구 매핑 (nohup / systemd / & / daemon(3))
 §8.  실전 예 — 같은 ./server 를 다섯 가지 방식으로 띄우기
```

---

## §1. "fork 로 만든다" 의 끝 — PID 0 / 1 / 2

모든 프로세스가 fork 의 자손이라는 말은 거의 맞지만 **예외 셋**이 있다. 이 셋은 부팅 중에 커널이 자기 손으로 직접 만든다.

```
 PID  이름        누가 만드나                       역할
 ───  ────────    ────────────────────────────     ──────────────────────────
 0    swapper/    커널이 start_kernel 안에서        CPU 가 쉴 때 돌리는 idle
      idle        직접 구성 (fork 아님)             프로세스. 각 CPU 마다 하나
                                                   (swapper/0, swapper/1, ...)

 1    init        kernel_thread(kernel_init, ...)   유저 공간 첫 프로세스.
      (systemd)   으로 만들고, 이 스레드가          나중에 execve("/sbin/init")
                  /sbin/init 을 exec                모든 유저 프로세스의 조상

 2    kthreadd    kernel_thread(kthreadd, ...)     이후 모든 커널 스레드의 부모.
                  으로 만듦                         유저 공간 절대 안 감
```

```
 유저 공간 프로세스 트리
 ──────────────────────
 systemd (PID 1)
   ├── sshd
   │    └── bash (sshd 가 로그인 시 fork+exec)
   │         └── vim, ls, grep, ...
   ├── systemd-journald
   ├── NetworkManager
   ├── dockerd
   │    └── containerd
   │         └── (각 컨테이너 내부 프로세스)
   └── user@1000.service
         └── gnome-session
               └── firefox, terminal ...

 커널 스레드 트리
 ────────────────
 kthreadd (PID 2)
   ├── ksoftirqd/0
   ├── migration/0
   ├── kworker/u16:*
   ├── kswapd0
   └── ... 수십 개
```

### 왜 init 은 fork 의 베이스 케이스인가

fork 는 "기존 task_struct 를 복사해서 새 task_struct 를 만드는" 연산이다. **최소 하나의 task_struct 가 "없는 상태에서" 만들어져야** 재귀가 시작된다. 그 "없는 상태에서 만드는" 일은 커널의 C 코드가 직접 `struct task_struct` 를 `kzalloc` 한 뒤 필드를 채워 넣는 방식으로 이뤄진다.

---

## §2. 부팅 시퀀스 — start_kernel 에서 /sbin/init 까지

커널 코드 레벨로 내려가면 이렇다.

```c
// init/main.c  (매우 단순화)
asmlinkage __visible void __init start_kernel(void)
{
    // 1. 하드웨어 초기화, 메모리 할당자, 스케줄러, 인터럽트 등
    setup_arch(&command_line);
    build_all_zonelists(NULL);
    mm_init();
    sched_init();
    init_IRQ();
    timer_init();
    ...

    // 2. PID 0 (idle) 은 이 시점에 이미 존재 (cpu_idle 루프에 진입 준비)

    // 3. "첫 일반 프로세스" 를 만든다
    rest_init();
}

static noinline void __ref rest_init(void)
{
    int pid;

    rcu_scheduler_starting();

    // PID 1 생성: kernel_init 이라는 커널 스레드
    pid = kernel_thread(kernel_init, NULL, CLONE_FS);
    // 이 시점부터 "init" 이라는 PID 1 이 스케줄러 대기열에 있음

    // PID 2 생성: kthreadd (모든 커널 스레드의 부모)
    pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);

    // PID 0 은 여기서 idle 루프로 진입
    cpu_startup_entry(CPUHP_ONLINE);
}
```

PID 1 이 실제로 유저 공간 `/sbin/init` 이 되는 지점:

```c
// init/main.c  (단순화)
static int __ref kernel_init(void *unused)
{
    kernel_init_freeable();        // 파일시스템 마운트, 드라이버 로드 등
    ...

    // 여기서 /sbin/init (또는 systemd) 을 exec
    if (execute_command) {
        ret = run_init_process(execute_command);
        ...
    }
    if (!try_to_run_init_process("/sbin/init")   ||
        !try_to_run_init_process("/etc/init")    ||
        !try_to_run_init_process("/bin/init")    ||
        !try_to_run_init_process("/bin/sh"))
        return 0;

    panic("No working init found.  Try passing init= option to kernel.");
}
```

`execve` 는 **같은 task_struct 를 유지**한 채 코드/데이터를 교체한다. 그래서 PID 1 은 여전히 1 이지만 이제는 `systemd` 라는 유저 프로그램이 돌고 있는 셈.

```
 시간축
 ──────
 [부팅]
   커널 코드가 start_kernel 실행
   |
   v
 PID 0 생성  (idle)            - task_struct 를 커널이 직접 조립
   |
   v
 PID 1 생성  (kernel_init)      - kernel_thread 로 커널 스레드 생성
   |                              아직 유저 공간 없음
   v
 PID 2 생성  (kthreadd)
   |
   v
 PID 1 이 kernel_init 내부에서
 execve("/sbin/init") 실행
   |
   v
 이제 PID 1 = systemd (유저 공간)
   |
   v
 systemd 가 서비스/셸/로그인 매니저를 fork+exec
   |
   v
 (사용자가 로그인 후) bash 가 fork+exec 로 ls, vim 등 실행
```

이후 모든 유저 프로세스는 **PID 1 의 직·간접 후손**이다.

---

## §3. fork 의 fd 복제 메커니즘 — 커널 코드

`fork()` 가 호출되면 내부적으로 `clone` 시스템콜을 거쳐 `_do_fork` → `copy_process` 가 돈다. 이 중 fd 와 직접 관련된 건 `copy_files`.

```c
// kernel/fork.c  (단순화)
static int copy_files(unsigned long clone_flags, struct task_struct *tsk)
{
    struct files_struct *oldf = current->files;
    struct files_struct *newf;

    if (clone_flags & CLONE_FILES) {
        // 플래그가 있으면 같은 files_struct 를 공유 (pthread 방식)
        atomic_inc(&oldf->count);
        tsk->files = oldf;
        return 0;
    }

    // 기본 fork: files_struct 를 새로 만들고 fdtable 을 복제
    newf = dup_fd(oldf, NR_OPEN_MAX, &error);
    tsk->files = newf;
    return 0;
}
```

`dup_fd` 내부:

```c
// fs/file.c  (단순화)
struct files_struct *dup_fd(struct files_struct *oldf, unsigned int max_fds, int *errorp)
{
    struct files_struct *newf;
    struct fdtable *old_fdt, *new_fdt;

    newf = kmem_cache_alloc(files_cachep, GFP_KERNEL);
    ...
    old_fdt = files_fdtable(oldf);
    new_fdt = &newf->fdtab;

    // 배열 복사
    for (i = 0; i < old_fdt->max_fds; i++) {
        struct file *f = old_fdt->fd[i];
        if (f) {
            get_file(f);                 // struct file 의 참조 카운트 ++
            new_fdt->fd[i] = f;           // 같은 file 을 새 배열에서도 가리킴
        }
    }
    // close_on_exec 비트맵도 복사
    memcpy(new_fdt->close_on_exec, old_fdt->close_on_exec, ...);
    ...
    return newf;
}
```

핵심 두 줄:

```
 get_file(f)                    struct file 참조 카운트 증가
 new_fdt->fd[i] = f             같은 struct file 을 새 배열에서 가리킴
```

### 그림으로 정리

fork 전:

```
 부모 프로세스
   task_struct
     └─ files ─> files_struct(부모)
                   └─ fdtable
                       ├─ [0] ─> struct file (tty)       f_count=1
                       ├─ [1] ─> struct file (tty)       f_count=1
                       ├─ [2] ─> struct file (tty)       f_count=1
                       ├─ [3] ─> struct file (socket)    f_count=1
                       └─ [4] ─> struct file (a.txt)     f_count=1
```

fork 직후:

```
 부모                                  자식
  task_struct                           task_struct
   └─ files ─> files_struct(부모)        └─ files ─> files_struct(자식)
                └─ fdtable(부모)                      └─ fdtable(자식)
                    [0]─┐                                [0]─┐
                    [1]─┼─ 같은                          [1]─┼─ 같은
                    [2]─┤   struct file 들              [2]─┤
                    [3]─┤                               [3]─┤
                    [4]─┘                                [4]─┘
                             (f_count 가 전부 2 로 증가)
```

**결론**: fd 번호는 자식에서도 같지만, **양쪽에서 같은 `struct file` 을 가리킨다**. 즉 offset, 플래그까지 공유.

---

## §4. 공유된 fd 를 끊는 네 가지 방법

### 방법 1. 자식이 execve 전에 명시적으로 close

```c
pid_t pid = fork();
if (pid == 0) {
    // 자식 쪽
    close(3);
    close(4);
    close(5);
    execve("/usr/bin/myprog", argv, envp);
}
```

단순하고 명시적이지만, 열린 fd 가 많으면 일일이 close 해야 해서 실수 잦음.

### 방법 2. O_CLOEXEC / FD_CLOEXEC

**open 할 때부터** "이 fd 는 execve 하면 자동으로 닫아라" 라고 표시.

```c
// 이렇게 여는 순간 close-on-exec 가 세팅됨
int fd = open("a.txt", O_RDONLY | O_CLOEXEC);

// 또는 이미 열린 fd 에 나중에 붙이기
int flags = fcntl(fd, F_GETFD);
fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
```

fork 해도 자식이 fd 를 받긴 한다. 하지만 **execve 시점에 커널이 자동으로 닫는다**. 동작하는 커널 코드:

```c
// fs/exec.c  (단순화)
static int do_execveat_common(...)
{
    ...
    // close_on_exec 비트가 세팅된 모든 fd 를 닫음
    do_close_on_exec(current->files);
    ...
}

// fs/file.c
void do_close_on_exec(struct files_struct *files)
{
    struct fdtable *fdt = files_fdtable(files);
    for each set bit in fdt->close_on_exec {
        __close_fd(files, fd);
    }
}
```

**권장**: 새 fd 를 열 때 **기본적으로 `O_CLOEXEC` 를 붙이는 것이 현대 리눅스의 베스트 프랙티스**. 왜냐면 "나도 모르는 사이에 자식이 내 파일을 물려받는" 보안 사고를 막을 수 있다.

실제로 libc 의 `socket`, `pipe2`, `open` 최근 버전은 `SOCK_CLOEXEC`, `O_CLOEXEC`, `O_CLOEXEC` 플래그를 권장한다.

```
 O_CLOEXEC 있음           fork 후 자식에도 fd 있음
                         +
                         execve 하면 커널이 자동 close
                         = 자식 새 프로그램은 fd 못 봄

 O_CLOEXEC 없음 (기본)    fork 후 자식에도 fd 있음
                         +
                         execve 해도 유지
                         = 자식 새 프로그램이 그대로 fd 사용 가능
```

### 방법 3. posix_spawn + file_actions

fork+exec 대신 `posix_spawn` 한 방에. fd 조작을 file_actions 객체로 선언.

```c
posix_spawn_file_actions_t fa;
posix_spawn_file_actions_init(&fa);

posix_spawn_file_actions_addclose(&fa, 3);   // fd 3 닫기
posix_spawn_file_actions_adddup2(&fa, logfd, 1);  // stdout 을 logfd 로

pid_t pid;
posix_spawn(&pid, "/usr/bin/myprog", &fa, NULL, argv, envp);

posix_spawn_file_actions_destroy(&fa);
```

systemd 가 내부적으로 쓰는 방식.

### 방법 4. /proc/self/fd 를 돌며 싹 닫기 (방어적 프로그래밍)

```c
DIR *dir = opendir("/proc/self/fd");
struct dirent *e;
while ((e = readdir(dir))) {
    int fd = atoi(e->d_name);
    if (fd > 2) close(fd);
}
closedir(dir);
```

daemon 을 직접 만들 때 자주 쓰는 방어 코드. 부모가 어떤 fd 를 열어두고 있었는지 알 수 없을 때 안전장치.

---

## §5. 컨트롤링 TTY — "터미널에 붙어 있다" 의 정체

fd 0/1/2 가 터미널을 가리키는 것과, **프로세스가 "터미널에 붙어 있는" 것**은 다른 얘기다.

### 프로세스 관계의 네 계층

```
 +────────────────────────────────────────+
 |  Session                                |
 |    + 하나의 컨트롤링 TTY 에 연결될 수 있음│
 |    +────────────────────────────────+  |
 |    |  Process Group                  |  |
 |    |    + 하나의 "job" 을 이룸        |  |
 |    |    +─────────────────────────+  |  |
 |    |    |  Process                 |  |  |
 |    |    |    + task_struct         |  |  |
 |    |    |    + fdtable             |  |  |
 |    |    |    +──────────────────+  |  |  |
 |    |    |    |  Thread (task)    |  |  |  |
 |    |    |    +──────────────────+  |  |  |
 |    |    +─────────────────────────+  |  |
 |    +────────────────────────────────+  |
 +────────────────────────────────────────+
```

Session, Process Group, PID, TID 네 개가 모두 다른 개념.

### 컨트롤링 TTY 가 하는 일

컨트롤링 TTY 는 **세션 단위**의 속성이다. 한 세션은 최대 하나의 TTY 에 "붙어 있을" 수 있다.

```
 TTY 가 하는 일
 ────────────
 1.  Ctrl+C 누르면 TTY 가 SIGINT 를 "포그라운드 프로세스 그룹" 전체에 전달
 2.  Ctrl+Z 누르면 SIGTSTP
 3.  TTY 가 연결 끊기면 (예: SSH 끊김) SIGHUP 를 세션 리더에게 전달
 4.  백그라운드 프로세스가 TTY 에 쓰기 시도하면 SIGTTOU 발송
```

그래서 "터미널에서 Ctrl+C 로 죽일 수 있다" = "컨트롤링 TTY 에 붙어 있다" 와 거의 동치.

### 셸이 fork 할 때 일어나는 일

```
 bash (PID 3000, 세션 3000, pgid 3000, 컨트롤링 TTY = /dev/pts/0)
   |
   | fork
   v
 (자식, PID 4000, 세션 3000, pgid 3000, 컨트롤링 TTY = /dev/pts/0)
   |
   | execve("ls")
   v
 ls 가 실행됨.  fd 0/1/2 는 여전히 /dev/pts/0 을 가리킴
 세션/pgid 도 그대로 -> Ctrl+C 로 죽일 수 있음
```

반면 **백그라운드 & 는 세션/pgid 자체를 안 바꾸지만** 포그라운드 pgid 에서 빠진다. 그래서 Ctrl+C 는 안 먹힌다.

### 데몬은 "세션을 새로 만들어서" 분리

```
 bash (PID 3000)
   |
   | fork
   v
 (자식, PID 4000)
   |
   | setsid()  <- 여기서 새 세션 만듦
   v
 (PID 4000, 세션 4000, pgid 4000, 컨트롤링 TTY = 없음)
   |
   | execve("server")
   v
 server 가 TTY 와 완전 분리됨
```

`setsid()` 가 하는 일:

```
 1. 호출한 프로세스를 새 세션의 리더로 만든다
 2. 컨트롤링 TTY 와의 연결을 끊는다
 3. 새 프로세스 그룹도 만든다 (pgid = pid)

 단, 이미 세션 리더면 실패. 그래서 데몬 패턴은 먼저 fork 해서 리더가 아닌 상태에서 setsid 함.
```

---

## §6. daemonize 실전 — double-fork + setsid

전통적인 데몬 패턴 (SysV 스타일).

```c
void daemonize(void)
{
    pid_t pid;

    // 1. 첫 fork — 부모는 종료. 자식은 고아(orphan) 가 되고 PID 1 에 입양됨
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);       // 부모 종료 (쉘 프롬프트 복귀)

    // 2. setsid — 새 세션/pgid 만들고 TTY 분리
    if (setsid() < 0) exit(EXIT_FAILURE);

    // 3. 두 번째 fork — 이 자식은 세션 리더가 아니므로
    //                  앞으로 open("/dev/tty") 해도 TTY 를 다시 잡을 수 없음
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    // 4. 작업 디렉토리를 / 로 (언마운트 방지)
    chdir("/");

    // 5. umask 초기화 (상속 방지)
    umask(0);

    // 6. 표준 fd 를 /dev/null 로 리다이렉트
    int fd = open("/dev/null", O_RDWR);
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2) close(fd);

    // 7. (방어적) 다른 fd 도 전부 닫기
    // /proc/self/fd 스캔 또는 for(i=3;i<sysconf(_SC_OPEN_MAX);i++) close(i);
}
```

### 왜 두 번 fork 하나

**첫 fork** 만 하면 자식은 `setsid` 로 세션 리더가 된다. 세션 리더는 조건만 맞으면 `open("/dev/tty", O_RDWR)` 으로 TTY 를 다시 잡을 수 있다. 이걸 **영구적으로 막으려면** 한 번 더 fork 해서 세션 리더가 아닌 자식을 만들어 거기서 돌려야 한다.

```
 과정        PID      세션     세션리더?   TTY 잡을 수 있나
 ─────       ──       ──       ────       ──────────────
 원본 bash   3000    3000      yes        (이미 가짐)
 1차 fork   4000    3000      no         yes (아직 세션 안 바꿈)
  setsid    4000    4000      yes        no (방금 분리)
 2차 fork   5000    4000      no         no (리더 아님)
```

세 번째 줄 상태에서 멈추면 **언젠가 다시 TTY 가 붙을 위험**이 있다는 것이 핵심.

### systemd 가 하는 건 다름

현대 리눅스에선 `systemd` 가 서비스를 띄울 때 **double-fork 를 안 하고** 자기가 직접 부모가 되어 cgroup, 네임스페이스, stdin/out/err redirection, 환경변수 등을 `posix_spawn` 스타일로 설정한다.

```
 [systemd]
   |
   | 서비스 시작 시
   | 1. cgroup 만듦
   | 2. 파일 디스크립터 정리 (O_CLOEXEC 활용)
   | 3. setsid 등 세션 설정
   | 4. execve
   v
 [서비스]
   (TTY 없음, stdin=/dev/null, stdout/stderr=journal 파이프)
```

그래서 `systemctl start foo.service` 했을 때:
- 서비스 프로세스의 부모는 systemd (PID 1)
- 터미널에 출력 안 뜸 (stdout 이 journald 파이프로 연결됨)
- journald 가 그걸 받아 `/var/log/journal/...` 에 저장
- `journalctl -u foo.service` 로 볼 수 있음

---

## §7. 실무 도구 매핑

| 도구 | 터미널 분리? | fd 처리 | 동작 요약 |
| --- | --- | --- | --- |
| `./prog` (포그라운드) | 안 함 | stdin/out/err 상속 | 쉘이 fork+exec. 컨트롤링 TTY 유지 |
| `./prog &` (백그라운드) | 안 함 | stdin/out/err 상속 | 포그라운드 pgid 에서 빠짐. Ctrl+C 안 먹음. 쉘 종료 시 SIGHUP |
| `nohup ./prog &` | 반쯤 함 | stdin=/dev/null, stdout=nohup.out | SIGHUP 무시. 쉘 종료 후에도 살아남음 |
| `setsid ./prog` | 함 | 상속 | 새 세션. TTY 완전 분리. 한 번 fork 로 끝 |
| `disown %1` | 사후 분리 | 변화 없음 | 쉘의 job 테이블에서만 빠짐 (SIGHUP 안 보냄) |
| `systemd-run ./prog` | 함 | journald 로 | 임시 서비스로 떠서 systemd 가 관리 |
| 서비스 `ExecStart=` | 함 | 설정대로 | systemd 가 cgroup+fd+env 전부 준비 |
| `daemon(3)` 라이브러리 | 함 | `/dev/null` | `daemon(nochdir, noclose)` 으로 double-fork |

### nohup 실제 동작

```
 nohup CMD ARGS
 ───────
 1. 현재 fork -> 자식
 2. 자식이 SIGHUP 을 SIG_IGN 으로 설정
 3. stdin 을 /dev/null 로 redirect
 4. stdout 이 터미널이면 ./nohup.out (또는 ~/nohup.out) 으로 redirect
 5. stderr 는 stdout 과 같이
 6. execve(CMD, ARGS)
```

---

## §8. 같은 ./server 를 다섯 가지 방식으로 띄우기

```c
// server 라는 프로그램이 있다고 치자.
// 터미널에 "hello" 를 찍고 60 초 sleep 하는 프로그램.
// 각 띄우기 방식에서 fd / TTY / 부모 관계가 어떻게 달라지는지.
```

### (1) 포그라운드

```
 $ ./server
 hello                                    <- 터미널에 뜸
 (60초간 Ctrl+C 로 죽일 수 있음)

 구조:
   bash (pid=3000, sess=3000, pgid=3000, TTY=/dev/pts/0)
     └─ server (pid=4000, sess=3000, pgid=4000, TTY=/dev/pts/0)
                fd[0,1,2] = /dev/pts/0 (상속)
                포그라운드 pgid = 4000 (bash 가 설정)

 Ctrl+C 동작:
   TTY 가 "포그라운드 pgid=4000" 에 SIGINT 전달 -> server 죽음 -> bash 돌아옴
```

### (2) 백그라운드 &

```
 $ ./server &
 [1] 4000
 $ hello                                  <- 여전히 뜸 (stdout 상속)

 구조:
   bash (pid=3000, ..., 포그라운드 pgid=3000)
     └─ server (pid=4000, sess=3000, pgid=4000, TTY=/dev/pts/0)
                포그라운드 pgid 아님

 Ctrl+C:  SIGINT 는 포그라운드 pgid=3000 (bash) 로만 -> server 안 죽음
 bash 종료: bash 가 SIGHUP 을 job 들에 보냄 -> server 죽음 (디폴트 액션)
```

### (3) nohup &

```
 $ nohup ./server > out.log 2>&1 &

 구조:
   bash
     └─ server
          + fd[0] = /dev/null
          + fd[1] = out.log
          + fd[2] = out.log
          + SIGHUP = SIG_IGN

 bash 종료해도 server 살아남음 (SIGHUP 무시)
 하지만 여전히 sess=3000 (bash 의 세션) 에 속해 있음
```

### (4) setsid (진짜 분리)

```
 $ setsid ./server > out.log 2>&1 < /dev/null &

 구조:
   bash
     └─ (setsid 프로그램이 fork+setsid 한 결과)
            └─ server (pid=4000, sess=4000, pgid=4000, TTY=none)

 완전히 분리. TTY 도 없고 세션도 다름.
```

### (5) systemd-run

```
 $ systemd-run --unit=myserver ./server

 구조:
   systemd (pid=1)
     └─ server
          + cgroup: /system.slice/myserver.service
          + fd[0] = /dev/null
          + fd[1,2] = journald 소켓
          + sess = 새 세션
          + TTY 없음
          + 자동 restart 가능 (Unit 설정 시)
```

---

## 요약 체크리스트

```
 [ ] 모든 유저 프로세스는 PID 1 (init/systemd) 의 자손이다
 [ ] PID 0, 1, 2 는 fork 가 아니라 커널이 start_kernel 에서 직접 구성한다
 [ ] fork() 는 기본적으로 fdtable 을 복제한다 (같은 struct file 공유)
 [ ] CLONE_FILES 를 주면 아예 같은 files_struct 를 공유한다 (pthread 방식)
 [ ] fd 를 상속 안 하게 하려면:
      - O_CLOEXEC / FD_CLOEXEC
      - execve 전 explicit close
      - posix_spawn + file_actions
      - /proc/self/fd 스캔 후 전부 close
 [ ] 터미널 분리는 fd 차원이 아니라 세션 차원의 일이다 (setsid)
 [ ] 데몬 = double-fork + setsid + /dev/null redirect
 [ ] systemd 로 띄운 서비스는 double-fork 불필요. systemd 가 부모 역할 다 함
```

## 참고 연결

- [q13-cgi-fork-args.md](./q13-cgi-fork-args.md) — Tiny 서버가 CGI 자식을 fork+dup2+execve 로 띄우는 코드 레벨 동작
- [q04-filesystem.md](./q04-filesystem.md) — struct file / fdtable / VFS 구조
- [q18-thread-concurrency.md](./q18-thread-concurrency.md) — 스레드의 CLONE_FILES 공유와 대조되는 관점
