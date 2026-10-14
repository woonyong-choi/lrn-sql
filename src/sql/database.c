#include <assert.h>
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
#include "storage/overflow.h"
#include "storage/page_file.h"

/* 문장 수준 오류. 정리를 모두 마친 뒤 진입점이 결과로 만든다(그 결과를 만들 메모리를 먼저 돌려주기 위해). */
#define EXEC_PENDING ((database_exec_status_t)100)

typedef struct {
    result_error_class_t error_class;
    int code;
    char message[256];
} pending_error_t;

struct database {
    page_file_t *file;
    catalog_t *catalog;   /* 게시된 카탈로그. 문장이 성공했을 때만 교체한다 */
    bool writable;
    bool poisoned;
    /* 상주 메모리 회계(attach된 경우) */
    mem_ledger_t resident;
    size_t res_fixed;
    size_t res_catalog;
    size_t res_lists;
    /* 실행 중 문맥. 핸들은 단일 스레드라 한 번에 한 문장만 쓴다 */
    mem_ledger_t ledger;
    const database_cancel_t *cancel;
    pending_error_t pending;
};

static const lexer_limits_t LEXER_LIMITS = {
    (size_t)16 << 20, (size_t)16 << 20, (size_t)1 << 20, LEXER_PARAMETER_CEILING
};

#define NO_COLUMN CATALOG_NO_COLUMN
/* value_cast가 숫자를 문자열로 바꿀 때(INTEGER 20바이트, REAL 32바이트 이하)와 소수점 변환의 여유 */
#define CAST_SLACK 64u

/* ── 계상 보조 ── */

static bool q_reserve(database_t *db, size_t bytes)
{
    return mem_reserve(&db->ledger, MEM_SCOPE_QUERY, bytes);
}

static void q_release(database_t *db, size_t bytes)
{
    mem_release(&db->ledger, MEM_SCOPE_QUERY, bytes);
}

static void *q_malloc(database_t *db, size_t bytes)
{
    if (!q_reserve(db, bytes)) return NULL;
    void *block = malloc(bytes);
    if (block == NULL) q_release(db, bytes);
    return block;
}

static void *q_calloc(database_t *db, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    if (!q_reserve(db, bytes)) return NULL;
    void *block = calloc(count, size);
    if (block == NULL) q_release(db, bytes);
    return block;
}

/* block이 NULL이 아니면 해제하고 reserve했던 bytes를 돌려준다. */
static void q_free(database_t *db, void *block, size_t bytes)
{
    if (block == NULL) return;
    free(block);
    q_release(db, bytes);
}

static bool cancel_requested(const database_t *db)
{
    return db->cancel != NULL && db->cancel->requested != NULL && db->cancel->requested(db->cancel->ctx);
}

static size_t value_bytes(const value_t *value)
{
    return value->view.type == VALUE_TEXT || value->view.type == VALUE_BLOB ? value->view.size_bytes : 0;
}

/* database.c가 계상해 가진 값들을 비우고 그 바이트를 돌려준다. */
static void owned_clear(database_t *db, value_t *cells, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        q_release(db, value_bytes(&cells[i]));
        value_clear(&cells[i]);
    }
}

static value_status_t q_cast(database_t *db, const value_view_t *source, value_type_t type, value_t *out)
{
    size_t reserved = source->size_bytes + CAST_SLACK;
    if (!q_reserve(db, reserved)) return VALUE_NOMEM;
    value_status_t status = value_cast(source, type, out);
    if (status != VALUE_OK) {
        q_release(db, reserved);
        return status;
    }
    size_t kept = value_bytes(out);
    assert(kept <= reserved);
    q_release(db, reserved - kept);
    return VALUE_OK;
}

static value_status_t q_clone(database_t *db, const value_view_t *source, value_t *out)
{
    if (!q_reserve(db, source->size_bytes)) return VALUE_NOMEM;
    value_status_t status = value_clone(source, out);
    if (status != VALUE_OK) q_release(db, source->size_bytes);
    return status;
}

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
    mem_ledger_t resident = db->resident;
    size_t resident_bytes = db->res_fixed + db->res_catalog + db->res_lists;
    catalog_close(db->catalog);
    page_file_status_t closed = page_file_close(db->file);
    free(db);
    mem_release(&resident, MEM_SCOPE_RESIDENT, resident_bytes);
    mem_ledger_drop(&resident);
    return closed == PAGE_FILE_OK ? DATABASE_OK : from_page_file(closed);
}

bool database_is_poisoned(const database_t *db) { return db != NULL && db->poisoned; }

size_t database_table_count(const database_t *db)
{
    return db != NULL ? catalog_table_count(db->catalog) : 0;
}

/* ── 상주 메모리 ── */

database_status_t database_attach_ledger(database_t *db, const mem_ledger_t *resident)
{
    if (db == NULL) return DATABASE_INVALID;
    if (!mem_ledger_enabled(resident)) return DATABASE_OK;
    if (mem_ledger_enabled(&db->resident)) return DATABASE_INVALID;
    size_t fixed = sizeof(*db) + page_file_handle_bytes(db->file);
    size_t catalog = catalog_memory_bytes(db->catalog);
    size_t lists = page_file_list_peak_bytes(page_file_control(db->file)->page_count);
    if (!mem_reserve(resident, MEM_SCOPE_RESIDENT, fixed + catalog + lists)) return DATABASE_NOMEM;
    mem_ledger_retain(resident);
    db->resident = *resident;
    db->res_fixed = fixed;
    db->res_catalog = catalog;
    db->res_lists = lists;
    return DATABASE_OK;
}

size_t database_resident_bytes(const database_t *db)
{
    return db == NULL ? 0 : db->res_fixed + db->res_catalog + db->res_lists;
}

