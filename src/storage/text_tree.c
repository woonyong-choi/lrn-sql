#include <string.h>

#include "storage/text_tree.h"

#define LEVEL_ANY (-1)
#define ENTRY_FIXED_BYTES 8u
#define MAX_ENTRIES ((PAGE_BODY_BYTES - TEXT_TREE_HEADER_BYTES) / (ENTRY_FIXED_BYTES + 2u))

typedef struct {
    uint8_t body[PAGE_BODY_BYTES];
    uint8_t level;
    uint32_t count;
    uint32_t stamp;
} node_t;

typedef struct {
    const uint8_t *bytes;
    uint16_t size;
} text_key_t;

typedef struct {
    text_key_t key;
    uint32_t target;
    uint16_t slot;
} entry_t;

typedef struct {
    uint32_t page;
    bool is_empty;
    bool has_right;
    uint8_t right_key[TEXT_TREE_KEY_MAX_BYTES];
    uint16_t right_size;
    uint32_t right_page;
    uint8_t level;
    uint32_t count;
} change_t;

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static int compare_keys(text_key_t left, text_key_t right)
{
    size_t common = left.size < right.size ? left.size : right.size;
    int compared = common == 0 ? 0 : memcmp(left.bytes, right.bytes, common);
    if (compared != 0) return compared;
    return (left.size > right.size) - (left.size < right.size);
}

static size_t entry_size(entry_t entry)
{
    return ENTRY_FIXED_BYTES + entry.key.size;
}

static entry_t entry_at(const node_t *node, uint32_t index)
{
    uint16_t offset = get_u16(node->body + TEXT_TREE_HEADER_BYTES + 2u * index);
    const uint8_t *at = node->body + offset;
    uint16_t size = get_u16(at);
    return (entry_t){{at + 2, size}, get_u32(at + 2 + size), get_u16(at + 6 + size)};
}

static bool node_valid(const node_t *node)
{
    if (node->level > PK_TREE_MAX_LEVEL || node->count > MAX_ENTRIES) return false;
    if (node->level > 0 && node->count == 0) return false;
    size_t cursor = PAGE_BODY_BYTES;
    for (uint32_t i = 0; i < node->count; i++) {
        uint16_t offset = get_u16(node->body + TEXT_TREE_HEADER_BYTES + 2u * i);
        if (cursor < ENTRY_FIXED_BYTES || offset < TEXT_TREE_HEADER_BYTES + 2u * node->count ||
            offset > cursor - ENTRY_FIXED_BYTES) return false;
        uint16_t size = get_u16(node->body + offset);
        if (size > TEXT_TREE_KEY_MAX_BYTES || cursor < ENTRY_FIXED_BYTES + size ||
            offset != cursor - ENTRY_FIXED_BYTES - size) return false;
        cursor = offset;
        if (node->level > 0 && i == 0 && size != 0) return false;
        if (i > (node->level > 0 ? 1u : 0u) &&
            compare_keys(entry_at(node, i - 1).key, entry_at(node, i).key) >= 0) return false;
    }
    return true;
}

static pk_status_t load_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page,
                             int expected_level, node_t *node)
{
    pk_status_t status = io->read(io->context, page, node->body);
    if (status != PK_OK) return status;
    if (memcmp(node->body, TEXT_TREE_MAGIC, 4) != 0 ||
        get_u16(node->body + 4) != TEXT_TREE_VERSION || get_u32(node->body + 8) != table_id) return PK_CORRUPT;
    node->level = node->body[6];
    node->count = get_u32(node->body + 12);
    node->stamp = get_u32(node->body + 16);
    if (!node_valid(node) || (expected_level != LEVEL_ANY && node->level != expected_level)) return PK_CORRUPT;
    return PK_OK;
}

