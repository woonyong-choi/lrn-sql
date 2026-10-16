#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "sql/database.h"
#include "sql/result.h"
#include "storage/page_file.h"

/*
 * #180: 정수 기본 키 범위 SELECT의 비용 측정. 같은 데이터·같은 결과 소비에서 세 방식을 잰다.
 *   range   새 범위 경로(id >= ?)
 *   scan    변경 전과 같은 전체 스캔(id + 0 >= ?, 기본 키 조건으로 보지 않는다)
 *   sqlite  SQLite 3 (DELETE journal, synchronous=FULL, page 8192)
 * 시간 수치는 통과/실패 판정에 쓰지 않는다. 사용법: bench_primary_range <range|scan|sqlite> <행 수>
 */
#define RESULT_ROWS 100
#define QUERIES 200
#define WARMUP 20
#define BATCH 500

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

static size_t ours_exec(database_t *db, const char *sql)
{
    result_t *result = NULL;
    assert(database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result) == DATABASE_EXEC_OK);
    assert(result != NULL && result_kind(result) != RESULT_KIND_ERROR);
    size_t rows = 0;
    result_row_t row;
    while (result_next(result, &row) == RESULT_NEXT_ROW) rows++;
    result_release(&result);
    return rows;
}

static size_t sqlite_exec(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK);
    size_t rows = 0;
    int step;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) rows++;
    assert(step == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK);
    return rows;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double percentile_us(const uint64_t *sorted, size_t count, double p)
{
    size_t at = (size_t)(p * (double)(count - 1) + 0.5);
    return (double)sorted[at] / 1e3;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *mode = argv[1];
    bool sqlite = strcmp(mode, "sqlite") == 0;
    bool scan = strcmp(mode, "scan") == 0;
    assert(sqlite || scan || strcmp(mode, "range") == 0);
    long rows = strtol(argv[2], NULL, 10);
    assert(rows >= RESULT_ROWS && rows % BATCH == 0);
    const char *path = sqlite ? "build/bench_primary_range.db" : "build/bench_primary_range.lrdb";
    unlink(path);

    database_t *engine = NULL;
    sqlite3 *other = NULL;
    if (sqlite) {
        assert(sqlite3_open(path, &other) == SQLITE_OK);
        sqlite_exec(other, "PRAGMA page_size=8192");
        sqlite_exec(other, "PRAGMA mmap_size=0");
        sqlite_exec(other, "PRAGMA synchronous=FULL");
        sqlite_exec(other, "PRAGMA journal_mode=DELETE");
    } else {
        g_underlying = page_file_default_io();
        g_counted = g_underlying;
        g_counted.pread = count_read;
        page_file_set_default_io(&g_counted);
        assert(database_create(path, &engine) == DATABASE_OK);
    }
    const char *create = "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)";
    if (sqlite) sqlite_exec(other, create);
    else ours_exec(engine, create);

    char *sql = malloc(64 * BATCH + 64);
    assert(sql != NULL);
    for (long start = 1; start <= rows; start += BATCH) {
        size_t used = (size_t)sprintf(sql, "INSERT INTO t VALUES ");
        for (long id = start; id < start + BATCH; id++) {
            used += (size_t)sprintf(sql + used, "%s(%ld, 'value-%08ld')", id == start ? "" : ", ", id, id);
        }
        if (sqlite) sqlite_exec(other, sql);
        else ours_exec(engine, sql);
    }

    char query[128];
    snprintf(query, sizeof(query), scan ? "SELECT id, v FROM t WHERE id + 0 >= %ld" : "SELECT id, v FROM t WHERE id >= %ld",
             rows - RESULT_ROWS + 1);
    for (int i = 0; i < WARMUP; i++) assert((sqlite ? sqlite_exec(other, query) : ours_exec(engine, query)) == RESULT_ROWS);

    database_scan_stats_t before = {0}, after = {0};
    if (!sqlite) database_scan_stats(engine, &before);
    uint64_t reads_before = g_reads;
    uint64_t latency[QUERIES];
    for (int i = 0; i < QUERIES; i++) {
        uint64_t begin = now_ns();
        size_t got = sqlite ? sqlite_exec(other, query) : ours_exec(engine, query);
        latency[i] = now_ns() - begin;
        assert(got == RESULT_ROWS);
    }
    if (!sqlite) database_scan_stats(engine, &after);
    qsort(latency, QUERIES, sizeof(latency[0]), compare_u64);
    struct rusage usage;
    assert(getrusage(RUSAGE_SELF, &usage) == 0);
    printf("mode=%s rows=%ld result_rows=%d queries=%d p50_us=%.1f p95_us=%.1f p99_us=%.1f "
           "rows_scanned_per_query=%.1f index_pages_per_query=%.1f pread_per_query=%.1f maxrss_kb=%ld\n",
           mode, rows, RESULT_ROWS, QUERIES, percentile_us(latency, QUERIES, 0.50),
           percentile_us(latency, QUERIES, 0.95), percentile_us(latency, QUERIES, 0.99),
           (double)(after.rows_scanned - before.rows_scanned) / QUERIES,
           (double)(after.index_pages_read - before.index_pages_read) / QUERIES,
           (double)(g_reads - reads_before) / QUERIES, usage.ru_maxrss);
    free(sql);
    if (sqlite) assert(sqlite3_close(other) == SQLITE_OK);
    else {
        assert(database_close(engine) == DATABASE_OK);
        page_file_set_default_io(NULL);
    }
    unlink(path);
    return 0;
}
