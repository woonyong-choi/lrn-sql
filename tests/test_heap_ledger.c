#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alloc_audit.h"
#include "sql/mem_ledger.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/page_file.h"

/*
 * heap_store의 메모리 회계 순서 검사. heap_store.c만 -Dmalloc=audit_malloc -Dcalloc=audit_calloc
 * -Drealloc=audit_realloc -Dfree=audit_free로 빌드해(치환하지 않으면 시작할 때 실패) 정확한 추적 ledger와 맞춘다.
 * 이 ledger는 heap_store.c의 할당만 계상하므로 정리 경로에서 "살아 있는 요청 바이트 <= 장부 사용량"이 반환
 * 직후에도 맞아야 한다. 어긋나면 몫을 메모리 해제보다 먼저 돌려준 것이다. 세션 audit은 database 상주 몫의
 * 여유에 가려지는 작은 구조체(해제 목록, batch 배열, 페이지 목록)를 이 검사가 따로 본다.
 * heap_store가 해제하는 record_encode의 버퍼는 record.c가 할당하므로 record.c도 같은 치환으로 빌드한다.
 * 확인하는 것: batch 정상 게시·abort·free, cursor 순회와 중간 close, heap_collect_pages와 heap_free_pages,
 * n번째 reserve 실패에서의 정리. 이 검사는 치환한 heap_store.c가 실행한 경로에 대한 것이다.
 */

#define DIR_NAME "build/test_heap_ledger_dir"
#define DB_PATH DIR_NAME "/db.totedb"
#define COLUMNS 3

static int g_run, g_fail;
#define CHECK(cond, ...)                              \
    do {                                              \
        g_run++;                                      \
        if (!(cond)) {                                \
            g_fail++;                                 \
            if (g_fail <= 40) {                       \
                printf("  FAIL (line %d): ", __LINE__); \
                printf(__VA_ARGS__);                  \
                printf("\n");                         \
            }                                         \
        }                                             \
    } while (0)

typedef struct {
    size_t used;
    long reserves;
    long fail_at;
    unsigned long violations;
} state_t;

static state_t g;

static bool l_reserve(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    state_t *s = ctx;
    if (s->fail_at != 0 && ++s->reserves == s->fail_at) return false;
    if (s->fail_at == 0) s->reserves++;
    s->used += bytes;
    return true;
}

static void l_release(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    state_t *s = ctx;
    assert(bytes <= s->used);
    s->used -= bytes;
    if (audit_live_now() > s->used) s->violations++;  /* 해제 전에 돌려줬다 */
}

static const mem_ledger_ops_t OPS = {l_reserve, l_release, NULL, NULL};

static void on_alloc(void *ctx, size_t request, size_t live)
{
    (void)request;
    state_t *s = ctx;
    if (live > s->used) s->violations++;  /* reserve보다 앞선 할당 */
}

static void begin(void)
{
    memset(&g, 0, sizeof(g));
    audit_reset();
    audit_on_alloc = on_alloc;
    audit_ctx = &g;
}

static void finish_case(const char *what)
{
    CHECK(g.used == 0, "%s: 장부에 남은 몫 %zu", what, g.used);
    CHECK(audit_live_now() == 0, "%s: 살아 있는 할당 %zu", what, audit_live_now());
    CHECK(g.violations == 0, "%s: 반환·할당 순서 위반 %lu", what, g.violations);
    audit_on_alloc = NULL;
}

static void clean_dir(void)
{
    DIR *dir = opendir(DIR_NAME);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", DIR_NAME, entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(DIR_NAME);
}

static const value_type_t TYPES[COLUMNS] = {VALUE_INTEGER, VALUE_TEXT, VALUE_BLOB};

typedef struct {
    page_file_t *file;
    catalog_t *catalog;
    catalog_table_id_t tid;
} base_t;

/* ledger 없이 파일에 행을 커밋해 두고 닫는다. ledger 시험은 그 파일을 다시 연다 */
static void build_committed(size_t rows)
{
    clean_dir();
    mkdir("build", 0755);
    assert(mkdir(DIR_NAME, 0700) == 0);
    base_t b = {0};
    assert(page_file_create(DB_PATH, &b.file) == PAGE_FILE_OK);
    assert(catalog_open_empty(&b.catalog) == CATALOG_OK);
    catalog_column_spec_t cols[COLUMNS] = {{"id", 2, VALUE_INTEGER, false, NULL}, {"t", 1, VALUE_TEXT, false, NULL}, {"b", 1, VALUE_BLOB, false, NULL}};
    catalog_table_spec_t spec = {"items", 5, cols, COLUMNS, CATALOG_NO_COLUMN, false};
    assert(catalog_create_table(b.catalog, &spec, &b.tid) == CATALOG_OK);
    assert(catalog_store_save(b.file, b.catalog) == CATALOG_STORE_OK);
    assert(page_file_publish(b.file) == PAGE_FILE_OK);
    heap_t *heap;
    heap_schema_t schema = {TYPES, COLUMNS};
    assert(heap_open(b.file, b.tid, 0, &schema, &heap) == HEAP_OK);
    heap_batch_t *batch;
    assert(heap_batch_begin(heap, &batch) == HEAP_OK);
    uint8_t blob[2000];
    memset(blob, 7, sizeof(blob));
    char text[64];
    for (size_t i = 0; i < rows; i++) {
        snprintf(text, sizeof(text), "row %zu with some text", i);
        value_view_t v[COLUMNS] = {{VALUE_INTEGER, (int64_t)i, 0, NULL, 0},
                                   {VALUE_TEXT, 0, 0, (const uint8_t *)text, strlen(text)},
                                   {VALUE_BLOB, 0, 0, blob, i % 7 == 0 ? sizeof(blob) : 10}};
        heap_locator_t loc;
        assert(heap_batch_insert(batch, v, COLUMNS, &loc) == HEAP_OK);
    }
    heap_batch_result_t result;
    assert(heap_batch_finish(batch, &result) == HEAP_OK);
    assert(catalog_set_locators(b.catalog, b.tid, result.new_root, 0) == CATALOG_OK);
    assert(catalog_store_save(b.file, b.catalog) == CATALOG_STORE_OK);
    assert(heap_batch_release_old(batch) == HEAP_OK);
    heap_batch_free(batch);
    heap_close(heap);
    catalog_close(b.catalog);
    page_file_close(b.file);
}

