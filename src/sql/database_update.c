/*
 * database_update.c — RETURNING과 UPDATE·DELETE.
 */
#include "database_exec.h"

typedef struct {
    heap_locator_t locator;
    value_t *cells;
} match_t;

typedef struct {
    match_t *items;
    size_t count;
    size_t cap;
    size_t columns;
} matches_t;

/* 탐침으로 후보 행을 찾고 전체 조건을 거른다 */
typedef struct {
    database_t *db;
    heap_t *heap;
    const where_t *where;
    const probe_t *probe;
    value_t *row;
    matches_t *matches;
    bool keep_values;
    database_exec_status_t status;
} secondary_match_t;

typedef struct {
    database_t *db;
    heap_t *heap;
    const where_t *where;
    value_t *row;
    matches_t *matches;
    bool keep_values;
    database_exec_status_t status;
} range_match_t;

typedef struct {
    const value_view_t *view;
} key_ref_t;

/* 기본 키 갱신에서 일치하지 않은 행의 키를 모은다(색인 없는 표) */
typedef struct {
    database_t *db;
    size_t pk;
    value_t *keys;
    size_t count;
    size_t cap;
} other_keys_t;

/* ── RETURNING ── */

database_exec_status_t returning_open(database_t *db, const sqlstmt_returning_t *spec,
                                             const catalog_table_view_t *view, const sqlstmt_name_t *table,
                                             returning_t **out)
{
    *out = NULL;
    if (spec->count == 0) return DATABASE_EXEC_OK;
    returning_t *ret = q_calloc(db, 1, sizeof(*ret));
    if (ret == NULL) return DATABASE_EXEC_NOMEM;
    ret->ctx.db = db;
    ret->source = (source_ref_t){view, table, NULL, 0, false};
    ret->resolver = (resolver_t){&ret->source, 1};
    compile_ctx_t cc = {resolve_column, NULL, &ret->resolver, &ret->resolver};
    database_exec_status_t status = plan_outputs(db, spec->items, spec->count, &cc, &ret->ctx);
    if (status == DATABASE_EXEC_OK) status = select_prepare(&ret->ctx);
    if (status != DATABASE_EXEC_OK) {
        returning_close(db, ret);
        return status;
    }
    *out = ret;
    return DATABASE_EXEC_OK;
}

database_exec_status_t returning_row(returning_t *ret, const value_t *row)
{
    return ret != NULL ? select_emit(&ret->ctx, row) : DATABASE_EXEC_OK;
}

