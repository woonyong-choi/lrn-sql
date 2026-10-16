/*
 * database_check.c — 페이지 회계 검사(database_check_pages).
 */
#include "database_exec.h"

typedef struct {
    uint8_t *seen;      /* 페이지마다 닿은 횟수(255에서 멈춘다) */
    uint32_t count;     /* page_count */
    uint64_t outside;   /* 데이터 페이지 범위 밖을 가리킨 수 */
} page_marks_t;

/* 체인 순회: 읽기는 page_file 어댑터로 넘기고 release는 페이지를 기록만 한다 */
typedef struct {
    overflow_io_t inner;
    page_marks_t *marks;
} chain_walk_t;

/* ── 페이지 회계 검사 ── */

static void mark_page(page_marks_t *marks, uint32_t page)
{
    if (page < PAGE_FILE_CONTROL_PAGES || page >= marks->count) marks->outside++;
    else if (marks->seen[page] < UINT8_MAX) marks->seen[page]++;
}

static bool walk_read(void *context, uint32_t page, uint8_t *bytes)
{
    chain_walk_t *walk = context;
    return walk->inner.read(walk->inner.context, page, bytes);
}

/* 순회는 페이지를 받거나 쓰지 않는다. 불리면 실패로 끝낸다 */
static bool walk_allocate(void *context, uint32_t *page)
{
    (void)context;
    (void)page;
    return false;
}

static bool walk_write(void *context, uint32_t page, const uint8_t *bytes)
{
    (void)context;
    (void)page;
    (void)bytes;
    return false;
}

static bool walk_release(void *context, uint32_t page)
{
    chain_walk_t *walk = context;
    mark_page(walk->marks, page);
    return true;
}

static database_status_t from_heap(heap_status_t status)
{
    if (status == HEAP_OK) return DATABASE_OK;
    if (status == HEAP_NOMEM) return DATABASE_NOMEM;
    if (status == HEAP_IO) return DATABASE_IO;
    return DATABASE_CORRUPT;
}

/* 카탈로그 overflow 체인의 페이지. overflow_free의 체인 검증·순회를 해제 없이 빌린다 */
static database_status_t mark_catalog_chain(database_t *db, page_marks_t *marks)
{
    const page_file_control_t *control = page_file_control(db->file);
    if (control->catalog_head == PAGE_FILE_NO_PAGE) return DATABASE_OK;
    page_file_overflow_ctx_t ctx;
    chain_walk_t walk = {page_file_overflow_io(&ctx, db->file, 0), marks};
    overflow_io_t io = {&walk, PAGE_BODY_BYTES, walk_allocate, walk_release, walk_read, walk_write};
    overflow_status_t walked = overflow_free(&io, (overflow_ref_t){control->catalog_head, control->catalog_size});
    if (walked == OVERFLOW_OK) return DATABASE_OK;
    return walked == OVERFLOW_NOMEM ? DATABASE_NOMEM : walked == OVERFLOW_IO ? DATABASE_IO : DATABASE_CORRUPT;
}

/* 테이블 하나의 heap·overflow·기본 키·보조 색인 페이지 */
static database_status_t mark_table(database_t *db, catalog_table_id_t id, page_marks_t *marks)
{
    catalog_table_view_t view;
    if (catalog_describe(db->catalog, id, &view) != CATALOG_OK) return DATABASE_CORRUPT;
    size_t index_count = catalog_index_count(db->catalog, id);
    if (index_count > CATALOG_MAX_INDEXES) return DATABASE_CORRUPT;
    uint32_t roots[CATALOG_MAX_INDEXES];
    for (size_t i = 0; i < index_count; i++) {
        catalog_index_view_t index;
        if (catalog_index_at(db->catalog, id, i, &index) != CATALOG_OK) return DATABASE_CORRUPT;
        roots[i] = index.root;
    }
    heap_t *heap = NULL;
    heap_status_t status = open_heap(db, &view, id, &heap);
    uint32_t *pages = NULL;
    size_t count = 0;
    if (status == HEAP_OK) status = heap_collect_pages_with_indexes(heap, roots, index_count, &pages, &count);
    for (size_t i = 0; status == HEAP_OK && i < count; i++) mark_page(marks, pages[i]);
    if (heap != NULL) {
        heap_free_pages(heap, pages, count);
        heap_close(heap);
    }
    return from_heap(status);
}

// cost: time O(p + c·f), heap O(p), stack O(1), io O(p)
// vars: p = 데이터 페이지 수, c = 빈 목록 사슬 페이지 수, f = 빈 페이지 수
// basis: estimate
database_status_t database_check_pages(database_t *db, database_page_check_t *out)
{
    if (db == NULL || out == NULL || !db->writable || db->work.active) return DATABASE_INVALID;
    *out = (database_page_check_t){0};
    /* 빈 목록과 도달 페이지를 같은 게시 상태로 보도록 검사 동안 배타 잠금을 쥔다(#199) */
    database_status_t locked = database_lock_file(db, PAGE_FILE_LOCK_EXCLUSIVE);
    if (locked != DATABASE_OK) return locked;
    uint32_t count = page_file_control(db->file)->page_count;
    page_marks_t marks = {calloc(count > 0 ? count : 1, 1), count, 0};
    if (marks.seen == NULL) {
        page_file_unlock(db->file);
        return DATABASE_NOMEM;
    }
    database_status_t status = mark_catalog_chain(db, &marks);
    for (size_t t = 0; status == DATABASE_OK && t < catalog_table_count(db->catalog); t++) {
        catalog_table_view_t view;
        if (catalog_table_at(db->catalog, t, &view) != CATALOG_OK) status = DATABASE_CORRUPT;
        else status = mark_table(db, view.id, &marks);
    }
    for (uint32_t page = PAGE_FILE_CONTROL_PAGES; status == DATABASE_OK && page < count; page++) {
        page_file_role_t role;
        if (page_file_page_role(db->file, page, &role) != PAGE_FILE_OK) {
            status = DATABASE_CORRUPT;
            break;
        }
        uint8_t seen = marks.seen[page];
        out->data_pages++;
        if (role != PAGE_FILE_ROLE_IN_USE) {
            out->free++;
            if (seen > 0) out->conflicts++;
        } else if (seen == 0) {
            out->orphaned++;
        } else {
            out->reachable++;
            if (seen > 1) out->conflicts++;
        }
    }
    out->conflicts += marks.outside;
    free(marks.seen);
    page_file_unlock(db->file);
    if (status != DATABASE_OK) *out = (database_page_check_t){0};
    return status;
}
