#include <assert.h>
#include <stdbool.h>
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
#include "storage/page_file.h"

/* 같은 SQL과 결과 소비를 거친 탐색 측정. 시간 수치는 통과/실패 판정에 쓰지 않는다. */
#define ROWS 1000
#define LOOKUPS 1000

static page_file_io_t g_underlying;
static page_file_io_t g_counted;
static uint64_t g_reads;

static ssize_t count_read(int fd, void *buffer, size_t size, off_t offset)
{
    g_reads++;
    return g_underlying.pread(fd, buffer, size, offset);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void ours_exec(database_t *db, const char *sql, size_t expected_rows)
{
    result_t *result = NULL;
    assert(database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result) == DATABASE_EXEC_OK);
    assert(result != NULL);
    if (expected_rows == SIZE_MAX) assert(result_kind(result) == RESULT_KIND_COMMAND);
    else {
        assert(result_kind(result) == RESULT_KIND_ROWS);
        size_t rows = 0;
        result_row_t row;
        while (result_next(result, &row) == RESULT_NEXT_ROW) {
            assert(row.cell_count == 1 && row.cells[0].type == VALUE_INTEGER);
            rows++;
        }
        assert(rows == expected_rows);
    }
    result_release(&result);
}

static void sqlite_exec(sqlite3 *db, const char *sql, size_t expected_rows)
{
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK);
    size_t rows = 0;
    int step;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        if (strncmp(sql, "SELECT id", 9) == 0)
            assert(sqlite3_column_count(statement) == 1 && sqlite3_column_type(statement, 0) == SQLITE_INTEGER);
        rows++;
    }
    assert(step == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK);
    if (expected_rows != SIZE_MAX) assert(rows == expected_rows);
}

static off_t file_bytes(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool ours = strcmp(argv[1], "ours") == 0;
    bool wal = strcmp(argv[1], "wal") == 0;
    assert(ours || wal || strcmp(argv[1], "delete") == 0);
    const char *path = ours ? "build/bench_secondary_ours.lrdb" :
                       wal ? "build/bench_secondary_wal.db" : "build/bench_secondary_delete.db";
    char sidecar[160];
    unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    unlink(sidecar);

    database_t *engine = NULL;
    sqlite3 *sqlite = NULL;
    if (ours) {
        g_underlying = page_file_default_io();
        g_counted = g_underlying;
        g_counted.pread = count_read;
        page_file_set_default_io(&g_counted);
        assert(database_create(path, &engine) == DATABASE_OK);
    }
    else {
        assert(sqlite3_open(path, &sqlite) == SQLITE_OK);
        sqlite_exec(sqlite, "PRAGMA page_size=8192", SIZE_MAX);
        sqlite_exec(sqlite, "PRAGMA cache_size=-2048", SIZE_MAX);
        sqlite_exec(sqlite, "PRAGMA mmap_size=0", SIZE_MAX);
        sqlite_exec(sqlite, "PRAGMA synchronous=FULL", SIZE_MAX);
        sqlite_exec(sqlite, wal ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE", 1);
    }
    const char *create = "CREATE TABLE t (id INTEGER PRIMARY KEY, k TEXT, v TEXT)";
    const char *index = "CREATE INDEX by_k ON t (k)";
    if (ours) { ours_exec(engine, create, SIZE_MAX); ours_exec(engine, index, SIZE_MAX); }
    else { sqlite_exec(sqlite, create, SIZE_MAX); sqlite_exec(sqlite, index, SIZE_MAX); }

    uint64_t begin = now_ns();
    for (int i = 1; i <= ROWS; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'k%04d', 'same')", i, i);
        if (ours) ours_exec(engine, sql, SIZE_MAX);
        else sqlite_exec(sqlite, sql, SIZE_MAX);
    }
    uint64_t insert_ns = now_ns() - begin;

    const char *hit = "SELECT id FROM t WHERE k = 'k0500'";
    for (int i = 0; i < 50; i++) {
        if (ours) ours_exec(engine, hit, 1);
        else sqlite_exec(sqlite, hit, 1);
    }
    begin = now_ns();
    uint64_t reads_before = g_reads;
    for (int i = 0; i < LOOKUPS; i++) {
        if (ours) ours_exec(engine, hit, 1);
        else sqlite_exec(sqlite, hit, 1);
    }
    uint64_t lookup_ns = now_ns() - begin;
    uint64_t lookup_reads = g_reads - reads_before;
    database_scan_stats_t stats = {0};
    if (ours) database_scan_stats(engine, &stats);
    struct rusage usage;
    assert(getrusage(RUSAGE_SELF, &usage) == 0);
    off_t bytes = file_bytes(path);
    if (!ours && wal) {
        snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
        bytes += file_bytes(sidecar);
        snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
        bytes += file_bytes(sidecar);
    }
    printf("mode=%s rows=%d lookups=%d insert_ms=%.3f lookup_us=%.3f maxrss_bytes=%ld open_file_bytes=%lld index_lookups=%llu rows_scanned=%llu lookup_pread=%llu\n",
           argv[1], ROWS, LOOKUPS, insert_ns / 1e6, lookup_ns / (double)LOOKUPS / 1e3,
           usage.ru_maxrss, (long long)bytes, (unsigned long long)stats.index_lookups,
           (unsigned long long)stats.rows_scanned, (unsigned long long)lookup_reads);
    if (ours) assert(database_close(engine) == DATABASE_OK);
    else assert(sqlite3_close(sqlite) == SQLITE_OK);
    if (ours) page_file_set_default_io(NULL);
    return 0;
}
