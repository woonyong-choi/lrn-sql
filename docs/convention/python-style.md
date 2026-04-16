---
name: python-style
description: Python 코딩 스타일 컨벤션. Python 코드를 작성·리뷰·리팩터링할 때 활성화. ".py 파일", "파이썬", "Django", "Flask", "FastAPI", "pytest"가 언급되거나 Python 소스를 생성·수정할 때 이 스킬을 따른다.
---

# Python 코딩 스타일 컨벤션

> 목적: 팀원 간 코드 스타일을 통일하고, AI 도구(Claude, Codex)가 코드를 생성할 때도 동일한 규칙을 따르게 한다.
> 기반: PEP 8 + Google Python Style Guide 발췌. 크래프톤 정글 환경에 맞춰 조정.

---

## 포맷팅

### 들여쓰기

- 4칸 스페이스 (탭 금지)
- 한 줄 최대 88자 (black 기본값, PEP 8의 79자보다 실용적)

### 줄 바꿈

```python
# 긴 함수 호출 — 여는 괄호 뒤에서 줄바꿈
result = some_function(
    first_arg,
    second_arg,
    third_arg,
)

# 긴 조건문 — 연산자를 줄 앞에
if (
    condition_one
    and condition_two
    and condition_three
):
    do_something()

# 리스트/딕셔너리 — trailing comma 권장
colors = [
    "red",
    "green",
    "blue",
]
```

### 빈 줄

- 최상위 함수/클래스 사이: 2줄
- 클래스 내 메서드 사이: 1줄
- 함수 내부 논리 구분: 1줄
- 연속 빈 줄 2줄 이상 금지 (최상위 구분 제외)

### 따옴표

- 문자열: 쌍따옴표 `"hello"` (일관성 우선)
- docstring: 쌍따옴표 세 개 `"""docstring"""`
- f-string 적극 사용: `f"size: {n}"`

---

## 네이밍

| 대상 | 형식 | 예시 |
|------|------|------|
| 변수, 함수 | snake_case | `block_size`, `find_fit` |
| 상수 | UPPER_SNAKE_CASE | `MAX_HEAP`, `DEFAULT_PORT` |
| 클래스 | PascalCase | `FreeList`, `HttpClient` |
| 모듈, 패키지 | snake_case | `free_list.py`, `utils/` |
| private | `_` 접두사 | `_internal_state` |
| dunder | `__`양쪽`__` | `__init__`, `__repr__` |
| 사용 안하는 변수 | `_` | `for _ in range(n)` |

### 금지

- 한 글자 변수는 루프 카운터(`i`, `j`, `k`), 좌표(`x`, `y`), 수학(`n`, `m`)만 허용
- `l`(소문자 L), `O`(대문자 o), `I`(대문자 i) 단독 사용 금지 — 숫자와 혼동
- 약어는 명확할 때만: `db`, `url`, `api`, `idx`, `cnt`, `buf` 허용. 그 외는 풀어쓴다

---

## 할당 대상 (L-value / R-value 개념)

### 개념

Python에는 C처럼 엄격한 L-value/R-value 구분이 없지만, "대입 가능한 대상"과 "값 표현식"의 구분은 존재한다.

- **대입 가능 (assignable)** — 변수, 리스트 원소, 딕셔너리 키, 객체 속성, 언패킹 대상
- **대입 불가** — 리터럴, 함수 호출 결과, 연산 결과

```python
# 대입 가능
x = 10
arr[i] = 5
data["key"] = value
obj.attr = 42
a, b = 1, 2           # 언패킹

# 대입 불가 — SyntaxError
10 = x                 # ❌ 리터럴
a + b = c              # ❌ 연산 결과
func() = 3             # ❌ 함수 반환값
```

### 실수하기 쉬운 패턴

```python
# 튜플 언패킹은 L-value처럼 동작한다
first, *rest = [1, 2, 3, 4]   # first=1, rest=[2,3,4]

# 슬라이스 대입 — 리스트만 가능, 튜플은 불가
lst[1:3] = [10, 20]           # 좋음 — 리스트 슬라이스 대입
# tup[1:3] = (10, 20)         # ❌ TypeError — 튜플은 immutable

# walrus operator (:=) — 표현식 안에서 대입
if (n := len(data)) > 10:     # n에 대입하면서 비교
    print(f"데이터가 {n}개로 많다")

# augmented assignment는 대입 가능 대상에만
x += 1                        # 좋음
# 10 += x                     # ❌ SyntaxError
```

---

## 조건문 비교 순서

### 규칙: 변수를 왼쪽, 상수/리터럴을 오른쪽에 둔다

```python
# 좋음 — 자연스러운 순서
if count == 0:
if status != "error":
if size > MAX_SIZE:
if name in valid_names:

# 나쁨 — Yoda style (Python에서는 쓰지 않는다)
if 0 == count:
if "error" != status:
if MAX_SIZE < size:
```

### None 비교: `is` / `is not` 사용

