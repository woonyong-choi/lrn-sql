#include <stdlib.h>
#include <string.h>

#include "storage/record.h"
#include "storage/le.h"

#define RECORD_HEADER_BYTES 8u
#define RECORD_FIELD_BYTES 5u

static record_status_t value_size(const record_value_t *value, size_t *size)
{
    switch (value->type) {
        case RECORD_NULL: *size = 0; return RECORD_OK;
        case RECORD_INTEGER: *size = 8; return RECORD_OK;
        case RECORD_TEXT:
        case RECORD_BLOB:
            if (value->size_bytes > UINT32_MAX) return RECORD_LIMIT;
            if (value->size_bytes > 0 && value->bytes == NULL) return RECORD_INVALID;
            *size = value->size_bytes;
            return RECORD_OK;
        default: return RECORD_INVALID;
    }
}

static record_status_t encoded_size(const record_value_t *values, size_t count,
                                    size_t *size)
{
    if (count > RECORD_MAX_FIELDS) return RECORD_LIMIT;
    if (count > 0 && values == NULL) return RECORD_INVALID;
    size_t total = RECORD_HEADER_BYTES;
    for (size_t i = 0; i < count; i++) {
        size_t length;
        record_status_t status = value_size(&values[i], &length);
        if (status != RECORD_OK) return status;
        if (length > UINT32_MAX - RECORD_FIELD_BYTES) return RECORD_LIMIT;
        size_t addition = RECORD_FIELD_BYTES + length;
        if (total > UINT32_MAX - addition) return RECORD_LIMIT;
        total += addition;
    }
    *size = total;
    return RECORD_OK;
}

static void encode_value(uint8_t *bytes, const record_value_t *value, size_t length)
{
    bytes[0] = (uint8_t)value->type;
    le_put_u32(bytes + 1, (uint32_t)length);
    if (value->type == RECORD_INTEGER) {
        uint64_t bits = (uint64_t)value->integer;
        le_put_u64(bytes + 5, bits);
    } else if (length > 0) {
        memcpy(bytes + RECORD_FIELD_BYTES, value->bytes, length);
    }
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
    memcpy(bytes, "LRC1", 4);
    le_put_u32(bytes + 4, (uint32_t)count);
    size_t offset = RECORD_HEADER_BYTES;
    for (size_t i = 0; i < count; i++) {
        size_t length = 0;
        value_size(&values[i], &length);
        encode_value(bytes + offset, &values[i], length);
        offset += RECORD_FIELD_BYTES + length;
    }
    *output = (record_buffer_t){bytes, size};
    return RECORD_OK;
}

static record_status_t validate_record(const record_buffer_t *input, size_t *count)
{
    if (input == NULL || input->bytes == NULL) return RECORD_INVALID;
    if (input->size_bytes < RECORD_HEADER_BYTES) return RECORD_INVALID;
    if (input->size_bytes > UINT32_MAX) return RECORD_LIMIT;
    if (memcmp(input->bytes, "LRC1", 4) != 0) return RECORD_INVALID;
    size_t fields = le_get_u32(input->bytes + 4);
    if (fields > RECORD_MAX_FIELDS) return RECORD_LIMIT;
    size_t offset = RECORD_HEADER_BYTES;
    for (size_t i = 0; i < fields; i++) {
        if (input->size_bytes - offset < RECORD_FIELD_BYTES) return RECORD_INVALID;
        unsigned type = input->bytes[offset];
        size_t length = le_get_u32(input->bytes + offset + 1);
        offset += RECORD_FIELD_BYTES;
        if (length > input->size_bytes - offset) return RECORD_INVALID;
        if (type > RECORD_BLOB) return RECORD_INVALID;
        if (type == RECORD_NULL && length != 0) return RECORD_INVALID;
        if (type == RECORD_INTEGER && length != 8) return RECORD_INVALID;
        offset += length;
    }
    if (offset != input->size_bytes) return RECORD_INVALID;
    *count = fields;
    return RECORD_OK;
}

static int64_t decode_integer(const uint8_t *bytes)
{
    uint64_t bits = le_get_u64(bytes);
    if (bits <= INT64_MAX) return (int64_t)bits;
    return -1 - (int64_t)(UINT64_MAX - bits);
}

record_status_t record_decode(const record_buffer_t *input, record_value_t *values,
                              size_t capacity, size_t *count)
{
    if (count == NULL) return RECORD_INVALID;
    size_t fields;
    record_status_t status = validate_record(input, &fields);
    if (status != RECORD_OK) return status;
    if (fields > capacity) return RECORD_LIMIT;
    if (fields > 0 && values == NULL) return RECORD_INVALID;
    size_t offset = RECORD_HEADER_BYTES;
    for (size_t i = 0; i < fields; i++) {
        record_value_t value = {0};
        value.type = (record_type_t)input->bytes[offset];
        value.size_bytes = le_get_u32(input->bytes + offset + 1);
        offset += RECORD_FIELD_BYTES;
        if (value.type == RECORD_INTEGER) {
            value.integer = decode_integer(input->bytes + offset);
        } else if (value.type != RECORD_NULL) {
            value.bytes = input->bytes + offset;
        }
        values[i] = value;
        offset += value.size_bytes;
    }
    *count = fields;
    return RECORD_OK;
}
