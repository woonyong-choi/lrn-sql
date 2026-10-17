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
#define DB_PATH DIR_NAME "/db.totedb"

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
    /* ORDER BY가 있으면 순서도 결과의 일부다 */
    char *text = dump(result, strstr(sql, "ORDER BY") == NULL);
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
    expect_text(db, "SELECT COUNT(*) FROM users", "[1] 3");
    expect_class(db, "SELECT name, COUNT(*) FROM users", RESULT_E_SEMANTIC);  /* 묶지 않은 열 */
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
        {"SELECT * FROM users ORDER BY nothing", RESULT_E_SEMANTIC},
        {"SELECT * FROM users WHERE name + 1 = 2", RESULT_E_SEMANTIC},
        {"SELECT * FROM users LIMIT -1", RESULT_E_SEMANTIC},
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
    /* 기본 키 탐침과 나머지 조건을 함께 거른다 */
    expect_text(db, "SELECT id FROM big WHERE grp = 0 AND id = 1", "[1] 1");
    expect_text(db, "SELECT id FROM big WHERE grp = 1 AND id = 1", "[0]");
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
    /* 기본 키는 NOT NULL이다 */
    expect_class(db, "INSERT INTO u VALUES (NULL, 5)", RESULT_E_CONSTRAINT);
    expect_class(db, "UPDATE u SET k = NULL WHERE k = 'b'", RESULT_E_CONSTRAINT);
    expect_text(db, "SELECT v FROM u WHERE k IS NULL", "[0]");
    db = reopen(db, true);
    expect_class(db, "INSERT INTO u VALUES ('c', 1)", RESULT_E_CONSTRAINT);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* #65: TEXT 기본키의 점 조회·행 이동·재시작이 스캔 없이 같은 행을 가리킨다. */
/*
 * #180: 정수 기본 키의 대소 조건은 색인 구간만 읽고, 결과는 같은 조건의 전체 스캔(id + 0)과 같다.
 * 여러 잎에 걸친 키·삭제·갱신·재시작 뒤에도 빠짐·중복이 없어야 한다.
 */
static void expect_same_as_scan(database_t *db, const char *range, const char *scan)
{
    char sql[256], base[256];
    snprintf(sql, sizeof(sql), "SELECT id, v FROM t WHERE %s ORDER BY id", range);
    snprintf(base, sizeof(base), "SELECT id, v FROM t WHERE %s ORDER BY id", scan);
    char *got = query(db, sql), *want = query(db, base);
    CHECK(strcmp(got, want) == 0, "%s\n      기대: %.200s\n      실제: %.200s", sql, want, got);
    free(got);
    free(want);
    snprintf(sql, sizeof(sql), "SELECT count(*), count(DISTINCT id) FROM t WHERE %s", range);
    snprintf(base, sizeof(base), "SELECT count(*), count(*) FROM t WHERE %s", scan);
    got = query(db, sql);
    want = query(db, base);
    CHECK(strcmp(got, want) == 0, "%s: 중복 없는 행 수 %s, 실제 %s", sql, want, got);
    free(got);
    free(want);
}

