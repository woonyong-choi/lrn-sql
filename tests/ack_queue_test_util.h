#ifndef ACK_QUEUE_TEST_UTIL_H
#define ACK_QUEUE_TEST_UTIL_H

/*
 * ACK 큐 시험 공용 보조(#140). 합성 데이터만 쓴다. 시험 파일마다 directory를 정해 set_dir로 알려 준다.
 * 모든 함수는 static이라 포함한 시험 파일 안에서만 쓴다.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "queue/ack_queue.h"
#include "sql/database_txn.h"
#include "sql/result.h"

static int g_run, g_fail;

#define CHECK(cond, ...)                              \
    do {                                              \
        g_run++;                                      \
        if (!(cond)) {                                \
            g_fail++;                                 \
            printf("  FAIL (line %d): ", __LINE__);   \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
        }                                             \
    } while (0)

static char g_dir[128], g_db_path[192], g_wal_path[192];
static uint64_t g_now;

static __attribute__((unused)) uint64_t fake_clock(void *ctx)
{
    (void)ctx;
    return g_now;
}

static __attribute__((unused)) void set_dir(const char *dir)
{
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    snprintf(g_db_path, sizeof(g_db_path), "%s/db.lrdb", dir);
    snprintf(g_wal_path, sizeof(g_wal_path), "%s/db.wal", dir);
}

static __attribute__((unused)) void fresh_dir(void)
{
    DIR *d = opendir(g_dir);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", g_dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    } else {
        mkdir(g_dir, 0700);
    }
}

/* 시험이 모두 통과했을 때 작업 directory를 지운다. 실패하면 조사하도록 남긴다. */
static __attribute__((unused)) void remove_dir(void)
{
    fresh_dir();
    rmdir(g_dir);
}

static __attribute__((unused)) void make_db(void)
{
    fresh_dir();
    database_t *db = NULL;
    assert(database_create(g_db_path, &db) == DATABASE_OK);
    assert(database_close(db) == DATABASE_OK);
}

/* 트랜잭션 계층을 붙여 연다. with_wal이면 짝 WAL로 연다(처음이면 durable로 전환한다). */
static __attribute__((unused)) database_t *open_db(bool with_wal)
{
    database_txn_options_t o = {0};
    o.wal_path = with_wal ? g_wal_path : NULL;
    o.convert = with_wal;
    database_t *db = NULL;
    database_txn_info_t info;
    database_status_t st = database_open_transactional(g_db_path, &o, &db, &info);
    if (st != DATABASE_OK) {
        printf("  open failed status=%d pair=%d\n", (int)st, info.pair_status);
        return NULL;
    }
    return db;
}

static __attribute__((unused)) ackq_options_t qopts(const char *name, uint32_t max_attempts)
{
    ackq_options_t o = {0};
    o.name = name;
    o.max_attempts = max_attempts;
    o.now_ms = fake_clock;
    return o;
}

/* 문장 하나를 자동 commit으로 실행하고 오류 결과면 false */
static __attribute__((unused)) bool exec_ok(database_t *db, const char *sql)
{
    result_t *r = NULL;
    if (database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &r) != DATABASE_EXEC_OK || r == NULL) return false;
    bool ok = result_kind(r) != RESULT_KIND_ERROR;
    if (result_kind(r) == RESULT_KIND_ROWS) {
        result_row_t row;
        while (result_next(r, &row) == RESULT_NEXT_ROW) {}
    }
    result_release(&r);
    return ok;
}

/* 트랜잭션 안에서 문장 하나를 실행하고 오류 결과면 false */
static __attribute__((unused)) bool txn_ok(database_txn_t *txn, const char *sql)
{
    result_t *r = NULL;
    if (database_txn_exec(txn, NULL, (const uint8_t *)sql, strlen(sql), NULL, &r) != DATABASE_EXEC_OK || r == NULL) {
        return false;
    }
    bool ok = result_kind(r) != RESULT_KIND_ERROR;
    if (result_kind(r) == RESULT_KIND_ROWS) {
        result_row_t row;
        while (result_next(r, &row) == RESULT_NEXT_ROW) {}
    }
    result_release(&r);
    return ok;
}

/* 첫 행 첫 칸(INTEGER)을 돌려준다. 행이 없거나 정수가 아니면 -1 */
static __attribute__((unused)) int64_t scalar_int(database_t *db, const char *sql)
{
    result_t *r = NULL;
    if (database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &r) != DATABASE_EXEC_OK || r == NULL) return -1;
    int64_t value = -1;
    if (result_kind(r) == RESULT_KIND_ROWS) {
        result_row_t row;
        bool first = true;
        while (result_next(r, &row) == RESULT_NEXT_ROW) {
            if (first && row.cell_count > 0 && row.cells[0].type == VALUE_INTEGER) value = row.cells[0].integer;
            first = false;
        }
    }
    result_release(&r);
    return value;
}

static __attribute__((unused)) ackq_status_t enq(ackq_t *q, database_txn_t *txn, const char *payload, const char *key, uint64_t *id,
                         bool *dup)
{
    return ackq_enqueue(q, txn, (const uint8_t *)payload, strlen(payload), key, id, dup);
}

#endif
