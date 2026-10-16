/*
 * database_insert.c — INSERT와 INSERT ... ON CONFLICT.
 */
#include "database_exec.h"

/* 충돌을 볼 유일 제약: 기본 키 색인이거나 batch의 유일 보조 색인 */
typedef struct {
    size_t column;
    bool primary;
    size_t index;
} conflict_target_t;

/* 이 문장에서 넣거나 바꾼 행(DO UPDATE가 같은 행을 두 번 바꾸지 않게 한다) */
typedef struct {
    uint64_t *slots;            /* 0은 빈 자리. 값은 (directory_index << 16 | slot) + 1 */
    size_t cap;
    size_t count;
} locator_set_t;

typedef struct {
    const sqlstmt_conflict_t *spec;
    conflict_target_t targets[1 + CATALOG_MAX_INDEXES];
    size_t target_count;
    source_ref_t sources[2];    /* 이미 있는 행, excluded */
    resolver_t existing;        /* sources[0]만. 바깥 열을 보는 서브쿼리가 쓴다 */
    size_t *columns;            /* SET 대상 열 */
    sqleval_prog_t **progs;
    sqleval_prog_t *where;
    value_t *combined;          /* [이미 있는 행 n][excluded n] */
    value_view_t *views;
    size_t n;
    locator_set_t touched;
    uint64_t updated;
} conflict_t;

/* ── INSERT ── */

database_exec_status_t duplicate_key(database_t *db, const catalog_table_view_t *view)
{
    size_t pk = view->primary_key_column;
    return fail_result(db, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s", name_len(view->columns[pk].name_bytes),
                       view->columns[pk].name);
}

database_exec_status_t not_null_violation(database_t *db, const catalog_column_t *column)
{
    return fail_result(db, RESULT_E_CONSTRAINT, 8, "NOT NULL 제약을 어겼습니다: %.*s",
                       name_len(column->name_bytes), column->name);
}

/* 상수식 하나를 계산한다(INSERT 값). 열을 쓰면 NOT_CONSTANT 오류다 */
bool eval_constant(database_t *db, const sqlexpr_t *expr, value_t *out, database_exec_status_t *status)
{
    sqleval_prog_t *prog = NULL;
    if (!compile_expr(db, expr, NULL, &prog, status)) return false;
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_status_t ran = sqleval_run(prog, NULL, out, &error);
    sqleval_free(prog);
    if (ran == SQLEVAL_OK) return true;
    *status = eval_failure(db, &error);
    return false;
}

/* 색인 없는 표에서 key와 같은 기본 키가 있는지, identity용 최대 키를 훑어서 구한다 */
static database_exec_status_t scan_keys(database_t *db, heap_t *heap, size_t n, size_t pk, const value_t *key,
                                        bool *duplicate, bool *any, int64_t *max_key)
{
    size_t scanned_bytes = n * sizeof(value_t);
    value_t *scanned = q_calloc(db, n, sizeof(*scanned));
    if (scanned == NULL) return DATABASE_EXEC_NOMEM;
    heap_cursor_t *cursor = NULL;
    database_exec_status_t status = DATABASE_EXEC_OK;
    heap_status_t opened = heap_cursor_open(heap, &cursor);
    if (opened != HEAP_OK) status = heap_failure(db, opened, "cursor");
    while (status == DATABASE_EXEC_OK) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            break;
        }
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, scanned, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, next, "scan");
            break;
        }
        if (key != NULL && values_equal(&scanned[pk].view, &key->view)) *duplicate = true;
        if (scanned[pk].view.type == VALUE_INTEGER && (!*any || scanned[pk].view.integer > *max_key)) {
            *max_key = scanned[pk].view.integer;
            *any = true;
        }
        clear_cells(scanned, n);
    }
    heap_cursor_close(cursor);
    clear_cells(scanned, n);
    q_free(db, scanned, scanned_bytes);
    return status;
}

/* ── INSERT ... ON CONFLICT ── */

static const sqlstmt_name_t EXCLUDED_NAME = {(uint8_t *)"excluded", 8};

static uint64_t locator_key(heap_locator_t at) { return (((uint64_t)at.directory_index << 16) | at.slot) + 1; }

static size_t locator_slot(const locator_set_t *set, uint64_t key)
{
    size_t at = (size_t)(key * UINT64_C(0x9E3779B97F4A7C15) >> 32) & (set->cap - 1);
    while (set->slots[at] != 0 && set->slots[at] != key) at = (at + 1) & (set->cap - 1);
    return at;
}

