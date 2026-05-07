---
type: Lab
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
  - topic:gdb
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-2-user-programs-map]]"
  - "[[file-descriptor-knowledge]]"
  - "[[syscall-end-to-end]]"
  - "[[mmap-file-backed-page-knowledge]]"
---

# fd close/reuse Lab (닫힌 번호는 언제 다시 쓰이는가)

## 작은 질문

아래처럼 파일을 열고 닫은 뒤 다시 열면, 다음 `open()`은 몇 번 fd를 줄까?

```c
int a = open ("sample.txt");
int b = open ("sample.txt");
close (a);
int c = open ("sample.txt");
```

처음 배우는 사람은 `c`가 새 번호라서 `4`가 될 것 같다고 느낄 수 있다. 하지만 많은 OS에서는 닫힌 fd 번호가 다시 쓰일 수 있다.

이 Lab의 질문은 두 가지다.

```text
1. close(fd)는 fd table의 어느 바이트를 비우는가?
2. remove(name)은 열린 fd까지 닫는가, 아니면 이름만 지우는가?
```

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
- 정리
- 다음 링크

## 왜 필요한가

fd는 파일 자체가 아니라 "현재 프로세스가 가진 열린 파일 참조"다. 그래서 `close(fd)`와 `remove(name)`을 같은 종류의 정리 작업으로 생각하면 헷갈린다.

- `close(fd)`는 현재 프로세스의 fd table 슬롯 하나를 비운다.
- `remove(name)`은 디렉터리에서 파일 이름을 지운다.
- 열린 파일 객체나 inode는 다른 참조가 남아 있으면 바로 사라지지 않을 수 있다.

이 말은 즉, fd 생명주기는 적어도 세 층으로 나눠 봐야 한다.

```text
fd number
  -> current process fd table slot
    -> open file object
      -> inode / disk blocks / device state
```

## 핵심 모델

처음에는 fd table을 포인터 배열로 생각하면 된다.

```text
fd_table base = 0x10000000

index  slot address   value
0      0x10000000     reserved stdin
1      0x10000008     reserved stdout
2      0x10000010     0x80041230  -> struct file A
3      0x10000018     0x80041580  -> struct file B
4      0x10000020     NULL
```

`close(2)`가 끝나면 핵심 변화는 이것이다.

```text
fd_table[2] = NULL
```

그 다음 `open()`은 빈 칸을 찾는다. 이 구현의 PintOS는 `2`번부터 다시 훑으므로 `2`가 비어 있으면 다시 `2`를 반환한다.

## 예시 상황

값을 넣어 보자.

```text
처음 상태
next_fd = 2
fd_table[2] = NULL
fd_table[3] = NULL

a = open("sample.txt")
  -> fd_table[2] = 0x80041230
  -> a = 2
  -> next_fd = 3

b = open("sample.txt")
  -> fd_table[3] = 0x80041580
  -> b = 3
  -> next_fd = 4

close(a)
  -> file_close(fd_table[2])
  -> fd_table[2] = NULL
  -> next_fd = 2

c = open("sample.txt")
  -> fd_table[2]가 비어 있으므로 재사용
  -> c = 2
```

중요한 점은 `a`와 `c`가 같은 숫자 `2`여도 같은 열린 파일 객체라는 뜻은 아니라는 것이다. `a`가 가리키던 `struct file`은 닫혔고, `c`는 새 `open()`이 만든 다른 `struct file`을 가리킨다.

## Linux / Windows에서는

Linux에서는 fd table 슬롯과 open file description을 나눠 생각해야 한다.

```text
process fd table
  fd 3 -> open file description
           -> file offset
           -> status flags
           -> inode/socket/pipe/device
```

`close(3)`는 현재 프로세스의 3번 슬롯을 해제한다. 그 슬롯이 마지막 참조였다면 커널의 열린 파일 상태도 정리된다. 이후 `open()`은 사용할 수 있는 낮은 fd를 다시 줄 수 있다.

