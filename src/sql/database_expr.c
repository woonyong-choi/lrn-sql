/*
 * database_expr.c — WHERE와 식 컴파일 문맥: 열 이름 해석, 색인 탐침 추출, 행마다 조건 평가.
 */
#include "database_exec.h"

/* ── WHERE ── */

bool source_named(const source_ref_t *source, const sqlstmt_name_t *name)
{
    const sqlstmt_name_t *exposed = source->alias != NULL && source->alias->size > 0 ? source->alias : source->table;
    return exposed->size == name->size && memcmp(exposed->bytes, name->bytes, name->size) == 0;
}

size_t resolve_column(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name,
                             value_type_t *type)
{
    const resolver_t *r = ctx;
    size_t found = SQLEVAL_NO_COLUMN;
    for (size_t i = 0; i < r->count; i++) {
        const source_ref_t *source = &r->sources[i];
        if (qualifier->size > 0 && !source_named(source, qualifier)) continue;
        size_t column = find_column(source->view, name);
        if (column == SIZE_MAX) continue;
        if (found != SQLEVAL_NO_COLUMN) return SQLEVAL_AMBIGUOUS;
        found = source->offset + column;
        *type = source->view->columns[column].type;
    }
    return found;
}

/* 결합 행의 칸 번호가 속한 원본 */
size_t source_of(const resolver_t *r, size_t column)
{
    size_t i = 0;
    while (i + 1 < r->count && column >= r->sources[i + 1].offset) i++;
    return i;
}

/* ── 식과 WHERE ── */

/* 평가 오류를 결과로 바꾼다 */
database_exec_status_t eval_failure(database_t *db, const sqleval_error_t *error)
{
    const sqlstmt_name_t *name = error->name;
    int length = name != NULL ? name_len(name->size) : 0;
    const char *text = name != NULL ? (const char *)name->bytes : "";
    switch (error->status) {
        case SQLEVAL_NOMEM: return DATABASE_EXEC_NOMEM;
        case SQLEVAL_UNKNOWN_COLUMN:
            return fail_result(db, RESULT_E_SEMANTIC, 2, "컬럼이 없습니다: %.*s", length, text);
        case SQLEVAL_AMBIGUOUS_COLUMN:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_AMBIGUOUS_COLUMN, "컬럼 이름이 모호합니다: %.*s",
                               length, text);
        case SQLEVAL_UNKNOWN_FUNCTION:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_UNKNOWN_FUNCTION, "알 수 없는 함수입니다: %.*s",
                               length, text);
        case SQLEVAL_ARGUMENT:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_FUNCTION_ARGUMENT,
                               "함수 인자가 맞지 않습니다 (위치 %zu)", error->offset);
        case SQLEVAL_DIVIDE_BY_ZERO:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_DIVIDE_BY_ZERO, "0으로 나눴습니다 (위치 %zu)",
                               error->offset);
        case SQLEVAL_OVERFLOW:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_ARITHMETIC_OVERFLOW,
                               "계산이 범위를 넘거나 정확히 변환할 수 없습니다 (위치 %zu)", error->offset);
        case SQLEVAL_AGGREGATE:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_AGGREGATE_MISUSE,
                               "이 자리에는 집계 함수를 쓸 수 없습니다: %.*s", length, text);
        case SQLEVAL_NOT_GROUPED:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_NOT_GROUPED,
                               "GROUP BY에 없는 컬럼은 집계 밖에서 쓸 수 없습니다: %.*s", length, text);
        case SQLEVAL_SUBQUERY: return db->subquery_status;
        case SQLEVAL_SUBQUERY_SHAPE:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_SUBQUERY,
                               "식 자리의 서브쿼리는 열 하나, 스칼라 서브쿼리는 한 행 이하여야 합니다 (위치 %zu)",
                               error->offset);
        case SQLEVAL_NOT_CONSTANT:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_NOT_CONSTANT,
                               "이 자리에는 컬럼을 쓸 수 없습니다: %.*s", length, text);
        case SQLEVAL_LIMIT:
            return fail_result(db, RESULT_E_LIMIT, 5, "값이 한도를 넘습니다 (위치 %zu)", error->offset);
        default:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "형식이 맞지 않습니다 (위치 %zu)",
                               error->offset);
    }
}

