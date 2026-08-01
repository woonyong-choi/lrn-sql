# MiniDB vs PostgreSQL 16 — Range 조회 약점 발견·개선·재측정

이 문서는 "측정으로 약점을 찾고 → 코드로 고치고 → 재측정으로 개선을 증명한다"는
엔지니어링 루프를 그대로 기록한다. 모든 수치는 실제 실행 출력에서 옮긴 것이며,
개선 전(before)과 개선 후(after)는 **같은 소스**를 컴파일 플래그로만 나눠 같은
기계에서 측정했다.

측정일: 2026-08-01. 측정 기계: Linux x86_64, page_size=4096.

---

## 1. 측정 조건 (공정성 명시)

| 항목 | MiniDB | PostgreSQL |
|---|---|---|
| 버전 | `/tmp/minidb` 교육용 C 엔진 | PostgreSQL 16.13 (Ubuntu 16.13-0ubuntu0.24.04.1) |
| 빌드 | `cc -O2 -Wall -Iinclude` (sanitizer 제거). 원본 Makefile은 `-fsanitize=address,undefined -g` | 배포판 바이너리, 시스템 클러스터 |
| 인터페이스 | REPL 바이너리, SQL을 stdin 파이프, stdout→/dev/null. 같은 프로세스 = 단일 커넥션 | psycopg2, TCP 127.0.0.1, 단일 커넥션, autocommit=true |
| 트랜잭션/내구성 | 문장 단위 실행. **WAL 없음** — dirty 페이지는 캐시 축출·종료 시에만 디스크로 | autocommit=true, `synchronous_commit=on`, `fsync=on`(주) / `fsync=off`(병기) |
| 동시성 | 단일 프로세스 순차 (경합 없음) | 단일 커넥션 순차 |
| 데이터 | 단일 테이블. `value BIGINT` + 자동 `id BIGINT`. id는 1..N 연속 | `id BIGINT PRIMARY KEY, value BIGINT`. 동일 값·동일 시드(SEED=42) |
| 규모 | 10,000행 / 100,000행 두 가지 | 동일 |
| 반복 | 시나리오별 3회 중앙값 (PG fsync=on의 100k INSERT만 1회 — 문장별 WAL fsync로 137초 소요) | 동일 |

**MiniDB 측정 방식**: 시나리오별로 REPL 프로세스를 새로 띄워 전체 벽시계 시간을
재고, no-op 기준선(열기+`.exit`, 중앙값 ≈ 3.3~4.1 ms)을 뺐다. Point는 순수 시간이
기준선 노이즈에 가까워 1,000회 반복(≈2.5 µs/query)으로 보조 확인한다.

**공정성을 위한 명시**:
- MiniDB는 WAL·내구성이 없으므로 INSERT 비교의 주 기준은 **PG fsync=off**로 두되
  fsync=on도 병기한다. 그래도 MiniDB의 INSERT 우위는 근본적으로 불공정하다(아래 §6).
- **Range는 읽기 전용**이라 fsync 설정과 무관하다(측정으로도 fsync on/off 차이가 노이즈
  수준임을 확인). 따라서 Range 비교는 인덱스 접근 경로의 알고리즘 비교로 유효하다.
- MiniDB Range 질의는 `WHERE id >= a LIMIT 1000`, PG는 `WHERE id BETWEEN a AND a+999`.
  id가 1..N 연속이고 힙 순서=id 순서라 **반환 행 집합은 동일**하다. 차이는 접근 경로뿐이다.

---

## 2. 약점 확증 — "B+Tree를 만들어 놓고 범위 조회에 안 쓴다"

개선 전 코드에서 `WHERE id >= a`(및 `>`, `<`, `<=`)는 플래너가 `PREDICATE_FIELD_CMP`로
분류하여 **`ACCESS_PATH_TABLE_SCAN`(힙 전체 스캔)**으로 돌았다. B+Tree는 `id = X` 점
조회(`ACCESS_PATH_INDEX_LOOKUP`)에만 쓰였다. `EXPLAIN`과 `.debug` 계측(페이지 로드 수)으로
확증했다(10,000행, row_size=16, 힙 페이지당 ≈170행):

