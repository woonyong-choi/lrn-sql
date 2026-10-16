#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "storage/heap_store.h"
#include "storage/le.h"
#include "storage/overflow.h"
#include "storage/pk_tree.h"
#include "storage/text_tree.h"
#include "storage/record.h"
#include "storage/record_page.h"

_Static_assert(RECORD_PAGE_BYTES == PAGE_BODY_BYTES, "record page body must equal the page_file body");

/* record page 머리글의 slot 수(u16)는 record_page.c의 배치(4..5바이트)를 따른다. 테스트가 대조한다. */
#define RECORD_PAGE_SLOT_COUNT_OFFSET 4u
#define OVERFLOW_REF_BYTES 8u
#define REAL_BYTES 8u
#define SPILL_MIN_BYTES 16u
#define ROW_VERSION 1
#define ROW_FIELDS(columns) (1u + 2u * (columns))

enum { KIND_NULL = 0, KIND_INLINE = 1, KIND_OVERFLOW = 2 };

typedef struct {
    uint32_t *items;
    size_t len;
    size_t cap;
} u32_list_t;

typedef struct {
    uint16_t *items;
    size_t len;
    size_t cap;
} u16_list_t;

/* ledger는 선택이다. 있으면 이 모듈의 모든 할당을 MEM_SCOPE_QUERY로 먼저 reserve한다. */
struct heap {
    mem_ledger_t ledger;
    page_file_t *file;
    uint32_t table_id;
    uint32_t root;
    value_type_t *types;
    size_t column_count;
    uint32_t *entries;      /* record page ID, heap 순서 */
    uint16_t *free_bytes;   /* 같은 순서의 삽입 가능 공간 힌트 */
    size_t free_cap;
    bool has_hints;
    size_t entry_count;
    size_t entry_cap;       /* entries가 쥔 계상 용량 */
    uint32_t *dirs;         /* 디렉터리 페이지 ID, 체인 순서 */
    size_t dir_count;
    size_t dir_cap;
    bool is_tree;           /* true면 dirs[0]은 LHD2 루트이고 dirs[1..]이 잎이다. false면 LHD1 체인 순서 */
    page_file_status_t last_status;
    bool indexed;           /* 키 색인을 쓴다(heap_index_spec_t) */
    size_t key_column;
    value_type_t key_type;
    uint32_t index_root;    /* 열 때의 색인 root */
    heap_stats_t *stats;
};

struct heap_batch {
    mem_ledger_t ledger;
    heap_t *heap;
    uint32_t *entries;
    uint16_t *free_bytes;
    uint8_t **bufs;         /* 읽어 온 또는 새로 만든 record page 본문. NULL이면 아직 안 읽음 */
    bool *dirty;            /* true면 entries[i]가 이 batch가 새로 할당한 페이지 */
    size_t entry_count;
    size_t entry_cap;
    u32_list_t old_pages;   /* 게시 성공 뒤 release */
    u32_list_t fresh;       /* 이 batch가 할당했고 아직 살아 있는 페이지 */
    bool failed;
    bool finished;
    uint32_t new_root;
    uint32_t index_root;    /* 이 batch가 바꾸고 있는 색인의 현재 root */
    heap_secondary_spec_t *secondary;
    uint32_t *secondary_roots;
    size_t secondary_count;
};

struct heap_cursor {
    mem_ledger_t ledger;
    size_t row_charge;      /* 마지막으로 돌려준 행의 TEXT·BLOB 바이트. 다음 next·close에서 돌려준다 */
    heap_t *heap;
    size_t index;
    uint16_t next_slot;
    uint16_t slot_count;
    uint8_t *page;
};

/* ── 보조 함수 ── */

/* 계상 helper. 모두 QUERY scope이며 ledger가 없으면 항상 성공한다. */
static bool acct_reserve(const mem_ledger_t *ledger, size_t bytes)
{
    return mem_reserve(ledger, MEM_SCOPE_QUERY, bytes);
}

static void acct_release(const mem_ledger_t *ledger, size_t bytes)
{
    mem_release(ledger, MEM_SCOPE_QUERY, bytes);
}

/* 곱셈 오버플로를 reserve 거절로 취급한다. */
static bool mul_ok(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b) return false;
    *out = a * b;
    return true;
}

/*
 * list가 쥔 용량(cap * 4바이트)을 ledger에 계상한다. 늘릴 때는 새 용량을 먼저 reserve하고 realloc 뒤
 * 옛 용량을 release하므로 순간 최대치는 old+new다. 실패하면 list와 계상은 그대로다.
 */
static bool list_reserve(u32_list_t *list, size_t extra, const mem_ledger_t *ledger)
{
    if (list->len + extra <= list->cap) return true;
    size_t cap = list->cap == 0 ? 16 : list->cap;
    while (cap < list->len + extra) {
        if (cap > SIZE_MAX / 2 / sizeof(uint32_t)) return false;
        cap *= 2;
    }
    if (!acct_reserve(ledger, cap * sizeof(uint32_t))) return false;
    uint32_t *grown = realloc(list->items, cap * sizeof(uint32_t));
    if (grown == NULL) {
        acct_release(ledger, cap * sizeof(uint32_t));
        return false;
    }
    acct_release(ledger, list->cap * sizeof(uint32_t));
    list->items = grown;
    list->cap = cap;
    return true;
}

static bool list_push(u32_list_t *list, uint32_t value, const mem_ledger_t *ledger)
{
    if (!list_reserve(list, 1, ledger)) return false;
    list->items[list->len++] = value;
    return true;
}

static void list_free(u32_list_t *list, const mem_ledger_t *ledger)
{
    size_t bytes = list->cap * sizeof(uint32_t);
    free(list->items);
    *list = (u32_list_t){NULL, 0, 0};
    acct_release(ledger, bytes);  /* 해제한 뒤에 몫을 돌려준다 */
}

static bool list_reserve16(u16_list_t *list, size_t extra, const mem_ledger_t *ledger)
{
    if (list->len + extra <= list->cap) return true;
    size_t cap = list->cap == 0 ? 16 : list->cap;
    while (cap < list->len + extra) {
        if (cap > SIZE_MAX / 2 / sizeof(uint16_t)) return false;
        cap *= 2;
    }
    if (!acct_reserve(ledger, cap * sizeof(uint16_t))) return false;
    uint16_t *grown = realloc(list->items, cap * sizeof(uint16_t));
    if (grown == NULL) {
        acct_release(ledger, cap * sizeof(uint16_t));
        return false;
    }
    acct_release(ledger, list->cap * sizeof(uint16_t));
    list->items = grown;
    list->cap = cap;
    return true;
}

static void list_free16(u16_list_t *list, const mem_ledger_t *ledger)
{
    size_t bytes = list->cap * sizeof(uint16_t);
    free(list->items);
    *list = (u16_list_t){NULL, 0, 0};
    acct_release(ledger, bytes);
}

static bool list_remove(u32_list_t *list, uint32_t value)
{
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i] == value) {
            list->items[i] = list->items[--list->len];
            return true;
        }
    }
    return false;
}

static heap_status_t map_pf(heap_t *heap, page_file_status_t status)
{
    heap->last_status = status;
    switch (status) {
        case PAGE_FILE_OK: return HEAP_OK;
        case PAGE_FILE_NOMEM: return HEAP_NOMEM;
        case PAGE_FILE_LIMIT: return HEAP_LIMIT;
        case PAGE_FILE_CORRUPT:
        case PAGE_FILE_UNSUPPORTED:
        case PAGE_FILE_NO_CONTROL:
        case PAGE_FILE_INVALID: return HEAP_CORRUPT;
        default: return HEAP_IO;
    }
}

static heap_status_t map_overflow(heap_t *heap, overflow_status_t status,
                                  const page_file_overflow_ctx_t *ctx)
{
    switch (status) {
        case OVERFLOW_OK: return HEAP_OK;
        case OVERFLOW_INVALID: return HEAP_INVALID;
        case OVERFLOW_LIMIT: return HEAP_LIMIT;
        case OVERFLOW_NOMEM: return HEAP_NOMEM;
        case OVERFLOW_CORRUPT: return HEAP_CORRUPT;
        case OVERFLOW_IO:
        case OVERFLOW_ROLLBACK_FAILED:
        case OVERFLOW_UNRECLAIMED:
            if (ctx != NULL && ctx->last_status != PAGE_FILE_OK) return map_pf(heap, ctx->last_status);
            return HEAP_IO;
        default: return HEAP_IO;
    }
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static bool column_type_valid(value_type_t type)
{
    return type == VALUE_INTEGER || type == VALUE_REAL || type == VALUE_TEXT || type == VALUE_BLOB;
}

/* ── 행 형식 ── */

/* record 필드 배열(1 + 2n개)이 이 heap의 행 형식과 맞는지 검사한다. */
static heap_status_t check_fields(const heap_t *heap, const record_value_t *f, size_t count)
{
    if (count != ROW_FIELDS(heap->column_count)) return HEAP_CORRUPT;
    if (f[0].type != RECORD_INTEGER || f[0].integer != ROW_VERSION) return HEAP_CORRUPT;
    for (size_t i = 0; i < heap->column_count; i++) {
        const record_value_t *kind = &f[1 + 2 * i];
        const record_value_t *payload = &f[2 + 2 * i];
        if (kind->type != RECORD_INTEGER) return HEAP_CORRUPT;
        value_type_t type = heap->types[i];
        switch (kind->integer) {
            case KIND_NULL:
                if (payload->type != RECORD_NULL) return HEAP_CORRUPT;
                break;
            case KIND_INLINE:
                if (type == VALUE_INTEGER && payload->type != RECORD_INTEGER) return HEAP_CORRUPT;
                if (type == VALUE_REAL &&
                    (payload->type != RECORD_BLOB || payload->size_bytes != REAL_BYTES)) {
                    return HEAP_CORRUPT;
                }
                if (type == VALUE_TEXT && payload->type != RECORD_TEXT) return HEAP_CORRUPT;
                if (type == VALUE_BLOB && payload->type != RECORD_BLOB) return HEAP_CORRUPT;
                break;
            case KIND_OVERFLOW:
                if ((type != VALUE_TEXT && type != VALUE_BLOB) || payload->type != RECORD_BLOB ||
                    payload->size_bytes != OVERFLOW_REF_BYTES) {
                    return HEAP_CORRUPT;
                }
                break;
            default: return HEAP_CORRUPT;
        }
    }
    return HEAP_OK;
}

static heap_status_t parse_fields(const heap_t *heap, record_slice_t slice, record_value_t *fields,
                                  size_t capacity)
{
    record_buffer_t input = {(uint8_t *)slice.bytes, slice.size_bytes};
    size_t count = 0;
    record_status_t status = record_decode(&input, fields, capacity, &count);
    if (status == RECORD_NOMEM) return HEAP_NOMEM;
    if (status != RECORD_OK) return HEAP_CORRUPT;
    return check_fields(heap, fields, count);
}

static overflow_ref_t ref_from(const record_value_t *payload)
{
    overflow_ref_t ref;
    ref.head = le_get_u32(payload->bytes);
    ref.size_bytes = le_get_u32(payload->bytes + 4);
    return ref;
}

static heap_status_t set_bytes_value(value_type_t type, const uint8_t *bytes, size_t size,
                                     value_t *out)
{
    value_status_t status = type == VALUE_TEXT ? value_set_text(out, bytes, size)
                                               : value_set_blob(out, bytes, size);
    if (status == VALUE_NOMEM) return HEAP_NOMEM;
    return status == VALUE_OK ? HEAP_OK : HEAP_CORRUPT;
}

/*
 * overflow_read가 쓰는 임시 메모리의 상한: 데이터 buffer(size)와 페이지 buffer(page_bytes).
 * overflow_write는 페이지 ID 두 배열(2 * count * 4)과 페이지 buffer, overflow_free는 ID 배열과 페이지
 * buffer를 쓴다. count의 상한은 헤더가 OVERFLOW_MIN_PAGE_BYTES보다 작다는 사실로 잡는다.
 */
static size_t overflow_count_bound(size_t page_bytes, size_t size_bytes)
{
    size_t payload = page_bytes > OVERFLOW_MIN_PAGE_BYTES ? page_bytes - OVERFLOW_MIN_PAGE_BYTES : 1;
    return size_bytes / payload + 1;
}

static size_t overflow_read_bound(size_t page_bytes, size_t size_bytes)
{
    return size_bytes > 0 ? size_bytes + page_bytes : 0;
}

static size_t overflow_write_bound(size_t page_bytes, size_t size_bytes)
{
    return size_bytes > 0 ? 2 * overflow_count_bound(page_bytes, size_bytes) * sizeof(uint32_t) + page_bytes : 0;
}

static size_t overflow_free_bound(size_t page_bytes, size_t size_bytes)
{
    return size_bytes > 0 ? overflow_count_bound(page_bytes, size_bytes) * sizeof(uint32_t) + page_bytes : 0;
}

/*
 * 필드를 value_t 배열로 옮긴다. 실패하면 out은 바꾸지 않는다. 성공하면 *charged에 행이 쥔 TEXT·BLOB
 * 바이트(ledger에 계상한 몫)를 돌려준다. 임시 배열과 overflow 읽기 buffer는 끝나기 전에 돌려준다.
 */
static heap_status_t decode_row(heap_t *heap, const record_value_t *f, const bool *selected,
                                value_t *out, size_t *charged)
{
    size_t n = heap->column_count;
    const mem_ledger_t *ledger = &heap->ledger;
    *charged = 0;
    size_t tmp_bytes;
    if (!mul_ok(n, sizeof(value_t), &tmp_bytes) || !acct_reserve(ledger, tmp_bytes)) return HEAP_NOMEM;
    value_t *tmp = calloc(n, sizeof(*tmp));
    if (tmp == NULL) {
        acct_release(ledger, tmp_bytes);
        return HEAP_NOMEM;
    }
    heap_status_t result = HEAP_OK;
    size_t persistent = 0;
    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, heap->file, heap->table_id);
    for (size_t i = 0; i < n && result == HEAP_OK; i++) {
        if (selected != NULL && !selected[i]) continue;
        const record_value_t *kind = &f[1 + 2 * i];
        const record_value_t *payload = &f[2 + 2 * i];
        value_type_t type = heap->types[i];
        if (kind->integer == KIND_NULL) continue;
        if (kind->integer == KIND_OVERFLOW) {
            overflow_ref_t ref = ref_from(payload);
            size_t transient = overflow_read_bound(io.page_bytes, ref.size_bytes);
            if (!acct_reserve(ledger, transient)) {
                result = HEAP_NOMEM;
                break;
            }
            uint8_t *bytes = NULL;
            overflow_status_t read = overflow_read(&io, ref, &bytes);
            if (read != OVERFLOW_OK) {
                acct_release(ledger, transient);
                result = map_overflow(heap, read, &ctx);
                break;
            }
            if (!acct_reserve(ledger, ref.size_bytes)) {
                free(bytes);
                acct_release(ledger, transient);
                result = HEAP_NOMEM;
                break;
            }
            result = set_bytes_value(type, bytes, ref.size_bytes, &tmp[i]);
            free(bytes);
            acct_release(ledger, transient);
            if (result == HEAP_OK) persistent += ref.size_bytes;
            else acct_release(ledger, ref.size_bytes);
            continue;
        }
        switch (type) {
            case VALUE_INTEGER:
                if (value_set_integer(&tmp[i], payload->integer) != VALUE_OK) result = HEAP_CORRUPT;
                break;
            case VALUE_REAL: {
                uint64_t bits = le_get_u64(payload->bytes);
                double real;
                memcpy(&real, &bits, sizeof(real));
                value_status_t status = value_set_real(&tmp[i], real);
                if (status != VALUE_OK) result = HEAP_CORRUPT;
                break;
            }
            default:
                if (!acct_reserve(ledger, payload->size_bytes)) {
                    result = HEAP_NOMEM;
                    break;
                }
                result = set_bytes_value(type, payload->bytes, payload->size_bytes, &tmp[i]);
                if (result == HEAP_OK) persistent += payload->size_bytes;
                else acct_release(ledger, payload->size_bytes);
                break;
        }
    }
    if (result == HEAP_OK) {
        for (size_t i = 0; i < n; i++) {
            value_clear(&out[i]);
            out[i] = tmp[i];
        }
        *charged = persistent;
    } else {
        for (size_t i = 0; i < n; i++) value_clear(&tmp[i]);
        acct_release(ledger, persistent);
    }
    free(tmp);
    acct_release(ledger, tmp_bytes);
    return result;
}

