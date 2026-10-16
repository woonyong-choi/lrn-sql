#ifndef PK_TREE_H
#define PK_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/heap_store.h"
#include "storage/page_file.h"

/*
 * 정수 기본 키 -> heap locator의 지속형 COW B+Tree. 페이지 입출력과 할당은 호출자가 주는 pk_tree_io_t로
 * 하므로 이 모듈은 메모리를 할당하지 않고 page_file·commit도 부르지 않는다(heap batch가 io를 제공해
 * 색인 페이지가 행 페이지와 같은 fresh·old 목록으로 게시된다).
 *
 * 노드 본문(little-endian, PAGE_KIND_INDEX, 8160바이트):
 *   0..3   magic "LPK1"
 *   4..5   version u16 = 1
 *   6      level u8 (0이 leaf)
 *   8..11  table ID u32
 *   12..15 항목 수 u32
 *   16..19 stamp u32. root에서만 뜻이 있다: 이 색인이 가리키는 heap 디렉터리 root 번호.
 *          heap과 다른 root를 가리키면 색인이 그 heap의 것이 아니므로 열 때 쓰지 않는다.
 *   24..   leaf 항목 16바이트(key i64, 디렉터리 순번 u32, slot u16, 0 u16), internal 항목 12바이트(그 child
 *          부분 트리의 하한 key i64, child 페이지 u32). internal의 첫 항목 key는 쓰지 않는다(-inf).
 * leaf·internal 모두 key가 엄격히 증가해야 하며 child의 level은 부모보다 정확히 1 작아야 한다. 어긋나면
 * PK_CORRUPT다. 삭제는 비어 버린 노드만 부모에서 떼며 합치지 않는다(빈 공간은 같은 범위에 다시 쓸 때 쓴다).
 *
 * COW: 바뀌는 노드는 새 페이지에 쓰고 옛 페이지는 io->release에 넘긴다. 이 batch가 이미 만든 페이지는
 * io->is_fresh가 알려 주며 제자리에 다시 쓴다(어느 commit된 상태도 가리키지 않는다). 연산 도중 실패하면
 * root는 호출자에게 돌려주지 않으므로 호출자는 batch 전체를 버려야 한다.
 */

#define PK_TREE_MAGIC "LPK1"
#define PK_TREE_VERSION 1u
#define PK_TREE_HEADER_BYTES 24u
#define PK_TREE_LEAF_ENTRY_BYTES 16u
#define PK_TREE_NODE_ENTRY_BYTES 12u
#define PK_TREE_LEAF_CAPACITY ((PAGE_BODY_BYTES - PK_TREE_HEADER_BYTES) / PK_TREE_LEAF_ENTRY_BYTES)
#define PK_TREE_NODE_CAPACITY ((PAGE_BODY_BYTES - PK_TREE_HEADER_BYTES) / PK_TREE_NODE_ENTRY_BYTES)
#define PK_TREE_MAX_LEVEL 8u

typedef enum {
    PK_OK,
    PK_NOT_FOUND,
    PK_EXISTS,
    PK_CORRUPT,
    PK_IO,    /* io 호출이 실패했다. 원인은 io를 준 쪽이 안다 */
    PK_NOMEM, /* io 호출이 메모리 부족으로 실패했다 */
    PK_LIMIT  /* 키 또는 트리 깊이의 형식 한계 */
} pk_status_t;

/* 쓰기 연산이 아닌 조회·순회에는 allocate·release·is_fresh·write가 NULL이어도 된다. */
typedef struct {
    void *context;
    pk_status_t (*read)(void *context, uint32_t page_id, uint8_t body[PAGE_BODY_BYTES]);
    pk_status_t (*write)(void *context, uint32_t page_id, const uint8_t body[PAGE_BODY_BYTES]);
    pk_status_t (*allocate)(void *context, uint32_t *page_id);
    pk_status_t (*release)(void *context, uint32_t page_id);
    bool (*is_fresh)(void *context, uint32_t page_id);
} pk_tree_io_t;

/* 빈 leaf 하나를 만들어 root로 돌려준다. */
pk_status_t pk_tree_create(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root);
/* root 페이지의 stamp를 읽는다. 형식이 맞지 않으면 PK_CORRUPT다. */
pk_status_t pk_tree_stamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, uint32_t *stamp);
/* root를 stamp로 다시 쓴다. root가 바뀔 수 있다. */
pk_status_t pk_tree_restamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, uint32_t stamp);

pk_status_t pk_tree_find(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, int64_t key,
                         heap_locator_t *locator);
/* 가장 큰 key. 비어 있으면 *present가 false다. */
pk_status_t pk_tree_max(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, bool *present,
                        int64_t *key);

/*
 * key를 넣는다. 이미 있으면 replace일 때만 값을 바꾸고(*previous에 옛 값) 아니면 PK_EXISTS다.
 * previous는 NULL이어도 된다. 없던 key면 *previous는 건드리지 않는다.
 */
pk_status_t pk_tree_put(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, int64_t key,
                        heap_locator_t locator, bool replace, heap_locator_t *previous);
/* key를 지운다. 없으면 PK_NOT_FOUND이며 *removed는 지운 값이다. */
pk_status_t pk_tree_remove(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, int64_t key,
                           heap_locator_t *removed);

/*
 * low 이상 high 이하인 key를 오름차순으로 visit에 넘긴다. 범위와 겹치는 child만 내려가므로 읽는 페이지는
 * 깊이 + 범위에 걸친 잎 수에 비례한다. visit이 false를 돌려주면 그 자리에서 멈추고 *stopped를 true로 둔다.
 */
pk_status_t pk_tree_visit_range(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, int64_t low,
                                int64_t high, bool (*visit)(void *context, int64_t key, heap_locator_t locator),
                                void *context, bool *stopped);

/* 모든 노드 페이지를 visit에 넘긴다. visit이 false를 돌려주면 PK_NOMEM으로 멈춘다. */
pk_status_t pk_tree_visit_pages(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                                bool (*visit)(void *context, uint32_t page_id), void *context);

#endif
