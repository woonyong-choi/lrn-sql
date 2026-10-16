/*
 * test_pk_tree.c -- 기본 키 색인(#158, storage/pk_tree.h) 검사
 *
 * 메모리 위의 가짜 페이지 저장소로 COW 계약을 대조한다: commit된 페이지는 절대 다시 쓰지 않고, commit 뒤
 * 놓은 페이지만 사라지며, 도달 가능한 페이지 수와 살아 있는 페이지 수가 같다(누수·이중 해제 없음).
 * 기대 결과는 구현이 아니라 정렬된 배열 모델에서 얻는다. 합성 데이터만 쓴다.
 *
 * 빌드: tests/test_pk_tree.c, src/storage/pk_tree.c
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/pk_tree.h"

#define TABLE_ID 7u
#define MAX_PAGES 4096u

static int g_run = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_run++;                                          \
        if (!(cond)) {                                    \
            g_fail++;                                     \
            printf("  FAIL (line %d): ", __LINE__);       \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

typedef enum { PAGE_UNUSED, PAGE_FRESH, PAGE_COMMITTED, PAGE_FREED } page_state_t;

typedef struct {
    uint8_t body[MAX_PAGES][PAGE_BODY_BYTES];
    page_state_t state[MAX_PAGES];
    uint32_t next;
    uint32_t old_released[MAX_PAGES];
    size_t old_count;
    long committed_writes;  /* commit된 페이지에 쓴 횟수. 0이어야 한다 */
    long bad_reads;
    long fail_alloc_at, alloc_calls;
    long reads;
} store_t;

static pk_status_t fake_read(void *context, uint32_t page_id, uint8_t body[PAGE_BODY_BYTES])
{
    store_t *store = context;
    store->reads++;
    if (page_id >= store->next || (store->state[page_id] != PAGE_FRESH && store->state[page_id] != PAGE_COMMITTED)) {
        store->bad_reads++;
        return PK_CORRUPT;
    }
    memcpy(body, store->body[page_id], PAGE_BODY_BYTES);
    return PK_OK;
}

static pk_status_t fake_write(void *context, uint32_t page_id, const uint8_t body[PAGE_BODY_BYTES])
{
    store_t *store = context;
    if (page_id >= store->next || store->state[page_id] == PAGE_COMMITTED) store->committed_writes++;
    if (page_id >= store->next || store->state[page_id] == PAGE_FREED) return PK_IO;
    memcpy(store->body[page_id], body, PAGE_BODY_BYTES);
    return PK_OK;
}

static pk_status_t fake_allocate(void *context, uint32_t *page_id)
{
    store_t *store = context;
    if (store->fail_alloc_at != 0 && ++store->alloc_calls == store->fail_alloc_at) return PK_NOMEM;
    assert(store->next < MAX_PAGES);
    *page_id = store->next++;
    store->state[*page_id] = PAGE_FRESH;
    return PK_OK;
}

static pk_status_t fake_release(void *context, uint32_t page_id)
{
    store_t *store = context;
    if (store->state[page_id] == PAGE_FRESH) {
        store->state[page_id] = PAGE_FREED;
        return PK_OK;
    }
    if (store->state[page_id] != PAGE_COMMITTED) return PK_IO;
    store->old_released[store->old_count++] = page_id;
    return PK_OK;
}

static bool fake_is_fresh(void *context, uint32_t page_id)
{
    return ((store_t *)context)->state[page_id] == PAGE_FRESH;
}

static pk_tree_io_t make_io(store_t *store)
{
    pk_tree_io_t io = {store, fake_read, fake_write, fake_allocate, fake_release, fake_is_fresh};
    return io;
}

/* 게시 성공: 이 batch의 페이지가 commit되고 대체된 옛 페이지가 놓인다. */
static void commit(store_t *store)
{
    for (uint32_t i = 2; i < store->next; i++) {
        if (store->state[i] == PAGE_FRESH) store->state[i] = PAGE_COMMITTED;
    }
    for (size_t i = 0; i < store->old_count; i++) store->state[store->old_released[i]] = PAGE_FREED;
    store->old_count = 0;
}

/* 게시하지 못한 batch: fresh 페이지를 놓고 옛 페이지 목록은 버린다. */
static void abort_batch(store_t *store)
{
    for (uint32_t i = 2; i < store->next; i++) {
        if (store->state[i] == PAGE_FRESH) store->state[i] = PAGE_FREED;
    }
    store->old_count = 0;
}

