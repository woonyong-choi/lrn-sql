#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE   /* pthread_cond_timedwait_relative_np */
#endif

#include "storage/transaction.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/*
 * 대기 시계. writer 대기는 deadline을 단조 시계(now_ms)로 재고, 조건 변수 대기는 짧은 조각으로 나눈다.
 * 조각 대기 자체가 단조여야 벽시계 역행 때 한 조각이 길어지지 않는다.
 *  - macOS: pthread_cond_timedwait_relative_np(상대 시간)
 *  - Linux: 조건 변수를 CLOCK_MONOTONIC으로 만들고 같은 시계의 절대 시각으로 대기
 *  - 그 밖: CLOCK_REALTIME 절대 시각 대기로 후퇴한다. 이때 벽시계가 크게 역행하면 한 조각이 길어질 수 있어
 *    txn_wait_is_monotonic()이 false를 돌려준다.
 */
#if defined(__APPLE__)
#define TXN_WAIT_KIND 1
#elif defined(__linux__)
#define TXN_WAIT_KIND 2
#else
#define TXN_WAIT_KIND 0
#endif

/* 대기 중 취소·시계를 다시 보는 간격. 단조 시계로 deadline을 재므로 벽시계 변경은 이 간격에만 영향을 준다. */
#define WAIT_SLICE_NS 5000000L

struct txn {
    txn_manager_t *manager;
    uint64_t id;
    uint64_t snapshot_seq;
    txn_state_t state;
    txn_access_t access;
    txn_guarantee_t guarantee;
    const txn_cancel_t *cancel;
    void *write;
    bool linked;
    txn_t *prev;
    txn_t *next;
};

typedef struct {
    uint64_t txn_id;
    txn_commit_info_t info;
} ring_entry_t;

struct txn_manager {
    txn_backend_ops_t ops;
    void *ctx;
    txn_guarantee_t max_guarantee;
    uint64_t (*now_ms)(void *clock_ctx);
    void *clock_ctx;

    pthread_mutex_t mutex;
    pthread_cond_t writer_free;
    pthread_mutex_t reclaim_mutex;   /* 잠금 순서: reclaim_mutex -> mutex */

    uint64_t last_committed;
    uint32_t epoch;                  /* 이 manager 세대. id 상위 32비트 */
    uint32_t next_serial;            /* 이 세대에서 발급한 id 수 + 1 */
    bool writer_active;
    uint64_t writer_id;
    bool poisoned;
    size_t readers;
    size_t max_readers;
    size_t live_handles;
    txn_t *active;                   /* 열린 snapshot 목록(reader와 writer) */

    ring_entry_t *ring;
    size_t ring_cap;
    size_t ring_count;
    size_t ring_next;
    uint64_t last_reclaim;           /* reclaim_mutex로 보호 */
};

/* 현재 스레드가 어느 manager의 backend callback 안에 있는지. 쓰기 API 재진입 거절에 쓴다. */
static _Thread_local const txn_manager_t *callback_owner;

void txn_cancel_init(txn_cancel_t *cancel) { atomic_init(&cancel->requested, false); }

void txn_cancel_request(txn_cancel_t *cancel)
{
    atomic_store_explicit(&cancel->requested, true, memory_order_release);
}

bool txn_cancel_requested(const txn_cancel_t *cancel)
{
    return cancel != NULL && atomic_load_explicit(&cancel->requested, memory_order_acquire);
}

static uint64_t monotonic_ms(void *clock_ctx)
{
    struct timespec now;

    (void)clock_ctx;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool in_callback(const txn_manager_t *manager) { return callback_owner == manager; }

static const txn_manager_t *enter_callback(const txn_manager_t *manager)
{
    const txn_manager_t *saved = callback_owner;

    callback_owner = manager;
    return saved;
}

static void leave_callback(const txn_manager_t *saved) { callback_owner = saved; }

bool txn_wait_is_monotonic(void) { return TXN_WAIT_KIND != 0; }

static int init_cond(pthread_cond_t *cond)
{
#if TXN_WAIT_KIND == 2
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);

    if (rc != 0) {
        return rc;
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(cond, &attr);
    }
    pthread_condattr_destroy(&attr);
    return rc;
#else
    return pthread_cond_init(cond, NULL);
#endif
}

