/*
 * database_join.c — 결합(INNER·LEFT·CROSS·첫 RIGHT): 원본마다 한 단계인 중첩 루프와 색인 탐침.
 */
#include "database_exec.h"

static database_exec_status_t join_run(join_t *join, size_t pos);

/* ── 결합: 원본마다 한 단계인 중첩 루프. 앞 단계 값으로 색인 탐침을 만든다 ── */

static database_exec_status_t conds_hold(join_t *join, size_t level, bool after, bool *holds)
{
    *holds = true;
    for (size_t i = 0; i < join->cond_count && *holds; i++) {
        const join_cond_t *cond = &join->conds[i];
        if (cond->level != level || cond->after != after) continue;
        sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
        if (sqleval_test(cond->prog, join->row, holds, &error) != SQLEVAL_OK) return eval_failure(join->db, &error);
    }
    return DATABASE_EXEC_OK;
}

/* 이 단계 원본의 행 하나를 받았다 */
static database_exec_status_t join_accept(void *context, const value_t *row)
{
    (void)row;
    join_level_t *step = context;
    join_t *join = step->join;
    bool holds;
    database_exec_status_t status = conds_hold(join, step->level, false, &holds);
    if (status != DATABASE_EXEC_OK || !holds) return status;
    step->matched = true;
    status = conds_hold(join, step->level, true, &holds);
    if (status != DATABASE_EXEC_OK || !holds) return status;
    return join_run(join, step->pos + 1);
}

/* 탐침 값을 계산해 이 단계의 where를 만든다. NULL이거나 열 형식으로 정확히 바꿀 수 없으면 맞는 행이 없다 */
static database_exec_status_t join_probes(join_t *join, const join_level_t *step, where_t *where)
{
    database_t *db = join->db;
    memset(where, 0, sizeof(*where));
    for (size_t i = 0; i < step->probe_count && !where->never; i++) {
        value_t value = {0};
        sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
        if (sqleval_run(step->probes[i].value, join->row, &value, &error) != SQLEVAL_OK) return eval_failure(db, &error);
        if (value.view.type == VALUE_NULL) {
            where->never = true;
            continue;
        }
        if (step->probes[i].op != SQLEXPR_OP_EQ) {
            /* 정수 열과 정수 값의 대소 비교만 구간으로 쓴다. 나머지는 단계 조건 검사에 맡긴다 */
            if (step->probes[i].type == VALUE_INTEGER && value.view.type == VALUE_INTEGER) {
                narrow_range(where, step->probes[i].column, step->probes[i].op, value.view.integer);
            }
            value_clear(&value);
            continue;
        }
        probe_t *probe = &where->probes[where->probe_count];
        value_status_t cast = q_cast(db, &value.view, step->probes[i].type, &probe->literal);
        value_clear(&value);
        if (cast == VALUE_NOMEM) return DATABASE_EXEC_NOMEM;
        if (cast != VALUE_OK) {
            where->never = true;
            continue;
        }
        probe->column = step->probes[i].column;
        where->probe_count++;
    }
    return DATABASE_EXEC_OK;
}

/* 실체화한 결과를 훑는다. 값은 빌려 쓴다 */
static database_exec_status_t scan_relation(join_t *join, join_level_t *step, value_t *row)
{
    const relation_t *rel = step->relation;
    size_t n = rel->column_count;
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (size_t r = 0; r < rel->row_count && status == DATABASE_EXEC_OK && !*step->sink.stop; r++) {
        if (cancel_requested(join->db)) return cancelled_result(join->db);
        for (size_t c = 0; c < n; c++) row[c] = (value_t){rel->cells[r * n + c].view, false};
        status = step->sink.fn(step->sink.ctx, row);
    }
    memset(row, 0, n * sizeof(*row));
    return status;
}

