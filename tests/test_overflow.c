#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/checksum.h"
#include "storage/overflow.h"
#include "storage/record_page.h"

#define PAGE_BYTES 128u
#define PAYLOAD_BYTES (PAGE_BYTES - 28u)
#define MAX_PAGES 512u

typedef struct {
    uint8_t data[MAX_PAGES][PAGE_BYTES];
    bool live[MAX_PAGES];
    size_t live_count;
    /* 0이면 꺼짐, n이면 n번째 호출이 실패한다. */
    int fail_allocate;
    int fail_read;
    int fail_write;
    int fail_release;
    size_t releases;
} store_t;

static bool hit(int *countdown)
{
    return *countdown != 0 && --*countdown == 0;
}

static bool store_allocate(void *context, uint32_t *id)
{
    store_t *store = context;
    if (hit(&store->fail_allocate)) return false;
    for (uint32_t i = 1; i < MAX_PAGES; i++) {
        if (store->live[i]) continue;
        store->live[i] = true;
        store->live_count++;
        *id = i;
        return true;
    }
    return false;
}

static bool store_release(void *context, uint32_t id)
{
    store_t *store = context;
    store->releases++;
    if (hit(&store->fail_release)) return false;
    if (id == 0 || id >= MAX_PAGES || !store->live[id]) return false;
    store->live[id] = false;
    store->live_count--;
    memset(store->data[id], 0, PAGE_BYTES);
    return true;
}

static bool store_read(void *context, uint32_t id, uint8_t *page)
{
    store_t *store = context;
    if (hit(&store->fail_read)) return false;
    if (id == 0 || id >= MAX_PAGES) return false;
    memcpy(page, store->data[id], PAGE_BYTES);
    return true;
}

static bool store_write(void *context, uint32_t id, const uint8_t *page)
{
    store_t *store = context;
    if (hit(&store->fail_write)) return false;
    if (id == 0 || id >= MAX_PAGES || !store->live[id]) return false;
    memcpy(store->data[id], page, PAGE_BYTES);
    return true;
}

static overflow_io_t io_for(store_t *store)
{
    return (overflow_io_t){store, PAGE_BYTES, store_allocate, store_release,
                           store_read, store_write};
}

static void fill(uint8_t *bytes, size_t size, unsigned seed)
{
    for (size_t i = 0; i < size; i++) bytes[i] = (uint8_t)(i * 31 + seed);
}

static size_t pages_for(size_t size)
{
    return (size + PAYLOAD_BYTES - 1) / PAYLOAD_BYTES;
}

static uint32_t load_u32(const uint8_t *bytes)
{
    return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void store_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (i * 8));
}

/* 테스트가 일부러 만든 손상 페이지가 checksum만 맞도록 다시 봉인한다. */
static void reseal(uint8_t *page)
{
    store_u32(page + 24, crc32c(crc32c(0, page, 24), page + 28, PAGE_BYTES - 28));
}

static overflow_ref_t write_value(store_t *store, size_t size, unsigned seed)
{
    uint8_t *bytes = malloc(size ? size : 1);
    overflow_io_t io = io_for(store);
    overflow_ref_t ref = {0};
    fill(bytes, size, seed);
    assert(overflow_write(&io, bytes, size, &ref) == OVERFLOW_OK);
    free(bytes);
    return ref;
}

static void assert_value(store_t *store, overflow_ref_t ref, unsigned seed)
{
    overflow_io_t io = io_for(store);
    uint8_t *actual = NULL, *expected = malloc(ref.size_bytes ? ref.size_bytes : 1);
    fill(expected, ref.size_bytes, seed);
    assert(overflow_read(&io, ref, &actual) == OVERFLOW_OK);
    assert(ref.size_bytes == 0 ? actual == NULL
                               : memcmp(actual, expected, ref.size_bytes) == 0);
    free(actual);
    free(expected);
}

/* #81: 빈 값·한 페이지 경계·여러 페이지 값이 크기 그대로 왕복하고 회수된다. */
static void boundaries_roundtrip_and_free(void)
{
    size_t sizes[] = {0, 1, PAYLOAD_BYTES - 1, PAYLOAD_BYTES, PAYLOAD_BYTES + 1,
                      2 * PAYLOAD_BYTES, 3 * PAYLOAD_BYTES + 5, 40000};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        store_t *store = calloc(1, sizeof(*store));
        overflow_io_t io = io_for(store);
        overflow_ref_t ref = write_value(store, sizes[i], 5);
        assert(ref.size_bytes == sizes[i]);
        assert((ref.head == OVERFLOW_NO_PAGE) == (sizes[i] == 0));
        assert(store->live_count == pages_for(sizes[i]));
        assert_value(store, ref, 5);
        assert(overflow_free(&io, ref) == OVERFLOW_OK);
        assert(store->live_count == 0);
        free(store);
    }
}