typedef struct {
    size_t catalog_bytes;  /* 게시되면 상주할 카탈로그 몫 */
    size_t lists_bytes;    /* 게시 뒤 해제 목록 몫의 상한(줄이지 않는다) */
    size_t extra;          /* 이 계획이 새로 reserve한 몫 */
} resident_plan_t;

/*
 * 게시 전에 상주 몫이 늘어날 만큼을 reserve한다. 게시 뒤에는 실패할 수 없어야 하므로 미리 잡는다.
 * 새 카탈로그가 만들어진 뒤 옛 카탈로그가 사라지기 전까지 둘이 함께 있는 순간은 문장 몫(work)이 맡는다.
 * new_pages는 이 문장이 늘릴 수 있는 페이지 수의 상한이다.
 */
static bool resident_prepare(database_t *db, const catalog_t *work, size_t new_pages, resident_plan_t *plan)
{
    *plan = (resident_plan_t){db->res_catalog, db->res_lists, 0};
    if (!mem_ledger_enabled(&db->resident)) return true;
    size_t chain = catalog_encoded_bytes(work) / (PAGE_BODY_BYTES - OVERFLOW_MIN_PAGE_BYTES) + 1;
    uint64_t pages = (uint64_t)page_file_control(db->file)->page_count + new_pages + chain;
    if (pages > UINT32_MAX) pages = UINT32_MAX;
    plan->catalog_bytes = catalog_memory_bytes(work);
    plan->lists_bytes = page_file_list_peak_bytes((uint32_t)pages);
    if (plan->lists_bytes < db->res_lists) plan->lists_bytes = db->res_lists;
    size_t extra = 0;
    if (plan->catalog_bytes > db->res_catalog) extra += plan->catalog_bytes - db->res_catalog;
    extra += plan->lists_bytes - db->res_lists;
    if (!mem_reserve(&db->resident, MEM_SCOPE_RESIDENT, extra)) return false;
    plan->extra = extra;
    return true;
}

/*
 * 게시 뒤(adopted)에는 카탈로그 몫을 새 값으로 맞추고 줄었으면 돌려준다. 게시하지 못한 경우에도 해제 목록
 * 몫은 유지한다(실패한 시도가 페이지를 할당했을 수 있다). 카탈로그 몫으로 reserve했던 증가분은 돌려준다.
 */
static void resident_finish(database_t *db, const resident_plan_t *plan, bool adopted)
{
    if (!mem_ledger_enabled(&db->resident)) return;
    size_t catalog_extra = plan->catalog_bytes > db->res_catalog ? plan->catalog_bytes - db->res_catalog : 0;
    if (adopted) {
        if (plan->catalog_bytes < db->res_catalog) {
            mem_release(&db->resident, MEM_SCOPE_RESIDENT, db->res_catalog - plan->catalog_bytes);
        }
        db->res_catalog = plan->catalog_bytes;
    } else {
        mem_release(&db->resident, MEM_SCOPE_RESIDENT, catalog_extra);
    }
    db->res_lists = plan->lists_bytes;
}

/* ── 오류 결과 ── */

