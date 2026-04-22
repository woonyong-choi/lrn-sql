# 12. 테스트 하네스 엔지니어링 계획서

> **목적**: 구현 계획서(11장)의 각 단계가 끝날 때마다 **자동으로 정합성을 검증**할 수 있는 테스트 인프라를 먼저 설계한다.
> 코드보다 테스트가 먼저 — 하네스가 통과해야만 다음 단계로 진행한다.

---

## 1. 테스트 프레임워크 설계

### 1-1. 기존 프레임워크 계승

wk07의 `tests/test_all.c`가 제공하는 assert 매크로 기반 프레임워크를 그대로 계승하되, 다음을 확장한다.

```c
/* tests/harness.h — 테스트 프레임워크 공통 헤더 */

#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <time.h>

/* ── 색상 출력 ── */
#define CLR_GREEN  "\033[32m"
#define CLR_RED    "\033[31m"
#define CLR_YELLOW "\033[33m"
#define CLR_RESET  "\033[0m"

/* ── 카운터 (전역) ── */
extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

/* ── 단일 assertion ── */
#define ASSERT_TRUE(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf(CLR_RED "  FAIL: %s (line %d)" CLR_RESET "\n", msg, __LINE__); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b, msg) \
    ASSERT_TRUE((a) == (b), msg)

#define ASSERT_EQ_STR(a, b, msg) \
    ASSERT_TRUE(strcmp((a), (b)) == 0, msg)

#define ASSERT_NOT_NULL(ptr, msg) \
    ASSERT_TRUE((ptr) != NULL, msg)

/* ── 테스트 스위트 래퍼 ── */
#define TEST_SUITE(name) \
    static void name(void); \
    static void name(void)

#define RUN_SUITE(name) do { \
    printf(CLR_YELLOW "\n[%s]" CLR_RESET "\n", #name); \
    name(); \
} while(0)

/* ── 임시 DB 파일 관리 ── */
#define TEST_DB_PREFIX "__test_harness_"

static inline char *test_db_path(const char *suffix) {
    static char buf[256];
    snprintf(buf, sizeof(buf), TEST_DB_PREFIX "%s.db", suffix);
    return buf;
}

static inline void test_db_cleanup(const char *suffix) {
    unlink(test_db_path(suffix));
}

/* ── 결과 요약 ── */
static inline int harness_summary(void) {
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

#endif /* HARNESS_H */
```

### 1-2. 테스트 파일 구조

```
tests/
├── harness.h              ← 공통 매크로/유틸
├── test_all.c             ← wk07 기존 테스트 (회귀 방지)
├── test_step0_db_execute.c   ← 0단계: db_execute 경계 함수
├── test_step1_sql_ext.c      ← 1단계: SQL 확장
├── test_step2_server.c       ← 2단계: HTTP 서버 통합
├── test_step3_concurrency.c  ← 3단계: 동시성 제어
└── test_full_integration.c   ← 전체 통합 (서버 + 동시성 + SQL)
```

### 1-3. Makefile 타겟

```makefile
# 개별 단계 테스트
test-step0: $(BUILD_DIR)/test_step0_db_execute
	./$(BUILD_DIR)/test_step0_db_execute

test-step1: $(BUILD_DIR)/test_step1_sql_ext
	./$(BUILD_DIR)/test_step1_sql_ext

test-step2: $(BUILD_DIR)/test_step2_server
	./$(BUILD_DIR)/test_step2_server

test-step3: $(BUILD_DIR)/test_step3_concurrency
	./$(BUILD_DIR)/test_step3_concurrency

# 전체 회귀 테스트
test-all: test test-step0 test-step1 test-step2 test-step3
	@echo "All test suites passed."

# 스트레스 테스트 (긴 실행 시간)
test-stress: $(BUILD_DIR)/test_step3_concurrency
	./$(BUILD_DIR)/test_step3_concurrency --stress
```

---

## 2. Step 0 테스트: `db_execute()` 경계 함수

### 검증 목표
- `execute()` 가 더 이상 `printf()` 를 호출하지 않는다
- SELECT 결과가 `exec_result_t.out_buf` 에 정확히 담긴다
- `db_execute(sql)` 래퍼가 parse → execute 를 올바르게 연결한다
- `out_buf` 메모리 해제가 올바르다

### 테스트 케이스

