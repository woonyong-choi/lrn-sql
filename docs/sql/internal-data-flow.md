# SQL 엔진 내부 데이터 흐름

이 문서는 현재 코드가 한 문장을 처리할 때 파일 페이지, 메모리 프레임, 임시 버퍼를 어떻게 사용하는지 설명한다. 그림의 숫자 `id=7`, `page=1`, `slot=2`는 주소를 읽는 방법을 보여 주는 예시다. 그림은 모두 Daphnis `.dap` 원본에서 생성한 움직이는 SVG다. 각 그림의 단계 제목을 따라가면 정지 화면의 화살표가 실행 순서가 된다.

## 먼저 구분할 네 가지 주소

| 이름 | 무엇을 가리키나 | 누가 보관하나 | 언제까지 유효한가 |
|---|---|---|---|
| `id` | SQL 행의 논리 키 | DB 헤더의 `next_id`, B+Tree 리프 | 해당 행이 살아 있는 동안 |
| `page_id` | DB 파일의 페이지 번호 | 헤더·트리·힙 연결과 `row_ref` | 페이지를 재활용하기 전까지 |
| `slot_id` | 힙 페이지 안의 슬롯 번호 | 리프의 `row_ref` | 슬롯이 FREE가 되기 전까지 |
| 프레임 포인터 | 현재 메모리에 적재한 페이지 바이트 | pager의 프레임 배열 | pin·래치를 쥔 동안만 안전 |

`row_ref_t`는 `page_id` 4바이트와 `slot_id` 2바이트로 된 6바이트 주소다. B+Tree의 리프는 행 바이트 대신 `id → row_ref`를 보관한다. 행 자체는 힙 페이지에 있고, 슬롯의 `offset`이 페이지 안 행 시작 위치를 가리킨다. 근거는 [페이지 형식](../../include/storage/page_format.h)과 [힙 구현](../../src/storage/table.c)이다.

## 1. 파일에서 행까지

![DB 헤더의 루트 페이지 ID를 따라 B+Tree 리프로 내려간 뒤 row_ref의 페이지와 슬롯으로 힙 행 바이트를 찾는다](../assets/sql-data-layout.svg)

[그림 원본](../assets/sql-data-layout.dap)

새 파일은 page 0에 DB 헤더, page 1에 빈 힙, page 2에 빈 리프 루트를 만든다. 이후 페이지는 빈 페이지 목록을 먼저 재사용하고, 없으면 `next_page_id`에서 배정한다. 파일 오프셋은 `page_id × page_size`다. 페이지 크기는 새 파일을 만들 때 OS 페이지 크기로 정하고 헤더에 저장하므로, 다른 환경에서 다시 열 때도 파일에 기록된 크기를 사용한다.

힙 페이지는 16바이트 헤더, 8바이트 슬롯 목록, 뒤에서 앞으로 놓이는 고정 길이 행 바이트로 구성된다. 리프 페이지는 20바이트 헤더와 14바이트 `(id, row_ref)` 엔트리를 담는다. 내부 페이지의 경계 키는 어느 자식 페이지로 내려갈지만 정한다. 이 구성은 [페이지 형식](../../include/storage/page_format.h), [B+Tree](../../src/storage/bptree.c), [pager](../../src/storage/pager.c)에 정의되어 있다.

## 2. INSERT가 데이터를 넣는 순서

![INSERT가 값을 검증하고 id를 소비한 뒤 힙 슬롯에 행을 복사하고 B+Tree 리프에 주소를 등록하며 필요할 때 리프를 분할한다](../assets/sql-insert-lifecycle.svg)

[그림 원본](../assets/sql-insert-lifecycle.dap)

| 순서 | 현재 코드의 동작 | 메모리와 페이지 변화 |
|---|---|---|
| 1 | `exec_insert`가 컬럼 수와 정수 범위를 검증한다. | 실패하면 행 쓰기 전에 반환한다. |
| 2 | `next_id`를 증가시키고 해당 id의 X 잠금을 요청한다. | 헤더 사본이 dirty가 된다. 잠금 실패 뒤 id를 되돌리는 코드는 없다. |
| 3 | `row_serialize`가 `row_buf`에 고정 길이 행을 만든다. | `calloc`한 임시 버퍼가 생긴다. |
| 4 | `heap_insert`가 마지막 힙 페이지의 빈 슬롯 또는 새 공간을 확인한다. 부족하면 새 힙 페이지를 연결한다. | 쓰기 래치 안에서 행을 복사하고 `row_ref`를 반환한다. 삭제된 슬롯 재사용은 기존 `offset`을 쓴다. |
| 5 | `bptree_insert`가 `(id, row_ref)`를 리프에 정렬 삽입한다. | 리프가 가득 차면 임시 배열과 새 페이지를 쓰고 `next_leaf`·`prev_leaf`·부모 경계 키를 고친다. 부모까지 차면 분할을 전파해 새 루트를 만든다. |
| 6 | `row_buf`를 해제하고 `row_count`를 늘린다. | 바뀐 페이지는 dirty 프레임에 남는다. 파일 기록은 별도 시점이다. |

