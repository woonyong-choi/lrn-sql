---
type: Knowledge
status: Draft
week:
  - vm
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - week:vm
  - topic:mmap
  - topic:page-table
  - topic:fd
  - topic:gdb
  - layer:memory
  - layer:kernel
  - layer:device
  - layer:emulator
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-3-4-virtual-memory-map]]"
  - "[[lazy-file-backed-page-trace]]"
  - "[[supplemental-page-table-knowledge]]"
  - "[[page-fault-trace]]"
  - "[[frame-eviction-trace]]"
  - "[[swap-lab]]"
  - "[[file-descriptor-knowledge]]"
---
# mmap file-backed page는 어떻게 파일을 메모리처럼 보이게 하나

## 작은 질문

`read(fd, buf, 4096)`은 파일 바이트를 유저 버퍼로 복사한다.

그런데 `mmap(addr, 4096, writable, fd, 0)`을 호출하면 왜 `addr[0]`을 읽는 것만으로 파일 첫 바이트가 보일까?

```c
char *p = mmap((void *) 0x10000000, 4096, 0, fd, 0);
char c = p[123];
```

이 코드는 `read()`를 직접 호출하지 않는다. 그래도 운영체제는 파일 offset `123`의 바이트를 `p[123]`처럼 보이게 만들어야 한다.

이 말은 즉, `mmap`은 "파일 내용을 지금 복사해라"가 아니라 **이 가상 주소 범위를 이 파일의 이 offset과 연결해 두라**는 약속이다.

## 목차

- 꼬리 질문
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

## 꼬리 질문

이번 문서는 다음 질문을 한 줄로 이어서 답한다.

- 왜 파일을 메모리처럼 보이게 해야 할까?
- 실제 OS는 `mmap`을 어떻게 주소 공간의 의미로 기억할까?
- PintOS는 이 의미를 어떤 구조체와 함수로 단순화했을까?
- QEMU는 이 file-backed mapping 의미를 알고 있을까?
- 숫자를 넣으면 `addr + 5000`은 파일의 몇 번째 바이트일까?
- GDB로 어디를 보면 lazy loading과 write-back을 확인할 수 있을까?

## 왜 필요한가

파일 I/O에는 두 가지 관점이 있다.

| 방식 | 유저 코드가 보는 것 | 커널이 하는 일 |
|---|---|---|
| `read` / `write` | 파일과 버퍼 사이의 복사 | syscall마다 파일 offset에서 바이트를 복사 |
| `mmap` | 파일이 메모리 배열처럼 보임 | 주소 범위를 파일 backing 정보와 연결하고 page fault 때 채움 |

`mmap`이 필요한 이유는 "파일을 메모리처럼 다루는 인터페이스"를 만들기 위해서다.

큰 파일 일부를 랜덤 접근할 때 매번 `lseek + read`를 호출하지 않고, 포인터 연산으로 접근할 수 있다. 실제 OS에서는 실행 파일, 공유 라이브러리, 데이터베이스, IPC, 큰 힙 할당까지 `mmap` 계열 메커니즘을 넓게 쓴다.

핵심은 지연이다.

```text
mmap 호출 시점:
  가상 주소 범위와 파일 offset 관계만 기록
  물리 프레임은 아직 없을 수 있음

첫 접근 시점:
  page fault 발생
  OS가 파일에서 page 단위로 읽음
  page table에 VA -> frame 매핑 설치
  fault를 낸 명령어를 다시 실행
```

## 핵심 모델

머릿속 모델은 "주소 범위와 파일 구간을 잇는 표"다.

```text
user VA range
  0x10000000 ~ 0x10000fff
    -> file = sample.txt
    -> file offset = 0
    -> bytes from file = 4096
    -> zero bytes = 0
    -> writable = false

  0x10001000 ~ 0x10001fff
    -> file = sample.txt
    -> file offset = 4096
    -> bytes from file = 1904
    -> zero bytes = 2192
    -> writable = false
```

