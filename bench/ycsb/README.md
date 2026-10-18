# 공식 YCSB 비교 바인딩

YCSB 0.17.0의 `site.ycsb.Client`와 공식 workload A~F를 수정 없이 실행한다. Java 21, Python 3.9 이상, C 컴파일러, SQLite 개발 헤더가 필요하다. Maven은 쓰지 않는다. `JAVA_HOME`을 지정하면 해당 JDK의 `java`와 `javac`를 함께 고정한다.

```sh
python3 bench/ycsb/run.py build
python3 bench/ycsb/run.py test --output build/ycsb/checks
python3 bench/ycsb/run.py collect --output docs/experiments/ycsb-sqlite --run-id primary
python3 bench/ycsb/run.py analyze --output docs/experiments/ycsb-sqlite
python3 bench/ycsb/run.py verify --output docs/experiments/ycsb-sqlite
```

`build`는 SHA-256으로 고정한 공식 JAR 다섯 개를 `build/ycsb/deps`로 받는다. 공식 workload 파일과 Apache 2.0 LICENSE·NOTICE 원문도 lock과 대조한다. ToteDB 공유 라이브러리는 기존 `make all`의 `-O2 -g` 빌드를 사용한다. JNI도 `-O2 -g`이며 시스템 SQLite를 링크한다. 실제 JDK, 컴파일 명령, SQLite 버전과 compile options는 원자료 manifest에 남긴다.

`test`는 양쪽 어댑터를 `-Xcheck:jni`로 실행한다. CRUD, 부분 갱신, 필드 선택, 없는 키, 중복 키, NUL·0xff·빈 BLOB, Unicode 키, 사전식 scan, 짧은 scan 끝부분, 재개방과 파일 디스크립터 누수, SQL 오류, 4개 worker의 쓰기와 읽기, BUSY 재시도와 소진, SQLite commit BUSY의 미반영을 검사한다. wire의 잘림·초과·종류 오류도 검사한다. 이후 30행, 40개 논리 연산, 1·4 worker, A~F의 공식 Client 실행 48개와 전수 audit를 검사한다. 이 결과는 성능 보고에 쓰지 않는다. 기존 검사 출력 폴더를 덮어쓰지 않으므로 다시 검사할 때 다른 `--output`을 지정한다.

두 엔진은 같은 Java SQL 생성, 매개변수 배열, JNI 라이브러리, 결과 객체, YCSB 상태 변환을 쓴다. worker마다 연결 하나를 연다. ToteDB는 공개 `tote_open/exec/free/close`와 결과 wire를 쓰며 SQLite는 공개 `sqlite3_prepare_v2/bind/step/finalize`를 매 호출 수행한다. 준비 문장 캐시는 없다. SQLite 결과를 ToteDB wire로 다시 인코딩하지 않는다. wire의 INTEGER·TEXT·BLOB·NULL과 명령·오류 결과만 이 바인딩의 범위이며 REAL 결과는 거절한다. 오류의 BUSY 분류만 재시도한다. IO·UNCERTAIN·POISONED를 재시도하지 않는다.

키는 YCSB가 만든 문자열 그대로 TEXT 기본 키에 저장한다. `field0`부터 `field9`까지 각각 100바이트 BLOB이다. read와 scan은 요청한 필드만 반환하며 scan은 `key >= start ORDER BY key LIMIT n`이다. 공식 F의 read와 update는 각각 autocommit이다. SQLite는 page size 8192, DELETE journal, synchronous FULL, fullfsync OFF, busy timeout 0, cache size 256 pages, mmap size 0으로 설정하고 생성 시 유효 PRAGMA를 확인한다. 캐시는 ToteDB의 기본 256페이지에 맞추며 실제 메모리 사용량이 같다는 뜻은 아니다. ToteDB의 COW·fsync와 전원 장애 내구성이 같다고 주장하지 않는다.

BUSY 지연은 두 엔진 모두 0.5 ms에서 시작하여 20 ms까지 늘고, 한 SQL 호출당 60초에 소진한다. 이 대기는 YCSB 연산 지연에 포함된다. 연결 열기도 같은 상한으로 재시도하며 `open_retries`에 별도로 기록한다. 초기화 SQL은 `setup_attempts/setup_retries`, 요청 SQL은 `attempts/retries/exhausted`로 분리한다. `BINDING` 집계는 연결을 성공적으로 닫은 뒤에만 출력한다.

