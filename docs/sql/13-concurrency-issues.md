# 13. MiniDB 동시성 이슈 및 해결 기록

> 최종 갱신: 2026-04-22

이 문서는 현재 코드베이스에서 실제로 겪었던 동시성 문제와, 이미 해결한 항목,
그리고 아직 남아 있는 구조적 제약을 정리한다.

---

## 1. 현재 동시성 계층

MiniDB는 다음 순서의 제어 계층을 사용한다.

1. Row Lock / Range Lock
2. Engine RWLock
3. Page latch
4. Pager mutex
5. Header lock

역할 분리는 다음과 같다.

- Row/Range lock:
  - 논리적 충돌 제어
  - `id` 기반 point/range 연산 보호
- Engine RWLock:
  - DDL 독점
  - DML 공유
- Page latch:
  - B+ tree / heap 페이지의 물리적 동시 접근 제어
- Pager mutex:
  - frame 메타데이터 보호
- Header lock:
  - `next_id`, `row_count` 같은 카운터 보호

---

## 2. 해결된 문제

### 2-1. Head-of-Line Blocking

#### 증상

- 고정 크기 thread pool + keep-alive 조합에서 worker가 특정 fd에 묶였다.
- 새 요청이 큐에서 오래 대기했다.

#### 해결

- thread pool 제거
- `accept()`마다 전용 스레드 생성
- `pthread_detach`
- `MAX_CONNECTIONS=128` 제한

#### 현재 상태

- 해결됨
- 서버 모델은 thread-per-connection 기준으로 문서화해야 한다

---

### 2-2. Heap 페이지 래치 누락

#### 증상

- `heap_fetch`, `heap_insert`, `heap_delete`가 페이지 래치 없이 접근하던 시점이 있었다.
- torn read / 동시 삽입 충돌 / 슬롯 상태 레이스가 가능했다.

#### 해결

- heap 읽기 경로에 `rlatch`
- heap 쓰기 경로에 `wlatch`
- 테스트의 `pager_unpin`도 `pager_unlatch_r`로 정리

#### 현재 상태

- 해결됨

---

### 2-3. keep-alive 에러 응답 불일치

#### 증상

- keep-alive 요청에서 성공 응답과 에러 응답의 `Connection` 헤더 동작이 달랐다.

#### 해결

- `http_send_error_keepalive()` 경로를 사용하도록 서버 응답 조립을 정리했다.

#### 현재 상태

- 해결됨

---

### 2-4. scan 기반 UPDATE/DELETE의 논리적 race

#### 증상

- scan으로 대상을 모은 뒤 나중에 수정/삭제하는 2-pass 구조에서,
  대상 row 확정과 실제 쓰기 사이에 다른 스레드가 끼어들 수 있었다.

#### 해결

- scan 결과로 모은 `id`들을 정렬
- 대상 `id` 전체에 row X lock 획득
- 실제 쓰기 직전에 현재 predicate를 재검증

#### 현재 상태

- id 수집 기반 scan DML에 대해서는 해결됨

---

### 2-5. ORDER BY 전역 상태 data race

#### 증상

- `ORDER BY` 비교 컨텍스트가 전역 변수에 저장되어 동시 요청 간 덮어쓰기 가능성이 있었다.

#### 해결

- 전역 정렬 컨텍스트 제거
- 비교 함수와 로컬 정렬 경로로 치환

#### 현재 상태

- 해결됨

---

### 2-6. B+ tree internal underflow 미복구

#### 증상

- leaf delete 후 internal node가 underfull이어도 root shrink 외에는 방치되었다.

#### 해결

- internal borrow / merge / parent propagation 구현
- 높이 축소 회귀 테스트 추가

#### 현재 상태

- 해결됨

---

### 2-7. pager / lock release / heap scan hot path 병목

#### 증상

- `find_frame()` 선형 탐색
- `lock_release_all()` 전체 lock table 순회
- `heap_scan()`이 콜백 동안 page rlatch 장시간 보유

#### 해결

- `page_id -> frame_idx` 해시 인덱스
- 스레드별 보유 lock 목록 기반 해제
- page snapshot 후 latch 조기 해제

#### 현재 상태

- 해결됨

---

## 3. 남아 있는 제약

### 3-1. writer starvation

#### 설명

- 현재 lock table은 "보유 중인 lock"만 충돌 검사에 반영한다.
- 대기 중인 X 요청이 있어도 새로운 S 요청이 계속 들어올 수 있다.

#### 영향

- read-heavy 상황에서 writer가 오래 밀릴 수 있다.

#### 우선순위

- 높음

---

### 3-2. Engine RWLock 때문에 write 병렬성이 제한됨

#### 설명

- page latch는 페이지 단위 병렬성을 허용할 수 있지만,
  그 위에서 `engine_lock`이 모든 write DML을 사실상 직렬화한다.

#### 영향

- 서로 다른 페이지를 쓰는 UPDATE/DELETE/INSERT도 동시에 엔진 안으로 들어가기 어렵다.

#### 우선순위

- 높음

---

### 3-3. non-id predicate에 대한 완전한 predicate locking 부재

#### 설명

- `name='Alice'`, `age > 20` 같은 조건은 현재 scan 후 개별 row lock 방식으로만 다룬다.
- 이미 발견한 row의 수정 충돌은 줄였지만, 의미론적 phantom까지 완전히 막지는 못한다.

#### 영향

- SQL 의미론 기준의 강한 격리 수준을 기대하면 부족하다.

#### 우선순위

- 중간

---

## 4. 현재 평가

현재 구현은 교육용 프로젝트로서는 상당히 강한 편이다.

- thread-per-connection으로 서버 병목을 제거했고
- logical lock + physical latch를 분리했으며
- 실제로 sanitizer와 동시성 테스트를 통과하고 있다

다만 "쓰기 병렬성"과 "writer fairness"는 아직 구조적으로 남아 있는 숙제다.
다음 동시성 작업은 이 두 축을 중심으로 진행하는 것이 맞다.
