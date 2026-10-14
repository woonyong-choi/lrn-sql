/*
 * schema.h — 행 직렬화/역직렬화 및 스키마 레이아웃 인터페이스
 *
 * 컬럼 메타데이터를 기반으로 행의 바이트 오프셋을 계산하고,
 * row_value_t ↔ uint8_t[] 간 변환 함수를 제공한다.
 */

#ifndef SCHEMA_H
#define SCHEMA_H

#include "page_format.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 행 값 공용체.
 * 하나의 컬럼 값을 타입별로 저장한다.
 * 메모리 내에서 행 데이터를 다룰 때 사용된다.
 */
typedef union {
    int32_t  int_val;       /* INT 타입 값 (4바이트) */
    int64_t  bigint_val;    /* BIGINT 타입 값 (8바이트) */
    char     str_val[256];  /* VARCHAR 타입 값 (최대 255자 + null) */
} row_value_t;

/* 컬럼 오프셋을 계산하고 row_size를 헤더에 기록한다 */
uint16_t schema_compute_layout(db_header_t *hdr);

/*
 * 파일에서 읽은 헤더의 스키마가 사용해도 안전한지 검사한다. 값을 고치지 않는다.
 * 안전하면 true. 아니면 false이고 why에 이유(정적 문자열)를 돌려준다.
 *   - column_count는 MAX_COLUMNS 이하, 0이면 row_size도 0
 *   - 컬럼 이름은 NUL로 끝나고 비어 있지 않음
 *   - type은 INT(4바이트)·BIGINT(8바이트)·VARCHAR(1~255바이트)이며 size가 type과 맞음
 *   - offset은 앞 컬럼 size의 누적합, row_size는 전체 합
 *   - 슬롯 하나와 행이 힙 페이지 하나에 들어감
 *   - v2의 table_name은 NUL로 끝남
 */
bool schema_validate_header(const db_header_t *hdr, uint32_t page_size, const char **why);

/* 행 직렬화: row_value_t 배열 → 바이트 버퍼 */
void row_serialize(const db_header_t *hdr, const row_value_t *values, uint8_t *buf);

/* 행 역직렬화: 바이트 버퍼 → row_value_t 배열 */
void row_deserialize(const db_header_t *hdr, const uint8_t *buf, row_value_t *values);

#endif /* SCHEMA_H */
