# q21. 프로세스 부모 결정 기준 · subreaper · 가상 메모리 · heap vs mmap · demand paging

## 질문

```
 1.  "크롬이 떠 있는데 엣지를 열면 엣지는 크롬의 자식인가?" 부모가 되는 기준이 뭔가?
 2.  터미널에서 프로그램을 띄우면 터미널이 부모인가? 셸(bash)이 부모인가?
 3.  자식이 먼저 죽으면 고아가 된 손자는 "할아버지" 로 reparent 되는가?
 4.  subreaper 플래그는 어디서 언제 켜는가? systemd --user, tini 같은 게 이걸 쓰나?
 5.  libc 는 가상 메모리의 어디에 있는가? 힙 영역인가? 커널 메모리인가?
 6.  가상 메모리 레이아웃 다이어그램의 방향 — 스택이 위에서 아래로, 힙이 아래에서 위로 — 맞는가?
 7.  힙(brk/sbrk)과 mmap 은 뭐가 다른가?
 8.  mmap 을 호출하면 즉시 물리 메모리가 할당되는가? 힙은? (demand paging 의 실체)
 9.  VSZ 와 RSS 의 차이, overcommit, OOM killer 와 이들의 관계는?
```

## 한 줄 답

```
 (1) "현재 실행 상태로 fork() 를 호출하는 프로세스" 가 부모. "focus 된 창" 이나 "가장 최근 실행" 아님.
 (2) 셸(bash)이 직접 부모. 터미널 에뮬레이터는 bash 의 부모, 타겟 프로그램의 조부모.
 (3) 리눅스에서는 "가장 가까운 subreaper" 혹은 (없으면) init(PID 1) 으로 reparent. 조부모 아님.
 (4) prctl(PR_SET_CHILD_SUBREAPER, 1) 시스템콜. systemd --user, tini, dumb-init, docker init 등이 사용.
 (5) libc 는 유저 공간 가상 메모리의 mmap 영역에 로드됨. 커널 메모리 아님. ld-linux 가 mmap 으로 매핑.
 (6) 관례: 높은 주소가 위. 스택은 위(고주소)에서 아래(저주소)로 자라고, 힙은 아래(저주소)에서 위로.
 (7) brk/sbrk = 단일 연속 익명 영역 확장, 중간 해제 불가. mmap = 독립된 VMA, 파일 매핑/공유 가능.
 (8) 둘 다 즉시 물리 메모리 할당 안 함 (demand paging). 처음 접근하는 페이지마다 page fault 로 할당.
 (9) VSZ = 약속된 가상 주소 합, RSS = 실제 차지한 물리 메모리. overcommit 전략 덕에 VSZ >> RSS 가능, 
     물리 메모리 실제 고갈 시 OOM killer 발동.
```

## 목차

```
 §1.  부모 프로세스 결정 기준 — "current" 와 fork() 호출자
 §2.  터미널과 셸 — 누가 누구의 부모인가
 §3.  고아 프로세스 reparenting — PID 1 로 가는 여정
 §4.  subreaper 메커니즘 — prctl 과 find_new_reaper()
 §5.  가상 메모리 레이아웃 — 높은 주소가 위
 §6.  libc 는 어디에 있는가 — mmap 영역, 공유 .text, COW .data
 §7.  heap (brk/sbrk) 의 구조와 한계
 §8.  mmap 의 구조와 자유도
 §9.  malloc 의 라우팅 — brk 로 갈지 mmap 으로 갈지
 §10. demand paging — 가상 예약 vs 물리 할당의 분리
 §11. VSZ / RSS / overcommit / OOM
 §12. 실전 관찰법 — /proc/PID/{maps,status,smaps}
```

---

## §1. 부모 프로세스 결정 기준

### 커널의 한 줄 공리

```
 fork() 를 호출한 프로세스가 부모가 된다.
 "부모" 의 기준은 fork() 를 누가 호출했느냐 뿐.
```

### 코드

```c
// kernel/fork.c  (요약)
static __latent_entropy struct task_struct *copy_process(...)
{
    struct task_struct *p;
    p = dup_task_struct(current, node);  // 현재 task 를 복제
    ...
    // 여기서 real_parent 를 결정
    if (clone_flags & (CLONE_PARENT | CLONE_THREAD))
        p->real_parent = current->real_parent;   // CLONE_PARENT: 부모를 따라감 (스레드)
    else
        p->real_parent = current;                // 일반 fork: 호출자가 부모
    ...
}
```