database_exec_status_t returning_finish(database_t *db, returning_t *ret, const char *tag, uint64_t affected,
                                               result_t **success)
{
    if (ret == NULL) return open_command(db, tag, affected, success) == RESULT_OK ? DATABASE_EXEC_OK : DATABASE_EXEC_NOMEM;
    result_status_t finished = result_builder_finish(&ret->ctx.builder, success);
    if (finished == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    return finished == RESULT_OK ? DATABASE_EXEC_OK : fail_result(db, RESULT_E_LIMIT, 17, "결과를 만들지 못했습니다");
}

void returning_close(database_t *db, returning_t *ret)
{
    if (ret == NULL) return;
    select_ctx_free(&ret->ctx);
    q_free(db, ret, sizeof(*ret));
}

/* ── UPDATE · DELETE ── */

static void matches_free(database_t *db, matches_t *matches)
{
    for (size_t i = 0; i < matches->count; i++) {
        if (matches->items[i].cells != NULL) {
            owned_clear(db, matches->items[i].cells, matches->columns);
            q_free(db, matches->items[i].cells, matches->columns * sizeof(value_t));
        }
    }
    q_free(db, matches->items, matches->cap * sizeof(match_t));
    memset(matches, 0, sizeof(*matches));
}

/* 일치한 행(locator와 값)을 모은다. keep_values가 false면 값은 보관하지 않는다. */
static bool matches_add(database_t *db, matches_t *matches, heap_locator_t locator, const value_t *row,
                        bool keep_values)
{
    if (matches->count == matches->cap) {
        /* 새 배열을 먼저 reserve하고 복사한 뒤 옛 배열을 돌려준다. 순간 최대치는 old+new다 */
        size_t cap = matches->cap == 0 ? 64 : matches->cap * 2;
        if (cap > SIZE_MAX / sizeof(match_t) || !q_reserve(db, cap * sizeof(match_t))) return false;
        match_t *grown = realloc(matches->items, cap * sizeof(*grown));
        if (grown == NULL) {
            q_release(db, cap * sizeof(match_t));
            return false;
        }
        q_release(db, matches->cap * sizeof(match_t));
        matches->items = grown;
        matches->cap = cap;
    }
    match_t *slot = &matches->items[matches->count];
    slot->locator = locator;
    slot->cells = NULL;
    if (keep_values) {
        slot->cells = q_calloc(db, matches->columns, sizeof(*slot->cells));
        if (slot->cells == NULL) return false;
        for (size_t i = 0; i < matches->columns; i++) {
            if (q_clone(db, &row[i].view, &slot->cells[i]) != VALUE_OK) {
                owned_clear(db, slot->cells, matches->columns);
                q_free(db, slot->cells, matches->columns * sizeof(value_t));
                slot->cells = NULL;
                return false;
            }
        }
    }
    matches->count++;
    return true;
}

static bool collect_secondary_match(void *context, heap_locator_t locator)
{
    secondary_match_t *scan = context;
    if (cancel_requested(scan->db)) {
        scan->status = cancelled_result(scan->db);
        return false;
    }
    heap_status_t got = heap_get(scan->heap, locator, scan->row);
    if (got != HEAP_OK) {
        scan->status = heap_failure(scan->db, got, "index lookup");
        return false;
    }
    bool matched = false;
    if (!probe_holds(scan->probe, scan->row)) {
        scan->status = heap_failure(scan->db, HEAP_CORRUPT, "index reference");
    } else {
        scan->status = where_matches(scan->db, scan->where, scan->row, &matched);
        if (scan->status == DATABASE_EXEC_OK && matched &&
            !matches_add(scan->db, scan->matches, locator, scan->row, scan->keep_values)) {
            scan->status = DATABASE_EXEC_NOMEM;
        }
    }
    heap_release_row(scan->heap, scan->row);
    return scan->status == DATABASE_EXEC_OK;
}

static bool collect_range_match(void *context, heap_locator_t locator)
{
    range_match_t *scan = context;
    if (cancel_requested(scan->db)) {
        scan->status = cancelled_result(scan->db);
        return false;
    }
    heap_status_t got = heap_get(scan->heap, locator, scan->row);
    if (got != HEAP_OK) {
        scan->status = heap_failure(scan->db, got, "index range");
        return false;
    }
    bool matched = false;
    scan->status = where_matches(scan->db, scan->where, scan->row, &matched);
    if (scan->status == DATABASE_EXEC_OK && matched &&
        !matches_add(scan->db, scan->matches, locator, scan->row, scan->keep_values)) {
        scan->status = DATABASE_EXEC_NOMEM;
    }
    heap_release_row(scan->heap, scan->row);
    return scan->status == DATABASE_EXEC_OK;
}

/*
 * WHERE에 맞는 행을 모은다. 기본 키·보조 색인 탐침이나 기본 키 구간이 있으면 그것으로 후보를 찾고, 없으면 전체를 훑는다.
 * scan_other가 NULL이 아니면 훑는 동안 맞지 않은 행마다 부른다(기본 키 갱신의 중복 검사용).
 */
static database_exec_status_t collect_matches(database_t *db, const catalog_table_view_t *view, catalog_table_id_t id,
                                              heap_t *heap, const where_t *where, value_t *row, bool keep_values,
                                              bool force_scan, matches_t *matches,
                                              database_exec_status_t (*scan_other)(void *, const value_t *),
                                              void *other_ctx)
{
    size_t n = view->column_count;
    if (where->never && scan_other == NULL) return DATABASE_EXEC_OK;
    size_t by_key = force_scan ? SIZE_MAX : key_probe(view, heap, where);
    heap_secondary_spec_t secondary;
    size_t by_secondary = force_scan || by_key != SIZE_MAX ? SIZE_MAX : secondary_probe(db, id, where, &secondary);
    if (by_key != SIZE_MAX) {
        bool is_hit;
        heap_locator_t at;
        database_exec_status_t status = locate_key(db, heap, &where->probes[by_key].literal.view, &is_hit, &at);
        if (status != DATABASE_EXEC_OK || !is_hit) return status;
        heap_status_t got = heap_get(heap, at, row);
        if (got != HEAP_OK) return heap_failure(db, got, "lookup");
        bool matched = false;
        status = where_matches(db, where, row, &matched);
        if (status == DATABASE_EXEC_OK && matched && !matches_add(db, matches, at, row, keep_values)) {
            status = DATABASE_EXEC_NOMEM;
        }
        heap_release_row(heap, row);
        return status;
    }
    if (by_secondary != SIZE_MAX) {
        secondary_match_t scan = {db, heap, where, &where->probes[by_secondary], row, matches, keep_values,
                                  DATABASE_EXEC_OK};
        heap_status_t visited = heap_secondary_visit_equal(heap, secondary, &where->probes[by_secondary].literal.view,
                                                           collect_secondary_match, &scan);
        if (scan.status != DATABASE_EXEC_OK) return scan.status;
        return visited == HEAP_OK ? DATABASE_EXEC_OK : heap_failure(db, visited, "index lookup");
    }
    size_t by_range = force_scan ? SIZE_MAX : key_range(view, heap, where);
    if (by_range != SIZE_MAX) {
        range_match_t scan = {db, heap, where, row, matches, keep_values, DATABASE_EXEC_OK};
        bool stopped = false;
        heap_status_t visited = heap_visit_key_range(heap, where->ranges[by_range].low, where->ranges[by_range].high,
                                                     collect_range_match, &scan, &stopped);
        if (scan.status != DATABASE_EXEC_OK) return scan.status;
        return visited == HEAP_OK ? DATABASE_EXEC_OK : heap_failure(db, visited, "index range");
    }
    heap_cursor_t *cursor = NULL;
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) return heap_failure(db, made, "cursor");
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (;;) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            break;
        }
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, next, "scan");
            break;
        }
        bool matched = false;
        status = where_matches(db, where, row, &matched);
        if (status == DATABASE_EXEC_OK && matched) {
            if (!matches_add(db, matches, at, row, keep_values)) status = DATABASE_EXEC_NOMEM;
        } else if (status == DATABASE_EXEC_OK && scan_other != NULL) {
            status = scan_other(other_ctx, row);
        }
        clear_cells(row, n);
        if (status != DATABASE_EXEC_OK) break;
    }
    heap_cursor_close(cursor);
    return status;
}

