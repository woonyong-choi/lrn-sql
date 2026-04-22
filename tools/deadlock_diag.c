/*
 * deadlock_diag.c - 데드락/hang 진단용 멀티스레드 테스트
 *
 * 네트워크 없이 db_execute()를 직접 호출해서
 * 페이저/래치 계층에서 hang이 발생하는지 확인한다.
 */
#include "db.h"
#include "storage/pager.h"
#include "sql/executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static pager_t g_pager;
static atomic_int g_done = 0;
static atomic_int g_ok = 0;
static atomic_int g_fail = 0;
static atomic_int g_started = 0;
static int g_total_per_thread = 200;
static int g_num_threads = 8;

/* 시드 ID 범위: 1~100, 이후 INSERT가 추가 */
static atomic_uint_least64_t g_max_id;

static void *worker(void *arg) {
    int tid = (int)(long)arg;
    unsigned int seed = (unsigned int)(tid * 12345 + 67890);

    atomic_fetch_add(&g_started, 1);

    for (int i = 0; i < g_total_per_thread; i++) {
        int r = rand_r(&seed) % 100;
        char sql[256];
        exec_result_t res;

        if (r < 60) {
            /* INSERT */
            snprintf(sql, sizeof(sql),
                     "INSERT INTO users VALUES (0, 'user_%d_%d', 'u%d_%d@test.com', %d)",
                     tid, i, tid, i, 20 + (rand_r(&seed) % 40));
            res = db_execute(&g_pager, sql);
        } else if (r < 85) {
            /* SELECT by id */
            uint64_t max = atomic_load(&g_max_id);
            if (max < 1) max = 1;
            uint64_t id = 1 + (rand_r(&seed) % max);
            snprintf(sql, sizeof(sql),
                     "SELECT * FROM users WHERE id = %lu", (unsigned long)id);
            res = db_execute(&g_pager, sql);
        } else if (r < 95) {
            /* UPDATE */
            uint64_t max = atomic_load(&g_max_id);
            if (max < 1) max = 1;
            uint64_t id = 1 + (rand_r(&seed) % max);
            snprintf(sql, sizeof(sql),
                     "UPDATE users SET age = %d WHERE id = %lu",
                     20 + (rand_r(&seed) % 40), (unsigned long)id);
            res = db_execute(&g_pager, sql);
        } else {
            /* DELETE */
            uint64_t max = atomic_load(&g_max_id);
            if (max < 1) max = 1;
            uint64_t id = 1 + (rand_r(&seed) % max);
            snprintf(sql, sizeof(sql),
                     "DELETE FROM users WHERE id = %lu", (unsigned long)id);
            res = db_execute(&g_pager, sql);
        }

        if (res.status == 0) {
            atomic_fetch_add(&g_ok, 1);
        } else {
            atomic_fetch_add(&g_fail, 1);
        }

        int total = atomic_load(&g_ok) + atomic_load(&g_fail);
        if (total % 100 == 0) {
            fprintf(stderr, "\r  progress: %d/%d (ok=%d fail=%d)   ",
                    total, g_total_per_thread * g_num_threads,
                    atomic_load(&g_ok), atomic_load(&g_fail));
        }
    }

    atomic_fetch_add(&g_done, 1);
    return NULL;
}

int main(void) {
    const char *db_path = "__deadlock_diag__.db";

    fprintf(stderr, "=== Deadlock Diagnosis ===\n");
    fprintf(stderr, "Threads: %d, Requests/thread: %d\n", g_num_threads, g_total_per_thread);

    /* DB 초기화 */
    if (pager_open(&g_pager, db_path, true) != 0) {
        fprintf(stderr, "pager_open failed\n");
        return 1;
    }
    db_init();

    /* 스키마 설정 */
    exec_result_t r = db_execute(&g_pager, "CREATE TABLE users (id BIGINT, name VARCHAR(32), email VARCHAR(64), age INT)");
    if (r.status != 0) {
        fprintf(stderr, "CREATE TABLE failed: %s\n", r.message);
        return 1;
    }

    /* 시드 100건 */
    fprintf(stderr, "Seeding 100 rows...\n");
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES (0, 'seed_%d', 'seed%d@test.com', %d)",
                 i, i, 20 + (i % 40));
        r = db_execute(&g_pager, sql);
        if (r.status != 0) {
            fprintf(stderr, "Seed %d failed: %s\n", i, r.message);
        }
    }
    atomic_store(&g_max_id, 100);
    fprintf(stderr, "Seed complete. Starting concurrent phase...\n");

    /* 워커 스레드 생성 */
    pthread_t threads[32];
    for (int i = 0; i < g_num_threads; i++) {
        pthread_create(&threads[i], NULL, worker, (void *)(long)i);
    }

    /* 타임아웃 감시: 30초 */
    for (int sec = 0; sec < 30; sec++) {
        sleep(1);
        int done = atomic_load(&g_done);
        int ok = atomic_load(&g_ok);
        int fail = atomic_load(&g_fail);
        fprintf(stderr, "\r  [%2ds] done_threads=%d/%d  ok=%d fail=%d total=%d   ",
                sec + 1, done, g_num_threads, ok, fail, ok + fail);
        if (done == g_num_threads) break;
    }
    fprintf(stderr, "\n");

    int done = atomic_load(&g_done);
    if (done < g_num_threads) {
        fprintf(stderr, "HANG DETECTED: only %d/%d threads finished!\n", done, g_num_threads);
        fprintf(stderr, "ok=%d fail=%d total=%d\n",
                atomic_load(&g_ok), atomic_load(&g_fail),
                atomic_load(&g_ok) + atomic_load(&g_fail));

        /* 서버 상태 덤프 */
        fprintf(stderr, "\nPager state:\n");
        int pinned = 0, dirty = 0, valid = 0;
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (g_pager.frames[i].is_valid) {
                valid++;
                if (g_pager.frames[i].is_dirty) dirty++;
                if (g_pager.frames[i].pin_count > 0) pinned++;
            }
        }
        fprintf(stderr, "  frames: valid=%d dirty=%d pinned=%d / %d\n",
                valid, dirty, pinned, MAX_FRAMES);

        /* pinned 프레임 상세 */
        fprintf(stderr, "  pinned frames detail:\n");
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (g_pager.frames[i].is_valid && g_pager.frames[i].pin_count > 0) {
                fprintf(stderr, "    frame[%d]: page_id=%u pin=%u dirty=%d\n",
                        i, g_pager.frames[i].page_id,
                        g_pager.frames[i].pin_count,
                        g_pager.frames[i].is_dirty);
            }
        }

        /* 강제 종료 */
        _exit(1);
    }

    /* 정상 종료 */
    for (int i = 0; i < g_num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    fprintf(stderr, "\nSUCCESS: all %d threads completed\n", g_num_threads);
    fprintf(stderr, "ok=%d fail=%d\n", atomic_load(&g_ok), atomic_load(&g_fail));

    db_destroy();
    pager_close(&g_pager);
    unlink(db_path);
    return 0;
}
