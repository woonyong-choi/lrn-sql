#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/checksum.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/page_file.h"

/*
 * heap_store 검사. 합성 데이터만 쓰며 DB 파일은 build/ 아래 전용 디렉터리에 만든다.
 * heap_store.c는 -Dmalloc=hs_test_malloc -Dcalloc=hs_test_calloc -Drealloc=hs_test_realloc
 * 으로 빌드해 할당 실패를 주입한다. 치환 없이 빌드하면 시작할 때 실패한다.
 */

#define DIR_NAME "build/test_heap_store_dir"
#define DB_PATH DIR_NAME "/db.lrdb"
#define COLUMNS 5
#define ZERO_ROW {{{VALUE_NULL, 0, 0, NULL, 0}, false}}

static int g_run = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_run++;                                          \
        if (!(cond)) {                                    \
            g_fail++;                                     \
            printf("  FAIL (line %d): ", __LINE__);       \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ── 할당 실패 주입 ── */
static long g_alloc_fail_at;
static long g_alloc_calls;
static long g_alloc_total;

static int alloc_fails(void)
{
    g_alloc_total++;
    return g_alloc_fail_at != 0 && ++g_alloc_calls == g_alloc_fail_at;
}

void *hs_test_malloc(size_t n) { return alloc_fails() ? NULL : malloc(n); }
void *hs_test_calloc(size_t a, size_t b) { return alloc_fails() ? NULL : calloc(a, b); }
void *hs_test_realloc(void *p, size_t n) { return alloc_fails() ? NULL : realloc(p, n); }

/* ── I/O 실패 주입 ── */
typedef struct {
    int pwrite_fail_at, pwrite_calls;
    int pread_fail_at, pread_calls;
    int fsync_fail_at, fsync_calls;
} hooks_t;

static hooks_t g_hooks;

static ssize_t hook_pread(int fd, void *buf, size_t size, off_t offset)
{
    if (g_hooks.pread_fail_at != 0 && ++g_hooks.pread_calls == g_hooks.pread_fail_at) {
        errno = EIO;
        return -1;
    }
    return pread(fd, buf, size, offset);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (g_hooks.pwrite_fail_at != 0 && ++g_hooks.pwrite_calls == g_hooks.pwrite_fail_at) {
        errno = ENOSPC;
        return -1;
    }
    return pwrite(fd, buf, size, offset);
}

static int hook_fsync(int fd)
{
    if (g_hooks.fsync_fail_at != 0 && ++g_hooks.fsync_calls == g_hooks.fsync_fail_at) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static const page_file_io_t HOOKED = {hook_pread, hook_pwrite, hook_fsync, link, unlink};

/* ── 파일·데이터 보조 ── */
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

static void fresh_dir(void)
{
    clean_dir();
    assert(mkdir(DIR_NAME, 0700) == 0);
}

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)n + 1);
    assert(bytes != NULL && fread(bytes, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return bytes;
}

static void spit(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL && fwrite(bytes, 1, size, f) == size);
    fclose(f);
}