또 하나 중요한 실제 OS 동작은 Unix 계열의 `unlink` 모델이다. 파일 이름을 지워도 이미 열린 fd는 계속 읽고 쓸 수 있다. 이름과 열린 파일 객체의 생명주기가 분리되어 있기 때문이다.

Windows는 `HANDLE` 모델이다. 핸들 값은 Linux fd처럼 작은 연속 index로 설명하기 어렵고, 파일 삭제도 sharing mode와 delete disposition 같은 정책에 영향을 받는다. 그래서 Windows의 `CloseHandle`과 파일 삭제 동작을 Unix `close`/`unlink`와 1:1로 단순 대응시키면 안 된다.

## PintOS에서는

이 저장소의 PintOS 구현은 fd table을 `struct thread` 안에 둔다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/thread.h`
  - `struct file **fd_table`
  - `int next_fd`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`
  - `process_add_file`
  - `process_close_file`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
  - `open`
  - `close`
  - `remove`

PintOS의 `process_add_file()`은 `next_fd`에서 시작하지 않고 `2`부터 `FD_MAX`까지 빈 칸을 찾는다.

```text
for fd = 2..FD_MAX-1:
  fd_table[fd]가 비었으면 그 칸을 사용
```

그래서 번호 재사용의 직접 원인은 "낮은 번호부터 다시 스캔한다"는 점이다. `next_fd`는 새 fd를 반환한 뒤 다음 후보처럼 갱신되고, `close()` 때 낮은 값으로 되돌아간다. 하지만 현재 코드에서 allocation 시작점으로 직접 쓰이진 않는다.

`remove(name)`은 다른 경로다.

```text
remove("sample.txt")
  -> filesys_remove("sample.txt")
  -> dir_remove(root, "sample.txt")
  -> directory entry의 in_use = false
  -> inode_remove(inode)
  -> inode->removed = true
```

열린 fd가 남아 있으면 `struct file`이 inode를 계속 잡고 있다. 그래서 `remove()`가 성공해도 그 fd로 읽고 쓰는 흐름은 남을 수 있다.

## QEMU에서는

QEMU는 PintOS의 fd 번호를 알지 못한다.

PintOS가 `close(2)`를 처리할 때 QEMU는 "guest OS 내부 배열 한 칸이 NULL이 되었다"는 의미를 해석하지 않는다. 그건 PintOS 커널 메모리 안의 자료구조 변화일 뿐이다.

QEMU가 관찰하는 것은 더 아래 층이다.

```text
PintOS remove/write/read
  -> PintOS filesys / inode / disk
  -> guest IDE I/O
  -> QEMU IDE device model
  -> QEMU BlockBackend
  -> host backing file
```

연결할 QEMU 코드는 다음 정도로 잡으면 된다.

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/hw/ide/core.c`
  - `ide_sector_read`
  - `ide_sector_write`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/block/block-backend.c`
  - `blk_aio_preadv`
  - `blk_aio_pwritev`

즉 fd 재사용은 PintOS 커널의 table 정책이고, QEMU의 역할은 그 결과로 발생하는 디스크 sector read/write를 장치처럼 처리하는 것이다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| `close(fd)` | fd/HANDLE 참조 해제 | `fd_table[fd] = NULL`, `file_close()` | guest fd 의미를 모름 |
| 번호 재사용 | Linux는 낮은 빈 fd 재사용 가능, Windows HANDLE은 다르게 봐야 함 | `2`부터 빈 칸 스캔 | 해당 없음 |
| 열린 파일 객체 | 실제 커널 VFS/object manager 구조 | `struct file`과 `struct inode` | host block backend 자료구조 |
| `remove(name)` | Unix는 이름 제거와 열린 fd 생명주기 분리 | directory entry 제거 후 inode에 `removed` 표시 | sector I/O만 처리 |
| 교육용 단순화 | refcount, 권한, lock, filesystem 종류가 복잡 | 단일 fd table page와 단순 file/inode 구조 | OS 정책 없음 |

## 코드 증거

