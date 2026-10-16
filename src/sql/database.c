/*
 * database.c — 데이터베이스 핸들의 열기·닫기, 메모리 계상, 작업 카탈로그, 게시, 문장 실행 진입점과 쓰기 작업.
 */
#include "database_exec.h"

/* 문장 수준 오류. 정리를 모두 마친 뒤 진입점이 결과로 만든다(그 결과를 만들 메모리를 먼저 돌려주기 위해). */
#define EXEC_PENDING ((database_exec_status_t)100)

/* value_cast가 숫자를 문자열로 바꿀 때(INTEGER 20바이트, REAL 32바이트 이하)와 소수점 변환의 여유 */
#define CAST_SLACK 64u

/* 열기 admission: 잠근 fd의 크기와 제어 페이지가 약속한 카탈로그 크기를 소비해 할당 전에 reserve한다. */
typedef struct {
    const mem_ledger_t *resident;
    const char *path;
    size_t admitted;
} admission_t;

typedef struct {
    size_t catalog_bytes;  /* 게시되면 상주할 카탈로그 몫 */
    size_t lists_bytes;    /* 게시 뒤 해제 목록 몫의 상한(줄이지 않는다) */
    size_t extra;          /* 이 계획이 새로 reserve한 몫 */
} resident_plan_t;

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

static size_t overflow_write_bound(size_t size_bytes);

static const lexer_limits_t LEXER_LIMITS = {
    (size_t)16 << 20, (size_t)16 << 20, (size_t)1 << 20, LEXER_PARAMETER_CEILING
};

/* ── 계상 보조 ── */

bool q_reserve(database_t *db, size_t bytes)
{
    return mem_reserve(&db->ledger, MEM_SCOPE_QUERY, bytes);
}

void q_release(database_t *db, size_t bytes)
{
    mem_release(&db->ledger, MEM_SCOPE_QUERY, bytes);
}

void *q_malloc(database_t *db, size_t bytes)
{
    if (!q_reserve(db, bytes)) return NULL;
    void *block = malloc(bytes);
    if (block == NULL) q_release(db, bytes);
    return block;
}

void *q_calloc(database_t *db, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    if (!q_reserve(db, bytes)) return NULL;
    void *block = calloc(count, size);
    if (block == NULL) q_release(db, bytes);
    return block;
}

/* block이 NULL이 아니면 해제하고 reserve했던 bytes를 돌려준다. */
void q_free(database_t *db, void *block, size_t bytes)
{
    if (block == NULL) return;
    free(block);
    q_release(db, bytes);
}

bool cancel_requested(const database_t *db)
{
    return db->cancel != NULL && db->cancel->requested != NULL && db->cancel->requested(db->cancel->ctx);
}

size_t value_bytes(const value_t *value)
{
    return value->view.type == VALUE_TEXT || value->view.type == VALUE_BLOB ? value->view.size_bytes : 0;
}

/* database.c가 계상해 가진 값들을 비우고 그 바이트를 돌려준다. */
void owned_clear(database_t *db, value_t *cells, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        size_t bytes = value_bytes(&cells[i]);
        value_clear(&cells[i]);
        q_release(db, bytes);  /* 비운 뒤에 몫을 돌려준다 */
    }
}

value_status_t q_cast(database_t *db, const value_view_t *source, value_type_t type, value_t *out)
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

value_status_t q_clone(database_t *db, const value_view_t *source, value_t *out)
{
    if (!q_reserve(db, source->size_bytes)) return VALUE_NOMEM;
    value_status_t status = value_clone(source, out);
    if (status != VALUE_OK) q_release(db, source->size_bytes);
    return status;
}

/* ── 작업 카탈로그와 페이지 목록 ── */

/* 지금 문장이 읽고 고칠 카탈로그. 작업 안의 문장이면 작업 카탈로그, 아니면 게시된 카탈로그 */
const catalog_t *base_catalog(const database_t *db)
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
        case PAGE_FILE_FORKED: return DATABASE_FORKED;
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

/* 열기·생성이 쥐고 돌려준 잠금을 놓는다. 이후 잠금은 문장·작업 동안만 쥐며 기다리지 않는다(#199) */
static void release_open_lock(page_file_t *file)
{
    page_file_set_lock_timeout(file, 0);
    page_file_unlock(file);
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
    if (status == DATABASE_OK) release_open_lock(db->file);
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
    release_open_lock(db->file);
    *out = db;
    return DATABASE_OK;
}

