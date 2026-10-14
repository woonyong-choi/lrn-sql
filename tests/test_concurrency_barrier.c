/*
 * test_concurrency_barrier.c -- 힙 tail·B+ tree 루트·header dirty의 결정적 스케줄 검사 (#19)
 *
 * MINIDB_TEST_HOOKS로 컴파일한 pager·table·bptree에서만 동작한다. 훅이 특정 지점에서 스레드를
 * 세워 두어 아래 겹침을 시간 의존 없이 재현한다.
 *   1. 새 tail 게시 직전에 다른 스레드가 같은 tail의 latch를 잡고 놓는 경우:
 *      자신이 잡은 페이지의 latch·pin만 풀고, 이미 늘어난 tail은 새로 붙이지 않고 재사용한다.
 *   2. 같은 tail이 찬 것을 본 두 스레드가 동시에 확장하려는 경우: 페이지는 하나만 늘어난다.
 *   3. 루트를 읽은 뒤 latch를 기다리는 사이 루트가 분할된 경우: latch 뒤 재검증으로 다시 내려간다.
 *   4. flush가 header를 스냅샷한 뒤 들어온 변경, flush 실패: dirty 표시를 잃지 않는다.
 * 기본 실행은 위 정상 경로만 검사한다. 잠금 순서 검사기가 역순을 세는지는 일부러 역순으로 잠그는
 * 음성 시험이라 --check-order-detector로 별도 프로세스에서만 실행한다(TSan은 이 모드에서 진단하고 66으로 끝난다).
 * 모든 대기에는 제한 시간이 있어 정지하면 멈춘 위치를 출력하고 종료한다.
 */

#include "storage/bptree.h"
#include "storage/pager.h"
#include "storage/table.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ROW_SIZE 100
#define WAIT_SECONDS 20

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, (msg));              \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

static void hang(const char *what)
{
    fprintf(stderr, "HANG: %s (%d초 안에 끝나지 않음)\n", what, WAIT_SECONDS);
    _exit(3);
}

/* ── 관문: 도착을 세고 열릴 때까지 세워 둔다 ── */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int arrived;
    bool open;
} gate_t;

#define GATE_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, false}

static struct timespec deadline(void)
{
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);
    at.tv_sec += WAIT_SECONDS;
    return at;
}

static void gate_pass(gate_t *gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->arrived++;
    pthread_cond_broadcast(&gate->cond);
    struct timespec at = deadline();
    while (!gate->open) {
        if (pthread_cond_timedwait(&gate->cond, &gate->mutex, &at) != 0) hang("gate_pass");
    }
    pthread_mutex_unlock(&gate->mutex);
}

static void gate_wait_arrived(gate_t *gate, int count, const char *what)
{
    pthread_mutex_lock(&gate->mutex);
    struct timespec at = deadline();
    while (gate->arrived < count) {
        if (pthread_cond_timedwait(&gate->cond, &gate->mutex, &at) != 0) hang(what);
    }
    pthread_mutex_unlock(&gate->mutex);
}