```python
# 좋음 — PEP 8 권장, identity 비교
if result is None:
if result is not None:

# 나쁨 — 동등 비교 (None은 싱글턴이므로 is가 맞다)
if result == None:
if result != None:

# 나쁨 — Yoda style
if None is result:
```

### 불리언 비교: 직접 비교하지 않는다

```python
# 좋음 — 값 자체를 조건으로
if is_valid:
if not is_valid:

# 나쁨 — True/False와 명시적 비교
if is_valid == True:
if is_valid is True:     # 극히 드문 경우(sentinel 구분)에만 허용
if is_valid == False:
```

### 빈 컬렉션 비교: falsy 활용

```python
# 좋음 — Pythonic
if not items:             # 빈 리스트/딕셔너리/문자열
if items:                 # 비어있지 않으면

# 나쁨 — 명시적 길이 비교
if len(items) == 0:
if items == []:
if items == {}:
```

### 타입 비교: isinstance 사용

```python
# 좋음
if isinstance(x, int):
if isinstance(x, (int, float)):

# 나쁨 — 정확한 타입 비교 (상속 무시)
if type(x) == int:
if type(x) is int:        # 허용하지만 isinstance 선호
```

### 범위 비교: chained comparison

```python
# 좋음 — Python의 chained comparison
if 0 <= index < size:
if low <= value <= high:

# 나쁨 — and로 분리
if index >= 0 and index < size:
if value >= low and value <= high:
```

### 멤버십 비교: `in` 활용

```python
# 좋음
if status in ("active", "pending", "review"):
if char not in whitespace:

# 나쁨 — or 나열
if status == "active" or status == "pending" or status == "review":
```

---

## Import

### 순서 (isort 기본 규칙)

```python
# 1. 표준 라이브러리
import os
import sys
from pathlib import Path

# 2. 서드파티 라이브러리
import requests
from flask import Flask

# 3. 프로젝트 내부 모듈
from myproject.utils import parse_config
from myproject.models import User
```

### 원칙

- 그룹 사이 빈 줄 1개
- `import *` 금지
- 모듈 단위 import 선호: `import os` > `from os import path`
- 단, 자주 쓰는 이름은 직접 import 허용: `from typing import Optional`

---

## 타입 힌트

### 원칙

- 공개 함수의 파라미터와 반환값에는 반드시 타입 힌트
- 내부 함수는 권장 (필수 아님)
- 변수 힌트는 타입이 자명하지 않을 때만

```python
# 좋음
def find_fit(size: int) -> Optional[Block]:
    """size 이상인 가용 블록을 찾는다."""
    ...

def allocate(request: AllocRequest) -> AllocResponse:
    ...

# 변수 힌트 — 타입이 자명하지 않을 때
result: dict[str, list[int]] = parse_data(raw)

# 불필요 — 타입이 자명
count = 0          # int가 명백
name = "hello"     # str가 명백
```

### 자주 쓰는 타입

```python
from typing import Optional, Union
from collections.abc import Sequence, Mapping

# Python 3.10+
def process(data: str | None) -> list[int]:
    ...

# Python 3.9 이하
def process(data: Optional[str]) -> list[int]:
    ...
```

---

## 함수

### 원칙

- 한 함수 30줄 이내 권장
- 파라미터 5개 이하 권장 (넘으면 dataclass/dict로 묶기)
- 하나의 함수는 하나의 일만 한다
- 부수효과(side effect)가 있으면 함수 이름에 드러낸다: `save_to_db()`, `print_report()`

### docstring

Google style docstring:

```python
def mm_malloc(size: int) -> Optional[int]:
    """size 바이트 이상의 정렬된 블록을 할당한다.

    가용 리스트를 first-fit으로 탐색하고, 실패 시 힙을 확장한다.

    Args:
        size: 요청 바이트 수. 0이면 None을 반환한다.

    Returns:
        할당된 블록의 payload 주소. 실패 시 None.

    Raises:
        MemoryError: 힙 확장에 실패했을 때.
    """
    ...
```

### 간단한 함수는 한 줄 docstring

```python
def is_allocated(header: int) -> bool:
    """헤더의 allocated 비트를 확인한다."""
    return bool(header & 0x1)
```

---

## 클래스

```python
class FreeList:
    """가용 블록을 관리하는 명시적 이중 연결 리스트.

    Attributes:
        head: 리스트의 첫 번째 노드.
        size: 현재 가용 블록 수.
    """

    def __init__(self) -> None:
        self.head: Optional[Block] = None
        self.size: int = 0

    def insert(self, block: Block) -> None:
        """블록을 리스트 앞에 삽입한다 (LIFO)."""
        ...

    def remove(self, block: Block) -> None:
        """블록을 리스트에서 제거한다."""
        ...

    def __len__(self) -> int:
        return self.size

    def __repr__(self) -> str:
        return f"FreeList(size={self.size})"
```

### 원칙

- `__init__`에서 모든 인스턴스 속성을 선언한다
- public 메서드 → private 메서드 순서
- `@property`는 계산 비용이 낮을 때만
- dataclass는 데이터 보관용 클래스에 적극 활용

```python
from dataclasses import dataclass

@dataclass
class Block:
    address: int
    size: int
    is_allocated: bool = False
```

