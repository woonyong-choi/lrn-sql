/*
 * test_ack_queue.c — 선택형 ACK 큐(#140)의 의미를 결정적으로 검사한다. 시간은 g_now로 직접 주입하고 합성 데이터만 쓴다.
 * 공개 API(database, database_txn, ack_queue)로만 호출한다. 내구성 실패와 종료 주입은 test_ack_queue_fault.c가 맡는다.
 *
 * 빌드(저장소 루트): make test-ack-queue
 */
#include <pthread.h>

#include "ack_queue_test_util.h"

#define DIR "__test__ack_queue"

static ackq_claim_t g_items[ACKQ_MAX_CLAIM_BATCH];

static size_t claim_n(ackq_t *q, session_t *db, size_t n, uint64_t lease_ms)
{
    size_t count = 0;
    ackq_status_t st = ackq_claim(q, db, n, lease_ms, g_items, &count);
    CHECK(st == ACKQ_OK, "claim status %s", ackq_status_name(st));
    return count;
}

/* ── 1. 설치하지 않으면 DB만 쓴다 ── */

static void test_optional_install(void)
{
    printf("\n[선택 설치: 큐 없이 DB 단독 사용]\n");
    make_db();
    session_t *db = open_db();
    assert(db != NULL);
    CHECK(exec_ok(db, "CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT NOT NULL)"), "CRUD 준비");
    CHECK(exec_ok(db, "INSERT INTO notes (body) VALUES ('a')") && exec_ok(db, "UPDATE notes SET body = 'b' WHERE id = 1"),
          "DB 단독 INSERT·UPDATE");
    CHECK(scalar_int(db, "SELECT id FROM notes WHERE body = 'b'") == 1, "DB 단독 SELECT");
    size_t tables = database_table_count(raw_db(db));
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q = (ackq_t *)1;
    CHECK(ackq_open(db, &o, &q) == ACKQ_NOT_INSTALLED, "설치 전 open은 NOT_INSTALLED");
    CHECK(database_table_count(raw_db(db)) == tables, "열기가 테이블을 만들지 않는다");
    CHECK(exec_ok(db, "DELETE FROM notes WHERE id = 1"), "DB 단독 DELETE");
    close_db(db);

    db = open_db();
    CHECK(database_table_count(raw_db(db)) == tables, "재시작 뒤에도 큐 테이블이 없다");
    CHECK(ackq_install(db, &o, &q) == ACKQ_OK && q != NULL, "install");
    CHECK(database_table_count(raw_db(db)) == tables + 2, "설치가 큐 테이블과 key 보존 테이블을 만든다");
    ackq_close(&q);
    ackq_t *again = NULL;
    ackq_t *reopened = NULL;
    CHECK(ackq_install(db, &o, &again) == ACKQ_OK && ackq_open(db, &o, &reopened) == ACKQ_OK, "install·open은 멱등이다");
    CHECK(database_table_count(raw_db(db)) == tables + 2, "다시 설치해도 테이블은 그대로");
    ackq_close(&again);
    ackq_close(&reopened);
    ackq_close(&q);
    ackq_options_t bad = qopts("a-b", 0);
    CHECK(ackq_install(db, &bad, &q) == ACKQ_INVALID && q == NULL, "이름 문자 검증");
    bad = qopts("", 0);
    CHECK(ackq_open(db, &bad, &q) == ACKQ_INVALID, "빈 이름 거절");
    close_db(db);
}

/* ── 2. enqueue·bounded claim·FIFO ── */

static void test_enqueue_claim(void)
{
    printf("\n[enqueue와 bounded claim]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 1000;
    uint64_t ids[10];
    for (int i = 0; i < 10; i++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "job-%d", i);
        CHECK(enq(q, db, payload, NULL, &ids[i], NULL) == ACKQ_OK, "enqueue %d", i);
        CHECK(i == 0 || ids[i] > ids[i - 1], "id는 증가한다");
    }
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready == 10 && stats.leased == 0, "대기 10");

    size_t n = claim_n(q, db, 3, 500);
    CHECK(n == 3, "배치는 요청한 3개로 제한된다 (n=%zu)", n);
    for (size_t i = 0; i < n; i++) {
        CHECK(g_items[i].id == ids[i] && g_items[i].fence == 1 && g_items[i].attempts == 1, "FIFO id·fence 1·시도 1");
        CHECK(g_items[i].lease_until_ms == 1500, "lease는 now + lease_ms");
        char want[32];
        snprintf(want, sizeof(want), "job-%zu", i);
        CHECK(g_items[i].payload_size == strlen(want) && memcmp(g_items[i].payload, want, strlen(want)) == 0,
              "payload 왕복");
    }
    ackq_claims_release(g_items, n);
    n = claim_n(q, db, 100, 500);
    CHECK(n == 7 && g_items[0].id == ids[3], "남은 7개만 나온다 (n=%zu)", n);
    ackq_claims_release(g_items, n);
    CHECK(claim_n(q, db, 5, 500) == 0, "더 가져올 것이 없으면 0개");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready == 0 && stats.leased == 10, "처리 중 10");

    ackq_claim_t one[1];
    size_t count = 7;
    CHECK(ackq_claim(q, db, 0, 500, one, &count) == ACKQ_INVALID && count == 0, "max_items 0 거절");
    CHECK(ackq_claim(q, db, ACKQ_MAX_CLAIM_BATCH + 1, 500, one, &count) == ACKQ_INVALID, "배치 상한 거절");
    CHECK(ackq_claim(q, db, 1, 0, one, &count) == ACKQ_INVALID, "lease 0 거절");
    ackq_close(&q);
    close_db(db);
}

/* ── 3. payload 경계와 idempotency key ── */

