/*
 * test_step2_concurrency.c — 동시성 제어 테스트 (Step 4)
 *
 * 검증 항목:
 *   1. lock_table: S/X 호환성, timeout, release_all
 *   2. db_execute 멀티스레드 안전성
 *   3. 동시 INSERT 정합성 (row_count == 기대치)
 */

#include "server/lock_table.h"
#include "server/http.h"
#include "storage/pager.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

/* ── 테스트 프레임워크 ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

#define TEST(name) do { \
    fprintf(stderr, "\033[33m\n[%s]\033[0m\n", #name); \
    name(); \
} while(0)

/* ══════════════════════════════════════
 *  1. lock_table 단위 테스트
 * ══════════════════════════════════════ */

/* S-S 호환: 같은 row에 S lock 2개 동시 가능해야 한다 */
static lock_table_t g_lt;

typedef struct {
    uint64_t row_id;
    lock_mode_t mode;
    int result;       /* 0=성공, -1=timeout */
    int delay_ms;     /* lock 획득 후 유지 시간 */
    int64_t acquired_ms;
} lock_thread_arg_t;

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *lock_thread_fn(void *arg)
{
    lock_thread_arg_t *a = (lock_thread_arg_t *)arg;
    a->result = lock_acquire(&g_lt, a->row_id, a->mode);
    if (a->result == 0 && a->delay_ms > 0) {
        a->acquired_ms = mono_ms();
        usleep((unsigned)(a->delay_ms * 1000));
    } else if (a->result == 0) {
        a->acquired_ms = mono_ms();
    }
    /* Strict 2PL: 종료 시 해제 */
    lock_release_all(&g_lt);
    return NULL;
}

