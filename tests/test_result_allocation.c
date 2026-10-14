/*
 * test_result_allocation.c -- 출력 버퍼 할당 실패의 SQL 오류 전달과 소유권 검사 (#128)
 *
 * MINIDB_TEST_HOOKS로 컴파일한 executor.c에서만 동작한다. executor가 SELECT/EXPLAIN 결과를 쌓는
 * 출력 버퍼의 malloc·realloc을 n번째 호출에서 실패시켜 아래를 확인한다.
 *   1. 실패는 status -1 과 오류 메시지로 전달되고 out_buf는 NULL, out_len은 0이다
 *      (일부만 쌓인 결과를 성공으로 돌려주지 않는다).
 *   2. 실패 직후 같은 문장을 다시 실행하면 주입 없는 결과와 한 글자도 다르지 않다 (재시도).
 *   3. 성공이든 실패든 출력 버퍼는 한 번만 해제되거나 호출자에게 한 번만 넘어간다
 *      (executor 안에 남은 버퍼 0, 넘긴 버퍼는 성공 때만 1 증가, 호출자가 한 번 free).
 *   4. 실패 뒤 pin·latch·row lock이 남지 않는다.
 *   5. 출력이 필요 없는 문장(빈 결과)은 첫 malloc이 실패해도 성공이다.
 * 첫 realloc이 실패한 뒤에도 문장이 다음 행을 계속 append 하려 하므로 ASan·UBSan 아래에서
 * 범위 밖 쓰기가 없는지도 함께 본다. 데이터는 모두 합성 데이터다.
 *
 * 빌드: 다른 test_* 와 같은 방식으로 tests/test_result_allocation.c 와 $(OBJS) 를 함께 링크하되
 * 모든 소스를 -DMINIDB_TEST_HOOKS 로 컴파일한다.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "storage/pager.h"

#define DB_PATH "__test__result_allocation.db"
#define ROWS 300
#define ERR_MSG "결과 출력 버퍼 할당에 실패했습니다"

static int g_run = 0;
static int g_fail = 0;

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

typedef struct {
    const char *name;
    const char *sql;
    bool needs_output;   /* 결과 행이 있어 출력 버퍼가 필요한 문장 */
} query_t;

static const query_t QUERIES[] = {
    {"INDEX_LOOKUP", "SELECT * FROM t WHERE id = 5", true},
    {"INDEX_RANGE", "SELECT * FROM t WHERE id BETWEEN 1 AND 300", true},
    {"TABLE_SCAN", "SELECT * FROM t WHERE age >= 0", true},
    {"TABLE_SCAN_ORDER", "SELECT * FROM t ORDER BY age DESC", true},
    {"COUNT", "SELECT COUNT(*) FROM t", true},
    {"EXPLAIN", "EXPLAIN SELECT * FROM t WHERE id BETWEEN 1 AND 300", true},
    {"INDEX_RANGE_EMPTY", "SELECT * FROM t WHERE id BETWEEN 1000 AND 2000", false},
    {"TABLE_SCAN_EMPTY", "SELECT * FROM t WHERE age >= 100000", false},
};
#define QUERY_COUNT ((int)(sizeof(QUERIES) / sizeof(QUERIES[0])))

typedef struct {
    int status;
    char message[512];
    char *out;           /* 호출자 소유 복사본 (NULL 가능) */
    size_t out_len;
    long mallocs;
    long reallocs;
} outcome_t;

static void stats(long *m, long *r, long *live, long *xfer)
{
    executor_test_out_stats(m, r, live, xfer);
}