```
minidb> EXPLAIN SELECT * FROM bench WHERE id >= 5000 LIMIT 1000
Access Path: TABLE_SCAN
  Filter: id (comparison)
  Scan: all heap pages
minidb> SELECT * FROM bench WHERE id >= 5000 LIMIT 1000
[debug] 소요: 3.89ms | 페이지 로드: 36 (히트: 36, 미스: 0)   ← 힙을 처음부터 스캔

minidb> EXPLAIN SELECT * FROM bench WHERE id = 5000
Access Path: INDEX_LOOKUP
  Index: B+ Tree (id)
minidb> SELECT * FROM bench WHERE id = 5000
[debug] 소요: 0.04ms | 페이지 로드: 3 (히트: 1, 미스: 2)     ← 점 조회는 B+Tree 사용
```

`id >= 5000 LIMIT 1000`은 힙을 **처음부터** 읽으며 id<5000 행 ~5,000개를 역직렬화·판정한
뒤 매칭 1,000개를 모아 조기 종료한다 → 36 페이지. 시작 키가 뒤로 갈수록 나빠진다:
`id >= 9000 LIMIT 1000`은 힙 거의 전체(~59페이지)를 훑는다. 즉 **O(테이블 크기)**.

**리프 형제 포인터 확인**: `leaf_page_header_t`에 `next_leaf_page_id`/`prev_leaf_page_id`가
이미 존재하고(분할·병합 시 유지됨), 삽입 코드가 이를 올바로 갱신한다. 따라서 리프까지
O(log N) 하강 후 형제 포인터를 따라 순차 순회하면 범위 스캔을 O(log N + 반환행)으로
구현할 수 있다 — 자료구조는 준비돼 있는데 실행기가 쓰지 않고 있었다.

---

## 3. 개선 구현 — B+Tree 인덱스 범위 스캔

`id` 범위 술어를 힙 스캔 대신 **B+Tree 리프 순차 순회**로 처리하도록 고쳤다.
파서에 `BETWEEN a AND b`를 추가하고, `id >= / > / <= / <` 를 범위 술어로 승격했다.

### 바꾼 파일과 함수

| 파일 | 변경 |
|---|---|
| `include/sql/statement.h` | `PREDICATE_ID_RANGE` 술어 종류 추가. 경계 필드 추가: `range_lo, range_hi, has_lo, has_hi, lo_inclusive, hi_inclusive` |
| `src/sql/parser.c` | `parse_where()`: (1) `id BETWEEN a AND b` 파싱 → 양끝 포함 범위, (2) `id` 컬럼의 `>=,>,<=,<` 를 `PREDICATE_ID_RANGE`로 승격(포함/배타 경계 설정). `parse()`의 EXPLAIN 분기가 범위 필드도 복사 |
| `include/sql/planner.h`, `src/sql/planner.c` | `ACCESS_PATH_INDEX_RANGE` 접근 경로 추가 + 이름. SELECT이고 `PREDICATE_ID_RANGE`이며 `ORDER BY`/`COUNT`가 아니면 이 경로 선택(LIMIT 허용). `index_range_enabled()` 컴파일 가드(`-DMINIDB_DISABLE_INDEX_RANGE`로 개선 전 힙 스캔 재현) |
| `include/storage/bptree.h`, `src/storage/bptree.c` | `bptree_range_scan()` 신규: 하한 키가 있으면 그 리프까지 `find_leaf_rlatch`로 하강, 없으면 `find_leftmost_leaf_rlatch()`로 최좌단 리프까지 하강한 뒤, `next_leaf_page_id` 형제 포인터를 따라 오름차순 순회하며 콜백 호출. 상한 초과 시 즉시 중단. 리프 이동은 **다음 리프 rlatch를 먼저 잡고 현재를 해제**하는 leaf-chain latch coupling |
| `src/sql/executor.c` | `exec_index_range_scan()` 신규(2단계: ① 리프 순회로 `row_ref` 수집, ② 힙에서 읽어 출력). `match_predicate()`에 `PREDICATE_ID_RANGE` 경계 검사 추가(ORDER BY/DELETE/UPDATE/COUNT의 table-scan fallback용). `execute()` 디스패치와 `EXPLAIN` 출력에 INDEX_RANGE 추가 |

