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
  - layer:cpu
  - layer:memory
  - layer:kernel
  - topic:page-table
  - topic:frame
  - topic:gdb
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-3-4-virtual-memory-map]]"
  - "[[address-translation-memory]]"
  - "[[cpu-register-execution]]"
---
# 페이지 폴트 (Page Fault) Trace

## 작은 질문

왜 어떤 주소 접근은 “그냥 죽는(segfault)” 반면, 어떤 주소 접근은 운영체제가 페이지를 준비해준 뒤 아무 일 없던 것처럼 계속 실행될까?

이 질문의 핵심은 “page fault = 무조건 에러”가 아니라는 점이다. **page fault는 CPU가 운영체제에게 던지는 질문**에 가깝다.

> “방금 이 가상 주소를 읽거나(또는 쓰거나) 실행하려고 했는데, 지금은 변환이 안 된다. 이걸 네가 처리할 건가, 아니면 죽일 건가?”

## 왜 필요한가

운영체제가 하려는 일은 크게 두 가지다.

1) 보호(protection): 다른 프로세스 메모리를 못 건드리게 막기
2) 지연 로딩(demand paging): “필요할 때만” 페이지를 메모리에 올려서 RAM을 아끼기

둘 다 CPU 혼자서는 결정을 못 한다. 그래서 **CPU는 “문제 발생”을 예외(exception)로 올리고, 운영체제가 정책을 결정**한다.

## 핵심 모델 (머릿속에 넣을 최소 모델)

page fault(#PF)는 x86에서 “메모리 접근을 주소 변환으로 이어갈 수 없다”거나 “접근 권한이 맞지 않는다”를 의미하는 CPU 예외다.

CPU는 page fault가 나면 운영체제에게 두 가지 힌트를 남긴다.

- **CR2**: fault가 난 가상 주소(“어떤 VA 때문에?”)
- **error code**: 어떤 종류의 fault인지(“not present인가? write인가? user인가?” 등)

운영체제는 이 정보를 보고:

- (합법적인 경우) 페이지를 준비하고 매핑을 설치한 뒤 **그 명령어를 다시 실행**하게 한다
- (불법/버그인 경우) 프로세스를 죽인다

## 예시 상황 2개 (같은 #PF, 다른 결말)

### A) 합법: 스택이 아래로 자라야 하는 순간

유저 프로그램이 함수 호출을 깊게 해서 스택을 더 쓰려고 할 때, 아직 매핑되지 않은 스택 페이지를 처음 건드릴 수 있다.

- page fault 발생
- OS가 “이건 스택 성장으로 봐줄 만하다”라고 판단
- 새 프레임을 할당하고 매핑 설치
- 프로그램은 계속 실행

### B) 불법: 터무니없는 포인터로 접근

예를 들어 `char *p = (char *)0xdeadbeef; *p = 1;` 같은 접근은 보통 “유저가 만지면 안 되는 주소”다.

- page fault 발생
- OS가 “정상적인 매핑/성장 규칙에 해당 없음”이라고 판단
- 프로세스 종료

## Linux / Windows에서는 (현실 기준)

현실 OS는 page fault를 다음처럼 다룬다.

- **합법일 수 있는 fault**: demand paging, [[mmap-file-backed-page-knowledge|file mapping(mmap)]], copy-on-write 같은 정책으로 처리 후 재시작
- **불법 fault**: Linux는 보통 `SIGSEGV`(segmentation fault)로, Windows는 Access Violation 계열로 종료/예외 처리

즉 현실 OS는 “어떤 fault를 살릴지” 정책이 많고, 그 정책이 파일 시스템/캐시/보안/성능과 깊게 엮인다.

## PintOS에서는 (Trace의 본체)

PintOS에서 page fault의 관문은 여기다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/exception.c`
  - `intr_register_int(14, ..., page_fault, "#PF ...")`
  - `page_fault(struct intr_frame *f)`

흐름을 사건 순서로 쓰면 다음과 같다.

```text
유저 코드(또는 커널 코드)가 어떤 VA에 접근
  -> x86 MMU가 주소 변환/권한 체크 시도
  -> 실패하면 CPU가 #PF 예외를 발생
  -> PintOS의 page_fault() 핸들러로 진입
     - CR2에서 faulting VA를 읽음
     - error_code를 PF_P/PF_W/PF_U로 해석
     - (VM 켜짐) vm_try_handle_fault()에게 처리 기회를 줌
     - 처리 불가면 kill()로 종료
  -> 처리 성공이면 핸들러에서 복귀
  -> CPU는 fault를 일으킨 명령어를 다시 실행(= “아무 일 없던 것처럼” 보임)
```

핵심은 “PintOS가 page fault를 처리할지 말지를 `vm_try_handle_fault()`에서 결정”하게 되어 있다는 점이다.

### PintOS 코드 증거 1: CR2와 error_code 해석

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/exception.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/userprog/exception.h` (PF_P/PF_W/PF_U)

핵심 부분(요지만):

```c
fault_addr = (void *) rcr2();
intr_enable();

not_present = (f->error_code & PF_P) == 0;
write       = (f->error_code & PF_W) != 0;
user        = (f->error_code & PF_U) != 0;
```

이 코드가 의미하는 것:

- **CR2는 “fault가 난 VA”**다. (`rcr2()`로 읽음)
- **error_code는 “fault의 종류”**를 비트로 담고 있다.

### PintOS 코드 증거 2: “처리할 기회”를 주는 vm_try_handle_fault()

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/exception.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/vm/vm.c`

VM이 켜졌다면(프로젝트 3 이후), 다음 코드가 열린 문이 된다.

```c
if (vm_try_handle_fault(f, fault_addr, user, write, not_present))
    return;
```

`vm_try_handle_fault()`는 현재 코드 상태에서 TODO 스켈레톤이지만, “fault를 살릴지/죽일지”의 정책을 채우는 자리라는 점이 중요하다.

### PintOS 코드 증거 3: 매핑 설치(pml4_set_page)

page fault를 살리려면 결국 “VA page → frame” 매핑을 page table에 설치해야 한다.

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/mmu.c`

요지만 보면:

```c
*pte = vtop(kpage) | PTE_P | (rw ? PTE_W : 0) | PTE_U;
```

이 줄은 “유저 가상 페이지(upage)가 가리키는 페이지 테이블 엔트리에, 프레임 물리 주소 + 권한 비트를 기록한다”는 뜻이다.

## QEMU에서는 (PintOS가 믿는 ‘CPU의 #PF’가 만들어지는 곳)

PintOS의 `page_fault()`는 guest OS 코드고, QEMU는 guest의 syscall/VM 정책을 처리하지 않는다.

QEMU가 하는 일은 “guest CPU가 메모리를 접근할 때 x86 MMU가 하는 일(주소 변환/권한 체크/예외 발생)”을 흉내 내는 것이다.

대표 단서는 여기서 볼 수 있다.

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/emulate/x86_mmu.c`
  - `mmu_gva_to_gpa()`로 변환 시도
  - 실패 시 #PF를 raise하고, faulting VA를 CR2(또는 예외 payload)로 남김

예: read 경로(요지만)에서는 CR2에 해당하는 `env->cr[2]`에 VA를 넣고 #PF를 raise한다.

```c
env->cr[2] = gva;
x86_emul_raise_exception(env, EXCP0E_PAGE, error_code);
```

예: write 경로에서는 “pending exception에서 CR2를 아직 세팅하지 않기” 위한 payload로 VA를 저장할 수 있다.

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/machine.c`
  - pending → injected로 바뀌는 순간 `env->cr[2] = env->exception_payload;`로 옮기는 로직이 있다.

즉 PintOS가 `rcr2()`로 읽는 “CR2”의 값도, 결국 QEMU 내부에서는 guest CPU state(`env->cr[2]`)로 표현되고 #PF 전달 과정에서 관리된다.

## 차이점 (현실 OS vs PintOS vs QEMU)

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| page fault의 의미 | “OS가 정책으로 결정” (살릴 수도 죽일 수도) | 과제 구현에 따라 달라짐(VM 전에는 대부분 kill) | x86 하드웨어의 #PF 동작을 재현 |
| fault 정보 | CR2 + error code + 추가 비트들 | CR2 + PF_P/PF_W/PF_U 중심으로 단순화 | guest CPU state 안에 CR2/error code를 유지 |
| 처리 주체 | 커널 VM 서브시스템 | `vm_try_handle_fault()`가 정책 자리 | 정책 없음. 하드웨어 이벤트만 흉내 |

## 숫자와 메모리: “fault_addr는 무엇이고, 왜 page base로 내리나?”

예를 들어 fault 주소가 `0x8048123`이라면:

```text
page size  = 0x1000 (4096)
fault VA   = 0x8048123
page base  = 0x8048000
offset     = 0x123
```

page fault 처리 코드는 보통 `page base` 단위로 “어떤 페이지가 비어 있다/권한이 없다”를 판단한다. 같은 페이지 안의 어떤 바이트(`offset`)를 건드렸는지는 보통 나중 문제다.

## 직접 확인 (GDB 체크리스트)

아래는 “page fault가 나면 무엇을 보면 되는지” 최소 체크리스트다.

1) fault 주소 확인
   - breakpoint: `page_fault`
   - `p/x fault_addr`

