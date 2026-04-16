---
name: c-style
description: C 코딩 스타일 컨벤션. C 코드를 작성·리뷰·리팩터링할 때 활성화. "C 코드", "malloc", "포인터", "헤더 파일", ".c 파일", ".h 파일"이 언급되거나 C 소스를 생성·수정할 때 이 스킬을 따른다.
---

# C 코딩 스타일 컨벤션

> 목적: 팀원 간 코드 스타일을 통일하고, AI 도구(Claude, Codex)가 코드를 생성할 때도 동일한 규칙을 따르게 한다.
> 기반: Linux kernel style + K&R 변형. 크래프톤 정글 시스템 프로그래밍 환경에 맞춰 조정.

---

## 포맷팅

### 들여쓰기

- 4칸 스페이스 (탭 금지)
- 한 줄 최대 80자 (불가피하면 100자까지 허용)

### 중괄호

함수 정의만 다음 줄, 나머지(if/for/while/struct)는 같은 줄:

```c
/* 함수 정의: 여는 중괄호를 다음 줄에 */
int find_fit(size_t size)
{
    /* 제어문: 여는 중괄호를 같은 줄에 */
    if (size == 0) {
        return -1;
    }

    for (int i = 0; i < MAX_SIZE; i++) {
        if (block[i] >= size) {
            return i;
        }
    }
    return -1;
}
```

### 단일 문장 블록

if/for/while 본문이 한 줄이라도 중괄호를 쓴다:

```c
/* 좋음 */
if (ptr == NULL) {
    return;
}

/* 나쁨 — 중괄호 없음 */
if (ptr == NULL)
    return;
```

### 공백

```c
/* 키워드 뒤 공백 */
if (condition)
for (int i = 0; i < n; i++)
while (running)

/* 함수 호출은 공백 없음 */
printf("hello");
malloc(size);

/* 이항 연산자 양쪽 공백, 단항 연산자는 붙임 */
int x = a + b;
int *p = &x;
size_t len = ~mask;

/* 포인터 선언: * 는 변수에 붙인다 */
int *ptr;       /* 좋음 */
int* ptr;       /* 나쁨 */
int * ptr;      /* 나쁨 */
```

### 빈 줄

- 함수 사이: 1줄
- 함수 내부 논리 구분: 1줄
- 연속 빈 줄 2줄 이상 금지

---

## 네이밍

### 규칙

| 대상 | 형식 | 예시 |
|------|------|------|
| 변수, 함수 | snake_case | `block_size`, `find_fit` |
| 상수, 매크로 | UPPER_SNAKE_CASE | `MAX_HEAP`, `ALIGNMENT` |
| typedef 타입 | snake_case + `_t` | `block_t`, `header_t` |
| struct 태그 | snake_case | `struct free_node` |
| enum 값 | UPPER_SNAKE_CASE | `STATUS_OK`, `STATUS_ERR` |
| 파일명 | snake_case | `mm.c`, `free_list.c` |

### 금지

- 헝가리안 표기법 (`iCount`, `pNode`) 쓰지 않는다
- 한 글자 변수는 루프 카운터(`i`, `j`, `k`)와 포인터 반복(`p`, `q`)만 허용
- 약어가 명확하지 않으면 풀어 쓴다: `blk` → `block`, `sz` → `size` (단, `ptr`, `len`, `idx`, `cnt`, `buf`, `src`, `dst`는 허용)

---

## L-value / R-value

### 개념

- **L-value** — 메모리 위치를 가진 표현식. 대입의 왼쪽에 올 수 있다. (변수, 배열 원소, 포인터 역참조 등)
- **R-value** — 임시 값. 대입의 왼쪽에 올 수 없다. (리터럴, 함수 반환값, 산술 결과 등)

### 규칙

```c
/* L-value: 대입 가능 */
int x = 10;
arr[i] = 5;
*ptr = 42;
node->data = 7;

/* R-value: 대입 불가 — 컴파일 에러 */
10 = x;           /* ❌ 리터럴은 L-value가 아니다 */
a + b = c;        /* ❌ 산술 결과는 L-value가 아니다 */
get_value() = 3;  /* ❌ 반환값은 L-value가 아니다 (포인터 반환 제외) */
```

### 실수하기 쉬운 패턴

