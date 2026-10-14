/*
 * test_concurrency_insert.c -- 동시 INSERT 병렬 stress와 종료 뒤 불변식 검사 (#19)
 *
 * MINIDB_TEST_HOOKS로 컴파일한 전체 소스에서 동작한다. writer 여러 개가 힙 페이지 경계와
 * B+ tree 루트 분할을 넘도록 INSERT하는 동안 reader가 COUNT와 점 조회를 계속한다.
 * 끝난 뒤 다음을 확인한다.
 *   - 모든 worker가 제한 시간 안에 끝난다. 못 끝나면 스레드별 마지막 대기 위치(훅이 기록한 지점과
 *     page ID)를 출력하고 종료한다.
 *   - row_count·힙 행 수·인덱스 키가 기대 건수와 같고 힙 체인에 중복·누락이 없다.
 *   - 남은 pin·latch가 없고 잠금 순서 위반이 없다.
 *   - flush·재열기 뒤에도 같은 행이 보인다.
 * 성공은 이 실행 경로에 한정된 증거이며 모든 interleaving이나 과거 CI 정지의 원인을 증명하지 않는다.
 */

#include "db.h"
#include "storage/bptree.h"
#include "storage/pager.h"
#include "storage/table.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 32
#define WAIT_SECONDS 120

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, (msg));              \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

/* ── 스레드별 마지막 진행 위치 ── */
typedef struct {
    _Atomic int point;
    _Atomic uint32_t page;
    _Atomic uint64_t events;
    const char *name;
} progress_t;

static progress_t g_progress[MAX_THREADS];
static _Thread_local progress_t *g_mine;

static void hook_progress(minidb_hook_point_t point, pager_t *pager, uint32_t page_id)
{
    (void)pager;
    if (g_mine == NULL) return;
    atomic_store(&g_mine->point, (int)point);
    atomic_store(&g_mine->page, page_id);
    atomic_fetch_add(&g_mine->events, 1);
}

static const char *point_name(int point)
{
    static const char *names[] = {"TAIL_READ", "TAIL_LOCK_WAIT", "TAIL_LINK", "TAIL_PUBLISH",
                                  "ROOT_READ", "FLUSH_SNAPSHOT", "LATCH_WAIT", "LATCH_ACQUIRED"};
    return point >= 0 && point < (int)(sizeof(names) / sizeof(names[0])) ? names[point] : "none";
}

static void dump_progress_and_exit(size_t threads)
{
    fprintf(stderr, "HANG: %d초 안에 worker가 끝나지 않음. 스레드별 마지막 위치:\n", WAIT_SECONDS);
    for (size_t i = 0; i < threads; i++) {
        fprintf(stderr, "  %-8s 마지막 훅 %-15s page %u (훅 %" PRIu64 "회)\n", g_progress[i].name,
                point_name(atomic_load(&g_progress[i].point)), atomic_load(&g_progress[i].page),
                atomic_load(&g_progress[i].events));
    }
    _exit(3);
}

/* ── worker ── */
typedef struct {
    pthread_t thread;
    size_t index;
    pager_t *pager;
    int inserts;
    int tag;
    bool reader;
    int failures;
} worker_t;

static _Atomic int g_writers_left;
static _Atomic size_t g_finished;

static void *worker_body(void *arg)
{
    worker_t *worker = arg;
    g_mine = &g_progress[worker->index];
    char sql[160];
    if (!worker->reader) {
        for (int i = 0; i < worker->inserts; i++) {
            snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('t%d_r%d', %d)", worker->tag, i, i);
            exec_result_t result = db_execute(worker->pager, sql);
            if (result.status != 0) worker->failures++;
            free(result.out_buf);
        }
        atomic_fetch_sub(&g_writers_left, 1);
    } else {
        int rounds = 0;
        while (atomic_load(&g_writers_left) > 0 || rounds < 3) {
            exec_result_t count = db_execute(worker->pager, "SELECT COUNT(*) FROM users");
            if (count.status != 0) worker->failures++;
            free(count.out_buf);
            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id = %d", 1 + rounds % 50);
            exec_result_t point = db_execute(worker->pager, sql);
            if (point.status != 0) worker->failures++;
            free(point.out_buf);
            rounds++;
        }
    }
    atomic_fetch_add(&g_finished, 1);
    return NULL;
}

/* ── 종료 뒤 불변식 ── */
static bool count_row(const uint8_t *row, row_ref_t ref, void *ctx)
{
    (void)row;
    (void)ref;
    (*(size_t *)ctx)++;
    return true;
}

