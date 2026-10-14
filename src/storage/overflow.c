#include <stdlib.h>
#include <string.h>

#include "storage/overflow.h"

#define OVERFLOW_HEADER_BYTES 24u
#define OVERFLOW_CHECKED_BYTES 20u

typedef struct {
    uint32_t index;
    uint32_t next;
    uint32_t chunk;
    uint32_t total;
} page_header_t;

static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t read_u32(const uint8_t *bytes)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value |= (uint32_t)bytes[i] << (i * 8);
    return value;
}

/* 체크섬 필드를 뺀 페이지 전체를 덮어 패딩 손상도 잡는다. */
static uint32_t checksum(const uint8_t *page, size_t page_bytes)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < page_bytes; i++) {
        if (i >= OVERFLOW_CHECKED_BYTES && i < OVERFLOW_HEADER_BYTES) continue;
        hash = (hash ^ page[i]) * 16777619u;
    }
    return hash;
}

static bool io_valid(const overflow_io_t *io)
{
    return io != NULL && io->allocate != NULL && io->release != NULL &&
           io->read != NULL && io->write != NULL &&
           io->page_bytes >= OVERFLOW_MIN_PAGE_BYTES &&
           io->page_bytes <= OVERFLOW_MAX_PAGE_BYTES;
}

static size_t payload_bytes(const overflow_io_t *io)
{
    return io->page_bytes - OVERFLOW_HEADER_BYTES;
}

static size_t page_count(const overflow_io_t *io, size_t size_bytes)
{
    size_t payload = payload_bytes(io);
    return size_bytes / payload + (size_bytes % payload != 0);
}

static void encode_page(uint8_t *page, size_t page_bytes, page_header_t header,
                        const uint8_t *chunk)
{
    memset(page, 0, page_bytes);
    memcpy(page, "LRO1", 4);
    write_u32(page + 4, header.index);
    write_u32(page + 8, header.next);
    write_u32(page + 12, header.chunk);
    write_u32(page + 16, header.total);
    if (header.chunk > 0) memcpy(page + OVERFLOW_HEADER_BYTES, chunk, header.chunk);
    write_u32(page + 20, checksum(page, page_bytes));
}

static bool decode_page(const uint8_t *page, size_t page_bytes, page_header_t *header)
{
    if (memcmp(page, "LRO1", 4) != 0) return false;
    header->index = read_u32(page + 4);
    header->next = read_u32(page + 8);
    header->chunk = read_u32(page + 12);
    header->total = read_u32(page + 16);
    if (header->chunk > page_bytes - OVERFLOW_HEADER_BYTES) return false;
    return read_u32(page + 20) == checksum(page, page_bytes);
}

static void release_all(const overflow_io_t *io, const uint32_t *ids, size_t count,
                        bool *failed)
{
    for (size_t i = 0; i < count; i++) {
        if (!io->release(io->context, ids[i])) *failed = true;
    }
}

static overflow_status_t write_pages(const overflow_io_t *io, const uint8_t *bytes,
                                     size_t size_bytes, const uint32_t *ids,
                                     size_t count, uint8_t *page)
{
    size_t payload = payload_bytes(io);
    for (size_t i = 0; i < count; i++) {
        size_t offset = i * payload;
        size_t chunk = size_bytes - offset < payload ? size_bytes - offset : payload;
        page_header_t header = {(uint32_t)i, i + 1 < count ? ids[i + 1] : OVERFLOW_NO_PAGE,
                                (uint32_t)chunk, (uint32_t)size_bytes};
        encode_page(page, io->page_bytes, header, bytes + offset);
        if (!io->write(io->context, ids[i], page)) return OVERFLOW_IO;
    }
    return OVERFLOW_OK;
}

static overflow_status_t allocate_pages(const overflow_io_t *io, uint32_t *ids,
                                        size_t count, size_t *allocated)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t id = OVERFLOW_NO_PAGE;
        if (!io->allocate(io->context, &id)) return OVERFLOW_IO;
        if (id == OVERFLOW_NO_PAGE) return OVERFLOW_INVALID;
        ids[i] = id;
        *allocated = i + 1;
    }
    return OVERFLOW_OK;
}