page table은 "지금 VA가 어느 frame으로 번역되는가"를 담는다.

[[supplemental-page-table-knowledge|supplemental page table]]은 "아직 frame은 없어도 이 VA가 어떤 파일에서 와야 하는가"를 담는다.

따라서 `mmap`은 보통 page table을 바로 채우는 작업이 아니라 SPT에 file-backed page 약속을 등록하는 작업으로 이해하면 된다.

## 예시 상황

파일 길이가 `6000`바이트이고, 사용자가 다음처럼 매핑한다고 하자.

```c
char *p = mmap((void *) 0x10000000, 6000, 1, fd, 0);
```

page size가 `4096 = 0x1000`이면 두 개의 page 약속이 생긴다.

| VA page | 파일 offset | 파일에서 읽을 바이트 | 0으로 채울 바이트 |
|---|---:|---:|---:|
| `0x10000000` | `0` | `4096` | `0` |
| `0x10001000` | `4096` | `1904` | `2192` |

이후 `p[5000]`을 읽는다고 하자.

```text
base VA        = 0x10000000
access VA      = 0x10000000 + 5000 = 0x10001388
page base      = 0x10001000
offset in page = 0x388 = 904
file offset    = 4096 + 904 = 5000
```

처음 접근이라 page table에 mapping이 없으면 [[page-fault-trace|page fault]]가 난다. OS는 SPT에서 `0x10001000`의 backing 정보를 찾고, 파일 offset `4096`부터 `1904`바이트를 읽어 frame 앞부분에 넣고 나머지 `2192`바이트를 0으로 채운다.

## Linux / Windows에서는

Linux에서 `mmap()`은 프로세스 주소 공간에 VMA를 만든다. VMA는 "이 주소 범위의 의미"를 들고 있다.

- 어떤 파일과 연결되는가
- 어느 offset부터 연결되는가
- 읽기/쓰기/실행 권한은 무엇인가
- `MAP_SHARED`인가, `MAP_PRIVATE`인가
- page fault가 나면 어떤 fault handler가 채울 것인가

파일 내용은 보통 page cache와 연결된다. `read()`와 `mmap()`은 유저 인터페이스는 다르지만, 실제 파일 page를 캐시하고 dirty page를 write-back하는 문제를 공유한다.

Windows도 파일 mapping object와 view를 통해 비슷한 문제를 푼다. Win32 API 이름은 `CreateFileMapping`, `MapViewOfFile`이고, NT 커널 내부에서는 section object와 view, page fault 처리가 연결된다.

현실 OS는 PintOS보다 훨씬 복잡하다. 공유 매핑, 사적 매핑, copy-on-write, file truncation, page cache 일관성, NUMA, huge page, 보안 권한까지 다룬다.

## PintOS에서는

PintOS는 Project 3 VM에서 `mmap`을 교육용으로 축소한다.

사용자 레벨 wrapper는 이미 있다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/lib/user/syscall.c`
  - `mmap()`은 `syscall5(SYS_MMAP, addr, length, writable, fd, offset)`을 호출한다.
  - `munmap()`은 `syscall1(SYS_MUNMAP, addr)`을 호출한다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/lib/syscall-nr.h`
  - `SYS_MMAP`
  - `SYS_MUNMAP`

하지만 현재 저장소 상태에서 커널 쪽은 아직 완성되지 않은 스켈레톤이다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
  - `syscall_handler()`의 `switch`에는 아직 `SYS_MMAP`, `SYS_MUNMAP` case가 없다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/file.c`
  - `file_backed_swap_in()`
  - `file_backed_swap_out()`
  - `do_mmap()`
  - `do_munmap()`
  - 모두 채워야 할 자리다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/vm/file.h`
  - `struct file_page`가 비어 있어 file, offset, read bytes 같은 metadata를 추가해야 한다.

PintOS에서 구현할 핵심 흐름은 다음처럼 잡으면 된다.