/* ── 열기 ── */

/* 이 heap이 쥔 모든 계상(구조체·types·entries·dirs)을 돌려주고 해제한다. */
static void heap_destroy(heap_t *heap)
{
    const mem_ledger_t ledger = heap->ledger;
    size_t bytes = sizeof(*heap) + heap->column_count * sizeof(*heap->types) +
                   (heap->entry_cap + heap->dir_cap) * sizeof(uint32_t) +
                   heap->free_cap * sizeof(uint16_t);
    free(heap->types);
    free(heap->entries);
    free(heap->free_bytes);
    free(heap->dirs);
    free(heap);
    acct_release(&ledger, bytes);
}

/* 디렉터리 페이지 한 장의 종류와 table ID를 확인하고 본문을 읽는다. 다른 종류는 UNSUPPORTED_LOCATOR다. */
static heap_status_t read_directory_page(heap_t *heap, uint32_t page, uint8_t *body)
{
    page_file_page_info_t info;
    page_file_status_t status = page_file_page_info(heap->file, page, &info);
    if (status != PAGE_FILE_OK) return map_pf(heap, status);
    if (info.kind != PAGE_KIND_DIRECTORY) return HEAP_UNSUPPORTED_LOCATOR;
    if (info.table_id != heap->table_id) return HEAP_CORRUPT;
    status = page_file_read(heap->file, page, PAGE_KIND_DIRECTORY, heap->table_id, body);
    return status == PAGE_FILE_OK ? HEAP_OK : map_pf(heap, status);
}

/* 항목 수가 한 페이지의 한도 안이고 항목 뒤가 모두 0인지 검사한다. */
static heap_status_t check_directory_count(const uint8_t *body, bool hinted, uint32_t *count)
{
    *count = le_get_u32(body + 16);
    size_t width = hinted ? 6u : 4u;
    size_t limit = hinted ? HEAP_HINT_DIRECTORY_ENTRIES : HEAP_DIRECTORY_ENTRIES;
    if (*count > limit) return HEAP_CORRUPT;
    static const uint8_t zero[PAGE_BODY_BYTES];
    size_t used = HEAP_DIRECTORY_HEADER_BYTES + (size_t)*count * width;
    return memcmp(body + used, zero, PAGE_BODY_BYTES - used) == 0 ? HEAP_OK : HEAP_CORRUPT;
}

static heap_status_t read_tree_leaf(heap_t *heap, uint32_t page, bool is_last, u32_list_t *entries,
                                    u16_list_t *free_bytes, bool *has_hints, bool *format_known)
{
    uint8_t body[PAGE_BODY_BYTES];
    heap_status_t result = read_directory_page(heap, page, body);
    if (result == HEAP_UNSUPPORTED_LOCATOR) return HEAP_CORRUPT;
    if (result != HEAP_OK) return result;
    bool hinted = memcmp(body, HEAP_HINT_DIRECTORY_MAGIC, 4) == 0 &&
                  le_get_u16(body + 4) == HEAP_HINT_DIRECTORY_VERSION;
    bool legacy = memcmp(body, HEAP_DIRECTORY_MAGIC, 4) == 0 &&
                  le_get_u16(body + 4) == HEAP_DIRECTORY_VERSION;
    if ((!hinted && !legacy) || (*format_known && *has_hints != hinted) ||
        le_get_u16(body + 6) != 0 || le_get_u32(body + 8) != heap->table_id || le_get_u32(body + 12) != PAGE_FILE_NO_PAGE) {
        return HEAP_CORRUPT;
    }
    *has_hints = hinted;
    *format_known = true;
    uint32_t count;
    result = check_directory_count(body, hinted, &count);
    if (result != HEAP_OK) return result;
    size_t limit = hinted ? HEAP_HINT_DIRECTORY_ENTRIES : HEAP_DIRECTORY_ENTRIES;
    if (count == 0 || (!is_last && count != limit)) return HEAP_CORRUPT;
    if (!list_reserve(entries, count, &heap->ledger) ||
        !list_reserve16(free_bytes, count, &heap->ledger)) return HEAP_NOMEM;
    for (uint32_t i = 0; i < count; i++) {
        size_t offset = HEAP_DIRECTORY_HEADER_BYTES + (size_t)i * (hinted ? 6u : 4u);
        entries->items[entries->len++] = le_get_u32(body + offset);
        free_bytes->items[free_bytes->len++] = hinted ? le_get_u16(body + offset + 4u) : 0;
    }
    return HEAP_OK;
}

/* LHD2 루트 본문(body)을 해석해 잎을 모두 읽는다. 루트 머리글의 어긋남은 첫 페이지 규칙대로 UNSUPPORTED다. */
static heap_status_t read_tree(heap_t *heap, uint32_t root, const uint8_t *body, u32_list_t *entries,
                               u16_list_t *free_bytes, u32_list_t *dirs, bool *has_hints)
{
    if (le_get_u16(body + 4) != HEAP_TREE_ROOT_VERSION || le_get_u16(body + 6) != 0 ||
        le_get_u32(body + 8) != heap->table_id || le_get_u32(body + 12) != 0) {
        return HEAP_UNSUPPORTED_LOCATOR;
    }
    uint32_t leaves;
    heap_status_t result = check_directory_count(body, false, &leaves);
    if (result != HEAP_OK) return result;
    if (leaves == 0) return HEAP_CORRUPT;
    if (!list_push(dirs, root, &heap->ledger) || !list_reserve(dirs, leaves, &heap->ledger)) return HEAP_NOMEM;
    bool format_known = false;
    for (uint32_t i = 0; i < leaves && result == HEAP_OK; i++) {
        uint32_t leaf = le_get_u32(body + HEAP_DIRECTORY_HEADER_BYTES + (size_t)i * 4);
        dirs->items[dirs->len++] = leaf;
        result = read_tree_leaf(heap, leaf, i + 1 == leaves, entries,
                                free_bytes, has_hints, &format_known);
    }
    return result;
}

static heap_status_t read_directory(heap_t *heap, const page_file_control_t *control)
{
    uint32_t page = heap->root;
    const mem_ledger_t *ledger = &heap->ledger;
    u32_list_t entries = {NULL, 0, 0};
    u16_list_t free_bytes = {NULL, 0, 0};
    u32_list_t dirs = {NULL, 0, 0};
    heap_status_t result = HEAP_OK;
    bool is_tree = false;
    bool has_hints = false, format_known = false;
    uint8_t body[PAGE_BODY_BYTES];
    /* 게시 전인 변경(한 commit으로 묶는 여러 문장)이 새로 할당한 페이지도 범위 안이다 */
    uint32_t allocated = page_file_allocated_pages(heap->file);
    (void)control;

    while (page != PAGE_FILE_NO_PAGE && result == HEAP_OK) {
        if (dirs.len >= allocated) { result = HEAP_CORRUPT; break; }  /* 순환 */
        result = read_directory_page(heap, page, body);
        if (result == HEAP_UNSUPPORTED_LOCATOR && dirs.len > 0) result = HEAP_CORRUPT;
        if (result != HEAP_OK) break;
        if (dirs.len == 0 && memcmp(body, HEAP_TREE_ROOT_MAGIC, 4) == 0) {
            is_tree = true;
            result = read_tree(heap, page, body, &entries, &free_bytes, &dirs, &has_hints);
            break;
        }
        bool hinted = memcmp(body, HEAP_HINT_DIRECTORY_MAGIC, 4) == 0 &&
                      le_get_u16(body + 4) == HEAP_HINT_DIRECTORY_VERSION;
        bool legacy = memcmp(body, HEAP_DIRECTORY_MAGIC, 4) == 0 &&
                      le_get_u16(body + 4) == HEAP_DIRECTORY_VERSION;
        if ((!hinted && !legacy) || (format_known && has_hints != hinted) ||
            le_get_u16(body + 6) != 0 || le_get_u32(body + 8) != heap->table_id) {
            result = dirs.len == 0 ? HEAP_UNSUPPORTED_LOCATOR : HEAP_CORRUPT;
            break;
        }
        has_hints = hinted;
        format_known = true;
        uint32_t count;
        result = check_directory_count(body, hinted, &count);
        if (result != HEAP_OK) break;
        if (!list_push(&dirs, page, ledger) || !list_reserve(&entries, count, ledger) ||
            !list_reserve16(&free_bytes, count, ledger)) { result = HEAP_NOMEM; break; }
        for (uint32_t i = 0; i < count; i++) {
            size_t offset = HEAP_DIRECTORY_HEADER_BYTES + (size_t)i * (hinted ? 6u : 4u);
            entries.items[entries.len++] = le_get_u32(body + offset);
            free_bytes.items[free_bytes.len++] = hinted ? le_get_u16(body + offset + 4u) : 0;
        }
        page = le_get_u32(body + 12);
    }

    if (result == HEAP_OK) {
        /* 범위와 중복: 항목과 디렉터리 페이지가 서로 겹치지 않아야 한다. */
        size_t total = entries.len + dirs.len;
        size_t all_bytes = 0;
        bool reserved = mul_ok(total, sizeof(uint32_t), &all_bytes) && acct_reserve(ledger, all_bytes);
        uint32_t *all = reserved && total > 0 ? malloc(total * sizeof(*all)) : NULL;
        if (total > 0 && all == NULL) {
            if (reserved) acct_release(ledger, all_bytes);
            result = HEAP_NOMEM;
        } else if (total > 0) {
            memcpy(all, entries.items, entries.len * sizeof(*all));
            memcpy(all + entries.len, dirs.items, dirs.len * sizeof(*all));
            qsort(all, total, sizeof(*all), compare_u32);
            for (size_t i = 0; i < total; i++) {
                if (all[i] < PAGE_FILE_CONTROL_PAGES || all[i] >= allocated ||
                    (i > 0 && all[i] == all[i - 1])) {
                    result = HEAP_CORRUPT;
                    break;
                }
            }
            free(all);
            acct_release(ledger, all_bytes);
        }
    }
    if (result == HEAP_OK && entries.len != free_bytes.len) result = HEAP_CORRUPT;
    if (result != HEAP_OK) {
        list_free(&entries, ledger);
        list_free16(&free_bytes, ledger);
        list_free(&dirs, ledger);
        return result;
    }
    heap->entries = entries.items;
    heap->free_bytes = free_bytes.items;
    heap->free_cap = free_bytes.cap;
    heap->has_hints = has_hints;
    heap->entry_count = entries.len;
    heap->entry_cap = entries.cap;
    heap->dirs = dirs.items;
    heap->dir_count = dirs.len;
    heap->dir_cap = dirs.cap;
    heap->is_tree = is_tree;
    return HEAP_OK;
}

/* ── 키 색인: 읽기 ── */

static pk_status_t pk_from_page_file(heap_t *heap, page_file_status_t status)
{
    heap->last_status = status;
    switch (status) {
        case PAGE_FILE_OK: return PK_OK;
        case PAGE_FILE_NOMEM: return PK_NOMEM;
        case PAGE_FILE_CORRUPT:
        case PAGE_FILE_UNSUPPORTED:
        case PAGE_FILE_NO_CONTROL:
        case PAGE_FILE_INVALID: return PK_CORRUPT;
        default: return PK_IO;
    }
}

/* PK_IO는 항상 직전 page_file 실패 뒤에 오므로 last_status가 원인이다. */
static heap_status_t heap_from_pk(heap_t *heap, pk_status_t status)
{
    switch (status) {
        case PK_OK: return HEAP_OK;
        case PK_NOT_FOUND: return HEAP_NOT_FOUND;
        case PK_EXISTS: return HEAP_KEY_EXISTS;
        case PK_CORRUPT: return HEAP_CORRUPT;
        case PK_NOMEM: return HEAP_NOMEM;
        case PK_LIMIT: return HEAP_LIMIT;
        default: return heap->last_status == PAGE_FILE_OK ? HEAP_IO : map_pf(heap, heap->last_status);
    }
}

static pk_status_t index_read(void *context, uint32_t page_id, uint8_t body[PAGE_BODY_BYTES])
{
    heap_t *heap = context;
    if (heap->stats != NULL) heap->stats->index_pages_read++;
    return pk_from_page_file(heap, page_file_read(heap->file, page_id, PAGE_KIND_INDEX, heap->table_id, body));
}

static pk_tree_io_t index_read_io(heap_t *heap)
{
    pk_tree_io_t io = {heap, index_read, NULL, NULL, NULL, NULL};
    return io;
}

