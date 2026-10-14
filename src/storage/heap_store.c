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

struct heap {
    page_file_t *file;
    uint32_t table_id;
    uint32_t root;
    value_type_t *types;
    size_t column_count;
    uint32_t *entries;      /* record page ID, heap 순서 */
    size_t entry_count;
    uint32_t *dirs;         /* 디렉터리 페이지 ID, 체인 순서 */
    size_t dir_count;
    page_file_status_t last_status;
};

struct heap_batch {
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

static bool list_reserve(u32_list_t *list, size_t extra)
{
    if (list->len + extra <= list->cap) return true;
    size_t cap = list->cap == 0 ? 16 : list->cap;
    while (cap < list->len + extra) {
        if (cap > SIZE_MAX / 2 / sizeof(uint32_t)) return false;
        cap *= 2;
    }
    uint32_t *grown = realloc(list->items, cap * sizeof(uint32_t));
    if (grown == NULL) return false;
    list->items = grown;
    list->cap = cap;
    return true;
}

static bool list_push(u32_list_t *list, uint32_t value)
{
    if (!list_reserve(list, 1)) return false;
    list->items[list->len++] = value;
    return true;
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

/* 필드를 value_t 배열로 옮긴다. 실패하면 out은 바꾸지 않는다. */
static heap_status_t decode_row(heap_t *heap, const record_value_t *f, value_t *out)
{
    size_t n = heap->column_count;
    value_t *tmp = calloc(n, sizeof(*tmp));
    if (tmp == NULL) return HEAP_NOMEM;
    heap_status_t result = HEAP_OK;
    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, heap->file, heap->table_id);
    for (size_t i = 0; i < n && result == HEAP_OK; i++) {
        const record_value_t *kind = &f[1 + 2 * i];
        const record_value_t *payload = &f[2 + 2 * i];
        value_type_t type = heap->types[i];
        if (kind->integer == KIND_NULL) continue;
        if (kind->integer == KIND_OVERFLOW) {
            overflow_ref_t ref = ref_from(payload);
            uint8_t *bytes = NULL;
            overflow_status_t read = overflow_read(&io, ref, &bytes);
            if (read != OVERFLOW_OK) {
                result = map_overflow(heap, read, &ctx);
                break;
            }
            result = set_bytes_value(type, bytes, ref.size_bytes, &tmp[i]);
            free(bytes);
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
                result = set_bytes_value(type, payload->bytes, payload->size_bytes, &tmp[i]);
                break;
        }
    }
    if (result == HEAP_OK) {
        for (size_t i = 0; i < n; i++) {
            value_clear(&out[i]);
            out[i] = tmp[i];
        }
    } else {
        for (size_t i = 0; i < n; i++) value_clear(&tmp[i]);
    }
    free(tmp);
    return result;
}

/* ── 열기 ── */

static void heap_free_contents(heap_t *heap)
{
    free(heap->types);
    free(heap->entries);
    free(heap->dirs);
}

static heap_status_t read_directory(heap_t *heap, const page_file_control_t *control)
{
    uint32_t page = heap->root;
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
        if (!list_push(&dirs, page) || !list_reserve(&entries, count)) { result = HEAP_NOMEM; break; }
        for (uint32_t i = 0; i < count; i++) {
            entries.items[entries.len++] = get_u32(body + HEAP_DIRECTORY_HEADER_BYTES + (size_t)i * 4);
        }
        page = get_u32(body + 12);
    }

    if (result == HEAP_OK) {
        /* 범위와 중복: 항목과 디렉터리 페이지가 서로 겹치지 않아야 한다. */
        size_t total = entries.len + dirs.len;
        uint32_t *all = total > 0 ? malloc(total * sizeof(*all)) : NULL;
        if (total > 0 && all == NULL) {
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
        }
    }
    if (result != HEAP_OK) {
        free(entries.items);
        free(dirs.items);
        return result;
    }
    heap->entries = entries.items;
    heap->entry_count = entries.len;
    heap->dirs = dirs.items;
    heap->dir_count = dirs.len;
    return HEAP_OK;
}

