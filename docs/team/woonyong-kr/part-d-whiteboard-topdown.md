# Part D. OS 기반 (FD · 프로세스 · 메모리) — 화이트보드 탑다운 발표안

이 문서는 Part D 발표를 위한 **실전용 화이트보드 원고**다.
목표는 네트워크 위에 앉아 있는 OS 의 뼈대 — **VFS · FD/FDT · 프로세스 족보 · 가상 메모리** — 를 설명하고, 왜 이게 소켓 동작의 전제가 되는지를 **실제 커널 자료구조와 /proc 관찰** 로 끝까지 밀어붙이는 것이다.

Part D 는 "모든 것은 파일" 이라는 캐치프레이즈를 구호가 아니라 **검증 가능한 구조** 로 풀어낸다.

## Part D 에서 끝까지 밀고 갈 한 문장

```text
소켓도 결국 fd 고, fd 는 프로세스별 FDT 를 통해 struct file,
그 아래 v-node/i-node(또는 struct socket) 로 내려가며,
프로세스와 그들이 잡은 fd 는 fork/dup2 로 복제되고,
주소 공간은 스택/힙/mmap/브리지된 커널 메모리로 나뉘어
demand paging 으로 실제 물리 메모리에 연결된다.
```

## 발표 전에 칠판에 미리 고정할 숫자

```text
조사용 프로세스
  shell      PID = 500
  ./tiny 8080 PID = 1234, PPID = 500
  child CGI   PID = 1300, PPID = 1234

표준 fd 약속
  0 = stdin  1 = stdout  2 = stderr
  3 = listen socket
  4 = accept socket

VM 레이아웃 (예시, x86_64 userland)
  0x0000_0000_0040_0000  .text 시작
  0x0000_5555_5555_6000  heap start (brk)
  0x0000_7ffe_????_????  stack top
  0x0000_7fff_????_????  kernel 경계

메모리 상태 예시
  VSZ = 24 MB
  RSS =  4 MB
```

## 화이트보드 배치

```text
+--------------------------------------------------------------------------------+
| 상단: "fd -> file -> (v-node|socket) -> inode/sock"                            |
+--------------------------------------+-----------------------------------------+
| 왼쪽: OS 자료구조 체인                | 오른쪽: /proc · strace 실물 증거         |
| task_struct / fdtable / file / inode | /proc/PID/fd , /proc/PID/maps , strace  |
+--------------------------------------+-----------------------------------------+
| 하단: 끝까지 남길 키워드                                                     |
| VFS / FDT / dup2 / fork / reparent / O_CLOEXEC / brk / mmap / VSZ / RSS / OOM |
+--------------------------------------------------------------------------------+
```

## 발표 흐름 전체 지도

```text
Scene 1   "모든 것은 파일" 선언 + 왜 네트워크 위가 아니라 OS 밑바닥부터 가는가
Scene 2   VFS 4객체: superblock / inode / dentry / file
Scene 3   FD / FDT / file table / v-node / i-node 의 연결
Scene 4   표준 0/1/2, 터미널 vs GUI 실행
Scene 5   dup2 로 fd 번호 재배치, CGI 연결
Scene 6   fork 의 fdtable 복제 + O_CLOEXEC
Scene 7   프로세스 족보: PID 1, reparent, subreaper
Scene 8   close / refcount, TCP vs UDP close
Scene 9   가상 메모리 레이아웃 (stack/heap/mmap)
Scene 10  demand paging / VSZ vs RSS / OOM / adder.c 마무리
```

---

## Scene 1. 네트워크보다 먼저: 왜 OS 밑바닥부터 보나

칠판에 가장 먼저 이 한 줄을 쓴다.

```text
socket       <- 사실은 fd
fd           <- 사실은 FDT 엔트리
FDT 엔트리   <- 사실은 struct file*
struct file* <- 사실은 v-node / socket / pipe / ...
```

그리고 이렇게 말한다.

`Part A, B, C 는 소켓을 중심으로 이야기했지만, 소켓은 결국 fd 이고 fd 는 OS 가 관리합니다. 그 밑바닥을 한 번 뜯어야 Part A, B, C 의 이야기가 다시 단단해집니다.`

이 장면에서 반드시 짚을 것:

- fd 는 정수 한 개다.
- 이 정수는 프로세스별 테이블의 인덱스다.
- 그 테이블 엔트리는 커널의 `struct file` 을 가리킨다.
- `struct file` 은 종류에 따라 v-node/inode 로, 혹은 `struct socket -> struct sock` 으로 내려간다.

꼭 남길 문장:

`"모든 것은 파일이다" 는 구호가 아니라, fd 라는 같은 인덱스로 파일·소켓·파이프·장치를 꺼내 쓸 수 있다는 구조적 사실입니다.`

### 직접 검증 — fd 로 꺼낼 수 있는 "객체" 가 진짜로 다양하다

```bash
# 내 셸 프로세스가 들고 있는 fd 종류
ls -l /proc/$$/fd
# lrwx... 0 -> /dev/pts/0
# lrwx... 1 -> /dev/pts/0
# lrwx... 2 -> /dev/pts/0
# lrwx... 3 -> /tmp/log.txt
# lrwx... 4 -> socket:[123]
# lrwx... 5 -> anon_inode:[eventpoll]
# lrwx... 6 -> pipe:[456]
```

`socket:[...]`, `anon_inode:[...]`, `pipe:[...]` 이 같은 테이블에서 같은 형식으로 찍힌다. **그게 곧 "모든 것이 파일" 의 실물.**

---

## Scene 2. VFS 4객체 — superblock / inode / dentry / file

칠판에 네 개 객체를 네모로 그린다.

```text
+-----------+    +-----------+    +-----------+    +-----------+
| superblk  |--->|   inode   |--->|  dentry   |--->|   file    |
+-----------+    +-----------+    +-----------+    +-----------+
 파일시스템      파일 메타      이름/경로       열린 핸들
 전체 정보      (i번호)        캐시          (refcount/offset)
```

정확한 역할도 옆에 적는다.

```text
superblock  = mount 된 파일시스템 한 개의 메타 (ext4, tmpfs, ...)
inode       = 실제 파일 하나 (크기, 권한, 블록 포인터, mode)
dentry      = "경로의 한 조각" <-> inode 매핑, path walk 의 캐시
file        = open 한 번마다 생기는 사용 중 상태 (offset, flags)
```

꼭 말할 문장:

`파일 "하나" 는 inode 한 개이지만, 여러 번 open 하면 struct file 은 각기 다르게 생깁니다. 그래서 같은 파일을 여러 fd 로 열어도 offset 이 독립적일 수 있습니다.`

초보자가 자주 헷갈리는 것:

- inode 와 file 은 다르다. inode = "파일 자체", file = "열린 핸들".
- dentry 는 경로 -> inode 의 캐시지, 파일 본체가 아니다.

### 직접 검증 — inode, dentry, 열린 파일 찾아보기

```bash
# (1) inode 번호 = 파일시스템이 이 파일을 식별하는 id
ls -li /etc/hostname
# 1234567 -rw-r--r-- 1 root root 10 ...

# (2) 같은 파일을 두 번 open 해도 file 은 각자 생긴다
python3 - <<'PY'
f1 = open('/etc/hostname'); f2 = open('/etc/hostname')
import os
print('f1.fd =', f1.fileno(), 'f2.fd =', f2.fileno())
print('offset f1/f2 =', f1.tell(), f2.tell())
f1.read(3)
print('after read3, f1/f2 offset =', f1.tell(), f2.tell())
PY
# f1 읽으면 f1.offset 만 3, f2.offset 은 그대로 0

# (3) 마운트된 superblock 목록
mount | head -5                 # 커널 시점의 mount 테이블
cat /proc/self/mountinfo | head -3
```

화이트보드에서 강조: 동일 inode 에 대해 `f1.tell() != f2.tell()` 이 된 그 순간이 **`struct file` 이 두 개로 갈라졌다** 는 증거다.

---

## Scene 3. FD / FDT / file table / v-node / i-node — 전체 체인

Part D 의 중심 그림이다. 이 체인을 칠판에 크게 그린다.