```text
SYS_MMAP
  -> fd로 struct file 찾기
  -> addr/length/offset 유효성 검사
  -> file_reopen(file)로 mapping 생명주기를 fd close와 분리
  -> page 단위로 VM_FILE uninit page를 SPT에 삽입
  -> 성공하면 시작 addr 반환

첫 접근 page fault
  -> page_fault()
  -> vm_try_handle_fault()
  -> spt_find_page()
  -> vm_do_claim_page()
  -> uninit_initialize()
  -> file_backed_initializer()
  -> file_backed_swap_in()
  -> file_read_at() + zero fill

munmap 또는 process exit
  -> mapping 범위 page 순회
  -> dirty이면 file_write_at()
  -> page table mapping 제거
  -> SPT에서 제거
  -> reopened file close
```

여기서 중요한 단순화가 있다.

PintOS의 `mmap` syscall은 Linux처럼 `prot`, `flags`를 받지 않는다. user wrapper의 인자는 `addr, length, writable, fd, offset`이다. 그래서 `MAP_SHARED`와 `MAP_PRIVATE`의 모든 조합을 일반화해서 다루는 모델이 아니다.

테스트가 기대하는 핵심은 "파일 backed page를 lazy load하고, writable mapping에서 dirty page를 적절히 파일에 되돌리는가"다.

## QEMU에서는

QEMU는 guest OS인 PintOS의 `mmap` 의미를 모른다.

QEMU가 보는 것은 다음뿐이다.

- guest CPU가 `syscall` instruction을 실행한다.
- PintOS 커널 코드가 guest memory 안의 `struct page`, `struct file_page`, SPT를 읽고 쓴다.
- page fault가 나면 x86 예외처럼 PintOS에 전달한다.
- PintOS가 page table을 고친 뒤 같은 명령어를 다시 실행하면, QEMU는 guest physical memory read/write를 수행한다.
- PintOS 파일 시스템이 디스크 sector를 읽거나 쓰면, QEMU IDE 장치 모델이 block backend I/O로 바꾼다.

QEMU 메모리와 디스크 경로에서 볼 수 있는 단서는 다음이다.

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/system/physmem.c`
  - guest physical address를 `MemoryRegion` 안 offset으로 바꾸고 guest RAM 바이트를 읽고 쓴다.
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/hw/ide/core.c`
  - `ide_sector_read()`
  - `ide_sector_write()`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/block/block-backend.c`
  - `blk_aio_preadv()`
  - `blk_aio_pwritev()`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/include/block/block-common.h`
  - `BDRV_SECTOR_BITS = 9`
  - `BDRV_SECTOR_SIZE = 512`

즉, PintOS `mmap`의 file offset, dirty page, `file_write_at()` 같은 의미는 QEMU 정책이 아니다. QEMU는 그 결과로 바뀐 guest RAM 바이트와 disk sector I/O를 하드웨어처럼 보이게 할 뿐이다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| `mmap` 의미 | 주소 공간에 파일/익명 object view를 만든다 | VM 과제용 file-backed mapping syscall | 모른다 |
| metadata | VMA, section, page cache, PTE, COW 등 | SPT의 `struct page`와 `struct file_page`에 직접 설계 | guest RAM, IDE device, block backend |
| page fault 처리 | 복잡한 VM 서브시스템 | `page_fault()` -> `vm_try_handle_fault()` -> `swap_in()` | #PF와 memory access를 하드웨어처럼 재현 |
| write-back | dirty tracking, page cache, msync/munmap/eviction 정책 | dirty page이면 `file_write_at()`로 되돌리는 단순 모델 | guest 파일 시스템 의미를 모름 |
| API | `addr, length, prot, flags, fd, offset` | `addr, length, writable, fd, offset` | guest API 없음 |

## 코드 증거

### 1. 유저 프로그램은 SYS_MMAP 번호로 커널에 들어간다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/lib/user/syscall.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/lib/syscall-nr.h`

핵심:

```c
void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
    return (void *) syscall5 (SYS_MMAP, addr, length, writable, fd, offset);
}

void
munmap (void *addr) {
    syscall1 (SYS_MUNMAP, addr);
}
```