heap_status_t heap_open(page_file_t *file, uint32_t table_id, uint32_t heap_root_page,
                        const heap_schema_t *schema, heap_t **out)
{
    if (file == NULL || schema == NULL || out == NULL || schema->types == NULL ||
        schema->column_count == 0 || schema->column_count > HEAP_MAX_COLUMNS) {
        return HEAP_INVALID;
    }
    for (size_t i = 0; i < schema->column_count; i++) {
        if (!column_type_valid(schema->types[i])) return HEAP_INVALID;
    }
    heap_t *heap = calloc(1, sizeof(*heap));
    if (heap == NULL) return HEAP_NOMEM;
    heap->file = file;
    heap->table_id = table_id;
    heap->root = heap_root_page;
    heap->column_count = schema->column_count;
    heap->types = malloc(schema->column_count * sizeof(*heap->types));
    if (heap->types == NULL) {
        free(heap);
        return HEAP_NOMEM;
    }
    memcpy(heap->types, schema->types, schema->column_count * sizeof(*heap->types));
    heap_status_t status = read_directory(heap, page_file_control(file));
    if (status != HEAP_OK) {
        heap_free_contents(heap);
        free(heap);
        return status;
    }
    *out = heap;
    return HEAP_OK;
}

void heap_close(heap_t *heap)
{
    if (heap == NULL) return;
    heap_free_contents(heap);
    free(heap);
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

static heap_status_t row_from_slot(heap_t *heap, const uint8_t *page, uint16_t slot, value_t *row)
{
    record_slice_t slice;
    record_page_status_t got = record_page_get(page, slot, &slice);
    if (got == RECORD_PAGE_NOT_FOUND) return HEAP_NOT_FOUND;
    if (got != RECORD_PAGE_OK) return HEAP_CORRUPT;
    record_value_t *fields = malloc(ROW_FIELDS(heap->column_count) * sizeof(*fields));
    if (fields == NULL) return HEAP_NOMEM;
    heap_status_t status = parse_fields(heap, slice, fields, ROW_FIELDS(heap->column_count));
    if (status == HEAP_OK) status = decode_row(heap, fields, row);
    free(fields);
    return status;
}

heap_status_t heap_get(heap_t *heap, heap_locator_t locator, value_t *row)
{
    if (heap == NULL || row == NULL) return HEAP_INVALID;
    if (locator.directory_index >= heap->entry_count) return HEAP_NOT_FOUND;
    uint8_t page[RECORD_PAGE_BYTES];
    heap_status_t status = read_record_page(heap, heap->entries[locator.directory_index], page);
    if (status != HEAP_OK) return status;
    return row_from_slot(heap, page, locator.slot, row);
}

/* ── cursor ── */

heap_status_t heap_cursor_open(heap_t *heap, heap_cursor_t **out)
{
    if (heap == NULL || out == NULL) return HEAP_INVALID;
    heap_cursor_t *cursor = calloc(1, sizeof(*cursor));
    if (cursor == NULL) return HEAP_NOMEM;
    cursor->heap = heap;
    *out = cursor;
    return HEAP_OK;
}

heap_status_t heap_cursor_next(heap_cursor_t *cursor, value_t *row, heap_locator_t *locator)
{
    if (cursor == NULL || row == NULL || locator == NULL) return HEAP_INVALID;
    heap_t *heap = cursor->heap;
    for (;;) {
        if (cursor->page == NULL) {
            if (cursor->index >= heap->entry_count) return HEAP_DONE;
            cursor->page = malloc(RECORD_PAGE_BYTES);
            if (cursor->page == NULL) return HEAP_NOMEM;
            heap_status_t status = read_record_page(heap, heap->entries[cursor->index], cursor->page);
            if (status != HEAP_OK) {
                free(cursor->page);
                cursor->page = NULL;
                return status;
            }
            cursor->slot_count = get_u16(cursor->page + RECORD_PAGE_SLOT_COUNT_OFFSET);
            cursor->next_slot = 0;
        }
        while (cursor->next_slot < cursor->slot_count) {
            uint16_t slot = cursor->next_slot;
            heap_status_t status = row_from_slot(heap, cursor->page, slot, row);
            if (status == HEAP_NOT_FOUND) {
                cursor->next_slot++;
                continue;
            }
            if (status != HEAP_OK) return status;  /* 같은 행을 다시 시도할 수 있다 */
            cursor->next_slot++;
            locator->directory_index = (uint32_t)cursor->index;
            locator->slot = slot;
            return HEAP_OK;
        }
        free(cursor->page);
        cursor->page = NULL;
        cursor->index++;
    }
}

void heap_cursor_close(heap_cursor_t *cursor)
{
    if (cursor == NULL) return;
    free(cursor->page);
    free(cursor);
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
    return list_push(ctx->out, page_id);
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
    u32_list_t list = {NULL, 0, 0};
    heap_status_t result = HEAP_OK;
    record_value_t *fields = malloc(ROW_FIELDS(heap->column_count) * sizeof(*fields));
    uint8_t *page = malloc(RECORD_PAGE_BYTES);
    if (fields == NULL || page == NULL) result = HEAP_NOMEM;
    if (result == HEAP_OK && (!list_reserve(&list, heap->dir_count + heap->entry_count))) result = HEAP_NOMEM;
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
                overflow_status_t freed = overflow_free(&io, ref_from(&fields[2 + 2 * c]));
                if (freed != OVERFLOW_OK) result = map_overflow(heap, freed, &ctx.inner);
            }
        }
    }
    free(fields);
    free(page);
    if (result != HEAP_OK) {
        free(list.items);
        return result;
    }
    *pages = list.items;
    *count = list.len;
    return HEAP_OK;
}