database_status_t database_open(const char *path, bool writable, database_t **out)
{
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    return open_database(path, writable, NULL, out);
}

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

database_exec_status_t fail_result(database_t *db, result_error_class_t error_class, int code,
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

int name_len(size_t size) { return (int)(size < 64 ? size : 64); }

static bool catalog_name_equals(const char *name, size_t size, const sqlstmt_name_t *other)
{
    return size == other->size && (size == 0 || memcmp(name, other->bytes, size) == 0);
}

/* 저장소 오류를 결과로 바꾼다. NOMEM은 결과가 아니라 NOMEM이다. */
database_exec_status_t heap_failure(database_t *db, heap_status_t status, const char *where)
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

database_exec_status_t cancelled_result(database_t *db)
{
    return fail_result(db, RESULT_E_CANCELLED, DATABASE_CODE_CANCELLED, "문장이 취소되어 아무것도 반영하지 않았습니다");
}

/* ── 공통 보조 ── */

void clear_cells(value_t *cells, size_t count)
{
    for (size_t i = 0; i < count; i++) value_clear(&cells[i]);
}

bool lookup_table(database_t *db, const sqlstmt_name_t *name, catalog_table_id_t *id,
                         catalog_table_view_t *view, database_exec_status_t *status)
{
    const catalog_t *catalog = base_catalog(db);
    catalog_status_t found = catalog_find(catalog, (const char *)name->bytes, name->size, id);
    if (found == CATALOG_OK && catalog_describe(catalog, *id, view) == CATALOG_OK) return true;
    *status = fail_result(db, RESULT_E_SEMANTIC, 1, "테이블이 없습니다: %.*s", name_len(name->size),
                          (const char *)name->bytes);
    return false;
}

size_t find_column(const catalog_table_view_t *view, const sqlstmt_name_t *name)
{
    for (size_t i = 0; i < view->column_count; i++) {
        if (catalog_name_equals(view->columns[i].name, view->columns[i].name_bytes, name)) return i;
    }
    return SIZE_MAX;
}

database_exec_status_t unknown_column(database_t *db, const sqlstmt_name_t *name)
{
    return fail_result(db, RESULT_E_SEMANTIC, 2, "컬럼이 없습니다: %.*s", name_len(name->size),
                       (const char *)name->bytes);
}

heap_status_t open_heap(database_t *db, const catalog_table_view_t *view,
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

heap_status_t configure_secondary(heap_batch_t *batch, const catalog_t *catalog,
                                         catalog_table_id_t table_id)
{
    size_t count = catalog_index_count(catalog, table_id);
    if (count > CATALOG_MAX_INDEXES) return HEAP_CORRUPT;
    heap_secondary_spec_t specs[CATALOG_MAX_INDEXES];
    for (size_t i = 0; i < count; i++) {
        catalog_index_view_t view;
        if (catalog_index_at(catalog, table_id, i, &view) != CATALOG_OK) return HEAP_CORRUPT;
        specs[i] = (heap_secondary_spec_t){view.column, view.unique, view.ordered, view.root};
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

void work_close(database_t *db, work_t *work)
{
    if (work->cat == NULL) return;
    catalog_close(work->cat);
    q_release(db, work->charged);
    *work = (work_t){NULL, 0};
}

/* 게시된 카탈로그를 복제한다. 복제 동안의 순간 최대치를 먼저 reserve하고 끝나면 복제본 몫만 남긴다. */
bool work_clone(database_t *db, work_t *work)
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
void work_settle(database_t *db, work_t *work, size_t peak)
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

database_exec_status_t publish(database_t *db, work_t *work, catalog_table_id_t id,
                                      heap_batch_t *batch,
                                      const uint32_t *drop_pages, size_t drop_count,
                                      result_t *success, result_t **out)
{
    database_exec_status_t status = publish_inner(db, work, id, batch, drop_pages,
                                                  drop_count, success, out);
    heap_batch_free(batch);
    return status;
}

result_status_t open_command(database_t *db, const char *tag, uint64_t affected, result_t **result)
{
    result_command_t command = {affected, tag, strlen(tag)};
    return result_open_command_ex(&command, &db->ledger, result);
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

/* ── 여러 프로세스 잠금(#199) ── */

/*
 * 다른 핸들이 게시한 카탈로그를 다시 읽는다. 상주 몫은 새 카탈로그와 늘어난 해제 목록 상한으로 맞춘다. 실패하면
 * 게시된 카탈로그와 몫은 그대로다.
 */
static database_status_t reload_catalog(database_t *db)
{
    size_t lists = page_file_list_peak_bytes(page_file_control(db->file)->page_count);
    size_t more_lists = lists > db->res_lists ? lists - db->res_lists : 0;
    if (!mem_reserve(&db->resident, MEM_SCOPE_RESIDENT, more_lists)) return DATABASE_NOMEM;
    bool bounded = mem_ledger_enabled(&db->resident);
    admission_t admission = {&db->resident, NULL, 0};
    catalog_t *fresh = NULL;
    catalog_store_status_t loaded =
        catalog_store_load_admitted(db->file, bounded ? admit_catalog : NULL, &admission, &fresh);
    if (loaded != CATALOG_STORE_OK) {
        int saved = errno;
        mem_release(&db->resident, MEM_SCOPE_RESIDENT, more_lists + admission.admitted);
        errno = saved;
        return loaded == CATALOG_STORE_EMPTY ? DATABASE_CORRUPT : from_store(loaded);
    }
    size_t exact = bounded ? catalog_memory_bytes(fresh) : 0;
    assert(exact <= admission.admitted);
    mem_release(&db->resident, MEM_SCOPE_RESIDENT, admission.admitted - exact + db->res_catalog);
    catalog_close(db->catalog);
    db->catalog = fresh;
    db->res_catalog = exact;
    db->res_lists += more_lists;
    return DATABASE_OK;
}

database_status_t database_lock_file(database_t *db, page_file_lock_t mode)
{
    bool changed = false;
    page_file_status_t locked = page_file_lock(db->file, mode, &changed);
    if (locked != PAGE_FILE_OK) return from_page_file(locked);
    if (!changed) return DATABASE_OK;
    database_status_t status = reload_catalog(db);
    if (status != DATABASE_OK) page_file_unlock(db->file);
    return status;
}

/* 문장 앞에서 잠금을 얻지 못한 결과. 실행 전 거절이라 반영된 것이 없다 */
static database_exec_status_t lock_failure(database_t *db, database_status_t status)
{
    switch (status) {
        case DATABASE_LOCKED:
            return fail_result(db, RESULT_E_BUSY, DATABASE_CODE_FILE_LOCKED, "다른 연결이 파일을 쓰는 중입니다");
        case DATABASE_FORKED:
            return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_FORKED,
                               "fork한 자식 프로세스는 부모가 연 핸들을 쓸 수 없습니다. 자식에서 파일을 새로 여세요");
        case DATABASE_NOMEM: return DATABASE_EXEC_NOMEM;
        case DATABASE_CORRUPT: return fail_result(db, RESULT_E_IO, 35, "다른 연결이 게시한 상태를 읽지 못했습니다(손상)");
        default: return fail_result(db, RESULT_E_IO, 36, "다른 연결이 게시한 상태를 읽지 못했습니다(입출력)");
    }
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
    /* 작업 밖 문장은 문장 동안 잠금을 쥔다. 작업은 시작할 때 배타 잠금을 잡았다 */
    bool locked_here = page_file_lock_mode(db->file) == PAGE_FILE_LOCK_NONE;
    if (locked_here) {
        page_file_lock_t mode = statement->kind == SQLSTMT_SELECT ? PAGE_FILE_LOCK_SHARED : PAGE_FILE_LOCK_EXCLUSIVE;
        database_status_t locked = database_lock_file(db, mode);
        if (locked != DATABASE_OK) {
            status = lock_failure(db, locked);
            goto done;
        }
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
    if (locked_here) page_file_unlock(db->file);
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
    /* 작업이 끝날 때까지 배타 잠금을 쥔다. 다른 핸들의 쓰기는 그동안 시작하지 못한다(#199) */
    database_status_t locked = database_lock_file(db, PAGE_FILE_LOCK_EXCLUSIVE);
    if (locked != DATABASE_OK) return locked;
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
    page_file_unlock(db->file);
}
