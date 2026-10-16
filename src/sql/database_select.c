/*
 * database_select.c — SELECT 실행: 출력·정렬·LIMIT, 행 공급(기본 키·보조 색인·전체 스캔), 집합 연산.
 */
#include "database_exec.h"

typedef struct {
    database_t *db;
    heap_t *heap;
    const where_t *where;
    const probe_t *probe;
    value_t *row;
    const bool *needed;
    const row_sink_t *sink;
    database_exec_status_t status;
} secondary_scan_t;

typedef struct {
    database_t *db;
    heap_t *heap;
    const where_t *where;
    value_t *row;
    const bool *needed;
    const row_sink_t *sink;
    database_exec_status_t status;
} range_scan_t;

static void select_rows_free(select_ctx_t *ctx);

/* ── SELECT ── */

static size_t select_width(const select_ctx_t *ctx) { return ctx->out_count + ctx->extra_count; }

/* 출력 열이 정해진 뒤 결과 열·계산 자리·결과 builder를 만든다 */
database_exec_status_t select_prepare(select_ctx_t *ctx)
{
    database_t *db = ctx->db;
    ctx->columns = q_malloc(db, (ctx->out_count > 0 ? ctx->out_count : 1) * sizeof(result_column_t));
    ctx->cells = q_calloc(db, ctx->out_count > 0 ? ctx->out_count : 1, sizeof(value_t));
    ctx->views = q_malloc(db, (ctx->out_count > 0 ? ctx->out_count : 1) * sizeof(value_view_t));
    if (ctx->columns == NULL || ctx->cells == NULL || ctx->views == NULL) return DATABASE_EXEC_NOMEM;
    for (size_t i = 0; i < ctx->out_count; i++) ctx->columns[i] = ctx->outs[i].meta;
    result_builder_spec_t spec = {ctx->columns, ctx->out_count, NULL};
    result_status_t created = result_builder_create_ex(&spec, &db->ledger, &ctx->builder);
    if (created == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    if (created != RESULT_OK) return fail_result(db, RESULT_E_LIMIT, 15, "결과 컬럼 한도를 넘습니다");
    return DATABASE_EXEC_OK;
}

void select_ctx_free(select_ctx_t *ctx)
{
    database_t *db = ctx->db;
    size_t slots = ctx->out_count > 0 ? ctx->out_count : 1;
    result_builder_destroy(&ctx->builder);
    select_rows_free(ctx);
    if (ctx->cells != NULL) owned_clear(db, ctx->cells, ctx->out_count);
    q_free(db, ctx->cells, slots * sizeof(value_t));
    q_free(db, ctx->views, slots * sizeof(value_view_t));
    q_free(db, ctx->columns, slots * sizeof(result_column_t));
    for (size_t i = 0; ctx->outs != NULL && i < ctx->out_count; i++) sqleval_free(ctx->outs[i].prog);
    q_free(db, ctx->outs, ctx->outs_bytes);
    for (size_t k = 0; ctx->keys != NULL && k < ctx->key_count; k++) sqleval_free(ctx->keys[k].prog);
    q_free(db, ctx->keys, ctx->keys_bytes);
    memset(ctx, 0, sizeof(*ctx));
    ctx->db = db;
}

static void select_rows_free(select_ctx_t *ctx)
{
    size_t width = select_width(ctx);
    for (size_t i = 0; i < ctx->row_count; i++) {
        owned_clear(ctx->db, ctx->rows[i], width);
        q_free(ctx->db, ctx->rows[i], width * sizeof(value_t));
    }
    q_free(ctx->db, ctx->rows, ctx->row_cap * sizeof(value_t *));
    ctx->rows = NULL;
    ctx->row_count = ctx->row_cap = 0;
}

/* 식을 계산해 계상한 값으로 *out에 둔다. prog가 NULL이면 row[column]을 복제한다 */
database_exec_status_t select_value(database_t *db, const sqleval_prog_t *prog, size_t column,
                                           const value_t *row, value_t *out)
{
    if (prog == NULL) return q_clone(db, &row[column].view, out) == VALUE_OK ? DATABASE_EXEC_OK : DATABASE_EXEC_NOMEM;
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    if (sqleval_run(prog, row, out, &error) != SQLEVAL_OK) return eval_failure(db, &error);
    if (!q_reserve(db, value_bytes(out))) {
        value_clear(out);
        return DATABASE_EXEC_NOMEM;
    }
    return DATABASE_EXEC_OK;
}

static database_exec_status_t add_result_row(database_t *db, result_builder_t *builder, const value_view_t *views,
                                             size_t count)
{
    result_status_t added = result_builder_add_row(builder, views, count);
    if (added == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    if (added != RESULT_OK) return fail_result(db, RESULT_E_LIMIT, 16, "결과가 한도를 넘습니다");
    return DATABASE_EXEC_OK;
}

/* WHERE를 통과한 행 하나를 내보내거나 모은다 */
database_exec_status_t select_emit(select_ctx_t *ctx, const value_t *row)
{
    database_t *db = ctx->db;
    if (!ctx->buffered) {
        if (ctx->skipped < ctx->offset) {
            ctx->skipped++;
            return DATABASE_EXEC_OK;
        }
        database_exec_status_t status = DATABASE_EXEC_OK;
        for (size_t i = 0; i < ctx->out_count && status == DATABASE_EXEC_OK; i++) {
            const select_out_t *out = &ctx->outs[i];
            if (out->prog == NULL) {
                ctx->views[i] = row[out->column].view;
                continue;
            }
            status = select_value(db, out->prog, out->column, row, &ctx->cells[i]);
            ctx->views[i] = ctx->cells[i].view;
        }
        if (status == DATABASE_EXEC_OK) status = add_result_row(db, ctx->builder, ctx->views, ctx->out_count);
        owned_clear(db, ctx->cells, ctx->out_count);
        if (status == DATABASE_EXEC_OK && ++ctx->emitted == ctx->limit && ctx->has_limit) ctx->done = true;
        return status;
    }
    if (ctx->row_count == ctx->row_cap) {
        size_t cap = ctx->row_cap == 0 ? 64 : ctx->row_cap * 2;
        if (cap > SIZE_MAX / sizeof(value_t *) || !q_reserve(db, cap * sizeof(value_t *))) return DATABASE_EXEC_NOMEM;
        value_t **grown = realloc(ctx->rows, cap * sizeof(*grown));
        if (grown == NULL) {
            q_release(db, cap * sizeof(value_t *));
            return DATABASE_EXEC_NOMEM;
        }
        q_release(db, ctx->row_cap * sizeof(value_t *));
        ctx->rows = grown;
        ctx->row_cap = cap;
    }
    size_t width = select_width(ctx);
    value_t *values = q_calloc(db, width, sizeof(*values));
    if (values == NULL) return DATABASE_EXEC_NOMEM;
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (size_t i = 0; i < ctx->out_count && status == DATABASE_EXEC_OK; i++) {
        status = select_value(db, ctx->outs[i].prog, ctx->outs[i].column, row, &values[i]);
    }
    for (size_t k = 0; k < ctx->key_count && status == DATABASE_EXEC_OK; k++) {
        if (ctx->keys[k].prog != NULL) status = select_value(db, ctx->keys[k].prog, 0, row, &values[ctx->keys[k].slot]);
    }
    if (status != DATABASE_EXEC_OK) {
        owned_clear(db, values, width);
        q_free(db, values, width * sizeof(value_t));
        return status;
    }
    ctx->rows[ctx->row_count++] = values;
    return DATABASE_EXEC_OK;
}

static int compare_outputs(const select_ctx_t *ctx, const value_t *a, const value_t *b)
{
    for (size_t i = 0; i < ctx->out_count; i++) {
        int order = sqleval_compare(&a[i].view, &b[i].view);
        if (order != 0) return order;
    }
    return 0;
}

static int compare_keys_of(const select_ctx_t *ctx, const value_t *a, const value_t *b)
{
    for (size_t k = 0; k < ctx->key_count; k++) {
        const select_key_t *key = &ctx->keys[k];
        const value_view_t *x = &a[key->slot].view, *y = &b[key->slot].view;
        bool x_null = x->type == VALUE_NULL, y_null = y->type == VALUE_NULL;
        if (x_null || y_null) {
            if (x_null && y_null) continue;
            return (x_null == key->nulls_first) ? -1 : 1;
        }
        int order = sqleval_compare(x, y);
        if (order != 0) return key->desc ? -order : order;
    }
    return 0;
}

/* 안정 병합 정렬. scratch는 count개 자리 */
static void merge_sort(const select_ctx_t *ctx, value_t **rows, value_t **scratch, size_t count, bool by_keys)
{
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int order = by_keys ? compare_keys_of(ctx, rows[j], rows[i]) : compare_outputs(ctx, rows[j], rows[i]);
                scratch[k++] = order < 0 ? rows[j++] : rows[i++];
            }
            while (i < mid) scratch[k++] = rows[i++];
            while (j < hi) scratch[k++] = rows[j++];
        }
        memcpy(rows, scratch, count * sizeof(*rows));
    }
}