static void invalid_arguments_are_rejected(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    overflow_io_t small = io, huge = io, missing = io;
    small.page_bytes = 24;
    huge.page_bytes = OVERFLOW_MAX_PAGE_BYTES + 1;
    missing.read = NULL;
    uint8_t byte = 1, *out = (uint8_t *)&byte;
    overflow_ref_t ref = {7, 7};
    assert(overflow_write(&small, &byte, 1, &ref) == OVERFLOW_INVALID);
    assert(overflow_write(&huge, &byte, 1, &ref) == OVERFLOW_INVALID);
    assert(overflow_write(&missing, &byte, 1, &ref) == OVERFLOW_INVALID);
    assert(overflow_write(NULL, &byte, 1, &ref) == OVERFLOW_INVALID);
    assert(overflow_write(&io, NULL, 1, &ref) == OVERFLOW_INVALID);
    assert(overflow_write(&io, &byte, 1, NULL) == OVERFLOW_INVALID);
    assert(ref.head == 7 && ref.size_bytes == 7 && store->live_count == 0);
    assert(overflow_read(&io, (overflow_ref_t){0, 5}, &out) == OVERFLOW_INVALID);
    assert(overflow_read(&io, (overflow_ref_t){3, 0}, &out) == OVERFLOW_INVALID);
    assert(overflow_free(&io, (overflow_ref_t){0, 5}) == OVERFLOW_INVALID);
    assert(out == &byte);
    free(store);
}

/* 손상 체인은 CORRUPT로 끝나고 출력은 그대로이며 회수도 하지 않는다. */
static void assert_chain_rejected(store_t *store, overflow_ref_t ref)
{
    overflow_io_t io = io_for(store);
    uint8_t marker, *out = &marker;
    size_t live = store->live_count;
    assert(overflow_read(&io, ref, &out) == OVERFLOW_CORRUPT);
    assert(out == &marker);
    assert(overflow_free(&io, ref) == OVERFLOW_CORRUPT);
    assert(store->live_count == live);
}

static void damaged_chains_are_detected(void)
{
    store_t *store = calloc(1, sizeof(*store));
    size_t size = 2 * PAYLOAD_BYTES + 40;
    overflow_ref_t ref = write_value(store, size, 9);
    uint32_t second = load_u32(store->data[ref.head] + 8);
    uint32_t third = load_u32(store->data[second] + 8);
    uint8_t original[3][PAGE_BYTES];
    memcpy(original[0], store->data[ref.head], PAGE_BYTES);
    memcpy(original[1], store->data[second], PAGE_BYTES);
    memcpy(original[2], store->data[third], PAGE_BYTES);
    uint32_t ids[3] = {ref.head, second, third};

    for (size_t p = 0; p < 3; p++) {
        for (size_t offset = 0; offset < PAGE_BYTES; offset += 13) {
            store->data[ids[p]][offset] ^= 0x40;
            assert_chain_rejected(store, ref);
            memcpy(store->data[ids[p]], original[p], PAGE_BYTES);
        }
    }

    /* 순환: 둘째 페이지가 첫 페이지를 다시 가리킨다. */
    store_u32(store->data[second] + 8, ref.head);
    reseal(store->data[second]);
    assert_chain_rejected(store, ref);
    memcpy(store->data[second], original[1], PAGE_BYTES);

    /* 자기 순환과 끝 페이지의 순환 */
    store_u32(store->data[third] + 8, third);
    reseal(store->data[third]);
    assert_chain_rejected(store, ref);
    memcpy(store->data[third], original[2], PAGE_BYTES);

    /* 체인이 일찍 끝남: 첫 페이지 next를 0으로 */
    store_u32(store->data[ref.head] + 8, OVERFLOW_NO_PAGE);
    reseal(store->data[ref.head]);
    assert_chain_rejected(store, ref);
    memcpy(store->data[ref.head], original[0], PAGE_BYTES);

    /* 페이지 순서 뒤바꿈 */
    store_u32(store->data[ref.head] + 8, third);
    reseal(store->data[ref.head]);
    assert_chain_rejected(store, ref);
    memcpy(store->data[ref.head], original[0], PAGE_BYTES);

    /* 참조 크기가 체인보다 길거나 짧음 */
    for (uint32_t wrong = 1; wrong < 4 * PAYLOAD_BYTES; wrong += 17) {
        if (wrong == size) continue;
        assert_chain_rejected(store, (overflow_ref_t){ref.head, wrong});
    }
    /* 이미 회수돼 0으로 지워진 페이지 번호 */
    assert_chain_rejected(store, (overflow_ref_t){MAX_PAGES - 1, (uint32_t)size});
    assert_value(store, ref, 9);
    free(store);
}

