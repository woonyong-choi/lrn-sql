#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/database.h"
#include "sql/sql_statement.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/page_file.h"

struct database {
    page_file_t *file;
    catalog_t *catalog;   /* 게시된 카탈로그. 문장이 성공했을 때만 교체한다 */
    bool writable;
    bool poisoned;
};

static const lexer_limits_t LEXER_LIMITS = {
    (size_t)16 << 20, (size_t)16 << 20, (size_t)1 << 20, LEXER_PARAMETER_CEILING
};

#define NO_COLUMN CATALOG_NO_COLUMN

/* ── 열기·닫기 ── */

static database_status_t from_page_file(page_file_status_t status)
{
    switch (status) {
        case PAGE_FILE_OK: return DATABASE_OK;
        case PAGE_FILE_INVALID: return DATABASE_INVALID;
        case PAGE_FILE_CORRUPT:
        case PAGE_FILE_NO_CONTROL: return DATABASE_CORRUPT;
        case PAGE_FILE_UNSUPPORTED: return DATABASE_UNSUPPORTED;
        case PAGE_FILE_EXISTS: return DATABASE_EXISTS;
        case PAGE_FILE_LOCKED: return DATABASE_LOCKED;
        case PAGE_FILE_NOMEM: return DATABASE_NOMEM;
        default: return DATABASE_IO;
    }
}

static database_status_t from_store(catalog_store_status_t status)
{
    switch (status) {
        case CATALOG_STORE_OK: return DATABASE_OK;
        case CATALOG_STORE_NOMEM: return DATABASE_NOMEM;
        case CATALOG_STORE_UNSUPPORTED: return DATABASE_UNSUPPORTED;
        case CATALOG_STORE_IO:
        case CATALOG_STORE_OUTCOME_UNKNOWN: return DATABASE_IO;
        case CATALOG_STORE_INVALID: return DATABASE_INVALID;
        default: return DATABASE_CORRUPT;
    }
}

database_status_t database_create(const char *path, database_t **out)
{
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    database_t *db = calloc(1, sizeof(*db));
    if (db == NULL) return DATABASE_NOMEM;
    db->writable = true;
    page_file_status_t created = page_file_create(path, &db->file);
    if (created != PAGE_FILE_OK) {
        int saved = errno;
        free(db);
        errno = saved;
        return from_page_file(created);
    }
    database_status_t status = DATABASE_OK;
    catalog_status_t opened = catalog_open_empty(&db->catalog);
    if (opened != CATALOG_OK) {
        status = DATABASE_NOMEM;
    } else {
        catalog_store_status_t saved = catalog_store_save(db->file, db->catalog);
        status = from_store(saved);
        if (status == DATABASE_OK) {
            page_file_status_t published = page_file_publish(db->file);
            /* PUBLISHED_DIRTY는 파일이 열리지만 이름의 내구성을 주장할 수 없다 */
            status = published == PAGE_FILE_OK ? DATABASE_OK : from_page_file(published);
        }
    }
    if (status != DATABASE_OK) {
        int saved = errno;
        catalog_close(db->catalog);
        page_file_close(db->file);
        free(db);
        errno = saved;
        return status;
    }
    *out = db;
    return DATABASE_OK;
}

database_status_t database_open(const char *path, bool writable, database_t **out)
{
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    database_t *db = calloc(1, sizeof(*db));
    if (db == NULL) return DATABASE_NOMEM;
    db->writable = writable;
    page_file_status_t opened = page_file_open(path, writable, &db->file, NULL);
    if (opened != PAGE_FILE_OK) {
        int saved = errno;
        free(db);
        errno = saved;
        return from_page_file(opened);
    }
    catalog_store_status_t loaded = catalog_store_load(db->file, &db->catalog);
    if (loaded != CATALOG_STORE_OK) {
        int saved = errno;
        page_file_close(db->file);
        free(db);
        errno = saved;
        return loaded == CATALOG_STORE_EMPTY ? DATABASE_CORRUPT : from_store(loaded);
    }
    *out = db;
    return DATABASE_OK;
}

database_status_t database_close(database_t *db)
{
    if (db == NULL) return DATABASE_OK;
    catalog_close(db->catalog);
    page_file_status_t closed = page_file_close(db->file);
    free(db);
    return closed == PAGE_FILE_OK ? DATABASE_OK : from_page_file(closed);
}

bool database_is_poisoned(const database_t *db) { return db != NULL && db->poisoned; }

size_t database_table_count(const database_t *db)
{
    return db != NULL ? catalog_table_count(db->catalog) : 0;
}

/* ── 오류 결과 ── */