### 1) fd table 크기

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`

핵심 정의:

```c
#define FD_MAX (PGSIZE / sizeof (struct file *))
```

x86-64에서 `PGSIZE = 4096`, 포인터 크기 `8`바이트라면 `FD_MAX = 512`다.

### 2) `open()`은 열린 파일 객체를 table에 꽂는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`

핵심 흐름:

```c
opened_file = filesys_open (file);
fd = process_add_file (opened_file);
```

`open()`은 파일 이름 문자열을 매번 들고 다니지 않는다. 한 번 열린 뒤에는 `struct file *`가 fd table에 들어간다.

### 3) `process_add_file()`은 낮은 빈 칸을 찾는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`

핵심 흐름:

```c
for (fd = 2; fd < FD_MAX; fd++) {
    if (curr->fd_table[fd] != NULL)
        continue;

    curr->fd_table[fd] = f;
    if (fd >= curr->next_fd)
        curr->next_fd = fd + 1;
    return fd;
}
```

이 코드 때문에 `fd_table[2]`가 비면 다음 `open()`이 다시 `2`를 줄 수 있다.

### 4) `process_close_file()`은 slot을 비우고 `next_fd`를 되감는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/process.c`

핵심 흐름:

```c
file_close (curr->fd_table[fd]);
curr->fd_table[fd] = NULL;
if (fd < curr->next_fd)
    curr->next_fd = fd;
```

여기서 `NULL`은 "이 fd 번호는 현재 프로세스에서 더 이상 열린 파일을 가리키지 않는다"는 뜻이다.

### 5) `remove()`는 fd table이 아니라 directory entry를 지운다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/filesys.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/directory.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/inode.c`

핵심 흐름:

```c
remove(file)
  -> filesys_remove(file)
  -> dir_remove(dir, name)
  -> e.in_use = false
  -> inode_remove(inode)
  -> inode->removed = true
```

그리고 `inode_close()`는 마지막 opener가 사라졌고 `removed`가 true일 때 block을 해제한다.

```c
if (--inode->open_cnt == 0) {
    if (inode->removed) {
        free_map_release (...);
    }
    free (inode);
}
```

이 구조가 "이름은 사라졌지만 열린 fd로는 아직 접근 가능"한 동작을 만든다.

## 숫자와 메모리

### fd slot 주소 계산

fd table이 4KB 한 페이지이고 base 주소가 `0x10000000`이라고 하자.

```text
PGSIZE                = 4096 = 0x1000
sizeof(struct file *) = 8
FD_MAX                = 4096 / 8 = 512
```

slot 주소는 이렇게 계산한다.

```text
slot(fd) = fd_table_base + fd * 8

slot(2) = 0x10000000 + 2 * 8 = 0x10000010
slot(3) = 0x10000000 + 3 * 8 = 0x10000018
slot(4) = 0x10000000 + 4 * 8 = 0x10000020
```

### close 전후 바이트 감각

`fd_table[2]`에 `0x80041230`이 들어 있었다고 하자.

```text
fd_table[2] pointer value = 0x0000000080041230
little-endian bytes       = 30 12 04 80 00 00 00 00
```

`close(2)` 뒤에는 그 8바이트가 null pointer 값으로 바뀐다.

```text
fd_table[2] pointer value = 0x0000000000000000
little-endian bytes       = 00 00 00 00 00 00 00 00
```

이 말은 즉, fd 재사용은 추상 정책이 아니라 fd table page 안의 포인터 슬롯 하나가 비는 일이다.

### remove와 open count 감각

`syn-remove` 테스트의 핵심 상황을 숫자로 줄이면 이렇다.

```text
open("deleteme")
  fd_table[2] -> struct file
  struct file -> inode X
  inode X.open_cnt >= 1
  inode X.removed = false

remove("deleteme")
  directory entry.in_use = false
  inode X.removed = true
  inode X.open_cnt는 열린 file 때문에 아직 남음

write(fd_table[2], ...)
  fd_table[2] -> struct file -> inode X
  이름 검색 없이 열린 inode로 기록

close(2)
  fd_table[2] = NULL
  file_close()
  inode_close()
  마지막 참조라면 inode/data block 해제
```