static heap_status_t setup_index(heap_t *heap, const heap_index_spec_t *spec)
{
    heap->index_root = spec->index_root;
    heap->stats = spec->stats;
    if (spec->key_column >= heap->column_count) return HEAP_OK;
    value_type_t type = heap->types[spec->key_column];
    if (type != VALUE_INTEGER && type != VALUE_TEXT) return HEAP_OK;
    heap->key_column = spec->key_column;
    heap->key_type = type;
    if (spec->index_root == PAGE_FILE_NO_PAGE) {
        heap->indexed = heap->root == PAGE_FILE_NO_PAGE;
        return HEAP_OK;
    }
    pk_tree_io_t io = index_read_io(heap);
    uint32_t stamp = 0;
    pk_status_t checked = type == VALUE_INTEGER
                              ? pk_tree_stamp(&io, heap->table_id, spec->index_root, &stamp)
                              : text_tree_stamp(&io, heap->table_id, spec->index_root, &stamp);
    heap_status_t status = heap_from_pk(heap, checked);
    if (status == HEAP_OK) heap->indexed = heap->root != PAGE_FILE_NO_PAGE && stamp == heap->root;
    return status;
}

bool heap_is_indexed(const heap_t *heap) { return heap != NULL && heap->indexed; }

heap_status_t heap_open(page_file_t *file, uint32_t table_id, uint32_t heap_root_page,
                        const heap_schema_t *schema, heap_t **out)
{
    return heap_open_ex(file, table_id, heap_root_page, schema, NULL, out);
}

heap_status_t heap_open_ex(page_file_t *file, uint32_t table_id, uint32_t heap_root_page,
                           const heap_schema_t *schema, const mem_ledger_t *ledger, heap_t **out)
{
    return heap_open_indexed(file, table_id, heap_root_page, schema, NULL, ledger, out);
}

heap_status_t heap_open_indexed(page_file_t *file, uint32_t table_id, uint32_t heap_root_page,
                                const heap_schema_t *schema, const heap_index_spec_t *spec,
                                const mem_ledger_t *ledger, heap_t **out)
{
    if (file == NULL || schema == NULL || out == NULL || schema->types == NULL ||
        schema->column_count == 0 || schema->column_count > HEAP_MAX_COLUMNS) {
        return HEAP_INVALID;
    }
    for (size_t i = 0; i < schema->column_count; i++) {
        if (!column_type_valid(schema->types[i])) return HEAP_INVALID;
    }
    size_t fixed = sizeof(heap_t) + schema->column_count * sizeof(value_type_t);
    mem_ledger_t none = {NULL, NULL};
    const mem_ledger_t *use = ledger != NULL ? ledger : &none;
    if (!acct_reserve(use, fixed)) return HEAP_NOMEM;
    heap_t *heap = calloc(1, sizeof(*heap));
    if (heap == NULL) {
        acct_release(use, fixed);
        return HEAP_NOMEM;
    }
    heap->ledger = *use;
    heap->file = file;
    heap->table_id = table_id;
    heap->root = heap_root_page;
    heap->column_count = schema->column_count;
    heap->types = malloc(schema->column_count * sizeof(*heap->types));
    if (heap->types == NULL) {
        free(heap);
        acct_release(use, fixed);
        return HEAP_NOMEM;
    }
    memcpy(heap->types, schema->types, schema->column_count * sizeof(*heap->types));
    heap_status_t status = read_directory(heap, page_file_control(file));
    if (status == HEAP_OK && spec != NULL) status = setup_index(heap, spec);
    if (status != HEAP_OK) {
        heap_destroy(heap);
        return status;
    }
    *out = heap;
    return HEAP_OK;
}

void heap_close(heap_t *heap)
{
    if (heap == NULL) return;
    heap_destroy(heap);
}

uint32_t heap_root(const heap_t *heap) { return heap != NULL ? heap->root : PAGE_FILE_NO_PAGE; }

size_t heap_record_page_count(const heap_t *heap) { return heap != NULL ? heap->entry_count : 0; }

page_file_status_t heap_last_page_file_status(const heap_t *heap)
{
    return heap != NULL ? heap->last_status : PAGE_FILE_OK;
}

/* ── record page 읽기 ── */

static heap_status_t read_record_page(heap_t *heap, uint32_t page_id, uint8_t *page)
{
    page_file_status_t status = page_file_read(heap->file, page_id, PAGE_KIND_RECORD, heap->table_id, page);
    if (status != PAGE_FILE_OK) return map_pf(heap, status);
    return record_page_validate(page) == RECORD_PAGE_OK ? HEAP_OK : HEAP_CORRUPT;
}

/* 성공하면 *charged가 행이 쥔 TEXT·BLOB 바이트다. 호출자가 value_clear한 뒤 그만큼을 돌려준다. */
static heap_status_t row_from_slot(heap_t *heap, const uint8_t *page, uint16_t slot, value_t *row,
                                   const bool *selected, size_t *charged)
{
    *charged = 0;
    record_slice_t slice;
    record_page_status_t got = record_page_get(page, slot, &slice);
    if (got == RECORD_PAGE_NOT_FOUND) return HEAP_NOT_FOUND;
    if (got != RECORD_PAGE_OK) return HEAP_CORRUPT;
    size_t field_bytes;
    if (!mul_ok(ROW_FIELDS(heap->column_count), sizeof(record_value_t), &field_bytes) ||
        !acct_reserve(&heap->ledger, field_bytes)) {
        return HEAP_NOMEM;
    }
    record_value_t *fields = malloc(field_bytes);
    if (fields == NULL) {
        acct_release(&heap->ledger, field_bytes);
        return HEAP_NOMEM;
    }
    heap_status_t status = parse_fields(heap, slice, fields, ROW_FIELDS(heap->column_count));
    if (status == HEAP_OK) status = decode_row(heap, fields, selected, row, charged);
    free(fields);
    acct_release(&heap->ledger, field_bytes);
    return status;
}

/* 행의 TEXT·BLOB 바이트 합. row_from_slot이 계상한 몫과 같다. */
static size_t row_bytes(const value_t *row, size_t count)
{
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (row[i].view.type == VALUE_TEXT || row[i].view.type == VALUE_BLOB) total += row[i].view.size_bytes;
    }
    return total;
}

void heap_release_row(const heap_t *heap, value_t *row)
{
    if (heap == NULL || row == NULL) return;
    size_t bytes = row_bytes(row, heap->column_count);
    for (size_t i = 0; i < heap->column_count; i++) value_clear(&row[i]);
    acct_release(&heap->ledger, bytes);
}

void heap_free_pages(const heap_t *heap, uint32_t *pages, size_t count)
{
    free(pages);
    if (heap != NULL) acct_release(&heap->ledger, count * sizeof(uint32_t));
}

heap_status_t heap_get_projected(heap_t *heap, heap_locator_t locator, const bool *selected,
                                 value_t *row)
{
    if (heap == NULL || row == NULL) return HEAP_INVALID;
    if (locator.directory_index >= heap->entry_count) return HEAP_NOT_FOUND;
    uint8_t page[RECORD_PAGE_BYTES];
    heap_status_t status = read_record_page(heap, heap->entries[locator.directory_index], page);
    if (status != HEAP_OK) return status;
    size_t charged;
    return row_from_slot(heap, page, locator.slot, row, selected, &charged);
}

heap_status_t heap_get(heap_t *heap, heap_locator_t locator, value_t *row)
{
    return heap_get_projected(heap, locator, NULL, row);
}

/* ── 키 색인: 조회 ── */

typedef struct {
    bool present;
    int64_t integer;
    size_t size_bytes;
    uint8_t bytes[TEXT_TREE_KEY_MAX_BYTES];
} row_key_t;

/* 행(record 조각)의 키 컬럼 값을 읽는다. NULL이면 present가 false다. */
static heap_status_t slice_key(const heap_t *heap, record_slice_t slice, row_key_t *out)
{
    size_t field_bytes;
    if (!mul_ok(ROW_FIELDS(heap->column_count), sizeof(record_value_t), &field_bytes) ||
        !acct_reserve(&heap->ledger, field_bytes)) {
        return HEAP_NOMEM;
    }
    record_value_t *fields = malloc(field_bytes);
    if (fields == NULL) {
        acct_release(&heap->ledger, field_bytes);
        return HEAP_NOMEM;
    }
    heap_status_t status = parse_fields(heap, slice, fields, ROW_FIELDS(heap->column_count));
    if (status == HEAP_OK) {
        out->present = fields[1 + 2 * heap->key_column].integer != KIND_NULL;
        if (out->present && heap->key_type == VALUE_INTEGER) {
            out->integer = fields[2 + 2 * heap->key_column].integer;
        } else if (out->present) {
            size_t row_bytes = heap->column_count * sizeof(value_t);
            if (!acct_reserve(&heap->ledger, row_bytes)) status = HEAP_NOMEM;
            else {
                value_t *row = calloc(heap->column_count, sizeof(*row));
                if (row == NULL) status = HEAP_NOMEM;
                else {
                    bool selected[HEAP_MAX_COLUMNS] = {false};
                    selected[heap->key_column] = true;
                    size_t charged = 0;
                    status = decode_row((heap_t *)heap, fields, selected, row, &charged);
                    if (status == HEAP_OK) {
                        out->size_bytes = row[heap->key_column].view.size_bytes;
                        if (out->size_bytes > TEXT_TREE_KEY_MAX_BYTES) status = HEAP_LIMIT;
                        else if (out->size_bytes > 0) {
                            memcpy(out->bytes, row[heap->key_column].view.bytes, out->size_bytes);
                        }
                    }
                    for (size_t i = 0; i < heap->column_count; i++) value_clear(&row[i]);
                    acct_release(&heap->ledger, charged);
                    free(row);
                }
                acct_release(&heap->ledger, row_bytes);
            }
        }
    }
    free(fields);
    acct_release(&heap->ledger, field_bytes);
    return status;
}

/* 색인이 가리킨 행이 실제로 있고 그 키가 같은지 확인한다. 아니면 낡은 locator이므로 CORRUPT다. */
static heap_status_t check_indexed_row(heap_t *heap, heap_locator_t at, const row_key_t *key)
{
    if (at.directory_index >= heap->entry_count) return HEAP_CORRUPT;
    uint8_t page[RECORD_PAGE_BYTES];
    heap_status_t status = read_record_page(heap, heap->entries[at.directory_index], page);
    if (status != HEAP_OK) return status;
    record_slice_t slice;
    if (record_page_get(page, at.slot, &slice) != RECORD_PAGE_OK) return HEAP_CORRUPT;
    row_key_t found;
    status = slice_key(heap, slice, &found);
    if (status != HEAP_OK) return status;
    if (!found.present) return HEAP_CORRUPT;
    if (heap->key_type == VALUE_INTEGER) return found.integer == key->integer ? HEAP_OK : HEAP_CORRUPT;
    return found.size_bytes == key->size_bytes &&
                   (found.size_bytes == 0 || memcmp(found.bytes, key->bytes, found.size_bytes) == 0)
               ? HEAP_OK : HEAP_CORRUPT;
}

// cost: time O(d·log c + p), heap O(1), stack O(1), io d + 1
// mem: stack ≈ size(node_t) + size(record page)
// vars: d = 색인 깊이, c = 노드당 항목 수, p = 행 하나의 필드 해석
// basis: estimate
// alt: heap_cursor 전체 스캔. time O(n), heap O(1), io O(n). 잃는 것: 키 조회 시간
heap_status_t heap_find_key(heap_t *heap, int64_t key, heap_locator_t *locator)
{
    if (heap == NULL || locator == NULL || !heap->indexed || heap->key_type != VALUE_INTEGER) return HEAP_INVALID;
    if (heap->stats != NULL) heap->stats->index_lookups++;
    if (heap->index_root == PAGE_FILE_NO_PAGE) return HEAP_NOT_FOUND;
    pk_tree_io_t io = index_read_io(heap);
    heap_locator_t at;
    heap_status_t status = heap_from_pk(heap, pk_tree_find(&io, heap->table_id, heap->index_root, key, &at));
    if (status != HEAP_OK) return status;
    row_key_t expected = {.present = true, .integer = key};
    status = check_indexed_row(heap, at, &expected);
    if (status == HEAP_OK) *locator = at;
    return status;
}

typedef struct {
    heap_t *heap;
    bool (*visit)(void *context, heap_locator_t locator);
    void *context;
    heap_status_t status;
} key_range_t;

static bool visit_key_entry(void *context, int64_t key, heap_locator_t locator)
{
    key_range_t *range = context;
    row_key_t expected = {.present = true, .integer = key};
    range->status = check_indexed_row(range->heap, locator, &expected);
    return range->status == HEAP_OK && range->visit(range->context, locator);
}

// cost: time O(d·log c + k·p), heap O(1), stack O(d), io d + l + k
// mem: stack ≈ d·size(node_t) + size(record page)
// vars: d = 색인 깊이, c = 노드당 항목 수, k = 범위 안 행 수, l = 범위에 걸친 잎 수, p = 행 하나의 필드 해석
// basis: estimate
// alt: heap_cursor 전체 스캔. time O(n), io O(n). 잃는 것: 좁은 범위의 조회 시간
heap_status_t heap_visit_key_range(heap_t *heap, int64_t low, int64_t high,
                                   bool (*visit)(void *context, heap_locator_t locator), void *context,
                                   bool *stopped)
{
    if (heap == NULL || visit == NULL || stopped == NULL || !heap->indexed || heap->key_type != VALUE_INTEGER) {
        return HEAP_INVALID;
    }
    *stopped = false;
    if (heap->stats != NULL) heap->stats->index_lookups++;
    if (heap->index_root == PAGE_FILE_NO_PAGE) return HEAP_OK;
    pk_tree_io_t io = index_read_io(heap);
    key_range_t range = {heap, visit, context, HEAP_OK};
    bool halted = false;
    heap_status_t status = heap_from_pk(heap, pk_tree_visit_range(&io, heap->table_id, heap->index_root, low, high,
                                                                  visit_key_entry, &range, &halted));
    if (status != HEAP_OK) return status;
    if (range.status != HEAP_OK) return range.status;
    *stopped = halted;
    return HEAP_OK;
}

heap_status_t heap_find_text_key(heap_t *heap, const uint8_t *key, size_t key_bytes,
                                 heap_locator_t *locator)
{
    if (heap == NULL || locator == NULL || !heap->indexed || heap->key_type != VALUE_TEXT ||
        (key_bytes > 0 && key == NULL)) return HEAP_INVALID;
    if (key_bytes > TEXT_TREE_KEY_MAX_BYTES) return HEAP_LIMIT;
    if (heap->stats != NULL) heap->stats->index_lookups++;
    if (heap->index_root == PAGE_FILE_NO_PAGE) return HEAP_NOT_FOUND;
    pk_tree_io_t io = index_read_io(heap);
    heap_locator_t at;
    heap_status_t status = heap_from_pk(heap, text_tree_find(&io, heap->table_id, heap->index_root,
                                                              key, key_bytes, &at));
    if (status != HEAP_OK) return status;
    row_key_t expected = {.present = true, .size_bytes = key_bytes};
    if (key_bytes > 0) memcpy(expected.bytes, key, key_bytes);
    status = check_indexed_row(heap, at, &expected);
    if (status == HEAP_OK) *locator = at;
    return status;
}