static database_exec_status_t fail_result(result_t **out, result_error_class_t error_class, int code,
                                          const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static database_exec_status_t fail_result(result_t **out, result_error_class_t error_class, int code,
                                          const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    result_error_t error = {error_class, code, message, strlen(message), false};
    result_t *result = NULL;
    result_status_t opened = result_open_error(&error, &result);
    if (opened == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    if (opened != RESULT_OK) return DATABASE_EXEC_INVALID;
    *out = result;
    return DATABASE_EXEC_OK;
}

static int name_len(size_t size) { return (int)(size < 64 ? size : 64); }

static bool catalog_name_equals(const char *name, size_t size, const sqlstmt_name_t *other)
{
    return size == other->size && (size == 0 || memcmp(name, other->bytes, size) == 0);
}

/* 저장소 오류를 결과로 바꾼다. NOMEM은 결과가 아니라 NOMEM이다. */
static database_exec_status_t heap_failure(database_t *db, result_t **out, heap_status_t status,
                                           const char *where)
{
    if (page_file_is_poisoned(db->file)) db->poisoned = true;
    switch (status) {
        case HEAP_NOMEM: return DATABASE_EXEC_NOMEM;
        case HEAP_IO:
            return fail_result(out, RESULT_E_IO, (int)status, "저장소 I/O 오류 (%s, errno %d)", where, errno);
        case HEAP_LIMIT:
            return fail_result(out, RESULT_E_LIMIT, (int)status, "행이 저장 한도를 넘습니다 (%s)", where);
        default:
            return fail_result(out, RESULT_E_INTERNAL, (int)status,
                               "저장소가 손상됐거나 지원하지 않는 형식입니다 (%s, 상태 %d)", where, (int)status);
    }
}

/* ── 공통 보조 ── */

static void clear_cells(value_t *cells, size_t count)
{
    for (size_t i = 0; i < count; i++) value_clear(&cells[i]);
}

static bool lookup_table(database_t *db, const sqlstmt_name_t *name, catalog_table_id_t *id,
                         catalog_table_view_t *view, result_t **out, database_exec_status_t *status)
{
    catalog_status_t found = catalog_find(db->catalog, (const char *)name->bytes, name->size, id);
    if (found == CATALOG_OK && catalog_describe(db->catalog, *id, view) == CATALOG_OK) return true;
    *status = fail_result(out, RESULT_E_SEMANTIC, 1, "테이블이 없습니다: %.*s", name_len(name->size),
                          (const char *)name->bytes);
    return false;
}

static size_t find_column(const catalog_table_view_t *view, const sqlstmt_name_t *name)
{
    for (size_t i = 0; i < view->column_count; i++) {
        if (catalog_name_equals(view->columns[i].name, view->columns[i].name_bytes, name)) return i;
    }
    return SIZE_MAX;
}

static database_exec_status_t unknown_column(result_t **out, const sqlstmt_name_t *name)
{
    return fail_result(out, RESULT_E_SEMANTIC, 2, "컬럼이 없습니다: %.*s", name_len(name->size),
                       (const char *)name->bytes);
}

static heap_status_t open_heap(database_t *db, const catalog_table_view_t *view,
                               catalog_table_id_t id, heap_t **heap)
{
    value_type_t *types = malloc(view->column_count * sizeof(*types));
    if (types == NULL) return HEAP_NOMEM;
    for (size_t i = 0; i < view->column_count; i++) types[i] = view->columns[i].type;
    heap_schema_t schema = {types, view->column_count};
    heap_status_t status = heap_open(db->file, id, view->heap_root, &schema, heap);
    free(types);
    return status;
}

static catalog_status_t clone_catalog(const catalog_t *source, catalog_t **clone)
{
    uint8_t *bytes = NULL;
    size_t size = 0;
    catalog_status_t status = catalog_encode(source, &bytes, &size);
    if (status != CATALOG_OK) return status;
    status = catalog_decode(bytes, size, clone);
    free(bytes);
    return status;
}

/* 값을 컬럼 타입으로 정확히 변환한다. 실패하면 결과를 만들고 *status에 반환값을 둔다. */
static bool cast_for_column(const value_t *source, const catalog_column_t *column, value_t *target,
                            result_t **out, database_exec_status_t *status)
{
    value_status_t cast = value_cast(&source->view, column->type, target);
    if (cast == VALUE_OK) return true;
    if (cast == VALUE_NOMEM) *status = DATABASE_EXEC_NOMEM;
    else if (cast == VALUE_LIMIT) *status = fail_result(out, RESULT_E_LIMIT, (int)cast, "컬럼 %.*s의 값이 한도를 넘습니다",
                                                          name_len(column->name_bytes), column->name);
    else *status = fail_result(out, RESULT_E_SEMANTIC, (int)cast, "컬럼 %.*s에 저장할 수 없는 값입니다",
                               name_len(column->name_bytes), column->name);
    return false;
}

static bool values_equal(const value_view_t *a, const value_view_t *b)
{
    int order = 1;
    return a->type == b->type && value_compare(a, b, &order) == VALUE_OK && order == 0;
}

/* ── WHERE ── */

typedef struct {
    bool present;
    size_t column;
    sqlstmt_op_t op;
    bool never;          /* NULL과의 비교는 항상 unknown이라 어떤 행도 고르지 않는다 */
    value_t literal;     /* 컬럼 타입으로 변환한 값 */
} where_t;

static void where_clear(where_t *where) { value_clear(&where->literal); }

static bool compile_where(const catalog_table_view_t *view, const sqlstmt_where_t *source,
                          where_t *where, result_t **out, database_exec_status_t *status)
{
    memset(where, 0, sizeof(*where));
    if (!source->present) return true;
    where->present = true;
    where->op = source->op;
    where->column = find_column(view, &source->column);
    if (where->column == SIZE_MAX) {
        *status = unknown_column(out, &source->column);
        return false;
    }
    if (source->op == SQLSTMT_OP_IS_NULL || source->op == SQLSTMT_OP_IS_NOT_NULL) return true;
    if (source->value.view.type == VALUE_NULL) {
        where->never = true;
        return true;
    }
    return cast_for_column(&source->value, &view->columns[where->column], &where->literal, out, status);
}

static bool where_matches(const where_t *where, const value_t *row)
{
    if (!where->present) return true;
    const value_view_t *cell = &row[where->column].view;
    if (where->op == SQLSTMT_OP_IS_NULL) return cell->type == VALUE_NULL;
    if (where->op == SQLSTMT_OP_IS_NOT_NULL) return cell->type != VALUE_NULL;
    if (where->never || cell->type == VALUE_NULL) return false;
    int order = 0;
    if (value_compare(cell, &where->literal.view, &order) != VALUE_OK) return false;
    switch (where->op) {
        case SQLSTMT_OP_EQ: return order == 0;
        case SQLSTMT_OP_NE: return order != 0;
        case SQLSTMT_OP_LT: return order < 0;
        case SQLSTMT_OP_LE: return order <= 0;
        case SQLSTMT_OP_GT: return order > 0;
        case SQLSTMT_OP_GE: return order >= 0;
        default: return false;
    }
}

/* ── 게시 ── */

/*
 * 변경을 게시한다. work·success는 이 함수가 소비한다(성공하면 work가 새 카탈로그가 되고 success가
 * *out이 된다). batch가 있으면 finish부터 한다. 호출자는 batch·heap을 이 함수 뒤에 해제한다.
 */
static database_exec_status_t publish(database_t *db, catalog_t *work, catalog_table_id_t id,
                                      uint32_t primary_root, heap_batch_t *batch,
                                      const uint32_t *drop_pages, size_t drop_count,
                                      result_t *success, result_t **out)
{
    if (batch != NULL) {
        heap_batch_result_t finished;
        heap_status_t finish = heap_batch_finish(batch, &finished);
        if (finish != HEAP_OK) {
            heap_batch_abort(batch);
            catalog_close(work);
            result_release(&success);
            return heap_failure(db, out, finish, "finish");
        }
        if (catalog_set_locators(work, id, finished.new_root, primary_root) != CATALOG_OK) {
            heap_batch_abort(batch);
            catalog_close(work);
            result_release(&success);
            return fail_result(out, RESULT_E_INTERNAL, 3, "카탈로그 locator를 갱신하지 못했습니다");
        }
    }

    catalog_store_status_t saved = catalog_store_save(db->file, work);
    int saved_errno = errno;
    if (saved == CATALOG_STORE_OK || saved == CATALOG_STORE_UNRECLAIMED) {
        /* 게시됐다. 옛 페이지는 이제 놓아도 된다. 놓지 못한 페이지는 새는 공간일 뿐이다 */
        if (batch != NULL) heap_batch_release_old(batch);
        for (size_t i = 0; i < drop_count; i++) page_file_release(db->file, drop_pages[i]);
        catalog_close(db->catalog);
        db->catalog = work;
        *out = success;
        return DATABASE_EXEC_OK;
    }
    if (saved == CATALOG_STORE_OUTCOME_UNKNOWN) {
        /* 어느 쪽이 디스크에 남았는지 알 수 없다. 어느 페이지도 놓지 않고 핸들을 쓰지 않는다 */
        db->poisoned = true;
        catalog_close(work);
        result_release(&success);
        errno = saved_errno;
        return fail_result(out, RESULT_E_IO, (int)saved,
                           "커밋 결과를 알 수 없습니다(errno %d). 데이터베이스를 닫고 다시 열어 확인하세요", saved_errno);
    }
    /* 제어 쓰기를 시도하기 전의 실패라 게시된 상태는 그대로다 */
    if (batch != NULL) heap_batch_abort(batch);
    if (page_file_is_poisoned(db->file)) db->poisoned = true;
    catalog_close(work);
    result_release(&success);
    if (saved == CATALOG_STORE_NOMEM) return DATABASE_EXEC_NOMEM;
    if (saved == CATALOG_STORE_LIMIT) return fail_result(out, RESULT_E_LIMIT, (int)saved, "카탈로그가 저장 한도를 넘습니다");
    errno = saved_errno;
    return fail_result(out, RESULT_E_IO, (int)saved, "게시하지 못했습니다(상태 %d, errno %d). 기존 데이터는 그대로입니다",
                       (int)saved, saved_errno);
}

static result_status_t open_command(const char *tag, uint64_t affected, result_t **result)
{
    result_command_t command = {affected, tag, strlen(tag)};
    return result_open_command(&command, result);
}

/* ── CREATE · DROP ── */

static database_exec_status_t exec_create(database_t *db, const sqlstmt_create_t *create, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_t *work = NULL;
    result_t *success = NULL;
    catalog_column_spec_t *columns = malloc(create->column_count * sizeof(*columns));
    if (columns == NULL) return DATABASE_EXEC_NOMEM;
    for (size_t i = 0; i < create->column_count; i++) {
        const sqlstmt_column_def_t *def = &create->columns[i];
        columns[i] = (catalog_column_spec_t){(const char *)def->name.bytes, def->name.size, def->type,
                                             def->not_null};
    }
    catalog_table_spec_t spec = {(const char *)create->table.bytes, create->table.size, columns,
                                 create->column_count, create->primary_key_column, create->has_identity};
    catalog_status_t cloned = clone_catalog(db->catalog, &work);
    if (cloned != CATALOG_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    catalog_table_id_t id;
    catalog_status_t made = catalog_create_table(work, &spec, &id);
    if (made == CATALOG_EXISTS) {
        status = fail_result(out, RESULT_E_SEMANTIC, 4, "이미 있는 테이블입니다: %.*s",
                             name_len(create->table.size), (const char *)create->table.bytes);
        goto done;
    }
    if (made == CATALOG_NOMEM) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (made == CATALOG_LIMIT) {
        status = fail_result(out, RESULT_E_LIMIT, 5, "카탈로그 한도를 넘습니다");
        goto done;
    }
    if (made != CATALOG_OK) {
        status = fail_result(out, RESULT_E_SEMANTIC, 6, "테이블 정의가 올바르지 않습니다");
        goto done;
    }
    if (open_command("CREATE TABLE", 0, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, work, id, 0, NULL, NULL, 0, success, out);
    work = NULL;
    success = NULL;
done:
    catalog_close(work);
    result_release(&success);
    free(columns);
    return status;
}

static database_exec_status_t exec_drop(database_t *db, const sqlstmt_drop_t *drop, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &drop->table, &id, &view, out, &status)) return status;

    heap_t *heap = NULL;
    uint32_t *pages = NULL;
    size_t page_count = 0;
    catalog_t *work = NULL;
    result_t *success = NULL;
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) return heap_failure(db, out, opened, "open");
    heap_status_t collected = heap_collect_pages(heap, &pages, &page_count);
    if (collected != HEAP_OK) {
        status = heap_failure(db, out, collected, "collect");
        goto done;
    }
    if (clone_catalog(db->catalog, &work) != CATALOG_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (catalog_drop_table(work, id) != CATALOG_OK) {
        status = fail_result(out, RESULT_E_INTERNAL, 7, "테이블을 지우지 못했습니다");
        goto done;
    }
    if (open_command("DROP TABLE", 0, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, work, id, 0, NULL, pages, page_count, success, out);
    work = NULL;
    success = NULL;
done:
    catalog_close(work);
    result_release(&success);
    free(pages);
    heap_close(heap);
    return status;
}

/* ── INSERT ── */

static database_exec_status_t not_null_violation(result_t **out, const catalog_column_t *column)
{
    return fail_result(out, RESULT_E_CONSTRAINT, 8, "NOT NULL 제약을 어겼습니다: %.*s",
                       name_len(column->name_bytes), column->name);
}

static database_exec_status_t exec_insert(database_t *db, const sqlstmt_insert_t *insert, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &insert->table, &id, &view, out, &status)) return status;
    size_t n = view.column_count;

    value_t *cells = calloc(n, sizeof(*cells));
    size_t *source = malloc(n * sizeof(*source));  /* 컬럼 번호 -> 값 번호 */
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    catalog_t *work = NULL;
    result_t *success = NULL;
    heap_cursor_t *cursor = NULL;
    value_t *scanned = NULL;
    if (cells == NULL || source == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) source[i] = SIZE_MAX;
    if (insert->has_columns) {
        for (size_t i = 0; i < insert->column_count; i++) {
            size_t column = find_column(&view, &insert->columns[i]);
            if (column == SIZE_MAX) {
                status = unknown_column(out, &insert->columns[i]);
                goto done;
            }
            if (source[column] != SIZE_MAX) {
                status = fail_result(out, RESULT_E_SEMANTIC, 9, "컬럼이 두 번 나옵니다: %.*s",
                                     name_len(insert->columns[i].size), (const char *)insert->columns[i].bytes);
                goto done;
            }
            source[column] = i;
        }
    } else {
        if (insert->value_count != n) {
            status = fail_result(out, RESULT_E_SEMANTIC, 10, "값 개수(%zu)가 컬럼 수(%zu)와 다릅니다",
                                 insert->value_count, n);
            goto done;
        }
        for (size_t i = 0; i < n; i++) source[i] = i;
    }
    for (size_t i = 0; i < n; i++) {
        if (source[i] == SIZE_MAX) continue;
        if (!cast_for_column(&insert->values[source[i]], &view.columns[i], &cells[i], out, &status)) goto done;
    }

    if (clone_catalog(db->catalog, &work) != CATALOG_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, out, opened, "open");
        goto done;
    }

    size_t pk = view.primary_key_column;
    bool auto_id = pk != NO_COLUMN && view.has_identity && cells[pk].view.type == VALUE_NULL;
    bool check_pk = pk != NO_COLUMN && cells[pk].view.type != VALUE_NULL;
    if (auto_id || check_pk) {
        int64_t max_key = INT64_MIN;
        bool any = false;
        scanned = calloc(n, sizeof(*scanned));
        if (scanned == NULL) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        heap_status_t opened_cursor = heap_cursor_open(heap, &cursor);
        if (opened_cursor != HEAP_OK) {
            status = heap_failure(db, out, opened_cursor, "cursor");
            goto done;
        }
        for (;;) {
            heap_locator_t at;
            heap_status_t next = heap_cursor_next(cursor, scanned, &at);
            if (next == HEAP_DONE) break;
            if (next != HEAP_OK) {
                status = heap_failure(db, out, next, "scan");
                goto done;
            }
            if (check_pk && values_equal(&scanned[pk].view, &cells[pk].view)) {
                status = fail_result(out, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s",
                                     name_len(view.columns[pk].name_bytes), view.columns[pk].name);
                goto done;
            }
            if (auto_id && scanned[pk].view.type == VALUE_INTEGER) {
                if (!any || scanned[pk].view.integer > max_key) max_key = scanned[pk].view.integer;
                any = true;
            }
            clear_cells(scanned, n);
        }
        if (auto_id) {
            uint64_t next_value = 0;
            unsigned long skipped = 0;
            for (;;) {
                catalog_status_t reserved = catalog_reserve_identity(work, id, &next_value);
                if (reserved != CATALOG_OK) {
                    status = fail_result(out, RESULT_E_LIMIT, 12, "identity 값을 더 줄 수 없습니다");
                    goto done;
                }
                if (!any || (int64_t)next_value > max_key) break;
                if (++skipped > DATABASE_IDENTITY_SKIP_LIMIT) {
                    status = fail_result(out, RESULT_E_LIMIT, 13, "identity 카운터를 기존 최대 키 뒤로 옮기지 못했습니다");
                    goto done;
                }
            }
            if (value_set_integer(&cells[pk], (int64_t)next_value) != VALUE_OK) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (view.columns[i].not_null && cells[i].view.type == VALUE_NULL) {
            status = not_null_violation(out, &view.columns[i]);
            goto done;
        }
    }

    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began != HEAP_OK) {
        status = heap_failure(db, out, began, "batch");
        goto done;
    }
    value_view_t *views = malloc(n * sizeof(*views));
    if (views == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) views[i] = cells[i].view;
    heap_locator_t placed;
    heap_status_t inserted = heap_batch_insert(batch, views, n, &placed);
    free(views);
    if (inserted != HEAP_OK) {
        status = heap_failure(db, out, inserted, "insert");
        goto done;
    }
    if (catalog_set_row_count(work, id, view.row_count + 1) != CATALOG_OK) {
        status = fail_result(out, RESULT_E_LIMIT, 14, "행 수 한도를 넘습니다");
        goto done;
    }
    if (open_command("INSERT", 1, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, work, id, view.primary_root, batch, NULL, 0, success, out);
    work = NULL;
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) {
        heap_batch_abort(batch);
    }
    heap_batch_free(batch);
    heap_cursor_close(cursor);
    if (scanned != NULL) clear_cells(scanned, n);
    free(scanned);
    heap_close(heap);
    catalog_close(work);
    result_release(&success);
    if (cells != NULL) clear_cells(cells, n);
    free(cells);
    free(source);
    return status;
}