static int compare_keys(const key_ref_t *x, const key_ref_t *y)
{
    int order = 0;
    value_compare(x->view, y->view, &order);
    return order;
}

/* 제자리 힙 정렬. libc qsort는 내부에서 임시 메모리를 쓸 수 있어 계상 밖의 할당이 생기므로 쓰지 않는다. */
static void sift_down(key_ref_t *keys, size_t root, size_t count)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= count) return;
        if (child + 1 < count && compare_keys(&keys[child], &keys[child + 1]) < 0) child++;
        if (compare_keys(&keys[root], &keys[child]) >= 0) return;
        key_ref_t swap = keys[root];
        keys[root] = keys[child];
        keys[child] = swap;
        root = child;
    }
}

static void sort_keys(key_ref_t *keys, size_t count)
{
    if (count < 2) return;
    for (size_t i = count / 2; i > 0; i--) sift_down(keys, i - 1, count);
    for (size_t end = count - 1; end > 0; end--) {
        key_ref_t swap = keys[0];
        keys[0] = keys[end];
        keys[end] = swap;
        sift_down(keys, 0, end);
    }
}

static bool has_duplicate(key_ref_t *keys, size_t count)
{
    sort_keys(keys, count);
    for (size_t i = 1; i < count; i++) {
        if (values_equal(keys[i - 1].view, keys[i].view)) return true;
    }
    return false;
}