`current` = "지금 이 시스템 콜을 호출한 CPU 가 실행 중인 task_struct". 즉 **fork() 를 부르기 위해 CPU 에 로드된 프로세스**.

### 흔한 오해들

```
 "최근에 활성화된 창" 의 프로세스가 부모 → 아님
 "사용자가 focus 한 창" 의 프로세스가 부모 → 아님
 "GUI 가 마지막으로 클릭한 아이콘" 의 프로세스 → 아님
 "현재 메모리에 로드된 프로세스 중 아무거나" → 아님

 유일한 기준: "누가 (어떤 task 가) fork() 시스템 콜을 호출했는가"
 그 호출한 프로세스가 곧 current 이고, 그가 자식의 real_parent 가 된다.
```

### 크롬/엣지 예시

```
 시나리오 A: 윈도우 작업 표시줄(explorer.exe / gnome-shell 등)에서 크롬 아이콘 클릭
   윈도우 매니저 / 작업 표시줄 프로세스가 fork+exec("chrome")
   → Chrome 의 부모 = 윈도우 매니저 (또는 시스템 런처)

 시나리오 B: 크롬이 이미 떠 있는 상태에서 다시 엣지 아이콘 클릭
   또 윈도우 매니저가 fork+exec("msedge")
   → Edge 의 부모도 = 윈도우 매니저
   → Edge 는 Chrome 과 "형제" 관계 (공통 부모를 가진 sibling)

 "크롬이 떠 있어서 엣지가 크롬의 자식이 된다" 는 성립하지 않는다.
 크롬이 엣지 실행 파일을 직접 fork+exec 해야만 Edge 가 Chrome 의 자식.
 (예: 브라우저 내부에서 헬퍼 프로세스를 띄우는 경우는 그 구조가 맞음)
```

### 실무 트리 예

```
 systemd (PID 1)
   └── gnome-shell
         ├── chrome          ← gnome-shell 이 fork+exec
         ├── msedge          ← gnome-shell 이 fork+exec (chrome 의 sibling)
         └── gnome-terminal-server
               └── bash      ← terminal 이 fork+exec (PTY 연결)
                     └── ./myprog  ← bash 가 fork+exec
```

---

## §2. 터미널과 셸 — 누가 부모인가

### 흔한 오해

```
 "터미널에서 ls 를 쳤으니 터미널이 ls 의 부모"

 실제는 한 층 더 들어가야 한다:
   터미널 에뮬레이터(gnome-terminal-server, xterm, alacritty 등)는
   단지 "화면에 픽셀로 그리고, 키보드 입력을 PTY 로 전달" 하는 GUI 프로세스.
   실제 명령을 실행하는 건 PTY 안에서 돌아가는 셸(bash, zsh, fish).
```

### 실제 프로세스 트리

```
 gnome-terminal-server           ← 부모: gnome-shell (GUI 매니저)
   └── bash                      ← 부모: gnome-terminal-server (fork+exec 시점에)
         └── ls                  ← 부모: bash (bash 가 fork+exec 해서 ls 실행)
                                  └── ls 가 "./myprog" 같은 걸 실행하는 건 아님
                                     (ls 는 그냥 디렉토리 읽고 끝나면 exit)
         └── vim file.txt        ← 부모: bash
         └── ./myprog            ← 부모: bash
```

### 왜 이렇게 복잡한가

```
 터미널 에뮬레이터의 역할은 PTY (pseudo-terminal) 제공:
   /dev/pts/N  (슬레이브) — bash 가 stdin/stdout/stderr 로 연결됨
   /dev/ptmx   (마스터)   — terminal emulator 가 읽고 쓰며 화면에 그림

 bash 는 마치 "시리얼 콘솔에 연결된 것처럼" PTY 슬레이브 뒤에서 돌아간다.
 키보드 입력 → terminal emulator → PTY 마스터 write → PTY 슬레이브 read → bash → 실행
 프로그램 출력 → bash/자식의 stdout write → PTY 슬레이브 → PTY 마스터 → terminal 이 화면에 렌더링
```

### 확인하는 법

```
 # 터미널 안에서
 $ ps -o pid,ppid,comm
   PID    PPID COMMAND
  12345  12340 bash                    ← bash 의 ppid 는 gnome-terminal-server
  12500  12345 ps                      ← ps 의 ppid 는 bash

 # 트리로
 $ pstree -p $$
   bash(12345)───pstree(12789)
```

---