/* ── SELECT ── */

static database_exec_status_t exec_select(database_t *db, const sqlstmt_select_t *select, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &select->table, &id, &view, out, &status)) return status;
    size_t n = view.column_count;
    size_t selected = select->select_all ? n : select->column_count;

    size_t *picks = malloc((selected > 0 ? selected : 1) * sizeof(*picks));
    result_column_t *columns = malloc((selected > 0 ? selected : 1) * sizeof(*columns));
    value_view_t *cells = malloc((selected > 0 ? selected : 1) * sizeof(*cells));
    value_t *row = calloc(n, sizeof(*row));
    heap_t *heap = NULL;
    heap_cursor_t *cursor = NULL;
    result_builder_t *builder = NULL;
    where_t where;
    memset(&where, 0, sizeof(where));
    if (picks == NULL || columns == NULL || cells == NULL || row == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < selected; i++) {
        size_t column = select->select_all ? i : find_column(&view, &select->columns[i]);
        if (column == SIZE_MAX) {
            status = unknown_column(out, &select->columns[i]);
            goto done;
        }
        picks[i] = column;
        columns[i] = (result_column_t){(const uint8_t *)view.columns[column].name,
                                       view.columns[column].name_bytes, view.columns[column].type,
                                       !view.columns[column].not_null};
    }
    if (!compile_where(&view, &select->where, &where, out, &status)) goto done;

    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, out, opened, "open");
        goto done;
    }
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) {
        status = heap_failure(db, out, made, "cursor");
        goto done;
    }
    result_builder_spec_t spec = {columns, selected, NULL};
    result_status_t created = result_builder_create(&spec, &builder);
    if (created == RESULT_NOMEM) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (created != RESULT_OK) {
        status = fail_result(out, RESULT_E_LIMIT, 15, "결과 컬럼 한도를 넘습니다");
        goto done;
    }
    for (;;) {
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, out, next, "scan");
            goto done;
        }
        if (where_matches(&where, row)) {
            for (size_t i = 0; i < selected; i++) cells[i] = row[picks[i]].view;
            result_status_t added = result_builder_add_row(builder, cells, selected);
            if (added == RESULT_NOMEM) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            if (added != RESULT_OK) {
                status = fail_result(out, RESULT_E_LIMIT, 16, "결과가 한도를 넘습니다");
                goto done;
            }
        }
        clear_cells(row, n);
    }
    result_t *rows = NULL;
    result_status_t finished = result_builder_finish(&builder, &rows);
    if (finished == RESULT_NOMEM) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (finished != RESULT_OK) {
        status = fail_result(out, RESULT_E_LIMIT, 17, "결과를 만들지 못했습니다");
        goto done;
    }
    *out = rows;