heap_status_t heap_max_key(heap_t *heap, bool *present, int64_t *key)
{
    if (heap == NULL || present == NULL || key == NULL || !heap->indexed ||
        heap->key_type != VALUE_INTEGER) return HEAP_INVALID;
    if (heap->stats != NULL) heap->stats->index_lookups++;
    *present = false;
    if (heap->index_root == PAGE_FILE_NO_PAGE) return HEAP_OK;
    pk_tree_io_t io = index_read_io(heap);
    return heap_from_pk(heap, pk_tree_max(&io, heap->table_id, heap->index_root, present, key));
}

/* ── cursor ── */

heap_status_t heap_cursor_open(heap_t *heap, heap_cursor_t **out)
{
    if (heap == NULL || out == NULL) return HEAP_INVALID;
    if (!acct_reserve(&heap->ledger, sizeof(heap_cursor_t))) return HEAP_NOMEM;
    heap_cursor_t *cursor = calloc(1, sizeof(*cursor));
    if (cursor == NULL) {
        acct_release(&heap->ledger, sizeof(heap_cursor_t));
        return HEAP_NOMEM;
    }
    cursor->heap = heap;
    cursor->ledger = heap->ledger;
    *out = cursor;
    return HEAP_OK;
}

static void cursor_drop_page(heap_cursor_t *cursor)
{
    if (cursor->page == NULL) return;
    free(cursor->page);
    cursor->page = NULL;
    acct_release(&cursor->ledger, RECORD_PAGE_BYTES);
}

heap_status_t heap_cursor_next_projected(heap_cursor_t *cursor, const bool *selected, value_t *row,
                                         heap_locator_t *locator)
{
    if (cursor == NULL || row == NULL || locator == NULL) return HEAP_INVALID;
    heap_t *heap = cursor->heap;
    /* 앞서 돌려준 행은 호출자가 이미 clear했다. 그 몫을 여기서 돌려준다. */
    acct_release(&cursor->ledger, cursor->row_charge);
    cursor->row_charge = 0;
    for (;;) {
        if (cursor->page == NULL) {
            if (cursor->index >= heap->entry_count) return HEAP_DONE;
            if (!acct_reserve(&cursor->ledger, RECORD_PAGE_BYTES)) return HEAP_NOMEM;
            cursor->page = malloc(RECORD_PAGE_BYTES);
            if (cursor->page == NULL) {
                acct_release(&cursor->ledger, RECORD_PAGE_BYTES);
                return HEAP_NOMEM;
            }
            heap_status_t status = read_record_page(heap, heap->entries[cursor->index], cursor->page);
            if (status != HEAP_OK) {
                cursor_drop_page(cursor);
                return status;
            }
            cursor->slot_count = le_get_u16(cursor->page + RECORD_PAGE_SLOT_COUNT_OFFSET);
            cursor->next_slot = 0;
        }
        while (cursor->next_slot < cursor->slot_count) {
            uint16_t slot = cursor->next_slot;
            size_t charged;
            heap_status_t status = row_from_slot(heap, cursor->page, slot, row, selected, &charged);
            if (status == HEAP_NOT_FOUND) {
                cursor->next_slot++;
                continue;
            }
            if (status != HEAP_OK) return status;  /* 같은 행을 다시 시도할 수 있다 */
            cursor->next_slot++;
            cursor->row_charge = charged;
            if (heap->stats != NULL) heap->stats->rows_scanned++;
            locator->directory_index = (uint32_t)cursor->index;
            locator->slot = slot;
            return HEAP_OK;
        }
        cursor_drop_page(cursor);
        cursor->index++;
    }
}

heap_status_t heap_cursor_next(heap_cursor_t *cursor, value_t *row, heap_locator_t *locator)
{
    return heap_cursor_next_projected(cursor, NULL, row, locator);
}

void heap_cursor_close(heap_cursor_t *cursor)
{
    if (cursor == NULL) return;
    mem_ledger_t ledger = cursor->ledger;
    cursor_drop_page(cursor);
    acct_release(&ledger, cursor->row_charge);
    free(cursor);
    acct_release(&ledger, sizeof(heap_cursor_t));
}

/* ── 모든 페이지 수집(DROP) ── */

typedef struct {
    heap_t *heap;
    page_file_overflow_ctx_t inner;
    overflow_io_t inner_io;
    u32_list_t *out;
} collect_ctx_t;

static bool collect_allocate(void *context, uint32_t *page_id)
{
    (void)context;
    (void)page_id;
    return false;
}

static bool collect_release(void *context, uint32_t page_id)
{
    collect_ctx_t *ctx = context;
    return list_push(ctx->out, page_id, &ctx->heap->ledger);
}

static bool collect_read(void *context, uint32_t page_id, uint8_t *page)
{
    collect_ctx_t *ctx = context;
    return ctx->inner_io.read(ctx->inner_io.context, page_id, page);
}

static bool collect_write(void *context, uint32_t page_id, const uint8_t *page)
{
    (void)context;
    (void)page_id;
    (void)page;
    return false;
}

typedef struct {
    u32_list_t *list;
    const mem_ledger_t *ledger;
} index_collect_t;

static bool collect_index_page(void *context, uint32_t page_id)
{
    index_collect_t *collect = context;
    return list_push(collect->list, page_id, collect->ledger);
}

static heap_status_t collect_index_pages(heap_t *heap, u32_list_t *list)
{
    pk_tree_io_t io = index_read_io(heap);
    index_collect_t collect = {list, &heap->ledger};
    pk_status_t visited = heap->key_type == VALUE_TEXT
                              ? text_tree_visit_pages(&io, heap->table_id, heap->index_root,
                                                      collect_index_page, &collect)
                              : pk_tree_visit_pages(&io, heap->table_id, heap->index_root,
                                                    collect_index_page, &collect);
    return heap_from_pk(heap, visited);
}

heap_status_t heap_collect_pages_with_indexes(heap_t *heap, const uint32_t *roots, size_t root_count,
                                              uint32_t **pages, size_t *count)
{
    if (heap == NULL || pages == NULL || count == NULL ||
        (root_count > 0 && roots == NULL)) return HEAP_INVALID;
    const mem_ledger_t *ledger = &heap->ledger;
    u32_list_t list = {NULL, 0, 0};
    heap_status_t result = HEAP_OK;
    size_t field_bytes = 0;
    record_value_t *fields = NULL;
    uint8_t *page = NULL;
    bool page_reserved = false;
    if (!mul_ok(ROW_FIELDS(heap->column_count), sizeof(*fields), &field_bytes) ||
        !acct_reserve(ledger, field_bytes)) {
        return HEAP_NOMEM;
    }
    fields = malloc(field_bytes);
    if (acct_reserve(ledger, RECORD_PAGE_BYTES)) {
        page_reserved = true;
        page = malloc(RECORD_PAGE_BYTES);
    }
    if (fields == NULL || page == NULL) result = HEAP_NOMEM;
    if (result == HEAP_OK && (!list_reserve(&list, heap->dir_count + heap->entry_count, ledger))) result = HEAP_NOMEM;
    /* 읽기에 쓰지 않는 어긋난 색인도 DROP 때는 그 페이지를 회수해야 한다. */
    if (result == HEAP_OK && heap->index_root != PAGE_FILE_NO_PAGE) {
        result = collect_index_pages(heap, &list);
    }
    pk_tree_io_t index_io = index_read_io(heap);
    index_collect_t index_collect = {&list, ledger};
    for (size_t i = 0; i < root_count && result == HEAP_OK; i++) {
        if (roots[i] == PAGE_FILE_NO_PAGE) continue;
        pk_status_t visited = text_tree_visit_pages(&index_io, heap->table_id, roots[i],
                                                     collect_index_page, &index_collect);
        result = heap_from_pk(heap, visited);
    }
    for (size_t i = 0; result == HEAP_OK && i < heap->dir_count; i++) list.items[list.len++] = heap->dirs[i];
    for (size_t i = 0; result == HEAP_OK && i < heap->entry_count; i++) {
        list.items[list.len++] = heap->entries[i];
        result = read_record_page(heap, heap->entries[i], page);
        if (result != HEAP_OK) break;
        uint16_t slots = le_get_u16(page + RECORD_PAGE_SLOT_COUNT_OFFSET);
        for (uint16_t slot = 0; slot < slots && result == HEAP_OK; slot++) {
            record_slice_t slice;
            record_page_status_t got = record_page_get(page, slot, &slice);
            if (got == RECORD_PAGE_NOT_FOUND) continue;
            if (got != RECORD_PAGE_OK) { result = HEAP_CORRUPT; break; }
            result = parse_fields(heap, slice, fields, ROW_FIELDS(heap->column_count));
            for (size_t c = 0; result == HEAP_OK && c < heap->column_count; c++) {
                if (fields[1 + 2 * c].integer != KIND_OVERFLOW) continue;
                collect_ctx_t ctx;
                ctx.heap = heap;
                ctx.out = &list;
                ctx.inner_io = page_file_overflow_io(&ctx.inner, heap->file, heap->table_id);
                overflow_io_t io = {&ctx, ctx.inner_io.page_bytes, collect_allocate, collect_release,
                                    collect_read, collect_write};
                overflow_ref_t ref = ref_from(&fields[2 + 2 * c]);
                size_t transient = overflow_free_bound(io.page_bytes, ref.size_bytes);
                if (!acct_reserve(ledger, transient)) {
                    result = HEAP_NOMEM;
                    break;
                }
                overflow_status_t freed = overflow_free(&io, ref);
                acct_release(ledger, transient);
                if (freed != OVERFLOW_OK) result = map_overflow(heap, freed, &ctx.inner);
            }
        }
    }
    free(fields);
    acct_release(ledger, field_bytes);
    free(page);
    if (page_reserved) acct_release(ledger, RECORD_PAGE_BYTES);
    if (result != HEAP_OK) {
        list_free(&list, ledger);
        return result;
    }
    /* 남는 용량 없이 정확한 크기로 넘긴다. 새 배열을 먼저 reserve하므로 순간 최대치는 old+new다. */
    if (list.len == 0) {
        list_free(&list, ledger);
        *pages = NULL;
        *count = 0;
        return HEAP_OK;
    }
    if (!acct_reserve(ledger, list.len * sizeof(uint32_t))) {
        list_free(&list, ledger);
        return HEAP_NOMEM;
    }
    uint32_t *exact = malloc(list.len * sizeof(uint32_t));
    if (exact == NULL) {
        acct_release(ledger, list.len * sizeof(uint32_t));
        list_free(&list, ledger);
        return HEAP_NOMEM;
    }
    memcpy(exact, list.items, list.len * sizeof(uint32_t));
    *pages = exact;
    *count = list.len;
    list_free(&list, ledger);
    return HEAP_OK;
}

heap_status_t heap_collect_pages(heap_t *heap, uint32_t **pages, size_t *count)
{
    return heap_collect_pages_with_indexes(heap, NULL, 0, pages, count);
}

/* ── batch ── */

/* batch의 entries·bufs·dirty 배열이 항목 하나당 쥐는 바이트 */
#define BATCH_ENTRY_BYTES (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t *) + sizeof(bool))

heap_status_t heap_batch_begin(heap_t *heap, heap_batch_t **out)
{
    if (heap == NULL || out == NULL) return HEAP_INVALID;
    size_t cap = heap->entry_count > 0 ? heap->entry_count : 8;
    size_t bytes = sizeof(heap_batch_t);
    size_t arrays;
    if (!mul_ok(cap, BATCH_ENTRY_BYTES, &arrays)) return HEAP_NOMEM;
    bytes += arrays;
    if (!acct_reserve(&heap->ledger, bytes)) return HEAP_NOMEM;
    heap_batch_t *batch = calloc(1, sizeof(*batch));
    if (batch == NULL) {
        acct_release(&heap->ledger, bytes);
        return HEAP_NOMEM;
    }
    batch->ledger = heap->ledger;
    batch->heap = heap;
    batch->entry_count = heap->entry_count;
    batch->entry_cap = cap;
    batch->entries = malloc(batch->entry_cap * sizeof(*batch->entries));
    batch->free_bytes = calloc(batch->entry_cap, sizeof(*batch->free_bytes));
    batch->bufs = calloc(batch->entry_cap, sizeof(*batch->bufs));
    batch->dirty = calloc(batch->entry_cap, sizeof(*batch->dirty));
    if (batch->entries == NULL || batch->free_bytes == NULL || batch->bufs == NULL || batch->dirty == NULL) {
        free(batch->entries);
        free(batch->free_bytes);
        free(batch->bufs);
        free(batch->dirty);
        free(batch);
        acct_release(&heap->ledger, bytes);
        return HEAP_NOMEM;
    }
    if (heap->entry_count > 0) {
        memcpy(batch->entries, heap->entries, heap->entry_count * sizeof(*batch->entries));
        if (heap->has_hints) {
            memcpy(batch->free_bytes, heap->free_bytes, heap->entry_count * sizeof(*batch->free_bytes));
        } else {
            /* 옛 LHD1/LHD2를 처음 쓸 때만 실제 record page에서 힌트를 재구성한다. */
            uint8_t page[RECORD_PAGE_BYTES];
            for (size_t i = 0; i < heap->entry_count; i++) {
                heap_status_t read = read_record_page(heap, heap->entries[i], page);
                size_t available;
                if (read == HEAP_OK && record_page_available(page, &available) != RECORD_PAGE_OK)
                    read = HEAP_CORRUPT;
                if (read != HEAP_OK) {
                    heap_batch_free(batch);
                    return read;
                }
                batch->free_bytes[i] = (uint16_t)available;
            }
        }
    }
    batch->new_root = heap->root;
    batch->index_root = heap->index_root;
    *out = batch;
    return HEAP_OK;
}

void heap_batch_free(heap_batch_t *batch)
{
    if (batch == NULL) return;
    mem_ledger_t ledger = batch->ledger;
    size_t pages = 0;
    size_t structure = sizeof(heap_batch_t) + batch->entry_cap * BATCH_ENTRY_BYTES;
    for (size_t i = 0; i < batch->entry_count; i++) {
        if (batch->bufs[i] != NULL) pages++;
        free(batch->bufs[i]);
    }
    free(batch->entries);
    free(batch->free_bytes);
    free(batch->bufs);
    free(batch->dirty);
    size_t secondary_bytes = batch->secondary_count *
                             (sizeof(*batch->secondary) + sizeof(*batch->secondary_roots));
    free(batch->secondary);
    free(batch->secondary_roots);
    list_free(&batch->old_pages, &ledger);
    list_free(&batch->fresh, &ledger);
    free(batch);
    /* 모든 메모리를 해제한 뒤에 몫을 돌려준다 */
    acct_release(&ledger, pages * RECORD_PAGE_BYTES);
    acct_release(&ledger, secondary_bytes);
    acct_release(&ledger, structure);
}

