/*
 * schema.c — 행(row) 직렬화/역직렬화 및 스키마 레이아웃 계산
 *
 * 역할:
 *   column_meta_t 배열을 기반으로 각 컬럼의 바이트 오프셋을 계산하고,
 *   row_value_t ↔ uint8_t[] 변환을 수행한다.
 *
 * 행 바이트 레이아웃 예시:
 *   CREATE TABLE users (name VARCHAR(32), age INT)
 *   → 시스템이 id BIGINT를 자동 추가
 *
 *   columns = [id BIGINT(8), name VARCHAR(32), age INT(4)]
 *   offsets = [0, 8, 40]
 *   row_size = 44바이트
 *
 *   직렬화된 행 (44바이트 버퍼):
 *   [8바이트: id][32바이트: name (남는 공간은 0)][4바이트: age]
 *   |offset=0  ||offset=8                       ||offset=40  |
 */

#include "storage/schema.h"
#include <string.h>

/*
 * schema_compute_layout - 컬럼 레이아웃을 계산한다.
 *
 * 각 컬럼의 offset을 순서대로 누적하고, 전체 row_size를 헤더에 기록한다.
 *
 * 예시: columns = [{size=8}, {size=32}, {size=4}]
 *   columns[0].offset = 0   (0부터 시작)
 *   columns[1].offset = 8   (0 + 8)
 *   columns[2].offset = 40  (8 + 32)
 *   row_size = 44            (40 + 4)
 *
 * 반환값 = row_size (한 행의 총 바이트 수)
 */
uint16_t schema_compute_layout(db_header_t *hdr)
{
    uint16_t offset = 0;
    for (uint16_t i = 0; i < hdr->column_count; i++)
    {
        hdr->columns[i].offset = offset;
        offset += hdr->columns[i].size;
    }
    hdr->row_size = offset;
    return offset;
}

/*
 * row_serialize - row_value_t 배열을 바이트 버퍼로 직렬화한다.
 *
 * 각 컬럼 타입에 따라 바이트 버퍼의 해당 오프셋에 값을 복사한다.
 *
 * 예시: values = [id=1, name="Alice", age=25], row_size=44
 *   buf 초기: [00 00 00 ... (44바이트 전부 0)]
 *
 *   INT (4바이트):     memcpy(buf+40, &25, 4)
 *   BIGINT (8바이트):  memcpy(buf+0, &1, 8)
 *   VARCHAR (32바이트): memcpy(buf+8, "Alice", 5)
 *     → 나머지 27바이트는 memset으로 이미 0 (null 패딩)
 *
 *   결과 buf:
 *   [01 00 00 00 00 00 00 00][Alice\0\0\0...][19 00 00 00]
 *   |____ id=1 (LE) ________||___ name _____||__ age=25 __|
 */
void row_serialize(const db_header_t *hdr, const row_value_t *values, uint8_t *buf)
{
    memset(buf, 0, hdr->row_size);
    for (uint16_t i = 0; i < hdr->column_count; i++)
    {
        const column_meta_t *col = &hdr->columns[i];
        uint8_t *dst = buf + col->offset;
        switch (col->type)
        {
            case COL_TYPE_INT:
            {
                /* 4바이트 리틀 엔디안으로 기록 */
                int32_t v = values[i].int_val;
                memcpy(dst, &v, 4);
                break;
            }
            case COL_TYPE_BIGINT:
            {
                /* 8바이트 리틀 엔디안으로 기록 */
                int64_t v = values[i].bigint_val;
                memcpy(dst, &v, 8);
                break;
            }
            case COL_TYPE_VARCHAR:
            {
                /*
                 * col->size - 1 바이트까지 복사 (null 종단 보장)
                 * 예: VARCHAR(32)에 "Alice"(5자) 저장
                 *   → 5바이트 복사, 나머지 27바이트는 0 (memset으로 이미 초기화됨)
                 */
                size_t len = strlen(values[i].str_val);
                if (len >= col->size)
                {
                    len = col->size - 1;
                }
                memcpy(dst, values[i].str_val, len);
                break;
            }
        }
    }
}

/*
 * row_deserialize - 바이트 버퍼를 row_value_t 배열로 역직렬화한다.
 *
 * row_serialize의 역연산이다.
 *
 * 예시: buf의 offset=8 위치에서 VARCHAR(32) 읽기
 *   memcpy(values[1].str_val, buf+8, 32)
 *   values[1].str_val[32] = '\0'  ← 안전한 null 종단
 *   → values[1].str_val = "Alice"
 */
void row_deserialize(const db_header_t *hdr, const uint8_t *buf, row_value_t *values)
{
    memset(values, 0, sizeof(row_value_t) * hdr->column_count);
    for (uint16_t i = 0; i < hdr->column_count; i++)
    {
        const column_meta_t *col = &hdr->columns[i];
        const uint8_t *src = buf + col->offset;
        switch (col->type)
        {
            case COL_TYPE_INT:
            {
                memcpy(&values[i].int_val, src, 4);
                break;
            }
            case COL_TYPE_BIGINT:
            {
                memcpy(&values[i].bigint_val, src, 8);
                break;
            }
            case COL_TYPE_VARCHAR:
            {
                memcpy(values[i].str_val, src, col->size);
                values[i].str_val[col->size] = '\0';
                break;
            }
        }
    }
}
