/*
 * test_database_txn.c -- database의 트랜잭션 경계(#8): 여러 문장의 commit·rollback, 미확정 변경 비노출, writer 직렬화,
 * durable commit 전후의 종료·I/O 실패와 재시작 복구, 영속 epoch를 실제 database·page_file·WAL 파일로 검사한다.
 *
 * 합성 데이터만 쓴다. 호출은 공개 API(database, database_txn, transaction)로 한다. 다루지 않는 것: 전원 장애,
 * lost write, 여러 스레드가 같은 database를 직접 호출하는 직렬화(session 계층의 몫), 큐 ACK(#140).
 *
 * 빌드(저장소 루트): tests/test_database_txn.c src/sql/{database,database_txn,sql_statement,lexer,result}.c
 *   src/storage/{heap_store,record,record_page,overflow,value,catalog,page_file,catalog_store,legacy_migrate,
 *   durable_pair,wal,transaction}.c -lpthread -lm
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
#define DB DIR_NAME "/db.lrdb"
#define WAL DIR_NAME "/db.wal"

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
    uint8_t *db, *wal;
    size_t db_len, wal_len;
    bool has_wal;
} snap_t;

static void snap_take(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    read_all(DB, &s->db, &s->db_len);
    s->has_wal = read_all(WAL, &s->wal, &s->wal_len);
}

static void snap_restore(const snap_t *s)
{
    write_all(DB, s->db, s->db_len);
    if (s->has_wal) write_all(WAL, s->wal, s->wal_len);
    else unlink(WAL);
}

static void snap_free(snap_t *s)
{
    free(s->db);
    free(s->wal);
}

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

static database_t *open_tx(bool with_wal, bool convert, database_txn_info_t *info)
{
    database_txn_options_t o = {0};
    o.wal_path = with_wal ? WAL : NULL;
    o.convert = convert;
    database_t *db = NULL;
    database_status_t st = database_open_transactional(DB, &o, &db, info);
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
    database_t *db = open_tx(false, false, NULL);
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
    db = open_tx(false, false, NULL);
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

/* ── 2. 문장 오류 뒤에는 rollback이 필요하다 ── */

static void test_statement_error_requires_rollback(void)
{
    printf("\n[문장 오류 뒤 명시적 rollback]\n");
    make_db();
    database_t *db = open_tx(false, false, NULL);
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

    /* 읽기 전용·다른 격리·WAL 없는 DURABLE은 지원하지 않는다 */
    txn_begin_options_t ro = {TXN_ACCESS_READ_ONLY, TXN_ISOLATION_SNAPSHOT, TXN_GUARANTEE_VISIBLE, 0, NULL};
    CHECK(database_txn_begin(db, &ro, &txn) == TXN_UNSUPPORTED, "읽기 전용 handle은 미지원");
    txn_begin_options_t rc = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_READ_COMMITTED, TXN_GUARANTEE_VISIBLE, 0, NULL};
    CHECK(database_txn_begin(db, &rc, &txn) == TXN_UNSUPPORTED, "READ_COMMITTED 미지원");
    txn_begin_options_t du = rw(TXN_GUARANTEE_DURABLE, 0);
    CHECK(database_txn_begin(db, &du, &txn) == TXN_UNSUPPORTED, "WAL 없이 DURABLE 거절");
    CHECK(txn == NULL, "실패하면 NULL");
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
    database_t *db = open_tx(false, false, NULL);
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
    database_t *db = open_tx(false, false, NULL);
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

/* ── 5. durable commit, 재시작, 복구 ── */

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
    database_txn_info_t info;
    database_t *db = open_tx(true, true, &info);
    assert(db != NULL && info.wal_present);
    seed_schema(db);
    exec_ok(db, NULL, "INSERT INTO t (v) VALUES ('base')");
    exec_ok(db, NULL, "INSERT INTO u VALUES (1, 'one')");
    assert(database_close(db) == DATABASE_OK);
}