### 파서가 지원하게 된 문법
```
WHERE id >= a            (하한 포함)
WHERE id >  a            (하한 배타)
WHERE id <= b            (상한 포함)
WHERE id <  b            (상한 배타)
WHERE id BETWEEN a AND b  ← 신규 (양끝 포함)
```
(BETWEEN은 현재 `id` 컬럼만 지원 — §6 한계 참조.)

### 설계 포인트 — 정확성/안전성
- **행 순서 불변**: B+Tree는 id 오름차순을 내주고, id 순서 = 힙 삽입 순서라 기존 힙
  스캔과 **동일한 순서·동일한 행**을 출력한다. `LIMIT` 조기 종료도 유지.
- **래치 순서**: 1단계(리프 순회)는 B+Tree 리프 rlatch만, 2단계(힙 fetch)는 힙 rlatch만
  잡는다. 두 래치를 동시에 잡지 않아 래치 순서 데드락 여지가 없다.
- **페이지 로드 최적화**: 연속 `row_ref`는 대개 같은 힙 페이지를 가리키므로, 같은
  페이지가 이어지는 동안 rlatch를 재사용해 페이지당 1회만 로드한다.

### 검증
- **원본 Makefile(sanitizer 포함)로 빌드 성공**: `make` (gcc `-Wall -Wextra -Werror
  -fsanitize=address,undefined`).
- **ASAN/UBSAN 무오류**: 10,000행 INSERT + 범위/점/BETWEEN/ORDER BY/COUNT/UPDATE/DELETE
  혼합 스모크 테스트를 sanitizer 빌드로 실행 → 런타임 오류 0.
- **정확성 교차검증**: 같은 술어를 INDEX_RANGE와 TABLE_SCAN(ORDER BY로 강제) 두 경로로
  실행해 출력이 **byte-identical**임을 확인(예: `id >= 9990` 11행 일치). BETWEEN 양끝
  포함, `>` 배타 경계, LIMIT 조기 종료 모두 기대값과 일치.
- 이 저장소에는 `tests/` 디렉터리가 없어(원본 Makefile의 `make test`가 참조하는 파일
  부재) 위 기능 스모크 테스트로 회귀를 검증했다.

### 계측 — 개선 전/후 페이지 로드 (`.debug`, 10,000행)
| 질의 | 개선 전(힙 스캔) | 개선 후(INDEX_RANGE) |
|---|---|---|
| `id >= 5000 LIMIT 1000` | 36 loads / 3.89 ms | 16 loads / ~2.0 ms |
| `id >= 9000 LIMIT 1000` | ~59 loads (힙 거의 전체) | 15 loads (시작 위치 무관) |

개선 전은 시작 키가 뒤로 갈수록 로드가 늘지만, 개선 후는 **시작 위치·테이블 크기에
둔감**하다.

---

## 4. 재측정 결과

### 4-a. Range 개선 전/후 (핵심) — MiniDB, ops/sec (중앙값 3회)

| 규모 | 개선 전 (힙 스캔) | 개선 후 (INDEX_RANGE) | 배수 |
|---|---:|---:|---:|
| 10,000행 | 2,531 (39.51 ms/100q) | **3,374** (29.64 ms/100q) | ×1.33 |
| 100,000행 | 630 (158.62 ms/100q) | **3,292** (30.38 ms/100q) | **×5.22** |

- **힙 스캔은 규모에 급격히 나빠진다**: 2,531 → 630 (데이터 10배에 처리량 ~4배 하락).
  평균 시작 키가 N/2라 평균 N/2행을 훑는 O(N) 특성 그대로다.
