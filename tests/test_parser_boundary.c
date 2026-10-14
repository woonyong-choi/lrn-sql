/*
 * test_parser_boundary.c -- 이슈 #126: 긴 SQL 입력은 전체를 검증하거나 실행 없이 거절한다
 *
 * 실제 db_execute 경로로 경계 입력, 거절된 입력의 저장 효과 0, 거절 뒤 정상 질의를 검사한다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "db.h"
#include "sql/parser.h"
#include "storage/pager.h"

#define TEST_DB_PATH "__test_parser_boundary.db"
#define SELECT_ALL   "SELECT * FROM users"

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define CHECK(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

static int run_status(pager_t *pager, const char *sql)
{
    exec_result_t r = db_execute(pager, sql);
    free(r.out_buf);
    return r.status;
}

/* 현재 테이블 내용을 복사해 반환한다. 호출자가 free */
static char *snapshot(pager_t *pager)
{
    exec_result_t r = db_execute(pager, SELECT_ALL);
    assert(r.status == 0 && r.out_buf != NULL);
    return r.out_buf;
}

static void open_fresh_db(pager_t *pager)
{
    unlink(TEST_DB_PATH);
    assert(pager_open(pager, TEST_DB_PATH, true) == 0);
    assert(run_status(pager, "CREATE TABLE users (name VARCHAR(32), age INT)") == 0);
    assert(run_status(pager, "INSERT INTO users VALUES ('alice', 20)") == 0);
    assert(run_status(pager, "INSERT INTO users VALUES ('bob', 30)") == 0);
}

static void close_db(pager_t *pager)
{
    pager_close(pager);
    unlink(TEST_DB_PATH);
}

/* head 뒤를 공백으로 채워 전체 길이를 total로 맞추고 tail을 붙인다 */
static char *pad_statement(const char *head, size_t total, const char *tail)
{
    size_t tail_len = strlen(tail);
    size_t head_len = strlen(head);
    assert(head_len + tail_len <= total);
    char *sql = malloc(total + 1);
    assert(sql != NULL);
    memcpy(sql, head, head_len);
    memset(sql + head_len, ' ', total - head_len - tail_len);
    memcpy(sql + total - tail_len, tail, tail_len);
    sql[total] = '\0';
    return sql;
}

/* 거절된 입력은 -1을 반환하고 저장 내용을 바꾸지 않으며 다음 정상 질의를 받는다 */
static void check_rejected(pager_t *pager, const char *sql, const char *label)
{
    char *before = snapshot(pager);

    CHECK(run_status(pager, sql) == -1, label);

    char *after = snapshot(pager);
    CHECK(strcmp(before, after) == 0, label);
    free(before);
    free(after);
}

typedef struct {
    const char *label;
    const char *head;
    const char *tail;
} suffix_case_t;

/* #126: 4095바이트 뒤에 오는 잘못된 suffix가 절단으로 사라져 앞부분만 실행되던 결함 */
static void parse_long_statement_with_invalid_suffix_is_rejected(void)
{
    static const suffix_case_t cases[] = {
        {"UPDATE", "UPDATE users SET name = 'hijacked' WHERE id = 1", "ZZZ"},
        {"DELETE", "DELETE FROM users WHERE id = 1", "ZZZ"},
        {"INSERT", "INSERT INTO users VALUES ('mallory', 99)", "ZZZ"},
        {"UPDATE-semicolon", "UPDATE users SET age = 77 WHERE id = 2", "; DROP TABLE users"},
    };
    pager_t pager;
    open_fresh_db(&pager);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *sql = pad_statement(cases[i].head, PARSER_MAX_INPUT_LEN + 5, cases[i].tail);
        check_rejected(&pager, sql, cases[i].label);
        free(sql);
    }

    close_db(&pager);
}

