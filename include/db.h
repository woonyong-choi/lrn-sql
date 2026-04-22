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

/*
 * db_execute - SQL 문자열을 파싱하고 실행한다.
 *
 * 반환된 exec_result_t의 out_buf는 호출자가 free해야 한다.
 */
exec_result_t db_execute(pager_t *pager, const char *sql);

#endif /* DB_H */