---

## 에러 처리

### 원칙

- bare `except:` 금지 — 반드시 예외 타입 명시
- 에러를 삼키지 않는다 (catch 후 무시 금지)
- 복구 가능하면 처리, 불가능하면 상위로 전파

```python
# 좋음
try:
    data = json.loads(raw)
except json.JSONDecodeError as e:
    logger.error("JSON 파싱 실패: %s", e)
    raise

# 나쁨 — bare except
try:
    data = json.loads(raw)
except:
    pass

# 좋음 — 구체적 예외, 유의미한 메시지
def read_config(path: str) -> dict:
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        raise FileNotFoundError(f"설정 파일이 없습니다: {path}")
    except json.JSONDecodeError as e:
        raise ValueError(f"설정 파일 파싱 실패: {path}") from e
```

### EAFP vs LBYL

```python
# EAFP (Easier to Ask Forgiveness) — Pythonic
try:
    value = data[key]
except KeyError:
    value = default

# 또는 더 간결하게
value = data.get(key, default)

# LBYL (Look Before You Leap) — 성능이 중요할 때
if key in data:
    value = data[key]
```

---

## 로깅

```python
import logging

logger = logging.getLogger(__name__)

# 좋음 — lazy formatting
logger.info("블록 할당: size=%d, addr=0x%x", size, addr)
logger.error("할당 실패: %s", error)

# 나쁨 — eager formatting (로그 레벨 꺼져도 문자열 생성)
logger.info(f"블록 할당: size={size}, addr={hex(addr)}")
```

- `print()` 대신 `logging` 사용 (디버깅용 print는 커밋 전 제거)
- 로그 레벨: DEBUG < INFO < WARNING < ERROR < CRITICAL

---

## 테스트

### 네이밍

```python
# 파일: test_<모듈명>.py
# 함수: test_<기능>_<시나리오>

def test_find_fit_returns_first_matching_block():
    ...

def test_find_fit_returns_none_when_no_fit():
    ...

def test_coalesce_merges_adjacent_free_blocks():
    ...
```

### 구조

```python
def test_malloc_zero_returns_none():
    """size 0 요청 시 None을 반환해야 한다."""
    # Arrange
    heap = Heap(capacity=1024)

    # Act
    result = heap.malloc(0)

    # Assert
    assert result is None
```

### 원칙

- Arrange-Act-Assert 패턴
- 한 테스트에 한 가지만 검증
- 테스트 간 의존성 없음 (독립 실행 가능)
- fixture는 `conftest.py`에

---

## 도구 설정

### pyproject.toml (권장 통합 설정)

```toml
[tool.black]
line-length = 88

[tool.isort]
profile = "black"

[tool.mypy]
python_version = "3.10"
warn_return_any = true
warn_unused_configs = true

[tool.pytest.ini_options]
testpaths = ["tests"]
```

### 포맷터 & 린터

| 도구 | 역할 | 명령어 |
|------|------|--------|
| black | 자동 포맷팅 | `black .` |
| isort | import 정렬 | `isort .` |
| ruff | 린팅 (flake8 대체) | `ruff check .` |
| mypy | 타입 체크 | `mypy .` |
| pytest | 테스트 | `pytest` |

---

## 안티패턴

```python
# ❌ mutable default argument
def append(item, lst=[]):       # 나쁨 — 리스트가 공유됨
    lst.append(item)
    return lst

def append(item, lst=None):    # 좋음
    if lst is None:
        lst = []
    lst.append(item)
    return lst

# ❌ global 남용
global_state = {}              # 모듈 레벨 상태 최소화, 필요하면 클래스로

# ❌ 지나친 한 줄 표현
result = [x for x in data if x > 0 and x % 2 == 0 and x < 100]  # 나쁨

# 좋음 — 가독성 우선
result = [
    x for x in data
    if x > 0
    and x % 2 == 0
    and x < 100
]

# ❌ 빈 컬렉션을 == 로 비교
if lst == []:     # 나쁨
if not lst:       # 좋음

# ❌ isinstance 대신 type 비교
if type(x) == int:    # 나쁨
if isinstance(x, int):  # 좋음
```

---

## 빠른 참조

| 항목 | 규칙 |
|------|------|
| 들여쓰기 | 4칸 스페이스 |
| 줄 길이 | 88자 |
| 네이밍 | snake_case, 클래스 PascalCase, 상수 UPPER |
| 조건문 순서 | 변수 왼쪽, 상수 오른쪽 (No Yoda) |
| None 비교 | `is None` / `is not None` |
| 빈 컬렉션 | `if not items:` (falsy 활용) |
| 범위 비교 | `0 <= i < n` (chained) |
| 따옴표 | 쌍따옴표 `""` |
| import 순서 | 표준 → 서드파티 → 내부 |
| 타입 힌트 | 공개 함수 필수 |
| docstring | Google style |
| 함수 크기 | 30줄 이내 |
| 테스트 | Arrange-Act-Assert |
| 포맷터 | black + isort |
| 린터 | ruff + mypy |