/* 새 페이지 번호를 받아 fresh에 기록한다. 기록할 자리를 먼저 잡아 두어 할당 뒤 실패로 새지 않는다. */
static heap_status_t batch_alloc(heap_batch_t *batch, uint32_t *page_id)
{
    if (!list_reserve(&batch->fresh, 1, &batch->ledger)) return HEAP_NOMEM;
    page_file_status_t status = page_file_alloc(batch->heap->file, page_id);
    if (status != PAGE_FILE_OK) return map_pf(batch->heap, status);
    batch->fresh.items[batch->fresh.len++] = *page_id;
    return HEAP_OK;
}

typedef struct {
    heap_batch_t *batch;
    page_file_overflow_ctx_t inner;
    overflow_io_t inner_io;
} batch_io_ctx_t;

static bool batch_io_allocate(void *context, uint32_t *page_id)
{
    batch_io_ctx_t *ctx = context;
    return batch_alloc(ctx->batch, page_id) == HEAP_OK;
}

static heap_status_t batch_release_page(heap_batch_t *batch, uint32_t page_id)
{
    if (list_remove(&batch->fresh, page_id)) {
        /* 이 batch가 만든 페이지는 어떤 commit된 상태도 가리키지 않아 곧바로 놓아도 된다. */
        return map_pf(batch->heap, page_file_release(batch->heap->file, page_id));
    }
    /* commit된 상태가 가리키는 페이지는 게시 성공 뒤에야 놓는다. */
    return list_push(&batch->old_pages, page_id, &batch->ledger) ? HEAP_OK : HEAP_NOMEM;
}

static bool batch_io_release(void *context, uint32_t page_id)
{
    batch_io_ctx_t *ctx = context;
    return batch_release_page(ctx->batch, page_id) == HEAP_OK;
}

static bool batch_io_read(void *context, uint32_t page_id, uint8_t *page)
{
    batch_io_ctx_t *ctx = context;
    return ctx->inner_io.read(ctx->inner_io.context, page_id, page);
}

static bool batch_io_write(void *context, uint32_t page_id, const uint8_t *page)
{
    batch_io_ctx_t *ctx = context;
    return ctx->inner_io.write(ctx->inner_io.context, page_id, page);
}

static overflow_io_t batch_io(batch_io_ctx_t *ctx, heap_batch_t *batch)
{
    ctx->batch = batch;
    ctx->inner_io = page_file_overflow_io(&ctx->inner, batch->heap->file, batch->heap->table_id);
    overflow_io_t io = {ctx, ctx->inner_io.page_bytes, batch_io_allocate, batch_io_release,
                        batch_io_read, batch_io_write};
    return io;
}

static heap_status_t fail(heap_batch_t *batch, heap_status_t status)
{
    if (status != HEAP_OK && status != HEAP_INVALID && status != HEAP_NOT_FOUND) batch->failed = true;
    return status;
}

/* ── 키 색인: batch ── */

static pk_status_t batch_index_read(void *context, uint32_t page_id, uint8_t body[PAGE_BODY_BYTES])
{
    return index_read(((heap_batch_t *)context)->heap, page_id, body);
}

static pk_status_t batch_index_write(void *context, uint32_t page_id, const uint8_t body[PAGE_BODY_BYTES])
{
    heap_t *heap = ((heap_batch_t *)context)->heap;
    return pk_from_page_file(heap, page_file_write(heap->file, page_id, PAGE_KIND_INDEX, heap->table_id, body));
}

static pk_status_t pk_from_heap(heap_status_t status)
{
    if (status == HEAP_OK) return PK_OK;
    return status == HEAP_NOMEM ? PK_NOMEM : PK_IO;
}

static pk_status_t batch_index_allocate(void *context, uint32_t *page_id)
{
    return pk_from_heap(batch_alloc(context, page_id));
}

static pk_status_t batch_index_release(void *context, uint32_t page_id)
{
    return pk_from_heap(batch_release_page(context, page_id));
}

/* 최근에 만든 페이지일수록 다시 만지므로 끝에서부터 찾는다. */
static bool batch_index_is_fresh(void *context, uint32_t page_id)
{
    const u32_list_t *fresh = &((heap_batch_t *)context)->fresh;
    for (size_t i = fresh->len; i > 0; i--) {
        if (fresh->items[i - 1] == page_id) return true;
    }
    return false;
}

static pk_tree_io_t batch_index_io(heap_batch_t *batch)
{
    pk_tree_io_t io = {batch, batch_index_read, batch_index_write, batch_index_allocate, batch_index_release,
                       batch_index_is_fresh};
    return io;
}

static bool same_locator(heap_locator_t a, heap_locator_t b);

/* 정수를 순서 보존 8바이트(부호 비트를 뒤집은 big-endian)로 붙인다 */
static size_t put_ordered_integer(uint8_t *key, int64_t integer)
{
    uint64_t bits = (uint64_t)integer ^ (UINT64_C(1) << 63);
    for (size_t i = 0; i < 8; i++) key[i] = (uint8_t)(bits >> (56u - 8u * i));
    return 8;
}

/* 비고유 색인 키의 꼬리. ordered면 정수 기본 키, 아니면 행 위치다. 유일 색인은 꼬리가 없다 */
typedef struct {
    bool unique;
    const value_view_t *primary;   /* ordered일 때만 */
    heap_locator_t locator;
} secondary_tail_t;

static heap_status_t encode_secondary_key(const value_view_t *value, secondary_tail_t tail,
                                          uint8_t key[TEXT_TREE_KEY_MAX_BYTES], size_t *key_bytes)
{
    size_t used = 0;
    if (value->type == VALUE_INTEGER || value->type == VALUE_REAL) {
        uint64_t bits;
        if (value->type == VALUE_INTEGER) bits = (uint64_t)value->integer ^ (UINT64_C(1) << 63);
        else {
            double real = value->real == 0.0 ? 0.0 : value->real;
            memcpy(&bits, &real, sizeof(bits));
            bits = (bits & (UINT64_C(1) << 63)) ? ~bits : bits ^ (UINT64_C(1) << 63);
        }
        for (size_t i = 0; i < 8; i++) key[used++] = (uint8_t)(bits >> (56u - 8u * i));
    } else if (value->type == VALUE_TEXT || value->type == VALUE_BLOB) {
        for (size_t i = 0; i < value->size_bytes; i++) {
            if (used + 2u > TEXT_TREE_KEY_MAX_BYTES) return HEAP_LIMIT;
            key[used++] = value->bytes[i];
            if (value->bytes[i] == 0) key[used++] = 0xff;
        }
        if (used + 2u > TEXT_TREE_KEY_MAX_BYTES) return HEAP_LIMIT;
        key[used++] = 0;
        key[used++] = 0;
    } else return HEAP_INVALID;
    if (tail.unique) {
        /* 꼬리 없음 */
    } else if (tail.primary != NULL) {
        if (tail.primary->type != VALUE_INTEGER) return HEAP_CORRUPT;   /* 기본 키는 NULL일 수 없다 */
        if (used + 8u > TEXT_TREE_KEY_MAX_BYTES) return HEAP_LIMIT;
        used += put_ordered_integer(key + used, tail.primary->integer);
    } else {
        if (used + 6u > TEXT_TREE_KEY_MAX_BYTES) return HEAP_LIMIT;
        for (size_t i = 0; i < 4; i++) {
            key[used++] = (uint8_t)(tail.locator.directory_index >> (24u - 8u * i));
        }
        key[used++] = (uint8_t)(tail.locator.slot >> 8);
        key[used++] = (uint8_t)tail.locator.slot;
    }
    *key_bytes = used;
    return HEAP_OK;
}

bool heap_secondary_key_fits(const value_view_t *value)
{
    if (value == NULL || value->type == VALUE_NULL) return false;
    uint8_t key[TEXT_TREE_KEY_MAX_BYTES];
    size_t key_bytes;
    /* 꼬리는 기본 키(8바이트)가 위치(6바이트)보다 길다. 더 긴 쪽으로 판정한다 */
    static const value_view_t widest = {VALUE_INTEGER, 0, 0.0, NULL, 0};
    return encode_secondary_key(value, (secondary_tail_t){false, &widest, {0, 0}}, key, &key_bytes) == HEAP_OK;
}

// cost: time O(d·log c + m), heap O(1), stack O(d), io O(d + m/c)
// vars: d = 색인 깊이, c = 노드당 항목 수, m = 일치하는 locator 수
// basis: estimate
heap_status_t heap_secondary_visit_equal(heap_t *heap, heap_secondary_spec_t spec,
                                         const value_view_t *value,
                                         bool (*visit)(void *context, heap_locator_t locator), void *context)
{
    if (heap == NULL || value == NULL || visit == NULL || spec.column >= heap->column_count ||
        value->type == VALUE_NULL) return HEAP_INVALID;
    if (spec.root == PAGE_FILE_NO_PAGE) return heap->root == PAGE_FILE_NO_PAGE ? HEAP_OK : HEAP_CORRUPT;
    uint8_t key[TEXT_TREE_KEY_MAX_BYTES];
    size_t key_bytes;
    heap_status_t encoded = encode_secondary_key(value, (secondary_tail_t){true, NULL, {0, 0}}, key, &key_bytes);
    if (encoded != HEAP_OK) return encoded;
    pk_tree_io_t io = index_read_io(heap);
    uint32_t stamp;
    pk_status_t checked = text_tree_stamp(&io, heap->table_id, spec.root, &stamp);
    heap_status_t status = heap_from_pk(heap, checked);
    if (status != HEAP_OK) return status;
    if (stamp != heap->root) return HEAP_CORRUPT;
    if (heap->stats != NULL) heap->stats->index_lookups++;
    if (spec.unique) {
        heap_locator_t locator;
        checked = text_tree_find(&io, heap->table_id, spec.root, key, key_bytes, &locator);
        if (checked == PK_NOT_FOUND) return HEAP_OK;
        if (checked != PK_OK) return heap_from_pk(heap, checked);
        return visit(context, locator) ? HEAP_OK : HEAP_NOMEM;
    }
    checked = text_tree_visit_prefix(&io, heap->table_id, spec.root, key, key_bytes, visit, context);
    return heap_from_pk(heap, checked);
}

// cost: time O(d·log c), heap O(1), io O(d)
// vars: d = 색인 깊이, c = 노드당 항목 수
// basis: estimate
heap_status_t heap_batch_find_key(heap_batch_t *batch, const value_view_t *key, heap_locator_t *locator)
{
    if (batch == NULL || key == NULL || locator == NULL || batch->failed || !batch->heap->indexed ||
        key->type != batch->heap->key_type) return HEAP_INVALID;
    if (key->type == VALUE_TEXT && key->size_bytes > TEXT_TREE_KEY_MAX_BYTES) return HEAP_NOT_FOUND;
    if (batch->index_root == PAGE_FILE_NO_PAGE) return HEAP_NOT_FOUND;
    pk_tree_io_t io = batch_index_io(batch);
    pk_status_t found = key->type == VALUE_INTEGER
                            ? pk_tree_find(&io, batch->heap->table_id, batch->index_root, key->integer, locator)
                            : text_tree_find(&io, batch->heap->table_id, batch->index_root, key->bytes,
                                             key->size_bytes, locator);
    return found == PK_NOT_FOUND ? HEAP_NOT_FOUND : heap_from_pk(batch->heap, found);
}

heap_status_t heap_batch_secondary_find(heap_batch_t *batch, size_t index, const value_view_t *value,
                                        heap_locator_t *locator)
{
    if (batch == NULL || value == NULL || locator == NULL || batch->failed || index >= batch->secondary_count ||
        !batch->secondary[index].unique) return HEAP_INVALID;
    if (value->type == VALUE_NULL || batch->secondary_roots[index] == PAGE_FILE_NO_PAGE) return HEAP_NOT_FOUND;
    uint8_t key[TEXT_TREE_KEY_MAX_BYTES];
    size_t key_bytes;
    heap_status_t encoded = encode_secondary_key(value, (secondary_tail_t){true, NULL, {0, 0}}, key, &key_bytes);
    if (encoded == HEAP_LIMIT) return HEAP_NOT_FOUND;
    if (encoded != HEAP_OK) return encoded;
    pk_tree_io_t io = batch_index_io(batch);
    pk_status_t found = text_tree_find(&io, batch->heap->table_id, batch->secondary_roots[index], key, key_bytes,
                                       locator);
    return found == PK_NOT_FOUND ? HEAP_NOT_FOUND : heap_from_pk(batch->heap, found);
}

heap_status_t heap_batch_configure_secondary(heap_batch_t *batch, const heap_secondary_spec_t *specs,
                                             size_t count)
{
    if (batch == NULL || batch->finished || batch->failed || batch->secondary != NULL ||
        count > 64 || (count > 0 && specs == NULL)) return HEAP_INVALID;
    if (count == 0) return HEAP_OK;
    size_t bytes = count * (sizeof(*batch->secondary) + sizeof(*batch->secondary_roots));
    if (!acct_reserve(&batch->ledger, bytes)) return fail(batch, HEAP_NOMEM);
    heap_secondary_spec_t *copied = malloc(count * sizeof(*copied));
    uint32_t *roots = malloc(count * sizeof(*roots));
    if (copied == NULL || roots == NULL) {
        free(copied);
        free(roots);
        acct_release(&batch->ledger, bytes);
        return fail(batch, HEAP_NOMEM);
    }
    pk_tree_io_t io = index_read_io(batch->heap);
    for (size_t i = 0; i < count; i++) {
        /* ordered 꼬리는 정수 기본 키가 있는 heap의 비고유 색인에만 쓴다 */
        bool bad_order = specs[i].ordered && (specs[i].unique || batch->heap->key_type != VALUE_INTEGER ||
                                              batch->heap->key_column >= batch->heap->column_count);
        if (specs[i].column >= batch->heap->column_count || bad_order) {
            free(copied);
            free(roots);
            acct_release(&batch->ledger, bytes);
            return fail(batch, HEAP_INVALID);
        }
        if (specs[i].root != PAGE_FILE_NO_PAGE) {
            uint32_t stamp;
            pk_status_t checked = text_tree_stamp(&io, batch->heap->table_id, specs[i].root, &stamp);
            heap_status_t status = heap_from_pk(batch->heap, checked);
            if (status == HEAP_OK && stamp != batch->heap->root) status = HEAP_CORRUPT;
            if (status != HEAP_OK) {
                free(copied);
                free(roots);
                acct_release(&batch->ledger, bytes);
                return fail(batch, status);
            }
        }
        copied[i] = specs[i];
        roots[i] = specs[i].root;
    }
    batch->secondary = copied;
    batch->secondary_roots = roots;
    batch->secondary_count = count;
    return HEAP_OK;
}

