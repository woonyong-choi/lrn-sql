/*
 * 트랜잭션 상태 코어 검사. 실제 DB(page_file·database)가 아니라 다중 버전 key-value 가짜 backend 위에서
 * manager 계약(가시성 순번, outcome, 보증, 경합, 회수 하한, 재진입)을 검사한다.
 * 빌드: gcc -Wall -Wextra -Werror -g -std=c11 -Iinclude -pthread tests/test_transaction.c
 *       src/storage/transaction.c -o build/test_transaction
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "storage/transaction.h"

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define MAX_VERSIONS 256

typedef struct {
    int key;
    int value;
    uint64_t seq;       /* 이 버전을 만든 commit 순번 */
    uint64_t dead_seq;  /* 이 순번 commit이 대체함. 0이면 최신 */
    int freed;
} version_t;

typedef struct {
    pthread_mutex_t lock;
    version_t versions[MAX_VERSIONS];
    int count;
    int pending_key[8];
    int pending_value[8];
    int pending_count;
    /* 다음 publish에 줄 결과 */
    txn_outcome_t next_outcome;
    txn_guarantee_t next_achieved;
    int begin_calls, publish_calls, finish_calls, close_calls, reclaim_calls;
    int finish_applied, finish_not_applied, finish_unknown;
    int fail_begin;
    int backend_poisoned;
    int writers_inside;
    int max_writers_inside;
    uint64_t last_horizon;
    int reenter_probe;
    txn_manager_t *manager;
    int reenter_results[6];
} fake_t;

static void fake_init(fake_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    pthread_mutex_init(&fake->lock, NULL);
    fake->next_outcome = TXN_OUTCOME_APPLIED;
    fake->next_achieved = TXN_GUARANTEE_SYNCED;
}

static int fake_get(fake_t *fake, int key, uint64_t snapshot, int *value)
{
    int found = 0;

    pthread_mutex_lock(&fake->lock);
    for (int i = 0; i < fake->count; i++) {
        version_t *v = &fake->versions[i];
        if (v->key == key && v->seq <= snapshot && (v->dead_seq == 0 || v->dead_seq > snapshot)) {
            *value = v->value;
            found = 1;
        }
    }
    pthread_mutex_unlock(&fake->lock);
    return found;
}

static void fake_stage(fake_t *fake, int key, int value)
{
    fake->pending_key[fake->pending_count] = key;
    fake->pending_value[fake->pending_count] = value;
    fake->pending_count++;
}

static void probe_reentry(fake_t *fake)
{
    txn_t *txn = NULL;
    txn_commit_info_t info;

    fake->reenter_results[0] = txn_begin(fake->manager, NULL, &txn);
    fake->reenter_results[1] = txn_query_commit(fake->manager, 1, &info);
    fake->reenter_results[2] = txn_manager_close(&fake->manager) == TXN_REENTRANT;
    (void)txn_manager_last_committed(fake->manager);
    (void)txn_manager_oldest_snapshot(fake->manager);
    (void)txn_manager_is_poisoned(fake->manager);
    fake->reenter_results[3] = 1;
}

static txn_status_t fake_write_begin(void *ctx, uint64_t base_seq, void **write)
{
    fake_t *fake = ctx;

    (void)base_seq;
    pthread_mutex_lock(&fake->lock);
    fake->begin_calls++;
    if (fake->fail_begin) {
        pthread_mutex_unlock(&fake->lock);
        return TXN_IO;
    }
    fake->writers_inside++;
    if (fake->writers_inside > fake->max_writers_inside) {
        fake->max_writers_inside = fake->writers_inside;
    }
    fake->pending_count = 0;
    pthread_mutex_unlock(&fake->lock);
    *write = fake;
    return TXN_OK;
}

static txn_publish_result_t fake_publish(void *ctx, void *write, uint64_t commit_seq,
                                         txn_guarantee_t requested)
{
    fake_t *fake = ctx;
    txn_publish_result_t result;

    (void)write;
    (void)requested;
    if (fake->reenter_probe) {
        probe_reentry(fake);
    }
    pthread_mutex_lock(&fake->lock);
    fake->publish_calls++;
    result.outcome = fake->next_outcome;
    result.achieved = fake->next_achieved;
    if (fake->next_outcome != TXN_OUTCOME_NOT_APPLIED) {
        /* UNKNOWN인 경우에도 장치에는 적용됐을 수 있으므로 버전을 기록한다(응답 손실 모델) */
        for (int i = 0; i < fake->pending_count; i++) {
            for (int j = 0; j < fake->count; j++) {
                if (fake->versions[j].key == fake->pending_key[i] && fake->versions[j].dead_seq == 0) {
                    fake->versions[j].dead_seq = commit_seq;
                }
            }
            fake->versions[fake->count++] = (version_t){.key = fake->pending_key[i],
                                                        .value = fake->pending_value[i],
                                                        .seq = commit_seq};
        }
    }
    pthread_mutex_unlock(&fake->lock);
    return result;
}

