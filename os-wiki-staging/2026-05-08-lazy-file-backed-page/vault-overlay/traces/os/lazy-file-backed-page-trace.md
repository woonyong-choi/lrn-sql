---
type: Trace
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
  - topic:frame
  - topic:gdb
  - layer:memory
  - layer:kernel
  - layer:device
  - layer:emulator
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-3-4-virtual-memory-map]]"
  - "[[mmap-file-backed-page-knowledge]]"
  - "[[supplemental-page-table-knowledge]]"
  - "[[page-fault-trace]]"
  - "[[frame-eviction-trace]]"
---

# Lazy File-Backed Page Trace

## 작은 질문

`mmap()`은 파일을 메모리에 붙인다고 말한다.

그런데 좋은 `mmap` 구현은 호출 직후에 파일 전체를 RAM으로 읽지 않는다. `lazy-file` 테스트는 이 점을 직접 검사한다.

```c
map = mmap((void *) 0x10000000, length, 0, fd, 0);
pa = get_phys_addr(&actual[0]);
CHECK(pa == 0, "check if page is not loaded");
```

작은 질문은 이것이다.

> `mmap()`은 성공했는데, 왜 첫 접근 전에는 물리 주소가 없어야 할까?

답은 `mmap`이 "지금 읽기"가 아니라 "나중에 page fault가 나면 읽을 약속을 SPT에 남기기"이기 때문이다.

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

이번 Trace는 다음 질문을 순서대로 따라간다.

- 왜 `mmap()` 직후에 page table mapping을 만들면 lazy가 아닐까?
- 실제 OS는 "아직 안 읽었지만 합법인 file page"를 어디에 기억할까?
- PintOS에서는 왜 `VM_UNINIT`으로 태어난 뒤 `VM_FILE`로 바뀌어야 할까?
- `aux`에는 어떤 숫자와 포인터가 들어가야 첫 fault 때 파일을 읽을 수 있을까?
- QEMU는 lazy file page라는 의미를 알고 있을까?
- `lazy-file` 테스트의 `get_phys_addr()`는 무엇을 확인하는가?

## 왜 필요한가

파일 매핑의 핵심 이익은 필요한 page만 읽는 것이다.

예를 들어 10MB 파일을 `mmap()`했지만 프로그램이 첫 4KB만 읽고 끝난다면, 나머지 9MB 이상을 미리 RAM에 올릴 이유가 없다.

```text
즉시 로딩:
  mmap 호출 시 파일 page 전체를 읽음
  단순하지만 RAM과 I/O를 낭비할 수 있음

lazy loading:
  mmap 호출 시 page별 약속만 기록
  첫 접근 page fault 때 해당 page만 읽음
```

이 말은 즉, lazy file-backed page는 [[supplemental-page-table-knowledge|SPT]]와 [[page-fault-trace|page fault]]가 협력해야 한다.

## 핵심 모델

흐름을 한 줄로 잡으면 다음과 같다.

```text
SYS_MMAP
  -> do_mmap()
  -> page별 VM_UNINIT page를 SPT에 등록
  -> 첫 메모리 접근
  -> #PF
  -> vm_try_handle_fault()
  -> vm_do_claim_page()
  -> uninit_initialize()
  -> file_backed_initializer()
  -> file_backed_swap_in()
  -> file_read_at() + zero fill
  -> pml4_set_page()
  -> 원래 load 명령어 재실행
```

여기서 `VM_UNINIT`은 "아직 frame에 내용이 없지만, 첫 접근 때 어떤 타입으로 초기화할지 알고 있는 page"다.

`VM_FILE`은 "파일 offset을 backing으로 가진 page"다.

따라서 `mmap()` 직후의 page는 보통 `VM_FILE` 의미를 품은 `VM_UNINIT` page로 시작한다. 첫 fault에서 실제 frame을 얻고, `file_backed_initializer()`를 거치며 file-backed page로 바뀐다.

## 예시 상황

파일 크기가 `6000`바이트라고 하자.

```c
char *actual = (char *) 0x10000000;
void *map = mmap(actual, 8192, 0, fd, 0);
```