bool compile_expr(database_t *db, const sqlexpr_t *expr, const resolver_t *resolver, sqleval_prog_t **prog,
                         database_exec_status_t *status)
{
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_env_t env = {resolver != NULL ? resolve_column : NULL, NULL, (void *)resolver, subquery_hook, db, outer_hook};
    const resolver_t *saved = db->compile_resolver;
    db->compile_resolver = resolver;
    sqleval_status_t compiled = sqleval_compile(expr, &env, prog, &error);
    db->compile_resolver = saved;
    if (compiled == SQLEVAL_OK) return true;
    *status = eval_failure(db, &error);
    return false;
}

void where_clear(database_t *db, where_t *where)
{
    for (size_t i = 0; i < where->probe_count; i++) owned_clear(db, &where->probes[i].literal, 1);
    sqleval_free(where->prog);
    memset(where, 0, sizeof(*where));
}

/* 값을 컬럼 타입으로 정확히 변환한다. 실패하면 *status에 반환값을 둔다. */
bool cast_for_column(database_t *db, const value_view_t *source, const catalog_column_t *column,
                            value_t *target, database_exec_status_t *status)
{
    value_status_t cast = q_cast(db, source, column->type, target);
    if (cast == VALUE_OK) return true;
    if (cast == VALUE_NOMEM) *status = DATABASE_EXEC_NOMEM;
    else if (cast == VALUE_LIMIT) *status = fail_result(db, RESULT_E_LIMIT, (int)cast, "컬럼 %.*s의 값이 한도를 넘습니다",
                                                          name_len(column->name_bytes), column->name);
    else *status = fail_result(db, RESULT_E_SEMANTIC, (int)cast, "컬럼 %.*s에 저장할 수 없는 값입니다",
                               name_len(column->name_bytes), column->name);
    return false;
}

bool values_equal(const value_view_t *a, const value_view_t *b)
{
    int order = 1;
    return a->type == b->type && value_compare(a, b, &order) == VALUE_OK && order == 0;
}

/* 컬럼이 오른쪽에 있으면 뒤집은 비교. a < 5 와 5 > a는 같다 */
sqlexpr_op_t flip_comparison(sqlexpr_op_t op)
{
    switch (op) {
        case SQLEXPR_OP_LT: return SQLEXPR_OP_GT;
        case SQLEXPR_OP_LE: return SQLEXPR_OP_GE;
        case SQLEXPR_OP_GT: return SQLEXPR_OP_LT;
        case SQLEXPR_OP_GE: return SQLEXPR_OP_LE;
        default: return op;
    }
}

/* 정수 컬럼과 정수 상수의 대소 비교를 구간으로 모은다. 다른 꼴은 구간 없이 전체 조건 평가에 맡긴다 */
static void collect_range(const sqlexpr_t *expr, const resolver_t *resolver, where_t *where)
{
    sqlexpr_op_t op = expr->op;
    const sqlexpr_t *column = expr->args[0], *constant = expr->args[1];
    if (column->kind != SQLEXPR_COLUMN) {
        column = expr->args[1];
        constant = expr->args[0];
        op = flip_comparison(op);
    }
    if (column->kind != SQLEXPR_COLUMN || constant->kind != SQLEXPR_LITERAL) return;
    value_type_t type;
    size_t index = resolve_column((void *)resolver, &column->qualifier, &column->name, &type);
    if (index == SQLEVAL_NO_COLUMN || index == SQLEVAL_AMBIGUOUS) return;
    if (constant->value.view.type == VALUE_NULL) {
        where->never = true;
        return;
    }
    if (type != VALUE_INTEGER || constant->value.view.type != VALUE_INTEGER) return;
    narrow_range(where, index, op, constant->value.view.integer);
}

