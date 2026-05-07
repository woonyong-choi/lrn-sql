---
type: Lab
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
  - layer:memory
  - layer:kernel
  - topic:page-table
  - topic:frame
  - topic:gdb
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-3-4-virtual-memory-map]]"
  - "[[page-table-entry-bits-knowledge]]"
  - "[[frame-eviction-trace]]"
  - "[[tlb-cr3-address-space-switch-knowledge]]"
---

# PTE accessed/dirty bit는 eviction에서 어떻게 보이나

## 작은 질문

프레임이 부족하면 운영체제는 어떤 페이지를 내보내야 한다.

그런데 커널은 어떻게 "이 페이지는 최근에 쓰였나?", "이 페이지는 수정됐나?"를 알까?

처음에는 `struct page` 안에 그런 필드가 있을 것 같지만, x86에서는 중요한 단서가 page table entry, 즉 PTE 안에도 들어 있다.

> `accessed` bit는 최근 접근 여부를, `dirty` bit는 수정 여부를 알려주는 하드웨어 단서다.

## 왜 필요한가

eviction은 아무 프레임이나 덮어쓰는 일이 아니다.

운영체제는 두 가지를 판단해야 한다.

1. 최근에 계속 쓰는 page를 피할 수 있는가?
2. 수정된 page라면 swap이나 file backing에 먼저 저장해야 하는가?

이 판단을 위해 PTE의 `accessed` bit와 `dirty` bit가 쓰인다.

```text
accessed = 1
  -> 최근 read/write/execute가 있었다는 단서

dirty = 1
  -> page가 수정되었으므로 그냥 버리면 데이터 손실 가능
```

이 말은 즉, frame eviction은 `struct frame` 목록만 보는 문제가 아니라 page table bit를 함께 읽는 문제다.

## 핵심 모델

머릿속에는 다음 모델을 넣으면 된다.

```text
CPU가 VA 접근
  -> page table walk
  -> PTE present/writable/user 확인
  -> 접근이 성공하면 accessed bit가 켜질 수 있음
  -> 쓰기 접근이면 dirty bit가 켜질 수 있음

커널 eviction 정책
  -> PTE accessed bit를 샘플링
  -> 필요하면 accessed bit를 0으로 지움
  -> 다음 순회 때 다시 1이 되었는지 확인
  -> dirty bit가 1이면 swap/file write-back 필요
```

`accessed`는 "절대 내보내면 안 된다"는 뜻이 아니다. 최근에 접근했다는 힌트다.

`dirty`는 "내용이 backing store와 달라졌을 수 있다"는 뜻이다. 그래서 dirty page를 그냥 버리면 파일 변경이나 anonymous page 내용이 사라질 수 있다.

## 예시 상황

page A, B, C가 있고 frame은 두 개뿐이라고 하자.

```text
F0 -> page A
F1 -> page B

새 page C를 올려야 함
```

clock 알고리즘 계열 정책은 이런 식으로 볼 수 있다.

```text
1차 순회:
  A.accessed = 1 -> 최근 썼으니 A.accessed = 0으로 지우고 한 번 봐줌
  B.accessed = 0 -> victim 후보

B.dirty 확인:
  dirty = 0 -> 그냥 mapping 제거 가능
  dirty = 1 -> swap/file에 먼저 저장한 뒤 mapping 제거
```

이 방식은 완벽한 LRU는 아니지만, "최근에 참조된 page를 한 번 더 살려준다"는 감각을 준다.

## Linux / Windows에서는

Linux와 Windows도 하드웨어가 제공하는 accessed/dirty 계열 단서를 VM 정책에 활용한다.

다만 현실 OS는 이 bit만 보고 단순하게 victim을 고르지 않는다.

| OS | 현실에서 함께 고려하는 것 |
|---|---|
| Linux | active/inactive LRU, page cache, anonymous/file-backed page, cgroup, NUMA, writeback 상태 |
| Windows | working set, standby/modified page list, section object, pagefile, memory compression |

실제 OS는 SMP, TLB shootdown, page cache 일관성, 파일 write-back, 메모리 압축까지 함께 다룬다. PintOS는 그중 "page table bit를 읽고 eviction/write-back 판단에 연결한다"는 핵심만 남긴다.

## PintOS에서는

PintOS에서 PTE bit는 다음 파일에 정의되어 있다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/pte.h`

```c
#define PTE_A 0x20
#define PTE_D 0x40
```

PintOS는 이 bit를 읽고 지우는 helper를 이미 제공한다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/mmu.c`
  - `pml4_is_accessed()`
  - `pml4_set_accessed()`
  - `pml4_is_dirty()`
  - `pml4_set_dirty()`
  - `pml4_clear_page()`