/* 같은 길이의 다른 체인으로 next를 이으면 head 식별자가 달라 거절한다. */
static void cross_chain_splice_is_rejected(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    size_t size = 3 * PAYLOAD_BYTES;
    overflow_ref_t first = write_value(store, size, 1);
    overflow_ref_t second = write_value(store, size, 2);
    uint32_t first_ids[3], second_ids[3];
    first_ids[0] = first.head;
    second_ids[0] = second.head;
    for (size_t i = 1; i < 3; i++) {
        first_ids[i] = load_u32(store->data[first_ids[i - 1]] + 8);
        second_ids[i] = load_u32(store->data[second_ids[i - 1]] + 8);
    }
    uint8_t saved[PAGE_BYTES];
    for (size_t at = 0; at < 2; at++) {
        for (size_t into = at + 1; into < 3; into++) {
            memcpy(saved, store->data[first_ids[at]], PAGE_BYTES);
            store_u32(store->data[first_ids[at]] + 8, second_ids[into]);
            reseal(store->data[first_ids[at]]);
            size_t live = store->live_count;
            uint8_t marker, *out = &marker;
            assert(overflow_read(&io, first, &out) == OVERFLOW_CORRUPT && out == &marker);
            assert(overflow_free(&io, first) == OVERFLOW_CORRUPT);
            assert(store->live_count == live);
            memcpy(store->data[first_ids[at]], saved, PAGE_BYTES);
        }
    }
    assert_value(store, first, 1);
    assert_value(store, second, 2);
    assert(overflow_free(&io, first) == OVERFLOW_OK);
    assert_value(store, second, 2);
    assert(overflow_free(&io, second) == OVERFLOW_OK && store->live_count == 0);
    free(store);
}

static bool allocate_duplicate(void *context, uint32_t *id)
{
    static uint32_t first_id;
    if (first_id != OVERFLOW_NO_PAGE) {
        *id = first_id;
        return true;
    }
    bool ok = store_allocate(context, id);
    first_id = *id;
    return ok;
}

/* 이미 받은 번호를 다시 주는 어댑터는 INVALID로 막고 번호마다 한 번만 반납한다. */
static void duplicate_allocation_is_rejected(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    uint8_t bytes[3 * PAYLOAD_BYTES] = {0};
    overflow_ref_t ref = {5, 6};
    io.allocate = allocate_duplicate;
    assert(overflow_write(&io, bytes, sizeof(bytes), &ref) == OVERFLOW_INVALID);
    assert(ref.head == 5 && ref.size_bytes == 6 && store->live_count == 0);
    assert(store->releases == 1);
    free(store);
}

/* 할당·쓰기 중간 실패는 이번 호출이 잡은 페이지를 모두 돌려놓는다. */
static void write_failure_reclaims_allocations(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    size_t size = 4 * PAYLOAD_BYTES - 3, pages = pages_for(size);
    uint8_t *bytes = malloc(size);
    fill(bytes, size, 1);
    for (int step = 1; step <= (int)pages; step++) {
        overflow_ref_t ref = {11, 22};
        store->fail_allocate = step;
        assert(overflow_write(&io, bytes, size, &ref) == OVERFLOW_IO);
        assert(ref.head == 11 && ref.size_bytes == 22 && store->live_count == 0);
        store->fail_write = step;
        assert(overflow_write(&io, bytes, size, &ref) == OVERFLOW_IO);
        assert(ref.head == 11 && ref.size_bytes == 22 && store->live_count == 0);
    }
    /* 되돌리기 자체가 실패하면 별도 상태로 알린다. */
    overflow_ref_t ref = {11, 22};
    store->fail_write = 2;
    store->fail_release = 1;
    assert(overflow_write(&io, bytes, size, &ref) == OVERFLOW_ROLLBACK_FAILED);
    assert(ref.head == 11 && ref.size_bytes == 22);
    assert(store->live_count == 1 && store->releases >= pages);
    free(bytes);
    free(store);
}

static bool allocate_null_on_second(void *context, uint32_t *id)
{
    static int calls;
    if (calls++ == 1) {
        *id = OVERFLOW_NO_PAGE;
        return true;
    }
    return store_allocate(context, id);
}

/* id 0을 돌려주는 어댑터는 INVALID로 막고 이미 받은 페이지를 돌려준다. */
static void allocator_returning_null_page_is_rejected(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    uint8_t bytes[3 * PAYLOAD_BYTES] = {0};
    overflow_ref_t ref = {5, 6};
    io.allocate = allocate_null_on_second;
    assert(overflow_write(&io, bytes, sizeof(bytes), &ref) == OVERFLOW_INVALID);
    assert(ref.head == 5 && ref.size_bytes == 6 && store->live_count == 0);
    free(store);
}

