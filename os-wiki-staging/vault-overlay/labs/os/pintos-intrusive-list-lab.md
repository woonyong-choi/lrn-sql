---
type: Lab
status: Active
systems:
  - Linux
  - Windows
  - PintOS
tags:
  - domain:os
  - domain:pintos
  - layer:kernel
  - layer:memory
  - topic:intrusive-list
  - topic:list_entry
  - topic:offsetof
related_to:
  - "[[week-1-threads-map]]"
  - "[[thread-scheduler-trace]]"
  - "[[context-switch-trace]]"
---

# PintOS intrusive list 실험 (`list_entry`를 바이트로 이해하기)

## 작은 질문

- `ready_list`는 왜 `struct thread *` 리스트가 아니라 `struct list_elem *` 리스트인가?
- `list_entry(e, struct thread, elem)`은 어떻게 “바깥 구조체”로 되돌아갈 수 있을까?
- `struct thread`의 `elem`을 동시에 두 개 리스트에 넣으면 왜 바로 망가질까?

## 왜 필요한가

운영체제는 커널 내부에서 “많은 객체(thread, page, file, lock waiter, ...)를 리스트로 묶어 관리”한다.

이때 흔히 두 가지 선택지가 있다.

1) **비-intrusive(list node를 따로 할당)**: 노드(링크)를 heap에서 따로 만들고, 노드가 객체 포인터를 가진다.
2) **intrusive(객체 안에 링크 필드를 포함)**: 객체 자체가 `prev/next` 같은 링크를 “필드로 내장”한다.

PintOS의 리스트는 2) intrusive 방식이다.

- 장점: 동적 할당이 없어도 된다(커널에서 “할당 실패”를 피하기 쉽다), 캐시 친화적일 수 있다
- 단점: 타입 안전성이 약하고, **한 `list_elem`은 한 순간에 한 리스트에만** 들어갈 수 있다

## 핵심 모델

### 1) intrusive list는 “리스트 원소 포인터”만 본다

PintOS 리스트는 `struct list_elem`만을 원소로 다룬다.

- 리스트는 `struct list_elem prev/next`만 알면 된다.
- 하지만 우리는 결국 `struct thread *`가 필요하다.

그래서 “되돌아가기”가 필요하고, 그 역할이 `list_entry`다.

### 2) `list_entry`는 결국 `offsetof` 기반의 포인터 산술이다

PintOS 구현(요약):

- `pintos/include/lib/kernel/list.h`: `list_entry(LIST_ELEM, STRUCT, MEMBER)`
- 핵심 아이디어: `LIST_ELEM`의 주소에서 “`STRUCT` 안에서 `MEMBER`가 놓인 오프셋”을 빼면 `STRUCT *`가 된다.

이 말은 즉, `list_entry`는 “바이트 레벨 주소 계산”이다.

## 예시 상황: `struct thread`의 `elem`은 어디 바이트에 있나?

PintOS의 `struct thread`에는 리스트에 연결될 수 있는 필드가 들어 있다.

- `pintos/include/threads/thread.h`: `struct list_elem elem;` (ready_list, waiters, 과제에 따라 sleep_list 등에서 사용)

이때 다음이 성립해야 한다.

- `&t->elem`은 `t`가 가리키는 메모리 블록 “중간 어딘가”다.
- `list_entry(&t->elem, struct thread, elem) == t`가 되어야 한다.

## Linux / Windows에서는

현실 OS도 intrusive list 패턴을 많이 쓴다.

- Linux는 유명한 `struct list_head` 패턴을 통해 같은 문제를 푼다(개념적으로 PintOS와 동일).
- Windows도 커널 내부에 연결 리스트 기반 자료구조가 많다(구현은 다르지만 같은 방향의 문제를 푼다).

핵심은 “성능/안정성/할당 실패”를 이유로 커널에서 이런 선택이 자주 나온다는 점이다.

