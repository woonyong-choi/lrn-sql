/*
 * test_parser_boundary.c -- 이슈 #126: 긴 SQL 입력은 전체를 검증하거나 실행 없이 거절한다
 *
 * 실제 db_execute 경로로 경계 입력, 거절된 입력의 저장 효과 0, 거절 뒤 정상 질의를 검사한다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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
    db_destroy();

    printf("parser boundary: %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