static database_exec_status_t join_run(join_t *join, size_t pos)
{
    if (pos == join->level_count) return join->out->fn(join->out->ctx, join->row);
    size_t level = join->order[pos];
    join_level_t *step = &join->levels[level];
    const source_ref_t *source = &join->sources[level];
    where_t where;
    memset(&where, 0, sizeof(where));
    step->matched = false;
    database_exec_status_t status = step->relation != NULL ? scan_relation(join, step, join->row + source->offset)
                                                           : join_probes(join, step, &where);
    if (status == DATABASE_EXEC_OK && step->relation == NULL) {
        status = scan_rows(join->db, &step->view, step->id, step->heap, &where, join->needed + source->offset,
                           join->row + source->offset, &step->sink);
    }
    where_clear(join->db, &where);
    if (status == DATABASE_EXEC_OK && step->left && !step->matched && !*join->out->stop) {
        /* 맞는 행이 없으면 이 원본의 열을 NULL로 둔 행 하나 */
        bool holds;
        status = conds_hold(join, level, true, &holds);
        if (status == DATABASE_EXEC_OK && holds) status = join_run(join, pos + 1);
    }
    return status;
}

/* 맨 위 단계. FROM이 없으면 행 하나다 */
static database_exec_status_t join_start(join_t *join)
{
    if (join->level_count > 0) return join_run(join, 0);
    bool holds;
    database_exec_status_t status = conds_hold(join, 0, false, &holds);
    return status == DATABASE_EXEC_OK && holds ? join->out->fn(join->out->ctx, NULL) : status;
}

static size_t count_conjuncts(const sqlexpr_t *expr)
{
    if (expr == NULL) return 0;
    if (expr->kind == SQLEXPR_BINARY && expr->op == SQLEXPR_OP_AND) {
        return count_conjuncts(expr->args[0]) + count_conjuncts(expr->args[1]);
    }
    return 1;
}

/* prog이 쓰는 원본 중 가장 늦게 실행되는 것. 열을 쓰지 않으면 처음 실행되는 원본 */
static size_t last_level(join_t *join, const sqleval_prog_t *prog, bool *uses_any)
{
    memset(join->scratch, 0, join->width > 0 ? join->width : 1);
    sqleval_mark_columns(prog, join->scratch, join->width);
    resolver_t all = {join->sources, join->level_count};
    size_t level = join->level_count > 0 ? join->order[0] : 0;
    *uses_any = false;
    for (size_t c = 0; c < join->width; c++) {
        if (!join->scratch[c]) continue;
        *uses_any = true;
        size_t at = source_of(&all, c);
        if (join->levels[at].pos > join->levels[level].pos) level = at;
    }
    return level;
}

/* 열 = 식, 열 대소 비교 식 꼴이면 그 단계의 색인 탐침·구간으로 쓴다(조건 자체도 그대로 검사한다) */
static database_exec_status_t try_probe(join_t *join, const sqlexpr_t *expr, const resolver_t *resolver, size_t level)
{
    if (expr->kind != SQLEXPR_BINARY || expr->op < SQLEXPR_OP_EQ || expr->op > SQLEXPR_OP_GE ||
        expr->op == SQLEXPR_OP_NE || level >= join->level_count) {
        return DATABASE_EXEC_OK;
    }
    join_level_t *step = &join->levels[level];
    const source_ref_t *source = &join->sources[level];
    for (size_t side = 0; side < 2 && step->probe_count < WHERE_MAX_PROBES; side++) {
        const sqlexpr_t *column = expr->args[side], *other = expr->args[1 - side];
        if (column->kind != SQLEXPR_COLUMN) continue;
        value_type_t type;
        size_t at = resolve_column((void *)resolver, &column->qualifier, &column->name, &type);
        if (at == SQLEVAL_NO_COLUMN || at == SQLEVAL_AMBIGUOUS || at < source->offset ||
            at >= source->offset + source->view->column_count) {
            continue;
        }
        sqleval_prog_t *value = NULL;
        database_exec_status_t status = DATABASE_EXEC_OK;
        if (!compile_expr(join->db, other, resolver, &value, &status)) return status;
        bool uses_any;
        size_t needs = last_level(join, value, &uses_any);
        if (uses_any && join->levels[needs].pos >= step->pos) {
            sqleval_free(value);
            continue;
        }
        sqlexpr_op_t op = side == 0 ? expr->op : flip_comparison(expr->op);
        step->probes[step->probe_count++] = (join_probe_t){at - source->offset, type, value, op};
        return DATABASE_EXEC_OK;
    }
    return DATABASE_EXEC_OK;
}