static void test_lock_ss_compatible(void)
{
    lock_table_init(&g_lt);

    /* 두 스레드가 같은 row에 S lock */
    lock_thread_arg_t a1 = { .row_id = 100, .mode = LOCK_S, .delay_ms = 200 };
    lock_thread_arg_t a2 = { .row_id = 100, .mode = LOCK_S, .delay_ms = 0 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, lock_thread_fn, &a1);
    usleep(50000); /* t1이 먼저 lock 잡도록 */
    pthread_create(&t2, NULL, lock_thread_fn, &a2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT(a1.result == 0, "S lock #1 성공");
    ASSERT(a2.result == 0, "S lock #2 성공 (S-S 호환)");

    lock_table_destroy(&g_lt);
}

/* S-X 비호환: S가 잡힌 row에 X lock은 대기해야 한다 */
static void test_lock_sx_conflict(void)
{
    lock_table_init(&g_lt);

    /* t1: S lock 잡고 500ms 유지, t2: X lock 시도 → 대기 후 성공 */
    lock_thread_arg_t a1 = { .row_id = 200, .mode = LOCK_S, .delay_ms = 500 };
    lock_thread_arg_t a2 = { .row_id = 200, .mode = LOCK_X, .delay_ms = 0 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, lock_thread_fn, &a1);
    usleep(50000);
    pthread_create(&t2, NULL, lock_thread_fn, &a2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT(a1.result == 0, "S lock 성공");
    ASSERT(a2.result == 0, "X lock 성공 (S 해제 후 획득)");

    lock_table_destroy(&g_lt);
}

/* X-X 비호환 + timeout: X가 잡힌 row에 X lock 시도 → 3초 timeout */
static void test_lock_xx_timeout(void)
{
    lock_table_init(&g_lt);

    /* t1: X lock 잡고 4초 유지 (timeout보다 긴) */
    lock_thread_arg_t a1 = { .row_id = 300, .mode = LOCK_X, .delay_ms = 4000 };
    lock_thread_arg_t a2 = { .row_id = 300, .mode = LOCK_X, .delay_ms = 0 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, lock_thread_fn, &a1);
    usleep(50000);
    pthread_create(&t2, NULL, lock_thread_fn, &a2);

    pthread_join(t2, NULL); /* t2가 먼저 timeout으로 끝남 */
    pthread_join(t1, NULL);

    ASSERT(a1.result == 0, "X lock #1 성공");
    ASSERT(a2.result == -1, "X lock #2 timeout (3초 초과)");

    lock_table_destroy(&g_lt);
}

/* 다른 row는 서로 독립 */
static void test_lock_different_rows(void)
{
    lock_table_init(&g_lt);

    lock_thread_arg_t a1 = { .row_id = 400, .mode = LOCK_X, .delay_ms = 200 };
    lock_thread_arg_t a2 = { .row_id = 401, .mode = LOCK_X, .delay_ms = 0 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, lock_thread_fn, &a1);
    usleep(50000);
    pthread_create(&t2, NULL, lock_thread_fn, &a2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT(a1.result == 0, "X lock row=400 성공");
    ASSERT(a2.result == 0, "X lock row=401 성공 (다른 row, 독립)");

    lock_table_destroy(&g_lt);
}

static void test_lock_release_all_many(void)
{
    lock_table_init(&g_lt);

    ASSERT(lock_acquire(&g_lt, 500, LOCK_S) == 0, "point S lock 획득");
    ASSERT(lock_acquire(&g_lt, 501, LOCK_X) == 0, "point X lock 획득");
    ASSERT(lock_acquire_range(&g_lt, 600, 650, LOCK_S) == 0, "range S lock 획득");

    lock_stats_t before = lock_table_stats(&g_lt);
    ASSERT(before.total == 3, "보유 lock 3개 집계");
    ASSERT(before.shared == 2, "shared lock 2개 집계");
    ASSERT(before.exclusive == 1, "exclusive lock 1개 집계");

    lock_release_all(&g_lt);

    lock_stats_t after = lock_table_stats(&g_lt);
    ASSERT(after.total == 0, "release_all 후 lock 0개");
    ASSERT(after.shared == 0, "release_all 후 shared 0개");
    ASSERT(after.exclusive == 0, "release_all 후 exclusive 0개");

    lock_table_destroy(&g_lt);
}

static void test_lock_writer_priority(void)
{
    lock_table_init(&g_lt);

    lock_thread_arg_t reader1 = {
        .row_id = 550, .mode = LOCK_S, .delay_ms = 700, .acquired_ms = -1
    };
    lock_thread_arg_t writer = {
        .row_id = 550, .mode = LOCK_X, .delay_ms = 400, .acquired_ms = -1
    };
    lock_thread_arg_t reader2 = {
        .row_id = 550, .mode = LOCK_S, .delay_ms = 0, .acquired_ms = -1
    };

    int64_t start_ms = mono_ms();

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, lock_thread_fn, &reader1);
    usleep(50000);
    pthread_create(&t2, NULL, lock_thread_fn, &writer);
    usleep(50000);
    pthread_create(&t3, NULL, lock_thread_fn, &reader2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    ASSERT(reader1.result == 0, "선행 reader lock 성공");
    ASSERT(writer.result == 0, "writer lock 성공");
    ASSERT(reader2.result == 0, "후행 reader lock 성공");

    ASSERT(writer.acquired_ms - start_ms >= 500,
           "writer는 선행 reader 해제 후에야 획득");
    ASSERT(reader2.acquired_ms - writer.acquired_ms >= 250,
           "writer 대기 시작 뒤 들어온 reader는 writer 뒤로 밀림");

    lock_table_destroy(&g_lt);
}

/* ══════════════════════════════════════
 *  2. db_execute 멀티스레드 INSERT
 * ══════════════════════════════════════ */

static pager_t g_pager;
static pthread_mutex_t g_count_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_success_count = 0;

typedef struct {
    int thread_id;
    int insert_count;
} insert_thread_arg_t;

static void *insert_thread_fn(void *arg)
{
    insert_thread_arg_t *a = (insert_thread_arg_t *)arg;
    int ok = 0;
    for (int i = 0; i < a->insert_count; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('t%d_r%d', %d)",
                 a->thread_id, i, a->thread_id * 1000 + i);
        exec_result_t res = db_execute(&g_pager, sql);
        if (res.status == 0) ok++;
        free(res.out_buf);
    }
    pthread_mutex_lock(&g_count_mutex);
    g_success_count += ok;
    pthread_mutex_unlock(&g_count_mutex);
    return NULL;
}

static void test_concurrent_insert(void)
{
    /* DB 생성 */
    remove("__test__conc.db");
    ASSERT(pager_open(&g_pager, "__test__conc.db", true) == 0, "DB 생성");

    db_init();

    /* 테이블 생성 */
    exec_result_t r = db_execute(&g_pager, "CREATE TABLE users (name VARCHAR(32), score INT)");
    ASSERT(r.status == 0, "CREATE TABLE 성공");
    free(r.out_buf);

    /* 4스레드 × 25건 = 100건 동시 INSERT */
    #define NUM_THREADS 4
    #define INSERTS_PER_THREAD 25

    g_success_count = 0;
    pthread_t threads[NUM_THREADS];
    insert_thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].insert_count = INSERTS_PER_THREAD;
        pthread_create(&threads[i], NULL, insert_thread_fn, &args[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    ASSERT(g_success_count == NUM_THREADS * INSERTS_PER_THREAD,
           "100건 INSERT 전부 성공");

    /* COUNT로 정합성 검증 */
    r = db_execute(&g_pager, "SELECT COUNT(*) FROM users");
    ASSERT(r.status == 0, "COUNT(*) 성공");
    /* 결과에서 숫자 추출 */
    if (r.out_buf) {
        int count = 0;
        /* "COUNT(*)\n----------\n100\n" 형태에서 마지막 숫자 파싱 */
        char *p = r.out_buf;
        char *last_line = p;
        while (*p) {
            if (*p == '\n' && *(p+1) && *(p+1) != '\n')
                last_line = p + 1;
            p++;
        }
        count = atoi(last_line);
        ASSERT(count == NUM_THREADS * INSERTS_PER_THREAD,
               "row_count == 100 (정합성 확인)");
    }
    free(r.out_buf);

    db_destroy();
    pager_close(&g_pager);
    remove("__test__conc.db");
}

/* ══════════════════════════════════════
 *  3. 동시 SELECT + UPDATE (다른 row)
 * ══════════════════════════════════════ */

typedef struct {
    const char *sql;
    int status;
} query_thread_arg_t;

static void *query_thread_fn(void *arg)
{
    query_thread_arg_t *a = (query_thread_arg_t *)arg;
    exec_result_t res = db_execute(&g_pager, a->sql);
    a->status = res.status;
    free(res.out_buf);
    return NULL;
}

static void test_concurrent_read_write(void)
{
    remove("__test__rw.db");
    ASSERT(pager_open(&g_pager, "__test__rw.db", true) == 0, "DB 생성");
    db_init();

    exec_result_t r;
    r = db_execute(&g_pager, "CREATE TABLE users (name VARCHAR(32), score INT)");
    free(r.out_buf);
    r = db_execute(&g_pager, "INSERT INTO users VALUES ('alice', 100)");
    free(r.out_buf);
    r = db_execute(&g_pager, "INSERT INTO users VALUES ('bob', 200)");
    free(r.out_buf);

    /* 동시에: SELECT id=1 (S lock) + UPDATE id=2 (X lock) → 둘 다 성공해야 */
    query_thread_arg_t q1 = { .sql = "SELECT * FROM users WHERE id = 1" };
    query_thread_arg_t q2 = { .sql = "UPDATE users SET score = 999 WHERE id = 2" };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, query_thread_fn, &q1);
    pthread_create(&t2, NULL, query_thread_fn, &q2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT(q1.status == 0, "SELECT id=1 성공");
    ASSERT(q2.status == 0, "UPDATE id=2 성공 (다른 row, 비충돌)");

    /* UPDATE 결과 확인 */
    r = db_execute(&g_pager, "SELECT * FROM users WHERE id = 2");
    ASSERT(r.status == 0, "UPDATE 후 SELECT 성공");
    if (r.out_buf) {
        ASSERT(strstr(r.out_buf, "999") != NULL, "score=999로 변경됨");
    }
    free(r.out_buf);

    db_destroy();
    pager_close(&g_pager);
    remove("__test__rw.db");
}

typedef struct {
    lock_table_t *lt;
    uint64_t row_id;
    lock_mode_t mode;
    int delay_ms;
    int result;
} db_lock_thread_arg_t;

static void *db_lock_thread_fn(void *arg)
{
    db_lock_thread_arg_t *a = (db_lock_thread_arg_t *)arg;
    a->result = lock_acquire(a->lt, a->row_id, a->mode);
    if (a->result == 0 && a->delay_ms > 0) {
        usleep((unsigned)(a->delay_ms * 1000));
    }
    lock_release_all(a->lt);
    return NULL;
}

/* INSERT는 활성 range lock과 충돌해야 한다 */
typedef struct {
    uint64_t low;
    uint64_t high;
    lock_mode_t mode;
    int delay_ms;
    int result;
} range_lock_thread_arg_t;

static void *range_lock_thread_fn(void *arg)
{
    range_lock_thread_arg_t *a = (range_lock_thread_arg_t *)arg;
    a->result = lock_acquire_range(db_get_lock_table(), a->low, a->high, a->mode);
    if (a->result == 0 && a->delay_ms > 0) {
        usleep((unsigned)(a->delay_ms * 1000));
    }
    lock_release_all(db_get_lock_table());
    return NULL;
}

static void test_insert_gap_conflict(void)
{
    remove("__test__gap.db");
    ASSERT(pager_open(&g_pager, "__test__gap.db", true) == 0, "DB 생성");
    db_init();

    exec_result_t r = db_execute(&g_pager, "CREATE TABLE users (name VARCHAR(32), score INT)");
    ASSERT(r.status == 0, "CREATE TABLE 성공");
    free(r.out_buf);

    range_lock_thread_arg_t a = {
        .low = 1, .high = UINT64_MAX, .mode = LOCK_X, .delay_ms = 3500
    };
    pthread_t holder;
    pthread_create(&holder, NULL, range_lock_thread_fn, &a);
    usleep(50000);

    r = db_execute(&g_pager, "INSERT INTO users VALUES ('blocked', 1)");
    ASSERT(r.status == -1, "range lock 충돌 시 INSERT timeout");
    if (r.message[0] != '\0') {
        ASSERT(strstr(r.message, "gap check timeout") != NULL,
               "INSERT timeout 메시지 확인");
    }
    free(r.out_buf);

    pthread_join(holder, NULL);
    ASSERT(a.result == 0, "range lock 획득 성공");

    db_destroy();
    pager_close(&g_pager);
    remove("__test__gap.db");
}

static void test_delete_scan_lock_conflict(void)
{
    remove("__test__scan_delete.db");
    ASSERT(pager_open(&g_pager, "__test__scan_delete.db", true) == 0, "DB 생성");
    db_init();

    exec_result_t r = db_execute(&g_pager, "CREATE TABLE users (name VARCHAR(32), score INT)");
    ASSERT(r.status == 0, "CREATE TABLE 성공");
    free(r.out_buf);

    r = db_execute(&g_pager, "INSERT INTO users VALUES ('alice', 100)");
    ASSERT(r.status == 0, "alice INSERT 성공");
    free(r.out_buf);

    r = db_execute(&g_pager, "INSERT INTO users VALUES ('bob', 200)");
    ASSERT(r.status == 0, "bob INSERT 성공");
    free(r.out_buf);

    db_lock_thread_arg_t holder_arg = {
        .lt = db_get_lock_table(),
        .row_id = 1,
        .mode = LOCK_X,
        .delay_ms = 3500,
        .result = -1
    };

    pthread_t holder;
    pthread_create(&holder, NULL, db_lock_thread_fn, &holder_arg);
    usleep(50000);

    r = db_execute(&g_pager, "DELETE FROM users WHERE name = 'alice'");
    ASSERT(r.status == -1, "scan DELETE is blocked by held row lock");
    if (r.message[0] != '\0') {
        ASSERT(strstr(r.message, "row lock 획득 timeout") != NULL,
               "scan DELETE timeout message 확인");
    }
    free(r.out_buf);

    pthread_join(holder, NULL);
    ASSERT(holder_arg.result == 0, "holder row lock 획득 성공");

    r = db_execute(&g_pager, "SELECT * FROM users WHERE id = 1");
    ASSERT(r.status == 0, "conflicted DELETE 후 원본 행 조회 성공");
    if (r.out_buf) {
        ASSERT(strstr(r.out_buf, "alice") != NULL, "alice row remains after timeout");
        free(r.out_buf);
    }

    db_destroy();
    pager_close(&g_pager);
    remove("__test__scan_delete.db");
}

static void test_http_keepalive_error_header(void)
{
    int fds[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair 생성");

    http_send_error_keepalive(fds[0], "oops", 4);

    char buf[512];
    ssize_t n = recv(fds[1], buf, sizeof(buf) - 1, 0);
    ASSERT(n > 0, "keep-alive error 응답 수신");
    if (n > 0) {
        buf[n] = '\0';
        ASSERT(strstr(buf, "Connection: keep-alive") != NULL,
               "에러 응답이 keep-alive 헤더를 보냄");
    }

    close(fds[0]);
    close(fds[1]);
}

static void test_http_read_request_timeout(void)
{
    int fds[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair 생성");

    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = 200000
    };
    ASSERT(setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0,
           "수신 timeout 설정");

    http_request_t req;
    ASSERT(http_read_request(fds[0], &req) == -1,
           "유휴 소켓은 recv timeout 후 읽기 실패로 종료");

    close(fds[0]);
    close(fds[1]);
}

/* ══════════════════════════════════════
 *  main
 * ══════════════════════════════════════ */
int main(void)
{
    fprintf(stderr, "=== Step 2: Concurrency Test Suite ===\n");

    /* lock_table 단위 테스트 */
    TEST(test_lock_ss_compatible);
    TEST(test_lock_sx_conflict);
    TEST(test_lock_xx_timeout);
    TEST(test_lock_different_rows);
    TEST(test_lock_release_all_many);
    TEST(test_lock_writer_priority);

    /* 멀티스레드 db_execute 테스트 */
    TEST(test_concurrent_insert);
    TEST(test_concurrent_read_write);
    TEST(test_insert_gap_conflict);
    TEST(test_delete_scan_lock_conflict);
    TEST(test_http_keepalive_error_header);
    TEST(test_http_read_request_timeout);

    fprintf(stderr, "\n========================================\n");
    if (g_fail == 0) {
        fprintf(stderr, "\033[32mALL PASSED: %d/%d\033[0m\n",
                g_pass, g_pass + g_fail);
    } else {
        fprintf(stderr, "\033[31mFAILED: %d passed, %d failed\033[0m\n",
                g_pass, g_fail);
    }
    fprintf(stderr, "========================================\n");

    return g_fail > 0 ? 1 : 0;
}