static bool locator_seen(const locator_set_t *set, heap_locator_t at)
{
    return set->cap > 0 && set->slots[locator_slot(set, locator_key(at))] != 0;
}

static database_exec_status_t locator_add(database_t *db, locator_set_t *set, heap_locator_t at)
{
    if ((set->count + 1) * 2 > set->cap) {
        size_t cap = set->cap == 0 ? 64 : set->cap * 2;
        uint64_t *grown = q_calloc(db, cap, sizeof(uint64_t));
        if (grown == NULL) return DATABASE_EXEC_NOMEM;
        locator_set_t bigger = {grown, cap, 0};
        for (size_t i = 0; i < set->cap; i++) {
            if (set->slots[i] != 0) bigger.slots[locator_slot(&bigger, set->slots[i])] = set->slots[i];
        }
        bigger.count = set->count;
        q_free(db, set->slots, set->cap * sizeof(uint64_t));
        *set = bigger;
    }
    size_t slot = locator_slot(set, locator_key(at));
    if (set->slots[slot] == 0) {
        set->slots[slot] = locator_key(at);
        set->count++;
    }
    return DATABASE_EXEC_OK;
}

/*
 * DO UPDATE 식의 열: excluded.c는 넣으려던 값, 테이블.c는 이미 있는 행. 한정 없는 이름은 두 쪽에 다 있어 모호하다
 * (PostgreSQL과 같음. SQLite는 이미 있는 행으로 받아 서로 다르다)
 */
static size_t conflict_resolve(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name,
                               value_type_t *type)
{
    conflict_t *c = ctx;
    bool excluded = qualifier->size == EXCLUDED_NAME.size && memcmp(qualifier->bytes, EXCLUDED_NAME.bytes, 8) == 0;
    resolver_t one = {&c->sources[excluded ? 1 : 0], 1};
    const sqlstmt_name_t none = {NULL, 0};
    size_t column = resolve_column(&one, excluded ? &none : qualifier, name, type);
    return qualifier->size == 0 && column != SQLEVAL_NO_COLUMN ? SQLEVAL_AMBIGUOUS : column;
}

static bool conflict_compile(database_t *db, conflict_t *c, const sqlexpr_t *expr, sqleval_prog_t **prog,
                             database_exec_status_t *status)
{
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_env_t env = {conflict_resolve, NULL, c, subquery_hook, db, outer_hook};
    const resolver_t *saved = db->compile_resolver;
    db->compile_resolver = &c->existing;
    sqleval_status_t compiled = sqleval_compile(expr, &env, prog, &error);
    db->compile_resolver = saved;
    if (compiled == SQLEVAL_OK) return true;
    *status = eval_failure(db, &error);
    return false;
}

static void conflict_close(database_t *db, conflict_t *c)
{
    size_t count = c->spec != NULL ? c->spec->assignment_count : 0;
    for (size_t i = 0; c->progs != NULL && i < count; i++) sqleval_free(c->progs[i]);
    q_free(db, c->progs, (count > 0 ? count : 1) * sizeof(sqleval_prog_t *));
    q_free(db, c->columns, (count > 0 ? count : 1) * sizeof(size_t));
    sqleval_free(c->where);
    if (c->combined != NULL) owned_clear(db, c->combined + c->n, c->n);
    q_free(db, c->combined, (c->n > 0 ? 2 * c->n : 1) * sizeof(value_t));
    q_free(db, c->views, (c->n > 0 ? c->n : 1) * sizeof(value_view_t));
    q_free(db, c->touched.slots, c->touched.cap * sizeof(uint64_t));
    memset(c, 0, sizeof(*c));
}

static bool unique_indexed(const catalog_t *catalog, catalog_table_id_t id, size_t column, size_t *index)
{
    size_t count = catalog_index_count(catalog, id);
    for (size_t i = 0; i < count; i++) {
        catalog_index_view_t view;
        if (catalog_index_at(catalog, id, i, &view) == CATALOG_OK && view.unique && view.column == column) {
            *index = i;
            return true;
        }
    }
    return false;
}

