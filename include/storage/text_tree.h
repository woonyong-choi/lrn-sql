#ifndef TEXT_TREE_H
#define TEXT_TREE_H

#include "storage/pk_tree.h"

/*
 * 바이트 키 -> heap locator의 지속형 COW B+Tree. TEXT 기본 키와 보조 색인이 쓴다(보조 색인 키는 값 뒤에 기본 키나
 * 위치를 붙인 꼬리를 단다). 노드 슬롯과 항목은 실제 키 바이트를 담고 키는 memcmp 순서로 정렬한다.
 * 키는 TEXT_TREE_KEY_MAX_BYTES 이하다. 페이지 입출력·할당·COW 규칙은 pk_tree.h와 같다.
 */
#define TEXT_TREE_MAGIC "LTX1"
#define TEXT_TREE_VERSION 1u
#define TEXT_TREE_KEY_MAX_BYTES 1024u
#define TEXT_TREE_HEADER_BYTES 24u

pk_status_t text_tree_create(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root);
pk_status_t text_tree_stamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, uint32_t *stamp);
pk_status_t text_tree_restamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, uint32_t stamp);
pk_status_t text_tree_find(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                           const uint8_t *key, size_t key_bytes, heap_locator_t *locator);
pk_status_t text_tree_put(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root,
                          const uint8_t *key, size_t key_bytes, heap_locator_t locator,
                          bool replace, heap_locator_t *previous);
pk_status_t text_tree_remove(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root,
                             const uint8_t *key, size_t key_bytes, heap_locator_t *removed);
pk_status_t text_tree_visit_pages(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                                  bool (*visit)(void *context, uint32_t page_id), void *context);
pk_status_t text_tree_visit_prefix(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                                   const uint8_t *prefix, size_t prefix_bytes,
                                   bool (*visit)(void *context, heap_locator_t locator), void *context);

#endif
