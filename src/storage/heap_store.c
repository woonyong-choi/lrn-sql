#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "storage/heap_store.h"
#include "storage/overflow.h"
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

/* ledger는 선택이다. 있으면 이 모듈의 모든 할당을 MEM_SCOPE_QUERY로 먼저 reserve한다. */
struct heap {
    mem_ledger_t ledger;
    page_file_t *file;
    uint32_t table_id;
    uint32_t root;
    value_type_t *types;
    size_t column_count;
    uint32_t *entries;      /* record page ID, heap 순서 */
    size_t entry_count;
    size_t entry_cap;       /* entries가 쥔 계상 용량 */
    uint32_t *dirs;         /* 디렉터리 페이지 ID, 체인 순서 */
    size_t dir_count;
    size_t dir_cap;
    page_file_status_t last_status;
};

struct heap_batch {
    mem_ledger_t ledger;
    heap_t *heap;
    uint32_t *entries;
    uint8_t **bufs;         /* 읽어 온 또는 새로 만든 record page 본문. NULL이면 아직 안 읽음 */
    bool *dirty;            /* true면 entries[i]가 이 batch가 새로 할당한 페이지 */
    size_t entry_count;
    size_t entry_cap;
    u32_list_t old_pages;   /* 게시 성공 뒤 release */
    u32_list_t fresh;       /* 이 batch가 할당했고 아직 살아 있는 페이지 */
    bool failed;
    bool finished;
    uint32_t new_root;
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

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

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
    acct_release(ledger, list->cap * sizeof(uint32_t));
    free(list->items);
    *list = (u32_list_t){NULL, 0, 0};
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
    ref.head = get_u32(payload->bytes);
    ref.size_bytes = get_u32(payload->bytes + 4);
    return ref;
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
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
static heap_status_t decode_row(heap_t *heap, const record_value_t *f, value_t *out, size_t *charged)
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
                uint64_t bits = get_u64(payload->bytes);
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
                   (heap->entry_cap + heap->dir_cap) * sizeof(uint32_t);
    free(heap->types);
    free(heap->entries);
    free(heap->dirs);
    free(heap);
    acct_release(&ledger, bytes);
}

static heap_status_t read_directory(heap_t *heap, const page_file_control_t *control)
{
    uint32_t page = heap->root;
    const mem_ledger_t *ledger = &heap->ledger;
    u32_list_t entries = {NULL, 0, 0};
    u32_list_t dirs = {NULL, 0, 0};
    heap_status_t result = HEAP_OK;
    uint8_t body[PAGE_BODY_BYTES];

    while (page != PAGE_FILE_NO_PAGE && result == HEAP_OK) {
        if (dirs.len >= control->page_count) { result = HEAP_CORRUPT; break; }  /* 순환 */
        page_file_page_info_t info;
        page_file_status_t status = page_file_page_info(heap->file, page, &info);
        if (status != PAGE_FILE_OK) { result = map_pf(heap, status); break; }
        if (info.kind != PAGE_KIND_DIRECTORY) {
            result = dirs.len == 0 ? HEAP_UNSUPPORTED_LOCATOR : HEAP_CORRUPT;
            break;
        }
        if (info.table_id != heap->table_id) { result = HEAP_CORRUPT; break; }
        status = page_file_read(heap->file, page, PAGE_KIND_DIRECTORY, heap->table_id, body);
        if (status != PAGE_FILE_OK) { result = map_pf(heap, status); break; }
        if (memcmp(body, HEAP_DIRECTORY_MAGIC, 4) != 0 || get_u16(body + 4) != HEAP_DIRECTORY_VERSION ||
            get_u16(body + 6) != 0 || get_u32(body + 8) != heap->table_id) {
            result = dirs.len == 0 ? HEAP_UNSUPPORTED_LOCATOR : HEAP_CORRUPT;
            break;
        }
        uint32_t count = get_u32(body + 16);
        if (count > HEAP_DIRECTORY_ENTRIES) { result = HEAP_CORRUPT; break; }
        for (size_t i = HEAP_DIRECTORY_HEADER_BYTES + (size_t)count * 4; i < PAGE_BODY_BYTES; i++) {
            if (body[i] != 0) { result = HEAP_CORRUPT; break; }
        }
        if (result != HEAP_OK) break;
        if (!list_push(&dirs, page, ledger) || !list_reserve(&entries, count, ledger)) { result = HEAP_NOMEM; break; }
        for (uint32_t i = 0; i < count; i++) {
            entries.items[entries.len++] = get_u32(body + HEAP_DIRECTORY_HEADER_BYTES + (size_t)i * 4);
        }
        page = get_u32(body + 12);
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
                if (all[i] < PAGE_FILE_CONTROL_PAGES || all[i] >= control->page_count ||
                    (i > 0 && all[i] == all[i - 1])) {
                    result = HEAP_CORRUPT;
                    break;
                }
            }
            free(all);
            acct_release(ledger, all_bytes);
        }
    }
    if (result != HEAP_OK) {
        list_free(&entries, ledger);
        list_free(&dirs, ledger);
        return result;
    }
    heap->entries = entries.items;
    heap->entry_count = entries.len;
    heap->entry_cap = entries.cap;
    heap->dirs = dirs.items;
    heap->dir_count = dirs.len;
    heap->dir_cap = dirs.cap;
    return HEAP_OK;
}

