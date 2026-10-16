/*
 * test_ack_queue_capi.c — Python 같은 다른 언어가 쓰는 평탄한 C 인터페이스(#140)의 경계 검사.
 *
 * ack_queue_capi.c를 직접 포함해 realloc 실패를 주입하고 static 인코더를 직접 검사한다. 보는 것:
 *   - 결과 인코딩 왕복과 출력 상한(상한 이하 통과, 상한 +1 거절, 부분 행 없음), 길이 u32 초과 거절
 *   - 모든 realloc 지점의 할당 실패가 NOMEM이고 출력이 없으며 다음 호출이 같은 결과를 준다
 *   - 다른 database의 트랜잭션을 lrndb_exec·lrnq_*가 실행 전에 거절한다
 *   - 살아 있는 큐·트랜잭션 핸들이 있으면 lrndb_close가 BUSY이고 핸들은 그대로다
 *   - 여러 스레드가 실행·인코딩을 번갈아 불러도 결과가 깨지지 않는다(잠금 구간이 결과 소비까지 이어진다)
 * 합성 데이터만 쓴다.
 */
#include <pthread.h>
#include <stdlib.h>

static long g_realloc_calls, g_realloc_fail_at;
static size_t g_realloc_largest;   /* 요청한 가장 큰 크기. 선언한 상한을 넘지 않는지 본다 */

static void *capi_test_realloc(void *pointer, size_t size)
{
    if (size > g_realloc_largest) g_realloc_largest = size;
    if (g_realloc_fail_at != 0 && ++g_realloc_calls == g_realloc_fail_at) return NULL;
    return realloc(pointer, size);
}

#define realloc capi_test_realloc
#include "../src/queue/ack_queue_capi.c"
#undef realloc

#include "ack_queue_test_util.h"

#define DIR "__test__ack_queue_capi"

static lrndb_t *open_capi_db(const char *name)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s.lrdb", g_dir, name);
    lrndb_t *db = NULL;
    int st = lrndb_open(path, 1, &db);
    assert(st == LRN_OK);
    return db;
}

/* 인코딩 버퍼를 사람이 읽는 한 줄로 바꿔 기대 문자열과 비교한다 */
typedef struct {
    const uint8_t *p;
    size_t n, at;
} reader_t;

static uint64_t rd(reader_t *r, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes && r->at < r->n; i++) v |= (uint64_t)r->p[r->at++] << (8 * i);
    return v;
}

static void render(const uint8_t *buf, size_t n, char *out, size_t cap)
{
    reader_t r = {buf, n, 0};
    size_t len = 0;
#define EMIT(...) len += (size_t)snprintf(out + len, cap - len, __VA_ARGS__)
    out[0] = '\0';
    uint8_t kind = (uint8_t)rd(&r, 1);
    if (kind == 1) {
        uint32_t tag = (uint32_t)rd(&r, 4);
        EMIT("CMD %.*s ", (int)tag, (const char *)buf + r.at);
        r.at += tag;
        EMIT("%llu", (unsigned long long)rd(&r, 8));
    } else if (kind == 2) {
        uint32_t cls = (uint32_t)rd(&r, 4);
        EMIT("ERR %u", cls);
    } else {
        uint32_t cols = (uint32_t)rd(&r, 4);
        EMIT("ROWS[");
        for (uint32_t i = 0; i < cols; i++) {
            uint32_t l = (uint32_t)rd(&r, 4);
            EMIT("%s%.*s", i ? "," : "", (int)l, (const char *)buf + r.at);
            r.at += l;
            /* 열 형식 번호와 NULL 가능(?) */
            uint8_t type = (uint8_t)rd(&r, 1);
            EMIT(":%u%s", type, rd(&r, 1) ? "?" : "");
        }
        EMIT("]");
        while (r.at < r.n && rd(&r, 1) == 1) {
            EMIT(" (");
            for (uint32_t i = 0; i < cols; i++) {
                uint8_t t = (uint8_t)rd(&r, 1);
                EMIT("%s", i ? "|" : "");
                if (t == 0) EMIT("NULL");
                else if (t == 1) EMIT("%lld", (long long)rd(&r, 8));
                else if (t == 2) {
                    uint64_t bits = rd(&r, 8);
                    double d;
                    memcpy(&d, &bits, 8);
                    EMIT("%g", d);
                } else {
                    uint32_t l = (uint32_t)rd(&r, 4);
                    EMIT("%s%u", t == 3 ? "t" : "b", l);
                    r.at += l;
                }
            }
            EMIT(")");
        }
    }
#undef EMIT
}

