/*
 * test_database_txn.c -- database의 트랜잭션 경계(#8): 여러 문장의 commit·rollback, 미확정 변경 비노출, writer 직렬화,
 * commit 전후의 종료·I/O 실패와 재시작 뒤 상태를 실제 database·page_file 파일로 검사한다.
 *
 * 합성 데이터만 쓴다. 호출은 공개 API(database, database_txn, transaction)로 한다. 다루지 않는 것: 전원 장애,
 * lost write, 여러 스레드가 같은 database를 직접 호출하는 직렬화(session 계층의 몫), 큐 ACK(#140).
 *
 * 빌드(저장소 루트): tests/test_database_txn.c src/sql/{database,database_txn,sql_statement,sql_eval,lexer,result}.c
 *   src/storage/{heap_store,record,record_page,overflow,checksum,value,catalog,page_file,page_cache,catalog_store,
 *   pk_tree,text_tree,transaction}.c -lpthread -lm
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sql/database_txn.h"
#include "sql/result.h"
#include "storage/page_file.h"
#include "storage/value.h"

#define DIR_NAME "__test__database_txn"
#define DB DIR_NAME "/db.totedb"

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

/* ── 파일 보조 ── */

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

static bool read_all(const char *path, uint8_t **buf, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    *len = (size_t)st.st_size;
    *buf = malloc(*len > 0 ? *len : 1);
    size_t got = 0;
    while (got < *len) {
        ssize_t r = pread(fd, *buf + got, *len - got, (off_t)got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == *len;
}

static void write_all(const char *path, const uint8_t *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    size_t put = 0;
    while (put < len) {
        ssize_t r = pwrite(fd, buf + put, len - put, (off_t)put);
        if (r <= 0) break;
        put += (size_t)r;
    }
    close(fd);
}

typedef struct {
    uint8_t *db;
    size_t db_len;
} snap_t;

static void snap_take(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    read_all(DB, &s->db, &s->db_len);
}

static void snap_restore(const snap_t *s) { write_all(DB, s->db, s->db_len); }

static void snap_free(snap_t *s) { free(s->db); }

static off_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

/* ── 결과를 한 줄로 ── */

typedef struct {
    char *text;
    size_t len, cap;
} buffer_t;

static void put(buffer_t *b, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void put(buffer_t *b, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(NULL, 0, format, args);
    va_end(args);
    assert(n >= 0);
    if (b->len + (size_t)n + 1 > b->cap) {
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->text = realloc(b->text, b->cap);
        assert(b->text != NULL);
    }
    va_start(args, format);
    vsnprintf(b->text + b->len, (size_t)n + 1, format, args);
    va_end(args);
    b->len += (size_t)n;
}

static void put_cell(buffer_t *b, const value_view_t *cell)
{
    char text[64];
    size_t needed;
    switch (cell->type) {
        case VALUE_NULL: put(b, "NULL"); break;
        case VALUE_INTEGER: put(b, "%lld", (long long)cell->integer); break;
        case VALUE_REAL:
            assert(value_format(cell, text, sizeof(text), &needed) == VALUE_OK);
            put(b, "%s", text);
            break;
        default:
            put(b, cell->type == VALUE_TEXT ? "'" : "x'");
            for (size_t i = 0; i < cell->size_bytes; i++) {
                if (cell->type == VALUE_TEXT && cell->bytes[i] >= 0x20 && cell->bytes[i] < 0x7f) {
                    put(b, "%c", cell->bytes[i]);
                } else {
                    put(b, "\\%02x", cell->bytes[i]);
                }
            }
            put(b, "'");
    }
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
                    if (i > 0) put(&line, "|");
                    put_cell(&line, &row.cells[i]);
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

/* 트랜잭션 밖(자동 commit) 실행. txn이 NULL이 아니면 그 트랜잭션 안에서 실행한다. */
static char *run(database_t *db, database_txn_t *txn, const char *sql, const value_view_t *params, size_t count)
{
    lexer_binding_t binding = {params, count};
    result_t *result = NULL;
    const uint8_t *text = (const uint8_t *)sql;
    database_exec_status_t st = txn != NULL
        ? database_txn_exec(txn, NULL, text, strlen(sql), count > 0 ? &binding : NULL, &result)
        : database_exec_ex(db, NULL, text, strlen(sql), count > 0 ? &binding : NULL, &result);
    if (st != DATABASE_EXEC_OK || result == NULL) return strdup("EXEC-STATUS-FAIL");
    char *line = dump(result);
    result_release(&result);
    return line;
}

static bool is_ok(const char *line) { return strncmp(line, "CMD ", 4) == 0; }

static void exec_ok(database_t *db, database_txn_t *txn, const char *sql)
{
    char *line = run(db, txn, sql, NULL, 0);
    CHECK(is_ok(line), "%s -> %s", sql, line);
    free(line);
}

static void expect(database_t *db, database_txn_t *txn, const char *sql, const char *expected)
{
    char *line = run(db, txn, sql, NULL, 0);
    CHECK(strcmp(line, expected) == 0, "%s\n      기대: %s\n      실제: %s", sql, expected, line);
    free(line);
}

/* 두 테이블의 전체 상태 */
static char *state_of(database_t *db)
{
    char *a = run(db, NULL, "SELECT * FROM t", NULL, 0);
    char *b = run(db, NULL, "SELECT * FROM u", NULL, 0);
    char *both = malloc(strlen(a) + strlen(b) + 8);
    sprintf(both, "%s ## %s", a, b);
    free(a);
    free(b);
    return both;
}

/* ── 열기 ── */

static void make_db(void)
{
    fresh_dir();
    database_t *db = NULL;
    assert(database_create(DB, &db) == DATABASE_OK);
    assert(database_close(db) == DATABASE_OK);
}

static database_t *open_tx(void)
{
    database_t *db = NULL;
    database_status_t st = database_open_transactional(DB, NULL, &db);
    if (st != DATABASE_OK) {
        printf("  open 실패 status=%d\n", (int)st);
        return NULL;
    }
    return db;
}

static void seed_schema(database_t *db)
{
    exec_ok(db, NULL, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, v TEXT)");
    exec_ok(db, NULL, "CREATE TABLE u (k INTEGER, tag TEXT)");
}

static txn_begin_options_t rw(txn_guarantee_t guarantee, uint32_t timeout_ms)
{
    txn_begin_options_t o = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_SNAPSHOT, guarantee, timeout_ms, NULL};
    return o;
}

/* ── 1. 여러 문장의 commit과 rollback, 미확정 변경 비노출 ── */

static void test_commit_and_rollback(void)
{
    printf("\n[여러 문장의 commit·rollback과 미확정 변경 비노출]\n");
    make_db();
    database_t *db = open_tx();
    assert(db != NULL);
    seed_schema(db);
    exec_ok(db, NULL, "INSERT INTO t (v) VALUES ('a')");
    exec_ok(db, NULL, "INSERT INTO u VALUES (1, 'one')");
    char *before = state_of(db);

    /* commit: INSERT·UPDATE·DELETE와 두 테이블 변경이 한 번에 반영된다 */
    txn_begin_options_t o = rw(TXN_GUARANTEE_SYNCED, 0);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    exec_ok(db, txn, "INSERT INTO t (v) VALUES ('b')");
    exec_ok(db, txn, "INSERT INTO t (v) VALUES ('c')");
    exec_ok(db, txn, "UPDATE t SET v = 'A' WHERE id = 1");
    exec_ok(db, txn, "DELETE FROM t WHERE id = 2");
    exec_ok(db, txn, "INSERT INTO u VALUES (2, 'two')");
    expect(db, txn, "SELECT * FROM t", "[2] 1|'A'/ 3|'c'");
    /* 다른 연결(트랜잭션 밖)은 게시된 상태만 본다 */
    char *outside = state_of(db);
    CHECK(strcmp(outside, before) == 0, "미확정 변경이 보인다: %s", outside);
    free(outside);
    /* 다른 writer는 기다리지 않으면 BUSY, 변경 문장은 BUSY 오류 결과다 */
    database_txn_t *second = NULL;
    CHECK(database_txn_begin(db, &o, &second) == TXN_BUSY && second == NULL, "둘째 writer는 BUSY");
    char *blocked = run(db, NULL, "INSERT INTO u VALUES (9, 'x')", NULL, 0);
    CHECK(strcmp(blocked, "ERR 7/29") == 0, "트랜잭션 밖 변경은 BUSY 오류: %s", blocked);
    free(blocked);
    txn_commit_info_t info;
    CHECK(database_txn_commit(txn, &info) == TXN_OK && info.outcome == TXN_OUTCOME_APPLIED, "commit");
    CHECK(database_txn_release(&txn) == TXN_OK && txn == NULL, "release");
    char *after = state_of(db);
    CHECK(strcmp(after, "[2] 1|'A'/ 3|'c' ## [2] 1|'one'/ 2|'two'") == 0, "commit 뒤 상태: %s", after);
    free(after);

    /* 닫고 다시 열어도 같다 */
    CHECK(database_close(db) == DATABASE_OK, "close");
    db = open_tx();
    after = state_of(db);
    CHECK(strcmp(after, "[2] 1|'A'/ 3|'c' ## [2] 1|'one'/ 2|'two'") == 0, "재시작 뒤 상태: %s", after);
    free(after);
    free(before);
    before = state_of(db);

    /* rollback: 모두 버려지고 다음 writer가 실행된다 */
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    exec_ok(db, txn, "INSERT INTO t (v) VALUES ('gone')");
    exec_ok(db, txn, "DELETE FROM u WHERE k = 1");
    exec_ok(db, txn, "UPDATE t SET v = 'x'");
    CHECK(database_txn_rollback(txn) == TXN_OK && database_txn_state(txn) == TXN_STATE_ROLLED_BACK, "rollback");
    after = state_of(db);
    CHECK(strcmp(after, before) == 0, "rollback 뒤 상태: %s", after);
    free(after);
    CHECK(database_txn_release(&txn) == TXN_OK, "release");
    exec_ok(db, NULL, "INSERT INTO u VALUES (3, 'three')");
    expect(db, NULL, "SELECT * FROM u", "[3] 1|'one'/ 2|'two'/ 3|'three'");

    /* release만 해도(close 전 정리) rollback이다 */
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    exec_ok(db, txn, "INSERT INTO u VALUES (4, 'four')");
    CHECK(database_txn_release(&txn) == TXN_OK, "release는 rollback");
    expect(db, NULL, "SELECT * FROM u", "[3] 1|'one'/ 2|'two'/ 3|'three'");
    exec_ok(db, NULL, "INSERT INTO u VALUES (5, 'five')");

    /* 해제하지 않은 트랜잭션이 있으면 close는 BUSY이고 핸들은 그대로다 */
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    CHECK(database_close(db) == DATABASE_BUSY, "열린 트랜잭션이 있으면 close는 BUSY");
    exec_ok(db, txn, "INSERT INTO u VALUES (6, 'six')");
    CHECK(database_txn_commit(txn, NULL) == TXN_OK, "commit");
    database_txn_release(&txn);
    CHECK(database_close(db) == DATABASE_OK, "close");
    free(before);
}

static void test_index_transaction(void)
{
    printf("\n[보조 인덱스의 commit·rollback과 writer 직렬화]\n");
    make_db();
    database_t *db = open_tx();
    assert(db != NULL);
    exec_ok(db, NULL, "CREATE TABLE indexed (id INTEGER PRIMARY KEY, email TEXT)");
    exec_ok(db, NULL, "CREATE UNIQUE INDEX by_email ON indexed (email)");
    txn_begin_options_t options = rw(TXN_GUARANTEE_SYNCED, 0);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(db, &options, &txn) == TXN_OK, "인덱스 writer 시작");
    exec_ok(db, txn, "INSERT INTO indexed VALUES (1, 'a@x')");
    expect(db, txn, "SELECT id FROM indexed WHERE email = 'a@x'", "[1] 1");
    expect(db, NULL, "SELECT id FROM indexed WHERE email = 'a@x'", "[0]");
    database_txn_t *second = NULL;
    CHECK(database_txn_begin(db, &options, &second) == TXN_BUSY && second == NULL,
          "같은 unique 값을 쓸 수 있는 둘째 writer는 대기");
    CHECK(database_txn_commit(txn, NULL) == TXN_OK, "인덱스 writer 게시");
    CHECK(database_txn_release(&txn) == TXN_OK, "인덱스 writer 해제");
    expect(db, NULL, "SELECT id FROM indexed WHERE email = 'a@x'", "[1] 1");
    CHECK(database_close(db) == DATABASE_OK, "인덱스 재열기 전 종료");
    db = open_tx();
    assert(db != NULL);
    expect(db, NULL, "SELECT id FROM indexed WHERE email = 'a@x'", "[1] 1");
    CHECK(database_txn_begin(db, &options, &txn) == TXN_OK, "rollback writer 시작");
    exec_ok(db, txn, "UPDATE indexed SET email = 'b@x' WHERE email = 'a@x'");
    CHECK(database_txn_rollback(txn) == TXN_OK, "인덱스와 행 변경 rollback");
    CHECK(database_txn_release(&txn) == TXN_OK, "rollback writer 해제");
    expect(db, NULL, "SELECT id FROM indexed WHERE email = 'a@x'", "[1] 1");
    expect(db, NULL, "SELECT id FROM indexed WHERE email = 'b@x'", "[0]");
    CHECK(database_close(db) == DATABASE_OK, "인덱스 거래 검사 종료");
}

/* ── 2. 문장 오류 뒤에는 rollback이 필요하다 ── */

static void test_statement_error_requires_rollback(void)
{
    printf("\n[문장 오류 뒤 명시적 rollback]\n");
    make_db();
    database_t *db = open_tx();
    seed_schema(db);
    exec_ok(db, NULL, "INSERT INTO u VALUES (1, 'one')");
    txn_begin_options_t o = rw(TXN_GUARANTEE_SYNCED, 0);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    exec_ok(db, txn, "INSERT INTO u VALUES (2, 'two')");
    CHECK(!database_txn_failed(txn), "아직 실패 상태가 아니다");
    expect(db, txn, "INSERT INTO nosuch VALUES (1)", "ERR 2/1");
    CHECK(database_txn_failed(txn), "오류 뒤 실패 상태");
    expect(db, txn, "INSERT INTO u VALUES (3, 'three')", "ERR 2/26");
    expect(db, txn, "SELECT * FROM u", "ERR 2/26");
    CHECK(database_txn_commit(txn, NULL) == TXN_INVALID, "실패 상태의 commit은 거절");
    CHECK(database_txn_state(txn) == TXN_STATE_ACTIVE, "거절된 commit은 ACTIVE를 유지한다");
    CHECK(database_txn_rollback(txn) == TXN_OK, "rollback");
    database_txn_release(&txn);
    expect(db, NULL, "SELECT * FROM u", "[1] 1|'one'");

    /* 오류 결과가 아니라도 취소·한도로 실패하면 같은 상태다. 새 트랜잭션은 정상이다 */
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "새 트랜잭션");
    exec_ok(db, txn, "INSERT INTO u VALUES (2, 'two')");
    CHECK(database_txn_commit(txn, NULL) == TXN_OK, "commit");
    database_txn_release(&txn);
    expect(db, NULL, "SELECT * FROM u", "[2] 1|'one'/ 2|'two'");

    /* 읽기 전용·다른 격리는 지원하지 않는다. 어떤 보증을 요청해도 commit은 DURABLE이다 */
    txn_begin_options_t ro = {TXN_ACCESS_READ_ONLY, TXN_ISOLATION_SNAPSHOT, TXN_GUARANTEE_VISIBLE, 0, NULL};
    CHECK(database_txn_begin(db, &ro, &txn) == TXN_UNSUPPORTED, "읽기 전용 handle은 미지원");
    txn_begin_options_t rc = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_READ_COMMITTED, TXN_GUARANTEE_VISIBLE, 0, NULL};
    CHECK(database_txn_begin(db, &rc, &txn) == TXN_UNSUPPORTED, "READ_COMMITTED 미지원");
    CHECK(txn == NULL, "실패하면 NULL");
    txn_begin_options_t du = rw(TXN_GUARANTEE_DURABLE, 0);
    txn_commit_info_t info;
    CHECK(database_txn_begin(db, &du, &txn) == TXN_OK, "DURABLE 요청");
    exec_ok(db, txn, "INSERT INTO u VALUES (3, 'three')");
    CHECK(database_txn_commit(txn, &info) == TXN_OK && info.guarantee == TXN_GUARANTEE_DURABLE, "달성 보증은 DURABLE");
    database_txn_release(&txn);
    database_close(db);
}