static void check_primary_key_ranges(database_t *db)
{
    static const char *const cases[][2] = {
        {"id >= 5990", "id + 0 >= 5990"},          {"id > 5990", "id + 0 > 5990"},
        {"id <= 14", "id + 0 <= 14"},              {"id < 14", "id + 0 < 14"},
        {"id > 1000 AND id <= 2100", "id + 0 > 1000 AND id + 0 <= 2100"},
        {"3001 < id AND 3010 >= id", "3001 < id + 0 AND 3010 >= id + 0"},
        {"id >= 7000", "id + 0 >= 7000"},          {"id < 2", "id + 0 < 2"},
        {"id > 50 AND id < 40", "id + 0 > 50 AND id + 0 < 40"},
        {"id > 9223372036854775807", "id + 0 > 9223372036854775807"},
        {"id < -9223372036854775808", "id < -9223372036854775807 - 1"},
        {"id >= 100 AND id <= 2000 AND v = 'u'", "id + 0 >= 100 AND id + 0 <= 2000 AND v = 'u'"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) expect_same_as_scan(db, cases[i][0], cases[i][1]);
}

static void test_primary_key_range(void)
{
    printf("\n[test_primary_key_range]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    /* 짝수 키 3000개: 잎 하나(508개)를 넘어 여러 잎에 걸친다 */
    for (unsigned batch = 0; batch < 30; batch++) {
        char sql[4096];
        size_t used = (size_t)snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ");
        for (unsigned i = 0; i < 100; i++) {
            unsigned id = 2 * (batch * 100 + i + 1);
            used += (size_t)snprintf(sql + used, sizeof(sql) - used, "%s(%u, 'r%u')", i ? ", " : "", id, id);
        }
        exec_ok(db, sql);
    }
    database_scan_stats_t before, after;
    database_scan_stats(db, &before);
    expect_text(db, "SELECT count(*) FROM t WHERE id >= 5990", "[1] 6");
    database_scan_stats(db, &after);
    CHECK(after.rows_scanned == before.rows_scanned && after.index_lookups == before.index_lookups + 1 &&
              after.index_pages_read - before.index_pages_read <= 3,
          "범위 조회는 전체 스캔 없이 색인 경로와 잎 몇 장만 읽음 (스캔 %llu, 조회 %llu, 색인 페이지 %llu)",
          (unsigned long long)(after.rows_scanned - before.rows_scanned),
          (unsigned long long)(after.index_lookups - before.index_lookups),
          (unsigned long long)(after.index_pages_read - before.index_pages_read));
    database_scan_stats(db, &before);
    expect_text(db, "SELECT count(*) FROM t WHERE id + 0 >= 5990", "[1] 6");
    database_scan_stats(db, &after);
    CHECK(after.rows_scanned == before.rows_scanned + 3000, "기본 키가 아닌 식은 전체 스캔");
    check_primary_key_ranges(db);

    /* UPDATE·DELETE도 같은 구간으로 대상 행을 찾는다 */
    database_scan_stats(db, &before);
    exec_ok(db, "UPDATE t SET v = 'w' WHERE id > 5900");
    exec_ok(db, "DELETE FROM t WHERE id <= 10");
    database_scan_stats(db, &after);
    CHECK(after.rows_scanned == before.rows_scanned, "구간 UPDATE·DELETE는 전체 스캔 없음");
    expect_text(db, "SELECT count(*), min(id), max(id) FROM t WHERE v = 'w'", "[1] 50|5902|6000");
    expect_text(db, "SELECT count(*), min(id) FROM t", "[1] 2995|12");
    exec_ok(db, "DELETE FROM t WHERE id % 7 = 0");
    exec_ok(db, "UPDATE t SET v = 'u' WHERE id % 5 = 0");
    exec_ok(db, "UPDATE t SET v = 'moved to a longer value so the row relocates' WHERE id % 11 = 0");
    check_primary_key_ranges(db);
    db = reopen(db, true);
    check_primary_key_ranges(db);
    expect_text(db, "SELECT id FROM t WHERE id > 100 ORDER BY id LIMIT 2", "[2] 102/ 104");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* #64: SELECT 결과는 열 이름·형식·NULL 가능 여부를 잃지 않는다. LEFT JOIN의 오른쪽 열은 NULL 가능이 된다 */
static void expect_columns(database_t *db, const char *sql, const char *expected)
{
    result_t *result = run_bound(db, sql, NULL, 0);
    char got[256];
    size_t used = 0;
    got[0] = '\0';
    for (size_t i = 0; i < result_column_count(result); i++) {
        const result_column_t *column = result_column(result, i);
        used += (size_t)snprintf(got + used, sizeof(got) - used, "%s%.*s:%d%s", i ? "," : "", (int)column->name_len,
                                 (const char *)column->name, (int)column->type, column->is_nullable ? "?" : "");
    }
    CHECK(strcmp(got, expected) == 0, "%s\n      기대: %s\n      실제: %s", sql, expected, got);
    result_release(&result);
}

static void test_result_column_metadata(void)
{
    printf("\n[test_result_column_metadata]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE p (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, score REAL, pic BLOB)");
    exec_ok(db, "CREATE TABLE o (oid INTEGER PRIMARY KEY, pid INTEGER NOT NULL)");
    expect_columns(db, "SELECT * FROM p", "id:1,name:3,score:2?,pic:4?");
    expect_columns(db, "SELECT p.id, o.oid, o.pid FROM p LEFT JOIN o ON o.pid = p.id", "id:1,oid:1?,pid:1?");
    /* 계산한 열(식·집계·GROUP BY 출력)은 NULL일 수 있다고 보수적으로 표시한다. NULL 가능은 "NULL일 수 있다"는 뜻이다 */
    expect_columns(db, "SELECT name AS who, score * 2 AS doubled, COUNT(*) AS n FROM p GROUP BY name, score",
                   "who:3?,doubled:2?,n:1?");
    expect_columns(db, "SELECT NULL AS nothing, 'x' AS t", "nothing:0?,t:3?");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

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
    /* #189: LIMIT을 채워 색인 순회를 멈춘 것은 오류가 아니다 */
    expect_text(db, "SELECT count(*) FROM (SELECT id FROM t WHERE label = 'same' LIMIT 2)", "[1] 2");
    db = reopen(db, true);
    expect_text(db, "SELECT id FROM t WHERE label = 'missing'", "[0]");
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_open_rejects_other_files_without_change(void)
{
    printf("\n[test_open_rejects_other_files_without_change]\n");
    fresh_dir();
    /* 다른 형식 파일(SQLite 머리)은 바꾸지 않고 UNSUPPORTED로 거절한다 */
    uint8_t legacy[16384];
    for (size_t i = 0; i < sizeof(legacy); i++) legacy[i] = (uint8_t)(i * 7u + 1u);
    memcpy(legacy, "SQLite format 3", 16);
    FILE *f = fopen(DB_PATH, "wb");
    assert(f != NULL && fwrite(legacy, 1, sizeof(legacy), f) == sizeof(legacy));
    fclose(f);
    database_t *db = NULL;
    CHECK(database_open(DB_PATH, true, &db) == DATABASE_UNSUPPORTED && db == NULL, "다른 형식 파일은 UNSUPPORTED");
    f = fopen(DB_PATH, "rb");
    uint8_t after[sizeof(legacy)];
    assert(f != NULL && fread(after, 1, sizeof(after), f) == sizeof(after));
    fclose(f);
    CHECK(memcmp(after, legacy, sizeof(legacy)) == 0, "열기 시도가 파일을 바꾸지 않음");
    CHECK(access(DB_PATH "-lock", F_OK) != 0, "다른 형식 파일 옆에 writer 잠금 파일을 만들지 않음");
    CHECK(database_create(DB_PATH, &db) == DATABASE_EXISTS, "이미 있으면 create는 거절");
    CHECK(database_open(DIR_NAME "/missing.totedb", false, &db) == DATABASE_IO, "없는 파일은 IO");
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
    /* 투영이 요청하는 페이지를 pread 수로 세고 overflow 읽기 실패를 주입하므로 읽기 캐시를 끈다 */
    page_file_set_default_cache_pages(0);
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
    page_file_set_default_cache_pages(PAGE_FILE_CACHE_PAGES);
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

/* #160·#161: ORDER BY·DISTINCT는 모은 행을 문장 예산 안에서만 쥐고, 넘치면 결과를 자르지 않고 거절한다 */
static database_exec_status_t run_budgeted(database_t *db, projection_memory_t *memory, const char *sql, result_t **out)
{
    mem_ledger_t ledger = {&PROJECTION_MEMORY, memory};
    database_exec_options_t options = {.ledger = &ledger};
    *out = NULL;
    return database_exec_ex(db, &options, (const uint8_t *)sql, strlen(sql), NULL, out);
}

static void test_order_and_distinct_respect_budget(void)
{
    printf("\n[test_order_and_distinct_respect_budget]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE sorted (id INTEGER PRIMARY KEY, grp INTEGER, body TEXT)");
    for (int i = 1; i <= 300; i++) {
        char sql[512];
        snprintf(sql, sizeof(sql), "INSERT INTO sorted VALUES (%d, %d, '%0200d')", i, i % 7 == 0 ? -1 : i % 5, i % 40);
        exec_ok(db, sql);
    }
    expect_text(db, "SELECT id, grp FROM sorted ORDER BY grp DESC, id DESC LIMIT 3", "[3] 299|4/ 289|4/ 284|4");
    expect_text(db, "SELECT id FROM sorted WHERE grp = -1 ORDER BY id LIMIT 2 OFFSET 1", "[2] 14/ 21");
    expect_text(db, "SELECT DISTINCT grp FROM sorted ORDER BY grp", "[6] -1/ 0/ 1/ 2/ 3/ 4");
    expect_text(db, "SELECT id FROM sorted WHERE grp = 9 ORDER BY id", "[0]");
    expect_text(db, "SELECT grp, COUNT(*) FROM sorted GROUP BY grp ORDER BY grp",
                "[6] -1|42/ 0|52/ 1|52/ 2|51/ 3|52/ 4|51");
    expect_text(db, "SELECT SUM(id), MAX(id) FROM sorted WHERE grp = 4", "[1] 7704|299");
    expect_text(db, "SELECT grp FROM sorted GROUP BY grp HAVING COUNT(*) < 52 ORDER BY 1", "[3] -1/ 2/ 4");
    expect_text(db, "SELECT COUNT(*), SUM(id) FROM sorted WHERE grp = 9", "[1] 0|NULL");
    expect_text(db, "SELECT COUNT(*) FROM sorted s JOIN sorted t ON t.grp = s.grp", "[1] 15078");
    expect_text(db, "SELECT grp FROM sorted UNION SELECT grp + 10 FROM sorted WHERE grp > 2 ORDER BY 1 DESC LIMIT 4",
                "[4] 14/ 13/ 4/ 3");
    expect_text(db, "SELECT s.id, t.id FROM sorted s LEFT JOIN sorted t ON t.id = s.id * 150 WHERE s.id < 4 ORDER BY 1",
                "[3] 1|150/ 2|300/ 3|NULL");

    const char *queries[] = {"SELECT body, id FROM sorted ORDER BY grp DESC, id", "SELECT DISTINCT body FROM sorted",
                             "SELECT body, COUNT(*), MIN(id) FROM sorted GROUP BY body ORDER BY body",
                             "SELECT COUNT(DISTINCT body), MAX(body) FROM sorted",
                             "SELECT s.id, t.body FROM sorted s LEFT JOIN sorted t ON t.id = s.id + 1 ORDER BY t.body, s.id",
                             "UPDATE sorted SET grp = grp RETURNING id, body",
                             "SELECT body FROM sorted UNION ALL SELECT body FROM sorted WHERE grp = 1 ORDER BY 1 LIMIT 100",
                             "SELECT body, grp FROM sorted UNION SELECT body, grp FROM sorted EXCEPT SELECT body, 0 FROM sorted",
                             ("WITH b AS (SELECT body, id FROM sorted WHERE grp >= 0) SELECT x.body FROM b x JOIN b y ON y.id = x.id "
                              "WHERE x.id IN (SELECT id FROM sorted WHERE grp <> 3)"),
                             "UPDATE sorted SET grp = grp WHERE id IN (SELECT id FROM sorted WHERE grp = 1) RETURNING body"};
    const size_t expected_rows[] = {300, 40, 40, 1, 300, 300, 100, 72, 206, 52};
    char *before = query(db, "SELECT COUNT(*), SUM(grp), SUM(LENGTH(body)) FROM sorted");
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        projection_memory_t memory = {0};
        result_t *result = NULL;
        CHECK(run_budgeted(db, &memory, queries[q], &result) == DATABASE_EXEC_OK && result_kind(result) == RESULT_KIND_ROWS,
              "%s: 예산 없이 성공", queries[q]);
        size_t rows = 0;
        result_row_t row;
        while (result_next(result, &row) == RESULT_NEXT_ROW) rows++;
        result_release(&result);
        CHECK(rows == expected_rows[q] && memory.used == 0, "%s: 행 %zu, 남은 계상 %zu", queries[q], rows, memory.used);
        size_t peak = memory.peak;
        memory.peak = 0;
        memory.limit = peak / 2;
        database_exec_status_t limited = run_budgeted(db, &memory, queries[q], &result);
        bool refused = limited == DATABASE_EXEC_NOMEM ||
                       (limited == DATABASE_EXEC_OK && result != NULL && result_kind(result) == RESULT_KIND_ERROR);
        CHECK(refused, "%s: 예산 %zu(최대 %zu의 절반)에서 결과를 자르지 않고 거절", queries[q], memory.limit, peak);
        result_release(&result);
        CHECK(memory.used == 0, "%s: 거절 뒤 계상 정리", queries[q]);
        /* 어느 지점에서 한도에 걸려도 계상이 0으로 돌아온다 */
        for (size_t limit = 0; limit < peak; limit += peak / 97 + 1) {
            projection_memory_t swept = {.limit = limit};
            run_budgeted(db, &swept, queries[q], &result);
            result_release(&result);
            CHECK(swept.used == 0, "%s: 한도 %zu에서 계상 %zu 남음", queries[q], limit, swept.used);
        }
    }
    /* 한도에 걸린 변경 문장은 아무것도 바꾸지 않았다 */
    char *after = query(db, "SELECT COUNT(*), SUM(grp), SUM(LENGTH(body)) FROM sorted");
    CHECK(strcmp(before, after) == 0, "거절 뒤 데이터 보존: %s / %s", before, after);
    free(before);
    free(after);
    /* 정렬이 없는 LIMIT은 모으지 않고 내보내다 멈춘다 */
    projection_memory_t whole = {0}, first = {0};
    result_t *result = NULL;
    run_budgeted(db, &whole, "SELECT body FROM sorted", &result);
    result_release(&result);
    run_budgeted(db, &first, "SELECT body FROM sorted LIMIT 5", &result);
    result_release(&result);
    CHECK(first.peak * 2 < whole.peak, "LIMIT 5의 최대 계상 %zu < 전체 %zu의 절반", first.peak, whole.peak);
    database_close(db);
}

/* #42: 집계의 문장 메모리는 입력 행 수가 아니라 그룹 수에 비례하고, 그룹이 예산을 넘으면 결과를 자르지 않고 거절한다 */
static size_t aggregate_peak(database_t *db, const char *sql, size_t expected_rows)
{
    projection_memory_t memory = {0};
    result_t *result = NULL;
    CHECK(run_budgeted(db, &memory, sql, &result) == DATABASE_EXEC_OK && result_kind(result) == RESULT_KIND_ROWS,
          "%s: 예산 없이 성공", sql);
    size_t rows = 0;
    result_row_t row;
    while (result_next(result, &row) == RESULT_NEXT_ROW) rows++;
    result_release(&result);
    CHECK(rows == expected_rows && memory.used == 0, "%s: 행 %zu, 남은 계상 %zu", sql, rows, memory.used);
    return memory.peak;
}

static void fill_events(database_t *db, int from, int to)
{
    for (int start = from; start <= to; start += 100) {
        char sql[40000];
        size_t used = (size_t)snprintf(sql, sizeof(sql), "INSERT INTO events VALUES ");
        for (int i = start; i < start + 100; i++) {
            used += (size_t)snprintf(sql + used, sizeof(sql) - used, "%s(%d, %d, '%0200d')", i == start ? "" : ", ", i,
                                     i % 4, i);
        }
        exec_ok(db, sql);
    }
}

static void test_aggregate_memory_scales_with_groups(void)
{
    printf("\n[test_aggregate_memory_scales_with_groups]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE events (id INTEGER PRIMARY KEY, kind INTEGER, body TEXT)");
    const char *grouped = "SELECT kind, COUNT(*), SUM(id), MAX(body) FROM events GROUP BY kind";
    const char *whole = "SELECT COUNT(*), SUM(id), MIN(body) FROM events";
    fill_events(db, 1, 400);
    size_t grouped_small = aggregate_peak(db, grouped, 4), whole_small = aggregate_peak(db, whole, 1);
    fill_events(db, 401, 4000);
    size_t grouped_large = aggregate_peak(db, grouped, 4), whole_large = aggregate_peak(db, whole, 1);
    /* 행이 10배여도 그룹 4개의 상태와 행 하나의 버퍼만 쥔다(모든 행을 모으면 수백 KB가 늘어난다) */
    CHECK(grouped_large <= grouped_small + 4096 && whole_large <= whole_small + 4096,
          "행 400 → 4000에서 최대 계상 GROUP BY %zu → %zu, 전체 집계 %zu → %zu", grouped_small, grouped_large,
          whole_small, whole_large);
    expect_text(db, "SELECT kind, COUNT(*), SUM(id) FROM events GROUP BY kind",
                "[4] 0|1000|2002000/ 1|1000|1999000/ 2|1000|2000000/ 3|1000|2001000");

    /* 그룹이 많아 예산을 넘으면(다중 키·HAVING 포함) 결과를 자르지 않고 거절하고 계상을 모두 돌려준다 */
    const char *many = "SELECT body, kind, COUNT(*) FROM events GROUP BY body, kind HAVING COUNT(*) > 0";
    size_t peak = aggregate_peak(db, many, 4000);
    for (size_t limit = 1; limit < peak; limit += peak / 37 + 1) {
        projection_memory_t swept = {.limit = limit};
        result_t *result = NULL;
        database_exec_status_t status = run_budgeted(db, &swept, many, &result);
        bool refused = status == DATABASE_EXEC_NOMEM ||
                       (status == DATABASE_EXEC_OK && result != NULL && result_kind(result) == RESULT_KIND_ERROR);
        CHECK(refused, "한도 %zu(최대 %zu)에서 결과를 자르지 않고 거절", limit, peak);
        result_release(&result);
        CHECK(swept.used == 0, "한도 %zu에서 계상 %zu 남음", limit, swept.used);
    }
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/*
 * #140: 정수 기본 키 테이블의 비고유 색인은 같은 값 안에서 기본 키 순서다. WHERE 열 = 값 ORDER BY 기본 키 LIMIT은
 * 정렬하지 않고 색인 순서로 내보내다 LIMIT에서 멈춘다. 행 이동·기본 키 변경·삭제·재시작 뒤에도 결과가 같다.
 */
static void expect_same_order(database_t *db, const char *fast, const char *slow)
{
    char *got = query(db, fast), *want = query(db, slow);
    CHECK(strcmp(got, want) == 0, "%s\n      기대: %.200s\n      실제: %.200s", fast, want, got);
    free(got);
    free(want);
}

static void check_ordered_index(database_t *db)
{
    expect_same_order(db, "SELECT id, v FROM q WHERE state = 0 ORDER BY id LIMIT 5",
                      "SELECT id, v FROM q WHERE state + 0 = 0 ORDER BY id LIMIT 5");
    expect_same_order(db, "SELECT id FROM q WHERE state = 1 ORDER BY id LIMIT 7 OFFSET 3",
                      "SELECT id FROM q WHERE state + 0 = 1 ORDER BY id LIMIT 7 OFFSET 3");
    expect_same_order(db, "SELECT id FROM q WHERE state = 2 AND id > 900 ORDER BY id LIMIT 4",
                      "SELECT id FROM q WHERE state + 0 = 2 AND id + 0 > 900 ORDER BY id LIMIT 4");
    expect_same_order(db, "SELECT id FROM q WHERE id > 1500 ORDER BY id LIMIT 3",
                      "SELECT id FROM q WHERE id + 0 > 1500 ORDER BY id LIMIT 3");
    expect_same_order(db, "SELECT COUNT(*) FROM (SELECT id FROM q WHERE state = 0 ORDER BY id)",
                      "SELECT COUNT(*) FROM q WHERE state = 0");
}

static void test_ordered_secondary_index(void)
{
    printf("\n[test_ordered_secondary_index]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE q (id INTEGER PRIMARY KEY, state INTEGER NOT NULL, v TEXT)");
    exec_ok(db, "CREATE INDEX q_state ON q (state)");
    /* id를 거꾸로 넣어 힙 순서와 기본 키 순서를 다르게 한다 */
    for (int start = 2000; start > 0; start -= 100) {
        char sql[8192];
        size_t used = (size_t)snprintf(sql, sizeof(sql), "INSERT INTO q VALUES ");
        for (int id = start; id > start - 100; id--) {
            used += (size_t)snprintf(sql + used, sizeof(sql) - used, "%s(%d, %d, 'v%d')", id == start ? "" : ", ", id,
                                     id % 3, id);
        }
        exec_ok(db, sql);
    }
    check_ordered_index(db);
    /* 긴 값으로 바꿔 행을 옮기고, 상태와 기본 키를 바꾸고, 지운다 */
    exec_ok(db, "UPDATE q SET v = 'a much longer value that no longer fits in the old slot' WHERE id % 7 = 0");
    exec_ok(db, "UPDATE q SET state = 0 WHERE id % 11 = 0");
    exec_ok(db, "UPDATE q SET id = id + 5000 WHERE id % 13 = 0 AND id < 400");
    exec_ok(db, "DELETE FROM q WHERE id % 17 = 0");
    check_ordered_index(db);
    db = reopen(db, true);
    check_ordered_index(db);
    /* 색인 순서로 내보내면 LIMIT 2에서 멈추므로 모아서 정렬하는 DESC보다 문장 메모리가 훨씬 작다 */
    projection_memory_t streamed = {0}, sorted = {0};
    result_t *result = NULL;
    run_budgeted(db, &streamed, "SELECT id, v FROM q WHERE state = 0 ORDER BY id LIMIT 2", &result);
    result_release(&result);
    run_budgeted(db, &sorted, "SELECT id, v FROM q WHERE state = 0 ORDER BY id DESC LIMIT 2", &result);
    result_release(&result);
    CHECK(streamed.peak * 4 < sorted.peak, "색인 순서 LIMIT의 최대 계상 %zu < 정렬 %zu의 1/4", streamed.peak, sorted.peak);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

/* #81·#91: 데이터 페이지는 닿거나(카탈로그·heap·overflow·색인) 빈 목록에 정확히 한 번 있다. 고아와 이중 소유가 없다 */
static void expect_pages_accounted(database_t *db, const char *when)
{
    database_page_check_t check;
    database_status_t status = database_check_pages(db, &check);
    CHECK(status == DATABASE_OK && check.orphaned == 0 && check.conflicts == 0 &&
              check.reachable + check.free == check.data_pages,
          "%s: 상태 %d, 페이지 %llu = 닿음 %llu + 빈 %llu, 고아 %llu, 충돌 %llu", when, (int)status,
          (unsigned long long)check.data_pages, (unsigned long long)check.reachable,
          (unsigned long long)check.free, (unsigned long long)check.orphaned, (unsigned long long)check.conflicts);
}

/*
 * 페이지 목록은 디렉터리·행 페이지 수만큼 잡아 두고 색인 페이지를 먼저 넣었다. 색인이 그 여유를 넘게 크면 뒤의 행 페이지가
 * 목록 끝 밖에 쓰였다(DROP TABLE과 페이지 회계가 같은 함수를 쓴다). 색인이 여러 단계인 표로 둘 다 확인한다.
 */
static void test_page_list_with_deep_index(void)
{
    printf("\n[test_page_list_with_deep_index]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE deep (id INTEGER PRIMARY KEY, name TEXT, note TEXT)");
    exec_ok(db, "CREATE INDEX deep_name ON deep (name)");
    char *sql = malloc(64 * 500 + 64);
    assert(sql != NULL);
    for (int start = 1; start <= 10000; start += 500) {
        size_t used = (size_t)sprintf(sql, "INSERT INTO deep VALUES ");
        for (int id = start; id < start + 500; id++) {
            used += (size_t)sprintf(sql + used, "%s(%d, 'name-%d', 'n')", id == start ? "" : ", ", id, id);
        }
        exec_ok(db, sql);
    }
    free(sql);
    expect_pages_accounted(db, "색인이 깊은 표");
    exec_ok(db, "DROP TABLE deep");
    expect_pages_accounted(db, "깊은 표를 지운 뒤");
    assert(database_close(db) == DATABASE_OK);
}

static void test_page_accounting_with_large_values(void)
{
    printf("\n[test_page_accounting_with_large_values]\n");
    database_t *db = fresh_db();
    expect_pages_accounted(db, "빈 DB");
    exec_ok(db, "CREATE TABLE big (id INTEGER PRIMARY KEY, k TEXT, body BLOB)");
    exec_ok(db, "CREATE INDEX big_k ON big (k)");
    size_t size = 30000;
    uint8_t *blob = malloc(size);
    assert(blob != NULL);
    for (size_t i = 0; i < size; i++) blob[i] = (uint8_t)(i * 31 + 7);
    for (int i = 1; i <= 12; i++) {
        value_view_t params[3] = {{VALUE_INTEGER, i, 0.0, NULL, 0}, {VALUE_TEXT, 0, 0.0, (const uint8_t *)"key", 3},
                                  {VALUE_BLOB, 0, 0.0, blob, size / (size_t)(i % 3 + 1)}};
        result_t *r = run_bound(db, "INSERT INTO big VALUES (?, ?, ?)", params, 3);
        result_release(&r);
    }
    expect_pages_accounted(db, "큰 값 넣은 뒤");
    exec_ok(db, "UPDATE big SET body = X'00' WHERE id % 2 = 0");
    expect_pages_accounted(db, "큰 값 줄인 뒤");
    value_view_t grow[1] = {{VALUE_BLOB, 0, 0.0, blob, size}};
    result_t *r = run_bound(db, "UPDATE big SET body = ? WHERE id % 3 = 0", grow, 1);
    result_release(&r);
    exec_ok(db, "DELETE FROM big WHERE id > 9");
    expect_pages_accounted(db, "키우고 지운 뒤");
    exec_ok(db, "CREATE TABLE gone (id INTEGER PRIMARY KEY, body TEXT)");
    exec_ok(db, "INSERT INTO gone VALUES (1, 'x')");
    exec_ok(db, "DROP TABLE gone");
    db = reopen(db, true);
    expect_pages_accounted(db, "재열기 뒤");
    for (int round = 0; round < 3; round++) exec_ok(db, "UPDATE big SET k = k || 'x'");
    db = reopen(db, true);
    expect_pages_accounted(db, "세대를 여러 번 바꾸고 재열기한 뒤");
    free(blob);
    assert(database_close(db) == DATABASE_OK);
    clean_dir();
}

static void test_defaults_survive_reopen(void)
{
    printf("\n[test_defaults_survive_reopen]\n");
    database_t *db = fresh_db();
    exec_ok(db, "CREATE TABLE jobs (id INTEGER PRIMARY KEY AUTOINCREMENT, state TEXT NOT NULL DEFAULT 'ready', w REAL DEFAULT 2, "
                "b BLOB DEFAULT X'0102', n INTEGER)");
    exec_ok(db, "INSERT INTO jobs (id) VALUES (1)");
    db = reopen(db, true);
    exec_ok(db, "INSERT INTO jobs (id, n) VALUES (2, 9)");
    exec_ok(db, "INSERT INTO jobs DEFAULT VALUES");
    expect_text(db, "SELECT id, state, w, LENGTH(b), n FROM jobs ORDER BY id",
                "[3] 1|'ready'|2.0|2|NULL/ 2|'ready'|2.0|2|9/ 3|'ready'|2.0|2|NULL");
    expect_class(db, "CREATE TABLE bad (a INTEGER DEFAULT 'x')", RESULT_E_SEMANTIC);
    expect_class(db, "CREATE TABLE bad (a INTEGER DEFAULT (SELECT 1))", RESULT_E_SEMANTIC);
    database_close(db);
}

int main(void)
{
    test_defaults_survive_reopen();
    test_two_schemas_crud_and_reopen();
    test_values_round_trip_across_page_sizes();
    test_failed_statements_keep_existing_data();
    test_drop_removes_only_the_target();
    test_many_rows_updates_and_deletes_across_pages();
    test_identity_and_primary_key_rules();
    test_text_primary_key_index();
    test_result_column_metadata();
    test_primary_key_range();
    test_secondary_index_crud();
    test_secondary_index_many_duplicates();
    test_projected_overflow_reads();
    test_open_rejects_other_files_without_change();
    test_argument_errors();
    test_order_and_distinct_respect_budget();
    test_aggregate_memory_scales_with_groups();
    test_ordered_secondary_index();
    test_page_accounting_with_large_values();
    test_page_list_with_deep_index();
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