static void wait_slice(txn_manager_t *manager)
{
#if TXN_WAIT_KIND == 1
    struct timespec slice = {0, WAIT_SLICE_NS};

    pthread_cond_timedwait_relative_np(&manager->writer_free, &manager->mutex, &slice);
#else
    struct timespec until;

    clock_gettime(TXN_WAIT_KIND == 2 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &until);
    until.tv_nsec += WAIT_SLICE_NS;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec += 1;
        until.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&manager->writer_free, &manager->mutex, &until);
#endif
}

/*
 * 프로세스 안에서 쓴 세대 번호 기록. 자동 발급은 이 기록에 없는 다음 번호를 단조 증가로 고르고, 호출자가 지정한
 * 번호가 이미 기록에 있으면 거절한다. 기록은 manager가 닫혀도 프로세스가 끝날 때까지 남으므로 같은 프로세스에서
 * 한 번 쓴 번호는 다시 쓰이지 않는다. 프로세스를 넘는 유일성은 이 기록이 보장하지 못한다. 재시작하면 자동 발급은
 * 1부터 다시 시작하므로, 재시작 경계에서는 호출자가 backend에 저장한 증가하는 세대 번호를 넘겨야 한다.
 */
static pthread_mutex_t epoch_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *epoch_used;
static size_t epoch_used_count;
static size_t epoch_used_cap;
static uint64_t epoch_next_auto = 1;   /* UINT32_MAX를 넘으면 자동 발급이 소진된 것이다 */

static bool epoch_seen_locked(uint32_t epoch)
{
    for (size_t i = 0; i < epoch_used_count; i++) {
        if (epoch_used[i] == epoch) {
            return true;
        }
    }
    return false;
}

/* requested가 0이면 자동 발급. 중복은 INVALID, 자동 발급 소진은 LIMIT, 기록 확장 실패는 NOMEM이다. */
static txn_status_t epoch_reserve(uint32_t requested, uint32_t *out)
{
    txn_status_t status = TXN_OK;
    uint32_t epoch;

    pthread_mutex_lock(&epoch_mutex);
    if (epoch_used_count == epoch_used_cap) {
        size_t cap = epoch_used_cap != 0 ? epoch_used_cap * 2 : 16;
        uint32_t *grown = realloc(epoch_used, cap * sizeof(*grown));

        if (grown == NULL) {
            pthread_mutex_unlock(&epoch_mutex);
            return TXN_NOMEM;
        }
        epoch_used = grown;
        epoch_used_cap = cap;
    }
    if (requested != 0) {
        epoch = requested;
        if (epoch_seen_locked(epoch)) {
            status = TXN_INVALID;
        }
    } else {
        while (epoch_next_auto <= UINT32_MAX && epoch_seen_locked((uint32_t)epoch_next_auto)) {
            epoch_next_auto++;
        }
        if (epoch_next_auto > UINT32_MAX) {
            status = TXN_LIMIT;
            epoch = 0;
        } else {
            epoch = (uint32_t)epoch_next_auto++;
        }
    }
    if (status == TXN_OK) {
        epoch_used[epoch_used_count++] = epoch;
        *out = epoch;
    }
    pthread_mutex_unlock(&epoch_mutex);
    return status;
}

/* 0이면 이 세대의 id가 소진된 것이다. */
static uint64_t next_id_locked(txn_manager_t *manager)
{
    if (manager->next_serial == 0) {
        return 0;
    }
    return ((uint64_t)manager->epoch << 32) | manager->next_serial++;
}

static void link_active(txn_manager_t *manager, txn_t *txn)
{
    txn->prev = NULL;
    txn->next = manager->active;
    if (manager->active != NULL) {
        manager->active->prev = txn;
    }
    manager->active = txn;
    txn->linked = true;
}

static void unlink_active(txn_manager_t *manager, txn_t *txn)
{
    if (!txn->linked) {
        return;
    }
    if (txn->prev != NULL) {
        txn->prev->next = txn->next;
    } else {
        manager->active = txn->next;
    }
    if (txn->next != NULL) {
        txn->next->prev = txn->prev;
    }
    txn->linked = false;
}