done:
    result_builder_destroy(&builder);
    heap_cursor_close(cursor);
    heap_close(heap);
    if (row != NULL) clear_cells(row, n);
    free(row);
    where_clear(&where);
    free(picks);
    free(columns);
    free(cells);
    return status;
}

/* ── UPDATE · DELETE ── */

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

static void matches_free(matches_t *matches)
{
    for (size_t i = 0; i < matches->count; i++) {
        if (matches->items[i].cells != NULL) clear_cells(matches->items[i].cells, matches->columns);
        free(matches->items[i].cells);
    }
    free(matches->items);
    memset(matches, 0, sizeof(*matches));
}

/* 일치한 행(locator와 값)을 모은다. keep_values가 false면 값은 보관하지 않는다. */
static bool matches_add(matches_t *matches, heap_locator_t locator, const value_t *row, bool keep_values)
{
    if (matches->count == matches->cap) {
        size_t cap = matches->cap == 0 ? 64 : matches->cap * 2;
        match_t *grown = realloc(matches->items, cap * sizeof(*grown));
        if (grown == NULL) return false;
        matches->items = grown;
        matches->cap = cap;
    }
    match_t *slot = &matches->items[matches->count];
    slot->locator = locator;
    slot->cells = NULL;
    if (keep_values) {
        slot->cells = calloc(matches->columns, sizeof(*slot->cells));
        if (slot->cells == NULL) return false;
        for (size_t i = 0; i < matches->columns; i++) {
            if (value_clone(&row[i].view, &slot->cells[i]) != VALUE_OK) {
                clear_cells(slot->cells, matches->columns);
                free(slot->cells);
                slot->cells = NULL;
                return false;
            }
        }
    }
    matches->count++;
    return true;
}