static void check_heap_chain(pager_t *pager, size_t expected_rows)
{
    static uint32_t seen[8192];
    size_t pages = 0;
    uint32_t pid = pager->header.first_heap_page_id, last = 0;
    while (pid != 0) {
        for (size_t i = 0; i < pages; i++) CHECK(seen[i] != pid, "힙 체인에 순환 또는 중복");
        CHECK(pages < sizeof(seen) / sizeof(seen[0]), "힙 체인이 너무 길다");
        seen[pages++] = pid;
        uint8_t *page = pager_get_page(pager, pid);
        CHECK(page != NULL, "힙 페이지 읽기");
        heap_page_header_t hph;
        memcpy(&hph, page, sizeof(hph));
        pager_unpin(pager, pid);
        last = pid;
        pid = hph.next_heap_page_id;
    }
    CHECK(pages >= 2, "힙 페이지 경계를 넘도록 삽입했다");
    CHECK(atomic_load(&pager->last_heap_page_id) == last, "tail이 체인의 마지막 페이지다");
    size_t rows = 0;
    CHECK(heap_scan(pager, pager->header.row_size, count_row, &rows) == 0, "heap_scan");
    CHECK(rows == expected_rows, "체인의 행 수");
}

static void check_index(pager_t *pager, size_t expected_rows, uint32_t first_root)
{
    for (uint64_t key = 1; key <= expected_rows; key++) {
        row_ref_t ref;
        CHECK(bptree_search(pager, key, &ref), "모든 id가 인덱스에 있다");
    }
    row_ref_t ref;
    CHECK(!bptree_search(pager, expected_rows + 1, &ref), "없는 키는 없다");
    CHECK(pager_root_id(pager) != first_root, "루트가 분할을 거쳤다");
}

static void run_round(int writers, int readers, int inserts, int round)
{
    char path[64];
    snprintf(path, sizeof(path), "__test_19_stress_%d_%d.db", (int)getpid(), round);
    unlink(path);
    pager_t pager;
    CHECK(pager_open(&pager, path, true) == 0, "pager_open");
    db_init();
    exec_result_t create = db_execute(&pager, "CREATE TABLE users (name VARCHAR(32), score INT)");
    CHECK(create.status == 0, "CREATE TABLE");
    free(create.out_buf);
    uint32_t first_root = pager_root_id(&pager);
    uint64_t violations = pager_test_lock_order_violations();

    worker_t workers[MAX_THREADS];
    size_t total_threads = (size_t)(writers + readers);
    CHECK(total_threads <= MAX_THREADS, "스레드 수");
    atomic_store(&g_writers_left, writers);
    atomic_store(&g_finished, 0);
    minidb_test_hook = hook_progress;
    for (size_t i = 0; i < total_threads; i++) {
        bool reader = i >= (size_t)writers;
        workers[i] = (worker_t){.index = i, .pager = &pager, .inserts = inserts,
                                .tag = (int)i, .reader = reader};
        g_progress[i] = (progress_t){.name = reader ? "reader" : "writer"};
        CHECK(pthread_create(&workers[i].thread, NULL, worker_body, &workers[i]) == 0, "pthread_create");
    }
    for (int waited = 0; atomic_load(&g_finished) < total_threads; waited++) {
        if (waited >= WAIT_SECONDS * 100) dump_progress_and_exit(total_threads);
        usleep(10000);
    }
    for (size_t i = 0; i < total_threads; i++) pthread_join(workers[i].thread, NULL);
    minidb_test_hook = NULL;

    size_t expected = (size_t)writers * (size_t)inserts;
    for (size_t i = 0; i < total_threads; i++) CHECK(workers[i].failures == 0, "worker의 문장이 모두 성공");
    CHECK(pager.header.row_count == expected, "header row_count");
    check_heap_chain(&pager, expected);
    check_index(&pager, expected, first_root);
    uint32_t pinned, latched;
    pager_test_quiescence(&pager, &pinned, &latched);
    CHECK(pinned == 0 && latched == 0, "남은 pin·latch 없음");
    CHECK(pager_test_lock_order_violations() == violations, "잠금 순서 위반 없음");

    /* flush와 재열기 뒤에도 같은 행 */
    CHECK(pager_flush_all(&pager) == 0, "flush");
    CHECK(!atomic_load(&pager.header_dirty), "flush 뒤 header dirty 없음");
    db_destroy();
    CHECK(pager_close(&pager) == 0, "pager_close");
    pager_t again;
    CHECK(pager_open(&again, path, false) == 0, "다시 열기");
    CHECK(again.header.row_count == expected, "재열기 뒤 row_count");
    size_t rows = 0;
    CHECK(heap_scan(&again, again.header.row_size, count_row, &rows) == 0 && rows == expected,
          "재열기 뒤 행 수");
    for (uint64_t key = 1; key <= expected; key++) {
        row_ref_t ref;
        CHECK(bptree_search(&again, key, &ref), "재열기 뒤 모든 id");
    }
    CHECK(pager_close(&again) == 0, "pager_close");
    unlink(path);
}

int main(void)
{
    /* 한 페이지(수백 행)와 리프 하나(수백 키)를 넘기도록 건수를 잡는다 */
    run_round(4, 1, 600, 0);
    run_round(8, 2, 400, 1);
    run_round(16, 2, 250, 2);
    puts("concurrency insert: writers and readers finish, rows and keys match, no pin/latch/order violation");
    return 0;
}
