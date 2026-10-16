/*
 * database_group.c — GROUP BY·HAVING과 집계 함수(해시 집계).
 */
#include "database_exec.h"

static int compare_group_keys(const void *ctx, const value_t *a, const value_t *b);

/* ── 그룹과 집계 ── */

static bool same_name(const sqlstmt_name_t *a, const sqlstmt_name_t *b)
{
    return a->size == b->size && (a->size == 0 || memcmp(a->bytes, b->bytes, a->size) == 0);
}

/* 두 식이 같은 모양인지(GROUP BY 식과 출력 식의 대응). 한쪽에만 테이블 이름이 붙은 열도 같다고 본다 */
static bool same_expr(const sqlexpr_t *a, const sqlexpr_t *b)
{
    if (a->kind != b->kind || a->arg_count != b->arg_count) return false;
    switch (a->kind) {
        case SQLEXPR_LITERAL: return values_equal(&a->value.view, &b->value.view) || (a->value.view.type == VALUE_NULL && b->value.view.type == VALUE_NULL);
        case SQLEXPR_COLUMN:
            if (!same_name(&a->name, &b->name)) return false;
            return a->qualifier.size == 0 || b->qualifier.size == 0 || same_name(&a->qualifier, &b->qualifier);
        case SQLEXPR_FUNCTION:
            if (!same_name(&a->name, &b->name) || a->star != b->star || a->distinct != b->distinct) return false;
            break;
        case SQLEXPR_CAST:
            if (a->cast_type != b->cast_type) return false;
            break;
        default:
            if (a->op != b->op || a->negated != b->negated || a->has_operand != b->has_operand || a->has_else != b->has_else) {
                return false;
            }
            break;
    }
    for (size_t i = 0; i < a->arg_count; i++) {
        if (!same_expr(a->args[i], b->args[i])) return false;
    }
    return true;
}

size_t group_match(void *ctx, const sqlexpr_t *expr, value_type_t *type)
{
    const group_plan_t *plan = ctx;
    for (size_t i = 0; i < plan->agg_count; i++) {
        if (plan->aggs[i].call == expr) {
            *type = plan->aggs[i].type;
            return plan->key_count + i;
        }
    }
    for (size_t i = 0; i < plan->key_count; i++) {
        if (same_expr(plan->keys[i], expr)) {
            *type = plan->key_types[i];
            return i;
        }
    }
    return SQLEVAL_NO_COLUMN;
}

/* 그룹 질의에서 집계 밖의 열: 테이블에 있으면 '묶지 않은 열' 오류, 없으면 없는 열 */
size_t group_resolve(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name, value_type_t *type)
{
    const group_plan_t *plan = ctx;
    size_t column = resolve_column((void *)plan->table, qualifier, name, type);
    return column == SQLEVAL_NO_COLUMN || column == SQLEVAL_AMBIGUOUS ? column : SQLEVAL_UNGROUPED;
}

/* expr 안의 집계 호출을 모은다. 집계 안의 집계는 거절한다 */
static bool has_aggregate(const sqlexpr_t *expr)
{
    if (expr == NULL) return false;
    if (sqleval_is_aggregate(expr)) return true;
    for (size_t i = 0; i < expr->arg_count; i++) {
        if (has_aggregate(expr->args[i])) return true;
    }
    return false;
}