static uint64_t fnv(const uint8_t *bytes, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

static uint64_t file_hash(void)
{
    size_t size;
    uint8_t *bytes = slurp(DB_PATH, &size);
    uint64_t h = fnv(bytes, size);
    free(bytes);
    return h;
}

/* page_file이 쓰는 체크섬(CRC32C, 28..31 제외)을 다시 계산한다. */
static void reseal(uint8_t *page)
{
    uint32_t sum = crc32c(crc32c(0, page, 28), page + 32, PAGE_FILE_PAGE_BYTES - 32);
    for (unsigned i = 0; i < 4; i++) page[28 + i] = (uint8_t)(sum >> (i * 8));
}

/* ── 행 모델 ── */
static const value_type_t TYPES[COLUMNS] = {VALUE_INTEGER, VALUE_REAL, VALUE_TEXT, VALUE_BLOB, VALUE_TEXT};

typedef struct {
    bool present;
    value_t cells[COLUMNS];
} model_row_t;

#define MAX_MODEL 6000
static model_row_t g_model[MAX_MODEL];

static void clear_row(value_t *row)
{
    for (int i = 0; i < COLUMNS; i++) value_clear(&row[i]);
}

static size_t text_length(int variant)
{
    static const size_t lengths[] = {0, 1, 17, 100, 3800, 4001, 9000, 20000};
    return lengths[variant % 8];
}

/* seed로 결정되는 합성 행. id는 seed다. */
static void make_row(int seed, int variant, value_t *row)
{
    clear_row(row);
    assert(value_set_integer(&row[0], seed) == VALUE_OK);
    if (variant % 5 == 3) {
        assert(value_set_null(&row[1]) == VALUE_OK);
    } else if (variant % 7 == 2) {
        assert(value_set_real(&row[1], -0.0) == VALUE_OK);
    } else {
        assert(value_set_real(&row[1], seed * 0.25 + 1e-9) == VALUE_OK);
    }
    size_t len = text_length(variant);
    uint8_t *bytes = malloc(len + 1);
    assert(bytes != NULL);
    for (size_t i = 0; i < len; i++) bytes[i] = (uint8_t)('a' + (seed + (int)i) % 26);
    if (variant % 4 == 1 && len >= 3) {  /* 다바이트 UTF-8로 끝나도록 앞 3바이트를 U+20AC로 */
        bytes[0] = 0xE2;
        bytes[1] = 0x82;
        bytes[2] = 0xAC;
    }
    assert(value_set_text(&row[2], bytes, len) == VALUE_OK);
    for (size_t i = 0; i < len; i++) bytes[i] = (uint8_t)(i * 7 + (size_t)seed);
    if (variant % 6 == 1) {
        assert(value_set_null(&row[3]) == VALUE_OK);
    } else {
        if (len > 4) {
            bytes[1] = 0;  /* 내장 NUL */
        }
        assert(value_set_blob(&row[3], bytes, variant % 3 == 2 ? 0 : len) == VALUE_OK);
    }
    if (variant % 8 == 5) {
        assert(value_set_null(&row[4]) == VALUE_OK);
    } else {
        assert(value_set_text(&row[4], (const uint8_t *)"", 0) == VALUE_OK);  /* NULL과 구별되는 빈 TEXT */
    }
    free(bytes);
}

static bool rows_equal(const value_t *a, const value_t *b)
{
    for (int i = 0; i < COLUMNS; i++) {
        const value_view_t *x = &a[i].view, *y = &b[i].view;
        if (x->type != y->type) return false;
        int order = 1;
        if (value_compare(x, y, &order) != VALUE_OK || order != 0) return false;
        if (x->type == VALUE_REAL && signbit(x->real) != signbit(y->real)) return false;
        if ((x->type == VALUE_TEXT || x->type == VALUE_BLOB) && x->size_bytes != y->size_bytes) return false;
    }
    return true;
}

static void model_reset(void)
{
    for (int i = 0; i < MAX_MODEL; i++) {
        if (g_model[i].present) clear_row(g_model[i].cells);
        g_model[i].present = false;
    }
}

static void model_put(int id, int variant)
{
    assert(id >= 0 && id < MAX_MODEL);
    make_row(id, variant, g_model[id].cells);
    g_model[id].present = true;
}

static void to_views(const value_t *row, value_view_t *views)
{
    for (int i = 0; i < COLUMNS; i++) views[i] = value_as_view(&row[i]);
}

/* ── 고정 구성: page_file + catalog + 테이블 하나 + heap ── */
typedef struct {
    page_file_t *file;
    catalog_t *catalog;
    catalog_table_id_t tid;
    heap_t *heap;
    uint32_t new_root;
} fx_t;

static void fx_create(fx_t *fx)
{
    fresh_dir();
    memset(fx, 0, sizeof(*fx));
    assert(page_file_create(DB_PATH, &fx->file) == PAGE_FILE_OK);
    assert(catalog_open_empty(&fx->catalog) == CATALOG_OK);
    catalog_column_spec_t cols[COLUMNS] = {
        {"id", 2, VALUE_INTEGER, false, NULL}, {"r", 1, VALUE_REAL, false, NULL}, {"t", 1, VALUE_TEXT, false, NULL},
        {"b", 1, VALUE_BLOB, false, NULL},     {"t2", 2, VALUE_TEXT, false, NULL},
    };
    catalog_table_spec_t spec = {"items", 5, cols, COLUMNS, CATALOG_NO_COLUMN, false};
    assert(catalog_create_table(fx->catalog, &spec, &fx->tid) == CATALOG_OK);
    assert(catalog_store_save(fx->file, fx->catalog) == CATALOG_STORE_OK);
    assert(page_file_publish(fx->file) == PAGE_FILE_OK);
    heap_schema_t schema = {TYPES, COLUMNS};
    assert(heap_open(fx->file, fx->tid, 0, &schema, &fx->heap) == HEAP_OK);
}

static uint32_t catalog_heap_root(const catalog_t *catalog, catalog_table_id_t tid)
{
    catalog_table_view_t view;
    assert(catalog_describe(catalog, tid, &view) == CATALOG_OK);
    return view.heap_root;
}

static void fx_close(fx_t *fx)
{
    heap_close(fx->heap);
    catalog_close(fx->catalog);
    page_file_close(fx->file);
    memset(fx, 0, sizeof(*fx));
}

/* 디스크에 있는 파일을 열어 catalog가 가리키는 heap을 연다. */
static void fx_open_existing(fx_t *fx)
{
    assert(page_file_open(DB_PATH, true, &fx->file, NULL) == PAGE_FILE_OK);
    assert(catalog_store_load(fx->file, &fx->catalog) == CATALOG_STORE_OK);
    assert(catalog_find(fx->catalog, "items", 5, &fx->tid) == CATALOG_OK);
    heap_schema_t schema = {TYPES, COLUMNS};
    assert(heap_open(fx->file, fx->tid, catalog_heap_root(fx->catalog, fx->tid), &schema, &fx->heap) == HEAP_OK);
}

/* 닫았다가 다시 열어 catalog가 가리키는 heap을 연다. */
static void fx_reopen(fx_t *fx)
{
    fx_close(fx);
    fx_open_existing(fx);
}

/* finish가 끝난 batch의 옛 페이지를 놓고(commit의 빈 목록에 적힌다) catalog로 게시한다. 반영되지 않으면 catalog_store가 되돌린다. */
static catalog_store_status_t fx_save_finished(fx_t *fx, heap_batch_t *batch, const heap_batch_result_t *result)
{
    assert(catalog_set_locators(fx->catalog, fx->tid, result->new_root, 0) == CATALOG_OK);
    assert(heap_batch_release_old(batch) == HEAP_OK);
    catalog_store_status_t saved = catalog_store_save(fx->file, fx->catalog);
    fx->new_root = result->new_root;
    return saved;
}

/* finish -> 옛 페이지 release -> catalog 게시. 실패는 abort하지 않고 돌려준다. */
static catalog_store_status_t fx_publish(fx_t *fx, heap_batch_t *batch, heap_status_t *finish_status)
{
    heap_batch_result_t result;
    heap_status_t finished = heap_batch_finish(batch, &result);
    if (finish_status != NULL) *finish_status = finished;
    if (finished != HEAP_OK) return CATALOG_STORE_INVALID;
    return fx_save_finished(fx, batch, &result);
}

/* 게시에 성공한 뒤 새 root로 heap을 다시 연다. 실패 주입을 끈 뒤에 부른다. */
static void fx_adopt(fx_t *fx)
{
    heap_close(fx->heap);
    heap_schema_t schema = {TYPES, COLUMNS};
    assert(heap_open(fx->file, fx->tid, fx->new_root, &schema, &fx->heap) == HEAP_OK);
}

static catalog_store_status_t fx_commit(fx_t *fx, heap_batch_t *batch)
{
    catalog_store_status_t saved = fx_publish(fx, batch, NULL);
    if (saved == CATALOG_STORE_OK) fx_adopt(fx);
    return saved;
}

/* heap을 순회해 id를 키로 모델과 대조한다. 행 수가 같고 각 행이 같아야 한다. */
static int g_scan_locators[MAX_MODEL];

static bool heap_matches_model(heap_t *heap)
{
    size_t expected = 0;
    for (int i = 0; i < MAX_MODEL; i++) expected += g_model[i].present;
    bool seen[MAX_MODEL] = {false};
    heap_cursor_t *cursor;
    if (heap_cursor_open(heap, &cursor) != HEAP_OK) return false;
    value_t row[COLUMNS] = ZERO_ROW;
    heap_locator_t locator;
    size_t found = 0;
    bool ok = true;
    for (;;) {
        heap_status_t status = heap_cursor_next(cursor, row, &locator);
        if (status == HEAP_DONE) break;
        if (status != HEAP_OK) { ok = false; break; }
        int64_t id = row[0].view.integer;
        if (id < 0 || id >= MAX_MODEL || !g_model[id].present || seen[id] || !rows_equal(row, g_model[id].cells)) {
            ok = false;
            break;
        }
        seen[id] = true;
        g_scan_locators[id] = (int)locator.directory_index;
        found++;
        clear_row(row);
    }
    clear_row(row);
    heap_cursor_close(cursor);
    return ok && found == expected;
}

/* ── 테스트 ── */

static void test_empty_heap_and_page_slot_layout(void)
{
    printf("\n[test_empty_heap_and_page_slot_layout]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    CHECK(heap_root(fx.heap) == 0 && heap_record_page_count(fx.heap) == 0, "빈 heap");
    CHECK(heap_matches_model(fx.heap), "빈 heap 순회");
    value_t row[COLUMNS] = ZERO_ROW;
    CHECK(heap_get(fx.heap, (heap_locator_t){0, 0}, row) == HEAP_NOT_FOUND, "없는 locator");

    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    heap_batch_result_t result;
    CHECK(heap_batch_finish(batch, &result) == HEAP_OK && result.new_root == 0 && result.old_count == 0 &&
              result.new_count == 0,
          "변경 없는 batch는 아무것도 쓰지 않음");
    heap_batch_free(batch);

    /* record_page의 slot 수 위치 가정: 지운 slot을 건너뛰고 나머지를 모두 돌려준다 */
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    heap_locator_t loc[20];
    for (int i = 0; i < 20; i++) {
        model_put(i, 3);
        value_view_t views[COLUMNS];
        to_views(g_model[i].cells, views);
        assert(heap_batch_insert(batch, views, COLUMNS, &loc[i]) == HEAP_OK);
    }
    for (int i = 0; i < 20; i += 3) {
        heap_status_t deleted = heap_batch_delete(batch, loc[i]);
        if (deleted != HEAP_OK) fprintf(stderr, "delete %d -> %d\n", i, deleted);
        assert(deleted == HEAP_OK);
        clear_row(g_model[i].cells);
        g_model[i].present = false;
    }
    CHECK(heap_batch_delete(batch, loc[0]) == HEAP_NOT_FOUND, "삭제한 행 재삭제는 NOT_FOUND이고 batch를 망치지 않음");
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    CHECK(heap_matches_model(fx.heap), "삭제한 slot을 건너뛰고 나머지를 돌려줌");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "다시 열어도 같음");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

static void test_all_value_kinds_and_sizes_round_trip(void)
{
    printf("\n[test_all_value_kinds_and_sizes_round_trip]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    for (int id = 0; id < 64; id++) {
        model_put(id, id);
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        heap_locator_t loc;
        heap_status_t status = heap_batch_insert(batch, views, COLUMNS, &loc);
        CHECK(status == HEAP_OK, "삽입 %d: %d", id, status);
    }
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    CHECK(heap_matches_model(fx.heap), "삽입한 모든 값 종류와 크기가 같은 핸들에서 일치");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "다시 열어도 일치");

    /* 1MB TEXT와 BLOB, 값 경계(8000바이트 행 한도 부근) */
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    size_t big = 1000000;
    uint8_t *bytes = malloc(big);
    assert(bytes != NULL);
    for (size_t i = 0; i < big; i++) bytes[i] = (uint8_t)(i * 31u + 7u);
    value_t row[COLUMNS] = ZERO_ROW;
    make_row(100, 0, row);
    value_clear(&row[3]);
    assert(value_set_blob(&row[3], bytes, big) == VALUE_OK);
    value_clear(&row[2]);
    uint8_t *text = malloc(big);
    assert(text != NULL);
    for (size_t i = 0; i < big; i++) text[i] = (uint8_t)('a' + i % 26);
    assert(value_set_text(&row[2], text, big) == VALUE_OK);
    value_view_t views[COLUMNS];
    to_views(row, views);
    heap_locator_t loc;
    CHECK(heap_batch_insert(batch, views, COLUMNS, &loc) == HEAP_OK, "1MB 값 삽입");
    clear_row(g_model[100].cells);
    for (int i = 0; i < COLUMNS; i++) {
        assert(value_clone(&row[i].view, &g_model[100].cells[i]) == VALUE_OK);
    }
    g_model[100].present = true;
    clear_row(row);
    free(bytes);
    free(text);
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "1MB 값이 잘림 없이 왕복");

    heap_locator_t first = {0, 0};
    value_t got[COLUMNS] = ZERO_ROW;
    CHECK(heap_get(fx.heap, first, got) == HEAP_OK && got[0].view.type == VALUE_INTEGER, "locator로 한 행 읽기");
    clear_row(got);
    fx_close(&fx);
    model_reset();
    clean_dir();
}