static uint64_t horizon_locked(const txn_manager_t *manager)
{
    uint64_t oldest = manager->last_committed;

    for (const txn_t *txn = manager->active; txn != NULL; txn = txn->next) {
        if (txn->snapshot_seq < oldest) {
            oldest = txn->snapshot_seq;
        }
    }
    return oldest;
}

static void ring_add_locked(txn_manager_t *manager, const txn_t *txn, const txn_commit_info_t *info)
{
    ring_entry_t *slot = &manager->ring[manager->ring_next];

    slot->txn_id = txn->id;
    slot->info = *info;
    manager->ring_next = (manager->ring_next + 1) % manager->ring_cap;
    if (manager->ring_count < manager->ring_cap) {
        manager->ring_count++;
    }
}

/* horizon이 올랐으면 backend에 알린다. mutex를 잡지 않고 부르며 reclaim끼리는 직렬화된다. */
static void notify_reclaim(txn_manager_t *manager)
{
    const txn_manager_t *saved;
    uint64_t horizon;

    if (manager->ops.reclaim == NULL) {
        return;
    }
    pthread_mutex_lock(&manager->reclaim_mutex);
    pthread_mutex_lock(&manager->mutex);
    horizon = horizon_locked(manager);
    pthread_mutex_unlock(&manager->mutex);
    if (horizon > manager->last_reclaim) {
        manager->last_reclaim = horizon;
        saved = enter_callback(manager);
        manager->ops.reclaim(manager->ctx, horizon);
        leave_callback(saved);
    }
    pthread_mutex_unlock(&manager->reclaim_mutex);
}

static bool guarantee_valid(txn_guarantee_t guarantee)
{
    return guarantee >= TXN_GUARANTEE_VISIBLE && guarantee <= TXN_GUARANTEE_DURABLE;
}

txn_status_t txn_manager_open(const txn_manager_spec_t *spec, txn_manager_t **out)
{
    txn_manager_t *manager;
    size_t ring_cap;
    size_t max_readers;
    txn_status_t status;
    uint32_t epoch;

    if (out == NULL) {
        return TXN_INVALID;
    }
    *out = NULL;
    if (spec == NULL || spec->ops == NULL || spec->ops->write_begin == NULL ||
        spec->ops->publish == NULL || spec->ops->write_finish == NULL ||
        !guarantee_valid(spec->max_guarantee) || spec->max_readers > TXN_MAX_MAX_READERS ||
        spec->commit_ring > TXN_MAX_COMMIT_RING || spec->initial_seq == UINT64_MAX) {
        return TXN_INVALID;
    }
    ring_cap = spec->commit_ring != 0 ? spec->commit_ring : TXN_DEFAULT_COMMIT_RING;
    max_readers = spec->max_readers != 0 ? spec->max_readers : TXN_DEFAULT_MAX_READERS;
    manager = calloc(1, sizeof(*manager));
    if (manager == NULL) {
        return TXN_NOMEM;
    }
    manager->ring = calloc(ring_cap, sizeof(*manager->ring));
    if (manager->ring == NULL) {
        free(manager);
        return TXN_NOMEM;
    }
    status = TXN_NOMEM;
    if (pthread_mutex_init(&manager->mutex, NULL) != 0) {
        goto fail_ring;
    }
    if (init_cond(&manager->writer_free) != 0) {
        goto fail_mutex;
    }
    if (pthread_mutex_init(&manager->reclaim_mutex, NULL) != 0) {
        goto fail_cond;
    }
    /* 마지막에 예약한다. 앞선 할당이 실패해도 세대 번호를 소모하지 않는다. */
    status = epoch_reserve(spec->epoch, &epoch);
    if (status != TXN_OK) {
        goto fail_reclaim_mutex;
    }
    manager->ops = *spec->ops;
    manager->ctx = spec->ctx;
    manager->max_guarantee = spec->max_guarantee;
    manager->now_ms = spec->now_ms != NULL ? spec->now_ms : monotonic_ms;
    manager->clock_ctx = spec->clock_ctx;
    manager->last_committed = spec->initial_seq;
    manager->last_reclaim = spec->initial_seq;
    manager->epoch = epoch;
    manager->next_serial = 1;
    manager->max_readers = max_readers;
    manager->ring_cap = ring_cap;
    *out = manager;
    return TXN_OK;

fail_reclaim_mutex:
    pthread_mutex_destroy(&manager->reclaim_mutex);
fail_cond:
    pthread_cond_destroy(&manager->writer_free);
fail_mutex:
    pthread_mutex_destroy(&manager->mutex);
fail_ring:
    free(manager->ring);
    free(manager);
    return status;
}

