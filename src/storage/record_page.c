#include <stdlib.h>
#include <string.h>

#include "storage/record_page.h"

#define PAGE_HEADER_BYTES 8u
#define SLOT_BYTES 4u

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static const uint8_t *get_slot(const uint8_t *page, uint16_t slot)
{
    return page + PAGE_HEADER_BYTES + slot * SLOT_BYTES;
}

void record_page_init(uint8_t page[RECORD_PAGE_BYTES])
{
    memset(page, 0, RECORD_PAGE_BYTES);
    memcpy(page, RECORD_PAGE_MAGIC, 4);
}

record_page_status_t record_page_validate(const uint8_t page[RECORD_PAGE_BYTES])
{
    if (page == NULL) return RECORD_PAGE_INVALID;
    if (memcmp(page, RECORD_PAGE_MAGIC, 4) != 0) return RECORD_PAGE_INVALID;
    if (read_u16(page + 6) != 0) return RECORD_PAGE_INVALID;
    uint16_t count = read_u16(page + 4);
    if (count > (RECORD_PAGE_BYTES - PAGE_HEADER_BYTES) / SLOT_BYTES) {
        return RECORD_PAGE_INVALID;
    }
    size_t lower = PAGE_HEADER_BYTES + count * SLOT_BYTES;
    size_t upper = RECORD_PAGE_BYTES;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *slot = get_slot(page, i);
        size_t offset = read_u16(slot);
        size_t size = read_u16(slot + 2);
        if (offset == 0) {
            if (size != 0) return RECORD_PAGE_INVALID;
            continue;
        }
        if (size > upper - lower) return RECORD_PAGE_INVALID;
        upper -= size;
        if (offset != upper) return RECORD_PAGE_INVALID;
    }
    return RECORD_PAGE_OK;
}

record_page_status_t record_page_get(const uint8_t *page, uint16_t slot,
                                     record_slice_t *output)
{
    if (output == NULL) return RECORD_PAGE_INVALID;
    record_page_status_t status = record_page_validate(page);
    if (status != RECORD_PAGE_OK) return status;
    if (slot >= read_u16(page + 4)) return RECORD_PAGE_NOT_FOUND;
    const uint8_t *entry = get_slot(page, slot);
    uint16_t offset = read_u16(entry);
    if (offset == 0) return RECORD_PAGE_NOT_FOUND;
    *output = (record_slice_t){page + offset, read_u16(entry + 2)};
    return RECORD_PAGE_OK;
}

typedef struct {
    uint16_t slot;
    uint16_t count;
    record_slice_t value;
    int is_delete;
} page_change_t;

static record_page_status_t measure_change(const uint8_t *page,
                                            const page_change_t *change)
{
    size_t total = PAGE_HEADER_BYTES + change->count * SLOT_BYTES;
    if (total > RECORD_PAGE_BYTES) return RECORD_PAGE_FULL;
    if (change->value.size_bytes > RECORD_PAGE_BYTES - total) return RECORD_PAGE_FULL;
    total += change->value.size_bytes;
    for (uint16_t i = 0; i < read_u16(page + 4); i++) {
        if (i == change->slot) continue;
        total += read_u16(get_slot(page, i) + 2);
        if (total > RECORD_PAGE_BYTES) return RECORD_PAGE_FULL;
    }
    return RECORD_PAGE_OK;
}

/* cost: time O(p), heap O(p), stack O(1), alloc 1 */
/* vars: p = RECORD_PAGE_BYTES; 전체 페이지를 준비한 뒤 게시한다. */
/* basis: estimate */
static record_page_status_t apply_change(uint8_t *page, const page_change_t *change)
{
    record_page_status_t status = measure_change(page, change);
    if (status != RECORD_PAGE_OK) return status;
    uint8_t *next = calloc(1, RECORD_PAGE_BYTES);
    if (next == NULL) return RECORD_PAGE_NOMEM;
    record_page_init(next);
    write_u16(next + 4, change->count);
    size_t upper = RECORD_PAGE_BYTES;
    for (uint16_t i = 0; i < change->count; i++) {
        record_slice_t value;
        if (i == change->slot) {
            if (change->is_delete) continue;
            value = change->value;
        } else {
            const uint8_t *entry = get_slot(page, i);
            if (read_u16(entry) == 0) continue;
            value = (record_slice_t){page + read_u16(entry), read_u16(entry + 2)};
        }
        upper -= value.size_bytes;
        if (value.size_bytes > 0) memcpy(next + upper, value.bytes, value.size_bytes);
        uint8_t *entry = next + PAGE_HEADER_BYTES + i * SLOT_BYTES;
        write_u16(entry, (uint16_t)upper);
        write_u16(entry + 2, (uint16_t)value.size_bytes);
    }
    memcpy(page, next, RECORD_PAGE_BYTES);
    free(next);
    return RECORD_PAGE_OK;
}

record_page_status_t record_page_insert(uint8_t *page, record_slice_t value,
                                        uint16_t *output)
{
    if (output == NULL) return RECORD_PAGE_INVALID;
    if (value.size_bytes > 0 && value.bytes == NULL) return RECORD_PAGE_INVALID;
    record_page_status_t status = record_page_validate(page);
    if (status != RECORD_PAGE_OK) return status;
    uint16_t slot = read_u16(page + 4);
    page_change_t change = {slot, (uint16_t)(slot + 1), value, 0};
    status = apply_change(page, &change);
    if (status == RECORD_PAGE_OK) *output = slot;
    return status;
}

record_page_status_t record_page_update(uint8_t *page, uint16_t slot,
                                        record_slice_t value)
{
    if (value.size_bytes > 0 && value.bytes == NULL) return RECORD_PAGE_INVALID;
    record_slice_t previous;
    record_page_status_t status = record_page_get(page, slot, &previous);
    if (status != RECORD_PAGE_OK) return status;
    page_change_t change = {slot, read_u16(page + 4), value, 0};
    return apply_change(page, &change);
}

record_page_status_t record_page_delete(uint8_t *page, uint16_t slot)
{
    record_slice_t previous;
    record_page_status_t status = record_page_get(page, slot, &previous);
    if (status != RECORD_PAGE_OK) return status;
    page_change_t change = {slot, read_u16(page + 4), {NULL, 0}, 1};
    return apply_change(page, &change);
}
