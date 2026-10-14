/*
 * 한 행 변경(INSERT·UPDATE·DELETE) 한 번의 디렉터리 페이지 읽기·쓰기 수와 지연, DB 크기를 재는 로컬 계측 도구(#159).
 * 통과·실패를 판정하지 않고 CI에 넣지 않는다. 합성 데이터만 쓰며 파일은 인자로 받은 디렉터리 아래에 만든다.
 * 문장 하나는 database.c와 같은 순서(heap_open → batch → finish → catalog 게시 → 옛 페이지 release)로 흉내 낸다.
 * 빌드: gcc -O2 -std=c11 -Iinclude tests/measure_heap_directory.c <heap_store.c 변형> src/storage/{record,record_page,
 *       overflow,value,catalog,page_file,catalog_store,legacy_migrate}.c -lm -o build/measure_heap_directory
 * 실행: build/measure_heap_directory <작업 디렉터리> <디렉터리 페이지 수> [문장 수=30] [반복=3]
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/page_file.h"

#define ROW_PAYLOAD_BYTES 3000u
#define ROWS_PER_PAGE 2u
#define LOAD_ROWS_PER_COMMIT 4000u
#define SPARE_PAGES 64u  /* 계측 중 삽입이 디렉터리 페이지 수를 바꾸지 않게 남기는 여유 */
#define KIND_SLOTS 5u

typedef struct {
    unsigned long reads[KIND_SLOTS];
    unsigned long writes[KIND_SLOTS];
} io_counts_t;

static io_counts_t g_io;

static void count_kind(unsigned long *slots, const void *page, size_t size)
{
    if (size != PAGE_FILE_PAGE_BYTES) return;
    unsigned kind = ((const uint8_t *)page)[6];
    if (kind < KIND_SLOTS) slots[kind]++;
}

static ssize_t counting_pread(int fd, void *buf, size_t size, off_t offset)
{
    ssize_t done = pread(fd, buf, size, offset);
    if (done == (ssize_t)size) count_kind(g_io.reads, buf, size);
    return done;
}

static ssize_t counting_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    count_kind(g_io.writes, buf, size);
    return pwrite(fd, buf, size, offset);
}

static const page_file_io_t COUNTING = {counting_pread, counting_pwrite, fsync, link, unlink};
static const value_type_t TYPES[2] = {VALUE_INTEGER, VALUE_BLOB};

typedef struct {
    page_file_t *file;
    catalog_t *catalog;
    catalog_table_id_t tid;
    char path[512];
} db_t;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void db_create(db_t *db, const char *dir, int tag)
{
    snprintf(db->path, sizeof(db->path), "%s/m%d.lrdb", dir, tag);
    unlink(db->path);
    assert(page_file_create(db->path, &db->file) == PAGE_FILE_OK);
    assert(catalog_open_empty(&db->catalog) == CATALOG_OK);
    catalog_column_spec_t cols[2] = {{"id", 2, VALUE_INTEGER, false}, {"payload", 7, VALUE_BLOB, false}};
    catalog_table_spec_t spec = {"items", 5, cols, 2, CATALOG_NO_COLUMN, false};
    assert(catalog_create_table(db->catalog, &spec, &db->tid) == CATALOG_OK);
    assert(catalog_store_save(db->file, db->catalog) == CATALOG_STORE_OK);
    assert(page_file_publish(db->file) == PAGE_FILE_OK);
}

static uint32_t current_root(const db_t *db)
{
    catalog_table_view_t view;
    assert(catalog_describe(db->catalog, db->tid, &view) == CATALOG_OK);
    return view.heap_root;
}

typedef enum { OP_INSERT, OP_UPDATE, OP_DELETE } op_t;

/* 문장 하나. target은 UPDATE·DELETE의 locator, seed는 행 내용을 정한다. */
static void run_statement(db_t *db, op_t op, heap_locator_t target, int64_t seed)
{
    heap_schema_t schema = {TYPES, 2};
    heap_t *heap = NULL;
    heap_batch_t *batch = NULL;
    assert(heap_open(db->file, db->tid, current_root(db), &schema, &heap) == HEAP_OK);
    assert(heap_batch_begin(heap, &batch) == HEAP_OK);
    uint8_t payload[ROW_PAYLOAD_BYTES];
    memset(payload, (int)(seed & 0xFF), sizeof(payload));
    value_t cells[2] = {{{VALUE_NULL, 0, 0, NULL, 0}, false}, {{VALUE_NULL, 0, 0, NULL, 0}, false}};
    assert(value_set_integer(&cells[0], seed) == VALUE_OK);
    assert(value_set_blob(&cells[1], payload, sizeof(payload)) == VALUE_OK);
    value_view_t views[2] = {value_as_view(&cells[0]), value_as_view(&cells[1])};
    heap_locator_t placed;
    if (op == OP_INSERT) assert(heap_batch_insert(batch, views, 2, &placed) == HEAP_OK);
    if (op == OP_UPDATE) assert(heap_batch_update(batch, target, views, 2, &placed) == HEAP_OK);
    if (op == OP_DELETE) assert(heap_batch_delete(batch, target) == HEAP_OK);
    value_clear(&cells[0]);
    value_clear(&cells[1]);
    heap_batch_result_t result;
    assert(heap_batch_finish(batch, &result) == HEAP_OK);
    assert(catalog_set_locators(db->catalog, db->tid, result.new_root, 0) == CATALOG_OK);
    assert(catalog_store_save(db->file, db->catalog) == CATALOG_STORE_OK);
    assert(heap_batch_release_old(batch) == HEAP_OK);
    heap_batch_free(batch);
    heap_close(heap);
}