/* 모은 행에 DISTINCT·ORDER BY를 적용한다 */
static database_exec_status_t select_settle(select_ctx_t *ctx)
{
    database_t *db = ctx->db;
    size_t count = ctx->row_count;
    if (count > 1) {
        size_t scratch_bytes = count * sizeof(value_t *);
        value_t **scratch = q_malloc(db, scratch_bytes);
        if (scratch == NULL) return DATABASE_EXEC_NOMEM;
        if (ctx->distinct) {
            merge_sort(ctx, ctx->rows, scratch, count, false);
            size_t kept = 1, width = select_width(ctx);
            for (size_t i = 1; i < count; i++) {
                if (compare_outputs(ctx, ctx->rows[kept - 1], ctx->rows[i]) == 0) {
                    owned_clear(db, ctx->rows[i], width);
                    q_free(db, ctx->rows[i], width * sizeof(value_t));
                } else {
                    ctx->rows[kept++] = ctx->rows[i];
                }
            }
            ctx->row_count = count = kept;
        }
        if (ctx->key_count > 0) merge_sort(ctx, ctx->rows, scratch, count, true);
        q_free(db, scratch, scratch_bytes);
    }
    return DATABASE_EXEC_OK;
}

/* 모은 행을 DISTINCT·ORDER BY·OFFSET·LIMIT 순서로 결과에 넣는다 */
database_exec_status_t select_flush(select_ctx_t *ctx)
{
    database_t *db = ctx->db;
    database_exec_status_t settled = select_settle(ctx);
    if (settled != DATABASE_EXEC_OK) return settled;
    size_t count = ctx->row_count;
    for (size_t i = (size_t)(ctx->offset < count ? ctx->offset : count); i < count; i++) {
        if (ctx->has_limit && ctx->emitted >= ctx->limit) break;
        for (size_t c = 0; c < ctx->out_count; c++) ctx->views[c] = ctx->rows[i][c].view;
        database_exec_status_t status = add_result_row(db, ctx->builder, ctx->views, ctx->out_count);
        if (status != DATABASE_EXEC_OK) return status;
        ctx->emitted++;
    }
    return DATABASE_EXEC_OK;
}

