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
  - layer:memory
  - layer:kernel
  - layer:emulator
  - topic:frame
  - topic:swap
  - topic:page-table
  - topic:gdb
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-3-4-virtual-memory-map]]"
  - "[[supplemental-page-table-knowledge]]"
  - "[[page-fault-trace]]"
  - "[[address-translation-memory]]"
---

# 프레임 교체 (Frame Eviction) Trace

## 작은 질문

유저 프로그램이 새 페이지를 필요로 하는데 남은 물리 프레임이 하나도 없다면, 운영체제는 무엇을 해야 할까?

처음에는 “그냥 메모리 부족으로 실패하면 되지 않나?”라고 생각하기 쉽다. 하지만 가상 메모리(VM)를 가진 운영체제는 보통 바로 포기하지 않는다.

대신 이런 질문을 던진다.

> 지금 RAM에 올라온 페이지 중 하나를 잠시 내보내고, 그 자리에 새 페이지를 올릴 수 있나?

이 Trace의 핵심은 **프레임은 한정된 실제 자리이고, 페이지는 그 자리를 빌려 쓰는 가상 메모리 단위**라는 점이다.

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

가상 메모리는 프로세스에게 “메모리가 넉넉한 것처럼” 보이게 만든다. 하지만 실제 DRAM은 유한하다.

그래서 운영체제는 세 가지 일을 해야 한다.

1. 어떤 프레임이 비어 있는지 관리한다.
2. 비어 있는 프레임이 없으면 victim frame을 고른다.
3. victim의 내용이 필요하면 swap 영역이나 파일에 저장한 뒤, 새 페이지가 그 프레임을 쓰게 한다.

이 말은 즉, frame eviction은 [[page-fault-trace]]의 뒷부분이다. `vm_try_handle_fault()`가 “살릴 수 있는 fault”라고 판단해도, 실제 프레임을 구하지 못하면 프로그램은 계속 실행될 수 없다.

## 핵심 모델

머릿속에는 다음 대응 관계를 넣으면 된다.

| 개념 | 질문 |
|---|---|
| `struct page` | 이 유저 VA는 어떤 의미의 페이지인가? |
| `struct frame` | 지금 어느 실제 프레임이 이 페이지 내용을 담고 있는가? |
| page table PTE | CPU가 이 VA를 지금 어떤 프레임으로 번역할 수 있는가? |
| swap/file backing | 프레임에서 쫓겨난 내용을 어디에 보관할 것인가? |

흐름은 이렇게 이어진다.

```text
page fault
  -> SPT에서 struct page 찾기
  -> vm_do_claim_page(page)
  -> vm_get_frame()
     -> 빈 user frame이 있으면 사용
     -> 없으면 vm_evict_frame()
        -> vm_get_victim()
        -> swap_out(victim->page)
        -> old VA의 PTE를 not-present로 바꿈
        -> victim frame을 새 page에 재사용
  -> pml4_set_page(page->va, frame->kva)
  -> swap_in(page, frame->kva)
```

여기서 SPT는 [[supplemental-page-table-knowledge|supplemental page table]]이다. page table이 “현재 CPU가 번역할 수 있는가”를 답한다면, SPT는 “이 VA가 원래 합법적인 페이지인가, backing은 어디인가”를 기억한다.

## 예시 상황

페이지 크기가 4096B이고, 유저 풀에 쓸 수 있는 프레임이 3개뿐이라고 해보자.

```text
frame F0 -> page A (VA 0x8048000)
frame F1 -> page B (VA 0x8049000)
frame F2 -> page C (VA 0x804a000)

새로 필요한 page D (VA 0x804b000)
```

빈 프레임이 없으므로 운영체제는 A/B/C 중 하나를 victim으로 골라야 한다.

예를 들어 B를 고르면:

```text
1. B가 수정되었는지 확인
2. 필요하면 B 내용을 swap/file에 기록
3. B의 PTE present bit를 끔
4. F1.page를 D로 바꿈
5. D 내용을 F1.kva로 읽어 옴
6. D의 PTE를 F1에 매핑
```

