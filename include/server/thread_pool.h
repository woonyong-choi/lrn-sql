/*
 * thread_pool.h — 스레드 풀 인터페이스
 *
 * Worker 스레드들이 bounded 원형 큐에서 job(client_fd)을 꺼내 처리한다.
 * 스레드 수는 OS 코어 수(sysconf)로 결정한다.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "storage/pager.h"

typedef struct {
    int client_fd;
} job_t;

typedef struct {
    pthread_t *threads;       /* worker 스레드 배열 */
    int        thread_count;  /* worker 수 */
    job_t     *queue;         /* bounded 원형 큐 */
    int        queue_cap;     /* 큐 용량 */
    int        queue_size;    /* 현재 대기 job 수 */
    int        head;          /* dequeue 인덱스 */
    int        tail;          /* enqueue 인덱스 */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    bool       shutdown;
    pager_t   *pager;         /* 공유 DB */
} thread_pool_t;

/* 스레드 풀 생성 (thread_count=0이면 코어 수 사용, queue_cap=0이면 64) */
int  thread_pool_init(thread_pool_t *pool, pager_t *pager,
                      int thread_count, int queue_cap);

/* job(client_fd) 등록 — 큐가 가득 차면 블로킹 */
int  thread_pool_submit(thread_pool_t *pool, int client_fd);

/* 풀 종료: shutdown 플래그 → 모든 worker join */
void thread_pool_destroy(thread_pool_t *pool);

#endif /* THREAD_POOL_H */
