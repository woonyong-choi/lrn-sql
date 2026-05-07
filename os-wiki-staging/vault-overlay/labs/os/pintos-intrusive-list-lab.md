---
type: Lab
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
  - week:threads
  - layer:memory
  - layer:kernel
  - topic:casting
  - topic:byte-buffer
  - topic:scheduler
related_to:
  - "[[concept-to-code-map]]"
  - "[[week-1-threads-map]]"
  - "[[thread-scheduler-trace]]"
  - "[[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]"
---

# PintOS intrusive list 실험: `list_elem` 주소에서 `struct thread` 복원하기
 
## 작은 질문
 
PintOS의 `ready_list`를 보면, 리스트가 들고 있는 건 `struct thread *`가 아니라 `struct list_elem *`다.
 
그러면 이런 질문이 생긴다.
 
- `ready_list`의 원소 하나를 보려면 왜 `list_entry(e, struct thread, elem)` 같은 매크로가 필요할까?
- 리스트가 “thread를 담는다”는 말은 정확히 **어떤 주소(바이트)를 담는다**는 뜻일까?
 
## 왜 필요한가
 
Threads 과제를 하다 보면 다음 문제가 자주 생긴다.
 
- `ready_list`가 망가져서 스케줄러가 엉뚱한 스레드를 고른다.
- “우선순위 정렬”은 맞게 했다고 생각했는데, 실제로는 다른 `list_elem`을 넣어버렸다.
- 한 스레드가 “두 리스트에 동시에 들어가면서” 링크가 깨진다. (intrusive list에서 특히 흔한 실수)
 
이때 디버깅의 핵심은 **리스트가 들고 있는 포인터가 정확히 무엇을 가리키는지**를 눈으로 확인하는 것이다.
 
## 핵심 모델 (머릿속에 넣을 최소 모델)
 
PintOS의 리스트는 **intrusive list**다.
 
- non-intrusive list: “노드 객체(node)가 따로 있고”, 노드가 `void *data` 같은 걸 가리킨다.
- intrusive list: “노드 객체가 따로 없다.” 대신 **바깥 구조체 안에 노드(`struct list_elem`)를 포함**한다.
 
즉 PintOS에서:
 
- 리스트가 들고 있는 건 “바깥 구조체 포인터”가 아니라
- “바깥 구조체 안에 박혀 있는 `struct list_elem`의 주소”다.
 
그래서 `list_entry()`는 “`list_elem` 주소 → 바깥 구조체 주소”를 되돌리는 변환기다.

## Linux / Windows에서는 (현실 연결: 같은 아이디어가 이미 있다)

PintOS의 intrusive list는 “교육용 특이한 기법”이 아니라, 실제 OS에서도 매우 흔한 패턴이다.

- Linux 커널: `list_head` + `container_of()` 패턴으로 같은 일을 한다.
  - 리스트가 들고 있는 건 `struct task_struct *`가 아니라, 그 안에 박힌 `struct list_head`의 주소다.
  - 그래서 “노드 주소에서 컨테이너 주소로 돌아가기”가 필수다.
- Windows 커널: `LIST_ENTRY`라는 이름으로 같은 구조를 쓴다.

결론은 하나다.

> “리스트가 스레드를 담는다”는 말은 보통 “스레드 구조체 *안의 어떤 필드 주소*를 연결한다”는 뜻이다.
 
## PintOS에서는 (코드 증거)
 
### 1) `list_entry()`가 실제로 하는 일
 
- PintOS: `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/lib/kernel/list.h`
  - 파일 상단 주석이 intrusive list 모델을 정확히 설명한다.
  - `list_entry(LIST_ELEM, STRUCT, MEMBER)` 매크로는 `offsetof()`로 “바깥 구조체에서 MEMBER가 몇 바이트 떨어져 있는지”를 이용해 되돌아간다.
 
매크로(핵심만):
 
```c
#define list_entry(LIST_ELEM, STRUCT, MEMBER) \
  ((STRUCT *) ((uint8_t *) &(LIST_ELEM)->next \
    - offsetof (STRUCT, MEMBER.next)))
```
 
포인트:
 
- `LIST_ELEM`은 `struct list_elem *`다.
- `offsetof(STRUCT, MEMBER.next)`는 “STRUCT 시작 주소에서 MEMBER.next 필드까지의 바이트 거리”다.
- 따라서 “`&(LIST_ELEM)->next`에서 그 거리만큼 빼면” STRUCT 시작 주소로 돌아간다.
 
### 2) `struct thread` 안에 `list_elem`이 여러 개 있을 수 있다
 
`struct thread`는 보통 ready_list에 들어갈 때 쓰는 `elem` 말고도,
다른 목적으로 쓰는 `list_elem`을 추가로 들 수 있다. (예: donation 리스트)
 