/* LIMIT·OFFSET의 상수 값. NULL이면 제한 없음(LIMIT) 또는 0(OFFSET) */
bool constant_count(database_t *db, const sqlexpr_t *expr, bool *present, uint64_t *value,
                           database_exec_status_t *status)
{
    *present = false;
    if (expr == NULL) return true;
    sqleval_prog_t *prog = NULL;
    if (!compile_expr(db, expr, NULL, &prog, status)) return false;
    value_t result = {0};
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_status_t ran = sqleval_run(prog, NULL, &result, &error);
    sqleval_free(prog);
    if (ran != SQLEVAL_OK) {
        *status = eval_failure(db, &error);
        return false;
    }
    bool ok = result.view.type == VALUE_NULL || (result.view.type == VALUE_INTEGER && result.view.integer >= 0);
    if (ok && result.view.type == VALUE_INTEGER) {
        *present = true;
        *value = (uint64_t)result.view.integer;
    }
    value_clear(&result);
    if (!ok) *status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_BAD_LIMIT, "LIMIT·OFFSET은 0 이상의 정수여야 합니다");
    return ok;
}

/* 기본 키 탐침 번호. 없으면 SIZE_MAX */
size_t key_probe(const catalog_table_view_t *view, const heap_t *heap, const where_t *where)
{
    if (!heap_is_indexed(heap) || where->never) return SIZE_MAX;
    for (size_t i = 0; i < where->probe_count; i++) {
        const value_view_t *key = &where->probes[i].literal.view;
        if (where->probes[i].column == view->primary_key_column &&
            (key->type != VALUE_TEXT || key->size_bytes <= TEXT_TREE_KEY_MAX_BYTES)) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* 정수 기본 키의 구간 번호. 구간이 없거나 양쪽 다 열려 있으면 SIZE_MAX */
size_t key_range(const catalog_table_view_t *view, const heap_t *heap, const where_t *where)
{
    if (!heap_is_indexed(heap) || where->never) return SIZE_MAX;
    for (size_t i = 0; i < where->range_count; i++) {
        const range_t *range = &where->ranges[i];
        if (range->column == view->primary_key_column && view->columns[range->column].type == VALUE_INTEGER &&
            (range->low != INT64_MIN || range->high != INT64_MAX)) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* 보조 색인 탐침 번호. 없으면 SIZE_MAX */
size_t secondary_probe(database_t *db, catalog_table_id_t table_id, const where_t *where,
                              heap_secondary_spec_t *spec)
{
    if (where->never) return SIZE_MAX;
    const catalog_t *catalog = base_catalog(db);
    size_t count = catalog_index_count(catalog, table_id);
    for (size_t p = 0; p < where->probe_count; p++) {
        if (!heap_secondary_key_fits(&where->probes[p].literal.view)) continue;
        for (size_t i = 0; i < count; i++) {
            catalog_index_view_t index;
            if (catalog_index_at(catalog, table_id, i, &index) != CATALOG_OK) continue;
            if (index.column != where->probes[p].column) continue;
            *spec = (heap_secondary_spec_t){index.column, index.unique, index.ordered, index.root};
            return p;
        }
    }
    return SIZE_MAX;
}

database_exec_status_t locate_key(database_t *db, heap_t *heap, const value_view_t *key, bool *is_hit,
                                         heap_locator_t *at)
{
    heap_status_t found = key->type == VALUE_TEXT ? heap_find_text_key(heap, key->bytes, key->size_bytes, at)
                                                  : heap_find_key(heap, key->integer, at);
    *is_hit = found == HEAP_OK;
    return found == HEAP_OK || found == HEAP_NOT_FOUND ? DATABASE_EXEC_OK : heap_failure(db, found, "lookup");
}

/* ── 행 공급: WHERE에 맞는 행을 받는 쪽에 넘긴다 ── */

static bool scan_secondary_row(void *context, heap_locator_t locator)
{
    secondary_scan_t *scan = context;
    database_t *db = scan->db;
    if (cancel_requested(db)) {
        scan->status = cancelled_result(db);
        return false;
    }
    heap_status_t got = heap_get_projected(scan->heap, locator, scan->needed, scan->row);
    if (got != HEAP_OK) {
        scan->status = heap_failure(db, got, "index lookup");
        return false;
    }
    bool matched = false;
    if (!probe_holds(scan->probe, scan->row)) {
        scan->status = heap_failure(db, HEAP_CORRUPT, "index reference");
    } else {
        scan->status = where_matches(db, scan->where, scan->row, &matched);
        if (scan->status == DATABASE_EXEC_OK && matched) scan->status = scan->sink->fn(scan->sink->ctx, scan->row);
    }
    heap_release_row(scan->heap, scan->row);
    return scan->status == DATABASE_EXEC_OK && !*scan->sink->stop;
}

static bool scan_range_row(void *context, heap_locator_t locator)
{
    range_scan_t *scan = context;
    database_t *db = scan->db;
    if (cancel_requested(db)) {
        scan->status = cancelled_result(db);
        return false;
    }
    heap_status_t got = heap_get_projected(scan->heap, locator, scan->needed, scan->row);
    if (got != HEAP_OK) {
        scan->status = heap_failure(db, got, "index range");
        return false;
    }
    bool matched = false;
    scan->status = where_matches(db, scan->where, scan->row, &matched);
    if (scan->status == DATABASE_EXEC_OK && matched) scan->status = scan->sink->fn(scan->sink->ctx, scan->row);
    heap_release_row(scan->heap, scan->row);
    return scan->status == DATABASE_EXEC_OK && !*scan->sink->stop;
}

/* 기본 키·보조 색인 탐침이나 기본 키 구간이 있으면 그것으로, 없으면 전체를 훑는다 */
database_exec_status_t scan_rows(database_t *db, const catalog_table_view_t *table, catalog_table_id_t id,
                                        heap_t *heap, const where_t *where, const bool *needed, value_t *row,
                                        const row_sink_t *sink)
{
    if (*sink->stop || where->never) return DATABASE_EXEC_OK;
    bool matched = false;
    size_t n = table->column_count;
    size_t by_key = key_probe(table, heap, where);
    heap_secondary_spec_t secondary;
    size_t by_secondary = by_key == SIZE_MAX ? secondary_probe(db, id, where, &secondary) : SIZE_MAX;
    if (by_key != SIZE_MAX) {
        bool is_hit;
        heap_locator_t at;
        database_exec_status_t status = locate_key(db, heap, &where->probes[by_key].literal.view, &is_hit, &at);
        if (status != DATABASE_EXEC_OK || !is_hit) return status;
        heap_status_t got = heap_get_projected(heap, at, needed, row);
        if (got != HEAP_OK) return heap_failure(db, got, "lookup");
        status = where_matches(db, where, row, &matched);
        if (status == DATABASE_EXEC_OK && matched) status = sink->fn(sink->ctx, row);
        heap_release_row(heap, row);
        return status;
    }
    if (by_secondary != SIZE_MAX) {
        secondary_scan_t scan = {db, heap, where, &where->probes[by_secondary], row, needed, sink, DATABASE_EXEC_OK};
        heap_status_t visited = heap_secondary_visit_equal(heap, secondary, &where->probes[by_secondary].literal.view,
                                                           scan_secondary_row, &scan);
        /* LIMIT을 채워 방문을 멈추면 색인은 멈춤을 NOMEM으로 알린다. 오류가 아니다 */
        if (scan.status != DATABASE_EXEC_OK || *sink->stop) return scan.status;
        return visited == HEAP_OK ? DATABASE_EXEC_OK : heap_failure(db, visited, "index lookup");
    }
    size_t by_range = key_range(table, heap, where);
    if (by_range != SIZE_MAX) {
        range_scan_t scan = {db, heap, where, row, needed, sink, DATABASE_EXEC_OK};
        bool stopped = false;
        heap_status_t visited = heap_visit_key_range(heap, where->ranges[by_range].low, where->ranges[by_range].high,
                                                     scan_range_row, &scan, &stopped);
        if (scan.status != DATABASE_EXEC_OK) return scan.status;
        return visited == HEAP_OK ? DATABASE_EXEC_OK : heap_failure(db, visited, "index range");
    }
    heap_cursor_t *cursor = NULL;
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) return heap_failure(db, made, "cursor");
    database_exec_status_t status = DATABASE_EXEC_OK;
    while (!*sink->stop) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            break;
        }
        heap_locator_t at;
        heap_status_t next = heap_cursor_next_projected(cursor, needed, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, next, "scan");
            break;
        }
        status = where_matches(db, where, row, &matched);
        if (status == DATABASE_EXEC_OK && matched) status = sink->fn(sink->ctx, row);
        clear_cells(row, n);
        if (status != DATABASE_EXEC_OK) break;
    }
    heap_cursor_close(cursor);
    return status;
}

