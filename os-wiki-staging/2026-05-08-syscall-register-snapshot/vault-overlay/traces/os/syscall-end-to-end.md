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
  - layer:user
  - layer:kernel
  - layer:cpu
  - topic:syscall
  - topic:fd
related_to:
  - "[[week-2-user-programs-map]]"
---

# 시스템 콜 End-to-End

## 작은 질문

`printf("hello")`를 호출했을 뿐인데, 왜 화면에 글자가 출력될까?

초보자 입장에서는 `printf`가 화면을 직접 만지는 것처럼 느껴진다. 하지만 유저 프로그램은 터미널 장치나 커널 메모리를 직접 제어할 권한이 없다. 그래서 운영체제는 "요청만 할 수 있는 문"을 제공한다. 그 문이 시스템 콜이다.

## 핵심 모델

시스템 콜은 유저 프로그램이 커널 기능을 요청하는 제한된 진입점이다.

예:

```c
write(1, "hello", 5);
```

유저 프로그램은 화면 장치를 직접 만지지 않는다. 인자를 register에 넣고 kernel entry instruction을 실행한다.

## Linux / Windows에서는

Linux에서는 `write(1, "hello", 5)`가 fd `1`인 stdout에 5바이트를 쓰라는 요청이다. 유저 코드는 syscall ABI에 맞게 syscall 번호와 인자를 레지스터에 넣고 커널로 진입한다.

Windows에서는 일반 애플리케이션이 보통 `WriteFile` 같은 Win32 API를 호출한다. 내부적으로는 Native API와 kernel transition으로 내려간다. Linux의 fd 모델과 달리 Windows는 handle 모델로 이해하는 편이 좋다.

핵심 차이:

- Linux: fd 중심
- Windows: handle 중심
- 둘 다 유저 모드가 장치를 직접 만지지 못하게 막는다

## PintOS에서는

PintOS에서는 syscall이 과제의 핵심이다.

대표 파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall-entry.S`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/lib/syscall-nr.h`
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/interrupt.h`

흐름:

```text
user code
  -> syscall wrapper
  -> register에 syscall number와 args 배치
  -> syscall entry
  -> struct intr_frame 구성
  -> syscall_handler
  -> f->R.rax로 syscall number 판별
  -> f->R.rdi/rsi/rdx로 인자 사용
  -> return value를 f->R.rax에 기록
  -> user mode로 복귀
```

이 흐름에서 register가 실제로 어디에 저장되는지 헷갈리면 [[syscall-register-snapshot-trace]]를 먼저 보면 된다. 그 문서는 `RAX/RDI/RSI/RDX`가 `struct intr_frame`의 `f->R.*` 필드로 고정되는 순간을 숫자 offset과 GDB 확인 방법까지 내려가서 다룬다.

## QEMU에서는

QEMU는 guest instruction 실행을 에뮬레이션한다.

PintOS가 `syscall` 또는 interrupt/trap 계열 진입을 수행하면, QEMU는 guest CPU semantics에 맞게 privilege transition과 register/state 변화를 재현한다. guest 입장에서는 CPU가 직접 수행한 것처럼 보인다.

중요한 점은 QEMU가 `SYS_WRITE`를 보고 파일에 쓰는 것이 아니라는 것이다. `SYS_WRITE`의 의미는 PintOS 커널 코드가 해석한다. QEMU는 그 코드가 실행될 CPU와 메모리 환경을 만들어준다.

## 차이점

| 항목 | Linux / Windows | PintOS | QEMU |
|---|---|---|---|
| syscall set | 매우 많고 안정 ABI 필요 | 과제에서 필요한 일부 syscall | syscall을 처리하는 OS가 아니라 CPU 동작을 에뮬레이션 |
| pointer validation | 보안 핵심 | 과제에서 직접 구현 | guest memory access가 가능하도록 하드웨어 관점 제공 |
| fd/handle | Linux fd, Windows handle | 단순 fd table | fd 개념 없음. 장치와 메모리를 에뮬레이션 |

## 코드 증거

PintOS에서 syscall 번호는 `struct intr_frame` 안의 `RAX`에서 읽는다.

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/userprog/syscall.c`

핵심 코드:

```c
switch (f->R.rax) {
case SYS_WRITE:
    f->R.rax = write (f->R.rdi, (const void *) f->R.rsi, f->R.rdx);
    break;
}
```

이 코드가 증명하는 것:

- `f->R.rax`: syscall 번호이자 반환값 위치
- `f->R.rdi`: 첫 번째 인자, 여기서는 fd
- `f->R.rsi`: 두 번째 인자, 여기서는 user buffer 주소
- `f->R.rdx`: 세 번째 인자, 여기서는 size

`struct intr_frame`은 interrupt/syscall 진입 시 저장된 guest register snapshot이다.

파일:

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/interrupt.h`

## 숫자 예제

```text
write(1, 0x8048123, 5)

RAX = SYS_WRITE
RDI = 1
RSI = 0x8048123
RDX = 5

if success:
  RAX = 5
```

주소 `0x8048123` 검증:

```text
page size = 0x1000
page base = 0x8048000
offset    = 0x123
```

## 메모리 보기

`RSI = 0x8048123`은 문자열 자체가 아니다. 문자열이 있는 guest virtual address다.

그 주소의 바이트를 읽으면:

```text
68 65 6c 6c 6f
```

ASCII로 해석할 때만 `"hello"`가 된다.

## 디버깅 체크리스트

- syscall handler breakpoint
- `p/x f->R.rax`
- `p/x f->R.rdi`
- `x/5cb f->R.rsi`
- invalid pointer test에서 page fault 흐름 확인

## 다음으로 볼 문서

- [[week-2-user-programs-map]]
- [[syscall-register-snapshot-trace]]: syscall 번호와 인자가 register snapshot으로 저장되는 방식
- [[file-descriptor-knowledge]]: `fd` 숫자가 커널의 열린 파일 객체로 바뀌는 방식
- [[process-wait-exit-trace]]: `SYS_WAIT`과 `SYS_EXIT`가 프로세스 생명주기로 이어지는 방식
- [[cpu-register-execution]]
- [[address-translation-memory]]
