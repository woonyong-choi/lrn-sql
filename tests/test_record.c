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
    /* 형식 판 1 + 필드 수 1 + 코드(1·1·1·1·1·3) + 정수 8·8 + 값 바이트 */
    assert(buffer.size_bytes == 1 + 1 + 8 + 16 + sizeof(text) - 1 + sizeof(large));
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

static void expect_bytes(const record_value_t *input, size_t count, const uint8_t *expected, size_t size)
{
    record_buffer_t buffer = {0};
    assert(record_encode(input, count, &buffer) == RECORD_OK);
    assert(buffer.size_bytes == size);
    assert(memcmp(buffer.bytes, expected, size) == 0);
    record_value_t output[8];
    size_t got = 0;
    assert(record_decode(&buffer, output, 8, &got) == RECORD_OK && got == count);
    free(buffer.bytes);
}

/* #192: 고정 golden bytes로 형식 판·varint·정수 최소 폭·TEXT/BLOB 코드를 검사한다. */
static void record_golden_format_matches_contract(void)
{
    record_value_t pair[] = {{.type=RECORD_NULL}, {.type=RECORD_INTEGER, .integer=-1}};
    const uint8_t pair_bytes[] = {0x02, 2, 0, 3, 0xff};
    expect_bytes(pair, 2, pair_bytes, sizeof(pair_bytes));

    record_value_t small[] = {{.type=RECORD_INTEGER, .integer=0}, {.type=RECORD_INTEGER, .integer=1},
                              {.type=RECORD_INTEGER, .integer=2}, {.type=RECORD_INTEGER, .integer=-129},
                              {.type=RECORD_INTEGER, .integer=INT64_MIN}};
    const uint8_t small_bytes[] = {0x02, 5, 1, 2, 3, 4, 10,
                                   2, 0x7f, 0xff, 0, 0, 0, 0, 0, 0, 0, 0x80};
    expect_bytes(small, 5, small_bytes, sizeof(small_bytes));

    uint8_t long_text[70];
    memset(long_text, 'a', sizeof(long_text));
    record_value_t bytes[] = {{.type=RECORD_TEXT, .bytes=(const uint8_t *)"hi", .size_bytes=2},
                              {.type=RECORD_BLOB, .bytes=(const uint8_t *)"\x00", .size_bytes=1},
                              {.type=RECORD_TEXT, .bytes=long_text, .size_bytes=sizeof(long_text)}};
    /* 12+2*2=16, 11+2*1=13, 12+2*70=152 -> varint 0x98 0x01 */
    uint8_t long_bytes[1 + 1 + 4 + 2 + 1 + sizeof(long_text)] = {0x02, 3, 16, 13, 0x98, 0x01, 'h', 'i', 0};
    memcpy(long_bytes + 9, long_text, sizeof(long_text));
    expect_bytes(bytes, 3, long_bytes, sizeof(long_bytes));
}

/* #192: 한 값의 인코딩은 하나다. 최소가 아닌 정수·varint, 옛 형식, 남거나 모자란 바이트는 거절한다. */
static void record_rejects_non_canonical_input(void)
{
    const uint8_t cases[][6] = {
        {0x02, 1, 4, 42, 0},        /* 42를 2바이트로 */
        {0x02, 1, 3, 0},            /* 0은 코드 1 */
        {0x02, 1, 3, 1},            /* 1은 코드 2 */
        {0x02, 0x81, 0x00, 1},      /* 필드 수 varint가 최소가 아님 */
        {0x02, 1, 0x8d, 0x00, 0},   /* 코드 varint가 최소가 아님(13) */
        {'L', 'R', 'C', '1', 0, 0}, /* 옛 형식 */
        {0x02, 1, 1, 0},            /* 남는 바이트 */
        {0x02, 1, 16, 'h'},         /* 모자란 바이트 */
    };
    const size_t sizes[] = {5, 4, 4, 4, 5, 6, 4, 4};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        record_buffer_t input = {(uint8_t *)cases[i], sizes[i]};
        record_value_t output = {.type = RECORD_TEXT, .integer = 77};
        size_t count = 55;
        assert(record_decode(&input, &output, 1, &count) == RECORD_INVALID);
        assert(output.type == RECORD_TEXT && output.integer == 77 && count == 55);
    }
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
    buffer.bytes[2] = 0x7f;  /* 58바이트 BLOB 코드인데 값이 1바이트뿐 */
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
    size_t available;
    assert(record_page_available(page, &available) == RECORD_PAGE_OK && available == RECORD_PAGE_BYTES - 8u);
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
    assert(record_page_available(page, &available) == RECORD_PAGE_OK && available == RECORD_PAGE_BYTES - 19u);
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
    uint8_t payload[8149]; /* 본문 8160 - 헤더 8 - 슬롯 4 = 8148이 최대 */
    memset(payload, 'x', sizeof(payload));
    uint16_t slot = 123;
    record_page_init(page);
    memcpy(before, page, sizeof(page));
    assert(record_page_insert(page, (record_slice_t){payload, 8149}, &slot) == RECORD_PAGE_FULL);
    assert(slot == 123 && memcmp(page, before, sizeof(page)) == 0);
    assert(record_page_insert(page, (record_slice_t){payload, 8148}, &slot) == RECORD_PAGE_OK);
    assert(slot == 0);
    record_slice_t output = {NULL, 123};
    page[8] = 0xff;
    memcpy(before, page, sizeof(page));
    assert(record_page_get(page, slot, &output) == RECORD_PAGE_INVALID);
    assert(record_page_delete(page, slot) == RECORD_PAGE_INVALID);
    assert(output.bytes == NULL && output.size_bytes == 123);
    assert(memcmp(page, before, sizeof(page)) == 0);
}