static void test_wrong_inputs_are_rejected_without_change(void)
{
    printf("\n[test_wrong_inputs_are_rejected_without_change]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    value_t row[COLUMNS] = ZERO_ROW;
    make_row(1, 0, row);
    value_view_t views[COLUMNS];
    to_views(row, views);
    heap_locator_t loc;
    CHECK(heap_batch_insert(batch, views, COLUMNS - 1, &loc) == HEAP_INVALID, "컬럼 수 불일치");
    value_view_t wrong[COLUMNS];
    to_views(row, wrong);
    wrong[0].type = VALUE_TEXT;
    CHECK(heap_batch_insert(batch, wrong, COLUMNS, &loc) == HEAP_INVALID, "타입 불일치");
    to_views(row, wrong);
    wrong[1].real = NAN;
    CHECK(heap_batch_insert(batch, wrong, COLUMNS, &loc) == HEAP_INVALID, "유한하지 않은 실수");
    to_views(row, wrong);
    static const uint8_t bad_utf8[] = {0xC0, 0x80};
    wrong[2] = (value_view_t){VALUE_TEXT, 0, 0, bad_utf8, 2};
    CHECK(heap_batch_insert(batch, wrong, COLUMNS, &loc) == HEAP_INVALID, "잘못된 UTF-8");
    to_views(row, wrong);
    wrong[3] = (value_view_t){VALUE_BLOB, 0, 0, NULL, 3};
    CHECK(heap_batch_insert(batch, wrong, COLUMNS, &loc) == HEAP_INVALID, "bytes 없는 BLOB");
    CHECK(heap_batch_insert(batch, NULL, COLUMNS, &loc) == HEAP_INVALID, "NULL 값 배열");
    /* 입력 오류는 batch를 망치지 않는다 */
    CHECK(heap_batch_insert(batch, views, COLUMNS, &loc) == HEAP_OK, "입력 오류 뒤에도 같은 batch로 삽입");
    model_put(1, 0);
    clear_row(row);
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    CHECK(heap_matches_model(fx.heap), "유효한 행만 저장됨");

    heap_schema_t bad = {TYPES, 0};
    heap_t *other;
    CHECK(heap_open(fx.file, fx.tid, 0, &bad, &other) == HEAP_INVALID, "컬럼 0개 schema");
    value_type_t with_null[2] = {VALUE_INTEGER, VALUE_NULL};
    bad = (heap_schema_t){with_null, 2};
    CHECK(heap_open(fx.file, fx.tid, 0, &bad, &other) == HEAP_INVALID, "NULL 컬럼 타입");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

static void test_update_delete_and_row_moves(void)
{
    printf("\n[test_update_delete_and_row_moves]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    heap_locator_t locs[200];
    for (int id = 0; id < 200; id++) {
        model_put(id, id % 8 == 7 ? 3 : id % 5);  /* 큰 변형(20000)은 섞지 않아 한 페이지에 여러 행 */
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        assert(heap_batch_insert(batch, views, COLUMNS, &locs[id]) == HEAP_OK);
    }
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    CHECK(heap_matches_model(fx.heap), "초기 200행");
    size_t pages_before = heap_record_page_count(fx.heap);

    /* 두 번째 batch: 크기 증가·감소·삭제. 커밋 뒤 locator는 다시 순회로 얻는다 */
    assert(heap_matches_model(fx.heap));
    heap_batch_t *second;
    assert(heap_batch_begin(fx.heap, &second) == HEAP_OK);
    int moved = 0;
    for (int id = 0; id < 200; id += 7) {
        /* 이 id의 현재 locator: 순회 */
        heap_cursor_t *cursor;
        assert(heap_cursor_open(fx.heap, &cursor) == HEAP_OK);
        value_t row[COLUMNS] = ZERO_ROW;
        heap_locator_t at = {0, 0};
        bool found = false;
        for (;;) {
            heap_status_t status = heap_cursor_next(cursor, row, &at);
            if (status != HEAP_OK) break;
            bool hit = row[0].view.integer == id;
            clear_row(row);
            if (hit) { found = true; break; }
        }
        heap_cursor_close(cursor);
        assert(found);
        if (id % 14 == 0) {
            model_put(id, 7);  /* 20000바이트로 커짐: overflow로 가거나 페이지를 옮김 */
            value_view_t views[COLUMNS];
            to_views(g_model[id].cells, views);
            heap_locator_t placed;
            heap_status_t status = heap_batch_update(second, at, views, COLUMNS, &placed);
            CHECK(status == HEAP_OK, "커지는 갱신 %d: %d", id, status);
            if (placed.directory_index != at.directory_index || placed.slot != at.slot) moved++;
            value_t again[COLUMNS] = ZERO_ROW;
            CHECK(heap_batch_get(second, placed, again) == HEAP_OK && rows_equal(again, g_model[id].cells),
                  "같은 batch 안에서 갱신한 행을 읽음");
            clear_row(again);
        } else if (id % 21 == 0) {
            assert(heap_batch_delete(second, at) == HEAP_OK);
            clear_row(g_model[id].cells);
            g_model[id].present = false;
        } else {
            model_put(id, 0);  /* 빈 값으로 줄어듦 */
            value_view_t views[COLUMNS];
            to_views(g_model[id].cells, views);
            heap_locator_t placed;
            assert(heap_batch_update(second, at, views, COLUMNS, &placed) == HEAP_OK);
        }
    }
    assert(fx_commit(&fx, second) == CATALOG_STORE_OK);
    heap_batch_free(second);
    CHECK(heap_matches_model(fx.heap), "갱신·삭제 뒤 모델과 일치");
    CHECK(heap_record_page_count(fx.heap) >= pages_before, "record page는 줄지 않음");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "다시 열어도 일치 (이동한 행 %d)", moved);
    fx_close(&fx);
    model_reset();
    clean_dir();
}

/* load_many_pages가 넣는 행. 모델에도 같은 행을 넣는다. */
static void model_put_loaded(int id)
{
    make_row(id, 4, g_model[id].cells);
    value_clear(&g_model[id].cells[3]);
    assert(value_set_null(&g_model[id].cells[3]) == VALUE_OK);
    g_model[id].present = true;
}

/* 행 하나가 약 3990바이트라 한 record page에 두 행이 들어간다. 4300행이면 디렉터리 잎이 둘이다. */
static void load_many_pages(fx_t *fx, int rows)
{
    heap_batch_t *batch;
    assert(heap_batch_begin(fx->heap, &batch) == HEAP_OK);
    for (int id = 0; id < rows; id++) {
        model_put_loaded(id);
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        heap_locator_t loc;
        heap_status_t status = heap_batch_insert(batch, views, COLUMNS, &loc);
        if (status != HEAP_OK) {
            CHECK(false, "삽입 %d: %d", id, status);
            break;
        }
    }
    assert(fx_commit(fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
}

static void test_many_pages_need_several_directory_pages(void)
{
    printf("\n[test_many_pages_need_several_directory_pages]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    load_many_pages(&fx, 4300);
    size_t pages = heap_record_page_count(fx.heap);
    CHECK(pages > HEAP_DIRECTORY_ENTRIES, "디렉터리 한 장의 항목 수(%u)를 넘는 record page %zu개",
          (unsigned)HEAP_DIRECTORY_ENTRIES, pages);
    CHECK(heap_matches_model(fx.heap), "여러 디렉터리 페이지를 지나 모든 행을 순서대로 읽음");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "다시 열어도 일치");

    uint32_t *all;
    size_t count;
    CHECK(heap_collect_pages(fx.heap, &all, &count) == HEAP_OK && count > pages + 1,
          "DROP용 페이지 목록에 디렉터리·record page·overflow가 모두 있음");
    bool unique = true;
    for (size_t i = 0; i < count && unique; i++) {
        if (all[i] < PAGE_FILE_CONTROL_PAGES) unique = false;
        for (size_t j = i + 1; j < count && j < i + 64; j++) {
            if (all[i] == all[j]) unique = false;
        }
    }
    CHECK(unique, "목록에 제어 페이지나 (인접한) 중복이 없음");
    free(all);
    fx_close(&fx);
    model_reset();
    clean_dir();
}

/* heap_root 위치의 종류 구별 */
static void test_locator_kinds_are_distinguished(void)
{
    printf("\n[test_locator_kinds_are_distinguished]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    for (int id = 0; id < 30; id++) {
        model_put(id, id % 5);
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        heap_locator_t loc;
        assert(heap_batch_insert(batch, views, COLUMNS, &loc) == HEAP_OK);
    }
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    uint32_t good_root = heap_root(fx.heap);
    uint32_t *pages;
    size_t count;
    assert(heap_collect_pages(fx.heap, &pages, &count) == HEAP_OK);
    uint32_t record_page = 0, overflow_page = 0;
    for (size_t i = 0; i < count; i++) {
        page_file_page_info_t info;
        assert(page_file_page_info(fx.file, pages[i], &info) == PAGE_FILE_OK);
        if (info.kind == PAGE_KIND_RECORD && record_page == 0) record_page = pages[i];
        if (info.kind == PAGE_KIND_OVERFLOW && overflow_page == 0) overflow_page = pages[i];
    }
    free(pages);
    assert(record_page != 0 && overflow_page != 0);
    heap_schema_t schema = {TYPES, COLUMNS};
    heap_t *probe = NULL;

    /* #77이 만든 단일 record root는 디렉터리로 재해석하지 않는다. 파일 내용은 그대로다 */
    heap_close(fx.heap);
    fx.heap = NULL;
    page_file_close(fx.file);
    /* 아래에서 게시된 페이지를 손상시켜 본다. 최신 세대의 manifest가 그 페이지를 대조하지 않게 빈 commit을 한다 */
    assert(page_file_open(DB_PATH, true, &fx.file, NULL) == PAGE_FILE_OK && page_file_commit(fx.file) == PAGE_FILE_OK);
    page_file_close(fx.file);
    uint64_t before = file_hash();
    assert(page_file_open(DB_PATH, false, &fx.file, NULL) == PAGE_FILE_OK);
    CHECK(heap_open(fx.file, fx.tid, record_page, &schema, &probe) == HEAP_UNSUPPORTED_LOCATOR && probe == NULL,
          "record root는 UNSUPPORTED_LOCATOR");
    CHECK(heap_open(fx.file, fx.tid, overflow_page, &schema, &probe) == HEAP_UNSUPPORTED_LOCATOR,
          "overflow 페이지는 UNSUPPORTED_LOCATOR");
    CHECK(heap_open(fx.file, fx.tid, 0, &schema, &probe) == HEAP_OK, "root 0은 빈 heap");
    heap_close(probe);
    CHECK(heap_open(fx.file, fx.tid, 9999, &schema, &probe) == HEAP_CORRUPT, "범위 밖 locator");
    CHECK(heap_open(fx.file, fx.tid + 1, good_root, &schema, &probe) == HEAP_CORRUPT,
          "다른 table의 디렉터리를 이 table로 열면 거절");
    page_file_close(fx.file);
    CHECK(file_hash() == before, "열기 시도가 파일을 바꾸지 않음");

    /* 디렉터리 본문 변조(체크섬은 다시 계산): 첫 페이지는 UNSUPPORTED, 항목·체인 오류는 CORRUPT */
    size_t size;
    uint8_t *orig = slurp(DB_PATH, &size);
    uint8_t *edit = malloc(size);
    assert(edit != NULL);
    size_t dir_at = (size_t)good_root * PAGE_FILE_PAGE_BYTES;
    struct {
        const char *name;
        size_t offset;      /* 본문 안 위치 */
        uint8_t value;
        heap_status_t expect;
    } cases[] = {
        {"magic 변조", 0, 'X', HEAP_UNSUPPORTED_LOCATOR},
        {"version 2", 4, 2, HEAP_UNSUPPORTED_LOCATOR},
        {"flags 변조", 6, 1, HEAP_UNSUPPORTED_LOCATOR},
        {"항목 수 한도 초과", 17, 0x40, HEAP_CORRUPT},
        {"다음 페이지가 자기 자신", 12, 0xFF, HEAP_CORRUPT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memcpy(edit, orig, size);
        uint8_t *page = edit + dir_at;
        if (i == 4) {
            for (unsigned b = 0; b < 4; b++) page[PAGE_ENVELOPE_BYTES + 12 + b] = (uint8_t)(good_root >> (8 * b));
        } else {
            page[PAGE_ENVELOPE_BYTES + cases[i].offset] = cases[i].value;
        }
        reseal(page);
        spit(DB_PATH, edit, size);
        probe = NULL;
        assert(page_file_open(DB_PATH, false, &fx.file, NULL) == PAGE_FILE_OK);
        heap_status_t status = heap_open(fx.file, fx.tid, good_root, &schema, &probe);
        CHECK(status == cases[i].expect && probe == NULL, "%s: %d", cases[i].name, status);
        page_file_close(fx.file);
    }
    /* 중복 항목과 제어 페이지를 가리키는 항목 */
    memcpy(edit, orig, size);
    uint8_t *page = edit + dir_at;
    memcpy(page + PAGE_ENVELOPE_BYTES + HEAP_DIRECTORY_HEADER_BYTES + 4,
           page + PAGE_ENVELOPE_BYTES + HEAP_DIRECTORY_HEADER_BYTES, 4);
    reseal(page);
    spit(DB_PATH, edit, size);
    assert(page_file_open(DB_PATH, false, &fx.file, NULL) == PAGE_FILE_OK);
    probe = NULL;
    CHECK(heap_open(fx.file, fx.tid, good_root, &schema, &probe) == HEAP_CORRUPT, "중복 항목");
    page_file_close(fx.file);
    memcpy(edit, orig, size);
    page = edit + dir_at;
    page[PAGE_ENVELOPE_BYTES + HEAP_DIRECTORY_HEADER_BYTES] = 0;
    page[PAGE_ENVELOPE_BYTES + HEAP_DIRECTORY_HEADER_BYTES + 1] = 0;
    page[PAGE_ENVELOPE_BYTES + HEAP_DIRECTORY_HEADER_BYTES + 2] = 0;
    page[PAGE_ENVELOPE_BYTES + HEAP_DIRECTORY_HEADER_BYTES + 3] = 0;
    reseal(page);
    spit(DB_PATH, edit, size);
    assert(page_file_open(DB_PATH, false, &fx.file, NULL) == PAGE_FILE_OK);
    probe = NULL;
    CHECK(heap_open(fx.file, fx.tid, good_root, &schema, &probe) == HEAP_CORRUPT, "제어 페이지를 가리키는 항목");
    page_file_close(fx.file);
    spit(DB_PATH, orig, size);
    free(orig);
    free(edit);

    /* record page 안의 행 형식 오류는 읽을 때 CORRUPT */
    size_t sz2;
    uint8_t *file_bytes = slurp(DB_PATH, &sz2);
    size_t rec_at = (size_t)record_page * PAGE_FILE_PAGE_BYTES;
    file_bytes[rec_at + PAGE_ENVELOPE_BYTES + 3] ^= 1;  /* record page magic */
    reseal(file_bytes + rec_at);
    spit(DB_PATH, file_bytes, sz2);
    free(file_bytes);
    assert(page_file_open(DB_PATH, false, &fx.file, NULL) == PAGE_FILE_OK);
    assert(heap_open(fx.file, fx.tid, good_root, &schema, &probe) == HEAP_OK);
    heap_cursor_t *cursor;
    assert(heap_cursor_open(probe, &cursor) == HEAP_OK);
    value_t row[COLUMNS] = ZERO_ROW;
    heap_locator_t at;
    heap_status_t status = HEAP_OK;
    int rows = 0;
    while ((status = heap_cursor_next(cursor, row, &at)) == HEAP_OK) {
        rows++;
        clear_row(row);
    }
    CHECK(status == HEAP_CORRUPT, "손상된 record page는 CORRUPT로 끝남 (읽은 행 %d)", rows);
    heap_cursor_close(cursor);
    heap_close(probe);
    page_file_close(fx.file);
    fx.file = NULL;
    fx.heap = NULL;
    catalog_close(fx.catalog);
    model_reset();
    clean_dir();
}

/* COW: 게시된 상태가 가리키던 페이지는 다음 게시 뒤에도 덮어쓰이지 않는다 */
static void test_copy_on_write_keeps_published_pages(void)
{
    printf("\n[test_copy_on_write_keeps_published_pages]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    for (int id = 0; id < 40; id++) {
        model_put(id, id % 8);
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        heap_locator_t loc;
        assert(heap_batch_insert(batch, views, COLUMNS, &loc) == HEAP_OK);
    }
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    uint32_t gen1_root = heap_root(fx.heap);
    uint32_t *gen1_pages;
    size_t gen1_count;
    assert(heap_collect_pages(fx.heap, &gen1_pages, &gen1_count) == HEAP_OK);

    /* 같은 상태의 모델을 복사해 두고 다음 세대를 만든다 */
    static model_row_t gen1_model[MAX_MODEL];
    for (int i = 0; i < 40; i++) {
        gen1_model[i].present = g_model[i].present;
        for (int c = 0; c < COLUMNS; c++) {
            memset(&gen1_model[i].cells[c], 0, sizeof(gen1_model[i].cells[c]));
            assert(value_clone(&g_model[i].cells[c].view, &gen1_model[i].cells[c]) == VALUE_OK);
        }
    }
    size_t pending_at_begin = page_file_pending_release_count(fx.file);
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    heap_cursor_t *cursor;
    assert(heap_cursor_open(fx.heap, &cursor) == HEAP_OK);
    value_t row[COLUMNS] = ZERO_ROW;
    heap_locator_t at;
    while (heap_cursor_next(cursor, row, &at) == HEAP_OK) {
        int id = (int)row[0].view.integer;
        clear_row(row);
        if (id % 2 == 0) {
            assert(heap_batch_delete(batch, at) == HEAP_OK);
            clear_row(g_model[id].cells);
            g_model[id].present = false;
        } else {
            model_put(id, 6);
            value_view_t views[COLUMNS];
            to_views(g_model[id].cells, views);
            heap_locator_t placed;
            assert(heap_batch_update(batch, at, views, COLUMNS, &placed) == HEAP_OK);
        }
    }
    heap_cursor_close(cursor);
    heap_batch_result_t result;
    assert(heap_batch_finish(batch, &result) == HEAP_OK);
    CHECK(result.new_root != gen1_root, "새 디렉터리");
    CHECK(page_file_pending_release_count(fx.file) == pending_at_begin,
          "게시하기 전에는 옛 페이지를 page_file에 놓지 않음");

    bool new_overlaps_old = false;
    for (size_t i = 0; i < result.new_count; i++) {
        for (size_t j = 0; j < gen1_count; j++) {
            if (result.new_pages[i] == gen1_pages[j]) new_overlaps_old = true;
        }
    }
    CHECK(!new_overlaps_old, "새로 쓴 페이지는 게시된 세대의 페이지와 겹치지 않음");
    CHECK(result.old_count > 0, "게시 성공 뒤 놓을 옛 페이지가 있음");
    assert(catalog_set_locators(fx.catalog, fx.tid, result.new_root, 0) == CATALOG_OK);
    assert(catalog_store_save(fx.file, fx.catalog) == CATALOG_STORE_OK);
    /* 페이지 회계: 옛 세대의 모든 페이지는 새 세대가 계속 쓰거나 놓을 목록에 있다(누수 없음) */
    {
        heap_schema_t gen2_schema = {TYPES, COLUMNS};
        heap_t *gen2_heap;
        assert(heap_open(fx.file, fx.tid, result.new_root, &gen2_schema, &gen2_heap) == HEAP_OK);
        uint32_t *gen2_pages;
        size_t gen2_count;
        assert(heap_collect_pages(gen2_heap, &gen2_pages, &gen2_count) == HEAP_OK);
        bool all_accounted = true, old_list_clean = true;
        for (size_t i = 0; i < gen1_count; i++) {
            bool in_old = false, in_new = false;
            for (size_t j = 0; j < result.old_count; j++) in_old |= result.old_pages[j] == gen1_pages[i];
            for (size_t j = 0; j < gen2_count; j++) in_new |= gen2_pages[j] == gen1_pages[i];
            if (!in_old && !in_new) all_accounted = false;
            if (in_old && in_new) old_list_clean = false;
        }
        for (size_t j = 0; j < result.old_count; j++) {
            for (size_t i = 0; i < gen2_count; i++) {
                if (result.old_pages[j] == gen2_pages[i]) old_list_clean = false;
            }
        }
        CHECK(all_accounted, "옛 세대의 모든 페이지(체인 포함)가 계속 쓰이거나 놓을 목록에 있음");
        CHECK(old_list_clean, "놓을 목록에 새 세대가 쓰는 페이지가 없음");
        free(gen2_pages);
        heap_close(gen2_heap);
    }
    assert(heap_batch_release_old(batch) == HEAP_OK);
    heap_batch_free(batch);

    /* 옛 root로 연 heap은 새 세대가 게시된 뒤에도 옛 행을 그대로 읽는다 */
    heap_schema_t schema = {TYPES, COLUMNS};
    heap_t *old_heap;
    assert(heap_open(fx.file, fx.tid, gen1_root, &schema, &old_heap) == HEAP_OK);
    static model_row_t saved[MAX_MODEL];
    memcpy(saved, g_model, sizeof(saved));
    memcpy(g_model, gen1_model, sizeof(g_model));
    CHECK(heap_matches_model(old_heap), "옛 root로 열면 옛 행이 그대로 보임(제자리 덮어쓰기 없음)");
    memcpy(g_model, saved, sizeof(saved));
    heap_close(old_heap);

    /* 옛 페이지는 commit이 두 번 성공하기 전에는 새 batch가 받지 못한다 */
    heap_close(fx.heap);
    assert(heap_open(fx.file, fx.tid, result.new_root, &schema, &fx.heap) == HEAP_OK);
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    value_t extra[COLUMNS] = ZERO_ROW;
    make_row(1000, 4, extra);
    value_view_t views[COLUMNS];
    to_views(extra, views);
    heap_locator_t placed;
    assert(heap_batch_insert(batch, views, COLUMNS, &placed) == HEAP_OK);
    heap_batch_result_t next;
    assert(heap_batch_finish(batch, &next) == HEAP_OK);
    bool reused = false;
    for (size_t i = 0; i < next.new_count; i++) {
        for (size_t j = 0; j < gen1_count; j++) {
            if (next.new_pages[i] == gen1_pages[j]) reused = true;
        }
    }
    CHECK(!reused, "직전 세대(fallback)가 가리킨 페이지를 새 batch가 재사용하지 않음");
    CHECK(heap_batch_abort(batch) == HEAP_OK, "게시하지 않으면 abort로 새 페이지를 놓음");
    heap_batch_free(batch);
    clear_row(extra);
    for (int i = 0; i < 40; i++) clear_row(gen1_model[i].cells);
    free(gen1_pages);
    fx_close(&fx);
    model_reset();
    clean_dir();
}

/* 한 단계씩 실패시킨다. 게시 전 실패는 abort 뒤 게시된 상태가 그대로이고, 불확정이면 둘 중 하나다 */
typedef enum { FAIL_PWRITE, FAIL_PREAD, FAIL_FSYNC, FAIL_ALLOC } fail_kind_t;

static void arm(fail_kind_t kind, int n)
{
    memset(&g_hooks, 0, sizeof(g_hooks));
    g_alloc_fail_at = 0;
    g_alloc_calls = 0;
    if (kind == FAIL_PWRITE) g_hooks.pwrite_fail_at = n;
    if (kind == FAIL_PREAD) g_hooks.pread_fail_at = n;
    if (kind == FAIL_FSYNC) g_hooks.fsync_fail_at = n;
    if (kind == FAIL_ALLOC) g_alloc_fail_at = n;
}

static void disarm(void)
{
    memset(&g_hooks, 0, sizeof(g_hooks));
    g_alloc_fail_at = 0;
}

/* 삽입 3(하나는 20000바이트), 갱신 2(하나는 커짐), 삭제 2를 한 batch로 한다. */
static void apply_script(fx_t *fx, heap_batch_t *batch, heap_status_t *failure)
{
    *failure = HEAP_OK;
    heap_cursor_t *cursor;
    if (heap_cursor_open(fx->heap, &cursor) != HEAP_OK) { *failure = HEAP_NOMEM; return; }
    value_t row[COLUMNS] = ZERO_ROW;
    heap_locator_t at;
    int seen = 0;
    for (;;) {
        heap_status_t status = heap_cursor_next(cursor, row, &at);
        if (status == HEAP_DONE) break;
        if (status != HEAP_OK) { *failure = status; break; }
        int id = (int)row[0].view.integer;
        clear_row(row);
        seen++;
        if (seen > 6) continue;
        if (seen <= 2) {
            value_t grown[COLUMNS] = ZERO_ROW;
            make_row(id, 7, grown);
            value_view_t views[COLUMNS];
            to_views(grown, views);
            heap_locator_t placed;
            status = heap_batch_update(batch, at, views, COLUMNS, &placed);
            clear_row(grown);
        } else if (seen <= 4) {
            status = heap_batch_delete(batch, at);
        } else {
            status = HEAP_OK;
        }
        if (status != HEAP_OK) { *failure = status; break; }
    }
    heap_cursor_close(cursor);
    for (int i = 0; *failure == HEAP_OK && i < 3; i++) {
        value_t fresh[COLUMNS] = ZERO_ROW;
        make_row(900 + i, i == 0 ? 7 : 2, fresh);
        value_view_t views[COLUMNS];
        to_views(fresh, views);
        heap_locator_t loc;
        heap_status_t status = heap_batch_insert(batch, views, COLUMNS, &loc);
        clear_row(fresh);
        if (status != HEAP_OK) *failure = status;
    }
}

static void test_failures_never_change_published_state(void)
{
    printf("\n[test_failures_never_change_published_state]\n");
    page_file_set_default_io(&HOOKED);
    static const char *names[] = {"pwrite", "pread", "fsync", "alloc"};
    int unknown_seen = 0, not_applied_seen = 0, batch_failures = 0;
    for (int kind = FAIL_PWRITE; kind <= FAIL_ALLOC; kind++) {
        for (int n = 1; n <= 400; n++) {
            fx_t fx;
            disarm();
            fx_create(&fx);
            model_reset();
            heap_batch_t *seed;
            assert(heap_batch_begin(fx.heap, &seed) == HEAP_OK);
            for (int id = 0; id < 12; id++) {
                model_put(id, id % 5);
                value_view_t views[COLUMNS];
                to_views(g_model[id].cells, views);
                heap_locator_t loc;
                assert(heap_batch_insert(seed, views, COLUMNS, &loc) == HEAP_OK);
            }
            assert(fx_commit(&fx, seed) == CATALOG_STORE_OK);
            heap_batch_free(seed);
            /* 새 세대와 비교할 모델은 스크립트가 끝난 뒤 따로 만든다 */
            static model_row_t baseline[MAX_MODEL];
            memcpy(baseline, g_model, sizeof(baseline));
            uint64_t generation_before = page_file_control(fx.file)->generation;

            heap_batch_t *batch;
            assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
            arm((fail_kind_t)kind, n);
            heap_status_t failure;
            apply_script(&fx, batch, &failure);
            heap_status_t finish_status = HEAP_OK;
            catalog_store_status_t saved = CATALOG_STORE_OK;
            if (failure == HEAP_OK) saved = fx_publish(&fx, batch, &finish_status);
            bool triggered = failure != HEAP_OK || finish_status != HEAP_OK || saved != CATALOG_STORE_OK ||
                             (kind == FAIL_ALLOC ? g_alloc_calls >= n
                                                 : kind == FAIL_PWRITE   ? g_hooks.pwrite_calls >= n
                                                 : kind == FAIL_PREAD    ? g_hooks.pread_calls >= n
                                                                         : g_hooks.fsync_calls >= n);
            if (!triggered) {
                heap_batch_free(batch);
                fx_close(&fx);
                model_reset();
                clean_dir();
                break;  /* 이 단계 번호가 스크립트의 호출 수를 넘었다 */
            }
            disarm();

            bool batch_stage = failure != HEAP_OK || finish_status != HEAP_OK;
            if (batch_stage) {
                batch_failures++;
                CHECK(failure == HEAP_OK || failure == HEAP_IO || failure == HEAP_NOMEM,
                      "%s %d: 실패 상태 %d", names[kind], n, failure);
                size_t reusable_mid = page_file_reusable_count(fx.file);
                size_t pending_mid = page_file_pending_release_count(fx.file);
                size_t tracked = heap_batch_fresh_count(batch);
                CHECK(heap_batch_abort(batch) == HEAP_OK, "%s %d: abort", names[kind], n);
                CHECK(page_file_reusable_count(fx.file) - reusable_mid == tracked &&
                          page_file_pending_release_count(fx.file) == pending_mid && heap_batch_fresh_count(batch) == 0,
                      "%s %d: abort가 추적한 새 페이지(%zu)를 곧바로 재사용 목록에 놓음", names[kind], n, tracked);
                heap_batch_free(batch);
                CHECK(heap_matches_model(fx.heap), "%s %d: 게시된 상태 불변(같은 핸들)", names[kind], n);
                fx_reopen(&fx);
                CHECK(heap_matches_model(fx.heap), "%s %d: 게시된 상태 불변(다시 열기)", names[kind], n);
            } else if (saved != CATALOG_STORE_OK) {
                /* 게시 단계 실패 */
                size_t gen_after = page_file_control(fx.file)->generation;
                (void)gen_after;
                if (saved == CATALOG_STORE_OUTCOME_UNKNOWN) {
                    unknown_seen++;
                    CHECK(page_file_is_poisoned(fx.file), "%s %d: 불확정이면 poison", names[kind], n);
                    heap_batch_free(batch);  /* abort도 release도 하지 않는다 */
                    page_file_close(fx.file);
                    fx.file = NULL;
                    heap_close(fx.heap);
                    catalog_close(fx.catalog);
                    memset(&fx, 0, sizeof(fx));
                    assert(page_file_open(DB_PATH, true, &fx.file, NULL) == PAGE_FILE_OK);
                    assert(catalog_store_load(fx.file, &fx.catalog) == CATALOG_STORE_OK);
                    assert(catalog_find(fx.catalog, "items", 5, &fx.tid) == CATALOG_OK);
                    heap_schema_t schema = {TYPES, COLUMNS};
                    assert(heap_open(fx.file, fx.tid, catalog_heap_root(fx.catalog, fx.tid), &schema, &fx.heap) == HEAP_OK);
                    uint64_t generation_after = page_file_control(fx.file)->generation;
                    if (generation_after == generation_before) {
                        CHECK(heap_matches_model(fx.heap), "%s %d: 불확정 결과가 옛 상태", names[kind], n);
                    } else {
                        /* 새 세대가 보이면 그 내용이 온전하고 일관돼야 한다: 모든 행이 읽히고 id가 유일 */
                        heap_cursor_t *cursor;
                        assert(heap_cursor_open(fx.heap, &cursor) == HEAP_OK);
                        value_t row[COLUMNS] = ZERO_ROW;
                        heap_locator_t at;
                        int rows = 0;
                        heap_status_t status;
                        while ((status = heap_cursor_next(cursor, row, &at)) == HEAP_OK) {
                            rows++;
                            clear_row(row);
                        }
                        CHECK(status == HEAP_DONE && rows >= 1, "%s %d: 불확정 뒤 새 세대도 온전함 (%d행)", names[kind], n, rows);
                        heap_cursor_close(cursor);
                    }
                } else if (saved == CATALOG_STORE_UNRECLAIMED) {
                    /* 게시는 성공했고 옛 체인만 회수하지 못했다. 새 상태가 온전히 읽혀야 한다 */
                    heap_batch_free(batch);
                    fx_adopt(&fx);
                    heap_cursor_t *cursor;
                    assert(heap_cursor_open(fx.heap, &cursor) == HEAP_OK);
                    value_t row[COLUMNS] = ZERO_ROW;
                    heap_locator_t at;
                    int rows = 0;
                    heap_status_t status;
                    while ((status = heap_cursor_next(cursor, row, &at)) == HEAP_OK) {
                        rows++;
                        clear_row(row);
                    }
                    CHECK(status == HEAP_DONE && rows >= 1, "%s %d: 회수 실패여도 새 상태가 읽힘", names[kind], n);
                    heap_cursor_close(cursor);
                } else {
                    not_applied_seen++;
                    CHECK(saved == CATALOG_STORE_IO || saved == CATALOG_STORE_NOMEM,
                          "%s %d: 제어 쓰기 전 실패 상태 %d", names[kind], n, saved);
                    CHECK(heap_batch_abort(batch) == HEAP_OK, "%s %d: 게시 실패 뒤 abort", names[kind], n);
                    heap_batch_free(batch);
                    CHECK(heap_matches_model(fx.heap), "%s %d: 게시 실패 뒤 상태 불변", names[kind], n);
                }
            } else {
                heap_batch_free(batch);
            }
            memcpy(g_model, baseline, sizeof(baseline));
            fx_close(&fx);
            model_reset();
            clean_dir();
        }
    }
    disarm();
    page_file_set_default_io(NULL);
    CHECK(batch_failures > 20, "batch 단계 실패 지점을 충분히 지남 (%d)", batch_failures);
    CHECK(unknown_seen > 0, "불확정 결과 경로를 지남 (%d)", unknown_seen);
    printf("  (batch 단계 %d, 제어 쓰기 전 게시 실패 %d, 불확정 %d)\n", batch_failures, not_applied_seen, unknown_seen);
}

/* ── 국소 디렉터리 갱신(#159) ── */

#define TREE_ROWS 4300
#define MAX_DIRS 8
#define TARGETS 2

/* heap이 가리키는 디렉터리 페이지를 collect 순서(루트, 잎...)로 돌려준다. */
static size_t directory_pages(heap_t *heap, uint32_t file_dirs[MAX_DIRS], page_file_t *file)
{
    uint32_t *all;
    size_t count;
    assert(heap_collect_pages(heap, &all, &count) == HEAP_OK);
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        page_file_page_info_t info;
        assert(page_file_page_info(file, all[i], &info) == PAGE_FILE_OK);
        if (info.kind != PAGE_KIND_DIRECTORY) continue;
        assert(found < MAX_DIRS);
        file_dirs[found++] = all[i];
    }
    free(all);
    return found;
}

static size_t count_directory_pages(page_file_t *file, const uint32_t *pages, size_t count)
{
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        page_file_page_info_t info;
        assert(page_file_page_info(file, pages[i], &info) == PAGE_FILE_OK);
        found += info.kind == PAGE_KIND_DIRECTORY;
    }
    return found;
}

static bool contains_page(const uint32_t *pages, size_t count, uint32_t page)
{
    for (size_t i = 0; i < count; i++) {
        if (pages[i] == page) return true;
    }
    return false;
}

typedef struct {
    int id;
    heap_locator_t at;
} target_t;

/* 순회 순서의 앞쪽 TARGETS개 행(첫 잎에 속한다)과 마지막 행을 찾는다. */
static void find_targets(heap_t *heap, target_t first[TARGETS], target_t *last)
{
    heap_cursor_t *cursor;
    assert(heap_cursor_open(heap, &cursor) == HEAP_OK);
    value_t row[COLUMNS] = ZERO_ROW;
    heap_locator_t at;
    int seen = 0;
    while (heap_cursor_next(cursor, row, &at) == HEAP_OK) {
        target_t found = {(int)row[0].view.integer, at};
        clear_row(row);
        if (seen < TARGETS) first[seen] = found;
        *last = found;
        seen++;
    }
    heap_cursor_close(cursor);
}

/* 대상 행들의 모델을 크기가 작아진 상태(shrunk)나 적재한 상태로 맞춘다. */
static void set_targets_model(const target_t *targets, bool shrunk)
{
    for (int i = 0; i < TARGETS; i++) {
        if (shrunk) model_put(targets[i].id, 0);
        else model_put_loaded(targets[i].id);
    }
}

static heap_status_t update_targets(heap_batch_t *batch, const target_t *targets)
{
    for (int i = 0; i < TARGETS; i++) {
        value_view_t views[COLUMNS];
        to_views(g_model[targets[i].id].cells, views);
        heap_locator_t placed;
        heap_status_t status = heap_batch_update(batch, targets[i].at, views, COLUMNS, &placed);
        if (status != HEAP_OK) return status;
    }
    return HEAP_OK;
}

static bool root_has_magic(fx_t *fx, const char *magic)
{
    uint8_t body[PAGE_BODY_BYTES];
    uint32_t root = catalog_heap_root(fx->catalog, fx->tid);
    assert(page_file_read(fx->file, root, PAGE_KIND_DIRECTORY, fx->tid, body) == PAGE_FILE_OK);
    return memcmp(body, magic, 4) == 0;
}

static uint32_t directory_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void directory_put_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (i * 8u));
}

/* #159: 한 행을 바꿔도 바뀐 record page가 속한 잎과 루트만 새로 쓴다. 나머지 잎 페이지 번호는 그대로다. */
static void test_one_row_change_rewrites_only_its_leaf(void)
{
    printf("\n[test_one_row_change_rewrites_only_its_leaf]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    load_many_pages(&fx, TREE_ROWS);
    uint32_t before[MAX_DIRS], after[MAX_DIRS];
    size_t before_count = directory_pages(fx.heap, before, fx.file);
    CHECK(before_count == 3 && root_has_magic(&fx, HEAP_TREE_ROOT_MAGIC), "루트 하나와 잎 둘의 트리 (%zu장)", before_count);
    target_t first[TARGETS], last;
    find_targets(fx.heap, first, &last);

    /* 첫 잎의 행을 갱신: 새 루트와 새 첫 잎만 쓰고 둘째 잎은 그대로 */
    set_targets_model(first, true);
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    CHECK(update_targets(batch, first) == HEAP_OK, "첫 잎의 행 갱신");
    heap_batch_result_t result;
    assert(heap_batch_finish(batch, &result) == HEAP_OK);
    CHECK(count_directory_pages(fx.file, result.new_pages, result.new_count) == 2, "새로 쓴 디렉터리 페이지는 루트와 잎 하나");
    CHECK(contains_page(result.old_pages, result.old_count, before[0]) &&
              contains_page(result.old_pages, result.old_count, before[1]) &&
              !contains_page(result.old_pages, result.old_count, before[2]),
          "옛 루트와 옛 첫 잎만 반환 대상이고 둘째 잎은 아님");
    assert(fx_save_finished(&fx, batch, &result) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    fx_adopt(&fx);
    size_t after_count = directory_pages(fx.heap, after, fx.file);
    CHECK(after_count == 3 && after[0] != before[0] && after[1] != before[1] && after[2] == before[2],
          "갱신 뒤 둘째 잎은 같은 페이지를 가리킴");
    CHECK(heap_matches_model(fx.heap), "갱신한 행과 나머지 행이 모델과 일치");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "다시 열어도 일치");
    memcpy(before, after, sizeof(before));

    /* 마지막 행 삭제: 새 루트와 새 둘째 잎만 쓰고 첫 잎은 그대로 */
    find_targets(fx.heap, first, &last);
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    CHECK(heap_batch_delete(batch, last.at) == HEAP_OK, "마지막 행 삭제");
    clear_row(g_model[last.id].cells);
    g_model[last.id].present = false;
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    after_count = directory_pages(fx.heap, after, fx.file);
    CHECK(after_count == 3 && after[0] != before[0] && after[1] == before[1] && after[2] != before[2],
          "삭제 뒤 첫 잎은 같은 페이지를 가리킴");
    CHECK(heap_matches_model(fx.heap), "삭제 뒤 모델과 일치");
    memcpy(before, after, sizeof(before));

    /* best-fit은 끝이 아닌 페이지를 고를 수 있다. 바뀐 잎 하나만 다시 써야 한다. */
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    model_put(TREE_ROWS, 2);
    value_view_t views[COLUMNS];
    to_views(g_model[TREE_ROWS].cells, views);
    heap_locator_t placed;
    CHECK(heap_batch_insert(batch, views, COLUMNS, &placed) == HEAP_OK, "행 삽입");
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    after_count = directory_pages(fx.heap, after, fx.file);
    CHECK(after_count == 3 && after[0] != before[0] &&
          ((after[1] != before[1]) != (after[2] != before[2])),
          "삽입 뒤 바뀐 잎 하나만 다시 씀");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "삽입 뒤 다시 열어도 일치");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

/* 옛 코드가 쓴 LHD1 여러 장짜리 체인을 읽고, 다음 변경에서 트리로 바꾸며 체인 페이지를 모두 반환한다. */
static void test_legacy_chain_directory_is_read_and_replaced(void)
{
    printf("\n[test_legacy_chain_directory_is_read_and_replaced]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    load_many_pages(&fx, TREE_ROWS);
    uint32_t tree[MAX_DIRS];
    assert(directory_pages(fx.heap, tree, fx.file) == 3);

    /* 현재 LHD3의 record page ID를 옛 LHD1 체인으로 다시 적는다. */
    uint32_t chain[2];
    size_t record_count = heap_record_page_count(fx.heap);
    uint32_t *record_ids = malloc(record_count * sizeof(*record_ids));
    assert(record_ids != NULL);
    size_t copied = 0;
    for (int i = 0; i < 2; i++) {
        uint8_t body[PAGE_BODY_BYTES];
        assert(page_file_read(fx.file, tree[1 + i], PAGE_KIND_DIRECTORY, fx.tid, body) == PAGE_FILE_OK);
        assert(memcmp(body, HEAP_HINT_DIRECTORY_MAGIC, 4) == 0);
        uint32_t count = directory_u32(body + 16);
        for (uint32_t j = 0; j < count; j++)
            record_ids[copied++] = directory_u32(body + HEAP_DIRECTORY_HEADER_BYTES + (size_t)j * 6u);
        assert(page_file_alloc(fx.file, &chain[i]) == PAGE_FILE_OK);
    }
    assert(copied == record_count && record_count > HEAP_DIRECTORY_ENTRIES &&
           record_count <= HEAP_DIRECTORY_ENTRIES * 2u);
    for (int i = 0; i < 2; i++) {
        uint8_t body[PAGE_BODY_BYTES] = {0};
        size_t first = (size_t)i * HEAP_DIRECTORY_ENTRIES;
        size_t count = record_count - first;
        if (count > HEAP_DIRECTORY_ENTRIES) count = HEAP_DIRECTORY_ENTRIES;
        memcpy(body, HEAP_DIRECTORY_MAGIC, 4);
        body[4] = HEAP_DIRECTORY_VERSION;
        directory_put_u32(body + 8, fx.tid);
        directory_put_u32(body + 12, i == 0 ? chain[1] : 0);
        directory_put_u32(body + 16, (uint32_t)count);
        for (size_t j = 0; j < count; j++)
            directory_put_u32(body + HEAP_DIRECTORY_HEADER_BYTES + j * 4u, record_ids[first + j]);
        assert(page_file_write(fx.file, chain[i], PAGE_KIND_DIRECTORY, fx.tid, body) == PAGE_FILE_OK);
    }
    free(record_ids);
    assert(catalog_set_locators(fx.catalog, fx.tid, chain[0], 0) == CATALOG_OK);
    assert(catalog_store_save(fx.file, fx.catalog) == CATALOG_STORE_OK);
    fx_reopen(&fx);
    CHECK(!root_has_magic(&fx, HEAP_TREE_ROOT_MAGIC) && catalog_heap_root(fx.catalog, fx.tid) == chain[0],
          "체인으로 게시한 상태");
    CHECK(heap_matches_model(fx.heap), "LHD1 체인의 모든 행을 순서대로 읽음");

    target_t first[TARGETS], last;
    find_targets(fx.heap, first, &last);
    set_targets_model(first, true);
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    CHECK(update_targets(batch, first) == HEAP_OK, "체인 heap의 행 갱신");
    heap_batch_result_t result;
    assert(heap_batch_finish(batch, &result) == HEAP_OK);
    CHECK(contains_page(result.old_pages, result.old_count, chain[0]) &&
              contains_page(result.old_pages, result.old_count, chain[1]),
          "옛 체인 페이지를 모두 반환 대상에 넣음");
    CHECK(count_directory_pages(fx.file, result.new_pages, result.new_count) == 3, "트리 전체(루트와 잎 둘)를 새로 씀");
    assert(fx_save_finished(&fx, batch, &result) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    fx_reopen(&fx);
    CHECK(root_has_magic(&fx, HEAP_TREE_ROOT_MAGIC), "다음 변경으로 트리가 됨");
    CHECK(heap_matches_model(fx.heap), "변환 뒤 모든 행이 모델과 일치");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

static void test_legacy_tree_directory_is_read_and_replaced(void)
{
    printf("\n[test_legacy_tree_directory_is_read_and_replaced]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    load_many_pages(&fx, TREE_ROWS);
    uint32_t current[MAX_DIRS];
    assert(directory_pages(fx.heap, current, fx.file) == 3);
    size_t record_count = heap_record_page_count(fx.heap);
    uint32_t *record_ids = malloc(record_count * sizeof(*record_ids));
    assert(record_ids != NULL);
    size_t copied = 0;
    for (int leaf = 0; leaf < 2; leaf++) {
        uint8_t body[PAGE_BODY_BYTES];
        assert(page_file_read(fx.file, current[leaf + 1], PAGE_KIND_DIRECTORY, fx.tid, body) == PAGE_FILE_OK);
        assert(memcmp(body, HEAP_HINT_DIRECTORY_MAGIC, 4) == 0);
        uint32_t count = directory_u32(body + 16);
        for (uint32_t i = 0; i < count; i++)
            record_ids[copied++] = directory_u32(body + HEAP_DIRECTORY_HEADER_BYTES + (size_t)i * 6u);
    }
    assert(copied == record_count && record_count > HEAP_DIRECTORY_ENTRIES &&
           record_count <= HEAP_DIRECTORY_ENTRIES * 2u);
    uint32_t legacy_leaves[2], legacy_root;
    for (int leaf = 0; leaf < 2; leaf++) {
        uint8_t body[PAGE_BODY_BYTES] = {0};
        size_t first = (size_t)leaf * HEAP_DIRECTORY_ENTRIES;
        size_t count = record_count - first;
        if (count > HEAP_DIRECTORY_ENTRIES) count = HEAP_DIRECTORY_ENTRIES;
        memcpy(body, HEAP_DIRECTORY_MAGIC, 4);
        body[4] = HEAP_DIRECTORY_VERSION;
        directory_put_u32(body + 8, fx.tid);
        directory_put_u32(body + 16, (uint32_t)count);
        for (size_t i = 0; i < count; i++)
            directory_put_u32(body + HEAP_DIRECTORY_HEADER_BYTES + i * 4u, record_ids[first + i]);
        assert(page_file_alloc(fx.file, &legacy_leaves[leaf]) == PAGE_FILE_OK);
        assert(page_file_write(fx.file, legacy_leaves[leaf], PAGE_KIND_DIRECTORY, fx.tid, body) == PAGE_FILE_OK);
    }
    free(record_ids);
    uint8_t root_body[PAGE_BODY_BYTES] = {0};
    memcpy(root_body, HEAP_TREE_ROOT_MAGIC, 4);
    root_body[4] = HEAP_TREE_ROOT_VERSION;
    directory_put_u32(root_body + 8, fx.tid);
    directory_put_u32(root_body + 16, 2);
    directory_put_u32(root_body + HEAP_DIRECTORY_HEADER_BYTES, legacy_leaves[0]);
    directory_put_u32(root_body + HEAP_DIRECTORY_HEADER_BYTES + 4u, legacy_leaves[1]);
    assert(page_file_alloc(fx.file, &legacy_root) == PAGE_FILE_OK);
    assert(page_file_write(fx.file, legacy_root, PAGE_KIND_DIRECTORY, fx.tid, root_body) == PAGE_FILE_OK);
    assert(catalog_set_locators(fx.catalog, fx.tid, legacy_root, 0) == CATALOG_OK);
    assert(catalog_store_save(fx.file, fx.catalog) == CATALOG_STORE_OK);
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "옛 LHD2/LHD1 트리를 읽음");
    target_t first[TARGETS], last;
    find_targets(fx.heap, first, &last);
    set_targets_model(first, true);
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    CHECK(update_targets(batch, first) == HEAP_OK, "옛 트리에서 새 공간 힌트 트리로 변환");
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "옛 트리 변환 뒤 재열기와 모든 행이 일치");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

/* 잎이 한 장이면 여유 바이트가 저장된 LHD3 페이지 한 장만 쓴다. */
static void test_single_leaf_heap_keeps_hint_format(void)
{
    printf("\n[test_single_leaf_heap_keeps_hint_format]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    load_many_pages(&fx, 400);
    uint32_t dirs[MAX_DIRS];
    CHECK(directory_pages(fx.heap, dirs, fx.file) == 1 && root_has_magic(&fx, HEAP_HINT_DIRECTORY_MAGIC),
          "항목이 한 장 안이면 LHD3 한 장");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

static void test_deleted_space_reused_after_reopen(void)
{
    printf("\n[test_deleted_space_reused_after_reopen]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_locator_t old[6];
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    for (int id = 0; id < 6; id++) {
        model_put(id, 4);
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        assert(heap_batch_insert(batch, views, COLUMNS, &old[id]) == HEAP_OK);
    }
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    size_t original_pages = heap_record_page_count(fx.heap);
    CHECK(original_pages >= 3 && old[0].directory_index == 0,
          "여러 record page에 행을 분산해 빈 공간 재사용 조건 마련");
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    assert(heap_batch_delete(batch, old[0]) == HEAP_OK);
    clear_row(g_model[0].cells);
    g_model[0].present = false;
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    fx_reopen(&fx);
    CHECK(root_has_magic(&fx, HEAP_HINT_DIRECTORY_MAGIC), "재시작 뒤에도 LHD3 공간 힌트가 남음");
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    model_put(6, 4);
    value_view_t views[COLUMNS];
    to_views(g_model[6].cells, views);
    heap_locator_t inserted;
    assert(heap_batch_insert(batch, views, COLUMNS, &inserted) == HEAP_OK);
    CHECK(inserted.directory_index == old[0].directory_index && inserted.slot != old[0].slot,
          "앞쪽 페이지의 빈 바이트를 쓰되 삭제된 slot 번호는 재사용하지 않음");
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    CHECK(heap_record_page_count(fx.heap) == original_pages, "삭제 뒤 재삽입이 record page를 늘리지 않음");
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "재열기 뒤 기존 행과 새 행이 일치");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

static void test_stale_space_hint_is_rechecked(void)
{
    printf("\n[test_stale_space_hint_is_rechecked]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    heap_batch_t *batch;
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    for (int id = 0; id < 6; id++) {
        model_put(id, 4);
        value_view_t views[COLUMNS];
        to_views(g_model[id].cells, views);
        heap_locator_t locator;
        assert(heap_batch_insert(batch, views, COLUMNS, &locator) == HEAP_OK);
    }
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    /* 아래에서 게시된 디렉터리를 제자리에서 고친다. 최신 세대의 manifest가 대조하지 않게 빈 commit을 한다 */
    assert(page_file_commit(fx.file) == PAGE_FILE_OK);
    uint32_t root = catalog_heap_root(fx.catalog, fx.tid);
    uint8_t body[PAGE_BODY_BYTES];
    assert(page_file_read(fx.file, root, PAGE_KIND_DIRECTORY, fx.tid, body) == PAGE_FILE_OK);
    assert(memcmp(body, HEAP_HINT_DIRECTORY_MAGIC, 4) == 0);
    body[HEAP_DIRECTORY_HEADER_BYTES + 4u] = 0xff;
    body[HEAP_DIRECTORY_HEADER_BYTES + 5u] = 0xff;
    assert(page_file_write(fx.file, root, PAGE_KIND_DIRECTORY, fx.tid, body) == PAGE_FILE_OK);
    fx_reopen(&fx);
    assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
    model_put(6, 4);
    value_view_t views[COLUMNS];
    to_views(g_model[6].cells, views);
    heap_locator_t placed;
    CHECK(heap_batch_insert(batch, views, COLUMNS, &placed) == HEAP_OK,
          "실제 페이지가 가득 찼으면 과대한 힌트를 무시하고 다른 페이지를 고름");
    CHECK(placed.directory_index != 0, "과대한 첫 페이지 힌트를 그대로 믿지 않음");
    assert(fx_commit(&fx, batch) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    fx_reopen(&fx);
    CHECK(heap_matches_model(fx.heap), "낡은 힌트 뒤 재시작에서도 모든 행이 일치");
    fx_close(&fx);
    model_reset();
    clean_dir();
}

/*
 * 트리 heap에서 한 단계씩 실패시킨다. 게시 전 실패는 abort 뒤 다시 열어도 옛 상태이고, 불확정이면 옛 상태나 새 상태
 * 중 하나다. 어느 경우든 같은 변경을 다시 해 성공하고, 이어지는 commit이 재사용 중인 잎을 덮어쓰지 않는다.
 */
static void test_tree_failures_keep_every_leaf_reachable(void)
{
    printf("\n[test_tree_failures_keep_every_leaf_reachable]\n");
    fx_t fx;
    fx_create(&fx);
    model_reset();
    load_many_pages(&fx, TREE_ROWS);
    target_t targets[TARGETS], last;
    find_targets(fx.heap, targets, &last);
    fx_close(&fx);
    size_t size;
    uint8_t *original = slurp(DB_PATH, &size);
    page_file_set_default_io(&HOOKED);
    static const char *names[] = {"pwrite", "pread", "fsync", "alloc"};
    int failures = 0, not_applied = 0, unknown = 0, new_state_after_unknown = 0;
    for (int kind = FAIL_PWRITE; kind <= FAIL_ALLOC; kind++) {
        for (int n = 1; n <= 200; n++) {
            disarm();
            spit(DB_PATH, original, size);
            fx_open_existing(&fx);
            set_targets_model(targets, true);
            heap_batch_t *batch;
            assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
            arm((fail_kind_t)kind, n);
            heap_status_t finish_status = HEAP_OK;
            catalog_store_status_t saved = CATALOG_STORE_OK;
            heap_status_t failure = update_targets(batch, targets);
            if (failure == HEAP_OK) saved = fx_publish(&fx, batch, &finish_status);
            bool triggered = failure != HEAP_OK || finish_status != HEAP_OK || saved != CATALOG_STORE_OK ||
                             (kind == FAIL_ALLOC ? g_alloc_calls >= n
                                                 : kind == FAIL_PWRITE   ? g_hooks.pwrite_calls >= n
                                                 : kind == FAIL_PREAD    ? g_hooks.pread_calls >= n
                                                                         : g_hooks.fsync_calls >= n);
            disarm();
            if (!triggered) {
                heap_batch_free(batch);
                fx_close(&fx);
                break;  /* 이 단계 번호가 이 변경의 호출 수를 넘었다 */
            }
            failures++;
            bool new_state = false;
            bool batch_stage = failure != HEAP_OK || finish_status != HEAP_OK;
            if (!batch_stage && (saved == CATALOG_STORE_OK || saved == CATALOG_STORE_UNRECLAIMED)) {
                heap_batch_free(batch);  /* 게시는 성공했다 */
                new_state = true;
            } else if (!batch_stage && saved == CATALOG_STORE_OUTCOME_UNKNOWN) {
                unknown++;
                heap_batch_free(batch);  /* 불확정이면 abort도 release도 하지 않는다 */
            } else {
                if (!batch_stage) not_applied++;
                CHECK(heap_batch_abort(batch) == HEAP_OK, "%s %d: abort", names[kind], n);
                heap_batch_free(batch);
            }
            fx_close(&fx);
            fx_open_existing(&fx);
            set_targets_model(targets, new_state);
            bool at_expected = heap_matches_model(fx.heap);
            if (!at_expected && !batch_stage && saved == CATALOG_STORE_OUTCOME_UNKNOWN) {
                set_targets_model(targets, true);
                at_expected = heap_matches_model(fx.heap);
                new_state = at_expected;
                new_state_after_unknown += at_expected;
            }
            CHECK(at_expected, "%s %d: 재시작 뒤 옛 상태 또는 새 상태가 온전히 읽힘", names[kind], n);

            /* 같은 변경을 다시 한다. 이미 새 상태면 건너뛴다 */
            if (!new_state) {
                set_targets_model(targets, true);
                assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
                CHECK(update_targets(batch, targets) == HEAP_OK, "%s %d: 재시도 갱신", names[kind], n);
                CHECK(fx_commit(&fx, batch) == CATALOG_STORE_OK, "%s %d: 재시도 게시", names[kind], n);
                heap_batch_free(batch);
            }
            CHECK(heap_matches_model(fx.heap), "%s %d: 재시도 뒤 새 상태", names[kind], n);
            /* 이어지는 commit을 여러 번 해 반환한 페이지가 재사용돼도 살아 있는 잎이 덮이지 않는지 본다 */
            for (int round = 0; round < 3; round++) {
                for (int i = 0; i < TARGETS; i++) model_put(targets[i].id, round % 2 == 0 ? 2 : 0);
                assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
                CHECK(update_targets(batch, targets) == HEAP_OK, "%s %d: 이어진 갱신 %d", names[kind], n, round);
                CHECK(fx_commit(&fx, batch) == CATALOG_STORE_OK, "%s %d: 이어진 게시 %d", names[kind], n, round);
                heap_batch_free(batch);
            }
            fx_reopen(&fx);
            CHECK(heap_matches_model(fx.heap), "%s %d: 이어진 commit 뒤 다시 열어도 일치", names[kind], n);
            fx_close(&fx);
            set_targets_model(targets, false);
        }
    }
    disarm();
    page_file_set_default_io(NULL);
    free(original);
    CHECK(failures > 20, "실패 지점을 충분히 지남 (%d)", failures);
    CHECK(not_applied > 0 && unknown > 0, "게시 전 실패(%d)와 불확정(%d) 경로를 모두 지남", not_applied, unknown);
    printf("  (실패 지점 %d, 제어 쓰기 전 게시 실패 %d, 불확정 %d, 그 중 새 상태 %d)\n", failures, not_applied, unknown,
           new_state_after_unknown);
    model_reset();
    clean_dir();
}

int main(void)
{
    /* 할당 치환이 적용된 빌드인지 확인한다. */
    void *probe = malloc(1);
    free(probe);
    g_alloc_total = 0;
    {
        fx_t fx;
        fx_create(&fx);
        heap_batch_t *batch;
        assert(heap_batch_begin(fx.heap, &batch) == HEAP_OK);
        heap_batch_free(batch);
        assert(g_alloc_total > 0);
        fx_close(&fx);
        clean_dir();
    }
    test_empty_heap_and_page_slot_layout();
    test_all_value_kinds_and_sizes_round_trip();
    test_wrong_inputs_are_rejected_without_change();
    test_update_delete_and_row_moves();
    test_many_pages_need_several_directory_pages();
    test_locator_kinds_are_distinguished();
    test_copy_on_write_keeps_published_pages();
    test_failures_never_change_published_state();
    test_one_row_change_rewrites_only_its_leaf();
    test_legacy_chain_directory_is_read_and_replaced();
    test_legacy_tree_directory_is_read_and_replaced();
    test_single_leaf_heap_keeps_hint_format();
    test_deleted_space_reused_after_reopen();
    test_stale_space_hint_is_rechecked();
    test_tree_failures_keep_every_leaf_reachable();
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
