#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "sql/database.h"
#include "sql/result.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/page_file.h"

/* 삭제 후 동일 크기 행을 재삽입한다. 시간은 진단 수치이며 성공 조건은 행 수다. */
#define ROWS 80
#define REPLACEMENTS 40
#define PAYLOAD 3000

static page_file_io_t base_io;
static page_file_io_t counted_io;
static uint64_t reads;

static ssize_t read_counted(int fd, void *buffer, size_t size, off_t offset)
{
    reads++;
    return base_io.pread(fd, buffer, size, offset);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void run_ours(database_t *db, const char *sql, size_t expected)
{
    result_t *result = NULL;
    assert(database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result) == DATABASE_EXEC_OK);
    assert(result != NULL);
    if (expected != SIZE_MAX) {
        size_t rows = 0;
        result_row_t row;
        while (result_next(result, &row) == RESULT_NEXT_ROW) rows++;
        assert(rows == expected);
    }
    result_release(&result);
}

static void run_sqlite(sqlite3 *db, const char *sql, size_t expected)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    size_t rows = 0;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) rows++;
    assert(step == SQLITE_DONE);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    if (expected != SIZE_MAX) assert(rows == expected);
}

static void run(database_t *ours, sqlite3 *sqlite, const char *sql, size_t expected)
{
    if (ours != NULL) run_ours(ours, sql, expected);
    else run_sqlite(sqlite, sql, expected);
}

static size_t record_pages(const char *path)
{
    page_file_t *file = NULL;
    page_file_open_info_t info;
    assert(page_file_open(path, false, &file, &info) == PAGE_FILE_OK);
    catalog_t *catalog = NULL;
    assert(catalog_store_load(file, &catalog) == CATALOG_STORE_OK);
    catalog_table_id_t id;
    assert(catalog_find(catalog, "t", 1, &id) == CATALOG_OK);
    catalog_table_view_t table;
    assert(catalog_describe(catalog, id, &table) == CATALOG_OK);
    value_type_t types[2] = {table.columns[0].type, table.columns[1].type};
    heap_schema_t schema = {types, 2};
    heap_t *heap = NULL;
    assert(heap_open(file, id, table.heap_root, &schema, &heap) == HEAP_OK);
    size_t pages = heap_record_page_count(heap);
    heap_close(heap);
    catalog_close(catalog);
    assert(page_file_close(file) == PAGE_FILE_OK);
    return pages;
}

static long long file_bytes(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return (long long)st.st_size;
}

static size_t sqlite_pragma_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    size_t value = (size_t)sqlite3_column_int64(stmt, 0);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    return value;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool ours_mode = strcmp(argv[1], "ours") == 0;
    assert(ours_mode || strcmp(argv[1], "sqlite") == 0);
    const char *path = ours_mode ? "build/bench_free_space_ours.lrdb" : "build/bench_free_space_sqlite.db";
    unlink(path);
    database_t *ours = NULL;
    sqlite3 *sqlite = NULL;
    if (ours_mode) {
        base_io = page_file_default_io();
        counted_io = base_io;
        counted_io.pread = read_counted;
        page_file_set_default_io(&counted_io);
        assert(database_create(path, &ours) == DATABASE_OK);
    } else {
        assert(sqlite3_open(path, &sqlite) == SQLITE_OK);
        run_sqlite(sqlite, "PRAGMA page_size=8192", SIZE_MAX);
        run_sqlite(sqlite, "PRAGMA cache_size=-2048", SIZE_MAX);
        run_sqlite(sqlite, "PRAGMA mmap_size=0", SIZE_MAX);
        run_sqlite(sqlite, "PRAGMA synchronous=FULL", SIZE_MAX);
        run_sqlite(sqlite, "PRAGMA journal_mode=DELETE", 1);
    }
    run(ours, sqlite, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)", SIZE_MAX);
    char payload[PAYLOAD + 1];
    memset(payload, 'a', PAYLOAD);
    payload[PAYLOAD] = 0;
    char sql[PAYLOAD + 100];
    for (int i = 1; i <= ROWS; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, payload);
        run(ours, sqlite, sql, SIZE_MAX);
    }
    for (int i = 1; i <= REPLACEMENTS; i++) {
        snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id = %d", i);
        run(ours, sqlite, sql, SIZE_MAX);
    }
    size_t pages_before = 0, free_before = 0;
    if (!ours_mode) {
        pages_before = sqlite_pragma_int(sqlite, "PRAGMA page_count");
        free_before = sqlite_pragma_int(sqlite, "PRAGMA freelist_count");
    }
    if (ours_mode) assert(database_close(ours) == DATABASE_OK);
    else assert(sqlite3_close(sqlite) == SQLITE_OK);
    ours = NULL;
    sqlite = NULL;
    if (ours_mode) pages_before = record_pages(path);
    long long bytes_before = file_bytes(path);
    if (ours_mode) assert(database_open(path, true, &ours) == DATABASE_OK);
    else assert(sqlite3_open(path, &sqlite) == SQLITE_OK);
    uint64_t reads_before = reads;
    uint64_t start = now_ns();
    for (int i = ROWS + 1; i <= ROWS + REPLACEMENTS; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", i, payload);
        run(ours, sqlite, sql, SIZE_MAX);
    }
    uint64_t elapsed = now_ns() - start;
    uint64_t insert_reads = reads - reads_before;
    run(ours, sqlite, "SELECT id FROM t", ROWS);
    size_t pages_after = 0, free_after = 0;
    if (!ours_mode) {
        pages_after = sqlite_pragma_int(sqlite, "PRAGMA page_count");
        free_after = sqlite_pragma_int(sqlite, "PRAGMA freelist_count");
    }
    if (ours_mode) assert(database_close(ours) == DATABASE_OK);
    else assert(sqlite3_close(sqlite) == SQLITE_OK);
    if (ours_mode) pages_after = record_pages(path);
    struct rusage usage;
    assert(getrusage(RUSAGE_SELF, &usage) == 0);
    printf("mode=%s rows=%d replacements=%d payload=%d record_pages_before=%zu record_pages_after=%zu "
           "freelist_before=%zu freelist_after=%zu file_bytes_before=%lld file_bytes_after=%lld "
           "replacement_ms=%.3f replacement_pread=%llu maxrss_bytes=%ld\n",
           argv[1], ROWS, REPLACEMENTS, PAYLOAD, pages_before, pages_after,
           free_before, free_after, bytes_before, file_bytes(path), elapsed / 1e6,
           (unsigned long long)insert_reads, usage.ru_maxrss);
    if (ours_mode) page_file_set_default_io(NULL);
    return 0;
}
