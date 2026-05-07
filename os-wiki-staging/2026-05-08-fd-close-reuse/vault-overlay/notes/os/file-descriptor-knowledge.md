---
type: Knowledge
status: Draft
week:
  - user-programs
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - week:user-programs
  - layer:user
  - layer:kernel
  - layer:device
  - topic:fd
  - topic:syscall
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-2-user-programs-map]]"
  - "[[syscall-end-to-end]]"
  - "[[user-pointer-validation-trace]]"
---

# 파일 디스크립터는 왜 작은 정수인가

## 작은 질문

`write(1, "hello", 5)`에서 `1`은 대체 무엇일까?

처음에는 `1`이 화면 장치의 주소처럼 느껴질 수 있다. 하지만 `1`은 주소가 아니다. 유저 프로그램이 커널에게 "내가 가진 1번 I/O 대상에 5바이트를 써 달라"고 말하기 위한 **작은 번호표**다.

이 번호표를 file descriptor, 줄여서 fd라고 부른다.

## 목차

- 왜 필요한가
- 핵심 모델
- 예시 상황
- Linux / Windows에서는
- PintOS에서는
- QEMU에서는
- 차이점
- 코드 증거
- 숫자와 메모리
- 직접 확인
- 다음 링크

## 왜 필요한가

유저 프로그램이 파일, 터미널, 파이프, 소켓 같은 I/O 대상을 직접 만지면 문제가 생긴다.

- 디스크 파일의 실제 위치, inode, 캐시 상태를 유저가 알 필요가 없다.
- 터미널과 디스크 파일은 구현이 다르지만, 유저 코드는 둘 다 `read`/`write`로 다루고 싶다.
- 커널 객체 주소를 유저에게 그대로 주면 보안과 안정성이 깨진다.

그래서 OS는 유저에게 커널 포인터를 주지 않고, 프로세스마다 작은 정수 번호를 준다.

이 말은 즉, fd는 **유저 공간에서 커널 내부 I/O 객체를 간접 참조하기 위한 index**다.

## 핵심 모델

fd를 이해하는 최소 모델은 이렇다.

```text
user process
  fd = 3
    |
    v
kernel: current process의 fd table
  fd_table[3] -> 열린 파일 객체
                    |
                    v
                 inode / device / socket 같은 실제 I/O 대상
```

유저 프로그램은 `3`만 본다. 커널은 현재 프로세스의 fd table에서 `3`번 칸을 찾아 실제 객체로 내려간다.

중요한 결론은 두 가지다.

- 같은 숫자 `3`이라도 프로세스가 다르면 다른 대상을 가리킬 수 있다.
- fd는 파일 내용이 아니라 "열린 상태"를 가리킨다. 열린 상태에는 현재 읽기 위치 같은 정보가 붙을 수 있다.

## 예시 상황

아래 코드를 보자.

```c
int fd = open ("sample.txt");
write (fd, "abc", 3);
close (fd);
```

겉으로 보이는 값은 작은 정수 하나다.

```text
fd = 2 또는 3 같은 작은 정수
```

하지만 커널 안에서는 대략 이런 흐름이 생긴다.

```text
open("sample.txt")
  -> 파일 이름 검증
  -> 파일 시스템에서 sample.txt를 찾음
  -> 열린 파일 객체(struct file)를 만듦
  -> 현재 프로세스 fd_table의 빈 칸에 꽂음
  -> 그 칸 번호를 유저에게 반환
```

그 다음 `write(fd, ...)`는 파일 이름을 다시 찾지 않는다. fd table에서 바로 열린 파일 객체를 찾는다.

## Linux / Windows에서는

Linux에서는 fd가 프로세스의 file descriptor table index다.

단순화하면 다음 구조로 내려간다.

```text
task_struct
  -> files_struct
    -> fdtable[fd]
      -> struct file
        -> inode / socket / pipe / device
```

`stdin`, `stdout`, `stderr`는 관례적으로 fd `0`, `1`, `2`다.

```text
0: standard input
1: standard output
2: standard error
```

그래서 shell에서 redirection이 가능하다. 예를 들어 `stdout`을 파일로 바꾸면, 프로그램은 여전히 `write(1, ...)`를 호출하지만 fd table의 1번 칸이 터미널이 아니라 파일을 가리키게 된다.

Windows는 같은 문제를 handle 모델로 푼다. Win32 API에서 `CreateFile`, `ReadFile`, `WriteFile`은 `HANDLE`을 사용한다. Linux fd처럼 작은 정수 index로 설명하기보다는, 커널 object를 가리키는 불투명한 handle 값으로 이해하는 편이 좋다.

둘의 공통점은 같다. 유저 프로그램은 커널 내부 객체 주소를 직접 받지 않고, OS가 검증할 수 있는 간접 식별자를 받는다.

## PintOS에서는

이 저장소의 PintOS 구현은 fd table을 `struct thread` 안에 둔다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/thread.h`
  - `struct file **fd_table`
  - `int next_fd`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`
  - `process_add_file`
  - `process_get_file`
  - `process_close_file`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
  - `open`
  - `read`
  - `write`
  - `close`