static void fake_write_finish(void *ctx, void *write, txn_outcome_t outcome, uint64_t commit_seq)
{
    fake_t *fake = ctx;

    (void)write;
    (void)commit_seq;
    pthread_mutex_lock(&fake->lock);
    fake->finish_calls++;
    fake->writers_inside--;
    if (outcome == TXN_OUTCOME_APPLIED) {
        fake->finish_applied++;
    } else if (outcome == TXN_OUTCOME_NOT_APPLIED) {
        fake->finish_not_applied++;
    } else {
        fake->finish_unknown++;
    }
    pthread_mutex_unlock(&fake->lock);
}

static void fake_reclaim(void *ctx, uint64_t horizon)
{
    fake_t *fake = ctx;

    pthread_mutex_lock(&fake->lock);
    fake->reclaim_calls++;
    fake->last_horizon = horizon;
    for (int i = 0; i < fake->count; i++) {
        version_t *v = &fake->versions[i];
        if (v->dead_seq != 0 && v->dead_seq <= horizon) {
            v->freed = 1;
        }
    }
    pthread_mutex_unlock(&fake->lock);
}

static bool fake_is_poisoned(void *ctx)
{
    fake_t *fake = ctx;
    bool poisoned;

    pthread_mutex_lock(&fake->lock);
    poisoned = fake->backend_poisoned != 0;
    pthread_mutex_unlock(&fake->lock);
    return poisoned;
}

static void fake_close(void *ctx)
{
    fake_t *fake = ctx;

    fake->close_calls++;
}

static const txn_backend_ops_t FAKE_OPS = {fake_write_begin, fake_publish, fake_write_finish,
                                           fake_reclaim, fake_is_poisoned, fake_close};

static txn_manager_t *open_manager(fake_t *fake, txn_guarantee_t max, size_t max_readers)
{
    txn_manager_spec_t spec = {.ops = &FAKE_OPS,
                               .ctx = fake,
                               .max_guarantee = max,
                               .max_readers = max_readers,
                               .commit_ring = 4};
    txn_manager_t *manager = NULL;

    CHECK(txn_manager_open(&spec, &manager) == TXN_OK);
    fake->manager = manager;
    return manager;
}

static txn_t *begin_rw(txn_manager_t *manager)
{
    txn_t *txn = NULL;

    CHECK(txn_begin(manager, NULL, &txn) == TXN_OK);
    return txn;
}

static txn_t *begin_ro(txn_manager_t *manager)
{
    txn_begin_options_t options = {.access = TXN_ACCESS_READ_ONLY};
    txn_t *txn = NULL;

    CHECK(txn_begin(manager, &options, &txn) == TXN_OK);
    return txn;
}

static void commit_value(fake_t *fake, int key, int value)
{
    txn_t *txn = begin_rw(fake->manager);

    fake_stage(fake, key, value);
    CHECK(txn_commit(txn, NULL) == TXN_OK);
    txn_release(&txn);
}

static void test_isolation_and_rollback(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *before;
    txn_t *writer;
    txn_t *after;
    int value = 0;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    commit_value(&fake, 1, 10);

    before = begin_ro(manager);
    CHECK(txn_snapshot_seq(before) == 1);
    writer = begin_rw(manager);
    fake_stage(&fake, 1, 20);
    /* commit 전: 변경은 어떤 snapshot에도 보이지 않는다 */
    CHECK(fake_get(&fake, 1, txn_snapshot_seq(before), &value) && value == 10);
    CHECK(fake_get(&fake, 1, txn_manager_last_committed(manager), &value) && value == 10);
    CHECK(txn_commit(writer, NULL) == TXN_OK);
    txn_release(&writer);

    /* commit 뒤: 먼저 연 snapshot은 그대로, 새 snapshot은 본다 */
    CHECK(fake_get(&fake, 1, txn_snapshot_seq(before), &value) && value == 10);
    after = begin_ro(manager);
    CHECK(txn_snapshot_seq(after) == 2);
    CHECK(fake_get(&fake, 1, txn_snapshot_seq(after), &value) && value == 20);

    /* rollback: 순번도 값도 변하지 않는다 */
    writer = begin_rw(manager);
    fake_stage(&fake, 1, 99);
    CHECK(txn_rollback(writer) == TXN_OK);
    CHECK(txn_state(writer) == TXN_STATE_ROLLED_BACK);
    CHECK(txn_manager_last_committed(manager) == 2);
    CHECK(fake.finish_not_applied == 1);
    CHECK(fake_get(&fake, 1, 2, &value) && value == 20);
    CHECK(txn_commit(writer, NULL) == TXN_NOT_ACTIVE);
    CHECK(txn_rollback(writer) == TXN_NOT_ACTIVE);
    txn_release(&writer);
    txn_release(&before);
    txn_release(&after);
    CHECK(txn_manager_close(&manager) == TXN_OK);
    CHECK(manager == NULL && fake.close_calls == 1);
}