static database_exec_status_t fail_result(database_t *db, result_error_class_t error_class, int code,
                                          const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static database_exec_status_t fail_result(database_t *db, result_error_class_t error_class, int code,
                                          const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(db->pending.message, sizeof(db->pending.message), format, args);
    va_end(args);
    db->pending.error_class = error_class;
    db->pending.code = code;
    return EXEC_PENDING;
}

static int name_len(size_t size) { return (int)(size < 64 ? size : 64); }

static bool catalog_name_equals(const char *name, size_t size, const sqlstmt_name_t *other)
{
    return size == other->size && (size == 0 || memcmp(name, other->bytes, size) == 0);
}

/* 저장소 오류를 결과로 바꾼다. NOMEM은 결과가 아니라 NOMEM이다. */
static database_exec_status_t heap_failure(database_t *db, heap_status_t status, const char *where)
{
    if (page_file_is_poisoned(db->file)) db->poisoned = true;
    switch (status) {
        case HEAP_NOMEM: return DATABASE_EXEC_NOMEM;
        case HEAP_IO:
            return fail_result(db, RESULT_E_IO, (int)status, "저장소 I/O 오류 (%s, errno %d)", where, errno);
        case HEAP_LIMIT:
            return fail_result(db, RESULT_E_LIMIT, (int)status, "행이 저장 한도를 넘습니다 (%s)", where);
        default:
            return fail_result(db, RESULT_E_INTERNAL, (int)status,
                               "저장소가 손상됐거나 지원하지 않는 형식입니다 (%s, 상태 %d)", where, (int)status);
    }
}

static database_exec_status_t cancelled_result(database_t *db)
{
    return fail_result(db, RESULT_E_CANCELLED, DATABASE_CODE_CANCELLED, "문장이 취소되어 아무것도 반영하지 않았습니다");
}

/* ── 공통 보조 ── */

static void clear_cells(value_t *cells, size_t count)
{
    for (size_t i = 0; i < count; i++) value_clear(&cells[i]);
}

static bool lookup_table(database_t *db, const sqlstmt_name_t *name, catalog_table_id_t *id,
                         catalog_table_view_t *view, database_exec_status_t *status)
{
    catalog_status_t found = catalog_find(db->catalog, (const char *)name->bytes, name->size, id);
    if (found == CATALOG_OK && catalog_describe(db->catalog, *id, view) == CATALOG_OK) return true;
    *status = fail_result(db, RESULT_E_SEMANTIC, 1, "테이블이 없습니다: %.*s", name_len(name->size),
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

static database_exec_status_t unknown_column(database_t *db, const sqlstmt_name_t *name)
{
    return fail_result(db, RESULT_E_SEMANTIC, 2, "컬럼이 없습니다: %.*s", name_len(name->size),
                       (const char *)name->bytes);
}

static heap_status_t open_heap(database_t *db, const catalog_table_view_t *view,
                               catalog_table_id_t id, heap_t **heap)
{
    size_t bytes = view->column_count * sizeof(value_type_t);
    value_type_t *types = q_malloc(db, bytes);
    if (types == NULL) return HEAP_NOMEM;
    for (size_t i = 0; i < view->column_count; i++) types[i] = view->columns[i].type;
    heap_schema_t schema = {types, view->column_count};
    heap_status_t status = heap_open_ex(db->file, id, view->heap_root, &schema, &db->ledger, heap);
    q_free(db, types, bytes);
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

/* ── 문장 몫의 작업 카탈로그 ── */

typedef struct {
    catalog_t *cat;
    size_t charged;  /* 이 카탈로그에 대해 reserve해 둔 바이트 */
} work_t;

static void work_close(database_t *db, work_t *work)
{
    if (work->cat == NULL) return;
    catalog_close(work->cat);
    q_release(db, work->charged);
    *work = (work_t){NULL, 0};
}

/* 게시된 카탈로그를 복제한다. 복제 동안의 순간 최대치를 먼저 reserve하고 끝나면 복제본 몫만 남긴다. */
static bool work_clone(database_t *db, work_t *work)
{
    size_t peak = catalog_clone_peak_bytes(db->catalog);
    if (!q_reserve(db, peak)) return false;
    catalog_t *clone = NULL;
    if (clone_catalog(db->catalog, &clone) != CATALOG_OK) {
        q_release(db, peak);
        return false;
    }
    size_t kept = catalog_memory_bytes(clone);
    assert(kept <= peak);
    q_release(db, peak - kept);
    *work = (work_t){clone, kept};
    return true;
}

/* 호출 뒤 카탈로그 크기에 맞춰 reserve를 정리한다. peak는 호출 전에 reserve한 순간 최대치다. */
static void work_settle(database_t *db, work_t *work, size_t peak)
{
    size_t now = catalog_memory_bytes(work->cat);
    if (now >= work->charged) {
        size_t growth = now - work->charged;
        assert(growth <= peak);
        q_release(db, peak - growth);
    } else {
        q_release(db, peak + (work->charged - now));
    }
    work->charged = now;
}

/* ── WHERE ── */

typedef struct {
    bool present;
    size_t column;
    sqlstmt_op_t op;
    bool never;          /* NULL과의 비교는 항상 unknown이라 어떤 행도 고르지 않는다 */
    value_t literal;     /* 컬럼 타입으로 변환한 값 */
} where_t;

static void where_clear(database_t *db, where_t *where) { owned_clear(db, &where->literal, 1); }

/* 값을 컬럼 타입으로 정확히 변환한다. 실패하면 *status에 반환값을 둔다. */
static bool cast_for_column(database_t *db, const value_t *source, const catalog_column_t *column,
                            value_t *target, database_exec_status_t *status)
{
    value_status_t cast = q_cast(db, &source->view, column->type, target);
    if (cast == VALUE_OK) return true;
    if (cast == VALUE_NOMEM) *status = DATABASE_EXEC_NOMEM;
    else if (cast == VALUE_LIMIT) *status = fail_result(db, RESULT_E_LIMIT, (int)cast, "컬럼 %.*s의 값이 한도를 넘습니다",
                                                          name_len(column->name_bytes), column->name);
    else *status = fail_result(db, RESULT_E_SEMANTIC, (int)cast, "컬럼 %.*s에 저장할 수 없는 값입니다",
                               name_len(column->name_bytes), column->name);
    return false;
}

static bool values_equal(const value_view_t *a, const value_view_t *b)
{
    int order = 1;
    return a->type == b->type && value_compare(a, b, &order) == VALUE_OK && order == 0;
}

static bool compile_where(database_t *db, const catalog_table_view_t *view, const sqlstmt_where_t *source,
                          where_t *where, database_exec_status_t *status)
{
    memset(where, 0, sizeof(*where));
    if (!source->present) return true;
    where->present = true;
    where->op = source->op;
    where->column = find_column(view, &source->column);
    if (where->column == SIZE_MAX) {
        *status = unknown_column(db, &source->column);
        return false;
    }
    if (source->op == SQLSTMT_OP_IS_NULL || source->op == SQLSTMT_OP_IS_NOT_NULL) return true;
    if (source->value.view.type == VALUE_NULL) {
        where->never = true;
        return true;
    }
    return cast_for_column(db, &source->value, &view->columns[where->column], &where->literal, status);
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

/* overflow_write·overflow_free가 쓰는 임시 메모리의 상한(overflow.c의 쪽 수 계산과 헤더 크기 하한으로 잡는다) */
static size_t overflow_pages_bound(size_t size_bytes)
{
    return size_bytes / (PAGE_BODY_BYTES - OVERFLOW_MIN_PAGE_BYTES) + 1;
}

static size_t overflow_write_bound(size_t size_bytes)
{
    return size_bytes == 0 ? 0 : 2 * overflow_pages_bound(size_bytes) * sizeof(uint32_t) + PAGE_BODY_BYTES;
}

static size_t overflow_free_bound(size_t size_bytes)
{
    return size_bytes == 0 ? 0 : overflow_pages_bound(size_bytes) * sizeof(uint32_t) + PAGE_BODY_BYTES;
}

/*
 * 게시 전에 실패했다고 확인된 경우의 정리. 새로 할당한 페이지를 놓고(abort) 문장 몫을 모두 돌려준다.
 */
static void publish_discard(database_t *db, work_t *work, heap_batch_t *batch, result_t **success)
{
    if (batch != NULL) heap_batch_abort(batch);
    work_close(db, work);
    result_release(success);
}

/*
 * 변경을 게시한다. work·success·batch는 이 함수가 소비한다: 성공하면 work가 새 카탈로그가 되고
 * success가 *out이 되며, 어느 경우에도 batch는 메모리를 정확히 한 번 해제한다. 디스크 페이지는
 * 결과에 따라 다르다. 게시 성공이면 옛 페이지를 release하고, 제어 쓰기 전의 실패면 새로 할당한
 * 페이지를 release(abort)하며, 불확정이면 어느 페이지도 놓지 않는다(메모리만 해제). batch가 NULL이면
 * 페이지 변경이 없는 문장이다. 호출자는 이 함수를 부른 뒤 batch를 쓰거나 해제하지 않는다.
 * heap은 호출자가 닫는다. 취소는 이 함수의 첫머리에서만 확인한다. 게시를 시작한 뒤에는 보지 않는다.
 */
static database_exec_status_t publish_inner(database_t *db, work_t *work, catalog_table_id_t id,
                                            uint32_t primary_root, heap_batch_t *batch,
                                            const uint32_t *drop_pages, size_t drop_count,
                                            result_t *success, result_t **out)
{
    if (cancel_requested(db)) {
        publish_discard(db, work, batch, &success);
        return cancelled_result(db);
    }
    size_t new_pages = 0;
    if (batch != NULL) {
        heap_batch_result_t finished;
        heap_status_t finish = heap_batch_finish(batch, &finished);
        if (finish != HEAP_OK) {
            publish_discard(db, work, batch, &success);
            return heap_failure(db, finish, "finish");
        }
        new_pages = finished.new_count;
        if (catalog_set_locators(work->cat, id, finished.new_root, primary_root) != CATALOG_OK) {
            publish_discard(db, work, batch, &success);
            return fail_result(db, RESULT_E_INTERNAL, 3, "카탈로그 locator를 갱신하지 못했습니다");
        }
    }

    resident_plan_t plan;
    if (!resident_prepare(db, work->cat, new_pages, &plan)) {
        publish_discard(db, work, batch, &success);
        return DATABASE_EXEC_NOMEM;
    }
    /* catalog_store_save의 부호화 buffer와 overflow 임시 메모리의 상한. 옛 체인을 놓을 때의 임시도 포함한다 */
    size_t encoded = catalog_encoded_bytes(work->cat);
    size_t write_peak = encoded + overflow_write_bound(encoded);
    size_t free_peak = overflow_free_bound(catalog_encoded_bytes(db->catalog));
    size_t save_peak = write_peak > free_peak ? write_peak : free_peak;
    if (!q_reserve(db, save_peak)) {
        resident_finish(db, &plan, false);
        publish_discard(db, work, batch, &success);
        return DATABASE_EXEC_NOMEM;
    }

    catalog_store_status_t saved = catalog_store_save(db->file, work->cat);
    int saved_errno = errno;
    q_release(db, save_peak);
    if (saved == CATALOG_STORE_OK || saved == CATALOG_STORE_UNRECLAIMED) {
        /* 게시됐다. 옛 페이지는 이제 놓아도 된다. 놓지 못한 페이지는 새는 공간일 뿐이다 */
        if (batch != NULL) heap_batch_release_old(batch);
        for (size_t i = 0; i < drop_count; i++) page_file_release(db->file, drop_pages[i]);
        catalog_close(db->catalog);
        db->catalog = work->cat;
        q_release(db, work->charged);  /* 이 카탈로그는 이제 문장 몫이 아니라 상주 몫이다 */
        *work = (work_t){NULL, 0};
        resident_finish(db, &plan, true);
        *out = success;
        return DATABASE_EXEC_OK;
    }
    if (saved == CATALOG_STORE_OUTCOME_UNKNOWN) {
        /* 어느 쪽이 디스크에 남았는지 알 수 없다. 어느 페이지도 놓지 않고 핸들을 쓰지 않는다 */
        db->poisoned = true;
        resident_finish(db, &plan, false);
        work_close(db, work);
        result_release(&success);
        errno = saved_errno;
        return fail_result(db, RESULT_E_IO, (int)saved,
                           "커밋 결과를 알 수 없습니다(errno %d). 데이터베이스를 닫고 다시 열어 확인하세요", saved_errno);
    }
    /* 제어 쓰기를 시도하기 전의 실패라 게시된 상태는 그대로다 */
    resident_finish(db, &plan, false);
    if (page_file_is_poisoned(db->file)) db->poisoned = true;
    publish_discard(db, work, batch, &success);
    if (saved == CATALOG_STORE_NOMEM) return DATABASE_EXEC_NOMEM;
    if (saved == CATALOG_STORE_LIMIT) return fail_result(db, RESULT_E_LIMIT, (int)saved, "카탈로그가 저장 한도를 넘습니다");
    errno = saved_errno;
    return fail_result(db, RESULT_E_IO, (int)saved, "게시하지 못했습니다(상태 %d, errno %d). 기존 데이터는 그대로입니다",
                       (int)saved, saved_errno);
}

static database_exec_status_t publish(database_t *db, work_t *work, catalog_table_id_t id,
                                      uint32_t primary_root, heap_batch_t *batch,
                                      const uint32_t *drop_pages, size_t drop_count,
                                      result_t *success, result_t **out)
{
    database_exec_status_t status = publish_inner(db, work, id, primary_root, batch, drop_pages,
                                                  drop_count, success, out);
    heap_batch_free(batch);
    return status;
}

static result_status_t open_command(database_t *db, const char *tag, uint64_t affected, result_t **result)
{
    result_command_t command = {affected, tag, strlen(tag)};
    return result_open_command_ex(&command, &db->ledger, result);
}

/* ── CREATE · DROP ── */

static database_exec_status_t exec_create(database_t *db, const sqlstmt_create_t *create, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    size_t columns_bytes = create->column_count * sizeof(catalog_column_spec_t);
    catalog_column_spec_t *columns = q_malloc(db, columns_bytes);
    if (columns == NULL) return DATABASE_EXEC_NOMEM;
    size_t names_bytes = create->table.size;
    for (size_t i = 0; i < create->column_count; i++) {
        const sqlstmt_column_def_t *def = &create->columns[i];
        columns[i] = (catalog_column_spec_t){(const char *)def->name.bytes, def->name.size, def->type,
                                             def->not_null};
        names_bytes += def->name.size;
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
    status = publish(db, &work, id, 0, NULL, NULL, 0, success, out);
    success = NULL;
done:
    work_close(db, &work);
    result_release(&success);
    q_free(db, columns, columns_bytes);
    return status;
}

static database_exec_status_t exec_drop(database_t *db, const sqlstmt_drop_t *drop, result_t **out)
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
    heap_status_t collected = heap_collect_pages(heap, &pages, &page_count);
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
    status = publish(db, &work, id, 0, NULL, pages, page_count, success, out);
    success = NULL;
done:
    work_close(db, &work);
    result_release(&success);
    heap_free_pages(heap, pages, page_count);
    heap_close(heap);
    return status;
}

/* ── INSERT ── */

static database_exec_status_t not_null_violation(database_t *db, const catalog_column_t *column)
{
    return fail_result(db, RESULT_E_CONSTRAINT, 8, "NOT NULL 제약을 어겼습니다: %.*s",
                       name_len(column->name_bytes), column->name);
}

static database_exec_status_t exec_insert(database_t *db, const sqlstmt_insert_t *insert, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &insert->table, &id, &view, &status)) return status;
    size_t n = view.column_count;

    size_t cells_bytes = n * sizeof(value_t), source_bytes = n * sizeof(size_t);
    size_t scanned_bytes = n * sizeof(value_t), views_bytes = n * sizeof(value_view_t);
    value_t *cells = q_calloc(db, n, sizeof(*cells));
    size_t *source = q_malloc(db, source_bytes);  /* 컬럼 번호 -> 값 번호 */
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    heap_cursor_t *cursor = NULL;
    value_t *scanned = NULL;
    value_view_t *views = NULL;
    if (cells == NULL || source == NULL) {
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
    for (size_t i = 0; i < n; i++) {
        if (source[i] == SIZE_MAX) continue;
        if (!cast_for_column(db, &insert->values[source[i]], &view.columns[i], &cells[i], &status)) goto done;
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

    size_t pk = view.primary_key_column;
    bool auto_id = pk != NO_COLUMN && view.has_identity && cells[pk].view.type == VALUE_NULL;
    bool check_pk = pk != NO_COLUMN && cells[pk].view.type != VALUE_NULL;
    if (auto_id || check_pk) {
        int64_t max_key = INT64_MIN;
        bool any = false;
        scanned = q_calloc(db, n, sizeof(*scanned));
        if (scanned == NULL) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        heap_status_t opened_cursor = heap_cursor_open(heap, &cursor);
        if (opened_cursor != HEAP_OK) {
            status = heap_failure(db, opened_cursor, "cursor");
            goto done;
        }
        for (;;) {
            if (cancel_requested(db)) {
                status = cancelled_result(db);
                goto done;
            }
            heap_locator_t at;
            heap_status_t next = heap_cursor_next(cursor, scanned, &at);
            if (next == HEAP_DONE) break;
            if (next != HEAP_OK) {
                status = heap_failure(db, next, "scan");
                goto done;
            }
            if (check_pk && values_equal(&scanned[pk].view, &cells[pk].view)) {
                status = fail_result(db, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s",
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
    }
    for (size_t i = 0; i < n; i++) {
        if (view.columns[i].not_null && cells[i].view.type == VALUE_NULL) {
            status = not_null_violation(db, &view.columns[i]);
            goto done;
        }
    }

    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began != HEAP_OK) {
        status = heap_failure(db, began, "batch");
        goto done;
    }
    views = q_malloc(db, views_bytes);
    if (views == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) views[i] = cells[i].view;
    heap_locator_t placed;
    heap_status_t inserted = heap_batch_insert(batch, views, n, &placed);
    if (inserted != HEAP_OK) {
        status = heap_failure(db, inserted, "insert");
        goto done;
    }
    if (catalog_set_row_count(work.cat, id, view.row_count + 1) != CATALOG_OK) {
        status = fail_result(db, RESULT_E_LIMIT, 14, "행 수 한도를 넘습니다");
        goto done;
    }
    if (open_command(db, "INSERT", 1, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    q_free(db, views, views_bytes);
    views = NULL;
    status = publish(db, &work, id, view.primary_root, batch, NULL, 0, success, out);
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) {
        heap_batch_abort(batch);
    }
    heap_batch_free(batch);
    heap_cursor_close(cursor);
    if (scanned != NULL) clear_cells(scanned, n);
    q_free(db, scanned, scanned_bytes);
    heap_close(heap);
    work_close(db, &work);
    result_release(&success);
    q_free(db, views, views_bytes);
    if (cells != NULL) owned_clear(db, cells, n);
    q_free(db, cells, cells_bytes);
    q_free(db, source, source_bytes);
    return status;
}

/* ── SELECT ── */

static database_exec_status_t exec_select(database_t *db, const sqlstmt_select_t *select, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &select->table, &id, &view, &status)) return status;
    size_t n = view.column_count;
    size_t selected = select->select_all ? n : select->column_count;
    size_t slots = selected > 0 ? selected : 1;

    size_t picks_bytes = slots * sizeof(size_t), columns_bytes = slots * sizeof(result_column_t);
    size_t cells_bytes = slots * sizeof(value_view_t), row_bytes = n * sizeof(value_t);
    size_t *picks = q_malloc(db, picks_bytes);
    result_column_t *columns = q_malloc(db, columns_bytes);
    value_view_t *cells = q_malloc(db, cells_bytes);
    value_t *row = q_calloc(db, n, sizeof(*row));
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
            status = unknown_column(db, &select->columns[i]);
            goto done;
        }
        picks[i] = column;
        columns[i] = (result_column_t){(const uint8_t *)view.columns[column].name,
                                       view.columns[column].name_bytes, view.columns[column].type,
                                       !view.columns[column].not_null};
    }
    if (!compile_where(db, &view, &select->where, &where, &status)) goto done;

    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, opened, "open");
        goto done;
    }
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) {
        status = heap_failure(db, made, "cursor");
        goto done;
    }
    result_builder_spec_t spec = {columns, selected, NULL};
    result_status_t created = result_builder_create_ex(&spec, &db->ledger, &builder);
    if (created == RESULT_NOMEM) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (created != RESULT_OK) {
        status = fail_result(db, RESULT_E_LIMIT, 15, "결과 컬럼 한도를 넘습니다");
        goto done;
    }
    for (;;) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, next, "scan");
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
                status = fail_result(db, RESULT_E_LIMIT, 16, "결과가 한도를 넘습니다");
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
        status = fail_result(db, RESULT_E_LIMIT, 17, "결과를 만들지 못했습니다");
        goto done;
    }
    *out = rows;