static pk_status_t store_node(const pk_tree_io_t *io, uint32_t table_id, node_t *node,
                              uint32_t old_page, uint32_t *page)
{
    memcpy(node->body, TEXT_TREE_MAGIC, 4);
    put_u16(node->body + 4, TEXT_TREE_VERSION);
    node->body[6] = node->level;
    node->body[7] = 0;
    put_u32(node->body + 8, table_id);
    put_u32(node->body + 12, node->count);
    put_u32(node->body + 16, node->stamp);
    put_u32(node->body + 20, 0);
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

static void copy_entries(const node_t *node, entry_t *entries)
{
    for (uint32_t i = 0; i < node->count; i++) entries[i] = entry_at(node, i);
}

static size_t encoded_size(const entry_t *entries, uint32_t count, uint8_t level)
{
    size_t total = TEXT_TREE_HEADER_BYTES + 2u * count;
    for (uint32_t i = 0; i < count; i++) {
        total += i == 0 && level > 0 ? ENTRY_FIXED_BYTES : entry_size(entries[i]);
    }
    return total;
}

static void build_node(node_t *node, uint8_t level, uint32_t stamp,
                       const entry_t *entries, uint32_t count)
{
    memset(node, 0, sizeof(*node));
    node->level = level;
    node->stamp = stamp;
    node->count = count;
    size_t cursor = PAGE_BODY_BYTES;
    for (uint32_t i = 0; i < count; i++) {
        entry_t entry = entries[i];
        if (level > 0 && i == 0) entry.key = (text_key_t){NULL, 0};
        cursor -= entry_size(entry);
        put_u16(node->body + TEXT_TREE_HEADER_BYTES + 2u * i, (uint16_t)cursor);
        put_u16(node->body + cursor, entry.key.size);
        if (entry.key.size > 0) memcpy(node->body + cursor + 2, entry.key.bytes, entry.key.size);
        put_u32(node->body + cursor + 2 + entry.key.size, entry.target);
        put_u16(node->body + cursor + 6 + entry.key.size, entry.slot);
    }
}

static void describe(change_t *out, uint8_t level, uint32_t count)
{
    out->level = level;
    out->count = count;
}

/* 키 길이와 슬롯 배열까지 합친 바이트 크기로 나눠 짧은 키의 밀도를 유지한다. */
static pk_status_t save_entries(const pk_tree_io_t *io, uint32_t table_id, const entry_t *entries,
                                uint32_t count, uint8_t level, uint32_t stamp,
                                uint32_t old_page, change_t *out)
{
    node_t left;
    if (encoded_size(entries, count, level) <= PAGE_BODY_BYTES) {
        build_node(&left, level, stamp, entries, count);
        pk_status_t status = store_node(io, table_id, &left, old_page, &out->page);
        if (status == PK_OK) describe(out, level, count);
        return status;
    }
    size_t half = encoded_size(entries, count, level) / 2u;
    size_t used = TEXT_TREE_HEADER_BYTES;
    uint32_t keep = 0;
    while (keep + 1 < count && used + 2u + entry_size(entries[keep]) <= half) {
        used += 2u + entry_size(entries[keep]);
        keep++;
    }
    if (keep == 0) keep = 1;
    while (keep + 1 < count && encoded_size(entries + keep, count - keep, level) > PAGE_BODY_BYTES) keep++;
    if (encoded_size(entries, keep, level) > PAGE_BODY_BYTES ||
        encoded_size(entries + keep, count - keep, level) > PAGE_BODY_BYTES) return PK_LIMIT;
    node_t right;
    build_node(&left, level, stamp, entries, keep);
    build_node(&right, level, 0, entries + keep, count - keep);
    out->right_size = entries[keep].key.size;
    if (out->right_size > 0) memcpy(out->right_key, entries[keep].key.bytes, out->right_size);
    pk_status_t status = store_node(io, table_id, &left, old_page, &out->page);
    if (status != PK_OK) return status;
    status = store_node(io, table_id, &right, PAGE_FILE_NO_PAGE, &out->right_page);
    if (status != PK_OK) return status;
    out->has_right = true;
    describe(out, level, keep);
    return PK_OK;
}

static uint32_t leaf_search(const node_t *node, text_key_t key, bool *found)
{
    uint32_t low = 0, high = node->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        if (compare_keys(entry_at(node, mid).key, key) < 0) low = mid + 1;
        else high = mid;
    }
    *found = low < node->count && compare_keys(entry_at(node, low).key, key) == 0;
    return low;
}