겉으로는 “D 접근이 잠깐 느렸다” 정도로 보이지만, 내부에서는 4KB 단위 자리 바꾸기가 일어난다.

## Linux / Windows에서는

Linux와 Windows는 모두 훨씬 복잡한 물리 메모리 관리자와 페이지 교체 정책을 가진다.

- Linux는 LRU 계열 reclaim, anonymous/file-backed page, page cache, writeback, cgroup, NUMA 같은 요소를 함께 다룬다.
- Windows도 working set, standby/modified page list, section object, pagefile 같은 구조로 resident page와 backing store를 관리한다.

현실 OS의 핵심 질문도 PintOS와 같다.

> 이 페이지를 RAM에서 내보내도 되는가? 내보낸다면 내용을 어디에 보관해야 하는가?

다만 현실 OS는 SMP, 장치 I/O, 파일 캐시, 보안, 성능 정책까지 포함하므로 PintOS보다 훨씬 많은 상태를 본다.

## PintOS에서는

PintOS KAIST VM 스켈레톤에서 frame eviction의 뼈대는 다음 파일에 있다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/vm.c`
  - `vm_get_frame()`
  - `vm_evict_frame()`
  - `vm_get_victim()`
  - `vm_do_claim_page()`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/vm/vm.h`
  - `struct page`
  - `struct frame`
  - `struct page_operations`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/anon.c`
  - `anon_swap_in()`
  - `anon_swap_out()`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/file.c`
  - `file_backed_swap_in()`
  - `file_backed_swap_out()`

현재 코드 상태에서 중요한 점은, eviction이 아직 구현된 동작이 아니라 **TODO로 남은 설계 자리**라는 것이다.

`vm_get_frame()` 주석은 이미 방향을 말한다.

```text
palloc()으로 frame을 얻는다.
유저 풀 메모리가 꽉 차면 frame을 evict해서 사용 가능한 공간을 얻는다.
```

하지만 실제 함수 본문은 아직 채워져 있지 않다. 따라서 이 문서는 “현재 구현이 이렇게 동작한다”가 아니라, **스켈레톤이 요구하는 흐름을 어디에 채워야 하는지**를 읽는 Trace다.

## QEMU에서는

QEMU는 PintOS의 frame eviction 정책을 알지 못한다.

QEMU 관점에서 PintOS의 유저 프레임은 guest physical memory 안의 4KB 영역이고, 그 guest physical memory는 QEMU process의 `MemoryRegion`/`RAMBlock` 같은 구조로 backing된다.

대표 코드 위치:

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/include/system/memory.h`
  - `struct MemoryRegion`
  - `address_space_rw()`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/include/system/ramblock.h`
  - `struct RAMBlock`
  - `host`
  - `ramblock_ptr()`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/system/physmem.c`
  - `address_space_rw()`
  - `cpu_physical_memory_read()`
  - `cpu_physical_memory_write()`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/system/excp_helper.c`
  - x86 page walk 중 present/accessed/dirty bit를 확인하고 갱신하는 경로

중요한 차이는 이것이다.

- PintOS swap out: guest OS가 “내 페이지를 디스크/파일 backing으로 내보낸다”고 결정한다.
- Host OS swap out: macOS/Linux가 QEMU process의 host memory를 swap할 수 있다.

