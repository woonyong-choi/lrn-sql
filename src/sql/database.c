#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/database.h"
#include "database_internal.h"
#include "sql/sql_eval.h"
#include "sql/sql_statement.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/overflow.h"
#include "storage/page_file.h"
#include "storage/text_tree.h"

/* 문장 수준 오류. 정리를 모두 마친 뒤 진입점이 결과로 만든다(그 결과를 만들 메모리를 먼저 돌려주기 위해). */
#define EXEC_PENDING ((database_exec_status_t)100)

typedef struct {
    result_error_class_t error_class;
    int code;
    char message[256];
} pending_error_t;

typedef struct {
    uint32_t *items;
    size_t len;
    size_t cap;
} page_list_t;

/* 쓰기 작업의 상태. database_internal.h의 설명을 따른다 */
typedef struct {
    bool active;
    bool failed;
    bool inside;                 /* 지금 실행 중인 문장이 이 작업의 문장이다(작업 카탈로그 기준으로 읽는다) */
    catalog_t *cat;              /* 작업 카탈로그. NULL이면 바꾼 것이 없다 */
    page_list_t old_committed;   /* 게시된 상태가 가리키고 작업이 대체한 페이지. 게시 성공 뒤에만 놓는다 */
    size_t res_cat;              /* resident로 계상한 작업 카탈로그 몫 */
    size_t res_lists;            /* resident로 계상한 위 목록들의 상한 */
} work_state_t;

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
    /* 쓰기 작업과 transaction 계층 */
    work_state_t work;
    bool read_only_call;
    database_control_t *control_sink;
    const database_layer_t *layer;
    void *layer_ctx;
    heap_stats_t stats;   /* 문장이 연 heap들이 올리는 누적 계수기 */
    /* 문장 동안 쥐는 서브쿼리·WITH·FROM 서브쿼리 결과와 보이는 WITH 이름 */
    struct relation *relations;
    struct cte_scope *ctes;
    database_exec_status_t subquery_status;  /* 서브쿼리 실행이 실패한 결과(평가기 오류에서 돌려준다) */
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
        size_t bytes = value_bytes(&cells[i]);
        value_clear(&cells[i]);
        q_release(db, bytes);  /* 비운 뒤에 몫을 돌려준다 */
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

/* ── 작업 카탈로그와 페이지 목록 ── */

/* 지금 문장이 읽고 고칠 카탈로그. 작업 안의 문장이면 작업 카탈로그, 아니면 게시된 카탈로그 */
static const catalog_t *base_catalog(const database_t *db)
{
    return db->work.inside && db->work.cat != NULL ? db->work.cat : db->catalog;
}

/* len + extra개를 담을 수 있게 키운다. 실패하면 목록은 그대로다. */
static bool list_reserve(page_list_t *list, size_t extra)
{
    if (extra > SIZE_MAX / sizeof(uint32_t) - list->len) return false;
    size_t need = list->len + extra;
    if (need <= list->cap) return true;
    size_t cap = list->cap == 0 ? 16 : list->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2 / sizeof(uint32_t)) return false;
        cap *= 2;
    }
    uint32_t *items = realloc(list->items, cap * sizeof(uint32_t));
    if (items == NULL) return false;
    list->items = items;
    list->cap = cap;
    return true;
}

/* reserve한 뒤에만 부른다 */
static void list_push(page_list_t *list, uint32_t page)
{
    assert(list->len < list->cap);
    list->items[list->len++] = page;
}

