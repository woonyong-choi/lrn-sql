/*
 * test_session_txn.c -- session 위의 트랜잭션(#8): BEGIN·COMMIT·ROLLBACK 문장과 C API가 같은 트랜잭션을 쓰는지, 서로 다른
 * 세션(연결) 사이에서 미확정 값이 보이지 않고 writer가 직렬화되며 rollback·close 뒤 다음 writer가 실행되는지, 풀의
 * 예산 계상(작업 카탈로그·페이지 목록)이 열린 트랜잭션 동안 유지되고 끝나면 돌아오는지, commit 뒤 재시작을 실제 파일로 확인한다.
 *
 * 합성 데이터만 쓴다. 다루지 않는 것: 전원 장애, 큐 ACK(#140), 여러 스레드가 같은 세션을 쓰는 경우(세션은 한 번에 한 스레드).
 *
 * 빌드와 실행: make test-transaction
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sql/session.h"

#define DIR_NAME "__test__session_txn"
#define DB DIR_NAME "/db.lrdb"

static int g_run, g_fail;

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

static void fresh_dir(void)
{
    DIR *d = opendir(DIR_NAME);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", DIR_NAME, e->d_name);
            unlink(path);
        }
        closedir(d);
    } else {
        mkdir(DIR_NAME, 0700);
    }
}

/* ── 결과를 한 줄로 ── */

typedef struct {
    char *text;
    size_t len, cap;
} buffer_t;