빈 슬롯 탐색은 마지막 힙 페이지를 먼저 보고, 삭제가 있었을 수 있다는 힌트가 있을 때만 전체 힙 체인을 다시 걷는다. 선택한 페이지는 쓰기 래치를 잡은 뒤 공간을 다시 확인한다. 이는 그 사이 다른 스레드가 페이지를 채울 수 있기 때문이다. 근거는 [실행기](../../src/sql/executor.c)의 `exec_insert`, [힙](../../src/storage/table.c)의 `find_heap_page`·`heap_insert`, [B+Tree](../../src/storage/bptree.c)의 `bptree_insert`·`propagate_insert`다.

여기에는 WAL이나 여러 문장을 묶는 원자적 트랜잭션이 없다. 헤더의 id 소비, 힙 쓰기, 인덱스 쓰기 사이에서 오류가 날 때 전체를 되돌리는 계약을 그림이 뜻하지 않는다.

## 3. SELECT가 데이터를 꺼내는 순서

![SELECT가 id 점 조건이면 리프 한 건, id 범위면 리프 연결, 정렬 조건이면 힙 스캔을 선택하고 결과 버퍼로 행을 옮긴다](../assets/sql-read-lifecycle.svg)

[그림 원본](../assets/sql-read-lifecycle.dap)

| SQL 예시 | `EXPLAIN`에서 확인한 경로 | 실제 읽기와 메모리 |
|---|---|---|
| `SELECT * FROM users WHERE id = 2` | `INDEX_LOOKUP` | 리프에서 `row_ref` 하나를 찾고 `heap_fetch`가 힙 페이지의 읽기 래치를 잡는다. 행을 역직렬화한 뒤 래치를 놓는다. |
| `SELECT * FROM users WHERE id BETWEEN 1 AND 3 LIMIT 2` | `INDEX_RANGE` | 하한 리프부터 `next_leaf`를 따라 두 주소를 수집한다. 리프 래치를 모두 놓은 뒤 힙을 읽고 LIMIT에서 멈춘다. |
| `SELECT * FROM users WHERE id >= 2 ORDER BY age DESC LIMIT 1` | `TABLE_SCAN` | 정렬 요청 때문에 힙 체인을 훑고 Top-K 행 버퍼를 만든다. 현재 인덱스 정렬을 재사용하지 않는다. |
| `SELECT COUNT(*) FROM users WHERE age >= 25` | `TABLE_SCAN` | 힙 행마다 조건을 검사해 개수를 센다. |
| `SELECT COUNT(*) FROM users` | `TABLE_SCAN` | 계획 이름과 달리 실행기는 헤더의 `row_count`를 직접 읽는다. 힙 스캔은 하지 않는다. |

범위 조회는 먼저 `row_ref` 배열을 `realloc`으로 키우며 수집하고, 그 다음 힙 페이지를 읽는다. 그래서 리프와 힙의 읽기 래치를 동시에 쥐지 않는다. 연속된 주소가 같은 힙 페이지를 가리키면 그 페이지의 래치를 재사용한다. 일반 `heap_scan`은 페이지별 살아 있는 행을 임시 snapshot으로 복사하고 래치를 푼 뒤 콜백을 호출한다. snapshot 할당이 실패하면 슬롯마다 래치를 다시 잡는 경로가 있다. 근거는 [B+Tree 범위 순회](../../src/storage/bptree.c), [힙 스캔](../../src/storage/table.c), [실행기](../../src/sql/executor.c)다.

## 4. DELETE와 빈 공간 재사용

![DELETE가 row_ref로 힙 슬롯을 FREE 목록에 넣고 B+Tree 키를 제거한 뒤 부족한 리프를 차용 또는 병합하며 다음 INSERT가 빈 슬롯을 재사용한다](../assets/sql-delete-reuse.svg)

[그림 원본](../assets/sql-delete-reuse.dap)

`DELETE WHERE id = 7`은 `bptree_search`로 주소를 얻고 `heap_delete`로 슬롯을 FREE로 바꾼 다음 `bptree_delete`로 키를 지운다. 행 바이트는 지우지 않는다. 다음 INSERT는 `free_slot_head`에서 슬롯을 꺼내 같은 `offset`에 새 행을 덮어쓴다. 리프에서 키가 너무 적어지면 오른쪽 또는 왼쪽 형제에게서 차용하거나 병합한다. 병합으로 해제한 페이지는 헤더의 `free_page_head` 연결 목록에 들어가 다음 페이지 할당에 쓰인다. `UPDATE WHERE id = 7`은 별도 실행 예시다. id 자체는 바꾸지 않고 힙 행을 제자리 수정하므로 B+Tree의 주소는 유지된다. 근거는 [힙](../../src/storage/table.c), [B+Tree](../../src/storage/bptree.c), [실행기](../../src/sql/executor.c)다.