두 현상은 이름은 비슷하지만 층이 다르다. PintOS가 `anon_swap_out()`을 구현해도 QEMU가 그 의미를 해석하는 것이 아니다. QEMU는 guest가 디스크 장치와 메모리를 읽고 쓰는 하드웨어 효과를 제공할 뿐이다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| victim 선택 | 복잡한 reclaim/working set 정책 | `vm_get_victim()`에 과제 구현 | 정책 없음 |
| 프레임 관리 | 전역/NUMA/per-process 상태가 얽힘 | `struct frame`과 별도 frame table을 직접 설계 | guest RAM backing을 제공 |
| dirty 판단 | PTE dirty, writeback 상태, file cache 등 | `pml4_is_dirty()`와 page type별 `swap_out()` 활용 가능 | guest PTE bit 효과를 에뮬레이션 |
| 내보낼 위치 | swap/pagefile/file backing | anon은 swap, file-backed는 파일 writeback | OS 정책을 모름 |
| 실패 처리 | OOM killer, working set 조정, 프로세스 예외 등 | 구현 실패 시 테스트 실패 또는 panic 가능 | guest가 한 메모리 접근의 하드웨어 결과만 제공 |

## 코드 증거

### 1. frame은 page를 되돌아보는 포인터를 가진다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/vm/vm.h`

핵심 구조:

```c
struct page {
    const struct page_operations *operations;
    void *va;
    struct frame *frame;
    ...
};

struct frame {
    void *kva;
    struct page *page;
};
```

이 양방향 연결이 있어야 “이 프레임을 비우면 어느 유저 VA의 매핑을 깨야 하는가?”를 추적할 수 있다.

### 2. palloc은 유저 풀에서 4KB 프레임을 얻는다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/palloc.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/palloc.h`

`palloc_get_page(PAL_USER)`는 user pool에서 free page 하나를 얻는다. 실패하면 `NULL`을 반환할 수 있다.

이 실패가 바로 eviction을 시도해야 하는 지점이다.

### 3. page table mapping은 pml4_set_page()가 설치한다

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/mmu.c`

핵심 줄:

```c
*pte = vtop(kpage) | PTE_P | (rw ? PTE_W : 0) | PTE_U;
```

이 말은 “유저 VA page가 `kpage`가 가리키는 물리 프레임으로 번역된다”는 뜻이다.

### 4. eviction 후에는 old PTE를 not-present로 만들어야 한다

같은 파일의 `pml4_clear_page()`는 present bit를 끈다.

```c
*pte &= ~PTE_P;
```

victim page가 쫓겨났는데 PTE가 계속 present라면 CPU는 예전 프레임을 그대로 접근할 수 있다. 그러면 새 page와 old page가 같은 프레임을 동시에 소유한 것처럼 되어 메모리 내용이 깨진다.

### 5. accessed/dirty bit는 victim 선택과 writeback 판단의 근거가 된다

PintOS는 이미 helper를 제공한다.

- `pml4_is_accessed()`
- `pml4_set_accessed()`
- `pml4_is_dirty()`
- `pml4_set_dirty()`

`PTE_A = 0x20`, `PTE_D = 0x40`이다. 예를 들어 clock 알고리즘을 쓰면 `accessed` bit를 보고 “최근에 쓴 페이지는 한 번 봐주고”, `dirty` bit를 보고 “수정된 페이지는 swap/file에 기록해야 한다”고 판단할 수 있다.

QEMU의 x86 TCG 경로도 같은 하드웨어 의미를 흉내 낸다. `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/system/excp_helper.c`에는 page walk 중 `PG_ACCESSED_MASK`를 세우고, store 접근이면 `PG_DIRTY_MASK`까지 세우는 흐름이 있다.

## 숫자와 메모리

### 한 프레임은 4096B 자리다

```text
PGSIZE = 4096 = 0x1000

frame->kva = 0x800012340000
page->va   = 0x0000008048000
```

`pml4_set_page(pml4, page->va, frame->kva, writable)`가 성공하면:

```text
VA 0x8048123
  page base = 0x8048000
  offset    = 0x123

frame base = frame->kva
byte addr  = frame->kva + 0x123
```

즉 유저가 `0x8048123`을 읽는다는 말은, 현재 mapping 기준으로 `frame->kva + 0x123` 위치의 바이트를 읽는다는 뜻이다.

### PTE flag는 낮은 비트에 들어간다

예를 들어 어떤 PTE가 다음 값이라고 해보자.

```text
PTE = 0x0000000012345067
```

낮은 12비트는 flag이고, 위쪽 비트는 frame base 주소다.

```text
PTE_ADDR(PTE) = 0x0000000012345000
flags         = 0x067

