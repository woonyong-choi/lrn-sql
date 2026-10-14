#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/record.h"
#include "storage/record_page.h"

static int should_fail;

void *record_test_malloc(size_t size)
{
    return should_fail ? NULL : malloc(size);
}

void *record_test_calloc(size_t count, size_t size)
{
    return should_fail ? NULL : calloc(count, size);
}

/* #81: 할당 실패는 원본 페이지·값·출력 식별자를 변경하지 않는다. */
int main(void)
{
    record_value_t value = {.type=RECORD_INTEGER, .integer=7};
    record_buffer_t buffer = {NULL, 123};
    should_fail = 1;
    assert(record_encode(&value, 1, &buffer) == RECORD_NOMEM);
    assert(buffer.bytes == NULL && buffer.size_bytes == 123);
    should_fail = 0;
    assert(record_encode(&value, 1, &buffer) == RECORD_OK);

    uint8_t page[RECORD_PAGE_BYTES];
    uint8_t before[RECORD_PAGE_BYTES];
    record_page_init(page);
    uint16_t slot;
    record_slice_t payload = {buffer.bytes, buffer.size_bytes};
    assert(record_page_insert(page, payload, &slot) == RECORD_PAGE_OK);
    memcpy(before, page, sizeof(page));

    should_fail = 1;
    uint16_t output = 123;
    assert(record_page_insert(page, payload, &output) == RECORD_PAGE_NOMEM);
    assert(output == 123 && memcmp(before, page, sizeof(page)) == 0);
    assert(record_page_update(page, slot, (record_slice_t){NULL, 0}) == RECORD_PAGE_NOMEM);
    assert(memcmp(before, page, sizeof(page)) == 0);
    assert(record_page_delete(page, slot) == RECORD_PAGE_NOMEM);
    assert(memcmp(before, page, sizeof(page)) == 0);

    should_fail = 0;
    assert(record_page_delete(page, slot) == RECORD_PAGE_OK);
    free(buffer.bytes);
    puts("record: allocation failures preserve page and output; next operation succeeds");
    return 0;
}