static void test_payload_and_idempotency(void)
{
    printf("\n[payload 경계와 idempotency key]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 10;
    uint64_t id, other;
    bool dup = true;
    CHECK(ackq_enqueue(q, db, NULL, 0, NULL, &id, NULL) == ACKQ_OK, "빈 payload");
    CHECK(claim_n(q, db, 1, 100) == 1 && g_items[0].payload_size == 0 && g_items[0].payload == NULL, "빈 payload 왕복");
    ackq_claims_release(g_items, 1);

    uint8_t *max = malloc(ACKQ_DEFAULT_MAX_PAYLOAD_BYTES + 1);
    for (size_t i = 0; i <= ACKQ_DEFAULT_MAX_PAYLOAD_BYTES; i++) max[i] = (uint8_t)(i * 31u);
    CHECK(ackq_enqueue(q, db, max, ACKQ_DEFAULT_MAX_PAYLOAD_BYTES, NULL, &id, NULL) == ACKQ_OK, "기본 상한 크기 payload");
    CHECK(ackq_enqueue(q, db, max, ACKQ_DEFAULT_MAX_PAYLOAD_BYTES + 1, NULL, &other, NULL) == ACKQ_LIMIT,
          "기본 상한+1은 자르지 않고 거절");
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready + stats.leased == 2, "거절은 작업을 만들지 않는다");
    CHECK(claim_n(q, db, 1, 100) == 1 && g_items[0].payload_size == ACKQ_DEFAULT_MAX_PAYLOAD_BYTES &&
              memcmp(g_items[0].payload, max, ACKQ_DEFAULT_MAX_PAYLOAD_BYTES) == 0, "상한 payload 바이트 일치");
    ackq_claims_release(g_items, 1);
    free(max);
    static const uint8_t binary[] = {0x00, 0xff, 0x00, 'a'};
    CHECK(ackq_enqueue(q, db, binary, sizeof(binary), NULL, &id, NULL) == ACKQ_OK, "NUL 포함 BLOB");
    CHECK(claim_n(q, db, 1, 100) == 1 && g_items[0].payload_size == 4 && memcmp(g_items[0].payload, binary, 4) == 0,
          "내장 NUL 왕복");
    ackq_claims_release(g_items, 1);

    CHECK(enq(q, db, "same", "k1", &id, &dup) == ACKQ_OK && !dup, "key 첫 enqueue");
    CHECK(enq(q, db, "same", "k1", &other, &dup) == ACKQ_OK && dup && other == id, "같은 key·payload는 기존 작업");
    CHECK(enq(q, db, "different", "k1", &other, &dup) == ACKQ_CONFLICT, "같은 key·다른 payload는 CONFLICT");
    CHECK(enq(q, db, "same", "k2", &other, &dup) == ACKQ_OK && !dup && other != id, "다른 key는 새 작업");
    CHECK(enq(q, db, "same", NULL, &other, &dup) == ACKQ_OK && !dup, "key 없는 같은 payload는 새 작업");
    CHECK(enq(q, db, "x", "", &other, NULL) == ACKQ_INVALID, "빈 key 거절");
    char longkey[ACKQ_IDEM_KEY_MAX + 2];
    memset(longkey, 'k', sizeof(longkey));
    longkey[ACKQ_IDEM_KEY_MAX + 1] = '\0';
    CHECK(enq(q, db, "x", longkey, &other, NULL) == ACKQ_LIMIT, "긴 key 거절");
    CHECK(enq(q, db, "x", "\xff\xfe", &other, NULL) == ACKQ_INVALID, "잘못된 UTF-8 key 거절");

    /* 끝난 작업의 key도 중복으로 인식한다(재전송이 같은 작업을 다시 만들지 않는다) */
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK, "stats");
    size_t before = (size_t)(stats.ready + stats.leased + stats.acked + stats.failed);
    CHECK(claim_n(q, db, 100, 100) >= 1, "나머지 claim");
    uint64_t k1_fence = 0;
    for (size_t i = 0; i < 100; i++) {
        if (g_items[i].id == id) k1_fence = g_items[i].fence;
    }
    CHECK(ackq_ack(q, db, id, k1_fence) == ACKQ_OK, "key 작업 ack");
    ackq_claims_release(g_items, 100);
    CHECK(enq(q, db, "same", "k1", &other, &dup) == ACKQ_OK && dup && other == id, "ack된 작업의 key도 중복");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && (size_t)(stats.ready + stats.leased + stats.acked + stats.failed) == before,
          "중복 enqueue는 행을 늘리지 않는다");
    ackq_close(&q);
    close_db(db);
}

/* ── 4. 동시 claim: 겹치지 않고 빠짐없이 ── */

typedef struct {
    ackq_t *queue;
    size_t claimed;
    uint64_t seen[64];
    int errors;
    session_t *session;   /* 세션은 한 스레드만 쓴다. worker마다 따로 연다 */
} worker_t;

static void *claim_worker(void *arg)
{
    worker_t *w = arg;
    ackq_claim_t items[4];
    for (;;) {
        size_t count = 0;
        ackq_status_t st = ackq_claim(w->queue, w->session, 4, 100000, items, &count);
        if (st == ACKQ_BUSY) continue;
        if (st != ACKQ_OK) {
            w->errors++;
            return NULL;
        }
        if (count == 0) return NULL;
        for (size_t i = 0; i < count; i++) w->seen[w->claimed++] = items[i].id;
        ackq_claims_release(items, count);
    }
}

static void test_concurrent_claim(void)
{
    printf("\n[동시 claim]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    o.begin_timeout_ms = TXN_WAIT_FOREVER;
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 1;
    for (int i = 0; i < 40; i++) {
        uint64_t id;
        assert(enq(q, db, "p", NULL, &id, NULL) == ACKQ_OK);
    }
    /* 한 worker가 트랜잭션을 쥐고 있으면 다른 claim은 기다리지 않고 BUSY다 */
    ackq_options_t nowait = qopts("jobs", 0);
    ackq_t *q2;
    assert(ackq_open(db, &nowait, &q2) == ACKQ_OK);
    session_t *holder = NULL;
    assert((holder = begin_txn(db)) != NULL);
    size_t count = 9;
    CHECK(ackq_claim(q, holder, 2, 1000, g_items, &count) == ACKQ_OK && count == 2, "holder가 2개를 claim한다");
    uint64_t held[2] = {g_items[0].id, g_items[1].id};
    ackq_claims_release(g_items, count);
    ackq_claim_t one[1];
    CHECK(ackq_claim(q2, db, 1, 1000, one, &count) == ACKQ_BUSY && count == 0, "writer가 쥐고 있으면 BUSY");
    CHECK(rollback_txn(&holder) == TXN_OK, "holder rollback");
    CHECK(claim_n(q2, db, 2, 1000) == 2 && g_items[0].id == held[0] && g_items[1].id == held[1],
          "rollback한 claim은 사라져 같은 작업이 다시 나온다");
    ackq_claims_release(g_items, 2);

    worker_t workers[4];
    memset(workers, 0, sizeof(workers));
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        workers[i].queue = q;
        assert(session_open(session_database(db), &workers[i].session) == SESSION_OK);
        pthread_create(&threads[i], NULL, claim_worker, &workers[i]);
    }
    size_t total = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
        session_close(workers[i].session);
        total += workers[i].claimed;
        CHECK(workers[i].errors == 0, "worker %d 오류 없음", i);
    }
    CHECK(total == 38, "남은 38개를 빠짐없이 나눠 가졌다 (total=%zu)", total);
    bool seen[64] = {false};
    int duplicates = 0;
    for (int i = 0; i < 4; i++) {
        for (size_t j = 0; j < workers[i].claimed; j++) {
            uint64_t id = workers[i].seen[j];
            if (id >= 64 || seen[id]) duplicates++;
            else seen[id] = true;
        }
    }
    CHECK(duplicates == 0, "한 작업이 두 worker에게 가지 않는다 (중복 %d)", duplicates);
    ackq_close(&q2);
    ackq_close(&q);
    close_db(db);
}

/* ── 5. lease 만료와 fencing ── */