/* 조건을 최상위 AND로 나눠 단계에 배정한다. on_level이 SIZE_MAX면 WHERE다 */
static database_exec_status_t add_conds(join_t *join, const sqlexpr_t *expr, const resolver_t *resolver,
                                        size_t on_level, const char *clause)
{
    if (expr->kind == SQLEXPR_BINARY && expr->op == SQLEXPR_OP_AND) {
        database_exec_status_t status = add_conds(join, expr->args[0], resolver, on_level, clause);
        return status == DATABASE_EXEC_OK ? add_conds(join, expr->args[1], resolver, on_level, clause) : status;
    }
    database_t *db = join->db;
    join_cond_t *cond = &join->conds[join->cond_count];
    database_exec_status_t status = DATABASE_EXEC_OK;
    if (!compile_expr(db, expr, resolver, &cond->prog, &status)) return status;
    join->cond_count++;
    value_type_t type = sqleval_type(cond->prog);
    if (type != VALUE_INTEGER && type != VALUE_NULL) {
        return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "%s 조건은 참·거짓이어야 합니다", clause);
    }
    bool uses_any;
    size_t level = last_level(join, cond->prog, &uses_any);
    if (on_level != SIZE_MAX) {
        cond->level = on_level;
    } else {
        cond->level = level;
        /* LEFT JOIN 쪽 열을 보는 WHERE 조건은 NULL 채움 뒤에 본다 */
        cond->after = level < join->level_count && join->levels[level].left;
    }
    return cond->after ? DATABASE_EXEC_OK : try_probe(join, expr, resolver, cond->level);
}

static void join_free(join_t *join)
{
    database_t *db = join->db;
    for (size_t i = 0; i < join->level_count && join->levels != NULL; i++) {
        heap_close(join->levels[i].heap);
        for (size_t p = 0; p < join->levels[i].probe_count; p++) sqleval_free(join->levels[i].probes[p].value);
    }
    for (size_t i = 0; i < join->cond_count; i++) sqleval_free(join->conds[i].prog);
    q_free(db, join->conds, (join->cond_cap > 0 ? join->cond_cap : 1) * sizeof(join_cond_t));
    q_free(db, join->levels, (join->level_count > 0 ? join->level_count : 1) * sizeof(join_level_t));
    q_free(db, join->order, (join->level_count > 0 ? join->level_count : 1) * sizeof(size_t));
    q_free(db, join->sources, (join->level_count > 0 ? join->level_count : 1) * sizeof(source_ref_t));
    if (join->row != NULL) clear_cells(join->row, join->width);
    q_free(db, join->row, (join->width > 0 ? join->width : 1) * sizeof(value_t));
    q_free(db, join->needed, join->width > 0 ? join->width : 1);
    q_free(db, join->scratch, join->width > 0 ? join->width : 1);
    memset(join, 0, sizeof(*join));
}

