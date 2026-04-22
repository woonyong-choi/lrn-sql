# 14. MiniDB 최신 코드 감사 보고서

> 감사일: 2026-04-22
>
> 대상:
> `pager.c`, `table.c`, `bptree.c`, `executor.c`, `lock_table.c`,
> `server.c`, `http.c`, `db.c`, `parser.c`, `planner.c`

---

## 1. 감사 범위

이번 감사는 다음 관점으로 진행했다.

- 동시성 정확성
- 데이터 정합성
- 메모리 안전성
- 테스트 커버리지
- hot path 성능 병목

추가 확인:

- stricter compile check
  - `-Wpedantic`
  - `-Wshadow`
  - `-Wconversion`
  - `-Wsign-conversion`
- sanitizer 기반 테스트 스위트

---

## 2. 해결 완료된 주요 항목

### 2-1. `UPDATE ... SET id = ...` 차단

- 시스템 컬럼 `id`는 더 이상 수정할 수 없다.
- 인덱스와 힙의 불일치 위험을 제거했다.

### 2-2. scan 기반 DML의 대상 row 정합성 보강

- 대상 `id`를 모은 뒤 row X lock 획득
- 실제 쓰기 직전 predicate 재검증
- 부분 성공/중간 개입 가능성을 줄였다

### 2-3. ORDER BY 전역 상태 제거

- 동시 정렬 요청 간 data race를 제거했다.

### 2-4. B+ tree internal underflow 복구

- delete-heavy 상황에서도 트리 높이 축소가 가능해졌다.

### 2-5. 출력/수집 경로 메모리 처리 보강

- `buf_append()` / row collect 확장 경로에서 재할당 실패를 더 안전하게 다룬다.

### 2-6. pager 초기화와 hot path 개선

- `find_frame()` 해시화
- reopen 경로 안전성 정리
- `heap_scan()` latch 보유 시간 단축
- `lock_release_all()` 보유 lock 목록화

### 2-7. dead code / 경고 정리

- 사용되지 않던 `SLOT_DEAD` 제거
- stricter compile check 기준 sign conversion 정리

---

## 3. 현재 활성 리스크

### P1. writer starvation

#### 설명

- 대기 중인 X 요청이 있어도 새로운 S 요청이 계속 들어올 수 있다.
- 현재 lock table은 wait queue 기반 fairness를 구현하지 않는다.

#### 영향

- read-heavy 부하에서 writer가 장시간 밀릴 수 있다.

#### 권장 방향

- 대기 writer 존재 시 후속 shared 요청을 막는 정책
- 또는 명시적 wait queue / ticket 기반 공정성 도입

---

### P1. `engine_lock`이 write 병렬성을 막음

#### 설명

- page latch와 row lock이 더 세밀한 병렬성을 허용할 수 있어도,
  write DML은 결국 상단의 engine rwlock에서 크게 묶인다.

#### 영향

- 서로 다른 페이지를 쓰는 작업도 쉽게 직렬화된다.

#### 권장 방향

- DDL과 DML schema mutation만 분리
- write DML에 대한 더 세밀한 진입 정책 재설계

---

### P2. HTTP 입력 검증이 충분히 엄격하지 않음

#### 설명

- `http.c`는 `Content-Length`를 `atoi()`로 해석한다.
- 음수/이상치/헤더 조합 오류에 대한 방어가 강하지 않다.

#### 영향

- malformed input에 대해 견고성이 떨어질 수 있다.

#### 권장 방향

- `strtoul` 사용
- 0 미만/상한 초과/중복 헤더 검증
- malformed request 테스트 추가

---

### P2. 명시적 스키마 제약 옵션 부재

#### 설명

- 현재 `id`는 암묵적 시스템 컬럼이자 사실상 PK 역할을 한다.
- 하지만 `PRIMARY KEY`, `UNIQUE`, `NOT NULL` 같은 문법은 아직 없다.

#### 영향

- SQL 사용자 기대치와 문법 표현력이 제한된다.

#### 권장 방향

- `PRIMARY KEY` 문법 도입
- 현재 `id` 자동 생성 정책과의 규칙 명확화

---

### P2. 단일 테이블 전제

#### 설명

- 헤더와 실행기 전반이 단일 테이블을 전제로 설계되어 있다.

#### 영향

- 스키마 확장성과 SQL 현실성이 제한된다.

#### 권장 방향

- 테이블 카탈로그 도입
- table별 root/heap/schema 분리

---

## 4. 감사 결과 요약

현재 코드는 다음 기준에서는 꽤 안정적이다.

- 기본 CRUD
- SQL 확장 기능
- id 기반 인덱스 경로
- scan 기반 DML 보정
- sanitizer 회귀 테스트
- stricter compile check

반대로 다음은 아직 "프로덕션 수준"으로 보기 어렵다.

- writer fairness
- write 병렬성
- malformed HTTP 방어
- 명시적 스키마 제약
- 다중 테이블

---

## 5. 최신 검증 기록

2026-04-22 기준 확인한 내용:

- `test_all`: 76/76
- `test_step0`: 24/24
- `test_step1`: 72/72
- `test_step2`: 44/44
- stricter compile check: 통과

즉, 현재 코드는 "교육용 엔진으로서 강한 완성도"를 갖고 있지만,
다음 단계는 기능 추가보다 공정성·표현력·입력 검증 쪽이 더 중요하다.