static void put(buffer_t *b, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void put(buffer_t *b, const char *format, ...)
{
    char piece[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(piece, sizeof(piece), format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(piece));
    if (b->len + (size_t)n + 1 > b->cap) {
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->text = realloc(b->text, b->cap);
        assert(b->text != NULL);
    }
    memcpy(b->text + b->len, piece, (size_t)n + 1);
    b->len += (size_t)n;
}

static int compare_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *dump(result_t *result)
{
    buffer_t out = {NULL, 0, 0};
    put(&out, "%s", "");
    switch (result_kind(result)) {
        case RESULT_KIND_COMMAND: {
            const result_command_t *c = result_command(result);
            put(&out, "CMD %.*s %llu", (int)c->tag_len, c->tag, (unsigned long long)c->affected_rows);
            break;
        }
        case RESULT_KIND_ERROR: {
            result_row_t row;
            result_next(result, &row);
            const result_error_t *e = result_outcome(result).error;
            put(&out, "ERR %d/%d", (int)e->error_class, e->code);
            break;
        }
        default: {
            char **lines = NULL;
            size_t count = 0, cap = 0;
            result_row_t row;
            while (result_next(result, &row) == RESULT_NEXT_ROW) {
                buffer_t line = {NULL, 0, 0};
                put(&line, "%s", "");
                for (size_t i = 0; i < row.cell_count; i++) {
                    const value_view_t *cell = &row.cells[i];
                    if (i > 0) put(&line, "|");
                    if (cell->type == VALUE_INTEGER) put(&line, "%lld", (long long)cell->integer);
                    else if (cell->type == VALUE_TEXT) put(&line, "'%.*s'", (int)cell->size_bytes, cell->bytes);
                    else put(&line, "?");
                }
                if (count == cap) {
                    cap = cap == 0 ? 16 : cap * 2;
                    lines = realloc(lines, cap * sizeof(*lines));
                }
                lines[count++] = line.text;
            }
            if (count > 1) qsort(lines, count, sizeof(*lines), compare_lines);
            put(&out, "[%zu]", count);
            for (size_t i = 0; i < count; i++) {
                put(&out, "%s%s", i > 0 ? "/ " : " ", lines[i]);
                free(lines[i]);
            }
            free(lines);
        }
    }
    return out.text;
}

static char *run(session_t *s, const char *sql)
{
    result_t *result = NULL;
    database_exec_status_t st = session_exec(s, NULL, (const uint8_t *)sql, strlen(sql), NULL, &result);
    if (st != DATABASE_EXEC_OK || result == NULL) return strdup("EXEC-STATUS-FAIL");
    char *line = dump(result);
    result_release(&result);
    return line;
}


static void expect(session_t *s, const char *sql, const char *expected)
{
    char *line = run(s, sql);
    CHECK(strcmp(line, expected) == 0, "%s\n      기대: %s\n      실제: %s", sql, expected, line);
    free(line);
}

static void ok(session_t *s, const char *sql)
{
    char *l = run(s, sql);
    CHECK(strncmp(l, "CMD ", 4) == 0, "%s -> %s", sql, l);
    free(l);
}

/* ── 풀과 데이터베이스 ── */

static session_limits_t limits(size_t shared)
{
    session_limits_t l = {shared, shared, shared, 16, 0};
    return l;
}

typedef struct {
    session_pool_t *pool;
    session_db_t *db;
} env_t;

static void create_plain_file(void)
{
    fresh_dir();
    database_t *db = NULL;
    assert(database_create(DB, &db) == DATABASE_OK);
    assert(database_close(db) == DATABASE_OK);
}

static bool open_env(env_t *env, size_t shared)
{
    session_limits_t l = limits(shared);
    if (session_pool_open(&l, &env->pool) != SESSION_OK) return false;
    database_status_t ds = DATABASE_OK;
    session_status_t ss = session_pool_open_transactional(env->pool, DB, NULL, &env->db, &ds);
    if (ss != SESSION_OK) {
        printf("  open 실패 session=%d database=%d\n", (int)ss, (int)ds);
        session_pool_close(env->pool);
        return false;
    }
    return true;
}

static void close_env(env_t *env)
{
    database_status_t ds;
    session_status_t st = session_pool_close_database(env->db, &ds);
    CHECK(st == SESSION_OK && ds == DATABASE_OK, "close database %d/%d", (int)st, (int)ds);
    CHECK(session_pool_close(env->pool) == SESSION_OK, "close pool");
}

static void schema(session_t *s)
{
    ok(s, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, v TEXT)");
    ok(s, "CREATE TABLE u (k INTEGER, tag TEXT)");
}

/* ── 1. 문장으로 BEGIN·COMMIT·ROLLBACK, 서로 다른 세션 ── */

static void test_sql_control_two_sessions(void)
{
    printf("\n[BEGIN·COMMIT·ROLLBACK 문장과 두 세션]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *a = NULL, *b = NULL;
    assert(session_open(env.db, &a) == SESSION_OK && session_open(env.db, &b) == SESSION_OK);
    schema(a);
    ok(a, "INSERT INTO t (v) VALUES ('base')");

    expect(a, "BEGIN", "CMD BEGIN 0");
    CHECK(session_txn_active(a) && !session_txn_active(b), "a만 트랜잭션 안");
    ok(a, "INSERT INTO t (v) VALUES ('pending')");
    ok(a, "UPDATE t SET v = 'changed' WHERE id = 1");
    ok(a, "INSERT INTO u VALUES (1, 'one')");
    expect(a, "SELECT * FROM t", "[2] 1|'changed'/ 2|'pending'");
    /* 다른 연결은 미확정 값을 보지 못한다 */
    expect(b, "SELECT * FROM t", "[1] 1|'base'");
    expect(b, "SELECT * FROM u", "[0]");
    /* 다른 연결의 변경 문장과 BEGIN은 BUSY다 */
    expect(b, "INSERT INTO u VALUES (9, 'x')", "ERR 7/29");
    expect(b, "BEGIN", "ERR 7/29");
    expect(b, "COMMIT", "ERR 2/36");
    expect(a, "BEGIN", "ERR 2/35");
    expect(a, "COMMIT", "CMD COMMIT 0");
    CHECK(!session_txn_active(a), "commit 뒤 트랜잭션이 없다");
    expect(b, "SELECT * FROM t", "[2] 1|'changed'/ 2|'pending'");
    expect(b, "SELECT * FROM u", "[1] 1|'one'");

    /* rollback: 모두 버려지고 다른 세션의 writer가 실행된다 */
    expect(a, "START TRANSACTION", "CMD BEGIN 0");
    ok(a, "DELETE FROM t");
    ok(a, "INSERT INTO u VALUES (2, 'two')");
    expect(b, "SELECT * FROM t", "[2] 1|'changed'/ 2|'pending'");
    expect(a, "ROLLBACK", "CMD ROLLBACK 0");
    expect(a, "SELECT * FROM t", "[2] 1|'changed'/ 2|'pending'");
    expect(b, "BEGIN TRANSACTION", "CMD BEGIN 0");
    ok(b, "INSERT INTO u VALUES (3, 'three')");
    expect(b, "ROLLBACK TRANSACTION", "CMD ROLLBACK 0");
    expect(a, "ROLLBACK", "ERR 2/36");

    /* 세션을 닫으면 열린 트랜잭션이 rollback되고 다음 writer가 실행된다 */
    expect(a, "BEGIN", "CMD BEGIN 0");
    ok(a, "INSERT INTO u VALUES (4, 'four')");
    CHECK(session_close(a) == SESSION_OK, "close");
    a = NULL;
    expect(b, "SELECT * FROM u", "[1] 1|'one'");
    expect(b, "BEGIN", "CMD BEGIN 0");
    ok(b, "INSERT INTO u VALUES (5, 'five')");
    expect(b, "COMMIT", "CMD COMMIT 0");
    expect(b, "SELECT * FROM u", "[2] 1|'one'/ 5|'five'");
    session_close(b);
    close_env(&env);
}

/* ── 2. 문장 오류 뒤 rollback ── */

static void test_failed_transaction(void)
{
    printf("\n[실패한 트랜잭션]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *s = NULL;
    assert(session_open(env.db, &s) == SESSION_OK);
    schema(s);
    expect(s, "BEGIN", "CMD BEGIN 0");
    ok(s, "INSERT INTO u VALUES (1, 'one')");
    expect(s, "INSERT INTO nosuch VALUES (1)", "ERR 2/1");
    CHECK(session_txn_failed(s), "실패 상태");
    expect(s, "INSERT INTO u VALUES (2, 'two')", "ERR 2/26");
    expect(s, "COMMIT", "ERR 2/26");
    CHECK(session_txn_active(s) && session_txn_failed(s), "COMMIT 거절 뒤에도 rollback을 기다린다");
    txn_commit_info_t info;
    CHECK(session_txn_commit(s, &info) == TXN_INVALID, "C API commit도 거절");
    expect(s, "ROLLBACK", "CMD ROLLBACK 0");
    expect(s, "SELECT * FROM u", "[0]");
    session_close(s);
    close_env(&env);
}

/* ── 3. C API와 같은 트랜잭션 ── */

static void test_c_api(void)
{
    printf("\n[C API와 문장이 같은 트랜잭션을 쓴다]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *s = NULL, *other = NULL;
    assert(session_open(env.db, &s) == SESSION_OK && session_open(env.db, &other) == SESSION_OK);
    schema(s);
    session_txn_options_t o = {TXN_GUARANTEE_SYNCED, 0};
    CHECK(session_txn_begin(s, &o) == TXN_OK && session_txn_active(s), "C API begin");
    CHECK(session_txn_begin(s, &o) == TXN_INVALID, "이미 열려 있다");
    ok(s, "INSERT INTO t (v) VALUES ('via sql')");
    expect(other, "SELECT * FROM t", "[0]");
    CHECK(session_txn_begin(other, &o) == TXN_BUSY, "다른 세션의 begin은 BUSY");
    expect(s, "COMMIT", "CMD COMMIT 0");  /* 문장으로 끝낸다 */
    expect(other, "SELECT * FROM t", "[1] 1|'via sql'");
    CHECK(session_txn_commit(s, NULL) == TXN_NOT_ACTIVE, "열린 트랜잭션이 없다");

    expect(s, "BEGIN", "CMD BEGIN 0");  /* 문장으로 시작하고 C API로 끝낸다 */
    ok(s, "INSERT INTO t (v) VALUES ('via api')");
    txn_commit_info_t info;
    CHECK(session_txn_commit(s, &info) == TXN_OK && info.outcome == TXN_OUTCOME_APPLIED, "C API commit");
    expect(other, "SELECT * FROM t", "[2] 1|'via sql'/ 2|'via api'");
    CHECK(session_txn_begin(other, &o) == TXN_OK, "begin");
    ok(other, "DELETE FROM t");
    CHECK(session_txn_rollback(other) == TXN_OK && !session_txn_active(other), "C API rollback");
    CHECK(session_txn_rollback(other) == TXN_NOT_ACTIVE, "열린 트랜잭션이 없다");
    expect(s, "SELECT * FROM t", "[2] 1|'via sql'/ 2|'via api'");
    session_close(s);
    session_close(other);
    close_env(&env);
}

/* ── 4. 기다리는 BEGIN은 실행 잠금을 쥐지 않는다 ── */

typedef struct {
    session_t *session;
    char *line;
} waiter_t;

static void *waiter_main(void *arg)
{
    waiter_t *w = arg;
    session_set_begin_timeout(w->session, 5000);
    w->line = run(w->session, "BEGIN");
    return NULL;
}

static void test_waiting_begin(void)
{
    printf("\n[기다리는 BEGIN]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *a = NULL, *b = NULL;
    assert(session_open(env.db, &a) == SESSION_OK && session_open(env.db, &b) == SESSION_OK);
    schema(a);
    expect(a, "BEGIN", "CMD BEGIN 0");
    ok(a, "INSERT INTO u VALUES (1, 'a')");
    waiter_t w = {b, NULL};
    pthread_t th;
    pthread_create(&th, NULL, waiter_main, &w);
    usleep(150 * 1000);  /* b가 기다리는 중이다. a의 문장과 commit은 막히지 않아야 한다 */
    ok(a, "INSERT INTO u VALUES (2, 'b')");
    expect(a, "COMMIT", "CMD COMMIT 0");
    pthread_join(th, NULL);
    CHECK(w.line != NULL && strcmp(w.line, "CMD BEGIN 0") == 0, "기다리던 BEGIN이 성공한다: %s", w.line);
    free(w.line);
    expect(b, "SELECT * FROM u", "[2] 1|'a'/ 2|'b'");
    expect(b, "ROLLBACK", "CMD ROLLBACK 0");

    /* 시간 안에 풀리지 않으면 BUSY 결과다 */
    expect(a, "BEGIN", "CMD BEGIN 0");
    session_set_begin_timeout(b, 60);
    char *late = run(b, "BEGIN");
    CHECK(strcmp(late, "ERR 7/29") == 0, "timeout: %s", late);
    free(late);
    expect(a, "ROLLBACK", "CMD ROLLBACK 0");

    /* C API의 timeout */
    session_txn_options_t to = {TXN_GUARANTEE_SYNCED, 60};
    CHECK(session_txn_begin(a, &to) == TXN_OK, "begin");
    CHECK(session_txn_begin(b, &to) == TXN_TIMEOUT, "C API timeout");
    session_txn_rollback(a);
    session_close(a);
    session_close(b);
    close_env(&env);
}

/* ── 5. 예산 ── */

static void test_budget(void)
{
    printf("\n[풀 예산 계상]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *s = NULL;
    assert(session_open(env.db, &s) == SESSION_OK);
    schema(s);
    session_pool_stats_t base;
    session_pool_stats(env.pool, &base);
    expect(s, "BEGIN", "CMD BEGIN 0");
    for (int i = 0; i < 20; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO t (v) VALUES ('row %d')", i);
        ok(s, sql);
    }
    session_pool_stats_t during;
    session_pool_stats(env.pool, &during);
    CHECK(during.shared_used > base.shared_used, "열린 트랜잭션의 작업 카탈로그와 목록이 상주 몫으로 계상된다 (%zu -> %zu)",
          base.shared_used, during.shared_used);
    CHECK(during.accounting_faults == 0 && during.result_used == 0, "회계 불변식 위반 없음");
    expect(s, "ROLLBACK", "CMD ROLLBACK 0");
    session_pool_stats_t after;
    session_pool_stats(env.pool, &after);
    CHECK(after.shared_used <= base.shared_used + 8192,
          "rollback 뒤 작업 몫이 돌아온다 (%zu -> %zu -> %zu)", base.shared_used, during.shared_used, after.shared_used);
    CHECK(after.accounting_faults == 0, "회계 불변식 위반 없음");

    /* commit 뒤에도 작업 몫은 게시된 카탈로그 몫으로 바뀌어 한도 안이다 */
    expect(s, "BEGIN", "CMD BEGIN 0");
    ok(s, "INSERT INTO t (v) VALUES ('kept')");
    expect(s, "COMMIT", "CMD COMMIT 0");
    session_pool_stats_t committed;
    session_pool_stats(env.pool, &committed);
    CHECK(committed.accounting_faults == 0, "commit 뒤에도 불변식 위반 없음");
    session_close(s);
    close_env(&env);

    /* 예산이 모자라면 문장은 반영되지 않고 트랜잭션은 실패 상태가 된다 */
    create_plain_file();
    session_limits_t l = limits(0);
    session_pool_t *pool = NULL;
    /* 열 때의 몫을 알려면 한 번 열어 측정한다 */
    assert(session_pool_open(&(session_limits_t){64u << 20, 64u << 20, 64u << 20, 16, 0}, &pool) == SESSION_OK);
    database_txn_options_t o = {0};
    session_db_t *sdb = NULL;
    assert(session_pool_open_transactional(pool, DB, &o, &sdb, NULL) == SESSION_OK);
    session_pool_stats_t opened;
    session_pool_stats(pool, &opened);
    session_pool_close_database(sdb, NULL);
    session_pool_close(pool);
    l = limits(opened.shared_used + 64 * 1024);
    assert(session_pool_open(&l, &pool) == SESSION_OK);
    assert(session_pool_open_transactional(pool, DB, &o, &sdb, NULL) == SESSION_OK);
    assert(session_open(sdb, &s) == SESSION_OK);
    schema(s);
    expect(s, "BEGIN", "CMD BEGIN 0");
    /* 작업 목록은 새 페이지를 곧바로 재사용해 자라지 않으므로, 점점 큰 값으로 문장 임시 메모리가 예산을 넘게 한다 */
    bool refused = false;
    static char sql[256 * 1024];
    for (int i = 1; i <= 128 && !refused; i++) {
        int n = snprintf(sql, sizeof(sql), "INSERT INTO t (v) VALUES ('");
        memset(sql + n, 'x', (size_t)i * 2048);
        snprintf(sql + n + (size_t)i * 2048, sizeof(sql) - (size_t)n - (size_t)i * 2048, "')");
        char *line = run(s, sql);
        if (strncmp(line, "ERR ", 4) == 0) {
            refused = true;
            CHECK(strncmp(line, "ERR 5/", 6) == 0, "예산 거절은 NOMEM 오류 결과: %s", line);
        }
        free(line);
    }
    CHECK(refused, "작은 예산에서는 어느 시점에 거절된다");
    CHECK(session_txn_failed(s), "거절된 트랜잭션은 실패 상태");
    expect(s, "ROLLBACK", "CMD ROLLBACK 0");
    session_pool_stats_t end;
    session_pool_stats(pool, &end);
    CHECK(end.accounting_faults == 0, "예산 거절 뒤 불변식 위반 없음");
    expect(s, "SELECT * FROM t", "[0]");
    session_close(s);
    session_pool_close_database(sdb, NULL);
    session_pool_close(pool);
}

/* ── 6. commit과 재시작 ── */

static void test_commit_restart(void)
{
    printf("\n[BEGIN·COMMIT과 재시작]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *s = NULL;
    assert(session_open(env.db, &s) == SESSION_OK);
    schema(s);
    expect(s, "BEGIN", "CMD BEGIN 0");
    ok(s, "INSERT INTO t (v) VALUES ('d1')");
    ok(s, "INSERT INTO u VALUES (1, 'u1')");
    expect(s, "COMMIT", "CMD COMMIT 0");
    expect(s, "BEGIN", "CMD BEGIN 0");
    ok(s, "INSERT INTO t (v) VALUES ('rolled back')");
    expect(s, "ROLLBACK", "CMD ROLLBACK 0");
    expect(s, "START TRANSACTION", "CMD BEGIN 0");
    ok(s, "UPDATE t SET v = 'd2' WHERE id = 1");
    expect(s, "COMMIT", "CMD COMMIT 0");
    expect(s, "BEGIN TRANSACTION", "CMD BEGIN 0");
    ok(s, "INSERT INTO u VALUES (2, 'second')");
    expect(s, "COMMIT", "CMD COMMIT 0");
    session_close(s);
    close_env(&env);

    assert(open_env(&env, 64u << 20));
    assert(session_open(env.db, &s) == SESSION_OK);
    expect(s, "SELECT * FROM t", "[1] 1|'d2'");
    expect(s, "SELECT * FROM u", "[2] 1|'u1'/ 2|'second'");
    session_close(s);
    close_env(&env);

    /* 응답 직후 프로세스가 종료돼도 commit은 보존된다 */
    pid_t pid = fork();
    if (pid == 0) {
        env_t c;
        if (!open_env(&c, 64u << 20)) _exit(4);
        session_t *cs = NULL;
        if (session_open(c.db, &cs) != SESSION_OK) _exit(5);
        char *a = run(cs, "BEGIN");
        char *b = run(cs, "INSERT INTO t (v) VALUES ('after-kill')");
        char *d = run(cs, "COMMIT");
        _exit(strcmp(a, "CMD BEGIN 0") == 0 && strncmp(b, "CMD", 3) == 0 && strcmp(d, "CMD COMMIT 0") == 0 ? 0 : 3);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "자식 commit 응답");
    assert(open_env(&env, 64u << 20));
    assert(session_open(env.db, &s) == SESSION_OK);
    expect(s, "SELECT * FROM t", "[2] 1|'d2'/ 2|'after-kill'");
    /* 복구 뒤의 기본 키 색인도 같은 상태를 가리킨다 */
    expect(s, "SELECT * FROM t WHERE id = 2", "[1] 2|'after-kill'");
    expect(s, "SELECT * FROM t WHERE id = 3", "[0]");
    session_close(s);
    close_env(&env);
}

/* ── 6-1. 기본 키 색인과 트랜잭션(#158) ── */

static void test_key_index_follows_transactions(void)
{
    printf("\n[기본 키 색인이 트랜잭션의 commit·rollback을 따른다]\n");
    create_plain_file();
    env_t env;
    assert(open_env(&env, 64u << 20));
    session_t *a = NULL, *b = NULL;
    assert(session_open(env.db, &a) == SESSION_OK && session_open(env.db, &b) == SESSION_OK);
    schema(a);
    ok(a, "INSERT INTO t (v) VALUES ('base1')");
    ok(a, "INSERT INTO t (v) VALUES ('base2')");

    expect(a, "BEGIN", "CMD BEGIN 0");
    ok(a, "INSERT INTO t VALUES (10, 'pending')");
    ok(a, "UPDATE t SET id = 11 WHERE id = 10");
    ok(a, "DELETE FROM t WHERE id = 1");
    expect(a, "SELECT * FROM t WHERE id = 11", "[1] 11|'pending'");
    expect(a, "SELECT * FROM t WHERE id = 10", "[0]");
    expect(a, "SELECT * FROM t WHERE id = 1", "[0]");
    char *dup = run(a, "INSERT INTO t VALUES (11, 'dup')");
    CHECK(strncmp(dup, "ERR ", 4) == 0, "작업 안의 중복 삽입은 거절: %s", dup);
    free(dup);
    expect(a, "ROLLBACK", "CMD ROLLBACK 0");
    expect(a, "SELECT * FROM t WHERE id = 1", "[1] 1|'base1'");
    expect(a, "SELECT * FROM t WHERE id = 11", "[0]");
    expect(b, "SELECT * FROM t WHERE id = 10", "[0]");
    ok(a, "INSERT INTO t VALUES (10, 'reuse')");

    expect(a, "BEGIN", "CMD BEGIN 0");
    ok(a, "UPDATE t SET id = 20 WHERE id = 2");
    ok(a, "INSERT INTO t VALUES (21, 'second')");
    ok(a, "DELETE FROM t WHERE id = 21");
    ok(a, "INSERT INTO t VALUES (21, 'third')");
    expect(a, "COMMIT", "CMD COMMIT 0");
    expect(b, "SELECT * FROM t WHERE id = 20", "[1] 20|'base2'");
    expect(b, "SELECT * FROM t WHERE id = 2", "[0]");
    expect(b, "SELECT * FROM t WHERE id = 21", "[1] 21|'third'");
    session_close(a);
    session_close(b);
    close_env(&env);

    assert(open_env(&env, 64u << 20));
    assert(session_open(env.db, &a) == SESSION_OK);
    expect(a, "SELECT * FROM t WHERE id = 20", "[1] 20|'base2'");
    expect(a, "SELECT * FROM t WHERE id = 21", "[1] 21|'third'");
    expect(a, "SELECT * FROM t WHERE id = 10", "[1] 10|'reuse'");
    expect(a, "SELECT * FROM t WHERE id = 11", "[0]");
    expect(a, "SELECT * FROM t WHERE id = 2", "[0]");
    session_close(a);
    close_env(&env);
}

/* ── 7. 거절 ── */

static void test_rejections(void)
{
    printf("\n[지원하지 않는 사용]\n");
    /* 트랜잭션 계층이 없는 데이터베이스의 세션 */
    create_plain_file();
    session_limits_t l = limits(64u << 20);
    session_pool_t *pool = NULL;
    assert(session_pool_open(&l, &pool) == SESSION_OK);
    session_db_t *sdb = NULL;
    assert(session_pool_open_database(pool, DB, true, &sdb, NULL) == SESSION_OK);
    session_t *s = NULL;
    assert(session_open(sdb, &s) == SESSION_OK);
    expect(s, "BEGIN", "ERR 2/34");
    expect(s, "COMMIT", "ERR 2/34");
    CHECK(session_txn_begin(s, NULL) == TXN_UNSUPPORTED, "계층 없는 세션의 C API");
    CHECK(!session_txn_active(s), "열린 트랜잭션 없음");
    session_close(s);
    session_pool_close_database(sdb, NULL);
    session_pool_close(pool);

    /* 옛 보증 선택 문법은 없다. 어떤 보증을 요청해도 commit은 DURABLE이다 */
    env_t env;
    assert(open_env(&env, 64u << 20));
    assert(session_open(env.db, &s) == SESSION_OK);
    char *old = run(s, "BEGIN DURABLE");
    CHECK(strncmp(old, "ERR 1/", 6) == 0, "BEGIN DURABLE은 구문 오류: %s", old);
    free(old);
    session_txn_options_t o = {TXN_GUARANTEE_DURABLE, 0};
    CHECK(session_txn_begin(s, &o) == TXN_OK, "C API DURABLE 요청");
    txn_commit_info_t info;
    CHECK(session_txn_commit(s, &info) == TXN_OK && info.guarantee == TXN_GUARANTEE_DURABLE, "달성 보증은 DURABLE");
    /* 열린 트랜잭션이 있는 세션이 있으면 database를 닫을 수 없다 */
    expect(s, "BEGIN", "CMD BEGIN 0");
    database_status_t ds;
    CHECK(session_pool_close_database(env.db, &ds) == SESSION_BUSY, "세션이 남아 있으면 BUSY");
    session_close(s);
    close_env(&env);
}

int main(void)
{
    mkdir(DIR_NAME, 0700);
    test_sql_control_two_sessions();
    test_failed_transaction();
    test_c_api();
    test_waiting_begin();
    test_budget();
    test_commit_restart();
    test_key_index_follows_transactions();
    test_rejections();
    fresh_dir();
    rmdir(DIR_NAME);
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