static database_exec_status_t collect_aggregates(database_t *db, const sqlexpr_t *expr, const resolver_t *table,
                                                 group_plan_t *plan)
{
    if (expr == NULL) return DATABASE_EXEC_OK;
    if (!sqleval_is_aggregate(expr)) {
        for (size_t i = 0; i < expr->arg_count; i++) {
            database_exec_status_t status = collect_aggregates(db, expr->args[i], table, plan);
            if (status != DATABASE_EXEC_OK) return status;
        }
        return DATABASE_EXEC_OK;
    }
    if (expr->arg_count > 0 && has_aggregate(expr->args[0])) {
        return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_AGGREGATE_MISUSE, "집계 함수 안에 집계 함수를 쓸 수 없습니다");
    }
    if (plan->agg_count == plan->agg_cap) {
        size_t cap = plan->agg_cap == 0 ? 8 : plan->agg_cap * 2;
        if (!q_reserve(db, cap * sizeof(aggregate_t))) return DATABASE_EXEC_NOMEM;
        aggregate_t *grown = realloc(plan->aggs, cap * sizeof(*grown));
        if (grown == NULL) {
            q_release(db, cap * sizeof(aggregate_t));
            return DATABASE_EXEC_NOMEM;
        }
        q_release(db, plan->agg_cap * sizeof(aggregate_t));
        plan->aggs = grown;
        plan->agg_cap = cap;
    }
    aggregate_t *agg = &plan->aggs[plan->agg_count];
    memset(agg, 0, sizeof(*agg));
    agg->call = expr;
    static const char *const NAMES[] = {"COUNT", "SUM", "AVG", "MIN", "MAX"};
    for (size_t k = 0; k < 5; k++) {
        if (expr->name.size == strlen(NAMES[k]) && memcmp(expr->name.bytes, NAMES[k], expr->name.size) == 0) {
            agg->kind = (agg_kind_t)k;
        }
    }
    plan->agg_count++;
    database_exec_status_t status = DATABASE_EXEC_OK;
    value_type_t arg_type = VALUE_INTEGER;
    if (!expr->star) {
        if (!compile_expr(db, expr->args[0], table, &agg->arg, &status)) return status;
        arg_type = sqleval_type(agg->arg);
    }
    switch (agg->kind) {
        case AGG_COUNT: agg->type = VALUE_INTEGER; break;
        case AGG_SUM:
        case AGG_AVG:
            if (arg_type != VALUE_INTEGER && arg_type != VALUE_REAL && arg_type != VALUE_NULL) {
                return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "%.*s는 숫자만 받습니다",
                                   name_len(expr->name.size), (const char *)expr->name.bytes);
            }
            agg->type = agg->kind == AGG_AVG || arg_type == VALUE_REAL ? VALUE_REAL : VALUE_INTEGER;
            break;
        default: agg->type = arg_type; break;
    }
    return DATABASE_EXEC_OK;
}

void group_plan_free(database_t *db, group_plan_t *plan)
{
    for (size_t i = 0; i < plan->key_count; i++) sqleval_free(plan->key_progs != NULL ? plan->key_progs[i] : NULL);
    q_free(db, plan->keys, plan->keys_bytes);
    q_free(db, plan->key_progs, plan->keys_bytes);
    q_free(db, plan->key_types, plan->keys_bytes);
    for (size_t i = 0; i < plan->agg_count; i++) sqleval_free(plan->aggs[i].arg);
    q_free(db, plan->aggs, plan->agg_cap * sizeof(aggregate_t));
    memset(plan, 0, sizeof(*plan));
}

/* GROUP BY 식을 정한다. 정수 리터럴은 선택 목록 번호, 테이블에 없는 이름이 별칭과 같으면 그 식이다 */
database_exec_status_t plan_groups(database_t *db, const sqlstmt_select_t *select, const resolver_t *table,
                                          group_plan_t *plan)
{
    plan->table = table;
    size_t count = select->group_count;
    plan->keys_bytes = (count > 0 ? count : 1) * sizeof(void *);
    plan->keys = q_calloc(db, count > 0 ? count : 1, sizeof(void *));
    plan->key_progs = q_calloc(db, count > 0 ? count : 1, sizeof(void *));
    plan->key_types = q_calloc(db, count > 0 ? count : 1, sizeof(void *));
    if (plan->keys == NULL || plan->key_progs == NULL || plan->key_types == NULL) return DATABASE_EXEC_NOMEM;
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (size_t g = 0; g < count; g++) {
        const sqlexpr_t *expr = select->group_by[g];
        if (expr->kind == SQLEXPR_LITERAL && expr->value.view.type == VALUE_INTEGER) {
            int64_t position = expr->value.view.integer;
            if (position < 1 || (uint64_t)position > select->item_count || select->items[position - 1].star) {
                return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_BAD_ORDER_POSITION,
                                   "GROUP BY 번호가 선택 목록 밖입니다: %lld", (long long)position);
            }
            expr = select->items[position - 1].expr;
        } else if (expr->kind == SQLEXPR_COLUMN && expr->qualifier.size == 0) {
            value_type_t unused;
            bool is_column = resolve_column((void *)table, &expr->qualifier, &expr->name, &unused) != SQLEVAL_NO_COLUMN;
            for (size_t i = 0; i < select->item_count && !is_column; i++) {
                if (!select->items[i].star && same_name(&select->items[i].alias, &expr->name)) {
                    expr = select->items[i].expr;
                    break;
                }
            }
        }
        plan->keys[g] = expr;
        plan->key_count++;
        if (!compile_expr(db, expr, table, &plan->key_progs[g], &status)) return status;
        plan->key_types[g] = sqleval_type(plan->key_progs[g]);
    }
    for (size_t i = 0; i < select->item_count && status == DATABASE_EXEC_OK; i++) {
        status = collect_aggregates(db, select->items[i].expr, table, plan);
    }
    if (status == DATABASE_EXEC_OK) status = collect_aggregates(db, select->having, table, plan);
    for (size_t k = 0; k < select->order_count && status == DATABASE_EXEC_OK; k++) {
        status = collect_aggregates(db, select->order[k].expr, table, plan);
    }
    return status;
}