/* 한 번 실행하고 호출자 쪽 해제를 정확히 한 번 수행한다. 소유권 계약을 여기서 검사한다. */
static outcome_t run(pager_t *p, const char *sql, long malloc_at, long realloc_at, const char *label)
{
    outcome_t o;
    memset(&o, 0, sizeof(o));

    long m0, r0, live0, x0;
    executor_test_fail_out_alloc(malloc_at, realloc_at);
    stats(&m0, &r0, &live0, &x0);
    long xfer_before = x0;

    exec_result_t res = db_execute(p, sql);

    long live, xfer;
    stats(&o.mallocs, &o.reallocs, &live, &xfer);
    o.status = res.status;
    snprintf(o.message, sizeof(o.message), "%s", res.message);
    o.out_len = res.out_len;

    CHECK(live == 0, "%s: 실행 뒤 executor 안에 남은 출력 버퍼 0 (live=%ld)", label, live);
    if (res.out_buf != NULL) {
        CHECK(xfer == xfer_before + 1, "%s: 성공 때 소유권은 정확히 한 번 넘어간다 (%ld -> %ld)",
              label, xfer_before, xfer);
        CHECK(strlen(res.out_buf) == res.out_len, "%s: out_len이 실제 길이와 같다", label);
        o.out = strdup(res.out_buf);
        free(res.out_buf);   /* 호출자가 정확히 한 번 해제 */
    } else {
        CHECK(xfer == xfer_before, "%s: 넘긴 버퍼가 없으면 소유권 이전도 없다", label);
        CHECK(res.out_len == 0, "%s: out_buf가 없으면 out_len은 0", label);
    }

    uint32_t pinned = 0, latched = 0;
    pager_test_quiescence(p, &pinned, &latched);
    CHECK(pinned == 0 && latched == 0, "%s: pin=%u latch=%u 이 남지 않는다", label, pinned, latched);
    lock_stats_t ls = db_lock_stats();
    CHECK(ls.total == 0, "%s: row lock %d개가 남지 않는다", label, ls.total);

    executor_test_fail_out_alloc(0, 0);
    return o;
}

static void outcome_free(outcome_t *o)
{
    free(o->out);
    o->out = NULL;
}

static void build_db(pager_t *p)
{
    unlink(DB_PATH);
    if (pager_open(p, DB_PATH, true) != 0) {
        printf("DB를 만들지 못했습니다\n");
        exit(2);
    }
    exec_result_t r = db_execute(p, "CREATE TABLE t (name VARCHAR(64), age INT)");
    free(r.out_buf);
    if (r.status != 0) {
        printf("CREATE 실패: %s\n", r.message);
        exit(2);
    }
    for (int i = 0; i < ROWS; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES ('row-%04d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', %d)", i, i);
        r = db_execute(p, sql);
        free(r.out_buf);
        if (r.status != 0) {
            printf("INSERT 실패: %s\n", r.message);
            exit(2);
        }
    }
}

static bool same_success(const outcome_t *a, const outcome_t *b)
{
    if (a->status != b->status || a->out_len != b->out_len) return false;
    if ((a->out == NULL) != (b->out == NULL)) return false;
    if (a->out != NULL && strcmp(a->out, b->out) != 0) return false;
    return strcmp(a->message, b->message) == 0;
}