static uint32_t child_index(const node_t *node, text_key_t key)
{
    uint32_t low = 1, high = node->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        if (compare_keys(entry_at(node, mid).key, key) <= 0) low = mid + 1;
        else high = mid;
    }
    return low - 1;
}

pk_status_t text_tree_create(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root)
{
    node_t node;
    memset(&node, 0, sizeof(node));
    return store_node(io, table_id, &node, PAGE_FILE_NO_PAGE, root);
}

pk_status_t text_tree_stamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t root, uint32_t *stamp)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, root, LEVEL_ANY, &node);
    if (status == PK_OK) *stamp = node.stamp;
    return status;
}

pk_status_t text_tree_restamp(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, uint32_t stamp)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, *root, LEVEL_ANY, &node);
    if (status != PK_OK) return status;
    node.stamp = stamp;
    return store_node(io, table_id, &node, *root, root);
}

// cost: time O(d·log c), heap O(1), stack O(1), io d
// mem: stack ≈ size(node_t)
// vars: d = 트리 깊이, c = 노드당 항목 수
// basis: estimate
pk_status_t text_tree_find(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                           const uint8_t *key, size_t key_bytes, heap_locator_t *locator)
{
    if (key_bytes > TEXT_TREE_KEY_MAX_BYTES || (key_bytes > 0 && key == NULL)) return PK_LIMIT;
    text_key_t lookup = {key, (uint16_t)key_bytes};
    node_t node;
    int level = LEVEL_ANY;
    uint32_t page = root;
    for (;;) {
        pk_status_t status = load_node(io, table_id, page, level, &node);
        if (status != PK_OK) return status;
        if (node.level == 0) break;
        page = entry_at(&node, child_index(&node, lookup)).target;
        level = node.level - 1;
    }
    bool found;
    uint32_t index = leaf_search(&node, lookup, &found);
    if (!found) return PK_NOT_FOUND;
    entry_t entry = entry_at(&node, index);
    *locator = (heap_locator_t){entry.target, entry.slot};
    return PK_OK;
}

static pk_status_t put_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page,
                            int expected_level, text_key_t key, heap_locator_t locator,
                            bool replace, heap_locator_t *previous, change_t *out)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    memset(out, 0, sizeof(*out));
    out->page = page;
    describe(out, node.level, node.count);
    entry_t entries[MAX_ENTRIES + 1u];
    copy_entries(&node, entries);
    if (node.level == 0) {
        bool found;
        uint32_t index = leaf_search(&node, key, &found);
        if (found) {
            heap_locator_t old = {entries[index].target, entries[index].slot};
            if (previous != NULL) *previous = old;
            if (!replace) return PK_EXISTS;
            if (old.directory_index == locator.directory_index && old.slot == locator.slot) return PK_OK;
            entries[index].target = locator.directory_index;
            entries[index].slot = locator.slot;
            return save_entries(io, table_id, entries, node.count, node.level, node.stamp, page, out);
        }
        memmove(entries + index + 1u, entries + index, (node.count - index) * sizeof(*entries));
        entries[index] = (entry_t){key, locator.directory_index, locator.slot};
        return save_entries(io, table_id, entries, node.count + 1u, node.level, node.stamp, page, out);
    }
    uint32_t slot = child_index(&node, key);
    change_t below;
    status = put_node(io, table_id, entries[slot].target, node.level - 1, key, locator,
                      replace, previous, &below);
    if (status != PK_OK) return status;
    if (!below.has_right && below.page == entries[slot].target) return PK_OK;
    entries[slot].target = below.page;
    uint32_t count = node.count;
    if (below.has_right) {
        memmove(entries + slot + 2u, entries + slot + 1u, (count - slot - 1u) * sizeof(*entries));
        entries[slot + 1u] = (entry_t){{below.right_key, below.right_size}, below.right_page, 0};
        count++;
    }
    return save_entries(io, table_id, entries, count, node.level, node.stamp, page, out);
}