## §3. 고아 프로세스 reparenting

### 흔한 오해

```
 "A 가 B 를 fork 하고, B 가 C 를 fork 했다. B 가 죽으면 C 는 A 의 자식이 된다."

 → 리눅스에서는 기본적으로 성립하지 않는다. 
   "가장 가까운 조상" 이 아니라 "가장 가까운 subreaper" 로 올라간다.
   subreaper 가 없으면 PID 1 (init) 이 거둔다.
```

### 메커니즘 — find_new_reaper()

```c
// kernel/exit.c  (요약)
static struct task_struct *find_new_reaper(struct task_struct *father,
                                            struct task_struct *child_reaper)
{
    struct task_struct *reaper;

    // 같은 스레드 그룹에 살아있는 다른 스레드가 있으면 거기로
    reaper = find_alive_thread(father);
    if (reaper) return reaper;

    // 없으면 real_parent 를 따라 올라가면서 subreaper 를 찾는다
    for (reaper = father->real_parent;
         !(reaper->signal->is_child_subreaper);
         reaper = reaper->real_parent) {
        if (reaper == &init_task)
            return father->nsproxy->pid_ns_for_children->child_reaper;
    }
    return reaper;
}
```

### 시나리오 다이어그램

```
 초기:
     A  (subreaper 아님)
     └── B
           └── C
                 └── D

 B 가 exit 하면 (C, D 는 고아) →
   find_new_reaper(B, ...) 호출
   → B->real_parent = A
   → A 는 subreaper 아님
   → A->real_parent = ... → ... → init(PID 1)
   → C, D 는 init(PID 1) 로 reparent 된다. A 의 자식으로 가지 않는다.
```

### "이게 왜 문제가 되나?"

```
 실제 사용 사례:
   - 개발자 데몬이 자식 프로세스들을 띄우고 관리하려 함
   - 자식이 손자를 더 fork 함 (e.g. bash 가 다시 또 다른 프로그램 실행)
   - 중간 자식(bash)이 죽으면 손자 프로세스들이 갑자기 init(PID 1) 밑으로 감
   - 데몬이 "내 자식들 전부 wait()" 하려 해도 손자들은 이미 init 자식이라 불가능

 해결책: 나(데몬)를 subreaper 로 만들어서 손자들이 죽을 때 나한테 오게 한다.
```

---

## §4. subreaper 메커니즘

### 플래그 켜기

```c
#include <sys/prctl.h>

// 이 프로세스를 subreaper 로 지정.
// 자손 중 고아가 생기면 PID 1 로 가지 않고 나한테 온다.
prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
```

한 번 켜면 이후 fork 한 자손 전체에 대해 적용. 끌 때는 `prctl(PR_SET_CHILD_SUBREAPER, 0, ...)`.

### 내부 상태

```c
// include/linux/sched/signal.h
struct signal_struct {
    ...
    unsigned int is_child_subreaper:1;
    unsigned int has_child_subreaper:1;
    ...
};
```

### 어디서 쓰나

```
 프로세스                        왜 subreaper 인가
 ─────────────────────          ─────────────────────────────────────────────
 systemd (PID 1)                 모든 고아의 최종 reaper (PID 1 자체)
 systemd --user                  유저 세션의 고아를 거두기 위해 (데스크톱 세션)
 tini / dumb-init                컨테이너 안에서 PID 1 역할 수행 시 zombie 수집
 docker init (docker run --init) 위와 동일 — 이미지 내부에서 시작 PID 1 제공
 개발자의 long-running supervisor 자식 관리하려고 직접 prctl 호출
```

### 시나리오 재구성

```
 tini 가 subreaper 로 켜진 컨테이너 안
     tini (PID 1, is_child_subreaper=1)
       └── myapp
             └── helper_worker

 myapp 이 죽으면:
   → find_new_reaper(myapp)
   → myapp->real_parent = tini, tini->is_child_subreaper = 1  → 검색 종료
   → helper_worker 의 새 부모 = tini
   → tini 가 helper_worker 의 SIGCHLD / wait 를 처리해서 좀비 방지

 (tini 없이 그냥 앱을 컨테이너 PID 1 로 띄우면 좀비 수집 안 되어 fd/pid 누수)
```

### 관련 함수

```c
prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);  // 켜기
prctl(PR_GET_CHILD_SUBREAPER, &val, 0, 0, 0); // 조회
```

---

## §5. 가상 메모리 레이아웃