static void gate_open(gate_t *gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->open = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

static void gate_reset(gate_t *gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->arrived = 0;
    gate->open = false;
    pthread_mutex_unlock(&gate->mutex);
}

/* ── worker ── */
enum { ROLE_NONE, ROLE_A, ROLE_B };
static _Thread_local int g_role;

typedef struct {
    pthread_t thread;
    _Atomic bool done;
    int role;
    pager_t *pager;
    uint64_t key;
    row_ref_t ref;
    bool found;
} worker_t;

static void start_worker(worker_t *worker, void *(*body)(void *))
{
    atomic_store(&worker->done, false);
    CHECK(pthread_create(&worker->thread, NULL, body, worker) == 0, "pthread_create");
}

static void join_worker(worker_t *worker, const char *what)
{
    for (int waited = 0; waited < WAIT_SECONDS * 1000; waited++) {
        if (atomic_load(&worker->done)) {
            pthread_join(worker->thread, NULL);
            return;
        }
        usleep(1000);
    }
    hang(what);
}

static void *insert_row_body(void *arg)
{
    worker_t *worker = arg;
    g_role = worker->role;
    uint8_t row[ROW_SIZE];
    memset(row, (int)worker->key, sizeof(row));
    worker->ref = heap_insert(worker->pager, row, ROW_SIZE);
    atomic_store(&worker->done, true);
    return NULL;
}

static void *search_body(void *arg)
{
    worker_t *worker = arg;
    g_role = worker->role;
    worker->found = bptree_search(worker->pager, worker->key, &worker->ref);
    atomic_store(&worker->done, true);
    return NULL;
}

static void *index_insert_body(void *arg)
{
    worker_t *worker = arg;
    g_role = worker->role;
    row_ref_t ref = {(uint32_t)worker->key, 1};
    worker->found = bptree_insert(worker->pager, worker->key, ref) == 0;
    atomic_store(&worker->done, true);
    return NULL;
}

/* ── 공통 준비 ── */
static char g_path[96];

/* 검사가 실패해 exit해도 임시 DB 파일이 남지 않게 한다 */
static void remove_db_file(void)
{
    if (g_path[0] != '\0') unlink(g_path);
}

static void open_db(pager_t *pager, const char *tag)
{
    static bool registered;
    if (!registered) {
        atexit(remove_db_file);
        registered = true;
    }
    snprintf(g_path, sizeof(g_path), "__test_19_%s_%d.db", tag, (int)getpid());
    unlink(g_path);
    CHECK(pager_open(pager, g_path, true) == 0, "pager_open");
}

static void close_db(pager_t *pager)
{
    CHECK(pager_close(pager) == 0, "pager_close");
    unlink(g_path);
}

static void assert_quiescent(pager_t *pager, const char *when)
{
    uint32_t pinned, latched;
    pager_test_quiescence(pager, &pinned, &latched);
    if (pinned != 0 || latched != 0) {
        fprintf(stderr, "%s: pin이 남은 프레임 %u, latch가 남은 프레임 %u\n", when, pinned, latched);
        exit(1);
    }
}

static bool count_row(const uint8_t *row, row_ref_t ref, void *ctx)
{
    (void)row;
    (void)ref;
    (*(size_t *)ctx)++;
    return true;
}

/* 힙 체인을 따라가며 페이지 수와 행 수를 센다. 같은 페이지를 두 번 지나면 실패한다 */
static size_t walk_heap_chain(pager_t *pager, uint32_t *last_page)
{
    size_t pages = 0;
    uint32_t seen[64];
    uint32_t pid = pager->header.first_heap_page_id;
    while (pid != 0) {
        for (size_t i = 0; i < pages; i++) CHECK(seen[i] != pid, "힙 체인에 순환");
        CHECK(pages < 64, "힙 체인이 너무 길다");
        seen[pages++] = pid;
        uint8_t *page = pager_get_page(pager, pid);
        CHECK(page != NULL, "힙 페이지 읽기");
        heap_page_header_t hph;
        memcpy(&hph, page, sizeof(hph));
        pager_unpin(pager, pid);
        *last_page = pid;
        pid = hph.next_heap_page_id;
    }
    return pages;
}

static size_t count_rows(pager_t *pager)
{
    size_t rows = 0;
    CHECK(heap_scan(pager, ROW_SIZE, count_row, &rows) == 0, "heap_scan");
    return rows;
}

/* 한 페이지에 들어가는 행 수: 새 DB에서 tail이 바뀔 때까지 넣어 센다 */
static uint32_t heap_page_capacity(void)
{
    pager_t probe;
    open_db(&probe, "heapcap");
    uint8_t row[ROW_SIZE] = {0};
    uint32_t rows = 0;
    while (atomic_load(&probe.last_heap_page_id) == 1) {
        row_ref_t ref = heap_insert(&probe, row, ROW_SIZE);
        CHECK(ref.page_id != 0, "probe insert");
        if (ref.page_id == 1) rows++;
    }
    close_db(&probe);
    CHECK(rows > 2, "한 페이지에 행이 두 개 이상 들어가야 한다");
    return rows;
}

/* 첫 리프가 가득 찬 뒤 루트가 바뀔 때까지 넣어 세운 키 수 */
static uint64_t leaf_capacity(void)
{
    pager_t probe;
    open_db(&probe, "leafcap");
    uint32_t first_root = pager_root_id(&probe);
    uint64_t keys = 0;
    while (pager_root_id(&probe) == first_root) {
        row_ref_t ref = {1, (uint16_t)(keys % 100)};
        CHECK(bptree_insert(&probe, ++keys, ref) == 0, "probe key");
    }
    close_db(&probe);
    return keys - 1;
}

/* ── 1. tail 게시 직전의 겹침 ── */
static gate_t g_publish = GATE_INIT, g_latch = GATE_INIT, g_lockwait = GATE_INIT;
static uint32_t g_first_tail;

static void hook_tail_window(minidb_hook_point_t point, pager_t *pager, uint32_t page_id)
{
    (void)pager;
    if (g_role == ROLE_B && point == MINIDB_HOOK_TAIL_PUBLISH) gate_pass(&g_publish);
    if (g_role == ROLE_A && point == MINIDB_HOOK_LATCH_ACQUIRED && page_id == g_first_tail) {
        gate_pass(&g_latch);
    }
}

static void test_tail_publish_window(void)
{
    uint32_t capacity = heap_page_capacity();
    pager_t pager;
    open_db(&pager, "window");
    uint8_t row[ROW_SIZE] = {7};
    for (uint32_t i = 0; i < capacity; i++) {
        CHECK(heap_insert(&pager, row, ROW_SIZE).page_id == 1, "첫 페이지를 채운다");
    }
    g_first_tail = 1;
    CHECK(atomic_load(&pager.last_heap_page_id) == 1, "아직 첫 페이지가 tail");
    uint64_t violations = pager_test_lock_order_violations();

    gate_reset(&g_publish);
    gate_reset(&g_latch);
    minidb_test_hook = hook_tail_window;
    worker_t extender = {.role = ROLE_B, .pager = &pager, .key = 2};
    worker_t observer = {.role = ROLE_A, .pager = &pager, .key = 3};

    start_worker(&extender, insert_row_body);
    gate_wait_arrived(&g_publish, 1, "확장 스레드가 게시 직전에 도달");
    /* 확장 스레드는 옛 tail에 새 페이지를 이은 뒤 latch를 풀었지만 아직 tail을 게시하지 않았다 */
    CHECK(atomic_load(&pager.last_heap_page_id) == 1, "게시 전에는 옛 tail");

    start_worker(&observer, insert_row_body);
    gate_wait_arrived(&g_latch, 1, "관찰 스레드가 옛 tail의 latch를 잡음");

    gate_open(&g_publish);
    join_worker(&extender, "확장 스레드");
    /* 페이지 2는 B+ tree 루트라 새 힙 페이지의 번호는 그 뒤다 */
    uint32_t second = atomic_load(&pager.last_heap_page_id);
    CHECK(second != 1 && extender.ref.page_id == second, "확장 스레드는 새 tail에 삽입");

    /* 관찰 스레드는 tail이 바뀐 뒤에도 자기가 잡은 페이지 1의 latch·pin을 풀어야 한다 */
    gate_open(&g_latch);
    join_worker(&observer, "관찰 스레드");
    minidb_test_hook = NULL;

    CHECK(observer.ref.page_id == second, "이미 늘어난 tail을 재사용");
    assert_quiescent(&pager, "tail 겹침 뒤");
    uint32_t last = 0;
    CHECK(walk_heap_chain(&pager, &last) == 2 && last == second, "새 페이지를 하나만 붙였다");
    CHECK(count_rows(&pager) == (size_t)capacity + 2, "행이 빠지지 않았다");
    CHECK(pager_test_lock_order_violations() == violations, "잠금 순서 위반 없음");
    close_db(&pager);
}

/* ── 2. 같은 tail을 두 스레드가 동시에 확장 ── */
static void hook_two_extenders(minidb_hook_point_t point, pager_t *pager, uint32_t page_id)
{
    (void)pager;
    (void)page_id;
    if (g_role != ROLE_NONE && point == MINIDB_HOOK_TAIL_LOCK_WAIT) gate_pass(&g_lockwait);
}

static void test_two_extenders_add_one_page(void)
{
    uint32_t capacity = heap_page_capacity();
    pager_t pager;
    open_db(&pager, "twin");
    uint8_t row[ROW_SIZE] = {9};
    for (uint32_t i = 0; i < capacity; i++) {
        CHECK(heap_insert(&pager, row, ROW_SIZE).page_id == 1, "첫 페이지를 채운다");
    }
    gate_reset(&g_lockwait);
    minidb_test_hook = hook_two_extenders;
    worker_t first = {.role = ROLE_A, .pager = &pager, .key = 4};
    worker_t second = {.role = ROLE_B, .pager = &pager, .key = 5};
    start_worker(&first, insert_row_body);
    start_worker(&second, insert_row_body);
    gate_wait_arrived(&g_lockwait, 2, "두 스레드가 확장 지점에 도달");
    gate_open(&g_lockwait);
    join_worker(&first, "첫째");
    join_worker(&second, "둘째");
    minidb_test_hook = NULL;

    uint32_t last = 0;
    CHECK(walk_heap_chain(&pager, &last) == 2, "같은 tail에서 페이지는 하나만 늘어난다");
    CHECK(last != 1 && first.ref.page_id == last && second.ref.page_id == last,
          "둘 다 새 tail에 삽입");
    CHECK(count_rows(&pager) == (size_t)capacity + 2, "행이 빠지지 않았다");
    assert_quiescent(&pager, "동시 확장 뒤");
    close_db(&pager);
}

/* ── 3. 루트를 읽은 뒤 분할 ── */
static gate_t g_root = GATE_INIT;
static _Atomic int g_root_reads[3];

static void hook_root_read(minidb_hook_point_t point, pager_t *pager, uint32_t page_id)
{
    (void)pager;
    (void)page_id;
    if (g_role == ROLE_NONE || point != MINIDB_HOOK_ROOT_READ) return;
    int reads = atomic_fetch_add(&g_root_reads[g_role], 1) + 1;
    if (reads == 1) gate_pass(&g_root);  /* 첫 읽기에서만 세운다. 재시도는 그대로 지나간다 */
}

static void fill_leaf(pager_t *pager, uint64_t capacity)
{
    for (uint64_t key = 1; key <= capacity; key++) {
        row_ref_t ref = {(uint32_t)key, (uint16_t)(key % 100)};
        CHECK(bptree_insert(pager, key, ref) == 0, "리프를 채운다");
    }
}

static void test_search_revalidates_root_after_split(void)
{
    uint64_t capacity = leaf_capacity();
    pager_t pager;
    open_db(&pager, "rootread");
    fill_leaf(&pager, capacity);
    uint32_t old_root = pager_root_id(&pager);

    gate_reset(&g_root);
    atomic_store(&g_root_reads[ROLE_A], 0);
    minidb_test_hook = hook_root_read;
    worker_t reader = {.role = ROLE_A, .pager = &pager, .key = capacity};
    start_worker(&reader, search_body);
    gate_wait_arrived(&g_root, 1, "검색 스레드가 옛 루트를 읽음");

    row_ref_t split_ref = {99, 1};
    CHECK(bptree_insert(&pager, capacity + 1, split_ref) == 0, "분할을 일으키는 삽입");
    CHECK(pager_root_id(&pager) != old_root, "루트가 새 페이지로 바뀌었다");

    gate_open(&g_root);
    join_worker(&reader, "검색 스레드");
    minidb_test_hook = NULL;

    /* 옛 루트는 이제 왼쪽 리프다. 재검증 없이 거기서 찾으면 오른쪽 리프의 가장 큰 키를 놓친다 */
    CHECK(reader.found, "루트가 바뀐 뒤에도 키를 찾는다");
    CHECK(reader.ref.page_id == (uint32_t)capacity, "올바른 값");
    CHECK(atomic_load(&g_root_reads[ROLE_A]) == 2, "latch 뒤 재검증으로 루트를 다시 읽었다");
    assert_quiescent(&pager, "검색 재시도 뒤");
    close_db(&pager);
}

static void test_insert_revalidates_root_after_split(void)
{
    uint64_t capacity = leaf_capacity();
    pager_t pager;
    open_db(&pager, "rootwrite");
    fill_leaf(&pager, capacity);
    uint32_t old_root = pager_root_id(&pager);

    gate_reset(&g_root);
    atomic_store(&g_root_reads[ROLE_B], 0);
    minidb_test_hook = hook_root_read;
    worker_t inserter = {.role = ROLE_B, .pager = &pager, .key = capacity + 2};
    start_worker(&inserter, index_insert_body);
    gate_wait_arrived(&g_root, 1, "삽입 스레드가 옛 루트를 읽음");

    row_ref_t split_ref = {98, 1};
    CHECK(bptree_insert(&pager, capacity + 1, split_ref) == 0, "분할을 일으키는 삽입");
    CHECK(pager_root_id(&pager) != old_root, "루트가 새 페이지로 바뀌었다");

    gate_open(&g_root);
    join_worker(&inserter, "삽입 스레드");
    minidb_test_hook = NULL;

    CHECK(inserter.found, "삽입 성공");
    CHECK(atomic_load(&g_root_reads[ROLE_B]) == 2, "latch 뒤 재검증으로 루트를 다시 읽었다");
    for (uint64_t key = 1; key <= capacity + 2; key++) {
        row_ref_t ref;
        CHECK(bptree_search(&pager, key, &ref), "모든 키가 새 루트에서 보인다");
    }
    assert_quiescent(&pager, "삽입 재시도 뒤");
    close_db(&pager);
}

/* ── 4. header dirty와 flush ── */
static bool g_fail_writes;

static ssize_t io_pread(int fd, void *buf, size_t n, off_t off)
{
    return pread(fd, buf, n, off);
}

static ssize_t io_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (g_fail_writes) {
        errno = EIO;
        return -1;
    }
    return pwrite(fd, buf, n, off);
}