/* ── 3. 대기하는 writer ── */

typedef struct {
    database_t *db;
    txn_status_t status;
    database_txn_t *txn;
} waiter_t;

static void *waiter_main(void *arg)
{
    waiter_t *w = arg;
    txn_begin_options_t o = rw(TXN_GUARANTEE_SYNCED, 5000);
    w->status = database_txn_begin(w->db, &o, &w->txn);
    return NULL;
}

static void test_waiting_writer(void)
{
    printf("\n[writer 대기와 다음 writer 실행]\n");
    make_db();
    database_t *db = open_tx();
    seed_schema(db);
    txn_begin_options_t o = rw(TXN_GUARANTEE_SYNCED, 0);
    database_txn_t *first = NULL;
    CHECK(database_txn_begin(db, &o, &first) == TXN_OK, "첫 writer");
    exec_ok(db, first, "INSERT INTO u VALUES (1, 'first')");
    waiter_t w = {db, TXN_OK, NULL};
    pthread_t th;
    pthread_create(&th, NULL, waiter_main, &w);
    usleep(150 * 1000);  /* 둘째가 대기 중이다 */
    CHECK(database_txn_rollback(first) == TXN_OK, "rollback");
    database_txn_release(&first);
    pthread_join(th, NULL);
    CHECK(w.status == TXN_OK && w.txn != NULL, "rollback 뒤 대기하던 writer가 실행된다: %d", (int)w.status);
    if (w.txn != NULL) {
        exec_ok(db, w.txn, "INSERT INTO u VALUES (2, 'second')");
        CHECK(database_txn_commit(w.txn, NULL) == TXN_OK, "commit");
        database_txn_release(&w.txn);
    }
    expect(db, NULL, "SELECT * FROM u", "[1] 2|'second'");

    /* timeout */
    database_txn_t *holder = NULL;
    CHECK(database_txn_begin(db, &o, &holder) == TXN_OK, "holder");
    txn_begin_options_t shortwait = rw(TXN_GUARANTEE_SYNCED, 50);
    database_txn_t *late = NULL;
    CHECK(database_txn_begin(db, &shortwait, &late) == TXN_TIMEOUT, "대기 timeout");
    database_txn_release(&holder);
    database_close(db);
}

