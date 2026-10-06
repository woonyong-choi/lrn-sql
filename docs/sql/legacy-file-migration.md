# 기존 단일 테이블 파일 이름 등록

파일 형식 v1에는 테이블 이름이 저장되지 않았다. 이 때문에 잘못된 이름의
`DROP TABLE`이 실제 테이블을 지울 수 있었다. v2 파일은 단일 테이블 이름을
헤더에 저장하고 모든 SQL 문장의 이름을 검사한다.

v1 파일은 그대로 열 수 있지만, 이름이 있는 기존 테이블에 대한 SQL은
변경하지 않고 오류를 반환한다. 기존 행과 페이지 배치는 유지된다.
테이블 이름을 자동으로 추측하거나 첫 SQL의 이름으로 등록하지 않는다.

## 전환

테이블의 원래 이름을 확인한 뒤 서버와 REPL을 종료한다. 파일을 먼저
복사하고, 복사본에서 전환을 시험한다.

```sh
cp data.db data.db.before-v2
./build-nosan/minidb --adopt-table users data.db
printf "SELECT * FROM users LIMIT 1\n.exit\n" | ./build-nosan/minidb data.db
```

`users`는 해당 파일에 원래 있던 테이블 이름으로 바꾼다. 등록 명령은 v1의
비어 있지 않은 테이블에서만 작동하며, 헤더 버전을 2로 바꾸고 지정한
이름을 기록한다. 행과 인덱스 페이지는 다시 쓰지 않는다. 이미 v2인 파일에
재실행하면 오류를 반환한다.

이름을 잘못 등록했거나 전환 뒤 조회가 실패하면 파일을 닫은 상태에서
백업을 복원한다.

```sh
cp data.db.before-v2 data.db
```

`make test-legacy-adopt`는 v1 형식의 임시 파일에서 등록 전 SQL 차단,
명시적 등록, 재열기와 잘못된 이름의 `DROP TABLE` 거절을 확인한다.