### 올바른 방향 (x86_64 기준)

```
 높은 주소 (0x7fff_ffff_ffff 쪽)
    ┌────────────────────────────┐
    │ [vsyscall / vdso / vvar]   │  커널이 제공하는 "빠른 시스템콜 페이지"
    ├────────────────────────────┤
    │                            │
    │  stack                     │  main 함수 지역 변수, 리턴 주소, 프레임
    │  ↓ (자람 방향: 고→저)      │
    │                            │
    ├────────────────────────────┤  ← 여기 어딘가에 stack top
    │                            │
    │  ...  (빈 가상 주소 공간)   │
    │                            │
    ├────────────────────────────┤
    │                            │
    │  mmap 영역                 │  libc.so, ld-linux, mmap() 호출 결과
    │  (여러 개의 VMA)           │  JIT 메모리, 큰 malloc(>128KB) 도 여기
    │                            │
    ├────────────────────────────┤
    │                            │
    │  heap                      │  brk/sbrk 가 관리하는 영역
    │  ↑ (자람 방향: 저→고)      │  작은 malloc 이 채움
    │                            │
    ├────────────────────────────┤  ← program break (brk)
    │  BSS          .bss         │  초기화 안 된 전역/정적 변수 (0)
    │  Data         .data        │  초기화된 전역/정적 변수
    ├────────────────────────────┤
    │  Text         .text        │  실행 코드 (read-only)
    ├────────────────────────────┤
    │  (ELF 헤더)                │
    └────────────────────────────┘
 낮은 주소 (0x0000_0000_0000)       0 번지 근처는 NULL pointer 트랩용 매핑 금지 영역
```

### 왜 "높은 주소가 위" 인가

```
 단순 관례 이상의 이유:
   - /proc/PID/maps 는 "낮은 주소 먼저, 높은 주소 나중" 순으로 나옴.
     사람이 읽을 때 "높은 주소가 위" 로 세로로 보면 상단부터 stack/mmap 이 보임.
   - 교과서 (Intel 매뉴얼, CSAPP, OS 교재 등) 모두 이 방향.
   - 스택과 힙의 "자람 방향" 을 직관적으로 시각화: 서로 가운데를 향해 자람.
```

### 실물 확인

```
 $ cat /proc/self/maps
 55e1c2b10000-55e1c2b12000 r--p 00000000 ... /usr/bin/cat   ← .text (낮은 주소)
 55e1c2b12000-55e1c2b1b000 r-xp ...                         ← .text (실행)
 55e1c2b1b000-55e1c2b1e000 r--p ...                         ← .rodata
 55e1c2b1e000-55e1c2b20000 r--p ...                         ← .data
 55e1c2b20000-55e1c2b21000 rw-p ...                         ← .bss
 55e1c3c7f000-55e1c3ca0000 rw-p [heap]                       ← heap
 7ff27b400000-7ff27b428000 r--p /usr/lib/.../libc.so.6      ← mmap 영역
 7ff27b428000-7ff27b5c1000 r-xp                             ← libc .text
 7ff27b5c1000-7ff27b619000 r--p                             ← libc .rodata
 7ff27b619000-7ff27b61d000 r--p                             ← libc .data (read-only)
 7ff27b61d000-7ff27b61f000 rw-p                             ← libc .bss/.data (COW)
 7ff27b685000-7ff27b6a5000 r-xp /usr/lib/ld-linux-x86-64.so.2
 7ffd5c7f4000-7ffd5c815000 rw-p [stack]                      ← stack (높은 주소)
 7ffd5c8ea000-7ffd5c8ed000 r--p [vvar]
 7ffd5c8ed000-7ffd5c8ee000 r-xp [vdso]
 ffffffffff600000-ffffffffff601000 r-xp [vsyscall]           ← 최상단
```

---

## §6. libc 는 어디에 있는가

### 결론

```
 libc 는 "유저 공간 가상 메모리의 mmap 영역" 에 있다. 
 • 커널 메모리가 아니다 (커널은 어떤 프로세스의 가상 주소 공간에서도 유저가 볼 수 없는 부분).
 • 힙이 아니다 (heap 은 brk/sbrk 로 확장하는 별도 영역).
 • ld-linux 가 mmap() 으로 /usr/lib/x86_64-linux-gnu/libc.so.6 파일을 매핑한 결과.
```

### 로드 과정