수집 기본값은 1천·1만 행, A~F, worker 1·4, 엔진 둘, 반복 5회로 정식 실행 240개이다. 조건별 별도 preflight 48개를 먼저 실행한다. 반복 1·3·5는 ToteDB부터, 2·4는 SQLite부터 실행한다. 엔진별 공식 load는 단일 worker와 `dataintegrity=true`로 수행한다. 두 엔진의 초기 전체 digest 일치를 확인하고, 닫힌 기준 DB를 매 실행 전에 새 파일로 복사한다. 정식 실행은 `dataintegrity=false`로 실제 값을 바꾼다. 독립적인 `ThreadLocalRandom` 요청이므로 요청 trace가 같다는 뜻은 아니다.

모든 실행은 새 JVM과 `-Xms256m -Xmx1g`를 쓴다. JIT 예열을 하지 않는다. 파일시스템 캐시를 통제하지 않으며 실행 직전 전체 audit 읽기가 있다. preflight도 새 DB와 새 JVM을 쓰므로 성능 예열로 해석하지 않는다. 실행당 180초 제한을 넘기면 프로세스 그룹을 종료하고 실패와 부분 로그를 보존한다. 성공적으로 audit·원자료 저장을 마친 trial DB만 지우며 실패 DB와 기준 DB는 `build/ycsb/runs/<run-id>`에 남긴다.

원자료는 `data/raw/<run-id>`의 manifest, baseline load, trial properties, 명령·종료·시간·CPU·RSS·fault·부하, stdout/stderr, 전후 audit JSON, 정규화 `records.jsonl`이다. audit는 별도 JVM으로 DB를 다시 열어 모든 키와 필드를 읽는다. 행 수, 키 순서·유일성, 필드 수·크기, 전체/키 digest, 추가·누락 행, 기준 대비 바뀐 필드 수를 확인한다. 비교용 canonical snapshot은 생성물인 `build/ycsb/runs`에 둔다. 공식 측정 키는 ASCII이며 전수 audit의 정렬 검사는 그 범위에서 적용한다. 동시 무작위 갱신의 모든 중간 상태를 증명하지는 않는다.

`analyze`는 원자료만 읽어 `results/summary.json`, `summary.csv`, `operations.csv`를 다시 만든다. 전체 예상 조건 집합과 worker·논리 연산·native 호출·상태 수를 대조한다. 오류가 있는 조건은 비교 불가로 표시하고 비율을 만들지 않는다. 조건별 반복 중앙값·최소·최대와 각 실행의 연산별 p50/p95/p99를 보고한다. 처리량 비율은 반복 번호로 짝지은 ToteDB/SQLite 비율의 중앙값과 seed 226, 1만 번 bootstrap의 95% percentile 구간이다. 5쌍의 불확실성이 크며 다중 비교 보정은 하지 않는다. `verify`는 SHA-256 파일 목록과 실제 원자료 집합을 대조하고 요약 재생성 일치를 확인한다. 하나의 실험 출력 폴더에는 수집 run 하나만 둔다. 기존 manifest가 있으면 수집을 시작하기 전에 거절하고 원자료를 보존한다. 다시 수집할 때는 새 `--output`을 지정한다.

```sh
python3 bench/ycsb/run.py collect --output build/ycsb/recollection --run-id recollection
python3 bench/ycsb/run.py analyze --output build/ycsb/recollection
python3 bench/ycsb/run.py verify --output build/ycsb/recollection
```

프로파일은 정식 수집 후 별도 디렉터리에서 실행한다.

```sh
python3 bench/ycsb/run.py profile --output docs/experiments/ycsb-sqlite \
  --run-id profile --sizes 10000 --workloads C,E --threads 1 \
  --repetitions 1 --operations 10000 --timeout 180
```

프로파일도 양쪽에 같은 조건을 적용한다. 프로세스 CPU 시간·최대 RSS·major/minor fault를 기록하고 macOS에서는 실행 중 `sample`을 시도한다. 짧은 실행은 attach 전에 끝날 수 있다. `sample_available`과 `sample_exit_code`를 확인하고 실제 stack이 없는 경우 병목을 확정하지 않는다. 프로파일 실행의 성능 숫자는 정식 결과와 합치지 않는다.
