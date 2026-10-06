# 출처와 기여 경계

이 저장소는 크래프톤 정글 팀 과제(원본: [Jungle-12-303/wk08_1](https://github.com/Jungle-12-303/wk08_1))에서
시작했고, 과제 종료 후 개인 저장소에서 계속 수정·학습·확장하고 있습니다.
팀 코드 전체를 개인 구현으로 주장하지 않습니다. 팀 기간의 기여 구분은 원본 저장소의
커밋 저자와 파일별 diff로 확인합니다.

## 팀 과제

| 항목 | 내용 |
|---|---|
| 원본 팀 저장소 | [Jungle-12-303/wk08_1](https://github.com/Jungle-12-303/wk08_1) |
| 기간 | 2026-04-16 ~ 2026-05-08 (커밋 `40327ef` ~ `93114c8`). 다른 팀원의 마지막 커밋은 2026-04-22 |
| 팀 구성 | 5인. `main`에 병합된 기여자는 4인(최우녕·정범진·이호준·최현진)이고, 나머지 1인의 커밋은 병합되지 않은 브랜치에 남아 있다 — `git shortlog -sne --all`로 확인 |
| 본인 담당 | 저장소 전반의 엔진 코드(parser·planner·executor·pager·bptree·table·lock_table)를 개인 주도로 대부분 직접 구현 |

## 개인 확장 (팀 과제 종료 후)

커밋 범위: [`f70b243`](https://github.com/woonyong-choi/lrn-sql/commit/f70b243) (2026-08-01) ~ `da1624b` (2026-09-23), 23개 커밋.
`git log --author='woonyong' --since=2026-06-01 --reverse --format='%h %ad %s' --date=short` 기준입니다.

| 무엇이 달라졌나 | 커밋 | 파일 |
|---|---|---|
| B+Tree 인덱스 **범위 스캔**(`INDEX_RANGE`) 추가 — `BETWEEN`·부등호 파싱, 접근 경로, 리프 순회 | `7ea5a09` | `bptree.c`, `parser.c`, `planner.c`, `executor.c` |
| 1M 행 INSERT의 **O(N²) 결함 2건** 수정 — 힙 재탐색 힌트, REPL lock 해제 누락 | `e640fdd` | `table.c`, `pager.c`, `main.c`, `executor.c` |
| **sanitizer 선택 빌드**(ASAN/UBSAN)와 테스트를 묶는 CI 게이트 도입 | `f814357`, `f862e3e` | `Makefile`, `.github/workflows/ci.yml` |
| 저장소 정리 — 빌드 산출물·Python 캐시·작업용 스테이징 폴더 제거 | `9b92a14`, `df55747` | `.gitignore` 외 |
| 문서 — 저장 구조 선택 근거, 실행 방법, 출처·기여 경계 명시 | `f70b243`, `cce95b9`, `413db92`, `5b3d044` | `README.md`, `docs/` |
| B+Tree **구조 불변식 property test** 추가 — 무작위 삽입·삭제 후 트리 전체를 걸어 8종 검사, 결함 5종 주입으로 검증 | `239f302` | `tests/test_bptree_property.c` |
| `INDEX_RANGE` **엔드투엔드 테스트** 추가 — 경계·LIMIT·삭제 후, 힙 스캔 결과와 대조 | `239f302` | `tests/test_step1_sql_ext.c` |
| **EXPLAIN이 실행기와 다른 계획을 보고하던 버그** 수정 + 회귀 테스트 | `7f6cb84` | `parser.c`, `planner.c`, `tests/` |
| **규모별 기울기 벤치마크**(`make bench`) — 재현되지 않던 절대 배수를 배가 계수로 교체, 당시 그래프 생성 포함 | `7f6cb84` | `bench/scaling.py` |
| **설계 노트** — 버린 대안과 이유, 정확성 검증의 한계, 다음 병목 | `7f6cb84` | `docs/design.md` |
| **Strict 2PL lock 해제·힙 체인 재탐색 회귀 테스트**(`make test-step3`) | `da1624b` | `tests/test_step3_regression.c` |

팀 과제 종료 시점(`93114c8`) 대비 엔진 코드 변경은 `src/` 기준 7개 파일 **+462/-88행**입니다
(`git diff --shortstat 93114c8 HEAD -- src/`, 2026-09-23 측정).
