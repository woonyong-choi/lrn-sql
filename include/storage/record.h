#ifndef RECORD_H
#define RECORD_H

#include <stddef.h>
#include <stdint.h>

#define RECORD_MAX_FIELDS 1024

typedef enum {
    RECORD_OK,
    RECORD_INVALID,
    RECORD_LIMIT,
    RECORD_NOMEM
} record_status_t;

typedef enum {
    RECORD_NULL,
    RECORD_INTEGER,
    RECORD_TEXT,
    RECORD_BLOB
} record_type_t;

typedef struct {
    record_type_t type;
    int64_t integer;
    const uint8_t *bytes;
    size_t size_bytes;
} record_value_t;

typedef struct {
    uint8_t *bytes;
    size_t size_bytes;
} record_buffer_t;

/* 성공한 buffer는 호출자가 free하며 실패 시 출력은 변경하지 않는다. */
record_status_t record_encode(const record_value_t *values, size_t count,
                              record_buffer_t *output);
/* bytes 값은 입력 buffer를 빌리며 실패 시 values와 count는 변경하지 않는다. */
record_status_t record_decode(const record_buffer_t *input, record_value_t *values,
                              size_t capacity, size_t *count);

#endif