static void read_and_release_failures_are_reported(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    size_t size = 3 * PAYLOAD_BYTES;
    overflow_ref_t ref = write_value(store, size, 4);
    uint8_t marker, *out = &marker;
    for (int step = 1; step <= 3; step++) {
        store->fail_read = step;
        assert(overflow_read(&io, ref, &out) == OVERFLOW_IO && out == &marker);
        store->fail_read = step;
        assert(overflow_free(&io, ref) == OVERFLOW_IO && store->live_count == 3);
    }
    /* 회수 중간 실패여도 나머지는 모두 시도하고 고아 수를 남긴다. */
    store->fail_release = 2;
    assert(overflow_free(&io, ref) == OVERFLOW_UNRECLAIMED);
    assert(store->live_count == 1);
    free(store);
}

/* update는 새 체인을 쓰고 slot 참조를 바꾼 뒤에만 옛 체인을 회수한다. */
static void load_ref(record_slice_t slice, overflow_ref_t *ref)
{
    assert(slice.size_bytes == 8);
    ref->head = load_u32(slice.bytes);
    ref->size_bytes = load_u32(slice.bytes + 4);
}

static record_slice_t pack_ref(overflow_ref_t ref, uint8_t bytes[8])
{
    store_u32(bytes, ref.head);
    store_u32(bytes + 4, ref.size_bytes);
    return (record_slice_t){bytes, 8};
}

static void update_and_delete_keep_lifetime(void)
{
    store_t *store = calloc(1, sizeof(*store));
    overflow_io_t io = io_for(store);
    uint8_t page[RECORD_PAGE_BYTES], packed[8];
    uint16_t slot;
    record_page_init(page);
    overflow_ref_t old_ref = write_value(store, 300, 1);
    assert(record_page_insert(page, pack_ref(old_ref, packed), &slot) == RECORD_PAGE_OK);
    size_t old_pages = pages_for(300), new_size = 700, new_pages = pages_for(new_size);
    uint8_t *bytes = malloc(new_size);
    fill(bytes, new_size, 2);

    /* 새 체인 쓰기가 어디서 실패해도 옛 값은 그대로 읽힌다. */
    for (int step = 1; step <= (int)new_pages; step++) {
        overflow_ref_t next = {0};
        store->fail_write = step;
        assert(overflow_write(&io, bytes, new_size, &next) == OVERFLOW_IO);
        overflow_ref_t current;
        record_slice_t got;
        assert(record_page_get(page, slot, &got) == RECORD_PAGE_OK);
        load_ref(got, &current);
        assert(store->live_count == old_pages);
        assert_value(store, current, 1);
    }

    /* 새 체인이 기록된 직후 slot 갱신 전에는 옛 값과 새 값이 모두 살아 있다. */
    overflow_ref_t new_ref;
    assert(overflow_write(&io, bytes, new_size, &new_ref) == OVERFLOW_OK);
    assert(store->live_count == old_pages + new_pages);
    assert_value(store, old_ref, 1);
    assert(record_page_update(page, slot, pack_ref(new_ref, packed)) == RECORD_PAGE_OK);
    assert_value(store, new_ref, 2);

    /* 옛 체인 회수 중 실패하면 새 값은 유효하고 고아 한 페이지가 보고된다. */
    store->fail_release = 1;
    assert(overflow_free(&io, old_ref) == OVERFLOW_UNRECLAIMED);
    assert(store->live_count == new_pages + 1);
    assert_value(store, new_ref, 2);

    /* 재사용되지 않은 회수 페이지는 거절한다. 재사용 뒤의 오래된 참조는 보장하지 않는다. */
    uint8_t marker, *out = &marker;
    assert(overflow_read(&io, old_ref, &out) != OVERFLOW_OK && out == &marker);

    /* delete: slot을 먼저 지우고 체인을 회수하면 남는 페이지가 없다. */
    record_slice_t got;
    assert(record_page_get(page, slot, &got) == RECORD_PAGE_OK);
    overflow_ref_t current;
    load_ref(got, &current);
    assert(record_page_delete(page, slot) == RECORD_PAGE_OK);
    assert(record_page_get(page, slot, &got) == RECORD_PAGE_NOT_FOUND);
    assert(overflow_free(&io, current) == OVERFLOW_OK);
    assert(store->live_count == 1);
    free(bytes);
    free(store);
}

int main(void)
{
    boundaries_roundtrip_and_free();
    invalid_arguments_are_rejected();
    damaged_chains_are_detected();
    cross_chain_splice_is_rejected();
    duplicate_allocation_is_rejected();
    write_failure_reclaims_allocations();
    allocator_returning_null_page_is_rejected();
    read_and_release_failures_are_reported();
    update_and_delete_keep_lifetime();
    puts("overflow: boundaries, corruption, cross-chain splice, partial failures, reclaim and update/delete lifetime");
    return 0;
}
