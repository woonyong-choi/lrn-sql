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
  - layer:kernel
  - layer:cpu
  - topic:scheduler
  - topic:interrupt
  - topic:gdb
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-1-threads-map]]"
  - "[[interrupt-timer-qemu]]"
  - "[[context-switch-trace]]"
---

# 스레드 스케줄러 (Thread Scheduler) Trace

## 작은 질문

`thread_create()`로 스레드를 만들면 “바로 실행”되는 것처럼 보이지만, 실제로는 언제 CPU를 받는 걸까?

여기서 초보자가 헷갈리는 포인트는 두 가지다.

- 스레드가 “존재한다”는 것과 “지금 CPU에서 실행 중이다”는 것은 다르다.
- 스케줄러는 “스레드를 실행시킨다”가 아니라 **다음 CPU 주인을 고른다**에 가깝다.

이 Trace는 PintOS의 `ready_list`를 중심으로 다음 흐름을 추적한다.

```text
thread_unblock()  -> ready_list에 들어감(READY)
timer tick        -> TIME_SLICE 만료면 양보 예약
thread_yield()    -> 현재 스레드도 ready_list로(READY)
next_thread_to_run() -> ready_list에서 하나를 뽑음
schedule()        -> 문맥 교체(context switch)로 내려감
```

## 왜 필요한가

스케줄링은 OS의 “시간 관리”다.

- CPU는 한 번에 하나의 코드만 실행한다.
- 스레드는 여러 개일 수 있다.
- 그래서 OS는 “누가 다음 1ms(혹은 1 tick)를 쓰는가?”를 계속 결정해야 한다.

이 결정을 이해하면, 이후에 나오는 거의 모든 주제가 연결된다.

- timer interrupt가 왜 중요한가
- lock을 잡고 오래 버티면 왜 시스템이 느려지는가
- priority donation 같은 정책이 왜 필요한가

## 핵심 모델 (최소 모델)

스레드는 상태를 가진다.

| 상태 | 의미 | ready_list에 있나? |
|---|---|---|
| RUNNING | 지금 CPU에서 실행 중 | 아니오 |
| READY | 실행할 준비 완료(대기열) | 예 |
| BLOCKED | 사건을 기다림(lock, sleep, I/O 등) | 아니오 |

스케줄러는 크게 두 일만 한다.

1) READY 중에서 “다음 실행”을 고른다.  
2) RUNNING을 READY/BLOCKED로 바꾸고, 선택된 스레드로 문맥을 바꾼다.  

## 예시 상황 (우선순위 + ready_list)

PintOS가 우선순위 스케줄링(높을수록 먼저)을 사용한다고 하자.

```text
T1 priority=10
T2 priority=20
T3 priority=31
```

`ready_list`가 priority 내림차순으로 유지된다면, 항상 `T3 -> T2 -> T1` 순으로 뽑히는 게 “정상”이다.

이제 질문이 바뀐다.

- “어떤 순간에 스레드가 ready_list로 들어가나?”
- “어떤 순간에 현재 스레드가 CPU를 양보하나?”

## Linux / Windows에서는 (현실 기준으로 잡기)

현실 OS는 PintOS보다 훨씬 복잡하다.

- Linux: per-CPU run queue, CFS(공정성), wakeup preemption, tickless, SMP load balancing 등
- Windows: priority 기반 스케줄링, quantum(타임 슬라이스), 다양한 wait 이유/스케줄링 클래스

하지만 PintOS로 축소해도 핵심은 변하지 않는다.

- “READY 큐가 있고”
- “타이머가 주기적으로 선점을 가능하게 만들고”
- “문맥 교체로 다음 실행 흐름으로 넘어간다”

## PintOS에서는 (코드로 내려가기)

### 1) ready_list는 무엇이고 어떻게 유지되나?

PintOS는 실행 가능한 스레드를 `ready_list`에 모아 둔다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`
  - `static struct list ready_list;`

이 코드베이스에서는 ready_list를 “우선순위 내림차순”으로 유지한다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`

```c
list_insert_ordered (&ready_list, &t->elem, thread_priority, NULL);
```

비교 함수:

```c
return list_entry (a, struct thread, elem)->priority >
       list_entry (b, struct thread, elem)->priority;
```

즉 “ready_list의 맨 앞”이 항상 가장 높은 우선순위 후보가 된다.

### 2) 언제 ready_list에 들어가나? (thread_unblock)

