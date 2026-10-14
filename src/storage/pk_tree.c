#include <string.h>

#include "storage/pk_tree.h"

#define LEVEL_ANY (-1)

/* 가득 찬 노드에 항목 하나를 더 끼워 넣어도 넘치지 않도록 항목 하나만큼 여유를 둔다. */
#define NODE_BODY_BYTES (PAGE_BODY_BYTES + PK_TREE_LEAF_ENTRY_BYTES)

typedef struct {
    uint8_t body[NODE_BODY_BYTES];
    uint8_t level;
    uint32_t count;
    uint32_t stamp;
} node_t;

typedef struct {
    uint32_t page;       /* 바뀐 노드의 새 페이지 번호 */
    bool is_empty;       /* 항목이 모두 사라져 노드를 놓았다 */
    bool has_right;      /* 노드가 쪼개져 오른쪽 형제가 생겼다 */
    int64_t right_key;
    uint32_t right_page;
    uint8_t level;       /* 결과 노드의 level과 항목 수(root 접기 판단용) */
    uint32_t count;
} change_t;

/* ── 바이트 보조 ── */

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int64_t get_i64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return (int64_t)v;
}

static void put_i64(uint8_t *p, int64_t value)
{
    uint64_t v = (uint64_t)value;
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ── 노드 접근 ── */

static size_t entry_bytes(uint8_t level)
{
    return level == 0 ? PK_TREE_LEAF_ENTRY_BYTES : PK_TREE_NODE_ENTRY_BYTES;
}

static uint32_t capacity(uint8_t level)
{
    return level == 0 ? (uint32_t)PK_TREE_LEAF_CAPACITY : (uint32_t)PK_TREE_NODE_CAPACITY;
}

static uint8_t *entry_at(node_t *node, uint32_t index)
{
    return node->body + PK_TREE_HEADER_BYTES + (size_t)index * entry_bytes(node->level);
}

static int64_t key_at(const node_t *node, uint32_t index)
{
    return get_i64(node->body + PK_TREE_HEADER_BYTES + (size_t)index * entry_bytes(node->level));
}

static heap_locator_t locator_at(const node_t *node, uint32_t index)
{
    const uint8_t *entry = node->body + PK_TREE_HEADER_BYTES + (size_t)index * PK_TREE_LEAF_ENTRY_BYTES;
    return (heap_locator_t){get_u32(entry + 8),
                            (uint16_t)((uint16_t)entry[12] | (uint16_t)entry[13] << 8)};
}

static void set_leaf(node_t *node, uint32_t index, int64_t key, heap_locator_t locator)
{
    uint8_t *entry = entry_at(node, index);
    put_i64(entry, key);
    put_u32(entry + 8, locator.directory_index);
    entry[12] = (uint8_t)locator.slot;
    entry[13] = (uint8_t)(locator.slot >> 8);
    entry[14] = 0;
    entry[15] = 0;
}

static uint32_t child_at(const node_t *node, uint32_t index)
{
    return get_u32(node->body + PK_TREE_HEADER_BYTES + (size_t)index * PK_TREE_NODE_ENTRY_BYTES + 8);
}

static void set_child(node_t *node, uint32_t index, int64_t key, uint32_t child)
{
    uint8_t *entry = entry_at(node, index);
    put_i64(entry, key);
    put_u32(entry + 8, child);
}

static bool same_locator(heap_locator_t a, heap_locator_t b)
{
    return a.directory_index == b.directory_index && a.slot == b.slot;
}

/* 첫 번째로 key 이상인 leaf 항목의 위치. 같은 key가 있으면 *found가 true다. */
static uint32_t leaf_search(const node_t *node, int64_t key, bool *found)
{
    uint32_t low = 0, high = node->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (key_at(node, mid) < key) low = mid + 1;
        else high = mid;
    }
    *found = low < node->count && key_at(node, low) == key;
    return low;
}

/* key가 속한 child의 위치. 첫 항목의 key는 -inf로 본다. */
static uint32_t child_index(const node_t *node, int64_t key)
{
    uint32_t low = 1, high = node->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (key_at(node, mid) <= key) low = mid + 1;
        else high = mid;
    }
    return low - 1;
}