// cost: time O(d·c·k), heap O(1), stack O(d), io O(d)
// mem: stack ≈ d·(size(node_t) + MAX_ENTRIES·size(entry_t))
// vars: d = 트리 깊이, c = 노드 항목 수, k = 평균 키 바이트
// basis: estimate
pk_status_t text_tree_put(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root,
                          const uint8_t *key, size_t key_bytes, heap_locator_t locator,
                          bool replace, heap_locator_t *previous)
{
    if (key_bytes > TEXT_TREE_KEY_MAX_BYTES || (key_bytes > 0 && key == NULL)) return PK_LIMIT;
    change_t out;
    pk_status_t status = put_node(io, table_id, *root, LEVEL_ANY,
                                  (text_key_t){key, (uint16_t)key_bytes}, locator, replace, previous, &out);
    if (status != PK_OK) return status;
    if (!out.has_right) {
        *root = out.page;
        return PK_OK;
    }
    if (out.level >= PK_TREE_MAX_LEVEL) return PK_LIMIT;
    entry_t top[2] = {{{NULL, 0}, out.page, 0},
                      {{out.right_key, out.right_size}, out.right_page, 0}};
    node_t node;
    build_node(&node, (uint8_t)(out.level + 1u), 0, top, 2);
    return store_node(io, table_id, &node, PAGE_FILE_NO_PAGE, root);
}

static pk_status_t remove_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page,
                               int expected_level, text_key_t key, heap_locator_t *removed, change_t *out)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    memset(out, 0, sizeof(*out));
    out->page = page;
    entry_t entries[MAX_ENTRIES + 1u];
    copy_entries(&node, entries);
    uint32_t count = node.count;
    if (node.level == 0) {
        bool found;
        uint32_t index = leaf_search(&node, key, &found);
        if (!found) return PK_NOT_FOUND;
        *removed = (heap_locator_t){entries[index].target, entries[index].slot};
        memmove(entries + index, entries + index + 1u, (count - index - 1u) * sizeof(*entries));
        count--;
    } else {
        uint32_t slot = child_index(&node, key);
        change_t below;
        status = remove_node(io, table_id, entries[slot].target, node.level - 1, key, removed, &below);
        if (status != PK_OK) return status;
        if (!below.is_empty && below.page == entries[slot].target) {
            describe(out, node.level, count);
            return PK_OK;
        }
        if (below.is_empty) {
            memmove(entries + slot, entries + slot + 1u, (count - slot - 1u) * sizeof(*entries));
            count--;
            if (slot == 0 && count > 0) entries[0].key = (text_key_t){NULL, 0};
        } else entries[slot].target = below.page;
    }
    describe(out, node.level, count);
    if (count > 0) return save_entries(io, table_id, entries, count, node.level, node.stamp, page, out);
    out->is_empty = true;
    return io->release(io->context, page);
}

static pk_status_t collapse_root(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root, change_t *top)
{
    while (top->level > 0 && top->count == 1) {
        node_t node;
        pk_status_t status = load_node(io, table_id, *root, LEVEL_ANY, &node);
        if (status != PK_OK) return status;
        uint32_t child = entry_at(&node, 0).target;
        status = io->release(io->context, *root);
        if (status != PK_OK) return status;
        *root = child;
        status = load_node(io, table_id, child, node.level - 1, &node);
        if (status != PK_OK) return status;
        describe(top, node.level, node.count);
    }
    return PK_OK;
}

