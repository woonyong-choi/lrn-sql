/*
 * test_value_legacy_varchar.c — 레거시 저장 경로의 VARCHAR(n) 경계 (#38)
 *
 * 실제 db_execute·pager로 CREATE·INSERT·UPDATE를 실행하고 재시작(pager 닫고 다시 열기) 뒤 값을 읽는다.
 * 이 경로의 컬럼은 n바이트 고정 폭 슬롯이다. 따라서 n은 바이트 수 상한이고(문자 수 상한은 가변 길이 저장이
 * 필요하다), 슬롯에 들어가지 않는 값은 절단하지 않고 거절한다. n-1·n바이트는 그대로 저장되고 n+1바이트는
 * 오류이며, 거절은 행·기존 값을 바꾸지 않는다. 잘리는 다바이트 문자(깨진 UTF-8)와 비정상 폭(0, 255 초과,
 * 숫자가 아닌 폭)은 오류이며 프로세스가 죽지 않는다.
 *
 * 빌드: gcc -Wall -Wextra -Werror -g -std=gnu11 -Iinclude -pthread tests/test_value_legacy_varchar.c \
 *         src/storage/pager.c src/storage/schema.c src/storage/table.c src/storage/bptree.c src/sql/parser.c \
 *         src/sql/planner.c src/sql/executor.c src/server/lock_table.c src/server/http.c src/server/server.c \
 *         src/db.c -o build/test_value_legacy_varchar
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "sql/executor.h"
#include "storage/pager.h"

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

static const char *DB_PATH = "build/test_value_legacy_varchar.db";

static void open_db(pager_t *pager, bool create)
{
    if (create) unlink(DB_PATH);
    CHECK(pager_open(pager, DB_PATH, create) == 0);
}

/* SQL을 실행하고 상태를 돌려준다. out이 NULL이 아니면 SELECT 출력(해제 책임은 호출자)을 넘긴다. */
static int run(pager_t *pager, const char *sql, char **out)
{
    exec_result_t r = db_execute(pager, sql);
    if (out != NULL) {
        *out = r.out_buf;
    } else if (r.out_buf != NULL) {
        free(r.out_buf);
    }
    return r.status;
}

/* SELECT 결과에 "| <값>\n" 형태의 행이 있는지 */
static int select_has(pager_t *pager, const char *table, const char *value)
{
    char sql[128];
    char needle[600];
    char *out = NULL;
    snprintf(sql, sizeof(sql), "SELECT * FROM %s", table);
    snprintf(needle, sizeof(needle), "| %s\n", value);
    if (run(pager, sql, &out) != 0 || out == NULL) {
        free(out);
        return 0;
    }
    int found = strstr(out, needle) != NULL;
    free(out);
    return found;
}

static void insert_ok(pager_t *pager, const char *value)
{
    char sql[700];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('%s')", value);
    CHECK(run(pager, sql, NULL) == 0);
}

static void insert_rejected(pager_t *pager, const char *value)
{
    char sql[700];
    uint64_t rows = pager->header.row_count;
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('%s')", value);
    CHECK(run(pager, sql, NULL) == -1);
    CHECK(pager->header.row_count == rows);   /* 거절은 행을 만들지 않는다 */
}

/* n-1, n, n+1바이트 ASCII와 다바이트 문자 경계. 재시작 뒤에도 값이 그대로다. */
static void test_insert_boundary_and_restart(void)
{
    pager_t pager;
    open_db(&pager, true);
    CHECK(run(&pager, "CREATE TABLE t (name VARCHAR(8))", NULL) == 0);
    insert_ok(&pager, "abcdefg");                  /* n-1 */
    insert_ok(&pager, "abcdefgh");                 /* n: 절단 없이 저장 */
    insert_rejected(&pager, "abcdefghi");          /* n+1: 거절 */
    insert_ok(&pager, "가나");                      /* 6바이트 */
    insert_ok(&pager, "가나ab");                    /* 정확히 8바이트 */
    insert_rejected(&pager, "가나다");              /* 9바이트: 문자 중간을 자르지 않고 거절 */
    insert_rejected(&pager, "😀😀😀");               /* 12바이트 */
    insert_ok(&pager, "😀😀");                      /* 정확히 8바이트 */
    CHECK(pager.header.row_count == 5);
    CHECK(select_has(&pager, "t", "abcdefg") && select_has(&pager, "t", "abcdefgh"));
    CHECK(select_has(&pager, "t", "가나") && select_has(&pager, "t", "가나ab") && select_has(&pager, "t", "😀😀"));
    pager_close(&pager);

    open_db(&pager, false);   /* 재시작 */
    CHECK(pager.header.row_count == 5);
    CHECK(select_has(&pager, "t", "abcdefg") && select_has(&pager, "t", "abcdefgh"));
    CHECK(select_has(&pager, "t", "가나") && select_has(&pager, "t", "가나ab") && select_has(&pager, "t", "😀😀"));
    CHECK(!select_has(&pager, "t", "abcdefghi"));
    pager_close(&pager);
    unlink(DB_PATH);
}