static size_t group_width(const group_plan_t *plan) { return plan->key_count + plan->agg_count; }

static uint64_t mix_hash(uint64_t hash, uint64_t word)
{
    hash ^= word + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash;
}

/* sqleval_compare로 같은 값은 같은 해시. 정수값 REAL은 같은 정수로 본다 */
static uint64_t value_hash(const value_view_t *v)
{
    switch (v->type) {
        case VALUE_NULL: return UINT64_C(0x6e756c6c);
        case VALUE_INTEGER: return mix_hash(1, (uint64_t)v->integer);
        case VALUE_REAL:
            if (v->real == floor(v->real) && v->real >= -9223372036854775808.0 && v->real < 9223372036854775808.0) {
                return mix_hash(1, (uint64_t)(int64_t)v->real);
            } else {
                uint64_t bits;
                memcpy(&bits, &v->real, sizeof(bits));
                return mix_hash(2, bits);
            }
        default: {
            uint64_t hash = UINT64_C(1469598103934665603) ^ (uint64_t)v->type;
            for (size_t i = 0; i < v->size_bytes; i++) hash = (hash ^ v->bytes[i]) * UINT64_C(1099511628211);
            return mix_hash(3, hash);
        }
    }
}

static bool group_table_grow(group_rows_t *rows)
{
    database_t *db = rows->db;
    size_t cap = rows->slot_cap == 0 ? 64 : rows->slot_cap * 2;
    if (cap > SIZE_MAX / sizeof(group_t *)) return false;
    group_t **slots = q_calloc(db, cap, sizeof(group_t *));
    if (slots == NULL) return false;
    for (size_t i = 0; i < rows->count; i++) {
        size_t at = (size_t)rows->groups[i]->hash & (cap - 1);
        while (slots[at] != NULL) at = (at + 1) & (cap - 1);
        slots[at] = rows->groups[i];
    }
    q_free(db, rows->slots, rows->slot_cap * sizeof(group_t *));
    rows->slots = slots;
    rows->slot_cap = cap;
    return true;
}

/* 키가 같은 그룹을 찾고, 없으면 rows->keys를 넘겨받아 새 그룹을 만든다 */
static database_exec_status_t group_find(group_rows_t *rows, uint64_t hash, group_t **out)
{
    database_t *db = rows->db;
    const group_plan_t *plan = rows->plan;
    if (rows->slot_cap != 0) {
        for (size_t at = (size_t)hash & (rows->slot_cap - 1); rows->slots[at] != NULL;
             at = (at + 1) & (rows->slot_cap - 1)) {
            group_t *group = rows->slots[at];
            if (group->hash == hash && compare_group_keys(plan, group->cells, rows->keys) == 0) {
                *out = group;
                return DATABASE_EXEC_OK;
            }
        }
    }
    /* 표를 반 넘게 채우지 않는다 */
    if ((rows->count + 1) * 2 > rows->slot_cap && !group_table_grow(rows)) return DATABASE_EXEC_NOMEM;
    if (rows->count == rows->cap) {
        size_t cap = rows->cap == 0 ? 64 : rows->cap * 2;
        group_t **grown = q_malloc(db, cap * sizeof(group_t *));
        if (grown == NULL) return DATABASE_EXEC_NOMEM;
        if (rows->count > 0) memcpy(grown, rows->groups, rows->count * sizeof(group_t *));
        q_free(db, rows->groups, rows->cap * sizeof(group_t *));
        rows->groups = grown;
        rows->cap = cap;
    }
    size_t width = group_width(plan);
    group_t *group = q_calloc(db, 1, sizeof(group_t));
    value_t *cells = group == NULL ? NULL : q_calloc(db, width > 0 ? width : 1, sizeof(value_t));
    agg_state_t *states = cells == NULL ? NULL : q_calloc(db, plan->agg_count > 0 ? plan->agg_count : 1, sizeof(agg_state_t));
    if (states == NULL) {
        q_free(db, cells, (width > 0 ? width : 1) * sizeof(value_t));
        q_free(db, group, sizeof(group_t));
        return DATABASE_EXEC_NOMEM;
    }
    if (plan->key_count > 0) {
        memcpy(cells, rows->keys, plan->key_count * sizeof(value_t));
        memset(rows->keys, 0, plan->key_count * sizeof(value_t));
    }
    *group = (group_t){hash, cells, states};
    size_t at = (size_t)hash & (rows->slot_cap - 1);
    while (rows->slots[at] != NULL) at = (at + 1) & (rows->slot_cap - 1);
    rows->slots[at] = group;
    rows->groups[rows->count++] = group;
    *out = group;
    return DATABASE_EXEC_OK;
}