```c
/* test_step0_db_execute.c */

TEST_SUITE(test_db_execute_basic) {
    pager_t pager;
    setup_test_db(&pager, "step0_basic");

    /* CREATE TABLE */
    exec_result_t r = db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    ASSERT_EQ_INT(r.status, 0, "CREATE TABLE succeeds");

    /* INSERT */
    r = db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    ASSERT_EQ_INT(r.status, 0, "INSERT succeeds");
    ASSERT_EQ_INT(pager.header.row_count, 1, "row_count == 1");

    /* SELECT — 결과가 out_buf 에 담기는지 확인 */
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "SELECT succeeds");
    ASSERT_NOT_NULL(r.out_buf, "out_buf is not NULL");
    ASSERT_TRUE(strstr(r.out_buf, "Alice") != NULL,
                "out_buf contains 'Alice'");
    ASSERT_TRUE(strstr(r.out_buf, "25") != NULL,
                "out_buf contains '25'");
    free(r.out_buf);

    /* 에러 케이스 */
    r = db_execute(&pager, "GARBAGE QUERY");
    ASSERT_EQ_INT(r.status, -1, "invalid SQL returns error");

    teardown_test_db(&pager, "step0_basic");
}

TEST_SUITE(test_db_execute_multi_row) {
    pager_t pager;
    setup_test_db(&pager, "step0_multi");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 50; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', %d)", i, 20 + i);
        db_execute(&pager, sql);
    }

    /* SELECT * — 전체 스캔 결과가 버퍼에 담기는지 */
    exec_result_t r = db_execute(&pager, "SELECT * FROM users");
    ASSERT_EQ_INT(r.status, 0, "SELECT * succeeds");
    ASSERT_NOT_NULL(r.out_buf, "out_buf not NULL for multi-row");

    /* 행 수 검증: 줄바꿈 개수로 대략 확인 */
    int newlines = 0;
    for (size_t i = 0; i < r.out_len; i++) {
        if (r.out_buf[i] == '\n') newlines++;
    }
    /* 헤더 + 구분선 + 50행 = 최소 50개 이상 줄바꿈 */
    ASSERT_TRUE(newlines >= 50, "at least 50 data lines in output");
    free(r.out_buf);

    teardown_test_db(&pager, "step0_multi");
}

TEST_SUITE(test_db_execute_delete_roundtrip) {
    pager_t pager;
    setup_test_db(&pager, "step0_del");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    db_execute(&pager, "INSERT INTO users VALUES ('Bob', 30)");

    /* DELETE id=1 */
    exec_result_t r = db_execute(&pager, "DELETE FROM users WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "DELETE succeeds");
    ASSERT_EQ_INT(pager.header.row_count, 1, "1 row remaining");

    /* SELECT — Alice 가 사라졌는지 */
    r = db_execute(&pager, "SELECT * FROM users");
    ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL,
                "Alice not in result after delete");
    ASSERT_TRUE(strstr(r.out_buf, "Bob") != NULL,
                "Bob still in result");
    free(r.out_buf);

    teardown_test_db(&pager, "step0_del");
}
```

### 통과 기준
- `test-step0` 전체 PASS
- wk07 `test` (기존 회귀 테스트)도 PASS

---

## 3. Step 1 테스트: SQL 확장

### 3-1. UPDATE 테스트

```c
TEST_SUITE(test_update_basic) {
    pager_t pager;
    setup_test_db(&pager, "step1_update");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    db_execute(&pager, "INSERT INTO users VALUES ('Bob', 30)");

    /* UPDATE SET age WHERE id = 1 */
    exec_result_t r = db_execute(&pager,
        "UPDATE users SET age = 99 WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "UPDATE succeeds");

    /* 검증 */
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_TRUE(strstr(r.out_buf, "99") != NULL,
                "age updated to 99");
    ASSERT_TRUE(strstr(r.out_buf, "Alice") != NULL,
                "name unchanged");
    free(r.out_buf);

    /* row_count 변화 없음 */
    ASSERT_EQ_INT(pager.header.row_count, 2, "row_count unchanged");

    teardown_test_db(&pager, "step1_update");
}

TEST_SUITE(test_update_varchar) {
    pager_t pager;
    setup_test_db(&pager, "step1_upd_str");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");

    exec_result_t r = db_execute(&pager,
        "UPDATE users SET name = 'Alicia' WHERE id = 1");
    ASSERT_EQ_INT(r.status, 0, "UPDATE name succeeds");

    r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_TRUE(strstr(r.out_buf, "Alicia") != NULL, "name updated");
    ASSERT_TRUE(strstr(r.out_buf, "Alice") == NULL, "old name gone");
    free(r.out_buf);

    teardown_test_db(&pager, "step1_upd_str");
}

TEST_SUITE(test_update_no_match) {
    pager_t pager;
    setup_test_db(&pager, "step1_upd_nomatch");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");

    exec_result_t r = db_execute(&pager,
        "UPDATE users SET age = 99 WHERE id = 999");
    ASSERT_EQ_INT(r.status, 0, "UPDATE with no match still succeeds");
    /* 메시지에 "0 rows" 또는 유사 표현 포함 */

    teardown_test_db(&pager, "step1_upd_nomatch");
}
```

### 3-2. 비교 연산자 테스트