static database_exec_status_t conflict_open(database_t *db, const sqlstmt_insert_t *insert,
                                            const catalog_table_view_t *view, catalog_table_id_t id, bool indexed,
                                            conflict_t *c)
{
    memset(c, 0, sizeof(*c));
    const sqlstmt_conflict_t *spec = &insert->conflict;
    if (!spec->present) return DATABASE_EXEC_OK;
    c->spec = spec;
    c->n = view->column_count;
    const catalog_t *catalog = base_catalog(db);
    size_t pk = view->primary_key_column;
    if (spec->has_target) {
        size_t column = find_column(view, &spec->target);
        if (column == SIZE_MAX) return unknown_column(db, &spec->target);
        size_t index = 0;
        if (column == pk && indexed) {
            c->targets[c->target_count++] = (conflict_target_t){column, true, 0};
        } else if (unique_indexed(catalog, id, column, &index)) {
            c->targets[c->target_count++] = (conflict_target_t){column, false, index};
        } else {
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_CONFLICT, "ON CONFLICT 대상 열에 유일 색인이 없습니다: %.*s",
                               name_len(spec->target.size), (const char *)spec->target.bytes);
        }
    } else {
        if (pk != NO_COLUMN) {
            if (!indexed) {
                return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_CONFLICT, "색인이 없는 기본 키에는 ON CONFLICT를 쓸 수 없습니다");
            }
            c->targets[c->target_count++] = (conflict_target_t){pk, true, 0};
        }
        size_t count = catalog_index_count(catalog, id);
        for (size_t i = 0; i < count; i++) {
            catalog_index_view_t index;
            if (catalog_index_at(catalog, id, i, &index) == CATALOG_OK && index.unique) {
                c->targets[c->target_count++] = (conflict_target_t){index.column, false, i};
            }
        }
    }
    c->combined = q_calloc(db, c->n > 0 ? 2 * c->n : 1, sizeof(value_t));
    c->views = q_malloc(db, (c->n > 0 ? c->n : 1) * sizeof(value_view_t));
    if (c->combined == NULL || c->views == NULL) return DATABASE_EXEC_NOMEM;
    if (!spec->do_update) return DATABASE_EXEC_OK;
    c->sources[0] = (source_ref_t){view, &insert->table, NULL, 0, false};
    c->sources[1] = (source_ref_t){view, &EXCLUDED_NAME, NULL, c->n, false};
    c->existing = (resolver_t){&c->sources[0], 1};
    size_t count = spec->assignment_count;
    c->columns = q_calloc(db, count > 0 ? count : 1, sizeof(size_t));
    c->progs = q_calloc(db, count > 0 ? count : 1, sizeof(sqleval_prog_t *));
    if (c->columns == NULL || c->progs == NULL) return DATABASE_EXEC_NOMEM;
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (size_t i = 0; i < count; i++) {
        const sqlstmt_assignment_t *assignment = &spec->assignments[i];
        size_t column = find_column(view, &assignment->column);
        if (column == SIZE_MAX) return unknown_column(db, &assignment->column);
        size_t unused;
        if (column == pk || unique_indexed(catalog, id, column, &unused)) {
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_CONFLICT,
                               "ON CONFLICT DO UPDATE로 기본 키나 유일 색인 열은 바꿀 수 없습니다: %.*s",
                               name_len(assignment->column.size), (const char *)assignment->column.bytes);
        }
        c->columns[i] = column;
        if (!conflict_compile(db, c, assignment->value, &c->progs[i], &status)) return status;
    }
    if (spec->where != NULL) {
        if (!conflict_compile(db, c, spec->where, &c->where, &status)) return status;
        value_type_t type = sqleval_type(c->where);
        if (type != VALUE_INTEGER && type != VALUE_NULL) {
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_TYPE, "ON CONFLICT의 WHERE 조건은 참·거짓이어야 합니다");
        }
    }
    return DATABASE_EXEC_OK;
}

/* 넣으려는 행이 대상 제약 중 하나와 충돌하면 그 행의 위치를 준다 */
static database_exec_status_t conflict_find(database_t *db, conflict_t *c, heap_batch_t *batch, const value_t *cells,
                                            bool *found, heap_locator_t *at)
{
    *found = false;
    for (size_t t = 0; t < c->target_count && !*found; t++) {
        const conflict_target_t *target = &c->targets[t];
        const value_view_t *value = &cells[target->column].view;
        if (value->type == VALUE_NULL) continue;  /* NULL은 어떤 값과도 충돌하지 않는다 */
        heap_status_t looked = target->primary ? heap_batch_find_key(batch, value, at)
                                               : heap_batch_secondary_find(batch, target->index, value, at);
        if (looked == HEAP_OK) *found = true;
        else if (looked != HEAP_NOT_FOUND) return heap_failure(db, looked, "conflict lookup");
    }
    return DATABASE_EXEC_OK;
}