/* ── 4. rollback이 쌓여도 페이지가 계속 자라지 않는다 ── */

static void test_rollback_space(void)
{
    printf("\n[rollback 반복의 공간 회수]\n");
    make_db();
    database_t *db = open_tx();
    seed_schema(db);
    exec_ok(db, NULL, "INSERT INTO t (v) VALUES ('seed')");
    exec_ok(db, NULL, "INSERT INTO u VALUES (1, 'seed')");
    txn_begin_options_t o = rw(TXN_GUARANTEE_SYNCED, 0);
    off_t at10 = 0, at40 = 0;
    for (int i = 0; i < 40; i++) {
        database_txn_t *txn = NULL;
        assert(database_txn_begin(db, &o, &txn) == TXN_OK);
        exec_ok(db, txn, "INSERT INTO t (v) VALUES ('rolled back row')");
        exec_ok(db, txn, "UPDATE t SET v = 'changed' WHERE id = 1");
        database_txn_rollback(txn);
        database_txn_release(&txn);
        exec_ok(db, NULL, "UPDATE u SET tag = 'x' WHERE k = 1");  /* commit이 있어야 해제 페이지가 재사용된다 */
        if (i == 9) at10 = file_size(DB);
        if (i == 39) at40 = file_size(DB);
    }
    CHECK(at40 - at10 <= 16 * 8192, "파일 크기가 계속 자란다: %lld -> %lld", (long long)at10, (long long)at40);
    expect(db, NULL, "SELECT * FROM t", "[1] 1|'seed'");
    database_close(db);
}