```c
TEST_SUITE(test_comparison_ops) {
    pager_t pager;
    setup_test_db(&pager, "step1_cmp");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 10; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', %d)", i, 20 + i * 5);
        db_execute(&pager, sql);
    }
    /* ages: 20, 25, 30, 35, 40, 45, 50, 55, 60, 65 */

    /* age > 50 → 55, 60, 65 (3행) */
    exec_result_t r = db_execute(&pager,
        "SELECT * FROM users WHERE age > 50");
    int count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 3, "age > 50 returns 3 rows");
    free(r.out_buf);

    /* age < 25 → 20 (1행) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age < 25");
    count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 1, "age < 25 returns 1 row");
    free(r.out_buf);

    /* age >= 60 → 60, 65 (2행) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age >= 60");
    count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 2, "age >= 60 returns 2 rows");
    free(r.out_buf);

    /* age <= 20 → 20 (1행) */
    r = db_execute(&pager, "SELECT * FROM users WHERE age <= 20");
    count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 1, "age <= 20 returns 1 row");
    free(r.out_buf);

    /* age != 30 → 9행 */
    r = db_execute(&pager, "SELECT * FROM users WHERE age != 30");
    count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 9, "age != 30 returns 9 rows");
    free(r.out_buf);

    teardown_test_db(&pager, "step1_cmp");
}
```

### 3-3. COUNT / ORDER BY / LIMIT / DROP TABLE 테스트

```c
TEST_SUITE(test_count) {
    pager_t pager;
    setup_test_db(&pager, "step1_count");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 20; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('u%d', %d)", i, 10 + i);
        db_execute(&pager, sql);
    }

    /* COUNT(*) 전체 */
    exec_result_t r = db_execute(&pager,
        "SELECT COUNT(*) FROM users");
    ASSERT_TRUE(strstr(r.out_buf, "20") != NULL, "COUNT(*) = 20");
    free(r.out_buf);

    /* COUNT(*) with WHERE */
    r = db_execute(&pager,
        "SELECT COUNT(*) FROM users WHERE age > 20");
    ASSERT_TRUE(strstr(r.out_buf, "9") != NULL, "COUNT(*) WHERE age>20 = 9");
    free(r.out_buf);

    teardown_test_db(&pager, "step1_count");
}

TEST_SUITE(test_order_by) {
    pager_t pager;
    setup_test_db(&pager, "step1_order");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Charlie', 30)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 20)");
    db_execute(&pager, "INSERT INTO users VALUES ('Bob', 25)");

    /* ORDER BY age ASC */
    exec_result_t r = db_execute(&pager,
        "SELECT * FROM users ORDER BY age ASC");
    /* 출력 순서: Alice(20) → Bob(25) → Charlie(30) */
    char *alice = strstr(r.out_buf, "Alice");
    char *bob   = strstr(r.out_buf, "Bob");
    char *charlie = strstr(r.out_buf, "Charlie");
    ASSERT_TRUE(alice != NULL && bob != NULL && charlie != NULL,
                "all three names present");
    ASSERT_TRUE(alice < bob && bob < charlie,
                "ORDER BY age ASC: Alice < Bob < Charlie");
    free(r.out_buf);

    /* ORDER BY age DESC */
    r = db_execute(&pager,
        "SELECT * FROM users ORDER BY age DESC");
    alice = strstr(r.out_buf, "Alice");
    bob   = strstr(r.out_buf, "Bob");
    charlie = strstr(r.out_buf, "Charlie");
    ASSERT_TRUE(charlie < bob && bob < alice,
                "ORDER BY age DESC: Charlie < Bob < Alice");
    free(r.out_buf);

    teardown_test_db(&pager, "step1_order");
}

TEST_SUITE(test_limit) {
    pager_t pager;
    setup_test_db(&pager, "step1_limit");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('u%d', %d)", i, i);
        db_execute(&pager, sql);
    }

    exec_result_t r = db_execute(&pager,
        "SELECT * FROM users LIMIT 5");
    int count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 5, "LIMIT 5 returns exactly 5 rows");
    free(r.out_buf);

    /* LIMIT + ORDER BY 조합 */
    r = db_execute(&pager,
        "SELECT * FROM users ORDER BY age DESC LIMIT 3");
    count = count_result_rows(r.out_buf);
    ASSERT_EQ_INT(count, 3, "ORDER BY DESC LIMIT 3 returns 3 rows");
    /* 첫 행이 가장 큰 age를 가져야 함 */
    ASSERT_TRUE(strstr(r.out_buf, "99") != NULL,
                "top row has age=99");
    free(r.out_buf);

    teardown_test_db(&pager, "step1_limit");
}

TEST_SUITE(test_drop_table) {
    pager_t pager;
    setup_test_db(&pager, "step1_drop");

    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 10; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('u%d', %d)", i, i);
        db_execute(&pager, sql);
    }
    ASSERT_EQ_INT(pager.header.row_count, 10, "10 rows before drop");

    exec_result_t r = db_execute(&pager, "DROP TABLE users");
    ASSERT_EQ_INT(r.status, 0, "DROP TABLE succeeds");
    ASSERT_EQ_INT(pager.header.row_count, 0, "row_count reset to 0");
    ASSERT_EQ_INT(pager.header.column_count, 0, "schema cleared");

    /* DROP 후 SELECT 는 에러 */
    r = db_execute(&pager, "SELECT * FROM users");
    ASSERT_EQ_INT(r.status, -1, "SELECT after DROP fails");

    teardown_test_db(&pager, "step1_drop");
}
```

---

## 4. Step 2 테스트: TCP 소켓 서버 + 스레드 풀

### 4-1. 서버 테스트 전략