`SYS_MMAP`, `SYS_MUNMAP` 번호도 enum에 있다.

### 2. 현재 커널 syscall_handler에는 mmap case가 아직 없다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`

현재 `switch (f->R.rax)`는 `SYS_HALT`부터 `SYS_TELL` 계열까지만 처리하고, default로 빠진다.

이 말은 즉, 지금 코드 상태에서는 `mmap()`을 호출해도 커널이 `do_mmap()`으로 연결하지 않는다. VM 과제 구현에서는 `SYS_MMAP`과 `SYS_MUNMAP` case를 추가해야 한다.

### 3. file-backed page 연산 테이블은 준비되어 있다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/file.c`

핵심 구조:

```c
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};
```

이 테이블은 "이 page가 file-backed일 때 page fault로 들어오면 어떤 함수를 부를지"를 정한다.

### 4. lazy page는 첫 fault 때 구체 타입으로 바뀐다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/uninit.c`

핵심:

```c
return uninit->page_initializer (page, uninit->type, kva) &&
    (init ? init (page, aux) : true);
```

처음에는 `VM_UNINIT`으로 SPT에 들어가 있다가, 첫 접근 때 `file_backed_initializer()`가 page operations를 file-backed로 바꾸고, 이어서 초기화 callback이 파일 내용을 frame에 채운다.

### 5. 실제 파일 바이트는 file_read_at/file_write_at으로 오간다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/file.c`

핵심:

```c
off_t
file_read_at (struct file *file, void *buffer, off_t size, off_t file_ofs) {
    return inode_read_at (file->inode, buffer, size, file_ofs);
}

off_t
file_write_at (struct file *file, const void *buffer, off_t size,
               off_t file_ofs) {
    return inode_write_at (file->inode, buffer, size, file_ofs);
}
```

`mmap` page-in은 현재 file position을 움직이면 안 되므로 `file_read()`보다 `file_read_at()`이 맞다. write-back도 같은 이유로 `file_write_at()`을 써야 한다.

### 6. 테스트가 요구하는 동작

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/mmap-read.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/mmap-write.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/mmap-clean.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/mmap-off.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/mmap-bad-off.c`

테스트에서 읽을 수 있는 요구사항:

- 파일을 매핑한 뒤 `memcmp(actual, sample, ...)`로 메모리 접근이 파일 내용과 같아야 한다.
- 파일 끝 뒤 같은 page의 나머지 바이트는 0이어야 한다.
- writable mapping에서 메모리로 쓴 뒤 `munmap()`하면 파일에 반영되어야 한다.
- clean page는 `munmap()` 때 덮어쓰면 안 된다.
- page-aligned nonzero offset은 파일 중간을 mapping하는 정상 사례로 동작해야 한다.
- offset이 page-aligned가 아니면 실패해야 한다.

## 숫자와 메모리

다시 `length = 6000` 예시를 바이트 단위로 보자.

```text
PGSIZE      = 4096 = 0x1000
addr        = 0x10000000
length      = 6000 = 0x1770
file offset = 0

page 0:
  va          = 0x10000000
  file offset = 0
  read bytes  = 4096
  zero bytes  = 0

page 1:
  va          = 0x10001000
  file offset = 4096
  read bytes  = 6000 - 4096 = 1904 = 0x770
  zero bytes  = 4096 - 1904 = 2192 = 0x890
```

`p[5000]`은 두 번째 page 안의 `904`번째 바이트다.

```text
5000 decimal = 0x1388
0x1388 - 0x1000 = 0x388

access VA      = 0x10001388
page base      = 0x10001000
offset in page = 0x388
file offset    = 0x1000 + 0x388 = 0x1388
```

메모리 내용은 이렇게 채워져야 한다.

```text
frame for 0x10001000:
  byte 0x000 ~ 0x76f : file offset 0x1000 ~ 0x176f
  byte 0x770 ~ 0xfff : 00 00 00 ... 00
