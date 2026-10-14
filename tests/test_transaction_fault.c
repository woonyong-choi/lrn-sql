/*
 * 할당 실패 주입. transaction.c를 직접 포함해 malloc·calloc을 세고 N번째를 실패시킨 뒤, 실패한 호출이
 * NOMEM으로 끝나고 상태가 보존되며 누수가 없는지 검사한다.
 * 빌드: gcc -Wall -Wextra -Werror -g -std=c11 -Iinclude -pthread tests/test_transaction_fault.c -o build/test_transaction_fault
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

static long alloc_count;
static long fail_at;   /* 1부터. 0이면 실패시키지 않는다 */
static long live_blocks;

static void *fault_calloc(size_t n, size_t size)
{
    alloc_count++;
    if (fail_at != 0 && alloc_count == fail_at) {
        return NULL;
    }
    void *block = calloc(n, size);
    if (block != NULL) {
        live_blocks++;
    }
    return block;
}

static void fault_free(void *block)
{
    if (block != NULL) {
        live_blocks--;
    }
    free(block);
}

#define calloc fault_calloc
#define malloc fault_malloc_unused
#define free fault_free
#include "../src/storage/transaction.c"
#undef calloc
#undef malloc
#undef free

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static int finishes;
static int begins;

static txn_status_t be_begin(void *ctx, uint64_t base, void **write)
{
    (void)ctx;
    (void)base;
    (void)write;
    begins++;
    return TXN_OK;
}

static txn_publish_result_t be_publish(void *ctx, void *write, uint64_t seq, txn_guarantee_t requested)
{
    (void)ctx;
    (void)write;
    (void)seq;
    (void)requested;
    return (txn_publish_result_t){TXN_OUTCOME_APPLIED, TXN_GUARANTEE_SYNCED};
}

static void be_finish(void *ctx, void *write, txn_outcome_t outcome, uint64_t seq)
{
    (void)ctx;
    (void)write;
    (void)outcome;
    (void)seq;
    finishes++;
}

static const txn_backend_ops_t OPS = {be_begin, be_publish, be_finish, NULL, NULL, NULL};

/* 한 번의 시나리오. 실패하는 할당 위치마다 같은 시나리오를 다시 돌린다. */
static void scenario(void)
{
    txn_manager_spec_t spec = {.ops = &OPS, .max_guarantee = TXN_GUARANTEE_SYNCED};
    txn_manager_t *manager = NULL;
    txn_t *writer = NULL;
    txn_t *reader = NULL;
    txn_status_t status = txn_manager_open(&spec, &manager);

    if (status != TXN_OK) {
        CHECK(status == TXN_NOMEM && manager == NULL);
        return;
    }
    status = txn_begin(manager, NULL, &writer);
    if (status != TXN_OK) {
        CHECK(status == TXN_NOMEM && writer == NULL);
        /* 실패한 begin은 슬롯도 순번도 남기지 않는다 */
        CHECK(txn_manager_oldest_snapshot(manager) == 0);
        CHECK(begins == 0 && finishes == 0);
    } else {
        CHECK(txn_commit(writer, NULL) == TXN_OK);
        CHECK(txn_release(&writer) == TXN_OK);
    }
    status = txn_begin(manager, &(txn_begin_options_t){.access = TXN_ACCESS_READ_ONLY}, &reader);
    if (status != TXN_OK) {
        CHECK(status == TXN_NOMEM && reader == NULL);
    } else {
        CHECK(txn_release(&reader) == TXN_OK);
    }
    /* 실패한 호출 뒤에도 새 트랜잭션을 정상으로 받을 수 있다 */
    status = txn_begin(manager, NULL, &writer);
    CHECK(status == TXN_OK || (status == TXN_NOMEM && writer == NULL));
    CHECK(txn_release(&writer) == TXN_OK);
    CHECK(txn_manager_close(&manager) == TXN_OK);
    CHECK(begins == finishes);
}