- **인덱스 범위 스캔은 규모에 둔감하다**: 3,374 → 3,292 (거의 불변). O(log N + 반환행)
  가설을 실측이 확인한다.

### 4-b. PostgreSQL 16.13 — ops/sec

| 시나리오 | 10k fsync=off | 10k fsync=on | 100k fsync=off | 100k fsync=on |
|---|---:|---:|---:|---:|
| INSERT (개별 문장) | 10,144 | 737.7 | 9,462 | 729.5 |
| Point `id=?` ×1000 | 9,149 | 10,863 | 9,466 | 9,986 |
| Range ×100 | 1,654 | 1,875 | 1,706 | 1,787 |
| Full scan ×10 | 251.6 | 241.7 | 19.3 | 12.6 |

PG의 Range는 규모에 둔감(1,654~1,787) — `EXPLAIN`으로 인덱스 범위 스캔 확인:
```
Index Scan using bench_pkey on bench
  Index Cond: ((id >= 5000) AND (id <= 5999))
  Buffers: shared hit=14   (rows=1000)
```
PG Range의 절대 처리량이 낮은 주 요인은 스토리지가 아니라 **질의당 TCP 왕복 +
parse/plan**(prepared statement 미사용)이다.

### 4-c. Range 맞대결 — MiniDB(개선 후) vs PG

| 규모 | MiniDB 개선 전 | MiniDB 개선 후 | PG fsync=off | PG fsync=on |
|---|---:|---:|---:|---:|
| 10,000행 | 2,531 | **3,374** | 1,654 | 1,875 |
| 100,000행 | **630** | **3,292** | 1,706 | 1,787 |

- **100k에서 개선 전 힙 스캔(630)은 PG(1,706)에 명확히 진다** — 배경에서 예고한
  "데이터가 크면 힙 스캔이 밀린다"가 이 기계에서 재현됐다.
- **개선 후(3,292)는 100k에서 PG를 ~1.9배 앞선다.** 즉 이번 개선은 "졌던(혹은 곧 질)
  항목을 알고리즘으로 뒤집은" 사례다.
- 10k에서는 개선 전(2,531)도 PG(1,654)보다 빨랐다 — 이전 세션에서 PG가 근소 우위였던
  것과 달리, 이 규모의 Range는 60 ms 안팎이라 측정 노이즈가 크다. **규모를 키운 100k가
  훨씬 신뢰할 수 있는 신호**이며, 거기서 힙 스캔의 열세가 뚜렷하다.

### 4-d. 회귀 확인 — Point·INSERT·Full (개선 전/후 동일해야 함)

개선 전/후 바이너리는 **planner의 id-범위 라우팅만** 다르고 INSERT/Point/Full 코드
경로는 동일하다(같은 소스, 매크로 한 줄 차이). 측정값도 노이즈 범위 내에서 일치:

| 시나리오 | 10k 전 | 10k 후 | 100k 전 | 100k 후 |
|---|---:|---:|---:|---:|
| INSERT | 406,935 | 415,998 | 85,702 | 86,931 |
| Point | 402,250 | 518,721 | 230,317 | 193,181 |
| Full | 328.7 | 334.4 | 32.3 | 32.9 |

Point 100k의 230k→193k 차이는 회귀가 아니라 노이즈다: 순수 시간이 기준선(≈4 ms)에
근접한 4~5 ms 구간이라 변동이 크다. 코드 경로가 동일하므로 실제 성능 변화는 없다.
결론: **범위 스캔 도입으로 인한 회귀 없음.**

---

## 5. 유의미한 성과인가 — 정직한 평가

- **알고리즘 개선으로 얻은 것 (진짜 성과)**: Range. 같은 엔진·같은 반환 행에서 접근
  경로만 힙 O(N) → 인덱스 O(log N + 반환행)으로 바꿔 **100k에서 630→3,292 ops/sec (×5.2)**를
  얻었고, 규모 둔감성(3,374→3,292)을 실측으로 증명했다. 이 개선은 PG가 이미 하던
  "인덱스 범위 스캔"을 MiniDB도 하게 만든 것으로, PG 대비 100k Range 열세(630 vs 1,706)를
  우세(3,292 vs 1,706)로 뒤집었다.