static secondary_tail_t secondary_tail(const heap_batch_t *batch, size_t index, const value_view_t *primary,
                                       heap_locator_t locator)
{
    const heap_secondary_spec_t *spec = &batch->secondary[index];
    return (secondary_tail_t){spec->unique, spec->ordered ? primary : NULL, locator};
}

/* value는 색인 열의 값, primary는 같은 행의 기본 키 값이다 */
static heap_status_t secondary_put_one(heap_batch_t *batch, size_t index, const value_view_t *value,
                                       const value_view_t *primary, heap_locator_t locator)
{
    if (value->type == VALUE_NULL) return HEAP_OK;
    uint8_t key[TEXT_TREE_KEY_MAX_BYTES];
    size_t key_bytes;
    heap_status_t status = encode_secondary_key(value, secondary_tail(batch, index, primary, locator), key,
                                                &key_bytes);
    if (status != HEAP_OK) return status;
    pk_tree_io_t io = batch_index_io(batch);
    uint32_t *root = &batch->secondary_roots[index];
    if (*root == PAGE_FILE_NO_PAGE) {
        status = heap_from_pk(batch->heap, text_tree_create(&io, batch->heap->table_id, root));
        if (status != HEAP_OK) return status;
    }
    return heap_from_pk(batch->heap, text_tree_put(&io, batch->heap->table_id, root,
                                                  key, key_bytes, locator, false, NULL));
}

heap_status_t heap_batch_secondary_insert(heap_batch_t *batch, const value_view_t *values,
                                          size_t count, heap_locator_t locator)
{
    if (batch == NULL || batch->finished || batch->failed || values == NULL ||
        count != batch->heap->column_count) return HEAP_INVALID;
    for (size_t i = 0; i < batch->secondary_count; i++) {
        heap_status_t status = secondary_put_one(batch, i, &values[batch->secondary[i].column],
                                                 &values[batch->heap->key_column], locator);
        if (status != HEAP_OK) return fail(batch, status);
    }
    return HEAP_OK;
}

heap_status_t heap_batch_secondary_remove(heap_batch_t *batch, const value_view_t *values,
                                          size_t count, heap_locator_t locator)
{
    if (batch == NULL || batch->finished || batch->failed || values == NULL ||
        count != batch->heap->column_count) return HEAP_INVALID;
    pk_tree_io_t io = batch_index_io(batch);
    for (size_t i = 0; i < batch->secondary_count; i++) {
        const value_view_t *value = &values[batch->secondary[i].column];
        if (value->type == VALUE_NULL) continue;
        uint8_t key[TEXT_TREE_KEY_MAX_BYTES];
        size_t key_bytes;
        heap_status_t status = encode_secondary_key(value, secondary_tail(batch, i, &values[batch->heap->key_column],
                                                                          locator), key, &key_bytes);
        if (status != HEAP_OK) return fail(batch, status);
        heap_locator_t removed;
        pk_status_t result = text_tree_remove(&io, batch->heap->table_id,
                                              &batch->secondary_roots[i], key, key_bytes, &removed);
        if (result == PK_NOT_FOUND || (result == PK_OK && !same_locator(removed, locator))) {
            return fail(batch, HEAP_CORRUPT);
        }
        status = heap_from_pk(batch->heap, result);
        if (status != HEAP_OK) return fail(batch, status);
    }
    return HEAP_OK;
}

static bool same_locator(heap_locator_t a, heap_locator_t b)
{
    return a.directory_index == b.directory_index && a.slot == b.slot;
}

heap_status_t heap_batch_secondary_build(heap_batch_t *batch, size_t index, heap_t *source)
{
    if (batch == NULL || source == NULL || batch->finished || batch->failed ||
        index >= batch->secondary_count || batch->secondary_roots[index] != PAGE_FILE_NO_PAGE ||
        source != batch->heap) return HEAP_INVALID;
    pk_tree_io_t io = batch_index_io(batch);
    heap_status_t status = heap_from_pk(batch->heap, text_tree_create(&io, source->table_id,
                                                                      &batch->secondary_roots[index]));
    if (status != HEAP_OK) return fail(batch, status);
    size_t bytes = source->column_count * sizeof(value_t);
    if (!acct_reserve(&batch->ledger, bytes)) return fail(batch, HEAP_NOMEM);
    value_t *row = calloc(source->column_count, sizeof(*row));
    if (row == NULL) {
        acct_release(&batch->ledger, bytes);
        return fail(batch, HEAP_NOMEM);
    }
    heap_cursor_t *cursor = NULL;
    status = heap_cursor_open(source, &cursor);
    while (status == HEAP_OK) {
        heap_locator_t locator;
        heap_status_t next = heap_cursor_next(cursor, row, &locator);
        if (next == HEAP_DONE) break;
        if (next != HEAP_OK) {
            status = next;
            break;
        }
        status = secondary_put_one(batch, index, &row[batch->secondary[index].column].view,
                                   &row[batch->heap->key_column].view, locator);
        for (size_t column = 0; column < source->column_count; column++) value_clear(&row[column]);
    }
    heap_cursor_close(cursor);
    free(row);
    acct_release(&batch->ledger, bytes);
    return fail(batch, status);
}

/* 빈 heap의 첫 변경이 색인 root(빈 leaf)를 만든다. 키가 NULL인 행만 있어도 root는 있어야 색인이 이어진다. */
static heap_status_t index_ensure_root(heap_batch_t *batch)
{
    if (batch->index_root != PAGE_FILE_NO_PAGE) return HEAP_OK;
    pk_tree_io_t io = batch_index_io(batch);
    pk_status_t created = batch->heap->key_type == VALUE_INTEGER
                              ? pk_tree_create(&io, batch->heap->table_id, &batch->index_root)
                              : text_tree_create(&io, batch->heap->table_id, &batch->index_root);
    return heap_from_pk(batch->heap, created);
}

static heap_status_t index_put(heap_batch_t *batch, const value_view_t *key, heap_locator_t at)
{
    pk_tree_io_t io = batch_index_io(batch);
    pk_status_t inserted = batch->heap->key_type == VALUE_INTEGER
                               ? pk_tree_put(&io, batch->heap->table_id, &batch->index_root,
                                             key->integer, at, false, NULL)
                               : text_tree_put(&io, batch->heap->table_id, &batch->index_root,
                                               key->bytes, key->size_bytes, at, false, NULL);
    return heap_from_pk(batch->heap, inserted);
}

/* 색인에서 키를 지운다. 없거나 다른 행을 가리키면 색인이 heap과 어긋난 것이다. */
static heap_status_t index_remove(heap_batch_t *batch, const row_key_t *key, heap_locator_t at)
{
    pk_tree_io_t io = batch_index_io(batch);
    heap_locator_t removed;
    pk_status_t status = batch->heap->key_type == VALUE_INTEGER
                             ? pk_tree_remove(&io, batch->heap->table_id, &batch->index_root,
                                              key->integer, &removed)
                             : text_tree_remove(&io, batch->heap->table_id, &batch->index_root,
                                                key->bytes, key->size_bytes, &removed);
    if (status == PK_NOT_FOUND) return HEAP_CORRUPT;
    if (status == PK_OK && !same_locator(removed, at)) return HEAP_CORRUPT;
    return heap_from_pk(batch->heap, status);
}

static heap_status_t index_insert_row(heap_batch_t *batch, const value_view_t *values, heap_locator_t at)
{
    heap_status_t status = index_ensure_root(batch);
    if (status != HEAP_OK) return status;
    const value_view_t *cell = &values[batch->heap->key_column];
    return cell->type == VALUE_NULL ? HEAP_OK : index_put(batch, cell, at);
}

static bool same_key(const heap_t *heap, const row_key_t *old, const value_view_t *current)
{
    if (!old->present || current->type == VALUE_NULL) return false;
    if (heap->key_type == VALUE_INTEGER) return old->integer == current->integer;
    return old->size_bytes == current->size_bytes &&
           (old->size_bytes == 0 || memcmp(old->bytes, current->bytes, old->size_bytes) == 0);
}

/* 행이 옮겨졌거나 키가 바뀌었을 때만 색인을 바꾼다. */
static heap_status_t index_update_row(heap_batch_t *batch, row_key_t old, const value_view_t *values,
                                      heap_locator_t old_at, heap_locator_t new_at)
{
    const value_view_t *cell = &values[batch->heap->key_column];
    bool present = cell->type != VALUE_NULL;
    if (same_key(batch->heap, &old, cell)) {
        if (same_locator(old_at, new_at)) return HEAP_OK;
        pk_tree_io_t io = batch_index_io(batch);
        heap_locator_t previous;
        pk_status_t status = batch->heap->key_type == VALUE_INTEGER
                                 ? pk_tree_put(&io, batch->heap->table_id, &batch->index_root,
                                               old.integer, new_at, true, &previous)
                                 : text_tree_put(&io, batch->heap->table_id, &batch->index_root,
                                                 old.bytes, old.size_bytes, new_at, true, &previous);
        if (status == PK_OK && !same_locator(previous, old_at)) return HEAP_CORRUPT;
        return heap_from_pk(batch->heap, status);
    }
    if (old.present) {
        heap_status_t status = index_remove(batch, &old, old_at);
        if (status != HEAP_OK) return status;
    }
    return present ? index_put(batch, cell, new_at) : HEAP_OK;
}

/* 게시할 heap root가 정해진 뒤 색인 root에 그 번호를 새긴다. */
static heap_status_t index_restamp(heap_batch_t *batch, uint32_t heap_root_page)
{
    pk_tree_io_t io = batch_index_io(batch);
    pk_status_t stamped = batch->heap->key_type == VALUE_INTEGER
                              ? pk_tree_restamp(&io, batch->heap->table_id, &batch->index_root, heap_root_page)
                              : text_tree_restamp(&io, batch->heap->table_id, &batch->index_root, heap_root_page);
    return heap_from_pk(batch->heap, stamped);
}

static heap_status_t load_page(heap_batch_t *batch, size_t index, uint8_t **page)
{
    if (batch->bufs[index] == NULL) {
        if (!acct_reserve(&batch->ledger, RECORD_PAGE_BYTES)) return HEAP_NOMEM;
        uint8_t *buf = malloc(RECORD_PAGE_BYTES);
        if (buf == NULL) {
            acct_release(&batch->ledger, RECORD_PAGE_BYTES);
            return HEAP_NOMEM;
        }
        heap_status_t status = read_record_page(batch->heap, batch->entries[index], buf);
        if (status != HEAP_OK) {
            free(buf);
            acct_release(&batch->ledger, RECORD_PAGE_BYTES);
            return status;
        }
        batch->bufs[index] = buf;
    }
    *page = batch->bufs[index];
    return HEAP_OK;
}

/* 이 record page를 새 페이지로 옮긴다. 옛 페이지는 게시 성공 뒤 놓을 목록에 넣는다. */
static heap_status_t mark_dirty(heap_batch_t *batch, size_t index)
{
    if (batch->dirty[index]) return HEAP_OK;
    if (!list_reserve(&batch->old_pages, 1, &batch->ledger)) return HEAP_NOMEM;
    uint32_t fresh;
    heap_status_t status = batch_alloc(batch, &fresh);
    if (status != HEAP_OK) return status;
    batch->old_pages.items[batch->old_pages.len++] = batch->entries[index];
    batch->entries[index] = fresh;
    batch->dirty[index] = true;
    return HEAP_OK;
}

static heap_status_t append_page(heap_batch_t *batch, size_t *index, uint8_t **page)
{
    if (batch->entry_count == batch->entry_cap) {
        /* 세 배열을 새로 만들어 한 번에 바꾼다. 새 크기를 먼저 reserve하므로 순간 최대치는 old+new다. */
        size_t cap = batch->entry_cap * 2;
        size_t new_bytes = cap * BATCH_ENTRY_BYTES;
        size_t old_bytes = batch->entry_cap * BATCH_ENTRY_BYTES;
        if (!acct_reserve(&batch->ledger, new_bytes)) return HEAP_NOMEM;
        uint32_t *entries = malloc(cap * sizeof(*entries));
        uint16_t *free_bytes = malloc(cap * sizeof(*free_bytes));
        uint8_t **bufs = calloc(cap, sizeof(*bufs));
        bool *dirty = calloc(cap, sizeof(*dirty));
        if (entries == NULL || free_bytes == NULL || bufs == NULL || dirty == NULL) {
            free(entries);
            free(free_bytes);
            free(bufs);
            free(dirty);
            acct_release(&batch->ledger, new_bytes);
            return HEAP_NOMEM;
        }
        memcpy(entries, batch->entries, batch->entry_count * sizeof(*entries));
        memcpy(free_bytes, batch->free_bytes, batch->entry_count * sizeof(*free_bytes));
        memcpy(bufs, batch->bufs, batch->entry_count * sizeof(*bufs));
        memcpy(dirty, batch->dirty, batch->entry_count * sizeof(*dirty));
        free(batch->entries);
        free(batch->free_bytes);
        free(batch->bufs);
        free(batch->dirty);
        batch->entries = entries;
        batch->free_bytes = free_bytes;
        batch->bufs = bufs;
        batch->dirty = dirty;
        batch->entry_cap = cap;
        acct_release(&batch->ledger, old_bytes);
    }
    if (!acct_reserve(&batch->ledger, RECORD_PAGE_BYTES)) return HEAP_NOMEM;
    uint8_t *buf = malloc(RECORD_PAGE_BYTES);
    if (buf == NULL) {
        acct_release(&batch->ledger, RECORD_PAGE_BYTES);
        return HEAP_NOMEM;
    }
    uint32_t fresh;
    heap_status_t status = batch_alloc(batch, &fresh);
    if (status != HEAP_OK) {
        free(buf);
        acct_release(&batch->ledger, RECORD_PAGE_BYTES);
        return status;
    }
    record_page_init(buf);
    size_t i = batch->entry_count++;
    batch->entries[i] = fresh;
    batch->free_bytes[i] = RECORD_PAGE_BYTES - 8u;
    batch->bufs[i] = buf;
    batch->dirty[i] = true;
    *index = i;
    *page = buf;
    return HEAP_OK;
}