/* ── 집합 연산(UNION·EXCEPT·INTERSECT) ── */

/* 행 배열 자리를 need개 이상으로 늘린다 */
static database_exec_status_t select_rows_reserve(select_ctx_t *ctx, size_t need)
{
    database_t *db = ctx->db;
    if (need <= ctx->row_cap) return DATABASE_EXEC_OK;
    size_t cap = ctx->row_cap == 0 ? 64 : ctx->row_cap;
    while (cap < need) cap *= 2;
    if (cap > SIZE_MAX / sizeof(value_t *) || !q_reserve(db, cap * sizeof(value_t *))) return DATABASE_EXEC_NOMEM;
    value_t **grown = realloc(ctx->rows, cap * sizeof(*grown));
    if (grown == NULL) {
        q_release(db, cap * sizeof(value_t *));
        return DATABASE_EXEC_NOMEM;
    }
    q_release(db, ctx->row_cap * sizeof(value_t *));
    ctx->rows = grown;
    ctx->row_cap = cap;
    return DATABASE_EXEC_OK;
}

/* 행을 정렬하고 중복을 지운다 */
static database_exec_status_t select_dedupe(select_ctx_t *ctx)
{
    bool distinct = ctx->distinct;
    ctx->distinct = true;
    database_exec_status_t status = select_settle(ctx);
    ctx->distinct = distinct;
    return status;
}

