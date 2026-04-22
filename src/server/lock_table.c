/*
 * lock_table.c — Row Lock + Range Lock 구현 (Strict 2PL + 3초 timeout)
 *
 * Point Lock: 해시 테이블로 row_id별 lock 엔트리를 관리한다.
 * Range Lock: 별도 연결 리스트로 [low, high] 범위 lock을 관리한다.
 *
 * 충돌 규칙:
 *   S-S는 호환, S-X/X-S/X-X는 대기.
 *   Point vs Range: point key가 range [low, high] 안에 있으면 충돌 검사.
 *   Range vs Range: 두 범위가 겹치면 충돌 검사.
 *
 * 3초 내에 lock을 획득하지 못하면 -1(timeout)을 반환한다.
 */

#include "server/lock_table.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static uint32_t hash_row_id(uint64_t row_id)
{
    return (uint32_t)(row_id % LOCK_TABLE_BUCKETS);
}

void lock_table_init(lock_table_t *lt)
{
    memset(lt->buckets, 0, sizeof(lt->buckets));
    lt->range_locks = NULL;
    pthread_mutex_init(&lt->mutex, NULL);
    pthread_cond_init(&lt->cond, NULL);
}

void lock_table_destroy(lock_table_t *lt)
{
    /* point locks 해제 */
    for (int i = 0; i < LOCK_TABLE_BUCKETS; i++) {
        lock_entry_t *e = lt->buckets[i];
        while (e) {
            lock_entry_t *next = e->next;
            free(e);
            e = next;
        }
        lt->buckets[i] = NULL;
    }
    /* range locks 해제 */
    lock_entry_t *r = lt->range_locks;
    while (r) {
        lock_entry_t *next = r->next;
        free(r);
        r = next;
    }
    lt->range_locks = NULL;
    pthread_mutex_destroy(&lt->mutex);
    pthread_cond_destroy(&lt->cond);
}

/* ── 충돌 검사 ── */

/* Point vs Point: 같은 row_id에 비호환 lock이 있는지 */
static int point_vs_point(lock_table_t *lt, uint64_t row_id,
                          lock_mode_t mode, pthread_t self)
{
    uint32_t bucket = hash_row_id(row_id);
    lock_entry_t *e = lt->buckets[bucket];
    while (e) {
        if (e->row_id == row_id && !pthread_equal(e->owner, self)) {
            if (e->mode == LOCK_X || mode == LOCK_X)
                return 1;
        }
        e = e->next;
    }
    return 0;
}

/* Point vs Range: point key가 다른 스레드의 range lock 범위 안에 있는지 */
static int point_vs_range(lock_table_t *lt, uint64_t key,
                          lock_mode_t mode, pthread_t self)
{
    lock_entry_t *e = lt->range_locks;
    while (e) {
        if (!pthread_equal(e->owner, self)
            && key >= e->row_id && key <= e->range_high) {
            if (e->mode == LOCK_X || mode == LOCK_X)
                return 1;
        }
        e = e->next;
    }
    return 0;
}

/* Range vs Range: 두 범위가 겹치고 비호환 mode인지 */
static int range_vs_range(lock_table_t *lt, uint64_t low, uint64_t high,
                          lock_mode_t mode, pthread_t self)
{
    lock_entry_t *e = lt->range_locks;
    while (e) {
        if (!pthread_equal(e->owner, self)
            && low <= e->range_high && e->row_id <= high) {
            if (e->mode == LOCK_X || mode == LOCK_X)
                return 1;
        }
        e = e->next;
    }
    return 0;
}

/* Range vs Point: 범위 안에 다른 스레드의 point lock이 있는지 */
static int range_vs_point(lock_table_t *lt, uint64_t low, uint64_t high,
                          lock_mode_t mode, pthread_t self)
{
    for (int i = 0; i < LOCK_TABLE_BUCKETS; i++) {
        lock_entry_t *e = lt->buckets[i];
        while (e) {
            if (!pthread_equal(e->owner, self)
                && e->row_id >= low && e->row_id <= high) {
                if (e->mode == LOCK_X || mode == LOCK_X)
                    return 1;
            }
            e = e->next;
        }
    }
    return 0;
}

/* point lock의 전체 충돌 검사 (point-vs-point + point-vs-range) */
static int conflict_exists(lock_table_t *lt, uint64_t row_id,
                           lock_mode_t mode, pthread_t self)
{
    if (point_vs_point(lt, row_id, mode, self))
        return 1;
    if (point_vs_range(lt, row_id, mode, self))
        return 1;
    return 0;
}

/* range lock의 전체 충돌 검사 (range-vs-range + range-vs-point) */
static int range_conflict_exists(lock_table_t *lt, uint64_t low, uint64_t high,
                                 lock_mode_t mode, pthread_t self)
{
    if (range_vs_range(lt, low, high, mode, self))
        return 1;
    if (range_vs_point(lt, low, high, mode, self))
        return 1;
    return 0;
}