done:
    result_builder_destroy(&builder);
    heap_cursor_close(cursor);
    heap_close(heap);
    if (row != NULL) clear_cells(row, n);
    q_free(db, row, row_bytes);
    where_clear(db, &where);
    q_free(db, picks, picks_bytes);
    q_free(db, columns, columns_bytes);
    q_free(db, cells, cells_bytes);
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

typedef struct {
    const value_view_t *view;
} key_ref_t;

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

typedef struct {
    size_t column;
    value_t value;
} assignment_c;

static database_exec_status_t exec_update(database_t *db, const sqlstmt_update_t *update, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &update->table, &id, &view, &status)) return status;
    size_t n = view.column_count;

    size_t assigns_count = update->assignment_count > 0 ? update->assignment_count : 1;
    size_t assigns_bytes = assigns_count * sizeof(assignment_c);
    size_t row_bytes = n * sizeof(value_t), views_bytes = n * sizeof(value_view_t);
    assignment_c *assigns = q_calloc(db, assigns_count, sizeof(*assigns));
    value_t *row = q_calloc(db, n, sizeof(*row));
    value_view_t *views = q_malloc(db, views_bytes);
    key_ref_t *keys = NULL;
    size_t keys_bytes = 0;
    value_t *other_keys = NULL;
    size_t other_cap = 0;
    size_t other_count = 0, key_count = 0;
    heap_t *heap = NULL;
    heap_cursor_t *cursor = NULL;
    heap_batch_t *batch = NULL;
    work_t work = {NULL, 0};
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
            status = unknown_column(db, &update->assignments[i].column);
            goto done;
        }
        for (size_t j = 0; j < i; j++) {
            if (assigns[j].column == column) {
                status = fail_result(db, RESULT_E_SEMANTIC, 9, "컬럼이 두 번 나옵니다: %.*s",
                                     name_len(update->assignments[i].column.size),
                                     (const char *)update->assignments[i].column.bytes);
                goto done;
            }
        }
        assigns[i].column = column;
        if (!cast_for_column(db, &update->assignments[i].value, &view.columns[column], &assigns[i].value, &status)) {
            goto done;
        }
        if (view.columns[column].not_null && assigns[i].value.view.type == VALUE_NULL) {
            status = not_null_violation(db, &view.columns[column]);
            goto done;
        }
        if (column == pk) pk_assigned = true;
    }
    if (!compile_where(db, &view, &update->where, &where, &status)) goto done;

    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, opened, "open");
        goto done;
    }
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) {
        status = heap_failure(db, made, "cursor");
        goto done;
    }
    for (;;) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, next, "scan");
            goto done;
        }
        if (where_matches(&where, row)) {
            if (!matches_add(db, &matches, at, row, true)) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
        } else if (pk_assigned && row[pk].view.type != VALUE_NULL) {
            /* 배열을 하나씩 늘린다. 새 크기를 먼저 reserve하고 복사 뒤 옛 크기를 돌려준다 */
            size_t grown_cap = other_count + 1;
            if (grown_cap > SIZE_MAX / sizeof(value_t) || !q_reserve(db, grown_cap * sizeof(value_t))) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            value_t *grown = realloc(other_keys, grown_cap * sizeof(*grown));
            if (grown == NULL) {
                q_release(db, grown_cap * sizeof(value_t));
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            q_release(db, other_cap * sizeof(value_t));
            other_keys = grown;
            other_cap = grown_cap;
            memset(&other_keys[other_count], 0, sizeof(other_keys[other_count]));
            if (q_clone(db, &row[pk].view, &other_keys[other_count]) != VALUE_OK) {
                status = DATABASE_EXEC_NOMEM;
                goto done;
            }
            other_count++;
        }
        clear_cells(row, n);
    }

    if (matches.count == 0) {
        if (open_command(db, "UPDATE", 0, &success) != RESULT_OK) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        *out = success;
        success = NULL;
        goto done;
    }

    if (pk_assigned) {
        /* 갱신 뒤 기본 키가 유일한지: 일치하지 않은 행의 키 + 새 키를 정렬해 이웃을 비교한다 */
        if (other_count + matches.count > SIZE_MAX / sizeof(*keys)) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        keys_bytes = (other_count + matches.count) * sizeof(*keys);
        keys = q_malloc(db, keys_bytes);
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
        sort_keys(keys, key_count);
        for (size_t i = 1; i < key_count; i++) {
            if (values_equal(keys[i - 1].view, keys[i].view)) {
                status = fail_result(db, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s",
                                     name_len(view.columns[pk].name_bytes), view.columns[pk].name);
                goto done;
            }
        }
    }

    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began != HEAP_OK) {
        status = heap_failure(db, began, "batch");
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        for (size_t i = 0; i < n; i++) {
            views[i] = matches.items[m].cells[i].view;
        }
        for (size_t i = 0; i < update->assignment_count; i++) views[assigns[i].column] = assigns[i].value.view;
        heap_locator_t placed;
        heap_status_t changed = heap_batch_update(batch, matches.items[m].locator, views, n, &placed);
        if (changed != HEAP_OK) {
            status = heap_failure(db, changed, "update");
            goto done;
        }
    }
    if (open_command(db, "UPDATE", matches.count, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, &work, id, view.primary_root, batch, NULL, 0, success, out);
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_cursor_close(cursor);
    heap_close(heap);
    work_close(db, &work);
    result_release(&success);
    matches_free(db, &matches);
    if (other_keys != NULL) owned_clear(db, other_keys, other_count);
    q_free(db, other_keys, other_cap * sizeof(value_t));
    q_free(db, keys, keys_bytes);
    if (row != NULL) clear_cells(row, n);
    q_free(db, row, row_bytes);
    q_free(db, views, views_bytes);
    if (assigns != NULL) {
        for (size_t i = 0; i < update->assignment_count; i++) owned_clear(db, &assigns[i].value, 1);
    }
    q_free(db, assigns, assigns_bytes);
    where_clear(db, &where);
    return status;
}