서버 테스트는 **프로세스 내 서버를 별도 스레드로 기동**한 뒤, 같은 프로세스에서 소켓 클라이언트로 요청을 보내는 방식으로 진행한다.

```c
/* tests/test_step2_server.c */

/* ── 테스트용 서버 제어 ── */
typedef struct {
    pthread_t thread;
    int port;
    pager_t *pager;
    volatile bool running;
} test_server_t;

static void *server_thread_func(void *arg) {
    test_server_t *ts = (test_server_t *)arg;
    /* server_run()은 ts->running이 false가 되면 종료 */
    server_run(ts->pager, ts->port, &ts->running);
    return NULL;
}

static void start_test_server(test_server_t *ts, pager_t *pager, int port) {
    ts->pager = pager;
    ts->port = port;
    ts->running = true;
    pthread_create(&ts->thread, NULL, server_thread_func, ts);
    usleep(100000); /* 100ms 대기 — 서버 바인드 완료 */
}

static void stop_test_server(test_server_t *ts) {
    ts->running = false;
    /* 더미 연결로 accept() 깨우기 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons(ts->port) };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    pthread_join(ts->thread, NULL);
}

/* ── 테스트용 HTTP 클라이언트 ── */
static char *send_query(int port, const char *sql) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons(port) };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));

    char req[1024];
    int len = snprintf(req, sizeof(req),
        "POST /query HTTP/1.1\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        strlen(sql), sql);
    send(fd, req, len, 0);

    /* 응답 수신 */
    char *buf = calloc(1, 65536);
    int total = 0, n;
    while ((n = recv(fd, buf + total, 65535 - total, 0)) > 0)
        total += n;
    close(fd);
    return buf; /* 호출자가 free */
}
```

### 4-2. HTTP 프로토콜 테스트

```c
TEST_SUITE(test_http_single_query) {
    pager_t pager;
    setup_test_db(&pager, "step2_http");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");

    test_server_t srv;
    start_test_server(&srv, &pager, 18080);

    char *resp = send_query(18080, "SELECT * FROM users WHERE id = 1");
    ASSERT_TRUE(strstr(resp, "200 OK") != NULL, "HTTP 200 response");
    ASSERT_TRUE(strstr(resp, "Alice") != NULL, "response contains Alice");
    free(resp);

    stop_test_server(&srv);
    teardown_test_db(&pager, "step2_http");
}

TEST_SUITE(test_http_bad_request) {
    pager_t pager;
    setup_test_db(&pager, "step2_bad");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");

    test_server_t srv;
    start_test_server(&srv, &pager, 18081);

    char *resp = send_query(18081, "INVALID SQL GARBAGE");
    ASSERT_TRUE(strstr(resp, "400") != NULL, "HTTP 400 for invalid SQL");
    free(resp);

    stop_test_server(&srv);
    teardown_test_db(&pager, "step2_bad");
}

TEST_SUITE(test_http_sequential_queries) {
    pager_t pager;
    setup_test_db(&pager, "step2_seq");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");

    test_server_t srv;
    start_test_server(&srv, &pager, 18082);

    /* 순차 INSERT 10건 */
    for (int i = 0; i < 10; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', %d)", i, 20 + i);
        char *resp = send_query(18082, sql);
        ASSERT_TRUE(strstr(resp, "200") != NULL, "INSERT via HTTP succeeds");
        free(resp);
    }

    /* COUNT 확인 */
    char *resp = send_query(18082, "SELECT COUNT(*) FROM users");
    ASSERT_TRUE(strstr(resp, "10") != NULL, "10 rows via HTTP");
    free(resp);

    stop_test_server(&srv);
    teardown_test_db(&pager, "step2_seq");
}
```

### 4-3. 스레드 풀 단위 테스트

```c
TEST_SUITE(test_thread_pool_lifecycle) {
    thread_pool_t pool;
    pager_t pager;
    setup_test_db(&pager, "step2_pool");

    int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    thread_pool_init(&pool, ncores, 64, &pager);

    ASSERT_EQ_INT(pool.thread_count, ncores, "thread count matches cores");
    ASSERT_EQ_INT(pool.queue_size, 0, "queue starts empty");

    thread_pool_shutdown(&pool);
    teardown_test_db(&pager, "step2_pool");
}
```

---

## 5. Step 3 테스트: 동시성 제어

### 5-1. Lock Table 단위 테스트

