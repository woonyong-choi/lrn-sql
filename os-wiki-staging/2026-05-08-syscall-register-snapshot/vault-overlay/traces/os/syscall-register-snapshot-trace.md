---
type: Trace
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
  - layer:cpu
  - layer:user
  - layer:kernel
  - layer:emulator
  - topic:register
  - topic:syscall
  - topic:gdb
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-2-user-programs-map]]"
  - "[[cpu-register-execution]]"
  - "[[syscall-end-to-end]]"
---

# syscall 레지스터 스냅샷 흐름

## 작은 질문

`write(1, "hi", 2)`를 호출하면 `1`, `"hi"`, `2`라는 값은 커널의 어느 변수로 들어갈까?

처음에는 "함수 인자니까 C 함수처럼 stack에 있겠지"라고 생각하기 쉽다. 하지만 x86-64 시스템 콜 경계에서는 먼저 register가 중요하다. 유저 프로그램은 syscall 번호와 인자를 register에 넣고, 커널은 진입 순간의 register 값을 저장한 뒤 그 저장본을 읽는다.

## 왜 필요한가

커널은 유저 프로그램을 믿을 수 없다.

그래서 커널은 유저 코드가 계속 실행 중인 register를 직접 믿고 쓰는 대신, 커널 진입 순간의 상태를 한 번 고정한다. 이 고정된 상태가 PintOS에서는 `struct intr_frame`으로 보인다.

이 말은 즉, 시스템 콜 디버깅은 "C 함수 인자 찾기"가 아니라 "진입 시점 register snapshot 찾기"부터 시작해야 한다.

## 핵심 모델

시스템 콜 인자 전달은 다음 네 칸짜리 표로 시작하면 된다.

| 의미 | x86-64 register | PintOS에서 읽는 위치 |
|---|---|---|
| syscall 번호 | `RAX` | `f->R.rax` |
| 1번째 인자 | `RDI` | `f->R.rdi` |
| 2번째 인자 | `RSI` | `f->R.rsi` |
| 3번째 인자 | `RDX` | `f->R.rdx` |

PintOS의 syscall handler는 register를 직접 읽는 것처럼 보이지만, 정확히는 `syscall_entry.S`가 커널 스택에 만들어 둔 `struct intr_frame`의 필드를 읽는다.

## 예시 상황

유저 프로그램이 다음 요청을 한다고 하자.

```c
write(1, "hi", 2);
```

PintOS의 syscall 번호 enum에서 `SYS_WRITE`는 `10`이다. 따라서 handler에 들어가기 직전 핵심 값은 이렇게 보면 된다.

```text
RAX = 10          # SYS_WRITE
RDI = 1           # stdout fd
RSI = 0x0000000008048123  # "hi"가 있는 user virtual address 예시
RDX = 2           # length
```

커널이 성공적으로 2바이트를 쓰면 반환값도 `RAX`로 돌아간다.

```text
return RAX = 2
```

## Linux / Windows에서는

Linux x86-64도 syscall 번호와 인자를 register로 전달한다. 대표적으로 `write(fd, buf, count)`는 syscall ABI에 맞춰 `RAX`, `RDI`, `RSI`, `RDX` 같은 register에 값이 놓인 뒤 커널 entry code로 넘어간다.

Windows 애플리케이션은 보통 `WriteFile` 같은 Win32 API를 호출한다. 내부적으로는 handle 기반 API, Native API, kernel transition으로 내려간다. Linux의 fd와 Windows의 handle은 사용자에게 보이는 이름은 다르지만, 둘 다 "유저 코드가 커널 객체 조작을 요청한다"는 점은 같다.

실제 OS는 이 지점에서 보안, ptrace/debugger, signal/APC, per-CPU entry stack, speculation mitigation, 감사와 tracing까지 고려한다. PintOS는 그 복잡도를 과제 학습용 코드로 줄인 것이다.

## PintOS에서는

PintOS의 흐름은 다음 순서로 읽으면 된다.

```text
user code
  -> syscall instruction
  -> CPU가 MSR_LSTAR에 설정된 syscall_entry로 이동
  -> syscall_entry.S가 커널 스택으로 전환
  -> register 값을 struct intr_frame 모양으로 push
  -> syscall_handler(struct intr_frame *f) 호출
  -> f->R.rax/rdi/rsi/rdx를 읽어 C 함수 호출
  -> 반환값을 f->R.rax에 기록
  -> syscall_entry.S가 register를 복원
  -> sysretq로 유저 모드 복귀
```

중요한 단순화가 있다. PintOS는 교육용 OS라서 syscall table, audit hook, seccomp, VFS 계층, per-architecture entry macro 같은 실제 커널의 많은 층을 크게 줄여 보여준다. 대신 `syscall.c`와 `syscall-entry.S`가 눈에 잘 보인다.

## QEMU에서는

QEMU는 `SYS_WRITE`의 의미를 처리하지 않는다.

QEMU가 하는 일은 guest CPU가 `syscall` 명령을 실행했을 때 x86-64 CPU처럼 보이게 만드는 것이다. TCG 경로에서는 `SYSCALL` decode가 helper 호출로 내려가고, helper는 `RCX`, `R11`, `CS`, `SS`, `RIP` 같은 guest CPU 상태를 바꾼다.

QEMU의 guest register는 host CPU register 그 자체가 아니다. QEMU 내부의 `CPUX86State` 안에 guest register 상태가 있고, GDB stub은 그 값을 읽어 GDB에 보여준다.

## 차이점