```c
/* 포인터 역참조는 L-value다 */
*(ptr + i) = value;    /* 좋음 — L-value */

/* 후위 증가의 결과는 R-value다 */
int *p = arr;
*p++ = 10;             /* 좋음 — *p에 대입 후 p 증가 */
/* 풀어쓰면: *p = 10; p = p + 1; */

/* const 변수는 L-value이지만 수정 불가 */
const int MAX = 100;
MAX = 200;             /* ❌ 컴파일 에러 — const L-value */

/* 캐스트 결과는 R-value다 */
(int)x = 5;            /* ❌ 캐스트 결과에 대입 불가 */
```

---

## 조건문 비교 순서

### 규칙: 변수를 왼쪽, 상수를 오른쪽에 둔다

```c
/* 좋음 — 자연스러운 순서 */
if (ptr == NULL)
if (size > 0)
if (count != MAX_SIZE)
if (status == STATUS_OK)

/* 나쁨 — Yoda style (이 프로젝트에서는 사용하지 않는다) */
if (NULL == ptr)
if (0 < size)
if (MAX_SIZE != count)
```

### 이유

- Yoda style (`if (NULL == ptr)`)은 `=`와 `==` 오타를 컴파일 타임에 잡기 위한 기법이다.
- 하지만 현대 컴파일러는 `-Wall`만으로 `if (ptr = NULL)` 실수를 경고한다.
- 자연어 어순에 맞는 `if (ptr == NULL)`이 읽기 쉽다.
- **따라서 이 프로젝트에서는 Yoda style을 쓰지 않는다.**

### 대입 실수 방지

```c
/* 위험 — 대입과 비교 혼동 */
if (x = 0) { ... }    /* ⚠️ 항상 false, -Wall이 경고함 */

/* 의도적 대입은 괄호를 한 겹 더 씌운다 */
if ((ptr = malloc(size)) != NULL) {
    /* 할당 성공 */
}
```

### NULL / 포인터 비교

```c
/* 명시적 비교를 선호한다 */
if (ptr == NULL)       /* 좋음 — 의도가 명확 */
if (ptr != NULL)       /* 좋음 */

/* 암묵적 비교도 허용하지만, 포인터는 명시적을 권장 */
if (!ptr)              /* 허용 — 숙련자 사이에서 흔함 */
if (ptr)               /* 허용 */

/* 정수 0과의 비교는 명시적 */
if (count == 0)        /* 좋음 */
if (!count)            /* 나쁨 — count가 불리언처럼 읽힘 */
```

### 범위 비교

```c
/* 범위 조건은 수직선 순서로 쓴다 (왼쪽 < 오른쪽) */
if (0 <= index && index < size)    /* 좋음 — 수직선: 0 ≤ index < size */
if (index >= 0 && index < size)    /* 허용 — 같은 의미 */
if (size > index && index >= 0)    /* 나쁨 — 읽기 어려움 */
```

---

## 헤더 파일

### Include Guard

```c
#ifndef MM_H
#define MM_H

/* 선언부 */

#endif /* MM_H */
```

### Include 순서

```c
/* 1. 대응하는 헤더 (foo.c → foo.h) */
#include "mm.h"

/* 2. 시스템/표준 라이브러리 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 3. 프로젝트 내부 헤더 */
#include "memlib.h"
#include "config.h"
```

### 원칙

- 헤더에 함수 구현 넣지 않는다 (인라인 제외)
- 헤더에서 다른 헤더를 최소한으로 include한다
- 전역 변수 선언은 헤더에, 정의는 .c에

---

## 함수

### 원칙

- 한 함수 50줄 이내 권장 (넘으면 분리 검토)
- 파라미터 4개 이하 권장 (넘으면 struct로 묶기 검토)
- 반환값이 에러를 나타내면 0 = 성공, -1 = 실패 (또는 NULL)
- 함수 선두에 한 줄 주석으로 역할 설명

```c
/* 가용 리스트에서 size 이상인 첫 번째 블록을 찾는다 */
static void *find_fit(size_t asize)
{
    ...
}
```

### static 사용

- 파일 외부에 노출할 필요 없는 함수와 전역 변수는 반드시 `static`
- 헤더에 선언하지 않는 함수 = `static`

---

## 메모리 관리

### malloc / free 규칙

```c
/* 반환값 항상 NULL 체크 */
void *ptr = malloc(size);
if (ptr == NULL) {
    /* 에러 처리 */
    return -1;
}

/* sizeof에 변수를 쓴다 (타입 대신) */
int *arr = malloc(n * sizeof(*arr));   /* 좋음 */
int *arr = malloc(n * sizeof(int));    /* 나쁨 — 타입 변경 시 불일치 위험 */

/* free 후 NULL 대입 */
free(ptr);
ptr = NULL;

/* calloc: 0 초기화가 필요할 때 */
int *arr = calloc(n, sizeof(*arr));
```

