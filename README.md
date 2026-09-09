# lrn-sql

SQL 한 문장이 파싱과 실행 계획을 거쳐 디스크의 행을 읽고 쓰는 과정을 구현한 C11 데이터베이스다. 단일 파일에 테이블과 B+Tree 인덱스를 저장하며, REPL에서 쿼리 결과와 페이지 접근 경로를 함께 확인할 수 있다.

크래프톤 정글 12기 팀 프로젝트 [Jungle-12-303/wk08_1](https://github.com/Jungle-12-303/wk08_1)을 보존하고 이어서 작업하는 개인 미러다. 아래 기능은 팀 저장소에서 이어진 구현을 포함하며, 개인 기여의 범위는 커밋 저자와 파일별 변경 이력으로 확인할 수 있다.

## 실행

GCC, Make, pthread가 필요하다. Linux 환경은 저장소의 Dev Container 설정을 사용할 수 있다.

```sh
make
./build/minidb demo.db
```

새 DB를 열었다면 REPL에 다음 내용을 한 줄씩 입력한다.

```sql
CREATE TABLE users (name VARCHAR(32), age INT)
INSERT INTO users VALUES ('Alice', 25)
SELECT * FROM users WHERE id = 1
EXPLAIN SELECT * FROM users WHERE id = 1
```

이 입력은 다음 행을 조회하고 실행 계획에 `INDEX_LOOKUP`을 표시한다. 자동 생성된 `id`의 B+Tree를 사용한 결과다.

```text
1 | Alice | 25
1행 조회 (INDEX_LOOKUP)
```

`.stats`는 행 수와 페이지 크기, 트리 높이를 보여 준다. `.debug`를 켜면 쿼리마다 페이지 로드·캐시 hit·miss가 출력되고, `.btree`로 인덱스 구조를 볼 수 있다. `.exit` 또는 Ctrl-D로 종료하면 dirty 페이지를 파일에 기록한다.

기본 빌드는 ASAN·UBSAN을 사용한다. macOS에서 sanitizer 초기화가 멈추는 경우에는 별도 빌드 디렉터리에 sanitizer를 제외한 실행 파일을 만들 수 있다.

```sh
make BUILD_DIR=build-nosan SANITIZE= all
./build-nosan/minidb demo.db
```

이 경로는 기능을 확인하기 위한 것으로, sanitizer 검사를 통과했다는 뜻은 아니다.

## 저장 구조와 실행 계획

**B+Tree와 규칙 기반 planner.** `id = 값`은 인덱스 점 조회로, 지원하는 `id` 범위 조건은 연결된 리프를 순회하는 경로로 보낸다. 그 밖의 조건은 heap scan으로 처리한다. 인덱스가 있다는 사실보다 어떤 SQL이 실제로 그 경로를 사용하는지가 중요하므로 `EXPLAIN`과 페이지 통계를 함께 제공한다.

**Slotted heap page.** 페이지 앞쪽의 slot과 뒤쪽의 행 데이터를 분리한다. 삭제한 slot은 다음 삽입에 재사용하며, 현재 행 데이터는 schema의 고정 `row_size`로 직렬화한다. 가변 길이 행 저장까지 구현한 것은 아니다.

**Pager와 동시성 제어.** 256개 frame에 page ID, pin count, dirty bit와 LRU 순서를 관리한다. 사용 중인 페이지를 교체하지 않고, 변경 페이지의 기록 시점을 저장 계층에 모았다. 페이지 latch와 행·범위 lock도 사용하지만, lock은 문장 종료 시 해제되는 autocommit 경로다.

| 코드 | 역할 |
| --- | --- |
| [main.c](src/main.c), [db.c](src/db.c) | REPL·HTTP 서버 진입과 SQL 요청 처리 |
| [planner.c](src/sql/planner.c) | 점 조회·범위 조회·테이블 스캔 선택 |
| [bptree.c](src/storage/bptree.c) | 인덱스 탐색·분할·삭제·리프 순회 |
| [table.c](src/storage/table.c) | 행 직렬화와 slot 재사용 |
| [pager.c](src/storage/pager.c) | 페이지 캐시와 디스크 I/O |
| [lock_table.c](src/server/lock_table.c) | 행·범위 lock 획득과 해제 |

## 검증

```sh
make test-all
```

저장 구조, SQL 실행과 확장 문법, 동시 요청을 검사한다. [2026-09-08 CI](https://github.com/woonyong-kr/lrn-sql/actions/runs/34203368247)에서는 이 README 변경 전의 코드로 Ubuntu에서 ASAN·UBSAN을 켠 검증이 통과했다. 새 실행 상태는 [Actions](https://github.com/woonyong-kr/lrn-sql/actions)에서 확인할 수 있다.

범위 조회가 heap scan으로 처리되던 문제와 INSERT 경로의 반복 작업을 줄인 과정은 [PostgreSQL 대조 실험](docs/benchmark-postgres.md)에 남아 있다. 인터페이스와 내구성 조건이 다르므로 문서의 처리량을 PostgreSQL과 동등한 조건의 성능으로 읽으면 안 된다.

## 지원 범위

단일 테이블과 자동 생성 `id` 인덱스를 중심으로 CREATE·INSERT·SELECT·UPDATE·DELETE·DROP과 일부 조건·정렬·집계를 구현했다. 일반화된 schema constraint, secondary index, 비용 기반 optimizer, 여러 문장을 묶는 트랜잭션과 WAL 기반 crash recovery는 없다. 정상 종료 후 파일을 다시 여는 것과 갑작스러운 중단에서 데이터를 복구하는 것은 다른 범위다.

과제 단계와 설계 자료는 [문서 목차](docs/README.md)에 있다.