static void open_base(base_t *b, uint32_t *root)
{
    memset(b, 0, sizeof(*b));
    assert(page_file_open(DB_PATH, true, &b->file, NULL) == PAGE_FILE_OK);
    assert(catalog_store_load(b->file, &b->catalog) == CATALOG_STORE_OK);
    assert(catalog_find(b->catalog, "items", 5, &b->tid) == CATALOG_OK);
    catalog_table_view_t view;
    assert(catalog_describe(b->catalog, b->tid, &view) == CATALOG_OK);
    *root = view.heap_root;
}

static void close_base(base_t *b)
{
    catalog_close(b->catalog);
    page_file_close(b->file);
}

static void test_cursor_and_pages(void)
{
    printf("cursor 순회·중간 close, collect_pages와 free_pages\n");
    build_committed(60);
    for (int mode = 0; mode < 3; mode++) {
        base_t b;
        uint32_t root;
        open_base(&b, &root);
        begin();
        mem_ledger_t ledger = {&OPS, &g};
        heap_t *heap;
        heap_schema_t schema = {TYPES, COLUMNS};
        assert(heap_open_ex(b.file, b.tid, root, &schema, &ledger, &heap) == HEAP_OK);
        char label[32];
        snprintf(label, sizeof(label), "mode %d", mode);
        if (mode < 2) {
            heap_cursor_t *cursor;
            assert(heap_cursor_open(heap, &cursor) == HEAP_OK);
            size_t seen = 0;
            for (;;) {
                value_t row[COLUMNS] = {0};
                heap_locator_t loc;
                heap_status_t st = heap_cursor_next(cursor, row, &loc);
                if (st != HEAP_OK) break;
                for (int c = 0; c < COLUMNS; c++) value_clear(&row[c]);
                seen++;
                if (mode == 1 && seen == 25) break;  /* 중간에 닫는다 */
            }
            CHECK(mode == 1 ? seen == 25 : seen == 60, "%s: 순회 %zu행", label, seen);
            heap_cursor_close(cursor);
        } else {
            uint32_t *pages = NULL;
            size_t count = 0;
            assert(heap_collect_pages(heap, &pages, &count) == HEAP_OK);
            CHECK(count > 0, "%s: 모은 페이지", label);
            heap_free_pages(heap, pages, count);
        }
        heap_close(heap);
        close_base(&b);
        finish_case(label);
    }
    clean_dir();
}

static void test_batch_paths(void)
{
    printf("batch: 게시 전 free, abort 뒤 free, n번째 reserve 실패\n");
    build_committed(30);
    long points = 0;
    for (int mode = 0; mode < 2; mode++) {
        for (long n = 0; n < 400; n++) {
            base_t b;
            uint32_t root;
            open_base(&b, &root);
            begin();
            mem_ledger_t ledger = {&OPS, &g};
            heap_t *heap;
            heap_schema_t schema = {TYPES, COLUMNS};
            assert(heap_open_ex(b.file, b.tid, root, &schema, &ledger, &heap) == HEAP_OK);
            g.reserves = 0;
            g.fail_at = n;  /* 0이면 주입 없음 */
            heap_batch_t *batch = NULL;
            bool begun = heap_batch_begin(heap, &batch) == HEAP_OK;
            bool all_ok = begun;
            uint8_t blob[3000];
            memset(blob, 3, sizeof(blob));
            for (int i = 0; begun && i < 25; i++) {
                char text[64];
                snprintf(text, sizeof(text), "added %d", i);
                value_view_t v[COLUMNS] = {{VALUE_INTEGER, 1000 + i, 0, NULL, 0},
                                           {VALUE_TEXT, 0, 0, (const uint8_t *)text, strlen(text)},
                                           {VALUE_BLOB, 0, 0, blob, i % 5 == 0 ? sizeof(blob) : 8}};
                heap_locator_t loc;
                if (heap_batch_insert(batch, v, COLUMNS, &loc) != HEAP_OK) {
                    all_ok = false;
                    break;
                }
            }
            heap_batch_result_t result;
            if (all_ok && heap_batch_finish(batch, &result) != HEAP_OK) all_ok = false;
            bool hit = n != 0 && g.reserves >= n;
            g.fail_at = 0;
            if (batch != NULL) {
                if (mode == 1 || !all_ok) heap_batch_abort(batch);  /* 게시하지 않고 놓는다 */
                heap_batch_free(batch);
            }
            heap_close(heap);
            close_base(&b);
            char label[96];
            snprintf(label, sizeof(label), "batch mode %d reserve %ld", mode, n);
            finish_case(label);
            if (hit) points++;
            if (n != 0 && !hit) break;
        }
    }
    printf("  reserve 실패 주입 %ld곳\n", points);
    CHECK(points > 10, "주입 지점 %ld", points);
    clean_dir();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_cursor_and_pages();
    test_batch_paths();
    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