/*
 * #190: slot 하나를 꺼낼 때는 그 slot의 경계만 본다. 페이지 전체 검사(record_page_validate)는 읽을 때 한 번이며,
 * slot마다 전체를 검사하면 페이지 하나를 훑는 비용이 slot 수의 제곱이 된다.
 */
static void record_page_get_checks_only_its_slot(void)
{
    uint8_t page[RECORD_PAGE_BYTES];
    record_page_init(page);
    uint16_t slots[3];
    const char *values[3] = {"first", "second", "third"};
    for (int i = 0; i < 3; i++) {
        record_slice_t value = {(const uint8_t *)values[i], strlen(values[i])};
        assert(record_page_insert(page, value, &slots[i]) == RECORD_PAGE_OK);
    }
    /* 가운데 slot의 크기를 페이지 밖까지로 깨뜨린다 */
    page[8 + 4 * slots[1] + 2] = 0xff;
    page[8 + 4 * slots[1] + 3] = 0xff;
    assert(record_page_validate(page) == RECORD_PAGE_INVALID);
    record_slice_t output = {NULL, 0};
    assert(record_page_get(page, slots[0], &output) == RECORD_PAGE_OK);
    assert(output.size_bytes == 5 && memcmp(output.bytes, "first", 5) == 0);
    assert(record_page_get(page, slots[2], &output) == RECORD_PAGE_OK);
    assert(output.size_bytes == 5 && memcmp(output.bytes, "third", 5) == 0);
    output = (record_slice_t){NULL, 9};
    assert(record_page_get(page, slots[1], &output) == RECORD_PAGE_INVALID);
    assert(output.bytes == NULL && output.size_bytes == 9);

    /* 위치가 slot 목록 안을 가리키면 거절한다 */
    record_page_init(page);
    for (int i = 0; i < 3; i++) {
        record_slice_t value = {(const uint8_t *)values[i], strlen(values[i])};
        assert(record_page_insert(page, value, &slots[i]) == RECORD_PAGE_OK);
    }
    page[8 + 4 * slots[2]] = 8;
    page[8 + 4 * slots[2] + 1] = 0;
    assert(record_page_get(page, slots[2], &output) == RECORD_PAGE_INVALID);
    /* 비어 있는 slot에 크기가 있으면 거절한다 */
    page[8 + 4 * slots[2]] = 0;
    page[8 + 4 * slots[2] + 2] = 1;
    assert(record_page_get(page, slots[2], &output) == RECORD_PAGE_INVALID);
}

/* #77: 본문 8192의 옛 LRP1 입력은 LRP2로 해석하지 않는다. */
static void record_page_rejects_previous_format(void)
{
    uint8_t old_empty[8192] = {'L', 'R', 'P', '1'};
    assert(record_page_validate(old_empty) == RECORD_PAGE_INVALID);
    uint8_t page[RECORD_PAGE_BYTES];
    record_page_init(page);
    assert(record_page_validate(page) == RECORD_PAGE_OK);
    assert(memcmp(page, "LRP2", 4) == 0);
    memcpy(page, "LRP1", 4);
    record_slice_t output = {NULL, 7};
    assert(record_page_validate(page) == RECORD_PAGE_INVALID);
    assert(record_page_get(page, 0, &output) == RECORD_PAGE_INVALID);
    assert(output.bytes == NULL && output.size_bytes == 7);
    uint16_t slot = 55;
    assert(record_page_insert(page, (record_slice_t){(const uint8_t *)"x", 1}, &slot) ==
           RECORD_PAGE_INVALID);
    assert(slot == 55);
}

int main(void)
{
    record_roundtrip_preserves_values();
    record_golden_format_matches_contract();
    record_rejects_non_canonical_input();
    record_invalid_input_preserves_output();
    record_page_changes_preserve_slot_identity();
    record_page_bounds_preserve_original();
    record_page_rejects_previous_format();
    record_page_get_checks_only_its_slot();
    puts("record: codec, stable slots, resize/delete, page bounds and malformed input passed");
    return 0;
}