/* 집계 인자 값 하나를 그룹 상태에 더한다. value는 계상된 값이며 넘겨받거나 비운다 */
static database_exec_status_t agg_add(database_t *db, const aggregate_t *agg, agg_state_t *state, value_t *value)
{
    if (value->view.type == VALUE_NULL) return DATABASE_EXEC_OK;
    if (agg->call->distinct) {
        if (state->seen_count == state->seen_cap) {
            size_t cap = state->seen_cap == 0 ? 8 : state->seen_cap * 2;
            value_t *grown = q_malloc(db, cap * sizeof(value_t));
            if (grown == NULL) return DATABASE_EXEC_NOMEM;
            if (state->seen_count > 0) memcpy(grown, state->seen, state->seen_count * sizeof(value_t));
            q_free(db, state->seen, state->seen_cap * sizeof(value_t));
            state->seen = grown;
            state->seen_cap = cap;
        }
        state->seen[state->seen_count++] = *value;
        memset(value, 0, sizeof(*value));
        return DATABASE_EXEC_OK;
    }
    state->present++;
    if (agg->kind == AGG_MIN || agg->kind == AGG_MAX) {
        int order = state->present == 1 ? 0 : sqleval_compare(&value->view, &state->best.view);
        if (state->present == 1 || (agg->kind == AGG_MIN ? order < 0 : order > 0)) {
            owned_clear(db, &state->best, 1);
            state->best = *value;
            memset(value, 0, sizeof(*value));
        }
    } else if (value->view.type == VALUE_INTEGER) {
        state->whole += value->view.integer;
    } else if (value->view.type == VALUE_REAL) {
        state->real += value->view.real;
    }
    return DATABASE_EXEC_OK;
}

database_exec_status_t group_collect(void *context, const value_t *row)
{
    group_rows_t *rows = context;
    database_t *db = rows->db;
    const group_plan_t *plan = rows->plan;
    if (rows->keys == NULL && plan->key_count > 0) {
        rows->keys = q_calloc(db, plan->key_count, sizeof(value_t));
        if (rows->keys == NULL) return DATABASE_EXEC_NOMEM;
    }
    database_exec_status_t status = DATABASE_EXEC_OK;
    uint64_t hash = 0;
    for (size_t g = 0; g < plan->key_count && status == DATABASE_EXEC_OK; g++) {
        status = select_value(db, plan->key_progs[g], 0, row, &rows->keys[g]);
        if (status == DATABASE_EXEC_OK) hash = mix_hash(hash, value_hash(&rows->keys[g].view));
    }
    group_t *group = NULL;
    if (status == DATABASE_EXEC_OK) status = group_find(rows, hash, &group);
    owned_clear(db, rows->keys, plan->key_count);  /* 새 그룹이 넘겨받았으면 이미 비어 있다 */
    for (size_t a = 0; a < plan->agg_count && status == DATABASE_EXEC_OK; a++) {
        const aggregate_t *agg = &plan->aggs[a];
        if (agg->arg == NULL) {
            group->states[a].present++;
            continue;
        }
        value_t value = {0};
        status = select_value(db, agg->arg, 0, row, &value);
        if (status == DATABASE_EXEC_OK) status = agg_add(db, agg, &group->states[a], &value);
        owned_clear(db, &value, 1);
    }
    return status;
}