`mmap()`이 성공하면 SPT에는 이런 약속 두 개가 들어가야 한다.

| SPT key | type before fault | file offset | read bytes | zero bytes |
|---|---|---:|---:|---:|
| `0x10000000` | `VM_UNINIT`, target `VM_FILE` | `0` | `4096` | `0` |
| `0x10001000` | `VM_UNINIT`, target `VM_FILE` | `4096` | `1904` | `2192` |

아직 page table에는 present mapping이 없어야 한다.

```text
get_phys_addr(0x10000000) == 0
get_phys_addr(0x10001000) == 0
```

이후 `actual[0]`을 읽으면 첫 page만 load된다.

```text
get_phys_addr(0x10000000) != 0
get_phys_addr(0x10001000) == 0
```

그 다음 `actual[4096]`을 읽으면 두 번째 page도 load된다.

## Linux / Windows에서는

Linux에서 `mmap()`은 주소 범위의 의미를 VMA에 남긴다. 파일 내용은 보통 첫 접근 page fault 때 page cache를 통해 준비된다.

Windows도 file mapping object와 view를 만든 뒤, 실제 physical page 연결은 접근과 fault 처리에 따라 지연될 수 있다.

현실 OS는 PintOS보다 훨씬 복잡하다. 공유 매핑, 사적 매핑, copy-on-write, page cache 일관성, 파일 크기 변경, 권한, NUMA, memory pressure까지 함께 고려한다.

그래도 핵심 질문은 같다.

> 이 주소는 아직 resident가 아니지만, 원래 합법적인 file-backed 주소인가?

## PintOS에서는

PintOS는 Project 3에서 이 흐름을 직접 구현하게 만든다.

현재 저장소 기준으로는 뼈대가 있고, file-backed lazy loading의 핵심 함수들은 아직 TODO 상태다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/lib/user/syscall.c`
  - `mmap()` wrapper가 `syscall5(SYS_MMAP, addr, length, writable, fd, offset)`을 호출한다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
  - `syscall_handler()`에는 아직 `SYS_MMAP`, `SYS_MUNMAP` case가 없다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/file.c`
  - `do_mmap()`, `do_munmap()`, `file_backed_swap_in()`, `file_backed_swap_out()`이 비어 있다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/uninit.c`
  - `uninit_initialize()`가 첫 fault 때 구체 page 타입으로 바꾸는 공통 관문이다.

구현자가 만들어야 하는 핵심 metadata는 page마다 다르다.

```text
file-backed aux:
  file        = file_reopen(fd의 struct file)
  offset      = 이 page가 읽을 파일 시작 offset
  read_bytes  = 파일에서 읽을 바이트 수
  zero_bytes  = frame 나머지를 0으로 채울 바이트 수
  writable    = page table에 줄 쓰기 권한
```

`file_reopen()`이 중요한 이유는 fd close와 mapping 생명주기를 분리하기 위해서다. `mmap-close` 계열 테스트는 파일 descriptor를 닫아도 mapping이 남아 있어야 하는 상황을 확인한다.

## QEMU에서는

QEMU는 lazy file-backed page라는 정책을 모른다.

QEMU가 보는 사건은 더 낮은 층이다.

```text
1. guest user code가 0x10000000을 읽으려 함
2. guest page table에 present mapping이 없음
3. QEMU가 x86 page walk를 흉내 내다가 #PF를 guest에 전달
4. PintOS page_fault()가 실행됨
5. PintOS가 file_read_at()으로 guest disk에서 bytes를 읽음
6. PintOS가 page table mapping을 설치함
7. 같은 guest instruction이 다시 실행됨
```

QEMU의 x86 TCG 경로에서는 page table walk 중 present bit가 없으면 `EXCP0E_PAGE` fault를 만들고, faulting address를 CR2로 전달한다.

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/system/excp_helper.c`
  - `mmu_translate()`
  - `x86_cpu_tlb_fill()`
  - `env->cr[2] = err.cr2`

