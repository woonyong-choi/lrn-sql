/*
 * table.h — 슬롯 기반 힙 테이블 인터페이스
 *
 * 행 데이터를 힙 페이지에 저장/조회/삭제/스캔하는 함수를 제공한다.
 * 모든 행 위치는 row_ref_t(page_id + slot_id)로 식별된다.
 */

#ifndef TABLE_H
#define TABLE_H

#include "pager.h"
#include "page_format.h"
#include <stdint.h>
#include <stdbool.h>

/* 행을 힙에 삽입하고, 저장된 위치(row_ref_t)를 반환한다 */
row_ref_t heap_insert(pager_t *pager, const uint8_t *row_data, uint16_t row_size);

/*
 * row_ref_t로 행을 조회한다.
 * 반환된 포인터는 캐시 페이지 내부를 가리키므로,
 * 사용 후 반드시 pager_unpin(pager, ref.page_id)을 호출해야 한다.
 */
const uint8_t *heap_fetch(pager_t *pager, row_ref_t ref, uint16_t row_size);

/* 슬롯을 FREE로 표시하여 행을 삭제한다 (톰스톤 방식) */
int heap_delete(pager_t *pager, row_ref_t ref);

/*
 * 스캔 콜백 타입.
 * false를 반환하면 스캔을 즉시 중단한다.
 */
typedef bool (*scan_cb)(const uint8_t *row_data, row_ref_t ref, void *ctx);

/* 모든 힙 페이지를 순회하며 살아 있는 행에 대해 콜백을 호출한다 */
void heap_scan(pager_t *pager, uint16_t row_size, scan_cb cb, void *ctx);

#endif /* TABLE_H */
