#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql/database.h"
#include "sql/result.h"
#include "storage/page_file.h"
#include "storage/text_tree.h"
#include "storage/value.h"

/*
 * database 검사. 합성 데이터만 쓰며 DB 파일은 build/ 아래 전용 디렉터리에 만든다.
 * 결과는 result.h의 소비 API로만 읽고 사람이 읽을 수 있는 한 줄 표현으로 바꿔 대조한다.
 */

#define DIR_NAME "build/test_database_dir"
#define DB_PATH DIR_NAME "/db.lrdb"

static int g_run = 0;
static int g_fail = 0;
static page_file_io_t g_page_io;
static size_t g_page_reads;
static bool g_fail_overflow_read;

typedef struct {
    size_t used;
    size_t peak;
    size_t limit;
} projection_memory_t;

static bool reserve_projection_memory(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    projection_memory_t *memory = ctx;
    if (memory->limit != 0 && bytes > memory->limit - memory->used) return false;
    memory->used += bytes;
    if (memory->used > memory->peak) memory->peak = memory->used;
    return true;
}

static void release_projection_memory(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    projection_memory_t *memory = ctx;
    assert(bytes <= memory->used);
    memory->used -= bytes;
}

static const mem_ledger_ops_t PROJECTION_MEMORY = {
    reserve_projection_memory, release_projection_memory, NULL, NULL};

static ssize_t count_page_read(int fd, void *buf, size_t size, off_t offset)
{
    g_page_reads++;
    ssize_t read = g_page_io.pread(fd, buf, size, offset);
    if (g_fail_overflow_read && read == PAGE_FILE_PAGE_BYTES && offset >= 2 * PAGE_FILE_PAGE_BYTES) {
        const uint8_t *page = buf;
        if (page[6] == PAGE_KIND_OVERFLOW && page[7] == 0) {
            errno = EIO;
            return -1;
        }
    }
    return read;
}

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

