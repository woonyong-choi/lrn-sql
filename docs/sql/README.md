# SQL Docs

> 최종 갱신: 2026-04-22

`docs/sql`은 이 저장소의 MiniDB 구현 상태를 따라가는 운영 문서 모음이다.
초기에는 설계·계획 문서로 시작했지만, 현재는 실제 코드 상태를 반영하는
"현재형 문서"로 유지한다.

## 현재 구현 요약

- 저장 엔진: 디스크 기반 단일 테이블 MiniDB
- 저장 구조: 힙 테이블 + B+ tree(id 인덱스) + pager 캐시
- SQL 기능:
  - `CREATE TABLE`
  - `INSERT`
  - `SELECT`
  - `DELETE`
  - `UPDATE`
  - `DROP TABLE`
  - `EXPLAIN`
  - `WHERE (=, !=, <, >, <=, >=)`
  - `COUNT(*)`
  - `ORDER BY`
  - `LIMIT`
- 시스템 컬럼: `id BIGINT` 자동 생성, 자동 증가, 수정 금지
- 동시성:
  - Row Lock / Range Lock
  - Engine RWLock
  - Page latch
  - Pager mutex
  - Header lock
- 서버:
  - HTTP/1.1 최소 서브셋
  - `POST /query`
  - `GET /stats`
  - thread-per-connection
  - keep-alive 지원
- 검증:
  - `test_all`: 76/76
  - `test_step0`: 24/24
  - `test_step1`: 72/72
  - `test_step2`: 44/44

## 문서 목록

- `11-implementation-plan.md`
  - 초기 구현 계획을 현재 구현 현황과 다음 백로그 중심으로 재정리한 문서
- `12-test-harness-plan.md`
  - 실제 테스트 스위트 구성, 실행 방법, 현재 커버리지와 공백을 정리한 문서
- `13-concurrency-issues.md`
  - 동시성 문제의 원인, 해결 내역, 남은 제약을 정리한 기록 문서
- `14-code-audit.md`
  - 최신 코드 감사 결과와 현재 남아 있는 리스크를 요약한 문서

## 이 문서를 읽는 순서

1. `README.md`
2. `11-implementation-plan.md`
3. `13-concurrency-issues.md`
4. `12-test-harness-plan.md`
5. `14-code-audit.md`

## 문서 유지 원칙

- 계획이 바뀌면 계획만 고치지 않고 현재 구현 상태도 같이 적는다.
- 코드에서 바뀐 내용이 문서와 충돌하면 문서를 코드에 맞춘다.
- 해결된 이슈는 "삭제"보다 "해결됨"으로 남겨 추적성을 유지한다.
- 아직 구현되지 않은 기능은 "지원 예정"이 아니라 "현재 미지원"으로 쓴다.