/* UPDATE도 같은 폭 규칙이다. 거절하면 기존 값이 그대로다. 인덱스 경로와 전체 스캔 경로를 모두 본다. */
static void test_update_boundary(void)
{
    pager_t pager;
    open_db(&pager, true);
    CHECK(run(&pager, "CREATE TABLE t (name VARCHAR(8))", NULL) == 0);
    insert_ok(&pager, "keep");
    insert_ok(&pager, "same");

    CHECK(run(&pager, "UPDATE t SET name = 'abcdefgh' WHERE id = 1", NULL) == 0);   /* n */
    CHECK(select_has(&pager, "t", "abcdefgh"));
    CHECK(run(&pager, "UPDATE t SET name = 'abcdefghi' WHERE id = 1", NULL) == -1);  /* n+1 */
    CHECK(select_has(&pager, "t", "abcdefgh") && !select_has(&pager, "t", "abcdefghi"));
    CHECK(run(&pager, "UPDATE t SET name = '가나다' WHERE id = 1", NULL) == -1);
    CHECK(select_has(&pager, "t", "abcdefgh"));
    CHECK(run(&pager, "UPDATE t SET name = 'abcdefghi' WHERE name = 'same'", NULL) == -1);   /* 스캔 경로 */
    CHECK(select_has(&pager, "t", "same"));
    CHECK(run(&pager, "UPDATE t SET name = '가나ab' WHERE name = 'same'", NULL) == 0);
    pager_close(&pager);

    open_db(&pager, false);
    CHECK(select_has(&pager, "t", "abcdefgh") && select_has(&pager, "t", "가나ab") && !select_has(&pager, "t", "same"));
    pager_close(&pager);
    unlink(DB_PATH);
}

/* 비정상 폭은 오류이고 테이블을 만들지 않는다. 이어서 정상 CREATE가 된다. */
static void test_invalid_widths(void)
{
    static const char *const BAD[] = {"0", "256", "300", "65535", "65536", "70000", "abc", "-1"};
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        pager_t pager;
        char sql[96];
        open_db(&pager, true);
        snprintf(sql, sizeof(sql), "CREATE TABLE t (name VARCHAR(%s))", BAD[i]);
        if (run(&pager, sql, NULL) != -1 || pager.header.column_count != 0) {
            fprintf(stderr, "bad width not rejected: VARCHAR(%s)\n", BAD[i]);
            failures++;
        }
        CHECK(run(&pager, "CREATE TABLE t (name VARCHAR(1))", NULL) == 0);   /* 거절 뒤에도 정상 */
        pager_close(&pager);
        unlink(DB_PATH);
    }
}

/* 폭의 양 끝: 1바이트와 255바이트, 두 VARCHAR 컬럼이 서로의 값을 건드리지 않는다. */
static void test_width_extremes_and_neighbors(void)
{
    pager_t pager;
    char wide[300];
    char sql[700];
    open_db(&pager, true);
    CHECK(run(&pager, "CREATE TABLE t (a VARCHAR(1), b VARCHAR(255), c VARCHAR(3))", NULL) == 0);
    memset(wide, 'x', 255);
    wide[255] = '\0';
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('a', '%s', 'zzz')", wide);
    CHECK(run(&pager, sql, NULL) == 0);                                  /* 1, 255, 3바이트 */
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('ab', '%s', 'zzz')", wide);
    CHECK(run(&pager, sql, NULL) == -1);                                 /* a만 n+1 */
    wide[255] = 'x';
    wide[256] = '\0';
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('a', '%s', 'zzz')", wide);
    CHECK(run(&pager, sql, NULL) == -1);                                 /* b만 n+1(256) */
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('a', 'x', 'zzzz')");
    CHECK(run(&pager, sql, NULL) == -1);                                 /* c만 n+1 */
    CHECK(pager.header.row_count == 1);
    pager_close(&pager);

    open_db(&pager, false);
    {
        char *out = NULL;
        CHECK(run(&pager, "SELECT * FROM t", &out) == 0 && out != NULL);
        wide[255] = '\0';
        char expect[400];
        snprintf(expect, sizeof(expect), "| a | %s | zzz\n", wide);
        CHECK(out != NULL && strstr(out, expect) != NULL);
        free(out);
    }
    pager_close(&pager);
    unlink(DB_PATH);
}

int main(void)
{
    test_insert_boundary_and_restart();
    test_update_boundary();
    test_invalid_widths();
    test_width_extremes_and_neighbors();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("legacy varchar: n-1/n bytes stored exactly, n+1 and cut multibyte rejected, bad widths rejected, restart exact");
    return 0;
}