```text
User process (task_struct)
     |
     v
 files_struct
     |
     v
  FDT (fd array)
     0 ---------> struct file*  (/dev/pts/0)
     1 ---------> struct file*  (/dev/pts/0)
     2 ---------> struct file*  (/dev/pts/0)
     3 ---------> struct file*  ->  struct socket  ->  struct sock (TCP)
     4 ---------> struct file*  ->  struct inode   ->  block device

file table (struct file)
     - f_pos (offset)
     - f_flags (O_RDWR, O_NONBLOCK, ...)
     - f_op  (VFS 가 호출할 함수 포인터)
     - f_count  (refcount)
     - private_data -> struct socket / inode / ...
```

그리고 아래 문장을 못박는다.

`유저가 "fd 3" 이라고 말하면, 커널은 task_struct -> files_struct -> FDT[3] -> struct file* -> (socket|inode) 순으로 따라갑니다.`

실제 사례를 두 개 붙인다.

```text
열린 파일:
  FDT[4] -> file_A -> inode -> 디스크의 block group / data blocks

열린 소켓:
  FDT[3] -> file_B -> socket -> sock -> TCP 상태 머신 / sk_buff queue
```

### 직접 검증 — fd 한 개를 커널 객체로 끝까지 추적

```bash
# 조사용 서버를 띄운다
python3 -m http.server 8080 >/dev/null &
SRV=$!

# (1) fd -> 대상 객체
ls -l /proc/$SRV/fd
# lrwx... 3 -> socket:[100]
# lrwx... 4 -> /home/.../file.html  (stat 했다면)

# (2) 같은 fd 의 상세 상태 (file table 레이어)
cat /proc/$SRV/fdinfo/3
# pos:    0
# flags:  02000002           <- O_RDWR|O_NONBLOCK 등
# mnt_id: 15
# ino:    100

# (3) 소켓 inode 번호를 소켓 상태와 매칭
ss -tanpie | awk -v pid=$SRV '$0 ~ pid { print }'
# LISTEN ... ino:100 dev:... users:(("python3",pid=SRV,fd=3))

# (4) /proc/net/tcp 에서 같은 inode 검색
printf '%x\n' 100                # hex 변환
grep -i $(printf '%08x' 100) /proc/net/tcp | head -3
```

화이트보드에서 강조: `ls -l /proc/PID/fd/3` 가 가리키는 `socket:[100]` → `ss ino:100` → `/proc/net/tcp` 의 같은 inode. **세 곳에서 같은 숫자 100** 이 나오는 것이 Scene 3 체인의 실물 증거.

---

## Scene 4. 0/1/2 는 왜 항상 stdin/stdout/stderr 인가

칠판에 먼저 규칙을 적는다.

```text
규칙
  fd 번호는 사용 가능한 가장 작은 정수
  0, 1, 2 는 open 한 게 아니라, 부모가 준비해 둔 것을 상속받은 것
```

그리고 두 가지 실행 경로를 비교한다.

```text
터미널에서 실행
  shell 이 /dev/pts/N 을 open -> dup2 로 0/1/2 에 꽂음
  그 상태로 fork/execve -> 자식이 같은 /dev/pts/N 을 상속

GUI 아이콘 클릭 실행
  Finder/systemd 같은 런처가 새 프로세스를 만들 때
  /dev/null 을 0/1/2 에 꽂거나
  log 파일을 꽂기도 한다
```

핵심 설명:

- 커널은 "0/1/2 는 stdin/stdout/stderr" 라는 것을 **강제하지 않는다**. libc 의 `stdin`, `stdout`, `stderr` 라는 FILE* 변수들이 관례적으로 그 번호를 사용할 뿐이다.
- 즉 0/1/2 는 **관례 + 런처가 세팅** 의 결과물이다.

### 직접 검증 — 같은 프로그램, 다른 런처, 다른 fd 연결

```bash
# (1) 터미널에서 실행한 프로세스의 0/1/2
sleep 60 &
P=$!
for i in 0 1 2; do readlink /proc/$P/fd/$i; done
# /dev/pts/0
# /dev/pts/0
# /dev/pts/0

# (2) 백그라운드 데몬 스타일 (nohup) 로 실행한 경우
nohup sleep 60 >/tmp/out </dev/null 2>&1 &
P=$!
for i in 0 1 2; do readlink /proc/$P/fd/$i; done
# /dev/null
# /tmp/out
# /tmp/out

# (3) systemd 서비스로 띄운 경우 (참고용)
# 0 -> /dev/null, 1/2 -> journald socket 인 경우가 많다
```

