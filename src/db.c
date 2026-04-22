/*
 * db.c -- db_execute() 경계 함수 구현
 *
 * parse → execute를 하나로 묶는 래퍼.
 * 서버(worker 스레드)와 REPL(main) 양쪽에서 동일하게 호출한다.
 *
 * 동시성 제어 3-레이어:
 *   Level 3 — Row Lock (Strict 2PL, 3초 timeout)
 *     id 기반 단건 연산에 S/X lock을 건다.
 *     SELECT WHERE id=X → S lock, UPDATE/DELETE WHERE id=X → X lock.
 *   Level 2 — Engine RWLock
 *     SELECT/EXPLAIN → rdlock (읽기 공유)
 *     INSERT/UPDATE/DELETE/CREATE/DROP → wrlock (쓰기 독점)
 *   Level 1 — Pager Mutex (pager.c 내부)
 *     프레임 메타데이터(pin, tick, dirty) 보호
 */

#include "db.h"
#include "sql/parser.h"
#include "server/lock_table.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>

/* Level 3: Row Lock — id 기반 단건 연산에 S/X lock */
static lock_table_t g_lock_table;

/* Level 2: Engine RWLock — SELECT 공유, 나머지 독점 */
static pthread_rwlock_t engine_lock = PTHREAD_RWLOCK_INITIALIZER;

void db_init(void)
{
    lock_table_init(&g_lock_table);
}

void db_destroy(void)
{
    lock_table_destroy(&g_lock_table);
}

lock_stats_t db_lock_stats(void)
{
    return lock_table_stats(&g_lock_table);
}

static int is_read_only(statement_type_t type)
{
    return type == STMT_SELECT || type == STMT_EXPLAIN;
}

/* id 기반 단건 연산인지 판별하고, 필요한 lock mode를 결정 */
static int needs_row_lock(const statement_t *stmt, lock_mode_t *mode)
{
    if (stmt->predicate_kind != PREDICATE_ID_EQ)
        return 0;

    switch (stmt->type) {
        case STMT_SELECT:
            *mode = LOCK_S;
            return 1;
        case STMT_UPDATE:
        case STMT_DELETE:
            *mode = LOCK_X;
            return 1;
        default:
            return 0;
    }
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

    /* Level 3: Row Lock 획득 (id 기반 단건 연산만) */
    lock_mode_t row_mode;
    int has_row_lock = needs_row_lock(&stmt, &row_mode);
    if (has_row_lock) {
        if (lock_acquire(&g_lock_table, stmt.pred_id, row_mode) != 0) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message),
                     "오류: row lock 획득 timeout (id=%" PRIu64 ", 3초 초과)",
                     stmt.pred_id);
            return res;
        }
    }

    /* Level 2: Engine RWLock 획득 */
    if (is_read_only(stmt.type)) {
        pthread_rwlock_rdlock(&engine_lock);
    } else {
        pthread_rwlock_wrlock(&engine_lock);
    }

    res = execute(pager, &stmt);

    /* Level 2: Engine RWLock 해제 */
    pthread_rwlock_unlock(&engine_lock);

    /* Level 3: Strict 2PL — 트랜잭션 종료 시 보유 lock 전부 해제 */
    if (has_row_lock) {
        lock_release_all(&g_lock_table);
    }

    return res;
}
