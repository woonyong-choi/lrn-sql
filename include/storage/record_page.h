#ifndef RECORD_PAGE_H
#define RECORD_PAGE_H

#include <stddef.h>
#include <stdint.h>

#define RECORD_PAGE_BYTES 8192u

typedef enum {
    RECORD_PAGE_OK,
    RECORD_PAGE_INVALID,
    RECORD_PAGE_FULL,
    RECORD_PAGE_NOT_FOUND,
    RECORD_PAGE_NOMEM
} record_page_status_t;

typedef struct {
    const uint8_t *bytes;
    size_t size_bytes;
} record_slice_t;

void record_page_init(uint8_t page[RECORD_PAGE_BYTES]);
record_page_status_t record_page_validate(const uint8_t page[RECORD_PAGE_BYTES]);
/* 조회 결과는 page를 빌리며 다음 수정부터 무효다. */
record_page_status_t record_page_get(const uint8_t *page, uint16_t slot,
                                     record_slice_t *output);
/* 실패 시 page와 output은 그대로이며 삭제한 slot 번호는 재사용하지 않는다. */
record_page_status_t record_page_insert(uint8_t *page, record_slice_t value,
                                        uint16_t *output);
record_page_status_t record_page_update(uint8_t *page, uint16_t slot,
                                        record_slice_t value);
record_page_status_t record_page_delete(uint8_t *page, uint16_t slot);

#endif