화이트보드에서 강조: Scene 4 의 표 "터미널 vs GUI" 가 `readlink` 두 세트 출력과 **1:1 로 같다**.

---

## Scene 5. dup2 로 fd 를 갈아끼운다

칠판에 3단계를 그린다.

```text
상태 A                       상태 B (dup2(3, 1) 후)
  FDT[0] -> pts               FDT[0] -> pts
  FDT[1] -> pts               FDT[1] -> socket   (원래 FDT[1] 는 close)
  FDT[2] -> pts               FDT[2] -> pts
  FDT[3] -> socket            FDT[3] -> socket   (둘이 같은 struct file* 가리킴)
```

정확한 문장:

`dup2(src, dst) 는 "dst 번을 src 와 같은 객체로 만들어라" 입니다. 원래 dst 에 있던 것은 먼저 close 됩니다.`

CGI 연결도 그대로 적는다.

```text
CGI 에서
  connfd = 4
  dup2(connfd, 1)   -> stdout 이 소켓을 가리킴
  execve(cgi_prog)  -> printf() 한 내용이 바로 socket 으로 write
```

Part C 의 Scene 6 과 **정확히 같은 그림** 이라는 점을 짚는다.

### 직접 검증 — dup2 로 stdout 갈아끼우기

```bash
# (1) 가장 단순한 dup2 실험: stdout 을 파일로
bash -c 'exec 1>/tmp/dup2-demo.txt; echo hello; ls /proc/self/fd/1 -l'
cat /tmp/dup2-demo.txt
# hello
# /proc/self/fd/1 -> /tmp/dup2-demo.txt   <- dup2 의 효과

# (2) strace 로 dup2 를 직접 관찰
strace -e trace=dup2,openat,write bash -c 'exec 3>/tmp/x; dup2 3 1 2>/dev/null; echo via-dup2'
# openat(AT_FDCWD, "/tmp/x", O_WRONLY|O_CREAT, 0666) = 3
# dup2(3, 1)                                         = 1
# write(1, "via-dup2\n", 9)                          = 9

# (3) Scene 5 의 "원래 dst 는 close" 도 직접 확인
python3 - <<'PY'
import os
f = open('/tmp/y','w')
print('before dup2, f.fileno =', f.fileno())
os.dup2(f.fileno(), 1)                  # stdout 을 f 로
print('after dup2, this line goes to /tmp/y not terminal')
PY
cat /tmp/y   # "after dup2, ..."
```

화이트보드에서 강조: `readlink /proc/self/fd/1` 가 `/dev/pts/0` → `/tmp/...` 로 바뀌는 순간이 dup2 의 실물.

---

## Scene 6. fork 는 FDT 까지 복제한다 + O_CLOEXEC

칠판에 부모/자식 FDT 를 나란히 그린다.

```text
부모 (pre-fork)            부모 (post-fork)           자식 (post-fork)
  FDT[0]..[2] -> pts         FDT[0]..[2] -> pts         FDT[0]..[2] -> pts
  FDT[3] -> socket A         FDT[3] -> socket A         FDT[3] -> socket A
  FDT[4] -> file B           FDT[4] -> file B           FDT[4] -> file B

  struct file A.count = 1    struct file A.count = 2    (같은 객체, refcount 증가)
```

중요한 문장:

- fork 는 task_struct, vm, fdtable 을 모두 복제한다.
- FDT 엔트리는 복제되지만 **가리키는 struct file 은 공유** 된다.
- 그래서 같은 offset/상태가 부모/자식에서 공유된다.

O_CLOEXEC 는 여기서 등장:

```text
O_CLOEXEC 플래그가 붙은 fd 는
  fork 는 통과하지만
  execve 직후 커널이 자동으로 close 한다
```

꼭 남길 문장:

`dup2 는 "명시적 공유" 를 위한 도구이고, O_CLOEXEC 는 "의도하지 않은 상속을 막는" 도구입니다.`

### 직접 검증 — fork fdtable 공유 & O_CLOEXEC