```
 $ ./myprog 실행 시
   1. 커널이 exec 처리 중 ELF 헤더를 파싱
   2. ELF 가 dynamic linker (보통 /lib64/ld-linux-x86-64.so.2) 를 필요로 함을 발견
   3. 커널이 ld-linux 를 먼저 매핑해서 실행시킴
   4. ld-linux 가 myprog 의 NEEDED 라이브러리 목록 확인 (libc.so.6, ...)
   5. ld-linux 가 각 .so 를 mmap() 으로 매핑
       - .text 섹션: MAP_PRIVATE | PROT_READ|PROT_EXEC (여러 프로세스 간 공유)
       - .rodata:   MAP_PRIVATE | PROT_READ
       - .data/.bss: MAP_PRIVATE | PROT_READ|PROT_WRITE (COW — 수정 시 이 프로세스용 복사본)
   6. ld-linux 가 relocation / GOT/PLT 채우기
   7. myprog 의 main() 으로 점프
```

### 공유 vs 사유

```
 libc.text (코드) — PROT_READ|PROT_EXEC, MAP_PRIVATE
   여러 프로세스가 같은 물리 페이지를 공유해서 본다.
   (페이지 테이블이 같은 물리 프레임을 가리킴)
   수정 불가니까 공유해도 문제 없음.
   → 메모리 절약의 핵심.

 libc.data (초기값 있는 전역) — PROT_READ|PROT_WRITE, MAP_PRIVATE
   처음엔 공유로 매핑됨 (디스크 페이지 그대로).
   한 프로세스가 쓰려 하면 page fault → copy-on-write → 전용 페이지로 분리.

 libc.bss (초기값 없는 전역 = 0) — 익명 매핑
   처음 접근 시 zero page 에서 복사되는 식.
```

### "libc 가 수정되지 않는 동안 왜 모든 프로세스가 메모리를 복사 안 하는가?"

```
 이유 = mmap MAP_PRIVATE + PROT_READ (+ PROT_EXEC) 덕분.
 
 파일 매핑의 경우 커널이:
   1. 파일의 해당 페이지를 page cache 에 읽어둠 (디스크 캐시)
   2. 여러 프로세스의 페이지 테이블이 모두 그 한 개의 물리 페이지를 가리키게 함
   3. 읽기 전용이라 수정될 일 없음 → 영원히 공유
 
 결과:
   시스템 전체에 libc.so 인스턴스가 "사실상 1 개" 만 존재 (RSS 로는 공유분 때문에 
   작게 잡힘). Linux 의 PSS(proportional set size) 가 이걸 반영한 지표.
```

---

## §7. heap (brk/sbrk)

### 구조

```
 heap 은 프로세스 당 단 하나의 연속된 "익명 메모리 영역".
 그 끝 위치가 "program break" (brk 포인터).
 
 ┌─────────────────────────┐
 │ heap                    │
 │                         │
 │  (사용 중 / 미사용 섞임)│
 │                         │
 └─────────────────────────┘  ← program break = brk
 (그 위는 mmap 영역이 차지함)

 brk 를 위로 올리면 heap 이 확장됨.
 brk 를 아래로 내리면 heap 이 축소됨 (거의 안 함 — 파편화 문제).
```

### 시스템 콜

```c
#include <unistd.h>

void *brk(void *end);       // 절대 주소로 break 설정
void *sbrk(intptr_t incr);  // 현재 break 를 incr 만큼 이동, 이전 break 반환
                            // sbrk(0) 으로 현재 위치 조회 가능
```

### 한계

```
 1. 단일 연속 영역이라 중간을 "구멍" 으로 만들 수 없다.
    free() 가 매우 작은 조각을 돌려줘도, brk 를 내려서 OS 에 반환하는 건 연속된 
    끝 부분만 가능. 중간은 "free list" 로만 관리.

 2. 파일 매핑 불가 — 항상 익명(zero-filled).

 3. 공유 불가 — 항상 MAP_PRIVATE.

 4. 보호 속성 단일 — heap 전체가 PROT_READ|PROT_WRITE.

 5. 큰 할당이 오면 확장이 비효율적 (brk 한 번에 큰 점프).
```

### 왜 쓰나

```
 작은 할당에 경제적:
   brk 한 번으로 큰 덩어리를 가져와서 malloc 구현이 쪼개서 나눠준다.
   매번 mmap 호출하는 것보다 syscall 오버헤드가 훨씬 적음.
```

---

## §8. mmap

### 구조