/* record_page의 insert·update·delete는 페이지 한 장을 임시로 만든다. 그만큼을 잠시 계상한다. */
static record_page_status_t page_insert(heap_batch_t *batch, uint8_t *page, record_slice_t slice,
                                        uint16_t *slot)
{
    if (!acct_reserve(&batch->ledger, RECORD_PAGE_BYTES)) return RECORD_PAGE_NOMEM;
    record_page_status_t status = record_page_insert(page, slice, slot);
    acct_release(&batch->ledger, RECORD_PAGE_BYTES);
    return status;
}

static record_page_status_t page_update(heap_batch_t *batch, uint8_t *page, uint16_t slot,
                                        record_slice_t slice)
{
    if (!acct_reserve(&batch->ledger, RECORD_PAGE_BYTES)) return RECORD_PAGE_NOMEM;
    record_page_status_t status = record_page_update(page, slot, slice);
    acct_release(&batch->ledger, RECORD_PAGE_BYTES);
    return status;
}

static record_page_status_t page_delete(heap_batch_t *batch, uint8_t *page, uint16_t slot)
{
    if (!acct_reserve(&batch->ledger, RECORD_PAGE_BYTES)) return RECORD_PAGE_NOMEM;
    record_page_status_t status = record_page_delete(page, slot);
    acct_release(&batch->ledger, RECORD_PAGE_BYTES);
    return status;
}

/* ── 행 인코딩 ── */

static heap_status_t check_values(const heap_t *heap, const value_view_t *values, size_t count)
{
    if (values == NULL || count != heap->column_count) return HEAP_INVALID;
    for (size_t i = 0; i < count; i++) {
        const value_view_t *v = &values[i];
        if (v->type == VALUE_NULL) {
            if (v->bytes != NULL || v->size_bytes != 0) return HEAP_INVALID;
            continue;
        }
        if (v->type != heap->types[i]) return HEAP_INVALID;
        switch (v->type) {
            case VALUE_INTEGER: break;
            case VALUE_REAL:
                if (!isfinite(v->real)) return HEAP_INVALID;
                break;
            case VALUE_TEXT: {
                value_t probe = {{VALUE_NULL, 0, 0, NULL, 0}, false};
                if (!acct_reserve(&heap->ledger, v->size_bytes)) return HEAP_NOMEM;
                value_status_t status = value_clone(v, &probe);
                value_clear(&probe);
                acct_release(&heap->ledger, v->size_bytes);
                if (status == VALUE_NOMEM) return HEAP_NOMEM;
                if (status != VALUE_OK) return HEAP_INVALID;
                break;
            }
            case VALUE_BLOB:
                if (v->size_bytes > 0 && v->bytes == NULL) return HEAP_INVALID;
                break;
            default: return HEAP_INVALID;
        }
    }
    return HEAP_OK;
}

/* 인코딩이 길면 큰 TEXT·BLOB부터 overflow로 보낸다. 실패하면 이번 호출이 만든 체인은 되돌린다. */
/*
 * 성공하면 *out 버퍼를 호출자가 free하고, *encoded_reserved만큼을 그 뒤에 돌려준다(acct_release).
 * 인코딩 결과 크기는 8 + 필드마다 (5 + 길이)이며 길이는 INTEGER·REAL이 8, TEXT·BLOB이 바이트 수다.
 * overflow로 보내도 더 작아질 뿐이므로 이 상한을 인코딩 반복 동안 한 번만 reserve한다.
 */
static heap_status_t encode_row(heap_batch_t *batch, const value_view_t *values, size_t count,
                                record_buffer_t *out, size_t *encoded_reserved)
{
    heap_t *heap = batch->heap;
    const mem_ledger_t *ledger = &batch->ledger;
    *encoded_reserved = 0;
    heap_status_t status = check_values(heap, values, count);
    if (status != HEAP_OK) return status;

    size_t n = heap->column_count;
    size_t field_count = ROW_FIELDS(n);
    size_t fields_bytes, scratch_bytes, spilled_bytes;
    if (!mul_ok(field_count, sizeof(record_value_t), &fields_bytes) ||
        !mul_ok(n, 8, &scratch_bytes) || !mul_ok(n, sizeof(overflow_ref_t), &spilled_bytes)) {
        return HEAP_NOMEM;
    }
    size_t temp_bytes = fields_bytes + scratch_bytes + spilled_bytes + n * sizeof(bool);
    size_t encoded_bound = 8 + field_count * 13;
    for (size_t i = 0; i < n; i++) {
        if (values[i].type == VALUE_TEXT || values[i].type == VALUE_BLOB) {
            if (encoded_bound > SIZE_MAX - values[i].size_bytes) return HEAP_NOMEM;
            encoded_bound += values[i].size_bytes;
        }
    }
    if (encoded_bound > SIZE_MAX - temp_bytes || !acct_reserve(ledger, temp_bytes + encoded_bound)) {
        return HEAP_NOMEM;
    }
    record_value_t *fields = calloc(field_count, sizeof(*fields));
    uint8_t (*scratch)[8] = calloc(n, sizeof(*scratch));
    overflow_ref_t *spilled = calloc(n, sizeof(*spilled));
    bool *is_spilled = calloc(n, sizeof(*is_spilled));
    if (fields == NULL || scratch == NULL || spilled == NULL || is_spilled == NULL) {
        free(fields);
        free(scratch);
        free(spilled);
        free(is_spilled);
        acct_release(ledger, temp_bytes + encoded_bound);
        return HEAP_NOMEM;
    }
    fields[0] = (record_value_t){RECORD_INTEGER, ROW_VERSION, NULL, 0};
    for (size_t i = 0; i < n; i++) {
        record_value_t *kind = &fields[1 + 2 * i];
        record_value_t *payload = &fields[2 + 2 * i];
        const value_view_t *v = &values[i];
        *kind = (record_value_t){RECORD_INTEGER, KIND_INLINE, NULL, 0};
        switch (v->type) {
            case VALUE_NULL:
                kind->integer = KIND_NULL;
                *payload = (record_value_t){RECORD_NULL, 0, NULL, 0};
                break;
            case VALUE_INTEGER: *payload = (record_value_t){RECORD_INTEGER, v->integer, NULL, 0}; break;
            case VALUE_REAL: {
                uint64_t bits;
                memcpy(&bits, &v->real, sizeof(bits));
                le_put_u64(scratch[i], bits);
                *payload = (record_value_t){RECORD_BLOB, 0, scratch[i], REAL_BYTES};
                break;
            }
            case VALUE_TEXT: *payload = (record_value_t){RECORD_TEXT, 0, v->bytes, v->size_bytes}; break;
            default: *payload = (record_value_t){RECORD_BLOB, 0, v->bytes, v->size_bytes}; break;
        }
    }

    batch_io_ctx_t ctx;
    overflow_io_t io = batch_io(&ctx, batch);
    record_buffer_t encoded = {NULL, 0};
    status = HEAP_OK;
    for (;;) {
        record_status_t coded = record_encode(fields, field_count, &encoded);
        if (coded == RECORD_NOMEM) { status = HEAP_NOMEM; break; }
        if (coded != RECORD_OK) { status = HEAP_LIMIT; break; }
        if (encoded.size_bytes <= HEAP_INLINE_LIMIT_BYTES) break;

        size_t pick = n;
        size_t best = SPILL_MIN_BYTES;
        for (size_t i = 0; i < n; i++) {
            const record_value_t *payload = &fields[2 + 2 * i];
            if (is_spilled[i] || fields[1 + 2 * i].integer != KIND_INLINE) continue;
            if (heap->types[i] != VALUE_TEXT && heap->types[i] != VALUE_BLOB) continue;
            if (payload->size_bytes > best) {
                best = payload->size_bytes;
                pick = i;
            }
        }
        if (pick == n) {
            if (encoded.size_bytes > HEAP_MAX_ROW_BYTES) status = HEAP_LIMIT;
            break;
        }
        free(encoded.bytes);
        encoded.bytes = NULL;
        record_value_t *payload = &fields[2 + 2 * pick];
        if (payload->size_bytes > UINT32_MAX) { status = HEAP_LIMIT; break; }
        overflow_ref_t ref;
        size_t write_bytes = overflow_write_bound(io.page_bytes, payload->size_bytes);
        if (!acct_reserve(ledger, write_bytes)) { status = HEAP_NOMEM; break; }
        overflow_status_t written = overflow_write(&io, payload->bytes, payload->size_bytes, &ref);
        acct_release(ledger, write_bytes);
        if (written != OVERFLOW_OK) {
            status = map_overflow(heap, written, &ctx.inner);
            break;
        }
        spilled[pick] = ref;
        is_spilled[pick] = true;
        le_put_u32(scratch[pick], ref.head);
        le_put_u32(scratch[pick] + 4, ref.size_bytes);
        fields[1 + 2 * pick].integer = KIND_OVERFLOW;
        *payload = (record_value_t){RECORD_BLOB, 0, scratch[pick], OVERFLOW_REF_BYTES};
    }

    if (status != HEAP_OK) {
        free(encoded.bytes);
        encoded.bytes = NULL;
        for (size_t i = 0; i < n; i++) {
            if (!is_spilled[i]) continue;
            /* reserve가 거절되면 건너뛴다. 이 batch가 할당한 페이지는 fresh 목록이 abort에서 놓는다 */
            size_t free_bytes = overflow_free_bound(io.page_bytes, spilled[i].size_bytes);
            if (!acct_reserve(ledger, free_bytes)) continue;
            overflow_free(&io, spilled[i]);
            acct_release(ledger, free_bytes);
        }
        acct_release(ledger, encoded_bound);
    } else {
        *out = encoded;
        *encoded_reserved = encoded_bound;
    }
    free(fields);
    free(scratch);
    free(spilled);
    free(is_spilled);
    acct_release(ledger, temp_bytes);
    return status;
}

/* 지운·바뀐 행이 가리키던 overflow 체인을 게시 성공 뒤 놓을 목록에 넣는다. */
static heap_status_t release_row_chains(heap_batch_t *batch, record_slice_t slice)
{
    heap_t *heap = batch->heap;
    size_t field_bytes;
    if (!mul_ok(ROW_FIELDS(heap->column_count), sizeof(record_value_t), &field_bytes) ||
        !acct_reserve(&batch->ledger, field_bytes)) {
        return HEAP_NOMEM;
    }
    record_value_t *fields = malloc(field_bytes);
    if (fields == NULL) {
        acct_release(&batch->ledger, field_bytes);
        return HEAP_NOMEM;
    }
    heap_status_t status = parse_fields(heap, slice, fields, ROW_FIELDS(heap->column_count));
    batch_io_ctx_t ctx;
    overflow_io_t io = batch_io(&ctx, batch);
    for (size_t i = 0; status == HEAP_OK && i < heap->column_count; i++) {
        if (fields[1 + 2 * i].integer != KIND_OVERFLOW) continue;
        overflow_ref_t ref = ref_from(&fields[2 + 2 * i]);
        size_t transient = overflow_free_bound(io.page_bytes, ref.size_bytes);
        if (!acct_reserve(&batch->ledger, transient)) {
            status = HEAP_NOMEM;
            break;
        }
        overflow_status_t freed = overflow_free(&io, ref);
        acct_release(&batch->ledger, transient);
        if (freed != OVERFLOW_OK) status = map_overflow(heap, freed, &ctx.inner);
    }
    free(fields);
    acct_release(&batch->ledger, field_bytes);
    return status;
}

/* ── 연산 ── */

static heap_status_t refresh_page_hint(heap_batch_t *batch, size_t index, const uint8_t *page)
{
    size_t available;
    if (record_page_available(page, &available) != RECORD_PAGE_OK) return HEAP_CORRUPT;
    batch->free_bytes[index] = (uint16_t)available;
    return HEAP_OK;
}

static heap_status_t insert_slice(heap_batch_t *batch, record_slice_t slice, heap_locator_t *locator)
{
    uint8_t *page = NULL;
    size_t index = 0;
    heap_status_t status;
    size_t needed = slice.size_bytes + 4u;
    for (;;) {
        size_t candidate = SIZE_MAX;
        for (size_t i = 0; i < batch->entry_count; i++) {
            if (batch->free_bytes[i] < needed) continue;
            if (candidate == SIZE_MAX || batch->free_bytes[i] < batch->free_bytes[candidate] ||
                (batch->free_bytes[i] == batch->free_bytes[candidate] &&
                 batch->dirty[i] && !batch->dirty[candidate])) candidate = i;
        }
        if (candidate == SIZE_MAX) break;
        index = candidate;
        status = load_page(batch, index, &page);
        if (status != HEAP_OK) return status;
        status = refresh_page_hint(batch, index, page);
        if (status != HEAP_OK) return status;
        size_t actual = batch->free_bytes[index];
        if (actual < needed) continue;  /* 힌트는 권한이 아니므로 실제 페이지를 다시 확인한다. */
        uint16_t slot;
        record_page_status_t put = page_insert(batch, page, slice, &slot);
        if (put == RECORD_PAGE_OK) {
            status = mark_dirty(batch, index);
            if (status != HEAP_OK) return status;
            status = refresh_page_hint(batch, index, page);
            if (status != HEAP_OK) return status;
            *locator = (heap_locator_t){(uint32_t)index, slot};
            return HEAP_OK;
        }
        if (put != RECORD_PAGE_FULL) return put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
        batch->free_bytes[index] = 0;  /* 슬롯 수 한계라면 남은 바이트가 있어도 후보가 아니다. */
    }
    status = append_page(batch, &index, &page);
    if (status != HEAP_OK) return status;
    uint16_t slot;
    record_page_status_t put = page_insert(batch, page, slice, &slot);
    if (put == RECORD_PAGE_FULL) return HEAP_LIMIT;
    if (put != RECORD_PAGE_OK) return put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
    status = refresh_page_hint(batch, index, page);
    if (status != HEAP_OK) return status;
    *locator = (heap_locator_t){(uint32_t)index, slot};
    return HEAP_OK;
}

heap_status_t heap_batch_insert(heap_batch_t *batch, const value_view_t *values, size_t count,
                                heap_locator_t *locator)
{
    if (batch == NULL || locator == NULL || batch->finished) return HEAP_INVALID;
    if (batch->failed) return HEAP_BATCH_FAILED;
    record_buffer_t encoded;
    size_t encoded_reserved;
    heap_status_t status = encode_row(batch, values, count, &encoded, &encoded_reserved);
    if (status != HEAP_OK) return fail(batch, status);
    heap_locator_t placed;
    status = insert_slice(batch, (record_slice_t){encoded.bytes, encoded.size_bytes}, &placed);
    free(encoded.bytes);
    acct_release(&batch->ledger, encoded_reserved);
    if (status == HEAP_OK && batch->heap->indexed) status = index_insert_row(batch, values, placed);
    if (status != HEAP_OK) return fail(batch, status);
    *locator = placed;
    return HEAP_OK;
}

