#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql/session.h"
#include "storage/page_file.h"

/*
 * 세션·자원 수명 계약 검사. DB 단독 경로(공개 session API만 사용)를 확인한다. 큐가 없으므로 이 파일은 큐를 거친
 * 소비 경로를 검증하지 않는다. 아래 "모의 소비자"는 큐가 쓸 공개 API 호출 순서만 흉내 낸 것이다.
 * session.c는 -Dcalloc=st_calloc -Dfree=st_free로 빌드해 할당 실패를 주입한다(치환하지 않으면 시작할 때 실패).
 * 확인하는 것:
 *   - 어느 시점에도 shared = 풀·세션·계상 객체·데이터베이스 상주 + 쥔 결과이고 계상 불변식 위반(faults)이 0이다.
 *   - 정상·취소·한도 거절 뒤 query 사용량이 0이고 후속 문장이 정상 실행된다.
 *   - 결과를 쥔 동안 몫이 유지되고 release 뒤 반환된다. 쥔 채 session·database를 닫을 수 있고 풀은 BUSY다.
 *   - query 한도는 측정한 peak에서 성공하고 peak-1에서 거절된다.
 *   - session.c의 n번째 할당 실패마다 몫과 파일이 남지 않는다.
 *   - 여러 스레드가 각자 세션으로 실행해도 회계가 맞는다.
 */

#define DIR_NAME "build/test_session_dir"
#define DB_PATH DIR_NAME "/db.totedb"

static int g_run, g_fail;
#define CHECK(cond, ...)                              \
    do {                                              \
        g_run++;                                      \
        if (!(cond)) {                                \
            g_fail++;                                 \
            printf("  FAIL (line %d): ", __LINE__);   \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
        }                                             \
    } while (0)

/* ── session.c 할당 주입 ── */
static long g_fail_at = -1;  /* n번째(0부터) calloc을 실패시킨다. -1이면 끈다 */
static long g_calls;
static long g_live_blocks;

void *st_calloc(size_t count, size_t size)
{
    if (g_fail_at >= 0 && g_calls++ == g_fail_at) return NULL;
    void *p = calloc(count, size);
    if (p != NULL) __atomic_add_fetch(&g_live_blocks, 1, __ATOMIC_RELAXED);
    return p;
}

void st_free(void *p)
{
    if (p != NULL) __atomic_sub_fetch(&g_live_blocks, 1, __ATOMIC_RELAXED);
    free(p);
}

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

static bool file_exists(void)
{
    struct stat st;
    return stat(DB_PATH, &st) == 0;
}

static session_limits_t big_limits(void)
{
    return (session_limits_t){SIZE_MAX, SIZE_MAX, SIZE_MAX, 8, 0};
}

/* ── 보조 ── */
static session_pool_stats_t pool_stats(session_pool_t *pool)
{
    session_pool_stats_t stats;
    assert(session_pool_stats(pool, &stats) == SESSION_OK);
    return stats;
}

static session_stats_t sess_stats(session_t *session)
{
    session_stats_t stats;
    assert(session_stats_get(session, &stats) == SESSION_OK);
    return stats;
}

/* 결과를 끝까지 소비하고 행 수를 센다. 오류 결과면 클래스를 돌려준다 */
typedef struct {
    result_kind_t kind;
    size_t rows;
    result_error_class_t error_class;
    int code;
} summary_t;

static summary_t consume(result_t *result)
{
    summary_t s = {result_kind(result), 0, RESULT_E_NONE, 0};
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) s.rows++;
    result_outcome_t outcome = result_outcome(result);
    if (outcome.error != NULL) {
        s.error_class = outcome.error->error_class;
        s.code = outcome.error->code;
    }
    return s;
}

static summary_t run(session_t *session, const char *sql, result_t **keep)
{
    result_t *result = NULL;
    database_exec_status_t status =
        session_exec(session, NULL, (const uint8_t *)sql, strlen(sql), NULL, &result);
    summary_t s = {RESULT_KIND_ERROR, 0, RESULT_E_INTERNAL, -1};
    if (status == DATABASE_EXEC_OK) {
        s = consume(result);
    } else {
        s.code = 1000 + (int)status;
    }
    if (keep != NULL && status == DATABASE_EXEC_OK) {
        *keep = result;
    } else {
        result_release(&result);
    }
    return s;
}

/* 모든 invariant: 지금 쥔 결과 몫은 제외하고 query는 비었다 */
static void check_idle(session_pool_t *pool, session_t *session, const char *what)
{
    session_pool_stats_t p = pool_stats(pool);
    CHECK(p.accounting_faults == 0, "%s: faults %llu", what, (unsigned long long)p.accounting_faults);
    if (session != NULL) {
        session_stats_t s = sess_stats(session);
        CHECK(s.query_used == 0, "%s: query_used %zu", what, s.query_used);
        CHECK(p.shared_used == p.resident_used + p.result_used + s.query_used, "%s: shared %zu != %zu+%zu+%zu",
              what, p.shared_used, p.resident_used, p.result_used, s.query_used);
    }
}