static int io_fsync(int fd)
{
    return fsync(fd);
}

static const pager_io_t TEST_IO = {io_pread, io_pwrite, io_fsync};

static void hook_change_after_snapshot(minidb_hook_point_t point, pager_t *pager, uint32_t page_id)
{
    (void)page_id;
    /* 다른 스레드가 flush의 스냅샷 직후 header를 바꾼 상황 */
    if (point == MINIDB_HOOK_FLUSH_SNAPSHOT) pager_note_header_change(pager);
}

static bool header_dirty(pager_t *pager)
{
    return atomic_load(&pager->header_dirty);
}

static void test_header_dirty_survives_flush(void)
{
    pager_set_default_io(&TEST_IO);
    pager_t pager;
    open_db(&pager, "dirty");
    uint8_t row[ROW_SIZE] = {1};
    CHECK(heap_insert(&pager, row, ROW_SIZE).page_id == 1, "행 삽입");
    CHECK(header_dirty(&pager), "변경 뒤 header dirty");
    CHECK(pager_flush_all(&pager) == 0, "flush 성공");
    CHECK(!header_dirty(&pager), "성공한 flush는 dirty를 내린다");

    /* 스냅샷 뒤에 들어온 변경은 이번 flush에 담기지 않았으므로 dirty로 남아야 한다 */
    CHECK(heap_insert(&pager, row, ROW_SIZE).page_id == 1, "행 삽입");
    minidb_test_hook = hook_change_after_snapshot;
    CHECK(pager_flush_all(&pager) == 0, "flush 성공");
    minidb_test_hook = NULL;
    CHECK(header_dirty(&pager), "스냅샷 뒤의 변경 표시는 지워지지 않는다");
    CHECK(pager_flush_all(&pager) == 0 && !header_dirty(&pager), "다음 flush가 내린다");

    /* flush 실패는 내리기 전 dirty 값으로 되돌린다 */
    CHECK(heap_insert(&pager, row, ROW_SIZE).page_id == 1, "행 삽입");
    CHECK(header_dirty(&pager), "변경 뒤 header dirty");
    g_fail_writes = true;
    CHECK(pager_flush_all(&pager) == -1, "쓰기 실패는 flush 실패");
    CHECK(header_dirty(&pager), "실패하면 dirty가 복구된다");
    g_fail_writes = false;
    CHECK(pager_flush_all(&pager) == 0 && !header_dirty(&pager), "복구 뒤 flush 성공");

    /* 깨끗한 상태에서 실패해도 없던 dirty를 만들지 않는다 */
    g_fail_writes = true;
    CHECK(pager_flush_all(&pager) == -1, "header 쓰기 실패");
    CHECK(!header_dirty(&pager), "내리기 전에 dirty가 아니었다면 그대로 깨끗하다");
    g_fail_writes = false;
    CHECK(pager_flush_all(&pager) == 0, "다시 성공");

    CHECK(pager_close(&pager) == 0, "닫기");
    pager_t again;
    CHECK(pager_open(&again, g_path, false) == 0, "다시 열기");
    size_t rows = 0;
    CHECK(heap_scan(&again, ROW_SIZE, count_row, &rows) == 0 && rows == 3, "실패와 복구를 거친 행이 모두 있다");
    CHECK(pager_close(&again) == 0, "닫기");
    unlink(g_path);
    pager_set_default_io(NULL);
}