/* ── batch ── */

heap_status_t heap_batch_begin(heap_t *heap, heap_batch_t **out)
{
    if (heap == NULL || out == NULL) return HEAP_INVALID;
    heap_batch_t *batch = calloc(1, sizeof(*batch));
    if (batch == NULL) return HEAP_NOMEM;
    batch->heap = heap;
    batch->entry_count = heap->entry_count;
    batch->entry_cap = heap->entry_count > 0 ? heap->entry_count : 8;
    batch->entries = malloc(batch->entry_cap * sizeof(*batch->entries));
    batch->bufs = calloc(batch->entry_cap, sizeof(*batch->bufs));
    batch->dirty = calloc(batch->entry_cap, sizeof(*batch->dirty));
    if (batch->entries == NULL || batch->bufs == NULL || batch->dirty == NULL) {
        free(batch->entries);
        free(batch->bufs);
        free(batch->dirty);
        free(batch);
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
    for (size_t i = 0; i < batch->entry_count; i++) free(batch->bufs[i]);
    free(batch->entries);
    free(batch->bufs);
    free(batch->dirty);
    free(batch->old_pages.items);
    free(batch->fresh.items);
    free(batch);
}

/* 새 페이지 번호를 받아 fresh에 기록한다. 기록할 자리를 먼저 잡아 두어 할당 뒤 실패로 새지 않는다. */
static heap_status_t batch_alloc(heap_batch_t *batch, uint32_t *page_id)
{
    if (!list_reserve(&batch->fresh, 1)) return HEAP_NOMEM;
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
    return list_push(&batch->old_pages, page_id);
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
        uint8_t *buf = malloc(RECORD_PAGE_BYTES);
        if (buf == NULL) return HEAP_NOMEM;
        heap_status_t status = read_record_page(batch->heap, batch->entries[index], buf);
        if (status != HEAP_OK) {
            free(buf);
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
    if (!list_reserve(&batch->old_pages, 1)) return HEAP_NOMEM;
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
        size_t cap = batch->entry_cap * 2;
        uint32_t *entries = realloc(batch->entries, cap * sizeof(*entries));
        if (entries == NULL) return HEAP_NOMEM;
        batch->entries = entries;
        uint8_t **bufs = realloc(batch->bufs, cap * sizeof(*bufs));
        if (bufs == NULL) return HEAP_NOMEM;
        batch->bufs = bufs;
        bool *dirty = realloc(batch->dirty, cap * sizeof(*dirty));
        if (dirty == NULL) return HEAP_NOMEM;
        batch->dirty = dirty;
        for (size_t i = batch->entry_cap; i < cap; i++) {
            batch->bufs[i] = NULL;
            batch->dirty[i] = false;
        }
        batch->entry_cap = cap;
    }
    uint8_t *buf = malloc(RECORD_PAGE_BYTES);
    if (buf == NULL) return HEAP_NOMEM;
    uint32_t fresh;
    heap_status_t status = batch_alloc(batch, &fresh);
    if (status != HEAP_OK) {
        free(buf);
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
                value_status_t status = value_clone(v, &probe);
                value_clear(&probe);
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
static heap_status_t encode_row(heap_batch_t *batch, const value_view_t *values, size_t count,
                                record_buffer_t *out)
{
    heap_t *heap = batch->heap;
    heap_status_t status = check_values(heap, values, count);
    if (status != HEAP_OK) return status;

    size_t n = heap->column_count;
    size_t field_count = ROW_FIELDS(n);
    record_value_t *fields = calloc(field_count, sizeof(*fields));
    uint8_t (*scratch)[8] = calloc(n, sizeof(*scratch));
    overflow_ref_t *spilled = calloc(n, sizeof(*spilled));
    bool *is_spilled = calloc(n, sizeof(*is_spilled));
    if (fields == NULL || scratch == NULL || spilled == NULL || is_spilled == NULL) {
        free(fields);
        free(scratch);
        free(spilled);
        free(is_spilled);
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
        overflow_status_t written = overflow_write(&io, payload->bytes, payload->size_bytes, &ref);
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
            if (is_spilled[i]) overflow_free(&io, spilled[i]);
        }
    } else {
        *out = encoded;
    }
    free(fields);
    free(scratch);
    free(spilled);
    free(is_spilled);
    return status;
}

/* 지운·바뀐 행이 가리키던 overflow 체인을 게시 성공 뒤 놓을 목록에 넣는다. */
static heap_status_t release_row_chains(heap_batch_t *batch, record_slice_t slice)
{
    heap_t *heap = batch->heap;
    record_value_t *fields = malloc(ROW_FIELDS(heap->column_count) * sizeof(*fields));
    if (fields == NULL) return HEAP_NOMEM;
    heap_status_t status = parse_fields(heap, slice, fields, ROW_FIELDS(heap->column_count));
    batch_io_ctx_t ctx;
    overflow_io_t io = batch_io(&ctx, batch);
    for (size_t i = 0; status == HEAP_OK && i < heap->column_count; i++) {
        if (fields[1 + 2 * i].integer != KIND_OVERFLOW) continue;
        overflow_status_t freed = overflow_free(&io, ref_from(&fields[2 + 2 * i]));
        if (freed != OVERFLOW_OK) status = map_overflow(heap, freed, &ctx.inner);
    }
    free(fields);
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
        record_page_status_t put = record_page_insert(page, slice, &slot);
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
    record_page_status_t put = record_page_insert(page, slice, &slot);
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
    heap_status_t status = encode_row(batch, values, count, &encoded);
    if (status != HEAP_OK) return fail(batch, status);
    heap_locator_t placed;
    status = insert_slice(batch, (record_slice_t){encoded.bytes, encoded.size_bytes}, &placed);
    free(encoded.bytes);
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
    status = row_from_slot(batch->heap, page, locator.slot, row);
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
    status = encode_row(batch, values, count, &encoded);
    if (status != HEAP_OK) return fail(batch, status);
    /* 옛 행의 체인은 페이지를 바꾸기 전에 읽어 둔다. */
    status = release_row_chains(batch, old);
    if (status != HEAP_OK) {
        free(encoded.bytes);
        return fail(batch, status);
    }
    record_slice_t slice = {encoded.bytes, encoded.size_bytes};
    record_page_status_t put = record_page_update(page, locator.slot, slice);
    if (put == RECORD_PAGE_OK) {
        status = mark_dirty(batch, index);
        if (status == HEAP_OK) *new_locator = locator;
    } else if (put == RECORD_PAGE_FULL) {
        status = mark_dirty(batch, index);
        if (status == HEAP_OK && record_page_delete(page, locator.slot) != RECORD_PAGE_OK) status = HEAP_CORRUPT;
        if (status == HEAP_OK) status = insert_slice(batch, slice, new_locator);
    } else {
        status = put == RECORD_PAGE_NOMEM ? HEAP_NOMEM : HEAP_CORRUPT;
    }
    free(encoded.bytes);
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
    if (status == HEAP_OK && record_page_delete(page, locator.slot) != RECORD_PAGE_OK) status = HEAP_CORRUPT;
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
        uint32_t *ids = malloc(pages * sizeof(*ids));
        if (ids == NULL) {
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
        }
    }
    if (status == HEAP_OK && !list_reserve(&batch->old_pages, heap->dir_count)) status = HEAP_NOMEM;
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