static void test_lease_and_fencing(void)
{
    printf("\n[lease 만료와 stale ACK 거절]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 1000;
    uint64_t id;
    assert(enq(q, db, "work", NULL, &id, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 100) == 1);
    uint64_t fence_a = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    CHECK(fence_a == 1, "첫 fence");

    g_now = 1099;
    CHECK(claim_n(q, db, 1, 100) == 0, "lease가 끝나기 전에는 다른 worker가 못 가진다");
    g_now = 1100;
    CHECK(claim_n(q, db, 1, 100) == 1, "만료 시각에 다른 worker가 가져간다");
    uint64_t fence_b = g_items[0].fence;
    CHECK(fence_b == 2 && g_items[0].attempts == 2 && g_items[0].id == id, "fence·시도 증가");
    ackq_claims_release(g_items, 1);

    CHECK(ackq_ack(q, db, id, fence_a) == ACKQ_STALE, "옛 worker의 ack는 STALE");
    CHECK(ackq_nack(q, db, id, fence_a, 0, "late", NULL) == ACKQ_STALE, "옛 worker의 nack도 STALE");
    CHECK(ackq_fail(q, db, id, fence_a, "late") == ACKQ_STALE, "옛 worker의 fail도 STALE");
    CHECK(ackq_update(q, db, id, fence_a, (const uint8_t *)"x", 1) == ACKQ_STALE, "옛 worker의 update도 STALE");
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.leased == 1 && stats.acked == 0, "STALE은 아무것도 바꾸지 않는다");
    CHECK(ackq_ack(q, db, id, fence_b) == ACKQ_OK, "현재 claim의 ack는 성공");
    CHECK(ackq_ack(q, db, id, fence_b) == ACKQ_OK, "같은 claim의 중복 ack는 멱등이다");
    CHECK(ackq_ack(q, db, id, fence_a) == ACKQ_STALE, "끝난 작업에 옛 fence는 STALE");
    CHECK(ackq_ack(q, db, 999, 1) == ACKQ_NOT_FOUND, "없는 id");
    CHECK(ackq_ack(q, db, id, 0) == ACKQ_INVALID, "fence 0은 잘못된 인자");

    /* 만료됐지만 아무도 다시 가져가지 않았다면 같은 worker의 ack는 받아들인다 */
    uint64_t late;
    assert(enq(q, db, "slow", NULL, &late, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 50) == 1);
    uint64_t late_fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    g_now += 500;
    CHECK(ackq_ack(q, db, late, late_fence) == ACKQ_OK, "만료 뒤 재claim 전의 ack는 받아들인다");

    /* 명시적 recover 뒤에는 그 claim이 끝난다 */
    uint64_t gone;
    assert(enq(q, db, "gone", NULL, &gone, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 50) == 1);
    uint64_t gone_fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    g_now += 49;
    size_t recovered = 7;
    CHECK(ackq_recover(q, db, 10, &recovered) == ACKQ_OK && recovered == 0, "아직 안 만료면 회복 0");
    g_now += 1;
    CHECK(ackq_recover(q, db, 10, &recovered) == ACKQ_OK && recovered == 1, "만료된 lease 1개 회복");
    CHECK(ackq_ack(q, db, gone, gone_fence) == ACKQ_STALE, "회복된 작업에 옛 ack는 STALE");
    CHECK(claim_n(q, db, 1, 50) == 1 && g_items[0].id == gone && g_items[0].fence == gone_fence + 1, "회복 뒤 새 fence로 전달");
    ackq_claims_release(g_items, 1);
    ackq_close(&q);
    close_db(db);
}

/* #195: 같은 claim의 lease를 늘린다. fence는 그대로이고, 다른 worker가 다시 가져간 뒤의 연장은 STALE이다. */
static void test_lease_extension(void)
{
    printf("\n[lease 연장]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 1000;
    uint64_t id, until = 0;
    assert(enq(q, db, "long", NULL, &id, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 100) == 1);
    uint64_t fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);

    g_now = 1090;
    CHECK(ackq_extend(q, db, id, fence, 100, &until) == ACKQ_OK && until == 1190, "연장한 lease는 지금 + lease_ms");
    g_now = 1150;
    CHECK(claim_n(q, db, 1, 100) == 0, "원래 lease(1100)가 지나도 연장한 동안은 다른 worker가 못 가진다");
    g_now = 1189;
    CHECK(claim_n(q, db, 1, 100) == 0, "연장한 lease 직전");

    /* 만료됐지만 아직 아무도 가져가지 않았으면 같은 claim으로 연장할 수 있다 */
    g_now = 1300;
    CHECK(ackq_extend(q, db, id, fence, 100, &until) == ACKQ_OK && until == 1400, "만료 뒤 재claim 전 연장");
    CHECK(claim_n(q, db, 1, 100) == 0, "다시 연장한 동안 못 가진다");
    g_now = 1400;
    CHECK(claim_n(q, db, 1, 100) == 1 && g_items[0].fence == fence + 1, "연장한 lease가 끝나면 다른 worker가 가져간다");
    uint64_t newer = g_items[0].fence;
    ackq_claims_release(g_items, 1);

    until = 77;
    CHECK(ackq_extend(q, db, id, fence, 100, &until) == ACKQ_STALE && until == 77, "옛 claim의 연장은 STALE");
    char sql[96];
    snprintf(sql, sizeof(sql), "SELECT fence FROM tote_queue_jobs WHERE id = %llu", (unsigned long long)id);
    int64_t now_fence = scalar_int(db, sql);
    snprintf(sql, sizeof(sql), "SELECT lease_until FROM tote_queue_jobs WHERE id = %llu", (unsigned long long)id);
    CHECK(now_fence == (int64_t)newer && scalar_int(db, sql) == 1500, "STALE 연장은 lease와 fence를 바꾸지 않는다");
    CHECK(ackq_extend(q, db, 999, 1, 100, &until) == ACKQ_NOT_FOUND, "없는 id");
    CHECK(ackq_extend(q, db, id, newer, ACKQ_MAX_LEASE_MS + 1, &until) == ACKQ_INVALID, "lease 상한 초과");
    CHECK(ackq_extend(q, db, id, 0, 100, &until) == ACKQ_INVALID, "fence 0");
    CHECK(ackq_ack(q, db, id, newer) == ACKQ_OK, "현재 claim ack");
    CHECK(ackq_extend(q, db, id, newer, 100, &until) == ACKQ_STALE, "끝난 작업은 연장하지 않는다");
    ackq_close(&q);
    close_db(db);
}

