/*
 * bptree.h — 온디스크 B+ tree 인덱스 인터페이스
 *
 * id(uint64_t) → row_ref_t 매핑을 B+ tree로 관리한다.
 * 검색, 삽입, 삭제 모두 O(log N) 시간에 수행된다.
 */

#ifndef BPTREE_H
#define BPTREE_H

#include "pager.h"
#include "page_format.h"
#include <stdbool.h>

/* 삽입·삭제·범위 스캔이 페이지 읽기·할당 실패로 끝났음을 나타내는 반환값 */
#define BPTREE_ERR_IO (-2)

/*
 * 키로 검색하여 행 위치를 반환한다. 찾으면 true, 없으면 false.
 * 페이지 읽기에 실패해도 false다. "키 없음"과 구분하려면 호출 전후의
 * pager_io_error_seq()를 비교한다.
 */
bool bptree_search(pager_t *pager, uint64_t key, row_ref_t *out_ref);

/*
 * 범위 스캔 콜백. 오름차순 키 순서로 (key, row_ref)를 전달한다.
 * false를 반환하면 스캔을 즉시 중단한다.
 * 주의: 콜백 실행 중 리프 페이지의 읽기 래치가 걸려 있으므로,
 *       콜백 안에서 힙 fetch 등 다른 페이지 래치를 잡지 말 것
 *       (row_ref만 복사해 두고, 스캔 종료 후 힙을 읽어야 한다).
 */
typedef bool (*bptree_range_cb)(uint64_t key, row_ref_t ref, void *ctx);

/*
 * B+ tree 리프 순차 순회로 범위 스캔을 수행한다.
 * has_lo/has_hi로 하한/상한 존재 여부를, *_inclusive로 경계 포함 여부를 지정한다.
 * 반환값: 0 = 완료 또는 콜백이 중단, BPTREE_ERR_IO = 페이지 읽기 실패로 순회가 잘림.
 * 하한이 있으면 해당 키가 있는 리프까지 O(log N)로 하강한 뒤,
 * next_leaf_page_id 형제 포인터를 따라 오름차순으로 순회한다.
 */
int bptree_range_scan(pager_t *pager,
                       bool has_lo, uint64_t lo, bool lo_inclusive,
                       bool has_hi, uint64_t hi, bool hi_inclusive,
                       bptree_range_cb cb, void *ctx);

/*
 * 키-행 위치 쌍을 삽입한다. 성공 시 0, 중복 키 시 -1, I/O 실패 시 BPTREE_ERR_IO.
 * 분할을 시작한 뒤 실패하면 되돌릴 수 없어 pager가 poison된다.
 */
int bptree_insert(pager_t *pager, uint64_t key, row_ref_t ref);

/*
 * 키를 삭제한다. 성공 시 0, 키 없음 시 -1, I/O 실패 시 BPTREE_ERR_IO.
 * 재균형 도중 실패하면 되돌릴 수 없어 pager가 poison된다.
 */
int bptree_delete(pager_t *pager, uint64_t key);

/* B+ tree 구조를 표준 출력에 출력한다 (디버그용) */
void bptree_print(pager_t *pager);

/* 트리의 높이를 반환한다 (리프만 있으면 1). 페이지 읽기 실패 시 -1 */
int bptree_height(pager_t *pager);

#endif /* BPTREE_H */
