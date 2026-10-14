#ifndef OVERFLOW_H
#define OVERFLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OVERFLOW_NO_PAGE 0u
#define OVERFLOW_MIN_PAGE_BYTES 64u
#define OVERFLOW_MAX_PAGE_BYTES 65536u

typedef enum {
    OVERFLOW_OK,
    OVERFLOW_INVALID,
    OVERFLOW_LIMIT,
    OVERFLOW_NOMEM,
    OVERFLOW_IO,
    OVERFLOW_CORRUPT,
    /* 요청은 반영됐으나 일부 페이지를 회수하지 못했다. 고아 페이지가 남는다. */
    OVERFLOW_UNRECLAIMED,
    /* 쓰기가 실패했고 이미 할당한 페이지도 되돌리지 못했다. 출력은 변경하지 않는다. */
    OVERFLOW_ROLLBACK_FAILED
} overflow_status_t;

/*
 * 저장소 어댑터 계약. 모든 callback은 성공하면 true, 실패하면 false를 반환한다.
 * - allocate: 성공 시 OVERFLOW_NO_PAGE(0)가 아니고 기존 live 페이지와 겹치지 않는 새 번호를
 *   돌려준다. 실패하면 *page_id와 할당 상태를 바꾸지 않는다.
 * - read/write: 정확히 page_bytes 바이트를 다룬다. 부분 읽기·쓰기는 실패로 반환한다.
 * - release: 이 체인을 읽는 reader와 되돌릴 rollback 참조가 모두 끝난 뒤에만 호출된다.
 *   그 보장은 이 모듈이 아니라 호출하는 상위 계층의 reader 수명 계약이다.
 */
typedef struct {
    void *context;
    size_t page_bytes;
    bool (*allocate)(void *context, uint32_t *page_id);
    bool (*release)(void *context, uint32_t page_id);
    bool (*read)(void *context, uint32_t page_id, uint8_t *page);
    bool (*write)(void *context, uint32_t page_id, const uint8_t *page);
} overflow_io_t;

/* record가 보관하는 큰 값 참조. 빈 값은 head가 OVERFLOW_NO_PAGE다. */
typedef struct {
    uint32_t head;
    uint32_t size_bytes;
} overflow_ref_t;

/*
 * 항상 새 체인을 만든다. 기존 ref의 소유권과 해제는 호출자 책임이다.
 * 실패하면 ref는 그대로이고 이번 호출이 할당한 페이지는 회수한다.
 * 각 페이지는 체인 head 번호를 담아 다른 체인으로의 접합을 거절한다.
 */
overflow_status_t overflow_write(const overflow_io_t *io, const uint8_t *bytes,
                                 size_t size_bytes, overflow_ref_t *ref);
/*
 * 성공한 *bytes는 호출자가 free한다. 체인이 ref와 어긋나면 OVERFLOW_CORRUPT이며 출력은 그대로다.
 * 해제 뒤 재사용된 페이지를 가리키는 오래된 ref는 감지하지 못한다. reader가 살아 있는 동안
 * 해제하지 않는 것은 상위 계층 계약이다.
 */
overflow_status_t overflow_read(const overflow_io_t *io, overflow_ref_t ref,
                                uint8_t **bytes);
/* 체인을 끝까지 검증한 뒤 모든 페이지 회수를 시도한다. 손상 체인은 아무것도 회수하지 않는다. */
overflow_status_t overflow_free(const overflow_io_t *io, overflow_ref_t ref);

#endif