static void agg_state_clear(database_t *db, agg_state_t *state)
{
    owned_clear(db, &state->best, 1);
    owned_clear(db, state->seen, state->seen_count);
    q_free(db, state->seen, state->seen_cap * sizeof(value_t));
    memset(state, 0, sizeof(*state));
}

void group_rows_free(group_rows_t *rows)
{
    database_t *db = rows->db;
    const group_plan_t *plan = rows->plan;
    size_t width = group_width(plan);
    for (size_t i = 0; i < rows->count; i++) {
        group_t *group = rows->groups[i];
        owned_clear(db, group->cells, width);
        for (size_t a = 0; a < plan->agg_count; a++) agg_state_clear(db, &group->states[a]);
        q_free(db, group->states, (plan->agg_count > 0 ? plan->agg_count : 1) * sizeof(agg_state_t));
        q_free(db, group->cells, (width > 0 ? width : 1) * sizeof(value_t));
        q_free(db, group, sizeof(group_t));
    }
    if (rows->keys != NULL) {
        owned_clear(db, rows->keys, plan->key_count);
        q_free(db, rows->keys, plan->key_count * sizeof(value_t));
    }
    q_free(db, rows->groups, rows->cap * sizeof(group_t *));
    q_free(db, rows->slots, rows->slot_cap * sizeof(group_t *));
    memset(rows, 0, sizeof(*rows));
}

/* 안정 병합 정렬. cmp는 행 포인터 둘을 비교한다 */
void sort_rows(value_t **rows, value_t **scratch, size_t count, row_compare_fn cmp, const void *ctx)
{
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) scratch[k++] = cmp(ctx, rows[j], rows[i]) < 0 ? rows[j++] : rows[i++];
            while (i < mid) scratch[k++] = rows[i++];
            while (j < hi) scratch[k++] = rows[j++];
        }
        memcpy(rows, scratch, count * sizeof(*rows));
    }
}

static int compare_group_keys(const void *ctx, const value_t *a, const value_t *b)
{
    const group_plan_t *plan = ctx;
    for (size_t g = 0; g < plan->key_count; g++) {
        int order = sqleval_compare(&a[g].view, &b[g].view);
        if (order != 0) return order;
    }
    return 0;
}

int compare_first_value(const void *ctx, const value_t *a, const value_t *b)
{
    (void)ctx;
    return sqleval_compare(&a->view, &b->view);
}

/* 그룹 하나의 집계 결과를 out에 만든다(계상하지 않은 값). DISTINCT 값은 정렬해 중복을 없앤 뒤 더한다 */
static database_exec_status_t agg_finish(database_t *db, const aggregate_t *agg, agg_state_t *state, value_t *out)
{
    if (agg->call->distinct && state->seen_count > 0) {
        value_t **order = q_malloc(db, state->seen_count * sizeof(value_t *));
        value_t **scratch = order == NULL ? NULL : q_malloc(db, state->seen_count * sizeof(value_t *));
        if (scratch == NULL) {
            q_free(db, order, state->seen_count * sizeof(value_t *));
            return DATABASE_EXEC_NOMEM;
        }
        for (size_t i = 0; i < state->seen_count; i++) order[i] = &state->seen[i];
        sort_rows(order, scratch, state->seen_count, compare_first_value, NULL);
        q_free(db, scratch, state->seen_count * sizeof(value_t *));
        agg_state_t folded = {0};
        database_exec_status_t status = DATABASE_EXEC_OK;
        for (size_t i = 0; i < state->seen_count && status == DATABASE_EXEC_OK; i++) {
            if (i > 0 && sqleval_compare(&order[i - 1]->view, &order[i]->view) == 0) continue;
            value_t copy = {0};
            if (q_clone(db, &order[i]->view, &copy) != VALUE_OK) status = DATABASE_EXEC_NOMEM;
            else {
                aggregate_t plain = *agg;
                sqlexpr_t call = *agg->call;
                call.distinct = false;
                plain.call = &call;
                status = agg_add(db, &plain, &folded, &copy);
                owned_clear(db, &copy, 1);
            }
        }
        q_free(db, order, state->seen_count * sizeof(value_t *));
        if (status == DATABASE_EXEC_OK) status = agg_finish(db, agg, &folded, out);
        agg_state_clear(db, &folded);
        return status;
    }
    value_status_t set = VALUE_OK;
    if (agg->kind == AGG_COUNT) {
        set = value_set_integer(out, state->present);
    } else if (state->present == 0) {
        value_clear(out);
    } else if (agg->kind == AGG_MIN || agg->kind == AGG_MAX) {
        set = value_clone(&state->best.view, out);
    } else if (agg->kind == AGG_SUM && agg->type == VALUE_INTEGER) {
        if (state->whole > INT64_MAX || state->whole < INT64_MIN) {
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_ARITHMETIC_OVERFLOW, "SUM이 정수 범위를 넘습니다");
        }
        set = value_set_integer(out, (int64_t)state->whole);
    } else {
        double total = (double)state->whole + state->real;
        set = value_set_real(out, agg->kind == AGG_AVG ? total / (double)state->present : total);
    }
    if (set == VALUE_NOMEM) return DATABASE_EXEC_NOMEM;
    if (set != VALUE_OK) {
        return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_ARITHMETIC_OVERFLOW, "집계 결과가 범위를 넘습니다");
    }
    return DATABASE_EXEC_OK;
}

