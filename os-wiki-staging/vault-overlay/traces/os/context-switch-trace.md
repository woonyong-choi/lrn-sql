---
type: Trace
status: Draft
week:
  - threads
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - week:threads
  - layer:cpu
  - layer:kernel
  - topic:scheduler
  - topic:interrupt
  - topic:register
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-1-threads-map]]"
  - "[[interrupt-timer-qemu]]"
  - "[[cpu-register-execution]]"
---

# 컨텍스트 스위치 (Context Switch) Trace

## 작은 질문

스레드 A가 CPU를 쓰는 중인데, 스레드 B로 “넘어간다”는 말은 정확히 무슨 뜻일까?

초보자에게 가장 큰 함정은 “스레드가 바뀐다 = 함수 호출처럼 점프한다”로 오해하는 것이다. 실제로는 **CPU 레지스터 + 스택 + (유저프로세스라면) 페이지 테이블**까지 포함한 “실행 문맥”을 저장/복원해야 진짜로 스레드가 바뀐다.

이 문서의 목표는 아래 흐름을 *눈으로 추적*하는 것이다.

```text
timer interrupt
  -> thread_tick()
  -> intr_yield_on_return()
  -> intr_handler()가 interrupt 마무리하며 thread_yield()
  -> schedule()
  -> thread_launch(next)
  -> do_iret(&next->tf)
  -> (iretq) 다음 스레드의 RIP/RSP로 복귀
```

## 핵심 모델 (머릿속에 넣을 최소 모델)

컨텍스트 스위치는 “CPU가 가진 실행 상태”를 바꿔치기하는 일이다.

- 저장(현재 스레드): “지금 어디까지 실행했는지”를 잃지 않게 레지스터/스택 포인터 등을 메모리에 적어둔다.
- 선택(스케줄러): 다음에 실행할 스레드를 고른다.
- 복원(다음 스레드): 다음 스레드가 마지막으로 멈춘 지점의 레지스터/스택을 복원하고 그 지점으로 돌아간다.

중요: **스레드는 CPU 위에 “동시에” 존재하지 않는다.** CPU 위에는 언제나 하나의 레지스터 집합과 하나의 현재 스택만 있다. 그래서 “스레드가 여러 개”라는 말은 결국 “스레드별로 저장된 문맥이 여러 벌 있다”로 해석해야 한다.

## Linux / Windows에서는 (현실 기준으로 잡기)

현실 OS의 컨텍스트 스위치는 PintOS보다 훨씬 복잡하다.

- SMP(다중 CPU): CPU마다 run queue가 따로 있고, migration(스레드가 CPU를 옮김)도 발생한다.
- interrupt 우선순위, deferred work, preemption 모델(완전 선점/부분 선점/tickless) 등이 얽힌다.
- 주소 공간 전환(CR3 교체)은 보안/성능에 매우 중요해서, TLB flush 최적화 같은 디테일까지 간다.

하지만 본질은 같다.

- Linux: 아키텍처별로 “현재 task의 레지스터/스택 저장 → 다음 task 복원” 경로가 있고, 스케줄러는 “다음 실행 흐름”을 결정한다.
- Windows: 스레드/프로세스 오브젝트 모델 위에서 dispatcher가 다음 실행을 결정하고, 커널이 문맥을 교체한다.

PintOS는 이 현실을 “학습 가능한 크기”로 강하게 축소한 것이다.

## PintOS에서는 (코드로 내려가기)

### 1) 스레드의 문맥은 어디에 저장되는가?

PintOS에서 핵심 단서는 `struct thread` 안에 `struct intr_frame tf`가 있다는 점이다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/threads/thread.h`
  - “스레드 구조체는 전용 4KB 페이지에 있고, 나머지는 커널 스택”이라는 그림 설명이 있다.
  - `struct thread` 필드에 `struct intr_frame tf;`가 들어 있다.

이게 의미하는 바:

- “스레드별 문맥”은 결국 스레드가 소유한 메모리(여기서는 `struct thread` 안)로 저장된다.
- **커널 스택**도 스레드별로 존재한다(같은 4KB 페이지 상단).

### 2) timer interrupt가 왜 스케줄링으로 이어지는가?

timer interrupt는 “언젠가”가 아니라 **주기적으로** 들어오는 이벤트라서, OS가 CPU를 되찾아 다음 스레드로 바꿀 기회를 만든다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`
  - `thread_tick()`이 tick마다 호출되고, `TIME_SLICE`를 다 쓰면 `intr_yield_on_return()`을 호출한다.
- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/interrupt.c`
  - `intr_yield_on_return()`은 “인터럽트 끝날 때 양보(yield)하겠다”는 플래그를 세운다.
  - `intr_handler()`는 외부 인터럽트를 마무리하는 지점에서 그 플래그를 보고 `thread_yield()`를 실행한다.

여기서 중요한 관찰:

- `thread_tick()`은 “지금 당장 스케줄러를 호출”하지 않는다.
- 대신 “인터럽트 핸들러가 끝날 때” 안전한 지점에서 `thread_yield()`가 실행되도록 예약한다.

### 3) schedule()은 무엇을 바꾸는가?

스케줄러는 “누가 다음 CPU 주인인가?”를 정한다. PintOS에서는 `ready_list`에서 꺼내는 단순 모델로 시작한다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`
  - `next_thread_to_run()`이 `ready_list`에서 다음 스레드를 고른다.
  - `schedule()`이 `next->status = THREAD_RUNNING`, `thread_ticks = 0` 등을 처리한다.
  - (USERPROG일 때) `process_activate(next)`로 주소 공간을 전환한다.
  - 마지막에 `thread_launch(next)`로 “진짜 문맥 교체”로 내려간다.

### 4) thread_launch()와 do_iret()는 실제로 무슨 일을 하나?

PintOS KAIST 코드에서 컨텍스트 스위치의 “가장 날것”은 `thread_launch()`에 들어 있다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`
  - `tf_cur = &running_thread()->tf` : 현재 스레드의 문맥 저장 위치
  - `tf = &next->tf` : 다음 스레드의 문맥 복원 소스
  - 인라인 어셈블리로 “현재 레지스터들을 `tf_cur`에 써넣고”, 마지막에 `do_iret(tf)`를 호출한다.
  - `do_iret()`는 `tf`에 들어 있는 레지스터 값들을 복원하고 `iretq`로 복귀한다.

즉 PintOS에서 컨텍스트 스위치의 본질은:

1) 현재 CPU 레지스터들을 “현재 스레드의 `intr_frame`”에 저장  
2) 다음 스레드의 `intr_frame`을 CPU 레지스터에 복원  
3) `iretq`로 다음 스레드의 `RIP/RSP`로 점프  

## QEMU에서는 (역할 분리)

이 Trace에서 QEMU는 “스케줄러”가 아니다.

- PintOS의 `schedule()`/`thread_launch()`/`do_iret()`는 **guest OS 내부 코드**다.
- QEMU는 그 코드가 실행되는 동안, x86-64 CPU의 `iretq` 같은 명령이 “실제 CPU에서 실행된 것 같은 효과”를 내도록 에뮬레이션한다.

즉 “스레드가 바뀌는 정책/자료구조”는 PintOS가 만들고, “명령어가 CPU 상태를 바꾸는 물리적 효과”는 QEMU가 흉내 낸다.

## 숫자 예제 (4KB 페이지와 스택 포인터)

PintOS는 `struct thread`를 4KB 페이지에 올려두고, 같은 페이지 상단을 커널 스택으로 쓴다.

예를 들어 `t`의 주소가 `0x80000000`(페이지 시작)이라고 하자.

```text
PGSIZE = 0x1000
thread struct base = 0x80000000
top of page        = 0x80001000
```

`init_thread()`에서 다음처럼 초기 스택 포인터를 잡는다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`
  - `t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);`

`sizeof(void*) = 8`(x86-64)라면:

```text
t->tf.rsp = 0x80000000 + 0x1000 - 8
          = 0x80000ff8
```

이 값은 “커널 스택의 맨 위에서 8바이트 아래”다. 즉 이 스레드는 처음 실행될 때부터 “자기 전용 커널 스택”을 갖고 시작한다.

## 디버깅 체크리스트 (GDB로 눈으로 확인)

PintOS(QEMU+GDB)에서 아래를 확인하면 “스레드 전환이 진짜 레지스터/스택 교체”라는 걸 눈으로 볼 수 있다.

1) 타이머 틱에서 양보가 예약되는지
   - breakpoint: `thread_tick`, `intr_yield_on_return`
   - `intr_context()`가 true일 때만 `intr_yield_on_return()`이 호출되는지 확인

2) 인터럽트 핸들러 끝에서 `thread_yield()`가 실제 호출되는지
   - breakpoint: `intr_handler`
   - `yield_on_return`가 true가 되는지 관찰

3) 스케줄러가 next를 고르고 `thread_launch(next)`로 내려가는지
   - breakpoint: `schedule`, `thread_launch`
   - `p thread_current()->name`
   - `p next->name`

4) 컨텍스트 스위치 직전/직후의 스택 포인터가 바뀌는지
   - `thread_launch`에서 `info registers rsp rip`
   - `p/x running_thread()->tf.rsp`
   - `p/x next->tf.rsp`

## 다음으로 볼 문서

- [[interrupt-timer-qemu]]: “왜 timer interrupt가 스케줄링 기회인가”
- [[cpu-register-execution]]: “레지스터가 실행 문맥의 핵심인 이유”
- [[thread-scheduler-trace]]: ready_list/priority 정책 중심 Trace