static void shift_entries(node_t *node, uint32_t from, uint32_t to)
{
    size_t size = entry_bytes(node->level);
    memmove(entry_at(node, to), entry_at(node, from), (size_t)(node->count - from) * size);
}

/* ── 읽기·쓰기 ── */

static bool node_valid(const node_t *node)
{
    uint32_t first = node->level == 0 ? 0 : 1;
    if (node->level > PK_TREE_MAX_LEVEL || node->count > capacity(node->level)) return false;
    if (node->level > 0 && node->count == 0) return false;
    for (uint32_t i = first + 1; i < node->count; i++) {
        if (key_at(node, i - 1) >= key_at(node, i)) return false;
    }
    return true;
}

static pk_status_t load_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page, int expected_level,
                             node_t *node)
{
    pk_status_t status = io->read(io->context, page, node->body);
    if (status != PK_OK) return status;
    if (memcmp(node->body, PK_TREE_MAGIC, 4) != 0 ||
        ((uint32_t)node->body[4] | (uint32_t)node->body[5] << 8) != PK_TREE_VERSION ||
        get_u32(node->body + 8) != table_id) {
        return PK_CORRUPT;
    }
    node->level = node->body[6];
    node->count = get_u32(node->body + 12);
    node->stamp = get_u32(node->body + 16);
    if (!node_valid(node) || (expected_level != LEVEL_ANY && node->level != expected_level)) return PK_CORRUPT;
    return PK_OK;
}

/*
 * 노드를 쓴다. old_page가 이 batch가 만든 페이지면 제자리에, 아니면 새 페이지에 쓰고 옛 페이지를 놓는다.
 * old_page가 PAGE_FILE_NO_PAGE면 새 노드다.
 */
static pk_status_t store_node(const pk_tree_io_t *io, uint32_t table_id, node_t *node, uint32_t old_page,
                              uint32_t *page)
{
    size_t used = PK_TREE_HEADER_BYTES + (size_t)node->count * entry_bytes(node->level);
    memset(node->body + used, 0, PAGE_BODY_BYTES - used);
    memset(node->body, 0, 8);
    memcpy(node->body, PK_TREE_MAGIC, 4);
    node->body[4] = (uint8_t)PK_TREE_VERSION;
    node->body[6] = node->level;
    put_u32(node->body + 8, table_id);
    put_u32(node->body + 12, node->count);
    put_u32(node->body + 16, node->stamp);
    memset(node->body + 20, 0, 4);

    uint32_t target = old_page;
    bool in_place = old_page != PAGE_FILE_NO_PAGE && io->is_fresh(io->context, old_page);
    if (!in_place) {
        pk_status_t allocated = io->allocate(io->context, &target);
        if (allocated != PK_OK) return allocated;
    }
    pk_status_t written = io->write(io->context, target, node->body);
    if (written != PK_OK) return written;
    if (!in_place && old_page != PAGE_FILE_NO_PAGE) {
        pk_status_t released = io->release(io->context, old_page);
        if (released != PK_OK) return released;
    }
    *page = target;
    return PK_OK;
}

static void describe(change_t *out, const node_t *node)
{
    out->level = node->level;
    out->count = node->count;
}

/* ── 만들기·stamp ── */

pk_status_t pk_tree_create(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root)
{
    node_t node;
    memset(&node, 0, sizeof(node));
    return store_node(io, table_id, &node, PAGE_FILE_NO_PAGE, root);
}

pk_status_t pk_tree_stamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, uint32_t *stamp)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, root, LEVEL_ANY, &node);
    if (status == PK_OK) *stamp = node.stamp;
    return status;
}

pk_status_t pk_tree_restamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, uint32_t stamp)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, *root, LEVEL_ANY, &node);
    if (status != PK_OK) return status;
    node.stamp = stamp;
    return store_node(io, table_id, &node, *root, root);
}

/* ── 조회 ── */

// cost: time O(d·log c), heap O(1), stack O(1), io d
// mem: stack ≈ size(node_t)
// vars: d = 트리 깊이, c = 노드당 항목 수
// basis: estimate
pk_status_t pk_tree_find(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, int64_t key,
                         heap_locator_t *locator)
{
    node_t node;
    int level = LEVEL_ANY;
    uint32_t page = root;
    for (;;) {
        pk_status_t status = load_node(io, table_id, page, level, &node);
        if (status != PK_OK) return status;
        if (node.level == 0) break;
        page = child_at(&node, child_index(&node, key));
        level = node.level - 1;
    }
    bool found;
    uint32_t index = leaf_search(&node, key, &found);
    if (!found) return PK_NOT_FOUND;
    *locator = locator_at(&node, index);
    return PK_OK;
}

