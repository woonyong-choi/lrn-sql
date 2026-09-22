/*
 * test_step3_regression.c -- 회귀 테스트: Strict 2PL 잠금 해제 + 힙 체인 재탐색
 *
 * 포트폴리오 "범위와 한계"에서 증빙이 비어 있던 두 항목을 고정한다.
 *
 *   1. test_repl_insert_releases_locks
 *      REPL 경로(db_execute() 직접 호출)로 INSERT를 실행한 뒤,
 *      잠금 테이블에 보유 중인 잠금이 0건인지 검사한다.
 *      INSERT는 executor 내부(src/sql/executor.c: gap check)에서 새 id에
 *      X lock을 걸고 스스로 풀지 않으므로, db_execute() 끝의
 *      lock_release_all()이 빠지면 이 테스트가 실패한다.
 *      재현: src/db.c의 lock_release_all() 호출을 주석 처리.
 *
 *   2. test_sequential_insert_no_heap_rescan
 *      DELETE 없는 순차 INSERT를 2만 건 수행하고,
 *      find_heap_page()의 힙 체인 전체 재탐색이 단 한 번도 일어나지
 *      않았음을 pager->heap_chain_rescan_count로 검증한다.
 *      재현: -DMINIDB_DISABLE_FREE_HINT로 빌드하면 힌트 게이트가 사라져
 *      재탐색 횟수가 0을 넘는다 (docs/benchmark-postgres.md 결함 1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>

#include "db.h"
#include "storage/pager.h"

#define TEST_DB_PREFIX "__test_step3_"

/*
 * 순차 INSERT 건수 (DELETE 없음).
 *
 * 검출력은 행 수가 아니라 heap_chain_rescan_count가 0인지에서 나온다.
 * 이 카운터는 꼬리 페이지가 가득 찰 때마다 증가하므로, 힙 페이지가 여러
 * 장 만들어질 만큼만 삽입하면 결함을 잡는다. 실제로 같은 2만 건을
 * -DMINIDB_DISABLE_FREE_HINT로 빌드하면 카운터가 수천 회로 튄다
 * (docs/benchmark-postgres.md 결함 1). 20만 건은 ASAN·UBSAN 빌드에서
 * 한 시간을 넘겨 CI에 넣을 수 없었고, 검출력은 2만 건과 같다.
 *
 * 대조·확장 실험용으로 -DSEQ_INSERT_ROWS=N으로 덮어쓸 수 있다.
 */
#ifndef SEQ_INSERT_ROWS
#define SEQ_INSERT_ROWS 20000
#endif

/*
 * 재탐색이 없더라도 넘으면 안 되는 벽시계 상한 (초).
 * ASAN·UBSAN 빌드와 느린 CI 러너를 모두 덮도록 여유 있게 잡는다.
 * 이 단언은 보조일 뿐이고, 본 검사는 위의 카운터다.
 */
#define SEQ_INSERT_TIME_LIMIT_SEC 600.0

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

static int run_sql(pager_t *pager, const char *sql) {
    exec_result_t r = db_execute(pager, sql);
    if (r.out_buf) free(r.out_buf);
    return r.status;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ════════════════════════════════════════════════════════════ */
/*  1. REPL 경로 INSERT 후 잠금 잔여 0건                        */
/* ════════════════════════════════════════════════════════════ */
static void test_repl_insert_releases_locks(void) {
    printf(CLR_YELLOW "\n[test_repl_insert_releases_locks]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "locks");

    ASSERT_EQ_INT(run_sql(&pager,
        "CREATE TABLE users (name VARCHAR(32), age INT)"), 0,
        "CREATE TABLE succeeds");

    lock_stats_t st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "잠금 0건 (INSERT 이전 기준선)");

    /* 단건 INSERT — executor gap check가 새 id에 X lock을 건다 */
    ASSERT_EQ_INT(run_sql(&pager,
        "INSERT INTO users VALUES ('Alice', 25)"), 0, "INSERT 1건 성공");

    st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "INSERT 1건 후 보유 잠금 총 0건");
    ASSERT_EQ_INT(st.exclusive, 0, "INSERT 1건 후 X 잠금 0건");
    ASSERT_EQ_INT(st.shared, 0, "INSERT 1건 후 S 잠금 0건");

    /* 연속 INSERT — 매 문장마다 누적 없이 0으로 돌아와야 한다 */
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('u%d', %d)", i, 20 + i);
        if (run_sql(&pager, sql) != 0) {
            ASSERT_TRUE(0, "연속 INSERT 성공");
            break;
        }
    }
    st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "INSERT 50건 연속 실행 후 보유 잠금 0건");
    ASSERT_EQ_INT((int)pager.header.row_count, 51, "row_count == 51");

    /* 다른 문장 종류도 같은 경로로 잠금을 반납하는지 확인 */
    ASSERT_EQ_INT(run_sql(&pager, "SELECT * FROM users WHERE id = 1"), 0,
                  "SELECT WHERE id=1 성공");
    st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "SELECT(point S lock) 후 보유 잠금 0건");

    ASSERT_EQ_INT(run_sql(&pager, "UPDATE users SET age = 30 WHERE id = 1"), 0,
                  "UPDATE WHERE id=1 성공");
    st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "UPDATE(point X lock) 후 보유 잠금 0건");

    ASSERT_EQ_INT(run_sql(&pager, "SELECT * FROM users WHERE id > 10"), 0,
                  "SELECT WHERE id>10 성공");
    st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "SELECT(range S lock) 후 보유 잠금 0건");

    ASSERT_EQ_INT(run_sql(&pager, "DELETE FROM users WHERE id = 2"), 0,
                  "DELETE WHERE id=2 성공");
    st = db_lock_stats();
    ASSERT_EQ_INT(st.total, 0, "DELETE(point X lock) 후 보유 잠금 0건");

    teardown_test_db(&pager, "locks");
}