static database_exec_status_t keep_other_key(void *context, const value_t *row)
{
    other_keys_t *other = context;
    if (row[other->pk].view.type == VALUE_NULL) return DATABASE_EXEC_OK;
    if (other->count == other->cap) {
        size_t cap = other->cap == 0 ? 64 : other->cap * 2;
        if (cap > SIZE_MAX / sizeof(value_t) || !q_reserve(other->db, cap * sizeof(value_t))) return DATABASE_EXEC_NOMEM;
        value_t *grown = realloc(other->keys, cap * sizeof(*grown));
        if (grown == NULL) {
            q_release(other->db, cap * sizeof(value_t));
            return DATABASE_EXEC_NOMEM;
        }
        q_release(other->db, other->cap * sizeof(value_t));
        other->keys = grown;
        other->cap = cap;
    }
    memset(&other->keys[other->count], 0, sizeof(value_t));
    if (q_clone(other->db, &row[other->pk].view, &other->keys[other->count]) != VALUE_OK) return DATABASE_EXEC_NOMEM;
    other->count++;
    return DATABASE_EXEC_OK;
}

static bool same_locator_ref(heap_locator_t a, heap_locator_t b)
{
    return a.directory_index == b.directory_index && a.slot == b.slot;
}

database_exec_status_t exec_update(database_t *db, const sqlstmt_update_t *update, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &update->table, &id, &view, &status)) return status;
    size_t n = view.column_count;
    source_ref_t source = {&view, &update->table, NULL, 0, false};
    resolver_t resolver = {&source, 1};
    returning_t *ret = NULL;
    status = returning_open(db, &update->returning, &view, &update->table, &ret);
    if (status != DATABASE_EXEC_OK) return status;
    size_t assign_count = update->assignment_count;
    size_t columns_bytes = (assign_count > 0 ? assign_count : 1) * sizeof(size_t);
    size_t progs_bytes = (assign_count > 0 ? assign_count : 1) * sizeof(sqleval_prog_t *);
    size_t row_bytes = n * sizeof(value_t), views_bytes = n * sizeof(value_view_t);
    size_t *targets = q_malloc(db, columns_bytes);
    sqleval_prog_t **progs = q_calloc(db, assign_count > 0 ? assign_count : 1, sizeof(*progs));
    value_t *row = q_calloc(db, n, sizeof(*row));
    value_view_t *views = q_malloc(db, views_bytes);
    value_t *fresh = NULL;      /* 일치한 행마다 assign_count개의 새 값 */
    size_t fresh_bytes = 0;
    key_ref_t *keys = NULL;
    size_t keys_bytes = 0;
    other_keys_t other = {db, view.primary_key_column, NULL, 0, 0};
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    matches_t matches = {NULL, 0, 0, n};
    where_t where;
    memset(&where, 0, sizeof(where));
    if (targets == NULL || progs == NULL || row == NULL || views == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    size_t pk = view.primary_key_column;
    bool pk_assigned = false;
    for (size_t i = 0; i < assign_count; i++) {
        size_t column = find_column(&view, &update->assignments[i].column);
        if (column == SIZE_MAX) {
            status = unknown_column(db, &update->assignments[i].column);
            goto done;
        }
        for (size_t j = 0; j < i; j++) {
            if (targets[j] == column) {
                status = fail_result(db, RESULT_E_SEMANTIC, 9, "컬럼이 두 번 나옵니다: %.*s",
                                     name_len(update->assignments[i].column.size),
                                     (const char *)update->assignments[i].column.bytes);
                goto done;
            }
        }
        targets[i] = column;
        if (!compile_expr(db, update->assignments[i].value, &resolver, &progs[i], &status)) goto done;
        if (column == pk) pk_assigned = true;
    }
    if (!compile_where(db, &resolver, update->where, &where, &status)) goto done;

    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, opened, "open");
        goto done;
    }
    bool is_indexed = heap_is_indexed(heap);
    /* 색인 없는 표의 기본 키 갱신은 맞지 않은 행의 키도 봐야 해서 전체를 훑는다 */
    bool scan_all = pk_assigned && !is_indexed;
    status = collect_matches(db, &view, id, heap, &where, row, true, scan_all, &matches,
                             scan_all ? keep_other_key : NULL, &other);
    if (status != DATABASE_EXEC_OK) goto done;
    if (matches.count == 0) {
        status = returning_finish(db, ret, "UPDATE", 0, &success);
        if (status != DATABASE_EXEC_OK) goto done;
        *out = success;
        success = NULL;
        goto done;
    }

    /* 새 값: 일치한 행마다 SET 식을 그 행의 옛 값으로 계산하고 컬럼 형식으로 정확히 바꾼다 */
    if (assign_count > 0 && matches.count > SIZE_MAX / assign_count / sizeof(value_t)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    fresh_bytes = matches.count * (assign_count > 0 ? assign_count : 1) * sizeof(value_t);
    fresh = q_calloc(db, matches.count * (assign_count > 0 ? assign_count : 1), sizeof(value_t));
    if (fresh == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        for (size_t i = 0; i < assign_count; i++) {
            value_t computed = {0};
            sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
            if (sqleval_run(progs[i], matches.items[m].cells, &computed, &error) != SQLEVAL_OK) {
                status = eval_failure(db, &error);
                goto done;
            }
            bool cast = cast_for_column(db, &computed.view, &view.columns[targets[i]], &fresh[m * assign_count + i],
                                        &status);
            value_clear(&computed);
            if (!cast) goto done;
            if (view.columns[targets[i]].not_null && fresh[m * assign_count + i].view.type == VALUE_NULL) {
                status = not_null_violation(db, &view.columns[targets[i]]);
                goto done;
            }
        }
    }

    if (pk_assigned) {
        size_t pk_slot = 0;
        while (targets[pk_slot] != pk) pk_slot++;
        size_t total = matches.count + other.count;
        keys_bytes = total * sizeof(*keys);
        keys = q_malloc(db, keys_bytes);
        if (keys == NULL) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        size_t key_count = 0;
        for (size_t m = 0; m < matches.count; m++) {
            const value_view_t *key = &fresh[m * assign_count + pk_slot].view;
            if (key->type == VALUE_NULL) continue;
            keys[key_count++].view = key;
            if (!is_indexed) continue;
            /* 색인에서 같은 키를 가진 행이 있으면 그 행도 이번에 키가 바뀌어야 한다 */
            heap_locator_t at;
            heap_status_t found = key->type == VALUE_TEXT ? heap_find_text_key(heap, key->bytes, key->size_bytes, &at)
                                                          : heap_find_key(heap, key->integer, &at);
            if (found != HEAP_OK && found != HEAP_NOT_FOUND) {
                status = heap_failure(db, found, "lookup");
                goto done;
            }
            if (found == HEAP_OK) {
                bool is_matched = false;
                for (size_t j = 0; j < matches.count && !is_matched; j++) {
                    is_matched = same_locator_ref(matches.items[j].locator, at);
                }
                if (!is_matched) {
                    status = duplicate_key(db, &view);
                    goto done;
                }
            }
        }
        for (size_t i = 0; i < other.count; i++) keys[key_count++].view = &other.keys[i].view;
        if (has_duplicate(keys, key_count)) {
            status = duplicate_key(db, &view);
            goto done;
        }
    }

    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began == HEAP_OK) began = configure_secondary(batch, work.cat, id);
    if (began != HEAP_OK) {
        status = heap_failure(db, began, "batch");
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        for (size_t i = 0; i < n; i++) views[i] = matches.items[m].cells[i].view;
        heap_status_t removed = heap_batch_secondary_remove(batch, views, n, matches.items[m].locator);
        if (removed != HEAP_OK) {
            status = heap_failure(db, removed, "index");
            goto done;
        }
    }
    for (size_t m = 0; m < matches.count; m++) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        for (size_t i = 0; i < n; i++) views[i] = matches.items[m].cells[i].view;
        for (size_t i = 0; i < assign_count; i++) views[targets[i]] = fresh[m * assign_count + i].view;
        heap_locator_t placed;
        heap_status_t changed = heap_batch_update(batch, matches.items[m].locator, views, n, &placed);
        if (changed == HEAP_OK) changed = heap_batch_secondary_insert(batch, views, n, placed);
        if (changed != HEAP_OK) {
            status = heap_failure(db, changed, "update");
            goto done;
        }
        if (ret != NULL) {
            /* 바뀐 행: 옛 행 값과 새 값을 빌려 쓴다 */
            for (size_t i = 0; i < n; i++) row[i] = (value_t){views[i], false};
            status = returning_row(ret, row);
            memset(row, 0, n * sizeof(*row));
            if (status != DATABASE_EXEC_OK) goto done;
        }
    }
    status = returning_finish(db, ret, "UPDATE", matches.count, &success);
    if (status != DATABASE_EXEC_OK) goto done;
    status = publish(db, &work, id, batch, NULL, 0, success, out);
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_close(heap);
    work_close(db, &work);
    result_release(&success);
    if (fresh != NULL) owned_clear(db, fresh, matches.count * assign_count);
    q_free(db, fresh, fresh_bytes);
    matches_free(db, &matches);
    if (other.keys != NULL) owned_clear(db, other.keys, other.count);
    q_free(db, other.keys, other.cap * sizeof(value_t));
    q_free(db, keys, keys_bytes);
    if (row != NULL) clear_cells(row, n);
    q_free(db, row, row_bytes);
    q_free(db, views, views_bytes);
    for (size_t i = 0; progs != NULL && i < assign_count; i++) sqleval_free(progs[i]);
    q_free(db, progs, progs_bytes);
    q_free(db, targets, columns_bytes);
    where_clear(db, &where);
    returning_close(db, ret);
    return status;
}