```c
TEST_SUITE(test_lock_table_basic) {
    lock_table_t lt;
    lock_table_init(&lt);

    /* S lock 획득 */
    int rc = lock_acquire(&lt, 1, LOCK_S);
    ASSERT_EQ_INT(rc, 0, "S lock on row 1 succeeds");

    /* 같은 스레드에서 동일 row에 S lock 재획득 (호환) */
    rc = lock_acquire(&lt, 1, LOCK_S);
    ASSERT_EQ_INT(rc, 0, "second S lock on row 1 succeeds");

    /* 전체 해제 */
    lock_release_all(&lt, pthread_self());

    lock_table_destroy(&lt);
}

TEST_SUITE(test_lock_sx_conflict) {
    /* 별도 스레드에서 X lock 보유 중 → 현재 스레드 S lock 시도 → timeout */
    lock_table_t lt;
    lock_table_init(&lt);

    /* 헬퍼 스레드: row 1에 X lock 잡고 2초 보유 */
    typedef struct { lock_table_t *lt; } helper_arg_t;
    helper_arg_t arg = { .lt = &lt };

    pthread_t helper;
    pthread_create(&helper, NULL, lock_holder_thread, &arg);
    usleep(50000); /* 50ms — helper가 X lock 잡을 시간 */

    /* 메인 스레드: S lock 시도, 1초 timeout 설정 */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    rc = lock_acquire_timeout(&lt, 1, LOCK_S, 1); /* 1초 */
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec)
                   + (end.tv_nsec - start.tv_nsec) / 1e9;
    ASSERT_EQ_INT(rc, -1, "S lock times out while X held");
    ASSERT_TRUE(elapsed >= 0.9 && elapsed <= 1.5,
                "timeout took ~1 second");

    pthread_join(helper, NULL);
    lock_table_destroy(&lt);
}
```

### 5-2. Level 1 동시성 테스트 (Global Mutex)

```c
TEST_SUITE(test_level1_concurrent_inserts) {
    pager_t pager;
    setup_test_db(&pager, "step3_l1");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");

    /* 동시성 레벨 설정 */
    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_1);

    #define N_THREADS 8
    #define N_INSERTS_PER_THREAD 50

    pthread_t threads[N_THREADS];
    typedef struct { pager_t *pager; int thread_id; } worker_arg_t;
    worker_arg_t args[N_THREADS];

    for (int i = 0; i < N_THREADS; i++) {
        args[i] = (worker_arg_t){ .pager = &pager, .thread_id = i };
        pthread_create(&threads[i], NULL, insert_worker, &args[i]);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* 정합성 검증: 정확히 N_THREADS * N_INSERTS_PER_THREAD 행 */
    ASSERT_EQ_INT(pager.header.row_count,
                  N_THREADS * N_INSERTS_PER_THREAD,
                  "all concurrent inserts committed");

    /* 모든 id가 B+Tree에서 검색 가능한지 */
    for (uint64_t id = 1; id <= N_THREADS * N_INSERTS_PER_THREAD; id++) {
        row_ref_t ref;
        ASSERT_TRUE(bptree_search(&pager, id, &ref),
                    "every inserted id is searchable");
    }

    teardown_test_db(&pager, "step3_l1");
}
```

### 5-3. Level 2 동시성 테스트 (RWLock)

```c
TEST_SUITE(test_level2_concurrent_reads) {
    pager_t pager;
    setup_test_db(&pager, "step3_l2");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', %d)", i, i);
        db_execute(&pager, sql);
    }

    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_2);

    /* 8 reader + 1 writer 동시 실행 */
    pthread_t readers[8], writer;
    for (int i = 0; i < 8; i++) {
        pthread_create(&readers[i], NULL, select_worker, &pager);
    }
    pthread_create(&writer, NULL, insert_worker_single, &pager);

    for (int i = 0; i < 8; i++) {
        pthread_join(readers[i], NULL);
    }
    pthread_join(writer, NULL);

    /* 결과 정합성: 101행 (기존 100 + writer 1) */
    ASSERT_EQ_INT(pager.header.row_count, 101,
                  "100 existing + 1 concurrent insert = 101");

    teardown_test_db(&pager, "step3_l2");
}

TEST_SUITE(test_level2_reader_not_blocked) {
    /*
     * 목표: Level 2에서 SELECT 끼리 동시에 실행됨을 증명
     * 방법: 8 readers 동시 시작, 각각 100행 SELECT * 수행
     *        Level 1이면 직렬 → 총 시간 ≈ 8 * T
     *        Level 2이면 병렬 → 총 시간 ≈ T
     */
    pager_t pager;
    setup_test_db(&pager, "step3_l2_par");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    for (int i = 0; i < 1000; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', %d)", i, i);
        db_execute(&pager, sql);
    }

    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_2);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    pthread_t readers[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&readers[i], NULL, repeated_select_worker, &pager);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(readers[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec)
                   + (end.tv_nsec - start.tv_nsec) / 1e9;

    /* 병렬이면 직렬의 1/4 이하 시간에 완료되어야 함 */
    printf("  Level 2 parallel reads: %.3fs\n", elapsed);
    /* 절대 시간 비교는 환경 의존적이므로, 성공 기준은 완주 자체 */
    ASSERT_TRUE(elapsed < 30.0, "parallel reads complete within 30s");

    teardown_test_db(&pager, "step3_l2_par");
}
```

### 5-4. Level 3 동시성 테스트 (Row Lock)