/* #196: purge 뒤에도 보존 기간 동안 같은 key의 재시도는 새 작업을 만들지 않는다. */
static void test_idem_retention_after_purge(void)
{
    printf("\n[purge 뒤 idempotency key 보존]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    o.idem_retention_ms = 1000;
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 10;
    uint64_t id, other;
    bool dup = false;
    assert(enq(q, db, "mail-1", "order-1", &id, NULL) == ACKQ_OK);
    assert(enq(q, db, "plain", NULL, &other, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 2, 100) == 2);
    for (size_t i = 0; i < 2; i++) assert(ackq_ack(q, db, g_items[i].id, g_items[i].fence) == ACKQ_OK);
    ackq_claims_release(g_items, 2);
    size_t purged = 0;
    g_now = 100;
    CHECK(ackq_purge(q, db, false, 10, &purged) == ACKQ_OK && purged == 2, "끝난 작업 둘을 지운다");

    g_now = 1099;
    CHECK(enq(q, db, "mail-1", "order-1", &other, &dup) == ACKQ_OK && dup && other == id,
          "보존 기간 안의 같은 key·payload는 지운 작업의 id와 duplicate");
    CHECK(enq(q, db, "mail-2", "order-1", &other, &dup) == ACKQ_CONFLICT, "보존 기간 안의 다른 payload는 CONFLICT");
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready + stats.leased + stats.acked + stats.failed == 0,
          "중복 재시도는 작업을 만들지 않는다");

    /* 보존 기간(purge 시각 100 + 1000)이 지나면 같은 key로 새 작업을 만든다 */
    g_now = 1100;
    CHECK(enq(q, db, "mail-1", "order-1", &other, &dup) == ACKQ_OK && !dup && other != id, "기간이 지나면 새 작업");
    CHECK(scalar_int(db, "SELECT COUNT(*) FROM tote_queue_jobs_keys") == 0, "만료된 key는 지워졌다");

    /* 0이면 key를 남기지 않는다 */
    ackq_close(&q);
    ackq_options_t none = qopts("plain", 0);
    none.idem_retention_ms = ACKQ_RETENTION_NONE;
    assert(ackq_install(db, &none, &q) == ACKQ_OK);
    assert(enq(q, db, "a", "k", &id, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 100) == 1);
    assert(ackq_ack(q, db, g_items[0].id, g_items[0].fence) == ACKQ_OK);
    ackq_claims_release(g_items, 1);
    assert(ackq_purge(q, db, false, 10, &purged) == ACKQ_OK && purged == 1);
    CHECK(enq(q, db, "a", "k", &other, &dup) == ACKQ_OK && !dup && other != id, "보존하지 않는 큐는 purge 뒤 새 작업");
    CHECK(scalar_int(db, "SELECT COUNT(*) FROM tote_queue_plain_keys") == 0, "key를 남기지 않는다");
    ackq_close(&q);

    /* 기본 보존 기간은 7일 */
    ackq_options_t dflt = qopts("dflt", 0);
    assert(ackq_install(db, &dflt, &q) == ACKQ_OK);
    g_now = 0;
    assert(enq(q, db, "a", "k", &id, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 100) == 1);
    assert(ackq_ack(q, db, g_items[0].id, g_items[0].fence) == ACKQ_OK);
    ackq_claims_release(g_items, 1);
    assert(ackq_purge(q, db, false, 10, &purged) == ACKQ_OK && purged == 1);
    g_now = ACKQ_DEFAULT_IDEM_RETENTION_MS - 1;
    CHECK(enq(q, db, "a", "k", &other, &dup) == ACKQ_OK && dup && other == id, "기본 보존 기간 마지막 순간");
    g_now = ACKQ_DEFAULT_IDEM_RETENTION_MS;
    CHECK(enq(q, db, "a", "k", &other, &dup) == ACKQ_OK && !dup, "기본 보존 기간이 지나면 새 작업");
    ackq_close(&q);
    close_db(db);
}