```bash
# (1) 공유 offset: 부모 자식이 같은 파일을 번갈아 쓰면 offset 이 섞인다
python3 - <<'PY'
import os
f = open('/tmp/shared.log','w')
os.write(f.fileno(), b'A'*4 + b'\n')
pid = os.fork()
if pid == 0:   # child
    os.write(f.fileno(), b'CHILD\n')
    os._exit(0)
os.waitpid(pid, 0)
os.write(f.fileno(), b'PARENT\n')
f.close()
PY
cat /tmp/shared.log
# AAAA
# CHILD
# PARENT       <- 같은 offset 을 공유했으므로 순차 기록

# (2) refcount 가 2 가 되었다는 간접 증거: 부모가 close 해도 자식이 쓸 수 있다
# (위 예시에서 CHILD 가 fork 직전 열린 f 를 그대로 썼다는 사실 자체)

# (3) O_CLOEXEC 동작
python3 - <<'PY'
import os, fcntl
r, w = os.pipe2(os.O_CLOEXEC)           # Linux
print('flags =', hex(fcntl.fcntl(r, fcntl.F_GETFD)))   # FD_CLOEXEC = 1
PY
# flags = 0x1         <- execve 시 close 됨

# (4) 실제로 exec 을 해 보면 그 fd 는 사라진다
bash -c 'exec 3< /etc/hostname; python3 -c "import os; os.execvp(\"ls\", [\"ls\",\"-l\",\"/proc/self/fd\"])"' 2>&1 | head
# fd 3 가 살아남는지 여부가 O_CLOEXEC 여부로 결정된다
```

화이트보드에서 강조: `/tmp/shared.log` 출력 순서가 `AAAA → CHILD → PARENT` 로 **섞이지 않고 이어 붙는 것** 이 "FDT 는 복제, struct file 은 공유" 의 직접 증거.

---

## Scene 7. 프로세스 족보 — PID 1, reparent, subreaper

칠판에 트리를 그린다.

```text
PID 1 (systemd / init)
 ├─ sshd (100)
 │   └─ bash (500)
 │        └─ tiny (1234)
 │             └─ CGI child (1300)
 └─ dbus-daemon / cron / ...
```

핵심 문장 네 개.

- 모든 프로세스는 fork 의 사슬을 타고 올라가면 **PID 1** 에 닿는다.
- PID 1 은 커널이 처음 만든 유저 프로세스이고, 보통 **systemd** 다.
- 부모가 먼저 죽어 "고아" 가 된 자식은 PID 1 로 **reparent** 된다.
- `PR_SET_CHILD_SUBREAPER` 를 건 프로세스가 있으면 그 프로세스가 PID 1 을 대신해 reparent 를 받는다 (tini, systemd --user 등).

터미널이 누구의 자식인가도 같이 설명한다.

```text
"터미널에서 ls" -> bash 가 부모
"더블클릭으로 크롬" -> Finder / shell 이 부모가 아니라 launchd/systemd 가 부모
"크롬 있는데 엣지를 열면" -> 엣지는 크롬의 자식이 아니라 런처의 자식
```

### 직접 검증 — 족보·reparent·subreaper

```bash
# (1) PID 1 의 정체
ps -o pid,comm -p 1
# 1  systemd   (Linux) 또는 1 init / launchd (macOS)

# (2) 족보 시각화
pstree -p $$ | head -5
# systemd(1)---sshd(100)---sshd(...)---bash(500)---sleep(1234)

# (3) 고아 -> init 으로 reparent 되는 장면 직접 만들기
python3 - <<'PY' &
import os, time
pid = os.fork()
if pid == 0:             # child
    time.sleep(5)        # 부모가 먼저 죽기를 기다림
    print('child ppid =', os.getppid())
    os._exit(0)
else:
    print('parent pid =', os.getpid(), ' child =', pid)
    os._exit(0)          # 부모 먼저 죽음
PY
# 수 초 뒤
# child ppid = 1         <- reparent 됐음을 ps/출력으로 확인
# (subreaper 가 있다면 PID 1 대신 그 PID 로 바뀐다)

# (4) subreaper 설정 실험
python3 - <<'PY'
import ctypes
libc = ctypes.CDLL('libc.so.6', use_errno=True)
PR_SET_CHILD_SUBREAPER = 36
libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)
# 이 프로세스의 자손 중 고아가 되는 놈은 나한테 reparent 된다
PY
```