heap_status_t heap_open(page_file_t *file, uint32_t table_id, uint32_t heap_root_page,
                        const heap_schema_t *schema, heap_t **out)
{
    return heap_open_ex(file, table_id, heap_root_page, schema, NULL, out);
}

heap_status_t heap_open_ex(page_file_t *file, uint32_t table_id, uint32_t heap_root_page,
                           const heap_schema_t *schema, const mem_ledger_t *ledger, heap_t **out)
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
                                   size_t *charged)
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
    if (status == HEAP_OK) status = decode_row(heap, fields, row, charged);
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
    if (heap != NULL) acct_release(&heap->ledger, count * sizeof(uint32_t));
    free(pages);
}

heap_status_t heap_get(heap_t *heap, heap_locator_t locator, value_t *row)
{
    if (heap == NULL || row == NULL) return HEAP_INVALID;
    if (locator.directory_index >= heap->entry_count) return HEAP_NOT_FOUND;
    uint8_t page[RECORD_PAGE_BYTES];
    heap_status_t status = read_record_page(heap, heap->entries[locator.directory_index], page);
    if (status != HEAP_OK) return status;
    size_t charged;
    return row_from_slot(heap, page, locator.slot, row, &charged);
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

heap_status_t heap_cursor_next(heap_cursor_t *cursor, value_t *row, heap_locator_t *locator)
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
            cursor->slot_count = get_u16(cursor->page + RECORD_PAGE_SLOT_COUNT_OFFSET);
            cursor->next_slot = 0;
        }
        while (cursor->next_slot < cursor->slot_count) {
            uint16_t slot = cursor->next_slot;
            size_t charged;
            heap_status_t status = row_from_slot(heap, cursor->page, slot, row, &charged);
            if (status == HEAP_NOT_FOUND) {
                cursor->next_slot++;
                continue;
            }
            if (status != HEAP_OK) return status;  /* 같은 행을 다시 시도할 수 있다 */
            cursor->next_slot++;
            cursor->row_charge = charged;
            locator->directory_index = (uint32_t)cursor->index;
            locator->slot = slot;
            return HEAP_OK;
        }
        cursor_drop_page(cursor);
        cursor->index++;
    }
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