typedef struct {
    const value_view_t *view;
} key_ref_t;

static int compare_keys(const void *a, const void *b)
{
    const key_ref_t *x = a, *y = b;
    int order = 0;
    value_compare(x->view, y->view, &order);
    return order;
}

typedef struct {
    size_t column;
    value_t value;
} assignment_c;

static database_exec_status_t exec_update(database_t *db, const sqlstmt_update_t *update, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &update->table, &id, &view, out, &status)) return status;
    size_t n = view.column_count;

    assignment_c *assigns = calloc(update->assignment_count > 0 ? update->assignment_count : 1, sizeof(*assigns));
    value_t *row = calloc(n, sizeof(*row));
    value_view_t *views = malloc(n * sizeof(*views));
    key_ref_t *keys = NULL;
    value_t *other_keys = NULL;
    size_t other_count = 0, key_count = 0;
    heap_t *heap = NULL;
    heap_cursor_t *cursor = NULL;
    heap_batch_t *batch = NULL;
    catalog_t *work = NULL;
    result_t *success = NULL;
    matches_t matches = {NULL, 0, 0, n};
    where_t where;
    memset(&where, 0, sizeof(where));
    if (assigns == NULL || row == NULL || views == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    size_t pk = view.primary_key_column;
    bool pk_assigned = false;
    for (size_t i = 0; i < update->assignment_count; i++) {
        size_t column = find_column(&view, &update->assignments[i].column);
        if (column == SIZE_MAX) {
            status = unknown_column(out, &update->assignments[i].column);
            goto done;
        }
        for (size_t j = 0; j < i; j++) {
            if (assigns[j].column == column) {
                status = fail_result(out, RESULT_E_SEMANTIC, 9, "컬럼이 두 번 나옵니다: %.*s",
                                     name_len(update->assignments[i].column.size),
                                     (const char *)update->assignments[i].column.bytes);
                goto done;
            }
        }
        assigns[i].column = column;
        if (!cast_for_column(&update->assignments[i].value, &view.columns[column], &assigns[i].value, out, &status)) {
            goto done;
        }
        if (view.columns[column].not_null && assigns[i].value.view.type == VALUE_NULL) {
            status = not_null_violation(out, &view.columns[column]);
            goto done;
        }
        if (column == pk) pk_assigned = true;
    }
    if (!compile_where(&view, &update->where, &where, out, &status)) goto done;

    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, out, opened, "open");
        goto done;
    }
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) {
        status = heap_failure(db, out, made, "cursor");
        goto done;
    }
    for (;;) {
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, out, next, "scan");
            goto done;
        }
        if (where_matches(&where, row)) {
            if (!matches_add(&matches, at, row, true)) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
        } else if (pk_assigned && row[pk].view.type != VALUE_NULL) {
            value_t *grown = realloc(other_keys, (other_count + 1) * sizeof(*grown));
            if (grown == NULL) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            other_keys = grown;
            memset(&other_keys[other_count], 0, sizeof(other_keys[other_count]));
            if (value_clone(&row[pk].view, &other_keys[other_count]) != VALUE_OK) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            other_count++;
        }
        clear_cells(row, n);
    }

    if (matches.count == 0) {
        if (open_command("UPDATE", 0, &success) != RESULT_OK) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        *out = success;
        success = NULL;
        goto done;
    }

    if (pk_assigned) {
        /* 갱신 뒤 기본 키가 유일한지: 일치하지 않은 행의 키 + 새 키를 정렬해 이웃을 비교한다 */
        keys = malloc((other_count + matches.count) * sizeof(*keys));
        if (keys == NULL) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        for (size_t i = 0; i < other_count; i++) keys[key_count++].view = &other_keys[i].view;
        for (size_t i = 0; i < update->assignment_count; i++) {
            if (assigns[i].column != pk || assigns[i].value.view.type == VALUE_NULL) continue;
            /* 같은 값을 둘 이상의 행에 쓰면 그 자체로 중복이다 */
            for (size_t m = 0; m < matches.count; m++) keys[key_count++].view = &assigns[i].value.view;
        }
        qsort(keys, key_count, sizeof(*keys), compare_keys);
        for (size_t i = 1; i < key_count; i++) {
            if (values_equal(keys[i - 1].view, keys[i].view)) {
                status = fail_result(out, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s",
                                     name_len(view.columns[pk].name_bytes), view.columns[pk].name);
                goto done;
            }
        }
    }

    if (clone_catalog(db->catalog, &work) != CATALOG_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began != HEAP_OK) {
        status = heap_failure(db, out, began, "batch");
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        for (size_t i = 0; i < n; i++) {
            views[i] = matches.items[m].cells[i].view;
        }
        for (size_t i = 0; i < update->assignment_count; i++) views[assigns[i].column] = assigns[i].value.view;
        heap_locator_t placed;
        heap_status_t changed = heap_batch_update(batch, matches.items[m].locator, views, n, &placed);
        if (changed != HEAP_OK) {
            status = heap_failure(db, out, changed, "update");
            goto done;
        }
    }
    if (open_command("UPDATE", matches.count, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, work, id, view.primary_root, batch, NULL, 0, success, out);
    work = NULL;
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_cursor_close(cursor);
    heap_close(heap);
    catalog_close(work);
    result_release(&success);
    matches_free(&matches);
    if (other_keys != NULL) clear_cells(other_keys, other_count);
    free(other_keys);
    free(keys);
    if (row != NULL) clear_cells(row, n);
    free(row);
    free(views);
    if (assigns != NULL) {
        for (size_t i = 0; i < update->assignment_count; i++) value_clear(&assigns[i].value);
    }
    free(assigns);
    where_clear(&where);
    return status;
}