static database_exec_status_t exec_delete(database_t *db, const sqlstmt_delete_t *del, result_t **out)
{
    database_exec_status_t status = DATABASE_EXEC_OK;
    catalog_table_id_t id;
    catalog_table_view_t view;
    if (!lookup_table(db, &del->table, &id, &view, &status)) return status;
    size_t n = view.column_count;

    size_t row_bytes = n * sizeof(value_t);
    value_t *row = q_calloc(db, n, sizeof(*row));
    heap_t *heap = NULL;
    heap_cursor_t *cursor = NULL;
    heap_batch_t *batch = NULL;
    work_t work = {NULL, 0};
    result_t *success = NULL;
    matches_t matches = {NULL, 0, 0, n};
    where_t where;
    memset(&where, 0, sizeof(where));
    if (row == NULL) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    if (!compile_where(db, &view, &del->where, &where, &status)) goto done;
    heap_status_t opened = open_heap(db, &view, id, &heap);
    if (opened != HEAP_OK) {
        status = heap_failure(db, opened, "open");
        goto done;
    }
    heap_status_t made = heap_cursor_open(heap, &cursor);
    if (made != HEAP_OK) {
        status = heap_failure(db, made, "cursor");
        goto done;
    }
    for (;;) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        heap_locator_t at;
        heap_status_t next = heap_cursor_next(cursor, row, &at);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = heap_failure(db, next, "scan");
            goto done;
        }
        if (where_matches(&where, row) && !matches_add(db, &matches, at, row, false)) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        clear_cells(row, n);
    }
    if (matches.count == 0) {
        if (open_command(db, "DELETE", 0, &success) != RESULT_OK) {
            status = DATABASE_EXEC_NOMEM;
            goto done;
        }
        *out = success;
        success = NULL;
        goto done;
    }
    if (!work_clone(db, &work)) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    heap_status_t began = heap_batch_begin(heap, &batch);
    if (began != HEAP_OK) {
        status = heap_failure(db, began, "batch");
        goto done;
    }
    for (size_t m = 0; m < matches.count; m++) {
        if (cancel_requested(db)) {
            status = cancelled_result(db);
            goto done;
        }
        heap_status_t removed = heap_batch_delete(batch, matches.items[m].locator);
        if (removed != HEAP_OK) {
            status = heap_failure(db, removed, "delete");
            goto done;
        }
    }
    if (catalog_set_row_count(work.cat, id, view.row_count - matches.count) != CATALOG_OK) {
        status = fail_result(db, RESULT_E_INTERNAL, 18, "행 수를 갱신하지 못했습니다");
        goto done;
    }
    if (open_command(db, "DELETE", matches.count, &success) != RESULT_OK) {
        status = DATABASE_EXEC_NOMEM;
        goto done;
    }
    status = publish(db, &work, id, view.primary_root, batch, NULL, 0, success, out);
    success = NULL;
    batch = NULL;
