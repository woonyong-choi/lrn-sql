# 11. MiniDB 구현 현황 및 다음 로드맵

> 최종 갱신: 2026-04-22
>
> 이 문서는 원래 "구현 계획서"였지만, 현재는 실제 구현 결과와 남은 백로그를
> 함께 기록하는 상태 문서로 유지한다.

---

## 1. 목표 대비 현재 상태

| 영역 | 목표 | 현재 상태 |
|------|------|-----------|
| 디스크 기반 저장 | 힙 + 인덱스 + pager | 완료 |
| SQL 실행 경계 | `db_execute()` + 버퍼 기반 출력 | 완료 |
| SQL 확장 | UPDATE/DELETE/COUNT/ORDER BY/LIMIT/EXPLAIN | 완료 |
| 서버 모드 | HTTP API 서버 | 완료 |
| 동시성 제어 | Row lock + latch + 엔진 락 | 완료 |
| keep-alive 대응 | 연결 재사용 지원 | 완료 |
| HoL blocking 완화 | 스레드 풀 병목 제거 | 완료 |
| 내부 정합성 보강 | id 수정 차단, scan DML 재검증 | 완료 |
| 성능 보강 | pager 조회/lock 해제/scan 경합 감소 | 완료 |
| 스키마 옵션 | 명시적 PK/제약 옵션 | 현재 미지원 |
| 다중 테이블 | 2개 이상 테이블 동시 관리 | 현재 미지원 |
| 2차 인덱스 | `name`, `age` 등 인덱스 | 현재 미지원 |

---

## 2. 현재 구현된 아키텍처

### 2-1. 저장 계층

- `pager.c`
  - 페이지 캐시
  - dirty flush
  - free page 재활용
  - `page_id -> frame_idx` 해시 인덱스
- `table.c`
  - 슬롯 기반 힙 테이블
  - tombstone/free slot 재사용
  - scan 시 page snapshot 후 latch 조기 해제
- `bptree.c`
  - `id` 인덱스용 B+ tree
  - leaf/internal split
  - delete 후 internal underflow 복구
- `schema.c`
  - row serialize / deserialize
  - column offset 계산

### 2-2. SQL 계층

- `parser.c`
  - `CREATE TABLE`, `INSERT`, `SELECT`, `DELETE`, `UPDATE`, `DROP TABLE`, `EXPLAIN`
  - `WHERE`
  - `COUNT(*)`
  - `ORDER BY`
  - `LIMIT`
- `planner.c`
  - 규칙 기반 access path 선택
  - `INDEX_LOOKUP`, `INDEX_DELETE`, `INDEX_UPDATE`, `TABLE_SCAN`
- `executor.c`
  - 버퍼 기반 결과 반환
  - `COUNT(*)` fast path
  - `ORDER BY LIMIT k` top-k 최적화
  - `id` 시스템 컬럼 수정 차단
  - scan 기반 `UPDATE`/`DELETE`의 row lock + 재검증

### 2-3. 동시성 계층

- Level 4: Row Lock / Range Lock
- Level 3: Engine RWLock
- Level 2: Page latch
- Level 1: Pager mutex
- 별도: `header_lock`

### 2-4. 서버 계층

- `server.c`
  - thread-per-connection
  - `MAX_CONNECTIONS=128`
  - graceful shutdown
  - `/stats` 제공
- `http.c`
  - `POST /query`
  - `GET /stats`
  - keep-alive 헤더 처리

---

## 3. 현재 사용자 관점 기능

### 3-1. 지원되는 SQL 예시

```sql
CREATE TABLE users (name VARCHAR(32), age INT);
INSERT INTO users VALUES ('Alice', 25);
SELECT * FROM users WHERE id = 1;
SELECT * FROM users WHERE age >= 20 ORDER BY age DESC LIMIT 10;
SELECT COUNT(*) FROM users WHERE age > 25;
UPDATE users SET age = 30 WHERE name = 'Alice';
DELETE FROM users WHERE id = 1;
DROP TABLE users;
EXPLAIN SELECT * FROM users WHERE id = 1;
```

### 3-2. 현재 제약

- 단일 테이블만 지원한다.
- `id`는 자동 생성되는 시스템 컬럼이다.
- `id`는 수정할 수 없다.
- 명시적 `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, 복합 키 문법은 아직 지원하지 않는다.
- `id` 외 컬럼에는 2차 인덱스가 없다.

---

## 4. 최근에 반영된 중요 보강

- thread pool 제거, thread-per-connection 전환
- heap 전 경로 latch 적용
- keep-alive 에러 응답 헤더 정합성 수정
- `UPDATE ... SET id = ...` 차단
- scan 기반 `UPDATE`/`DELETE`에 대상 row lock 보강
- `ORDER BY` 전역 상태 제거
- B+ tree internal underflow 복구
- `pager.find_frame()` 해시화
- `lock_release_all()` 보유 lock 목록화
- `heap_scan()`의 page latch 보유 시간 단축

---

## 5. 다음 우선순위 백로그

### 5-1. 스키마 기능

1. `PRIMARY KEY` 문법 지원
2. `id` 자동 생성 정책과 명시적 PK 규칙 정리
3. `NOT NULL` / `UNIQUE` 같은 제약 추가 여부 결정

### 5-2. 동시성/성능

1. writer starvation 방지
2. 전역 `engine_lock` 때문에 막히는 write 병렬성 완화
3. non-id predicate에 대한 더 강한 predicate locking 설계

### 5-3. 엔진 기능

1. 다중 테이블
2. 2차 인덱스
3. 더 엄격한 HTTP 입력 검증
4. WAL / recovery / MVCC는 아직 범위 밖

---

## 6. 결론

현재 코드는 "교육용 MiniDB" 수준을 넘어, 다음을 모두 실제로 갖춘 상태다.

- 디스크 기반 저장 엔진
- 규칙 기반 SQL 실행기
- HTTP API 서버
- 다층 동시성 제어
- sanitizer 기반 테스트 스위트

다만 스키마 옵션과 다중 테이블, 2차 인덱스는 아직 설계 단계가 아니라
"현재 미지원" 상태이므로, 이후 작업은 그 축으로 확장하는 것이 맞다.