/* 컬럼 op bound(op는 컬럼 쪽에서 본 대소 비교)로 그 컬럼의 구간을 좁힌다. 빈 구간이면 where->never */
void narrow_range(where_t *where, size_t index, sqlexpr_op_t op, int64_t bound)
{
    int64_t low = INT64_MIN, high = INT64_MAX;
    switch (op) {
        case SQLEXPR_OP_LT:
            if (bound == INT64_MIN) where->never = true;
            else high = bound - 1;
            break;
        case SQLEXPR_OP_LE: high = bound; break;
        case SQLEXPR_OP_GT:
            if (bound == INT64_MAX) where->never = true;
            else low = bound + 1;
            break;
        default: low = bound; break;
    }
    if (where->never) return;
    range_t *range = NULL;
    for (size_t i = 0; i < where->range_count && range == NULL; i++) {
        if (where->ranges[i].column == index) range = &where->ranges[i];
    }
    if (range == NULL) {
        if (where->range_count == WHERE_MAX_PROBES) return;
        range = &where->ranges[where->range_count++];
        *range = (range_t){index, INT64_MIN, INT64_MAX};
    }
    if (low > range->low) range->low = low;
    if (high < range->high) range->high = high;
    if (range->low > range->high) where->never = true;
}

/* 최상위 AND를 따라 내려가며 컬럼 = 상수 조건과 정수 대소 비교를 모은다 */
static bool collect_probes(database_t *db, const sqlexpr_t *expr, const resolver_t *resolver, where_t *where,
                           database_exec_status_t *status)
{
    if (expr->kind == SQLEXPR_BINARY && expr->op == SQLEXPR_OP_AND) {
        return collect_probes(db, expr->args[0], resolver, where, status) &&
               collect_probes(db, expr->args[1], resolver, where, status);
    }
    if (expr->kind == SQLEXPR_BINARY && expr->op >= SQLEXPR_OP_LT && expr->op <= SQLEXPR_OP_GE) {
        collect_range(expr, resolver, where);
        return true;
    }
    if (expr->kind != SQLEXPR_BINARY || expr->op != SQLEXPR_OP_EQ) return true;
    const sqlexpr_t *column = expr->args[0], *constant = expr->args[1];
    if (column->kind != SQLEXPR_COLUMN) {
        column = expr->args[1];
        constant = expr->args[0];
    }
    if (column->kind != SQLEXPR_COLUMN || constant->kind != SQLEXPR_LITERAL) return true;
    value_type_t type;
    size_t index = resolve_column((void *)resolver, &column->qualifier, &column->name, &type);
    if (index == SQLEVAL_NO_COLUMN || index == SQLEVAL_AMBIGUOUS) return true;
    if (constant->value.view.type == VALUE_NULL) {
        where->never = true;
        return true;
    }
    if (where->probe_count == WHERE_MAX_PROBES) return true;
    probe_t *probe = &where->probes[where->probe_count];
    value_status_t cast = q_cast(db, &constant->value.view, type, &probe->literal);
    if (cast == VALUE_NOMEM) {
        *status = DATABASE_EXEC_NOMEM;
        return false;
    }
    if (cast != VALUE_OK) {
        /* 컬럼 형식으로 정확히 바꿀 수 없는 숫자(예: 정수 컬럼 = 1.5)와는 같을 수 없다 */
        where->never = true;
        return true;
    }
    probe->column = index;
    where->probe_count++;
    return true;
}

bool compile_where(database_t *db, const resolver_t *resolver, const sqlexpr_t *source, where_t *where,
                          database_exec_status_t *status)
{
    memset(where, 0, sizeof(*where));
    if (source == NULL) return true;
    where->present = true;
    if (!compile_expr(db, source, resolver, &where->prog, status)) return false;
    value_type_t type = sqleval_type(where->prog);
    if (type != VALUE_INTEGER && type != VALUE_NULL) {
        *status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "WHERE 조건은 참·거짓이어야 합니다");
        return false;
    }
    return collect_probes(db, source, resolver, where, status);
}

/* row가 조건에 맞는지. 평가 오류는 결과로 바꿔 돌려준다 */
database_exec_status_t where_matches(database_t *db, const where_t *where, const value_t *row, bool *matched)
{
    *matched = !where->never;
    if (!where->present || where->never) return DATABASE_EXEC_OK;
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    if (sqleval_test(where->prog, row, matched, &error) == SQLEVAL_OK) return DATABASE_EXEC_OK;
    return eval_failure(db, &error);
}

/* 탐침으로 찾은 행이 그 탐침 조건을 만족하는지(색인이 가리키는 행의 검증) */
bool probe_holds(const probe_t *probe, const value_t *row)
{
    return values_equal(&row[probe->column].view, &probe->literal.view);
}