heap_status_t heap_batch_get(heap_batch_t *batch, heap_locator_t locator, value_t *row)
{
    if (batch == NULL || row == NULL || batch->finished) return HEAP_INVALID;
    if (batch->failed) return HEAP_BATCH_FAILED;
    if (locator.directory_index >= batch->entry_count) return HEAP_NOT_FOUND;
    uint8_t *page;
    heap_status_t status = load_page(batch, locator.directory_index, &page);
    if (status != HEAP_OK) return fail(batch, status);
    size_t charged;
    status = row_from_slot(batch->heap, page, locator.slot, row, NULL, &charged);
    return fail(batch, status);
}

heap_status_t heap_batch_update(heap_batch_t *batch, heap_locator_t locator,
                                const value_view_t *values, size_t count,
                                heap_locator_t *new_locator)
{
    if (batch == NULL || new_locator == NULL || batch->finished) return HEAP_INVALID;
    if (batch->failed) return HEAP_BATCH_FAILED;
    if (locator.directory_index >= batch->entry_count) return HEAP_NOT_FOUND;
    size_t index = locator.directory_index;
    uint8_t *page;
    heap_status_t status = load_page(batch, index, &page);
    if (status != HEAP_OK) return fail(batch, status);
    record_slice_t old;
    record_page_status_t got = record_page_get(page, locator.slot, &old);
    if (got == RECORD_PAGE_NOT_FOUND) return HEAP_NOT_FOUND;
    if (got != RECORD_PAGE_OK) return fail(batch, HEAP_CORRUPT);
    row_key_t old_key = {0};
    if (batch->heap->indexed) {
        status = slice_key(batch->heap, old, &old_key);
        if (status != HEAP_OK) return fail(batch, status);
    }

    record_buffer_t encoded;
    size_t encoded_reserved;
    status = encode_row(batch, values, count, &encoded, &encoded_reserved);
    if (status != HEAP_OK) return fail(batch, status);
    /* 옛 행의 체인은 페이지를 바꾸기 전에 읽어 둔다. */
    status = release_row_chains(batch, old);
    if (status != HEAP_OK) {
        free(encoded.bytes);
        acct_release(&batch->ledger, encoded_reserved);
        return fail(batch, status);
    }
    record_slice_t slice = {encoded.bytes, encoded.size_bytes};
    record_page_status_t put = page_update(batch, page, locator.slot, slice);
    if (put == RECORD_PAGE_OK) {
        status = mark_dirty(batch, index);
        if (status == HEAP_OK) status = refresh_page_hint(batch, index, page);
        if (status == HEAP_OK) *new_locator = locator;
    } else if (put == RECORD_PAGE_FULL) {
        status = mark_dirty(batch, index);
        if (status == HEAP_OK) {
            record_page_status_t removed = page_delete(batch, page, locator.slot);
            if (removed == RECORD_PAGE_NOMEM) status = HEAP_NOMEM;
            else if (removed != RECORD_PAGE_OK) status = HEAP_CORRUPT;
            else status = refresh_page_hint(batch, index, page);
        }
        if (status == HEAP_OK) status = insert_slice(batch, slice, new_locator);
    } else {
        status = put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
    }
    if (status == HEAP_OK && batch->heap->indexed) {
        status = index_update_row(batch, old_key, values, locator, *new_locator);
    }
    free(encoded.bytes);
    acct_release(&batch->ledger, encoded_reserved);
    return fail(batch, status);
}

heap_status_t heap_batch_delete(heap_batch_t *batch, heap_locator_t locator)
{
    if (batch == NULL || batch->finished) return HEAP_INVALID;
    if (batch->failed) return HEAP_BATCH_FAILED;
    if (locator.directory_index >= batch->entry_count) return HEAP_NOT_FOUND;
    size_t index = locator.directory_index;
    uint8_t *page;
    heap_status_t status = load_page(batch, index, &page);
    if (status != HEAP_OK) return fail(batch, status);
    record_slice_t old;
    record_page_status_t got = record_page_get(page, locator.slot, &old);
    if (got == RECORD_PAGE_NOT_FOUND) return HEAP_NOT_FOUND;
    if (got != RECORD_PAGE_OK) return fail(batch, HEAP_CORRUPT);
    row_key_t old_key = {0};
    if (batch->heap->indexed) {
        status = slice_key(batch->heap, old, &old_key);
        if (status != HEAP_OK) return fail(batch, status);
    }
    status = release_row_chains(batch, old);
    if (status == HEAP_OK) status = mark_dirty(batch, index);
    if (status == HEAP_OK) {
        record_page_status_t removed = page_delete(batch, page, locator.slot);
        if (removed == RECORD_PAGE_NOMEM) status = HEAP_NOMEM;
        else if (removed != RECORD_PAGE_OK) status = HEAP_CORRUPT;
        else status = refresh_page_hint(batch, index, page);
    }
    if (status == HEAP_OK && old_key.present) status = index_remove(batch, &old_key, locator);
    return fail(batch, status);
}

/* ── 게시 준비·정리 ── */

static heap_status_t write_directory_page(heap_t *heap, uint32_t page, const char *magic, uint16_t version,
                                          const uint32_t *items, const uint16_t *free_bytes, size_t count)
{
    uint8_t body[PAGE_BODY_BYTES];
    memset(body, 0, sizeof(body));
    memcpy(body, magic, 4);
    body[4] = (uint8_t)version;
    le_put_u32(body + 8, heap->table_id);
    le_put_u32(body + 16, (uint32_t)count);
    bool hinted = free_bytes != NULL;
    for (size_t k = 0; k < count; k++) {
        size_t offset = HEAP_DIRECTORY_HEADER_BYTES + k * (hinted ? 6u : 4u);
        le_put_u32(body + offset, items[k]);
        if (hinted) {
            body[offset + 4u] = (uint8_t)free_bytes[k];
            body[offset + 5u] = (uint8_t)(free_bytes[k] >> 8);
        }
    }
    page_file_status_t written = page_file_write(heap->file, page, PAGE_KIND_DIRECTORY, heap->table_id, body);
    return written == PAGE_FILE_OK ? HEAP_OK : map_pf(heap, written);
}

/* 옛 트리의 leaf번째 잎이 담던 항목이 이번 batch에서 하나도 바뀌지 않았으면 옛 잎을 그대로 쓸 수 있다. */
static bool leaf_unchanged(const heap_batch_t *batch, size_t leaf)
{
    const heap_t *heap = batch->heap;
    if (!heap->is_tree || !heap->has_hints || leaf + 1 >= heap->dir_count) return false;
    size_t first = leaf * HEAP_HINT_DIRECTORY_ENTRIES;
    size_t end = first + HEAP_HINT_DIRECTORY_ENTRIES;
    if (end > batch->entry_count) end = batch->entry_count;
    for (size_t i = first; i < end; i++) {
        if (batch->dirty[i] || batch->free_bytes[i] != heap->free_bytes[i]) return false;
    }
    return true;
}

/* 새 디렉터리의 i번째 페이지가 옛 트리의 바뀌지 않은 잎이면 true다. 새로 할당한 번호는 살아 있는 옛 페이지와 겹치지 않는다. */
static bool is_reused_directory(const heap_t *heap, size_t pages, const uint32_t *ids, size_t i)
{
    return heap->is_tree && pages > 1 && 0 < i && i < pages && i < heap->dir_count && ids[i] == heap->dirs[i];
}

/*
 * 새 디렉터리의 페이지 번호를 정한다. 잎이 둘 이상이면 ids[0]이 루트, ids[1..]이 잎이고 하나면 ids[0]이 그 한 장이다.
 * 바뀌지 않은 잎은 옛 번호를 그대로 두고 나머지는 새로 할당한다.
 */
static heap_status_t assign_directory_pages(heap_batch_t *batch, size_t pages, uint32_t *ids)
{
    for (size_t i = 0; i < pages; i++) {
        if (pages > 1 && i > 0 && leaf_unchanged(batch, i - 1)) {
            ids[i] = batch->heap->dirs[i];
            continue;
        }
        heap_status_t status = batch_alloc(batch, &ids[i]);
        if (status != HEAP_OK) return status;
    }
    return HEAP_OK;
}

static heap_status_t write_new_directories(heap_batch_t *batch, size_t pages, const uint32_t *ids)
{
    heap_t *heap = batch->heap;
    size_t lead = pages > 1 ? 1 : 0;
    for (size_t i = lead; i < pages; i++) {
        if (is_reused_directory(heap, pages, ids, i)) continue;
        size_t first = (i - lead) * HEAP_HINT_DIRECTORY_ENTRIES;
        size_t take = batch->entry_count - first;
        if (take > HEAP_HINT_DIRECTORY_ENTRIES) take = HEAP_HINT_DIRECTORY_ENTRIES;
        heap_status_t status = write_directory_page(heap, ids[i], HEAP_HINT_DIRECTORY_MAGIC,
                                                    HEAP_HINT_DIRECTORY_VERSION,
                                                    batch->entries + first, batch->free_bytes + first, take);
        if (status != HEAP_OK) return status;
    }
    if (lead == 0) return HEAP_OK;
    return write_directory_page(heap, ids[0], HEAP_TREE_ROOT_MAGIC,
                                HEAP_TREE_ROOT_VERSION, ids + 1, NULL, pages - 1);
}

static void retire_old_directories(heap_batch_t *batch, size_t pages, const uint32_t *ids)
{
    const heap_t *heap = batch->heap;
    for (size_t i = 0; i < heap->dir_count; i++) {
        if (is_reused_directory(heap, pages, ids, i)) continue;
        batch->old_pages.items[batch->old_pages.len++] = heap->dirs[i];
    }
}

/*
 * 바뀐 record page를 가리키는 새 디렉터리를 쓴다. 새 잎은 모두 LHD3이며 한 장의 한도를 넘으면
 * LHD2 루트 아래에 둔다. 트리 heap은 바뀐 잎과 루트만 새로 쓰고 옛 형식은 첫 변경 때 교체한다.
 */
static heap_status_t publish_directories(heap_batch_t *batch, uint32_t *new_root)
{
    heap_t *heap = batch->heap;
    size_t leaves = (batch->entry_count + HEAP_HINT_DIRECTORY_ENTRIES - 1) / HEAP_HINT_DIRECTORY_ENTRIES;
    size_t pages = leaves > 1 ? leaves + 1 : leaves;
    *new_root = PAGE_FILE_NO_PAGE;
    if (leaves > HEAP_DIRECTORY_ENTRIES) return HEAP_LIMIT;
    if (!list_reserve(&batch->old_pages, heap->dir_count, &batch->ledger)) return HEAP_NOMEM;
    size_t ids_bytes = pages * sizeof(uint32_t);
    bool ids_reserved = acct_reserve(&batch->ledger, ids_bytes);
    uint32_t *ids = ids_reserved && pages > 0 ? malloc(ids_bytes) : NULL;
    if (pages > 0 && ids == NULL) {
        if (ids_reserved) acct_release(&batch->ledger, ids_bytes);
        return HEAP_NOMEM;
    }
    heap_status_t status = pages > 0 ? assign_directory_pages(batch, pages, ids) : HEAP_OK;
    if (status == HEAP_OK && pages > 0) status = write_new_directories(batch, pages, ids);
    if (status == HEAP_OK) {
        retire_old_directories(batch, pages, ids);
        if (pages > 0) *new_root = ids[0];
    }
    free(ids);
    if (ids_reserved) acct_release(&batch->ledger, ids_bytes);
    return status;
}

heap_status_t heap_batch_finish(heap_batch_t *batch, heap_batch_result_t *result)
{
    if (batch == NULL || result == NULL || batch->finished) return HEAP_INVALID;
    if (batch->failed) return HEAP_BATCH_FAILED;
    heap_t *heap = batch->heap;

    bool changed = batch->fresh.len > 0 || batch->old_pages.len > 0;
    if (!changed) {
        *result = (heap_batch_result_t){heap->root, NULL, 0, NULL, 0, heap->index_root,
                                         batch->secondary_roots, batch->secondary_count};
        batch->finished = true;
        return HEAP_OK;
    }

    heap_status_t status = HEAP_OK;
    for (size_t i = 0; i < batch->entry_count && status == HEAP_OK; i++) {
        if (!batch->dirty[i]) continue;
        page_file_status_t written = page_file_write(heap->file, batch->entries[i], PAGE_KIND_RECORD,
                                                     heap->table_id, batch->bufs[i]);
        if (written != PAGE_FILE_OK) status = map_pf(heap, written);
    }
    uint32_t new_root = PAGE_FILE_NO_PAGE;
    if (status == HEAP_OK) status = publish_directories(batch, &new_root);
    if (status == HEAP_OK && heap->indexed && batch->index_root != PAGE_FILE_NO_PAGE) {
        status = index_restamp(batch, new_root);
    }
    pk_tree_io_t io = batch_index_io(batch);
    for (size_t i = 0; i < batch->secondary_count && status == HEAP_OK; i++) {
        if (batch->secondary_roots[i] == PAGE_FILE_NO_PAGE) continue;
        status = heap_from_pk(heap, text_tree_restamp(&io, heap->table_id,
                                                      &batch->secondary_roots[i], new_root));
    }
    if (status != HEAP_OK) return fail(batch, status);

    batch->new_root = new_root;
    batch->finished = true;
    *result = (heap_batch_result_t){new_root, batch->old_pages.items, batch->old_pages.len,
                                    batch->fresh.items, batch->fresh.len, batch->index_root,
                                    batch->secondary_roots, batch->secondary_count};
    return HEAP_OK;
}

heap_status_t heap_batch_release_old(heap_batch_t *batch)
{
    if (batch == NULL || !batch->finished) return HEAP_INVALID;
    heap_status_t first = HEAP_OK;
    for (size_t i = 0; i < batch->old_pages.len; i++) {
        heap_status_t status = map_pf(batch->heap, page_file_release(batch->heap->file, batch->old_pages.items[i]));
        if (status != HEAP_OK && first == HEAP_OK) first = status;
    }
    batch->old_pages.len = 0;
    return first;
}

heap_status_t heap_batch_abort(heap_batch_t *batch)
{
    if (batch == NULL) return HEAP_INVALID;
    heap_status_t first = HEAP_OK;
    for (size_t i = 0; i < batch->fresh.len; i++) {
        heap_status_t status = map_pf(batch->heap, page_file_release(batch->heap->file, batch->fresh.items[i]));
        if (status != HEAP_OK && first == HEAP_OK) first = status;
    }
    batch->fresh.len = 0;
    batch->old_pages.len = 0;
    batch->failed = true;
    return first;
}

size_t heap_batch_fresh_count(const heap_batch_t *batch)
{
    return batch != NULL ? batch->fresh.len : 0;
}