초기 유저 프로세스가 시작될 때 fd table을 0으로 채운 페이지 하나로 만든다.

```text
fd_table = palloc_get_page(PAL_ZERO)
next_fd  = 2
```

`next_fd = 2`인 이유는 0과 1을 표준 입력/출력용으로 예약하기 때문이다. 이 코드베이스의 `stdio.h`에는 다음 값이 정의되어 있다.

```c
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
```

PintOS의 `open()`은 파일 이름을 검사한 뒤 `filesys_open(file)`로 `struct file *`를 얻고, `process_add_file(opened_file)`로 fd table에 꽂는다.

흐름은 이렇게 읽으면 된다.

```text
open("sample.txt")
  -> filesys_open("sample.txt")
  -> opened_file: struct file *
  -> process_add_file(opened_file)
  -> curr->fd_table[fd] = opened_file
  -> return fd
```

`write(fd, buffer, size)`는 `fd == 1`이면 화면 출력으로 처리하고, 그 외에는 fd table에서 파일 객체를 찾는다.

```text
fd == 1
  -> putbuf(buffer, size)

fd != 1
  -> process_get_file(fd)
  -> file_write(file, buffer, size)
```

`close(fd)`는 fd table 칸을 비우고 `file_close()`로 열린 파일 객체를 닫는다.

```text
process_close_file(fd)
  -> file_close(curr->fd_table[fd])
  -> curr->fd_table[fd] = NULL
```

## QEMU에서는

QEMU는 PintOS의 fd 의미를 처리하지 않는다.

`SYS_OPEN`, `SYS_WRITE`, `SYS_CLOSE` 같은 의미는 guest OS인 PintOS가 `syscall_handler`에서 해석한다. QEMU는 그 코드가 실행되는 CPU, 메모리, 디스크 장치처럼 보이는 환경을 제공한다.

파일 I/O가 디스크까지 내려가면 역할이 갈라진다.

```text
PintOS syscall
  -> PintOS filesys/file/inode
  -> PintOS disk_read/disk_write
  -> guest ATA I/O port access
  -> QEMU IDE device model
  -> QEMU BlockBackend
  -> host의 backing file 또는 block device
```

볼 위치는 다음과 같다.

- PintOS 디스크 경로
  - `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/inode.c`
  - `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/devices/disk.c`
- QEMU 디스크 에뮬레이션 경로
  - `/Users/woonyong/workspace/Krafton-Jungle/QEMU/hw/ide/core.c`
  - `/Users/woonyong/workspace/Krafton-Jungle/QEMU/block/block-backend.c`

즉 QEMU의 host fd와 PintOS guest fd를 섞으면 안 된다. PintOS fd는 guest OS 내부 번호이고, QEMU가 내부적으로 사용하는 host fd는 QEMU 프로세스가 backing file이나 socket을 다룰 때 쓰는 완전히 다른 번호다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| 유저에게 보이는 값 | Linux fd, Windows HANDLE | fd 정수 | guest fd를 직접 해석하지 않음 |
| table 위치 | 프로세스별 커널 자료구조 | `struct thread`의 `fd_table` | QEMU 내부 장치/backend 자료구조 |
| fd 0/1/2 | stdin/stdout/stderr 관례 | 이 코드베이스는 0/1 예약, 2부터 파일 할당 | 해당 없음 |
| 열린 파일 상태 | `struct file`/open file description 등 복잡 | `struct file`에 inode, pos, deny_write | guest 디스크 요청을 block backend로 처리 |
| fork 후 fd | Linux는 열린 파일 description 공유가 중요 | 이 구현은 `file_duplicate()`로 pos를 복사한 별도 file 객체 생성 | 해당 없음 |

PintOS의 단순화가 중요하다. 실제 Linux의 VFS는 일반 파일, 소켓, 파이프, 장치 파일을 같은 fd 인터페이스로 묶는다. PintOS 2주차 구현은 주로 파일 시스템 과제에 필요한 크기로 축소되어 있다.

## 코드 증거

### 1) fd table은 `struct thread`에 있다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/thread.h`

핵심 필드:

```c
struct file **fd_table;
int next_fd;
```

이 말은 PintOS에서 현재 실행 중인 프로세스의 fd table을 찾으려면 `thread_current()`에서 시작한다는 뜻이다.

### 2) fd 최대 개수는 페이지 크기로 정해진다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`

핵심 정의:

```c
#define FD_MAX (PGSIZE / sizeof (struct file *))
```

x86-64에서 포인터가 8바이트이고 `PGSIZE = 4096`이면:

```text
FD_MAX = 4096 / 8 = 512
```

즉 이 구현의 fd table은 포인터 512개짜리 배열처럼 생각하면 된다.

### 3) `open()`은 `struct file *`를 fd table에 꽂는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`

핵심 흐름:

```c
opened_file = filesys_open (file);
fd = process_add_file (opened_file);
```

그리고 `process_add_file()`은 빈 칸을 찾아 넣는다.

```c
curr->fd_table[fd] = f;
return fd;
```

### 4) fd로 다시 파일 객체를 찾는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`

