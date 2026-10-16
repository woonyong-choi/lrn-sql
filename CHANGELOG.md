# 변경 기록

버전별 주요 변경을 적는다. 형식: [Keep a Changelog](https://keepachangelog.com/ko/1.1.0/). 버전: [유의적 버전](https://semver.org/lang/ko/). 1.0 전에는 파일 형식과 API가 호환 없이 바뀔 수 있다.

## 미출시

### 추가

- README 머리에 ToteDB 대표 아이콘 추가

## 0.1.0 - 2026-10-16

첫 공개판이다.

### 추가

- 여러 프로세스의 같은 파일 쓰기 적용 ([0d8ed17](https://github.com/woonyong-choi/totedb/commit/0d8ed17))
- 쓰는 동안 다른 프로세스의 조회 비차단 적용 ([6e2b94c](https://github.com/woonyong-choi/totedb/commit/6e2b94c))
- Python 생산자와 C worker의 같은 큐 공유 예제 추가 ([58c1e40](https://github.com/woonyong-choi/totedb/commit/58c1e40))
- lease 연장·idempotency 키 보존·payload 상한 옵션 추가 ([b8fc5ee](https://github.com/woonyong-choi/totedb/commit/b8fc5ee))
- 페이지 회계 검사와 큰 값 장애 시험 추가 ([83fa0dc](https://github.com/woonyong-choi/totedb/commit/83fa0dc))
- 결과 열 형식·NULL 가능 정보 전달 추가 ([63df0e2](https://github.com/woonyong-choi/totedb/commit/63df0e2))
- 정수 기본 키 범위 조회 도입 ([3dbf759](https://github.com/woonyong-choi/totedb/commit/3dbf759))
- `INSERT ... ON CONFLICT DO NOTHING` / `DO UPDATE` 지원 ([8a0c813](https://github.com/woonyong-choi/totedb/commit/8a0c813))
- 바깥 열을 참조하는 서브쿼리(상관 서브쿼리) 지원 ([27bbdf1](https://github.com/woonyong-choi/totedb/commit/27bbdf1))
- RIGHT [OUTER] JOIN 지원(첫 결합) ([92dc042](https://github.com/woonyong-choi/totedb/commit/92dc042))
- 컬럼 DEFAULT와 `INSERT ... DEFAULT VALUES` 지원, 기본 키 NOT NULL ([6a2a266](https://github.com/woonyong-choi/totedb/commit/6a2a266))
- 서브쿼리(스칼라·EXISTS·IN)와 WITH, FROM 서브쿼리 지원 ([6a17465](https://github.com/woonyong-choi/totedb/commit/6a17465))
- UNION·UNION ALL·EXCEPT·INTERSECT 지원 ([c955372](https://github.com/woonyong-choi/totedb/commit/c955372))
- INSERT·UPDATE·DELETE RETURNING 지원 ([255be69](https://github.com/woonyong-choi/totedb/commit/255be69))
- INNER·LEFT·CROSS JOIN과 여러 테이블 FROM 지원 ([e905203](https://github.com/woonyong-choi/totedb/commit/e905203))
- 집계 함수와 GROUP BY·HAVING 지원 ([49962e7](https://github.com/woonyong-choi/totedb/commit/49962e7))
- 식 트리·평가기와 SELECT 정렬·제한·중복 제거·별칭 추가 ([eb03dc5](https://github.com/woonyong-choi/totedb/commit/eb03dc5))
- 빈 페이지 목록을 영속하고 새 페이지를 즉시 재사용 ([ed52fc8](https://github.com/woonyong-choi/totedb/commit/ed52fc8))
- 일반 컬럼 B+Tree 색인과 고유 제약 추가 ([efea9f0](https://github.com/woonyong-choi/totedb/commit/efea9f0))
- ACK 큐 Python 어댑터와 공유 라이브러리 빌드 추가 ([164bef8](https://github.com/woonyong-choi/totedb/commit/164bef8))

### 변경

- 행 인코딩을 varint 코드 형식으로 변경 ([a932fa7](https://github.com/woonyong-choi/totedb/commit/a932fa7))
- 행 하나를 꺼낼 때 그 slot의 경계만 검사하도록 개선 ([6790be5](https://github.com/woonyong-choi/totedb/commit/6790be5))
- 기본 키 순서 보조 색인과 색인 순서 실행 도입 ([e18b89f](https://github.com/woonyong-choi/totedb/commit/e18b89f))
- GROUP BY·집계를 해시 집계로 변경 ([806f5c4](https://github.com/woonyong-choi/totedb/commit/806f5c4))
- commit을 fsync 한 번으로 줄이고 끝나지 않은 commit은 열 때 되돌림 ([b0d2234](https://github.com/woonyong-choi/totedb/commit/b0d2234))
- 페이지·overflow 체크섬을 하드웨어 CRC32C로 교체 ([fce7efd](https://github.com/woonyong-choi/totedb/commit/fce7efd))
- page_file에 검증된 페이지 읽기 캐시 추가 ([d039421](https://github.com/woonyong-choi/totedb/commit/d039421))

### 제거

- 옛 엔진과 REPL·HTTP 서버 제거 ([5e6b265](https://github.com/woonyong-choi/totedb/commit/5e6b265))
- 이전 형식 호환 코드 제거 ([c49bb6a](https://github.com/woonyong-choi/totedb/commit/c49bb6a))

### 수정

- 큰 id를 넣은 뒤 자동 id가 오류로 끝나는 문제 수정 ([87e376f](https://github.com/woonyong-choi/totedb/commit/87e376f))
- 표 페이지 목록을 모을 때 목록 끝 밖에 쓰는 문제 수정 ([b40cc06](https://github.com/woonyong-choi/totedb/commit/b40cc06))
- 함수·CAST 결과를 SQLite·PostgreSQL 기준으로 수정 ([216e95c](https://github.com/woonyong-choi/totedb/commit/216e95c))
- 보조 색인 순회의 LIMIT 멈춤 처리 수정 ([7b7a839](https://github.com/woonyong-choi/totedb/commit/7b7a839))