static void drop_row(select_ctx_t *ctx, value_t *row)
{
    size_t width = select_width(ctx);
    owned_clear(ctx->db, row, width);
    q_free(ctx->db, row, width * sizeof(value_t));
}

/* acc op other를 acc에 둔다. other의 행은 옮기거나 지운다 */
static database_exec_status_t combine_rows(select_ctx_t *acc, select_ctx_t *other, sqlstmt_set_op_t op)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    if (op == SQLSTMT_SET_UNION || op == SQLSTMT_SET_UNION_ALL) {
        status = select_rows_reserve(acc, acc->row_count + other->row_count);
        if (status != DATABASE_EXEC_OK) return status;
        memcpy(acc->rows + acc->row_count, other->rows, other->row_count * sizeof(value_t *));
        acc->row_count += other->row_count;
        other->row_count = 0;
        return op == SQLSTMT_SET_UNION ? select_dedupe(acc) : DATABASE_EXEC_OK;
    }
    /* EXCEPT·INTERSECT: 양쪽을 정렬·중복 제거한 뒤 함께 훑는다 */
    status = select_dedupe(acc);
    if (status == DATABASE_EXEC_OK) status = select_dedupe(other);
    if (status != DATABASE_EXEC_OK) return status;
    size_t kept = 0, j = 0;
    for (size_t i = 0; i < acc->row_count; i++) {
        while (j < other->row_count && compare_outputs(acc, other->rows[j], acc->rows[i]) < 0) j++;
        bool present = j < other->row_count && compare_outputs(acc, other->rows[j], acc->rows[i]) == 0;
        if (present == (op == SQLSTMT_SET_INTERSECT)) acc->rows[kept++] = acc->rows[i];
        else drop_row(acc, acc->rows[i]);
    }
    acc->row_count = kept;
    return DATABASE_EXEC_OK;
}