/* ── 5. commit과 재시작 ── */

/*
 * #81·#91: 켜면 기준 상태에 큰 값 행(overflow 체인)을 두고 트랜잭션이 그 값을 키우고, 큰 값을 넣었다가 줄인다.
 * 종료·실패 지점마다 체인 교체와 회수가 끼어든다.
 */
static bool g_large_values;
#define LARGE_BASE_BYTES 15000u
#define LARGE_GROWN_BYTES 30000u

static uint8_t *large_text(size_t size, char seed)
{
    uint8_t *bytes = malloc(size);
    assert(bytes != NULL);
    for (size_t i = 0; i < size; i++) bytes[i] = (uint8_t)('a' + (seed + i * 7) % 26);
    return bytes;
}

static bool run_bound_ok(database_t *db, database_txn_t *txn, const char *sql, int64_t key, size_t size, char seed)
{
    uint8_t *bytes = large_text(size, seed);
    value_view_t params[2] = {{VALUE_TEXT, 0, 0, bytes, size}, {VALUE_INTEGER, key, 0, NULL, 0}};
    char *line = run(db, txn, sql, params, 2);
    bool ok = is_ok(line);
    free(line);
    free(bytes);
    return ok;
}

static const char *WRITE_STATEMENTS[] = {
    "INSERT INTO t (v) VALUES ('durable-1')",
    "UPDATE t SET v = 'durable-0' WHERE id = 1",
    "INSERT INTO u VALUES (2, 'two')",
    "DELETE FROM u WHERE k = 1",
};