/* ── 식 컴파일 문맥 ── */

bool compile_in(database_t *db, const sqlexpr_t *expr, const compile_ctx_t *cc, sqleval_prog_t **prog,
                       database_exec_status_t *status)
{
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_env_t env = {cc->resolve, cc->match, cc->ctx, subquery_hook, db, outer_hook};
    /* 그룹 질의의 식은 그룹 행 위에서 계산하므로 안쪽 서브쿼리가 바깥 열을 볼 수 없다 */
    const resolver_t *saved = db->compile_resolver;
    db->compile_resolver = cc->match == NULL ? cc->table : NULL;
    sqleval_status_t compiled = sqleval_compile(expr, &env, prog, &error);
    db->compile_resolver = saved;
    if (compiled == SQLEVAL_OK) return true;
    *status = eval_failure(db, &error);
    return false;
}

static const char UNNAMED_COLUMN[] = "?column?";

/* t.*가 가리키는 원본. 없으면 SIZE_MAX */
static size_t star_source(const resolver_t *resolver, const sqlstmt_name_t *qualifier)
{
    for (size_t i = 0; i < resolver->count; i++) {
        if (source_named(&resolver->sources[i], qualifier)) return i;
    }
    return SIZE_MAX;
}

static result_column_t column_meta(const resolver_t *resolver, size_t column)
{
    const source_ref_t *source = &resolver->sources[source_of(resolver, column)];
    size_t at = column - source->offset;
    const catalog_column_t *def = &source->view->columns[at];
    /* 기본 키는 NOT NULL을 적지 않아도(AUTOINCREMENT) 저장된 값이 NULL일 수 없다 */
    bool stored_nullable = !def->not_null && at != source->view->primary_key_column;
    return (result_column_t){(const uint8_t *)def->name, def->name_bytes, def->type, stored_nullable || source->nullable};
}

/* 선택 목록을 출력 열로 펼친다. *는 모든 원본의 열, t.*는 그 원본의 열이다 */
database_exec_status_t plan_outputs(database_t *db, const sqlstmt_select_item_t *items, size_t item_count,
                                           const compile_ctx_t *cc, select_ctx_t *ctx)
{
    const resolver_t *resolver = cc->table;
    size_t width = resolver->count > 0 ? resolver->sources[resolver->count - 1].offset +
                                             resolver->sources[resolver->count - 1].view->column_count
                                       : 0;
    database_exec_status_t status = DATABASE_EXEC_OK;
    size_t total = 0;
    for (size_t i = 0; i < item_count; i++) {
        const sqlstmt_select_item_t *item = &items[i];
        if (!item->star) {
            total++;
            continue;
        }
        if (cc->match != NULL) {
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_NOT_GROUPED, "그룹 질의에서는 *를 쓸 수 없습니다");
        }
        if (resolver->count == 0) return fail_result(db, RESULT_E_SEMANTIC, 2, "FROM 없이 *를 쓸 수 없습니다");
        if (item->qualifier.size == 0) {
            total += width;
            continue;
        }
        size_t at = star_source(resolver, &item->qualifier);
        if (at == SIZE_MAX) {
            return fail_result(db, RESULT_E_SEMANTIC, 1, "테이블이 없습니다: %.*s", name_len(item->qualifier.size),
                               (const char *)item->qualifier.bytes);
        }
        total += resolver->sources[at].view->column_count;
    }
    ctx->outs_bytes = (total > 0 ? total : 1) * sizeof(select_out_t);
    ctx->outs = q_calloc(db, total > 0 ? total : 1, sizeof(select_out_t));
    if (ctx->outs == NULL) return DATABASE_EXEC_NOMEM;
    for (size_t i = 0; i < item_count && status == DATABASE_EXEC_OK; i++) {
        const sqlstmt_select_item_t *item = &items[i];
        if (item->star) {
            size_t from = 0, to = width;
            if (item->qualifier.size > 0) {
                const source_ref_t *source = &resolver->sources[star_source(resolver, &item->qualifier)];
                from = source->offset;
                to = from + source->view->column_count;
            }
            for (size_t c = from; c < to; c++) {
                select_out_t *out = &ctx->outs[ctx->out_count++];
                out->column = c;
                out->meta = column_meta(resolver, c);
            }
            continue;
        }
        select_out_t *out = &ctx->outs[ctx->out_count++];
        const sqlexpr_t *expr = item->expr;
        out->meta.is_nullable = true;
        if (cc->match == NULL && expr->kind == SQLEXPR_COLUMN) {
            value_type_t type;
            size_t column = resolve_column((void *)resolver, &expr->qualifier, &expr->name, &type);
            if (column != SQLEVAL_NO_COLUMN && column != SQLEVAL_AMBIGUOUS) {
                out->column = column;
                out->meta = column_meta(resolver, column);
            }
        }
        if (out->meta.name == NULL) {
            if (!compile_in(db, expr, cc, &out->prog, &status)) break;
            out->meta.type = sqleval_type(out->prog);
            out->meta.name = expr->kind == SQLEXPR_COLUMN ? expr->name.bytes : (const uint8_t *)UNNAMED_COLUMN;
            out->meta.name_len = expr->kind == SQLEXPR_COLUMN ? expr->name.size : sizeof(UNNAMED_COLUMN) - 1;
            if (expr->kind == SQLEXPR_FUNCTION) {
                out->meta.name = expr->name.bytes;
                out->meta.name_len = expr->name.size;
            }
        }
        if (item->alias.size > 0) {
            out->meta.name = item->alias.bytes;
            out->meta.name_len = item->alias.size;
        }
    }
    return status;
}