txn_status_t txn_manager_close(txn_manager_t **manager_ptr)
{
    txn_manager_t *manager;
    bool busy;

    if (manager_ptr == NULL) {
        return TXN_INVALID;
    }
    manager = *manager_ptr;
    if (manager == NULL) {
        return TXN_OK;
    }
    if (in_callback(manager)) {
        return TXN_REENTRANT;
    }
    pthread_mutex_lock(&manager->mutex);
    busy = manager->live_handles != 0;
    pthread_mutex_unlock(&manager->mutex);
    if (busy) {
        return TXN_BUSY;
    }
    if (manager->ops.close != NULL) {
        const txn_manager_t *saved = enter_callback(manager);

        manager->ops.close(manager->ctx);
        leave_callback(saved);
    }
    pthread_mutex_destroy(&manager->reclaim_mutex);
    pthread_cond_destroy(&manager->writer_free);
    pthread_mutex_destroy(&manager->mutex);
    free(manager->ring);
    free(manager);
    *manager_ptr = NULL;
    return TXN_OK;
}

static txn_status_t validate_options(const txn_manager_t *manager, const txn_begin_options_t *options)
{
    if ((options->access != TXN_ACCESS_READ_WRITE && options->access != TXN_ACCESS_READ_ONLY) ||
        (options->isolation != TXN_ISOLATION_SNAPSHOT &&
         options->isolation != TXN_ISOLATION_READ_COMMITTED &&
         options->isolation != TXN_ISOLATION_SERIALIZABLE) ||
        !guarantee_valid(options->guarantee)) {
        return TXN_INVALID;
    }
    if (options->isolation != TXN_ISOLATION_SNAPSHOT || options->guarantee > manager->max_guarantee) {
        return TXN_UNSUPPORTED;
    }
    return TXN_OK;
}

/* mutex를 잡고 호출한다. writer 슬롯이 비기를 기다리며 취소·timeout·poison을 구분한다. */
static txn_status_t wait_for_writer_locked(txn_manager_t *manager, const txn_begin_options_t *options)
{
    bool timed = options->timeout_ms != 0 && options->timeout_ms != TXN_WAIT_FOREVER;
    uint64_t deadline = timed ? manager->now_ms(manager->clock_ctx) + options->timeout_ms : 0;

    for (;;) {
        if (manager->poisoned) {
            return TXN_POISONED;
        }
        if (txn_cancel_requested(options->cancel)) {
            return TXN_CANCELLED;
        }
        if (!manager->writer_active) {
            return TXN_OK;
        }
        if (options->timeout_ms == 0) {
            return TXN_BUSY;
        }
        if (timed && manager->now_ms(manager->clock_ctx) >= deadline) {
            return TXN_TIMEOUT;
        }
        wait_slice(manager);
    }
}

static txn_status_t begin_reader(txn_manager_t *manager, txn_t *txn)
{
    pthread_mutex_lock(&manager->mutex);
    if (manager->poisoned) {
        pthread_mutex_unlock(&manager->mutex);
        return TXN_POISONED;
    }
    if (manager->readers >= manager->max_readers) {
        pthread_mutex_unlock(&manager->mutex);
        return TXN_LIMIT;
    }
    txn->id = next_id_locked(manager);
    if (txn->id == 0) {
        pthread_mutex_unlock(&manager->mutex);
        return TXN_LIMIT;
    }
    manager->readers++;
    txn->snapshot_seq = manager->last_committed;
    link_active(manager, txn);
    manager->live_handles++;
    pthread_mutex_unlock(&manager->mutex);
    return TXN_OK;
}