일반 컬럼 조건의 UPDATE·DELETE는 먼저 힙을 스캔해 대상 id를 배열에 모은 뒤 잠금을 획득하고 대상 행을 다시 검사한다. 스캔 도중 구조를 바꾸지 않기 위한 두 차례 경로다. 한 SQL 문장 동안의 잠금과 페이지 내용에 쓰는 짧은 래치는 다른 것이다. HTTP 경로는 `db_execute`가 선행 잠금과 엔진 RWLock을 잡고, REPL은 `execute`를 직접 호출한다. 두 경로 모두 문장 종료에 논리 잠금을 해제한다. [동시성 코드](../../src/db.c), [REPL](../../src/main.c), [행 잠금](../../src/server/lock_table.c)에 근거한다.

## 5. pager가 메모리와 디스크를 오가는 방법

![pager가 페이지 ID를 프레임 해시에서 찾고 pin과 래치를 잡으며 미스면 오래된 프레임을 교체하고 dirty 페이지를 나중에 기록한다](../assets/sql-pager-lifecycle.svg)

[그림 원본](../assets/sql-pager-lifecycle.dap)

`pager_t`는 256개 프레임과 512개 해시 버킷을 가진다. 각 프레임의 `data`는 `pager_open`에서 페이지 크기만큼 할당된다. `page_id`를 찾으면 pin을 늘리고 LRU 시각을 갱신한다. 없으면 빈 프레임이나 `pin_count == 0`인 가장 오래된 프레임을 선택한다. 선택한 프레임이 dirty이면 먼저 `pwrite`한 뒤 새 페이지를 `pread`한다. 모든 프레임이 pinned면 페이지 요청은 실패한다.

| 상태·행동 | 언제 | 뜻 |
|---|---|---|
| pin | `pager_get_page` 또는 래치 접근 함수 | 해당 프레임을 교체하지 못하게 한다. |
| 읽기·쓰기 래치 | `pager_get_page_rlatch`·`pager_get_page_wlatch` | 같은 페이지 바이트의 동시 접근을 보호한다. 해제 함수가 unpin도 수행한다. |
| dirty | 페이지 내용 변경 후 `pager_mark_dirty` | 디스크보다 프레임이 최신임을 표시한다. dirty 프레임 64개 이상이면 pin 없는 프레임을 16개 목표로 선제 기록한다. |
| 축출 | 캐시 미스에 빈 프레임이 없을 때 | pin 없는 LRU를 고르고 dirty이면 파일에 기록한다. |
| 정상 종료 | `pager_close` → `pager_flush_all` | 남은 dirty 프레임과 DB 헤더를 기록하고 `fsync`한 뒤 프레임 데이터를 해제한다. |

축출·워터마크 기록은 정상 종료의 `fsync`와 다르다. REPL의 `.flush`도 `pager_flush_all`을 직접 호출한다. crash recovery와 WAL은 구현되지 않았다. 근거는 [pager 구현](../../src/storage/pager.c)과 [REPL](../../src/main.c)이다.

## 6. 메모리 소유권과 반납

![문장 파싱 결과는 스택에 있고 범위 주소 배열과 출력 버퍼는 동적 할당하며 출력 버퍼는 호출자로 이전되고 pager 프레임은 DB 종료에 해제된다](../assets/sql-memory-ownership.svg)

[그림 원본](../assets/sql-memory-ownership.dap)

| 대상 | 만드는 곳 | 사용하는 곳 | 해제하는 곳 |
|---|---|---|---|
| `statement_t` | `parse` 호출자의 스택 | 계획기·실행기 | 문장 함수가 반환될 때 |
| INSERT `row_buf` | `exec_insert`의 `calloc` | `row_serialize`, `heap_insert` | 인덱스 삽입 시도 뒤 `free` |
| 범위 조회 `row_ref` 배열 | `range_collect_cb`의 `realloc` | `exec_index_range_scan`의 힙 조회 | 힙 조회 뒤 또는 할당 오류 뒤 `free` |
| 힙 스캔 snapshot | `heap_scan`의 페이지별 `malloc` | 래치를 푼 뒤 콜백 | 같은 페이지 처리 뒤 `free` |
| 정렬용 행 배열 | `row_collect_init`과 `row_collect_push` | `ORDER BY` 또는 Top-K | 출력 뒤 `row_collect_free` |
| `out_buf_t.data` | `buf_init`과 `buf_append` | 결과 문자열 생성 | `buf_transfer`로 `exec_result_t.out_buf`에 넘긴 뒤 REPL·HTTP 호출자가 `free` |
| 프레임 `data` | `pager_open`에서 256개 `calloc` | 힙·B+Tree 페이지 캐시 | `pager_close`에서 각 프레임 `free` |