/* FROM 원본을 찾고 결합 행 배치를 정한다 */
static database_exec_status_t join_open(database_t *db, const sqlstmt_select_t *select, join_t *join)
{
    memset(join, 0, sizeof(*join));
    join->db = db;
    size_t count = select->source_count;
    /* 해제 크기가 level_count를 따르므로 할당 전에 정한다 */
    join->level_count = count;
    join->levels = q_calloc(db, count > 0 ? count : 1, sizeof(join_level_t));
    join->order = q_calloc(db, count > 0 ? count : 1, sizeof(size_t));
    join->sources = q_calloc(db, count > 0 ? count : 1, sizeof(source_ref_t));
    if (join->levels == NULL || join->order == NULL || join->sources == NULL) return DATABASE_EXEC_NOMEM;
    /* A RIGHT JOIN B ON c는 B를 먼저 훑고 A를 LEFT로 붙인다. 행 배치(*의 열 순서)는 FROM 순서 그대로다 */
    bool right = count > 1 && select->sources[1].join == SQLSTMT_JOIN_RIGHT;
    for (size_t i = 0; i < count; i++) join->order[i] = i;
    if (right) {
        join->order[0] = 1;
        join->order[1] = 0;
    }
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (size_t i = 0; i < count; i++) {
        const sqlstmt_source_t *from = &select->sources[i];
        join_level_t *step = &join->levels[i];
        if (from->subquery != NULL) {
            status = relation_build(db, from, from->subquery, NULL, &step->relation);
        } else {
            status = cte_relation(db, &from->table, &step->relation);
        }
        if (status != DATABASE_EXEC_OK) return status;
        if (step->relation != NULL) {
            step->view = (catalog_table_view_t){0};
            step->view.columns = step->relation->columns;
            step->view.column_count = step->relation->column_count;
            step->view.primary_key_column = NO_COLUMN;
        } else if (!lookup_table(db, &from->table, &step->id, &step->view, &status)) {
            return status;
        }
        step->join = join;
        step->level = i;
        step->pos = right && i < 2 ? 1 - i : i;
        step->left = from->join == SQLSTMT_JOIN_LEFT || (right && i == 0);
        join->sources[i] = (source_ref_t){&step->view, &from->table, &from->alias, join->width, step->left};
        join->width += step->view.column_count;
        for (size_t k = 0; k < i; k++) {
            const sqlstmt_name_t *name = from->alias.size > 0 ? &from->alias : &from->table;
            if (source_named(&join->sources[k], name)) {
                return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_AMBIGUOUS_COLUMN,
                                   "FROM에 같은 이름이 두 번 나옵니다: %.*s", name_len(name->size),
                                   (const char *)name->bytes);
            }
        }
    }
    size_t cells = join->width > 0 ? join->width : 1;
    join->row = q_calloc(db, cells, sizeof(value_t));
    join->needed = q_calloc(db, cells, 1);
    join->scratch = q_calloc(db, cells, 1);
    join->cond_cap = count_conjuncts(select->where);
    for (size_t i = 0; i < count; i++) join->cond_cap += count_conjuncts(select->sources[i].on);
    join->conds = q_calloc(db, join->cond_cap > 0 ? join->cond_cap : 1, sizeof(join_cond_t));
    if (join->row == NULL || join->needed == NULL || join->scratch == NULL || join->conds == NULL) {
        return DATABASE_EXEC_NOMEM;
    }
    for (size_t i = 0; i < count && status == DATABASE_EXEC_OK; i++) {
        if (select->sources[i].on == NULL) continue;
        resolver_t visible = {join->sources, i + 1};
        /* RIGHT JOIN의 ON은 NULL로 채울 쪽(첫 원본)에서 본다 */
        size_t at = select->sources[i].join == SQLSTMT_JOIN_RIGHT ? 0 : i;
        status = add_conds(join, select->sources[i].on, &visible, at, "ON");
    }
    resolver_t all = {join->sources, count};
    if (status == DATABASE_EXEC_OK && select->where != NULL) status = add_conds(join, select->where, &all, SIZE_MAX, "WHERE");
    return status;
}

