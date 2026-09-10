# 🗃️ lrn-sql

SQL 한 문장을 파싱하고 실행 계획을 세워 디스크의 행을 읽고 쓰는 C11 데이터베이스입니다. Slotted Page, B+Tree, Buffer Pool과 행·범위 lock을 직접 구현합니다.

[SQL 엔진 구현 Wiki](https://docs.woonyong.com/wiki/lrn-sql/) · [설계 문서](docs/README.md) · [CI](https://github.com/woonyong-kr/lrn-sql/actions)

크래프톤 정글 팀 과제에서 시작해 **최우녕이 개인 주도로 대부분 직접 구현**했습니다. [팀 원본](https://github.com/Jungle-12-303/wk08_1)과 이 저장소에 개발 이력이 남아 있으며, 기반 과제·팀 기여와 이후 변경은 커밋 저자와 diff로 구분합니다.

## 실행

GCC, Make, pthread가 필요합니다. Linux는 저장소의 Dev Container 설정을 사용할 수 있습니다.

```sh
make
./build/minidb demo.db
```

새 DB의 REPL에서 다음 내용을 한 줄씩 입력합니다.

```sql
CREATE TABLE users (name VARCHAR(32), age INT)
INSERT INTO users VALUES ('Alice', 25)
SELECT * FROM users WHERE id = 1
EXPLAIN SELECT * FROM users WHERE id = 1
```

`1 | Alice | 25`를 조회하고 `INDEX_LOOKUP` 계획을 확인합니다. `.stats`는 페이지·트리 통계, `.debug`는 쿼리별 페이지 접근, `.btree`는 인덱스 구조를 보여 줍니다. `.exit` 또는 Ctrl-D로 종료하면 dirty 페이지를 기록합니다.

```sh
make test-all
# macOS에서 sanitizer 초기화 문제가 있다면 별도 빌드
make BUILD_DIR=build-nosan SANITIZE= all
./build-nosan/minidb demo.db
```

기본 빌드는 ASAN·UBSAN을 사용합니다. sanitizer를 제외한 빌드는 해당 검사를 대신하지 않습니다.

## 구현과 설계

SQL → parser·planner → index lookup/range scan/heap scan → pager → 디스크로 이어집니다.

| 코드 | 설계 선택 |
| --- | --- |
| [planner.c](src/sql/planner.c) | `id` 점·범위 조건은 B+Tree, 그 밖은 heap scan으로 선택 |
| [bptree.c](src/storage/bptree.c) | 분할·삭제와 연결된 리프의 범위 순회 |
| [table.c](src/storage/table.c) | Slotted Page의 삭제 slot 재사용, 고정 `row_size` 직렬화 |
| [pager.c](src/storage/pager.c) | 256개 frame의 pin·dirty·LRU로 사용 중인 페이지 교체 방지 |
| [lock_table.c](src/server/lock_table.c) | 행·범위 lock을 문장 종료에 해제하는 autocommit 경로 |

`make test-all`은 저장 구조·SQL·동시 요청을 검사합니다. 성능 개선 과정은 [PostgreSQL 대조 실험](docs/benchmark-postgres.md)에 있습니다. 인터페이스·내구성 조건이 달라 처리량을 동등 조건의 성능 비교로 읽으면 안 됩니다.

## 현재 범위

단일 테이블과 자동 생성 `id` 인덱스를 중심으로 CREATE·INSERT·SELECT·UPDATE·DELETE·DROP과 일부 조건·정렬·집계를 지원합니다. secondary index, 비용 기반 optimizer, 여러 문장을 묶는 트랜잭션과 WAL crash recovery는 없습니다. 정상 종료 후 재열기와 갑작스러운 중단에서의 복구는 다른 범위입니다.
