/*
 * lock_table.c — Row Lock 구현 (Strict 2PL + 3초 timeout)
 *
 * 해시 테이블로 row_id별 lock 엔트리를 관리한다.
 * S-S는 호환, S-X/X-S/X-X는 대기.
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
    pthread_mutex_init(&lt->mutex, NULL);
    pthread_cond_init(&lt->cond, NULL);
}

void lock_table_destroy(lock_table_t *lt)
{
    for (int i = 0; i < LOCK_TABLE_BUCKETS; i++) {
        lock_entry_t *e = lt->buckets[i];
        while (e) {
            lock_entry_t *next = e->next;
            free(e);
            e = next;
        }
        lt->buckets[i] = NULL;
    }
    pthread_mutex_destroy(&lt->mutex);
    pthread_cond_destroy(&lt->cond);
}

/* 충돌 검사: row_id에 대해 mode가 호환되지 않는 lock이 있는지 */
static int conflict_exists(lock_table_t *lt, uint64_t row_id,
                           lock_mode_t mode, pthread_t self)
{
    uint32_t bucket = hash_row_id(row_id);
    lock_entry_t *e = lt->buckets[bucket];
    while (e) {
        if (e->row_id == row_id && !pthread_equal(e->owner, self)) {
            /* S-S는 호환 */
            if (e->mode == LOCK_X || mode == LOCK_X)
                return 1;
        }
        e = e->next;
    }
    return 0;
}

int lock_acquire(lock_table_t *lt, uint64_t row_id, lock_mode_t mode)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;  /* 3초 timeout */

    pthread_t self = pthread_self();

    pthread_mutex_lock(&lt->mutex);

    /* 이미 같은 스레드가 같은 row에 lock을 갖고 있으면 업그레이드 확인 */
    uint32_t bucket = hash_row_id(row_id);
    lock_entry_t *e = lt->buckets[bucket];
    while (e) {
        if (e->row_id == row_id && pthread_equal(e->owner, self)) {
            /* 이미 X → 재진입 OK */
            if (e->mode == LOCK_X || mode == LOCK_S) {
                pthread_mutex_unlock(&lt->mutex);
                return 0;
            }
            /* S → X 업그레이드: 다른 S holder가 없어야 */
            break;
        }
        e = e->next;
    }

    /* 충돌 대기 */
    while (conflict_exists(lt, row_id, mode, self)) {
        int rc = pthread_cond_timedwait(&lt->cond, &lt->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&lt->mutex);
            return -1;
        }
    }

    /* S→X 업그레이드인 경우 기존 엔트리의 mode 변경 */
    if (e && e->row_id == row_id && pthread_equal(e->owner, self)) {
        e->mode = mode;
        pthread_mutex_unlock(&lt->mutex);
        return 0;
    }

    /* 새 엔트리 추가 */
    lock_entry_t *ne = (lock_entry_t *)malloc(sizeof(lock_entry_t));
    ne->row_id = row_id;
    ne->mode = mode;
    ne->owner = self;
    ne->next = lt->buckets[bucket];
    lt->buckets[bucket] = ne;

    pthread_mutex_unlock(&lt->mutex);
    return 0;
}

void lock_release_all(lock_table_t *lt)
{
    pthread_t self = pthread_self();
    int released = 0;

    pthread_mutex_lock(&lt->mutex);
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
    if (released > 0)
        pthread_cond_broadcast(&lt->cond);
    pthread_mutex_unlock(&lt->mutex);
}

lock_stats_t lock_table_stats(lock_table_t *lt)
{
    lock_stats_t s = {0, 0, 0};
    pthread_mutex_lock(&lt->mutex);
    for (int i = 0; i < LOCK_TABLE_BUCKETS; i++) {
        lock_entry_t *e = lt->buckets[i];
        while (e) {
            s.total++;
            if (e->mode == LOCK_S) s.shared++;
            else                   s.exclusive++;
            e = e->next;
        }
    }
    pthread_mutex_unlock(&lt->mutex);
    return s;
}