store 접근이면 QEMU는 x86 하드웨어처럼 PTE dirty bit 의미도 흉내 낸다. 같은 파일의 page walk 경로에서 store일 때 `PG_DIRTY_MASK`가 set 후보에 들어간다.

하지만 "이 dirty bit를 보고 file에 write-back할지"는 PintOS 정책이다. QEMU는 그 의미를 알지 않는다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| lazy file 의미 저장 | VMA/section/page cache 등 | SPT의 `VM_UNINIT` page와 `aux` | 없음 |
| 첫 접근 처리 | 커널 VM fault handler | `page_fault()` -> `vm_try_handle_fault()` | #PF 전달만 재현 |
| 파일 page-in | page cache와 filesystem 경로 | `file_backed_swap_in()`에서 `file_read_at()` | guest disk I/O를 장치처럼 처리 |
| dirty write-back | 복잡한 writeback/msync/munmap 정책 | `pml4_is_dirty()`와 `file_write_at()` 기반 단순 모델 | PTE dirty bit 효과만 흉내 |
| 테스트 관점 | OS별 API와 정책 다양 | `lazy-file`, `mmap-*`, `swap-file` | 테스트 의미를 모름 |

## 코드 증거

### 1. 유저 wrapper는 다섯 인자를 register에 싣는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/lib/user/syscall.c`

핵심:

```c
void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
    return (void *) syscall5 (SYS_MMAP, addr, length, writable, fd, offset);
}
```

x86-64 syscall register 배치는 다음처럼 읽으면 된다.

```text
RAX = SYS_MMAP
RDI = addr
RSI = length
RDX = writable
R10 = fd
R8  = offset
```

### 2. `VM_UNINIT`은 첫 fault 때 타입을 바꾼다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/uninit.c`

핵심:

```c
return uninit->page_initializer(page, uninit->type, kva) &&
    (init ? init(page, aux) : true);
```

이 줄은 두 단계를 합친다.

1. `page_initializer()`가 `struct page`를 목표 타입으로 바꾼다.
2. `init(page, aux)`가 실제 내용을 frame에 채운다.

file-backed mmap에서는 목표 타입이 `VM_FILE`이고, initializer는 `file_backed_initializer()`가 된다.

### 3. file-backed page 연산 테이블은 이미 갈고리만 있다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/file.c`

핵심:

```c
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};
```

`file_backed_initializer()`는 `page->operations = &file_ops`로 바꾸는 자리다. 그 뒤부터 `swap_in(page, kva)`는 file-backed용 `file_backed_swap_in()`으로 dispatch된다.

### 4. `lazy-file` 테스트는 resident 여부를 직접 본다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/lazy-file.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/lib/user/syscall.h`

`lazy-file.c`는 `get_phys_addr(&actual[i * PAGE_SIZE])`로 page가 실제 물리 frame에 연결됐는지 확인한다.

`get_phys_addr()`는 user test용 interrupt를 사용한다.

```c
asm volatile ("movq %0, %%rax" ::"r"(user_addr));
asm volatile ("int $0x42");
asm volatile ("\t movq %%rax, %0": "=r" (pa));
```

따라서 `pa == 0`은 "mapping 약속은 있지만 아직 present mapping은 없다"는 테스트 신호다.

### 5. 파일 위치를 흔들지 않으려면 `_at` 함수를 쓴다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/filesys/file.c`

핵심:

```c
file_read_at(file, buffer, size, file_ofs);
file_write_at(file, buffer, size, file_ofs);
```

`mmap` page-in/write-back은 page마다 고정된 파일 offset을 사용해야 한다. `file_read()`처럼 file object의 현재 위치를 움직이는 함수에 기대면 `read()` syscall, 다른 page fault, 같은 file object 공유와 섞여 버그가 된다.

## 숫자와 메모리

`small.txt`가 `5000`바이트이고, 테스트가 page-aligned 길이 `8192`로 매핑한다고 해보자.

```text
PGSIZE      = 4096 = 0x1000
base addr   = 0x10000000
file length = 5000 = 0x1388
map length  = 8192 = 0x2000
```

page별 약속은 이렇게 된다.