overflow_status_t overflow_write(const overflow_io_t *io, const uint8_t *bytes,
                                 size_t size_bytes, overflow_ref_t *ref)
{
    if (!io_valid(io) || ref == NULL) return OVERFLOW_INVALID;
    if (size_bytes > UINT32_MAX) return OVERFLOW_LIMIT;
    if (size_bytes > 0 && bytes == NULL) return OVERFLOW_INVALID;
    if (size_bytes == 0) {
        *ref = (overflow_ref_t){OVERFLOW_NO_PAGE, 0};
        return OVERFLOW_OK;
    }
    size_t count = page_count(io, size_bytes);
    if (count > SIZE_MAX / sizeof(uint32_t)) return OVERFLOW_LIMIT;
    uint32_t *ids = malloc(count * sizeof(uint32_t));
    uint8_t *page = malloc(io->page_bytes);
    if (ids == NULL || page == NULL) {
        free(ids);
        free(page);
        return OVERFLOW_NOMEM;
    }
    size_t allocated = 0;
    overflow_status_t status = allocate_pages(io, ids, count, &allocated);
    if (status == OVERFLOW_OK) status = write_pages(io, bytes, size_bytes, ids, count, page);
    if (status == OVERFLOW_OK) {
        *ref = (overflow_ref_t){ids[0], (uint32_t)size_bytes};
    } else {
        bool failed = false;
        release_all(io, ids, allocated, &failed);
        if (failed) status = OVERFLOW_ROLLBACK_FAILED;
    }
    free(ids);
    free(page);
    return status;
}

static overflow_status_t check_page(const overflow_io_t *io, const uint8_t *page,
                                    size_t index, overflow_ref_t ref, size_t remaining,
                                    page_header_t *header)
{
    if (!decode_page(page, io->page_bytes, header)) return OVERFLOW_CORRUPT;
    size_t payload = payload_bytes(io);
    size_t expected = remaining < payload ? remaining : payload;
    if (header->index != index || header->total != ref.size_bytes) return OVERFLOW_CORRUPT;
    if (header->chunk != expected) return OVERFLOW_CORRUPT;
    bool last = remaining == expected;
    if (last != (header->next == OVERFLOW_NO_PAGE)) return OVERFLOW_CORRUPT;
    return OVERFLOW_OK;
}

/* 체인을 검증하며 ids와 dest를 채운다. 둘 다 호출자가 크기를 보장하고 없으면 NULL이다. */
static overflow_status_t walk_chain(const overflow_io_t *io, overflow_ref_t ref,
                                    uint8_t *page, uint32_t *ids, uint8_t *dest)
{
    size_t remaining = ref.size_bytes;
    uint32_t id = ref.head;
    for (size_t index = 0; remaining > 0; index++) {
        if (id == OVERFLOW_NO_PAGE) return OVERFLOW_CORRUPT;
        if (!io->read(io->context, id, page)) return OVERFLOW_IO;
        page_header_t header;
        overflow_status_t status = check_page(io, page, index, ref, remaining, &header);
        if (status != OVERFLOW_OK) return status;
        if (ids != NULL) ids[index] = id;
        if (dest != NULL) {
            memcpy(dest + (ref.size_bytes - remaining), page + OVERFLOW_HEADER_BYTES,
                   header.chunk);
        }
        remaining -= header.chunk;
        id = header.next;
    }
    return OVERFLOW_OK;
}

static bool ref_valid(overflow_ref_t ref)
{
    return (ref.head == OVERFLOW_NO_PAGE) == (ref.size_bytes == 0);
}

overflow_status_t overflow_read(const overflow_io_t *io, overflow_ref_t ref,
                                uint8_t **bytes)
{
    if (!io_valid(io) || bytes == NULL || !ref_valid(ref)) return OVERFLOW_INVALID;
    if (ref.size_bytes == 0) {
        *bytes = NULL;
        return OVERFLOW_OK;
    }
    uint8_t *page = malloc(io->page_bytes);
    uint8_t *data = malloc(ref.size_bytes);
    if (page == NULL || data == NULL) {
        free(page);
        free(data);
        return OVERFLOW_NOMEM;
    }
    overflow_status_t status = walk_chain(io, ref, page, NULL, data);
    free(page);
    if (status != OVERFLOW_OK) {
        free(data);
        return status;
    }
    *bytes = data;
    return OVERFLOW_OK;
}

overflow_status_t overflow_free(const overflow_io_t *io, overflow_ref_t ref)
{
    if (!io_valid(io) || !ref_valid(ref)) return OVERFLOW_INVALID;
    if (ref.size_bytes == 0) return OVERFLOW_OK;
    size_t count = page_count(io, ref.size_bytes);
    if (count > SIZE_MAX / sizeof(uint32_t)) return OVERFLOW_LIMIT;
    uint32_t *ids = malloc(count * sizeof(uint32_t));
    uint8_t *page = malloc(io->page_bytes);
    if (ids == NULL || page == NULL) {
        free(ids);
        free(page);
        return OVERFLOW_NOMEM;
    }
    overflow_status_t status = walk_chain(io, ref, page, ids, NULL);
    if (status == OVERFLOW_OK) {
        bool failed = false;
        release_all(io, ids, count, &failed);
        if (failed) status = OVERFLOW_UNRECLAIMED;
    }
    free(ids);
    free(page);
    return status;
}
