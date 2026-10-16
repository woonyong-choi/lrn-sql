#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "storage/record.h"

/*
 * 행 하나의 바이트 형식(#192).
 *   형식 판 1바이트(RECORD_FORMAT) | 필드 수 varint | 필드마다 코드 varint | 값 바이트를 순서대로
 * 코드: 0 NULL, 1 정수 0, 2 정수 1, 3..10 정수 (코드-2)바이트(little-endian 2의 보수, 최소 폭),
 *       11+2n n바이트 BLOB, 12+2n n바이트 TEXT.
 * varint는 LEB128이다. 최소 길이가 아닌 varint와 최소 폭이 아닌 정수는 거절해 한 값의 인코딩을 하나로 둔다.
 */
#define RECORD_FORMAT 0x02u
#define CODE_NULL 0u
#define CODE_ZERO 1u
#define CODE_ONE 2u
#define CODE_INT_BASE 2u
#define CODE_BYTES_BASE 11u
#define VARINT_MAX_BYTES 10u

static size_t varint_size(uint64_t value)
{
    size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        size++;
    }
    return size;
}

static size_t put_varint(uint8_t *bytes, uint64_t value)
{
    size_t used = 0;
    while (value >= 0x80) {
        bytes[used++] = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    bytes[used++] = (uint8_t)value;
    return used;
}

/* 최소 길이의 varint만 읽는다. 실패하면 0을 돌려준다. */
static size_t get_varint(const uint8_t *bytes, size_t available, uint64_t *value)
{
    uint64_t result = 0;
    for (size_t i = 0; i < available && i < VARINT_MAX_BYTES; i++) {
        uint64_t part = bytes[i] & 0x7f;
        if (i == VARINT_MAX_BYTES - 1 && part > 1) return 0;
        result |= part << (7 * i);
        if ((bytes[i] & 0x80) == 0) {
            if (i > 0 && bytes[i] == 0) return 0;
            *value = result;
            return i + 1;
        }
    }
    return 0;
}

/* v를 담는 가장 작은 2의 보수 바이트 수(1..8) */
static size_t integer_width(int64_t value)
{
    for (size_t width = 1; width < 8; width++) {
        int64_t limit = (int64_t)1 << (8 * width - 1);
        if (value >= -limit && value < limit) return width;
    }
    return 8;
}

static record_status_t field_code(const record_value_t *value, uint64_t *code, size_t *payload)
{
    switch (value->type) {
        case RECORD_NULL:
            *code = CODE_NULL;
            *payload = 0;
            return RECORD_OK;
        case RECORD_INTEGER:
            if (value->integer == 0 || value->integer == 1) {
                *code = value->integer == 0 ? CODE_ZERO : CODE_ONE;
                *payload = 0;
            } else {
                *payload = integer_width(value->integer);
                *code = CODE_INT_BASE + *payload;
            }
            return RECORD_OK;
        case RECORD_TEXT:
        case RECORD_BLOB:
            if (value->size_bytes > UINT32_MAX) return RECORD_LIMIT;
            if (value->size_bytes > 0 && value->bytes == NULL) return RECORD_INVALID;
            *payload = value->size_bytes;
            *code = CODE_BYTES_BASE + 2u * (uint64_t)value->size_bytes + (value->type == RECORD_TEXT);
            return RECORD_OK;
        default:
            return RECORD_INVALID;
    }
}

static record_status_t encoded_size(const record_value_t *values, size_t count, size_t *size)
{
    if (count > RECORD_MAX_FIELDS) return RECORD_LIMIT;
    if (count > 0 && values == NULL) return RECORD_INVALID;
    size_t total = 1 + varint_size(count);
    for (size_t i = 0; i < count; i++) {
        uint64_t code;
        size_t payload;
        record_status_t status = field_code(&values[i], &code, &payload);
        if (status != RECORD_OK) return status;
        size_t addition = varint_size(code) + payload;
        if (total > UINT32_MAX - addition) return RECORD_LIMIT;
        total += addition;
    }
    *size = total;
    return RECORD_OK;
}

record_status_t record_encode(const record_value_t *values, size_t count,
                              record_buffer_t *output)
{
    if (output == NULL) return RECORD_INVALID;
    size_t size;
    record_status_t status = encoded_size(values, count, &size);
    if (status != RECORD_OK) return status;
    uint8_t *bytes = malloc(size);
    if (bytes == NULL) return RECORD_NOMEM;
    bytes[0] = RECORD_FORMAT;
    size_t head = 1 + put_varint(bytes + 1, count);
    size_t body = head;
    for (size_t i = 0; i < count; i++) {
        uint64_t code;
        size_t payload;
        field_code(&values[i], &code, &payload);
        body += varint_size(code);
    }
    for (size_t i = 0; i < count; i++) {
        uint64_t code;
        size_t payload;
        field_code(&values[i], &code, &payload);
        head += put_varint(bytes + head, code);
        if (values[i].type == RECORD_INTEGER) {
            uint64_t bits = (uint64_t)values[i].integer;
            for (size_t k = 0; k < payload; k++) bytes[body + k] = (uint8_t)(bits >> (8 * k));
        } else if (payload > 0) {
            memcpy(bytes + body, values[i].bytes, payload);
        }
        body += payload;
    }
    *output = (record_buffer_t){bytes, size};
    return RECORD_OK;
}

static int64_t read_integer(const uint8_t *bytes, size_t width)
{
    uint64_t bits = 0;
    for (size_t k = 0; k < width; k++) bits |= (uint64_t)bytes[k] << (8 * k);
    if (width < 8 && (bits >> (8 * width - 1)) != 0) bits |= UINT64_MAX << (8 * width);
    if (bits <= INT64_MAX) return (int64_t)bits;
    return -1 - (int64_t)(UINT64_MAX - bits);
}

/* width바이트 정수가 최소 폭인가: 한 바이트 짧게는 담을 수 없고, 0·1이 아니다(0·1은 상수 코드) */
static bool minimal_integer(const uint8_t *bytes, size_t width)
{
    int64_t value = read_integer(bytes, width);
    if (width == 1) return value != 0 && value != 1;
    int64_t limit = (int64_t)1 << (8 * (width - 1) - 1);
    return value < -limit || value >= limit;
}

/* 코드 하나가 차지하는 값 바이트 수. 길이가 범위를 넘으면 false다. */
static bool payload_of(uint64_t code, size_t *payload)
{
    if (code < CODE_BYTES_BASE) {
        *payload = code > CODE_ONE ? (size_t)(code - CODE_INT_BASE) : 0;
        return true;
    }
    uint64_t length = (code - CODE_BYTES_BASE) / 2;
    if (length > UINT32_MAX) return false;
    *payload = (size_t)length;
    return true;
}

static size_t read_varint(const uint8_t *bytes, size_t available, uint64_t *value)
{
    if (available > 0 && bytes[0] < 0x80) {
        *value = bytes[0];
        return 1;
    }
    return get_varint(bytes, available, value);
}

record_status_t record_decode(const record_buffer_t *input, record_value_t *values,
                              size_t capacity, size_t *count)
{
    if (count == NULL) return RECORD_INVALID;
    if (input == NULL || input->bytes == NULL || input->size_bytes < 2) return RECORD_INVALID;
    if (input->size_bytes > UINT32_MAX) return RECORD_LIMIT;
    const uint8_t *bytes = input->bytes;
    size_t size = input->size_bytes;
    if (bytes[0] != RECORD_FORMAT) return RECORD_INVALID;
    uint64_t fields;
    size_t head = 1 + read_varint(bytes + 1, size - 1, &fields);
    if (head == 1) return RECORD_INVALID;
    if (fields > RECORD_MAX_FIELDS) return RECORD_LIMIT;

    /*
     * 머리를 한 번 읽어 코드와 값 바이트 수를 모으고, 합이 남은 길이와 정확히 같은지 본다. 상수가 아닌 정수의
     * 위치는 따로 적어 두어 values에 쓰기 전에 최소 폭인지 확인한다.
     */
    uint64_t codes[RECORD_MAX_FIELDS];
    uint32_t payloads[RECORD_MAX_FIELDS];
    uint16_t integers[RECORD_MAX_FIELDS];
    size_t integer_count = 0, total = 0;
    for (size_t i = 0; i < fields; i++) {
        size_t used = 1;
        if (head < size && bytes[head] < 0x80) codes[i] = bytes[head];
        else used = get_varint(bytes + head, size - head, &codes[i]);
        size_t payload;
        if (used == 0 || !payload_of(codes[i], &payload)) return RECORD_INVALID;
        if (codes[i] > CODE_ONE && codes[i] < CODE_BYTES_BASE) integers[integer_count++] = (uint16_t)i;
        payloads[i] = (uint32_t)payload;
        head += used;
        total += payload;
        if (total > size) return RECORD_INVALID;
    }
    if (total != size - head) return RECORD_INVALID;
    if (fields > capacity) return RECORD_LIMIT;
    if (fields > 0 && values == NULL) return RECORD_INVALID;

    size_t at = head, next = 0;
    for (size_t i = 0; next < integer_count; i++) {
        if (i == integers[next]) {
            if (!minimal_integer(bytes + at, payloads[i])) return RECORD_INVALID;
            next++;
        }
        at += payloads[i];
    }
    at = head;
    for (size_t i = 0; i < fields; i++) {
        uint64_t code = codes[i];
        size_t payload = payloads[i];
        record_value_t value = {0};
        if (code == CODE_NULL) {
            value.type = RECORD_NULL;
        } else if (code < CODE_BYTES_BASE) {
            value.type = RECORD_INTEGER;
            value.integer = code == CODE_ZERO ? 0 : code == CODE_ONE ? 1 : read_integer(bytes + at, payload);
        } else {
            value.type = (code - CODE_BYTES_BASE) % 2 ? RECORD_TEXT : RECORD_BLOB;
            value.size_bytes = payload;
            if (payload > 0) value.bytes = bytes + at;
        }
        values[i] = value;
        at += payload;
    }
    *count = (size_t)fields;
    return RECORD_OK;
}