| 구분 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| syscall 의미 해석 | 실제 커널이 API/ABI 계약으로 처리 | `syscall_handler()`의 switch 문이 처리 | 처리하지 않음 |
| 인자 위치 | architecture ABI에 따른 register/stack | `struct intr_frame`의 `R` 필드 | guest CPU state로 보관 |
| 반환값 | register로 유저 모드에 반환 | `f->R.rax`를 고쳐 반환 | register 변화만 재현 |
| 복잡도 | 보안, tracing, SMP, 호환성 포함 | 과제용 최소 흐름 | 하드웨어 동작의 에뮬레이션 |

## 코드 증거

PintOS에서 먼저 확인할 파일은 네 개다.

| 파일 | 확인할 것 |
|---|---|
| `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/start.S` | `EFER_SCE`를 켜서 `syscall` 명령 사용 가능하게 함 |
| `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c` | `MSR_LSTAR`에 `syscall_entry`를 기록하고 `f->R.*`를 읽음 |
| `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall-entry.S` | register를 push해서 `struct intr_frame` 모양을 만듦 |
| `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/interrupt.h` | `struct gp_registers`, `struct intr_frame` 필드 순서 |

핵심 조각은 이 정도만 보면 된다.

```c
write_msr(MSR_LSTAR, (uint64_t) syscall_entry);
```

```asm
push %rax
push %rbx
...
movq %rsp, %rdi
call syscall_handler
```

```c
case SYS_WRITE:
    f->R.rax = write(f->R.rdi, (const void *) f->R.rsi, f->R.rdx);
    break;
```

QEMU 쪽 근거는 다음 파일에서 잡는다.

| 파일 | 확인할 것 |
|---|---|
| `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/emit.c.inc` | `gen_SYSCALL()`이 helper를 호출함 |
| `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/tcg/system/seg_helper.c` | `helper_syscall()`이 guest `RCX`, `R11`, `RIP` 등을 바꿈 |
| `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/cpu.h` | `CPUX86State`에 `regs[]`, `eip`, `eflags`, `cr[]`가 있음 |
| `/Users/woonyong/workspace/Krafton-Jungle/QEMU/target/i386/gdbstub.c` | GDB register read가 `env->regs[...]`, `env->eip`를 읽음 |

## 숫자와 메모리

`struct gp_registers`는 15개의 64-bit general purpose register를 저장한다.

```text
15 registers * 8 bytes = 120 bytes
```

PintOS의 필드 순서에서 offset을 계산하면 이렇게 된다.

| 필드 | offset |
|---|---:|
| `r15` | `0` |
| `r14` | `8` |
| `r13` | `16` |
| `r12` | `24` |
| `r11` | `32` |
| `r10` | `40` |
| `r9` | `48` |
| `r8` | `56` |
| `rsi` | `64` |
| `rdi` | `72` |
| `rbp` | `80` |
| `rdx` | `88` |
| `rcx` | `96` |
| `rbx` | `104` |
| `rax` | `112` |

예를 들어 `f`가 `0xffff80000020f000`이라고 하면 `SYS_WRITE` 번호가 담긴 `f->R.rax` 주소는 다음이다.

```text
f->R.rax address = 0xffff80000020f000 + 112
                 = 0xffff80000020f070
```

`RAX = 10`이라면 little endian 8바이트는 이렇게 보인다.

```text
0a 00 00 00 00 00 00 00
```

이 말은 즉, GDB에서 `x/8xb &f->R.rax`를 봤을 때 첫 바이트가 `0x0a`로 보이면 syscall 번호 `10`을 보고 있는 것이다.

## 직접 확인

PintOS GDB에서 `write` syscall을 잡고 싶으면 다음 순서로 확인한다.

```gdb
b syscall_handler
c
p/x f->R.rax
p/x f->R.rdi
p/x f->R.rsi
p/x f->R.rdx
x/8xb &f->R.rax
x/2cb f->R.rsi
```

기대 관찰값은 다음이다.

```text
f->R.rax == 0xa          # SYS_WRITE
f->R.rdi == 0x1          # stdout
f->R.rdx == 0x2          # 2 bytes
f->R.rsi -> 68 69        # 'h', 'i'
```

QEMU GDB stub 관점에서 확인할 때는 `info registers`가 guest register를 보여준다는 점을 기억한다. 그 값은 host macOS/Linux process의 현재 hardware register가 아니라 QEMU가 관리하는 guest CPU state에서 나온다.

## 정리

- 시스템 콜 인자는 C 함수 stack 인자가 아니라 register ABI에서 시작한다.
- PintOS는 진입 순간 register를 `struct intr_frame` 모양으로 커널 스택에 저장한다.
- `syscall_handler()`는 `f->R.rax/rdi/rsi/rdx`를 읽어 syscall 번호와 인자를 해석한다.
- 반환값은 다시 `f->R.rax`에 쓰이고, 복귀 시 `RAX`로 유저 프로그램에 보인다.
- QEMU는 syscall 의미를 처리하지 않고, guest CPU의 `syscall` 명령과 register 변화를 하드웨어처럼 재현한다.

## 다음 링크

- [[syscall-end-to-end]]: syscall 요청이 fd, buffer, size를 실제 커널 함수 호출로 바꾸는 전체 흐름
- [[cpu-register-execution]]: `RIP`, `RSP`, `RAX` 같은 register가 CPU 실행 모델에서 갖는 의미
- [[user-pointer-validation-trace]]: `RSI`에 들어온 user virtual address를 커널이 검증하는 흐름
- [[file-descriptor-knowledge]]: `RDI=1` 같은 fd 숫자가 열린 파일 객체로 바뀌는 방식
