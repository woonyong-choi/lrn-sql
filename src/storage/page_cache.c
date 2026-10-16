#include "page_cache.h"

#include <stdlib.h>
#include <string.h>

#include "storage/page_file.h"

#define EMPTY_SLOT UINT32_MAX

typedef struct {
    uint32_t page_id;
    uint32_t table_id;
    uint16_t kind;
    bool used;
    bool referenced;   /* CLOCK 기준 비트. 적중하면 켜고 바늘이 지나가면 끈다 */
} frame_t;

struct page_cache {
    size_t capacity;
    size_t used;
    size_t hand;
    size_t slot_mask;
    uint32_t *slots;   /* 해시 표. 프레임 번호 또는 EMPTY_SLOT. 선형 탐사, 삭제는 뒤 원소 당기기 */
    frame_t *frames;
    uint8_t *bodies;   /* capacity * PAGE_BODY_BYTES. 처음 put에서 할당한다 */
    bool disabled;     /* 본문 할당에 실패해 캐시를 끈 상태 */
    page_cache_stats_t stats;
};

static size_t slot_count(size_t capacity)
{
    size_t slots = 1;
    while (slots < capacity * 2) slots <<= 1;
    return slots;
}

size_t page_cache_bytes(size_t capacity)
{
    if (capacity == 0) return 0;
    return sizeof(page_cache_t) + slot_count(capacity) * sizeof(uint32_t) + capacity * sizeof(frame_t) +
           capacity * (size_t)PAGE_BODY_BYTES;
}

page_cache_t *page_cache_create(size_t capacity)
{
    if (capacity == 0 || capacity >= EMPTY_SLOT) return NULL;
    page_cache_t *cache = calloc(1, sizeof(*cache));
    if (cache == NULL) return NULL;
    size_t slots = slot_count(capacity);
    cache->capacity = capacity;
    cache->slot_mask = slots - 1;
    cache->slots = malloc(slots * sizeof(*cache->slots));
    cache->frames = calloc(capacity, sizeof(*cache->frames));
    if (cache->slots == NULL || cache->frames == NULL) {
        page_cache_destroy(cache);
        return NULL;
    }
    memset(cache->slots, 0xFF, slots * sizeof(*cache->slots));
    return cache;
}

void page_cache_destroy(page_cache_t *cache)
{
    if (cache == NULL) return;
    free(cache->slots);
    free(cache->frames);
    free(cache->bodies);
    free(cache);
}

static size_t home_slot(const page_cache_t *cache, uint32_t page_id)
{
    return ((size_t)page_id * 2654435761u) & cache->slot_mask;
}

/* page_id가 있는 슬롯 번호. 없으면 SIZE_MAX */
static size_t find_slot(const page_cache_t *cache, uint32_t page_id)
{
    for (size_t at = home_slot(cache, page_id);; at = (at + 1) & cache->slot_mask) {
        uint32_t frame = cache->slots[at];
        if (frame == EMPTY_SLOT) return SIZE_MAX;
        if (cache->frames[frame].page_id == page_id) return at;
    }
}

/* 선형 탐사 표에서 슬롯을 비우고 뒤에 이어진 원소를 제자리로 당겨 탐사 사슬이 끊기지 않게 한다 */
static void remove_slot(page_cache_t *cache, size_t hole)
{
    cache->slots[hole] = EMPTY_SLOT;
    for (size_t at = (hole + 1) & cache->slot_mask; cache->slots[at] != EMPTY_SLOT;
         at = (at + 1) & cache->slot_mask) {
        uint32_t frame = cache->slots[at];
        size_t home = home_slot(cache, cache->frames[frame].page_id);
        /* home이 (hole, at] 구간 밖이면 hole로 옮겨야 다시 찾을 수 있다 */
        bool reachable = hole <= at ? (home > hole && home <= at) : (home > hole || home <= at);
        if (reachable) continue;
        cache->slots[hole] = frame;
        cache->slots[at] = EMPTY_SLOT;
        hole = at;
    }
}

static void insert_slot(page_cache_t *cache, uint32_t frame)
{
    size_t at = home_slot(cache, cache->frames[frame].page_id);
    while (cache->slots[at] != EMPTY_SLOT) at = (at + 1) & cache->slot_mask;
    cache->slots[at] = frame;
}

bool page_cache_get(page_cache_t *cache, uint32_t page_id, uint16_t *kind, uint32_t *table_id, uint8_t *body)
{
    if (cache == NULL || cache->disabled || cache->bodies == NULL) {
        if (cache != NULL) cache->stats.misses++;
        return false;
    }
    size_t slot = find_slot(cache, page_id);
    if (slot == SIZE_MAX) {
        cache->stats.misses++;
        return false;
    }
    frame_t *frame = &cache->frames[cache->slots[slot]];
    frame->referenced = true;
    *kind = frame->kind;
    *table_id = frame->table_id;
    memcpy(body, cache->bodies + (size_t)cache->slots[slot] * PAGE_BODY_BYTES, PAGE_BODY_BYTES);
    cache->stats.hits++;
    return true;
}

/* 빈 프레임을 주거나 CLOCK으로 하나를 비워 준다 */
static uint32_t take_frame(page_cache_t *cache)
{
    if (cache->used < cache->capacity) return (uint32_t)cache->used++;
    for (;;) {
        frame_t *frame = &cache->frames[cache->hand];
        uint32_t index = (uint32_t)cache->hand;
        cache->hand = (cache->hand + 1) % cache->capacity;
        if (!frame->used) return index;  /* drop으로 해시에서 이미 빠진 프레임 */
        if (frame->referenced) {
            frame->referenced = false;
            continue;
        }
        remove_slot(cache, find_slot(cache, frame->page_id));
        frame->used = false;
        cache->stats.evictions++;
        return index;
    }
}

void page_cache_put(page_cache_t *cache, uint32_t page_id, uint16_t kind, uint32_t table_id, const uint8_t *body)
{
    if (cache == NULL || cache->disabled) return;
    if (cache->bodies == NULL) {
        cache->bodies = malloc(cache->capacity * (size_t)PAGE_BODY_BYTES);
        if (cache->bodies == NULL) {
            cache->disabled = true;
            return;
        }
    }
    size_t slot = find_slot(cache, page_id);
    uint32_t index;
    if (slot != SIZE_MAX) {
        index = cache->slots[slot];
    } else {
        index = take_frame(cache);
        cache->frames[index] = (frame_t){page_id, table_id, kind, true, false};
        insert_slot(cache, index);
    }
    frame_t *frame = &cache->frames[index];
    frame->kind = kind;
    frame->table_id = table_id;
    frame->referenced = true;
    memcpy(cache->bodies + (size_t)index * PAGE_BODY_BYTES, body, PAGE_BODY_BYTES);
}

void page_cache_drop(page_cache_t *cache, uint32_t page_id)
{
    if (cache == NULL || cache->disabled || cache->bodies == NULL) return;
    size_t slot = find_slot(cache, page_id);
    if (slot == SIZE_MAX) return;
    /* 프레임은 비운 채 두고 CLOCK이 다시 쓸 때까지 해시에서만 뺀다. 참조 비트를 꺼 먼저 재사용되게 한다 */
    frame_t *frame = &cache->frames[cache->slots[slot]];
    remove_slot(cache, slot);
    frame->used = false;
    frame->referenced = false;
}

page_cache_stats_t page_cache_stats(const page_cache_t *cache)
{
    page_cache_stats_t none = {0, 0, 0};
    return cache != NULL ? cache->stats : none;
}
