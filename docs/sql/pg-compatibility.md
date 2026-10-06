# PostgreSQL 18 호환성 검사

MiniDB는 자체 저장 엔진을 쓰는 단일 테이블 SQL 부분집합이다. 호환성은
PostgreSQL 18에서 같은 데이터로 실행한 성공·실패와 결과 행이 일치할 때만
확인한다. 이 문서는 PostgreSQL 전체 명령 지원을 뜻하지 않는다.

## 현재 검사 범위

| 기능 | 현재 상태 | 검사 |
|---|---|---|
| `CREATE TABLE`의 `INT`·`BIGINT`·`VARCHAR` | 첫 컬럼의 `id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY` 지원 | 동일 DDL로 스키마 준비 |
| `INSERT INTO ... (컬럼 목록) VALUES` | `id` 외 모든 사용자 컬럼을 지정하는 형태 지원 | 동일 SQL로 같은 행 삽입 |
| `SELECT *`·`COUNT(*)`·단일 `WHERE`·`ORDER BY`·`LIMIT` | 제한된 형태 지원 | 동일 SQL의 결과 행 비교 |
| `UPDATE`·`DELETE`·`DROP TABLE` | 제한된 형태 지원 | 동일 SQL의 성공·실패와 후속 조회 비교 |
| 잘못된 숫자 값 | 변경 전에 거절 | 양쪽의 실패 여부와 후속 조회 비교 |
| 없는 테이블 이름 | SQL 실행 전 거절 | INSERT·SELECT·UPDATE·DELETE·DROP의 오류와 후속 조회 비교 |

별도 SQL이 필요한 사례가 생기면
[`tests/pg_compat_cases.json`](../../tests/pg_compat_cases.json)에 이유를 기록하고
**문법 호환 통과로 세지 않는다**. 현재 fixture 20건은 모두 같은 SQL이다.
성공·실패와
결과 행만 비교하며, 오류 코드·메시지, 반환 컬럼 타입, 잠금·트랜잭션,
재시작 후 상태는 아직 판정하지 않는다. 문자열 `|`나 줄바꿈이 포함된
표 출력도 현재 하니스가 해석하지 못한다. 기존 `INSERT INTO ... VALUES`와
암묵적 `id` 생성은 레거시 문법으로 유지한다. PostgreSQL 호환 예시에서는
명시적 identity 선언과 INSERT 컬럼 목록을 사용한다.

## 실행

PostgreSQL 18 접속 문자열을 지정한다. 하니스는 임의 이름의 격리 스키마를
생성하고 검사 뒤 그 스키마만 삭제한다.

```sh
make SANITIZE= BUILD_DIR=build-pg build-pg/sql_probe
PGCOMPAT_DSN='postgresql://USER:PASSWORD@localhost:5432/TEST_DB' \
  python3 tests/pg_compat.py --probe ./build-pg/sql_probe --require-postgres
```

`make test-pg-compat`는 PostgreSQL이 없으면 이유를 표시하고 건너뛴다.
CI의 `pg-compat` 작업은 PostgreSQL 18 서비스를 띄우고
`--require-postgres`로 실행하므로 건너뛰기를 성공으로 처리하지 않는다.
