# 빌드와 시험

README의 빠른 시작에 담지 않은 빌드 변수, 시험 대상, 외부 엔진과의 비교 방법, 플랫폼 문제를 적는다.

## 빌드

| 항목 | 내용 |
|---|---|
| 언어 | C11, `-Wall -Wextra -Werror` |
| 도구 | GNU Make, GCC |
| 런타임 의존성 | POSIX 파일 I/O와 pthread, libm |
| Python 바인딩 | 표준 라이브러리 ctypes |
| 시험 의존성 | `libsqlite3-dev`, `sqlite3` CLI, pytest |

`make all`은 DB와 큐를 묶은 공유 라이브러리 `build/lib/libtotedb.so`(macOS에서는 `.dylib`)를 `-O2`로 만든다. 이 라이브러리는 sanitizer를 켜지 않는다. Python 바인딩은 환경 변수 `TOTEDB_LIB`가 가리키는 파일을 먼저 찾고, 없으면 `build/lib/`를 찾는다.

시험 바이너리는 `SANITIZE` 변수로 sanitizer를 고른다. 기본값은 `address,undefined`다. `BUILD_DIR`를 바꾸면 sanitizer 설정이 다른 빌드를 따로 둘 수 있다.

```sh
make test-all
make SANITIZE=thread BUILD_DIR=build-tsan test-transaction test-session test-ack-queue
make SANITIZE= BUILD_DIR=build-nosan test-database
```

## 시험 대상

| 대상 | 검사하는 것 |
|---|---|
| `make test` | 행 인코딩, overflow, 값, 카탈로그, lexer, 페이지 파일, 카탈로그 저장, 기본 키·TEXT 색인 트리 |
| `make test-overflow-file` | overflow 체인의 파일 왕복 |
| `make test-value-sqlite` | 값 비교·변환을 시스템 SQLite와 대조 |
| `make test-value-varchar` | VARCHAR 경계 값과 UTF-8 판정 |
| `make test-database` | SQL 문장 parser, heap 저장소, 문장 실행, SQLite 대조, I/O·할당 실패 주입, 메모리 한도 열기, 기본 키 색인 |
| `make test-transaction` | 트랜잭션 상태 기계, writer 경합, commit 결과, 실패 주입 |
| `make test-session` | 세션 메모리 예산과 결과 수명 |
| `make test-ack-queue` | ACK 큐의 C API와 실패 주입 |
| `make test-ack-queue-python` | Python 바인딩의 DB·트랜잭션·큐 계약 |
| `make test-all` | 위 대상 전체 |
| `make test-runner` | 시험 실행기와 실패 판정기 |
| `make test-fault` | 실패 주입 런타임 |
| `make test-result` | 결과 버퍼 인코딩과 할당 실패 |

각 시험의 결과와 실패 주입 시드는 `build/fault/`에 JSON으로 남는다. CI는 이 폴더를 sanitizer별 산출물로 올린다.

## 다른 엔진과 비교

같은 SQL을 이 엔진, SQLite, PostgreSQL에 실행해 행과 오류 종류를 비교한다. 사례는 `tests/sql_diff_cases.py`에 있다. 엔진마다 의미가 다른 사례는 사례 안에 차이를 적고 `DIFF`로 보고한다.

```sh
make test-sql-diff
make test-sql-diff SQLDIFF_DSN=postgresql://user:password@localhost:5432/sqldiff
```

`SQLDIFF_DSN`이 없으면 PostgreSQL 비교를 건너뛴다. `SQLDIFF_DSN`을 주면 PostgreSQL에 연결하지 못할 때 실패로 끝난다. PostgreSQL 비교에는 Python 패키지 `psycopg`와 `C` 정렬 규칙으로 만든 데이터베이스가 필요하다.

두 연결이 같은 파일을 번갈아 쓰는 동작은 SQLite 3.51.0과 비교한다. 이 대상은 SQLite 소스를 내려받아 sha256을 확인한 뒤 공유 라이브러리로 빌드하므로 네트워크가 필요하다.

```sh
make SANITIZE= BUILD_DIR=build-sqlite test-sqlite-session-diff
```

### 성능 측정

`bench/sqlite_workload.py`는 같은 SQL을 이 엔진과 시스템 SQLite에 주고, 문장마다 결과를 대조하면서 시간과 메모리를 잰다.

- 측정 항목
  - 자동 commit 삽입
  - 기본 키·구간·조건·전체 스캔 조회
  - 혼합 부하
  - 강제 종료 뒤 다시 열기
- 측정 조건(seed, 행 수, 반복 수, 내구성 설정)은 `bench/sqlite_workload.json`에 있다
- 결과가 하나라도 다르면 실패로 끝난다
- 시간 수치는 통과/실패 판정에 쓰지 않는다

```sh
python3 bench/sqlite_workload.py --smoke
python3 bench/sqlite_workload.py --output build/bench/sqlite_workload.json
python3 bench/sqlite_workload.py --output build/bench/sqlite_workload.json --analyze
```

## CI

`.github/workflows/ci.yml`은 Ubuntu에서 아래 작업을 실행한다.

| 작업 | 명령 |
|---|---|
| `test` (address, undefined) | `make SANITIZE=<sanitizer> test-all test-runner test-fault test-result` |
| `test` (thread) | `make SANITIZE=thread test-transaction test-session test-ack-queue` |
| `pg-compat` | PostgreSQL 18 서비스에 `make test-sql-diff`를 실행한 뒤 `make test-sqlite-session-diff` |
| `actionlint` | 워크플로 파일 검사 |

## macOS에서 ASan 바이너리가 멈추는 문제

macOS 26(Darwin 25)과 Apple clang 17에서는 ASan 바이너리가 `main()`에 닿기 전에 멈춘다. ASan 런타임 초기화가 자기 자신을 다시 부르는 문제이며 빈 `main()`을 `cc -fsanitize=address`로 빌드해도 같은 증상이 나온다. 스택은 `AsanInitInternal`, `InitializeShadowMemory`, `get_dyld_hdr`, `dyld_shared_cache_iterate_text_swift`, `malloc`, `__sanitizer_mz_malloc`를 거쳐 다시 `AsanInitFromRtl`로 들어가 spin lock에서 멈춘다. UBSan만 켠 빌드는 정상 동작한다.

`Makefile`은 Darwin 25 이상에서 `SANITIZE` 기본값을 `undefined`로 낮춘다. ASan을 켠 시험은 Linux나 저장소의 Dev Container(`.devcontainer/`)에서 실행한다.