static void setup_data(session_t *session)
{
    CHECK(run(session, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)", NULL).kind ==
              RESULT_KIND_COMMAND,
          "create table");
    for (int i = 0; i < 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t (name) VALUES ('row-number-%d')", i);
        CHECK(run(session, sql, NULL).kind == RESULT_KIND_COMMAND, "insert %d", i);
    }
}

/* 풀·DB·세션을 한 번에 연다 */
typedef struct {
    session_pool_t *pool;
    session_db_t *db;
    session_t *session;
} fixture_t;

static fixture_t open_fixture(const session_limits_t *limits)
{
    fixture_t f = {0};
    assert(session_pool_open(limits, &f.pool) == SESSION_OK);
    database_status_t ds;
    assert(session_pool_create_database(f.pool, DB_PATH, &f.db, &ds) == SESSION_OK);
    assert(session_open(f.db, &f.session) == SESSION_OK);
    return f;
}

static void close_fixture(fixture_t *f)
{
    assert(session_close(f->session) == SESSION_OK);
    database_status_t ds;
    assert(session_pool_close_database(f->db, &ds) == SESSION_OK);
    assert(session_pool_close(f->pool) == SESSION_OK);
}

/* ── 검사 ── */

static void test_lifecycle(void)
{
    printf("정상 경로·회계 불변식·결과 수명\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits);
    check_idle(f.pool, f.session, "열린 직후");
    setup_data(f.session);
    check_idle(f.pool, f.session, "준비 뒤");

    session_pool_stats_t before = pool_stats(f.pool);
    result_t *held = NULL;
    summary_t s = run(f.session, "SELECT * FROM t", &held);
    CHECK(s.kind == RESULT_KIND_ROWS && s.rows == 20, "select rows %zu", s.rows);
    session_pool_stats_t during = pool_stats(f.pool);
    session_stats_t sess = sess_stats(f.session);
    CHECK(during.result_used > 0, "결과를 쥐는 동안 몫이 유지된다");
    CHECK(sess.result_used == during.result_used, "세션 몫 %zu 풀 몫 %zu", sess.result_used, during.result_used);
    CHECK(sess.query_used == 0, "끝난 문장의 query 몫 %zu", sess.query_used);
    CHECK(during.shared_used == before.shared_used + during.result_used, "shared는 결과 몫만큼만 늘었다");
    check_idle(f.pool, f.session, "결과를 쥔 채");

    /* 쥔 채 후속 문장이 돌고 몫이 쌓인다 */
    result_t *second = NULL;
    CHECK(run(f.session, "SELECT name FROM t", &second).rows == 20, "후속 질의");
    CHECK(pool_stats(f.pool).result_used > during.result_used, "결과 두 개의 몫");
    result_release(&second);
    CHECK(pool_stats(f.pool).result_used == during.result_used, "하나 반환");

    /* 결과를 쥔 채 세션·데이터베이스를 닫고 풀은 BUSY */
    CHECK(session_close(f.session) == SESSION_OK, "결과가 남아도 세션을 닫는다");
    database_status_t ds;
    CHECK(session_pool_close_database(f.db, &ds) == SESSION_OK, "결과가 남아도 DB를 닫는다");
    CHECK(session_pool_close(f.pool) == SESSION_BUSY, "남은 결과가 있으면 풀은 BUSY");
    session_pool_stats_t after_close = pool_stats(f.pool);
    CHECK(after_close.result_used == during.result_used, "닫은 뒤에도 결과 몫 유지");
    CHECK(after_close.sessions_open == 0 && after_close.databases_open == 0, "열린 세션·DB 없음");
    result_release(&held);
    session_pool_stats_t end = pool_stats(f.pool);
    CHECK(end.result_used == 0 && end.accounting_faults == 0, "release 뒤 결과 몫 0");
    CHECK(end.shared_used == end.resident_used, "남은 몫은 상주뿐");
    CHECK(session_pool_close(f.pool) == SESSION_OK, "마지막 결과 release 뒤 풀을 닫는다");
    clean_dir();
}

static void test_busy_rules(void)
{
    printf("BUSY·LIMIT 규칙\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    limits.max_sessions = 2;
    fixture_t f = open_fixture(&limits);
    session_t *second = NULL, *third = NULL;
    CHECK(session_open(f.db, &second) == SESSION_OK, "두 번째");
    CHECK(session_open(f.db, &third) == SESSION_LIMIT, "max_sessions 초과");
    database_status_t ds;
    CHECK(session_pool_close_database(f.db, &ds) == SESSION_BUSY, "세션이 열려 DB를 못 닫는다");
    CHECK(session_pool_close(f.pool) == SESSION_BUSY, "열린 것이 있어 풀을 못 닫는다");
    CHECK(session_close(second) == SESSION_OK, "닫기");
    CHECK(session_open(f.db, &third) == SESSION_OK, "자리가 나면 다시 연다");
    CHECK(session_close(third) == SESSION_OK, "닫기");
    close_fixture(&f);
    clean_dir();
}

static void test_cancel(void)
{
    printf("취소 뒤 회수와 후속 질의\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits);
    setup_data(f.session);
    session_pool_stats_t base = pool_stats(f.pool);

    session_cancel(f.session);
    summary_t s = run(f.session, "SELECT * FROM t", NULL);
    CHECK(s.kind == RESULT_KIND_ERROR && s.error_class == RESULT_E_CANCELLED && s.code == DATABASE_CODE_CANCELLED,
          "취소 결과 kind=%d class=%d code=%d", s.kind, s.error_class, s.code);
    check_idle(f.pool, f.session, "취소 뒤");
    CHECK(pool_stats(f.pool).shared_used == base.shared_used, "취소 뒤 shared가 기준으로 돌아온다");
    CHECK(run(f.session, "SELECT * FROM t", NULL).rows == 20, "취소 뒤 후속 질의");

    /* 취소된 INSERT는 아무것도 반영하지 않는다 */
    session_cancel(f.session);
    s = run(f.session, "INSERT INTO t (name) VALUES ('never')", NULL);
    CHECK(s.error_class == RESULT_E_CANCELLED, "INSERT 취소");
    CHECK(run(f.session, "SELECT * FROM t", NULL).rows == 20, "취소된 INSERT는 반영되지 않았다");

    check_idle(f.pool, f.session, "취소 시험 끝");
    close_fixture(&f);
    clean_dir();
}

static bool cancel_always(void *ctx)
{
    (void)ctx;
    return true;
}

static void test_cancel_option(void)
{
    printf("호출자 취소 조건\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits);
    setup_data(f.session);
    database_cancel_t cancel = {cancel_always, NULL};
    session_exec_options_t options = {&cancel};
    result_t *result = NULL;
    const char *sql = "SELECT * FROM t";
    CHECK(session_exec(f.session, &options, (const uint8_t *)sql, strlen(sql), NULL, &result) == DATABASE_EXEC_OK,
          "취소도 오류 결과로 돌아온다");
    summary_t s = consume(result);
    CHECK(s.error_class == RESULT_E_CANCELLED, "호출자 조건으로 취소");
    result_release(&result);
    check_idle(f.pool, f.session, "호출자 취소 뒤");
    CHECK(run(f.session, "SELECT * FROM t", NULL).rows == 20, "후속 질의");
    close_fixture(&f);
    clean_dir();
}

static void test_query_limit_boundary(void)
{
    printf("query 한도 경계(peak, peak-1)\n");
    const char *statements[] = {
        "SELECT * FROM t",
        "SELECT name FROM t WHERE id > 5",
        "INSERT INTO t (name) VALUES ('boundary')",
        "UPDATE t SET name = 'changed' WHERE id = 3",
        "DELETE FROM t WHERE id = 4",
        "CREATE TABLE u (x INTEGER)",
    };
    for (size_t i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) {
        /* 측정: 한도 없는 풀에서 같은 준비 뒤 마지막 문장의 query peak */
        fresh_dir();
        session_limits_t limits = big_limits();
        fixture_t f = open_fixture(&limits);
        setup_data(f.session);
        CHECK(run(f.session, statements[i], NULL).kind != RESULT_KIND_ERROR, "측정 성공 %zu", i);
        size_t peak = sess_stats(f.session).last_query_peak;
        close_fixture(&f);
        CHECK(peak > 0, "peak 측정 %zu", i);

        for (int delta = 0; delta < 2; delta++) {
            fresh_dir();
            limits = big_limits();
            limits.query_limit = delta == 0 ? peak : peak - 1;
            session_limits_t setup_limits = big_limits();
            /* 준비는 큰 한도로, 시험 문장은 좁은 한도로 하려고 같은 DB를 닫고 다시 연다 */
            fixture_t g = open_fixture(&setup_limits);
            setup_data(g.session);
            close_fixture(&g);
            session_pool_t *pool;
            assert(session_pool_open(&limits, &pool) == SESSION_OK);
            session_db_t *db;
            database_status_t ds;
            assert(session_pool_open_database(pool, DB_PATH, true, &db, &ds) == SESSION_OK);
            session_t *session;
            assert(session_open(db, &session) == SESSION_OK);
            summary_t s = run(session, statements[i], NULL);
            if (delta == 0) {
                CHECK(s.kind != RESULT_KIND_ERROR, "한도=peak 성공 %zu (class %d)", i, s.error_class);
            } else {
                CHECK(s.kind == RESULT_KIND_ERROR && s.error_class == RESULT_E_NOMEM,
                      "한도=peak-1 거절 %zu (kind %d class %d code %d)", i, s.kind, s.error_class, s.code);
                /* 거절 뒤에도 후속 질의가 돈다(한도를 충족하는 짧은 문장) */
                CHECK(sess_stats(session).query_used == 0, "거절 뒤 query 0");
            }
            check_idle(pool, session, "경계 뒤");
            assert(session_close(session) == SESSION_OK);
            assert(session_pool_close_database(db, &ds) == SESSION_OK);
            assert(session_pool_close(pool) == SESSION_OK);
        }
    }
    clean_dir();
}

/* #79: 읽기 캐시 프레임은 데이터베이스의 상주 몫이라 shared 예산에 들어가고, 그 몫 때문에 경계에서 열기가 거절된다 */
static size_t create_peak_with_cache(size_t pages, size_t *resident)
{
    page_file_set_default_cache_pages(pages);
    fresh_dir();
    session_limits_t limits = big_limits();
    session_pool_t *pool;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    size_t pool_only = pool_stats(pool).shared_used;
    session_db_t *db;
    database_status_t ds;
    assert(session_pool_create_database(pool, DB_PATH, &db, &ds) == SESSION_OK);
    *resident = pool_stats(pool).shared_used - pool_only;
    size_t peak = pool_stats(pool).shared_peak;
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    assert(session_pool_close(pool) == SESSION_OK);
    clean_dir();
    return peak;
}

static void test_cache_counts_in_shared_budget(void)
{
    printf("읽기 캐시가 shared 예산에 든다\n");
    size_t none, few, full;
    size_t peak_none = create_peak_with_cache(0, &none);
    create_peak_with_cache(16, &few);
    size_t peak_full = create_peak_with_cache(PAGE_FILE_CACHE_PAGES, &full);
    CHECK(none < few && few < full && full - none >= (size_t)PAGE_FILE_CACHE_PAGES * PAGE_FILE_PAGE_BYTES / 2,
          "캐시 0·16·%u장 상주 몫 %zu < %zu < %zu", PAGE_FILE_CACHE_PAGES, none, few, full);
    /* 캐시 없이 열리는 한도라도 캐시를 켜면 거절된다 */
    session_limits_t limits = big_limits();
    limits.shared_limit = peak_full - 1;
    CHECK(peak_none <= limits.shared_limit, "캐시 없는 열기 최대 %zu <= 한도 %zu", peak_none, limits.shared_limit);
    for (int cached = 0; cached < 2; cached++) {
        page_file_set_default_cache_pages(cached ? PAGE_FILE_CACHE_PAGES : 0);
        fresh_dir();
        session_pool_t *pool;
        assert(session_pool_open(&limits, &pool) == SESSION_OK);
        session_db_t *db = NULL;
        database_status_t ds;
        session_status_t st = session_pool_create_database(pool, DB_PATH, &db, &ds);
        CHECK(cached ? st == SESSION_NOMEM : st == SESSION_OK, "캐시 %s: 한도 %zu에서 열기 %s", cached ? "켬" : "끔",
              limits.shared_limit, cached ? "거절" : "허용");
        if (st == SESSION_OK) assert(session_pool_close_database(db, &ds) == SESSION_OK);
        assert(session_pool_close(pool) == SESSION_OK);
        clean_dir();
    }
    page_file_set_default_cache_pages(PAGE_FILE_CACHE_PAGES);
}

static void test_shared_limit(void)
{
    printf("shared 한도와 세션·DB 열기\n");
    /* 열기 몫을 측정 */
    fresh_dir();
    session_limits_t limits = big_limits();
    session_pool_t *pool;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    size_t pool_only = pool_stats(pool).shared_used;
    CHECK(pool_only > 0, "풀 구조체도 shared 몫이다");
    session_db_t *db;
    database_status_t ds;
    assert(session_pool_create_database(pool, DB_PATH, &db, &ds) == SESSION_OK);
    size_t with_db = pool_stats(pool).shared_used;
    size_t with_db_peak = pool_stats(pool).shared_peak;
    session_t *session;
    assert(session_open(db, &session) == SESSION_OK);
    size_t with_session = pool_stats(pool).shared_used;
    assert(session_close(session) == SESSION_OK);
    CHECK(pool_stats(pool).shared_used == with_db, "세션을 닫으면 몫이 돌아온다(account 포함)");
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    CHECK(pool_stats(pool).shared_used == pool_only, "DB를 닫으면 풀 몫만 남는다");
    assert(session_pool_close(pool) == SESSION_OK);
    clean_dir();

    /* 한도가 풀 구조체보다 작으면 열리지 않는다 */
    limits = big_limits();
    limits.shared_limit = pool_only - 1;
    CHECK(session_pool_open(&limits, &pool) == SESSION_NOMEM, "풀 구조체 몫보다 작은 한도");
    limits = big_limits();

    /* DB: 열기 도중 최댓값(admission)이 경계다 */
    size_t create_peak = with_db_peak;
    CHECK(create_peak >= with_db, "create peak %zu >= 상주 %zu", create_peak, with_db);
    fresh_dir();
    limits.shared_limit = create_peak - 1;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    CHECK(session_pool_create_database(pool, DB_PATH, &db, &ds) == SESSION_NOMEM, "DB peak-1 거절");
    CHECK(!file_exists(), "거절된 create는 파일을 남기지 않는다");
    CHECK(pool_stats(pool).shared_used == pool_only && pool_stats(pool).databases_open == 0, "거절 뒤 몫 복구");
    assert(session_pool_close(pool) == SESSION_OK);

    /* 세션: 남은 여유가 구조체 몫 미만이 될 때까지만 열린다 */
    size_t ov = with_session - with_db;
    fresh_dir();
    limits.shared_limit = create_peak + 3 * ov + (ov > 1 ? ov / 2 : 0);
    limits.max_sessions = 1000;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    assert(session_pool_create_database(pool, DB_PATH, &db, &ds) == SESSION_OK);
    size_t room = limits.shared_limit - pool_stats(pool).shared_used;
    size_t expect = room / ov, opened = 0;
    session_t *many[512];
    while (opened < 512) {
        size_t used = pool_stats(pool).shared_used;
        session_status_t st = session_open(db, &many[opened]);
        if (st == SESSION_OK) {
            CHECK(used + ov <= limits.shared_limit, "허용된 open은 한도 안 %zu", opened);
            opened++;
        } else {
            CHECK(st == SESSION_NOMEM && used + ov > limits.shared_limit, "거절은 한도를 넘을 때만");
            CHECK(pool_stats(pool).shared_used == used && pool_stats(pool).sessions_open == opened, "거절 뒤 복구");
            break;
        }
    }
    CHECK(opened == expect, "열린 세션 %zu 기대 %zu", opened, expect);
    for (size_t i = 0; i < opened; i++) assert(session_close(many[i]) == SESSION_OK);
    CHECK(pool_stats(pool).shared_used == with_db, "닫으면 DB 몫만 남는다");
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    assert(session_pool_close(pool) == SESSION_OK);
    clean_dir();
}

static void test_result_counts_against_limits(void)
{
    printf("쥔 결과가 다음 문장의 한도를 줄인다\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits);
    setup_data(f.session);
    result_t *held = NULL;
    run(f.session, "SELECT * FROM t", &held);
    size_t result_bytes = sess_stats(f.session).result_used;
    size_t peak = sess_stats(f.session).last_query_peak;
    CHECK(result_bytes > 0 && peak >= result_bytes, "측정 result=%zu peak=%zu", result_bytes, peak);
    result_release(&held);
    close_fixture(&f);

    /* session_limit이 질의 하나의 peak면 결과를 쥔 채 두 번째 질의는 거절되고 release 뒤에는 돈다 */
    limits = big_limits();
    limits.session_limit = peak;
    session_pool_t *pool;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    session_db_t *db;
    database_status_t ds;
    assert(session_pool_open_database(pool, DB_PATH, true, &db, &ds) == SESSION_OK);
    session_t *session;
    assert(session_open(db, &session) == SESSION_OK);
    CHECK(run(session, "SELECT * FROM t", &held).rows == 20, "한도 안의 첫 질의");
    summary_t s = run(session, "SELECT * FROM t", NULL);
    CHECK(s.kind == RESULT_KIND_ERROR && s.error_class == RESULT_E_NOMEM, "쥔 결과 때문에 둘째 거절");
    check_idle(pool, session, "거절 뒤");
    result_release(&held);
    CHECK(run(session, "SELECT * FROM t", NULL).rows == 20, "release 뒤 다시 돈다");
    check_idle(pool, session, "release 뒤");
    assert(session_close(session) == SESSION_OK);
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    assert(session_pool_close(pool) == SESSION_OK);
    clean_dir();
}

static void test_alloc_failure_sweep(void)
{
    printf("session.c n번째 할당 실패 sweep\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    session_pool_t *pool;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    size_t pool_only = pool_stats(pool).shared_used;
    session_db_t *db;
    database_status_t ds;
    assert(session_pool_create_database(pool, DB_PATH, &db, &ds) == SESSION_OK);
    size_t with_db = pool_stats(pool).shared_used;
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    assert(session_pool_close(pool) == SESSION_OK);

    long failures = 0;
    for (long n = 0; n < 64; n++) {
        fresh_dir();
        g_fail_at = n;
        g_calls = 0;
        long live_before = g_live_blocks;
        session_pool_t *p = NULL;
        session_db_t *d = NULL;
        session_t *sess = NULL;
        bool injected = true;
        session_status_t st = session_pool_open(&limits, &p);
        if (st != SESSION_OK) {
            CHECK(st == SESSION_NOMEM && p == NULL, "pool_open 실패 %ld", n);
            failures++;
        } else {
            st = session_pool_create_database(p, DB_PATH, &d, &ds);
            if (st != SESSION_OK) {
                CHECK(st == SESSION_NOMEM && d == NULL, "create 실패 %ld", n);
                CHECK(!file_exists(), "create 실패 뒤 파일이 남지 않는다 %ld", n);
                CHECK(pool_stats(p).shared_used == pool_only && pool_stats(p).databases_open == 0, "create 실패 뒤 몫 %ld", n);
                failures++;
            } else {
                st = session_open(d, &sess);
                if (st != SESSION_OK) {
                    CHECK(st == SESSION_NOMEM && sess == NULL, "open 실패 %ld", n);
                    CHECK(pool_stats(p).shared_used == with_db && pool_stats(p).sessions_open == 0, "open 실패 뒤 몫 %ld", n);
                    failures++;
                } else {
                    injected = false;
                    CHECK(run(sess, "CREATE TABLE a (x INTEGER)", NULL).kind == RESULT_KIND_COMMAND, "성공 경로 실행");
                    assert(session_close(sess) == SESSION_OK);
                }
                g_fail_at = -1;
                assert(session_pool_close_database(d, &ds) == SESSION_OK);
            }
            g_fail_at = -1;
            CHECK(pool_stats(p).accounting_faults == 0, "faults %ld", n);
            assert(session_pool_close(p) == SESSION_OK);
        }
        g_fail_at = -1;
        CHECK(g_live_blocks == live_before, "누수 %ld: live %ld", n, g_live_blocks - live_before);
        if (!injected) break;
    }
    CHECK(failures >= 4, "주입 지점 %ld개 확인", failures);
    g_fail_at = -1;
    clean_dir();
}

/* ── 스레드: 각자 세션, 같은 DB, 좁은 shared 한도 ── */
typedef struct {
    session_t *session;
    int done;
    int refused;
    int bad;
} worker_t;

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    for (int i = 0; i < 60; i++) {
        summary_t s = run(w->session, "SELECT * FROM t", NULL);
        if (s.kind == RESULT_KIND_ROWS && s.rows == 20) {
            w->done++;
        } else if (s.kind == RESULT_KIND_ERROR && s.error_class == RESULT_E_NOMEM) {
            w->refused++;
        } else {
            w->bad++;
        }
    }
    return NULL;
}

static void test_threads(void)
{
    printf("스레드 회계\n");
    fresh_dir();
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits);
    setup_data(f.session);
    run(f.session, "SELECT * FROM t", NULL);
    size_t peak = sess_stats(f.session).last_query_peak;
    close_fixture(&f);

    limits = big_limits();
    database_status_t ds;
    enum { N = 4 };
    limits = big_limits();
    limits.max_sessions = N + 1;
    session_pool_t *pool;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    session_db_t *db;
    assert(session_pool_open_database(pool, DB_PATH, true, &db, &ds) == SESSION_OK);
    worker_t workers[N] = {0};
    pthread_t threads[N];
    session_t *holder;
    assert(session_open(db, &holder) == SESSION_OK);
    for (int i = 0; i < N; i++) assert(session_open(db, &workers[i].session) == SESSION_OK);
    size_t base = pool_stats(pool).shared_used;
    /* 질의 하나가 들어갈 만큼만 남긴 풀은 새 풀이어야 하므로 한도를 지금 다시 만든다 */
    for (int i = 0; i < N; i++) assert(session_close(workers[i].session) == SESSION_OK);
    assert(session_close(holder) == SESSION_OK);
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    assert(session_pool_close(pool) == SESSION_OK);

    limits.shared_limit = base + peak;
    assert(session_pool_open(&limits, &pool) == SESSION_OK);
    assert(session_pool_open_database(pool, DB_PATH, true, &db, &ds) == SESSION_OK);
    assert(session_open(db, &holder) == SESSION_OK);
    for (int i = 0; i < N; i++) assert(session_open(db, &workers[i].session) == SESSION_OK);
    CHECK(pool_stats(pool).shared_used == base, "같은 기준 %zu vs %zu", pool_stats(pool).shared_used, base);

    /* 1단계(결정적): 결과를 쥔 동안 다른 스레드의 질의는 모두 거절된다 */
    result_t *held = NULL;
    CHECK(run(holder, "SELECT * FROM t", &held).rows == 20, "홀더 질의");
    for (int i = 0; i < N; i++) assert(pthread_create(&threads[i], NULL, worker_main, &workers[i]) == 0);
    for (int i = 0; i < N; i++) assert(pthread_join(threads[i], NULL) == 0);
    int refused1 = 0, done1 = 0, bad = 0;
    for (int i = 0; i < N; i++) {
        refused1 += workers[i].refused;
        done1 += workers[i].done;
        bad += workers[i].bad;
    }
    CHECK(done1 == 0 && refused1 == N * 60, "홀더가 쥔 동안 전부 거절: 실행 %d 거절 %d", done1, refused1);
    result_release(&held);
    check_idle(pool, holder, "1단계 뒤");

    /* 2단계: 경쟁. 결과는 실행 또는 NOMEM 거절뿐이고 끝난 뒤 기준으로 돌아온다 */
    for (int i = 0; i < N; i++) {
        workers[i].done = workers[i].refused = workers[i].bad = 0;
        assert(pthread_create(&threads[i], NULL, worker_main, &workers[i]) == 0);
    }
    for (int i = 0; i < N; i++) assert(pthread_join(threads[i], NULL) == 0);
    int done = 0, refused = 0;
    for (int i = 0; i < N; i++) {
        done += workers[i].done;
        refused += workers[i].refused;
        bad += workers[i].bad;
    }
    session_pool_stats_t stats = pool_stats(pool);
    CHECK(bad == 0, "정상도 NOMEM도 아닌 결과 %d", bad);
    CHECK(done > 0, "일부는 실행된다 %d", done);
    CHECK(done + refused == N * 60, "모든 문장이 끝났다");
    CHECK(stats.shared_peak <= limits.shared_limit, "peak %zu <= 한도 %zu", stats.shared_peak, limits.shared_limit);
    CHECK(stats.shared_used == base && stats.result_used == 0, "끝난 뒤 기준으로 복귀 %zu vs %zu", stats.shared_used, base);
    CHECK(stats.accounting_faults == 0, "faults %llu", (unsigned long long)stats.accounting_faults);
    printf("  (경쟁 단계 실행 %d, 거절 %d, peak %zu / 한도 %zu)\n", done, refused, stats.shared_peak,
           limits.shared_limit);
    for (int i = 0; i < N; i++) assert(session_close(workers[i].session) == SESSION_OK);
    assert(session_close(holder) == SESSION_OK);
    assert(session_pool_close_database(db, &ds) == SESSION_OK);
    assert(session_pool_close(pool) == SESSION_OK);
    clean_dir();
}

/* ── 모의 소비자 ──
 * 큐가 아니다. 큐가 소비할 때 쓸 공개 API 순서(문장마다 세션 열기→실행→결과 소비·release→세션 닫기)를
 * 별도 스레드에서 따르며 같은 계약을 확인한다. 큐 구현을 검증한 것이 아니다. */
typedef struct {
    session_db_t *db;
    const char *const *jobs;
    size_t count;
    summary_t *out;
    size_t cancel_index;
} consumer_t;

static void *consumer_main(void *arg)
{
    consumer_t *c = arg;
    for (size_t i = 0; i < c->count; i++) {
        session_t *session;
        assert(session_open(c->db, &session) == SESSION_OK);
        if (i == c->cancel_index) session_cancel(session);
        c->out[i] = run(session, c->jobs[i], NULL);
        assert(session_close(session) == SESSION_OK);
    }
    return NULL;
}

static void test_mock_consumer(void)
{
    printf("모의 소비자(큐 아님): 별도 스레드가 공개 API만으로 소비\n");
    const char *const jobs[] = {
        "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)",
        "INSERT INTO t (name) VALUES ('a')",
        "INSERT INTO t (name) VALUES ('b')",
        "SELECT * FROM t",
        "INSERT INTO t (name) VALUES ('after-cancel')",
        "SELECT * FROM t",
        "SELECT broken FROM",
    };
    enum { COUNT = sizeof(jobs) / sizeof(jobs[0]) };
    summary_t direct[COUNT], queued[COUNT];

    for (int pass = 0; pass < 2; pass++) {
        fresh_dir();
        session_limits_t limits = big_limits();
        session_pool_t *pool;
        assert(session_pool_open(&limits, &pool) == SESSION_OK);
        session_db_t *db;
        database_status_t ds;
        assert(session_pool_create_database(pool, DB_PATH, &db, &ds) == SESSION_OK);
        summary_t *out = pass == 0 ? direct : queued;
        consumer_t c = {db, jobs, COUNT, out, 3};  /* 네 번째 문장(SELECT)을 취소 */
        if (pass == 0) {
            /* DB 단독: 같은 순서를 호출 스레드에서 직접 */
            for (size_t i = 0; i < COUNT; i++) {
                session_t *session;
                assert(session_open(db, &session) == SESSION_OK);
                if (i == c.cancel_index) session_cancel(session);
                out[i] = run(session, jobs[i], NULL);
                assert(session_close(session) == SESSION_OK);
            }
        } else {
            pthread_t t;
            assert(pthread_create(&t, NULL, consumer_main, &c) == 0);
            assert(pthread_join(t, NULL) == 0);
        }
        session_pool_stats_t stats = pool_stats(pool);
        CHECK(stats.accounting_faults == 0 && stats.result_used == 0 && stats.sessions_open == 0, "pass %d 끝 상태", pass);
        assert(session_pool_close_database(db, &ds) == SESSION_OK);
        assert(session_pool_close(pool) == SESSION_OK);
    }
    for (size_t i = 0; i < COUNT; i++) {
        CHECK(direct[i].kind == queued[i].kind && direct[i].rows == queued[i].rows &&
                  direct[i].error_class == queued[i].error_class && direct[i].code == queued[i].code,
              "문장 %zu: DB 단독과 소비자 결과가 같다", i);
    }
    CHECK(direct[3].error_class == RESULT_E_CANCELLED, "취소된 문장");
    CHECK(direct[5].rows == 3, "취소 뒤 문장은 정상 반영 %zu", direct[5].rows);
    CHECK(direct[6].kind == RESULT_KIND_ERROR && direct[6].error_class == RESULT_E_SYNTAX, "구문 오류");
    clean_dir();
}

/* ── 실행 중인 결과 release와 pool_close ── */
typedef struct {
    result_t *held;
    int *go;
} releaser_t;

static void *releaser_main(void *arg)
{
    releaser_t *r = arg;
    while (!__atomic_load_n(r->go, __ATOMIC_ACQUIRE)) sched_yield();
    result_release(&r->held);
    return NULL;
}

static void test_release_races_pool_close(void)
{
    printf("결과 release와 pool_close 경합\n");
    enum { N = 6, ROUNDS = 40 };
    for (int round = 0; round < ROUNDS; round++) {
        fresh_dir();
        session_limits_t limits = big_limits();
        fixture_t f = open_fixture(&limits);
        setup_data(f.session);
        releaser_t jobs[N];
        int go = 0;
        for (int i = 0; i < N; i++) {
            jobs[i].held = NULL;
            jobs[i].go = &go;
            assert(run(f.session, "SELECT * FROM t", &jobs[i].held).rows == 20);
        }
        /* 세션·DB는 먼저 닫고 결과만 남긴다(수명 호출은 이 스레드가 직렬로 끝낸다) */
        assert(session_close(f.session) == SESSION_OK);
        database_status_t ds;
        assert(session_pool_close_database(f.db, &ds) == SESSION_OK);
        pthread_t threads[N];
        for (int i = 0; i < N; i++) assert(pthread_create(&threads[i], NULL, releaser_main, &jobs[i]) == 0);
        __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
        /* release가 도는 동안 pool_close를 되풀이한다. 성공하는 순간은 마지막 반환이 끝난 뒤여야 한다 */
        long busy = 0;
        session_status_t st;
        while ((st = session_pool_close(f.pool)) == SESSION_BUSY) {
            busy++;
            sched_yield();
        }
        CHECK(st == SESSION_OK, "round %d: 풀 닫기 (%d)", round, (int)st);
        /* pool_close 성공은 release 스레드의 마지막 쓰기(*result = NULL)까지 보장하지 않는다. join한 뒤에 읽는다 */
        for (int i = 0; i < N; i++) pthread_join(threads[i], NULL);
        for (int i = 0; i < N; i++) CHECK(jobs[i].held == NULL, "round %d: 결과 %d release됨", round, i);
        (void)busy;
    }
    clean_dir();
}

int main(void)
{
    test_lifecycle();
    test_busy_rules();
    test_cancel();
    test_cancel_option();
    test_query_limit_boundary();
    test_shared_limit();
    test_cache_counts_in_shared_budget();
    test_result_counts_against_limits();
    test_alloc_failure_sweep();
    test_threads();
    test_mock_consumer();
    test_release_races_pool_close();
    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