database_exec_status_t exec_delete(database_t *db, const sqlstmt_delete_t *del, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &del->table, &id, &view, &status)) return status;
    size_t n = view.column_count;
    source_ref_t source = {&view, &del->table, NULL, 0, false};
    resolver_t resolver = {&source, 1};
    returning_t *ret = NULL;
    status = returning_open(db, &del->returning, &view, &del->table, &ret);
    if (status != DATABASE_EXEC_OK) return status;
    size_t row_bytes = n * sizeof(value_t);
    value_t *row = q_calloc(db, n, sizeof(*row));
    /* 보조 색인을 지우거나 RETURNING으로 돌려줄 때는 지울 행의 값을 쥔다 */
    bool keep_values = catalog_index_count(base_catalog(db), id) > 0 || ret != NULL;
    size_t views_bytes = keep_values ? n * sizeof(value_view_t) : 0;
    value_view_t *views = keep_values ? q_malloc(db, views_bytes) : NULL;
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    matches_t matches = {NULL, 0, 0, n};
    where_t where;
    memset(&where, 0, sizeof(where));
    if (row == NULL || (keep_values && views == NULL)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (!compile_where(db, &resolver, del->where, &where, &status)) goto done;
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, opened, "open");
        goto done;
    }
    status = collect_matches(db, &view, id, heap, &where, row, keep_values, false, &matches, NULL, NULL);
    if (status != DATABASE_EXEC_OK) goto done;
    if (matches.count == 0) {
        status = returning_finish(db, ret, "DELETE", 0, &success);
        if (status != DATABASE_EXEC_OK) goto done;
        *out = success;
        success = NULL;
        goto done;
    }
    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began == HEAP_OK) began = configure_secondary(batch, work.cat, id);
    if (began != HEAP_OK) {
        status = heap_failure(db, began, "batch");
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        if (keep_values) {
            for (size_t i = 0; i < n; i++) views[i] = matches.items[m].cells[i].view;
            heap_status_t unindexed = heap_batch_secondary_remove(batch, views, n, matches.items[m].locator);
            if (unindexed != HEAP_OK) {
                status = heap_failure(db, unindexed, "index");
                goto done;
            }
        }
        heap_status_t removed = heap_batch_delete(batch, matches.items[m].locator);
        if (removed != HEAP_OK) {
            status = heap_failure(db, removed, "delete");
            goto done;
        }
        status = returning_row(ret, matches.items[m].cells);
        if (status != DATABASE_EXEC_OK) goto done;
    }
    if (catalog_set_row_count(work.cat, id, view.row_count - matches.count) != CATALOG_OK) {
        status = fail_result(db, RESULT_E_INTERNAL, 18, "행 수를 갱신하지 못했습니다");
        goto done;
    }
    status = returning_finish(db, ret, "DELETE", matches.count, &success);
    if (status != DATABASE_EXEC_OK) goto done;
    status = publish(db, &work, id, batch, NULL, 0, success, out);
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_close(heap);
    work_close(db, &work);
    result_release(&success);
    matches_free(db, &matches);
    if (row != NULL) clear_cells(row, n);
    q_free(db, row, row_bytes);
    q_free(db, views, views_bytes);
    where_clear(db, &where);
    returning_close(db, ret);
    return status;
}
