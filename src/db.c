/*
 * db.c -- db_execute() 경계 함수 구현
 *
 * parse → execute를 하나로 묶는 래퍼.
 * 서버(worker 스레드)와 REPL(main) 양쪽에서 동일하게 호출한다.
 *
 * 동시성 제어 4-레이어:
 *   Level 4 — Row Lock + Range Lock (Strict 2PL, 3초 timeout)
 *     Point Lock: id 기반 단건 연산에 S/X lock.
 *       SELECT WHERE id=X → S lock, UPDATE/DELETE WHERE id=X → X lock.
 *     Range Lock (Next-Key Lock): id 기반 범위 연산에 range lock.
 *       SELECT WHERE id>5 → S range [6, MAX], UPDATE WHERE id<10 → X range [1, 9]
 *     Gap Check: INSERT 시 새 id가 기존 range lock에 걸리면 대기.
 *   Level 3 — Engine RWLock
 *     DML (SELECT/INSERT/UPDATE/DELETE/EXPLAIN) → rdlock (동시 실행)
 *     DDL (CREATE TABLE/DROP TABLE) → wrlock (독점)
 *   Level 2 — Page Latch (pager 프레임별 rwlock)
 *     B+ tree 래치 커플링으로 페이지 단위 동시성 보장.
 *   Level 1 — Pager Mutex (pager.c 내부)
 *     프레임 메타데이터(pin, tick, dirty) 보호
 *   Header Lock — pager->header_lock (mutex)
 *     next_id, row_count 등 헤더 카운터 원자적 갱신
 */

#include "db.h"
#include "sql/parser.h"
#include "server/lock_table.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <pthread.h>

/* Level 4: Row Lock + Range Lock */
static lock_table_t g_lock_table;

/* Level 3: Engine RWLock — DML 공유, DDL 독점 */
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

lock_table_t *db_get_lock_table(void)
{
    return &g_lock_table;
}

/*
 * DDL 판별: CREATE TABLE / DROP TABLE만 wrlock (스키마 변경은 독점).
 * DML(SELECT/INSERT/UPDATE/DELETE/EXPLAIN)은 rdlock으로 동시 실행하되,
 * 래치 커플링(B+ tree)과 header_lock(next_id/row_count)으로 동시성을 보장한다.
 */
static int is_ddl(statement_type_t type)
{
    return type == STMT_CREATE_TABLE || type == STMT_DROP_TABLE;
}

/*
 * lock 요청 결정: statement를 분석하여 어떤 lock을 걸어야 하는지 결정한다.
 *
 * Point Lock:
 *   WHERE id = X → SELECT=S, UPDATE/DELETE=X
 *
 * Range Lock (Next-Key Lock):
 *   WHERE id > V  → range [V+1, UINT64_MAX]
 *   WHERE id >= V → range [V,   UINT64_MAX]
 *   WHERE id < V  → range [1,   V-1]
 *   WHERE id <= V → range [1,   V]
 *
 * INSERT:
 *   lock은 여기서 걸지 않음 — executor 내부에서 id 할당 후 gap check.
 */
typedef struct {
    int         need_lock;   /* lock이 필요한가 */
    int         is_range;    /* 0=point, 1=range */
    lock_mode_t mode;        /* S 또는 X */
    uint64_t    point_id;    /* point lock 대상 id */
    uint64_t    range_low;   /* range lock 하한 */
    uint64_t    range_high;  /* range lock 상한 */
} lock_request_t;

static lock_request_t determine_lock(const statement_t *stmt)
{
    lock_request_t req;
    memset(&req, 0, sizeof(req));

    /* lock mode 결정 */
    lock_mode_t mode;
    switch (stmt->type) {
        case STMT_SELECT:  mode = LOCK_S; break;
        case STMT_UPDATE:
        case STMT_DELETE:  mode = LOCK_X; break;
        default:           return req;  /* INSERT, DDL 등은 여기서 lock 안 검 */
    }

    /* Case 1: Point Lock — WHERE id = X */
    if (stmt->predicate_kind == PREDICATE_ID_EQ) {
        req.need_lock = 1;
        req.is_range  = 0;
        req.mode      = mode;
        req.point_id  = stmt->pred_id;
        return req;
    }

    /* Case 2: Range Lock — WHERE id > X, id < X 등 */
    if (stmt->predicate_kind == PREDICATE_FIELD_CMP
        && strncmp(stmt->pred_field, "id", 32) == 0) {

        uint64_t val = (uint64_t)atoll(stmt->pred_value);
        uint64_t low = 0, high = 0;

        switch (stmt->pred_op) {
            case OP_GT: low = val + 1; high = UINT64_MAX; break;
            case OP_GE: low = val;     high = UINT64_MAX; break;
            case OP_LT: low = 1;       high = (val > 1) ? val - 1 : 0; break;
            case OP_LE: low = 1;       high = val; break;
            case OP_NE: return req;  /* != 는 range lock 생략 */
            case OP_EQ: return req;  /* 여기 오면 안 됨 (PREDICATE_ID_EQ로 분류) */
        }

        if (low > 0 && high >= low) {
            req.need_lock  = 1;
            req.is_range   = 1;
            req.mode       = mode;
            req.range_low  = low;
            req.range_high = high;
        }
        return req;
    }

    return req;
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

    /* Level 4: Lock 획득 (point 또는 range) */
    lock_request_t lreq = determine_lock(&stmt);
    if (lreq.need_lock) {
        int rc;
        if (lreq.is_range) {
            rc = lock_acquire_range(&g_lock_table,
                                    lreq.range_low, lreq.range_high, lreq.mode);
        } else {
            rc = lock_acquire(&g_lock_table, lreq.point_id, lreq.mode);
        }
        if (rc != 0) {
            res.status = -1;
            if (lreq.is_range) {
                snprintf(res.message, sizeof(res.message),
                         "오류: range lock 획득 timeout ([%" PRIu64 ", %" PRIu64 "], 3초 초과)",
                         lreq.range_low, lreq.range_high);
            } else {
                snprintf(res.message, sizeof(res.message),
                         "오류: row lock 획득 timeout (id=%" PRIu64 ", 3초 초과)",
                         lreq.point_id);
            }
            return res;
        }
    }

    /* Level 3: Engine RWLock 획득 */
    if (is_ddl(stmt.type)) {
        pthread_rwlock_wrlock(&engine_lock);
    } else {
        pthread_rwlock_rdlock(&engine_lock);
    }

    res = execute(pager, &stmt);

    /* Level 3: Engine RWLock 해제 */
    pthread_rwlock_unlock(&engine_lock);

    /* Level 4: Strict 2PL — 문장 종료 시 보유 lock 전부 해제 (point + range) */
    lock_release_all(&g_lock_table);

    return res;
}