static database_exec_status_t exec_delete(database_t *db, const sqlstmt_delete_t *del, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &del->table, &id, &view, out, &status)) return status;
    size_t n = view.column_count;

    value_t *row = calloc(n, sizeof(*row));
    heap_t *heap = NULL;
    heap_cursor_t *cursor = NULL;
    heap_batch_t *batch = NULL;
    catalog_t *work = NULL;
    result_t *success = NULL;
    matches_t matches = {NULL, 0, 0, n};
    where_t where;
    memset(&where, 0, sizeof(where));
    if (row == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (!compile_where(&view, &del->where, &where, out, &status)) goto done;
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, out, opened, "open");
        goto done;
    }
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) {
        status = heap_failure(db, out, made, "cursor");
        goto done;
    }
    for (;;) {
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, out, next, "scan");
            goto done;
        }
        if (where_matches(&where, row) && !matches_add(&matches, at, row, false)) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        clear_cells(row, n);
    }
    if (matches.count == 0) {
        if (open_command("DELETE", 0, &success) != RESULT_OK) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        *out = success;
        success = NULL;
        goto done;
    }
    if (clone_catalog(db->catalog, &work) != CATALOG_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began != HEAP_OK) {
        status = heap_failure(db, out, began, "batch");
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        heap_status_t removed = heap_batch_delete(batch, matches.items[m].locator);
        if (removed != HEAP_OK) {
            status = heap_failure(db, out, removed, "delete");
            goto done;
        }
    }
    if (catalog_set_row_count(work, id, view.row_count - matches.count) != CATALOG_OK) {
        status = fail_result(out, RESULT_E_INTERNAL, 18, "행 수를 갱신하지 못했습니다");
        goto done;
    }
    if (open_command("DELETE", matches.count, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, work, id, view.primary_root, batch, NULL, 0, success, out);
    work = NULL;
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_cursor_close(cursor);
    heap_close(heap);
    catalog_close(work);
    result_release(&success);
    matches_free(&matches);
    if (row != NULL) clear_cells(row, n);
    free(row);
    where_clear(&where);
    return status;
}