- **원래 불공정하게 유리했던 것 (성과 아님)**: INSERT·Point·Full. MiniDB의 압도적 수치는
  엔진이 우수해서가 아니라 **WAL·내구성·MVCC·네트워크 프로토콜이 없기 때문**이다.
  - INSERT 87k vs PG fsync=on 730 ops/sec (≈119배)는 전부 "커밋마다 WAL fsync를 안 하는"
    비용 차이. fsync=off로도 남는 ~8배 차이는 TCP 왕복 + 문장별 parse/plan.
  - Point·Full의 우위도 인프로세스·캐시 상주·프로토콜 부재 덕이며, 스토리지 엔진
    우수성의 증거가 아니다.

즉 이 작업의 성과는 "MiniDB가 PG보다 빠르다"가 아니라, **측정으로 약점(Range 힙 스캔)을
특정하고 → B+Tree 범위 스캔으로 고치고 → 두 규모의 재측정으로 개선과 규모 둔감성을
증명한** 엔지니어링 루프 그 자체다.

---

## 6. 남은 한계 (정직하게)

1. **내구성 없음**: MiniDB는 WAL·redo가 없어 커밋이 메모리 쓰기일 뿐이다. PG fsync=on
   비교는 근본적으로 불공정하며, 크래시 복구·지속성을 전혀 제공하지 않는다.
2. **인프로세스·단일 커넥션·MVCC 없음**: 네트워크 스택·다중 트랜잭션 격리·동시성
   워크로드가 배제됐다. PG의 강점(옵티마이저·동시성·대용량)은 이 벤치에서 발휘되지 않는다.
3. **합성 데이터·소규모**: id 1..N 연속, 힙 순서=id 순서라 범위가 물리적으로 인접한
   가장 유리한 조건. 100k(≈1.6 MB)도 전부 캐시 상주라 "디스크 스토리지" 비교가 아니다.
4. **개선 범위가 좁음**: 인덱스 범위 스캔은 **`id`(기본 인덱스) SELECT**에만 적용된다.
   `BETWEEN`은 id 컬럼만, 비-id 컬럼 범위는 여전히 힙 스캔이며, DELETE/UPDATE의 id-범위는
   안전을 위해 table-scan 경로를 유지한다(경계 판정은 인덱스와 동일한 결과).
5. **2단계 수집 방식**: 무제한 `id >= a`(LIMIT 없음)는 반환 행 `row_ref`를 메모리에 모은
   뒤 힙을 읽는다. 벤치의 LIMIT 1000에서는 무해하나, 매우 큰 결과에는 스트리밍 대비
   메모리를 더 쓴다.

---

## 7. 산출물 파일 목록과 재현 명령

### 바뀐 소스 (개선)
- `include/sql/statement.h`, `src/sql/parser.c`
- `include/sql/planner.h`, `src/sql/planner.c`
- `include/storage/bptree.h`, `src/storage/bptree.c`
- `src/sql/executor.c`

### 벤치 스크립트
- `/tmp/minidb-bench/bench_minidb_param.py` — MiniDB 드라이버(바이너리·규모 인자)
- `/tmp/minidb-bench/bench_pg_param.py` — PostgreSQL 드라이버(env: `PG_N`, `PG_REPS_INSERT`, `PG_REPS_READ`)