static bool run_write_txn(database_t *db, txn_guarantee_t g, bool *commit_ok_out, txn_status_t *commit_status)
{
    txn_begin_options_t o = rw(g, 0);
    database_txn_t *txn = NULL;
    if (database_txn_begin(db, &o, &txn) != TXN_OK) return false;
    bool ok = true;
    for (size_t i = 0; i < sizeof(WRITE_STATEMENTS) / sizeof(WRITE_STATEMENTS[0]); i++) {
        char *line = run(db, txn, WRITE_STATEMENTS[i], NULL, 0);
        ok = ok && is_ok(line);
        free(line);
    }
    /* 바인딩으로 모든 값 종류를 한 문장에 */
    const uint8_t blob[] = {0, 1, 0xff};
    value_view_t params[5] = {
        {VALUE_INTEGER, -7, 0, NULL, 0},
        {VALUE_TEXT, 0, 0, (const uint8_t *)"bound \xed\x95\x9c", 9},
        {VALUE_NULL, 0, 0, NULL, 0},
        {VALUE_REAL, 0, 2.5, NULL, 0},
        {VALUE_BLOB, 0, 0, blob, 3},
    };
    (void)params;
    value_view_t two[2] = {params[0], params[1]};
    char *line = run(db, txn, "INSERT INTO u VALUES (?, ?)", two, 2);
    ok = ok && is_ok(line);
    free(line);
    if (g_large_values) {
        ok = ok && run_bound_ok(db, txn, "UPDATE u SET tag = ? WHERE k = ?", 3, LARGE_GROWN_BYTES, 'g');
        ok = ok && run_bound_ok(db, txn, "INSERT INTO u (tag, k) VALUES (?, ?)", 4, LARGE_BASE_BYTES, 'n');
        ok = ok && run_bound_ok(db, txn, "UPDATE u SET tag = SUBSTR(?, 1, 5) WHERE k = ?", 4, 10, 's');
    }
    txn_status_t cs = ok ? database_txn_commit(txn, NULL) : TXN_INVALID;
    if (commit_status != NULL) *commit_status = cs;
    if (commit_ok_out != NULL) *commit_ok_out = cs == TXN_OK;
    database_txn_release(&txn);
    return ok;
}

static const char *OLD_STATE = "[1] 1|'base'";
static const char *OLD_U = "[1] 1|'one'";

static void prepare_base(void)
{
    make_db();
    database_t *db = open_tx();
    assert(db != NULL);
    seed_schema(db);
    exec_ok(db, NULL, "INSERT INTO t (v) VALUES ('base')");
    exec_ok(db, NULL, "INSERT INTO u VALUES (1, 'one')");
    if (g_large_values) assert(run_bound_ok(db, NULL, "INSERT INTO u (tag, k) VALUES (?, ?)", 3, LARGE_BASE_BYTES, 'b'));
    assert(database_close(db) == DATABASE_OK);
}

static void test_commit_restart(void)
{
    printf("\n[commit과 재시작]\n");
    prepare_base();
    database_t *db = open_tx();
    CHECK(db != NULL, "자동 commit 4건은 적용된 채 닫혔다");
    if (db == NULL) return;
    char *old = state_of(db);
    CHECK(strcmp(old, "[1] 1|'base' ## [1] 1|'one'") == 0, "기준 상태: %s", old);
    free(old);
    bool committed = false;
    txn_status_t cs = TXN_OK;
    CHECK(run_write_txn(db, TXN_GUARANTEE_DURABLE, &committed, &cs) && committed, "commit %d", (int)cs);
    char *neu = state_of(db);
    CHECK(database_close(db) == DATABASE_OK, "close");

    db = open_tx();
    char *after = db != NULL ? state_of(db) : strdup("open-fail");
    CHECK(strcmp(after, neu) == 0, "재시작 뒤 같은 상태\n      전: %s\n      후: %s", neu, after);
    free(neu);
    free(after);

    /* rollback한 거래는 재시작 뒤 보이지 않는다 */
    txn_begin_options_t o = rw(TXN_GUARANTEE_DURABLE, 0);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    exec_ok(db, txn, "INSERT INTO u VALUES (77, 'rolled')");
    CHECK(database_txn_rollback(txn) == TXN_OK, "rollback");
    database_txn_release(&txn);
    exec_ok(db, NULL, "INSERT INTO u VALUES (78, 'auto')");
    CHECK(database_close(db) == DATABASE_OK, "close");
    db = open_tx();
    char *final_state = state_of(db);
    CHECK(strstr(final_state, "rolled") == NULL && strstr(final_state, "78|'auto'") != NULL,
          "rollback한 거래는 없고 뒤이은 거래는 있다: %s", final_state);
    free(final_state);
    database_close(db);
}

/* ── 종료·I/O 실패 주입 ── */

static long g_points_left = -1, g_points_seen, g_fail_at, g_fail_seen;
static bool g_fail_mode;

static bool point(void)
{
    g_points_seen++;
    if (g_fail_mode) return g_fail_at > 0 && ++g_fail_seen == g_fail_at;
    if (g_points_left >= 0 && g_points_left-- == 0) _exit(137);
    return false;
}

static ssize_t h_pwrite(int fd, const void *b, size_t n, off_t o)
{
    if (point()) {
        errno = EIO;
        return -1;
    }
    ssize_t r = pwrite(fd, b, n, o);
    point();
    return r;
}

static int h_fsync(int fd)
{
    if (point()) {
        errno = EIO;
        return -1;
    }
    int r = fsync(fd);
    point();
    return r;
}

static int h_link(const char *a, const char *b)
{
    point();
    return link(a, b);
}

static int h_unlink(const char *p)
{
    point();
    return unlink(p);
}