화이트보드에서 강조: `ppid = 1` 이 찍히는 순간이 **reparent 의 실물**. subreaper 예제에서는 같은 자리에 부모의 PID 가 대신 찍힌다.

---

## Scene 8. close 와 refcount — TCP vs UDP close 도 같이

칠판에 refcount 그림을 그린다.

```text
struct file A (refcount = 3)
   ^      ^      ^
   |      |      |
 parent  child1 child2    <- 세 프로세스가 같은 file 공유
                             close 시마다 refcount--
                             refcount == 0 일 때 실제 해제
```

꼭 남길 문장:

`close(fd) 는 내 FDT 엔트리를 지우고 struct file 의 refcount 를 1 감소시킬 뿐입니다. 실제 자원 회수는 refcount 가 0 이 될 때 일어납니다.`

TCP vs UDP close 도 비교한다.

```text
TCP close
  마지막 close -> struct file 해제 -> struct sock 상태 전이
  FIN 전송 -> ACK 기다림 -> TIME_WAIT -> 해제
  그래서 close(fd) 가 즉시 끝나는 듯 보여도 커널 내부는 수초 유지

UDP close
  상태 없음 -> 즉시 해제
```

### 직접 검증 — refcount 와 TCP 종료 잔상

```bash
# (1) /proc/PID/fdinfo 로 refcount 직접 보기 (커널 5.x+)
grep . /proc/$$/fdinfo/0 2>/dev/null | head
# pos/flags/... 이 찍힘. refcount 는 직접 변수 노출은 없지만
# 같은 struct file 을 공유하는 PID 를 나열해서 간접 확인 가능
sudo find /proc -maxdepth 3 -lname "socket:\[100\]" 2>/dev/null
# /proc/1234/fd/3
# /proc/1300/fd/3        <- 두 개 이상 잡혀 있으면 공유 = refcount>=2

# (2) close(fd) 시점과 커널 상태
python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1',8080))
print('before close fd=', s.fileno())
s.close()
print('after close, process continues')
time.sleep(10)
" &
sleep 0.2
ss -tan state time-wait '( dport = :8080 or sport = :8080 )'
# ... 수 초 동안 TIME-WAIT 잔상이 보인다

# (3) UDP 는 즉시 끝난다
python3 -c "
import socket
s = socket.socket(type=socket.SOCK_DGRAM); s.bind(('127.0.0.1',9100)); s.close()
"
ss -uan | grep 9100   # 즉시 사라짐
```

화이트보드에서 강조: TCP 쪽의 `TIME-WAIT` 엔트리와 UDP 쪽의 즉시 사라짐이 Scene 8 의 두 박스를 그대로 보여 준다.

---

## Scene 9. 가상 메모리 레이아웃 — stack / heap / mmap

칠판 오른쪽에 세로로 크게 그린다.

```text
high
  +---------------------+  0x7fff_ffff_ffff  (user 최상단)
  |       kernel         |  <- user 모드로는 접근 불가
  +---------------------+
  |       stack          |  <- 아래로 자람
  |         |            |
  |         v            |
  +---------------------+
  |                      |  <- 빈 구간
  |       mmap area      |  <- shared lib / 파일 매핑 / malloc(big)
  +---------------------+
  |         ^            |
  |         |            |
  |       heap           |  <- 위로 자람 (brk/sbrk)
  +---------------------+
  |       bss / data     |
  +---------------------+
  |       text / ro      |
  +---------------------+  0x00400000
low
```

꼭 짚을 포인트:

- 스택은 위에서 아래로, 힙은 아래에서 위로 자란다. 교과서 그림과 정확히 같다.
- `malloc` 은 작을 때 heap (brk), 크면 mmap 을 쓴다 (glibc 기본 128KB 경계).
- shared library (libc 등) 는 mmap 영역에 들어와 있다.

"libc 는 어디에 있나" 질문의 답:

`libc 는 유저 가상 메모리의 mmap 영역에 매핑되어 있습니다. 커널 메모리가 아니고, heap 도 아니고, 프로세스마다 별도로 매핑됩니다.`

