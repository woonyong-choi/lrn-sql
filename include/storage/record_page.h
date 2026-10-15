#ifndef RECORD_PAGE_H
#define RECORD_PAGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 저장 페이지 하나는 물리 8192바이트이며 앞 32바이트는 page_file의 공통 envelope이다.
 * 레코드 페이지는 남은 본문 8160바이트만 쓴다. 본문을 8192로 가정하면 envelope를 덮는다.
 * 같은 값을 page_file.h의 PAGE_BODY_BYTES가 가지며 page_file.c가 정적으로 대조한다.
 * 이전 형식(LRP1, 본문 8192)은 magic이 달라 이 모듈이 해석하지 않는다.
 */
#define RECORD_PAGE_BYTES 8160u
#define RECORD_PAGE_MAGIC "LRP2"

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
/* 새 slot을 만들 때 쓸 수 있는 연속 바이트 수. 삭제된 slot 번호는 재사용하지 않는다. */
record_page_status_t record_page_available(const uint8_t *page, size_t *bytes);

#endif
