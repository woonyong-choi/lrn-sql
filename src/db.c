/*
 * db.c -- db_execute() 경계 함수 구현
 *
 * parse → execute를 하나로 묶는 래퍼.
 * 서버(worker 스레드)와 REPL(main) 양쪽에서 동일하게 호출한다.
 *
 * 동시성 제어 (Level 2: Engine RWLock):
 *   - SELECT/EXPLAIN → rdlock (읽기 공유)
 *   - INSERT/UPDATE/DELETE/CREATE/DROP → wrlock (쓰기 독점)
 *   - 동일 pager에 대한 SELECT 끼리 동시 실행 가능
 *   - 쓰기는 독점 → B+ tree split 포함 안전
 */

#include "db.h"
#include "sql/parser.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* Level 2: Engine RWLock — SELECT 공유, 나머지 독점 */
static pthread_rwlock_t engine_lock = PTHREAD_RWLOCK_INITIALIZER;

static int is_read_only(statement_type_t type)
{
    return type == STMT_SELECT || type == STMT_EXPLAIN;
}

exec_result_t db_execute(pager_t *pager, const char *sql)
{
    exec_result_t res;
    memset(&res, 0, sizeof(res));

    statement_t stmt;
    if (parse(sql, &stmt) != 0) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message),
                 "오류: SQL 구문을 해석할 수 없습니다");
        return res;
    }

    /* Lock 획득 */
    if (is_read_only(stmt.type)) {
        pthread_rwlock_rdlock(&engine_lock);
    } else {
        pthread_rwlock_wrlock(&engine_lock);
    }

    res = execute(pager, &stmt);

    /* Lock 해제 */
    pthread_rwlock_unlock(&engine_lock);

    return res;
}
