# 빌드·테스트 상세

README의 "빠르게 실행하기"에 담지 않은 빌드 옵션, 플랫폼 문제, 테스트 스위트의 내용을 적습니다.

## 빌드 옵션

| 항목 | 내용 |
|---|---|
| 언어 | C11 (`-Wall -Wextra -Werror`) |
| 빌드 | GNU Make, GCC |
| 런타임 의존성 | POSIX pthread만 사용. 외부 DB·파서·인덱스 라이브러리 없음 |
| 진단 도구 | AddressSanitizer, UndefinedBehaviorSanitizer, gdb(스택 샘플링) |
| 서버 경로 | 자체 HTTP 처리(keep-alive, 요청 읽기 타임아웃) |
| 벤치마크 하니스 | Python 3 + psycopg2 (`bench/`), 대조군 PostgreSQL 16.13 |

`SANITIZE` 변수로 sanitizer를 고릅니다. 기본값은 `address,undefined`이고,
macOS 26(Darwin 25) 이상에서는 아래 이유로 `undefined`만 켜집니다.

```sh
make SANITIZE=address,undefined test-all   # 강제로 둘 다 (CI와 동일)
make BUILD_DIR=build-ubsan SANITIZE=undefined test-all
make BUILD_DIR=build-nosan SANITIZE= all   # sanitizer 없이
```

### 재현용 컴파일 가드

개선 전 동작을 현재 소스에서 그대로 빌드해, 같은 컴파일러·같은 주변 코드로 전후를 비교합니다.

| 가드 | 되살리는 동작 |
|---|---|
| `-DMINIDB_DISABLE_FREE_HINT` | 힙 빈 슬롯 힌트를 끄고 꼬리 페이지가 찰 때마다 힙 체인 전체를 재탐색 |
| `-DMINIDB_DISABLE_INDEX_RANGE` | `INDEX_RANGE` 접근 경로를 끄고 범위 질의를 힙 스캔으로 |
| `-DSEQ_INSERT_ROWS=N` | `make test-step3`의 순차 INSERT 회귀 검사 규모(기본 20,000) 변경 |

## macOS에서 ASan 바이너리가 멈추는 문제

**macOS 26(Darwin 25) + Apple clang 17에서는 ASan 바이너리가 `main()`에 닿기 전에 멈춥니다.**
lrn-sql의 문제가 아니라 ASan 런타임 초기화가 자기 자신을 재진입하는 것으로,
`int main(void){return 0;}` 한 줄을 `cc -fsanitize=address`로 빌드해도 똑같이 멈춥니다.
스택은 `AsanInitInternal` → `InitializeShadowMemory` → `get_dyld_hdr` →
`dyld_shared_cache_iterate_text_swift` → `malloc` → `__sanitizer_mz_malloc` →
다시 `AsanInitFromRtl`로 들어가 spin lock에 걸립니다. UBSan 단독은 정상 동작합니다.

그래서 `Makefile`은 Darwin 25 이상에서 기본값을 `undefined`로 낮춥니다.
ASan까지 켠 검사가 필요하면 저장소의 Dev Container(`.devcontainer/`)나 Linux에서 돌립니다.
CI(Ubuntu)는 항상 `address,undefined` 둘 다 켭니다.

## 테스트 스위트

| 스위트 | 파일 | 검증 대상 | 개수 | 명령 |
|---|---|---|---:|---|
| MiniDB Test Suite | `tests/test_all.c` | 페이지·힙·B+Tree·재열기 | 76 | `make test` |
| B+Tree Property | `tests/test_bptree_property.c` | 무작위 삽입·삭제 후 구조 불변식 8종, 범위 스캔 대조 | 215 | `make test-prop` |
| Step 0 — `db_execute` | `tests/test_step0_db_execute.c` | 문장 실행 진입점 | 24 | `make test-step0` |
| Step 1 — SQL Extension | `tests/test_step1_sql_ext.c` | 파싱·계획·조건·정렬·집계·`INDEX_RANGE`·EXPLAIN 일치 | 202 | `make test-step1` |
| Step 2 — Concurrency | `tests/test_step2_concurrency.c` | S/X lock 호환성, 범위 lock, 동시 INSERT, HTTP 경로 | 52 | `make test-step2` |
| Step 3 — Regression | `tests/test_step3_regression.c` | 고친 결함이 되살아나는지 | 24 | `make test-step3` |
| 합계 | | | **593** | `make test-all` |

숫자는 테스트 함수 개수가 아니라 **단언(assertion) 개수**의 합입니다.

### B+Tree property test가 무엇을 하는가

B+Tree는 오름차순 삽입만으로는 검증되지 않습니다. 그 모양은 리프가 오른쪽으로만 쪼개지는
한 가지 경우일 뿐이라, 형제 재분배와 병합이 섞이는 삭제 경로를 밟지 못합니다.
Property 스위트는 고정 시드 난수로 삽입·삭제를 섞어 돌리며 체크포인트마다 **트리 전체를 걸어**
리프 깊이 일치·키 순증가·부모 separator 구간·최소 점유율·리프 체인 대칭·페이지 중복 없음·
`parent_page_id` 정합·참조 모델 일치를 봅니다.
이 검사가 껍데기가 아닌지는 결함 5종(불균형 분할, separator 오프바이원, 리프 체인 끊김,
언더플로우 복구 비활성화, 범위 경계 제외 누락)을 일부러 심어 확인했고
**전부 서로 다른 불변식에 걸렸습니다**. 자세한 것은 [`design.md`](design.md) §5.

### 회귀 검사 (`make test-step3`, `make test-step2`)

| 검사 | 무엇을 잡는가 |
| --- | --- |
| `test_repl_insert_releases_locks` | REPL 경로 문장이 끝난 뒤 lock이 남는 회귀. `db_execute()`의 `lock_release_all()`이 빠지면 executor가 gap check에서 잡은 X lock이 해제되지 않고 누적된다 |
| `test_sequential_insert_no_heap_rescan` | DELETE 없는 순차 INSERT가 힙 체인을 다시 걷는 회귀. free slot 힌트가 사라지면 꼬리 페이지가 찰 때마다 전체 체인을 재탐색해 O(P²)로 붕괴한다 |

sanitizer를 끈 빌드는 이 검사들을 대신하지 않습니다.
CI(`.github/workflows/ci.yml`)는 Linux에서 `ASAN_OPTIONS=detect_leaks=1`,
`UBSAN_OPTIONS=halt_on_error=1`로 `make test-all`을 실행합니다.
