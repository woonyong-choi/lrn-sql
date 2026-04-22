/*
 * test_step0_db_execute.c -- Step 0 테스트: db_execute 경계 함수 + out_buf 검증
 *
 * 검증 항목:
 *   1. db_execute()가 parse → execute를 올바���게 연결한다
 *   2. SELECT 결과가 out_buf에 담긴다 (printf가 아님)
 *   3. INSERT/DELETE/CREATE TABLE 결과 메시���가 정확하다
 *   4. 잘못된 SQL에 대해 status=-1을 반환한다
 *   5. out_buf 메모리 해제가 올바르다 (sanitizer clean)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <inttypes.h>

#include "db.h"
#include "storage/pager.h"
#include "storage/schema.h"
#include "storage/bptree.h"

#define TEST_DB_PREFIX "__test_step0_"

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define CLR_GREEN  "\033[32m"
#define CLR_RED    "\033[31m"
#define CLR_YELLOW "\033[33m"
#define CLR_RESET  "\033[0m"

#define ASSERT_TRUE(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf(CLR_RED "  FAIL: %s (line %d)" CLR_RESET "\n", msg, __LINE__); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b, msg) ASSERT_TRUE((a) == (b), msg)

static char *test_db_path(const char *suffix) {
    static char buf[256];
    snprintf(buf, sizeof(buf), TEST_DB_PREFIX "%s.db", suffix);
    return buf;
}

static void setup_test_db(pager_t *pager, const char *name) {
    char *path = test_db_path(name);
    unlink(path);
    assert(pager_open(pager, path, true) == 0);
}

static void teardown_test_db(pager_t *pager, const char *name) {
    pager_close(pager);
    unlink(test_db_path(name));
}

/* ════════════════════════════════════════════════════════════ */
/*  1. db_execute 기본 동작                                    */
/* ════════════════════════════════════════════════════════════ */
static void test_db_execute_basic(void) {
    printf(CLR_YELLOW "\n[test_db_execute_basic]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "basic");

    /* CREATE TABLE */
    exec_result_t r = db_execute(&pager,
        "CREATE TABLE users (name VARCHAR(32), age INT)");
    ASSERT_EQ_INT(r.status, 0, "CREATE TABLE succeeds");
    if (r.out_buf) free(r.out_buf);

    /* INSERT */
    r = db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    ASSERT_EQ_INT(r.status, 0, "INSERT succeeds");
    ASSERT_EQ_INT((int)pager.header.row_count, 1, "row_count == 1");
    if (r.out_buf) free(r.out_buf);

    /* SELECT by id -- 결과가 out_buf에 담기는지 */
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "SELECT succeeds");
    ASSERT_TRUE(r.out_buf != NULL, "out_buf is not NULL");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Alice") != NULL,
                    "out_buf contains 'Alice'");
        ASSERT_TRUE(strstr(r.out_buf, "25") != NULL,
                    "out_buf contains '25'");
        free(r.out_buf);
    }

    /* 에러 케이스 */
    r = db_execute(&pager, "GARBAGE QUERY");
    ASSERT_EQ_INT(r.status, -1, "invalid SQL returns error");
    if (r.out_buf) free(r.out_buf);

    teardown_test_db(&pager, "basic");
}

/* ════════════════════════════════════════════════════════════ */
/*  2. 다중 행 SELECT -- out_buf에 모든 행이 담기는지           */
/* ════════════════════════════════════════════════════════════ */
static void test_db_execute_multi_row(void) {
    printf(CLR_YELLOW "\n[test_db_execute_multi_row]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "multi");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 50; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', %d)", i, 20 + i);
        exec_result_t r = db_execute(&pager, sql);
        if (r.out_buf) free(r.out_buf);
    }

    /* SELECT * (table scan, no WHERE) */
    exec_result_t r = db_execute(&pager, "SELECT * FROM users");
    ASSERT_EQ_INT(r.status, 0, "SELECT * succeeds");
    ASSERT_TRUE(r.out_buf != NULL, "out_buf not NULL for multi-row");

    if (r.out_buf) {
        /* 줄바꿈 수로 행 수 대략 확인 (헤더 2줄 + 데이터 50줄) */
        int newlines = 0;
        for (size_t i = 0; i < r.out_len; i++) {
            if (r.out_buf[i] == '\n') newlines++;
        }
        ASSERT_TRUE(newlines >= 52, "at least 52 lines (2 header + 50 data)");
        /* 첫 번째 행과 마지막 행 내용 확인 */
        ASSERT_TRUE(strstr(r.out_buf, "user0") != NULL, "contains user0");
        ASSERT_TRUE(strstr(r.out_buf, "user49") != NULL, "contains user49");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "multi");
}

