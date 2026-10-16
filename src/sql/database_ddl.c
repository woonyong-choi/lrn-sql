/*
 * database_ddl.c — CREATE·DROP TABLE·INDEX와 기본 키 색인 열기.
 */
#include "database_exec.h"

/* ── CREATE · DROP ── */

static bool has_subquery(const sqlexpr_t *expr)
{
    if (expr->subquery != NULL) return true;
    for (size_t i = 0; i < expr->arg_count; i++) {
        if (has_subquery(expr->args[i])) return true;
    }
    return false;
}

/* DEFAULT 식을 계산해 컬럼 형식으로 바꾼다. 우리 함수는 모두 결정적이라 CREATE 때 한 번 계산해도 같다 */
static bool column_default(database_t *db, const sqlstmt_column_def_t *def, value_t *out,
                           database_exec_status_t *status)
{
    if (has_subquery(def->default_value)) {
        *status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_NOT_CONSTANT, "DEFAULT에는 서브쿼리를 쓸 수 없습니다");
        return false;
    }
    value_t computed = {0};
    if (!eval_constant(db, def->default_value, &computed, status)) return false;
    catalog_column_t column = {(const char *)def->name.bytes, def->name.size, def->type, def->not_null, false, {0}};
    bool cast = cast_for_column(db, &computed.view, &column, out, status);
    value_clear(&computed);
    if (cast && (out->view.type == VALUE_TEXT || out->view.type == VALUE_BLOB) &&
        out->view.size_bytes > CATALOG_DEFAULT_MAX_BYTES) {
        *status = fail_result(db, RESULT_E_LIMIT, 5, "DEFAULT 값이 %u바이트를 넘습니다", CATALOG_DEFAULT_MAX_BYTES);
        return false;
    }
    return cast;
}