heap_status_t heap_collect_pages(heap_t *heap, uint32_t **pages, size_t *count)
{
    if (heap == NULL || pages == NULL || count == NULL) return HEAP_INVALID;
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
    for (size_t i = 0; result == HEAP_OK && i < heap->dir_count; i++) list.items[list.len++] = heap->dirs[i];
    for (size_t i = 0; result == HEAP_OK && i < heap->entry_count; i++) {
        list.items[list.len++] = heap->entries[i];
        result = read_record_page(heap, heap->entries[i], page);
        if (result != HEAP_OK) break;
        uint16_t slots = get_u16(page + RECORD_PAGE_SLOT_COUNT_OFFSET);
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

/* ── batch ── */

/* batch의 entries·bufs·dirty 배열이 항목 하나당 쥐는 바이트 */
#define BATCH_ENTRY_BYTES (sizeof(uint32_t) + sizeof(uint8_t *) + sizeof(bool))

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
    batch->bufs = calloc(batch->entry_cap, sizeof(*batch->bufs));
    batch->dirty = calloc(batch->entry_cap, sizeof(*batch->dirty));
    if (batch->entries == NULL || batch->bufs == NULL || batch->dirty == NULL) {
        free(batch->entries);
        free(batch->bufs);
        free(batch->dirty);
        free(batch);
        acct_release(&heap->ledger, bytes);
        return HEAP_NOMEM;
    }
    if (heap->entry_count > 0) {
        memcpy(batch->entries, heap->entries, heap->entry_count * sizeof(*batch->entries));
    }
    batch->new_root = heap->root;
    *out = batch;
    return HEAP_OK;
}

void heap_batch_free(heap_batch_t *batch)
{
    if (batch == NULL) return;
    mem_ledger_t ledger = batch->ledger;
    size_t pages = 0;
    for (size_t i = 0; i < batch->entry_count; i++) {
        if (batch->bufs[i] != NULL) pages++;
        free(batch->bufs[i]);
    }
    acct_release(&ledger, pages * RECORD_PAGE_BYTES);
    acct_release(&ledger, sizeof(heap_batch_t) + batch->entry_cap * BATCH_ENTRY_BYTES);
    free(batch->entries);
    free(batch->bufs);
    free(batch->dirty);
    list_free(&batch->old_pages, &ledger);
    list_free(&batch->fresh, &ledger);
    free(batch);
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

static bool batch_io_release(void *context, uint32_t page_id)
{
    batch_io_ctx_t *ctx = context;
    heap_batch_t *batch = ctx->batch;
    if (list_remove(&batch->fresh, page_id)) {
        /* 이 batch가 만든 페이지는 어떤 commit된 상태도 가리키지 않아 곧바로 놓아도 된다. */
        return page_file_release(batch->heap->file, page_id) == PAGE_FILE_OK;
    }
    /* commit된 상태가 가리키는 페이지는 게시 성공 뒤에야 놓는다. */
    return list_push(&batch->old_pages, page_id, &batch->ledger);
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
        uint8_t **bufs = calloc(cap, sizeof(*bufs));
        bool *dirty = calloc(cap, sizeof(*dirty));
        if (entries == NULL || bufs == NULL || dirty == NULL) {
            free(entries);
            free(bufs);
            free(dirty);
            acct_release(&batch->ledger, new_bytes);
            return HEAP_NOMEM;
        }
        memcpy(entries, batch->entries, batch->entry_count * sizeof(*entries));
        memcpy(bufs, batch->bufs, batch->entry_count * sizeof(*bufs));
        memcpy(dirty, batch->dirty, batch->entry_count * sizeof(*dirty));
        free(batch->entries);
        free(batch->bufs);
        free(batch->dirty);
        batch->entries = entries;
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
                put_u64(scratch[i], bits);
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
        put_u32(scratch[pick], ref.head);
        put_u32(scratch[pick] + 4, ref.size_bytes);
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

static heap_status_t insert_slice(heap_batch_t *batch, record_slice_t slice, heap_locator_t *locator)
{
    uint8_t *page = NULL;
    size_t index = 0;
    heap_status_t status;
    if (batch->entry_count > 0) {
        index = batch->entry_count - 1;
        status = load_page(batch, index, &page);
        if (status != HEAP_OK) return status;
        uint16_t slot;
        record_page_status_t put = page_insert(batch, page, slice, &slot);
        if (put == RECORD_PAGE_OK) {
            status = mark_dirty(batch, index);
            if (status != HEAP_OK) return status;
            *locator = (heap_locator_t){(uint32_t)index, slot};
            return HEAP_OK;
        }
        if (put != RECORD_PAGE_FULL) return put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
    }
    status = append_page(batch, &index, &page);
    if (status != HEAP_OK) return status;
    uint16_t slot;
    record_page_status_t put = page_insert(batch, page, slice, &slot);
    if (put == RECORD_PAGE_FULL) return HEAP_LIMIT;
    if (put != RECORD_PAGE_OK) return put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
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
    status = row_from_slot(batch->heap, page, locator.slot, row, &charged);
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
        if (status == HEAP_OK) *new_locator = locator;
    } else if (put == RECORD_PAGE_FULL) {
        status = mark_dirty(batch, index);
        if (status == HEAP_OK) {
            record_page_status_t removed = page_delete(batch, page, locator.slot);
            if (removed == RECORD_PAGE_NOMEM) status = HEAP_NOMEM;
            else if (removed != RECORD_PAGE_OK) status = HEAP_CORRUPT;
        }
        if (status == HEAP_OK) status = insert_slice(batch, slice, new_locator);
    } else {
        status = put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
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
    status = release_row_chains(batch, old);
    if (status == HEAP_OK) status = mark_dirty(batch, index);
    if (status == HEAP_OK) {
        record_page_status_t removed = page_delete(batch, page, locator.slot);
        if (removed == RECORD_PAGE_NOMEM) status = HEAP_NOMEM;
        else if (removed != RECORD_PAGE_OK) status = HEAP_CORRUPT;
    }
    return fail(batch, status);
}

/* ── 게시 준비·정리 ── */

heap_status_t heap_batch_finish(heap_batch_t *batch, heap_batch_result_t *result)
{
    if (batch == NULL || result == NULL || batch->finished) return HEAP_INVALID;
    if (batch->failed) return HEAP_BATCH_FAILED;
    heap_t *heap = batch->heap;

    bool changed = batch->fresh.len > 0 || batch->old_pages.len > 0;
    if (!changed) {
        *result = (heap_batch_result_t){heap->root, NULL, 0, NULL, 0};
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
    if (status == HEAP_OK && batch->entry_count > 0) {
        size_t pages = (batch->entry_count + HEAP_DIRECTORY_ENTRIES - 1) / HEAP_DIRECTORY_ENTRIES;
        size_t ids_bytes = pages * sizeof(uint32_t);
        bool ids_reserved = acct_reserve(&batch->ledger, ids_bytes);
        uint32_t *ids = ids_reserved ? malloc(ids_bytes) : NULL;
        if (ids == NULL) {
            if (ids_reserved) acct_release(&batch->ledger, ids_bytes);
            status = HEAP_NOMEM;
        } else {
            for (size_t i = 0; i < pages && status == HEAP_OK; i++) status = batch_alloc(batch, &ids[i]);
            for (size_t i = 0; i < pages && status == HEAP_OK; i++) {
                uint8_t body[PAGE_BODY_BYTES];
                memset(body, 0, sizeof(body));
                size_t first = i * HEAP_DIRECTORY_ENTRIES;
                size_t take = batch->entry_count - first;
                if (take > HEAP_DIRECTORY_ENTRIES) take = HEAP_DIRECTORY_ENTRIES;
                memcpy(body, HEAP_DIRECTORY_MAGIC, 4);
                body[4] = (uint8_t)HEAP_DIRECTORY_VERSION;
                put_u32(body + 8, heap->table_id);
                put_u32(body + 12, i + 1 < pages ? ids[i + 1] : 0);
                put_u32(body + 16, (uint32_t)take);
                for (size_t k = 0; k < take; k++) {
                    put_u32(body + HEAP_DIRECTORY_HEADER_BYTES + k * 4, batch->entries[first + k]);
                }
                page_file_status_t written = page_file_write(heap->file, ids[i], PAGE_KIND_DIRECTORY,
                                                             heap->table_id, body);
                if (written != PAGE_FILE_OK) status = map_pf(heap, written);
            }
            if (status == HEAP_OK) new_root = ids[0];
            free(ids);
            acct_release(&batch->ledger, ids_bytes);
        }
    }
    if (status == HEAP_OK && !list_reserve(&batch->old_pages, heap->dir_count, &batch->ledger)) status = HEAP_NOMEM;
    if (status != HEAP_OK) return fail(batch, status);
    for (size_t i = 0; i < heap->dir_count; i++) batch->old_pages.items[batch->old_pages.len++] = heap->dirs[i];

    batch->new_root = new_root;
    batch->finished = true;
    *result = (heap_batch_result_t){new_root, batch->old_pages.items, batch->old_pages.len,
                                    batch->fresh.items, batch->fresh.len};
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