static store_t *new_store(void)
{
    store_t *store = calloc(1, sizeof(*store));
    assert(store != NULL);
    store->next = 2;  /* 제어 페이지 자리 */
    return store;
}

static bool count_page(void *context, uint32_t page_id)
{
    (void)page_id;
    ++*(size_t *)context;
    return true;
}

static size_t live_pages(const store_t *store)
{
    size_t live = 0;
    for (uint32_t i = 2; i < store->next; i++) live += store->state[i] == PAGE_COMMITTED || store->state[i] == PAGE_FRESH;
    return live;
}

/* ── 정렬 배열 모델 ── */

typedef struct {
    int64_t key;
    heap_locator_t locator;
} model_entry_t;

typedef struct {
    model_entry_t *items;
    size_t count;
} model_t;

static size_t model_find(const model_t *model, int64_t key, bool *found)
{
    size_t low = 0, high = model->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (model->items[mid].key < key) low = mid + 1;
        else high = mid;
    }
    *found = low < model->count && model->items[low].key == key;
    return low;
}

static heap_locator_t locator_for(int64_t key, unsigned salt)
{
    return (heap_locator_t){(uint32_t)((uint64_t)key * 31u + salt), (uint16_t)(((uint64_t)key + salt) & 0x7fff)};
}

static bool same(heap_locator_t a, heap_locator_t b)
{
    return a.directory_index == b.directory_index && a.slot == b.slot;
}

typedef struct {
    const model_t *model;
    size_t next;      /* 다음에 와야 할 모델 항목 */
    size_t limit;     /* 이 수만큼 받으면 멈춘다 */
    size_t seen;
    bool ok;
} range_check_t;

static bool check_range_entry(void *context, int64_t key, heap_locator_t locator)
{
    range_check_t *check = context;
    const model_t *model = check->model;
    if (check->next >= model->count || model->items[check->next].key != key ||
        !same(model->items[check->next].locator, locator)) {
        check->ok = false;
        return false;
    }
    check->next++;
    return ++check->seen < check->limit;
}

/* #180: [low, high] 순회가 모델의 같은 구간을 순서대로 빠짐·중복 없이 돌려주고, 멈춤 요청에서 바로 멈춘다. */
static bool range_matches_model(const pk_tree_io_t *io, uint32_t root, const model_t *model, int64_t low,
                                int64_t high, size_t limit)
{
    bool unused;
    range_check_t check = {model, model_find(model, low, &unused), limit, 0, true};
    size_t end = model_find(model, high, &unused);
    if (unused) end++;
    size_t expected = low > high || end < check.next ? 0 : end - check.next;
    bool stopped = false;
    if (pk_tree_visit_range(io, TABLE_ID, root, low, high, check_range_entry, &check, &stopped) != PK_OK) {
        return false;
    }
    size_t want = expected < limit ? expected : limit;
    return check.ok && check.seen == want && stopped == (limit <= expected && limit > 0 && want == limit);
}

static bool tree_matches_model(const pk_tree_io_t *io, uint32_t root, const model_t *model, int64_t lowest,
                               int64_t highest)
{
    for (size_t i = 0; i < model->count; i++) {
        heap_locator_t found;
        if (pk_tree_find(io, TABLE_ID, root, model->items[i].key, &found) != PK_OK ||
            !same(found, model->items[i].locator)) {
            return false;
        }
    }
    for (int64_t key = lowest; key <= highest; key++) {
        bool in_model;
        model_find(model, key, &in_model);
        heap_locator_t found;
        pk_status_t status = pk_tree_find(io, TABLE_ID, root, key, &found);
        if (in_model != (status == PK_OK) || (!in_model && status != PK_NOT_FOUND)) return false;
    }
    /* 구간 끝이 내부 노드의 경계 키와 같을 때도 그 키를 빠뜨리지 않는다 */
    for (size_t i = 0; i < model->count; i++) {
        int64_t key = model->items[i].key;
        if (key > INT64_MIN && !range_matches_model(io, root, model, key - 1, key, SIZE_MAX)) return false;
    }
    int64_t span = highest - lowest;
    int64_t bounds[][2] = {{lowest, highest}, {lowest - 5, lowest + span / 3}, {lowest + span / 2, highest + 5},
                           {lowest + span / 4, lowest + span / 4}, {highest, lowest}, {INT64_MIN, INT64_MAX}};
    for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
        if (!range_matches_model(io, root, model, bounds[i][0], bounds[i][1], SIZE_MAX) ||
            !range_matches_model(io, root, model, bounds[i][0], bounds[i][1], 3)) {
            return false;
        }
    }
    bool present = false;
    int64_t maximum = 0;
    if (pk_tree_max(io, TABLE_ID, root, &present, &maximum) != PK_OK) return false;
    if (present != (model->count > 0)) return false;
    return !present || maximum == model->items[model->count - 1].key;
}

