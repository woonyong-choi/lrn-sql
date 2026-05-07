---
type: Knowledge
status: Draft
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - layer:cpu
  - system:pintos
  - system:qemu
  - topic:register
related_to:
  - "[[학습-가이드]]"
  - "[[syscall-register-snapshot-trace]]"
---

# CPU 레지스터와 명령어 실행

## Core Model

CPU 실행은 `RIP`가 가리키는 명령어를 읽고, 해석하고, 레지스터나 메모리를 바꾸고, 다음 `RIP`로 이동하는 반복이다.

처음에는 register를 "CPU 안의 변수"처럼 생각해도 된다. 다만 운영체제를 읽을 때는 한 가지를 더 붙여야 한다. interrupt, exception, syscall 경계에서는 그 순간의 register 값이 커널 stack의 저장본으로 바뀐다.

이 저장본을 보지 못하면 `write(1, "hi", 2)`의 `1`, `"hi"`, `2`가 커널의 어디로 들어갔는지 찾기 어렵다. 이 흐름은 [[syscall-register-snapshot-trace]]에서 별도로 따라간다.

운영체제 관점에서 중요한 레지스터는 보통 다음이다.

| 레지스터 | 의미 |
|---|---|
| `RIP` | 다음에 실행할 instruction address |
| `RSP` | 현재 stack top |
| `RBP` | stack frame 기준점 |
| `RAX` | 반환값, syscall number |
| `RDI/RSI/RDX` | 함수 또는 syscall 인자 |
| `CR3` | 현재 page table root |
| `RFLAGS` | interrupt enable 같은 CPU flag |

## Linux / Windows

Linux와 Windows 모두 CPU architecture의 ABI와 privilege level 위에서 실행된다.

차이는 OS API와 내부 구조에 있지만, CPU 입장에서는 user mode instruction, kernel mode transition, register save/restore, page table switching 같은 공통 문제가 있다.

## PintOS

PintOS에서 레지스터는 다음 상황에서 눈에 띈다.

- interrupt 또는 syscall 진입 시 `struct intr_frame`
- context switch 시 saved register와 stack pointer
- user program 시작 시 `RIP`, `RSP`
- syscall 인자 전달 시 `RAX`, `RDI`, `RSI`, `RDX`
- syscall 진입 후 `syscall_entry.S`가 만든 `struct intr_frame`

대표 파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/interrupt.h`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/intr-stubs.S`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall-entry.S`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/interrupt.h`

숫자로는 `struct gp_registers`가 15개 register를 8바이트씩 저장하므로 120바이트다. `rax`는 이 블록의 마지막 필드라 시작 주소 기준 offset `112`에 놓인다.

## QEMU

QEMU에서 guest register는 host CPU register와 같은 것이 아니다. QEMU는 guest CPU state를 내부 자료구조로 들고 있고, TCG 또는 가속기를 통해 guest instruction의 효과를 host에서 재현한다.

확인할 QEMU 영역:

- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/accel/tcg/`
- `/Users/woonyong/workspace/Krafton-Jungle/QEMU/gdbstub/`

## Differences

| 항목 | 실제 CPU | PintOS | QEMU |
|---|---|---|---|
| register | 하드웨어 플립플롭/마이크로아키텍처 상태 | interrupt frame과 thread context로 관찰 | guest CPU state 구조로 보관 |
| instruction | CPU가 직접 fetch/decode/execute | 실행된 결과를 OS가 관찰 | guest instruction을 번역/에뮬레이션 |
| page table | MMU가 사용 | PintOS가 설정 | QEMU가 guest-visible MMU 동작을 재현 |

## Code Evidence

볼 코드:

- PintOS: `struct intr_frame`
- PintOS: syscall handler가 `f->R.rax`를 읽는 부분
- PintOS: `syscall_entry.S`가 register를 push한 뒤 `movq %rsp, %rdi`로 handler 인자를 만드는 부분
- QEMU: x86 CPU state 정의와 GDB register export 경로

## Numeric Example

```text
RIP = 0x401000
instruction bytes = 48 89 e5
meaning = mov rbp, rsp

before:
  RSP = 0x7fffffffe000
  RBP = 0x0

after:
  RBP = 0x7fffffffe000
  RIP = 0x401003
```

## Memory View

명령어도 메모리 바이트다.

```text
48 89 e5
```

이 바이트를 instruction decoder로 보면 `mov rbp, rsp`가 되고, 그냥 정수로 보면 다른 값일 뿐이다.

## Debug Checklist

- PintOS GDB에서 `info registers`
- `x/16xb $rip`로 instruction bytes 확인
- syscall breakpoint에서 `p/x f->R.rax`
- QEMU GDB stub로 guest register 확인

## Links

- [[syscall-end-to-end]]
- [[syscall-register-snapshot-trace]]
- [[interrupt-timer-qemu]]
- [[tlb-cr3-address-space-switch-knowledge]]
- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]