/* ════════════════════════════════════════════════════════════ */
/*  2. 순차 INSERT 2만 건 — 힙 체인 재탐색 0회                   */
/* ════════════════════════════════════════════════════════════ */
static void test_sequential_insert_no_heap_rescan(void) {
    printf(CLR_YELLOW "\n[test_sequential_insert_no_heap_rescan]" CLR_RESET "\n");
    pager_t pager;
    setup_test_db(&pager, "rescan");

    ASSERT_EQ_INT(run_sql(&pager,
        "CREATE TABLE users (name VARCHAR(32), age INT)"), 0,
        "CREATE TABLE succeeds");

    ASSERT_EQ_INT((int)pager.heap_chain_rescan_count, 0,
                  "CREATE TABLE 직후 재탐색 0회");

    double t0 = now_sec();
    int insert_failed = 0;
    for (int i = 0; i < SEQ_INSERT_ROWS; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('u%d', %d)", i, i % 100);
        if (run_sql(&pager, sql) != 0) {
            insert_failed = i + 1;
            break;
        }
    }
    double elapsed = now_sec() - t0;

    ASSERT_EQ_INT(insert_failed, 0, "순차 INSERT SEQ_INSERT_ROWS건 전부 성공");
    ASSERT_EQ_INT((int)pager.header.row_count, SEQ_INSERT_ROWS,
                  "row_count == SEQ_INSERT_ROWS");

    printf("  INSERT %d건: %.3f초, 힙 체인 재탐색 %" PRIu64 "회\n",
           SEQ_INSERT_ROWS, elapsed, pager.heap_chain_rescan_count);

    /* 핵심 단언: DELETE가 한 번도 없었으므로 재탐색은 0회여야 한다 */
    ASSERT_TRUE(pager.heap_chain_rescan_count == 0,
                "DELETE 없는 순차 INSERT에서 힙 체인 재탐색 0회");

    /* 보조 단언: 재탐색이 없으면 벽시계도 상한 안에 들어온다 */
    ASSERT_TRUE(elapsed < SEQ_INSERT_TIME_LIMIT_SEC,
                "순차 INSERT가 시간 상한 내 완료");

    /* 대조군: DELETE 1건이 힌트를 되살려 재탐색을 다시 허용하는지 확인.
     * (힌트가 항상 false로 굳어 슬롯 재활용이 깨지는 반대 방향 회귀 방지) */
    ASSERT_EQ_INT(run_sql(&pager, "DELETE FROM users WHERE id = 1"), 0,
                  "DELETE 1건 성공");
    ASSERT_TRUE(pager.heap_may_have_free_slots,
                "DELETE 후 free slot 힌트가 다시 켜진다");

    teardown_test_db(&pager, "rescan");
}

/* ════════════════════════════════════════════════════════════ */
/*  main                                                       */
/* ════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== Step 3: Regression Test Suite ===\n");

    db_init();

    test_repl_insert_releases_locks();
    test_sequential_insert_no_heap_rescan();

    db_destroy();

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