static bool pages_match_reachable(const store_t *store, const pk_tree_io_t *io, uint32_t root)
{
    size_t reachable = 0;
    return pk_tree_visit_pages(io, TABLE_ID, root, count_page, &reachable) == PK_OK &&
           reachable == live_pages(store);
}

/* ── 검사 ── */

static void test_sequential_inserts_fill_leaves(void)
{
    printf("\n[test_sequential_inserts_fill_leaves]\n");
    store_t *store = new_store();
    pk_tree_io_t io = make_io(store);
    uint32_t root;
    assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
    model_t model = {calloc(4000, sizeof(model_entry_t)), 0};
    for (int64_t key = 1; key <= 3000; key++) {
        assert(pk_tree_put(&io, TABLE_ID, &root, key, locator_for(key, 0), false, NULL) == PK_OK);
        model.items[model.count++] = (model_entry_t){key, locator_for(key, 0)};
    }
    CHECK(tree_matches_model(&io, root, &model, 0, 3010), "오름차순 삽입 뒤 모델과 같음");
    commit(store);
    size_t pages = 0;
    pk_tree_visit_pages(&io, TABLE_ID, root, count_page, &pages);
    CHECK(pages <= 9, "오름차순 삽입은 leaf를 가득 채워 쪼갠다 (노드 %zu개)", pages);
    CHECK(store->committed_writes == 0, "commit 전에 만든 페이지만 제자리에 씀");
    CHECK(pages_match_reachable(store, &io, root), "도달 가능한 페이지 수와 살아 있는 페이지 수가 같음");
    free(model.items);
    free(store);
}

static void test_random_batches_keep_copy_on_write(void)
{
    printf("\n[test_random_batches_keep_copy_on_write]\n");
    store_t *store = new_store();
    pk_tree_io_t io = make_io(store);
    uint32_t root;
    assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
    commit(store);
    model_t model = {calloc(6000, sizeof(model_entry_t)), 0};
    unsigned seed = 12345;
    for (int round = 0; round < 120; round++) {
        for (int op = 0; op < 150; op++) {
            seed = seed * 1103515245u + 12345u;
            int64_t key = (int64_t)((seed >> 8) % 5000);
            bool found;
            size_t at = model_find(&model, key, &found);
            if (((seed >> 4) & 3u) == 0 && found) {
                heap_locator_t removed;
                CHECK(pk_tree_remove(&io, TABLE_ID, &root, key, &removed) == PK_OK && same(removed, model.items[at].locator),
                      "지운 값이 모델과 같음");
                memmove(&model.items[at], &model.items[at + 1], (model.count - at - 1) * sizeof(model_entry_t));
                model.count--;
            } else if (!found) {
                heap_locator_t value = locator_for(key, (unsigned)round);
                CHECK(pk_tree_put(&io, TABLE_ID, &root, key, value, false, NULL) == PK_OK, "새 키 삽입");
                memmove(&model.items[at + 1], &model.items[at], (model.count - at) * sizeof(model_entry_t));
                model.items[at] = (model_entry_t){key, value};
                model.count++;
            } else {
                heap_locator_t value = locator_for(key, (unsigned)round + 99), previous;
                CHECK(pk_tree_put(&io, TABLE_ID, &root, key, value, true, &previous) == PK_OK &&
                          same(previous, model.items[at].locator),
                      "교체는 옛 값을 돌려줌");
                model.items[at].locator = value;
            }
        }
        commit(store);
        if (round % 20 == 19) {
            CHECK(tree_matches_model(&io, root, &model, 0, 5010), "라운드 %d 뒤 모델과 같음", round);
            CHECK(pages_match_reachable(store, &io, root), "라운드 %d 뒤 페이지 누수·이중 해제 없음", round);
        }
    }
    CHECK(store->committed_writes == 0, "commit된 페이지를 제자리에 쓴 적이 없음 (%ld)", store->committed_writes);
    CHECK(store->bad_reads == 0, "놓은 페이지를 읽은 적이 없음 (%ld)", store->bad_reads);
    free(model.items);
    free(store);
}