## 직접 확인

### 1) fd 번호 재사용 확인

PintOS GDB에서 breakpoint를 건다.

```gdb
b process_add_file
b process_close_file
```

테스트 흐름은 직접 작은 유저 프로그램을 만들거나 `open-twice`, `close-normal`에 breakpoint를 걸어 따라갈 수 있다.

확인할 값:

```gdb
p fd
p thread_current()->next_fd
p/x thread_current()->fd_table
p/x thread_current()->fd_table[2]
p/x thread_current()->fd_table[3]
```

관찰 질문:

- 첫 `open()` 뒤 `fd_table[2]`는 `NULL`이 아닌가?
- `close(2)` 뒤 `fd_table[2]`는 `0x0`인가?
- 그 다음 `open()`은 낮은 빈 칸을 다시 쓰는가?
- `next_fd`는 되감기지만 allocation 시작점으로 직접 쓰이지 않는다는 점이 보이는가?

### 2) 닫힌 fd를 다시 닫을 때 보기

`close-twice` 테스트는 같은 fd를 두 번 닫는다.

```gdb
b process_close_file
```

두 번째 진입에서 볼 값:

```gdb
p fd
p/x thread_current()->fd_table[fd]
```

이 구현의 `process_close_file()`은 이미 비어 있는 칸이면 그냥 return한다.

### 3) `remove(name)` 뒤 열린 fd가 살아 있는지 보기

기존 테스트:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/filesys/base/syn-remove.c`

GDB에서:

```gdb
b remove
b filesys_remove
b dir_remove
b inode_remove
b inode_close
b file_write
b file_read
```

확인할 질문:

- `remove("deleteme")`가 `fd_table[fd]`를 직접 `NULL`로 만드는가?
- `dir_remove()`에서 directory entry의 `in_use`가 false로 바뀌는가?
- `inode_remove()` 뒤 `inode->removed`가 true가 되는가?
- `write(fd, ...)`는 파일 이름 없이 `struct file *`를 따라가는가?

### 4) QEMU 쪽 관찰 포인트

QEMU에서 fd table 의미를 찾으려 하지 말고, disk I/O만 본다.

```gdb
b ide_sector_read
b ide_sector_write
```

관찰 질문:

- PintOS가 directory entry나 inode sector를 읽고 쓰는 순간이 보이는가?
- QEMU의 `IDEState`나 `BlockBackend`에 PintOS fd 번호 `2`, `3` 같은 값이 의미 있게 저장되는가?

답은 두 번째 질문에서 "아니오"여야 한다. QEMU는 guest fd가 아니라 sector 단위 장치 동작을 다룬다.

## 정리

`close(fd)`는 "파일을 이름으로 삭제한다"가 아니다. 현재 프로세스 fd table에서 한 슬롯을 비우고, 그 슬롯이 가리키던 열린 파일 객체에 대한 참조를 정리하는 일이다.

`remove(name)`은 fd table이 아니라 디렉터리 이름을 지운다. 열린 파일 객체가 inode를 잡고 있으면 이름이 사라진 뒤에도 fd로 읽고 쓸 수 있다.

PintOS에서는 이 차이가 `process_close_file()`, `dir_remove()`, `inode_remove()`, `inode_close()`로 작게 드러난다. 실제 Linux/Windows는 훨씬 복잡하지만, "번호 슬롯", "열린 객체", "파일 이름"을 분리해서 보는 기준은 그대로 중요하다.

## 다음 링크

- [[file-descriptor-knowledge]]: fd가 작은 정수인 이유와 기본 table 모델
- [[syscall-end-to-end]]: `open`, `close`, `remove` syscall 인자가 register로 들어오는 흐름
- [[mmap-file-backed-page-knowledge]]: fd를 닫아도 mapping이 유지될 수 있는 이유
- [[week-2-user-programs-map]]: User Programs 주차 전체 흐름