/* #197: payload 상한은 큐 옵션이다. 0이면 기본 1 MiB이고, 범위를 벗어난 값은 설치를 거절한다. */
static void test_payload_limit_option(void)
{
    printf("\n[payload 상한 옵션]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("small", 0);
    o.max_payload_bytes = 16;
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    static const uint8_t bytes[17] = "0123456789abcdef";
    uint64_t id, rejected;
    CHECK(ackq_enqueue(q, db, bytes, 16, NULL, &id, NULL) == ACKQ_OK, "옵션 상한과 같은 크기");
    CHECK(ackq_enqueue(q, db, bytes, 17, NULL, &rejected, NULL) == ACKQ_LIMIT, "옵션 상한+1");
    CHECK(ackq_update(q, db, id, 0, bytes, 17) == ACKQ_LIMIT, "update도 같은 상한");
    ackq_close(&q);

    size_t big = ACKQ_DEFAULT_MAX_PAYLOAD_BYTES * 2;
    o = qopts("big", 0);
    o.max_payload_bytes = big;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    uint8_t *payload = malloc(big + 1);
    for (size_t i = 0; i <= big; i++) payload[i] = (uint8_t)(i * 7u);
    CHECK(ackq_enqueue(q, db, payload, big, NULL, &id, NULL) == ACKQ_OK, "늘린 상한 크기");
    CHECK(ackq_enqueue(q, db, payload, big + 1, NULL, &id, NULL) == ACKQ_LIMIT, "늘린 상한+1");
    CHECK(claim_n(q, db, 1, 100) == 1 && g_items[0].payload_size == big && memcmp(g_items[0].payload, payload, big) == 0,
          "늘린 상한 payload 왕복");
    ackq_claims_release(g_items, 1);
    free(payload);
    ackq_close(&q);

    o = qopts("toolarge", 0);
    o.max_payload_bytes = (size_t)ACKQ_MAX_PAYLOAD_LIMIT + 1;
    CHECK(ackq_install(db, &o, &q) == ACKQ_INVALID, "허용 범위를 넘는 상한은 거절");
    close_db(db);
}

/* ── 6. nack·retry·terminal failure ── */

static void test_nack_retry_terminal(void)
{
    printf("\n[nack·재시도·terminal failure]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 3);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 5000;
    uint64_t id;
    bool terminal = true;
    assert(enq(q, db, "retry", NULL, &id, NULL) == ACKQ_OK);

    assert(claim_n(q, db, 1, 1000) == 1);
    uint64_t fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    CHECK(ackq_nack(q, db, id, fence, 200, "temporary", &terminal) == ACKQ_OK && !terminal, "첫 nack은 재시도");
    CHECK(ackq_nack(q, db, id, fence, 200, "temporary", &terminal) == ACKQ_OK, "같은 claim의 중복 nack은 멱등");
    CHECK(claim_n(q, db, 1, 1000) == 0, "지연 동안은 claim되지 않는다");
    g_now = 5199;
    CHECK(claim_n(q, db, 1, 1000) == 0, "지연 직전에도 안 된다");
    g_now = 5200;
    CHECK(claim_n(q, db, 1, 1000) == 1 && g_items[0].attempts == 2 && g_items[0].fence == 2, "지연 뒤 시도 2로 재전달");
    fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    ackq_item_t listed[2];
    size_t listed_count = 0;
    CHECK(ackq_list(q, db, 0, 2, false, listed, &listed_count) == ACKQ_OK && listed_count == 1, "list 1건");
    CHECK(strcmp(listed[0].note, "temporary") == 0 && listed[0].state == ACKQ_STATE_LEASED, "마지막 이유가 남는다");
    ackq_list_release(listed, listed_count);

    CHECK(ackq_nack(q, db, id, fence, 0, NULL, &terminal) == ACKQ_OK && !terminal, "둘째 nack도 재시도(2/3)");
    CHECK(claim_n(q, db, 1, 1000) == 1 && g_items[0].attempts == 3, "시도 3");
    fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    CHECK(ackq_nack(q, db, id, fence, 0, "give up", &terminal) == ACKQ_OK && terminal, "시도 횟수에 닿으면 terminal");
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.failed == 1 && stats.ready == 0, "FAILED로 끝났다");
    CHECK(claim_n(q, db, 1, 1000) == 0, "FAILED는 다시 전달되지 않는다");
    CHECK(ackq_nack(q, db, id, fence, 0, NULL, &terminal) == ACKQ_OK && terminal, "terminal 뒤 같은 claim의 중복 nack은 멱등");
    CHECK(ackq_ack(q, db, id, fence) == ACKQ_STALE, "FAILED에 ack는 STALE");

    /* 명시적 terminal failure */
    uint64_t bad;
    assert(enq(q, db, "poison", NULL, &bad, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 1, 1000) == 1);
    fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    CHECK(ackq_fail(q, db, bad, fence, "malformed") == ACKQ_OK, "명시적 fail");
    CHECK(ackq_fail(q, db, bad, fence, "malformed") == ACKQ_OK, "중복 fail은 멱등");
    CHECK(ackq_ack(q, db, bad, fence) == ACKQ_STALE, "fail한 작업에 ack는 STALE");
    CHECK(ackq_fail(q, db, bad, fence, "\xff") == ACKQ_INVALID, "이유의 UTF-8 검증");
    char longnote[ACKQ_NOTE_MAX + 2];
    memset(longnote, 'n', sizeof(longnote));
    longnote[ACKQ_NOTE_MAX + 1] = '\0';
    CHECK(ackq_fail(q, db, bad, fence, longnote) == ACKQ_LIMIT, "이유 길이 한도");
    CHECK(ackq_list(q, db, 0, 10, false, listed, &listed_count) == ACKQ_OK, "list");
    CHECK(listed_count == 2 && listed[0].state == ACKQ_STATE_FAILED && listed[1].state == ACKQ_STATE_FAILED &&
              strcmp(listed[1].note, "malformed") == 0, "FAILED 둘과 이유");
    ackq_list_release(listed, listed_count);

    /* 만료 lease가 시도 횟수를 다 쓰면 recover가 FAILED로 끝낸다 */
    uint64_t dying;
    assert(enq(q, db, "crashy", NULL, &dying, NULL) == ACKQ_OK);
    for (int attempt = 1; attempt <= 3; attempt++) {
        size_t n = claim_n(q, db, 1, 10);
        CHECK(n == 1 && g_items[0].attempts == (uint32_t)attempt, "worker가 %d번째 죽는다", attempt);
        ackq_claims_release(g_items, n);
        g_now += 10;
        if (attempt < 3) continue;
        CHECK(claim_n(q, db, 1, 10) == 0, "시도를 다 쓴 만료 lease는 claim되지 않는다");
    }
    size_t recovered = 0;
    CHECK(ackq_recover(q, db, 10, &recovered) == ACKQ_OK && recovered == 1, "recover가 처리");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.failed == 3 && stats.leased == 0, "FAILED로 끝났다");
    size_t purged = 0;
    CHECK(ackq_purge(q, db, false, 10, &purged) == ACKQ_OK && purged == 0, "ACKED만 지우면 0");
    CHECK(ackq_purge(q, db, true, 10, &purged) == ACKQ_OK && purged == 3, "FAILED도 지우면 3");
    ackq_close(&q);
    close_db(db);
}

/* ── 7. update·release·purge·list cursor ── */

static void test_update_release_cursor(void)
{
    printf("\n[update·release·purge·bounded cursor]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 100;
    uint64_t ids[7];
    for (int i = 0; i < 7; i++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "payload-%d", i);
        assert(enq(q, db, payload, NULL, &ids[i], NULL) == ACKQ_OK);
    }
    CHECK(ackq_update(q, db, ids[0], 0, (const uint8_t *)"edited", 6) == ACKQ_OK, "READY 작업은 fence 0으로 수정");
    assert(claim_n(q, db, 1, 100) == 1);
    CHECK(g_items[0].payload_size == 6 && memcmp(g_items[0].payload, "edited", 6) == 0, "수정이 반영됐다");
    uint64_t fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);
    CHECK(ackq_update(q, db, ids[0], 0, (const uint8_t *)"z", 1) == ACKQ_STALE, "LEASED는 fence 0으로 못 고친다");
    CHECK(ackq_update(q, db, ids[0], fence, (const uint8_t *)"v2", 2) == ACKQ_OK, "claim 중에는 fence로 수정");
    CHECK(ackq_ack(q, db, ids[0], fence) == ACKQ_OK, "ack");
    CHECK(ackq_update(q, db, ids[0], fence, (const uint8_t *)"v3", 2) == ACKQ_STALE, "ACKED는 수정 불가");
    CHECK(ackq_update(q, db, 999, 0, (const uint8_t *)"v", 1) == ACKQ_NOT_FOUND, "없는 id");

    assert(claim_n(q, db, 2, 100) == 2);
    ackq_claims_release(g_items, 2);
    size_t released = 0;
    CHECK(ackq_release_leases(q, db, 10, &released) == ACKQ_OK && released == 2, "모든 lease 해제 2개");
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready == 6 && stats.leased == 0 && stats.acked == 1, "상태 집계");
    CHECK(claim_n(q, db, 1, 100) == 1 && g_items[0].id == ids[1] && g_items[0].fence == 2, "해제된 작업은 새 fence로 다시 나온다");
    ackq_claims_release(g_items, 1);

    /* cursor: 3개씩, payload 없이 */
    ackq_item_t page[3];
    size_t count = 0;
    uint64_t after = 0;
    size_t total = 0;
    for (int round = 0; round < 4; round++) {
        CHECK(ackq_list(q, db, after, 3, false, page, &count) == ACKQ_OK, "list 페이지");
        for (size_t i = 0; i < count; i++) {
            CHECK(page[i].payload == NULL && page[i].id > after, "payload를 복사하지 않고 id가 이어진다");
            after = page[i].id;
        }
        total += count;
        ackq_list_release(page, count);
        if (count == 0) break;
    }
    CHECK(total == 7, "cursor가 7개를 빠짐없이 읽었다 (total=%zu)", total);
    CHECK(ackq_list(q, db, ids[2], 2, true, page, &count) == ACKQ_OK && count == 2 && page[0].id == ids[3] &&
              page[0].payload_size == strlen("payload-3"), "with_payload는 반환 항목의 payload만");
    ackq_list_release(page, count);
    size_t purged = 0;
    CHECK(ackq_purge(q, db, false, 10, &purged) == ACKQ_OK && purged == 1, "ACKED 1개 삭제");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.acked == 0 && stats.ready + stats.leased == 6, "삭제 뒤 집계");
    uint64_t fresh;
    assert(enq(q, db, "after-purge", NULL, &fresh, NULL) == ACKQ_OK);
    CHECK(fresh > ids[6], "삭제한 id를 다시 쓰지 않는다 (옛 fence가 새 작업에 적용되면 안 된다)");
    ackq_close(&q);
    close_db(db);
}

