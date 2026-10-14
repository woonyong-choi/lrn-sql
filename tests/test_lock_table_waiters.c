/*
 * test_lock_table_waiters.c — lock_table 대기 writer 등록의 해제 균형 검사 (#129)
 *
 * lock_table.c를 직접 포함해 calloc 실패를 주입한다. 실제로 기다리는 스레드를 만들고, 대기 등록 뒤 lock entry
 * 할당이 실패하는 입력에서 waiting_writers가 시작 값으로 돌아오는지, 충돌 없는 후속 읽기가 3초 timeout을 기다리지
 * 않고 진입하는지, 다른 writer의 등록을 잘못 지우지 않는지를 point와 range 경로에서 각각 본다.
 * 성공·업그레이드·timeout 경로의 등록 해제와 반복 뒤 entry 누수 0도 함께 본다.
 *
 * 취소: lock_table은 취소 API를 제공하지 않는다(pthread_cancel은 mutex를 쥔 채 빠져나가므로 지원 경로가 아니다).
 * 그래서 지원되는 탈출 경로인 성공·업그레이드·timeout·할당 실패만 검사한다.
 *
 * 빌드: gcc -Wall -Wextra -Werror -g -std=c11 -Iinclude -pthread tests/test_lock_table_waiters.c \
 *         -o build/test_lock_table_waiters
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static atomic_int fail_next;     /* 1이면 다음 calloc 하나를 실패시킨다 */
static atomic_long live_blocks;  /* lock_table.c가 잡고 아직 해제하지 않은 블록 수 */

static void *fault_calloc(size_t n, size_t size)
{
    if (atomic_exchange(&fail_next, 0)) {
        return NULL;
    }
    void *block = calloc(n, size);
    if (block != NULL) {
        atomic_fetch_add(&live_blocks, 1);
    }
    return block;
}

static void counted_free(void *block)
{
    if (block != NULL) {
        atomic_fetch_sub(&live_blocks, 1);
        free(block);
    }
}

#define calloc(n, size) fault_calloc(n, size)
#define free(block) counted_free(block)
#include "../src/server/lock_table.c"
#undef calloc
#undef free

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* 한 스레드가 lock을 얻은 뒤 release 신호까지 쥐고 있다가 모두 해제한다. */
typedef struct {
    lock_table_t *lt;
    int range;
    uint64_t low;
    uint64_t high;
    lock_mode_t mode;
    int upgrade;          /* 1이면 같은 row의 S를 먼저 얻고 mode로 업그레이드를 요청한다 */
    int rc;
    int64_t acquire_ms;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int acquired;
    int release;
    pthread_t thread;
} worker_t;

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    int64_t start;

    if (w->upgrade) {
        lock_acquire(w->lt, w->low, LOCK_S);
    }
    start = mono_ms();
    w->rc = w->range ? lock_acquire_range(w->lt, w->low, w->high, w->mode) : lock_acquire(w->lt, w->low, w->mode);
    w->acquire_ms = mono_ms() - start;
    pthread_mutex_lock(&w->mutex);
    w->acquired = 1;
    pthread_cond_broadcast(&w->cond);
    while (!w->release) {
        pthread_cond_wait(&w->cond, &w->mutex);
    }
    pthread_mutex_unlock(&w->mutex);
    lock_release_all(w->lt);
    return NULL;
}

static void worker_start(worker_t *w, lock_table_t *lt, int range, uint64_t low, uint64_t high, lock_mode_t mode,
                         int upgrade)
{
    *w = (worker_t){.lt = lt, .range = range, .low = low, .high = high, .mode = mode, .upgrade = upgrade};
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
    pthread_create(&w->thread, NULL, worker_main, w);
}

static void worker_wait_acquired(worker_t *w)
{
    pthread_mutex_lock(&w->mutex);
    while (!w->acquired) {
        pthread_cond_wait(&w->cond, &w->mutex);
    }
    pthread_mutex_unlock(&w->mutex);
}

static void worker_finish(worker_t *w)
{
    pthread_mutex_lock(&w->mutex);
    w->release = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
}

static int waiting(lock_table_t *lt)
{
    pthread_mutex_lock(&lt->mutex);
    int n = lt->waiting_writers;
    pthread_mutex_unlock(&lt->mutex);
    return n;
}