static void list_free(page_list_t *list)
{
    free(list->items);
    *list = (page_list_t){NULL, 0, 0};
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

/* 열기 admission: 잠근 fd의 크기와 제어 페이지가 약속한 카탈로그 크기를 소비해 할당 전에 reserve한다. */
typedef struct {
    const mem_ledger_t *resident;
    const char *path;
    size_t admitted;
} admission_t;

static bool admit_file(void *ctx, uint64_t file_bytes)
{
    admission_t *admission = ctx;
    /* 열린 핸들의 page_count는 file_bytes / 페이지 크기 이하다(page_file_open_admitted) */
    uint64_t pages = file_bytes / PAGE_FILE_PAGE_BYTES;
    size_t bytes = sizeof(database_t) + page_file_open_peak_bytes(admission->path) +
                   page_file_list_peak_bytes((uint32_t)(pages > UINT32_MAX ? UINT32_MAX : pages));
    if (!mem_reserve(admission->resident, MEM_SCOPE_RESIDENT, bytes)) return false;
    admission->admitted += bytes;
    return true;
}

static bool admit_catalog(void *ctx, size_t catalog_bytes)
{
    admission_t *admission = ctx;
    /* 읽기 buffer가 살아 있는 동안 페이지 buffer 또는 복호화 임시가 함께 있다. 복호화된 카탈로그는 그 안에 든다 */
    size_t decode = catalog_decode_peak_bytes(catalog_bytes);
    size_t bytes = catalog_bytes + (decode > PAGE_BODY_BYTES ? decode : PAGE_BODY_BYTES);
    if (!mem_reserve(admission->resident, MEM_SCOPE_RESIDENT, bytes)) return false;
    admission->admitted += bytes;
    return true;
}

static database_status_t open_database(const char *path, bool writable, admission_t *admission,
                                       database_t **out)
{
    page_file_t *file = NULL;
    page_file_status_t opened = page_file_open_admitted(path, writable, &file, NULL,
                                                        admission != NULL ? admit_file : NULL, admission);
    if (opened != PAGE_FILE_OK) return from_page_file(opened);
    database_t *db = calloc(1, sizeof(*db));
    if (db == NULL) {
        page_file_close(file);
        return DATABASE_NOMEM;
    }
    db->writable = writable;
    db->file = file;
    catalog_store_status_t loaded = catalog_store_load_admitted(db->file, admission != NULL ? admit_catalog : NULL,
                                                                admission, &db->catalog);
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

database_status_t database_open(const char *path, bool writable, database_t **out)
{
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    return open_database(path, writable, NULL, out);
}

static void work_reset(database_t *db);

database_status_t database_close(database_t *db)
{
    if (db == NULL) return DATABASE_OK;
    if (db->layer != NULL && db->layer->close != NULL) {
        /* 계층이 닫을 수 없으면(열린 트랜잭션) 아무것도 바꾸지 않고 돌려준다 */
        database_status_t closed_layer = db->layer->close(db->layer_ctx, db);
        if (closed_layer != DATABASE_OK) return closed_layer;
        db->layer = NULL;
    }
    if (db->work.active) database_work_end(db, DATABASE_WORK_NOT_APPLIED);
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

void database_scan_stats(const database_t *db, database_scan_stats_t *out)
{
    if (db == NULL || out == NULL) return;
    *out = (database_scan_stats_t){db->stats.rows_scanned, db->stats.index_lookups, db->stats.index_pages_read};
}

size_t database_table_count(const database_t *db)
{
    return db != NULL ? catalog_table_count(db->catalog) : 0;
}

static size_t overflow_write_bound(size_t size_bytes);

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

/*
 * admission으로 reserve한 몫을 열린 핸들의 실제 상주 몫으로 바꾼다. 실제 몫이 admission보다 크면 이미 할당한
 * 뒤라 예산을 지켰다고 말할 수 없으므로 상한 계산의 오류로 보고 거절한다(assert로도 잡힌다).
 */
static database_status_t finish_admission(database_t *db, const mem_ledger_t *resident, size_t admitted)
{
    size_t fixed = sizeof(*db) + page_file_handle_bytes(db->file);
    size_t catalog = catalog_memory_bytes(db->catalog);
    size_t lists = page_file_list_peak_bytes(page_file_control(db->file)->page_count);
    size_t exact = fixed + catalog + lists;
    assert(exact <= admitted);
    if (exact > admitted) return DATABASE_INVALID;
    mem_release(resident, MEM_SCOPE_RESIDENT, admitted - exact);
    mem_ledger_retain(resident);
    db->resident = *resident;
    db->res_fixed = fixed;
    db->res_catalog = catalog;
    db->res_lists = lists;
    return DATABASE_OK;
}

database_status_t database_open_bounded(const char *path, bool writable, const mem_ledger_t *resident,
                                        database_t **out)
{
    if (!mem_ledger_enabled(resident)) return database_open(path, writable, out);
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    admission_t admission = {resident, path, 0};
    database_t *db = NULL;
    database_status_t opened = open_database(path, writable, &admission, &db);
    if (opened != DATABASE_OK) {
        int saved = errno;
        mem_release(resident, MEM_SCOPE_RESIDENT, admission.admitted);
        errno = saved;
        return opened;
    }
    database_status_t attached = finish_admission(db, resident, admission.admitted);
    if (attached != DATABASE_OK) {
        database_close(db);
        mem_release(resident, MEM_SCOPE_RESIDENT, admission.admitted);
        return attached;
    }
    *out = db;
    return DATABASE_OK;
}

database_status_t database_create_bounded(const char *path, const mem_ledger_t *resident, database_t **out)
{
    if (!mem_ledger_enabled(resident)) return database_create(path, out);
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    /* 새 파일은 빈 카탈로그(부호화 32바이트) 하나와 페이지 몇 장뿐이다 */
    size_t empty = 32;
    size_t catalog = catalog_decode_peak_bytes(empty);
    size_t admitted = sizeof(database_t) + page_file_open_peak_bytes(path) + catalog +
                      page_file_list_peak_bytes(16) + empty + overflow_write_bound(empty);
    if (!mem_reserve(resident, MEM_SCOPE_RESIDENT, admitted)) return DATABASE_NOMEM;
    database_t *db = NULL;
    database_status_t created = database_create(path, &db);
    if (created != DATABASE_OK) {
        int saved = errno;
        mem_release(resident, MEM_SCOPE_RESIDENT, admitted);
        errno = saved;
        return created;
    }
    database_status_t attached = finish_admission(db, resident, admitted);
    if (attached != DATABASE_OK) {
        database_close(db);
        mem_release(resident, MEM_SCOPE_RESIDENT, admitted);
        return attached;
    }
    *out = db;
    return DATABASE_OK;
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
        case HEAP_KEY_EXISTS: return fail_result(db, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다 (%s)", where);
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
    const catalog_t *catalog = base_catalog(db);
    catalog_status_t found = catalog_find(catalog, (const char *)name->bytes, name->size, id);
    if (found == CATALOG_OK && catalog_describe(catalog, *id, view) == CATALOG_OK) return true;
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
    heap_index_spec_t index = {view->primary_key_column, view->primary_root, &db->stats};
    heap_status_t status = heap_open_indexed(db->file, id, view->heap_root, &schema, &index, &db->ledger, heap);
    q_free(db, types, bytes);
    return status;
}

static heap_status_t configure_secondary(heap_batch_t *batch, const catalog_t *catalog,
                                         catalog_table_id_t table_id)
{
    size_t count = catalog_index_count(catalog, table_id);
    if (count > CATALOG_MAX_INDEXES) return HEAP_CORRUPT;
    heap_secondary_spec_t specs[CATALOG_MAX_INDEXES];
    for (size_t i = 0; i < count; i++) {
        catalog_index_view_t view;
        if (catalog_index_at(catalog, table_id, i, &view) != CATALOG_OK) return HEAP_CORRUPT;
        specs[i] = (heap_secondary_spec_t){view.column, view.unique, view.root};
    }
    return heap_batch_configure_secondary(batch, specs, count);
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
    const catalog_t *base = base_catalog(db);
    size_t peak = catalog_clone_peak_bytes(base);
    if (!q_reserve(db, peak)) return false;
    catalog_t *clone = NULL;
    if (clone_catalog(base, &clone) != CATALOG_OK) {
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

/* ── 서브쿼리·WITH·FROM 서브쿼리 결과 ── */

/*
 * 질의 결과를 문장이 끝날 때까지 쥔다. key는 결과를 만든 문법 노드(sqlexpr_t·sqlstmt_source_t·sqlstmt_cte_t)이며
 * 같은 노드는 한 번만 실행한다. 바깥 행에 기대지 않으므로 한 번 계산하면 된다.
 */
typedef struct relation {
    const void *key;
    catalog_column_t *columns;
    size_t column_count;
    char *names;
    size_t names_bytes;
    value_t *cells;             /* row_count × column_count */
    size_t row_count;
    size_t row_cap;
    sqleval_subquery_t info;    /* 식 자리 서브쿼리로 쓸 때 */
    value_view_t *set;          /* IN ( query )의 정렬된 값. 처음 필요할 때 만든다 */
    size_t set_bytes;
    struct relation *next;
} relation_t;

/* 보이는 WITH 이름: owner의 ctes[0..visible) */
typedef struct cte_scope {
    const sqlstmt_select_t *owner;
    size_t visible;
    struct cte_scope *parent;
} cte_scope_t;

static database_exec_status_t exec_select(database_t *db, const sqlstmt_select_t *select, result_t **out);
typedef int (*row_compare_fn)(const void *ctx, const value_t *a, const value_t *b);
static void sort_rows(value_t **rows, value_t **scratch, size_t count, row_compare_fn cmp, const void *ctx);
static int compare_first_value(const void *ctx, const value_t *a, const value_t *b);

static void relation_free(database_t *db, relation_t *rel)
{
    if (rel->cells != NULL) owned_clear(db, rel->cells, rel->row_count * rel->column_count);
    q_free(db, rel->cells, rel->row_cap * rel->column_count * sizeof(value_t));
    q_free(db, rel->columns, (rel->column_count > 0 ? rel->column_count : 1) * sizeof(catalog_column_t));
    q_free(db, rel->names, rel->names_bytes);
    q_free(db, rel->set, rel->set_bytes);
    q_free(db, rel, sizeof(*rel));
}

static void relations_free(database_t *db)
{
    while (db->relations != NULL) {
        relation_t *next = db->relations->next;
        relation_free(db, db->relations);
        db->relations = next;
    }
}

static relation_t *relation_find(const database_t *db, const void *key)
{
    for (relation_t *rel = db->relations; rel != NULL; rel = rel->next) {
        if (rel->key == key) return rel;
    }
    return NULL;
}

static database_exec_status_t relation_add_row(database_t *db, relation_t *rel, const result_row_t *row)
{
    size_t n = rel->column_count;
    if (rel->row_count == rel->row_cap) {
        size_t cap = rel->row_cap == 0 ? 16 : rel->row_cap * 2;
        if (n > 0 && cap > SIZE_MAX / n / sizeof(value_t)) return DATABASE_EXEC_NOMEM;
        size_t bytes = cap * n * sizeof(value_t), old = rel->row_cap * n * sizeof(value_t);
        if (!q_reserve(db, bytes)) return DATABASE_EXEC_NOMEM;
        value_t *grown = realloc(rel->cells, bytes > 0 ? bytes : 1);
        if (grown == NULL) {
            q_release(db, bytes);
            return DATABASE_EXEC_NOMEM;
        }
        q_release(db, old);
        memset(grown + rel->row_cap * n, 0, (cap - rel->row_cap) * n * sizeof(value_t));
        rel->cells = grown;
        rel->row_cap = cap;
    }
    value_t *cells = rel->cells + rel->row_count * n;
    for (size_t c = 0; c < n; c++) {
        if (q_clone(db, &row->cells[c], &cells[c]) != VALUE_OK) {
            owned_clear(db, cells, c);
            return DATABASE_EXEC_NOMEM;
        }
    }
    rel->row_count++;
    return DATABASE_EXEC_OK;
}

/* query를 실행해 key의 결과로 쥔다. cte가 열 이름을 주면 그 이름을 쓴다 */
static database_exec_status_t relation_build(database_t *db, const void *key, const sqlstmt_select_t *query,
                                             const sqlstmt_cte_t *cte, relation_t **out)
{
    *out = relation_find(db, key);
    if (*out != NULL) return DATABASE_EXEC_OK;
    if (cancel_requested(db)) return cancelled_result(db);
    result_t *result = NULL;
    database_exec_status_t status = exec_select(db, query, &result);
    if (status != DATABASE_EXEC_OK) return status;
    relation_t *rel = q_calloc(db, 1, sizeof(*rel));
    if (rel == NULL) {
        result_release(&result);
        return DATABASE_EXEC_NOMEM;
    }
    rel->key = key;
    rel->next = db->relations;
    db->relations = rel;
    size_t n = result_column_count(result);
    rel->column_count = n;
    if (cte != NULL && cte->column_count > 0 && cte->column_count != n) {
        result_release(&result);
        return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_SUBQUERY, "WITH %.*s의 열 이름 수(%zu)가 결과 열 수(%zu)와 다릅니다",
                           name_len(cte->name.size), (const char *)cte->name.bytes, cte->column_count, n);
    }
    for (size_t c = 0; c < n; c++) {
        bool renamed = cte != NULL && cte->column_count > 0;
        rel->names_bytes += renamed ? cte->columns[c].size : result_column(result, c)->name_len;
    }
    rel->columns = q_calloc(db, n > 0 ? n : 1, sizeof(catalog_column_t));
    rel->names = rel->names_bytes > 0 ? q_malloc(db, rel->names_bytes) : NULL;
    if (rel->columns == NULL || (rel->names_bytes > 0 && rel->names == NULL)) {
        result_release(&result);
        return DATABASE_EXEC_NOMEM;
    }
    size_t used = 0;
    for (size_t c = 0; c < n; c++) {
        const result_column_t *meta = result_column(result, c);
        const uint8_t *name = meta->name;
        size_t length = meta->name_len;
        if (cte != NULL && cte->column_count > 0) {
            name = cte->columns[c].bytes;
            length = cte->columns[c].size;
        }
        if (length > 0) memcpy(rel->names + used, name, length);
        rel->columns[c] = (catalog_column_t){rel->names + used, length, meta->type, !meta->is_nullable, false, {0}};
        used += length;
    }
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW && status == DATABASE_EXEC_OK) {
        status = relation_add_row(db, rel, &row);
    }
    if (status == DATABASE_EXEC_OK && next != RESULT_NEXT_DONE) {
        status = next == RESULT_NEXT_CANCELLED ? cancelled_result(db) : DATABASE_EXEC_NOMEM;
    }
    result_release(&result);
    if (status != DATABASE_EXEC_OK) return status;
    rel->info = (sqleval_subquery_t){n > 0 ? rel->columns[0].type : VALUE_NULL, n, rel->row_count,
                                     rel->row_count > 0 && n > 0 ? &rel->cells[0].view : NULL, NULL, 0, false};
    *out = rel;
    return DATABASE_EXEC_OK;
}

/* IN ( query )용 정렬·중복 제거한 첫 열 값 */
static database_exec_status_t relation_set(database_t *db, relation_t *rel)
{
    if (rel->set != NULL || rel->row_count == 0) return DATABASE_EXEC_OK;
    size_t n = rel->column_count;
    rel->set_bytes = rel->row_count * sizeof(value_view_t);
    rel->set = q_malloc(db, rel->set_bytes);
    size_t scratch_bytes = rel->row_count * sizeof(value_t *);
    value_t **order = q_malloc(db, scratch_bytes);
    value_t **scratch = q_malloc(db, scratch_bytes);
    database_exec_status_t status = DATABASE_EXEC_OK;
    if (rel->set == NULL || order == NULL || scratch == NULL) {
        status = DATABASE_EXEC_NOMEM;
    } else {
        size_t count = 0;
        for (size_t r = 0; r < rel->row_count; r++) {
            value_t *cell = &rel->cells[r * n];
            if (cell->view.type == VALUE_NULL) rel->info.set_has_null = true;
            else order[count++] = cell;
        }
        sort_rows(order, scratch, count, compare_first_value, NULL);
        for (size_t i = 0; i < count; i++) {
            if (rel->info.set_count == 0 ||
                sqleval_compare(&rel->set[rel->info.set_count - 1], &order[i]->view) != 0) {
                rel->set[rel->info.set_count++] = order[i]->view;
            }
        }
        rel->info.set = rel->set;
    }
    q_free(db, order, scratch_bytes);
    q_free(db, scratch, scratch_bytes);
    return status;
}

static sqleval_status_t subquery_hook(void *ctx, const sqlexpr_t *expr, const sqleval_subquery_t **out)
{
    database_t *db = ctx;
    relation_t *rel = NULL;
    database_exec_status_t status = relation_build(db, expr, expr->subquery, NULL, &rel);
    if (status == DATABASE_EXEC_OK && expr->kind == SQLEXPR_IN) status = relation_set(db, rel);
    if (status != DATABASE_EXEC_OK) {
        db->subquery_status = status;
        return status == DATABASE_EXEC_NOMEM ? SQLEVAL_NOMEM : SQLEVAL_SUBQUERY;
    }
    *out = &rel->info;
    return SQLEVAL_OK;
}

/* FROM의 이름이 보이는 WITH 이름이면 그 결과를 만든다. 아니면 *out은 NULL */
static database_exec_status_t cte_relation(database_t *db, const sqlstmt_name_t *name, relation_t **out)
{
    *out = NULL;
    for (cte_scope_t *scope = db->ctes; scope != NULL; scope = scope->parent) {
        for (size_t i = 0; i < scope->visible; i++) {
            const sqlstmt_cte_t *cte = &scope->owner->ctes[i];
            if (cte->name.size != name->size || memcmp(cte->name.bytes, name->bytes, name->size) != 0) continue;
            /* WITH 질의는 자기보다 앞의 이름만 본다 */
            cte_scope_t own = {scope->owner, i, scope->parent};
            cte_scope_t *saved = db->ctes;
            db->ctes = &own;
            database_exec_status_t status = relation_build(db, cte, cte->query, cte, out);
            db->ctes = saved;
            return status;
        }
    }
    return DATABASE_EXEC_OK;
}

/* ── 식과 WHERE ── */

/* FROM 원본 하나. 결합 행에서 이 원본의 열은 offset부터 view->column_count칸이다 */
typedef struct {
    const catalog_table_view_t *view;
    const sqlstmt_name_t *table;
    const sqlstmt_name_t *alias;       /* 없으면 NULL이나 size 0 */
    size_t offset;
    bool nullable;                     /* LEFT JOIN 쪽이라 NOT NULL 열도 NULL이 될 수 있다 */
} source_ref_t;

/* 열 이름 해석. 앞의 count개 원본만 본다(ON은 자기 원본까지만 본다). qualifier는 원본의 별칭이나 이름이다 */
typedef struct {
    const source_ref_t *sources;
    size_t count;
} resolver_t;

static bool source_named(const source_ref_t *source, const sqlstmt_name_t *name)
{
    const sqlstmt_name_t *exposed = source->alias != NULL && source->alias->size > 0 ? source->alias : source->table;
    return exposed->size == name->size && memcmp(exposed->bytes, name->bytes, name->size) == 0;
}

static size_t resolve_column(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name,
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
static size_t source_of(const resolver_t *r, size_t column)
{
    size_t i = 0;
    while (i + 1 < r->count && column >= r->sources[i + 1].offset) i++;
    return i;
}

/* 평가 오류를 결과로 바꾼다 */
static database_exec_status_t eval_failure(database_t *db, const sqleval_error_t *error)
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

static sqleval_status_t subquery_hook(void *ctx, const sqlexpr_t *expr, const sqleval_subquery_t **out);

static bool compile_expr(database_t *db, const sqlexpr_t *expr, const resolver_t *resolver, sqleval_prog_t **prog,
                         database_exec_status_t *status)
{
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_env_t env = {resolver != NULL ? resolve_column : NULL, NULL, (void *)resolver, subquery_hook, db};
    sqleval_status_t compiled = sqleval_compile(expr, &env, prog, &error);
    if (compiled == SQLEVAL_OK) return true;
    *status = eval_failure(db, &error);
    return false;
}

/* 컬럼 = 상수 꼴의 조건(색인 탐침 후보). literal은 컬럼 형식으로 정확히 바꾼 값이다 */
#define WHERE_MAX_PROBES 8
typedef struct {
    size_t column;
    value_t literal;
} probe_t;

typedef struct {
    bool present;
    sqleval_prog_t *prog;
    bool never;                        /* 최상위 AND 안의 등가 조건이 어떤 행과도 맞을 수 없다 */
    probe_t probes[WHERE_MAX_PROBES];
    size_t probe_count;
} where_t;

static void where_clear(database_t *db, where_t *where)
{
    for (size_t i = 0; i < where->probe_count; i++) owned_clear(db, &where->probes[i].literal, 1);
    sqleval_free(where->prog);
    memset(where, 0, sizeof(*where));
}

/* 값을 컬럼 타입으로 정확히 변환한다. 실패하면 *status에 반환값을 둔다. */
static bool cast_for_column(database_t *db, const value_view_t *source, const catalog_column_t *column,
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

static bool values_equal(const value_view_t *a, const value_view_t *b)
{
    int order = 1;
    return a->type == b->type && value_compare(a, b, &order) == VALUE_OK && order == 0;
}

/* 최상위 AND를 따라 내려가며 컬럼 = 상수 조건을 모은다 */
static bool collect_probes(database_t *db, const sqlexpr_t *expr, const resolver_t *resolver, where_t *where,
                           database_exec_status_t *status)
{
    if (expr->kind == SQLEXPR_BINARY && expr->op == SQLEXPR_OP_AND) {
        return collect_probes(db, expr->args[0], resolver, where, status) &&
               collect_probes(db, expr->args[1], resolver, where, status);
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

static bool compile_where(database_t *db, const resolver_t *resolver, const sqlexpr_t *source, where_t *where,
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
static database_exec_status_t where_matches(database_t *db, const where_t *where, const value_t *row, bool *matched)
{
    *matched = !where->never;
    if (!where->present || where->never) return DATABASE_EXEC_OK;
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    if (sqleval_test(where->prog, row, matched, &error) == SQLEVAL_OK) return DATABASE_EXEC_OK;
    return eval_failure(db, &error);
}

/* 탐침으로 찾은 행이 그 탐침 조건을 만족하는지(색인이 가리키는 행의 검증) */
static bool probe_holds(const probe_t *probe, const value_t *row)
{
    return values_equal(&row[probe->column].view, &probe->literal.view);
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
 * 쓰기 작업 안의 문장을 게시하지 않고 작업 카탈로그에 쌓는다. 데이터 페이지는 이미 새 자리에 쓰였다. 문장이 대체한
 * 페이지는 놓지 않고 목록에 보관한다(작업이 게시된 뒤에만 놓는다). 실패하면 이 문장의 새 페이지만 돌려주고 작업
 * 카탈로그와 목록은 그대로다. 호출자가 batch를 free한다.
 */
typedef struct {
    size_t cat_bytes;
    size_t lists_bytes;
    size_t work_lists_bytes;
    size_t extra;
} stage_plan_t;

static bool stage_prepare(database_t *db, const catalog_t *cat, size_t new_pages, stage_plan_t *plan)
{
    *plan = (stage_plan_t){db->work.res_cat, db->res_lists, db->work.res_lists, 0};
    if (!mem_ledger_enabled(&db->resident)) return true;
    size_t chain = catalog_encoded_bytes(cat) / (PAGE_BODY_BYTES - OVERFLOW_MIN_PAGE_BYTES) + 1;
    uint64_t pages = (uint64_t)page_file_allocated_pages(db->file) + new_pages + chain;
    if (pages > UINT32_MAX) pages = UINT32_MAX;
    plan->cat_bytes = catalog_memory_bytes(cat);
    size_t lists = page_file_list_peak_bytes((uint32_t)pages);
    plan->lists_bytes = lists > db->res_lists ? lists : db->res_lists;
    plan->work_lists_bytes = lists > db->work.res_lists ? lists : db->work.res_lists;
    size_t extra = plan->lists_bytes - db->res_lists + plan->work_lists_bytes - db->work.res_lists;
    if (plan->cat_bytes > db->work.res_cat) extra += plan->cat_bytes - db->work.res_cat;
    if (!mem_reserve(&db->resident, MEM_SCOPE_RESIDENT, extra)) return false;
    plan->extra = extra;
    return true;
}

static void stage_finish(database_t *db, const stage_plan_t *plan, bool adopted)
{
    if (!mem_ledger_enabled(&db->resident)) return;
    if (adopted) {
        if (plan->cat_bytes < db->work.res_cat) {
            mem_release(&db->resident, MEM_SCOPE_RESIDENT, db->work.res_cat - plan->cat_bytes);
        }
        db->work.res_cat = plan->cat_bytes;
    } else if (plan->cat_bytes > db->work.res_cat) {
        mem_release(&db->resident, MEM_SCOPE_RESIDENT, plan->cat_bytes - db->work.res_cat);
    }
    /* 실패한 시도가 페이지를 할당했을 수 있어 목록 몫은 유지한다 */
    db->res_lists = plan->lists_bytes;
    db->work.res_lists = plan->work_lists_bytes;
}

static database_exec_status_t stage_adopt(database_t *db, work_t *work, heap_batch_t *batch,
                                          const heap_batch_result_t *finished, const uint32_t *drop_pages,
                                          size_t drop_count, result_t *success, result_t **out)
{
    size_t old_total = finished->old_count + drop_count;
    if (!list_reserve(&db->work.old_committed, old_total)) {
        publish_discard(db, work, batch, &success);
        return DATABASE_EXEC_NOMEM;
    }
    stage_plan_t plan;
    if (!stage_prepare(db, work->cat, finished->new_count, &plan)) {
        publish_discard(db, work, batch, &success);
        return DATABASE_EXEC_NOMEM;
    }
    /*
     * 여기서부터 실패하지 않는다. 이 작업의 앞선 문장이 만든 페이지(아직 어느 제어 페이지도 가리킨 적 없다)는 곧바로
     * 놓아 다음 문장이 다시 쓴다(새 페이지의 release는 실패하지 않는다). 게시된 상태가 가리키는 페이지는 게시 뒤에 놓는다.
     */
    for (size_t i = 0; i < finished->old_count + drop_count; i++) {
        uint32_t page = i < finished->old_count ? finished->old_pages[i] : drop_pages[i - finished->old_count];
        if (page_file_is_fresh(db->file, page)) page_file_release(db->file, page);
        else list_push(&db->work.old_committed, page);
    }
    catalog_close(db->work.cat);
    db->work.cat = work->cat;
    q_release(db, work->charged);  /* 이 카탈로그는 이제 문장 몫이 아니라 상주 몫이다 */
    *work = (work_t){NULL, 0};
    stage_finish(db, &plan, true);
    *out = success;
    return DATABASE_EXEC_OK;
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
                                            heap_batch_t *batch,
                                            const uint32_t *drop_pages, size_t drop_count,
                                            result_t *success, result_t **out)
{
    if (cancel_requested(db)) {
        publish_discard(db, work, batch, &success);
        return cancelled_result(db);
    }
    size_t new_pages = 0;
    heap_batch_result_t finished = {0};
    if (batch != NULL) {
        heap_status_t finish = heap_batch_finish(batch, &finished);
        if (finish != HEAP_OK) {
            publish_discard(db, work, batch, &success);
            return heap_failure(db, finish, "finish");
        }
        new_pages = finished.new_count;
        if (catalog_set_locators(work->cat, id, finished.new_root, finished.new_primary_root) != CATALOG_OK) {
            publish_discard(db, work, batch, &success);
            return fail_result(db, RESULT_E_INTERNAL, 3, "카탈로그 locator를 갱신하지 못했습니다");
        }
        size_t secondary_count = catalog_index_count(work->cat, id);
        if (secondary_count != finished.secondary_count) {
            publish_discard(db, work, batch, &success);
            return fail_result(db, RESULT_E_INTERNAL, 3, "보조 색인 수가 맞지 않습니다");
        }
        for (size_t i = 0; i < secondary_count; i++) {
            catalog_index_view_t index;
            if (catalog_index_at(work->cat, id, i, &index) != CATALOG_OK ||
                catalog_set_index_root(work->cat, index.name, index.name_bytes,
                                       finished.new_secondary_roots[i]) != CATALOG_OK) {
                publish_discard(db, work, batch, &success);
                return fail_result(db, RESULT_E_INTERNAL, 3, "보조 색인 root를 갱신하지 못했습니다");
            }
        }
    }
    if (db->work.inside) return stage_adopt(db, work, batch, &finished, drop_pages, drop_count, success, out);

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

    /*
     * 새 상태가 가리키지 않는 옛 페이지는 commit 전에 놓아 그 commit의 빈 목록에 함께 적는다. 반영되지 않으면
     * catalog_store_save가 되돌린다. 놓지 못한 페이지는 새는 공간일 뿐이다
     */
    if (batch != NULL) heap_batch_release_old(batch);
    for (size_t i = 0; i < drop_count; i++) page_file_release(db->file, drop_pages[i]);
    catalog_store_status_t saved = catalog_store_save(db->file, work->cat);
    int saved_errno = errno;
    q_release(db, save_peak);
    if (saved == CATALOG_STORE_OK || saved == CATALOG_STORE_UNRECLAIMED) {
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
                                      heap_batch_t *batch,
                                      const uint32_t *drop_pages, size_t drop_count,
                                      result_t *success, result_t **out)
{
    database_exec_status_t status = publish_inner(db, work, id, batch, drop_pages,
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

static bool eval_constant(database_t *db, const sqlexpr_t *expr, value_t *out, database_exec_status_t *status);

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

static database_exec_status_t exec_create(database_t *db, const sqlstmt_create_t *create, result_t **out)
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

static database_exec_status_t exec_create_index(database_t *db, const sqlstmt_index_t *index, result_t **out)
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
    catalog_status_t added = catalog_add_index(work.cat, id, (const char *)index->name.bytes,
                                               index->name.size, (uint16_t)column, index->unique);
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
static database_exec_status_t probe_key_index(database_t *db, const catalog_table_view_t *view, heap_t *heap,
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

/* ── INSERT ── */

static database_exec_status_t duplicate_key(database_t *db, const catalog_table_view_t *view)
{
    size_t pk = view->primary_key_column;
    return fail_result(db, RESULT_E_CONSTRAINT, 11, "기본 키가 중복됩니다: %.*s", name_len(view->columns[pk].name_bytes),
                       view->columns[pk].name);
}

static database_exec_status_t not_null_violation(database_t *db, const catalog_column_t *column)
{
    return fail_result(db, RESULT_E_CONSTRAINT, 8, "NOT NULL 제약을 어겼습니다: %.*s",
                       name_len(column->name_bytes), column->name);
}

/* 상수식 하나를 계산한다(INSERT 값). 열을 쓰면 NOT_CONSTANT 오류다 */
static bool eval_constant(database_t *db, const sqlexpr_t *expr, value_t *out, database_exec_status_t *status)
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

/* RETURNING: 바뀐 행마다 목록을 계산해 결과 행으로 모은다. 목록이 없으면 명령 결과를 만든다 */
typedef struct returning returning_t;
static database_exec_status_t returning_open(database_t *db, const sqlstmt_returning_t *spec,
                                             const catalog_table_view_t *view, const sqlstmt_name_t *table,
                                             returning_t **out);
static database_exec_status_t returning_row(returning_t *ret, const value_t *row);
static database_exec_status_t returning_finish(database_t *db, returning_t *ret, const char *tag, uint64_t affected,
                                               result_t **success);
static void returning_close(database_t *db, returning_t *ret);

static database_exec_status_t exec_insert(database_t *db, const sqlstmt_insert_t *insert, result_t **out)
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
        if (status != DATABASE_EXEC_OK) goto done;
    }
    if (catalog_set_row_count(work.cat, id, view.row_count + insert->row_count) != CATALOG_OK) {
        status = fail_result(db, RESULT_E_LIMIT, 14, "행 수 한도를 넘습니다");
        goto done;
    }
    status = returning_finish(db, ret, "INSERT", insert->row_count, &success);
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
    returning_close(db, ret);
    q_free(db, views, views_bytes);
    if (cells != NULL) owned_clear(db, cells, n);
    q_free(db, cells, cells_bytes);
    q_free(db, source, source_bytes);
    return status;
}

/* ── SELECT ── */

/* 선택 목록의 출력 열 하나. prog가 NULL이면 테이블 열 column을 그대로 낸다 */
typedef struct {
    sqleval_prog_t *prog;
    size_t column;
    result_column_t meta;
} select_out_t;

/* 정렬 키. slot은 행 안의 자리: prog가 NULL이면 출력 열(ORDER BY 번호·별칭), 아니면 출력 열 뒤의 식 자리 */
typedef struct {
    sqleval_prog_t *prog;
    size_t slot;
    bool desc;
    bool nulls_first;
} select_key_t;

typedef struct {
    database_t *db;
    select_out_t *outs;
    size_t out_count;
    size_t outs_bytes;
    select_key_t *keys;
    size_t key_count;
    size_t extra_count;         /* 식 정렬 키 수(행마다 출력 열 뒤에 둔다) */
    size_t keys_bytes;
    result_column_t *columns;   /* out_count개 결과 열 */
    bool distinct;
    bool buffered;              /* ORDER BY·DISTINCT는 모아서 정렬한다 */
    bool has_limit;
    uint64_t limit;
    uint64_t offset;
    uint64_t skipped;           /* 스트리밍에서 OFFSET으로 건너뛴 행 */
    uint64_t emitted;
    bool done;                  /* LIMIT을 채웠다. 더 읽지 않는다 */
    result_builder_t *builder;
    value_t *cells;             /* out_count개. 스트리밍 행의 계산값 */
    value_view_t *views;
    value_t **rows;             /* 모은 행. 행마다 out_count + key_count개 값 */
    size_t row_count;
    size_t row_cap;
} select_ctx_t;

static size_t select_width(const select_ctx_t *ctx) { return ctx->out_count + ctx->extra_count; }

/* 출력 열이 정해진 뒤 결과 열·계산 자리·결과 builder를 만든다 */
static database_exec_status_t select_prepare(select_ctx_t *ctx)
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

static void select_rows_free(select_ctx_t *ctx);

static void select_ctx_free(select_ctx_t *ctx)
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
static database_exec_status_t select_value(database_t *db, const sqleval_prog_t *prog, size_t column,
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
static database_exec_status_t select_emit(select_ctx_t *ctx, const value_t *row)
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
static database_exec_status_t select_flush(select_ctx_t *ctx)
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
static bool constant_count(database_t *db, const sqlexpr_t *expr, bool *present, uint64_t *value,
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
static size_t key_probe(const catalog_table_view_t *view, const heap_t *heap, const where_t *where)
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

/* 보조 색인 탐침 번호. 없으면 SIZE_MAX */
static size_t secondary_probe(database_t *db, catalog_table_id_t table_id, const where_t *where,
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
            *spec = (heap_secondary_spec_t){index.column, index.unique, index.root};
            return p;
        }
    }
    return SIZE_MAX;
}

static database_exec_status_t locate_key(database_t *db, heap_t *heap, const value_view_t *key, bool *is_hit,
                                         heap_locator_t *at)
{
    heap_status_t found = key->type == VALUE_TEXT ? heap_find_text_key(heap, key->bytes, key->size_bytes, at)
                                                  : heap_find_key(heap, key->integer, at);
    *is_hit = found == HEAP_OK;
    return found == HEAP_OK || found == HEAP_NOT_FOUND ? DATABASE_EXEC_OK : heap_failure(db, found, "lookup");
}

/* ── 행 공급: WHERE에 맞는 행을 받는 쪽에 넘긴다 ── */

typedef struct {
    database_exec_status_t (*fn)(void *ctx, const value_t *row);
    void *ctx;
    const bool *stop;           /* 참이 되면 더 넘기지 않는다(LIMIT을 채움) */
} row_sink_t;

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

/* 기본 키·보조 색인 탐침이 있으면 그것으로, 없으면 전체를 훑는다 */
static database_exec_status_t scan_rows(database_t *db, const catalog_table_view_t *table, catalog_table_id_t id,
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
        if (scan.status != DATABASE_EXEC_OK) return scan.status;
        return visited == HEAP_OK ? DATABASE_EXEC_OK : heap_failure(db, visited, "index lookup");
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

/* ── 식 컴파일 문맥 ── */

typedef struct {
    sqleval_resolve_fn resolve;
    sqleval_match_fn match;
    void *ctx;
    const resolver_t *table;    /* FROM 원본들. 열 그대로 내보내기·*에 쓴다 */
} compile_ctx_t;

static bool compile_in(database_t *db, const sqlexpr_t *expr, const compile_ctx_t *cc, sqleval_prog_t **prog,
                       database_exec_status_t *status)
{
    sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
    sqleval_env_t env = {cc->resolve, cc->match, cc->ctx, subquery_hook, db};
    if (sqleval_compile(expr, &env, prog, &error) == SQLEVAL_OK) return true;
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
    const catalog_column_t *def = &source->view->columns[column - source->offset];
    return (result_column_t){(const uint8_t *)def->name, def->name_bytes, def->type, !def->not_null || source->nullable};
}

/* 선택 목록을 출력 열로 펼친다. *는 모든 원본의 열, t.*는 그 원본의 열이다 */
static database_exec_status_t plan_outputs(database_t *db, const sqlstmt_select_item_t *items, size_t item_count,
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
static database_exec_status_t plan_keys(database_t *db, const sqlstmt_select_t *select, const compile_ctx_t *cc,
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

/* ── 그룹과 집계 ── */

typedef enum { AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX } agg_kind_t;

typedef struct {
    const sqlexpr_t *call;
    agg_kind_t kind;
    sqleval_prog_t *arg;        /* COUNT(*)는 NULL */
    value_type_t type;          /* 결과 형식 */
} aggregate_t;

/* 그룹 행 배치: [GROUP BY 값 key_count개][집계 값 agg_count개] */
typedef struct {
    const resolver_t *table;
    const sqlexpr_t **keys;
    sqleval_prog_t **key_progs;
    value_type_t *key_types;
    size_t key_count;
    aggregate_t *aggs;
    size_t agg_count;
    size_t agg_cap;
    size_t keys_bytes;
} group_plan_t;

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

static size_t group_match(void *ctx, const sqlexpr_t *expr, value_type_t *type)
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
static size_t group_resolve(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name, value_type_t *type)
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

static void group_plan_free(database_t *db, group_plan_t *plan)
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
static database_exec_status_t plan_groups(database_t *db, const sqlstmt_select_t *select, const resolver_t *table,
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

/* 행마다 GROUP BY 값과 집계 인자를 모은다 */
typedef struct {
    database_t *db;
    const group_plan_t *plan;
    value_t **entries;
    size_t count;
    size_t cap;
} group_rows_t;

static size_t group_width(const group_plan_t *plan) { return plan->key_count + plan->agg_count; }

static database_exec_status_t group_collect(void *context, const value_t *row)
{
    group_rows_t *rows = context;
    database_t *db = rows->db;
    const group_plan_t *plan = rows->plan;
    if (rows->count == rows->cap) {
        size_t cap = rows->cap == 0 ? 64 : rows->cap * 2;
        if (cap > SIZE_MAX / sizeof(value_t *) || !q_reserve(db, cap * sizeof(value_t *))) return DATABASE_EXEC_NOMEM;
        value_t **grown = realloc(rows->entries, cap * sizeof(*grown));
        if (grown == NULL) {
            q_release(db, cap * sizeof(value_t *));
            return DATABASE_EXEC_NOMEM;
        }
        q_release(db, rows->cap * sizeof(value_t *));
        rows->entries = grown;
        rows->cap = cap;
    }
    size_t width = group_width(plan);
    value_t *entry = q_calloc(db, width > 0 ? width : 1, sizeof(value_t));
    if (entry == NULL) return DATABASE_EXEC_NOMEM;
    database_exec_status_t status = DATABASE_EXEC_OK;
    for (size_t g = 0; g < plan->key_count && status == DATABASE_EXEC_OK; g++) {
        status = select_value(db, plan->key_progs[g], 0, row, &entry[g]);
    }
    for (size_t a = 0; a < plan->agg_count && status == DATABASE_EXEC_OK; a++) {
        if (plan->aggs[a].arg != NULL) status = select_value(db, plan->aggs[a].arg, 0, row, &entry[plan->key_count + a]);
    }
    if (status != DATABASE_EXEC_OK) {
        owned_clear(db, entry, width);
        q_free(db, entry, (width > 0 ? width : 1) * sizeof(value_t));
        return status;
    }
    rows->entries[rows->count++] = entry;
    return DATABASE_EXEC_OK;
}

static void group_rows_free(group_rows_t *rows)
{
    size_t width = group_width(rows->plan);
    for (size_t i = 0; i < rows->count; i++) {
        owned_clear(rows->db, rows->entries[i], width);
        q_free(rows->db, rows->entries[i], (width > 0 ? width : 1) * sizeof(value_t));
    }
    q_free(rows->db, rows->entries, rows->cap * sizeof(value_t *));
    rows->entries = NULL;
    rows->count = rows->cap = 0;
}

/* 안정 병합 정렬. cmp는 행 포인터 둘을 비교한다 */

static void sort_rows(value_t **rows, value_t **scratch, size_t count, row_compare_fn cmp, const void *ctx)
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

static int compare_first_value(const void *ctx, const value_t *a, const value_t *b)
{
    (void)ctx;
    return sqleval_compare(&a->view, &b->view);
}

/* 한 그룹의 집계 하나를 계산한다. run은 그 그룹의 행들이다 */
static database_exec_status_t fold_aggregate(database_t *db, const group_plan_t *plan, size_t index, value_t **run,
                                             size_t count, value_t *out)
{
    const aggregate_t *agg = &plan->aggs[index];
    if (agg->arg == NULL) return value_set_integer(out, (int64_t)count) == VALUE_OK ? DATABASE_EXEC_OK : DATABASE_EXEC_NOMEM;
    size_t slot = plan->key_count + index;
    size_t values_bytes = (count > 0 ? count : 1) * sizeof(value_t *);
    value_t **values = q_malloc(db, values_bytes);
    if (values == NULL) return DATABASE_EXEC_NOMEM;
    size_t present = 0;
    for (size_t i = 0; i < count; i++) {
        if (run[i][slot].view.type != VALUE_NULL) values[present++] = &run[i][slot];
    }
    if (agg->call->distinct && present > 1) {
        value_t **scratch = q_malloc(db, present * sizeof(value_t *));
        if (scratch == NULL) {
            q_free(db, values, values_bytes);
            return DATABASE_EXEC_NOMEM;
        }
        sort_rows(values, scratch, present, compare_first_value, NULL);
        q_free(db, scratch, present * sizeof(value_t *));
        size_t kept = 1;
        for (size_t i = 1; i < present; i++) {
            if (sqleval_compare(&values[kept - 1]->view, &values[i]->view) != 0) values[kept++] = values[i];
        }
        present = kept;
    }
    database_exec_status_t status = DATABASE_EXEC_OK;
    value_status_t set = VALUE_OK;
    if (agg->kind == AGG_COUNT) {
        set = value_set_integer(out, (int64_t)present);
    } else if (present == 0) {
        value_clear(out);
    } else if (agg->kind == AGG_MIN || agg->kind == AGG_MAX) {
        const value_t *best = values[0];
        for (size_t i = 1; i < present; i++) {
            int order = sqleval_compare(&values[i]->view, &best->view);
            if (agg->kind == AGG_MIN ? order < 0 : order > 0) best = values[i];
        }
        set = q_clone(db, &best->view, out) == VALUE_OK ? VALUE_OK : VALUE_NOMEM;
        if (set == VALUE_OK) q_release(db, value_bytes(out));  /* 그룹 행이 따로 계상한다 */
    } else {
        bool integers = agg->type == VALUE_INTEGER;
        __int128 whole = 0;
        double real = 0.0;
        for (size_t i = 0; i < present; i++) {
            const value_view_t *v = &values[i]->view;
            if (v->type == VALUE_INTEGER) whole += v->integer;
            else real += v->real;
        }
        if (agg->kind == AGG_SUM && integers) {
            if (whole > INT64_MAX || whole < INT64_MIN) {
                status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_ARITHMETIC_OVERFLOW, "SUM이 정수 범위를 넘습니다");
            } else {
                set = value_set_integer(out, (int64_t)whole);
            }
        } else {
            double total = (double)whole + real;
            set = value_set_real(out, agg->kind == AGG_AVG ? total / (double)present : total);
        }
    }
    q_free(db, values, values_bytes);
    if (status != DATABASE_EXEC_OK) return status;
    if (set == VALUE_NOMEM) return DATABASE_EXEC_NOMEM;
    if (set != VALUE_OK) {
        return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_ARITHMETIC_OVERFLOW, "집계 결과가 범위를 넘습니다");
    }
    return DATABASE_EXEC_OK;
}

/* 모은 행을 그룹으로 접어 HAVING을 통과한 그룹 행을 내보낸다 */
static database_exec_status_t group_flush(database_t *db, const group_plan_t *plan, group_rows_t *rows,
                                          const sqleval_prog_t *having, select_ctx_t *ctx)
{
    size_t count = rows->count;
    if (count > 1 && plan->key_count > 0) {
        value_t **scratch = q_malloc(db, count * sizeof(value_t *));
        if (scratch == NULL) return DATABASE_EXEC_NOMEM;
        sort_rows(rows->entries, scratch, count, compare_group_keys, plan);
        q_free(db, scratch, count * sizeof(value_t *));
    }
    size_t width = group_width(plan);
    size_t group_bytes = (width > 0 ? width : 1) * sizeof(value_t);
    value_t *group = q_calloc(db, width > 0 ? width : 1, sizeof(value_t));
    if (group == NULL) return DATABASE_EXEC_NOMEM;
    database_exec_status_t status = DATABASE_EXEC_OK;
    /* GROUP BY가 없으면 행이 없어도 그룹 하나다 */
    size_t lo = 0;
    bool single = plan->key_count == 0;
    while (status == DATABASE_EXEC_OK && !ctx->done && (single ? lo == 0 : lo < count)) {
        size_t hi = single ? count : lo + 1;
        while (!single && hi < count && compare_group_keys(plan, rows->entries[lo], rows->entries[hi]) == 0) hi++;
        for (size_t g = 0; g < plan->key_count && status == DATABASE_EXEC_OK; g++) {
            status = q_clone(db, &rows->entries[lo][g].view, &group[g]) == VALUE_OK ? DATABASE_EXEC_OK : DATABASE_EXEC_NOMEM;
        }
        for (size_t a = 0; a < plan->agg_count && status == DATABASE_EXEC_OK; a++) {
            status = fold_aggregate(db, plan, a, rows->entries + lo, hi - lo, &group[plan->key_count + a]);
            if (status == DATABASE_EXEC_OK && !q_reserve(db, value_bytes(&group[plan->key_count + a]))) {
                value_clear(&group[plan->key_count + a]);
                status = DATABASE_EXEC_NOMEM;
            }
        }
        bool keep = true;
        if (status == DATABASE_EXEC_OK && having != NULL) {
            sqleval_error_t error = {SQLEVAL_OK, 0, NULL};
            if (sqleval_test(having, group, &keep, &error) != SQLEVAL_OK) status = eval_failure(db, &error);
        }
        if (status == DATABASE_EXEC_OK && keep) status = select_emit(ctx, group);
        owned_clear(db, group, width);
        if (single) break;
        lo = hi;
    }
    q_free(db, group, group_bytes);
    return status;
}

static database_exec_status_t emit_sink(void *context, const value_t *row) { return select_emit(context, row); }

static bool select_is_grouped(const sqlstmt_select_t *select)
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

/* ── 결합: 원본마다 한 단계인 중첩 루프. 앞 단계 값으로 색인 탐침을 만든다 ── */

/* 최상위 AND로 나눈 조건 하나. level 단계에서 행을 받을 때 본다. after면 LEFT JOIN의 NULL 채움 뒤에 본다 */
typedef struct {
    sqleval_prog_t *prog;
    size_t level;
    bool after;
} join_cond_t;

/* 이 단계 원본의 열 = 앞 단계만 쓰는 식 */
typedef struct {
    size_t column;              /* 원본 안 열 번호 */
    value_type_t type;
    sqleval_prog_t *value;
} join_probe_t;

typedef struct join join_t;

typedef struct {
    join_t *join;
    size_t level;               /* FROM 순서의 원본 번호 */
    size_t pos;                 /* 실행 순서. RIGHT JOIN이면 첫 두 원본이 바뀐다 */
    catalog_table_id_t id;
    catalog_table_view_t view;
    heap_t *heap;
    relation_t *relation;       /* WITH·FROM 서브쿼리 결과면 heap 대신 이것을 훑는다 */
    bool left;
    bool matched;               /* 이번 바깥 행에서 ON을 통과한 행이 있었는지 */
    join_probe_t probes[WHERE_MAX_PROBES];
    size_t probe_count;
    row_sink_t sink;
} join_level_t;

struct join {
    database_t *db;
    join_level_t *levels;       /* FROM 순서 */
    size_t *order;              /* 실행 순서 → 원본 번호 */
    size_t level_count;
    source_ref_t *sources;
    join_cond_t *conds;
    size_t cond_count;
    size_t cond_cap;
    value_t *row;               /* 결합 행 */
    size_t width;
    bool *needed;               /* 결합 행에서 읽을 칸 */
    bool *scratch;              /* 조건이 쓰는 칸 표시용 */
    const row_sink_t *out;
};

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

static database_exec_status_t join_run(join_t *join, size_t level);

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

/* 열 = 식 꼴이면 그 단계의 색인 탐침으로 쓴다(조건 자체도 그대로 검사한다) */
static database_exec_status_t try_probe(join_t *join, const sqlexpr_t *expr, const resolver_t *resolver, size_t level)
{
    if (expr->kind != SQLEXPR_BINARY || expr->op != SQLEXPR_OP_EQ || level >= join->level_count) {
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
        step->probes[step->probe_count++] = (join_probe_t){at - source->offset, type, value};
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

/*
 * SELECT 핵심 하나를 실행해 ctx에 행을 낸다. core_only면 ORDER BY·LIMIT·OFFSET을 보지 않고 행을 모두 ctx->rows에
 * 모은다(집합 연산의 한쪽). 아니면 결과 builder로 바로 내보내거나(스트리밍) 정렬하려고 모은다.
 */
static database_exec_status_t select_run(database_t *db, const sqlstmt_select_t *select, bool core_only,
                                         select_ctx_t *ctx)
{
    group_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    group_rows_t groups = {db, &plan, NULL, 0, 0};
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
static database_exec_status_t select_finish(select_ctx_t *ctx, result_t **out)
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
static database_exec_status_t exec_select(database_t *db, const sqlstmt_select_t *select, result_t **out)
{
    if (select->cte_count == 0) return exec_query(db, select, out);
    cte_scope_t scope = {select, select->cte_count, db->ctes};
    db->ctes = &scope;
    database_exec_status_t status = exec_query(db, select, out);
    db->ctes = scope.parent;
    return status;
}

/* ── RETURNING ── */

struct returning {
    select_ctx_t ctx;
    source_ref_t source;
    resolver_t resolver;
};

static database_exec_status_t returning_open(database_t *db, const sqlstmt_returning_t *spec,
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

static database_exec_status_t returning_row(returning_t *ret, const value_t *row)
{
    return ret != NULL ? select_emit(&ret->ctx, row) : DATABASE_EXEC_OK;
}

static database_exec_status_t returning_finish(database_t *db, returning_t *ret, const char *tag, uint64_t affected,
                                               result_t **success)
{
    if (ret == NULL) return open_command(db, tag, affected, success) == RESULT_OK ? DATABASE_EXEC_OK : DATABASE_EXEC_NOMEM;
    result_status_t finished = result_builder_finish(&ret->ctx.builder, success);
    if (finished == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    return finished == RESULT_OK ? DATABASE_EXEC_OK : fail_result(db, RESULT_E_LIMIT, 17, "결과를 만들지 못했습니다");
}

static void returning_close(database_t *db, returning_t *ret)
{
    if (ret == NULL) return;
    select_ctx_free(&ret->ctx);
    q_free(db, ret, sizeof(*ret));
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

/*
 * WHERE에 맞는 행을 모은다. 기본 키·보조 색인 탐침이 있으면 그것으로 후보를 찾고, 없으면 전체를 훑는다.
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

static bool has_duplicate(key_ref_t *keys, size_t count)
{
    sort_keys(keys, count);
    for (size_t i = 1; i < count; i++) {
        if (values_equal(keys[i - 1].view, keys[i].view)) return true;
    }
    return false;
}

/* 기본 키 갱신에서 일치하지 않은 행의 키를 모은다(색인 없는 표) */
typedef struct {
    database_t *db;
    size_t pk;
    value_t *keys;
    size_t count;
    size_t cap;
} other_keys_t;

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

static database_exec_status_t exec_update(database_t *db, const sqlstmt_update_t *update, result_t **out)
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

static database_exec_status_t exec_delete(database_t *db, const sqlstmt_delete_t *del, result_t **out)
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
    if (statement->kind == SQLSTMT_BEGIN || statement->kind == SQLSTMT_COMMIT || statement->kind == SQLSTMT_ROLLBACK) {
        if (db->control_sink == NULL) {
            status = fail_result(db, RESULT_E_SEMANTIC, 34, "트랜잭션 제어문은 연결(session)에서만 실행할 수 있습니다");
            goto done;
        }
        db->control_sink->kind = statement->kind == SQLSTMT_BEGIN ? DATABASE_CONTROL_BEGIN
                                 : statement->kind == SQLSTMT_COMMIT ? DATABASE_CONTROL_COMMIT
                                                                     : DATABASE_CONTROL_ROLLBACK;
        status = DATABASE_EXEC_CONTROL;
        goto done;
    }
    if (db->work.inside && db->work.failed) {
        status = fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_WORK_FAILED,
                             "이 트랜잭션은 이전 문장 오류로 실패한 상태입니다. rollback해야 합니다");
        goto done;
    }
    if (statement->kind != SQLSTMT_SELECT && db->read_only_call) {
        status = fail_result(db, RESULT_E_BUSY, DATABASE_CODE_WRITER_BUSY,
                             "다른 트랜잭션이 쓰기 중이라 변경 문장을 실행할 수 없습니다");
        goto done;
    }
    if (statement->kind != SQLSTMT_SELECT && !db->writable) {
        status = fail_result(db, RESULT_E_SEMANTIC, 22, "읽기 전용으로 연 데이터베이스입니다");
        goto done;
    }
    switch (statement->kind) {
        case SQLSTMT_CREATE_TABLE: status = exec_create(db, &statement->create, out); break;
        case SQLSTMT_CREATE_INDEX: status = exec_create_index(db, &statement->index, out); break;
        case SQLSTMT_INSERT: status = exec_insert(db, &statement->insert, out); break;
        case SQLSTMT_SELECT: status = exec_select(db, &statement->select, out); break;
        case SQLSTMT_UPDATE: status = exec_update(db, &statement->update, out); break;
        case SQLSTMT_DELETE: status = exec_delete(db, &statement->del, out); break;
        case SQLSTMT_DROP_TABLE: status = exec_drop(db, &statement->drop, out); break;
        default: status = fail_result(db, RESULT_E_INTERNAL, 21, "문장을 실행하지 못했습니다"); break;
    }
done:
    relations_free(db);
    sqlstmt_free(statement);
    q_release(db, parse_peak);
    lexer_free(lexed);
    q_release(db, lex_peak);
    return status;
}

static database_exec_status_t run_statement(database_t *db, const database_exec_options_t *opts,
                                            const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                            result_t **out)
{
    *out = NULL;
    db->ledger = opts != NULL && opts->ledger != NULL ? *opts->ledger : (mem_ledger_t){NULL, NULL};
    db->cancel = opts != NULL ? opts->cancel : NULL;
    database_exec_status_t status = exec_statement(db, opts, sql, size, binding, out);
    if (status == EXEC_PENDING || status == DATABASE_EXEC_NOMEM) status = materialize(db, status, out);
    db->ledger = (mem_ledger_t){NULL, NULL};
    db->cancel = NULL;
    return status;
}

database_exec_status_t database_exec_ex(database_t *db, const database_exec_options_t *opts,
                                        const uint8_t *sql, size_t size,
                                        const lexer_binding_t *binding, result_t **out)
{
    if (db == NULL || out == NULL || (size > 0 && sql == NULL)) return DATABASE_EXEC_INVALID;
    *out = NULL;
    if (db->layer != NULL && db->layer->exec != NULL) {
        return db->layer->exec(db->layer_ctx, db, opts, sql, size, binding, out);
    }
    return run_statement(db, opts, sql, size, binding, out);
}

database_exec_status_t database_exec_read_only(database_t *db, const database_exec_options_t *opts,
                                               const uint8_t *sql, size_t size,
                                               const lexer_binding_t *binding, result_t **out)
{
    if (db == NULL || out == NULL || (size > 0 && sql == NULL)) return DATABASE_EXEC_INVALID;
    db->read_only_call = true;
    database_exec_status_t status = run_statement(db, opts, sql, size, binding, out);
    db->read_only_call = false;
    return status;
}

void database_set_control_sink(database_t *db, database_control_t *sink)
{
    if (db == NULL) return;
    db->control_sink = sink;
    if (sink != NULL) *sink = (database_control_t){DATABASE_CONTROL_NONE};
}

database_exec_status_t database_command_result(database_t *db, const database_exec_options_t *opts, const char *tag,
                                               result_t **out)
{
    if (db == NULL || out == NULL || tag == NULL) return DATABASE_EXEC_INVALID;
    *out = NULL;
    db->ledger = opts != NULL && opts->ledger != NULL ? *opts->ledger : (mem_ledger_t){NULL, NULL};
    result_status_t opened = open_command(db, tag, 0, out);
    db->ledger = (mem_ledger_t){NULL, NULL};
    if (opened == RESULT_NOMEM) return DATABASE_EXEC_NOMEM;
    return opened == RESULT_OK ? DATABASE_EXEC_OK : DATABASE_EXEC_INVALID;
}

void *database_layer_ctx(const database_t *db) { return db != NULL ? db->layer_ctx : NULL; }

database_exec_status_t database_error_result(database_t *db, const database_exec_options_t *opts,
                                             result_error_class_t error_class, int code, const char *message,
                                             result_t **out)
{
    if (db == NULL || out == NULL || message == NULL) return DATABASE_EXEC_INVALID;
    *out = NULL;
    db->ledger = opts != NULL && opts->ledger != NULL ? *opts->ledger : (mem_ledger_t){NULL, NULL};
    fail_result(db, error_class, code, "%s", message);
    database_exec_status_t status = materialize(db, EXEC_PENDING, out);
    db->ledger = (mem_ledger_t){NULL, NULL};
    return status;
}

void database_poison(database_t *db)
{
    if (db != NULL) db->poisoned = true;
}

database_exec_status_t database_exec_in_work(database_t *db, const database_exec_options_t *opts,
                                             const uint8_t *sql, size_t size,
                                             const lexer_binding_t *binding, result_t **out)
{
    if (db == NULL || out == NULL || (size > 0 && sql == NULL) || !db->work.active) return DATABASE_EXEC_INVALID;
    db->work.inside = true;
    database_exec_status_t status = run_statement(db, opts, sql, size, binding, out);
    db->work.inside = false;
    bool failed = (status != DATABASE_EXEC_OK && status != DATABASE_EXEC_CONTROL) ||
                  (*out != NULL && result_kind(*out) == RESULT_KIND_ERROR);
    if (failed) db->work.failed = true;
    return status;
}

database_exec_status_t database_exec(database_t *db, const uint8_t *sql, size_t size,
                                     const lexer_binding_t *binding, result_t **out)
{
    return database_exec_ex(db, NULL, sql, size, binding, out);
}

/* ── 쓰기 작업 ── */

database_status_t database_set_layer(database_t *db, const database_layer_t *layer, void *ctx)
{
    if (db == NULL || layer == NULL || db->layer != NULL) return DATABASE_INVALID;
    db->layer = layer;
    db->layer_ctx = ctx;
    return DATABASE_OK;
}

page_file_t *database_page_file(database_t *db) { return db != NULL ? db->file : NULL; }

database_status_t database_work_begin(database_t *db)
{
    if (db == NULL || !db->writable || db->work.active) return DATABASE_INVALID;
    if (db->poisoned) return DATABASE_IO;
    db->work = (work_state_t){0};
    db->work.active = true;
    return DATABASE_OK;
}

bool database_work_active(const database_t *db) { return db != NULL && db->work.active; }
bool database_work_failed(const database_t *db) { return db != NULL && db->work.failed; }
bool database_work_dirty(const database_t *db) { return db != NULL && db->work.cat != NULL; }

database_work_result_t database_work_commit(database_t *db)
{
    database_work_result_t result = {DATABASE_WORK_NOT_APPLIED, false, EINVAL};
    if (db == NULL || !db->work.active || db->work.failed) return result;
    if (db->poisoned) {
        result.saved_errno = EIO;
        return result;
    }
    if (db->work.cat == NULL) {
        result.outcome = DATABASE_WORK_APPLIED;
        result.saved_errno = 0;
        return result;
    }
    size_t encoded = catalog_encoded_bytes(db->work.cat);
    size_t write_peak = encoded + overflow_write_bound(encoded);
    size_t free_peak = overflow_free_bound(catalog_encoded_bytes(db->catalog));
    size_t save_peak = write_peak > free_peak ? write_peak : free_peak;
    if (!mem_reserve(&db->resident, MEM_SCOPE_RESIDENT, save_peak)) {
        result.nomem = true;
        result.saved_errno = ENOMEM;
        return result;
    }
    /* 게시된 상태가 가리키던 페이지는 commit 전에 놓아 그 commit의 빈 목록에 함께 적는다(실패하면 되돌려진다) */
    for (size_t i = 0; i < db->work.old_committed.len; i++) page_file_release(db->file, db->work.old_committed.items[i]);
    catalog_store_status_t saved = catalog_store_save(db->file, db->work.cat);
    result.saved_errno = errno;
    mem_release(&db->resident, MEM_SCOPE_RESIDENT, save_peak);
    if (saved == CATALOG_STORE_OK || saved == CATALOG_STORE_UNRECLAIMED) {
        result.outcome = DATABASE_WORK_APPLIED;
        return result;
    }
    if (saved == CATALOG_STORE_OUTCOME_UNKNOWN) {
        db->poisoned = true;
        result.outcome = DATABASE_WORK_UNKNOWN;
        return result;
    }
    if (page_file_is_poisoned(db->file)) db->poisoned = true;
    result.nomem = saved == CATALOG_STORE_NOMEM;
    return result;
}

static void work_reset(database_t *db)
{
    mem_release(&db->resident, MEM_SCOPE_RESIDENT, db->work.res_cat + db->work.res_lists);
    list_free(&db->work.old_committed);
    db->work = (work_state_t){0};
}

void database_work_end(database_t *db, database_work_outcome_t outcome)
{
    if (db == NULL || !db->work.active) return;
    if (outcome == DATABASE_WORK_APPLIED) {
        if (db->work.cat != NULL) {
            catalog_close(db->catalog);
            db->catalog = db->work.cat;
            db->work.cat = NULL;
            /* 작업 카탈로그의 몫이 게시된 카탈로그의 몫이 된다 */
            mem_release(&db->resident, MEM_SCOPE_RESIDENT, db->res_catalog);
            db->res_catalog = db->work.res_cat;
            db->work.res_cat = 0;
        }
    } else if (outcome == DATABASE_WORK_NOT_APPLIED) {
        /* 작업이 만든 페이지는 어느 제어 페이지도 가리킨 적 없어 곧바로 다시 쓸 수 있다 */
        page_file_release_fresh(db->file);
    } else {
        db->poisoned = true;
    }
    catalog_close(db->work.cat);
    work_reset(db);
}
