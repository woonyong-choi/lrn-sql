/*
 * thread_pool.c — 스레드 풀 구현
 *
 * worker 스레드가 원형 큐에서 client_fd를 꺼내
 * HTTP 요청을 파싱하고 db_execute()로 처리한 뒤 응답을 보낸다.
 */

#include "server/thread_pool.h"
#include "server/http.h"
#include "db.h"
#include "storage/pager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

/* 현재 worker가 소속된 pool (stats 응답용) — worker_func에서 설정 */
static __thread thread_pool_t *tl_pool;

/* ── /stats JSON 응답 생성 ── */
static void handle_stats(pager_t *pager, int client_fd)
{
    thread_pool_t *pool = tl_pool;

    /* 스레드 풀 통계 */
    uint64_t active = 0, processed = 0, connections = 0;
    int queue_size = 0, queue_cap = 0, thread_count = 0;
    if (pool) {
        active      = atomic_load(&pool->active_workers);
        processed   = atomic_load(&pool->total_processed);
        connections = atomic_load(&pool->total_connections);
        pthread_mutex_lock(&pool->mutex);
        queue_size  = pool->queue_size;
        queue_cap   = pool->queue_cap;
        thread_count = pool->thread_count;
        pthread_mutex_unlock(&pool->mutex);
    }

    /* Row Lock 통계 */
    lock_stats_t ls = db_lock_stats();

    /* Pager 캐시 통계 */
    uint32_t frames_valid = 0, frames_dirty = 0, frames_pinned = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (pager->frames[i].is_valid) {
            frames_valid++;
            if (pager->frames[i].is_dirty) frames_dirty++;
            if (pager->frames[i].pin_count > 0) frames_pinned++;
        }
    }

    char body[2048];
    int len = snprintf(body, sizeof(body),
        "{\n"
        "  \"thread_pool\": {\n"
        "    \"workers_total\": %d,\n"
        "    \"workers_active\": %lu,\n"
        "    \"workers_idle\": %lu,\n"
        "    \"queue_pending\": %d,\n"
        "    \"queue_capacity\": %d,\n"
        "    \"total_processed\": %lu,\n"
        "    \"total_connections\": %lu\n"
        "  },\n"
        "  \"locks\": {\n"
        "    \"total\": %d,\n"
        "    \"shared\": %d,\n"
        "    \"exclusive\": %d\n"
        "  },\n"
        "  \"pager\": {\n"
        "    \"page_size\": %u,\n"
        "    \"max_frames\": %d,\n"
        "    \"frames_used\": %u,\n"
        "    \"frames_dirty\": %u,\n"
        "    \"frames_pinned\": %u,\n"
        "    \"total_pages\": %u,\n"
        "    \"free_pages\": %u\n"
        "  },\n"
        "  \"db\": {\n"
        "    \"row_count\": %lu,\n"
        "    \"next_id\": %lu\n"
        "  }\n"
        "}\n",
        thread_count,
        (unsigned long)active,
        (unsigned long)(thread_count > 0 ? (uint64_t)thread_count - active : 0),
        queue_size, queue_cap,
        (unsigned long)processed,
        (unsigned long)connections,
        ls.total, ls.shared, ls.exclusive,
        pager->page_size, MAX_FRAMES,
        frames_valid, frames_dirty, frames_pinned,
        pager->header.next_page_id,
        pager->header.free_page_head,
        (unsigned long)pager->header.row_count,
        (unsigned long)pager->header.next_id);

    http_send_ok(client_fd, body, (size_t)len);
}

/* ── 클라이언트 1건 처리 ── */
static void handle_client(pager_t *pager, int client_fd)
{
    http_request_t req;
    if (http_read_request(client_fd, &req) != 0 || !req.valid) {
        const char *msg = "오류: 잘못된 요청입니다";
        http_send_error(client_fd, msg, strlen(msg));
        return;
    }

    /* GET /stats 라우트 */
    if (req.route == ROUTE_STATS) {
        handle_stats(pager, client_fd);
        return;
    }

    exec_result_t res = db_execute(pager, req.body);

    /* 응답 본문 조립: out_buf(SELECT 결과) + message */
    char resp[8192];
    size_t off = 0;
    if (res.out_buf && res.out_len > 0) {
        size_t copy = res.out_len < sizeof(resp) - 1 ? res.out_len : sizeof(resp) - 1;
        memcpy(resp, res.out_buf, copy);
        off = copy;
    }
    /* 메시지 추가 */
    if (res.message[0] != '\0' && off < sizeof(resp) - 2) {
        int n = snprintf(resp + off, sizeof(resp) - off, "%s%s\n",
                         off > 0 ? "" : "", res.message);
        if (n > 0) off += (size_t)n;
    }

    if (res.status == 0) {
        http_send_ok(client_fd, resp, off);
    } else {
        http_send_error(client_fd, resp, off);
    }

    if (res.out_buf) free(res.out_buf);
}

/* ── worker 메인 루프 ── */
static void *worker_func(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;
    tl_pool = pool;  /* thread-local에 pool 포인터 저장 */

    while (1) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->queue_size == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->not_empty, &pool->mutex);

        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        /* dequeue */
        job_t job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->queue_cap;
        pool->queue_size--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        atomic_fetch_add(&pool->active_workers, 1);
        handle_client(pool->pager, job.client_fd);
        close(job.client_fd);
        atomic_fetch_sub(&pool->active_workers, 1);
        atomic_fetch_add(&pool->total_processed, 1);
    }
    return NULL;
}

/* ── 공개 API ── */

int thread_pool_init(thread_pool_t *pool, pager_t *pager,
                     int thread_count, int queue_cap)
{
    memset(pool, 0, sizeof(*pool));
    pool->pager = pager;

    /* 코어 수 기반 스레드 수 결정 */
    if (thread_count <= 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        thread_count = ncpu > 0 ? (int)ncpu : 4;
    }
    if (queue_cap <= 0) queue_cap = 64;

    pool->thread_count = thread_count;
    pool->queue_cap = queue_cap;
    pool->queue = (job_t *)calloc((size_t)queue_cap, sizeof(job_t));
    pool->threads = (pthread_t *)calloc((size_t)thread_count, sizeof(pthread_t));

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_func, pool) != 0) {
            fprintf(stderr, "[thread_pool] worker %d 생성 실패\n", i);
            pool->thread_count = i;
            return -1;
        }
    }

    fprintf(stderr, "[thread_pool] %d workers started (queue_cap=%d)\n",
            thread_count, queue_cap);
    return 0;
}

int thread_pool_submit(thread_pool_t *pool, int client_fd)
{
    pthread_mutex_lock(&pool->mutex);
    while (pool->queue_size == pool->queue_cap && !pool->shutdown)
        pthread_cond_wait(&pool->not_full, &pool->mutex);

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->queue[pool->tail].client_fd = client_fd;
    pool->tail = (pool->tail + 1) % pool->queue_cap;
    pool->queue_size++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void thread_pool_destroy(thread_pool_t *pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL);

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->queue);
    free(pool->threads);
}