/* 자동 발급 세대가 UINT32_MAX에서 소진되면 거절하고, 상한 이하의 지정도 거절한다. */
static void epoch_exhaustion(void)
{
    txn_manager_spec_t spec = {.ops = &OPS, .max_guarantee = TXN_GUARANTEE_SYNCED};
    txn_manager_t *manager = (txn_manager_t *)1;
    txn_t *writer = NULL;

    epoch_high_water = UINT32_MAX - 2;
    spec.epoch = UINT32_MAX - 1;
    CHECK(txn_manager_open(&spec, &manager) == TXN_OK);   /* 상한보다 큰 지정은 채택하고 상한이 오른다 */
    CHECK(txn_manager_close(&manager) == TXN_OK);
    spec.epoch = 0;
    CHECK(txn_manager_open(&spec, &manager) == TXN_OK);   /* 자동 발급은 상한 + 1 = UINT32_MAX */
    CHECK(txn_begin(manager, NULL, &writer) == TXN_OK && (uint32_t)(txn_id(writer) >> 32) == UINT32_MAX);
    CHECK(txn_release(&writer) == TXN_OK);
    CHECK(txn_manager_close(&manager) == TXN_OK);
    manager = (txn_manager_t *)1;
    CHECK(txn_manager_open(&spec, &manager) == TXN_LIMIT && manager == NULL);
    spec.epoch = UINT32_MAX;
    CHECK(txn_manager_open(&spec, &manager) == TXN_INVALID && manager == NULL);
    spec.epoch = 1;
    CHECK(txn_manager_open(&spec, &manager) == TXN_INVALID && manager == NULL);   /* 상한보다 작은 지정 */
    CHECK(epoch_high_water == UINT32_MAX);   /* 거절된 시도는 상한을 바꾸지 않는다 */
}

/* 할당 실패한 open은 세대 번호를 소모하지 않고, 사용한 번호의 거절은 할당을 남기지 않는다. */
static void epoch_open_failures(void)
{
    txn_manager_spec_t spec = {.ops = &OPS, .max_guarantee = TXN_GUARANTEE_SYNCED, .epoch = 0x60000000u};
    txn_manager_t *manager = (txn_manager_t *)1;
    txn_manager_t *dup = (txn_manager_t *)1;
    uint64_t high_before = epoch_high_water;
    long blocks;

    for (long point = 1; point <= 2; point++) {   /* manager, ring 할당 */
        alloc_count = 0;
        fail_at = point;
        live_blocks = 0;
        CHECK(txn_manager_open(&spec, &manager) == TXN_NOMEM && manager == NULL);
        CHECK(live_blocks == 0);
        CHECK(epoch_high_water == high_before);   /* 실패한 open은 상한을 올리지 않는다 */
        manager = (txn_manager_t *)1;
    }
    fail_at = 0;
    live_blocks = 0;
    CHECK(txn_manager_open(&spec, &manager) == TXN_OK);   /* 실패한 시도가 번호를 쓰지 않았다 */
    blocks = live_blocks;
    CHECK(blocks == 2);
    CHECK(txn_manager_open(&spec, &dup) == TXN_INVALID && dup == NULL);   /* 사용한 번호는 거절 */
    CHECK(live_blocks == blocks);   /* 거절된 open이 할당을 남기지 않는다 */
    CHECK(txn_manager_close(&manager) == TXN_OK);
    CHECK(live_blocks == 0);
    dup = (txn_manager_t *)1;
    CHECK(txn_manager_open(&spec, &dup) == TXN_INVALID && dup == NULL);   /* close 뒤에도 거절 */
    CHECK(live_blocks == 0);
}

static txn_status_t open_floor(uint32_t floor, txn_manager_t **manager)
{
    txn_manager_spec_t spec = {.ops = &OPS,
                               .max_guarantee = TXN_GUARANTEE_SYNCED,
                               .epoch_mode = TXN_EPOCH_ABOVE_FLOOR,
                               .epoch_floor = floor};

    return txn_manager_open(&spec, manager);
}

/*
 * 재시작 모형: 프로세스 상한을 0으로 되돌리고, 저장소가 가진 floor만으로 이전 세대와 겹치지 않는 번호를 받는다.
 * 저장 실패를 모형화해 floor를 올리지 않은 채 다시 열어도 이미 발급한 번호가 다시 나오지 않는지 본다.
 */