/* ── 실행 진입점 ── */

database_exec_status_t database_exec(database_t *db, const uint8_t *sql, size_t size,
                                     const lexer_binding_t *binding, result_t **out)
{
    if (db == NULL || out == NULL || (size > 0 && sql == NULL)) return DATABASE_EXEC_INVALID;
    *out = NULL;
    if (db->poisoned) {
        return fail_result(out, RESULT_E_IO, 19, "이 핸들은 불확정한 커밋 결과 또는 fsync 실패로 더 쓸 수 없습니다");
    }

    lexed_t *lexed = NULL;
    lexer_error_t lex_error;
    lexer_status_t scanned = lexer_scan(sql != NULL ? sql : (const uint8_t *)"", size, &LEXER_LIMITS,
                                        &lexed, &lex_error);
    if (scanned == LEXER_NOMEM) return DATABASE_EXEC_NOMEM;
    if (scanned == LEXER_LIMIT) return fail_result(out, RESULT_E_LIMIT, (int)lex_error.reason, "입력이 한도를 넘습니다");
    if (scanned != LEXER_OK) {
        return fail_result(out, RESULT_E_SYNTAX, (int)lex_error.reason, "어휘 오류 (바이트 %zu)", lex_error.offset);
    }

    database_exec_status_t status = DATABASE_EXEC_OK;
    sqlstmt_t *statement = NULL;
    if (lexed_statement_count(lexed) != 1) {
        status = fail_result(out, RESULT_E_SYNTAX, 20, "한 번에 문장 하나만 실행할 수 있습니다");
        goto done;
    }
    lexer_binding_t empty = {NULL, 0};
    const lexer_binding_t *bound = binding != NULL ? binding : &empty;
    lexer_status_t checked = lexer_check_binding(lexed, bound);
    if (checked != LEXER_OK) {
        status = fail_result(out, RESULT_E_SYNTAX, (int)checked, "매개변수 개수가 맞지 않습니다 (SQL %u개)",
                             (unsigned)lexed_parameter_count(lexed));
        goto done;
    }
    lexer_statement_t tokens;
    if (lexed_statement_at(lexed, 0, &tokens) != LEXER_OK) {
        status = fail_result(out, RESULT_E_INTERNAL, 21, "문장을 읽지 못했습니다");
        goto done;
    }
    sqlstmt_error_t parse_error;
    sqlstmt_status_t parsed = sqlstmt_parse(&tokens, bound, &statement, &parse_error);
    if (parsed == SQLSTMT_NOMEM) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (parsed != SQLSTMT_OK) {
        switch (parsed) {
            case SQLSTMT_SYNTAX:
                status = fail_result(out, RESULT_E_SYNTAX, (int)parse_error.reason, "문법 오류 (바이트 %zu)", parse_error.offset);
                break;
            case SQLSTMT_LIMIT:
                status = fail_result(out, RESULT_E_LIMIT, (int)parse_error.reason, "한도를 넘습니다 (바이트 %zu)", parse_error.offset);
                break;
            default:
                status = fail_result(out, RESULT_E_SEMANTIC, (int)parse_error.reason,
                                     "지원하지 않거나 올바르지 않은 문장입니다 (바이트 %zu)", parse_error.offset);
                break;
        }
        goto done;
    }
    if (statement->kind != SQLSTMT_SELECT && !db->writable) {
        status = fail_result(out, RESULT_E_SEMANTIC, 22, "읽기 전용으로 연 데이터베이스입니다");
        goto done;
    }
    switch (statement->kind) {
        case SQLSTMT_CREATE_TABLE: status = exec_create(db, &statement->create, out); break;
        case SQLSTMT_INSERT: status = exec_insert(db, &statement->insert, out); break;
        case SQLSTMT_SELECT: status = exec_select(db, &statement->select, out); break;
        case SQLSTMT_UPDATE: status = exec_update(db, &statement->update, out); break;
        case SQLSTMT_DELETE: status = exec_delete(db, &statement->del, out); break;
        case SQLSTMT_DROP_TABLE: status = exec_drop(db, &statement->drop, out); break;
    }
done:
    sqlstmt_free(statement);
    lexer_free(lexed);
    return status;
}