핵심 흐름:

```c
if (fd < 0 || fd >= FD_MAX)
    return NULL;
return curr->fd_table[fd];
```

이 조건이 없으면 유저가 `999999` 같은 fd를 넘겼을 때 fd table 밖을 읽게 된다.

### 5) 열린 파일 객체에는 현재 위치가 있다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/file.c`

핵심 필드:

```c
struct file {
    struct inode *inode;
    off_t pos;
    bool deny_write;
};
```

`file_read()`는 읽은 만큼 `pos`를 앞으로 이동한다.

```c
bytes_read = inode_read_at (..., file->pos);
file->pos += bytes_read;
```

그래서 fd는 단순히 "파일 이름"이 아니라 "열린 파일 상태"를 가리킨다고 말해야 정확하다.

## 숫자와 메모리

### fd table 배열 감각

이 구현에서 fd table은 4KB 페이지 하나다.

```text
PGSIZE                 = 4096 bytes
sizeof(struct file *)  = 8 bytes
FD_MAX                 = 512 slots
```

메모리 배치는 개념적으로 이렇게 볼 수 있다.

```text
fd_table base = 0x10000000 라고 가정

fd 0 slot address = 0x10000000 + 0 * 8 = 0x10000000
fd 1 slot address = 0x10000000 + 1 * 8 = 0x10000008
fd 2 slot address = 0x10000000 + 2 * 8 = 0x10000010
fd 3 slot address = 0x10000000 + 3 * 8 = 0x10000018
```

만약 `open("sample.txt")`가 fd `2`를 반환했다면:

```text
fd_table[2] = 0x80041230  # struct file *라고 가정
```

그 다음 `write(2, buffer, 5)`는:

```text
process_get_file(2)
  -> fd_table[2]
  -> 0x80041230
  -> file_write((struct file *)0x80041230, buffer, 5)
```

### 같은 파일을 두 번 열면

PintOS 테스트 `open-twice.c`는 같은 파일을 두 번 열어도 fd가 달라야 한다고 확인한다.

```text
h1 = open("sample.txt")  -> fd 2
h2 = open("sample.txt")  -> fd 3
```

개념적으로는 이렇게 된다.

```text
fd_table[2] -> struct file A -> same inode
fd_table[3] -> struct file B -> same inode
```

같은 inode를 가리켜도 `struct file`이 다르면 `pos`는 따로 움직일 수 있다.

## 직접 확인

### 1) `open()`이 반환한 fd 보기

PintOS GDB에서:

```gdb
b open
b process_add_file
```

확인할 값:

```gdb
p file
p opened_file
p fd
p thread_current()->fd_table
p thread_current()->next_fd
```

### 2) fd table 칸 보기

`process_add_file()`에서 fd가 정해진 뒤:

```gdb
p fd
p/x thread_current()->fd_table[fd]
p *(struct file *) thread_current()->fd_table[fd]
```

확인할 질문:

- fd 숫자는 몇인가?
- 그 칸에는 어떤 `struct file *`가 들어갔는가?
- `struct file.pos`는 몇에서 시작하는가?

### 3) `write(fd, ...)`가 fd를 다시 해석하는지 보기

```gdb
b write
b process_get_file
b file_write
```

확인할 값:

```gdb
p fd
p/x process_get_file(fd)
p *(struct file *) process_get_file(fd)
```

### 4) `close(fd)` 뒤 table이 비는지 보기

```gdb
b process_close_file
```

확인할 값:

```gdb
p fd
p/x thread_current()->fd_table[fd]
n
p/x thread_current()->fd_table[fd]
```

`close` 뒤에는 해당 칸이 `NULL`이 되어야 한다.

## 구현 주의

이 저장소의 syscall 구현은 함수마다 invalid fd 처리 강도가 조금 다르다.

- `write()`는 `process_get_file(fd)`가 `NULL`이면 `-1`을 반환한다.
- `close()`는 `fd < 2`, 범위 밖 fd, 비어 있는 칸을 조용히 무시한다.
- `filesize()`, `seek()`, `tell()`은 fd가 잘못되었을 때 `file_length`, `file_seek`, `file_tell`로 바로 내려갈 수 있으므로 직접 테스트하며 정책을 확인해야 한다.

학습할 때 중요한 판단 기준은 이것이다.

> fd는 유저가 마음대로 만든 숫자일 수 있으므로, 커널은 항상 "현재 프로세스 table에서 유효한 index인가?"를 먼저 확인해야 한다.

## 다음 링크

- [[fd-close-reuse-lab]]: `close(fd)` 뒤 같은 번호가 다시 쓰이는 순간과 `remove(name)`이 열린 fd와 분리되는 이유
- [[syscall-end-to-end]]: fd 숫자가 register 인자로 들어오는 전체 흐름
- [[user-pointer-validation-trace]]: fd와 함께 들어오는 user buffer를 왜 검증해야 하는지
- [[address-translation-memory]]: fd table 포인터와 user buffer 주소가 서로 다른 주소 공간 문제로 이어지는 지점
- [[week-2-user-programs-map]]: 2주차 User Programs 전체 지도