static void epoch_restart_model(void)
{
    txn_manager_t *manager = (txn_manager_t *)1;
    uint32_t persisted_floor = 0;   /* 저장소에 영속화된 가장 큰 세대 */
    uint32_t first;
    uint32_t unsaved;
    uint32_t second;

    epoch_high_water = 0;
    CHECK(open_floor(persisted_floor, &manager) == TXN_OK);
    first = txn_manager_epoch(manager);
    CHECK(first == 1);
    persisted_floor = first;   /* 노출 전에 영속화 */
    CHECK(txn_manager_close(&manager) == TXN_OK);

    /* 같은 프로세스에서 영속화에 실패한 open: floor는 그대로이고 번호는 소모된다 */
    CHECK(open_floor(persisted_floor, &manager) == TXN_OK);
    unsaved = txn_manager_epoch(manager);
    CHECK(unsaved == first + 1);
    CHECK(txn_manager_close(&manager) == TXN_OK);
    CHECK(open_floor(persisted_floor, &manager) == TXN_OK);
    CHECK(txn_manager_epoch(manager) == unsaved + 1);   /* 실패한 번호를 다시 쓰지 않는다 */
    CHECK(txn_manager_close(&manager) == TXN_OK);

    /* 재시작: 상한이 사라져도 저장된 floor 뒤의 번호를 받는다 */
    epoch_high_water = 0;
    CHECK(open_floor(persisted_floor, &manager) == TXN_OK);
    second = txn_manager_epoch(manager);
    CHECK(second == persisted_floor + 1 && second > first);
    CHECK(txn_manager_close(&manager) == TXN_OK);

    /* 재시작 직후 서로 다른 floor의 저장소 둘이 겹치지 않는다 */
    epoch_high_water = 0;
    CHECK(open_floor(50, &manager) == TXN_OK && txn_manager_epoch(manager) == 51);
    CHECK(txn_manager_close(&manager) == TXN_OK);
    CHECK(open_floor(10, &manager) == TXN_OK && txn_manager_epoch(manager) == 52);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

/* floor가 최댓값이면 LIMIT이고 상한을 바꾸지 않는다. 최댓값 직전까지는 마지막 번호를 받는다. */
static void epoch_floor_overflow(void)
{
    txn_manager_t *manager = (txn_manager_t *)1;
    uint64_t high_before;

    epoch_high_water = 0;
    manager = (txn_manager_t *)1;
    CHECK(open_floor(UINT32_MAX, &manager) == TXN_LIMIT && manager == NULL);
    CHECK(epoch_high_water == 0);
    CHECK(open_floor(UINT32_MAX - 1, &manager) == TXN_OK && txn_manager_epoch(manager) == UINT32_MAX);
    CHECK(txn_manager_close(&manager) == TXN_OK);
    high_before = epoch_high_water;
    CHECK(high_before == UINT32_MAX);
    manager = (txn_manager_t *)1;
    CHECK(open_floor(0, &manager) == TXN_LIMIT && manager == NULL);   /* 상한이 소진됐다 */
    CHECK(open_floor(UINT32_MAX - 1, &manager) == TXN_LIMIT && manager == NULL);
    CHECK(epoch_high_water == high_before);

    /* 상한이 최댓값 직전이고 floor가 작아도 마지막 번호 하나만 남는다 */
    epoch_high_water = UINT32_MAX - 1;
    CHECK(open_floor(3, &manager) == TXN_OK && txn_manager_epoch(manager) == UINT32_MAX);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

/* 할당 실패한 floor open은 번호를 소모하지 않는다. */
static void epoch_floor_alloc_failure(void)
{
    txn_manager_t *manager = (txn_manager_t *)1;
    uint64_t high_before;

    epoch_high_water = 1000;
    high_before = epoch_high_water;
    for (long point = 1; point <= 2; point++) {
        alloc_count = 0;
        fail_at = point;
        live_blocks = 0;
        CHECK(open_floor(5000, &manager) == TXN_NOMEM && manager == NULL);
        CHECK(live_blocks == 0 && epoch_high_water == high_before);
        manager = (txn_manager_t *)1;
    }
    fail_at = 0;
    CHECK(open_floor(5000, &manager) == TXN_OK && txn_manager_epoch(manager) == 5001);
    CHECK(txn_manager_close(&manager) == TXN_OK);
}

int main(void)
{
    long total;

    fail_at = 0;
    scenario();
    total = alloc_count;
    CHECK(total == 5);
    CHECK(live_blocks == 0);
    for (long i = 1; i <= total; i++) {
        alloc_count = 0;
        fail_at = i;
        live_blocks = 0;
        begins = 0;
        finishes = 0;
        scenario();
        CHECK(live_blocks == 0);
    }
    epoch_open_failures();
    epoch_exhaustion();
    epoch_restart_model();
    epoch_floor_overflow();
    epoch_floor_alloc_failure();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_transaction_fault: all passed (%ld allocation points)\n", total);
    return 0;
}