/* ── 8. 같은 트랜잭션: 업무 갱신과 ack ── */

static void test_same_transaction(void)
{
    printf("\n[업무 갱신과 ack를 같은 트랜잭션으로]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    assert(exec_ok(db, "CREATE TABLE orders (id INTEGER PRIMARY KEY, status TEXT NOT NULL)"));
    assert(exec_ok(db, "INSERT INTO orders VALUES (1, 'new')") && exec_ok(db, "INSERT INTO orders VALUES (2, 'new')"));
    g_now = 10;
    uint64_t a, b;
    assert(enq(q, db, "order-1", NULL, &a, NULL) == ACKQ_OK && enq(q, db, "order-2", NULL, &b, NULL) == ACKQ_OK);
    assert(claim_n(q, db, 2, 1000) == 2);
    uint64_t fa = g_items[0].fence, fb = g_items[1].fence;
    ackq_claims_release(g_items, 2);
    session_t *txn = NULL;
    assert((txn = begin_txn(db)) != NULL);
    CHECK(txn_ok(txn, "UPDATE orders SET status = 'done' WHERE id = 1"), "업무 갱신");
    CHECK(ackq_ack(q, txn, a, fa) == ACKQ_OK, "같은 트랜잭션에서 ack");
    CHECK(scalar_int(db, "SELECT id FROM orders WHERE status = 'done'") == -1, "commit 전에는 보이지 않는다");
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.acked == 0, "ack도 commit 전에는 보이지 않는다");
    CHECK(commit_txn(&txn) == TXN_OK, "commit");
    CHECK(scalar_int(db, "SELECT id FROM orders WHERE status = 'done'") == 1, "업무 갱신이 확정됐다");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.acked == 1, "ack도 함께 확정됐다");

    /* rollback이면 둘 다 사라지고 작업은 그대로 처리 중이다 */
    assert((txn = begin_txn(db)) != NULL);
    CHECK(txn_ok(txn, "UPDATE orders SET status = 'done' WHERE id = 2") && ackq_ack(q, txn, b, fb) == ACKQ_OK, "둘째 갱신과 ack");
    CHECK(rollback_txn(&txn) == TXN_OK, "rollback");
    CHECK(scalar_int(db, "SELECT id FROM orders WHERE status = 'new'") == 2, "업무 갱신이 취소됐다");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.acked == 1 && stats.leased == 1, "ack도 취소됐다");

    /* stale ack는 호출자의 트랜잭션을 실패시키지 않고 호출자가 판단한다 */
    assert((txn = begin_txn(db)) != NULL);
    CHECK(ackq_ack(q, txn, b, fb + 7) == ACKQ_STALE, "stale ack");
    CHECK(!session_txn_failed(txn), "STALE은 트랜잭션을 실패 상태로 만들지 않는다");
    CHECK(ackq_ack(q, txn, 999, 1) == ACKQ_NOT_FOUND && !session_txn_failed(txn), "NOT_FOUND도 마찬가지");
    CHECK(txn_ok(txn, "UPDATE orders SET status = 'ghost' WHERE id = 2"), "호출자가 이어서 업무 문장을 실행할 수 있다");
    CHECK(rollback_txn(&txn) == TXN_OK, "호출자가 stale이라 rollback");
    CHECK(scalar_int(db, "SELECT id FROM orders WHERE status = 'ghost'") == -1, "stale이면 업무 갱신을 적용하지 않았다");

    /* 같은 트랜잭션에서 enqueue와 업무 갱신 */
    assert((txn = begin_txn(db)) != NULL);
    uint64_t chained;
    CHECK(txn_ok(txn, "INSERT INTO orders VALUES (3, 'queued')") && enq(q, txn, "order-3", "ord-3", &chained, NULL) == ACKQ_OK,
          "주문 생성과 작업 등록");
    CHECK(commit_txn(&txn) == TXN_OK, "commit");
    CHECK(scalar_int(db, "SELECT id FROM orders WHERE id = 3") == 3 && ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready == 1,
          "주문과 작업이 함께 확정");

    /* 읽기 전용으로 연 DB는 쓰기를 거절하고 숨기지 않는다 */
    ackq_close(&q);
    close_db(db);
}

/* ── 9. 재시작 복구 ── */

static void test_restart_recovery(void)
{
    printf("\n[재시작 복구: 작업 상태 보존과 lease 의미]\n");
    make_db();
    session_t *db = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 1000;
    uint64_t ids[5];
    for (int i = 0; i < 5; i++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "r%d", i);
        assert(enq(q, db, payload, i == 4 ? "key-4" : NULL, &ids[i], NULL) == ACKQ_OK);
    }
    assert(claim_n(q, db, 3, 5000) == 3);
    uint64_t f0 = g_items[0].fence, f1 = g_items[1].fence, f2 = g_items[2].fence;
    ackq_claims_release(g_items, 3);
    assert(ackq_ack(q, db, ids[0], f0) == ACKQ_OK);
    assert(ackq_nack(q, db, ids[1], f1, 300, "later", NULL) == ACKQ_OK);
    /* ids[2]는 worker가 쥔 채로 프로세스가 사라진다 */
    ackq_close(&q);
    close_db(db);

    db = open_db();
    assert(ackq_open(db, &o, &q) == ACKQ_OK);
    ackq_stats_t stats;
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.acked == 1 && stats.leased == 1 && stats.ready == 3,
          "재시작 뒤 상태가 그대로다 (ack 1, lease 1, 대기 3)");
    CHECK(ackq_ack(q, db, ids[0], f0) == ACKQ_OK, "ack된 작업의 중복 ack는 재시작 뒤에도 멱등");
    CHECK(ackq_ack(q, db, ids[2], f2) == ACKQ_OK && ackq_stats(q, db, &stats) == ACKQ_OK && stats.acked == 2,
          "죽지 않은 worker가 같은 lease로 ack하면 받아들인다");
    uint64_t dup;
    bool is_dup = false;
    CHECK(enq(q, db, "r4", "key-4", &dup, &is_dup) == ACKQ_OK && is_dup && dup == ids[4], "idempotency key가 재시작을 넘는다");
    g_now = 1100;
    size_t n = claim_n(q, db, 10, 5000);
    CHECK(n == 2 && g_items[0].id == ids[3] && g_items[1].id == ids[4],
          "지연(300ms) 중인 nack 작업은 빼고 대기 중인 둘만 나온다 (n=%zu)", n);
    ackq_claims_release(g_items, n);
    g_now = 1300;
    n = claim_n(q, db, 10, 5000);
    CHECK(n == 1 && g_items[0].id == ids[1] && g_items[0].fence == f1 + 1, "지연이 지나면 나온다 (n=%zu)", n);
    ackq_claims_release(g_items, n);
    ackq_close(&q);
    close_db(db);

    /* worker가 죽은 채 lease가 만료되면 새 worker가 새 fence로 가져가고 옛 worker의 ack는 거절된다 */
    db = open_db();
    assert(ackq_open(db, &o, &q) == ACKQ_OK);
    g_now = 1300 + 5000;
    n = claim_n(q, db, 10, 1000);
    CHECK(n == 3, "만료된 lease 셋이 재전달된다 (n=%zu)", n);
    uint64_t redelivered_fence = 0;
    for (size_t i = 0; i < n; i++) {
        if (g_items[i].id == ids[1]) redelivered_fence = g_items[i].fence;
    }
    ackq_claims_release(g_items, n);
    CHECK(redelivered_fence == f1 + 2, "fence는 재시작을 넘어 계속 증가한다");
    CHECK(ackq_ack(q, db, ids[1], f1 + 1) == ACKQ_STALE, "죽은 worker의 늦은 ack는 거절");
    CHECK(ackq_ack(q, db, ids[1], redelivered_fence) == ACKQ_OK, "새 worker의 ack는 성공");
    ackq_close(&q);
    close_db(db);
}