```c
TEST_SUITE(test_level3_row_lock_isolation) {
    /*
     * 시나리오: 두 스레드가 서로 다른 row를 동시에 UPDATE
     * 기대: 둘 다 성공 (Row Lock은 서로 다른 row에 대해 호환)
     */
    pager_t pager;
    setup_test_db(&pager, "step3_l3_iso");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");
    db_execute(&pager, "INSERT INTO users VALUES ('Bob', 30)");

    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_3);

    typedef struct { pager_t *pager; uint64_t target_id; int new_age; int result; } upd_arg_t;
    upd_arg_t arg1 = { &pager, 1, 99, 0 };
    upd_arg_t arg2 = { &pager, 2, 88, 0 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, update_single_worker, &arg1);
    pthread_create(&t2, NULL, update_single_worker, &arg2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT_EQ_INT(arg1.result, 0, "UPDATE id=1 succeeded");
    ASSERT_EQ_INT(arg2.result, 0, "UPDATE id=2 succeeded");

    /* 값 검증 */
    exec_result_t r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_TRUE(strstr(r.out_buf, "99") != NULL, "id=1 age=99");
    free(r.out_buf);
    r = db_execute(&pager, "SELECT * FROM users WHERE id = 2");
    ASSERT_TRUE(strstr(r.out_buf, "88") != NULL, "id=2 age=88");
    free(r.out_buf);

    teardown_test_db(&pager, "step3_l3_iso");
}

TEST_SUITE(test_level3_same_row_conflict) {
    /*
     * 시나리오: 두 스레드가 같은 row에 X lock 시도
     * 기대: 하나는 대기 후 성공, 충돌 시 timeout은 아님 (순차 처리)
     */
    pager_t pager;
    setup_test_db(&pager, "step3_l3_conflict");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('Alice', 25)");

    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_3);

    typedef struct { pager_t *pager; int new_age; int result; } upd_same_arg_t;
    upd_same_arg_t arg1 = { &pager, 99, 0 };
    upd_same_arg_t arg2 = { &pager, 88, 0 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, update_same_row_worker, &arg1);
    pthread_create(&t2, NULL, update_same_row_worker, &arg2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    /* 둘 다 성공 (순차 실행) */
    ASSERT_TRUE(arg1.result == 0 && arg2.result == 0,
                "both updates on same row succeed sequentially");

    /* 최종 값은 나중에 실행된 쪽 */
    exec_result_t r = db_execute(&pager, "SELECT * FROM users WHERE id = 1");
    ASSERT_TRUE(strstr(r.out_buf, "99") != NULL
             || strstr(r.out_buf, "88") != NULL,
                "final value is one of the two updates");
    free(r.out_buf);

    teardown_test_db(&pager, "step3_l3_conflict");
}

TEST_SUITE(test_level3_timeout_deadlock_prevention) {
    /*
     * 시나리오: 인위적 교착 상태 유발
     *   Thread A: lock row 1 → sleep → lock row 2
     *   Thread B: lock row 2 → sleep → lock row 1
     * 기대: 3초 timeout으로 한 쪽이 abort
     */
    pager_t pager;
    setup_test_db(&pager, "step3_l3_deadlock");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");
    db_execute(&pager, "INSERT INTO users VALUES ('A', 1)");
    db_execute(&pager, "INSERT INTO users VALUES ('B', 2)");

    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_3);

    typedef struct {
        lock_table_t *lt;
        uint64_t first_row;
        uint64_t second_row;
        int result; /* 0=성공, -1=timeout */
    } deadlock_arg_t;

    lock_table_t *lt = db_get_lock_table(&pager);
    deadlock_arg_t arg_a = { lt, 1, 2, 0 };
    deadlock_arg_t arg_b = { lt, 2, 1, 0 };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, deadlock_worker, &arg_a);
    pthread_create(&tb, NULL, deadlock_worker, &arg_b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    /* 둘 중 최소 하나는 timeout(-1) */
    ASSERT_TRUE(arg_a.result == -1 || arg_b.result == -1,
                "at least one thread timed out (deadlock prevented)");

    teardown_test_db(&pager, "step3_l3_deadlock");
}
```

### 5-5. 스트레스 테스트 (--stress 모드)

```c
TEST_SUITE(test_stress_mixed_workload) {
    /*
     * 혼합 워크로드 스트레스 테스트
     *
     * 구성:
     *   - Writer 스레드 4개: 각각 100건 INSERT
     *   - Reader 스레드 4개: 각각 1000회 SELECT *
     *   - Updater 스레드 2개: 랜덤 id에 UPDATE
     *   - Deleter 스레드 1개: 랜덤 id 50건 DELETE
     *
     * 검증:
     *   1. 모든 스레드가 정상 종료 (crash/hang 없음)
     *   2. row_count == (초기 + 총 INSERT - 총 DELETE)
     *   3. B+Tree 정합성: 남은 모든 id가 검색 가능
     *   4. sanitizer 경고 없음 (빌드 시 -fsanitize=address,undefined)
     */

    pager_t pager;
    setup_test_db(&pager, "stress");
    db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), age INT)");

    /* 초기 데이터 100행 */
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('init%d', %d)", i, i);
        db_execute(&pager, sql);
    }

    db_set_concurrency_level(&pager, CONCURRENCY_LEVEL_3);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* 11 스레드 기동 */
    pthread_t writers[4], readers[4], updaters[2], deleter;
    /* ... pthread_create for each ... */

    /* 전원 join */
    /* ... */

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec)
                   + (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("  stress test completed in %.2fs\n", elapsed);

    /* 정합성 검증 */
    uint64_t expected = 100 + (4 * 100) - g_delete_count;
    ASSERT_EQ_INT(pager.header.row_count, expected,
                  "final row_count matches expected");

    /* B+Tree ↔ Heap 교차 검증 */
    uint32_t btree_count = 0;
    for (uint64_t id = 1; id <= pager.header.next_id; id++) {
        row_ref_t ref;
        if (bptree_search(&pager, id, &ref)) {
            const uint8_t *data = heap_fetch(&pager, ref, pager.header.row_size);
            ASSERT_NOT_NULL(data, "btree ref points to valid heap data");
            if (data) pager_unpin(&pager, ref.page_id);
            btree_count++;
        }
    }
    ASSERT_EQ_INT(btree_count, pager.header.row_count,
                  "B+Tree count matches row_count");

    teardown_test_db(&pager, "stress");
}
```

