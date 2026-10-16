# 기여 안내

이슈로 문제나 제안을 먼저 남기고, 합의한 변경을 브랜치와 PR로 보낸다.

## 시작 전에

동작·파일 형식·공개 API가 바뀌는 변경과 버그는 이슈를 먼저 연다. 이슈에는 왜 필요한지, 완료 조건, 하지 않는 것을 적는다. 오타·문서 표현처럼 동작이 바뀌지 않는 변경은 이슈 없이 PR만 보내도 된다.

## 개발 환경

GCC, GNU Make, Python 3, pytest가 필요하다. SQLite 대조 시험에는 `libsqlite3-dev`가 필요하다.

```sh
git clone https://github.com/woonyong-choi/totedb.git
cd totedb
make all
```

## 검사

PR을 열기 전에 아래 검사가 모두 통과해야 한다. CI가 같은 명령을 AddressSanitizer·UndefinedBehaviorSanitizer·ThreadSanitizer 빌드로 실행한다.

```sh
make test-all test-runner test-fault test-result
make SANITIZE=thread BUILD_DIR=build-tsan test-transaction test-session test-ack-queue
make test-sql-diff
```

새 시험은 고치려는 결함을 일부러 넣었을 때 실패해야 한다. PR 본문에 넣어 본 결함과 실패한 시험을 적는다.

## 커밋과 PR

- 브랜치: `{type}/{이슈 번호}-{설명}` 예: `fix/203-identity-after-large-id`
- 커밋 제목: `type(scope): 설명`. 설명은 한국어이고 `추가`·`제거`·`수정`·`변경`·`개선`·`분리`·`통합`·`이동`·`정리`·`갱신`·`적용`·`도입`·`되돌림` 중 하나로 끝낸다. 예: `fix(database): 큰 id를 넣은 뒤 자동 id가 오류로 끝나는 문제 수정`
- type: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `ci`, `chore`, `revert`. 호환이 깨지면 type 뒤에 `!`
- 커밋 본문: 한 줄에 사실 하나인 개조식
- 꼬리: 이슈를 닫으면 `Closes: #{번호}`, 관련만 있으면 `Refs: #{번호}`
- PR 본문: 무엇을 왜 바꿨는지, 검사 결과, 넣어 본 결함

## 문서

동작·계약·설정을 바꾸면 같은 PR에서 [문서 목록](../docs/README.md)의 해당 문서와 README 상태 표를 고친다. 사용자에게 보이는 변경은 [변경 기록](../CHANGELOG.md)에 적는다.