/* 4094·4095바이트는 실행되고 4096바이트부터 거절한다 */
static void parse_boundary_lengths_accept_up_to_limit_and_reject_beyond(void)
{
    static const size_t accepted[] = {PARSER_MAX_INPUT_LEN - 1, PARSER_MAX_INPUT_LEN};
    pager_t pager;
    open_fresh_db(&pager);

    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        char *sql = pad_statement("UPDATE users SET age = 41 WHERE id = 1", accepted[i], "");
        CHECK(run_status(&pager, sql) == 0, "boundary length statement runs");
        free(sql);
    }
    char *over = pad_statement("UPDATE users SET age = 99 WHERE id = 2",
                               PARSER_MAX_INPUT_LEN + 1, "");
    check_rejected(&pager, over, "4096 byte statement rejected");
    free(over);

    char *rows = snapshot(&pager);
    CHECK(strstr(rows, "41") != NULL, "accepted boundary update applied");
    CHECK(strstr(rows, "99") == NULL, "rejected update not applied");
    free(rows);

    close_db(&pager);
}

/* 다중 바이트 문자가 한계에 걸쳐도 조각난 바이트로 실행되지 않는다 */
static void parse_multibyte_character_at_limit_is_rejected(void)
{
    static const char hangul[] = "\xEA\xB0\x80"; /* U+AC00, 3바이트 */
    static const size_t ends[] = {PARSER_MAX_INPUT_LEN - 1, PARSER_MAX_INPUT_LEN,
                                  PARSER_MAX_INPUT_LEN + 1};
    pager_t pager;
    open_fresh_db(&pager);

    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        char *sql = pad_statement("UPDATE users SET age = 55 WHERE id = 1", ends[i], hangul);
        check_rejected(&pager, sql, "multibyte tail at limit rejected");
        free(sql);
    }
    CHECK(run_status(&pager, "UPDATE users SET name = '\xEA\xB0\x80\xEB\x82\x98' WHERE id = 1") == 0,
          "multibyte literal accepted");
    char *rows = snapshot(&pager);
    CHECK(strstr(rows, "\xEA\xB0\x80\xEB\x82\x98") != NULL, "multibyte literal stored whole");
    free(rows);

    close_db(&pager);
}

typedef struct {
    const char *label;
    const char *sql;
} bad_sql_case_t;

/* 닫히지 않은 따옴표와 필드 한계 초과는 일부만 저장하지 않고 거절한다 */
static void parse_unterminated_or_overflowing_input_is_rejected(void)
{
    static const bad_sql_case_t cases[] = {
        {"unterminated literal", "UPDATE users SET name = 'abc WHERE id = 1"},
        {"unterminated insert literal", "INSERT INTO users VALUES ('abc, 5)"},
        {"varchar size wraps uint16", "CREATE TABLE wide (name VARCHAR(70000))"},
        {"varchar size zero", "CREATE TABLE wide (name VARCHAR(0))"},
        {"varchar size not a number", "CREATE TABLE wide (name VARCHAR(abc))"},
        {"column name over 31 bytes",
         "CREATE TABLE wide (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa INT)"},
    };
    pager_t pager;
    open_fresh_db(&pager);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check_rejected(&pager, cases[i].sql, cases[i].label);

    char *literal = malloc(400);
    char *sql = malloc(500);
    assert(literal != NULL && sql != NULL);
    memset(literal, 'x', 300);
    literal[300] = '\0';
    snprintf(sql, 500, "UPDATE users SET name = '%s' WHERE id = 1", literal);
    check_rejected(&pager, sql, "string literal over 255 bytes");
    memset(literal, '7', 300);
    snprintf(sql, 500, "INSERT INTO users VALUES ('x', %s)", literal);
    check_rejected(&pager, sql, "unquoted value over 255 bytes");
    free(literal);
    free(sql);

    close_db(&pager);
}