static void release_writer_slot(txn_manager_t *manager)
{
    pthread_mutex_lock(&manager->mutex);
    manager->writer_active = false;
    pthread_cond_broadcast(&manager->writer_free);
    pthread_mutex_unlock(&manager->mutex);
}

static txn_status_t begin_writer(txn_manager_t *manager, const txn_begin_options_t *options, txn_t *txn)
{
    const txn_manager_t *saved;
    txn_status_t status;

    pthread_mutex_lock(&manager->mutex);
    status = wait_for_writer_locked(manager, options);
    if (status != TXN_OK) {
        pthread_mutex_unlock(&manager->mutex);
        return status;
    }
    txn->id = next_id_locked(manager);
    if (txn->id == 0) {
        pthread_mutex_unlock(&manager->mutex);
        return TXN_LIMIT;
    }
    manager->writer_active = true;
    manager->writer_id = txn->id;
    txn->snapshot_seq = manager->last_committed;
    link_active(manager, txn);
    manager->live_handles++;
    pthread_mutex_unlock(&manager->mutex);

    saved = enter_callback(manager);
    status = manager->ops.write_begin(manager->ctx, txn->snapshot_seq, &txn->write);
    leave_callback(saved);
    if (status == TXN_OK) {
        return TXN_OK;
    }
    if (status == TXN_COMMIT_FAILED || status == TXN_COMMIT_UNCERTAIN) {
        status = TXN_IO;
    }
    pthread_mutex_lock(&manager->mutex);
    unlink_active(manager, txn);
    manager->live_handles--;
    manager->writer_active = false;
    pthread_cond_broadcast(&manager->writer_free);
    pthread_mutex_unlock(&manager->mutex);
    return status;
}

txn_status_t txn_begin(txn_manager_t *manager, const txn_begin_options_t *options, txn_t **out)
{
    static const txn_begin_options_t defaults = {0};
    txn_status_t status;
    txn_t *txn;

    if (out == NULL) {
        return TXN_INVALID;
    }
    *out = NULL;
    if (manager == NULL) {
        return TXN_INVALID;
    }
    if (in_callback(manager)) {
        return TXN_REENTRANT;
    }
    if (options == NULL) {
        options = &defaults;
    }
    status = validate_options(manager, options);
    if (status != TXN_OK) {
        return status;
    }
    txn = calloc(1, sizeof(*txn));
    if (txn == NULL) {
        return TXN_NOMEM;
    }
    txn->manager = manager;
    txn->state = TXN_STATE_ACTIVE;
    txn->access = options->access;
    txn->guarantee = options->guarantee;
    txn->cancel = options->cancel;
    status = options->access == TXN_ACCESS_READ_ONLY ? begin_reader(manager, txn)
                                                      : begin_writer(manager, options, txn);
    if (status != TXN_OK) {
        free(txn);
        return status;
    }
    *out = txn;
    return TXN_OK;
}

static void terminate_reader(txn_t *txn, txn_state_t state)
{
    txn_manager_t *manager = txn->manager;

    pthread_mutex_lock(&manager->mutex);
    unlink_active(manager, txn);
    manager->readers--;
    txn->state = state;
    pthread_mutex_unlock(&manager->mutex);
    notify_reclaim(manager);
}

/*
 * writer를 끝낸다. 상태·순번·poison을 락 안에서 확정하고, 락 밖에서 write_finish를 정확히 한 번 부른 뒤,
 * backend poison을 확인하고 슬롯을 놓는다. 슬롯은 finish가 끝난 뒤에야 풀리므로 다음 writer는 이전 정리를 본다.
 */