/* 그룹을 키 순서로 안정 병합 정렬한다 */
static void sort_groups(const group_plan_t *plan, group_t **groups, group_t **scratch, size_t count)
{
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                scratch[k++] = compare_group_keys(plan, groups[j]->cells, groups[i]->cells) < 0 ? groups[j++] : groups[i++];
            }
            while (i < mid) scratch[k++] = groups[i++];
            while (j < hi) scratch[k++] = groups[j++];
        }
        memcpy(groups, scratch, count * sizeof(*groups));
    }
}

/* 그룹을 키 순서로 정렬해 집계를 마무리하고 HAVING을 통과한 그룹 행을 내보낸다 */
database_exec_status_t group_flush(database_t *db, const group_plan_t *plan, group_rows_t *rows,
                                          const sqleval_prog_t *having, select_ctx_t *ctx)
{
    /* GROUP BY가 없으면 행이 없어도 그룹 하나다 */
    if (plan->key_count == 0 && rows->count == 0) {
        group_t *empty = NULL;
        database_exec_status_t made = group_find(rows, 0, &empty);
        if (made != DATABASE_EXEC_OK) return made;
    }
    size_t count = rows->count;
    database_exec_status_t status = DATABASE_EXEC_OK;
    if (count > 1) {
        group_t **scratch = q_malloc(db, count * sizeof(group_t *));
        if (scratch == NULL) return DATABASE_EXEC_NOMEM;
        sort_groups(plan, rows->groups, scratch, count);
        q_free(db, scratch, count * sizeof(group_t *));
    }
    for (size_t i = 0; i < count && status == DATABASE_EXEC_OK && !ctx->done; i++) {
        group_t *group = rows->groups[i];
        for (size_t a = 0; a < plan->agg_count && status == DATABASE_EXEC_OK; a++) {
            value_t *out = &group->cells[plan->key_count + a];
            status = agg_finish(db, &plan->aggs[a], &group->states[a], out);
            if (status == DATABASE_EXEC_OK && !q_reserve(db, value_bytes(out))) {
                value_clear(out);
                status = DATABASE_EXEC_NOMEM;
            }
            if (status == DATABASE_EXEC_OK) agg_state_clear(db, &group->states[a]);
        }
        bool keep = true;
        if (status == DATABASE_EXEC_OK && having != NULL) {
            sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
            if (sqleval_test(having, group->cells, &keep, &error) != SQLEVAL_OK) status = eval_failure(db, &error);
        }
        if (status == DATABASE_EXEC_OK && keep) status = select_emit(ctx, group->cells);
    }
    return status;
}

database_exec_status_t emit_sink(void *context, const value_t *row) { return select_emit(context, row); }

bool select_is_grouped(const sqlstmt_select_t *select)
{
    if (select->group_count > 0 || select->having != NULL) return true;
    for (size_t i = 0; i < select->item_count; i++) {
        if (has_aggregate(select->items[i].expr)) return true;
    }
    for (size_t k = 0; k < select->order_count; k++) {
        if (has_aggregate(select->order[k].expr)) return true;
    }
    return false;
}