/* EXPLAIN 중첩은 재귀하지 않고 거절한다 */
static void parse_nested_explain_is_rejected(void)
{
    char sql[PARSER_MAX_INPUT_LEN + 1];
    size_t len = 0;
    while (len + 8 + sizeof(SELECT_ALL) <= PARSER_MAX_INPUT_LEN) {
        memcpy(sql + len, "EXPLAIN ", 8);
        len += 8;
    }
    strcpy(sql + len, SELECT_ALL);
    statement_t stmt;

    CHECK(parse(sql, &stmt) == -1, "nested explain rejected");
}

/* 긴 입력을 거절한 뒤 정상 문장은 조각나지 않고 실행되며, 재열기 뒤에도 같다 */
static void db_execute_after_rejected_long_input_runs_next_statement_whole(void)
{
    pager_t pager;
    open_fresh_db(&pager);
    char *sql = pad_statement("UPDATE users SET name = 'tail' WHERE id = 1",
                              PARSER_MAX_INPUT_LEN + 100, "ZZZ");

    CHECK(run_status(&pager, sql) == -1, "long input rejected");
    free(sql);
    CHECK(run_status(&pager, "UPDATE users SET age = 21 WHERE id = 1") == 0,
          "next statement runs after rejection");
    char *before_reopen = snapshot(&pager);

    pager_close(&pager);
    assert(pager_open(&pager, TEST_DB_PATH, false) == 0);
    char *after_reopen = snapshot(&pager);

    CHECK(strcmp(before_reopen, after_reopen) == 0, "data identical after reopen");
    CHECK(strstr(after_reopen, "tail") == NULL, "rejected update not persisted");
    CHECK(strstr(after_reopen, "21") != NULL, "next statement persisted");
    free(before_reopen);
    free(after_reopen);

    close_db(&pager);
}

/* 파일 전체 바이트를 읽는다. 호출자가 free */
static uint8_t *read_file_bytes(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size >= 0 && fseek(f, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)size + 1);
    assert(bytes != NULL && fread(bytes, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    *len = (size_t)size;
    return bytes;
}

typedef struct {
    const char *label;
    const char *type;
    bool accepted;
} ddl_width_case_t;

/*
 * 선언 폭·타입 토큰마다 별개의 빈 DB에서 실행한다.
 * 거절은 파일 바이트와 스키마를 바꾸지 않고, 같은 프로세스의 정상 DDL/INSERT와 재열기가 이어져야 한다.
 */
static void ddl_width_and_token_boundaries_reject_before_publish(void)
{
    static const ddl_width_case_t cases[] = {
        {"varchar zero", "VARCHAR(0)", false},
        {"varchar negative", "VARCHAR(-1)", false},
        {"varchar digits then junk", "VARCHAR(4junk)", false},
        {"varchar uint16 max", "VARCHAR(65535)", false},
        {"varchar uint16 max plus 1", "VARCHAR(65536)", false},
        {"varchar 254", "VARCHAR(254)", true},
        {"varchar 255", "VARCHAR(255)", true},
        {"varchar 256", "VARCHAR(256)", false},
        {"varchar 257", "VARCHAR(257)", false},
        {"bigint prefix match", "BIGINTjunk", false},
        {"int prefix match", "INTjunk", false},
        {"int longer name", "INTEGER", false},
    };
    char sql[128];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ddl_width_case_t *c = &cases[i];
        unlink(TEST_DB_PATH);
        pager_t pager;
        assert(pager_open(&pager, TEST_DB_PATH, true) == 0);
        pager_close(&pager);
        size_t before_len;
        uint8_t *before = read_file_bytes(TEST_DB_PATH, &before_len);

        assert(pager_open(&pager, TEST_DB_PATH, false) == 0);
        snprintf(sql, sizeof(sql), "CREATE TABLE t (name %s)", c->type);
        int status = run_status(&pager, sql);
        CHECK((status == 0) == c->accepted, c->label);
        if (!c->accepted) {
            CHECK(pager.header.column_count == 0 && pager.header.row_size == 0, c->label);
            pager_close(&pager);
            size_t after_len;
            uint8_t *after = read_file_bytes(TEST_DB_PATH, &after_len);
            CHECK(after_len == before_len && memcmp(before, after, before_len) == 0, c->label);
            free(after);
            assert(pager_open(&pager, TEST_DB_PATH, false) == 0);
            CHECK(run_status(&pager, "CREATE TABLE t (name VARCHAR(8))") == 0, c->label);
        }
        CHECK(run_status(&pager, "INSERT INTO t (name) VALUES ('x')") == 0, c->label);
        pager_close(&pager);

        assert(pager_open(&pager, TEST_DB_PATH, false) == 0);
        exec_result_t r = db_execute(&pager, "SELECT * FROM t");
        CHECK(r.status == 0 && r.out_buf != NULL && strstr(r.out_buf, "x") != NULL, c->label);
        free(r.out_buf);
        close_db(&pager);
        free(before);
    }
}