현재 VM 스켈레톤에서 eviction 정책 자체는 아직 TODO다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/vm.c`
  - `vm_get_victim()`
  - `vm_evict_frame()`
  - `vm_get_frame()`

따라서 이 Lab의 목표는 "이미 구현되어 있다"고 착각하는 것이 아니라, eviction을 구현할 때 어떤 bit를 어디서 읽어야 하는지 눈으로 확인하는 것이다.

## QEMU에서는

QEMU는 PintOS의 victim 선택 정책을 모른다.

대신 QEMU는 guest CPU의 x86 page walk를 흉내 내면서 PTE accessed/dirty bit가 켜지는 하드웨어 효과를 재현한다.

대표 근거:

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/system/excp_helper.c`
  - `PG_ACCESSED_MASK`
  - `PG_DIRTY_MASK`
  - `ptw_setl()`
  - page walk 중 accessed/dirty bit를 set하는 흐름

QEMU 관점의 흐름은 이렇게 읽으면 된다.

```text
guest VA 접근
  -> QEMU가 guest page table walk를 에뮬레이션
  -> 접근 성공이면 guest PTE의 accessed bit 효과를 반영
  -> store 접근이면 guest PTE의 dirty bit 효과도 반영
  -> 실패하면 #PF를 guest PintOS로 전달
```

여기서 중요한 구분이 있다.

QEMU가 `dirty` bit를 켜는 것은 "하드웨어가 PTE를 갱신한 것처럼 보이게 하는 일"이다. 어떤 page를 swap out할지, file에 write-back할지는 PintOS가 정한다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| accessed bit 사용 | reclaim/working set 판단의 여러 단서 중 하나 | eviction 정책 구현 시 사용할 수 있는 핵심 단서 | guest PTE에 하드웨어 효과를 재현 |
| dirty bit 사용 | page cache/writeback/swap/pagefile 정책과 연결 | anon swap-out, file-backed write-back 판단에 연결 | store access의 하드웨어 효과를 재현 |
| victim 선택 | 복잡한 VM 정책 | `vm_get_victim()`에 직접 구현 | 정책 없음 |
| 단순화 | 매우 복잡 | 교육용 frame table과 PTE helper 중심 | guest OS 의미를 모름 |

## 코드 증거

### 1. PTE bit 값은 낮은 bit에 있다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/pte.h`

핵심 값:

```c
#define PTE_P 0x1
#define PTE_W 0x2
#define PTE_U 0x4
#define PTE_A 0x20
#define PTE_D 0x40
```

PTE의 낮은 12비트는 flag로 쓰이고, frame base 주소는 위쪽 bit에 들어간다.

### 2. PintOS helper는 PTE를 직접 읽고 쓴다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/mmu.c`

핵심 흐름:

```c
bool
pml4_is_accessed (uint64_t *pml4, const void *vpage) {
    uint64_t *pte = pml4e_walk (pml4, (uint64_t) vpage, false);
    return pte != NULL && (*pte & PTE_A) != 0;
}
```

`pml4_set_accessed()`와 `pml4_set_dirty()`는 bit를 바꾼 뒤, 현재 CPU가 쓰는 주소 공간이면 `invlpg`를 호출한다.

이 말은 즉, PTE를 바꾼 뒤 낡은 TLB entry가 남아 있지 않게 조심한다는 뜻이다.

### 3. eviction 자리는 아직 정책 TODO다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/vm.c`

스켈레톤의 핵심 빈칸:

```c
static struct frame *
vm_get_victim (void) {
    struct frame *victim = NULL;
    /* TODO: The policy for eviction is up to you. */
    return victim;
}
```

여기에 clock 계열 정책을 넣는다면 보통 다음 helper들이 후보가 된다.

```text
pml4_is_accessed(thread_current()->pml4, page->va)
pml4_set_accessed(thread_current()->pml4, page->va, false)
pml4_is_dirty(thread_current()->pml4, page->va)
```

실제 구현에서는 victim page의 소유 process page table을 봐야 하므로, 단순히 `thread_current()->pml4`만 쓰면 안 되는 경우도 생각해야 한다. frame table에 page owner나 page table 기준을 어떻게 추적할지 설계가 필요하다.

### 4. QEMU는 accessed/dirty set 효과를 에뮬레이션한다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/system/excp_helper.c`

QEMU의 page walk는 access type에 따라 set할 bit를 만든다.

```c
uint32_t set = PG_ACCESSED_MASK;
if (access_type == MMU_DATA_STORE) {
    set |= PG_DIRTY_MASK;
}
```

그리고 `ptw_setl()`로 guest page table entry에 bit set 효과를 반영한다.

이 코드는 QEMU가 PintOS의 `struct page`를 이해한다는 뜻이 아니다. guest CPU가 page table walk 중 PTE A/D bit를 갱신하는 하드웨어 의미를 재현한다는 뜻이다.

## 숫자와 메모리

### 1. PTE 값 하나를 bit로 분해하기

처음 PTE가 다음 값이라고 하자.

```text
PTE = 0x0000000012345007
```

분해하면:

```text
frame base = PTE & ~0xfff
           = 0x0000000012345000

flags      = PTE & 0xfff
           = 0x007
```

`0x007`은 present/write/user만 켜진 상태다.

```text
0x001 PTE_P present
0x002 PTE_W writable
0x004 PTE_U user
0x020 PTE_A accessed = 아직 0
0x040 PTE_D dirty    = 아직 0
```

