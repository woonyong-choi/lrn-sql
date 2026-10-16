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
#include "sql/session.h"
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

static char g_dir[128], g_db_path[192];
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

/*
 * 큐는 세션으로 문장을 실행한다. 시험은 데이터베이스 하나를 풀 하나로 열고 그 세션을 db라고 부른다. 열린 연결은
 * 아래 표에 두고 close_db가 세션·데이터베이스·풀을 닫는다. 한도는 없다(예산 시험은 limits를 직접 준다).
 */
#define TEST_MAX_CONNS 8
typedef struct {
    session_pool_t *pool;
    session_db_t *db;
    session_t *session;
} test_conn_t;
static test_conn_t g_conns[TEST_MAX_CONNS];

static __attribute__((unused)) session_t *open_conn(const char *path, bool transactional, bool writable,
                                                    const session_limits_t *limits)
{
    static const session_limits_t unlimited = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0};
    for (size_t i = 0; i < TEST_MAX_CONNS; i++) {
        test_conn_t *c = &g_conns[i];
        if (c->session != NULL) continue;
        if (session_pool_open(limits != NULL ? limits : &unlimited, &c->pool) != SESSION_OK) return NULL;
        database_status_t st = DATABASE_OK;
        session_status_t made = transactional ? session_pool_open_transactional(c->pool, path, NULL, &c->db, &st)
                                              : session_pool_open_database(c->pool, path, writable, &c->db, &st);
        if (made != SESSION_OK || st != DATABASE_OK || session_open(c->db, &c->session) != SESSION_OK) {
            printf("  open failed session=%d database=%d\n", (int)made, (int)st);
            if (c->db != NULL) session_pool_close_database(c->db, NULL);
            session_pool_close(c->pool);
            *c = (test_conn_t){0};
            return NULL;
        }
        return c->session;
    }
    return NULL;
}

/* 트랜잭션 계층을 붙여 연다. */
static __attribute__((unused)) session_t *open_db(void)
{
    return open_conn(g_db_path, true, true, NULL);
}

/* 세션·데이터베이스·풀을 닫는다. 닫지 못하면 false */
static __attribute__((unused)) bool close_db(session_t *db)
{
    for (size_t i = 0; i < TEST_MAX_CONNS; i++) {
        test_conn_t *c = &g_conns[i];
        if (c->session != db || db == NULL) continue;
        bool ok = session_close(c->session) == SESSION_OK;
        database_status_t st = DATABASE_OK;
        ok = session_pool_close_database(c->db, &st) == SESSION_OK && st == DATABASE_OK && ok;
        ok = session_pool_close(c->pool) == SESSION_OK && ok;
        *c = (test_conn_t){0};
        return ok;
    }
    return false;
}

static __attribute__((unused)) database_t *raw_db(session_t *db)
{
    return session_db_handle(session_database(db));
}

/* 문장 작업 메모리와 이전된 결과가 모두 돌아왔고 계상 불변식이 깨지지 않았는지 */
static __attribute__((unused)) bool conn_settled(session_t *db)
{
    for (size_t i = 0; i < TEST_MAX_CONNS; i++) {
        if (g_conns[i].session != db || db == NULL) continue;
        session_stats_t stats;
        session_pool_stats_t pool;
        if (session_stats_get(db, &stats) != SESSION_OK || session_pool_stats(g_conns[i].pool, &pool) != SESSION_OK) {
            return false;
        }
        return stats.query_used == 0 && stats.result_used == 0 && pool.result_used == 0 && pool.accounting_faults == 0;
    }
    return false;
}

/* 같은 데이터베이스에 세션을 하나 더 열어 트랜잭션을 시작한다. 실패하면 NULL */
static __attribute__((unused)) session_t *begin_txn(session_t *db)
{
    session_t *txn = NULL;
    if (session_open(session_database(db), &txn) != SESSION_OK) return NULL;
    session_txn_options_t options = {TXN_GUARANTEE_DURABLE, 0};
    if (session_txn_begin(txn, &options) != TXN_OK) {
        session_close(txn);
        return NULL;
    }
    return txn;
}

/* 트랜잭션을 commit하고 그 세션을 닫는다. */
static __attribute__((unused)) txn_status_t commit_txn(session_t **txn)
{
    txn_status_t st = session_txn_commit(*txn, NULL);
    if (session_txn_active(*txn)) session_txn_rollback(*txn);
    session_close(*txn);
    *txn = NULL;
    return st;
}

static __attribute__((unused)) txn_status_t rollback_txn(session_t **txn)
{
    txn_status_t st = session_txn_rollback(*txn);
    session_close(*txn);
    *txn = NULL;
    return st;
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
static __attribute__((unused)) bool exec_ok(session_t *db, const char *sql)
{
    result_t *r = NULL;
    if (session_exec(db, NULL, (const uint8_t *)sql, strlen(sql), NULL, &r) != DATABASE_EXEC_OK || r == NULL) return false;
    bool ok = result_kind(r) != RESULT_KIND_ERROR;
    if (result_kind(r) == RESULT_KIND_ROWS) {
        result_row_t row;
        while (result_next(r, &row) == RESULT_NEXT_ROW) {}
    }
    result_release(&r);
    return ok;
}

/* 트랜잭션 안에서 문장 하나를 실행하고 오류 결과면 false */
static __attribute__((unused)) bool txn_ok(session_t *txn, const char *sql)
{
    result_t *r = NULL;
    if (session_exec(txn, NULL, (const uint8_t *)sql, strlen(sql), NULL, &r) != DATABASE_EXEC_OK || r == NULL) {
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
static __attribute__((unused)) int64_t scalar_int(session_t *db, const char *sql)
{
    result_t *r = NULL;
    if (session_exec(db, NULL, (const uint8_t *)sql, strlen(sql), NULL, &r) != DATABASE_EXEC_OK || r == NULL) return -1;
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

static __attribute__((unused)) ackq_status_t enq(ackq_t *q, session_t *txn, const char *payload, const char *key, uint64_t *id,
                         bool *dup)
{
    return ackq_enqueue(q, txn, (const uint8_t *)payload, strlen(payload), key, id, dup);
}

#endif
