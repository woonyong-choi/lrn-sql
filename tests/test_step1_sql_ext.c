/*
 * test_step1_sql_ext.c -- Step 1 테스트: SQL 확장 기능
 *
 * 검증 항목:
 *   1. UPDATE (인덱스 경로: WHERE id = N)
 *   2. UPDATE (스캔 경로: WHERE field = val)
 *   3. 비교 연산자 (!=, <, >, <=, >=) 필터링
 *   4. COUNT(*)
 *   5. ORDER BY (ASC, DESC)
 *   6. LIMIT
 *   7. ORDER BY + LIMIT 복합
 *   8. DROP TABLE
 *   9. EXPLAIN 확장 (UPDATE, DROP)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "db.h"
#include "storage/pager.h"

#define TEST_DB_PREFIX "__test_step1_"

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

/* 테이블 생성 + N행 삽입 헬퍼 */
static void create_and_populate(pager_t *pager, int n) {
    exec_result_t r;
    r = db_execute(pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    assert(r.status == 0);
    if (r.out_buf) free(r.out_buf);

    char sql[256];
    const char *names[] = {"Alice","Bob","Charlie","Dave","Eve"};
    int ages[] = {25, 30, 20, 35, 28};
    for (int i = 0; i < n && i < 5; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('%s', %d)",
                 names[i], ages[i]);
        r = db_execute(pager, sql);
        assert(r.status == 0);
        if (r.out_buf) free(r.out_buf);
    }
}

/* ════════════════════════════════════════════════════════════ */
/*  1. UPDATE via index (WHERE id = N)                         */
/* ════════════════════════════════════════════════════════════ */
static void test_update_by_id(void) {
    printf(CLR_YELLOW "\n[test_update_by_id]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "update_id");
    create_and_populate(&pager, 3);

    /* UPDATE users SET name = 'Alicia' WHERE id = 1 */
    exec_result_t r = db_execute(&pager,
        "UPDATE users SET name = 'Alicia' WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "UPDATE by id succeeds");
    if (r.out_buf) free(r.out_buf);

    /* 검증: SELECT로 확인 */
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "SELECT after UPDATE");
    ASSERT_TRUE(r.out_buf != NULL, "out_buf not NULL");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Alicia") != NULL,
                    "name updated to Alicia");
        ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL,
                    "old name Alice gone");
        free(r.out_buf);
    }

    /* 다른 행은 영향 없음 */
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 2");
    ASSERT_EQ_INT(r.status, 0, "SELECT id=2");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL,
                    "Bob unchanged");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "update_id");
}

/* ════════════════════════════════════════════════════════════ */
/*  2. UPDATE via table scan (WHERE name = 'Bob')              */
/* ════════════════════════════════════════════════════════════ */
static void test_update_by_scan(void) {
    printf(CLR_YELLOW "\n[test_update_by_scan]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "update_scan");
    create_and_populate(&pager, 3);

    exec_result_t r = db_execute(&pager,
        "UPDATE users SET age = 99 WHERE name = 'Bob'");
    ASSERT_EQ_INT(r.status, 0, "UPDATE by scan succeeds");
    if (r.out_buf) free(r.out_buf);

    /* 검증 */
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 2");
    ASSERT_EQ_INT(r.status, 0, "SELECT after scan UPDATE");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "99") != NULL,
                    "age updated to 99");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "update_scan");
}

/* ════════════════════════════════════════════════════════════ */
/*  3. 시스템 컬럼 id 수정 차단                                 */
/* ════════════════════════════════════════════════════════════ */
static void test_update_system_id_rejected(void) {
    printf(CLR_YELLOW "\n[test_update_system_id_rejected]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "update_id_reject");
    create_and_populate(&pager, 3);

    exec_result_t r = db_execute(&pager,
        "UPDATE users SET id = 99 WHERE id = 1");
    ASSERT_EQ_INT(r.status, -1, "UPDATE id is rejected");
    ASSERT_TRUE(strstr(r.message, "시스템 컬럼") != NULL,
                "error mentions system column");
    if (r.out_buf) free(r.out_buf);

    r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "original row still searchable by id=1");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Alice") != NULL,
                    "id update rejection keeps original row");
        ASSERT_TRUE(strstr(r.out_buf, "99") == NULL,
                    "id field was not rewritten");
        free(r.out_buf);
    }

    r = db_execute(&pager, "SELECT * FROM users WHERE id = 99");
    ASSERT_EQ_INT(r.status, 0, "SELECT id=99 returns normal empty result");
    ASSERT_TRUE(r.out_buf == NULL, "no new row for id=99");
    if (r.out_buf) free(r.out_buf);

    teardown_test_db(&pager, "update_id_reject");
}

