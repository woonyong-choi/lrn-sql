# 12. 테스트 하네스 현황 및 운영 가이드

> 최종 갱신: 2026-04-22

이 문서는 초기에 "테스트 하네스 계획서"였지만, 현재는 실제 테스트 스위트의
구성과 실행 방법을 기준으로 유지한다.

---

## 1. 현재 테스트 스위트 구성

### 1-1. 회귀 테스트

- `tests/test_all.c`
  - pager
  - schema
  - heap
  - B+ tree
  - parser
  - planner
  - persistence
  - delete/reuse

### 1-2. Step 0

- `tests/test_step0_db_execute.c`
  - `db_execute()` 경계 함수
  - `out_buf` 반환
  - 에러 처리
  - 영속성

### 1-3. Step 1

- `tests/test_step1_sql_ext.c`
  - `UPDATE`
  - 비교 연산자
  - `COUNT(*)`
  - `ORDER BY`
  - `LIMIT`
  - `ORDER BY + LIMIT`
  - `DROP TABLE`
  - `EXPLAIN`
  - 시스템 컬럼 `id` 수정 거부
  - `LIMIT 0`

### 1-4. Step 2

- `tests/test_step2_concurrency.c`
  - Row Lock S/X 호환성
  - timeout
  - `release_all`
  - 동시 INSERT
  - 동시 read/write
  - gap conflict
  - scan DELETE lock conflict
  - keep-alive 에러 응답 헤더

---

## 2. 최신 검증 결과

2026-04-22 기준 최신 실행 결과:

| 타깃 | 결과 |
|------|------|
| `test_all` | 76 / 76 통과 |
| `test_step0` | 24 / 24 통과 |
| `test_step1` | 72 / 72 통과 |
| `test_step2` | 44 / 44 통과 |

주의:

- 테스트 수는 이후 추가될 수 있으므로 이 숫자는 "기준 시점 스냅샷"이다.
- 문서 갱신 시에는 최근 실행 결과로 숫자를 같이 업데이트한다.

---

## 3. 권장 실행 방법

### 3-1. 기본

```bash
make test
make test-step0
make test-step1
make test-step2
make test-all
```

### 3-2. macOS 또는 별도 출력 디렉터리 권장

환경이 섞여 있을 때는 `BUILD_DIR`를 분리하는 편이 안전하다.

```bash
make -j4 BUILD_DIR=build_mac test-all
```

이유:

- 서로 다른 아키텍처/도구체인 산출물이 같은 `build/`에 섞이면 링크가 깨질 수 있다.
- 문서 갱신이나 대규모 리팩터링 직후에는 클린 빌드를 권장한다.

### 3-3. stricter compile check

코드 감사 시에는 아래 명령도 유용하다.

```bash
gcc -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
    -Iinclude -fsyntax-only \
    src/storage/pager.c src/storage/table.c src/server/lock_table.c \
    src/sql/parser.c src/sql/executor.c src/db.c
```

---

## 4. 현재 테스트가 잘 잡는 것

- basic CRUD 정합성
- row serialize / deserialize
- B+ tree split / delete / shrink
- `db_execute()` 반환 계약
- `id` 수정 금지
- scan 기반 DML lock timeout
- keep-alive 응답 헤더 일관성
- sanitizer 기반 메모리 오류

---

## 5. 아직 약한 부분

### 5-1. 서버 통합 테스트

- 실제 다중 클라이언트 HTTP keep-alive 장시간 테스트는 제한적이다.
- `/stats` JSON 구조 자체를 깊게 검증하지는 않는다.

### 5-2. 성능 회귀 테스트

- 벤치마크 도구는 있지만, PR 차원의 자동 성능 기준은 없다.
- `pager` 해시화와 `heap_scan` 개선은 현재 기능 회귀 중심으로만 검증한다.

### 5-3. malformed HTTP 입력

- `Content-Length` 이상값, 잘못된 헤더 조합에 대한 방어 테스트는 충분하지 않다.

### 5-4. predicate locking

- non-id predicate에 대한 강한 의미론적 동시성은 아직 테스트로 완전하게 모델링하지 않는다.

---

## 6. 새 테스트를 추가할 때의 원칙

1. 기능 테스트와 동시성 테스트를 분리한다.
2. 에러 메시지를 검사할 때는 핵심 문구만 본다.
3. lock timeout 계열 테스트는 시간 의존성을 최소화한다.
4. 새로운 버그를 고쳤다면 회귀 테스트를 먼저 추가한다.
5. 성능 개선은 "기능 회귀 없음"을 먼저 증명하고 벤치마크는 보조로 둔다.

---

## 7. 권장 운영 순서

기능 변경 후 권장 순서:

1. `make -j4 BUILD_DIR=build_mac test-step0`
2. `make -j4 BUILD_DIR=build_mac test-step1`
3. `make -j4 BUILD_DIR=build_mac test-step2`
4. `make -j4 BUILD_DIR=build_mac test`
5. 필요 시 stricter compile check

문서 변경만 있을 때도, 문서가 설명하는 상태가 실제와 맞는지 확인하려면
최소한 `test-all` 한 번은 최신 결과로 남겨두는 것이 좋다.