### 재현 명령
```bash
# 0) sanitizer 빌드 + ASAN 스모크 (무오류 확인)
cd /tmp/minidb && make                       # -O2 아님, -fsanitize=address,undefined

# 1) 벤치용 -O2 바이너리 두 개 (같은 소스, 매크로만 차이)
cd /tmp/minidb && rm -rf build-o2 && mkdir build-o2
SRCS="src/storage/pager.c src/storage/schema.c src/storage/table.c src/storage/bptree.c \
src/sql/parser.c src/sql/planner.c src/sql/executor.c src/server/http.c src/server/server.c \
src/server/lock_table.c src/db.c src/main.c"
cc -O2 -Wall -Iinclude -o build-o2/minidb-after  $SRCS -lpthread   # 개선 후 (INDEX_RANGE)
cc -O2 -Wall -DMINIDB_DISABLE_INDEX_RANGE -Iinclude -o build-o2/minidb-before $SRCS -lpthread  # 개선 전 (힙 스캔)

# 2) MiniDB 재측정 (10k, 100k)
cd /tmp/minidb-bench
python3 bench_minidb_param.py /tmp/minidb/build-o2/minidb-before "before" 10000
python3 bench_minidb_param.py /tmp/minidb/build-o2/minidb-after  "after"  10000
python3 bench_minidb_param.py /tmp/minidb/build-o2/minidb-before "before" 100000
python3 bench_minidb_param.py /tmp/minidb/build-o2/minidb-after  "after"  100000

# 3) PostgreSQL (bench 롤/DB: user=bench pw=bench db=bench, 127.0.0.1)
PGBIN=/usr/lib/postgresql/16/bin
su postgres -c "$PGBIN/psql -c \"ALTER SYSTEM SET fsync=off;\" -c \"SELECT pg_reload_conf();\""
PG_N=10000  python3 bench_pg_param.py fsync_off
PG_N=100000 python3 bench_pg_param.py fsync_off
su postgres -c "$PGBIN/psql -c \"ALTER SYSTEM SET fsync=on;\"  -c \"SELECT pg_reload_conf();\""
PG_N=10000  PG_REPS_INSERT=3 PG_REPS_READ=1 python3 bench_pg_param.py fsync_on
PG_N=100000 PG_REPS_INSERT=1 PG_REPS_READ=1 python3 bench_pg_param.py fsync_on
```

---

## 8. 실행 로그 일부 (실측 원본)

```
== MiniDB [before-heapscan] N=10000 (median of 3, baseline=3.4ms subtracted) ==
insert   ops/sec=406935.3   point ops/sec=402250.2   range ops/sec=2530.9   full ops/sec=328.7
== MiniDB [after-indexrange] N=10000 (baseline=3.3ms) ==
insert   ops/sec=415998.0   point ops/sec=518721.4   range ops/sec=3373.7   full ops/sec=334.4

== MiniDB [before-heapscan] N=100000 (baseline=4.1ms) ==
insert   ops/sec=85701.8    point ops/sec=230316.6   range ops/sec=630.4    full ops/sec=32.3
   rep: range=154.8ms / 163.3ms / 162.8ms   ← 힙 스캔, 규모에 악화
== MiniDB [after-indexrange] N=100000 (baseline=3.9ms) ==
insert   ops/sec=86931.3    point ops/sec=193181.1   range ops/sec=3291.8   full ops/sec=32.9
   rep: range=34.4ms / 32.9ms ...            ← 인덱스 범위, 규모 둔감

== PostgreSQL [fsync_off] N=10000  == insert 10144.0  point 9149.0  range 1654.5  full 251.6
== PostgreSQL [fsync_off] N=100000 == insert  9462.1  point 9465.8  range 1705.6  full  19.3
== PostgreSQL [fsync_on]  N=10000  == insert   737.7  point 10862.7 range 1874.9  full 241.7   (insert 13.5 s)
== PostgreSQL [fsync_on]  N=100000 == insert   729.5  point 9985.5  range 1786.7  full 12.6    (insert 137 s)

PG EXPLAIN (range):  Index Scan using bench_pkey  Index Cond ((id>=5000) AND (id<=5999))  rows=1000

ASAN/UBSAN 스모크(범위/점/BETWEEN/ORDER BY/COUNT/UPDATE/DELETE 혼합): runtime error 0
정확성 교차검증: INDEX_RANGE vs TABLE_SCAN 출력 byte-identical (id>=9990 → 11행 일치)
```