/* ════════════════════════════════════════════════════════════ */
/*  4. 비교 연산자 (>, <, >=, <=, !=)                          */
/* ════════════════════════════════════════════════════════════ */
static void test_comparison_operators(void) {
    printf(CLR_YELLOW "\n[test_comparison_operators]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "cmp_ops");
    create_and_populate(&pager, 5);
    /* id=1 Alice 25, id=2 Bob 30, id=3 Charlie 20, id=4 Dave 35, id=5 Eve 28 */

    exec_result_t r;

    /* age > 28 => Bob(30), Dave(35) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age > 28");
    ASSERT_EQ_INT(r.status, 0, "SELECT age > 28");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL, "Bob in age>28");
        ASSERT_TRUE(strstr(r.out_buf, "Dave") != NULL, "Dave in age>28");
        ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL, "Alice not in age>28");
        ASSERT_TRUE(strstr(r.out_buf, "Charlie") == NULL, "Charlie not in age>28");
        free(r.out_buf);
    }

    /* age < 26 => Alice(25), Charlie(20) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age < 26");
    ASSERT_EQ_INT(r.status, 0, "SELECT age < 26");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Alice") != NULL, "Alice in age<26");
        ASSERT_TRUE(strstr(r.out_buf, "Charlie") != NULL, "Charlie in age<26");
        ASSERT_TRUE(strstr(r.out_buf, "Bob") == NULL, "Bob not in age<26");
        free(r.out_buf);
    }

    /* age >= 30 => Bob(30), Dave(35) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age >= 30");
    ASSERT_EQ_INT(r.status, 0, "SELECT age >= 30");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL, "Bob in age>=30");
        ASSERT_TRUE(strstr(r.out_buf, "Dave") != NULL, "Dave in age>=30");
        ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL, "Alice not in age>=30");
        free(r.out_buf);
    }

    /* age <= 25 => Alice(25), Charlie(20) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age <= 25");
    ASSERT_EQ_INT(r.status, 0, "SELECT age <= 25");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Alice") != NULL, "Alice in age<=25");
        ASSERT_TRUE(strstr(r.out_buf, "Charlie") != NULL, "Charlie in age<=25");
        ASSERT_TRUE(strstr(r.out_buf, "Bob") == NULL, "Bob not in age<=25");
        free(r.out_buf);
    }

    /* age != 25 => Bob(30), Charlie(20), Dave(35), Eve(28) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age != 25");
    ASSERT_EQ_INT(r.status, 0, "SELECT age != 25");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL, "Bob in age!=25");
        ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL, "Alice not in age!=25");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "cmp_ops");
}

/* ════════════════════════════════════════════════════════════ */
/*  5. COUNT(*)                                                */
/* ════════════════════════════════════════════════════════════ */
static void test_count(void) {
    printf(CLR_YELLOW "\n[test_count]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "count");
    create_and_populate(&pager, 5);

    exec_result_t r;

    /* COUNT(*) 전체 */
    r = db_execute(&pager, "SELECT COUNT(*) FROM users");
    ASSERT_EQ_INT(r.status, 0, "COUNT(*) succeeds");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "5") != NULL,
                    "COUNT(*) = 5");
        free(r.out_buf);
    }

    /* COUNT(*) with WHERE */
    r = db_execute(&pager, "SELECT COUNT(*) FROM users WHERE age > 25");
    ASSERT_EQ_INT(r.status, 0, "COUNT(*) WHERE succeeds");
    if (r.out_buf) {
        /* Bob(30), Dave(35), Eve(28) = 3 */
        ASSERT_TRUE(strstr(r.out_buf, "3") != NULL,
                    "COUNT(*) WHERE age>25 = 3");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "count");
}

