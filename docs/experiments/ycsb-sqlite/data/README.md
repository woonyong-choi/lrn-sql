# 데이터

## 출처

| 항목 | 값 |
|---|---|
| 수집 방법 | 공식 YCSB 0.17.0 Client와 CoreWorkload, 공통 SQL/JNI 바인딩으로 양쪽 엔진 실행. 매 실행 전후 별도 연결로 전체 저장 내용 검사 |
| 수집 기간 | 2026-10-18 UTC |
| 개수 | 정식 240회, 사전 실행 48회, 초기 load 4회. 별도 profile은 상위 보고서에 구분 |
| 표본 여부 | 계획의 24조건을 엔진별 5회 반복. 실제 운영 데이터의 표본이 아닌 합성 데이터 |
| 라벨 | 없음. 성공·실패는 종료 상태, 공식 반환 상태, 연산 수와 저장 검사로 판정 |
| 알려진 문제 | 요청열은 ThreadLocalRandom의 독립 시행. 같은 초기 데이터와 분포를 사용하며 요청 순서는 같지 않음. 최초 환경 조회 실패는 workload 실행 전 기록으로 보존 |
| 개인정보 | 합성 키·값. 실행 명령에는 수집 머신의 사용자명과 절대 경로가 포함됨. 인증 정보는 없음 |
| 라이선스 | 직접 작성한 연결 코드·산출물은 저장소 LICENSE. 공식 YCSB 파일은 Apache-2.0 원문 LICENSE·NOTICE 보존 |

## 파일

| 파일 | 내용 | 만드는 스크립트 |
|---|---|---|
| `raw/20261018T002649Z-be5fb10/manifest.json` | 고정 소스 해시, 환경, 설정, 예정 실행 수 | `bench/ycsb/runner.py` |
| `raw/20261018T002649Z-be5fb10/records.jsonl` | 사전·정식 실행의 정규화된 관측 행 | `bench/ycsb/runner.py` |
| `raw/20261018T002649Z-be5fb10/baselines/` | 초기 load의 properties·명령·stdout·stderr·저장 검사 | `bench/ycsb/runner.py` |
| `raw/20261018T002649Z-be5fb10/trials/` | 실행마다 공식 출력·properties·process·전후 audit·정규화 record | `bench/ycsb/runner.py` |
| `raw/20261018T002128Z-ee7f9b0/startup-failure.json` | 첫 환경 조회 실패, 성능 표본 없음, 원본 로그 해시 | 실패 조사 시 기록 |
| `SHA256SUMS` | 원자료 파일의 SHA-256 | `bench/ycsb/analysis.py` |
| `../results/summary.json` | 원자료에서 다시 계산하는 집계·가설 판정 | `bench/ycsb/analysis.py` |
| `../results/summary.csv` | 조건별 처리량·범위·대응 비율 | `bench/ycsb/analysis.py` |
| `../results/operations.csv` | 연산별 수와 지연 분위수·범위 | `bench/ycsb/analysis.py` |

위 스크립트 경로는 저장소 루트 기준이다. 실행 명령은 [결과](../report.md)의 재현 방법에 있다. DB 파일과 내려받은 JAR·빌드 파일은 `build/`에만 생성하고 원자료에 넣지 않는다. 원자료와 로그는 고치지 않는다. `results/`는 `../run.sh analyze`로 다시 만든다. 원자료를 지우면 집계를 다시 만들 수 없다.

## 필드

### `records.jsonl`

UTF-8 JSON Lines로 한 줄에 한 실행을 쓴다. 결측값은 `null`이다. 파생 표는 UTF-8 CSV이며 결측값은 빈 칸이다.

| 필드 | 타입 | 단위 | 제약 | 뜻 | 예 |
|---|---|---|---|---|---|
| `schema_version` | integer | 없음 | 필수 | 관측 형식 버전 | `1` |
| `run_id` | string | 없음 | 필수 | 수집 묶음 | `20261018T002649Z-be5fb10` |
| `trial_id` | string | 없음 | 묶음 안 고유 | 크기·워크로드·스레드·단계·반복·엔진 | `10000-E-t1-measured-1-totedb` |
| `condition` | object | 행·개 | 필수 | `records`, `operations`, `threads`, `workload`, `engine` | `{"records":10000,"operations":1000,"threads":1,"workload":"E","engine":"totedb"}` |
| `ts_utc` | datetime | UTC | 필수 | child 시작 시각 | `2026-10-18T00:26:56.436385+00:00` |
| `input_identifier` | string | 없음 | SHA-256 | 초기 키·전체 필드 정규화 해시 | `020d732ae8d02fe2917fcd5f08f7f14b11c82762c8d20cc137bc934bf56a1c3f` |
| `git_sha` | string | 없음 | 필수 | 수집 시 소스 커밋 | `be5fb10c8fd5102929e088af41026bd2e125cf21` |
| `phase` | string | 없음 | `preflight`, `measured` | 통계 포함 여부 구분 | `measured` |
| `repetition` | integer | 회 | 사전 0, 정식 1 이상 | 교대 실행의 대응 반복 | `1` |
| `process` | object | s·B·건 | 필수 | 명령, exit, timeout, wall/CPU, RSS, fault, 부하 평균, sample 상태 | `{"exit_code":0,"timeout":false}` |
| `metrics` | object | ops/s·µs·건·B | 공식 출력과 바인딩에서 계산 | `ycsb` 연산별 수치, `binding` 호출·반환 행·바이트·재시도, 논리 연산·non-OK 수 | `{"logical_operations":1000,"non_ok":0}` |
| `audit_before`, `audit_after` | object | 행·B·개 | 별도 연결의 전체 읽기 | 행 수, 필드 수·길이, 전체·키 해시, 변경 필드·추가·누락 행 | `{"rows":10000,"field_count":10,"field_bytes":100}` |
| `database_size_bytes` | integer | B | 0 이상 | 종료 후 DB 파일 크기 | `1351680` |
| `problems` | array | 없음 | 빈 배열이면 검사 통과 | 종료·연산·반환값·저장 검사 불일치 | `[]` |

`process.json`, `audit-before.json`, `audit-after.json`, `record.json`은 같은 관측 행의 원본 근거다. `stdout.txt`와 `stderr.txt`는 공식 출력 그대로다. `run.properties`는 해당 실행에 넘긴 값이다. 명령과 시각은 같은 폴더의 `process.json`에 있다. F는 논리 RMW 한 번에 read와 update를 따로 호출하므로 SQL 호출 수가 논리 연산 수보다 많다. 사전 실행의 `dataintegrity=true` 값은 정식 갱신 값과 구분한다.