static void hooks_on(void)
{
    page_file_io_t pio = {pread, h_pwrite, h_fsync, h_link, h_unlink};
    page_file_set_default_io(&pio);
}

static void hooks_off(void)
{
    page_file_set_default_io(NULL);
}

/* 열려 있는 재시작 뒤 상태가 old 또는 new와 같은지, 그리고 이어서 쓸 수 있는지 */
typedef enum { STATE_OLD, STATE_NEW, STATE_OTHER, STATE_OPEN_FAILED } state_kind_t;

static state_kind_t classify(const char *old_state, const char *new_state, char **text)
{
    database_t *db = open_tx();
    if (db == NULL) return STATE_OPEN_FAILED;
    char *s = state_of(db);
    state_kind_t kind = strcmp(s, old_state) == 0 ? STATE_OLD : strcmp(s, new_state) == 0 ? STATE_NEW : STATE_OTHER;
    /* #81·#91: 다시 연 파일에 고아 페이지와 이중 소유(조기 회수)가 없다 */
    database_page_check_t pages;
    if (database_check_pages(db, &pages) != DATABASE_OK || pages.orphaned != 0 || pages.conflicts != 0) {
        printf("  페이지 회계: 고아 %llu, 충돌 %llu\n", (unsigned long long)pages.orphaned,
               (unsigned long long)pages.conflicts);
        kind = STATE_OTHER;
    }
    /* 복구 뒤 이어서 쓰고 다시 읽을 수 있다 */
    char *line = run(db, NULL, "INSERT INTO u VALUES (500, 'after')", NULL, 0);
    bool wrote = is_ok(line);
    free(line);
    database_close(db);
    if (!wrote) kind = STATE_OTHER;
    db = open_tx();
    if (db == NULL) {
        kind = STATE_OPEN_FAILED;
    } else {
        char *again = state_of(db);
        if (strstr(again, "500|'after'") == NULL) kind = STATE_OTHER;
        free(again);
        database_close(db);
    }
    if (text != NULL) *text = s;
    else free(s);
    return kind;
}

static void test_crash_points(void)
{
    printf("\n[commit 전후의 종료 지점과 I/O 실패%s]\n", g_large_values ? ": 큰 값" : "");
    prepare_base();
    snap_t base;
    snap_take(&base);
    database_t *before = open_tx();
    assert(before != NULL);
    char *old_state = state_of(before);
    database_close(before);

    /* 기대하는 새 상태와 지점 수: 처음부터 끝까지 실행 */
    hooks_on();
    database_t *db = open_tx();
    g_fail_mode = true;
    g_fail_at = 0;
    g_points_seen = 0;
    bool committed = false;
    CHECK(run_write_txn(db, TXN_GUARANTEE_DURABLE, &committed, NULL) && committed, "기준 실행");
    long total = g_points_seen;
    g_fail_mode = false;
    char *new_state = state_of(db);
    database_close(db);
    hooks_off();
    CHECK(total > 8, "지점 수 %ld", total);
    CHECK(strcmp(new_state, old_state) != 0, "새 상태가 다르다");

    int bad = 0, crashed = 0, acknowledged = 0;
    for (long k = 0; k <= total; k++) {
        snap_restore(&base);
        pid_t pid = fork();
        if (pid == 0) {
            hooks_on();
            database_t *c = open_tx();
            if (c == NULL) _exit(4);
            g_points_left = k;
            bool ok = false;
            run_write_txn(c, TXN_GUARANTEE_DURABLE, &ok, NULL);
            _exit(ok ? 0 : 3);
        }
        int status;
        waitpid(pid, &status, 0);
        bool died = WIFEXITED(status) && WEXITSTATUS(status) == 137;
        bool acked = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!died && !acked) {
            printf("  k=%ld 자식이 예상 밖으로 끝남 status=%d\n", k, status);
            bad++;
            continue;
        }
        crashed += died;
        acknowledged += acked;
        char *text = NULL;
        state_kind_t kind = classify(old_state, new_state, &text);
        bool good = kind == STATE_OLD || kind == STATE_NEW;  /* 부분 반영은 없다 */
        if (acked && kind != STATE_NEW) good = false;        /* 승인한 변경은 보존된다 */
        if (!good) {
            printf("  k=%ld %s: 상태 %s\n", k, acked ? "승인 응답" : "종료", text != NULL ? text : "(열기 실패)");
            bad++;
        }
        free(text);
    }
    CHECK(bad == 0 && crashed > 0, "종료 지점 %ld곳 어긋남 %d (종료 %d, 승인 %d)", total + 1, bad, crashed, acknowledged);

    /* I/O 실패: 승인 응답이면 새 상태, 확정 실패면 옛 상태, 불확정이면 둘 중 하나 */
    bad = 0;
    long failed = 0;
    int uncertain = 0;
    for (long k = 1; k <= total; k++) {
        snap_restore(&base);
        hooks_on();
        database_t *c = open_tx();
        g_fail_mode = true;
        g_fail_at = k;
        g_fail_seen = 0;
        bool ok = false;
        txn_status_t cs = TXN_OK;
        bool stmts = run_write_txn(c, TXN_GUARANTEE_DURABLE, &ok, &cs);
        failed += g_fail_seen >= k;
        g_fail_mode = false;
        hooks_off();
        bool poisoned = database_txn_layer_poisoned(c);
        database_close(c);
        char *text = NULL;
        state_kind_t kind = classify(old_state, new_state, &text);
        bool good;
        if (ok) good = kind == STATE_NEW;
        else if (cs == TXN_COMMIT_UNCERTAIN) {
            good = kind == STATE_OLD || kind == STATE_NEW;
            uncertain++;
            if (!poisoned) good = false;
        } else good = kind == STATE_OLD;
        (void)stmts;
        if (!good) {
            printf("  k=%ld ok=%d commit=%d 상태=%s\n", k, ok, (int)cs, text != NULL ? text : "(열기 실패)");
            bad++;
        }
        free(text);
    }
    CHECK(bad == 0 && failed > 0, "실패 지점 어긋남 %d (주입 %ld, 불확정 %d)", bad, failed, uncertain);
    free(old_state);
    free(new_state);
    snap_free(&base);
}