static void test_outcomes_and_poison(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *txn;
    txn_commit_info_t info;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);

    txn = begin_rw(manager);
    fake.next_outcome = TXN_OUTCOME_NOT_APPLIED;
    CHECK(txn_commit(txn, &info) == TXN_COMMIT_FAILED);
    CHECK(txn_state(txn) == TXN_STATE_FAILED && info.outcome == TXN_OUTCOME_NOT_APPLIED);
    CHECK(txn_manager_last_committed(manager) == 0 && !txn_manager_is_poisoned(manager));
    txn_release(&txn);

    fake.next_outcome = TXN_OUTCOME_APPLIED;
    txn = begin_rw(manager);
    CHECK(txn_commit(txn, &info) == TXN_OK);
    CHECK(info.outcome == TXN_OUTCOME_APPLIED && info.commit_seq == 1 &&
          info.guarantee == TXN_GUARANTEE_SYNCED);
    CHECK(txn_state(txn) == TXN_STATE_COMMITTED);
    txn_release(&txn);

    fake.next_outcome = TXN_OUTCOME_UNKNOWN;
    txn = begin_rw(manager);
    CHECK(txn_commit(txn, &info) == TXN_COMMIT_UNCERTAIN);
    CHECK(txn_state(txn) == TXN_STATE_UNCERTAIN && info.outcome == TXN_OUTCOME_UNKNOWN);
    CHECK(info.commit_seq == 1);   /* 불확실한 commit은 순번을 올리지 않는다 */
    CHECK(txn_manager_is_poisoned(manager));
    CHECK(fake.finish_unknown == 1);
    txn_release(&txn);

    CHECK(txn_begin(manager, NULL, &txn) == TXN_POISONED && txn == NULL);
    {
        txn_begin_options_t ro = {.access = TXN_ACCESS_READ_ONLY};
        CHECK(txn_begin(manager, &ro, &txn) == TXN_POISONED);
    }
    CHECK(fake.begin_calls == 3 && fake.finish_calls == 3);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