/* 255바이트 폭의 값은 그대로 들어가고 256바이트 값은 거절되며, 폭 최대 테이블도 재열기 뒤 읽힌다 */
static void ddl_widest_row_fits_page_and_value_over_width_is_rejected(void)
{
    char cols[512] = "";
    for (int i = 0; i < MAX_COLUMNS - 1; i++) {
        char col[32];
        snprintf(col, sizeof(col), "%sc%d VARCHAR(255)", i == 0 ? "" : ", ", i);
        strcat(cols, col);
    }
    char *value = malloc(300);
    char *sql = malloc(MAX_COLUMNS * 300);
    assert(value != NULL && sql != NULL);

    unlink(TEST_DB_PATH);
    pager_t pager;
    assert(pager_open(&pager, TEST_DB_PATH, true) == 0);
    snprintf(sql, 512 + 64, "CREATE TABLE wide (%s)", cols);
    CHECK(run_status(&pager, sql) == 0, "widest table accepted");
    CHECK(pager.header.row_size + sizeof(slot_t) + sizeof(heap_page_header_t) <= pager.page_size,
          "widest row fits one heap page");

    memset(value, 'v', 256);
    value[256] = '\0';
    sql[0] = '\0';
    strcat(sql, "INSERT INTO wide VALUES (");
    for (int i = 0; i < MAX_COLUMNS - 1; i++) {
        strcat(sql, i == 0 ? "'" : ", '");
        strcat(sql, value);
        strcat(sql, "'");
    }
    strcat(sql, ")");
    CHECK(run_status(&pager, sql) != 0, "256 byte value rejected in VARCHAR(255)");

    value[255] = '\0';
    sql[0] = '\0';
    strcat(sql, "INSERT INTO wide VALUES (");
    for (int i = 0; i < MAX_COLUMNS - 1; i++) {
        strcat(sql, i == 0 ? "'" : ", '");
        strcat(sql, value);
        strcat(sql, "'");
    }
    strcat(sql, ")");
    CHECK(run_status(&pager, sql) == 0, "255 byte values stored");
    pager_close(&pager);

    assert(pager_open(&pager, TEST_DB_PATH, false) == 0);
    exec_result_t r = db_execute(&pager, "SELECT * FROM wide");
    CHECK(r.status == 0 && r.out_buf != NULL && strstr(r.out_buf, value) != NULL, "255 byte value read back");
    free(r.out_buf);
    close_db(&pager);
    free(value);
    free(sql);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    db_init();
    parse_long_statement_with_invalid_suffix_is_rejected();
    parse_boundary_lengths_accept_up_to_limit_and_reject_beyond();
    parse_multibyte_character_at_limit_is_rejected();
    parse_unterminated_or_overflowing_input_is_rejected();
    parse_nested_explain_is_rejected();
    db_execute_after_rejected_long_input_runs_next_statement_whole();
    ddl_width_and_token_boundaries_reject_before_publish();
    ddl_widest_row_fits_page_and_value_over_width_is_rejected();
    db_destroy();

    printf("parser boundary: %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
