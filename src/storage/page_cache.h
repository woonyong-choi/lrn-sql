#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * page_file 내부의 읽기 캐시. 체크섬을 검증한 페이지 본문만 담고, 디스크에 성공적으로 쓴 내용으로만 갱신한다.
 * 그래서 캐시는 언제나 그 핸들이 디스크에 쓴(또는 읽어 검증한) 바이트와 같다. 손상 페이지는 담지 않는다.
 *
 * 용량은 프레임 수로 고정한다. 프레임 메모리는 처음 넣을 때 한 번에 할당하며, 할당에 실패하면 캐시를 끄고
 * 이후 호출은 모두 miss·무동작이 된다(동작은 캐시 없는 읽기와 같다). 계상은 page_cache_bytes의 상한으로 한다.
 * 교체는 CLOCK이다. 단일 스레드 계약이다(page_file과 같다).
 */

typedef struct page_cache page_cache_t;

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} page_cache_stats_t;

/* capacity 프레임 캐시의 요청 바이트 상한(구조체·해시 표·프레임 본문). capacity 0이면 0이다 */
size_t page_cache_bytes(size_t capacity);

/* capacity가 0이면 NULL(캐시 없음)이다. 할당 실패도 NULL이며 호출자는 캐시 없이 동작한다 */
page_cache_t *page_cache_create(size_t capacity);
void page_cache_destroy(page_cache_t *cache);

/* 적중하면 kind·table_id와 본문(PAGE_BODY_BYTES)을 복사하고 true. cache가 NULL이면 false */
bool page_cache_get(page_cache_t *cache, uint32_t page_id, uint16_t *kind, uint32_t *table_id, uint8_t *body);

/* 검증했거나 디스크에 쓴 본문을 넣는다. 같은 page_id가 있으면 바꾼다 */
void page_cache_put(page_cache_t *cache, uint32_t page_id, uint16_t kind, uint32_t table_id, const uint8_t *body);

/* 쓰기가 실패해 디스크 내용을 알 수 없게 된 페이지를 뺀다 */
void page_cache_drop(page_cache_t *cache, uint32_t page_id);

page_cache_stats_t page_cache_stats(const page_cache_t *cache);

#endif