// cost: time O(d), heap O(1), stack O(1), io d
// mem: stack ≈ size(node_t)
// vars: d = 트리 깊이
// basis: estimate
pk_status_t pk_tree_max(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, bool *present,
                        int64_t *key)
{
    node_t node;
    int level = LEVEL_ANY;
    uint32_t page = root;
    for (;;) {
        pk_status_t status = load_node(io, table_id, page, level, &node);
        if (status != PK_OK) return status;
        if (node.level == 0) break;
        page = child_at(&node, node.count - 1);
        level = node.level - 1;
    }
    *present = node.count > 0;
    if (node.count > 0) *key = key_at(&node, node.count - 1);
    return PK_OK;
}

/* ── 넣기 ── */

/* 가득 찬 노드(capacity + 1개)를 둘로 가른다. 맨 끝에 붙인 경우는 왼쪽을 가득 채워 순차 삽입의 낭비를 줄인다. */
static pk_status_t split_node(const pk_tree_io_t *io, uint32_t table_id, node_t *node, uint32_t old_page,
                              bool appended, change_t *out)
{
    node_t right;
    memset(&right, 0, sizeof(right));
    right.level = node->level;
    uint32_t keep = appended ? node->count - 1 : (node->count + 1) / 2;
    right.count = node->count - keep;
    memcpy(entry_at(&right, 0), entry_at(node, keep), (size_t)right.count * entry_bytes(node->level));
    out->right_key = key_at(&right, 0);
    node->count = keep;
    pk_status_t status = store_node(io, table_id, node, old_page, &out->page);
    if (status != PK_OK) return status;
    status = store_node(io, table_id, &right, PAGE_FILE_NO_PAGE, &out->right_page);
    if (status != PK_OK) return status;
    out->has_right = true;
    return PK_OK;
}

static pk_status_t save_grown(const pk_tree_io_t *io, uint32_t table_id, node_t *node, uint32_t page,
                              bool appended, change_t *out)
{
    pk_status_t status;
    if (node->count > capacity(node->level)) status = split_node(io, table_id, node, page, appended, out);
    else status = store_node(io, table_id, node, page, &out->page);
    describe(out, node);
    return status;
}

static pk_status_t put_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page, int expected_level,
                            int64_t key, heap_locator_t locator, bool replace, heap_locator_t *previous,
                            change_t *out)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    memset(out, 0, sizeof(*out));
    out->page = page;
    describe(out, &node);

    if (node.level == 0) {
        bool found;
        uint32_t index = leaf_search(&node, key, &found);
        if (found) {
            heap_locator_t existing = locator_at(&node, index);
            if (previous != NULL) *previous = existing;
            if (!replace) return PK_EXISTS;
            if (same_locator(existing, locator)) return PK_OK;
            set_leaf(&node, index, key, locator);
            return store_node(io, table_id, &node, page, &out->page);
        }
        shift_entries(&node, index, index + 1);
        set_leaf(&node, index, key, locator);
        node.count++;
        return save_grown(io, table_id, &node, page, index + 1 == node.count, out);
    }

    uint32_t slot = child_index(&node, key);
    change_t below;
    status = put_node(io, table_id, child_at(&node, slot), node.level - 1, key, locator, replace, previous,
                      &below);
    if (status != PK_OK) return status;
    if (!below.has_right && below.page == child_at(&node, slot)) return PK_OK;
    set_child(&node, slot, slot == 0 ? INT64_MIN : key_at(&node, slot), below.page);
    if (!below.has_right) return store_node(io, table_id, &node, page, &out->page);
    shift_entries(&node, slot + 1, slot + 2);
    set_child(&node, slot + 1, below.right_key, below.right_page);
    node.count++;
    return save_grown(io, table_id, &node, page, slot + 2 == node.count, out);
}