```
 mmap 영역은 "VMA(virtual memory area)" 의 집합이다.
 한 번 mmap 호출할 때마다 독립된 VMA 가 하나 생성되고, 
 프로세스 안에서 여러 개가 공존한다.

 mmap 영역 일부:
   [VMA #1] libc.so .text      PROT_READ|PROT_EXEC, MAP_PRIVATE, 파일 백업
   [VMA #2] libc.so .rodata    PROT_READ, MAP_PRIVATE, 파일 백업
   [VMA #3] libc.so .data      PROT_READ|PROT_WRITE, MAP_PRIVATE (COW)
   [VMA #4] ld-linux .text     ...
   [VMA #5] 익명 mmap 영역     PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE
   [VMA #6] shared memory      PROT_READ|PROT_WRITE, MAP_SHARED (여러 프로세스)
   [VMA #7] 파일 X 매핑        PROT_READ, MAP_SHARED
```

### 시스템 콜

```c
#include <sys/mman.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
```

### 특징

```
 1. 파일 매핑 가능 (fd 를 넘기면 파일 내용이 주소 공간에 나타남).
 2. 익명 매핑 가능 (MAP_ANONYMOUS — 파일 없이 0 으로 채움).
 3. 공유 매핑 가능 (MAP_SHARED — 여러 프로세스 간 실제로 같은 물리 메모리).
 4. 매핑 단위로 보호 속성 지정 (PROT_READ / PROT_WRITE / PROT_EXEC 조합).
 5. 중간을 독립적으로 munmap 할 수 있음 (VMA 단위 해제).
 6. 더 큰 주소 범위 지원, 정렬된 크기로 관리 쉬움.
```

### 주요 용도

```
 용도                           대표 호출
 ─────────────────────         ────────────────────────────────────────
 공유 라이브러리 로드            ld-linux 가 호출 (자동)
 큰 malloc (>128KB)             glibc malloc 이 내부적으로 mmap 선택
 공유 메모리 IPC                 shm_open + mmap, 또는 MAP_SHARED|MAP_ANONYMOUS+fork
 파일 I/O 속도 향상              파일 mmap 후 포인터로 접근 (read/write 대비 kcopy 제거)
 JIT 컴파일                     mmap(PROT_READ|PROT_WRITE) → 코드 씀 → 
                                mprotect(PROT_READ|PROT_EXEC) 로 전환
 sbrk 한계 우회                  임의 크기 익명 매핑
```

---

## §9. malloc 의 라우팅 — brk 로 갈지 mmap 으로 갈지

glibc ptmalloc 의 대략적 정책:

```
 malloc(size) 호출:
   if (size > M_MMAP_THRESHOLD)    // 기본 128 KB
       → mmap(MAP_ANONYMOUS|MAP_PRIVATE) 로 독립 VMA 생성
   else
       → arena 의 free list 확인
         있으면 반환
         없으면 brk 로 heap 확장하여 새 블록 확보
```

### 임계값 조정

```c
#include <malloc.h>
mallopt(M_MMAP_THRESHOLD, 64 * 1024);  // 64KB 이상은 mmap 으로
mallopt(M_TRIM_THRESHOLD, 128 * 1024); // heap 반환 임계값
```

### 왜 큰 건 mmap 인가

```
 • brk 로 큰 덩어리를 가져오면 heap 꼭대기에 붙어서 가운데 조각들을 OS 에 반환하기 힘듦.
 • mmap 은 free() 즉시 munmap 으로 OS 에 완전히 돌려줄 수 있음 (메모리 파편화 완화).
 • 보호 속성 / 매핑 주소 제어가 자유로움.
```

---

## §10. demand paging — "가상 예약 vs 물리 할당" 의 분리

이 부분이 가장 많이 오해되는 지점이다.

### 핵심 공리

```
 malloc / mmap / brk 모두 "가상 주소 범위를 예약" 만 한다.
 실제 물리 메모리 페이지는 "처음 접근하는 순간" 에 page fault 로 할당된다.
 
 이 정책이 demand paging (= lazy allocation) = Linux 기본 동작.
```

### 시나리오