database_exec_status_t exec_create(database_t *db, const sqlstmt_create_t *create, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    size_t columns_bytes = create->column_count * sizeof(catalog_column_spec_t);
    size_t defaults_bytes = create->column_count * sizeof(value_t);
    catalog_column_spec_t *columns = q_malloc(db, columns_bytes);
    value_t *defaults = q_calloc(db, create->column_count, sizeof(value_t));
    if (columns == NULL || defaults == NULL) {
        q_free(db, columns, columns_bytes);
        q_free(db, defaults, defaults_bytes);
        return DATABASE_EXEC_NOMEM;
    }
    size_t names_bytes = create->table.size;
    for (size_t i = 0; i < create->column_count; i++) {
        const sqlstmt_column_def_t *def = &create->columns[i];
        /* 기본 키는 NOT NULL이다(PostgreSQL과 같음. SQLite는 INTEGER 키에 번호를 매겨 서로 다르다) */
        bool is_key = i == create->primary_key_column && !create->has_identity;
        columns[i] = (catalog_column_spec_t){(const char *)def->name.bytes, def->name.size, def->type,
                                             def->not_null || is_key, NULL};
        names_bytes += def->name.size;
        if (def->default_value == NULL) continue;
        if (!column_default(db, def, &defaults[i], &status)) goto done;
        /* DEFAULT NULL은 기본값이 없는 것과 같다 */
        if (defaults[i].view.type != VALUE_NULL) columns[i].default_value = &defaults[i].view;
        names_bytes += defaults[i].view.type == VALUE_TEXT || defaults[i].view.type == VALUE_BLOB
                           ? defaults[i].view.size_bytes : 0;
    }
    catalog_table_spec_t spec = {(const char *)create->table.bytes, create->table.size, columns,
                                 create->column_count, create->primary_key_column, create->has_identity};
    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    catalog_table_id_t id;
    size_t create_peak = catalog_create_peak_bytes(work.cat, create->column_count, names_bytes);
    if (!q_reserve(db, create_peak)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    catalog_status_t made = catalog_create_table(work.cat, &spec, &id);
    work_settle(db, &work, create_peak);
    if (made == CATALOG_EXISTS) {
        status = fail_result(db, RESULT_E_SEMANTIC, 4, "이미 있는 테이블입니다: %.*s",
                             name_len(create->table.size), (const char *)create->table.bytes);
        goto done;
    }
    if (made == CATALOG_NOMEM) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (made == CATALOG_LIMIT) {
        status = fail_result(db, RESULT_E_LIMIT, 5, "카탈로그 한도를 넘습니다");
        goto done;
    }
    if (made != CATALOG_OK) {
        status = fail_result(db, RESULT_E_SEMANTIC, 6, "테이블 정의가 올바르지 않습니다");
        goto done;
    }
    if (open_command(db, "CREATE TABLE", 0, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, &work, id, NULL, NULL, 0, success, out);
    success = NULL;
done:
    work_close(db, &work);
    result_release(&success);
    q_free(db, columns, columns_bytes);
    owned_clear(db, defaults, create->column_count);
    q_free(db, defaults, defaults_bytes);
    return status;
}

database_exec_status_t exec_create_index(database_t *db, const sqlstmt_index_t *index, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &index->table, &id, &view, &status)) return status;
    size_t column = find_column(&view, &index->column);
    if (column == SIZE_MAX) return unknown_column(db, &index->column);
    work_t work = {NULL, 0};
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    result_t *success = NULL;
    if (!work_clone(db, &work)) return DATABASE_EXEC_NOMEM;
    size_t peak = catalog_add_index_peak_bytes(work.cat, index->name.size);
    if (!q_reserve(db, peak)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    /* 정수 기본 키가 있으면 비고유 색인의 꼬리를 기본 키로 둬 같은 값의 행을 기본 키 순서로 읽게 한다 */
    bool ordered = !index->unique && view.primary_key_column != CATALOG_NO_COLUMN &&
                   view.columns[view.primary_key_column].type == VALUE_INTEGER;
    catalog_status_t added = catalog_add_index(work.cat, id, (const char *)index->name.bytes,
                                               index->name.size, (uint16_t)column, index->unique, ordered);
    work_settle(db, &work, peak);
    if (added != CATALOG_OK) {
        if (added == CATALOG_NOMEM) status = DATABASE_EXEC_NOMEM;
        else if (added == CATALOG_LIMIT) status = fail_result(db, RESULT_E_LIMIT, 5, "인덱스 한도를 넘습니다");
        else status = fail_result(db, RESULT_E_SEMANTIC, 4, "인덱스 이름 또는 컬럼이 올바르지 않습니다");
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
        status = heap_failure(db, began, "index");
        goto done;
    }
    size_t index_position = catalog_index_count(work.cat, id) - 1u;
    heap_status_t built = heap_batch_secondary_build(batch, index_position, heap);
    if (built != HEAP_OK) {
        if (built == HEAP_KEY_EXISTS) status = fail_result(db, RESULT_E_CONSTRAINT, 11,
                                                            "기존 행에 중복된 인덱스 값이 있습니다");
        else status = heap_failure(db, built, "index build");
        goto done;
    }
    if (open_command(db, "CREATE INDEX", 0, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, &work, id, batch, NULL, 0, success, out);
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_close(heap);
    work_close(db, &work);
    result_release(&success);
    return status;
}

database_exec_status_t exec_drop(database_t *db, const sqlstmt_drop_t *drop, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &drop->table, &id, &view, &status)) return status;

    heap_t *heap = NULL;
    uint32_t *pages = NULL;
    size_t page_count = 0;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) return heap_failure(db, opened, "open");
    size_t index_count = catalog_index_count(base_catalog(db), id);
    uint32_t roots[CATALOG_MAX_INDEXES];
    if (index_count > CATALOG_MAX_INDEXES) {
        status = heap_failure(db, HEAP_CORRUPT, "index count");
        goto done;
    }
    for (size_t i = 0; i < index_count; i++) {
        catalog_index_view_t index;
        if (catalog_index_at(base_catalog(db), id, i, &index) != CATALOG_OK) {
            status = heap_failure(db, HEAP_CORRUPT, "index catalog");
            goto done;
        }
        roots[i] = index.root;
    }
    heap_status_t collected = heap_collect_pages_with_indexes(heap, roots, index_count,
                                                                &pages, &page_count);
    if (collected != HEAP_OK) {
        status = heap_failure(db, collected, "collect");
        goto done;
    }
    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (catalog_drop_table(work.cat, id) != CATALOG_OK) {
        status = fail_result(db, RESULT_E_INTERNAL, 7, "테이블을 지우지 못했습니다");
        goto done;
    }
    work_settle(db, &work, 0);
    if (open_command(db, "DROP TABLE", 0, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, &work, id, NULL, pages, page_count, success, out);
    success = NULL;
done:
    work_close(db, &work);
    result_release(&success);
    heap_free_pages(heap, pages, page_count);
    heap_close(heap);
    return status;
}

/* ── 기본 키 색인 ── */

/* INSERT의 중복 검사와 identity의 현재 최대 키를 전체 스캔 없이 색인에서 얻는다. */
database_exec_status_t probe_key_index(database_t *db, const catalog_table_view_t *view, heap_t *heap,
                                              const value_t *key, bool check_pk, bool need_max,
                                              bool *any, int64_t *max_key)
{
    if (check_pk) {
        heap_locator_t at;
        heap_status_t found = key->view.type == VALUE_TEXT
                                  ? heap_find_text_key(heap, key->view.bytes, key->view.size_bytes, &at)
                                  : heap_find_key(heap, key->view.integer, &at);
        if (found == HEAP_OK) {
            size_t pk = view->primary_key_column;
            return fail_result(db, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s",
                               name_len(view->columns[pk].name_bytes), view->columns[pk].name);
        }
        if (found != HEAP_NOT_FOUND) return heap_failure(db, found, "lookup");
    }
    if (!need_max) return DATABASE_EXEC_OK;
    heap_status_t maximum = heap_max_key(heap, any, max_key);
    return maximum == HEAP_OK ? DATABASE_EXEC_OK : heap_failure(db, maximum, "max");
}
