# SQL 엔진 — C로 구현한 저장·조회 학습 프로젝트

SQL을 파싱해 실행 계획을 세우고, 페이지에 행을 저장한 뒤 B+Tree 인덱스로 조회하는 C11 학습용 데이터베이스 엔진입니다.

[![CI](https://github.com/woonyong-choi/lrn-sql/actions/workflows/ci.yml/badge.svg)](https://github.com/woonyong-choi/lrn-sql/actions/workflows/ci.yml)

- 5인 팀 과제에서 parser, planner, executor와 페이지·인덱스·잠금 계층 구현을 주도했고, 과제 이후 범위 조회와 회귀 검사를 추가했습니다.
- 100만 행에서 드러난 반복 탐색과 잠금 누적을 gdb로 추적했습니다. 개선 전후는 단일 배수가 아니라 전 구간 기울기(INSERT `O(N^2.00) → O(N^1.19)`, 범위 질의 `O(N^0.88) → O(N^0.19)`)로 비교합니다.
- `make test-all`의 6개 스위트 593개 단언을 통과했습니다. CI는 Linux에서 ASAN·UBSAN을 함께 실행합니다.

## 데모

테이블 생성 → INSERT 1,000건 → 건수 확인과 범위 SELECT를 파이프 입력으로 실행한 실제 출력입니다. 재현은 `bash scripts/demo_repl.sh`이고, 데모 빌드는 sanitizer 없는 `SANITIZE=`입니다.

![lrn-sql 구동 GIF](https://raw.githubusercontent.com/woonyong-choi/lrn-sql/main/docs/demo.gif)

```
$ time (scripts/gen_inserts.sh 1000 | build-demo/minidb /tmp/demo.db | tail -2)
minidb> 1행 삽입 완료 (id=1000)
  real 0.027s
$ build-demo/minidb /tmp/demo.db <<'SQL'
> .debug
> SELECT * FROM users WHERE id >= 996;
id | name | email | age
-----------+------------+------------+-----------
996 | user996 | user996@example.com | 56
1000 | user1000 | user1000@example.com | 20
5행 조회 (INDEX_RANGE)
[debug] 소요: 0.01ms | 페이지 로드: 2 (히트: 1, 미스: 1) | 디스크 기록: 0
```

## 빠르게 실행하기

GCC, Make, pthread가 필요합니다. 아래는 2026-09-23에 전부 실행해 통과한 명령입니다.

```sh
git clone https://github.com/woonyong-choi/lrn-sql.git && cd lrn-sql

make SANITIZE= BUILD_DIR=build-nosan all          # 빌드
printf "CREATE TABLE users (id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, name VARCHAR(32), age INT)\nINSERT INTO users (name, age) VALUES ('Alice', 25)\nEXPLAIN SELECT * FROM users WHERE id = 1\nSELECT * FROM users WHERE id = 1\n.exit\n" | ./build-nosan/minidb demo.db

make test-all      # 593개 단언
make bench         # 규모별 기울기 재측정 (36초)
bash scripts/demo_repl.sh   # 위 데모 재현 (13초)
```

이 예시의 CREATE와 INSERT는 PostgreSQL 18에서도 같은 SQL로 실행된다. 호환 확인 범위와 제약은 [차등 검사](docs/sql/pg-compatibility.md)에 있다. 기존 v1 파일은 [테이블 이름 등록](docs/sql/legacy-file-migration.md)이 필요하다.

REPL에서는 `.stats`가 페이지·트리 통계, `.debug`가 쿼리별 페이지 접근, `.btree`가 인덱스 구조를 보여 줍니다. `.exit` 또는 Ctrl-D로 종료하면 dirty 페이지를 기록합니다.

기본 빌드는 sanitizer를 켭니다. **macOS 26(Darwin 25)에서는 ASan 바이너리가 `main()`에 닿기 전에 멈추는 플랫폼 문제가 있어** `Makefile`이 UBSan만 켭니다(원인과 스택은 [`docs/build-and-test.md`](docs/build-and-test.md)). ASan까지 켠 검사는 Dev Container나 Linux에서 돌립니다.

## 구조

```
src/sql/       parser.c · planner.c · executor.c      문장 → 접근 경로 → 실행
src/storage/   pager.c · table.c · bptree.c · schema.c  버퍼 풀 · 슬롯 힙 · B+Tree
src/server/    server.c · http.c · lock_table.c       연결당 스레드 · HTTP · Strict 2PL
tests/         6개 스위트 593개 단언
bench/         scaling.py(규모별 기울기) · PostgreSQL 대조군 하니스
docs/          design.md(설계 노트) · benchmark-postgres.md · build-and-test.md
```

![REPL과 HTTP가 SQL 실행기와 저장 계층으로 모이며, 단일 테이블 파일까지 이어진다](docs/assets/sql-layers.svg)

| 계층 | 현재 구현 | 남은 핵심 |
|---|---|---|
| 입력 | REPL의 한 줄 SQL과 디버그 명령, HTTP `POST /query` | 바인딩 매개변수, 여러 문장 입력 |
| 네트워크 | 최소 HTTP/1.1, keep-alive, 연결별 스레드, 연결 상한 128 | PostgreSQL 클라이언트 프로토콜, 인증·TLS |
| 파서·계획 | 7개 명령의 제한된 문법, `id` 점·범위 인덱스와 힙 스캔 선택 | 식·NULL·JOIN, 통계 기반 계획 |
| 실행·동시성 | 행·범위 S/X 잠금, HTTP 경로의 DDL 독점 잠금, 페이지 래치 | [동시 INSERT 간헐 정지](https://github.com/woonyong-choi/lrn-sql/issues/19), 다중 문장 트랜잭션·MVCC |
| 저장 | 단일 테이블 헤더, 슬롯 힙, `id` B+Tree, 256 프레임 pager | 다중 테이블 카탈로그, 2차 인덱스 |
| 내구성 | 캐시 축출·정상 종료 때 페이지 기록과 `fsync` | WAL, 강제 종료 후 복구·원자성 |
| 결과 | REPL 텍스트와 HTTP 본문 | 형식화된 결과·오류 계약, 큰 응답 처리 |

현재 상태를 나타낸 그림이다. 회색 경로도 구현된 흐름이며, 아직 없는 기능은 표의 오른쪽 칸에 적었다. HTTP는 `db_execute`에서 선행 잠금을 잡고 REPL은 `parse`·`execute`를 직접 호출한다. 실행기 내부의 잠금은 두 경로가 공유한다. 두 경로는 같은 저장 계층을 사용한다.

### 동시 요청

![서로 다른 HTTP 연결의 두 스레드가 행·범위 잠금과 공유 pager를 거쳐 시간차로 실행된다](docs/assets/sql-concurrent.svg)

이 그림의 움직이는 점은 연결별 스레드에서 겹쳐 처리되는 요청이다. 이벤트 큐나 비동기 I/O 런타임을 나타내지 않는다.

### B+Tree와 힙

![id 키가 내부 페이지와 리프를 지나 행의 페이지·슬롯 좌표를 찾아 힙 행에 닿는다](docs/assets/sql-btree.svg)

![리프가 가득 차면 키를 정렬해 두 리프로 나누고 부모 경계 키를 갱신한다](docs/assets/sql-btree-split.svg)

리프의 `id → row_ref(page_id, slot_id)`가 힙 행을 가리킨다. 점 조회는 경계 키를 따라 한 리프로 내려가고, 범위 조회는 리프 연결을 따라간다. 삽입 중 리프가 차면 분할하고 부모에 경계 키를 전파한다. 그림 속 `10·20·40·50·70`은 구조 설명용 값이다.

## SQL 명령의 실행 경로

아래 그림은 현재 지원하는 형태의 경로다. 각 명령의 PostgreSQL 18 전체 문법을 뜻하지 않는다. [같은 SQL 차등 검사](docs/sql/pg-compatibility.md)는 현재 20건이다.

### `CREATE TABLE`

![CREATE TABLE이 기존 테이블을 확인하고 id와 사용자 컬럼 배치를 파일 헤더에 등록한다](docs/assets/sql-create-table.svg)

### `INSERT`

![INSERT가 값을 검증한 뒤 id를 할당하고 힙의 행 좌표를 B+Tree에 등록한다](docs/assets/sql-insert.svg)

### `SELECT`

![SELECT가 id 점 조회, id 범위 조회, 힙 스캔 가운데 한 경로로 행을 읽는다](docs/assets/sql-select.svg)

### `UPDATE`

![UPDATE가 수정 값을 확인하고 대상 행을 잠근 뒤 조건을 재검사해 힙 행을 바꾼다](docs/assets/sql-update.svg)

### `DELETE`

![DELETE가 대상 행을 잠근 뒤 힙 슬롯과 B+Tree 키를 제거한다](docs/assets/sql-delete.svg)

### `DROP TABLE`

![DROP TABLE이 저장된 테이블 이름을 검사한 뒤 페이지와 헤더를 초기화한다](docs/assets/sql-drop-table.svg)

### `EXPLAIN`

![EXPLAIN이 안쪽 문장의 접근 경로를 계산해 출력하고 문장은 실행하지 않는다](docs/assets/sql-explain.svg)

그림 원본은 [`docs/assets/sql-layers.dap`](docs/assets/sql-layers.dap)과 같은 이름의 `.dap` 파일이다. Daphnis `0.1.3`에서 `npm exec --yes --package=daphnis@0.1.3 -- daphnis check docs/assets/sql-*.dap --strict --no-deprecated`로 검사하고 같은 입력을 `render`로 생성했다. 실행 코드와 연결한 근거는 [`db.c`](src/db.c), [`planner.c`](src/sql/planner.c), [`executor.c`](src/sql/executor.c), [`bptree.c`](src/storage/bptree.c)다.


- 설계 노트(구조 선택, 검증의 한계, 다음 병목): [`docs/design.md`](docs/design.md)
- 빌드 옵션·플랫폼 문제·테스트 스위트 상세: [`docs/build-and-test.md`](docs/build-and-test.md)
- 문서 목차: [`docs/README.md`](docs/README.md)

## 구현에서 고른 구조

- **B+Tree 인덱스** — 범위 질의를 위해 키 순서를 보존하고, 노드 하나를 디스크 페이지 하나에 맞춥니다. 범위 질의 기울기는 `O(N^0.88) → O(N^0.19)`로 측정했습니다. → [`src/storage/bptree.c`](src/storage/bptree.c), [`docs/design.md`](docs/design.md) §2
- **슬롯 페이지 힙** — 삭제한 슬롯을 재사용하고, 행의 `(page_id, slot_id)` 좌표를 유지해 B+Tree 리프가 가리키는 위치가 바뀌지 않게 합니다. → [`src/storage/table.c`](src/storage/table.c), [`docs/design.md`](docs/design.md) §1
- **latch와 lock 분리** — 페이지 보호는 짧은 latch로, 행·범위의 논리적 잠금은 문장이 끝날 때까지 유지합니다. S/X 호환성, writer 대기, 범위 충돌, 동시 INSERT를 검사합니다. → [`src/storage/pager.c`](src/storage/pager.c), [`src/server/lock_table.c`](src/server/lock_table.c)
- **성능 비교** — 단일 실행의 배수 대신 행 수별 실행 시간을 log-log 기울기로 비교합니다. 입력 크기가 바뀌어도 접근 경로의 변화를 확인하려는 기준입니다. → [`bench/scaling.md`](bench/scaling.md), `make bench`
- **회귀 대조군** — `-DMINIDB_DISABLE_FREE_HINT`, `-DMINIDB_DISABLE_INDEX_RANGE`로 현재 소스에서 이전 접근 경로만 끄고 비교합니다. 같은 컴파일러와 주변 코드에서 전후를 재현합니다. → [`Makefile`](Makefile), [`tests/test_step3_regression.c`](tests/test_step3_regression.c)

![규모별 INSERT·Range 소요 시간 (양축 로그)](https://raw.githubusercontent.com/woonyong-choi/lrn-sql/main/docs/scaling.svg)

| INSERT | 전 구간 기울기 | 50k | 100k | 200k | 400k |
|---|---|---:|---:|---:|---:|
| 개선 전 (`e640fdd` 직전) | **O(N^2.00)** | 0.131s | 0.461s | 1.901s | 8.415s |
| 개선 후 | **O(N^1.19)** | 0.071s | 0.157s | 0.350s | 0.863s |

| Range 질의 (각 1,000행 반환) | 전 구간 기울기 | 50k | 100k | 200k | 400k |
|---|---|---:|---:|---:|---:|
| 힙 스캔 (`-DMINIDB_DISABLE_INDEX_RANGE`) | **O(N^0.88)** | 0.062s | 0.118s | 0.183s | 0.412s |
| B+Tree 리프 순회 (`INDEX_RANGE`) | **O(N^0.19)** | 0.026s | 0.029s | 0.031s | 0.039s |

단일 규모의 절대 수치가 필요하면 `make bench-1m`입니다. 마지막으로 잰 값은 1M 행 기준 INSERT 632,962 ops/sec, Range 3,267 ops/sec(힙 스캔 108 ops/sec)입니다(2026-09-22, Apple M4, -O2, median of 3).
개선 후 수치가 PostgreSQL(1M INSERT `fsync=off` 10,919 ops/sec)보다 높은 것은 성능 우위가 아닙니다. 이 엔진은 **WAL이 없어** dirty 페이지를 캐시 축출·종료 시에만 디스크로 내리므로 내구성 조건이 다릅니다([`docs/benchmark-postgres.md`](docs/benchmark-postgres.md)).

## 검증

`make test-all`이 저장 구조·SQL·동시 요청·회귀를 검사합니다. 숫자는 테스트 함수가 아니라 **단언(assertion) 개수**의 합입니다.

| 스위트 | 검증 대상 | 개수 | 실행 명령 |
|---|---|---:|---|
| MiniDB Test Suite | 페이지·힙·B+Tree·재열기 | 76 | `make test` |
| B+Tree Property | 무작위 삽입·삭제 후 구조 불변식 8종, 범위 스캔 대조 | 215 | `make test-prop` |
| Step 0 — `db_execute` | 문장 실행 진입점 | 24 | `make test-step0` |
| Step 1 — SQL Extension | 파싱·계획·조건·정렬·집계·`INDEX_RANGE`·EXPLAIN 일치 | 202 | `make test-step1` |
| Step 2 — Concurrency | S/X lock 호환성, 범위 lock, 동시 INSERT, HTTP 경로 | 52 | `make test-step2` |
| Step 3 — Regression | 고친 결함 2건이 되살아나는지 | 24 | `make test-step3` |
| **합계** | | **593** | `make test-all` |

Property 스위트는 결함 5종(불균형 분할, separator 오프바이원, 리프 체인 끊김, 언더플로우 복구 비활성화, 범위 경계 제외 누락)을 주입해 각각 다른 불변식에서 실패하는지 확인했습니다. 스위트별 상세는 [`docs/build-and-test.md`](docs/build-and-test.md)에 있습니다.

CI는 GitHub Actions에서 `ASAN_OPTIONS=detect_leaks=1`, `UBSAN_OPTIONS=halt_on_error=1`로 `make test-all`을 실행합니다.

## 범위와 한계

- 단일 테이블과 자동 생성 `id` 인덱스가 중심입니다. **secondary index, 비용 기반 optimizer, 여러 문장을 묶는 트랜잭션(`BEGIN`/`COMMIT`)은 없습니다.**
- **WAL이 없습니다.** 정상 종료 후 재열기는 보장하지만, 갑작스러운 중단에서의 crash recovery는 범위 밖입니다.
- 벤치마크는 1M 행에서도 DB 파일이 52MB라 전량 페이지 캐시에 올라갑니다. **디스크가 실제 병목인 구간은 아직 측정하지 못했습니다.**
- 동시성 검사는 4~8스레드 규모이고, 데이터 레이스 자체를 잡는 ThreadSanitizer는 아직 CI에 넣지 않았습니다. 위 3번 결함이 ASAN 타이밍에서만 드러난 것도 그 때문입니다.
- 접근 경로 선택은 규칙 기반입니다. 선택도 통계를 보지 않으므로 secondary index가 생기면 다시 설계해야 합니다.

## 관련 링크

- [SQL 엔진 구현 Wiki](https://docs.woonyong.com/wiki/lrn-sql/) — 개념 정리
- [설계 노트 — 무엇을 고르고 무엇을 버렸나](docs/design.md)
- [PostgreSQL 대조 실험 전문](docs/benchmark-postgres.md) — 측정 조건·결함 진단·정직한 평가
- [출처와 기여 경계](docs/attribution.md) — 팀 과제 범위와 개인 확장 커밋
- [팀 원본 저장소 Jungle-12-303/wk08_1](https://github.com/Jungle-12-303/wk08_1)
- [CI 실행 기록](https://github.com/woonyong-choi/lrn-sql/actions)
- Database Internals (Alex Petrov) — Slotted Page·B+Tree 구조
- [PostgreSQL 16 문서](https://www.postgresql.org/docs/16/) — 대조군 설정(`synchronous_commit`, `fsync`)