/* 한 문장에 대해 malloc·realloc 각 순번 실패를 모두 주입한다 */
static void check_query(pager_t *p, const query_t *q, outcome_t *base)
{
    char label[160];
    snprintf(label, sizeof(label), "%s 기준", q->name);
    *base = run(p, q->sql, 0, 0, label);
    CHECK(base->status == 0, "%s: 주입 없이 성공 (%s)", q->name, base->message);
    CHECK(q->needs_output ? base->out != NULL : base->out == NULL, "%s: 출력 유무", q->name);
    printf("  %-18s malloc=%ld realloc=%ld bytes=%zu\n", q->name, base->mallocs, base->reallocs, base->out_len);

    for (long k = 1; k <= base->mallocs; k++) {
        snprintf(label, sizeof(label), "%s malloc#%ld 실패", q->name, k);
        outcome_t f = run(p, q->sql, k, 0, label);
        if (q->needs_output) {
            CHECK(f.status == -1, "%s: status -1 (got %d)", label, f.status);
            CHECK(f.out == NULL, "%s: 잘린 결과를 돌려주지 않는다", label);
            CHECK(strstr(f.message, ERR_MSG) != NULL, "%s: 공개 오류 메시지 (%s)", label, f.message);
        } else {
            CHECK(same_success(&f, base), "%s: 출력이 필요 없는 문장은 그대로 성공", label);
        }
        outcome_free(&f);
        snprintf(label, sizeof(label), "%s malloc#%ld 실패 뒤 재시도", q->name, k);
        outcome_t again = run(p, q->sql, 0, 0, label);
        CHECK(same_success(&again, base), "%s: 기준과 같은 결과", label);
        outcome_free(&again);
    }

    for (long k = 1; k <= base->reallocs; k++) {
        snprintf(label, sizeof(label), "%s realloc#%ld 실패", q->name, k);
        outcome_t f = run(p, q->sql, 0, k, label);
        CHECK(f.status == -1, "%s: status -1 (got %d)", label, f.status);
        CHECK(f.out == NULL, "%s: 잘린 성공 결과를 남기지 않는다", label);
        CHECK(strstr(f.message, ERR_MSG) != NULL, "%s: 공개 오류 메시지 (%s)", label, f.message);
        outcome_free(&f);
        snprintf(label, sizeof(label), "%s realloc#%ld 실패 뒤 재시도", q->name, k);
        outcome_t again = run(p, q->sql, 0, 0, label);
        CHECK(same_success(&again, base), "%s: 기준과 같은 결과", label);
        outcome_free(&again);
    }

    /* 실제 호출 수를 넘는 순번은 주입이 닿지 않으므로 기준과 같다 */
    snprintf(label, sizeof(label), "%s 범위 밖 순번", q->name);
    outcome_t beyond = run(p, q->sql, base->mallocs + 1, base->reallocs + 1, label);
    CHECK(same_success(&beyond, base), "%s: 주입이 닿지 않으면 기준과 같다", label);
    outcome_free(&beyond);
}

static void test_growth_needs_reallocs(const outcome_t *scan)
{
    printf("\n[test_growth_needs_reallocs]\n");
    CHECK(scan->reallocs >= 3, "큰 결과는 realloc을 여러 번 거친다 (%ld)", scan->reallocs);
}

static void test_failure_does_not_poison_later_statements(pager_t *p)
{
    printf("\n[test_failure_does_not_poison_later_statements]\n");
    /* 실패 뒤 쓰기와 읽기가 모두 이어진다 */
    outcome_t fail = run(p, "SELECT * FROM t WHERE age >= 0", 0, 1, "첫 realloc 실패");
    CHECK(fail.status == -1, "첫 realloc 실패는 오류");
    outcome_free(&fail);
    outcome_t ins = run(p, "INSERT INTO t VALUES ('after-failure', 7)", 0, 0, "실패 뒤 INSERT");
    CHECK(ins.status == 0, "실패 뒤 INSERT 성공 (%s)", ins.message);
    outcome_free(&ins);
    outcome_t cnt = run(p, "SELECT COUNT(*) FROM t", 0, 0, "실패 뒤 COUNT");
    CHECK(cnt.status == 0 && cnt.out != NULL && strstr(cnt.out, "301") != NULL, "행 수 301");
    outcome_free(&cnt);
}

int main(void)
{
    db_init();
    pager_t p;
    build_db(&p);

    printf("[test_each_allocation_failure]\n");
    outcome_t base[QUERY_COUNT];
    for (int i = 0; i < QUERY_COUNT; i++) {
        check_query(&p, &QUERIES[i], &base[i]);
    }
    test_growth_needs_reallocs(&base[2]);
    for (int i = 0; i < QUERY_COUNT; i++) outcome_free(&base[i]);
    test_failure_does_not_poison_later_statements(&p);

    pager_close(&p);
    unlink(DB_PATH);
    db_destroy();

    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