/* ════════════════════════════════════════════════════════════ */
/*  6. ORDER BY                                                */
/* ════════════════════════════════════════════════════════════ */
static void test_order_by(void) {
    printf(CLR_YELLOW "\n[test_order_by]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "orderby");
    create_and_populate(&pager, 5);
    /* id=1 Alice 25, id=2 Bob 30, id=3 Charlie 20, id=4 Dave 35, id=5 Eve 28 */

    exec_result_t r;

    /* ORDER BY age ASC => Charlie(20), Alice(25), Eve(28), Bob(30), Dave(35) */
    r = db_execute(&pager, "SELECT * FROM users ORDER BY age ASC");
    ASSERT_EQ_INT(r.status, 0, "ORDER BY age ASC");
    if (r.out_buf) {
        char *pos_charlie = strstr(r.out_buf, "Charlie");
        char *pos_alice   = strstr(r.out_buf, "Alice");
        char *pos_dave    = strstr(r.out_buf, "Dave");
        ASSERT_TRUE(pos_charlie != NULL, "Charlie in result");
        ASSERT_TRUE(pos_alice != NULL, "Alice in result");
        ASSERT_TRUE(pos_dave != NULL, "Dave in result");
        if (pos_charlie && pos_alice && pos_dave) {
            ASSERT_TRUE(pos_charlie < pos_alice, "Charlie before Alice (ASC)");
            ASSERT_TRUE(pos_alice < pos_dave, "Alice before Dave (ASC)");
        }
        free(r.out_buf);
    }

    /* ORDER BY age DESC => Dave(35), Bob(30), Eve(28), Alice(25), Charlie(20) */
    r = db_execute(&pager, "SELECT * FROM users ORDER BY age DESC");
    ASSERT_EQ_INT(r.status, 0, "ORDER BY age DESC");
    if (r.out_buf) {
        char *pos_dave    = strstr(r.out_buf, "Dave");
        char *pos_charlie = strstr(r.out_buf, "Charlie");
        ASSERT_TRUE(pos_dave != NULL && pos_charlie != NULL, "both in result");
        if (pos_dave && pos_charlie) {
            ASSERT_TRUE(pos_dave < pos_charlie, "Dave before Charlie (DESC)");
        }
        free(r.out_buf);
    }

    teardown_test_db(&pager, "orderby");
}

/* ════════════════════════════════════════════════════════════ */
/*  7. LIMIT                                                   */
/* ════════════════════════════════════════════════════════════ */
static void test_limit(void) {
    printf(CLR_YELLOW "\n[test_limit]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "limit");
    create_and_populate(&pager, 5);

    exec_result_t r;

    /* LIMIT 2 */
    r = db_execute(&pager, "SELECT * FROM users LIMIT 2");
    ASSERT_EQ_INT(r.status, 0, "LIMIT 2 succeeds");
    if (r.out_buf) {
        /* 헤더행 1줄 + 구분선 1줄 + 데이터 2줄 => 총 4줄 */
        int lines = 0;
        for (char *c = r.out_buf; *c; c++)
            if (*c == '\n') lines++;
        ASSERT_EQ_INT(lines, 4, "LIMIT 2 => 4 lines (header+sep+2rows)");
        free(r.out_buf);
    }

    r = db_execute(&pager, "SELECT * FROM users LIMIT 0");
    ASSERT_EQ_INT(r.status, 0, "LIMIT 0 succeeds");
    ASSERT_TRUE(r.out_buf == NULL, "LIMIT 0 returns no rows");
    if (r.out_buf) free(r.out_buf);

    teardown_test_db(&pager, "limit");
}

/* ════════════════════════════════════════════════════════════ */
/*  8. ORDER BY + LIMIT 복합                                   */
/* ════════════════════════════════════════════════════════════ */
static void test_order_by_limit(void) {
    printf(CLR_YELLOW "\n[test_order_by_limit]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "ob_limit");
    create_and_populate(&pager, 5);

    /* ORDER BY age DESC LIMIT 2 => Dave(35), Bob(30) */
    exec_result_t r = db_execute(&pager,
        "SELECT * FROM users ORDER BY age DESC LIMIT 2");
    ASSERT_EQ_INT(r.status, 0, "ORDER BY + LIMIT succeeds");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Dave") != NULL, "Dave in top 2");
        ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL, "Bob in top 2");
        ASSERT_TRUE(strstr(r.out_buf, "Charlie") == NULL, "Charlie not in top 2");
        ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL, "Alice not in top 2");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "ob_limit");
}