// cost: time O(d·c), heap O(1), stack O(d), io O(d)
// mem: stack ≈ 2·d·size(node_t)
// vars: d = 트리 깊이, c = 노드당 항목 수(항목 이동)
// basis: estimate
pk_status_t pk_tree_put(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, int64_t key,
                        heap_locator_t locator, bool replace, heap_locator_t *previous)
{
    change_t out;
    pk_status_t status = put_node(io, table_id, *root, LEVEL_ANY, key, locator, replace, previous, &out);
    if (status != PK_OK) return status;
    if (!out.has_right) {
        *root = out.page;
        return PK_OK;
    }
    node_t top;
    memset(&top, 0, sizeof(top));
    top.level = (uint8_t)(out.level + 1);
    if (top.level > PK_TREE_MAX_LEVEL) return PK_CORRUPT;
    top.count = 2;
    set_child(&top, 0, INT64_MIN, out.page);
    set_child(&top, 1, out.right_key, out.right_page);
    return store_node(io, table_id, &top, PAGE_FILE_NO_PAGE, root);
}

/* ── 지우기 ── */

static pk_status_t remove_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page, int expected_level,
                               int64_t key, heap_locator_t *removed, change_t *out)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    memset(out, 0, sizeof(*out));
    out->page = page;

    if (node.level == 0) {
        bool found;
        uint32_t index = leaf_search(&node, key, &found);
        if (!found) return PK_NOT_FOUND;
        *removed = locator_at(&node, index);
        shift_entries(&node, index + 1, index);
        node.count--;
    } else {
        uint32_t slot = child_index(&node, key);
        change_t below;
        status = remove_node(io, table_id, child_at(&node, slot), node.level - 1, key, removed, &below);
        if (status != PK_OK) return status;
        if (!below.is_empty && below.page == child_at(&node, slot)) {
            describe(out, &node);
            return PK_OK;
        }
        if (below.is_empty) {
            shift_entries(&node, slot + 1, slot);
            node.count--;
        } else {
            set_child(&node, slot, slot == 0 ? INT64_MIN : key_at(&node, slot), below.page);
        }
    }
    describe(out, &node);
    if (node.count > 0) return store_node(io, table_id, &node, page, &out->page);
    out->is_empty = true;
    return io->release(io->context, page);
}

/* root가 child 하나뿐인 internal 노드이면 child를 root로 올린다. */
static pk_status_t collapse_root(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, change_t *top)
{
    while (top->level > 0 && top->count == 1) {
        node_t node;
        pk_status_t status = load_node(io, table_id, *root, LEVEL_ANY, &node);
        if (status != PK_OK) return status;
        uint32_t child = child_at(&node, 0);
        status = io->release(io->context, *root);
        if (status != PK_OK) return status;
        *root = child;
        status = load_node(io, table_id, child, node.level - 1, &node);
        if (status != PK_OK) return status;
        describe(top, &node);
    }
    return PK_OK;
}

// cost: time O(d·c), heap O(1), stack O(d), io O(d)
// mem: stack ≈ d·size(node_t)
// vars: d = 트리 깊이, c = 노드당 항목 수(항목 이동)
// basis: estimate
pk_status_t pk_tree_remove(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, int64_t key,
                           heap_locator_t *removed)
{
    change_t out;
    pk_status_t status = remove_node(io, table_id, *root, LEVEL_ANY, key, removed, &out);
    if (status != PK_OK) return status;
    if (out.is_empty) return pk_tree_create(io, table_id, root);
    *root = out.page;
    return collapse_root(io, table_id, root, &out);
}

/* ── 순회 ── */

static pk_status_t visit_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page, int expected_level,
                              bool (*visit)(void *context, uint32_t page_id), void *context)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    if (!visit(context, page)) return PK_NOMEM;
    for (uint32_t i = 0; node.level > 0 && i < node.count; i++) {
        status = visit_node(io, table_id, child_at(&node, i), node.level - 1, visit, context);
        if (status != PK_OK) return status;
    }
    return PK_OK;
}

// cost: time O(n·c), heap O(1), stack O(d), io n
// mem: stack ≈ d·size(node_t)
// vars: n = 노드 수, d = 트리 깊이, c = 노드당 항목 수(순서 검사)
// basis: estimate
pk_status_t pk_tree_visit_pages(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                                bool (*visit)(void *context, uint32_t page_id), void *context)
{
    return visit_node(io, table_id, root, LEVEL_ANY, visit, context);
}