/* DO UPDATE: 충돌한 행을 SET으로 바꾼다. WHERE가 거짓이면 아무것도 하지 않는다 */
static database_exec_status_t conflict_update(database_t *db, conflict_t *c, heap_t *heap, heap_batch_t *batch,
                                              const catalog_table_view_t *view, heap_locator_t at, const value_t *cells,
                                              returning_t *ret)
{
    if (locator_seen(&c->touched, at)) {
        return fail_result(db, RESULT_E_CONSTRAINT, DATABASE_CODE_CONFLICT,
                           "ON CONFLICT DO UPDATE는 한 문장에서 같은 행을 두 번 바꿀 수 없습니다");
    }
    size_t n = c->n;
    value_t *existing = c->combined;
    memset(existing, 0, n * sizeof(value_t));
    heap_status_t got = heap_batch_get(batch, at, existing);
    if (got != HEAP_OK) return heap_failure(db, got, "conflict row");
    for (size_t i = 0; i < n; i++) c->combined[n + i] = (value_t){cells[i].view, false};
    database_exec_status_t status = DATABASE_EXEC_OK;
    bool apply = true;
    if (c->where != NULL) {
        sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
        if (sqleval_test(c->where, c->combined, &apply, &error) != SQLEVAL_OK) status = eval_failure(db, &error);
    }
    size_t count = c->spec->assignment_count;
    value_t *fresh = NULL;
    size_t fresh_bytes = (count > 0 ? count : 1) * sizeof(value_t);
    if (status == DATABASE_EXEC_OK && apply) {
        fresh = q_calloc(db, count > 0 ? count : 1, sizeof(value_t));
        if (fresh == NULL) status = DATABASE_EXEC_NOMEM;
    }
    for (size_t i = 0; status == DATABASE_EXEC_OK && apply && i < count; i++) {
        value_t computed = {0};
        sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
        if (sqleval_run(c->progs[i], c->combined, &computed, &error) != SQLEVAL_OK) {
            status = eval_failure(db, &error);
            break;
        }
        const catalog_column_t *column = &view->columns[c->columns[i]];
        bool cast = cast_for_column(db, &computed.view, column, &fresh[i], &status);
        value_clear(&computed);
        if (cast && column->not_null && fresh[i].view.type == VALUE_NULL) status = not_null_violation(db, column);
    }
    if (status == DATABASE_EXEC_OK && apply) {
        for (size_t i = 0; i < n; i++) c->views[i] = existing[i].view;
        heap_status_t changed = heap_batch_secondary_remove(batch, c->views, n, at);
        for (size_t i = 0; i < count; i++) c->views[c->columns[i]] = fresh[i].view;
        heap_locator_t placed = at;
        if (changed == HEAP_OK) changed = heap_batch_update(batch, at, c->views, n, &placed);
        if (changed == HEAP_OK) changed = heap_batch_secondary_insert(batch, c->views, n, placed);
        if (changed != HEAP_OK) status = heap_failure(db, changed, "conflict update");
        if (status == DATABASE_EXEC_OK) status = locator_add(db, &c->touched, at);
        if (status == DATABASE_EXEC_OK) status = locator_add(db, &c->touched, placed);
        if (status == DATABASE_EXEC_OK && ret != NULL) {
            /* 돌려줄 행: 이미 있는 행 자리를 바뀐 값으로 빌려 쓴다 */
            value_t *row = c->combined + n;
            for (size_t i = 0; i < n; i++) row[i] = (value_t){c->views[i], false};
            status = returning_row(ret, row);
        }
        if (status == DATABASE_EXEC_OK) c->updated++;
    }
    if (fresh != NULL) owned_clear(db, fresh, count);
    q_free(db, fresh, fresh_bytes);
    heap_release_row(heap, existing);
    memset(c->combined, 0, 2 * n * sizeof(value_t));
    return status;
}

