# 변경 기록

버전별 주요 변경을 적는다. 형식: [Keep a Changelog](https://keepachangelog.com/ko/1.1.0/). 버전: [유의적 버전](https://semver.org/lang/ko/). 1.0 전에는 파일 형식과 API가 호환 없이 바뀔 수 있다.

## 0.1.0 - 2026-10-16

첫 공개판이다.

### 추가

- 여러 프로세스의 같은 파일 쓰기 적용 ([528a006](https://github.com/woonyong-choi/totedb/commit/528a006))
- 쓰는 동안 다른 프로세스의 조회 비차단 적용 ([8a117e6](https://github.com/woonyong-choi/totedb/commit/8a117e6))
- Python 생산자와 C worker의 같은 큐 공유 예제 추가 ([4ce3b75](https://github.com/woonyong-choi/totedb/commit/4ce3b75))
- lease 연장·idempotency 키 보존·payload 상한 옵션 추가 ([ef2d963](https://github.com/woonyong-choi/totedb/commit/ef2d963))
- 페이지 회계 검사와 큰 값 장애 시험 추가 ([3b20661](https://github.com/woonyong-choi/totedb/commit/3b20661))
- 결과 열 형식·NULL 가능 정보 전달 추가 ([e74c43c](https://github.com/woonyong-choi/totedb/commit/e74c43c))
- 정수 기본 키 범위 조회 도입 ([28b9847](https://github.com/woonyong-choi/totedb/commit/28b9847))
- `INSERT ... ON CONFLICT DO NOTHING` / `DO UPDATE` 지원 ([698ab22](https://github.com/woonyong-choi/totedb/commit/698ab22))
- 바깥 열을 참조하는 서브쿼리(상관 서브쿼리) 지원 ([22ddf90](https://github.com/woonyong-choi/totedb/commit/22ddf90))
- RIGHT [OUTER] JOIN 지원(첫 결합) ([bd87d30](https://github.com/woonyong-choi/totedb/commit/bd87d30))
- 컬럼 DEFAULT와 `INSERT ... DEFAULT VALUES` 지원, 기본 키 NOT NULL ([1fb0423](https://github.com/woonyong-choi/totedb/commit/1fb0423))
- 서브쿼리(스칼라·EXISTS·IN)와 WITH, FROM 서브쿼리 지원 ([24dd713](https://github.com/woonyong-choi/totedb/commit/24dd713))
- UNION·UNION ALL·EXCEPT·INTERSECT 지원 ([9cc1576](https://github.com/woonyong-choi/totedb/commit/9cc1576))
- INSERT·UPDATE·DELETE RETURNING 지원 ([1cad429](https://github.com/woonyong-choi/totedb/commit/1cad429))
- INNER·LEFT·CROSS JOIN과 여러 테이블 FROM 지원 ([3c7b989](https://github.com/woonyong-choi/totedb/commit/3c7b989))
- 집계 함수와 GROUP BY·HAVING 지원 ([4e55984](https://github.com/woonyong-choi/totedb/commit/4e55984))
- 식 트리·평가기와 SELECT 정렬·제한·중복 제거·별칭 추가 ([5207f34](https://github.com/woonyong-choi/totedb/commit/5207f34))
- 빈 페이지 목록을 영속하고 새 페이지를 즉시 재사용 ([d75eef6](https://github.com/woonyong-choi/totedb/commit/d75eef6))
- 일반 컬럼 B+Tree 색인과 고유 제약 추가 ([4443706](https://github.com/woonyong-choi/totedb/commit/4443706))
- ACK 큐 Python 어댑터와 공유 라이브러리 빌드 추가 ([c38e5cd](https://github.com/woonyong-choi/totedb/commit/c38e5cd))

### 변경

- 행 인코딩을 varint 코드 형식으로 변경 ([ca19edc](https://github.com/woonyong-choi/totedb/commit/ca19edc))
- 행 하나를 꺼낼 때 그 slot의 경계만 검사하도록 개선 ([e788b9f](https://github.com/woonyong-choi/totedb/commit/e788b9f))
- 기본 키 순서 보조 색인과 색인 순서 실행 도입 ([514d86e](https://github.com/woonyong-choi/totedb/commit/514d86e))
- GROUP BY·집계를 해시 집계로 변경 ([d75101a](https://github.com/woonyong-choi/totedb/commit/d75101a))
- commit을 fsync 한 번으로 줄이고 끝나지 않은 commit은 열 때 되돌림 ([6638c63](https://github.com/woonyong-choi/totedb/commit/6638c63))
- 페이지·overflow 체크섬을 하드웨어 CRC32C로 교체 ([7f840db](https://github.com/woonyong-choi/totedb/commit/7f840db))
- page_file에 검증된 페이지 읽기 캐시 추가 ([7622aa0](https://github.com/woonyong-choi/totedb/commit/7622aa0))

### 제거

- 옛 엔진과 REPL·HTTP 서버 제거 ([7af03cf](https://github.com/woonyong-choi/totedb/commit/7af03cf))
- 이전 형식 호환 코드 제거 ([f4db9c3](https://github.com/woonyong-choi/totedb/commit/f4db9c3))

### 수정

- 큰 id를 넣은 뒤 자동 id가 오류로 끝나는 문제 수정 ([4d0945c](https://github.com/woonyong-choi/totedb/commit/4d0945c))
- 표 페이지 목록을 모을 때 목록 끝 밖에 쓰는 문제 수정 ([3734de7](https://github.com/woonyong-choi/totedb/commit/3734de7))
- 함수·CAST 결과를 SQLite·PostgreSQL 기준으로 수정 ([f0699c3](https://github.com/woonyong-choi/totedb/commit/f0699c3))
- 보조 색인 순회의 LIMIT 멈춤 처리 수정 ([b41f74f](https://github.com/woonyong-choi/totedb/commit/b41f74f))