/* 두 쪽 열 형식을 맞춘다. NULL은 다른 쪽을 따르고 INTEGER·REAL은 REAL, 그 밖에 다르면 오류 */
static bool merge_type(value_type_t *into, value_type_t other)
{
    if (*into == VALUE_NULL || *into == other) {
        if (*into == VALUE_NULL) *into = other;
        return true;
    }
    if (other == VALUE_NULL) return true;
    bool numeric = (*into == VALUE_INTEGER || *into == VALUE_REAL) && (other == VALUE_INTEGER || other == VALUE_REAL);
    if (numeric) *into = VALUE_REAL;
    return numeric;
}

/* 집합 연산 결과의 ORDER BY: 결과 열 번호나 첫 SELECT의 결과 열 이름만 쓴다 */
static database_exec_status_t plan_set_keys(database_t *db, const sqlstmt_select_t *head, select_ctx_t *ctx)
{
    ctx->keys_bytes = (head->order_count > 0 ? head->order_count : 1) * sizeof(select_key_t);
    ctx->keys = q_calloc(db, head->order_count > 0 ? head->order_count : 1, sizeof(select_key_t));
    if (ctx->keys == NULL) return DATABASE_EXEC_NOMEM;
    for (size_t k = 0; k < head->order_count; k++) {
        const sqlstmt_order_t *order = &head->order[k];
        select_key_t *key = &ctx->keys[ctx->key_count++];
        key->desc = order->desc;
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
        size_t found = SIZE_MAX;
        for (size_t i = 0; expr->kind == SQLEXPR_COLUMN && expr->qualifier.size == 0 && i < ctx->out_count; i++) {
            const result_column_t *meta = &ctx->outs[i].meta;
            if (meta->name_len != expr->name.size || memcmp(meta->name, expr->name.bytes, expr->name.size) != 0) continue;
            if (found != SIZE_MAX) {
                return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_AMBIGUOUS_COLUMN, "ORDER BY 이름이 모호합니다: %.*s",
                                   name_len(expr->name.size), (const char *)expr->name.bytes);
            }
            found = i;
        }
        if (found == SIZE_MAX) {
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_SET_OPERATION,
                               "집합 연산의 ORDER BY에는 결과 열 이름이나 번호만 쓸 수 있습니다");
        }
        key->slot = found;
    }
    return DATABASE_EXEC_OK;
}