이게 의미하는 바:
 
- **한 스레드는 동시에 여러 리스트에 들어갈 수 있지만**
- 각 리스트는 반드시 “서로 다른 `list_elem` 필드”를 써야 한다.
 
같은 `elem`을 두 리스트에 넣으면 링크가 바로 깨진다.

### 3) head/tail 센티널(sentinel)을 먼저 의식하자

- PintOS: `/Users/woonyong/workspace/Krafton-Jungle/SW_AI-W09-pintos/pintos/include/lib/kernel/list.h`
  - `struct list`는 `head`와 `tail`을 “항상 존재하는 센티널 원소”로 가진다.
  - `list_begin()`은 “비어 있지 않으면 첫 원소, 비어 있으면 tail”을 반환한다.

그래서 디버깅할 때의 안전 규칙은 이거 하나로 줄어든다.

> `list_empty()`로 비었는지 먼저 확인하고, 비어있지 않을 때만 `list_entry()`로 캐스팅한다.
 
## 숫자와 메모리: `list_elem` 주소에서 “컨테이너” 주소로 돌아가기
 
가정을 두고 계산해 보자.
 
```text
struct thread *t = 0x80000000
offsetof(struct thread, elem) = 0x120   (예시)
```
 
그러면:
 
```text
&t->elem = 0x80000000 + 0x120 = 0x80000120
&(t->elem.next) = 0x80000120 + 8 = 0x80000128   (x86-64 포인터 8바이트 가정)
```
 
`list_entry(e, struct thread, elem)`가 하는 계산은 결국:
 
```text
t = (uint8_t *)&e->next - offsetof(struct thread, elem.next)
```
 
즉 `e`(list_elem 포인터)가 주어지면, 바이트 오프셋을 빼서 `struct thread` 시작으로 되돌아간다.

## QEMU에서는 (역할 분리)

이 실험은 전부 PintOS(guest OS) 내부 자료구조 이야기다.

- QEMU는 `ready_list`를 만들지도, `struct thread`를 관리하지도 않는다.
- QEMU는 “타이머 인터럽트 같은 하드웨어 사건을 guest에 전달”할 뿐이다.

즉 list가 꼬이면 그건 PintOS 코드/자료구조 버그이고, QEMU는 “그 버그가 실행되는 환경”일 뿐이다.
 
## 직접 확인 (GDB 체크리스트)
 
아래는 “ready_list 첫 원소가 누구인지”를 주소 계산으로 확인하는 최소 루프다.
 
1) breakpoint를 건다
   - `b thread_yield`
   - 또는 `b schedule`
 
2) ready_list의 첫 원소(`list_elem *`)를 잡는다
 
```gdb
p list_begin(&ready_list)
```
 
3) 그 `list_elem *`에서 `struct thread *`로 복원한다
 
```gdb
p list_entry(list_begin(&ready_list), struct thread, elem)
p list_entry(list_begin(&ready_list), struct thread, elem)->name
```
 
4) 오프셋을 직접 확인해 “정말 빼기 계산”인지 확인한다 (선택)
 
```gdb
p/x (size_t) &((struct thread *)0)->elem
p/x (size_t) &((struct thread *)0)->elem.next
```
 
주의:
 
- `list_begin()`이 반환하는 게 head/tail 같은 “센티널(sentinel)”이면 `list_entry()`를 쓰면 안 된다.
- 보통 `list_empty(&ready_list)`를 먼저 확인하고, 비어있지 않을 때만 interior element에 `list_entry()`를 적용한다.

팁:

- `ready_list`는 `thread.c`의 `static` 전역이라, 심볼이 안 보이면 `thread_yield()`/`schedule()` 같은 함수 안에서 멈춘 상태에서 보는 편이 쉽다.
- “정말로 tail을 반환하는지”는 아래처럼 확인할 수 있다.

```gdb
p list_empty(&ready_list)
p list_begin(&ready_list) == list_end(&ready_list)
```
 
## 정리
 
- PintOS의 리스트는 “데이터 포인터를 담는 리스트”가 아니라 “구조체 안에 박힌 `list_elem` 주소를 연결하는 리스트”다.
- 그래서 디버깅할 때는 `list_elem *`가 어떤 struct의 어떤 필드인지(예: `thread.elem` vs `thread.donation_elem`)를 항상 의식해야 한다.
 
## 다음 링크
 
- [[thread-scheduler-trace]]: ready_list가 언제 바뀌는지 전체 흐름 추적
- [[context-switch-trace]]: 선택된 스레드로 레지스터/스택이 어떻게 바뀌는지
- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]: “같은 바이트를 타입으로 해석한다” 감각을 더 넓게 잡기