/* ORDER BY를 정렬 키로 바꾼다. 정수 리터럴은 선택 목록 번호, 한정 없는 이름이 별칭과 같으면 그 출력 열이다 */
database_exec_status_t plan_keys(database_t *db, const sqlstmt_select_t *select, const compile_ctx_t *cc,
                                        select_ctx_t *ctx)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    ctx->keys_bytes = (select->order_count > 0 ? select->order_count : 1) * sizeof(select_key_t);
    ctx->keys = q_calloc(db, select->order_count > 0 ? select->order_count : 1, sizeof(select_key_t));
    if (ctx->keys == NULL) return DATABASE_EXEC_NOMEM;
    for (size_t k = 0; k < select->order_count && status == DATABASE_EXEC_OK; k++) {
        const sqlstmt_order_t *order = &select->order[k];
        select_key_t *key = &ctx->keys[ctx->key_count++];
        key->desc = order->desc;
        /* 기본 NULL 위치는 SQLite와 같다: NULL이 가장 작다 */
        key->nulls_first = order->nulls == SQLSTMT_NULLS_DEFAULT ? !order->desc : order->nulls == SQLSTMT_NULLS_FIRST;
        const sqlexpr_t *expr = order->expr;
        if (expr->kind == SQLEXPR_LITERAL && expr->value.view.type == VALUE_INTEGER) {
            int64_t position = expr->value.view.integer;
            if (position < 1 || (uint64_t)position > ctx->out_count) {
                return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_BAD_ORDER_POSITION,
                                   "ORDER BY 번호가 선택 목록 밖입니다: %lld", (long long)position);
            }
            key->slot = (size_t)position - 1;
            continue;
        }
        bool by_alias = false;
        if (expr->kind == SQLEXPR_COLUMN && expr->qualifier.size == 0) {
            for (size_t i = 0, out = 0; i < select->item_count && !by_alias; i++) {
                const sqlstmt_select_item_t *item = &select->items[i];
                if (item->star) {
                    for (size_t s = 0; s < cc->table->count; s++) {
                        if (item->qualifier.size == 0 || source_named(&cc->table->sources[s], &item->qualifier)) {
                            out += cc->table->sources[s].view->column_count;
                        }
                    }
                    continue;
                }
                if (item->alias.size == expr->name.size &&
                    memcmp(item->alias.bytes, expr->name.bytes, expr->name.size) == 0) {
                    key->slot = out;
                    by_alias = true;
                }
                out++;
            }
        }
        if (by_alias) continue;
        if (compile_in(db, expr, cc, &key->prog, &status)) key->slot = ctx->out_count + ctx->extra_count++;
    }
    return status;
}