done:
    if (batch != NULL) heap_batch_abort(batch);
    heap_batch_free(batch);
    heap_cursor_close(cursor);
    heap_close(heap);
    work_close(db, &work);
    result_release(&success);
    matches_free(db, &matches);
    if (row != NULL) clear_cells(row, n);
    q_free(db, row, row_bytes);
    where_clear(db, &where);
    return status;
}

/* ── 실행 진입점 ── */

/* 정리를 마친 뒤 문장 수준 오류와 예산 거절을 결과로 만든다. 결과도 만들 수 없으면 NOMEM이다. */
static database_exec_status_t materialize(database_t *db, database_exec_status_t status, result_t **out)
{
    result_error_t error;
    if (status == EXEC_PENDING) {
        error = (result_error_t){db->pending.error_class, db->pending.code, db->pending.message,
                                 strlen(db->pending.message), false};
    } else if (status == DATABASE_EXEC_NOMEM && mem_ledger_enabled(&db->ledger)) {
        error = (result_error_t){RESULT_E_NOMEM, DATABASE_CODE_BUDGET, "메모리 예산을 넘어 문장을 실행하지 못했습니다. 아무것도 반영하지 않았습니다",
                                 0, false};
        error.message_len = strlen(error.message);
    } else {
        return status;
    }
    result_t *result = NULL;
    result_status_t opened = result_open_error_ex(&error, &db->ledger, &result);
    if (opened == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    if (opened != RESULT_OK) return DATABASE_EXEC_INVALID;
    *out = result;
    return DATABASE_EXEC_OK;
}

static database_exec_status_t exec_statement(database_t *db, const database_exec_options_t *opts,
                                             const uint8_t *sql, size_t size,
                                             const lexer_binding_t *binding, result_t **out)
{
    if (db->poisoned) {
        return fail_result(db, RESULT_E_IO, 19, "이 핸들은 불확정한 커밋 결과 또는 fsync 실패로 더 쓸 수 없습니다");
    }
    if (cancel_requested(db)) return cancelled_result(db);

    bool bounded = mem_ledger_enabled(&db->ledger);
    lexer_limits_t limits = LEXER_LIMITS;
    if (bounded) {
        size_t max_sql = opts != NULL && opts->max_sql_bytes > 0 ? opts->max_sql_bytes : DATABASE_BOUNDED_SQL_BYTES;
        if (max_sql < limits.max_input_bytes) limits.max_input_bytes = max_sql;
    }
    if (size > limits.max_input_bytes) return fail_result(db, RESULT_E_LIMIT, 25, "입력이 한도를 넘습니다");

    const uint8_t *text = sql != NULL ? sql : (const uint8_t *)"";
    size_t lex_peak = lexer_scan_peak_bytes(size, &limits);
    if (!q_reserve(db, lex_peak)) return DATABASE_EXEC_NOMEM;
    lexed_t *lexed = NULL;
    lexer_error_t lex_error;
    lexer_status_t scanned = lexer_scan(text, size, &limits, &lexed, &lex_error);
    if (scanned == LEXER_NOMEM) {
        q_release(db, lex_peak);
        return DATABASE_EXEC_NOMEM;
    }
    if (scanned == LEXER_LIMIT) {
        q_release(db, lex_peak);
        return fail_result(db, RESULT_E_LIMIT, (int)lex_error.reason, "입력이 한도를 넘습니다");
    }
    if (scanned != LEXER_OK) {
        q_release(db, lex_peak);
        return fail_result(db, RESULT_E_SYNTAX, (int)lex_error.reason, "어휘 오류 (바이트 %zu)", lex_error.offset);
    }

    database_exec_status_t status = DATABASE_EXEC_OK;
    sqlstmt_t *statement = NULL;
    size_t parse_peak = 0;
    if (lexed_statement_count(lexed) != 1) {
        status = fail_result(db, RESULT_E_SYNTAX, 20, "한 번에 문장 하나만 실행할 수 있습니다");
        goto done;
    }
    lexer_binding_t empty = {NULL, 0};
    const lexer_binding_t *bound = binding != NULL ? binding : &empty;
    lexer_status_t checked = lexer_check_binding(lexed, bound);
    if (checked != LEXER_OK) {
        status = fail_result(db, RESULT_E_SYNTAX, (int)checked, "매개변수 개수가 맞지 않습니다 (SQL %u개)",
                             (unsigned)lexed_parameter_count(lexed));
        goto done;
    }
    lexer_statement_t tokens;
    if (lexed_statement_at(lexed, 0, &tokens) != LEXER_OK) {
        status = fail_result(db, RESULT_E_INTERNAL, 21, "문장을 읽지 못했습니다");
        goto done;
    }
    parse_peak = sqlstmt_parse_peak_bytes(&tokens, bound);
    if (!q_reserve(db, parse_peak)) {
        parse_peak = 0;
        status = DATABASE_EXEC_NOMEM;
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
                status = fail_result(db, RESULT_E_SYNTAX, (int)parse_error.reason, "문법 오류 (바이트 %zu)", parse_error.offset);
                break;
            case SQLSTMT_LIMIT:
                status = fail_result(db, RESULT_E_LIMIT, (int)parse_error.reason, "한도를 넘습니다 (바이트 %zu)", parse_error.offset);
                break;
            default:
                status = fail_result(db, RESULT_E_SEMANTIC, (int)parse_error.reason,
                                     "지원하지 않거나 올바르지 않은 문장입니다 (바이트 %zu)", parse_error.offset);
                break;
        }
        goto done;
    }
    if (statement->kind != SQLSTMT_SELECT && !db->writable) {
        status = fail_result(db, RESULT_E_SEMANTIC, 22, "읽기 전용으로 연 데이터베이스입니다");
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
    q_release(db, parse_peak);
    lexer_free(lexed);
    q_release(db, lex_peak);
    return status;
}

database_exec_status_t database_exec_ex(database_t *db, const database_exec_options_t *opts,
                                        const uint8_t *sql, size_t size,
                                        const lexer_binding_t *binding, result_t **out)
{
    if (db == NULL || out == NULL || (size > 0 && sql == NULL)) return DATABASE_EXEC_INVALID;
    *out = NULL;
    db->ledger = opts != NULL && opts->ledger != NULL ? *opts->ledger : (mem_ledger_t){NULL, NULL};
    db->cancel = opts != NULL ? opts->cancel : NULL;
    database_exec_status_t status = exec_statement(db, opts, sql, size, binding, out);
    if (status == EXEC_PENDING || status == DATABASE_EXEC_NOMEM) status = materialize(db, status, out);
    db->ledger = (mem_ledger_t){NULL, NULL};
    db->cancel = NULL;
    return status;
}

database_exec_status_t database_exec(database_t *db, const uint8_t *sql, size_t size,
                                     const lexer_binding_t *binding, result_t **out)
{
    return database_exec_ex(db, NULL, sql, size, binding, out);
}