### 2. read 뒤에는 accessed가 켜질 수 있다

read 접근 후:

```text
PTE = 0x0000000012345007 | 0x20
    = 0x0000000012345027

flags = 0x027
```

이 상태는 "최근 접근됨, 하지만 아직 수정됐다는 단서는 없음"이다.

### 3. write 뒤에는 dirty가 같이 켜질 수 있다

write 접근 후:

```text
PTE = 0x0000000012345027 | 0x40
    = 0x0000000012345067

flags = 0x067
```

이제 `accessed=1`, `dirty=1`이다.

victim으로 고르면 anonymous page는 swap에, file-backed dirty page는 파일에 보존해야 한다.

### 4. accessed만 지우면 다시 관찰할 수 있다

clock 알고리즘은 accessed bit를 일부러 0으로 지운다.

```text
before = 0x0000000012345067
after  = before & ~0x20
       = 0x0000000012345047
```

dirty bit `0x40`은 남아 있다. 이 상태에서 프로그램이 다시 page를 읽거나 쓰면 accessed bit가 다시 켜질 수 있다.

## 직접 확인

### 1. PTE helper breakpoint 잡기

PintOS VM을 구현 중이라면 먼저 helper가 호출되는지 본다.

```gdb
b pml4_is_accessed
b pml4_set_accessed
b pml4_is_dirty
b pml4_set_dirty
b pml4_clear_page
b vm_get_victim
b vm_evict_frame
```

현재 스켈레톤에서는 `vm_get_victim()`과 `vm_evict_frame()`이 TODO라 바로 의미 있는 victim 선택이 보이지 않을 수 있다. 구현을 채운 뒤 다시 보면 된다.

### 2. 특정 page의 PTE 값 보기

`page->va`가 page-aligned user virtual address라고 하자.

```gdb
set $pml4 = thread_current()->pml4
set $upage = page->va
set $pte = pml4e_walk($pml4, (uint64_t)$upage, 0)
p/x *$pte
p/x *$pte & 0x20
p/x *$pte & 0x40
```

관찰 기준:

```text
*$pte & 0x20 != 0  -> accessed
*$pte & 0x40 != 0  -> dirty
```

### 3. accessed bit를 지운 뒤 다시 접근시키기

clock 정책을 확인할 때는 이런 순서가 좋다.

```gdb
call pml4_set_accessed($pml4, $upage, 0)
p/x *$pte
continue
```

이후 같은 page를 다시 읽으면 `0x20` bit가 다시 켜지는지 확인한다.

주의할 점이 있다. 이 확인은 page가 실제로 다시 접근되어야 의미가 있다. 단순히 GDB에서 변수만 보고 있으면 guest 프로그램이 해당 VA를 다시 읽었다고 볼 수 없다.

### 4. dirty page write-back 경계 보기

file-backed page를 다룬다면 다음 지점을 함께 본다.

```gdb
b file_backed_swap_out
b file_write_at
b pml4_is_dirty
```

확인 질문:

- clean file-backed page도 파일에 다시 쓰는가?
- dirty file-backed page만 `file_write_at()`으로 이어지는가?
- anonymous dirty page는 `anon_swap_out()`으로 이어지는가?

### 5. 테스트 후보

eviction과 dirty 보존을 확인하는 테스트 후보는 다음이다.

```text
tests/vm/page-linear
tests/vm/swap-anon
tests/vm/swap-file
tests/vm/mmap-write
tests/vm/mmap-clean
```

`mmap-write`와 `mmap-clean`을 비교하면 dirty 판단 감각이 좋아진다. mapping을 통해 수정한 page만 파일에 반영되어야 하고, 읽기만 한 clean page는 불필요하게 write-back하지 않아야 한다.

## 정리

PTE accessed/dirty bit는 frame eviction에서 "최근 접근"과 "수정 여부"를 알려주는 하드웨어 단서다.

PintOS는 `pml4_is_accessed()`, `pml4_set_accessed()`, `pml4_is_dirty()` 같은 helper를 제공하지만, victim 선택과 write-back 정책은 VM 과제에서 직접 구현해야 한다.

QEMU는 이 bit의 하드웨어 효과를 에뮬레이션할 뿐이다. 어떤 page를 내보내고 어디에 저장할지는 guest OS인 PintOS의 책임이다.

## 다음 링크

- [[page-table-entry-bits-knowledge]]: PTE 한 칸의 주소 bit와 flag bit를 더 자세히 분해
- [[frame-eviction-trace]]: victim page를 내보내고 frame을 재사용하는 전체 흐름
- [[tlb-cr3-address-space-switch-knowledge]]: PTE를 고친 뒤 TLB를 왜 무효화해야 하는가
- [[mmap-file-backed-page-knowledge]]: dirty file-backed page가 파일 write-back으로 이어지는 사례
- [[swap-lab]]: anonymous page가 swap slot에 보존되는 사례
- [[week-3-4-virtual-memory-map]]: VM 주차 전체 링크 허브