```c
char *p = malloc(1024 * 1024 * 1024);   // 1 GB 요청
// 이 시점에:
//   VSZ +1GB (가상 주소 예약)
//   RSS +0   (물리 메모리 아직 안 씀!)
//   → 실제 1GB 의 free memory 가 없어도 성공 가능 (overcommit)

for (size_t i = 0; i < 10 * 1024 * 1024; i += 4096) {
    p[i] = 1;                           // 페이지 당 첫 write
    // 각 write 마다:
    //   1) CPU 의 MMU 가 페이지 테이블 확인 → present bit = 0 → page fault
    //   2) 커널 do_page_fault() → handle_mm_fault() → do_anonymous_page()
    //   3) 물리 페이지 1 개 할당 + 페이지 테이블 채우기 + 0 으로 채움
    //   4) write 완료
}
// 이 시점에:
//   VSZ +1GB (불변)
//   RSS +10MB  (2560 페이지 × 4KB)
```

### page fault 경로

```c
// arch/x86/mm/fault.c  (요약)
DEFINE_IDTENTRY_RAW_ERRORCODE(exc_page_fault)
{
    ...
    do_user_addr_fault(regs, error_code, address);
}

// mm/memory.c
vm_fault_t handle_mm_fault(struct vm_area_struct *vma, ...)
{
    ...
    if (anon) 
        return do_anonymous_page(vmf);   // 0 으로 채운 새 페이지
    if (file)
        return do_cow_fault(vmf);        // 파일 페이지 → 쓰기 시 COW
    ...
}
```

### 두 종류의 fault

```
 Minor fault (디스크 I/O 없음)
   - 익명 페이지 최초 접근 (zero page 배정)
   - page cache 에 이미 있는 파일 페이지 접근
   - 가벼움 (μs 단위)

 Major fault (디스크 I/O 있음)
   - 파일 매핑인데 page cache 에 없음 → 디스크에서 읽어옴
   - 스왑 아웃된 페이지 접근 → 스왑에서 읽어옴
   - 느림 (ms 단위)
```

### mmap 도 동일

```
 void *q = mmap(NULL, 10 * 1024 * 1024 * 1024L, PROT_READ|PROT_WRITE, 
                MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);   // 10GB 매핑
 // VSZ +10GB, RSS +0
 // 실제 시스템 물리 메모리가 10GB 미만이어도 성공 (가상 주소만 예약)
 
 q[0] = 1;              // 이제 첫 페이지만 물리 할당 → RSS +4KB
```

### eager 할당 원할 때

```
 (1) MAP_POPULATE
     void *p = mmap(..., MAP_ANONYMOUS|MAP_PRIVATE|MAP_POPULATE, ...);
     → mmap 이 리턴하기 전에 모든 페이지 미리 매핑
 
 (2) mlock
     mlock(p, size);
     → 물리 메모리 확보 + 스왑 아웃 금지
 
 (3) madvise
     madvise(p, size, MADV_WILLNEED);
     → readahead 유도 (파일 매핑에서 유용)
 
 (4) 루프로 touch
     for (size_t i = 0; i < size; i += 4096) ((char*)p)[i] = 0;
```

### "즉시 할당" 의 오해

```
 오해: "mmap 은 즉시 물리 메모리 매핑한다" → X
 사실: mmap 이든 malloc 이든 brk 든, Linux 는 기본으로 lazy.
       차이는 "가상 주소 관리 방식" 이지 "물리 할당 시점" 이 아니다.
```

---

## §11. VSZ / RSS / overcommit / OOM

### VSZ 와 RSS

```
 VSZ (virtual size)
   - 프로세스가 "예약해 놓은" 가상 주소 공간의 합
   - /proc/PID/status 의 VmSize
   - 여기에는 아직 접근 안 한 페이지, 파일 매핑 전체 크기 등이 다 포함됨
   - "호언장담" — 나 이만큼 쓸지도 몰라요
 
 RSS (resident set size)
   - "실제로 물리 메모리에 올라와 있는" 만큼
   - /proc/PID/status 의 VmRSS
   - 공유 페이지는 그대로 다 포함됨 → 여러 프로세스의 RSS 합이 실제 메모리를 초과 가능
 
 PSS (proportional set size)
   - 공유 페이지를 "공유 프로세스 수로 나눠서" 각자에게 할당
   - RSS 의 과대 계상 문제 보완, /proc/PID/smaps 에서 확인
```

### overcommit

```
 Linux 는 기본적으로 overcommit 허용:
   /proc/sys/vm/overcommit_memory
     0 — 휴리스틱 (기본). 어느 정도 초과까지는 허용
     1 — 항상 허용. 커널이 가상 할당을 거부하지 않음
     2 — 엄격. (RAM + swap) × overcommit_ratio/100 까지만 허용
 
 덕분에 malloc(10GB) 가 12GB RAM 시스템에서도 거의 항상 성공한다.
 실제 다 안 쓰면 문제 없음.
```