/* waiting_writers가 want가 될 때까지 기다린다(최대 2초). 기다리는 스레드가 실제로 등록했는지 확인한다. */
static int wait_for_waiting(lock_table_t *lt, int want)
{
    for (int i = 0; i < 2000; i++) {
        if (waiting(lt) == want) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

/* point는 [low, low], range는 [low, high]를 쓴다. */
static uint64_t high_of(int range, uint64_t low, uint64_t span)
{
    return range ? low + span : low;
}

/* ── 1. 등록 뒤 할당 실패: 대기 수 복귀와 후속 읽기 ── */

#define FOLLOWUP_LIMIT_MS 1000   /* 기존 3초 timeout보다 충분히 짧다 */

static void scenario_oom_after_wait(int range, int inject)
{
    lock_table_t lt;
    worker_t holder;
    worker_t writer;
    worker_t reader;

    lock_table_init(&lt);
    worker_start(&holder, &lt, range, 10, high_of(range, 10, 10), LOCK_X, 0);
    worker_wait_acquired(&holder);
    CHECK(holder.rc == 0);
    worker_start(&writer, &lt, range, range ? 15 : 10, range ? 25 : 10, LOCK_X, 0);
    CHECK(wait_for_waiting(&lt, 1));

    if (inject) {
        atomic_store(&fail_next, 1);
    }
    worker_finish(&holder);   /* 해제로 writer가 깨어나 새 entry를 할당한다 */
    worker_wait_acquired(&writer);
    CHECK(writer.rc == (inject ? -1 : 0));
    CHECK(atomic_load(&fail_next) == 0);      /* 주입한 실패가 실제로 쓰였다 */
    CHECK(waiting(&lt) == 0);
    worker_finish(&writer);
    CHECK(lock_table_stats(&lt).total == 0);

    /* 충돌 없는 후속 S는 3초 timeout 없이 바로 얻는다 */
    worker_start(&reader, &lt, range, range ? 12 : 10, range ? 13 : 10, LOCK_S, 0);
    worker_wait_acquired(&reader);
    CHECK(reader.rc == 0 && reader.acquire_ms < FOLLOWUP_LIMIT_MS);
    worker_finish(&reader);
    CHECK(waiting(&lt) == 0 && lock_table_stats(&lt).total == 0);
    lock_table_destroy(&lt);
    CHECK(atomic_load(&live_blocks) == 0);
}

/* ── 2. 실패한 writer만 등록을 지운다 ── */

static void scenario_other_writer_stays(int range)
{
    lock_table_t lt;
    worker_t holder_a;
    worker_t holder_b;
    worker_t writer_a;
    worker_t writer_b;

    lock_table_init(&lt);
    worker_start(&holder_a, &lt, range, 10, high_of(range, 10, 5), LOCK_X, 0);
    worker_start(&holder_b, &lt, range, 100, high_of(range, 100, 5), LOCK_X, 0);
    worker_wait_acquired(&holder_a);
    worker_wait_acquired(&holder_b);
    worker_start(&writer_a, &lt, range, 10, high_of(range, 10, 5), LOCK_X, 0);
    worker_start(&writer_b, &lt, range, 100, high_of(range, 100, 5), LOCK_X, 0);
    CHECK(wait_for_waiting(&lt, 2));

    atomic_store(&fail_next, 1);
    worker_finish(&holder_a);
    worker_wait_acquired(&writer_a);
    CHECK(writer_a.rc == -1);
    CHECK(waiting(&lt) == 1);   /* 아직 기다리는 writer_b의 등록은 그대로다 */
    worker_finish(&writer_a);

    worker_finish(&holder_b);
    worker_wait_acquired(&writer_b);
    CHECK(writer_b.rc == 0);
    CHECK(waiting(&lt) == 0);
    worker_finish(&writer_b);
    CHECK(lock_table_stats(&lt).total == 0);
    lock_table_destroy(&lt);
    CHECK(atomic_load(&live_blocks) == 0);
}

/* 같은 자리를 기다리던 두 writer 중 하나만 할당에 실패한다. 실패한 쪽은 -1, 다른 쪽은 성공한다. */
static void scenario_two_writers_one_failure(int range)
{
    lock_table_t lt;
    worker_t holder;
    worker_t first;
    worker_t second;

    lock_table_init(&lt);
    worker_start(&holder, &lt, range, 10, high_of(range, 10, 5), LOCK_X, 0);
    worker_wait_acquired(&holder);
    worker_start(&first, &lt, range, 10, high_of(range, 10, 5), LOCK_X, 0);
    worker_start(&second, &lt, range, 10, high_of(range, 10, 5), LOCK_X, 0);
    CHECK(wait_for_waiting(&lt, 2));

    atomic_store(&fail_next, 1);
    worker_finish(&holder);
    worker_wait_acquired(&first);
    worker_wait_acquired(&second);
    CHECK((first.rc == -1) + (second.rc == -1) == 1);
    CHECK((first.rc == 0) + (second.rc == 0) == 1);
    CHECK(waiting(&lt) == 0);
    worker_finish(&first);
    worker_finish(&second);
    CHECK(lock_table_stats(&lt).total == 0);
    lock_table_destroy(&lt);
    CHECK(atomic_load(&live_blocks) == 0);
}

/* 이미 보유한 lock이 있는 스레드의 새 lock 할당이 실패해도 기존 보유 상태는 그대로다. */
static void scenario_held_state_survives(void)
{
    lock_table_t lt;

    lock_table_init(&lt);
    CHECK(lock_acquire(&lt, 1, LOCK_S) == 0);
    CHECK(lock_acquire_range(&lt, 50, 60, LOCK_S) == 0);
    CHECK(lock_table_stats(&lt).total == 2);

    atomic_store(&fail_next, 1);
    CHECK(lock_acquire(&lt, 2, LOCK_X) == -1);
    CHECK(lock_table_stats(&lt).total == 2 && waiting(&lt) == 0);
    atomic_store(&fail_next, 1);
    CHECK(lock_acquire_range(&lt, 70, 80, LOCK_X) == -1);
    CHECK(lock_table_stats(&lt).total == 2 && waiting(&lt) == 0);

    /* 기존 보유 lock은 정상 해제되고 이어서 같은 스레드가 다시 잡을 수 있다 */
    lock_release_all(&lt);
    CHECK(lock_table_stats(&lt).total == 0);
    CHECK(lock_acquire(&lt, 2, LOCK_X) == 0 && lock_acquire_range(&lt, 70, 80, LOCK_X) == 0);
    lock_release_all(&lt);
    lock_table_destroy(&lt);
    CHECK(atomic_load(&live_blocks) == 0);
}

/* ── 3. 업그레이드 성공 ── */

static void scenario_upgrade_success(void)
{
    lock_table_t lt;
    worker_t other;
    worker_t upgrader;
    lock_stats_t stats;

    lock_table_init(&lt);
    worker_start(&other, &lt, 0, 30, 30, LOCK_S, 0);
    worker_wait_acquired(&other);
    worker_start(&upgrader, &lt, 0, 30, 30, LOCK_X, 1);
    CHECK(wait_for_waiting(&lt, 1));
    worker_finish(&other);
    worker_wait_acquired(&upgrader);
    CHECK(upgrader.rc == 0 && waiting(&lt) == 0);
    stats = lock_table_stats(&lt);
    CHECK(stats.total == 1 && stats.exclusive == 1);
    worker_finish(&upgrader);
    CHECK(lock_table_stats(&lt).total == 0);
    lock_table_destroy(&lt);
    CHECK(atomic_load(&live_blocks) == 0);
}

/* ── 4. timeout: point·range·업그레이드 writer와 reader가 함께 시간 초과 ── */

static void scenario_timeouts(void)
{
    lock_table_t lt;
    worker_t holder_point;
    worker_t holder_range;
    worker_t holder_shared;
    worker_t upgrader;
    worker_t writer_point;
    worker_t writer_range;
    worker_t reader;
    lock_stats_t stats;

    lock_table_init(&lt);
    worker_start(&holder_point, &lt, 0, 10, 10, LOCK_X, 0);
    worker_start(&holder_range, &lt, 1, 100, 110, LOCK_X, 0);
    worker_start(&holder_shared, &lt, 0, 30, 30, LOCK_S, 0);
    worker_wait_acquired(&holder_point);
    worker_wait_acquired(&holder_range);
    worker_wait_acquired(&holder_shared);

    worker_start(&upgrader, &lt, 0, 30, 30, LOCK_X, 1);   /* 보유한 S의 업그레이드가 다른 S 때문에 대기 */
    CHECK(wait_for_waiting(&lt, 1));
    worker_start(&writer_point, &lt, 0, 10, 10, LOCK_X, 0);
    worker_start(&writer_range, &lt, 1, 105, 115, LOCK_X, 0);
    worker_start(&reader, &lt, 0, 10, 10, LOCK_S, 0);   /* reader는 등록하지 않는다 */
    CHECK(wait_for_waiting(&lt, 3));

    worker_wait_acquired(&upgrader);
    worker_wait_acquired(&writer_point);
    worker_wait_acquired(&writer_range);
    worker_wait_acquired(&reader);
    CHECK(upgrader.rc == -1 && writer_point.rc == -1 && writer_range.rc == -1 && reader.rc == -1);
    CHECK(writer_point.acquire_ms >= 2900 && writer_range.acquire_ms >= 2900);
    CHECK(waiting(&lt) == 0);

    /* timeout한 업그레이드는 기존 S를 잃지 않는다: 보유 4개(X 2, S 2)가 그대로다 */
    stats = lock_table_stats(&lt);
    CHECK(stats.total == 4 && stats.exclusive == 2 && stats.shared == 2);
    worker_finish(&upgrader);
    worker_finish(&writer_point);
    worker_finish(&writer_range);
    worker_finish(&reader);
    worker_finish(&holder_point);
    worker_finish(&holder_range);
    worker_finish(&holder_shared);
    CHECK(lock_table_stats(&lt).total == 0 && waiting(&lt) == 0);
    lock_table_destroy(&lt);
    CHECK(atomic_load(&live_blocks) == 0);
}

/* ── 5. 반복: 실패와 성공을 번갈아 돌려도 등록·entry가 새지 않는다 ── */

#define REPEATS 60

static void scenario_repeat(void)
{
    for (int i = 0; i < REPEATS; i++) {
        scenario_oom_after_wait(i % 2, i % 3 != 0);
    }
}

int main(void)
{
    for (int range = 0; range <= 1; range++) {
        scenario_oom_after_wait(range, 1);
        scenario_oom_after_wait(range, 0);
        scenario_other_writer_stays(range);
        scenario_two_writers_one_failure(range);
    }
    scenario_held_state_survives();
    scenario_upgrade_success();
    scenario_repeat();
    scenario_timeouts();

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_lock_table_waiters: all passed\n");
    return 0;
}