/* ── 10. 다른 database의 세션과 읽기 전용 database ── */

static void test_wrong_database_and_read_only(void)
{
    printf("\n[다른 database의 세션 거절과 읽기 전용 database]\n");
    make_db();
    session_t *a = open_db();
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *qa;
    assert(ackq_install(a, &o, &qa) == ACKQ_OK);
    g_now = 10;
    uint64_t id;
    assert(enq(qa, a, "a-job", NULL, &id, NULL) == ACKQ_OK);
    assert(claim_n(qa, a, 1, 1000) == 1);
    uint64_t fence = g_items[0].fence;
    ackq_claims_release(g_items, 1);

    /* 두 번째 database: 같은 이름의 큐 테이블은 없고 업무 테이블만 있다 */
    char path_b[192];
    snprintf(path_b, sizeof(path_b), "%s/other.totedb", g_dir);
    database_t *created = NULL;
    assert(database_create(path_b, &created) == DATABASE_OK && database_close(created) == DATABASE_OK);
    session_t *b = open_conn(path_b, true, true, NULL);
    assert(b != NULL);
    assert(exec_ok(b, "CREATE TABLE tote_queue_jobs (id INTEGER PRIMARY KEY, marker TEXT NOT NULL)"));
    assert(exec_ok(b, "INSERT INTO tote_queue_jobs VALUES (1, 'other-db')"));
    session_t *foreign = NULL;
    assert((foreign = begin_txn(b)) != NULL);
    CHECK(session_database(foreign) != session_database(a) && session_database(foreign) == session_database(b), "소속 확인");
    ackq_claim_t item[1];
    size_t count = 5;
    uint64_t other_id = 9;
    ackq_stats_t stats;
    ackq_item_t listed[1];
    CHECK(enq(qa, foreign, "x", NULL, &other_id, NULL) == ACKQ_INVALID && other_id == 0, "enqueue 거절");
    CHECK(ackq_claim(qa, foreign, 1, 100, item, &count) == ACKQ_INVALID && count == 0, "claim 거절");
    CHECK(ackq_ack(qa, foreign, id, fence) == ACKQ_INVALID, "ack 거절");
    CHECK(ackq_nack(qa, foreign, id, fence, 0, NULL, NULL) == ACKQ_INVALID, "nack 거절");
    CHECK(ackq_fail(qa, foreign, id, fence, NULL) == ACKQ_INVALID, "fail 거절");
    CHECK(ackq_update(qa, foreign, id, fence, (const uint8_t *)"x", 1) == ACKQ_INVALID, "update 거절");
    CHECK(ackq_recover(qa, foreign, 1, NULL) == ACKQ_INVALID && ackq_release_leases(qa, foreign, 1, NULL) == ACKQ_INVALID,
          "recover·release 거절");
    CHECK(ackq_purge(qa, foreign, true, 1, NULL) == ACKQ_INVALID, "purge 거절");
    CHECK(ackq_stats(qa, foreign, &stats) == ACKQ_INVALID, "stats 거절");
    CHECK(ackq_list(qa, foreign, 0, 1, false, listed, &count) == ACKQ_INVALID, "list 거절");
    CHECK(!session_txn_failed(foreign), "거절은 남의 트랜잭션을 실패시키지 않는다");
    CHECK(commit_txn(&foreign) == TXN_OK, "남의 트랜잭션은 영향 없이 commit된다");
    CHECK(scalar_int(b, "SELECT id FROM tote_queue_jobs WHERE marker = 'other-db'") == 1 && scalar_int(b, "SELECT id FROM tote_queue_jobs") == 1,
          "다른 database의 같은 이름 테이블이 바뀌지 않았다");
    CHECK(ackq_stats(qa, a, &stats) == ACKQ_OK && stats.leased == 1 && stats.acked == 0 && stats.ready == 0,
          "이 큐도 바뀌지 않았다");
    CHECK(ackq_ack(qa, a, id, fence) == ACKQ_OK, "올바른 호출은 그대로 된다");
    ackq_close(&qa);
    close_db(a);
    close_db(b);

    /* 읽기 전용으로 열면 읽기는 되고 쓰기는 숨기지 않고 거절한다 */
    session_t *ro = open_conn(g_db_path, false, false, NULL);
    assert(ro != NULL);
    ackq_t *qr = NULL;
    CHECK(ackq_open(ro, &o, &qr) == ACKQ_OK, "읽기 전용 열기");
    CHECK(ackq_stats(qr, ro, &stats) == ACKQ_OK && stats.acked == 1, "읽기 전용 stats");
    ackq_item_t page[2];
    CHECK(ackq_list(qr, ro, 0, 2, true, page, &count) == ACKQ_OK && count == 1 && page[0].payload_size == 5,
          "읽기 전용 list");
    ackq_list_release(page, count);
    CHECK(enq(qr, ro, "w", NULL, &id, NULL) == ACKQ_UNSUPPORTED && id == 0, "읽기 전용 enqueue는 UNSUPPORTED");
    count = 3;
    CHECK(ackq_claim(qr, ro, 1, 100, item, &count) == ACKQ_UNSUPPORTED && count == 0, "읽기 전용 claim은 UNSUPPORTED");
    CHECK(ackq_ack(qr, ro, 1, 1) == ACKQ_UNSUPPORTED, "읽기 전용 ack는 UNSUPPORTED");
    ackq_close(&qr);
    close_db(ro);
}