static void terminate_writer(txn_t *txn, txn_state_t state, txn_outcome_t outcome,
                             txn_guarantee_t achieved, txn_commit_info_t *out)
{
    txn_manager_t *manager = txn->manager;
    const txn_manager_t *saved;
    txn_commit_info_t info;
    uint64_t commit_seq;
    bool backend_poisoned;

    pthread_mutex_lock(&manager->mutex);
    if (outcome == TXN_OUTCOME_APPLIED) {
        manager->last_committed++;
    }
    commit_seq = manager->last_committed;
    if (outcome == TXN_OUTCOME_UNKNOWN) {
        manager->poisoned = true;
    }
    info.txn_id = txn->id;
    info.outcome = outcome;
    info.commit_seq = commit_seq;
    info.guarantee = outcome == TXN_OUTCOME_APPLIED ? achieved : TXN_GUARANTEE_VISIBLE;
    ring_add_locked(manager, txn, &info);
    unlink_active(manager, txn);
    txn->state = state;
    pthread_mutex_unlock(&manager->mutex);
    if (out != NULL) {
        *out = info;
    }

    saved = enter_callback(manager);
    manager->ops.write_finish(manager->ctx, txn->write, outcome, commit_seq);
    backend_poisoned = manager->ops.is_poisoned != NULL && manager->ops.is_poisoned(manager->ctx);
    leave_callback(saved);
    txn->write = NULL;
    if (backend_poisoned) {
        pthread_mutex_lock(&manager->mutex);
        manager->poisoned = true;
        pthread_mutex_unlock(&manager->mutex);
    }
    release_writer_slot(manager);
    notify_reclaim(manager);
}

static txn_outcome_t checked_outcome(const txn_publish_result_t *result, txn_guarantee_t requested)
{
    if (result->outcome == TXN_OUTCOME_NOT_APPLIED) {
        return TXN_OUTCOME_NOT_APPLIED;
    }
    if (result->outcome == TXN_OUTCOME_APPLIED && guarantee_valid(result->achieved) &&
        result->achieved >= requested) {
        return TXN_OUTCOME_APPLIED;
    }
    return TXN_OUTCOME_UNKNOWN;   /* 알 수 없는 값과 보증 미달은 적용 여부를 믿을 수 없다 */
}

static void fill_reader_info(txn_commit_info_t *info, const txn_t *txn, txn_outcome_t outcome)
{
    if (info == NULL) {
        return;
    }
    info->txn_id = txn->id;
    info->outcome = outcome;
    info->commit_seq = txn->snapshot_seq;
    info->guarantee = TXN_GUARANTEE_VISIBLE;
}

static txn_status_t commit_writer(txn_t *txn, txn_commit_info_t *info)
{
    txn_manager_t *manager = txn->manager;
    const txn_manager_t *saved;
    txn_publish_result_t result;
    txn_outcome_t outcome;
    uint64_t commit_seq;

    if (txn_cancel_requested(txn->cancel)) {
        terminate_writer(txn, TXN_STATE_ROLLED_BACK, TXN_OUTCOME_NOT_APPLIED, TXN_GUARANTEE_VISIBLE,
                         info);
        return TXN_CANCELLED;
    }
    pthread_mutex_lock(&manager->mutex);
    commit_seq = manager->last_committed + 1;   /* writer가 하나라 이 순번을 다른 commit이 가져갈 수 없다 */
    pthread_mutex_unlock(&manager->mutex);

    saved = enter_callback(manager);
    result = manager->ops.publish(manager->ctx, txn->write, commit_seq, txn->guarantee);
    leave_callback(saved);
    outcome = checked_outcome(&result, txn->guarantee);
    if (outcome == TXN_OUTCOME_APPLIED) {
        terminate_writer(txn, TXN_STATE_COMMITTED, outcome, result.achieved, info);
        return TXN_OK;
    }
    if (outcome == TXN_OUTCOME_NOT_APPLIED) {
        terminate_writer(txn, TXN_STATE_FAILED, outcome, TXN_GUARANTEE_VISIBLE, info);
        return TXN_COMMIT_FAILED;
    }
    terminate_writer(txn, TXN_STATE_UNCERTAIN, outcome, TXN_GUARANTEE_VISIBLE, info);
    return TXN_COMMIT_UNCERTAIN;
}

txn_status_t txn_commit(txn_t *txn, txn_commit_info_t *info)
{
    if (txn == NULL) {
        return TXN_INVALID;
    }
    if (in_callback(txn->manager)) {
        return TXN_REENTRANT;
    }
    if (txn->state != TXN_STATE_ACTIVE) {
        return TXN_NOT_ACTIVE;
    }
    if (txn->access == TXN_ACCESS_READ_ONLY) {
        terminate_reader(txn, TXN_STATE_COMMITTED);
        fill_reader_info(info, txn, TXN_OUTCOME_APPLIED);
        return TXN_OK;
    }
    return commit_writer(txn, info);
}