/* ════════════════════════════════════════════════════════════ */
/*  3. DELETE 라운드트립                                        */
/* ════════════════════════════════════════════════════════════ */
static void test_db_execute_delete_roundtrip(void) {
    printf(CLR_YELLOW "\n[test_db_execute_delete_roundtrip]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "del");

    exec_result_t r;
    r = db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    if (r.out_buf) free(r.out_buf);
    r = db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    if (r.out_buf) free(r.out_buf);
    r = db_execute(&pager, "INSERT INTO users VALUES ('Bob', 30)");
    if (r.out_buf) free(r.out_buf);

    /* DELETE id=1 */
    r = db_execute(&pager, "DELETE FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "DELETE succeeds");
    ASSERT_EQ_INT((int)pager.header.row_count, 1, "1 row remaining");
    if (r.out_buf) free(r.out_buf);

    /* SELECT * -- Alice 가 사라졌는지 */
    r = db_execute(&pager, "SELECT * FROM users");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL,
                    "Alice not in result after delete");
        ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL,
                    "Bob still in result");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "del");
}

/* ════════════════════════════════════════════════════════════ */
/*  4. EXPLAIN -- out_buf에 실행 계획이 담기는지                */
/* ════════════════════════════════════════════════════════════ */
static void test_db_execute_explain(void) {
    printf(CLR_YELLOW "\n[test_db_execute_explain]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "explain");

    exec_result_t r;
    r = db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    if (r.out_buf) free(r.out_buf);

    r = db_execute(&pager, "EXPLAIN SELECT * FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "EXPLAIN succeeds");
    ASSERT_TRUE(r.out_buf != NULL, "EXPLAIN out_buf not NULL");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "INDEX_LOOKUP") != NULL,
                    "EXPLAIN contains INDEX_LOOKUP");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "explain");
}

/* ════════════════════════════════════════════════════════════ */
/*  5. 대량 INSERT + 재오픈 + 검증 (영속성)                     */
/* ════════════════════════════════════════════════════════════ */
static void test_db_execute_persistence(void) {
    printf(CLR_YELLOW "\n[test_db_execute_persistence]" CLR_RESET "\n");
    char *path = test_db_path("persist");
    unlink(path);

    /* Phase 1: 생성 + INSERT 100건 */
    {
        pager_t pager;
        assert(pager_open(&pager, path, true) == 0);
        exec_result_t r;
        r = db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
        if (r.out_buf) free(r.out_buf);

        for (int i = 0; i < 100; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO users VALUES ('u%d', %d)", i, i);
            r = db_execute(&pager, sql);
            if (r.out_buf) free(r.out_buf);
        }
        ASSERT_EQ_INT((int)pager.header.row_count, 100, "100 rows inserted");
        pager_close(&pager);
    }

    /* Phase 2: 재오픈 + 검증 */
    {
        pager_t pager;
        assert(pager_open(&pager, path, false) == 0);
        ASSERT_EQ_INT((int)pager.header.row_count, 100, "100 rows after reopen");

        exec_result_t r = db_execute(&pager, "SELECT * FROM users WHERE id = 50");
        ASSERT_TRUE(r.out_buf != NULL, "row 50 found after reopen");
        if (r.out_buf) {
            ASSERT_TRUE(strstr(r.out_buf, "u49") != NULL, "row 50 name=u49");
            free(r.out_buf);
        }

        pager_close(&pager);
    }

    unlink(path);
}

/* ════════════════════════════════════════════════════════════ */
/*  main                                                       */
/* ════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== Step 0: db_execute Test Suite ===\n");

    test_db_execute_basic();
    test_db_execute_multi_row();
    test_db_execute_delete_roundtrip();
    test_db_execute_explain();
    test_db_execute_persistence();

    printf("\n════════════════════════════════════\n");
    if (g_tests_failed == 0) {
        printf(CLR_GREEN "ALL PASSED: %d/%d" CLR_RESET "\n",
               g_tests_passed, g_tests_run);
    } else {
        printf(CLR_RED "FAILED: %d/%d passed (%d failures)" CLR_RESET "\n",
               g_tests_passed, g_tests_run, g_tests_failed);
    }
    printf("════════════════════════════════════\n");
    return (g_tests_failed == 0) ? 0 : 1;
}