/* ORDER BY가 원본 하나의 정수 기본 키 하나(오름차순)인지 */
static bool orders_by_primary_key(const sqlstmt_select_t *select, const resolver_t *resolver, const join_level_t *level)
{
    if (select->order_count != 1 || select->order[0].desc || select->order[0].expr->kind != SQLEXPR_COLUMN) return false;
    const sqlexpr_t *expr = select->order[0].expr;
    value_type_t type;
    size_t column = resolve_column((void *)resolver, &expr->qualifier, &expr->name, &type);
    return column != SQLEVAL_NO_COLUMN && column != SQLEVAL_AMBIGUOUS &&
           level->view.primary_key_column != CATALOG_NO_COLUMN && column == level->view.primary_key_column &&
           type == VALUE_INTEGER;
}

/*
 * 이 단계의 scan_rows가 행을 기본 키 오름차순으로 내는지. scan_rows와 같은 순서로 경로를 고른다: 기본 키 등가(행 하나),
 * 보조 색인 등가(유일이면 행 하나, ordered면 같은 값 안에서 기본 키 순서), 기본 키 구간(키 순서). 전체 스캔은 힙 순서다.
 */
static database_exec_status_t scan_follows_primary_key(join_t *join, join_level_t *step, bool *ordered)
{
    *ordered = false;
    where_t where;
    database_exec_status_t status = join_probes(join, step, &where);
    if (status == DATABASE_EXEC_OK) {
        heap_secondary_spec_t secondary;
        if (where.never || key_probe(&step->view, step->heap, &where) != SIZE_MAX) *ordered = true;
        else if (secondary_probe(join->db, step->id, &where, &secondary) != SIZE_MAX) {
            *ordered = secondary.unique || secondary.ordered;   /* scan_rows는 구간보다 보조 색인을 먼저 쓴다 */
        } else {
            *ordered = key_range(&step->view, step->heap, &where) != SIZE_MAX;
        }
    }
    where_clear(join->db, &where);
    return status;
}

/*
 * SELECT 핵심 하나를 실행해 ctx에 행을 낸다. core_only면 ORDER BY·LIMIT·OFFSET을 보지 않고 행을 모두 ctx->rows에
 * 모은다(집합 연산의 한쪽). 아니면 결과 builder로 바로 내보내거나(스트리밍) 정렬하려고 모은다.
 */