```text
page 0:
  va          = 0x10000000
  file offset = 0
  read bytes  = 4096
  zero bytes  = 0

page 1:
  va          = 0x10001000
  file offset = 4096
  read bytes  = 5000 - 4096 = 904 = 0x388
  zero bytes  = 4096 - 904 = 3192 = 0xc78
```

`actual[5000]`은 두 번째 page의 zero-fill 영역 첫 바이트다.

```text
access VA      = 0x10000000 + 5000 = 0x10001388
page base      = 0x10001000
offset in page = 0x388

page 1에서:
  file bytes = offset 0x000 ~ 0x387
  zero bytes = offset 0x388 ~ 0xfff
```

이 바이트를 처음 읽으면 page fault가 나고, `file_backed_swap_in()`은 frame을 이렇게 채워야 한다.

```text
frame bytes:
  0x000 ~ 0x387 = file offset 0x1000 ~ 0x1387
  0x388 ~ 0xfff = 00 00 00 ... 00
```

write-back도 같은 숫자를 지켜야 한다. dirty page라고 해서 4096B 전체를 파일에 쓰면, 원래 파일 뒤 zero-fill padding까지 파일 내용처럼 취급하는 문제가 생긴다.

## 직접 확인

구현 중에는 다음 breakpoint 순서가 좋다.

```gdb
b syscall_handler
b do_mmap
b vm_alloc_page_with_initializer
b spt_insert_page
b page_fault
b vm_try_handle_fault
b vm_do_claim_page
b uninit_initialize
b file_backed_initializer
b file_backed_swap_in
b file_read_at
b pml4_set_page
```

`mmap()` 진입 직후에는 register를 본다.

```gdb
p/x f->R.rax
p/x f->R.rdi
p/x f->R.rsi
p/x f->R.rdx
p/x f->R.r10
p/x f->R.r8
```

`do_mmap()` 이후, 첫 접근 전에는 page table mapping이 없어야 한다.

```gdb
p/x pml4_get_page(thread_current()->pml4, (void *) 0x10000000)
```

첫 fault 이후에는 같은 주소가 frame으로 연결되어야 한다.

```gdb
p/x page->va
p page_get_type(page)
p/x page->frame->kva
x/32xb page->frame->kva
```

테스트로는 다음 흐름을 분리해서 본다.

```text
make -C build check TESTS=tests/vm/lazy-file
make -C build check TESTS=tests/vm/mmap-read
make -C build check TESTS=tests/vm/mmap-clean
make -C build check TESTS=tests/vm/swap-file
```

현재 저장소의 VM 구현은 아직 TODO가 많다. 그래서 위 명령은 "지금 통과해야 한다"가 아니라, 구현 후 lazy/resident/write-back 흐름을 검증하는 기준이다.

## 정리

- `mmap()` 직후에 page table present mapping이 생기면 lazy loading이 아니다.
- PintOS에서는 file-backed mapping을 `VM_UNINIT` page로 SPT에 넣고, 첫 fault 때 `VM_FILE` page로 바꾸는 흐름이 자연스럽다.
- `aux`는 file pointer, file offset, read bytes, zero bytes, writable 같은 "나중에 읽을 근거"를 보관해야 한다.
- `lazy-file` 테스트는 `get_phys_addr()`로 "아직 frame이 없는 합법 page"와 "접근 후 frame이 생긴 page"를 구분한다.
- QEMU는 lazy file page 정책을 모르고, page fault 전달, guest page table bit, guest memory/disk I/O만 하드웨어처럼 재현한다.

## 다음 링크

- [[mmap-file-backed-page-knowledge]]: file-backed mapping의 큰 개념과 API 차이
- [[supplemental-page-table-knowledge]]: 아직 resident가 아닌 page 의미를 기억하는 장부
- [[page-fault-trace]]: #PF가 `vm_try_handle_fault()`로 들어오는 관문
- [[frame-eviction-trace]]: file-backed page가 밀려날 때 dirty/write-back으로 이어지는 흐름
- [[swap-lab]]: anonymous page가 file이 아니라 swap slot을 backing으로 삼는 반대 사례
