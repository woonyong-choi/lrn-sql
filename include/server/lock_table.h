/*
 * lock_table.h — Row Lock (Level 3 동시성 제어)
 *
 * Strict 2PL: 트랜잭션 종료 시 한꺼번에 해제
 * Deadlock 방지: 3초 timeout (pthread_cond_timedwait)
 */

#ifndef LOCK_TABLE_H
#define LOCK_TABLE_H

#include <pthread.h>
#include <stdint.h>

typedef enum {
    LOCK_S,  /* shared (읽기) */
    LOCK_X   /* exclusive (쓰기) */
} lock_mode_t;

typedef struct lock_entry {
    uint64_t            row_id;
    uint64_t            range_high;  /* 0 = point lock, >0 = range lock [row_id, range_high] */
    lock_mode_t         mode;
    pthread_t           owner;
    uint8_t             is_range;    /* 0 = point, 1 = range */
    struct lock_entry  *next;        /* 같은 버킷(또는 range 리스트)의 다음 엔트리 */
    struct lock_entry  *prev;        /* 같은 버킷(또는 range 리스트)의 이전 엔트리 */
    struct lock_entry  *owner_next;  /* 현재 스레드 보유 lock 리스트의 다음 엔트리 */
    struct lock_entry  *owner_prev;  /* 현재 스레드 보유 lock 리스트의 이전 엔트리 */
} lock_entry_t;

#define LOCK_TABLE_BUCKETS 256

typedef struct {
    lock_entry_t *buckets[LOCK_TABLE_BUCKETS];  /* point locks (hash) */
    lock_entry_t *range_locks;                   /* range/gap locks (별도 리스트) */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;   /* lock 해제 시 broadcast */
} lock_table_t;

/* 초기화/정리 */
void lock_table_init(lock_table_t *lt);
void lock_table_destroy(lock_table_t *lt);

/* point lock 획득 (0=성공, -1=timeout) */
int  lock_acquire(lock_table_t *lt, uint64_t row_id, lock_mode_t mode);

/* range lock 획득: [low, high] 범위 (0=성공, -1=timeout)
 * Next-Key Lock: 범위 내 모든 키 + gap을 잠가 phantom insert 방지 */
int  lock_acquire_range(lock_table_t *lt, uint64_t low, uint64_t high, lock_mode_t mode);

/* 현재 스레드가 보유한 모든 lock 해제 (point + range) */
void lock_release_all(lock_table_t *lt);

/* 현재 보유 중인 lock 수 (S/X 각각) */
typedef struct {
    int total;
    int shared;
    int exclusive;
} lock_stats_t;

lock_stats_t lock_table_stats(lock_table_t *lt);

#endif /* LOCK_TABLE_H */
