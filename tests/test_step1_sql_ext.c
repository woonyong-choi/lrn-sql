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
 *  10. INDEX_RANGE (id 범위 조회 · 경계 · LIMIT · 삭제 후)
 *  11. EXPLAIN 과 실제 실행 경로 일치 (회귀)
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
/*                                                             */
/*  다섯 연산자가 전부 같은 코드 경로(비교 필터 + TABLE_SCAN)를 */
/*  타고 입력만 다르므로, 블록을 반복하지 않고 표로 돌린다.     */
/* ════════════════════════════════════════════════════════════ */
static void test_comparison_operators(void) {
    printf(CLR_YELLOW "\n[test_comparison_operators]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "cmp_ops");
    create_and_populate(&pager, 5);
    /* id=1 Alice 25, id=2 Bob 30, id=3 Charlie 20, id=4 Dave 35, id=5 Eve 28 */

    static const struct {
        const char *sql;
        const char *present[3];   /* NULL 로 끝나는 목록 */
        const char *absent[3];
    } cases[] = {
        { "SELECT * FROM users WHERE age > 28",
          { "Bob", "Dave", NULL }, { "Alice", "Charlie", NULL } },
        { "SELECT * FROM users WHERE age < 26",
          { "Alice", "Charlie", NULL }, { "Bob", "Dave", NULL } },
        { "SELECT * FROM users WHERE age >= 30",
          { "Bob", "Dave", NULL }, { "Alice", "Eve", NULL } },
        { "SELECT * FROM users WHERE age <= 25",
          { "Alice", "Charlie", NULL }, { "Bob", "Eve", NULL } },
        { "SELECT * FROM users WHERE age != 25",
          { "Bob", "Charlie", NULL }, { "Alice", NULL, NULL } },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        exec_result_t r = db_execute(&pager, cases[i].sql);
        ASSERT_EQ_INT(r.status, 0, cases[i].sql);
        if (r.out_buf) {
            for (int j = 0; j < 3 && cases[i].present[j]; j++) {
                ASSERT_TRUE(strstr(r.out_buf, cases[i].present[j]) != NULL,
                            cases[i].sql);
            }
            for (int j = 0; j < 3 && cases[i].absent[j]; j++) {
                ASSERT_TRUE(strstr(r.out_buf, cases[i].absent[j]) == NULL,
                            cases[i].sql);
            }
            free(r.out_buf);
        }
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
/* ════════════════════════════════════════════════════════════ */
/*  11. INDEX_RANGE — id 범위 조회 (B+tree 리프 순회)          */
/*                                                             */
/*  이 저장소가 "범위 질의를 힙 스캔에서 인덱스 스캔으로 바꿨다"*/
/*  고 주장하는 경로다. 계획이 실제로 INDEX_RANGE 인지, 그리고  */
/*  그 경로가 내놓는 행이 힙 스캔과 같은지를 함께 본다.         */
/* ════════════════════════════════════════════════════════════ */

/* 출력 버퍼에서 데이터 행 수를 센다 (헤더 2줄 제외) */
static int count_rows(const exec_result_t *r) {
    if (r->out_buf == NULL) return 0;
    int lines = 0;
    for (size_t i = 0; i < r->out_len; i++) if (r->out_buf[i] == '\n') lines++;
    return lines > 2 ? lines - 2 : 0;
}

static void populate_ids(pager_t *pager, int n) {
    exec_result_t r = db_execute(pager, "CREATE TABLE t (name VARCHAR(16), v INT)");
    assert(r.status == 0);
    if (r.out_buf) free(r.out_buf);
    for (int i = 1; i <= n; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('r%d', %d)", i, i);
        r = db_execute(pager, sql);
        assert(r.status == 0);
        if (r.out_buf) free(r.out_buf);
    }
}

static void test_index_range(void) {
    printf(CLR_YELLOW "\n[test_index_range]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "index_range");
    populate_ids(&pager, 200);   /* id = 1..200 */

    exec_result_t r;

    /* 계획이 정말 INDEX_RANGE 로 잡히는가 */
    r = db_execute(&pager, "EXPLAIN SELECT * FROM t WHERE id BETWEEN 10 AND 20");
    ASSERT_TRUE(r.out_buf && strstr(r.out_buf, "INDEX_RANGE") != NULL,
                "BETWEEN -> INDEX_RANGE 계획");
    if (r.out_buf) free(r.out_buf);

    r = db_execute(&pager, "EXPLAIN SELECT * FROM t WHERE id >= 10");
    ASSERT_TRUE(r.out_buf && strstr(r.out_buf, "INDEX_RANGE") != NULL,
                "id >= N -> INDEX_RANGE 계획");
    if (r.out_buf) free(r.out_buf);

    /* 경계 포함/제외가 행 수에 그대로 나타나는가 */
    static const struct { const char *sql; int expect; } cases[] = {
        { "SELECT * FROM t WHERE id BETWEEN 10 AND 20",        11 },
        { "SELECT * FROM t WHERE id >= 191",                   10 },
        { "SELECT * FROM t WHERE id > 191",                     9 },
        { "SELECT * FROM t WHERE id <= 5",                      5 },
        { "SELECT * FROM t WHERE id < 5",                       4 },
        { "SELECT * FROM t WHERE id BETWEEN 100 AND 100",       1 },
        { "SELECT * FROM t WHERE id BETWEEN 201 AND 300",       0 },
        { "SELECT * FROM t WHERE id BETWEEN 150 AND 120",       0 },
        { "SELECT * FROM t WHERE id >= 1 LIMIT 7",              7 },
        { "SELECT * FROM t WHERE id >= 1 LIMIT 0",              0 },
        { "SELECT * FROM t WHERE id >= 195 LIMIT 100",          6 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        r = db_execute(&pager, cases[i].sql);
        ASSERT_EQ_INT(r.status, 0, cases[i].sql);
        ASSERT_EQ_INT(count_rows(&r), cases[i].expect, cases[i].sql);
        if (r.out_buf) free(r.out_buf);
    }

    /* 인덱스 경로와 힙 스캔 경로가 같은 행을 같은 순서로 내는가.
     * WHERE 없는 SELECT 는 힙 순서(=id 순서)로 전부 내므로, 범위 조회
     * 결과가 그 앞부분과 글자 단위로 같아야 한다. */
    r = db_execute(&pager, "SELECT * FROM t WHERE id BETWEEN 1 AND 30");
    exec_result_t full = db_execute(&pager, "SELECT * FROM t LIMIT 30");
    ASSERT_TRUE(r.out_buf && full.out_buf
                && strcmp(r.out_buf, full.out_buf) == 0,
                "INDEX_RANGE 결과가 힙 스캔(LIMIT 30) 결과와 동일");
    if (r.out_buf) free(r.out_buf);
    if (full.out_buf) free(full.out_buf);

    /* 삭제된 행은 톰스톤이라 인덱스에서 빠지고 범위 결과에서도 빠져야 한다 */
    r = db_execute(&pager, "DELETE FROM t WHERE id = 15");
    ASSERT_EQ_INT(r.status, 0, "DELETE id=15");
    if (r.out_buf) free(r.out_buf);
    r = db_execute(&pager, "SELECT * FROM t WHERE id BETWEEN 10 AND 20");
    ASSERT_EQ_INT(count_rows(&r), 10, "삭제 후 범위 결과 10행");
    ASSERT_TRUE(r.out_buf && strstr(r.out_buf, "r15") == NULL,
                "삭제된 행이 범위 결과에 없다");
    if (r.out_buf) free(r.out_buf);

    teardown_test_db(&pager, "index_range");
}

/* ════════════════════════════════════════════════════════════ */
/*  12. EXPLAIN 이 실행기와 같은 계획을 보고하는가 (회귀)       */
/*                                                             */
/*  회귀 대상: EXPLAIN 이 안쪽 문장의 일부 필드만 복사하던 시절,*/
/*  `EXPLAIN SELECT * FROM t WHERE id = 1 LIMIT 3` 은          */
/*  INDEX_LOOKUP 을 찍었지만 같은 SELECT 는 TABLE_SCAN 으로     */
/*  돌았다. 계획을 알려주는 것이 유일한 임무인 명령이 실행기와  */
/*  갈라지면 EXPLAIN 으로 잰 모든 판단이 무효가 된다.           */
/* ════════════════════════════════════════════════════════════ */
static void test_explain_matches_execution(void) {
    printf(CLR_YELLOW "\n[test_explain_matches_execution]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "explain_exec");
    populate_ids(&pager, 50);

    /* 실행 결과 메시지에 실제 접근 경로가 찍히므로 그것을 정답으로 삼는다 */
    static const char *queries[] = {
        "SELECT * FROM t WHERE id = 1",
        "SELECT * FROM t WHERE id = 1 LIMIT 3",
        "SELECT * FROM t WHERE id >= 10",
        "SELECT * FROM t WHERE id >= 10 LIMIT 5",
        "SELECT * FROM t WHERE id >= 10 ORDER BY v",
        "SELECT * FROM t WHERE id BETWEEN 5 AND 9 ORDER BY v DESC",
        "SELECT COUNT(*) FROM t WHERE id = 1",
        "SELECT COUNT(*) FROM t WHERE id >= 10",
        "SELECT * FROM t WHERE v = 3",
        "SELECT * FROM t",
    };
    static const char *paths[] = {
        "INDEX_LOOKUP", "INDEX_RANGE", "TABLE_SCAN"
    };

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "EXPLAIN %s", queries[i]);
        exec_result_t e = db_execute(&pager, sql);
        exec_result_t x = db_execute(&pager, queries[i]);

        ASSERT_TRUE(e.out_buf != NULL, queries[i]);
        ASSERT_EQ_INT(x.status, 0, queries[i]);

        if (e.out_buf) {
            /* EXPLAIN 이 말한 경로와 실행기가 쓴 경로가 같아야 한다.
             * 실행 쪽 경로는 결과 메시지("N행 조회 (INDEX_RANGE)")에 있고,
             * COUNT(*) 처럼 메시지에 경로가 없으면 그 항목은 건너뛴다. */
            const char *said = NULL, *did = NULL;
            for (size_t k = 0; k < sizeof(paths) / sizeof(paths[0]); k++) {
                if (strstr(e.out_buf, paths[k])) said = paths[k];
                if (strstr(x.message, paths[k])) did = paths[k];
            }
            ASSERT_TRUE(said != NULL, queries[i]);
            if (said && did) {
                ASSERT_TRUE(strcmp(said, did) == 0, queries[i]);
                if (strcmp(said, did) != 0) {
                    printf("        EXPLAIN=%s / 실행=%s  <- %s\n",
                           said, did, queries[i]);
                }
            }
            free(e.out_buf);
        }
        if (x.out_buf) free(x.out_buf);
    }

    /* 중첩 EXPLAIN 은 계획 수립이 자기 자신을 부르게 되므로 거부한다 */
    exec_result_t r = db_execute(&pager, "EXPLAIN EXPLAIN SELECT * FROM t");
    ASSERT_EQ_INT(r.status, -1, "중첩 EXPLAIN 은 구문 오류");
    if (r.out_buf) free(r.out_buf);

    teardown_test_db(&pager, "explain_exec");
}

/* #4: 잘못된 INSERT 값은 행과 다음 id를 바꾸지 않는다. */
static void test_insert_numeric_values_are_validated(void) {
    pager_t pager;
    setup_test_db(&pager, "insert_numeric");
    exec_result_t r = db_execute(&pager,
        "CREATE TABLE users (name VARCHAR(32), age INT)");
    ASSERT_EQ_INT(r.status, 0, "create numeric test table");
    free(r.out_buf);

    const char *invalid[] = {
        "INSERT INTO users VALUES ('Bob', abc)",
        "INSERT INTO users VALUES ('Bob', 2147483648)",
        "INSERT INTO users VALUES ('Bob', -2147483649)",
        "INSERT INTO users VALUES ('Bob')",
        "INSERT INTO users VALUES ('Bob', 30, 'extra')"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        r = db_execute(&pager, invalid[i]);
        ASSERT_EQ_INT(r.status, -1, invalid[i]);
        free(r.out_buf);
    }

    r = db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    ASSERT_EQ_INT(r.status, 0, "valid INSERT succeeds after rejection");
    ASSERT_TRUE(strstr(r.message, "id=1") != NULL,
                "rejected INSERT does not consume an id");
    free(r.out_buf);
    r = db_execute(&pager, "SELECT * FROM users");
    ASSERT_TRUE(r.out_buf && strstr(r.out_buf, "Alice | 25") != NULL,
                "valid row keeps its value");
    ASSERT_TRUE(r.out_buf && strstr(r.out_buf, "Bob") == NULL,
                "invalid rows were not stored");
    free(r.out_buf);

    r = db_execute(&pager, "DROP TABLE users");
    ASSERT_EQ_INT(r.status, 0, "drop numeric test table");
    free(r.out_buf);
    r = db_execute(&pager, "CREATE TABLE metrics (value BIGINT)");
    ASSERT_EQ_INT(r.status, 0, "create BIGINT test table");
    free(r.out_buf);
    r = db_execute(&pager, "INSERT INTO metrics VALUES (9223372036854775808)");
    ASSERT_EQ_INT(r.status, -1, "BIGINT overflow is rejected");
    free(r.out_buf);
    r = db_execute(&pager, "INSERT INTO metrics VALUES (9223372036854775807)");
    ASSERT_EQ_INT(r.status, 0, "BIGINT maximum is accepted");
    free(r.out_buf);

    teardown_test_db(&pager, "insert_numeric");
}

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
    test_index_range();
    test_explain_matches_execution();
    test_insert_numeric_values_are_validated();

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