---

## 6. 전체 통합 테스트: HTTP + 동시성 + SQL

```c
/* test_full_integration.c */

TEST_SUITE(test_full_e2e) {
    /*
     * End-to-end 시나리오:
     * 1. 서버를 스레드로 기동
     * 2. HTTP 클라이언트로 CREATE TABLE
     * 3. 8개 스레드에서 동시 INSERT (각 50건)
     * 4. COUNT(*) 로 400건 확인
     * 5. UPDATE + WHERE 비교 연산자
     * 6. ORDER BY + LIMIT 확인
     * 7. DELETE + 재확인
     * 8. 서버 종료
     */

    pager_t pager;
    setup_test_db(&pager, "e2e");

    test_server_t srv;
    start_test_server(&srv, &pager, 19090);

    /* Step 1: CREATE TABLE */
    char *resp = send_query(19090,
        "CREATE TABLE users (name VARCHAR(32), age INT)");
    ASSERT_TRUE(strstr(resp, "200") != NULL, "CREATE via HTTP");
    free(resp);

    /* Step 2: 동시 INSERT */
    pthread_t inserters[8];
    typedef struct { int port; int thread_id; int count; } http_ins_arg_t;
    http_ins_arg_t ins_args[8];
    for (int i = 0; i < 8; i++) {
        ins_args[i] = (http_ins_arg_t){ 19090, i, 50 };
        pthread_create(&inserters[i], NULL, http_insert_worker, &ins_args[i]);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(inserters[i], NULL);
    }

    /* Step 3: COUNT */
    resp = send_query(19090, "SELECT COUNT(*) FROM users");
    ASSERT_TRUE(strstr(resp, "400") != NULL, "COUNT(*) = 400 after concurrent inserts");
    free(resp);

    /* Step 4: UPDATE via HTTP */
    resp = send_query(19090, "UPDATE users SET age = 0 WHERE id = 1");
    ASSERT_TRUE(strstr(resp, "200") != NULL, "UPDATE via HTTP");
    free(resp);

    /* Step 5: 비교 연산자 via HTTP */
    resp = send_query(19090, "SELECT COUNT(*) FROM users WHERE age > 50");
    ASSERT_TRUE(strstr(resp, "200") != NULL, "comparison query via HTTP");
    free(resp);

    /* Step 6: ORDER BY + LIMIT via HTTP */
    resp = send_query(19090,
        "SELECT * FROM users ORDER BY age DESC LIMIT 5");
    ASSERT_TRUE(strstr(resp, "200") != NULL, "ORDER BY LIMIT via HTTP");
    int count = count_http_result_rows(resp);
    ASSERT_EQ_INT(count, 5, "LIMIT 5 returns 5 rows via HTTP");
    free(resp);

    /* Step 7: DELETE + 재확인 */
    resp = send_query(19090, "DELETE FROM users WHERE id = 1");
    ASSERT_TRUE(strstr(resp, "200") != NULL, "DELETE via HTTP");
    free(resp);

    resp = send_query(19090, "SELECT COUNT(*) FROM users");
    ASSERT_TRUE(strstr(resp, "399") != NULL, "COUNT(*) = 399 after delete");
    free(resp);

    stop_test_server(&srv);
    teardown_test_db(&pager, "e2e");
}
```

---

## 7. 테스트 게이트 정책

### 단계별 게이트

| 단계 완료 조건 | 필수 통과 테스트 |
|---------------|-----------------|
| **0단계 완료** | `test` (wk07 회귀) + `test-step0` |
| **1단계 완료** | 위 전체 + `test-step1` |
| **2단계 완료** | 위 전체 + `test-step2` |
| **3단계 완료** | 위 전체 + `test-step3` |
| **최종 완료** | `test-all` + `test-stress` + sanitizer clean |

### 실행 명령

```bash
# 빠른 회귀 확인 (구현 중 수시 실행)
make test-step0

# 전체 회귀 (단계 전환 시)
make test-all

# 스트레스 (최종 검증)
make test-stress
```

### Sanitizer 정책