static int exec_str(lrndb_t *db, lrntxn_t *txn, const char *sql, const lrn_param_t *params, size_t n, char *out,
                    size_t cap)
{
    uint8_t *buf = NULL;
    size_t size = 0;
    int st = lrndb_exec(db, txn, sql, params, n, &buf, &size);
    if (st == LRN_OK) render(buf, size, out, cap);
    else out[0] = '\0';
    CHECK(st == LRN_OK ? buf != NULL : buf == NULL, "성공이 아니면 출력 버퍼가 없다");
    lrn_free(buf);
    return st;
}

static lrn_param_t p_int(int64_t v) { return (lrn_param_t){LRN_PARAM_INTEGER, v, 0, NULL, 0}; }
static lrn_param_t p_text(const char *s) { return (lrn_param_t){LRN_PARAM_TEXT, 0, 0, (const uint8_t *)s, strlen(s)}; }
static lrn_param_t p_blob(const void *b, size_t n) { return (lrn_param_t){LRN_PARAM_BLOB, 0, 0, b, n}; }
static lrn_param_t p_real(double d) { return (lrn_param_t){LRN_PARAM_REAL, 0, d, NULL, 0}; }
static lrn_param_t p_null(void) { return (lrn_param_t){LRN_PARAM_NULL, 0, 0, NULL, 0}; }

/* ── 1. DB 단독 사용과 인코딩 왕복 ── */

static void test_db_only_and_encoding(void)
{
    printf("\n[DB 단독 사용과 결과 인코딩 왕복]\n");
    make_db();
    lrndb_t *db = open_capi_db("main");
    char out[512];
    CHECK(exec_str(db, NULL, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, n INTEGER, s TEXT, b BLOB, r REAL)",
                   NULL, 0, out, sizeof(out)) == LRN_OK && strcmp(out, "CMD CREATE TABLE 0") == 0, "CREATE: %s", out);
    static const uint8_t blob[] = {0, 1, 2};
    lrn_param_t row1[] = {p_int(7), p_text("abc"), p_blob(blob, 3), p_real(1.5)};
    CHECK(exec_str(db, NULL, "INSERT INTO t (n, s, b, r) VALUES (?, ?, ?, ?)", row1, 4, out, sizeof(out)) == LRN_OK &&
              strcmp(out, "CMD INSERT 1") == 0, "INSERT 매개변수: %s", out);
    lrn_param_t row2[] = {p_null(), p_text(""), p_blob(NULL, 0), p_real(-0.5)};
    CHECK(exec_str(db, NULL, "INSERT INTO t (n, s, b, r) VALUES (?, ?, ?, ?)", row2, 4, out, sizeof(out)) == LRN_OK,
          "NULL·빈 값 INSERT");
    CHECK(exec_str(db, NULL, "SELECT id, n, s, b, r FROM t", NULL, 0, out, sizeof(out)) == LRN_OK &&
              strcmp(out, "ROWS[id:1,n:1?,s:3?,b:4?,r:2?] (1|7|t3|b3|1.5) (2|NULL|t0|b0|-0.5)") == 0, "SELECT 왕복: %s", out);
    CHECK(exec_str(db, NULL, "SELECT * FROM missing", NULL, 0, out, sizeof(out)) == LRN_OK && strncmp(out, "ERR ", 4) == 0,
          "문장 오류는 인코딩된 오류 결과: %s", out);
    lrn_param_t bad = {99, 0, 0, NULL, 0};
    uint8_t *buf = NULL;
    size_t size = 0;
    CHECK(lrndb_exec(db, NULL, "SELECT 1", &bad, 1, &buf, &size) == ACKQ_INVALID && buf == NULL, "잘못된 매개변수 type");
    CHECK(lrndb_exec(db, NULL, NULL, NULL, 0, &buf, &size) == ACKQ_INVALID, "SQL 없음");
    CHECK(lrndb_close(&db) == LRN_OK && db == NULL, "close");
}

/* ── 2. 출력 상한과 길이 한도 ── */