```

dirty write-back은 같은 page 안에서도 파일에 대응되는 구간만 조심해야 한다.

```text
page 1 전체 4096B 중:
  file-backed bytes = 1904B
  zero-fill tail    = 2192B

munmap 때 쓰기 후보:
  file_write_at(file, kva, 1904, 4096)
```

0으로 채운 tail까지 파일에 쓰면 원래 파일 길이를 넘어서는 write가 된다. PintOS의 기본 파일 시스템은 보통 파일 확장을 구현하지 않았으므로, 이 구간은 "파일 내용"이 아니라 page 안의 padding으로 다루는 편이 맞다.

## 직접 확인

PintOS 구현 중이라면 GDB에서 다음 지점을 잡는다.

```gdb
break syscall_handler
break do_mmap
break file_backed_swap_in
break file_backed_swap_out
break do_munmap
break file_read_at
break file_write_at
```

syscall 진입 직후 확인할 register:

```gdb
p/x f->R.rax
p/x f->R.rdi
p/x f->R.rsi
p/x f->R.rdx
p/x f->R.r10
p/x f->R.r8
```

x86-64 PintOS syscall 인자 관점에서는 다음처럼 해석한다.

```text
RAX = SYS_MMAP
RDI = addr
RSI = length
RDX = writable
R10 = fd
R8  = offset
```

page fault 이후에는 다음을 본다.

```gdb
p/x page->va
p page_get_type(page)
x/32xb page->frame->kva
```

write-back은 `mmap-write`와 `mmap-clean`을 비교하면 좋다.

- `mmap-write`: mapping을 통해 쓴 page가 `munmap()` 뒤 파일에 반영되어야 한다.
- `mmap-clean`: mapping으로 수정하지 않은 clean page를 `munmap()`이 다시 쓰면 안 된다.

회귀 테스트 후보:

```text
cd /Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos
make -C build check TESTS=tests/vm/mmap-read
make -C build check TESTS=tests/vm/mmap-write
make -C build check TESTS=tests/vm/mmap-clean
make -C build check TESTS=tests/vm/mmap-off
make -C build check TESTS=tests/vm/lazy-file
```

현재 저장소의 VM 구현은 아직 TODO가 많으므로, 위 명령은 지금 통과 보장용이 아니라 구현 후 회귀 확인용이다.

## 정리

- `mmap`은 파일을 즉시 복사하는 호출이 아니라, VA 범위와 파일 offset의 관계를 등록하는 호출이다.
- 실제 OS는 VMA/section/page cache 같은 큰 VM 구조로 이 관계를 관리한다.
- PintOS는 SPT, `struct page`, `struct file_page`, `file_backed_swap_in/out`으로 핵심만 구현하게 만든다.
- anonymous page는 swap slot이 backing이고, file-backed page는 원본 파일 offset과 dirty write-back이 backing이다.
- QEMU는 PintOS의 `mmap` 의미를 모르고, guest CPU, guest RAM, IDE disk I/O만 에뮬레이션한다.
- 숫자로는 page base, page offset, file offset, read bytes, zero bytes를 계속 맞춰야 한다.

## 다음 링크

- [[lazy-file-backed-page-trace]]: `mmap()` 직후 frame 없이 SPT 약속만 남기고 첫 접근 때 file page를 읽는 흐름
- [[supplemental-page-table-knowledge]]: `mmap` 약속을 page fault 때 다시 찾는 의미 장부
- [[page-fault-trace]]: file-backed page가 실제 frame으로 바뀌는 관문
- [[frame-eviction-trace]]: file-backed page가 밀려날 때 write-back 판단으로 이어지는 흐름
- [[swap-lab]]: anonymous page가 swap slot을 backing으로 삼는 반대 사례
- [[file-descriptor-knowledge]]: `fd` 숫자가 `struct file`로 해석되는 방식
- [[address-translation-memory]]: file-backed page가 결국 VA -> frame mapping이 되는 과정