### 포인터 안전

- 사용 전 NULL 체크
- 범위 밖 접근 금지 (배열 크기 상수화)
- dangling pointer 방지 (free 후 NULL)
- 포인터 연산은 의도를 주석으로 명시

---

## 주석

### 스타일

```c
/* 한 줄 주석은 이 스타일 */

/*
 * 여러 줄 주석은
 * 이 스타일로 작성한다
 */

// C99 스타일도 허용하지만 /* */ 을 선호
```

### 원칙

- "무엇(what)"보다 "왜(why)"를 적는다
- 코드와 주석이 불일치하면 주석이 틀린 것이다 — 코드 수정 시 주석도 함께 수정
- 자명한 코드에 주석 달지 않는다: `i++; /* i를 1 증가 */` ← 쓸모없음
- TODO/FIXME는 이름과 날짜를 남긴다: `/* TODO(woonyong): 경계 조건 처리 2026-04 */`

### 함수 문서화 (공개 함수)

```c
/*
 * mm_malloc - size 바이트 이상의 정렬된 블록을 할당한다.
 *
 * 가용 리스트를 first-fit으로 탐색하고, 실패 시 힙을 확장한다.
 * 반환값: 할당된 블록의 payload 포인터. 실패 시 NULL.
 */
void *mm_malloc(size_t size)
{
    ...
}
```

---

## 매크로

```c
/* 매크로 인자는 반드시 괄호로 감싼다 */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PACK(size, alloc) ((size) | (alloc))

/* 여러 문장 매크로는 do-while(0) */
#define LOG_ERROR(msg) do { \
    fprintf(stderr, "ERROR: %s\n", (msg)); \
    exit(1); \
} while (0)

/* 매직 넘버 금지 — 상수로 정의 */
#define ALIGNMENT 8
#define WSIZE 4
#define DSIZE 8
#define CHUNKSIZE (1 << 12)
```

---

## 에러 처리

- 에러 경로를 먼저 처리하고 리턴 (early return)
- 중첩 if 최소화

```c
/* 좋음 — early return */
void *mm_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    size_t asize = align(size + OVERHEAD);
    void *ptr = find_fit(asize);
    if (ptr == NULL) {
        ptr = extend_heap(asize);
        if (ptr == NULL) {
            return NULL;
        }
    }

    place(ptr, asize);
    return ptr;
}

/* 나쁨 — 깊은 중첩 */
void *mm_malloc(size_t size)
{
    if (size != 0) {
        size_t asize = align(size + OVERHEAD);
        void *ptr = find_fit(asize);
        if (ptr != NULL) {
            place(ptr, asize);
            return ptr;
        } else {
            ptr = extend_heap(asize);
            if (ptr != NULL) {
                place(ptr, asize);
                return ptr;
            }
        }
    }
    return NULL;
}
```

---

## 컴파일

### 컴파일 플래그

```bash
# 기본 개발용
gcc -Wall -Wextra -Werror -std=c99 -g -O0

# 경고를 에러로 — 경고 방치 금지
# -g: 디버깅 심볼
# -O0: 최적화 끔 (디버깅용)

# 제출/릴리즈용
gcc -Wall -Wextra -Werror -std=c99 -O2
```

### Makefile 필수

- `make` → 빌드
- `make clean` → 산출물 제거
- `make test` → 테스트 실행 (있을 경우)

---

## 빠른 참조

| 항목 | 규칙 |
|------|------|
| 들여쓰기 | 4칸 스페이스 |
| 줄 길이 | 80자 (최대 100자) |
| 네이밍 | snake_case, 상수 UPPER |
| 포인터 | `int *p` (변수에 붙임) |
| 중괄호 | 함수만 다음줄, 나머지 같은줄 |
| 조건문 순서 | 변수 왼쪽, 상수 오른쪽 (No Yoda) |
| 범위 비교 | `0 <= i && i < n` (수직선 순서) |
| 단일문 블록 | 항상 중괄호 |
| malloc | NULL 체크, sizeof(*var), free 후 NULL |
| 함수 크기 | 50줄 이내 |
| static | 외부 노출 불필요하면 static |
| 경고 | -Wall -Wextra -Werror |
