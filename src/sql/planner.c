/*
 * planner.c -- 규칙 기반 쿼리 플래너
 */

#include "sql/planner.h"
#include <string.h>
#include <stdbool.h>

/*
 * 인덱스 범위 스캔 사용 여부.
 * 기본값은 켜짐(true). 벤치마크에서 개선 전(힙 스캔) 경로를 재현하기 위해
 * -DMINIDB_DISABLE_INDEX_RANGE 로 컴파일하면 id 범위 술어가 다시 TABLE_SCAN으로
 * 라우팅된다. 소스는 하나로 유지하면서 before/after 를 같은 코드로 측정하기 위함.
 */
static inline bool index_range_enabled(void)
{
#ifdef MINIDB_DISABLE_INDEX_RANGE
    return false;
#else
    return true;
#endif
}

plan_t planner_create_plan(const statement_t *stmt)
{
    plan_t plan;

    switch (stmt->type) {
        case STMT_CREATE_TABLE:
            plan.access_path = ACCESS_PATH_CREATE_TABLE;
            break;
        case STMT_INSERT:
            plan.access_path = ACCESS_PATH_INSERT;
            break;
        case STMT_SELECT:
            if (stmt->predicate_kind == PREDICATE_ID_EQ && !stmt->select_count
                && !stmt->has_order_by && !stmt->has_limit)
                plan.access_path = ACCESS_PATH_INDEX_LOOKUP;
            else if (stmt->predicate_kind == PREDICATE_ID_RANGE && !stmt->select_count
                     && !stmt->has_order_by && index_range_enabled())
                /* id 범위 조회는 B+tree 리프 순차 순회로 처리 (LIMIT 조기 종료 가능) */
                plan.access_path = ACCESS_PATH_INDEX_RANGE;
            else
                plan.access_path = ACCESS_PATH_TABLE_SCAN;
            break;
        case STMT_DELETE:
            if (stmt->predicate_kind == PREDICATE_ID_EQ)
                plan.access_path = ACCESS_PATH_INDEX_DELETE;
            else
                plan.access_path = ACCESS_PATH_TABLE_SCAN;
            break;
        case STMT_UPDATE:
            if (stmt->predicate_kind == PREDICATE_ID_EQ)
                plan.access_path = ACCESS_PATH_INDEX_UPDATE;
            else
                plan.access_path = ACCESS_PATH_TABLE_SCAN;
            break;
        case STMT_DROP_TABLE:
            plan.access_path = ACCESS_PATH_DROP_TABLE;
            break;
        case STMT_EXPLAIN: {
            statement_t inner;
            memset(&inner, 0, sizeof(inner));
            inner.type = stmt->inner_type;
            inner.predicate_kind = stmt->inner_predicate;
            return planner_create_plan(&inner);
        }
    }

    return plan;
}

const char *access_path_name(access_path_t ap)
{
    switch (ap) {
        case ACCESS_PATH_TABLE_SCAN:    return "TABLE_SCAN";
        case ACCESS_PATH_INDEX_LOOKUP:  return "INDEX_LOOKUP";
        case ACCESS_PATH_INDEX_RANGE:   return "INDEX_RANGE";
        case ACCESS_PATH_INDEX_DELETE:  return "INDEX_DELETE";
        case ACCESS_PATH_INDEX_UPDATE:  return "INDEX_UPDATE";
        case ACCESS_PATH_INSERT:        return "INSERT";
        case ACCESS_PATH_CREATE_TABLE:  return "CREATE_TABLE";
        case ACCESS_PATH_DROP_TABLE:    return "DROP_TABLE";
    }
    return "UNKNOWN";
}