static void test_remove_everything_leaves_an_empty_tree(void)
{
    printf("\n[test_remove_everything_leaves_an_empty_tree]\n");
    store_t *store = new_store();
    pk_tree_io_t io = make_io(store);
    uint32_t root;
    assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
    for (int64_t key = 1; key <= 1500; key++) {
        assert(pk_tree_put(&io, TABLE_ID, &root, key, locator_for(key, 0), false, NULL) == PK_OK);
    }
    commit(store);
    for (int64_t key = 1; key <= 1500; key++) {
        heap_locator_t removed;
        assert(pk_tree_remove(&io, TABLE_ID, &root, key, &removed) == PK_OK);
        if (key % 100 == 0) commit(store);
    }
    commit(store);
    bool present = true;
    int64_t maximum = 0;
    CHECK(pk_tree_max(&io, TABLE_ID, root, &present, &maximum) == PK_OK && !present, "모두 지우면 비어 있음");
    heap_locator_t found;
    CHECK(pk_tree_find(&io, TABLE_ID, root, 5, &found) == PK_NOT_FOUND, "지운 키는 없음");
    CHECK(pages_match_reachable(store, &io, root) && live_pages(store) == 1, "빈 트리는 root 한 장만 남김 (%zu)",
          live_pages(store));
    CHECK(pk_tree_put(&io, TABLE_ID, &root, 3, locator_for(3, 0), false, NULL) == PK_OK &&
              pk_tree_find(&io, TABLE_ID, root, 3, &found) == PK_OK,
          "비운 뒤에도 다시 넣을 수 있음");
    free(store);
}

static void test_duplicate_and_missing_keys(void)
{
    printf("\n[test_duplicate_and_missing_keys]\n");
    store_t *store = new_store();
    pk_tree_io_t io = make_io(store);
    uint32_t root;
    assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
    heap_locator_t first = {4, 2}, other = {9, 1}, previous = {0, 0}, found;
    CHECK(pk_tree_put(&io, TABLE_ID, &root, INT64_MIN, first, false, NULL) == PK_OK, "최소 키");
    CHECK(pk_tree_put(&io, TABLE_ID, &root, INT64_MAX, first, false, NULL) == PK_OK, "최대 키");
    commit(store);
    uint32_t before = root;
    CHECK(pk_tree_put(&io, TABLE_ID, &root, INT64_MAX, other, false, &previous) == PK_EXISTS && same(previous, first),
          "중복 삽입은 EXISTS이고 기존 값을 알려 줌");
    CHECK(root == before && store->old_count == 0 && live_pages(store) == 1, "거절된 삽입은 아무것도 바꾸지 않음");
    CHECK(pk_tree_remove(&io, TABLE_ID, &root, 0, &found) == PK_NOT_FOUND && root == before, "없는 키 삭제는 NOT_FOUND");
    CHECK(pk_tree_find(&io, TABLE_ID, root, INT64_MIN, &found) == PK_OK && same(found, first), "INT64_MIN 조회");
    bool present;
    int64_t maximum;
    CHECK(pk_tree_max(&io, TABLE_ID, root, &present, &maximum) == PK_OK && present && maximum == INT64_MAX,
          "INT64_MAX가 최대");
    free(store);
}