### 직접 검증 — /proc/self/maps 로 전체 레이아웃 뽑기

```bash
# (1) 현재 셸의 VM 레이아웃
cat /proc/$$/maps | head -20
# 00400000-0040b000 r-xp ... /usr/bin/bash             <- text
# 0060a000-0060d000 rw-p ... /usr/bin/bash             <- data
# 020e5000-024c4000 rw-p ...                           <- heap
# 7f...-7f...      r-xp ... /lib/x86_64-linux-gnu/libc.so.6  <- mmap libc
# 7ffe...          rw-p ...                            [stack]

# (2) 실제 주소가 Scene 9 의 그림과 같은지 확인
cat /proc/$$/maps | grep -E 'heap|stack|libc'

# (3) pmap 으로 요약
pmap $$

# (4) malloc 크기에 따라 brk/mmap 갈리는 것 증명
python3 - <<'PY'
import os
with open(f'/proc/{os.getpid()}/maps') as f: before = f.read().count('\n')
b = bytearray(256*1024*1024)       # 256 MB -> mmap 영역에 뜬다
with open(f'/proc/{os.getpid()}/maps') as f: after  = f.read().count('\n')
print('maps lines before/after =', before, after)
PY
# before/after 가 1 이상 증가 = 새 mmap 영역 생성
```

화이트보드에서 강조: `cat /proc/self/maps` 가 그대로 **Scene 9 그림** 이다. 매핑 이름 (`libc.so.6`, `[heap]`, `[stack]`) 이 교과서 용어를 그대로 따라온다.

---

## Scene 10. demand paging · VSZ/RSS · OOM · adder.c 마무리

칠판에 demand paging 원리를 쓴다.

```text
mmap / malloc / brk       = VSZ 증가 (가상 주소 예약)
처음 접근 (read/write)    = page fault -> 물리 page 할당 -> RSS 증가
```

그 자리에 공식을 박는다.

```text
VSZ = 이 프로세스가 "보고 있는" 가상 공간의 크기
RSS = 실제로 물리 메모리에 올라와 있는 양
Overcommit = VSZ 합이 실제 RAM 을 초과해도 커널이 일단 허락
```

OOM 도 여기서 마무리:

```text
RSS 가 물리 한계를 넘어서면
-> 커널이 OOM killer 를 깨움
-> oom_score 가 높은 프로세스를 강제 종료
```

마지막으로 **adder.c 코드 디테일** 로 Part D 를 닫는다.

```text
CGI 예제 adder.c 에서는 왜 printf() 직접 쓰지 않고
sprintf 로 content 문자열을 먼저 만드는가?

=> Content-length 가 헤더에 먼저 나와야 하므로,
   body 를 다 만든 뒤 strlen() 을 재서 Content-length 로 박아야 한다.
   그래서 body 를 "버퍼" 에 먼저 쌓고, 그 길이를 재고, 헤더 + body 순서로 출력한다.

=> "=" 로 문자열을 넣는 것이 안 되는 것은 char buf[] = "..." 외에는
   C 에서 포인터 할당과 문자열 복사를 혼동하면 안 되기 때문.
   배열에 문자열을 "채우는" 것은 strcpy / sprintf 를 써야 한다.
```

### 직접 검증 — demand paging / VSZ-RSS / OOM / adder

```bash
# (1) demand paging 관찰: VSZ 만 올리고 RSS 는 그대로
python3 - <<'PY' &
b = bytearray(1_000_000_000)   # 1 GB 예약, 초기값 0 -> 일부만 물리 페이지
import time; time.sleep(30)
PY
P=$!
ps -o pid,vsz,rss,comm -p $P
# PID       VSZ       RSS  COMM
# 1234  1049000   1024000  python3          (값은 환경마다 다름)

# (2) 페이지를 "만지면" RSS 가 튀는 걸 본다
python3 - <<'PY'
import os, time, mmap
m = mmap.mmap(-1, 500*1024*1024)   # VSZ + 500MB, 아직 RSS 는 소량
time.sleep(2)
for i in range(0, 500*1024*1024, 4096):   # 모든 페이지 터치
    m[i] = 1
time.sleep(5)
PY
# 관찰하는 쪽: while true; do ps -o rss= -p $(pgrep -n python3); done

# (3) oom_score
cat /proc/$P/oom_score
cat /proc/$P/status | grep -E 'VmPeak|VmSize|VmRSS|VmData'

# (4) adder.c 검증 — 실제 빌드해서 돌려 보기
cat > /tmp/adder.c <<'C'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    char *qs = getenv("QUERY_STRING");
    int a=0,b=0;
    if (qs) sscanf(qs, "%d&%d", &a, &b);
    char content[512];
    sprintf(content, "Welcome to add.com: %d + %d = %d", a, b, a+b);
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    return 0;
}
C
cc /tmp/adder.c -o /tmp/adder
QUERY_STRING='15000&213' /tmp/adder
# Content-length: 36
# Content-type: text/html
#
# Welcome to add.com: 15000 + 213 = 15213
```

