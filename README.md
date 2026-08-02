# MiniDB

## 한눈에

| 구분 | 내용 |
|---|---|
| 무엇 | C11 로 만든 디스크 기반 SQL 엔진. parse, plan, execute, B+Tree/heap, pager 가 한 저장소에 있다 |
| 왜 | SQL 한 건이 디스크까지 내려가는 전체 경로를 직접 이어 보기 위해 |
| 내 몫 | 팀 프로젝트의 개인 보존 미러. 이 미러 main 기준 148 커밋 중 106 이 내 커밋이고, 파일 단위 경계는 git blame 으로 확인한다 |
| 스택 | C11 · pthread · Make |
| 검증된 사실 | 테스트 224/224 재실행 (아래 표). PostgreSQL 대조 벤치는 [docs/benchmark-postgres.md](docs/benchmark-postgres.md) |
| 한계 | 교육용. 단일 테이블 중심, WAL 복구와 트랜잭션 격리 없음 |

**같은 사람의 다른 저장소** · 이력서 허브: <https://woonyong-kr.github.io>
[Kyro(k8s-ops)](https://github.com/woonyong-kr/k8s-ops) · [MiniDB](https://github.com/woonyong-kr/minidb) · [PintOS](https://github.com/woonyong-kr/pintos) · [dx_framework](https://github.com/woonyong-kr/dx_framework) · [dx_content_interface](https://github.com/woonyong-kr/dx_content_interface)


> 크래프톤 정글 12기 팀 프로젝트의 개인 보존용 미러다. 원본은 [Jungle-12-303/wk08_1](https://github.com/Jungle-12-303/wk08_1)이며, 개인 기여는 커밋 저자(`woonyong.kr@gmail.com`)와 파일별 `git blame`으로 확인할 수 있다.

## 무엇을 푸는가

- C11로 단일 `.db` 파일에 row와 index를 페이지 단위로 저장하는 작은 SQL 엔진을 구현합니다.
- SQL parse부터 실행 계획, B+Tree 또는 heap 접근, pager cache와 디스크 I/O까지 한 요청의 전체 경로를 연결합니다.
- `SW_AI-W07-SQL`의 이전 단계 구현을 확장한 팀 프로젝트이며 Git 이력 기준(이 미러 main) 전체 148개 커밋 중 본인 커밋은 106개입니다. `git shortlog -sn` 으로 확인할 수 있습니다.

## 어디를 보면 되는가

| 경로 | 이 파일이 답하는 질문 |
| --- | --- |
| [`src/main.c`](src/main.c) | REPL과 HTTP server는 어떻게 시작합니까? |
| [`src/db.c`](src/db.c) | SQL 요청은 parser, planner, executor로 어떻게 전달됩니까? |
| [`src/sql/planner.c`](src/sql/planner.c) | `id` 점 조회와 heap scan을 어떤 기준으로 나눕니까? |
| [`src/storage/bptree.c`](src/storage/bptree.c) | B+Tree 노드의 탐색, 분할, 삭제와 리프 연결은 어떻게 동작합니까? |
| [`src/storage/table.c`](src/storage/table.c) | slotted heap page에서 row를 어떻게 삽입하고 재사용합니까? |
| [`src/storage/pager.c`](src/storage/pager.c) | page cache hit, pin, dirty flush, LRU 교체를 어떻게 관리합니까? |
| [`tests`](tests) | 저장 구조, SQL 기능, 동시성 경계를 어떤 테스트로 확인합니까? |

## 설계 판단

### Hash index 대신 B+Tree를 고른 이유

Hash index는 동등 비교 점 조회에는 유리하지만 key 순서를 보존하지 않아 범위 조회와 정렬을 직접 지원하지 못합니다. B+Tree는 정렬된 리프가 앞뒤로 연결되어 `BETWEEN`과 순서 기반 순회를 확장할 수 있고, 내부 노드부터 리프까지 탐색 경로가 정해집니다. 이 구현은 노드 하나를 디스크 페이지 하나에 맞추므로 탐색에 필요한 페이지 접근 횟수를 트리 높이에 묶을 수 있습니다.

### Slotted heap page를 고른 이유

페이지 앞쪽의 slot 배열과 뒤쪽의 row 데이터를 분리하면 삭제 시 실제 데이터를 당장 이동하지 않고 slot 상태를 바꿔 재사용할 수 있습니다. 현재 구현은 schema의 고정 `row_size`로 직렬화하지만, 이 구조는 향후 가변 길이 row를 지원할 때도 row를 옮긴 뒤 slot offset만 고치는 방식으로 확장할 수 있습니다.

### Pager cache를 둔 이유

상위 계층이 매번 `pread`와 `pwrite`를 호출하면 같은 페이지를 반복해서 읽고 dirty write 시점을 통제하기 어렵습니다. Pager는 256개 frame, `page_id` hash index, pin count, dirty bit와 LRU 교체를 한곳에서 관리하여 저장 계층이 동일한 I/O 규칙을 사용하도록 합니다.

## 어떻게 확인하나

GCC, Make, pthread가 준비된 환경에서 다음 명령을 사용합니다.

```bash
make
./build/minidb demo.db
make test-all
```

이번 macOS 환경의 기본 `make test-all`은 컴파일 후 AddressSanitizer 초기화 내부에서 8분 이상 진행되지 않아 중단했으며, 테스트 통과로 계산하지 않았습니다. 같은 소스를 sanitizer 없이 `/tmp`의 별도 빌드 디렉터리에 컴파일한 뒤 테스트 바이너리를 직접 재실행한 결과는 다음과 같습니다.

| 재실행한 바이너리 | 이번 실행 결과 |
| --- | ---: |
| `test_all` | 76/76 |
| `test_step0` | 24/24 |
| `test_step1` | 72/72 |
| `test_step2` | 52/52 |
| **합계** | **224/224** |

저장소 문서에 적혀 있던 `test_step2`의 `44/44`는 과거 기록이며, 위 표의 `52/52`가 이번에 재실행한 현재 값입니다.

## 한계

- 교육용 DBMS 프로젝트이며 운영 서비스의 사용자 수나 배포 성과를 주장하지 않습니다.
- 단일 table과 자동 생성 `id` index를 중심으로 구현되어 있으며 일반화된 schema constraint와 secondary index가 없습니다.
- transaction isolation level, WAL 기반 crash recovery, 비용 기반 query optimizer가 없습니다.