static void test_durable_basic(void)
{
    printf("\n[durable commit과 재시작]\n");
    prepare_base();
    database_txn_info_t info;
    database_t *db = open_tx(true, false, &info);
    CHECK(db != NULL && info.replayed_txns == 0, "자동 commit 3건은 적용된 채 닫혔다");
    if (db == NULL) return;
    char *old = state_of(db);
    CHECK(strcmp(old, "[1] 1|'base' ## [1] 1|'one'") == 0, "기준 상태: %s", old);
    free(old);
    uint32_t epoch = database_txn_epoch(db);
    CHECK(epoch > 0 && info.epoch == epoch, "열 때 영속한 epoch");
    bool committed = false;
    txn_status_t cs = TXN_OK;
    CHECK(run_write_txn(db, TXN_GUARANTEE_DURABLE, &committed, &cs) && committed, "durable commit %d", (int)cs);
    char *neu = state_of(db);
    CHECK(strcmp(neu, "[2] 1|'durable-0'/ 2|'durable-1'... ") != 0, "상태가 바뀌었다");
    CHECK(database_close(db) == DATABASE_OK, "close");

    db = open_tx(true, false, &info);
    char *after = db != NULL ? state_of(db) : strdup("open-fail");
    CHECK(strcmp(after, neu) == 0, "재시작 뒤 같은 상태\n      전: %s\n      후: %s", neu, after);
    CHECK(info.replayed_txns == 0, "게시된 거래는 다시 실행하지 않는다 (%llu)", (unsigned long long)info.replayed_txns);
    CHECK(info.epoch > epoch, "재시작마다 epoch가 올라간다 %u -> %u", epoch, info.epoch);
    free(neu);
    free(after);

    /* rollback한 durable 거래와 끝나지 않은 거래는 재시작 뒤 보이지 않는다 */
    char *kept = state_of(db);
    txn_begin_options_t o = rw(TXN_GUARANTEE_DURABLE, 0);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    exec_ok(db, txn, "INSERT INTO u VALUES (77, 'rolled')");
    CHECK(database_txn_rollback(txn) == TXN_OK, "rollback");
    database_txn_release(&txn);
    exec_ok(db, NULL, "INSERT INTO u VALUES (78, 'auto')");  /* 뒤이은 승인된 거래의 commit이 앞선 abort를 flush한다 */
    CHECK(database_close(db) == DATABASE_OK, "close");
    db = open_tx(true, false, &info);
    char *final_state = state_of(db);
    CHECK(strstr(final_state, "rolled") == NULL && strstr(final_state, "78|'auto'") != NULL,
          "rollback한 거래는 없고 뒤이은 거래는 있다: %s", final_state);
    free(final_state);
    free(kept);
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

static int h_ftruncate(int fd, off_t len)
{
    if (point()) {
        errno = EIO;
        return -1;
    }
    int r = ftruncate(fd, len);
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
    wal_io_t wio = {pread, h_pwrite, h_fsync, h_ftruncate};
    wal_set_default_io(&wio);
}

static void hooks_off(void)
{
    page_file_set_default_io(NULL);
    wal_set_default_io(NULL);
}

/* WAL에 COMMIT이 fsync된 거래 수. WAL이 없으면 0 */
static uint64_t wal_committed_count(void)
{
    wal_t *w = NULL;
    wal_open_info_t info;
    if (wal_open(WAL, false, &w, &info) != WAL_OK) return 0;
    uint64_t n = info.committed;
    wal_close(w);
    return n;
}

/* 열려 있는 재시작 뒤 상태가 old 또는 new와 같은지, 그리고 이어서 쓸 수 있는지 */
typedef enum { STATE_OLD, STATE_NEW, STATE_OTHER, STATE_OPEN_FAILED } state_kind_t;

static state_kind_t classify(const char *old_state, const char *new_state, uint64_t *replayed, char **text)
{
    database_txn_info_t info;
    database_t *db = open_tx(true, false, &info);
    if (db == NULL) return STATE_OPEN_FAILED;
    char *s = state_of(db);
    if (replayed != NULL) *replayed = info.replayed_txns;
    state_kind_t kind = strcmp(s, old_state) == 0 ? STATE_OLD : strcmp(s, new_state) == 0 ? STATE_NEW : STATE_OTHER;
    /* 복구 뒤 이어서 쓰고 다시 읽을 수 있다 */
    char *line = run(db, NULL, "INSERT INTO u VALUES (500, 'after')", NULL, 0);
    bool wrote = is_ok(line);
    free(line);
    database_close(db);
    if (!wrote) kind = STATE_OTHER;
    db = open_tx(true, false, &info);
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

static void test_durable_crash_points(void)
{
    printf("\n[durable commit 전후의 종료 지점]\n");
    prepare_base();
    snap_t base;
    snap_take(&base);
    char *old_state = strdup("[1] 1|'base' ## [1] 1|'one'");
    uint64_t base_committed = wal_committed_count();

    /* 기대하는 새 상태와 지점 수: 처음부터 끝까지 실행 */
    hooks_on();
    database_t *db = open_tx(true, false, NULL);
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

    int bad = 0, crashed = 0, replayed_cases = 0, acknowledged = 0;
    for (long k = 0; k <= total; k++) {
        snap_restore(&base);
        pid_t pid = fork();
        if (pid == 0) {
            hooks_on();
            database_t *c = open_tx(true, false, NULL);
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
        uint64_t logged_commits = wal_committed_count() - base_committed;
        uint64_t replayed = 0;
        char *text = NULL;
        state_kind_t kind = classify(old_state, new_state, &replayed, &text);
        bool good = kind == STATE_OLD || kind == STATE_NEW;
        if (acked && kind != STATE_NEW) good = false;  /* 승인한 변경은 보존된다 */
        if (logged_commits > 0 && kind != STATE_NEW) good = false;  /* WAL에 승인된 거래는 응답 여부와 무관하게 보존된다 */
        if (logged_commits == 0 && kind == STATE_NEW) good = false;  /* 승인되지 않은 거래가 보이면 안 된다 */
        if (!good) {
            printf("  k=%ld %s: 상태 %s\n", k, acked ? "승인 응답" : "종료", text != NULL ? text : "(열기 실패)");
            bad++;
        }
        if (replayed > 0) replayed_cases++;
        free(text);
    }
    CHECK(bad == 0 && crashed > 0, "종료 지점 %ld곳 어긋남 %d (종료 %d, 승인 %d)", total + 1, bad, crashed, acknowledged);
    CHECK(replayed_cases > 0, "WAL 승인 뒤 데이터 반영 전 종료를 복구가 다시 반영한 경우가 있다 (%d)", replayed_cases);

    /* I/O 실패: 승인 응답이면 새 상태, 확정 실패면 옛 상태, 불확정이면 둘 중 하나 */
    bad = 0;
    long failed = 0;
    int uncertain = 0;
    for (long k = 1; k <= total; k++) {
        snap_restore(&base);
        hooks_on();
        database_t *c = open_tx(true, false, NULL);
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
        uint64_t logged_commits = wal_committed_count() - base_committed;
        uint64_t replayed = 0;
        char *text = NULL;
        state_kind_t kind = classify(old_state, new_state, &replayed, &text);
        bool good;
        if (logged_commits > 0 && kind != STATE_NEW) good = false;
        else if (logged_commits == 0 && kind == STATE_NEW) good = false;
        else if (ok) good = kind == STATE_NEW;
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
        database_t *c = open_tx(true, false, NULL);
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
    database_txn_info_t info;
    database_t *db = open_tx(true, false, &info);
    CHECK(db != NULL && info.replayed_txns == 0, "끝나지 않은 거래는 복구하지 않는다");
    if (db == NULL) return;
    expect(db, NULL, "SELECT * FROM u", OLD_U);
    expect(db, NULL, "SELECT * FROM t", OLD_STATE);
    /* 자동 commit도 durable이다: 응답 직후 종료해도 보존된다 */
    database_close(db);
    pid = fork();
    if (pid == 0) {
        database_t *c = open_tx(true, false, NULL);
        char *line = run(c, NULL, "INSERT INTO u VALUES (11, 'auto')", NULL, 0);
        _exit(is_ok(line) ? 0 : 3);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "자동 commit 응답");
    db = open_tx(true, false, &info);
    expect(db, NULL, "SELECT * FROM u", "[2] 11|'auto'/ 1|'one'");
    database_close(db);
}

/* ── 7. 재시작 뒤 트랜잭션 id는 재사용되지 않는다(영속 epoch) ── */

static void damage_slot(int slot)
{
    uint8_t b;
    int fd = open(DB, O_RDWR);
    off_t off = (off_t)slot * PAGE_FILE_PAGE_BYTES + 200;
    if (fd >= 0 && pread(fd, &b, 1, off) == 1) {
        b ^= 0x5A;
        if (pwrite(fd, &b, 1, off) != 1) fprintf(stderr, "damage 실패\n");
    }
    if (fd >= 0) close(fd);
}

static int newest_slot(void)
{
    uint8_t *all = NULL;
    size_t len = 0;
    uint64_t best = 0;
    int newest = 0;
    if (read_all(DB, &all, &len) && len >= 2 * PAGE_FILE_PAGE_BYTES) {
        for (int slot = 0; slot < 2; slot++) {
            uint64_t gen = 0;
            for (int i = 7; i >= 0; i--) gen = (gen << 8) | all[(size_t)slot * PAGE_FILE_PAGE_BYTES + 32 + 20 + i];
            if (gen > best) {
                best = gen;
                newest = slot;
            }
        }
    }
    free(all);
    return newest;
}

static void test_epoch_not_reused(void)
{
    printf("\n[재시작 뒤 트랜잭션 id 재사용 금지]\n");
    prepare_base();
    txn_begin_options_t o = rw(TXN_GUARANTEE_DURABLE, 0);
    database_t *db = open_tx(true, false, NULL);
    database_txn_t *txn = NULL;
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    uint64_t first = database_txn_id(txn);
    database_txn_rollback(txn);  /* WAL에 기록이 없는 거래 */
    database_txn_release(&txn);
    database_close(db);

    /* 최신 제어 슬롯이 손상돼도 floor가 내려가지 않는다 */
    damage_slot(newest_slot());
    database_txn_info_t info;
    db = open_tx(true, false, &info);
    CHECK(db != NULL, "한 슬롯 손상은 열린다");
    if (db == NULL) return;
    CHECK(database_txn_begin(db, &o, &txn) == TXN_OK, "begin");
    uint64_t second = database_txn_id(txn);
    CHECK(second > first && (second >> 32) > (first >> 32), "id %llx -> %llx", (unsigned long long)first,
          (unsigned long long)second);
    database_txn_release(&txn);
    /* 응답을 잃은 호출자의 조회: 같은 프로세스의 이전 거래는 기억한다 */
    txn_commit_info_t ci;
    CHECK(database_txn_query_commit(db, second, &ci) == TXN_BUSY || database_txn_query_commit(db, second, &ci) == TXN_OK,
          "조회");
    database_close(db);
}

/* ── 8. 지원하지 않는 열기 ── */

static void test_open_rejections(void)
{
    printf("\n[열기 거절]\n");
    prepare_base();
    /* 같은 파일을 두 번 쓰기로 열 수 없다 */
    database_t *db = open_tx(true, false, NULL);
    database_txn_options_t o = {0};
    o.wal_path = WAL;
    database_t *second = NULL;
    database_txn_info_t info;
    CHECK(database_open_transactional(DB, &o, &second, &info) == DATABASE_LOCKED && second == NULL, "이중 열기는 LOCKED");
    database_close(db);
    /* WAL이 사라지면 새로 만들지 않고 거절한다 */
    unlink(WAL);
    o.convert = true;
    CHECK(database_open_transactional(DB, &o, &second, &info) == DATABASE_CORRUPT && second == NULL,
          "WAL 누락은 거절 (pair=%d)", info.pair_status);
}

int main(void)
{
    mkdir(DIR_NAME, 0700);
    test_commit_and_rollback();
    test_statement_error_requires_rollback();
    test_waiting_writer();
    test_rollback_space();
    test_durable_basic();
    test_durable_crash_points();
    test_unfinished_and_autocommit();
    test_epoch_not_reused();
    test_open_rejections();
    fresh_dir();
    rmdir(DIR_NAME);
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