화이트보드에서 강조: VSZ 는 크지만 RSS 는 작게 시작해서, 페이지를 만질 때마다 RSS 가 튀어오르는 그 관측값이 바로 **demand paging 의 실물**. adder.c 의 `strlen(content)` 이 `Content-length:` 로 들어가는 이유가 코드로 확정된다.

---

## 발표 10분 압축 버전

```text
1. "모든 것은 fd" 선언
2. VFS 4객체 (superblock / inode / dentry / file)
3. FDT -> struct file -> (socket|inode)
4. 0/1/2 관례와 터미널/GUI 차이
5. dup2 로 fd 갈아끼우기 (CGI 연결)
6. fork 의 fdtable 공유 + O_CLOEXEC
7. PID 1 / reparent / subreaper
8. close / refcount / TCP-UDP 종료 차이
9. VM 레이아웃 (/proc/self/maps)
10. demand paging / VSZ-RSS / OOM / adder.c 마무리
```

## 질문 받으면 어디까지 내려갈지

- `소켓도 정말 파일인가요?`
  - Scene 1, 3 으로 내려가 `ls -l /proc/PID/fd` 에 `socket:[...]` 가 나오는 걸 다시 보여 준다.

- `fd 가 같은 번호인데 어떻게 프로세스별로 다른가요?`
  - Scene 3 의 `task_struct -> files_struct -> FDT` 그림으로 돌아간다.

- `dup2 가 왜 CGI 에서 중요한가요?`
  - Scene 5 로 내려가 `readlink /proc/self/fd/1` 가 `socket:[...]` 이 되는 것을 보여 준다.

- `fork 하면 socket 도 복제되나요?`
  - Scene 6 의 "FDT 는 복제, struct file 은 공유 (refcount++)" 를 다시 적는다.

- `close 했는데 왜 TIME_WAIT 가 남나요?`
  - Scene 8 의 TCP vs UDP 비교로 간다.

- `malloc 이 1 GB 해도 왜 컴퓨터가 멀쩡한가요?`
  - Scene 10 의 VSZ vs RSS / demand paging / overcommit 을 다시 설명한다.

- `adder.c 는 왜 sprintf 를 쓰나요?`
  - Scene 10 의 Content-length 먼저 계산 이유를 다시 말한다.

## 발표 중 한 화면에 띄울 검증 치트시트

```bash
# fd 체인
ls -l /proc/$$/fd ; cat /proc/$$/fdinfo/<N>
ss -tanpie ; grep -i <hex-ino> /proc/net/tcp

# 0/1/2
for i in 0 1 2; do readlink /proc/$$/fd/$i; done

# dup2 / fork / O_CLOEXEC
strace -e trace=dup2,clone,execve,pipe2 bash -c '...'
python3 -c 'import os; os.pipe2(os.O_CLOEXEC)'

# 족보
ps -o pid,ppid,comm -p 1 ; pstree -p $$

# VM
cat /proc/$$/maps ; pmap $$
ps -o pid,vsz,rss,comm -p <PID>
cat /proc/<PID>/status | grep Vm
cat /proc/<PID>/oom_score
```

## 연결 문서

- `q04-filesystem.md`
- `q19-process-ancestry-fd-inheritance.md`
- `q20-fd-lifecycle-and-dispatch.md`
- `q21-process-parent-and-memory-deep-dive.md`