// cost: time O(d·c·k), heap O(1), stack O(d), io O(d)
// mem: stack ≈ d·(size(node_t) + MAX_ENTRIES·size(entry_t))
// vars: d = 트리 깊이, c = 노드 항목 수, k = 평균 키 바이트
// basis: estimate
pk_status_t text_tree_remove(const pk_tree_io_t *io, uint32_t table_id, uint32_t *root,
                             const uint8_t *key, size_t key_bytes, heap_locator_t *removed)
{
    if (key_bytes > TEXT_TREE_KEY_MAX_BYTES || (key_bytes > 0 && key == NULL)) return PK_LIMIT;
    change_t out;
    pk_status_t status = remove_node(io, table_id, *root, LEVEL_ANY,
                                     (text_key_t){key, (uint16_t)key_bytes}, removed, &out);
    if (status != PK_OK) return status;
    if (out.is_empty) return text_tree_create(io, table_id, root);
    *root = out.page;
    return collapse_root(io, table_id, root, &out);
}

static pk_status_t visit_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page,
                              int expected_level, bool (*visit)(void *context, uint32_t page_id), void *context)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    if (!visit(context, page)) return PK_NOMEM;
    for (uint32_t i = 0; node.level > 0 && i < node.count; i++) {
        status = visit_node(io, table_id, entry_at(&node, i).target, node.level - 1, visit, context);
        if (status != PK_OK) return status;
    }
    return PK_OK;
}

pk_status_t text_tree_visit_pages(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                                  bool (*visit)(void *context, uint32_t page_id), void *context)
{
    return visit_node(io, table_id, root, LEVEL_ANY, visit, context);
}

static int compare_prefix(text_key_t key, text_key_t prefix)
{
    size_t common = key.size < prefix.size ? key.size : prefix.size;
    int compared = common == 0 ? 0 : memcmp(key.bytes, prefix.bytes, common);
    if (compared != 0) return compared;
    return key.size < prefix.size ? -1 : 0;
}

/* 중복값의 composite key는 같은 prefix로 모이므로 시작 child부터 끝 경계까지만 방문한다. */
static pk_status_t visit_prefix_node(const pk_tree_io_t *io, uint32_t table_id, uint32_t page,
                                     int expected_level, text_key_t prefix,
                                     bool (*visit)(void *context, heap_locator_t locator), void *context)
{
    node_t node;
    pk_status_t status = load_node(io, table_id, page, expected_level, &node);
    if (status != PK_OK) return status;
    if (node.level == 0) {
        bool found;
        uint32_t start = leaf_search(&node, prefix, &found);
        for (uint32_t i = start; i < node.count; i++) {
            entry_t entry = entry_at(&node, i);
            if (compare_prefix(entry.key, prefix) != 0) break;
            heap_locator_t locator = {entry.target, entry.slot};
            if (!visit(context, locator)) return PK_NOMEM;
        }
        return PK_OK;
    }
    uint32_t start = child_index(&node, prefix);
    for (uint32_t i = start; i < node.count; i++) {
        entry_t child = entry_at(&node, i);
        if (i > start && compare_prefix(child.key, prefix) > 0) break;
        status = visit_prefix_node(io, table_id, child.target, node.level - 1, prefix, visit, context);
        if (status != PK_OK) return status;
    }
    return PK_OK;
}

// cost: time O(d·log c + m), heap O(1), stack O(d), io O(d + m/c)
// vars: d = 트리 깊이, c = 노드당 항목 수, m = 일치하는 항목 수
// basis: estimate
pk_status_t text_tree_visit_prefix(const pk_tree_io_t *io, uint32_t table_id, uint32_t root,
                                   const uint8_t *prefix, size_t prefix_bytes,
                                   bool (*visit)(void *context, heap_locator_t locator), void *context)
{
    if (visit == NULL || prefix_bytes > TEXT_TREE_KEY_MAX_BYTES ||
        (prefix_bytes > 0 && prefix == NULL)) return PK_LIMIT;
    return visit_prefix_node(io, table_id, root, LEVEL_ANY,
                             (text_key_t){prefix, (uint16_t)prefix_bytes}, visit, context);
}