database_exec_status_t select_run(database_t *db, const sqlstmt_select_t *select, bool core_only,
                                         select_ctx_t *ctx)
{
    group_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    group_rows_t groups = {db, &plan, NULL, 0, NULL, 0, 0, NULL};
    sqleval_prog_t *having = NULL;
    join_t join;
    database_exec_status_t status = join_open(db, select, &join);
    if (status != DATABASE_EXEC_OK) goto done;
    resolver_t resolver = {join.sources, join.level_count};
    size_t n = join.width;
    bool grouped = select_is_grouped(select);
    compile_ctx_t cc = {resolve_column, NULL, &resolver, &resolver};
    if (grouped) {
        status = plan_groups(db, select, &resolver, &plan);
        if (status != DATABASE_EXEC_OK) goto done;
        cc = (compile_ctx_t){group_resolve, group_match, &plan, &resolver};
    }
    status = plan_outputs(db, select->items, select->item_count, &cc, ctx);
    if (status != DATABASE_EXEC_OK) goto done;
    if (!core_only) status = plan_keys(db, select, &cc, ctx);
    if (status != DATABASE_EXEC_OK) goto done;
    if (select->having != NULL) {
        if (!compile_in(db, select->having, &cc, &having, &status)) goto done;
        value_type_t type = sqleval_type(having);
        if (type != VALUE_INTEGER && type != VALUE_NULL) {
            status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "HAVING 조건은 참·거짓이어야 합니다");
            goto done;
        }
    }
    ctx->distinct = select->distinct;
    if (core_only) {
        ctx->buffered = true;
    } else {
        if (!constant_count(db, select->limit, &ctx->has_limit, &ctx->limit, &status)) goto done;
        bool has_offset = false;
        if (!constant_count(db, select->offset, &has_offset, &ctx->offset, &status)) goto done;
        ctx->buffered = select->distinct || ctx->key_count > 0;
        status = select_prepare(ctx);
        if (status != DATABASE_EXEC_OK) goto done;
    }
    /* 읽을 칸: 그룹이면 GROUP BY·집계 인자가, 아니면 출력·정렬 식이 쓰는 칸과 조건·탐침이 쓰는 칸 */
    bool *needed = join.needed;
    if (grouped) {
        for (size_t g = 0; g < plan.key_count; g++) sqleval_mark_columns(plan.key_progs[g], needed, n);
        for (size_t a = 0; a < plan.agg_count; a++) sqleval_mark_columns(plan.aggs[a].arg, needed, n);
    } else {
        for (size_t i = 0; i < ctx->out_count; i++) {
            if (ctx->outs[i].prog == NULL) needed[ctx->outs[i].column] = true;
            else sqleval_mark_columns(ctx->outs[i].prog, needed, n);
        }
        for (size_t k = 0; k < ctx->key_count; k++) sqleval_mark_columns(ctx->keys[k].prog, needed, n);
    }
    for (size_t i = 0; i < join.cond_count; i++) sqleval_mark_columns(join.conds[i].prog, needed, n);
    for (size_t l = 0; l < join.level_count; l++) {
        for (size_t p = 0; p < join.levels[l].probe_count; p++) {
            sqleval_mark_columns(join.levels[l].probes[p].value, needed, n);
        }
    }

    if (ctx->has_limit && ctx->limit == 0) ctx->done = true;
    for (size_t l = 0; l < join.level_count; l++) {
        if (join.levels[l].relation != NULL) continue;
        heap_status_t opened = open_heap(db, &join.levels[l].view, join.levels[l].id, &join.levels[l].heap);
        if (opened != HEAP_OK) {
            status = heap_failure(db, opened, "open");
            goto done;
        }
    }
    /*
     * ORDER BY 기본 키이고 원본 하나를 기본 키 순서로 읽는다면 정렬하지 않고 바로 내보낸다. LIMIT을 채우면 읽기를 멈춘다.
     * 원본 하나의 조건은 탐침만 쓰므로 바깥 행 없이 판정할 수 있다.
     */
    if (!core_only && !grouped && !select->distinct && ctx->key_count == 1 && join.level_count == 1 &&
        join.levels[0].relation == NULL && orders_by_primary_key(select, &resolver, &join.levels[0])) {
        bool ordered = false;
        status = scan_follows_primary_key(&join, &join.levels[0], &ordered);
        if (status != DATABASE_EXEC_OK) goto done;
        if (ordered) ctx->buffered = false;
    }
    bool never_stop = false;
    row_sink_t sink = grouped ? (row_sink_t){group_collect, &groups, &never_stop} : (row_sink_t){emit_sink, ctx, &ctx->done};
    join.out = &sink;
    for (size_t l = 0; l < join.level_count; l++) join.levels[l].sink = (row_sink_t){join_accept, &join.levels[l], sink.stop};
    if (!*sink.stop) status = join_start(&join);
    if (status == DATABASE_EXEC_OK && grouped && !ctx->done) status = group_flush(db, &plan, &groups, having, ctx);
done:
    group_rows_free(&groups);
    sqleval_free(having);
    group_plan_free(db, &plan);
    join_free(&join);
    return status;
}

/* 모은 행을 정리해 결과로 만든다 */
database_exec_status_t select_finish(select_ctx_t *ctx, result_t **out)
{
    database_t *db = ctx->db;
    if (ctx->buffered) {
        database_exec_status_t status = select_flush(ctx);
        if (status != DATABASE_EXEC_OK) return status;
    }
    result_status_t finished = result_builder_finish(&ctx->builder, out);
    if (finished == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    return finished == RESULT_OK ? DATABASE_EXEC_OK : fail_result(db, RESULT_E_LIMIT, 17, "결과를 만들지 못했습니다");
}
