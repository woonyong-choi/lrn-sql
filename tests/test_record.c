#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/record.h"
#include "storage/record_page.h"

/* #81: NULL·빈 값·정수 경계·페이지보다 큰 값을 구분해 왕복한다. */
static void record_roundtrip_preserves_values(void)
{
    const uint8_t text[] = "한글🙂";
    uint8_t large[32769];
    for (size_t i = 0; i < sizeof(large); i++) large[i] = (uint8_t)i;
    record_value_t input[] = {
        {.type = RECORD_NULL},
        {.type = RECORD_TEXT},
        {.type = RECORD_INTEGER, .integer = INT64_MIN},
        {.type = RECORD_INTEGER, .integer = INT64_MAX},
        {.type = RECORD_TEXT, .bytes = text, .size_bytes = sizeof(text) - 1},
        {.type = RECORD_BLOB, .bytes = large, .size_bytes = sizeof(large)}
    };
    record_buffer_t buffer = {0};
    record_value_t output[6];
    size_t count = 0;

    assert(record_encode(input, 6, &buffer) == RECORD_OK);
    assert(buffer.size_bytes == 8 + 6 * 5 + 16 + sizeof(text) - 1 + sizeof(large));
    assert(record_decode(&buffer, output, 6, &count) == RECORD_OK);

    assert(count == 6);
    for (size_t i = 0; i < count; i++) {
        assert(output[i].type == input[i].type);
        if (input[i].type == RECORD_INTEGER) {
            assert(output[i].integer == input[i].integer);
        } else {
            assert(output[i].size_bytes == input[i].size_bytes);
            if (input[i].size_bytes) {
                assert(memcmp(output[i].bytes, input[i].bytes, input[i].size_bytes) == 0);
            }
        }
    }
    free(buffer.bytes);
}

/* #81: 고정 golden bytes로 endian·길이 형식을 검사한다. */
static void record_golden_format_matches_contract(void)
{
    const uint8_t expected[] = {
        'L','R','C','1', 2,0,0,0, 0,0,0,0,0,
        1,8,0,0,0, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    record_value_t input[] = {{.type=RECORD_NULL}, {.type=RECORD_INTEGER, .integer=-1}};
    record_buffer_t buffer = {0};
    assert(record_encode(input, 2, &buffer) == RECORD_OK);
    assert(buffer.size_bytes == sizeof(expected));
    assert(memcmp(buffer.bytes, expected, sizeof(expected)) == 0);
    free(buffer.bytes);
}

/* #81: 손상·잘린 입력은 부분 decode를 게시하지 않는다. */
static void record_invalid_input_preserves_output(void)
{
    record_value_t input = {.type=RECORD_INTEGER, .integer=42};
    record_buffer_t buffer = {0};
    assert(record_encode(&input, 1, &buffer) == RECORD_OK);
    record_value_t output = {.type=RECORD_INTEGER, .integer=123};
    size_t count = 99;
    for (size_t i = 0; i < buffer.size_bytes; i++) {
        record_buffer_t cut = {buffer.bytes, i};
        assert(record_decode(&cut, &output, 1, &count) == RECORD_INVALID);
        assert(output.integer == 123 && count == 99);
    }
    buffer.bytes[9] = 0xff;
    assert(record_decode(&buffer, &output, 1, &count) == RECORD_INVALID);
    assert(output.integer == 123 && count == 99);
    free(buffer.bytes);

    record_value_t invalid = {.type=RECORD_TEXT, .size_bytes=1};
    buffer = (record_buffer_t){NULL, 123};
    assert(record_encode(&invalid, 1, &buffer) == RECORD_INVALID);
    assert(buffer.bytes == NULL && buffer.size_bytes == 123);
    invalid.size_bytes = SIZE_MAX;
    assert(record_encode(&invalid, 1, &buffer) == RECORD_LIMIT);
}

/* #81: 행 성장·삭제로 압축해도 살아 있는 slot 번호는 유지한다. */
static void record_page_changes_preserve_slot_identity(void)
{
    uint8_t page[RECORD_PAGE_BYTES];
    uint8_t before[RECORD_PAGE_BYTES];
    uint8_t large[8000];
    memset(large, 'z', sizeof(large));
    record_page_init(page);
    uint16_t first, second, third;
    record_slice_t value = {(const uint8_t *)"one", 3};
    assert(record_page_insert(page, value, &first) == RECORD_PAGE_OK);
    value = (record_slice_t){(const uint8_t *)"two", 3};
    assert(record_page_insert(page, value, &second) == RECORD_PAGE_OK);
    value = (record_slice_t){large, sizeof(large)};
    assert(record_page_update(page, first, value) == RECORD_PAGE_OK);
    record_slice_t result;
    assert(record_page_get(page, second, &result) == RECORD_PAGE_OK);
    assert(result.size_bytes == 3 && memcmp(result.bytes, "two", 3) == 0);

    memcpy(before, page, sizeof(page));
    assert(record_page_update(page, second, value) == RECORD_PAGE_FULL);
    assert(memcmp(before, page, sizeof(page)) == 0);
    assert(record_page_delete(page, first) == RECORD_PAGE_OK);
    assert(record_page_get(page, first, &result) == RECORD_PAGE_NOT_FOUND);
    assert(record_page_insert(page, value, &third) == RECORD_PAGE_OK);
    assert(third != first && third != second);
    assert(record_page_get(page, second, &result) == RECORD_PAGE_OK);
    assert(result.size_bytes == 3 && memcmp(result.bytes, "two", 3) == 0);
    assert(record_page_get(page, third, &result) == RECORD_PAGE_OK);
    assert(result.size_bytes == sizeof(large));
    assert(memcmp(result.bytes, large, sizeof(large)) == 0);
    assert(record_page_update(page, third, (record_slice_t){NULL, 0}) == RECORD_PAGE_OK);
    assert(record_page_get(page, third, &result) == RECORD_PAGE_OK);
    assert(result.size_bytes == 0);
}

/* #81: page 길이 경계와 손상은 원본·출력 인자를 바꾸지 않는다. */
static void record_page_bounds_preserve_original(void)
{
    uint8_t page[RECORD_PAGE_BYTES];
    uint8_t before[RECORD_PAGE_BYTES];
    uint8_t payload[8181];
    memset(payload, 'x', sizeof(payload));
    uint16_t slot = 123;
    record_page_init(page);
    memcpy(before, page, sizeof(page));
    assert(record_page_insert(page, (record_slice_t){payload, 8181}, &slot) == RECORD_PAGE_FULL);
    assert(slot == 123 && memcmp(page, before, sizeof(page)) == 0);
    assert(record_page_insert(page, (record_slice_t){payload, 8180}, &slot) == RECORD_PAGE_OK);
    assert(slot == 0);
    record_slice_t output = {NULL, 123};
    page[8] = 0xff;
    memcpy(before, page, sizeof(page));
    assert(record_page_get(page, slot, &output) == RECORD_PAGE_INVALID);
    assert(record_page_delete(page, slot) == RECORD_PAGE_INVALID);
    assert(output.bytes == NULL && output.size_bytes == 123);
    assert(memcmp(page, before, sizeof(page)) == 0);
}

int main(void)
{
    record_roundtrip_preserves_values();
    record_golden_format_matches_contract();
    record_invalid_input_preserves_output();
    record_page_changes_preserve_slot_identity();
    record_page_bounds_preserve_original();
    puts("record: codec, stable slots, resize/delete, page bounds and malformed input passed");
    return 0;
}