static void test_failed_batch_leaves_committed_tree_intact(void)
{
    printf("\n[test_failed_batch_leaves_committed_tree_intact]\n");
    for (long fail_at = 1; fail_at <= 5; fail_at++) {
        store_t *store = new_store();
        pk_tree_io_t io = make_io(store);
        uint32_t root;
        assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
        model_t model = {calloc(2000, sizeof(model_entry_t)), 0};
        for (int64_t key = 1; key <= 1200; key++) {
            assert(pk_tree_put(&io, TABLE_ID, &root, key, locator_for(key, 0), false, NULL) == PK_OK);
            model.items[model.count++] = (model_entry_t){key, locator_for(key, 0)};
        }
        commit(store);
        uint32_t committed_root = root;
        store->fail_alloc_at = fail_at;
        store->alloc_calls = 0;
        pk_status_t status = PK_OK;
        for (int64_t key = 2000; key < 3600 && status == PK_OK; key++) {
            status = pk_tree_put(&io, TABLE_ID, &root, key, locator_for(key, 1), false, NULL);
        }
        CHECK(status == PK_NOMEM, "할당 실패 %ld번째가 전달됨 (%d)", fail_at, (int)status);
        abort_batch(store);
        CHECK(tree_matches_model(&io, committed_root, &model, 0, 1210), "실패한 batch 뒤에도 commit된 트리가 그대로 (%ld)", fail_at);
        CHECK(store->committed_writes == 0, "실패 전에도 commit된 페이지를 쓰지 않음 (%ld)", fail_at);
        CHECK(pages_match_reachable(store, &io, committed_root), "버린 batch의 페이지가 남지 않음 (%ld)", fail_at);
        free(model.items);
        free(store);
    }
}

static void test_damaged_nodes_are_corrupt(void)
{
    printf("\n[test_damaged_nodes_are_corrupt]\n");
    struct {
        const char *name;
        size_t offset;
        uint8_t value;
    } cases[] = {
        {"magic", 0, 'X'},
        {"version", 4, 9},
        {"level", 6, 9},
        {"table id", 8, 99},
        {"count", 13, 0xff},
        {"key order", 24 + 16, 0},  /* 둘째 leaf 항목 key의 최하위 바이트를 0으로 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        store_t *store = new_store();
        pk_tree_io_t io = make_io(store);
        uint32_t root;
        assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
        for (int64_t key = 1; key <= 5; key++) {
            assert(pk_tree_put(&io, TABLE_ID, &root, key * 10, locator_for(key, 0), false, NULL) == PK_OK);
        }
        commit(store);
        store->body[root][cases[i].offset] = cases[i].value;
        heap_locator_t found;
        CHECK(pk_tree_find(&io, TABLE_ID, root, 20, &found) == PK_CORRUPT, "%s가 어긋나면 CORRUPT", cases[i].name);
        free(store);
    }
    /* child의 level이 부모보다 1 작지 않으면 CORRUPT */
    store_t *store = new_store();
    pk_tree_io_t io = make_io(store);
    uint32_t root;
    assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
    for (int64_t key = 1; key <= 1200; key++) {
        assert(pk_tree_put(&io, TABLE_ID, &root, key, locator_for(key, 0), false, NULL) == PK_OK);
    }
    commit(store);
    uint32_t child = (uint32_t)store->body[root][24 + 8] | (uint32_t)store->body[root][24 + 9] << 8;
    store->body[child][6] = 3;
    heap_locator_t found;
    CHECK(pk_tree_find(&io, TABLE_ID, root, 1, &found) == PK_CORRUPT, "child level 불일치는 CORRUPT");
    free(store);
}

static void test_stamp_follows_root_rewrites(void)
{
    printf("\n[test_stamp_follows_root_rewrites]\n");
    store_t *store = new_store();
    pk_tree_io_t io = make_io(store);
    uint32_t root, stamp = 0;
    assert(pk_tree_create(&io, TABLE_ID, &root) == PK_OK);
    commit(store);
    CHECK(pk_tree_stamp(&io, TABLE_ID, root, &stamp) == PK_OK && stamp == 0, "새 root의 stamp는 0");
    uint32_t before = root;
    CHECK(pk_tree_restamp(&io, TABLE_ID, &root, 77) == PK_OK && root != before, "commit된 root는 새 페이지로 옮겨 씀");
    CHECK(pk_tree_stamp(&io, TABLE_ID, root, &stamp) == PK_OK && stamp == 77, "stamp를 읽음");
    uint32_t again = root;
    CHECK(pk_tree_restamp(&io, TABLE_ID, &root, 78) == PK_OK && root == again, "이 batch가 만든 root는 제자리에 씀");
    free(store);
}

int main(void)
{
    test_sequential_inserts_fill_leaves();
    test_random_batches_keep_copy_on_write();
    test_remove_everything_leaves_an_empty_tree();
    test_duplicate_and_missing_keys();
    test_failed_batch_leaves_committed_tree_intact();
    test_damaged_nodes_are_corrupt();
    test_stamp_follows_root_rewrites();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