BLOCKED 스레드는 사건이 끝나면 READY가 된다. 그때 `thread_unblock()`이 호출된다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`

핵심:

```c
ASSERT (t->status == THREAD_BLOCKED);
list_insert_ordered (&ready_list, &t->elem, thread_priority, NULL);
t->status = THREAD_READY;
```

중요한 디테일:

- 이 함수는 “즉시 선점”을 하지 않는다(주석에 명시).
- 그래서 “깨어났는데 바로 안 달린다” 같은 현상이 생길 수 있다.

그 대신 “선점 여부 판단”을 별도의 지점에서 수행한다(예: `check_preemption()`).

### 3) 언제 현재 스레드가 ready_list로 돌아가나? (thread_yield)

현재 스레드가 CPU를 양보하면 READY로 돌아가서 ready_list에 다시 들어간다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`

핵심:

```c
if (curr != idle_thread)
    list_insert_ordered (&ready_list, &curr->elem, thread_priority, NULL);
do_schedule (THREAD_READY);
```

즉 양보(yield)는 “나도 ready_list로 돌아간 뒤, 다음 후보를 뽑아 달라”는 뜻이다.

### 4) 다음 스레드는 어떻게 고르나? (next_thread_to_run)

ready_list가 비어 있으면 idle을, 아니면 ready_list 맨 앞을 뽑는다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`

```c
if (list_empty (&ready_list))
    return idle_thread;
return list_entry (list_pop_front (&ready_list), struct thread, elem);
```

“맨 앞을 뽑는다”는 건, 곧 “우선순위 가장 높은 스레드를 뽑는다”는 뜻이다.

### 5) 선점은 어디서 결정되나? (check_preemption)

이 코드베이스에서는 ready_list의 맨 앞과 현재 스레드 priority를 비교해 양보한다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`

```c
struct thread *front = list_entry (list_begin (&ready_list), struct thread, elem);
if (thread_current ()->priority < front->priority)
    thread_yield ();
```

즉 “더 높은 우선순위가 READY가 되면, 지금 RUNNING이 양보”한다.

### 6) 타이머 틱이 어떻게 선점 기회를 만들까? (TIME_SLICE)

tick마다 `thread_tick()`이 호출되고, `TIME_SLICE`를 다 쓰면 양보를 “예약”한다.

- `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/threads/thread.c`

```c
if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
```

`TIME_SLICE = 4`라면 “4 tick마다 한 번은 양보할 기회가 생긴다”로 해석할 수 있다.

문맥 교체(context switch)까지의 실제 연결은 [[context-switch-trace]]에서 끝까지 추적한다.

## QEMU에서는 (역할 분리)

QEMU는 스케줄러가 아니다.

- “ready_list, priority, preemption 정책”은 PintOS 코드의 일이다.
- QEMU는 guest에 timer interrupt 같은 하드웨어 이벤트를 전달해 “선점이 가능해지는 조건”을 제공한다.

즉, **정책(누가 다음?)은 PintOS**, **사건(틱/인터럽트)은 QEMU가 흉내** 낸다.

## 숫자 예제 (TIME_SLICE=4의 의미)

`TIME_SLICE = 4`일 때, 같은 스레드가 계속 RUNNING이라면 tick 카운터는 이렇게 쌓인다.

```text
tick 1: thread_ticks=1
tick 2: thread_ticks=2
tick 3: thread_ticks=3
tick 4: thread_ticks=4 -> intr_yield_on_return() 예약
```

그리고 인터럽트 핸들러가 끝나는 “안전한 지점”에서 실제 `thread_yield()`가 호출되어 스케줄링이 진행된다.

## 직접 확인 (GDB로 ready_list 보기)

1) ready_list에 누가 들어가는지
   - breakpoint: `thread_unblock`, `thread_yield`
   - 확인: `p thread_current()->name`, `p thread_current()->priority`

2) ready_list에서 누가 뽑히는지
   - breakpoint: `next_thread_to_run`
   - 확인: `p list_empty(&ready_list)`
   - 반환 직전에 `next` 후보의 `name/priority` 확인

3) 선점이 언제 발생하는지
   - breakpoint: `check_preemption`, `thread_tick`
   - `TIME_SLICE` 만료와 “더 높은 우선순위 ready”의 두 경로를 분리해 관찰

## 다음으로 볼 문서

- [[interrupt-timer-qemu]]: “tick”이 어떻게 만들어지고 guest로 들어오는가
- [[context-switch-trace]]: schedule가 실제 레지스터/스택 교체로 내려가는 흐름
- [[week-1-threads-map]]: Threads 주차 지도