static void test_backend_poison_after_commit(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *txn;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    txn = begin_rw(manager);
    fake.backend_poisoned = 1;
    CHECK(txn_commit(txn, NULL) == TXN_OK);   /* 이번 commit은 성공으로 확정 */
    CHECK(txn_manager_is_poisoned(manager));
    txn_release(&txn);
    CHECK(txn_begin(manager, NULL, &txn) == TXN_POISONED);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

static void test_commit_lookup(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *txn;
    txn_commit_info_t info;
    uint64_t first_id;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    txn = begin_rw(manager);
    first_id = txn_id(txn);
    fake_stage(&fake, 1, 1);
    CHECK(txn_query_commit(manager, first_id, &info) == TXN_BUSY);   /* 진행 중 */
    CHECK(txn_commit(txn, NULL) == TXN_OK);   /* 호출자가 응답(info)을 버린 상황 */
    txn_release(&txn);

    memset(&info, 0, sizeof(info));
    CHECK(txn_query_commit(manager, first_id, &info) == TXN_OK);
    CHECK(info.outcome == TXN_OUTCOME_APPLIED && info.commit_seq == 1 && info.txn_id == first_id);

    for (int i = 0; i < 4; i++) {   /* 링 크기 4: 첫 항목이 밀려난다 */
        txn = begin_rw(manager);
        CHECK(txn_rollback(txn) == TXN_OK);
        txn_release(&txn);
    }
    CHECK(txn_query_commit(manager, first_id, &info) == TXN_NOT_FOUND);
    CHECK(txn_query_commit(manager, 0, &info) == TXN_UNSUPPORTED);
    CHECK(txn_query_commit(manager, 1000, &info) == TXN_UNSUPPORTED);
    CHECK(txn_query_commit(manager, first_id + 4, &info) == TXN_OK && info.outcome == TXN_OUTCOME_NOT_APPLIED);

    fake.next_outcome = TXN_OUTCOME_UNKNOWN;
    txn = begin_rw(manager);
    first_id = txn_id(txn);
    CHECK(txn_commit(txn, NULL) == TXN_COMMIT_UNCERTAIN);
    txn_release(&txn);
    CHECK(txn_query_commit(manager, first_id, &info) == TXN_OK && info.outcome == TXN_OUTCOME_UNKNOWN);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

static void test_guarantee(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *txn = NULL;
    txn_begin_options_t options = {.guarantee = TXN_GUARANTEE_DURABLE};
    txn_commit_info_t info;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    CHECK(!(txn_manager_capabilities(manager) & TXN_CAP_DURABLE));
    CHECK(txn_begin(manager, &options, &txn) == TXN_UNSUPPORTED && txn == NULL);
    CHECK(fake.begin_calls == 0);   /* 거절은 backend를 건드리지 않는다 */
    options.guarantee = (txn_guarantee_t)9;
    CHECK(txn_begin(manager, &options, &txn) == TXN_INVALID);
    options.guarantee = TXN_GUARANTEE_VISIBLE;
    CHECK(txn_begin(manager, &options, &txn) == TXN_OK);
    CHECK(txn_commit(txn, &info) == TXN_OK);
    txn_release(&txn);

    options.guarantee = TXN_GUARANTEE_SYNCED;
    fake.next_achieved = TXN_GUARANTEE_VISIBLE;   /* 약속보다 낮게 달성했다고 보고 */
    CHECK(txn_begin(manager, &options, &txn) == TXN_OK);
    CHECK(txn_commit(txn, &info) == TXN_COMMIT_UNCERTAIN);
    CHECK(info.outcome == TXN_OUTCOME_UNKNOWN && txn_manager_is_poisoned(manager));
    txn_release(&txn);
    CHECK(txn_manager_close(&manager) == TXN_OK);

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_DURABLE, 0);
    CHECK(txn_manager_capabilities(manager) & TXN_CAP_DURABLE);
    options.guarantee = TXN_GUARANTEE_DURABLE;
    fake.next_achieved = TXN_GUARANTEE_DURABLE;
    CHECK(txn_begin(manager, &options, &txn) == TXN_OK);
    CHECK(txn_commit(txn, &info) == TXN_OK && info.guarantee == TXN_GUARANTEE_DURABLE);
    txn_release(&txn);
    CHECK(txn_manager_close(&manager) == TXN_OK);

    {   /* 알 수 없는 outcome 값 */
        txn_isolation_t unsupported[] = {TXN_ISOLATION_READ_COMMITTED, TXN_ISOLATION_SERIALIZABLE};
        txn_begin_options_t iso = {0};

        fake_init(&fake);
        manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
        for (size_t i = 0; i < 2; i++) {
            iso.isolation = unsupported[i];
            CHECK(txn_begin(manager, &iso, &txn) == TXN_UNSUPPORTED);
        }
        iso.isolation = (txn_isolation_t)7;
        CHECK(txn_begin(manager, &iso, &txn) == TXN_INVALID);
        fake.next_outcome = (txn_outcome_t)42;
        txn = begin_rw(manager);
        CHECK(txn_commit(txn, NULL) == TXN_COMMIT_UNCERTAIN);
        txn_release(&txn);
        CHECK(txn_manager_close(&manager) == TXN_OK);
    }
}

/* 호출마다 20ms씩 흐르는 시계. 실제 시간과 무관하게 timeout을 확정적으로 만든다. */
static uint64_t step_clock(void *ctx)
{
    uint64_t *now = ctx;

    *now += 20;
    return *now;
}

typedef struct {
    txn_manager_t *manager;
    txn_begin_options_t options;
    txn_status_t status;
    txn_t *txn;
} waiter_t;

static void *waiter_main(void *arg)
{
    waiter_t *waiter = arg;

    waiter->status = txn_begin(waiter->manager, &waiter->options, &waiter->txn);
    return NULL;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}

static void test_contention(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_manager_spec_t spec = {.ops = &FAKE_OPS, .max_guarantee = TXN_GUARANTEE_SYNCED};
    uint64_t clock_now = 0;
    txn_t *holder;
    txn_t *other = NULL;
    txn_begin_options_t options = {0};
    txn_cancel_t cancel;
    waiter_t waiter;
    pthread_t thread;

    fake_init(&fake);
    spec.ctx = &fake;
    spec.now_ms = step_clock;
    spec.clock_ctx = &clock_now;
    CHECK(txn_manager_open(&spec, &manager) == TXN_OK);
    fake.manager = manager;

    holder = begin_rw(manager);
    CHECK(txn_begin(manager, NULL, &other) == TXN_BUSY && other == NULL);   /* 비대기 */
    options.timeout_ms = 100;
    CHECK(txn_begin(manager, &options, &other) == TXN_TIMEOUT);   /* 단조 시계가 100ms를 넘김 */
    CHECK(clock_now >= 100);
    CHECK(fake.begin_calls == 1);   /* 대기한 쪽은 backend를 건드리지 않았다 */

    /* 읽기는 writer가 있어도 막히지 않는다 */
    other = begin_ro(manager);
    CHECK(txn_snapshot_seq(other) == 0);
    txn_release(&other);

    /* 대기 중 취소 */
    txn_cancel_init(&cancel);
    waiter = (waiter_t){.manager = manager, .options = {.timeout_ms = TXN_WAIT_FOREVER, .cancel = &cancel}};
    pthread_create(&thread, NULL, waiter_main, &waiter);
    sleep_ms(30);
    txn_cancel_request(&cancel);
    pthread_join(thread, NULL);
    CHECK(waiter.status == TXN_CANCELLED && waiter.txn == NULL);

    /* writer가 끝나면 대기자가 진행한다 */
    waiter = (waiter_t){.manager = manager, .options = {.timeout_ms = TXN_WAIT_FOREVER}};
    pthread_create(&thread, NULL, waiter_main, &waiter);
    sleep_ms(30);
    CHECK(txn_rollback(holder) == TXN_OK);
    pthread_join(thread, NULL);
    CHECK(waiter.status == TXN_OK && waiter.txn != NULL);
    CHECK(fake.finish_calls == 1);
    txn_release(&holder);

    /* 대기자가 있는 동안 poison되면 POISONED로 깨어난다 */
    {
        waiter_t second = {.manager = manager, .options = {.timeout_ms = TXN_WAIT_FOREVER}};

        pthread_create(&thread, NULL, waiter_main, &second);
        sleep_ms(30);
        fake.next_outcome = TXN_OUTCOME_UNKNOWN;
        CHECK(txn_commit(waiter.txn, NULL) == TXN_COMMIT_UNCERTAIN);
        pthread_join(thread, NULL);
        CHECK(second.status == TXN_POISONED && second.txn == NULL);
    }
    txn_release(&waiter.txn);

    /* commit 시작 전 취소는 변경을 버린다 */
    fake.next_outcome = TXN_OUTCOME_APPLIED;
    CHECK(txn_manager_close(&manager) == TXN_OK);
    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    txn_cancel_init(&cancel);
    options = (txn_begin_options_t){.cancel = &cancel};
    CHECK(txn_begin(manager, &options, &holder) == TXN_OK);
    fake_stage(&fake, 5, 5);
    txn_cancel_request(&cancel);
    {
        txn_commit_info_t info;

        CHECK(txn_commit(holder, &info) == TXN_CANCELLED);
        CHECK(info.outcome == TXN_OUTCOME_NOT_APPLIED);
    }
    CHECK(fake.publish_calls == 0 && fake.finish_not_applied == 1);
    CHECK(txn_state(holder) == TXN_STATE_ROLLED_BACK);
    CHECK(txn_manager_last_committed(manager) == 0);
    txn_release(&holder);
    CHECK(txn_begin(manager, &options, &holder) == TXN_CANCELLED);   /* 취소 뒤 begin도 거절 */
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

static void test_lifetime(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *writer;
    txn_t *reader;
    txn_t *none = NULL;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    writer = begin_rw(manager);
    reader = begin_ro(manager);
    CHECK(txn_manager_close(&manager) == TXN_BUSY && manager != NULL);
    CHECK(fake.close_calls == 0);

    CHECK(txn_release(&writer) == TXN_OK);   /* ACTIVE는 암묵 rollback */
    CHECK(writer == NULL && fake.finish_calls == 1 && fake.finish_not_applied == 1);
    CHECK(txn_release(&writer) == TXN_OK);   /* 반복 release는 NULL을 보고 무동작 */
    CHECK(fake.finish_calls == 1);
    CHECK(txn_manager_close(&manager) == TXN_BUSY);
    CHECK(txn_release(&reader) == TXN_OK && reader == NULL);
    CHECK(txn_release(&none) == TXN_OK);
    CHECK(txn_manager_close(&manager) == TXN_OK && manager == NULL);
    CHECK(fake.close_calls == 1);
    CHECK(txn_manager_close(&manager) == TXN_OK && fake.close_calls == 1);

    /* write_begin 실패: 슬롯이 풀리고 finish는 호출되지 않는다 */
    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    fake.fail_begin = 1;
    CHECK(txn_begin(manager, NULL, &writer) == TXN_IO && writer == NULL);
    CHECK(fake.finish_calls == 0);
    fake.fail_begin = 0;
    writer = begin_rw(manager);
    CHECK(txn_release(&writer) == TXN_OK);
    CHECK(fake.begin_calls == 2 && fake.finish_calls == 1);
    CHECK(txn_manager_close(&manager) == TXN_OK);

    /* 잘못된 인자 */
    {
        txn_manager_spec_t spec = {0};
        txn_manager_t *none_manager = (txn_manager_t *)1;

        CHECK(txn_manager_open(&spec, &none_manager) == TXN_INVALID && none_manager == NULL);
        CHECK(txn_manager_open(NULL, NULL) == TXN_INVALID);
        spec = (txn_manager_spec_t){.ops = &FAKE_OPS, .max_guarantee = (txn_guarantee_t)5};
        CHECK(txn_manager_open(&spec, &none_manager) == TXN_INVALID);
        CHECK(txn_begin(NULL, NULL, &none) == TXN_INVALID);
        CHECK(txn_commit(NULL, NULL) == TXN_INVALID);
        CHECK(txn_rollback(NULL) == TXN_INVALID);
        CHECK(txn_state(NULL) == TXN_STATE_NONE && txn_id(NULL) == 0);
    }
}

static void test_readers_and_horizon(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *r1;
    txn_t *r2;
    txn_t *r3;
    txn_t *over = NULL;
    txn_t *writer;
    int value;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 2);
    commit_value(&fake, 1, 10);   /* seq 1 */
    CHECK(txn_manager_oldest_snapshot(manager) == 1);

    r1 = begin_ro(manager);   /* snapshot 1 */
    commit_value(&fake, 1, 20);   /* seq 2: 옛 버전은 r1이 쓸 수 있다 */
    r2 = begin_ro(manager);   /* snapshot 2 */
    CHECK(txn_begin(manager, &(txn_begin_options_t){.access = TXN_ACCESS_READ_ONLY}, &over) == TXN_LIMIT);
    CHECK(over == NULL);
    CHECK(txn_manager_oldest_snapshot(manager) == 1);

    CHECK(fake.versions[0].dead_seq == 2 && !fake.versions[0].freed);   /* 즉시 해제 금지 */
    CHECK(fake_get(&fake, 1, txn_snapshot_seq(r1), &value) && value == 10);

    commit_value(&fake, 1, 30);   /* seq 3 */
    CHECK(!fake.versions[0].freed && !fake.versions[1].freed);

    txn_release(&r1);   /* 하한이 2로 오른다: seq 2가 대체한 옛 버전(10)만 해제 */
    CHECK(txn_manager_oldest_snapshot(manager) == 2);
    CHECK(fake.versions[0].freed && !fake.versions[1].freed);
    CHECK(fake_get(&fake, 1, txn_snapshot_seq(r2), &value) && value == 20);

    r3 = begin_ro(manager);   /* 한도 해제 확인 */
    CHECK(r3 != NULL);
    writer = begin_rw(manager);   /* writer의 snapshot도 하한에 들어간다 */
    CHECK(txn_snapshot_seq(writer) == 3 && txn_manager_oldest_snapshot(manager) == 2);
    txn_release(&r2);
    txn_release(&r3);
    CHECK(txn_manager_oldest_snapshot(manager) == 3);
    CHECK(fake.versions[1].freed && !fake.versions[2].freed);
    txn_release(&writer);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

static void test_reentrancy(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *txn;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    fake.reenter_probe = 1;
    txn = begin_rw(manager);
    CHECK(txn_commit(txn, NULL) == TXN_OK);   /* callback 안의 쓰기 API가 거절되어 교착 없이 끝난다 */
    CHECK(fake.reenter_results[0] == TXN_REENTRANT && fake.reenter_results[1] == TXN_REENTRANT);
    CHECK(fake.reenter_results[2] == 1 && fake.reenter_results[3] == 1);
    txn_release(&txn);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

/* writer의 id 상위 32비트가 manager 세대다. */
static uint32_t writer_epoch(txn_manager_t *manager)
{
    txn_t *txn = begin_rw(manager);
    uint32_t epoch = (uint32_t)(txn_id(txn) >> 32);

    txn_release(&txn);
    return epoch;
}

#define EXPLICIT_EPOCH 0x70000000u

static void test_epoch(void)
{
    fake_t fake;
    txn_manager_t *first;
    txn_manager_t *second;
    txn_manager_t *third = (txn_manager_t *)1;
    txn_manager_spec_t spec = {.ops = &FAKE_OPS, .ctx = &fake, .max_guarantee = TXN_GUARANTEE_SYNCED};
    txn_commit_info_t info;
    txn_t *txn;
    uint64_t old_id;
    uint64_t new_id;
    uint32_t e1;
    uint32_t e2;

    fake_init(&fake);
    first = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    txn = begin_rw(first);
    old_id = txn_id(txn);
    CHECK(txn_commit(txn, NULL) == TXN_OK);
    txn_release(&txn);
    CHECK(txn_manager_close(&first) == TXN_OK);

    /* 재open: 일련번호는 둘 다 1이지만 세대가 달라 id가 겹치지 않고 옛 id는 UNSUPPORTED다 */
    second = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    txn = begin_rw(second);
    new_id = txn_id(txn);
    CHECK((uint32_t)old_id == (uint32_t)new_id && old_id != new_id);
    CHECK((new_id >> 32) > (old_id >> 32));   /* 자동 발급은 단조 증가 */
    CHECK(txn_query_commit(second, old_id, &info) == TXN_UNSUPPORTED);
    CHECK(txn_commit(txn, NULL) == TXN_OK);
    txn_release(&txn);
    CHECK(txn_query_commit(second, new_id, &info) == TXN_OK && info.txn_id == new_id);
    CHECK(txn_query_commit(second, new_id + 1, &info) == TXN_NOT_FOUND);
    CHECK(txn_manager_close(&second) == TXN_OK);

    /* 호출자가 지정한 세대는 그대로 쓰인다. 자동 발급이 닿지 않는 큰 값을 쓴다. */
    spec.epoch = EXPLICIT_EPOCH;
    CHECK(txn_manager_open(&spec, &first) == TXN_OK);
    spec.epoch = EXPLICIT_EPOCH + 1;
    CHECK(txn_manager_open(&spec, &second) == TXN_OK);
    CHECK(txn_begin(first, NULL, &txn) == TXN_OK && (txn_id(txn) >> 32) == EXPLICIT_EPOCH);
    CHECK(txn_query_commit(second, txn_id(txn), &info) == TXN_UNSUPPORTED);
    txn_release(&txn);

    /* 같은 프로세스에서 쓴 세대의 재사용은 열려 있든 닫혔든 거절한다 */
    spec.epoch = EXPLICIT_EPOCH;
    CHECK(txn_manager_open(&spec, &third) == TXN_INVALID && third == NULL);
    CHECK(txn_manager_close(&first) == TXN_OK && txn_manager_close(&second) == TXN_OK);
    third = (txn_manager_t *)1;
    CHECK(txn_manager_open(&spec, &third) == TXN_INVALID && third == NULL);

    /* 상한보다 작은 지정도 거절한다 */
    spec.epoch = 5;
    third = (txn_manager_t *)1;
    CHECK(txn_manager_open(&spec, &third) == TXN_INVALID && third == NULL);

    /* 자동 발급은 상한 + 1로 계속 증가하고, 상한을 뛰어넘는 지정 뒤에는 그 다음 번호를 받는다 */
    first = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    e1 = writer_epoch(first);
    CHECK(txn_manager_close(&first) == TXN_OK);
    spec.epoch = e1 + 1;
    CHECK(txn_manager_open(&spec, &first) == TXN_OK);
    second = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    e2 = writer_epoch(second);
    CHECK(e2 == e1 + 2);
    CHECK(txn_manager_close(&first) == TXN_OK && txn_manager_close(&second) == TXN_OK);
}

#define EPOCH_THREADS 8
#define EPOCH_OPENS 50

typedef struct {
    uint32_t epochs[EPOCH_OPENS];
    int failures_seen;
} epoch_worker_t;

static txn_status_t nop_begin(void *ctx, uint64_t base, void **write)
{
    (void)ctx;
    (void)base;
    *write = NULL;
    return TXN_OK;
}

static txn_publish_result_t nop_publish(void *ctx, void *write, uint64_t seq, txn_guarantee_t requested)
{
    (void)ctx;
    (void)write;
    (void)seq;
    (void)requested;
    return (txn_publish_result_t){TXN_OUTCOME_APPLIED, TXN_GUARANTEE_SYNCED};
}

static void nop_finish(void *ctx, void *write, txn_outcome_t outcome, uint64_t seq)
{
    (void)ctx;
    (void)write;
    (void)outcome;
    (void)seq;
}

static const txn_backend_ops_t NOP_OPS = {nop_begin, nop_publish, nop_finish, NULL, NULL, NULL};

/* 동시에 manager를 열고 닫으며 자동 세대를 받는다. 결과는 스레드별 배열에만 쓴다. */
static void *epoch_worker_main(void *arg)
{
    epoch_worker_t *worker = arg;
    txn_manager_spec_t spec = {.ops = &NOP_OPS, .max_guarantee = TXN_GUARANTEE_SYNCED};

    for (int i = 0; i < EPOCH_OPENS; i++) {
        txn_manager_t *manager = NULL;
        txn_t *txn = NULL;

        if (txn_manager_open(&spec, &manager) != TXN_OK || txn_begin(manager, NULL, &txn) != TXN_OK) {
            worker->failures_seen++;
            continue;
        }
        worker->epochs[i] = (uint32_t)(txn_id(txn) >> 32);
        txn_release(&txn);
        txn_manager_close(&manager);
    }
    return NULL;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;

    return x < y ? -1 : x > y;
}

static void test_epoch_concurrent_open(void)
{
    static epoch_worker_t workers[EPOCH_THREADS];
    static uint32_t all[EPOCH_THREADS * EPOCH_OPENS];
    pthread_t threads[EPOCH_THREADS];
    int duplicates = 0;
    int seen_failures = 0;

    for (int i = 0; i < EPOCH_THREADS; i++) {
        pthread_create(&threads[i], NULL, epoch_worker_main, &workers[i]);
    }
    for (int i = 0; i < EPOCH_THREADS; i++) {
        pthread_join(threads[i], NULL);
        seen_failures += workers[i].failures_seen;
        for (int j = 0; j < EPOCH_OPENS; j++) {
            all[i * EPOCH_OPENS + j] = workers[i].epochs[j];
        }
    }
    CHECK(seen_failures == 0);
    qsort(all, EPOCH_THREADS * EPOCH_OPENS, sizeof(all[0]), compare_u32);
    for (int i = 1; i < EPOCH_THREADS * EPOCH_OPENS; i++) {
        if (all[i] == all[i - 1]) {
            duplicates++;
        }
    }
    CHECK(all[0] != 0 && duplicates == 0);   /* 동시 open에서도 세대가 겹치지 않는다 */
}

/* 호출마다 20ms 흐르는 시계로 timeout·취소·writer 슬롯 보존을 실제 시간과 무관하게 확정한다. */
static void test_timeout_deterministic(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_manager_spec_t spec = {.ops = &FAKE_OPS, .max_guarantee = TXN_GUARANTEE_SYNCED};
    uint64_t clock_now = 0;
    txn_cancel_t cancel;
    txn_begin_options_t options = {.timeout_ms = 50};
    txn_t *holder;
    txn_t *other = (txn_t *)1;
    txn_t *next = NULL;

    fake_init(&fake);
    spec.ctx = &fake;
    spec.now_ms = step_clock;
    spec.clock_ctx = &clock_now;
    CHECK(txn_manager_open(&spec, &manager) == TXN_OK);
    fake.manager = manager;

    holder = begin_rw(manager);
    CHECK(txn_begin(manager, &options, &other) == TXN_TIMEOUT && other == NULL);
    CHECK(clock_now >= 50);
    CHECK(txn_state(holder) == TXN_STATE_ACTIVE);   /* timeout이 writer 슬롯을 빼앗지 않는다 */
    CHECK(fake.begin_calls == 1 && fake.finish_calls == 0);

    /* 이미 취소된 요청은 기다리지 않고 CANCELLED이며 슬롯도 그대로다 */
    txn_cancel_init(&cancel);
    txn_cancel_request(&cancel);
    options = (txn_begin_options_t){.timeout_ms = TXN_WAIT_FOREVER, .cancel = &cancel};
    other = (txn_t *)1;
    CHECK(txn_begin(manager, &options, &other) == TXN_CANCELLED && other == NULL);
    CHECK(txn_state(holder) == TXN_STATE_ACTIVE && fake.begin_calls == 1);

    CHECK(txn_commit(holder, NULL) == TXN_OK);   /* 슬롯을 지킨 writer가 정상 commit */
    txn_release(&holder);
    CHECK(txn_begin(manager, &(txn_begin_options_t){.timeout_ms = 50}, &next) == TXN_OK);
    CHECK(fake.begin_calls == 2);
    txn_release(&next);
    CHECK(fake.finish_calls == fake.begin_calls);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

/*
 * 실제 단조 대기 경로가 timeout으로 끝나는지만 본다. 걸린 시간의 상한·하한은 CI 판정에 넣지 않는다(느린 실행기에서
 * 거짓 실패가 난다). 응답하지 않는 hang은 외부 test runner의 watchdog이 잡는다. 시간 계측은
 * tests/measure_transaction_timeout.c로 로컬에서 따로 본다.
 */
static void test_real_timeout_completes(void)
{
    fake_t fake;
    txn_manager_t *manager;
    txn_t *holder;
    txn_t *other = NULL;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    holder = begin_rw(manager);
    CHECK(txn_begin(manager, &(txn_begin_options_t){.timeout_ms = 60}, &other) == TXN_TIMEOUT);
    CHECK(other == NULL && txn_state(holder) == TXN_STATE_ACTIVE);
    CHECK(txn_wait_is_monotonic());
    txn_release(&holder);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

#define STRESS_THREADS 4
#define STRESS_ROUNDS 200

/* 스레드마다 자기 구조체만 쓴다. 합계는 join 뒤 main이 낸다. */
typedef struct {
    txn_manager_t *manager;
    int committed;
    int busy;
    int bad_snapshots;
} stress_t;

static void *stress_writer(void *arg)
{
    stress_t *stress = arg;

    for (int i = 0; i < STRESS_ROUNDS; i++) {
        txn_begin_options_t options = {.timeout_ms = TXN_WAIT_FOREVER};
        txn_t *txn = NULL;

        if (txn_begin(stress->manager, &options, &txn) != TXN_OK) {
            stress->busy++;
            continue;
        }
        if (txn_commit(txn, NULL) == TXN_OK) {
            stress->committed++;
        }
        txn_release(&txn);
    }
    return NULL;
}

static void *stress_reader(void *arg)
{
    stress_t *stress = arg;

    for (int i = 0; i < STRESS_ROUNDS * 4; i++) {
        txn_t *txn = NULL;

        if (txn_begin(stress->manager, &(txn_begin_options_t){.access = TXN_ACCESS_READ_ONLY}, &txn) ==
            TXN_OK) {
            if (txn_snapshot_seq(txn) > txn_manager_last_committed(stress->manager)) {
                stress->bad_snapshots++;
            }
            txn_commit(txn, NULL);
            txn_release(&txn);
        }
    }
    return NULL;
}

static void test_threads(void)
{
    fake_t fake;
    txn_manager_t *manager;
    stress_t writer_stress[STRESS_THREADS] = {0};
    stress_t reader_stress[STRESS_THREADS] = {0};
    pthread_t writers[STRESS_THREADS];
    pthread_t readers[STRESS_THREADS];
    int busy = 0;
    int committed = 0;
    int bad_snapshots = 0;

    fake_init(&fake);
    manager = open_manager(&fake, TXN_GUARANTEE_SYNCED, 0);
    for (int i = 0; i < STRESS_THREADS; i++) {
        writer_stress[i].manager = manager;
        reader_stress[i].manager = manager;
        pthread_create(&writers[i], NULL, stress_writer, &writer_stress[i]);
        pthread_create(&readers[i], NULL, stress_reader, &reader_stress[i]);
    }
    for (int i = 0; i < STRESS_THREADS; i++) {
        pthread_join(writers[i], NULL);
        pthread_join(readers[i], NULL);
        busy += writer_stress[i].busy;
        committed += writer_stress[i].committed;
        bad_snapshots += reader_stress[i].bad_snapshots;
    }
    CHECK(bad_snapshots == 0);
    CHECK(committed == fake.finish_applied);
    CHECK(fake.max_writers_inside == 1);   /* backend 안에 writer가 둘 들어간 적 없다 */
    CHECK(fake.begin_calls == fake.finish_calls);
    CHECK(txn_manager_last_committed(manager) == (uint64_t)fake.finish_applied);
    CHECK(fake.finish_applied == STRESS_THREADS * STRESS_ROUNDS - busy);
    CHECK(txn_manager_oldest_snapshot(manager) == txn_manager_last_committed(manager));
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

int main(void)
{
    test_isolation_and_rollback();
    test_outcomes_and_poison();
    test_backend_poison_after_commit();
    test_commit_lookup();
    test_guarantee();
    test_contention();
    test_lifetime();
    test_readers_and_horizon();
    test_reentrancy();
    test_epoch();
    test_epoch_concurrent_open();
    test_timeout_deterministic();
    test_real_timeout_completes();
    test_threads();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("test_transaction: all passed");
    return 0;
}