`heap_fetch`가 반환하는 포인터는 복사본이 아니라 프레임 내부 주소다. 호출자는 읽기 래치를 놓기 전에 값을 복사·역직렬화해야 한다. 결과 버퍼와 프레임 버퍼를 혼동하면 안 된다. HTTP는 결과를 8192바이트 응답 배열에 복사한 뒤 원래 `out_buf`를 해제한다. 큰 결과는 이 배열 크기로 잘릴 수 있으므로 결과 크기 계약이 아직 부족하다. 근거는 [실행기](../../src/sql/executor.c), [힙](../../src/storage/table.c), [HTTP 서버](../../src/server/server.c), [pager](../../src/storage/pager.c)다.

## 7. 명령과 절의 조합

![파서가 문장 하나와 지원 절을 끝까지 읽은 뒤 SELECT의 조건과 정렬 여부로 점 조회·범위 조회·힙 스캔을 고르고 변경 명령과 EXPLAIN을 분기한다](../assets/sql-command-composition.svg)

[그림 원본](../assets/sql-command-composition.dap)

| 명령 | 현재 파서가 받는 핵심 형태 | 실행 경로 |
|---|---|---|
| `CREATE TABLE` | 단일 테이블, INT·BIGINT·VARCHAR, 명시적 identity id 또는 암묵적 id | 헤더 스키마와 행 크기 설정 |
| `INSERT` | 한 행의 `VALUES`, 선택적 컬럼 목록, 모든 사용자 컬럼의 값 필요 | 값 검증 → 힙 삽입 → id B+Tree 등록 |
| `SELECT` | `*` 또는 `COUNT(*)`, 선택적 단일 WHERE, `ORDER BY` 다음 `LIMIT` | 조건과 절 조합에 따른 세 접근 경로. 조건 없는 COUNT는 헤더 집계 |
| `UPDATE` | 한 컬럼의 `SET`, 선택적 단일 `WHERE` | id 점은 인덱스, 그 밖은 힙 스캔 후 대상 재검사 |
| `DELETE` | 선택적 단일 `WHERE` | id 점은 인덱스, 그 밖은 힙 스캔 후 대상 재검사 |
| `DROP TABLE` | 등록된 단일 테이블 이름 | 페이지·헤더 초기화 |
| `EXPLAIN` | 지원하는 한 문장을 감싸되 중첩 불가 | 계획 문자열만 출력하고 안쪽 문장은 실행하지 않음 |

WHERE는 한 비교 조건 또는 `id BETWEEN a AND b`를 받는다. `OR`, `JOIN`, `OFFSET`, 여러 문장을 묶는 `BEGIN`·`COMMIT`, 한 번에 여러 SQL 문장 실행은 지원하지 않는다. 잔여 입력은 파서가 오류로 거절한다. `COUNT(*)`에 `ORDER BY`·`LIMIT`를 붙이면 파서는 읽더라도 현재 실행기는 집계 분기에서 그 절을 적용하지 않는다. 이 조합을 정상 지원으로 간주하면 안 된다. 근거는 [파서](../../src/sql/parser.c), [계획기](../../src/sql/planner.c), [실행기](../../src/sql/executor.c)다.

빈 파일에서 위 세 SELECT 예시와 `SELECT COUNT(*) FROM users WHERE age >= 25`를 실행해 각각 `INDEX_LOOKUP`, `INDEX_RANGE`, `TABLE_SCAN`, `TABLE_SCAN`을 확인했다. 준비 입력은 `CREATE TABLE users (id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, name VARCHAR(32), age INT)`와 Alice 25, Bob 31, Carol 20을 삽입하는 세 INSERT다. `SELECT * FROM users WHERE age = 25 OR age = 32`는 `오류: SQL 구문을 해석할 수 없습니다`로 거절됐다. 이 관측은 명령 조합의 일부를 검증한 것이며 PostgreSQL 전체 호환 판정이 아니다. [PostgreSQL 18 차등 검사 범위](pg-compatibility.md)에서 실제 비교 계약을 확인할 수 있다.

## 재생성과 검증

Daphnis 0.1.3의 `check --strict --no-deprecated`로 이 문서의 `.dap` 원본을 검사하고 같은 버전의 `render`로 SVG를 생성한다. 저장소 CI는 모든 `sql-*.dap`의 SVG가 재생성 결과와 바이트 단위로 같은지 확인한다. 그림은 현재 저장 경로를 설명하며, WAL·MVCC·PostgreSQL 네트워크 프로토콜의 동작을 표시하지 않는다.