/* ── 6. 끝나지 않은 거래와 자동 commit ── */

static void test_unfinished_and_autocommit(void)
{
    printf("\n[끝나지 않은 거래와 자동 commit의 재시작]\n");
    prepare_base();
    pid_t pid = fork();
    if (pid == 0) {
        database_t *c = open_tx();
        if (c == NULL) _exit(4);
        txn_begin_options_t o = rw(TXN_GUARANTEE_DURABLE, 0);
        database_txn_t *txn = NULL;
        if (database_txn_begin(c, &o, &txn) != TXN_OK) _exit(5);
        char *line = run(c, txn, "INSERT INTO u VALUES (9, 'unfinished')", NULL, 0);
        free(line);
        exec_ok(c, NULL, "INSERT INTO u VALUES (10, 'blocked')");  /* 쓰는 중이라 BUSY */
        _exit(137);  /* commit도 rollback도 없이 종료 */
    }
    int status;
    waitpid(pid, &status, 0);
    database_t *db = open_tx();
    CHECK(db != NULL, "다시 열린다");
    if (db == NULL) return;
    expect(db, NULL, "SELECT * FROM u", OLD_U);
    expect(db, NULL, "SELECT * FROM t", OLD_STATE);
    /* 자동 commit도 응답 직후 종료해도 보존된다 */
    database_close(db);
    pid = fork();
    if (pid == 0) {
        database_t *c = open_tx();
        char *line = run(c, NULL, "INSERT INTO u VALUES (11, 'auto')", NULL, 0);
        _exit(is_ok(line) ? 0 : 3);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "자동 commit 응답");
    db = open_tx();
    expect(db, NULL, "SELECT * FROM u", "[2] 11|'auto'/ 1|'one'");
    database_close(db);
}

/* ── 7. 같은 파일의 두 핸들(#199) ── */

static void check_accounting(database_t *db, const char *who)
{
    database_page_check_t check;
    CHECK(database_check_pages(db, &check) == DATABASE_OK && check.orphaned == 0 && check.conflicts == 0,
          "%s 페이지 회계: 고아 %llu, 충돌 %llu", who, (unsigned long long)check.orphaned,
          (unsigned long long)check.conflicts);
}

static void test_two_handles(void)
{
    printf("\n[같은 파일의 두 핸들]\n");
    prepare_base();
    database_t *a = open_tx();
    database_t *b = open_tx();
    CHECK(a != NULL && b != NULL, "같은 파일을 두 핸들로 연다");
    if (a == NULL || b == NULL) {
        database_close(a);
        database_close(b);
        return;
    }
    /* 한쪽이 commit하면 다른 쪽의 다음 문장이 새 행과 새 테이블을 본다 */
    exec_ok(a, NULL, "INSERT INTO u VALUES (2, 'two')");
    expect(b, NULL, "SELECT * FROM u", "[2] 1|'one'/ 2|'two'");
    exec_ok(b, NULL, "CREATE TABLE w (x INTEGER)");
    exec_ok(b, NULL, "INSERT INTO w VALUES (7)");
    expect(a, NULL, "SELECT * FROM w", "[1] 7");
    /* 한쪽의 쓰기 거래가 열린 동안 다른 쪽은 기다리지 않고 BUSY다. 1단계라 읽기도 BUSY다 */
    txn_begin_options_t o = rw(TXN_GUARANTEE_DURABLE, 0);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(a, &o, &txn) == TXN_OK, "a의 쓰기 거래");
    exec_ok(a, txn, "INSERT INTO u VALUES (3, 'three')");
    database_txn_t *other = NULL;
    CHECK(database_txn_begin(b, &o, &other) == TXN_BUSY && other == NULL, "a가 쓰는 동안 b의 BEGIN은 BUSY");
    char *blocked = run(b, NULL, "INSERT INTO u VALUES (9, 'x')", NULL, 0);
    /* 거래 계층의 자동 commit은 쓰기 시작에서 막혀 writer BUSY(29)다. 읽기는 문장 잠금에서 막혀 30이다 */
    CHECK(strcmp(blocked, "ERR 7/29") == 0, "a가 쓰는 동안 b의 변경은 BUSY: %s", blocked);
    free(blocked);
    char *read = run(b, NULL, "SELECT * FROM u", NULL, 0);
    CHECK(strcmp(read, "ERR 7/30") == 0, "a가 쓰는 동안 b의 읽기는 파일 잠금 BUSY: %s", read);
    free(read);
    txn_commit_info_t info;
    CHECK(database_txn_commit(txn, &info) == TXN_OK && info.outcome == TXN_OUTCOME_APPLIED, "a commit");
    database_txn_release(&txn);
    expect(b, NULL, "SELECT * FROM u", "[3] 1|'one'/ 2|'two'/ 3|'three'");
    /* b가 버린 거래는 a에 보이지 않고, 그 뒤 a의 변경을 b가 다시 읽는다 */
    CHECK(database_txn_begin(b, &o, &other) == TXN_OK, "b의 쓰기 거래");
    exec_ok(b, other, "INSERT INTO u VALUES (4, 'four')");
    CHECK(database_txn_rollback(other) == TXN_OK, "b rollback");
    database_txn_release(&other);
    expect(a, NULL, "SELECT * FROM u", "[3] 1|'one'/ 2|'two'/ 3|'three'");
    exec_ok(a, NULL, "DELETE FROM u WHERE k = 1");
    expect(b, NULL, "SELECT * FROM u", "[2] 2|'two'/ 3|'three'");
    check_accounting(a, "a");
    check_accounting(b, "b");
    CHECK(database_close(a) == DATABASE_OK && database_close(b) == DATABASE_OK, "두 핸들 닫기");
}