/* ── 11. DB 단독과 큐 경로의 같은 세션 계약(#79) ── */

/* 자동 commit 문장 하나. 오류 결과면 그 분류를 돌려주고 성공이면 RESULT_E_NONE */
static result_error_class_t exec_class(session_t *db, const char *sql, const value_view_t *params, size_t count)
{
    lexer_binding_t binding = {params, count};
    result_t *r = NULL;
    if (session_exec(db, NULL, (const uint8_t *)sql, strlen(sql), count > 0 ? &binding : NULL, &r) != DATABASE_EXEC_OK) {
        return RESULT_E_INTERNAL;
    }
    result_error_class_t error_class = RESULT_E_NONE;
    if (result_kind(r) == RESULT_KIND_ERROR) {
        result_row_t row;
        result_next(r, &row);
        error_class = result_outcome(r).error->error_class;
    }
    result_release(&r);
    return error_class;
}

static void test_session_contract_db_and_queue(void)
{
    printf("\n[DB 단독과 큐 경로의 같은 세션 계약]\n");
    make_db();
    /* 문장 하나의 작업 메모리 한도 256KB. 1MB 값은 어느 경로로 넣어도 한도를 넘는다 */
    session_limits_t limits = {SIZE_MAX, SIZE_MAX, 256u * 1024u, 8, 0};
    session_t *db = open_conn(g_db_path, true, true, &limits);
    assert(db != NULL);
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    assert(exec_ok(db, "CREATE TABLE notes (id INTEGER PRIMARY KEY, body BLOB NOT NULL)"));
    size_t big_size = 400u * 1024u;
    uint8_t *big = calloc(1, big_size);
    assert(big != NULL);
    value_view_t small_row[2] = {{VALUE_INTEGER, 1, 0.0, NULL, 0}, {VALUE_BLOB, 0, 0.0, (const uint8_t *)"x", 1}};
    value_view_t big_row[2] = {{VALUE_INTEGER, 2, 0.0, NULL, 0}, {VALUE_BLOB, 0, 0.0, big, big_size}};
    ackq_stats_t stats;
    uint64_t id;
    g_now = 10;

    /* 정상: 두 경로 모두 끝나면 작업 메모리와 결과 몫이 0이다 */
    CHECK(exec_class(db, "INSERT INTO notes VALUES (?, ?)", small_row, 2) == RESULT_E_NONE && conn_settled(db),
          "DB 단독 정상 문장 뒤 계상 0");
    CHECK(enq(q, db, "ok", NULL, &id, NULL) == ACKQ_OK && claim_n(q, db, 1, 1000) == 1 && conn_settled(db),
          "큐 정상 연산 뒤 계상 0");
    ackq_claims_release(g_items, 1);

    /* 문장 예산 초과: 두 경로 모두 결과를 반영하지 않고 거절하며 자원을 돌려준다. 이어지는 작은 문장은 된다 */
    CHECK(exec_class(db, "INSERT INTO notes VALUES (?, ?)", big_row, 2) == RESULT_E_NOMEM && conn_settled(db),
          "DB 단독 예산 초과는 NOMEM 오류 결과");
    CHECK(scalar_int(db, "SELECT COUNT(*) FROM notes") == 1, "DB 단독 초과 문장은 반영되지 않았다");
    uint64_t refused = 9;
    CHECK(ackq_enqueue(q, db, big, big_size, NULL, &refused, NULL) == ACKQ_NOMEM && refused == 0 && conn_settled(db),
          "큐 예산 초과는 ACKQ_NOMEM");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready == 0 && stats.leased == 1, "큐 초과 연산은 반영되지 않았다");
    CHECK(!session_txn_active(db), "큐의 자기 트랜잭션은 rollback되어 세션에 남지 않는다");
    CHECK(enq(q, db, "after", NULL, &id, NULL) == ACKQ_OK && conn_settled(db), "초과 뒤 큐 연산이 다시 된다");

    /* 취소: 다음 문장 하나를 취소한다. 큐 연산은 자기 트랜잭션을 rollback하고 ACKQ_CANCELLED다 */
    session_cancel(db);
    CHECK(exec_class(db, "INSERT INTO notes VALUES (3, X'00')", NULL, 0) == RESULT_E_CANCELLED && conn_settled(db),
          "DB 단독 문장 취소");
    CHECK(scalar_int(db, "SELECT COUNT(*) FROM notes") == 1, "취소된 DB 문장은 반영되지 않았다");
    session_cancel(db);
    CHECK(enq(q, db, "cancelled", NULL, &refused, NULL) == ACKQ_CANCELLED && conn_settled(db), "큐 연산 취소");
    CHECK(ackq_stats(q, db, &stats) == ACKQ_OK && stats.ready == 1 && !session_txn_active(db),
          "취소된 큐 연산은 반영되지 않았고 트랜잭션도 남지 않는다");

    /* 이전된 결과는 release 전까지 세션 몫으로 남고, 그동안의 큐 연산도 같은 세션 계상을 쓴다 */
    result_t *held = NULL;
    const char *select = "SELECT body FROM notes";
    assert(session_exec(db, NULL, (const uint8_t *)select, strlen(select), NULL, &held) == DATABASE_EXEC_OK);
    session_stats_t before, after;
    assert(session_stats_get(db, &before) == SESSION_OK);
    CHECK(before.result_used > 0 && before.query_used == 0, "쥔 결과는 세션의 이전된 몫");
    CHECK(enq(q, db, "while-held", NULL, &id, NULL) == ACKQ_OK, "결과를 쥔 채 큐 연산");
    assert(session_stats_get(db, &after) == SESSION_OK);
    CHECK(after.result_used == before.result_used && after.query_used == 0 && after.statements > before.statements,
          "큐 연산도 같은 세션의 문장으로 계상되고 끝나면 작업 메모리가 0");
    result_release(&held);
    CHECK(conn_settled(db), "release 뒤 결과 몫 반환");
    free(big);
    ackq_close(&q);
    CHECK(close_db(db), "세션·데이터베이스·풀을 닫는다");
}

int main(void)
{
    set_dir(DIR);
    test_optional_install();
    test_enqueue_claim();
    test_payload_and_idempotency();
    test_concurrent_claim();
    test_lease_and_fencing();
    test_lease_extension();
    test_idem_retention_after_purge();
    test_payload_limit_option();
    test_nack_retry_terminal();
    test_update_release_cursor();
    test_same_transaction();
    test_restart_recovery();
    test_wrong_database_and_read_only();
    test_session_contract_db_and_queue();
    printf("\ntest_ack_queue: %d checks, %d failed\n", g_run, g_fail);
    if (g_fail == 0) remove_dir();
    return g_fail == 0 ? 0 : 1;
}
