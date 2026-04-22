/*
 * db.h -- db_execute() 경계 함수
 *
 * 서버와 REPL 양쪽에서 호출하는 단일 SQL 실행 진입점.
 * 내부: parse → execute → 결과 반환
 */

#ifndef DB_H
#define DB_H

#include "sql/executor.h"
#include "storage/pager.h"
#include "server/lock_table.h"

/* Row Lock 테이블 초기화/정리 (서버·REPL 시작/종료 시 호출) */
void db_init(void);
void db_destroy(void);

/* 현재 Row Lock 통계 조회 */
lock_stats_t db_lock_stats(void);

/*
 * db_execute - SQL 문자열을 파싱하고 실행한다.
 *
 * 반환된 exec_result_t의 out_buf는 호출자가 free해야 한다.
 * id 기반 단건 연산 시 Row Lock(S/X)을 자동으로 획득/해제한다.
 * 3초 timeout 시 에러를 반환한다.
 */
exec_result_t db_execute(pager_t *pager, const char *sql);

#endif /* DB_H */