/* ════════════════════════════════════════════════════════════ */
/*  9. DROP TABLE                                              */
/* ════════════════════════════════════════════════════════════ */
static void test_drop_table(void) {
    printf(CLR_YELLOW "\n[test_drop_table]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "drop");
    create_and_populate(&pager, 3);

    exec_result_t r = db_execute(&pager, "DROP TABLE users");
    ASSERT_EQ_INT(r.status, 0, "DROP TABLE succeeds");
    if (r.out_buf) free(r.out_buf);

    /* DROP 후 row_count = 0, schema col_count = 0 */
    ASSERT_EQ_INT((int)pager.header.row_count, 0, "row_count = 0 after DROP");
    ASSERT_EQ_INT((int)pager.header.column_count, 0, "column_count = 0 after DROP");

    /* DROP 후 다시 CREATE + INSERT 가능 */
    r = db_execute(&pager, "CREATE TABLE items (title VARCHAR(32), price INT)");
    ASSERT_EQ_INT(r.status, 0, "CREATE TABLE after DROP");
    if (r.out_buf) free(r.out_buf);

    r = db_execute(&pager, "INSERT INTO items VALUES ('Widget', 100)");
    ASSERT_EQ_INT(r.status, 0, "INSERT after DROP+CREATE");
    if (r.out_buf) free(r.out_buf);

    r = db_execute(&pager, "SELECT * FROM items WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "SELECT after DROP+CREATE");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "Widget") != NULL, "Widget found");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "drop");
}

/* ════════════════════════════════════════════════════════════ */
/* 10. EXPLAIN 확장                                            */
/* ════════════════════════════════════════════════════════════ */
static void test_explain_extended(void) {
    printf(CLR_YELLOW "\n[test_explain_extended]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "explain_ext");
    create_and_populate(&pager, 1);

    exec_result_t r;

    /* EXPLAIN UPDATE by id => INDEX_UPDATE */
    r = db_execute(&pager, "EXPLAIN UPDATE users SET name = 'X' WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "EXPLAIN UPDATE by id");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "INDEX_UPDATE") != NULL,
                    "INDEX_UPDATE path");
        free(r.out_buf);
    }

    /* EXPLAIN UPDATE by field => TABLE_SCAN */
    r = db_execute(&pager, "EXPLAIN UPDATE users SET age = 99 WHERE name = 'Alice'");
    ASSERT_EQ_INT(r.status, 0, "EXPLAIN UPDATE by field");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "TABLE_SCAN") != NULL,
                    "TABLE_SCAN path for field UPDATE");
        free(r.out_buf);
    }

    /* EXPLAIN DROP TABLE => DROP_TABLE */
    r = db_execute(&pager, "EXPLAIN DROP TABLE users");
    ASSERT_EQ_INT(r.status, 0, "EXPLAIN DROP TABLE");
    if (r.out_buf) {
        ASSERT_TRUE(strstr(r.out_buf, "DROP_TABLE") != NULL,
                    "DROP_TABLE path");
        free(r.out_buf);
    }

    teardown_test_db(&pager, "explain_ext");
}

/* ════════════════════════════════════════════════════════════ */
/*  메인                                                       */
/* ════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== Step 1: SQL Extension Test Suite ===\n");

    test_update_by_id();
    test_update_by_scan();
    test_update_system_id_rejected();
    test_comparison_operators();
    test_count();
    test_order_by();
    test_limit();
    test_order_by_limit();
    test_drop_table();
    test_explain_extended();

    printf("\n");
    printf("========================================\n");
    if (g_tests_failed == 0)
        printf(CLR_GREEN "ALL PASSED: %d/%d" CLR_RESET "\n",
               g_tests_passed, g_tests_run);
    else
        printf(CLR_RED "FAILED: %d/%d (fail=%d)" CLR_RESET "\n",
               g_tests_passed, g_tests_run, g_tests_failed);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
