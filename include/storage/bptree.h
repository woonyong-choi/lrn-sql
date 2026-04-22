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

/* 키로 검색하여 행 위치를 반환한다. 찾으면 true, 없으면 false */
bool bptree_search(pager_t *pager, uint64_t key, row_ref_t *out_ref);

/* 키-행 위치 쌍을 삽입한다. 성공 시 0, 중복 키 시 -1 */
int bptree_insert(pager_t *pager, uint64_t key, row_ref_t ref);

/* 키를 삭제한다. 성공 시 0, 키 없음 시 -1 */
int bptree_delete(pager_t *pager, uint64_t key);

/* B+ tree 구조를 표준 출력에 출력한다 (디버그용) */
void bptree_print(pager_t *pager);

/* 트리의 높이를 반환한다 (리프만 있으면 1) */
int bptree_height(pager_t *pager);

#endif /* BPTREE_H */