## PintOS에서는 (코드 증거로 연결)

### 1) 리스트 설계 자체가 intrusive임을 문서로 밝힌다

- `pintos/include/lib/kernel/list.h` 상단 주석:
  - “동적 메모리 할당을 요구하지 않는다”
  - “원소가 될 구조체가 `struct list_elem`을 직접 포함해야 한다”

### 2) 스케줄러 ready list는 `thread->elem`을 꽂아 넣는다

- `pintos/threads/thread.c`: `static struct list ready_list;`
- `pintos/threads/thread.c`: `thread_unblock()` 등에서 `&t->elem`을 `ready_list`에 삽입한다.
- `pintos/threads/thread.c`: `next_thread_to_run()`에서 `list_pop_front(&ready_list)`의 결과를 `list_entry(..., struct thread, elem)`로 되돌린다.

### 3) “한 elem은 한 리스트만” 규칙을 실수로 깨기 쉽다

PintOS 기본 주석은 보통 다음 논리를 말한다.

- ready 상태일 때만 run queue(ready_list)에 있다
- blocked 상태일 때만 semaphore waiters 등에 있다
- 그래서 같은 `elem`을 재사용할 수 있다

하지만 과제를 진행하며 `sleep_list` 같은 자료구조를 추가하면 상황이 달라진다.

- `pintos/threads/thread.c`: `sleep_list`에 `&cur->elem`을 넣는 구현이 흔하다
- 이때도 “동시에 두 리스트에 들어가지 않게” 상태 설계를 유지해야 한다

## 숫자와 메모리: `list_entry`를 손으로 계산해 보기

가정을 하나 두자.

- `struct thread *t = (struct thread *)0x8048000`
- `offsetof(struct thread, elem) = 0x50` (예시 값: 실제 값은 구현/컴파일러 옵션에 따라 달라질 수 있다)

그럼:

```text
&t->elem = 0x8048000 + 0x50 = 0x8048050
```

반대로, `e = &t->elem`만 가지고 있을 때:

```text
t = (struct thread *)((uint8_t *)e - offsetof(struct thread, elem))
  = (struct thread *)(0x8048050 - 0x50)
  = 0x8048000
```

이 계산이 바로 intrusive list의 “되돌아가기”다.

## 직접 확인 (GDB)

목표: “정말로 `list_entry(&t->elem, struct thread, elem) == t`인가?”를 눈으로 확인한다.

1) `thread_unblock()` 또는 `next_thread_to_run()`에 브레이크를 건다.

```gdb
b thread_unblock
b next_thread_to_run
c
```

2) `t`와 `&t->elem`을 확인한다.

```gdb
p t
p &t->elem
```

3) `list_entry`를 적용한 값이 원래 포인터와 같은지 확인한다.

```gdb
p (struct thread *)((uint8_t *)&t->elem - offsetof(struct thread, elem))
```

4) 리스트에서 나온 `struct list_elem *e`를 `struct thread *`로 되돌려 본다.

```gdb
p e
p (struct thread *)((uint8_t *)e - offsetof(struct thread, elem))
```

## 정리

- intrusive list는 “객체 안에 링크를 내장”해서 동적 할당 없이 리스트를 구성한다.
- `list_entry`는 `offsetof` 기반 포인터 산술이며, 결국 바이트 주소 계산이다.
- 한 `list_elem`은 한 순간에 한 리스트에만 들어갈 수 있다. 과제에서 리스트를 늘릴수록 이 규칙이 더 중요해진다.

## 다음 링크

- [[바이트 버퍼와 캐스팅 실험]]: “주소/타입/해석” 감각을 더 단단히 만들기
- [[thread-scheduler-trace]]: ready_list가 실제로 어떻게 굴러가는지 흐름으로 보기
- [[context-switch-trace]]: 스케줄링 결정 이후 레지스터/스택이 어떻게 바뀌는지 보기
