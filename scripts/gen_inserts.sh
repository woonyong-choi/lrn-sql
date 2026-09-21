#!/bin/bash
# usage: scripts/gen_inserts.sh N — users 테이블에 넣을 INSERT 문 N개를 출력한다
seq 1 "${1:-1000}" | awk '{printf "INSERT INTO users VALUES (\x27user%d\x27, \x27user%d@example.com\x27, %d);\n", $1, $1, 20 + $1 % 40}'