txn_status_t txn_rollback(txn_t *txn)
{
    if (txn == NULL) {
        return TXN_INVALID;
    }
    if (in_callback(txn->manager)) {
        return TXN_REENTRANT;
    }
    if (txn->state != TXN_STATE_ACTIVE) {
        return TXN_NOT_ACTIVE;
    }
    if (txn->access == TXN_ACCESS_READ_ONLY) {
        terminate_reader(txn, TXN_STATE_ROLLED_BACK);
    } else {
        terminate_writer(txn, TXN_STATE_ROLLED_BACK, TXN_OUTCOME_NOT_APPLIED, TXN_GUARANTEE_VISIBLE, NULL);
    }
    return TXN_OK;
}

txn_status_t txn_release(txn_t **txn_ptr)
{
    txn_manager_t *manager;
    txn_t *txn;

    if (txn_ptr == NULL) {
        return TXN_INVALID;
    }
    txn = *txn_ptr;
    if (txn == NULL) {
        return TXN_OK;
    }
    manager = txn->manager;
    if (in_callback(manager)) {
        return TXN_REENTRANT;
    }
    if (txn->state == TXN_STATE_ACTIVE) {
        txn_rollback(txn);
    }
    pthread_mutex_lock(&manager->mutex);
    manager->live_handles--;
    pthread_mutex_unlock(&manager->mutex);
    free(txn);
    *txn_ptr = NULL;
    return TXN_OK;
}

txn_state_t txn_state(const txn_t *txn) { return txn == NULL ? TXN_STATE_NONE : txn->state; }

uint64_t txn_id(const txn_t *txn) { return txn == NULL ? 0 : txn->id; }

uint64_t txn_snapshot_seq(const txn_t *txn) { return txn == NULL ? 0 : txn->snapshot_seq; }

txn_status_t txn_query_commit(txn_manager_t *manager, uint64_t id, txn_commit_info_t *info)
{
    txn_status_t status = TXN_NOT_FOUND;

    if (manager == NULL || info == NULL) {
        return TXN_INVALID;
    }
    if (in_callback(manager)) {
        return TXN_REENTRANT;
    }
    pthread_mutex_lock(&manager->mutex);
    if ((uint32_t)(id >> 32) != manager->epoch || id == 0) {
        status = TXN_UNSUPPORTED;
    } else if ((uint32_t)id >= manager->next_serial && manager->next_serial != 0) {
        status = TXN_NOT_FOUND;   /* 이 세대가 아직 발급하지 않은 번호 */
    } else if (manager->writer_active && id == manager->writer_id) {
        status = TXN_BUSY;
    } else {
        for (size_t i = 0; i < manager->ring_count; i++) {
            if (manager->ring[i].txn_id == id) {
                *info = manager->ring[i].info;
                status = TXN_OK;
                break;
            }
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return status;
}

uint64_t txn_manager_last_committed(txn_manager_t *manager)
{
    uint64_t seq;

    pthread_mutex_lock(&manager->mutex);
    seq = manager->last_committed;
    pthread_mutex_unlock(&manager->mutex);
    return seq;
}

uint64_t txn_manager_oldest_snapshot(txn_manager_t *manager)
{
    uint64_t seq;

    pthread_mutex_lock(&manager->mutex);
    seq = horizon_locked(manager);
    pthread_mutex_unlock(&manager->mutex);
    return seq;
}

bool txn_manager_is_poisoned(txn_manager_t *manager)
{
    bool poisoned;

    pthread_mutex_lock(&manager->mutex);
    poisoned = manager->poisoned;
    pthread_mutex_unlock(&manager->mutex);
    return poisoned;
}

unsigned txn_manager_capabilities(const txn_manager_t *manager)
{
    return manager->max_guarantee == TXN_GUARANTEE_DURABLE ? TXN_CAP_DURABLE : 0;
}