/* 디렉터리 페이지 dir_pages장이 되도록 record page를 채운다. 적재 I/O는 계측에서 뺀다. */
static void load(db_t *db, uint32_t dir_pages)
{
    uint64_t rows = ((uint64_t)dir_pages * HEAP_DIRECTORY_ENTRIES - SPARE_PAGES) * ROWS_PER_PAGE;
    heap_schema_t schema = {TYPES, 2};
    uint8_t payload[ROW_PAYLOAD_BYTES];
    memset(payload, 7, sizeof(payload));
    for (uint64_t done = 0; done < rows;) {
        heap_t *heap = NULL;
        heap_batch_t *batch = NULL;
        assert(heap_open(db->file, db->tid, current_root(db), &schema, &heap) == HEAP_OK);
        assert(heap_batch_begin(heap, &batch) == HEAP_OK);
        for (uint32_t i = 0; i < LOAD_ROWS_PER_COMMIT && done < rows; i++, done++) {
            value_t cells[2] = {{{VALUE_NULL, 0, 0, NULL, 0}, false}, {{VALUE_NULL, 0, 0, NULL, 0}, false}};
            assert(value_set_integer(&cells[0], (int64_t)done) == VALUE_OK);
            assert(value_set_blob(&cells[1], payload, sizeof(payload)) == VALUE_OK);
            value_view_t views[2] = {value_as_view(&cells[0]), value_as_view(&cells[1])};
            heap_locator_t placed;
            assert(heap_batch_insert(batch, views, 2, &placed) == HEAP_OK);
            value_clear(&cells[0]);
            value_clear(&cells[1]);
        }
        heap_batch_result_t result;
        assert(heap_batch_finish(batch, &result) == HEAP_OK);
        assert(catalog_set_locators(db->catalog, db->tid, result.new_root, 0) == CATALOG_OK);
        assert(catalog_store_save(db->file, db->catalog) == CATALOG_STORE_OK);
        assert(heap_batch_release_old(batch) == HEAP_OK);
        heap_batch_free(batch);
        heap_close(heap);
    }
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void measure(db_t *db, const char *name, op_t op, uint32_t statements, uint32_t first_page,
                    uint32_t rep)
{
    double *millis = malloc(statements * sizeof(*millis));
    assert(millis != NULL);
    io_counts_t before = g_io;
    for (uint32_t i = 0; i < statements; i++) {
        heap_locator_t target = {first_page + i * 3u, op == OP_DELETE ? 1u : 0u};
        double start = now_ms();
        run_statement(db, op, target, (int64_t)(1000000 + rep * 1000 + i));
        millis[i] = now_ms() - start;
    }
    qsort(millis, statements, sizeof(*millis), compare_double);
    printf("%-6s rep=%u stmts=%u dir_reads/stmt=%.2f dir_writes/stmt=%.2f rec_writes/stmt=%.2f "
           "median_ms=%.3f p90_ms=%.3f\n",
           name, rep, statements,
           (double)(g_io.reads[PAGE_KIND_DIRECTORY] - before.reads[PAGE_KIND_DIRECTORY]) / statements,
           (double)(g_io.writes[PAGE_KIND_DIRECTORY] - before.writes[PAGE_KIND_DIRECTORY]) / statements,
           (double)(g_io.writes[PAGE_KIND_RECORD] - before.writes[PAGE_KIND_RECORD]) / statements,
           millis[statements / 2], millis[statements * 9 / 10]);
    free(millis);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fputs("usage: measure_heap_directory <dir> <directory pages> [statements] [repeats]\n", stderr);
        return 2;
    }
    uint32_t dir_pages = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t statements = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 30;
    uint32_t repeats = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 3;
    page_file_set_default_io(&COUNTING);
    for (uint32_t rep = 1; rep <= repeats; rep++) {
        db_t db;
        db_create(&db, argv[1], (int)rep);
        load(&db, dir_pages);
        struct stat loaded;
        assert(stat(db.path, &loaded) == 0);
        printf("rep=%u dir_pages=%u record_pages_target=%u loaded_bytes=%lld\n", rep, dir_pages,
               dir_pages * (uint32_t)HEAP_DIRECTORY_ENTRIES - SPARE_PAGES, (long long)loaded.st_size);
        measure(&db, "insert", OP_INSERT, statements, 0, rep);
        measure(&db, "update", OP_UPDATE, statements, 10, rep);
        measure(&db, "delete", OP_DELETE, statements, 11, rep);
        struct stat done;
        assert(stat(db.path, &done) == 0);
        printf("rep=%u final_bytes=%lld\n", rep, (long long)done.st_size);
        catalog_close(db.catalog);
        page_file_close(db.file);
        unlink(db.path);
    }
    return 0;
}