static uint64_t fnv(const uint8_t *bytes, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

/* ── 결과를 한 줄로 ── */
typedef struct {
    char *text;
    size_t len;
    size_t cap;
} buffer_t;

static void put(buffer_t *b, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void put(buffer_t *b, const char *format, ...)
{
    char piece[256];
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
        default: {
            const char *quote = cell->type == VALUE_TEXT ? "'" : "x'";
            if (cell->size_bytes <= 24) {
                put(b, "%s", quote);
                for (size_t i = 0; i < cell->size_bytes; i++) {
                    if (cell->type == VALUE_TEXT && cell->bytes[i] >= 0x20 && cell->bytes[i] < 0x7f) {
                        put(b, "%c", cell->bytes[i]);
                    } else {
                        put(b, "\\%02x", cell->bytes[i]);
                    }
                }
                put(b, "'");
            } else {
                put(b, "%s%zu#%016llx'", quote, cell->size_bytes, (unsigned long long)fnv(cell->bytes, cell->size_bytes));
            }
        }
    }
}

static int compare_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* ROWS는 "행 | 행 | ..."(정렬 선택), COMMAND는 "CMD tag n", ERROR는 "ERR class". 호출자가 free한다. */
static char *dump(result_t *result, bool sorted)
{
    buffer_t out = {NULL, 0, 0};
    put(&out, "%s", "");
    switch (result_kind(result)) {
        case RESULT_KIND_COMMAND: {
            const result_command_t *command = result_command(result);
            put(&out, "CMD %.*s %llu", (int)command->tag_len, command->tag, (unsigned long long)command->affected_rows);
            break;
        }
        case RESULT_KIND_ERROR: {
            result_row_t row;
            assert(result_next(result, &row) == RESULT_NEXT_FAILED);
            put(&out, "ERR %d", (int)result_outcome(result).error->error_class);
            break;
        }
        default: {
            size_t columns = result_column_count(result);
            char **lines = NULL;
            size_t count = 0, cap = 0;
            result_row_t row;
            result_next_t step;
            while ((step = result_next(result, &row)) == RESULT_NEXT_ROW) {
                buffer_t line = {NULL, 0, 0};
                put(&line, "%s", "");
                assert(row.cell_count == columns);
                for (size_t i = 0; i < row.cell_count; i++) {
                    if (i > 0) put(&line, "|");
                    put_cell(&line, &row.cells[i]);
                }
                if (count == cap) {
                    cap = cap == 0 ? 16 : cap * 2;
                    lines = realloc(lines, cap * sizeof(*lines));
                    assert(lines != NULL);
                }
                lines[count++] = line.text;
            }
            assert(step == RESULT_NEXT_DONE);
            if (sorted && count > 1) qsort(lines, count, sizeof(*lines), compare_lines);
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

static result_t *run_bound(database_t *db, const char *sql, const value_view_t *params, size_t count)
{
    lexer_binding_t binding = {params, count};
    result_t *result = NULL;
    database_exec_status_t status = database_exec(db, (const uint8_t *)sql, strlen(sql),
                                                  count > 0 ? &binding : NULL, &result);
    assert(status == DATABASE_EXEC_OK && result != NULL);
    return result;
}

/* sql을 실행해 결과 표현을 돌려준다. */
static char *query(database_t *db, const char *sql)
{
    result_t *result = run_bound(db, sql, NULL, 0);
    char *text = dump(result, true);
    result_release(&result);
    return text;
}

static void expect_text(database_t *db, const char *sql, const char *expected)
{
    char *got = query(db, sql);
    CHECK(strcmp(got, expected) == 0, "%s\n      기대: %s\n      실제: %s", sql, expected, got);
    free(got);
}

static void expect_class(database_t *db, const char *sql, result_error_class_t error_class)
{
    result_t *result = run_bound(db, sql, NULL, 0);
    bool ok = result_kind(result) == RESULT_KIND_ERROR;
    if (ok) {
        result_row_t row;
        result_next(result, &row);
        ok = result_outcome(result).error->error_class == error_class;
    }
    CHECK(ok, "%s: 오류 분류 %d를 기대", sql, (int)error_class);
    result_release(&result);
}

static void exec_ok(database_t *db, const char *sql)
{
    result_t *result = run_bound(db, sql, NULL, 0);
    bool ok = result_kind(result) == RESULT_KIND_COMMAND;
    if (!ok) fprintf(stderr, "예상하지 못한 결과: %s\n", sql);
    CHECK(ok, "%s", sql);
    result_release(&result);
}

static database_t *reopen(database_t *db, bool writable)
{
    assert(database_close(db) == DATABASE_OK);
    database_t *again;
    assert(database_open(DB_PATH, writable, &again) == DATABASE_OK);
    return again;
}

static database_t *fresh_db(void)
{
    fresh_dir();
    database_t *db;
    assert(database_create(DB_PATH, &db) == DATABASE_OK);
    return db;
}

/* ── 테스트 ── */

static void test_two_schemas_crud_and_reopen(void)
{
    printf("\n[test_two_schemas_crud_and_reopen]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(5) NOT NULL, "
                "score REAL, bio TEXT, avatar BLOB)");
    exec_ok(db, "CREATE TABLE logs (ts INTEGER, msg TEXT NOT NULL)");
    CHECK(database_table_count(db) == 2, "테이블 두 개");

    exec_ok(db, "INSERT INTO users (name) VALUES ('alice')");
    exec_ok(db, "INSERT INTO users (name, score, bio) VALUES ('bobbybobby', 2.5, 'hello')");
    exec_ok(db, "INSERT INTO users (name, avatar) VALUES ('carol', X'00ff10')");
    exec_ok(db, "INSERT INTO logs VALUES (100, 'boot')");
    exec_ok(db, "INSERT INTO logs (msg) VALUES ('no timestamp')");
    expect_text(db, "SELECT * FROM users",
                "[3] 1|'alice'|NULL|NULL|NULL/ 2|'bobbybobby'|2.5|'hello'|NULL/ 3|'carol'|NULL|NULL|x'\\00\\ff\\10'");
    expect_text(db, "SELECT * FROM logs", "[2] 100|'boot'/ NULL|'no timestamp'");
    CHECK(true, "VARCHAR(5)에 10글자를 잘림 없이 저장");

    result_t *updated = run_bound(db, "UPDATE users SET score = 9.5, bio = 'x' WHERE id = 1", NULL, 0);
    char *text = dump(updated, true);
    CHECK(strcmp(text, "CMD UPDATE 1") == 0, "갱신 영향 행 수: %s", text);
    free(text);
    result_release(&updated);
    exec_ok(db, "DELETE FROM users WHERE id = 2");
    expect_text(db, "SELECT id, name, score, bio FROM users", "[2] 1|'alice'|9.5|'x'/ 3|'carol'|NULL|NULL");
    expect_text(db, "SELECT name FROM users WHERE score IS NULL", "[1] 'carol'");
    expect_text(db, "SELECT name FROM users WHERE score IS NOT NULL", "[1] 'alice'");
    expect_text(db, "SELECT name FROM users WHERE score > 9", "[1] 'alice'");
    expect_text(db, "SELECT name FROM users WHERE score = NULL", "[0]");
    expect_text(db, "SELECT name FROM users WHERE name <> 'alice'", "[1] 'carol'");

    db = reopen(db, true);
    expect_text(db, "SELECT id, name, score, bio FROM users", "[2] 1|'alice'|9.5|'x'/ 3|'carol'|NULL|NULL");
    expect_text(db, "SELECT * FROM logs", "[2] 100|'boot'/ NULL|'no timestamp'");
    exec_ok(db, "INSERT INTO users (name) VALUES ('dave')");
    expect_text(db, "SELECT id FROM users WHERE name = 'dave'", "[1] 4");

    db = reopen(db, false);
    expect_class(db, "SELECT COUNT(*) FROM users", RESULT_E_SEMANTIC);  /* 함수 호출은 지원하지 않는다 */
    expect_class(db, "INSERT INTO logs VALUES (1, 'x')", RESULT_E_SEMANTIC);
    expect_text(db, "SELECT name FROM users WHERE id = 4", "[1] 'dave'");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_values_round_trip_across_page_sizes(void)
{
    printf("\n[test_values_round_trip_across_page_sizes]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE v (k INTEGER, t TEXT, b BLOB, r REAL)");
    exec_ok(db, "INSERT INTO v VALUES (1, '', X'', 0.0)");
    exec_ok(db, "INSERT INTO v VALUES (2, NULL, NULL, NULL)");
    exec_ok(db, "INSERT INTO v VALUES (3, '한글 \xe2\x82\xac \xf0\x9f\x98\x80', X'00', -0.0)");
    exec_ok(db, "INSERT INTO v VALUES (-4, 'a''b', X'DEADbeef', 1048576.5)");

    /* 매개변수로 큰 값: 8160바이트 본문을 넘는 TEXT·BLOB */
    size_t sizes[] = {8159, 8160, 8161, 20000, 300000};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint8_t *text = malloc(sizes[s]);
        uint8_t *blob = malloc(sizes[s]);
        assert(text != NULL && blob != NULL);
        for (size_t i = 0; i < sizes[s]; i++) {
            text[i] = (uint8_t)('a' + i % 26);
            blob[i] = (uint8_t)(i * 13 + s);
        }
        blob[1] = 0;
        value_view_t params[3] = {
            {VALUE_INTEGER, (int64_t)(100 + s), 0, NULL, 0},
            {VALUE_TEXT, 0, 0, text, sizes[s]},
            {VALUE_BLOB, 0, 0, blob, sizes[s]},
        };
        result_t *inserted = run_bound(db, "INSERT INTO v (k, t, b) VALUES (?, ?, ?)", params, 3);
        char *line = dump(inserted, true);
        CHECK(strcmp(line, "CMD INSERT 1") == 0, "큰 값 %zu 삽입: %s", sizes[s], line);
        free(line);
        result_release(&inserted);

        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT t, b FROM v WHERE k = %zu", 100 + s);
        char expected[160];
        snprintf(expected, sizeof(expected), "[1] '%zu#%016llx'|x'%zu#%016llx'", sizes[s],
                 (unsigned long long)fnv(text, sizes[s]), sizes[s], (unsigned long long)fnv(blob, sizes[s]));
        expect_text(db, sql, expected);
        free(text);
        free(blob);
    }
    expect_text(db, "SELECT k, t, b, r FROM v WHERE k < 4",
                "[4] -4|'a'b'|x'\\de\\ad\\be\\ef'|1048576.5/ 1|''|x''|0.0/ 2|NULL|NULL|NULL/ "
                "3|'\\ed\\95\\9c\\ea\\b8\\80 \\e2\\82\\ac \\f0\\9f\\98\\80'|x'\\00'|-0.0");
    db = reopen(db, true);
    expect_text(db, "SELECT k FROM v WHERE t = ''", "[1] 1");
    expect_text(db, "SELECT k FROM v WHERE t IS NULL", "[1] 2");
    result_t *big = run_bound(db, "SELECT t FROM v WHERE k = 104", NULL, 0);
    result_row_t row;
    assert(result_next(big, &row) == RESULT_NEXT_ROW);
    CHECK(row.cells[0].type == VALUE_TEXT && row.cells[0].size_bytes == 300000, "재열기 뒤 300000바이트 TEXT 길이");
    result_release(&big);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* 모든 테이블을 한 문자열로 */
static char *snapshot(database_t *db)
{
    char *users = query(db, "SELECT * FROM users");
    char *logs = query(db, "SELECT * FROM logs");
    size_t n = strlen(users) + strlen(logs) + 8;
    char *both = malloc(n);
    assert(both != NULL);
    snprintf(both, n, "%s ## %s", users, logs);
    free(users);
    free(logs);
    return both;
}

static void test_failed_statements_keep_existing_data(void)
{
    printf("\n[test_failed_statements_keep_existing_data]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, score REAL)");
    exec_ok(db, "CREATE TABLE logs (ts INTEGER, msg TEXT NOT NULL)");
    exec_ok(db, "INSERT INTO users (name, score) VALUES ('a', 1.0)");
    exec_ok(db, "INSERT INTO users (name, score) VALUES ('b', 2.0)");
    exec_ok(db, "INSERT INTO logs VALUES (1, 'one')");
    char *before = snapshot(db);

    struct {
        const char *sql;
        result_error_class_t expect;
    } failures[] = {
        {"CREATE TABLE users (x INTEGER)", RESULT_E_SEMANTIC},
        {"CREATE TABLE t (x INTEGER, x TEXT)", RESULT_E_SYNTAX},
        {"DROP TABLE nothing", RESULT_E_SEMANTIC},
        {"SELECT * FROM nothing", RESULT_E_SEMANTIC},
        {"SELECT nothing FROM users", RESULT_E_SEMANTIC},
        {"SELECT * FROM users WHERE nothing = 1", RESULT_E_SEMANTIC},
        {"INSERT INTO nothing VALUES (1)", RESULT_E_SEMANTIC},
        {"INSERT INTO users (nothing) VALUES (1)", RESULT_E_SEMANTIC},
        {"INSERT INTO users (name, name) VALUES ('a', 'b')", RESULT_E_SYNTAX},
        {"INSERT INTO logs VALUES (1)", RESULT_E_SEMANTIC},
        {"INSERT INTO logs (ts, msg) VALUES (1)", RESULT_E_SYNTAX},
        {"INSERT INTO users (id, name) VALUES ('abc', 'x')", RESULT_E_SEMANTIC},
        {"INSERT INTO users (name, score) VALUES ('x', X'00')", RESULT_E_SEMANTIC},
        {"INSERT INTO users (name, score) VALUES ('x', 'not a number')", RESULT_E_SEMANTIC},
        {"INSERT INTO users (score) VALUES (3.0)", RESULT_E_CONSTRAINT},
        {"INSERT INTO users (name) VALUES (NULL)", RESULT_E_CONSTRAINT},
        {"INSERT INTO users (id, name) VALUES (1, 'dup')", RESULT_E_CONSTRAINT},
        {"UPDATE users SET name = NULL", RESULT_E_CONSTRAINT},
        {"UPDATE users SET id = 1 WHERE id = 2", RESULT_E_CONSTRAINT},
        {"UPDATE users SET id = 7", RESULT_E_CONSTRAINT},
        {"UPDATE users SET nothing = 1", RESULT_E_SEMANTIC},
        {"UPDATE users SET score = 'abc'", RESULT_E_SEMANTIC},
        {"UPDATE users SET score = 1.0, score = 2.0", RESULT_E_SYNTAX},
        {"DELETE FROM users WHERE nothing = 1", RESULT_E_SEMANTIC},
        {"DELETE FROM users WHERE id = 'abc'", RESULT_E_SEMANTIC},
        {"SELEC * FROM users", RESULT_E_SYNTAX},
        {"SELECT * FROM users; SELECT * FROM logs", RESULT_E_SYNTAX},
        {"SELECT * FROM users ORDER BY id", RESULT_E_SEMANTIC},
        {"SELECT * FROM users WHERE name = ?", RESULT_E_SYNTAX},
        {"", RESULT_E_SYNTAX},
        {"INSERT INTO users (name) VALUES ('\x80')", RESULT_E_SYNTAX},
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        expect_class(db, failures[i].sql, failures[i].expect);
        char *after = snapshot(db);
        CHECK(strcmp(before, after) == 0, "실패한 문장 뒤 데이터 불변: %s\n      %s", failures[i].sql, after);
        free(after);
    }
    CHECK(database_table_count(db) == 2 && !database_is_poisoned(db), "테이블 수 불변, poison 아님");
    db = reopen(db, true);
    char *reopened = snapshot(db);
    CHECK(strcmp(before, reopened) == 0, "재열기 뒤에도 불변");
    free(reopened);
    exec_ok(db, "INSERT INTO users (name, score) VALUES ('c', 3.0)");
    expect_text(db, "SELECT id FROM users WHERE name = 'c'", "[1] 3");
    free(before);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_drop_removes_only_the_target(void)
{
    printf("\n[test_drop_removes_only_the_target]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE a (id INTEGER PRIMARY KEY AUTOINCREMENT, v TEXT)");
    exec_ok(db, "CREATE TABLE b (id INTEGER PRIMARY KEY AUTOINCREMENT, w TEXT)");
    exec_ok(db, "CREATE TABLE c (n INTEGER)");
    for (int i = 0; i < 30; i++) {
        char sql[4096];
        snprintf(sql, sizeof(sql), "INSERT INTO a (v) VALUES ('row %d %0*d')", i, 2000, 0);
        exec_ok(db, sql);
        snprintf(sql, sizeof(sql), "INSERT INTO b (w) VALUES ('w%d')", i);
        exec_ok(db, sql);
    }
    exec_ok(db, "INSERT INTO c VALUES (7)");
    char *a_before = query(db, "SELECT * FROM a");
    char *b_before = query(db, "SELECT * FROM b");
    char *c_before = query(db, "SELECT * FROM c");

    exec_ok(db, "DROP TABLE b");
    CHECK(database_table_count(db) == 2, "테이블 두 개가 남음");
    expect_class(db, "SELECT * FROM b", RESULT_E_SEMANTIC);
    char *a_after = query(db, "SELECT * FROM a");
    char *c_after = query(db, "SELECT * FROM c");
    CHECK(strcmp(a_before, a_after) == 0 && strcmp(c_before, c_after) == 0, "다른 테이블의 이름·행이 그대로");
    db = reopen(db, true);
    free(a_after);
    free(c_after);
    a_after = query(db, "SELECT * FROM a");
    c_after = query(db, "SELECT * FROM c");
    CHECK(strcmp(a_before, a_after) == 0 && strcmp(c_before, c_after) == 0, "재열기 뒤에도 그대로");
    expect_class(db, "SELECT * FROM b", RESULT_E_SEMANTIC);

    /* 같은 이름을 다른 schema로 다시 만들 수 있고 옛 행은 보이지 않는다 */
    exec_ok(db, "CREATE TABLE b (q REAL)");
    expect_text(db, "SELECT * FROM b", "[0]");
    exec_ok(db, "INSERT INTO b VALUES (1.25)");
    expect_text(db, "SELECT * FROM b", "[1] 1.25");
    exec_ok(db, "INSERT INTO a (v) VALUES ('after')");
    expect_text(db, "SELECT id FROM a WHERE v = 'after'", "[1] 31");
    free(a_before);
    free(b_before);
    free(c_before);
    free(a_after);
    free(c_after);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_many_rows_updates_and_deletes_across_pages(void)
{
    printf("\n[test_many_rows_updates_and_deletes_across_pages]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE big (id INTEGER PRIMARY KEY AUTOINCREMENT, grp INTEGER, body TEXT)");
    int rows = 240;
    for (int i = 0; i < rows; i++) {
        char sql[4096];
        snprintf(sql, sizeof(sql), "INSERT INTO big (grp, body) VALUES (%d, '%0*d')", i % 4, 1500 + (i % 3) * 700, i);
        exec_ok(db, sql);
    }
    expect_class(db, "SELECT id FROM big WHERE grp = 0 AND id = 1", RESULT_E_SEMANTIC);
    result_t *res = run_bound(db, "SELECT id FROM big WHERE grp = 1", NULL, 0);
    size_t ones = 0;
    result_row_t row;
    while (result_next(res, &row) == RESULT_NEXT_ROW) ones++;
    result_release(&res);
    CHECK(ones == (size_t)rows / 4, "grp=1 행 수 %zu", ones);

    res = run_bound(db, "UPDATE big SET body = 'short' WHERE grp = 2", NULL, 0);
    char *line = dump(res, true);
    CHECK(strcmp(line, "CMD UPDATE 60") == 0, "갱신 %s", line);
    free(line);
    result_release(&res);
    res = run_bound(db, "DELETE FROM big WHERE grp = 3", NULL, 0);
    line = dump(res, true);
    CHECK(strcmp(line, "CMD DELETE 60") == 0, "삭제 %s", line);
    free(line);
    result_release(&res);
    res = run_bound(db, "DELETE FROM big WHERE grp = 9", NULL, 0);
    line = dump(res, true);
    CHECK(strcmp(line, "CMD DELETE 0") == 0, "일치하는 행이 없으면 0: %s", line);
    free(line);
    result_release(&res);

    db = reopen(db, true);
    res = run_bound(db, "SELECT grp, body FROM big", NULL, 0);
    size_t count = 0, shorts = 0;
    while (result_next(res, &row) == RESULT_NEXT_ROW) {
        count++;
        if (row.cells[0].integer == 2) shorts += row.cells[1].size_bytes == 5;
        CHECK(row.cells[0].integer != 3, "삭제한 grp 3이 남지 않음");
    }
    result_release(&res);
    CHECK(count == 180 && shorts == 60, "재열기 뒤 행 %zu, 줄어든 행 %zu", count, shorts);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_identity_and_primary_key_rules(void)
{
    printf("\n[test_identity_and_primary_key_rules]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, v TEXT)");
    exec_ok(db, "INSERT INTO t (id, v) VALUES (10, 'ten')");
    exec_ok(db, "INSERT INTO t (v) VALUES ('auto after explicit')");
    expect_text(db, "SELECT id FROM t WHERE v = 'auto after explicit'", "[1] 11");
    exec_ok(db, "UPDATE t SET id = 50 WHERE id = 10");
    exec_ok(db, "INSERT INTO t (v) VALUES ('after update')");
    expect_text(db, "SELECT id FROM t WHERE v = 'after update'", "[1] 51");
    exec_ok(db, "CREATE TABLE u (k TEXT PRIMARY KEY, v INTEGER)");
    exec_ok(db, "INSERT INTO u VALUES ('a', 1)");
    exec_ok(db, "INSERT INTO u VALUES ('b', 2)");
    expect_class(db, "INSERT INTO u VALUES ('a', 3)", RESULT_E_CONSTRAINT);
    exec_ok(db, "UPDATE u SET k = 'c' WHERE k = 'a'");
    exec_ok(db, "INSERT INTO u VALUES ('a', 4)");
    exec_ok(db, "INSERT INTO u VALUES (NULL, 5)");
    exec_ok(db, "INSERT INTO u VALUES (NULL, 6)");
    expect_text(db, "SELECT v FROM u WHERE k IS NULL", "[2] 5/ 6");
    db = reopen(db, true);
    expect_class(db, "INSERT INTO u VALUES ('c', 1)", RESULT_E_CONSTRAINT);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* #65: TEXT 기본키의 점 조회·행 이동·재시작이 스캔 없이 같은 행을 가리킨다. */
static void test_text_primary_key_index(void)
{
    printf("\n[test_text_primary_key_index]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE t (k TEXT PRIMARY KEY, v TEXT)");
    for (unsigned i = 0; i < 80; i++) {
        char sql[100];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('k%03u', 'v%03u')", i, i);
        exec_ok(db, sql);
    }
    database_scan_stats_t before, after;
    database_scan_stats(db, &before);
    expect_text(db, "SELECT v FROM t WHERE k = 'k060'", "[1] 'v060'");
    database_scan_stats(db, &after);
    CHECK(after.index_lookups == before.index_lookups + 1 && after.rows_scanned == before.rows_scanned,
          "TEXT 기본키 점 조회는 색인을 쓰고 행 스캔을 늘리지 않음");

    exec_ok(db, "UPDATE t SET v = 'relocated value after update' WHERE k = 'k060'");
    expect_text(db, "SELECT v FROM t WHERE k = 'k060'", "[1] '28#7d1358b3ed32f3b8'");
    exec_ok(db, "UPDATE t SET k = 'renamed' WHERE k = 'k060'");
    expect_text(db, "SELECT v FROM t WHERE k = 'renamed'", "[1] '28#7d1358b3ed32f3b8'");
    expect_text(db, "SELECT v FROM t WHERE k = 'k060'", "[0]");
    expect_class(db, "INSERT INTO t VALUES ('renamed', 'duplicate')", RESULT_E_CONSTRAINT);
    db = reopen(db, true);
    database_scan_stats(db, &before);
    expect_text(db, "SELECT v FROM t WHERE k = 'renamed'", "[1] '28#7d1358b3ed32f3b8'");
    database_scan_stats(db, &after);
    CHECK(after.index_lookups == before.index_lookups + 1 && after.rows_scanned == before.rows_scanned,
          "재시작 뒤 TEXT 기본키 색인을 사용");
    exec_ok(db, "DELETE FROM t WHERE k = 'renamed'");
    expect_text(db, "SELECT v FROM t WHERE k = 'renamed'", "[0]");
    char key[TEXT_TREE_KEY_MAX_BYTES + 2u];
    memset(key, 'x', sizeof(key) - 1u);
    key[sizeof(key) - 1u] = '\0';
    char sql[TEXT_TREE_KEY_MAX_BYTES + 96u];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('%s', 'too long')", key);
    expect_class(db, sql, RESULT_E_LIMIT);
    expect_text(db, "SELECT v FROM t WHERE k = 'k001'", "[1] 'v001'");
    db = reopen(db, true);
    expect_text(db, "SELECT v FROM t WHERE k = 'k001'", "[1] 'v001'");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* #65: 보조 색인을 만든 뒤 DML·재열기·고유 중복의 게시 경계를 확인한다. */
static void test_secondary_index_crud(void)
{
    printf("\n[test_secondary_index_crud]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, email TEXT, group_name TEXT)");
    exec_ok(db, "INSERT INTO t VALUES (1, 'a@x', 'same')");
    exec_ok(db, "INSERT INTO t VALUES (2, 'b@x', 'same')");
    exec_ok(db, "CREATE INDEX by_group ON t (group_name)");
    exec_ok(db, "CREATE UNIQUE INDEX by_email ON t (email)");
    for (unsigned i = 0; i < 4; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "CREATE INDEX extra_%u ON t (group_name)", i);
        exec_ok(db, sql);
    }
    expect_class(db, "INSERT INTO t VALUES (3, 'a@x', 'other')", RESULT_E_CONSTRAINT);
    database_scan_stats_t before, after;
    database_scan_stats(db, &before);
    expect_text(db, "SELECT id FROM t WHERE group_name = 'same'", "[2] 1/ 2");
    database_scan_stats(db, &after);
    CHECK(after.index_lookups == before.index_lookups + 1 && after.rows_scanned == before.rows_scanned,
          "보조 색인에서 중복 TEXT 값 두 행을 찾고 전체 스캔을 하지 않음");
    expect_class(db, "UPDATE t SET email = 'duplicate' WHERE group_name = 'same'", RESULT_E_CONSTRAINT);
    expect_text(db, "SELECT id FROM t WHERE email = 'a@x'", "[1] 1");
    expect_text(db, "SELECT id FROM t WHERE email = 'b@x'", "[1] 2");
    exec_ok(db, "UPDATE t SET email = 'c@x' WHERE email = 'a@x'");
    exec_ok(db, "DELETE FROM t WHERE email = 'b@x'");
    db = reopen(db, true);
    expect_text(db, "SELECT id FROM t WHERE email = 'c@x'", "[1] 1");
    expect_text(db, "SELECT id FROM t WHERE group_name = 'same'", "[1] 1");
    exec_ok(db, "INSERT INTO t VALUES (3, 'b@x', 'same')");
    expect_text(db, "SELECT id FROM t WHERE group_name = 'same'", "[2] 1/ 3");
    exec_ok(db, "CREATE TABLE duplicates (id INTEGER PRIMARY KEY, email TEXT)");
    exec_ok(db, "INSERT INTO duplicates VALUES (1, 'same')");
    exec_ok(db, "INSERT INTO duplicates VALUES (2, 'same')");
    expect_class(db, "CREATE UNIQUE INDEX duplicate_email ON duplicates (email)", RESULT_E_CONSTRAINT);
    exec_ok(db, "CREATE INDEX duplicate_email ON duplicates (email)");
    expect_text(db, "SELECT id FROM duplicates WHERE email = 'same'", "[2] 1/ 2");
    db = reopen(db, true);
    expect_text(db, "SELECT id FROM duplicates WHERE email = 'same'", "[2] 1/ 2");
    exec_ok(db, "DROP TABLE duplicates");
    expect_class(db, "SELECT id FROM duplicates WHERE email = 'same'", RESULT_E_SEMANTIC);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* 같은 보조 키가 여러 잎에 걸쳐도 모든 locator를 한 번씩 반환한다. */
static void test_secondary_index_many_duplicates(void)
{
    printf("\n[test_secondary_index_many_duplicates]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, label TEXT)");
    exec_ok(db, "CREATE INDEX by_label ON t (label)");
    for (unsigned i = 1; i <= 480; i++) {
        char sql[80];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%u, 'same')", i);
        exec_ok(db, sql);
    }
    bool seen[481] = {false};
    result_t *result = run_bound(db, "SELECT id FROM t WHERE label = 'same'", NULL, 0);
    result_row_t row;
    size_t count = 0;
    while (result_next(result, &row) == RESULT_NEXT_ROW) {
        CHECK(row.cell_count == 1 && row.cells[0].type == VALUE_INTEGER &&
              row.cells[0].integer > 0 && row.cells[0].integer <= 480, "중복 색인 값의 행 범위");
        if (row.cell_count == 1 && row.cells[0].type == VALUE_INTEGER &&
            row.cells[0].integer > 0 && row.cells[0].integer <= 480) {
            unsigned id = (unsigned)row.cells[0].integer;
            CHECK(!seen[id], "중복 색인 값의 중복 반환 없음");
            seen[id] = true;
        }
        count++;
    }
    CHECK(count == 480, "분할된 색인 잎에서 480개 행 반환");
    result_release(&result);
    db = reopen(db, true);
    expect_text(db, "SELECT id FROM t WHERE label = 'missing'", "[0]");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_open_rejects_other_files_without_change(void)
{
    printf("\n[test_open_rejects_other_files_without_change]\n");
    fresh_dir();
    uint8_t legacy[16384];
    memset(legacy, 0, sizeof(legacy));
    memcpy(legacy, "MINIDB\0", 8);
    legacy[8] = 2;
    FILE *f = fopen(DB_PATH, "wb");
    assert(f != NULL && fwrite(legacy, 1, sizeof(legacy), f) == sizeof(legacy));
    fclose(f);
    database_t *db = NULL;
    CHECK(database_open(DB_PATH, true, &db) == DATABASE_UNSUPPORTED && db == NULL, "옛 파일은 UNSUPPORTED");
    f = fopen(DB_PATH, "rb");
    uint8_t after[sizeof(legacy)];
    assert(f != NULL && fread(after, 1, sizeof(after), f) == sizeof(after));
    fclose(f);
    CHECK(memcmp(after, legacy, sizeof(legacy)) == 0, "열기 시도가 파일을 바꾸지 않음");
    CHECK(database_create(DB_PATH, &db) == DATABASE_EXISTS, "이미 있으면 create는 거절");
    CHECK(database_open(DIR_NAME "/missing.lrdb", false, &db) == DATABASE_IO, "없는 파일은 IO");
    clean_dir();
}

/* #181: SELECT 투영과 WHERE가 필요한 overflow 페이지만 읽는다. */
static void test_projected_overflow_reads(void)
{
    printf("\n[test_projected_overflow_reads]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE items (id INTEGER PRIMARY KEY, payload TEXT)");
    uint8_t payload[9000];
    memset(payload, 'x', sizeof(payload));
    value_view_t params[2] = {{VALUE_INTEGER, 1, 0, NULL, 0},
                              {VALUE_TEXT, 0, 0, payload, sizeof(payload)}};
    result_t *inserted = run_bound(db, "INSERT INTO items VALUES (?, ?)", params, 2);
    result_release(&inserted);
    assert(database_close(db) == DATABASE_OK);

    g_page_io = page_file_default_io();
    page_file_io_t counted = g_page_io;
    counted.pread = count_page_read;
    page_file_set_default_io(&counted);
    assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
    g_page_reads = 0;
    expect_text(db, "SELECT id FROM items WHERE id = 1", "[1] 1");
    size_t id_reads = g_page_reads;
    g_page_reads = 0;
    result_t *payload_result = run_bound(db, "SELECT payload FROM items WHERE id = 1", NULL, 0);
    result_row_t row;
    assert(result_next(payload_result, &row) == RESULT_NEXT_ROW);
    CHECK(row.cells[0].size_bytes == sizeof(payload), "선택한 payload 길이");
    result_release(&payload_result);
    size_t payload_reads = g_page_reads;
    CHECK(payload_reads > id_reads, "비선택 overflow 읽기: id %zu, payload %zu", id_reads, payload_reads);
    printf("  page reads: id %zu, payload %zu\n", id_reads, payload_reads);
    g_page_reads = 0;
    expect_text(db, "SELECT id FROM items", "[1] 1");
    size_t scan_id_reads = g_page_reads;
    g_page_reads = 0;
    value_view_t needle = {VALUE_TEXT, 0, 0, payload, sizeof(payload)};
    result_t *filtered = run_bound(db, "SELECT id FROM items WHERE payload = ?", &needle, 1);
    assert(result_next(filtered, &row) == RESULT_NEXT_ROW);
    CHECK(row.cells[0].integer == 1, "WHERE에 필요한 overflow 값 복원");
    result_release(&filtered);
    CHECK(g_page_reads > scan_id_reads, "WHERE payload 페이지 읽기: scan %zu, filter %zu", scan_id_reads,
          g_page_reads);

    projection_memory_t memory = {0};
    mem_ledger_t ledger = {&PROJECTION_MEMORY, &memory};
    database_exec_options_t options = {.ledger = &ledger};
    const char *id_sql = "SELECT id FROM items WHERE id = 1";
    result_t *measured = NULL;
    CHECK(database_exec_ex(db, &options, (const uint8_t *)id_sql, strlen(id_sql), NULL,
                           &measured) == DATABASE_EXEC_OK, "id 메모리 측정");
    result_release(&measured);
    CHECK(memory.used == 0, "id 결과 반환 뒤 메모리 장부 정리");
    size_t id_peak = memory.peak;
    memory.peak = 0;
    const char *payload_sql = "SELECT payload FROM items WHERE id = 1";
    CHECK(database_exec_ex(db, &options, (const uint8_t *)payload_sql, strlen(payload_sql), NULL,
                           &measured) == DATABASE_EXEC_OK, "payload 메모리 측정");
    result_release(&measured);
    CHECK(memory.used == 0, "payload 결과 반환 뒤 메모리 장부 정리");
    CHECK(memory.peak > id_peak, "투영별 요청 메모리: id %zu, payload %zu", id_peak,
          memory.peak);
    printf("  accounted peak bytes: id %zu, payload %zu\n", id_peak, memory.peak);
    memory.limit = id_peak + (memory.peak - id_peak) / 2;
    measured = NULL;
    CHECK(database_exec_ex(db, &options, (const uint8_t *)id_sql, strlen(id_sql), NULL,
                           &measured) == DATABASE_EXEC_OK && measured != NULL &&
              result_kind(measured) == RESULT_KIND_ROWS, "중간 예산의 id 조회 성공");
    result_release(&measured);
    database_exec_status_t limited = database_exec_ex(db, &options, (const uint8_t *)payload_sql,
                                                       strlen(payload_sql), NULL, &measured);
    CHECK(limited == DATABASE_EXEC_NOMEM ||
          (limited == DATABASE_EXEC_OK && measured != NULL && result_kind(measured) == RESULT_KIND_ERROR),
          "중간 예산의 payload 조회 거절");
    result_release(&measured);
    CHECK(memory.used == 0, "예산 거절 뒤 메모리 장부 정리");
    CHECK(database_exec_ex(db, &options, (const uint8_t *)id_sql, strlen(id_sql), NULL,
                           &measured) == DATABASE_EXEC_OK && measured != NULL &&
              result_kind(measured) == RESULT_KIND_ROWS, "예산 거절 뒤 다음 질의 성공");
    result_release(&measured);
    memory.limit = 0;

    g_fail_overflow_read = true;
    expect_text(db, "SELECT id FROM items WHERE id = 1", "[1] 1");
    expect_class(db, "SELECT payload FROM items WHERE id = 1", RESULT_E_IO);
    g_fail_overflow_read = false;
    expect_text(db, "SELECT id FROM items WHERE id = 1", "[1] 1");

    assert(database_close(db) == DATABASE_OK);
    page_file_set_default_io(NULL);
    clean_dir();
}

static void test_argument_errors(void)
{
    printf("\n[test_argument_errors]\n");
    database_t *db = fresh_db();
    result_t *result = NULL;
    CHECK(database_exec(NULL, (const uint8_t *)"SELECT 1", 8, NULL, &result) == DATABASE_EXEC_INVALID, "NULL db");
    CHECK(database_exec(db, NULL, 3, NULL, &result) == DATABASE_EXEC_INVALID, "NULL sql");
    CHECK(database_exec(db, (const uint8_t *)"x", 1, NULL, NULL) == DATABASE_EXEC_INVALID, "NULL out");
    exec_ok(db, "CREATE TABLE t (a INTEGER)");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

int main(void)
{
    test_two_schemas_crud_and_reopen();
    test_values_round_trip_across_page_sizes();
    test_failed_statements_keep_existing_data();
    test_drop_removes_only_the_target();
    test_many_rows_updates_and_deletes_across_pages();
    test_identity_and_primary_key_rules();
    test_text_primary_key_index();
    test_secondary_index_crud();
    test_secondary_index_many_duplicates();
    test_projected_overflow_reads();
    test_open_rejects_other_files_without_change();
    test_argument_errors();
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