/* ── Point Lock 획득 ── */
int lock_acquire(lock_table_t *lt, uint64_t row_id, lock_mode_t mode)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;

    pthread_t self = pthread_self();

    pthread_mutex_lock(&lt->mutex);

    /* 이미 같은 스레드가 같은 row에 lock을 갖고 있으면 업그레이드 확인 */
    uint32_t bucket = hash_row_id(row_id);
    lock_entry_t *e = lt->buckets[bucket];
    while (e) {
        if (e->row_id == row_id && pthread_equal(e->owner, self)) {
            if (e->mode == LOCK_X || mode == LOCK_S) {
                pthread_mutex_unlock(&lt->mutex);
                return 0;
            }
            /* S → X 업그레이드 */
            break;
        }
        e = e->next;
    }

    /* 충돌 대기 (point-vs-point + point-vs-range) */
    while (conflict_exists(lt, row_id, mode, self)) {
        int rc = pthread_cond_timedwait(&lt->cond, &lt->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&lt->mutex);
            return -1;
        }
    }

    /* S→X 업그레이드 */
    if (e && e->row_id == row_id && pthread_equal(e->owner, self)) {
        e->mode = mode;
        pthread_mutex_unlock(&lt->mutex);
        return 0;
    }

    /* 새 point lock 엔트리 추가 */
    lock_entry_t *ne = (lock_entry_t *)malloc(sizeof(lock_entry_t));
    ne->row_id = row_id;
    ne->range_high = 0;  /* point lock */
    ne->mode = mode;
    ne->owner = self;
    ne->next = lt->buckets[bucket];
    lt->buckets[bucket] = ne;

    pthread_mutex_unlock(&lt->mutex);
    return 0;
}

/* ── Range Lock 획득 (Next-Key Lock) ── */
int lock_acquire_range(lock_table_t *lt, uint64_t low, uint64_t high,
                       lock_mode_t mode)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;

    pthread_t self = pthread_self();

    pthread_mutex_lock(&lt->mutex);

    /* 이미 같은 스레드가 동일/포함하는 range lock을 갖고 있는지 확인 */
    lock_entry_t *e = lt->range_locks;
    while (e) {
        if (pthread_equal(e->owner, self)
            && e->row_id <= low && e->range_high >= high) {
            /* 기존 range가 요청 범위를 포함 */
            if (e->mode == LOCK_X || mode == LOCK_S) {
                pthread_mutex_unlock(&lt->mutex);
                return 0;
            }
        }
        e = e->next;
    }

    /* 충돌 대기 (range-vs-range + range-vs-point) */
    while (range_conflict_exists(lt, low, high, mode, self)) {
        int rc = pthread_cond_timedwait(&lt->cond, &lt->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&lt->mutex);
            return -1;
        }
    }

    /* 새 range lock 엔트리 추가 */
    lock_entry_t *ne = (lock_entry_t *)malloc(sizeof(lock_entry_t));
    ne->row_id = low;
    ne->range_high = high;
    ne->mode = mode;
    ne->owner = self;
    ne->next = lt->range_locks;
    lt->range_locks = ne;

    pthread_mutex_unlock(&lt->mutex);
    return 0;
}

/* ── 현재 스레드의 모든 lock 해제 (point + range) ── */
void lock_release_all(lock_table_t *lt)
{
    pthread_t self = pthread_self();
    int released = 0;

    pthread_mutex_lock(&lt->mutex);

    /* point locks 해제 */
    for (int i = 0; i < LOCK_TABLE_BUCKETS; i++) {
        lock_entry_t **pp = &lt->buckets[i];
        while (*pp) {
            if (pthread_equal((*pp)->owner, self)) {
                lock_entry_t *del = *pp;
                *pp = del->next;
                free(del);
                released++;
            } else {
                pp = &(*pp)->next;
            }
        }
    }

    /* range locks 해제 */
    lock_entry_t **pp = &lt->range_locks;
    while (*pp) {
        if (pthread_equal((*pp)->owner, self)) {
            lock_entry_t *del = *pp;
            *pp = del->next;
            free(del);
            released++;
        } else {
            pp = &(*pp)->next;
        }
    }

    if (released > 0)
        pthread_cond_broadcast(&lt->cond);
    pthread_mutex_unlock(&lt->mutex);
}

lock_stats_t lock_table_stats(lock_table_t *lt)
{
    lock_stats_t s = {0, 0, 0};
    pthread_mutex_lock(&lt->mutex);

    /* point locks */
    for (int i = 0; i < LOCK_TABLE_BUCKETS; i++) {
        lock_entry_t *e = lt->buckets[i];
        while (e) {
            s.total++;
            if (e->mode == LOCK_S) s.shared++;
            else                   s.exclusive++;
            e = e->next;
        }
    }

    /* range locks */
    lock_entry_t *r = lt->range_locks;
    while (r) {
        s.total++;
        if (r->mode == LOCK_S) s.shared++;
        else                   s.exclusive++;
        r = r->next;
    }

    pthread_mutex_unlock(&lt->mutex);
    return s;
}