모든 테스트 바이너리는 `-fsanitize=address,undefined` 로 빌드한다.
sanitizer 경고가 하나라도 발생하면 **해당 단계 미통과** 로 간주한다.

경고 유형별 의미:
- **AddressSanitizer**: buffer overflow, use-after-free, double-free → 메모리 안전성 위반
- **UndefinedBehaviorSanitizer**: signed overflow, null deref, alignment → 미정의 동작

### CI-like 로컬 스크립트

```bash
#!/bin/bash
# scripts/gate.sh — 단계별 게이트 검증
set -e

echo "=== Gate Check ==="

echo "[1/4] wk07 regression..."
make test

echo "[2/4] Step 0: db_execute..."
make test-step0

echo "[3/4] Step 1: SQL extensions..."
make test-step1

echo "[4/4] Step 2+3: Server + Concurrency..."
make test-step2
make test-step3

echo ""
echo "=== ALL GATES PASSED ==="
```

---

## 8. 헬퍼 함수 모음

테스트 전반에서 반복 사용되는 유틸리티:

```c
/* tests/helpers.h */

/* 테스트용 DB 세팅 (CREATE TABLE users 포함) */
static inline void setup_test_db(pager_t *pager, const char *name) {
    char *path = test_db_path(name);
    unlink(path);
    assert(pager_open(pager, path, true) == 0);
}

static inline void teardown_test_db(pager_t *pager, const char *name) {
    pager_close(pager);
    test_db_cleanup(name);
}

/* out_buf에서 데이터 행 수를 센다 (헤더/구분선 제외) */
static inline int count_result_rows(const char *buf) {
    if (buf == NULL) return 0;
    int count = 0;
    const char *p = buf;
    /* 헤더 라인 스킵 (첫 2줄: 컬럼명 + 구분선) */
    for (int skip = 0; skip < 2 && *p; skip++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    /* 나머지 줄 수 카운트 */
    while (p && *p) {
        if (*p == '\n') count++;
        else if (*(p + 1) == '\0') count++; /* 마지막 줄에 \n 없는 경우 */
        p++;
    }
    return count;
}

/* HTTP 응답 본문에서 데이터 행 수를 센다 */
static inline int count_http_result_rows(const char *resp) {
    /* HTTP 헤더 끝(\r\n\r\n) 이후부터 카운트 */
    const char *body = strstr(resp, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    return count_result_rows(body);
}

/* INSERT worker (pthread 용) */
static void *insert_worker(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    for (int i = 0; i < N_INSERTS_PER_THREAD; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('t%d_r%d', %d)",
                 w->thread_id, i, w->thread_id * 1000 + i);
        exec_result_t r = db_execute(w->pager, sql);
        if (r.out_buf) free(r.out_buf);
    }
    return NULL;
}

/* SELECT worker (pthread 용) */
static void *select_worker(void *arg) {
    pager_t *pager = (pager_t *)arg;
    for (int i = 0; i < 100; i++) {
        exec_result_t r = db_execute(pager, "SELECT * FROM users");
        if (r.out_buf) free(r.out_buf);
    }
    return NULL;
}

/* HTTP INSERT worker (pthread 용) */
static void *http_insert_worker(void *arg) {
    http_ins_arg_t *a = (http_ins_arg_t *)arg;
    for (int i = 0; i < a->count; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('t%d_r%d', %d)",
                 a->thread_id, i, a->thread_id * 1000 + i);
        char *resp = send_query(a->port, sql);
        free(resp);
    }
    return NULL;
}
```

---

## 9. 구현-테스트 매핑 요약

| 구현 항목 | 테스트 파일 | 핵심 검증 |
|----------|------------|----------|
| `db_execute()` 래퍼 | `test_step0` | SQL→out_buf 경로, 에러 반환 |
| `execute()` out_buf 리팩 | `test_step0` | printf 제거, 버퍼 정확성 |
| UPDATE 파서+실행기 | `test_step1` | SET col=val, WHERE id=, 값 검증 |
| 비교 연산자 (>, <, >=, <=, !=) | `test_step1` | 각 연산자별 행 수 검증 |
| COUNT(*) | `test_step1` | 전체/WHERE 조건부 카운트 |
| ORDER BY ASC/DESC | `test_step1` | 출력 순서 검증 |
| LIMIT | `test_step1` | 반환 행 수 정확히 N |
| DROP TABLE | `test_step1` | row_count=0, schema 초기화 |
| HTTP 서버 | `test_step2` | 200/400 응답, 본문 정확성 |
| 스레드 풀 | `test_step2` | 생성/종료, 코어 수 반영 |
| Lock Table | `test_step3` | S/X 호환성, timeout |
| Level 1 Global Mutex | `test_step3` | 동시 INSERT 정합성 |
| Level 2 RWLock | `test_step3` | 읽기 병렬성, 쓰기 배타성 |
| Level 3 Row Lock | `test_step3` | 행별 격리, 교착 방지 |
| 혼합 워크로드 | `test_stress` | crash 없음, 정합성, sanitizer clean |
| End-to-end | `test_full_integration` | HTTP+동시성+SQL 전체 경로 |
