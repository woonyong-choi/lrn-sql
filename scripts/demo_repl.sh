#!/bin/bash
# 터미널 데모: 테이블 생성 → INSERT 1,000건 → 범위 SELECT + 실행 시간. 입력은 파이프로 넣는다.
# 기본 빌드는 sanitizer 가 켜져 있어 데모는 SANITIZE= 로 build-demo 에 따로 빌드한다.
cd "$(dirname "$0")/.." || exit 1
make -s SANITIZE= BUILD_DIR=build-demo build-demo/minidb >/dev/null || exit 1
rm -f /tmp/demo.db
DB=build-demo/minidb; TIMEFORMAT='  real %3Rs'
show() { printf '\033[1;32m$\033[0m %s\n' "$1"; }
echo "# lrn-sql — 테이블 생성 → INSERT 1,000건 → 범위 SELECT (실행 시간 출력)"
sleep 2.8

CREATE="CREATE TABLE users (name VARCHAR(32), email VARCHAR(32), age INT);"
show "echo \"$CREATE\" | $DB /tmp/demo.db"
echo "$CREATE" | $DB /tmp/demo.db
sleep 2.2

show "time (scripts/gen_inserts.sh 1000 | $DB /tmp/demo.db | tail -2)"
time (scripts/gen_inserts.sh 1000 | $DB /tmp/demo.db | tail -2)
sleep 2.6

Q=$'.debug\nSELECT COUNT(*) FROM users;\nSELECT * FROM users WHERE id >= 996;'
show "$DB /tmp/demo.db <<'SQL'"
printf '%s\n' "$Q" | sed 's/^/> /'
echo "> SQL"
printf '%s\n' "$Q" | $DB /tmp/demo.db
sleep 4.0