/* ── 8. 쓰는 도중 강제 종료되는 여러 프로세스(#199) ── */

/* 자식 프로세스: 자기 핸들로 큰 값 갱신·행 추가·카운터 증가를 한 거래로 계속 확정한다. 다른 writer가 있으면 다시 시도 */
static void writer_child(int worker)
{
    database_t *db = NULL;
    if (database_open_transactional(DB, NULL, &db) != DATABASE_OK) _exit(2);
    txn_begin_options_t o = rw(TXN_GUARANTEE_DURABLE, 0);
    for (int64_t i = 0;; i++) {
        database_txn_t *txn = NULL;
        txn_status_t begun = database_txn_begin(db, &o, &txn);
        if (begun == TXN_BUSY) {
            usleep(200);
            i--;
            continue;
        }
        if (begun != TXN_OK) _exit(3);
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO w VALUES (%lld)", (long long)(worker * 1000000 + i));
        char *added = run(db, txn, sql, NULL, 0);
        char *counted = run(db, txn, "UPDATE c SET n = n + 1 WHERE id = 1", NULL, 0);
        bool ok = is_ok(added) && is_ok(counted) &&
                  run_bound_ok(db, txn, "UPDATE u SET tag = ? WHERE k = ?", 3, 9000 + (size_t)(i % 7) * 900, (char)i);
        free(added);
        free(counted);
        txn_commit_info_t info;
        if (!ok || database_txn_commit(txn, &info) != TXN_OK) _exit(4);
        database_txn_release(&txn);
    }
}

/* writer가 쓰는 동안 읽기는 BUSY(7/30)다. Python 바인딩처럼 짧게 쉬며 다시 시도한다 */
static char *run_until_free(database_t *db, const char *sql)
{
    for (int attempt = 0;; attempt++) {
        char *line = run(db, NULL, sql, NULL, 0);
        if (strncmp(line, "ERR 7/", 6) != 0 || attempt == 20000) return line;
        free(line);
        usleep(250);
    }
}

static int64_t counter_of(database_t *db)
{
    char *line = run_until_free(db, "SELECT n FROM c");
    long long n = -1;
    sscanf(line, "[1] %lld", &n);
    free(line);
    return n;
}

static void test_processes_killed_while_writing(void)
{
    printf("\n[쓰는 도중 강제 종료되는 여러 프로세스]\n");
    g_large_values = true;
    prepare_base();
    g_large_values = false;
    database_t *db = open_tx();
    exec_ok(db, NULL, "CREATE TABLE w (k INTEGER PRIMARY KEY)");
    exec_ok(db, NULL, "CREATE TABLE c (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)");
    exec_ok(db, NULL, "INSERT INTO c VALUES (1, 0)");
    pid_t children[3];
    for (int i = 0; i < 3; i++) {
        children[i] = fork();
        if (children[i] == 0) writer_child(i + 1);
    }
    /* 둘을 차례로 죽이고, 그 사이에도 남은 writer가 계속 확정하는지 본다 */
    int64_t seen = 0;
    for (int i = 0; i < 2; i++) {
        usleep(300000);
        kill(children[i], SIGKILL);
        waitpid(children[i], NULL, 0);
        seen = counter_of(db);
        usleep(300000);
        int64_t later = counter_of(db);
        CHECK(later > seen, "%d번째 강제 종료 뒤 남은 writer가 계속 쓴다(%lld → %lld)", i + 1, (long long)seen,
              (long long)later);
    }
    kill(children[2], SIGKILL);
    int status = 0;
    waitpid(children[2], &status, 0);
    CHECK(WIFSIGNALED(status), "마지막 writer는 오류 없이 쓰다가 종료됨");
    /* 거래마다 w 한 행과 카운터 1 증가를 함께 확정했다. 반만 적용된 거래가 있으면 둘이 다르다 */
    char *rows = run_until_free(db, "SELECT COUNT(*) FROM w");
    char expected[64];
    snprintf(expected, sizeof(expected), "[1] %lld", (long long)counter_of(db));
    CHECK(strcmp(rows, expected) == 0, "w 행 수와 카운터가 같다: %s / %s", rows, expected);
    free(rows);
    check_accounting(db, "강제 종료 뒤");
    database_close(db);
    /* 다시 열어도 같고, 이어서 쓸 수 있다 */
    db = open_tx();
    check_accounting(db, "다시 연 뒤");
    exec_ok(db, NULL, "INSERT INTO w VALUES (-1)");
    database_close(db);
}

int main(void)
{
    mkdir(DIR_NAME, 0700);
    test_commit_and_rollback();
    test_index_transaction();
    test_statement_error_requires_rollback();
    test_waiting_writer();
    test_rollback_space();
    test_commit_restart();
    test_crash_points();
    g_large_values = true;
    test_crash_points();
    g_large_values = false;
    test_unfinished_and_autocommit();
    test_two_handles();
    test_processes_killed_while_writing();
    fresh_dir();
    rmdir(DIR_NAME);
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
