#!/bin/bash
# 1M 행 전/후 재측정 (README "버전업된 모습" 표의 재현). 소요: 약 6분 (개선 전 INSERT 가 회당 약 90초)
# - after   : 현재 코드
# - noindex : 현재 코드 + -DMINIDB_DISABLE_INDEX_RANGE (Range 가 힙 스캔)
# - prefix  : 수정 커밋 993d4d8 의 직전 코드 (INSERT O(N^2) 결함 2건이 남은 상태)
set -e
cd "$(dirname "$0")/.."
SRCS="storage/pager.c storage/schema.c storage/table.c storage/bptree.c sql/parser.c sql/planner.c sql/executor.c server/http.c server/server.c server/lock_table.c db.c main.c"
F=""; for f in $SRCS; do F="$F src/$f"; done
mkdir -p build-o2
cc -O2 -Wall -Iinclude -o build-o2/minidb-after $F -lpthread
cc -O2 -Wall -Iinclude -DMINIDB_DISABLE_INDEX_RANGE -o build-o2/minidb-noindex $F -lpthread
PRE=$(mktemp -d); git archive 993d4d8^ | tar -x -C "$PRE"
(cd "$PRE" && F2=""; for f in $SRCS; do F2="$F2 src/$f"; done; cc -O2 -Wall -Iinclude -o "$OLDPWD/build-o2/minidb-prefix" $F2 -lpthread)
rm -rf "$PRE"
for v in after noindex prefix; do
  lbl=after; [ "$v" != after ] && lbl=before
  echo "=== $v"; python3 bench/bench_minidb_param.py --rows 1000000 --label $lbl --binary build-o2/minidb-$v
done