database_exec_status_t exec_insert(database_t *db, const sqlstmt_insert_t *insert, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &insert->table, &id, &view, &status)) return status;
    size_t n = view.column_count;
    returning_t *ret = NULL;
    status = returning_open(db, &insert->returning, &view, &insert->table, &ret);
    if (status != DATABASE_EXEC_OK) return status;

    size_t cells_bytes = n * sizeof(value_t), source_bytes = n * sizeof(size_t), views_bytes = n * sizeof(value_view_t);
    value_t *cells = q_calloc(db, n, sizeof(*cells));
    size_t *source = q_malloc(db, source_bytes);  /* 컬럼 번호 -> 행 안의 값 번호 */
    value_view_t *views = q_malloc(db, views_bytes);
    value_t *earlier = NULL;                        /* 색인 없는 표에서 이 문장이 앞서 넣은 기본 키 */
    size_t earlier_bytes = 0, earlier_count = 0;
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    conflict_t conflict;
    memset(&conflict, 0, sizeof(conflict));
    uint64_t inserted_rows = 0;
    if (cells == NULL || source == NULL || views == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) source[i] = SIZE_MAX;
    if (insert->has_columns) {
        for (size_t i = 0; i < insert->column_count; i++) {
            size_t column = find_column(&view, &insert->columns[i]);
            if (column == SIZE_MAX) {
                status = unknown_column(db, &insert->columns[i]);
                goto done;
            }
            if (source[column] != SIZE_MAX) {
                status = fail_result(db, RESULT_E_SEMANTIC, 9, "컬럼이 두 번 나옵니다: %.*s",
                                     name_len(insert->columns[i].size), (const char *)insert->columns[i].bytes);
                goto done;
            }
            source[column] = i;
        }
    } else {
        if (insert->value_count != n) {
            status = fail_result(db, RESULT_E_SEMANTIC, 10, "값 개수(%zu)가 컬럼 수(%zu)와 다릅니다",
                                 insert->value_count, n);
            goto done;
        }
        for (size_t i = 0; i < n; i++) source[i] = i;
    }

    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, opened, "open");
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began == HEAP_OK) began = configure_secondary(batch, work.cat, id);
    if (began != HEAP_OK) {
        status = heap_failure(db, began, "batch");
        goto done;
    }
    size_t pk = view.primary_key_column;
    bool indexed = heap_is_indexed(heap);
    status = conflict_open(db, insert, &view, id, indexed, &conflict);
    if (status != DATABASE_EXEC_OK) goto done;
    bool max_known = false, any = false;
    int64_t max_key = INT64_MIN;
    if (pk != NO_COLUMN && !indexed) {
        earlier_bytes = insert->row_count * sizeof(value_t);
        earlier = q_calloc(db, insert->row_count > 0 ? insert->row_count : 1, sizeof(value_t));
        if (earlier == NULL) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
    }
    for (size_t r = 0; r < insert->row_count; r++) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        owned_clear(db, cells, n);
        for (size_t i = 0; i < n; i++) {
            if (source[i] == SIZE_MAX) {
                /* 목록에 없는 컬럼은 기본값(없으면 NULL) */
                if (view.columns[i].has_default && q_clone(db, &view.columns[i].default_value, &cells[i]) != VALUE_OK) {
                    status = DATABASE_EXEC_NOMEM;
                    goto done;
                }
                continue;
            }
            value_t computed = {0};
            if (!eval_constant(db, insert->values[r * insert->value_count + source[i]], &computed, &status)) goto done;
            bool cast = cast_for_column(db, &computed.view, &view.columns[i], &cells[i], &status);
            value_clear(&computed);
            if (!cast) goto done;
        }
        if (conflict.spec != NULL) {
            /* 넣으려는 행의 NOT NULL을 먼저 본다(자동 번호 키는 아직 비어 있다) */
            for (size_t i = 0; i < n; i++) {
                bool auto_key = i == pk && view.has_identity;
                if (!auto_key && view.columns[i].not_null && cells[i].view.type == VALUE_NULL) {
                    status = not_null_violation(db, &view.columns[i]);
                    goto done;
                }
            }
            bool found = false;
            heap_locator_t at;
            status = conflict_find(db, &conflict, batch, cells, &found, &at);
            if (status != DATABASE_EXEC_OK) goto done;
            if (found) {
                if (conflict.spec->do_update) {
                    status = conflict_update(db, &conflict, heap, batch, &view, at, cells, ret);
                    if (status != DATABASE_EXEC_OK) goto done;
                }
                continue;
            }
        }
        bool auto_id = pk != NO_COLUMN && view.has_identity && cells[pk].view.type == VALUE_NULL;
        bool check_pk = pk != NO_COLUMN && cells[pk].view.type != VALUE_NULL;
        if (auto_id || check_pk) {
            if (indexed) {
                bool any_now = false;
                int64_t max_now = INT64_MIN;
                status = probe_key_index(db, &view, heap, &cells[pk], check_pk, auto_id && !max_known, &any_now, &max_now);
                if (status != DATABASE_EXEC_OK) goto done;
                if (auto_id && !max_known && any_now && (!any || max_now > max_key)) {
                    any = true;
                    max_key = max_now;
                }
                if (auto_id) max_known = true;
            } else {
                bool duplicate = false;
                status = scan_keys(db, heap, n, pk, check_pk ? &cells[pk] : NULL, &duplicate, &any, &max_key);
                if (status != DATABASE_EXEC_OK) goto done;
                for (size_t e = 0; e < earlier_count && check_pk && !duplicate; e++) {
                    duplicate = values_equal(&earlier[e].view, &cells[pk].view);
                }
                if (duplicate) {
                    status = duplicate_key(db, &view);
                    goto done;
                }
            }
            if (auto_id) {
                uint64_t next_value = 0;
                unsigned long skipped = 0;
                for (;;) {
                    catalog_status_t reserved = catalog_reserve_identity(work.cat, id, &next_value);
                    if (reserved != CATALOG_OK) {
                        status = fail_result(db, RESULT_E_LIMIT, 12, "identity 값을 더 줄 수 없습니다");
                        goto done;
                    }
                    if (!any || (int64_t)next_value > max_key) break;
                    if (++skipped > DATABASE_IDENTITY_SKIP_LIMIT) {
                        status = fail_result(db, RESULT_E_LIMIT, 13, "identity 카운터를 기존 최대 키 뒤로 옮기지 못했습니다");
                        goto done;
                    }
                }
                if (value_set_integer(&cells[pk], (int64_t)next_value) != VALUE_OK) {
                    status = DATABASE_EXEC_NOMEM;
                    goto done;
                }
            }
            /* 이 문장이 뒤에서 identity를 줄 때 앞서 넣은 키도 넘어야 한다 */
            if (cells[pk].view.type == VALUE_INTEGER && (!any || cells[pk].view.integer > max_key)) {
                any = true;
                max_key = cells[pk].view.integer;
            }
            if (earlier != NULL && q_clone(db, &cells[pk].view, &earlier[earlier_count]) != VALUE_OK) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            if (earlier != NULL) earlier_count++;
        }
        for (size_t i = 0; i < n; i++) {
            if (view.columns[i].not_null && cells[i].view.type == VALUE_NULL) {
                status = not_null_violation(db, &view.columns[i]);
                goto done;
            }
        }
        for (size_t i = 0; i < n; i++) views[i] = cells[i].view;
        heap_locator_t placed;
        heap_status_t inserted = heap_batch_insert(batch, views, n, &placed);
        if (inserted == HEAP_OK) inserted = heap_batch_secondary_insert(batch, views, n, placed);
        if (inserted != HEAP_OK) {
            status = heap_failure(db, inserted, "insert");
            goto done;
        }
        status = returning_row(ret, cells);
        if (status == DATABASE_EXEC_OK && conflict.spec != NULL) status = locator_add(db, &conflict.touched, placed);
        if (status != DATABASE_EXEC_OK) goto done;
        inserted_rows++;
    }
    if (catalog_set_row_count(work.cat, id, view.row_count + inserted_rows) != CATALOG_OK) {
        status = fail_result(db, RESULT_E_LIMIT, 14, "행 수 한도를 넘습니다");
        goto done;
    }
    status = returning_finish(db, ret, "INSERT", inserted_rows + conflict.updated, &success);
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
    if (earlier != NULL) owned_clear(db, earlier, earlier_count);
    q_free(db, earlier, earlier_bytes);
    conflict_close(db, &conflict);
    returning_close(db, ret);
    q_free(db, views, views_bytes);
    if (cells != NULL) owned_clear(db, cells, n);
    q_free(db, cells, cells_bytes);
    q_free(db, source, source_bytes);
    return status;
}