2) fault 종류 확인
   - `p/x f->error_code`
   - `not_present/write/user`가 어떻게 해석되는지 관찰

3) “살렸는지/죽였는지” 확인
   - breakpoint: `vm_try_handle_fault` (VM 켰을 때)
   - `kill`로 떨어지는지, `return`으로 빠지는지

4) (살렸다면) 매핑이 설치됐는지 확인
   - `pml4_get_page()`로 해당 VA가 이제 유효해졌는지 확인
   - 필요하면 `x/16xb fault_addr`로 바이트도 본다

## 정리

- page fault는 “OS가 결정해야 하는 이벤트”다.
- CPU는 CR2 + error_code로 힌트를 주고, OS는 “살릴지/죽일지”를 정한다.
- PintOS에서는 `page_fault()` → `vm_try_handle_fault()`가 그 결정의 관문이다.
- QEMU는 “그 #PF가 guest에게 전달되는 하드웨어적 효과”를 재현한다.

## 다음으로 볼 문서

- [[address-translation-memory]]: 주소 변환의 큰 그림(숫자 예제 포함)
- [[supplemental-page-table-knowledge]]: page table만으로는 부족한 fault 처리용 의미 장부
- [[frame-eviction-trace]]: fault를 살리려는데 빈 frame이 없을 때 이어지는 흐름
- [[mmap-file-backed-page-knowledge]]: file-backed fault를 파일 offset과 frame으로 연결하는 흐름
- [[lazy-file-backed-page-trace]]: file-backed lazy page가 첫 접근 때 `VM_UNINIT`에서 `VM_FILE`로 바뀌는 흐름
- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]: 같은 바이트를 타입으로 해석하는 감각