static database_exec_status_t exec_compound(database_t *db, const sqlstmt_select_t *head, result_t **out)
{
    select_ctx_t acc, other;
    memset(&acc, 0, sizeof(acc));
    memset(&other, 0, sizeof(other));
    acc.db = other.db = db;
    database_exec_status_t status = select_run(db, head, true, &acc);
    if (status == DATABASE_EXEC_OK && acc.distinct) status = select_dedupe(&acc);
    for (const sqlstmt_select_t *left = head; status == DATABASE_EXEC_OK && left->next != NULL; left = left->next) {
        status = select_run(db, left->next, true, &other);
        if (status == DATABASE_EXEC_OK && other.out_count != acc.out_count) {
            status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_SET_OPERATION,
                                 "집합 연산 양쪽의 열 수가 다릅니다 (%zu, %zu)", acc.out_count, other.out_count);
        }
        for (size_t i = 0; status == DATABASE_EXEC_OK && i < acc.out_count; i++) {
            if (!merge_type(&acc.outs[i].meta.type, other.outs[i].meta.type)) {
                status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "집합 연산 %zu번째 열의 형식이 다릅니다", i + 1);
            }
            acc.outs[i].meta.is_nullable |= other.outs[i].meta.is_nullable;
        }
        if (status == DATABASE_EXEC_OK && other.distinct) status = select_dedupe(&other);
        if (status == DATABASE_EXEC_OK) status = combine_rows(&acc, &other, left->next_op);
        select_ctx_free(&other);
    }
    /* 결과는 결합한 행 그대로다: 출력 열은 행의 칸을 읽고 정렬 키는 결과 열을 가리킨다 */
    for (size_t i = 0; status == DATABASE_EXEC_OK && i < acc.out_count; i++) {
        sqleval_free(acc.outs[i].prog);
        acc.outs[i].prog = NULL;
        acc.outs[i].column = i;
        if (acc.outs[i].meta.type != VALUE_REAL) continue;
        for (size_t r = 0; r < acc.row_count; r++) {
            value_t *cell = &acc.rows[r][i];
            if (cell->view.type == VALUE_INTEGER) value_set_real(cell, (double)cell->view.integer);
        }
    }
    acc.distinct = false;
    if (status == DATABASE_EXEC_OK) status = plan_set_keys(db, head, &acc);
    if (status == DATABASE_EXEC_OK && !constant_count(db, head->limit, &acc.has_limit, &acc.limit, &status)) goto done;
    bool has_offset = false;
    if (status == DATABASE_EXEC_OK && !constant_count(db, head->offset, &has_offset, &acc.offset, &status)) goto done;
    if (status == DATABASE_EXEC_OK) status = select_prepare(&acc);
    if (status == DATABASE_EXEC_OK) status = select_finish(&acc, out);
done:
    select_ctx_free(&acc);
    return status;
}

static database_exec_status_t exec_query(database_t *db, const sqlstmt_select_t *select, result_t **out)
{
    if (select->next != NULL) return exec_compound(db, select, out);
    select_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.db = db;
    database_exec_status_t status = select_run(db, select, false, &ctx);
    if (status == DATABASE_EXEC_OK) status = select_finish(&ctx, out);
    select_ctx_free(&ctx);
    return status;
}

/* WITH 이름은 이 질의(과 안의 서브쿼리)에서만 보인다 */
database_exec_status_t exec_select(database_t *db, const sqlstmt_select_t *select, result_t **out)
{
    if (select->cte_count == 0) return exec_query(db, select, out);
    cte_scope_t scope = {select, select->cte_count, db->ctes};
    db->ctes = &scope;
    database_exec_status_t status = exec_query(db, select, out);
    db->ctes = scope.parent;
    return status;
}