### OOM killer

```
 실제로 메모리가 부족해지는 시점 (모든 페이지가 dirty 이고 스왑도 꽉 찼을 때):
   → OOM (Out Of Memory) 상황 발생
   → 커널의 oom_killer 발동
   → oom_score 가 높은 프로세스를 SIGKILL
   
 점수 기반:
   /proc/PID/oom_score
   /proc/PID/oom_score_adj  (-1000 ~ +1000 로 사용자가 조정 가능)

 관리 예:
   echo -1000 > /proc/PID/oom_score_adj   # 절대 죽이지 마 (DB 등 중요 프로세스)
   echo 1000  > /proc/PID/oom_score_adj   # 먼저 죽여 (일회성 작업)
```

### 실전 의미

```
 컨테이너에서 "메모리 리미트 1GB" 주면:
   - 그 안의 프로세스 VSZ 는 1GB 초과 가능
   - 하지만 RSS 가 1GB 에 도달하면 cgroup OOM killer 가 동작
   - cgroup 내부 프로세스부터 죽임 (시스템 전체에는 영향 X)
```

---

## §12. 실전 관찰법

### /proc/PID/maps — VMA 목록

```
 $ cat /proc/$$/maps | head -20
 556789abc000-556789abe000 r--p 00000000 fc:00 12345  /usr/bin/bash
 556789abe000-556789b32000 r-xp 00002000 fc:00 12345  /usr/bin/bash
 ...
 556789c00000-556789d00000 rw-p 00000000 00:00 0      [heap]
 ...
 7f1234000000-7f1234028000 r--p 00000000 fc:00 67890  /usr/lib/libc.so.6
 ...
 7ffe12340000-7ffe12361000 rw-p 00000000 00:00 0      [stack]
 
 형식: 주소범위 권한 offset dev inode 경로
   권한 예: r-xp = read/exec, private
          rw-s = read/write, shared
          rw-p = read/write, private (COW)
```

### /proc/PID/status — 요약 수치

```
 $ cat /proc/$$/status | grep -E 'Vm|Rss'
 VmPeak:     12345 kB   (평생 최대 VSZ)
 VmSize:     12000 kB   (현재 VSZ)
 VmHWM:       2345 kB   (평생 최대 RSS = high water mark)
 VmRSS:       2000 kB   (현재 RSS)
 RssAnon:     1200 kB
 RssFile:      700 kB
 RssShmem:     100 kB
 VmData:      3000 kB   (heap + 익명 매핑)
 VmStk:        200 kB
 VmExe:        500 kB
 VmLib:       4000 kB   (공유 라이브러리 코드 영역)
 VmPTE:         40 kB   (페이지 테이블 자체의 크기)
 VmSwap:        10 kB
```

### /proc/PID/smaps — VMA 별 상세

```
 각 VMA 마다 Pss, Rss, Shared_Clean, Private_Dirty 등을 보여줌.
 메모리 누수 추적, 어떤 라이브러리가 얼마나 쓰는지 분석에 필수.
```

### pmap 명령

```
 $ pmap -x $$
 Address           Kbytes     RSS   Dirty Mode  Mapping
 0000556789abc000     120       8       0 r--p  /usr/bin/bash
 0000556789abe000     852     600       0 r-xp  /usr/bin/bash
 ...
```

---

## 한 줄 정리

```
 부모 = fork() 를 호출한 current. focus/실행순서/트리 상 가까움 모두 무관.
 터미널 앱 → 셸 → 프로그램. 프로그램의 직접 부모는 셸(bash).
 고아 reparent 는 조부모가 아니라 "가까운 subreaper 또는 PID 1" 로.
 subreaper 는 prctl(PR_SET_CHILD_SUBREAPER, 1). systemd/tini/dumb-init 가 사용.
 libc 는 유저 공간 mmap 영역에 MAP_PRIVATE 로 매핑. 모든 프로세스가 같은 물리 페이지 공유.
 가상 메모리는 높은 주소가 위. 스택 고→저, 힙 저→고.
 heap = 연속 익명, brk 로 관리. mmap = 독립 VMA, 파일 매핑/공유 가능. malloc 은 크기로 라우팅.
 모든 할당은 lazy. 첫 접근 시 page fault 로 물리 할당. VSZ >> RSS 가능 (overcommit).
 진짜 부족해지면 OOM killer. oom_score_adj 로 우선순위 조정.
```