static void test_output_cap(void)
{
    printf("\n[출력 상한: 이하 통과, +1 거절, 부분 행 없음]\n");
    make_db();
    lrndb_t *db = open_capi_db("main");
    char out[2048];
    assert(exec_str(db, NULL, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, s TEXT)", NULL, 0, out, sizeof(out)) == LRN_OK);
    for (int i = 0; i < 20; i++) {
        lrn_param_t p = p_text("0123456789012345678901234567890123456789");
        assert(exec_str(db, NULL, "INSERT INTO t (s) VALUES (?)", &p, 1, out, sizeof(out)) == LRN_OK);
    }
    uint8_t *buf = NULL;
    size_t full = 0;
    assert(lrndb_exec(db, NULL, "SELECT id, s FROM t", NULL, 0, &buf, &full) == LRN_OK);
    lrn_free(buf);
    size_t original = lrn_max_result_bytes();
    CHECK(original == LRN_DEFAULT_MAX_RESULT_BYTES, "기본 상한");
    lrn_set_max_result_bytes(full);
    size_t got = 0;
    CHECK(lrndb_exec(db, NULL, "SELECT id, s FROM t", NULL, 0, &buf, &got) == LRN_OK && got == full, "상한과 같으면 통과");
    lrn_free(buf);
    for (size_t cap = full - 1; cap + 60 > full; cap--) {
        lrn_set_max_result_bytes(cap);
        buf = (uint8_t *)1;
        got = 99;
        CHECK(lrndb_exec(db, NULL, "SELECT id, s FROM t", NULL, 0, &buf, &got) == ACKQ_LIMIT && buf == NULL && got == 0,
              "상한 %zu(-%zu)는 부분 행 없이 LIMIT", cap, full - cap);
    }
    lrn_set_max_result_bytes(32);
    CHECK(lrndb_exec(db, NULL, "SELECT id, s FROM t", NULL, 0, &buf, &got) == ACKQ_LIMIT && buf == NULL, "작은 상한");
    CHECK(lrndb_exec(db, NULL, "SELECT id FROM t WHERE id = 1", NULL, 0, &buf, &got) == LRN_OK, "상한 안의 결과는 통과");
    lrn_free(buf);
    lrn_set_max_result_bytes(original);
    CHECK(lrndb_close(&db) == LRN_OK, "close");

    /* u32 길이 초과는 자르지 않고 거절한다(실제 4GiB를 만들지 않고 인코더를 직접 본다) */
    buffer_t b = {0};
    put_length_prefixed(&b, NULL, (size_t)UINT32_MAX + 1);
    CHECK(b.error == ACKQ_LIMIT && b.size == 0 && b.data == NULL, "u32를 넘는 길이는 LIMIT이고 아무것도 쓰지 않는다");
    buffer_t c = {0};
    lrn_set_max_result_bytes(10);
    put_bytes(&c, "0123456789", 10);
    CHECK(c.error == ACKQ_OK && c.size == 10, "상한과 같은 쓰기는 통과");
    put_bytes(&c, "x", 1);
    CHECK(c.error == ACKQ_LIMIT && c.size == 10, "상한을 넘는 쓰기는 거절하고 내용을 바꾸지 않는다");
    free(c.data);
    lrn_set_max_result_bytes(original);
    /* 아주 작은 상한(1~255)에서도 선언한 상한보다 크게 할당하지 않는다 */
    for (size_t cap = 1; cap <= 300; cap += (cap < 20 ? 1 : 37)) {
        lrn_set_max_result_bytes(cap);
        buffer_t e = {0};
        g_realloc_largest = 0;
        for (size_t i = 0; i < cap && e.error == ACKQ_OK; i++) put_bytes(&e, "z", 1);
        CHECK(e.error == ACKQ_OK && e.size == cap, "상한 %zu까지 채운다", cap);
        CHECK(g_realloc_largest <= cap, "상한 %zu: 요청한 최대 크기 %zu가 상한 이하", cap, g_realloc_largest);
        put_bytes(&e, "z", 1);
        CHECK(e.error == ACKQ_LIMIT && e.size == cap, "상한 %zu를 넘는 쓰기는 거절", cap);
        free(e.data);
    }
    lrn_set_max_result_bytes(original);
    buffer_t d = {0};
    put_bytes(&d, "a", 1);
    put_bytes(&d, "b", SIZE_MAX);
    CHECK(d.error == ACKQ_LIMIT && d.size == 1, "size_t 덧셈이 넘치는 입력도 거절");
    free(d.data);
}

/* ── 3. 할당 실패 ── */

static void test_allocation_failure(void)
{
    printf("\n[인코딩 버퍼 할당 실패]\n");
    make_db();
    lrndb_t *db = open_capi_db("main");
    char out[8192];
    assert(exec_str(db, NULL, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, s TEXT)", NULL, 0, out, sizeof(out)) == LRN_OK);
    for (int i = 0; i < 60; i++) {
        lrn_param_t p = p_text("payload-payload-payload-payload");
        assert(exec_str(db, NULL, "INSERT INTO t (s) VALUES (?)", &p, 1, out, sizeof(out)) == LRN_OK);
    }
    char reference[8192];
    g_realloc_fail_at = 1000000;   /* 실패시키지 않고 realloc 호출 수만 센다 */
    g_realloc_calls = 0;
    assert(exec_str(db, NULL, "SELECT id, s FROM t", NULL, 0, reference, sizeof(reference)) == LRN_OK);
    long calls = g_realloc_calls;
    g_realloc_fail_at = 0;
    CHECK(calls >= 3, "버퍼가 여러 번 자란다 (realloc %ld회)", calls);
    for (long k = 1; k <= calls; k++) {
        g_realloc_calls = 0;
        g_realloc_fail_at = k;
        uint8_t *buf = (uint8_t *)1;
        size_t size = 5;
        int st = lrndb_exec(db, NULL, "SELECT id, s FROM t", NULL, 0, &buf, &size);
        g_realloc_fail_at = 0;
        CHECK(st == ACKQ_NOMEM && buf == NULL && size == 0, "k=%ld: realloc 실패는 NOMEM이고 출력이 없다 (st=%d)", k, st);
        CHECK(exec_str(db, NULL, "SELECT id, s FROM t", NULL, 0, out, sizeof(out)) == LRN_OK && strcmp(out, reference) == 0,
              "k=%ld: 다음 호출은 같은 결과를 준다", k);
    }
    CHECK(lrndb_close(&db) == LRN_OK, "close");
}

/* ── 4. 다른 database의 트랜잭션 ── */

static void test_wrong_database_transaction(void)
{
    printf("\n[다른 database의 트랜잭션 거절]\n");
    make_db();
    lrndb_t *a = open_capi_db("a");
    lrndb_t *b = open_capi_db("b");
    lrnq_t *qa = NULL;
    assert(lrnq_open(a, "jobs", 1, 0, fake_clock, NULL, &qa) == LRN_OK);
    g_now = 5;
    uint64_t id = 0;
    int dup = 0;
    assert(lrnq_put(qa, NULL, (const uint8_t *)"x", 1, NULL, &id, &dup) == LRN_OK);
    lrnq_item_t item;
    assert(lrnq_get(qa, NULL, 0, 1000, &item) == LRN_OK);
    uint64_t fence = item.fence;
    lrnq_item_free(&item);

    char out[256];
    assert(exec_str(b, NULL, "CREATE TABLE ackq_jobs (id INTEGER PRIMARY KEY, marker TEXT NOT NULL)", NULL, 0, out, sizeof(out)) == LRN_OK);
    lrntxn_t *foreign = NULL;
    assert(lrndb_begin(b, &foreign) == LRN_OK);
    uint8_t *buf = NULL;
    size_t size = 0;
    CHECK(lrndb_exec(a, foreign, "DELETE FROM ackq_jobs", NULL, 0, &buf, &size) == ACKQ_INVALID && buf == NULL,
          "lrndb_exec는 다른 database의 트랜잭션을 거절한다");
    uint64_t counts[4], other = 7;
    size_t n = 0;
    lrnq_listitem_t list[1];
    CHECK(lrnq_put(qa, foreign, (const uint8_t *)"x", 1, NULL, &other, NULL) == ACKQ_INVALID, "put");
    CHECK(lrnq_get(qa, foreign, 0, 100, &item) == ACKQ_INVALID && item.payload == NULL, "get");
    CHECK(lrnq_ack(qa, foreign, id, fence) == ACKQ_INVALID && lrnq_fail(qa, foreign, id, fence, NULL) == ACKQ_INVALID, "ack·fail");
    CHECK(lrnq_nack(qa, foreign, id, fence, 0, NULL, NULL) == ACKQ_INVALID, "nack");
    CHECK(lrnq_update(qa, foreign, id, fence, (const uint8_t *)"y", 1) == ACKQ_INVALID, "update");
    CHECK(lrnq_recover(qa, foreign, 1, NULL) == ACKQ_INVALID && lrnq_release_leases(qa, foreign, 1, NULL) == ACKQ_INVALID &&
              lrnq_purge(qa, foreign, 0, 1, NULL) == ACKQ_INVALID, "recover·release·purge");
    CHECK(lrnq_stats(qa, foreign, counts) == ACKQ_INVALID && lrnq_list(qa, foreign, 0, 1, 0, list, &n) == ACKQ_INVALID && n == 0,
          "stats·list");
    CHECK(lrndb_txn_failed(foreign) == 0, "거절은 남의 트랜잭션을 실패시키지 않는다");
    CHECK(lrndb_commit(&foreign) == LRN_OK, "남의 트랜잭션은 그대로 commit된다");
    CHECK(exec_str(b, NULL, "SELECT id FROM ackq_jobs", NULL, 0, out, sizeof(out)) == LRN_OK && strcmp(out, "ROWS[id:1]") == 0,
          "다른 database의 테이블이 바뀌지 않았다: %s", out);
    CHECK(lrnq_stats(qa, NULL, counts) == LRN_OK && counts[1] == 1, "이 큐도 그대로다");
    CHECK(lrnq_ack(qa, NULL, id, fence) == LRN_OK, "올바른 호출은 된다");
    lrnq_close(&qa);
    CHECK(lrndb_close(&a) == LRN_OK && lrndb_close(&b) == LRN_OK, "close");
}

/* ── 5. 핸들 수명 ── */

static void test_handle_lifetime(void)
{
    printf("\n[살아 있는 큐·트랜잭션이 있으면 close는 BUSY]\n");
    make_db();
    lrndb_t *db = open_capi_db("main");
    lrnq_t *q = NULL;
    assert(lrnq_open(db, "jobs", 1, 0, fake_clock, NULL, &q) == LRN_OK);
    lrndb_t *keep = db;
    CHECK(lrndb_close(&db) == ACKQ_BUSY && db == keep, "큐가 있으면 BUSY이고 핸들은 그대로다");
    uint64_t id;
    g_now = 1;
    CHECK(lrnq_put(q, NULL, (const uint8_t *)"x", 1, NULL, &id, NULL) == LRN_OK, "BUSY 뒤에도 큐를 쓸 수 있다");
    lrntxn_t *txn = NULL;
    CHECK(lrndb_begin(db, &txn) == LRN_OK, "트랜잭션 시작");
    lrnq_close(&q);
    CHECK(lrndb_close(&db) == ACKQ_BUSY && db == keep, "트랜잭션이 있으면 BUSY");
    CHECK(lrndb_rollback(&txn) == LRN_OK && txn == NULL, "rollback");
    CHECK(lrndb_close(&db) == LRN_OK && db == NULL, "모두 닫으면 close");
    lrnq_close(&q);
    lrndb_close(&db);
}

/* ── 6. 여러 스레드가 실행과 인코딩을 번갈아 부른다 ── */

typedef struct {
    lrndb_t *db;
    int id;
    int errors;
} stress_t;

static void *stress_main(void *arg)
{
    stress_t *s = arg;
    for (int i = 0; i < 40; i++) {
        uint8_t *buf = NULL;
        size_t size = 0;
        lrn_param_t p = p_int(s->id * 1000 + i);
        if (lrndb_exec(s->db, NULL, "INSERT INTO t (n) VALUES (?)", &p, 1, &buf, &size) != LRN_OK) s->errors++;
        lrn_free(buf);
        buf = NULL;
        if (lrndb_exec(s->db, NULL, "SELECT id, n FROM t", NULL, 0, &buf, &size) != LRN_OK || size < 2) s->errors++;
        else {
            reader_t r = {buf, size, 0};
            if (rd(&r, 1) != 0) s->errors++;
        }
        lrn_free(buf);
    }
    return NULL;
}

static void test_threads(void)
{
    printf("\n[여러 스레드의 실행·인코딩]\n");
    make_db();
    lrndb_t *db = open_capi_db("main");
    char out[256];
    assert(exec_str(db, NULL, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, n INTEGER)", NULL, 0, out, sizeof(out)) == LRN_OK);
    stress_t workers[4];
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        workers[i] = (stress_t){db, i + 1, 0};
        pthread_create(&threads[i], NULL, stress_main, &workers[i]);
    }
    int errors = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
        errors += workers[i].errors;
    }
    CHECK(errors == 0, "스레드 오류 없음 (%d)", errors);
    uint8_t *buf = NULL;
    size_t size = 0;
    assert(lrndb_exec(db, NULL, "SELECT id FROM t", NULL, 0, &buf, &size) == LRN_OK);
    reader_t r = {buf, size, 0};
    rd(&r, 1);
    uint32_t cols = (uint32_t)rd(&r, 4);
    for (uint32_t i = 0; i < cols; i++) r.at += (size_t)rd(&r, 4) + 2;   /* 이름 뒤 형식·NULL 가능 */
    int rows = 0;
    while (r.at < r.n && rd(&r, 1) == 1) {
        rows++;
        rd(&r, 1);
        rd(&r, 8);
    }
    CHECK(rows == 160, "모든 INSERT가 반영됐다 (rows=%d)", rows);
    lrn_free(buf);
    CHECK(lrndb_close(&db) == LRN_OK, "close");
}

/* ── 7. 큐 왕복 ── */

static void test_queue_round_trip(void)
{
    printf("\n[CAPI 큐 왕복: put·get·ack·nack·update]\n");
    make_db();
    lrndb_t *db = open_capi_db("main");
    lrnq_t *q = NULL;
    CHECK(lrnq_open(db, "jobs", 0, 0, fake_clock, NULL, &q) == ACKQ_NOT_INSTALLED && q == NULL, "설치 전 open");
    assert(lrnq_open(db, "jobs", 1, 2, fake_clock, NULL, &q) == LRN_OK);
    g_now = 100;
    lrnq_item_t item;
    CHECK(lrnq_get(q, NULL, 0, 500, &item) == LRN_EMPTY, "비면 EMPTY");
    uint64_t id = 0, again = 0;
    int dup = 5;
    CHECK(lrnq_put(q, NULL, (const uint8_t *)"hello", 5, "k", &id, &dup) == LRN_OK && dup == 0, "put");
    CHECK(lrnq_put(q, NULL, (const uint8_t *)"hello", 5, "k", &again, &dup) == LRN_OK && dup == 1 && again == id, "중복 put");
    CHECK(lrnq_put(q, NULL, (const uint8_t *)"other", 5, "k", &again, &dup) == ACKQ_CONFLICT && again == 0, "충돌");
    CHECK(lrnq_get(q, NULL, 0, 500, &item) == LRN_OK && item.id == id && item.fence == 1 && item.payload_size == 5, "get");
    uint64_t fence = item.fence;
    lrnq_item_free(&item);
    CHECK(lrnq_update(q, NULL, id, fence, (const uint8_t *)"HELLO", 5) == LRN_OK, "update");
    int terminal = 9;
    CHECK(lrnq_nack(q, NULL, id, fence, 0, "retry", &terminal) == LRN_OK && terminal == 0, "nack");
    CHECK(lrnq_get(q, NULL, id, 500, &item) == LRN_OK && item.fence == 2 && memcmp(item.payload, "HELLO", 5) == 0, "id로 get");
    CHECK(lrnq_get(q, NULL, id, 500, &(lrnq_item_t){0}) == ACKQ_STALE, "claim 중인 id를 또 get하면 STALE");
    CHECK(lrnq_ack(q, NULL, id, 1) == ACKQ_STALE, "옛 fence ack는 STALE");
    CHECK(lrnq_ack(q, NULL, id, item.fence) == LRN_OK, "ack");
    lrnq_item_free(&item);
    uint64_t counts[4];
    CHECK(lrnq_stats(q, NULL, counts) == LRN_OK && counts[2] == 1 && counts[0] + counts[1] + counts[3] == 0, "집계");
    lrnq_listitem_t list[4];
    size_t n = 0;
    CHECK(lrnq_list(q, NULL, 0, 4, 1, list, &n) == LRN_OK && n == 1 && list[0].state == 2 && list[0].created_at_ms == 100 &&
              list[0].payload_size == 5 && list[0].note[0] == '\0', "list(ack가 이유를 비운다)");
    lrnq_list_free(list, n);
    size_t purged = 0;
    CHECK(lrnq_purge(q, NULL, 0, 10, &purged) == LRN_OK && purged == 1, "purge");
    lrnq_close(&q);
    CHECK(lrndb_close(&db) == LRN_OK, "close");
}

int main(void)
{
    set_dir(DIR);
    test_db_only_and_encoding();
    test_output_cap();
    test_allocation_failure();
    test_wrong_database_transaction();
    test_handle_lifetime();
    test_threads();
    test_queue_round_trip();
    printf("\ntest_ack_queue_capi: %d checks, %d failed\n", g_run, g_fail);
    if (g_fail == 0) remove_dir();
    return g_fail == 0 ? 0 : 1;
}