0x001 PTE_P present
0x002 PTE_W writable
0x004 PTE_U user
0x020 PTE_A accessed
0x040 PTE_D dirty
```

이 PTE는 “유저가 읽고 쓸 수 있고, 최근 접근되었고, 수정된 적이 있는 페이지”라는 뜻이다. victim으로 고르면 dirty data를 먼저 보존해야 한다.

### eviction은 프레임 내용 교체다

```text
eviction 전:
  F1.kva bytes = page B 내용
  B.frame      = F1
  F1.page      = B
  B의 PTE      = present

eviction 후:
  B.frame      = NULL 또는 non-resident 표시
  B의 PTE      = not-present
  F1.page      = D
  D.frame      = F1
  F1.kva bytes = page D 내용
  D의 PTE      = present
```

이 변화가 제대로 되지 않으면 대표적으로 두 버그가 난다.

- stale mapping: 쫓겨난 page B가 여전히 F1을 가리킨다.
- lost dirty data: 수정된 B를 저장하지 않고 F1을 D로 덮어쓴다.

## 직접 확인

아직 eviction 구현 전이라면 breakpoint 목표는 “어디가 비어 있는가”를 보는 것이다.

```gdb
b vm_get_frame
b vm_evict_frame
b vm_get_victim
b vm_do_claim_page
b pml4_set_page
b pml4_clear_page
```

확인할 값:

```gdb
p/x page->va
p/x frame->kva
p/x frame->page
p/x page->frame
p page->operations->type
```

구현 후에는 VM 테스트가 프레임 부족 상황을 만든다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/page-linear.c`
  - 5MB 버퍼를 읽고 수정하며 여러 페이지를 반복 접근한다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/swap-anon.c`
  - 20MB anonymous 배열의 각 페이지 첫 바이트를 쓰고 다시 읽어 일관성을 확인한다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/tests/vm/swap-file.c`
  - mmap된 file-backed page가 swap out/in 뒤에도 데이터를 보존하는지 본다.

관찰 목표는 이 문장으로 정리된다.

> 프레임은 바뀌어도, 유저 프로그램이 보는 페이지 내용은 보존되어야 한다.

## 정리

frame eviction은 “메모리가 부족할 때 페이지를 포기한다”가 아니라 “한정된 프레임 자리를 다른 페이지가 안전하게 재사용하게 한다”는 작업이다.

PintOS에서는 `vm_get_frame()`이 빈 프레임을 얻지 못하는 순간 `vm_evict_frame()`으로 이어져야 한다. victim의 `struct page`, PTE present bit, dirty/accessed bit, swap/file backing을 함께 처리해야 내용 보존과 주소 변환이 깨지지 않는다.

QEMU는 이 정책을 모르고, guest physical memory와 x86 page-table 하드웨어 효과를 제공한다. 그래서 “PintOS의 swap out”과 “host OS가 QEMU process를 swap out”하는 일은 반드시 구분해야 한다.

## 다음 링크

- [[page-fault-trace]]: fault가 `vm_try_handle_fault()`까지 들어오는 흐름
- [[supplemental-page-table-knowledge]]: 쫓겨난 page가 여전히 합법인지 기억하는 장부
- [[address-translation-memory]]: VA, page base, offset, PTE를 숫자로 분해하는 법
- [[page-table-entry-bits-knowledge]]: present/accessed/dirty bit가 eviction 판단으로 이어지는 이유
- [[pte-accessed-dirty-eviction-lab]]: accessed/dirty bit를 직접 읽고 지워 victim 선택과 write-back 경계를 확인
- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]: frame/page/PTE를 바이트로 보는 연습
- [[mmap-file-backed-page-knowledge]]: file-backed page는 eviction 때 어디로 writeback되는가
- [[swap-lab]]: anonymous page를 swap slot과 연결해 보는 실험