/* ── 5. 잠금 순서 검사기 자체 ── */
static void test_lock_order_checker_detects_reverse_order(void)
{
    pager_t pager;
    open_db(&pager, "order");
    uint64_t before = pager_test_lock_order_violations();
    /* 정방향: tail → pager_mutex 안의 header → 위반 없음 */
    pager_tail_lock(&pager);
    pager_header_lock(&pager);
    pager_header_unlock(&pager);
    pager_tail_unlock(&pager);
    CHECK(pager_test_lock_order_violations() == before, "정방향은 위반이 아니다");
    /* 역방향: header_lock을 쥔 채 tail_lock */
    pager_header_lock(&pager);
    pager_tail_lock(&pager);
    pager_tail_unlock(&pager);
    pager_header_unlock(&pager);
    CHECK(pager_test_lock_order_violations() == before + 1, "역방향을 센다");
    close_db(&pager);
}

/* 정상 경로: 모든 sanitizer에서 성공해야 한다. */
static int run_normal(void)
{
    test_tail_publish_window();
    test_two_extenders_add_one_page();
    test_search_revalidates_root_after_split();
    test_insert_revalidates_root_after_split();
    test_header_dirty_survives_flush();
    puts("concurrency barrier: tail window, single extension, root revalidation, header dirty and flush failure");
    return 0;
}

/* 음성 시험: 일부러 역순으로 잠가 검사기가 센다. 실제 lock-order-inversion이므로 TSan은 이 프로세스에서
 * 진단하고 종료 코드 66으로 끝나는 것이 기대 동작이다. 정상 경로와 같은 프로세스에 두지 않는다. */
static int run_order_detector(void)
{
    test_lock_order_checker_detects_reverse_order();
    puts("concurrency barrier: lock order detector counts a reverse acquisition");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return run_normal();
    if (argc == 2 && strcmp(argv[1], "--check-order-detector") == 0)
        return run_order_detector();
    fprintf(stderr, "usage: %s [--check-order-detector]\n", argv[0]);
    return 2;
}
