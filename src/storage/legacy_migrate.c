#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/catalog_store.h"
#include "storage/legacy_migrate.h"
#include "storage/page_format.h"
#include "storage/record.h"
#include "storage/record_page.h"
#include "storage/value.h"

/* 옛 파일은 쓴 호스트의 바이트 순서 그대로다. 이 변환기는 little-endian 호스트만 지원한다. */
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "legacy_migrate supports little-endian hosts only"
#endif

#define EINTR_RETRY_MAX 100
#define LEGACY_MIN_PAGE 512u
#define LEGACY_MAX_PAGE 65536u
#define LEGACY_HEAP_HEADER_BYTES ((uint32_t)sizeof(heap_page_header_t))
#define LEGACY_SLOT_BYTES ((uint32_t)sizeof(slot_t))
#define RECORD_PAGE_HEADER 8u
#define RECORD_SLOT_BYTES 4u

typedef struct {
    char name[33];
    uint8_t type;
    uint16_t size;
    uint16_t offset;
} legacy_column_t;

struct legacy_export {
    char table_name[33];
    uint16_t column_count;
    legacy_column_t columns[MAX_COLUMNS];
    uint64_t next_id;
    uint64_t row_count;
    uint8_t *rows;       /* 행마다 [u32 길이][record 바이트] */
    size_t rows_bytes;
    size_t rows_cap;
    int64_t *ids;
    size_t ids_cap;
    size_t record_bytes; /* 레코드 페이지에 쓰면 차지할 바이트 (헤더·슬롯 포함) */
};

typedef struct {
    int fd;
    page_file_io_t io;
    uint64_t size;
} reader_t;

static legacy_status_t read_exact(const reader_t *reader, uint8_t *buf, size_t size, uint64_t offset)
{
    size_t done = 0;
    int eintr = 0;
    while (done < size) {
        ssize_t got = reader->io.pread(reader->fd, buf + done, size - done, (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR && ++eintr <= EINTR_RETRY_MAX) continue;
            return LEGACY_IO;
        }
        if (got == 0) {
            errno = EIO;
            return LEGACY_IO;
        }
        eintr = 0;
        done += (size_t)got;
    }
    return LEGACY_OK;
}

static legacy_status_t map_page_file(page_file_status_t status)
{
    switch (status) {
        case PAGE_FILE_OK: return LEGACY_OK;
        case PAGE_FILE_EXISTS: return LEGACY_EXISTS;
        case PAGE_FILE_LOCKED: return LEGACY_LOCKED;
        case PAGE_FILE_NOMEM: return LEGACY_NOMEM;
        case PAGE_FILE_PUBLISHED_DIRTY: return LEGACY_PUBLISHED_DIRTY;
        case PAGE_FILE_INVALID: return LEGACY_INVALID;
        case PAGE_FILE_CORRUPT: return LEGACY_CORRUPT;
        default: return LEGACY_IO;
    }
}

static legacy_status_t map_store(catalog_store_status_t status)
{
    switch (status) {
        case CATALOG_STORE_OK: return LEGACY_OK;
        case CATALOG_STORE_NOMEM: return LEGACY_NOMEM;
        case CATALOG_STORE_INVALID:
        case CATALOG_STORE_STALE: return LEGACY_INVALID;
        case CATALOG_STORE_CORRUPT: return LEGACY_CORRUPT;
        case CATALOG_STORE_LIMIT: return LEGACY_LIMIT;
        default: return LEGACY_IO;
    }
}

/* ── 헤더 검증 ── */

static bool name_ok(const char *name, size_t capacity, size_t *length)
{
    size_t n = 0;
    while (n < capacity && name[n] != '\0') n++;
    if (n == 0 || n >= capacity) return false; /* 비었거나 NUL 종단이 없음 */
    *length = n;
    return true;
}

static legacy_status_t parse_header(const reader_t *reader, const char *v1_table_name,
                                    legacy_export_t *data, uint32_t *page_size,
                                    uint32_t *first_heap, uint32_t *next_page, uint32_t *row_size)
{
    uint8_t magic[8] = {0};
    if (reader->size < sizeof(magic)) return LEGACY_UNSUPPORTED;
    legacy_status_t status = read_exact(reader, magic, sizeof(magic), 0);
    if (status != LEGACY_OK) return status;
    if (memcmp(magic, "MINIDB\0", 8) != 0) return LEGACY_UNSUPPORTED;
    if (reader->size < sizeof(db_header_t)) return LEGACY_CORRUPT;

    db_header_t header;
    status = read_exact(reader, (uint8_t *)&header, sizeof(header), 0);
    if (status != LEGACY_OK) return status;
    if (header.version != 1 && header.version != 2) return LEGACY_UNSUPPORTED;
    uint32_t ps = header.page_size;
    if (ps < LEGACY_MIN_PAGE || ps > LEGACY_MAX_PAGE || (ps & (ps - 1)) != 0 ||
        ps < sizeof(db_header_t)) {
        return LEGACY_UNSUPPORTED;
    }
    if (header.next_page_id < 2 || (uint64_t)header.next_page_id * ps > reader->size) {
        return LEGACY_CORRUPT; /* 잘린 파일 */
    }
    if (header.first_heap_page_id < 1 || header.first_heap_page_id >= header.next_page_id) {
        return LEGACY_CORRUPT;
    }
    if (header.column_count < 1 || header.column_count > MAX_COLUMNS) return LEGACY_UNSUPPORTED;
    if (header.row_size == 0 || (uint32_t)header.row_size + LEGACY_HEAP_HEADER_BYTES + LEGACY_SLOT_BYTES > ps) {
        return LEGACY_CORRUPT;
    }
    if (header.next_id < 1) return LEGACY_CORRUPT;

    for (uint16_t i = 0; i < header.column_count; i++) {
        const column_meta_t *column = &header.columns[i];
        size_t length;
        if (!name_ok(column->name, sizeof(column->name), &length)) return LEGACY_CORRUPT;
        if (column->type == COL_TYPE_INT) {
            if (column->size != 4) return LEGACY_CORRUPT;
        } else if (column->type == COL_TYPE_BIGINT) {
            if (column->size != 8) return LEGACY_CORRUPT;
        } else if (column->type == COL_TYPE_VARCHAR) {
            if (column->size < 1 || column->size > 255) return LEGACY_UNSUPPORTED;
        } else {
            return LEGACY_UNSUPPORTED;
        }
        if ((uint32_t)column->offset + column->size > header.row_size) return LEGACY_CORRUPT;
        for (uint16_t j = 0; j < i; j++) {
            const column_meta_t *other = &header.columns[j];
            if (strcmp(other->name, column->name) == 0) return LEGACY_CORRUPT;
            if (column->offset < other->offset + other->size && other->offset < column->offset + column->size) {
                return LEGACY_CORRUPT; /* 컬럼 바이트 범위가 겹침 */
            }
        }
        if ((column->is_system != 0) != (i == 0)) return LEGACY_UNSUPPORTED;
        data->columns[i].type = column->type;
        data->columns[i].size = column->size;
        data->columns[i].offset = column->offset;
        memcpy(data->columns[i].name, column->name, sizeof(column->name));
        data->columns[i].name[32] = '\0';
    }
    if (header.columns[0].type != COL_TYPE_BIGINT || strcmp(header.columns[0].name, "id") != 0) {
        return LEGACY_UNSUPPORTED;
    }
    data->column_count = header.column_count;

    size_t name_length;
    if (header.version == 2) {
        if (!name_ok(header.table_name, sizeof(header.table_name), &name_length)) return LEGACY_CORRUPT;
        if (v1_table_name != NULL && strcmp(v1_table_name, header.table_name) != 0) return LEGACY_INVALID;
        memcpy(data->table_name, header.table_name, name_length + 1);
    } else {
        /* v1에는 이름이 없다. 호출자가 이름을 명시해야 하며 자동으로 짓지 않는다. */
        if (v1_table_name == NULL) return LEGACY_UNSUPPORTED;
        name_length = strlen(v1_table_name);
        if (name_length == 0 || name_length > 32) return LEGACY_INVALID;
        memcpy(data->table_name, v1_table_name, name_length + 1);
    }
    data->next_id = header.next_id;
    data->row_count = header.row_count;
    *page_size = ps;
    *first_heap = header.first_heap_page_id;
    *next_page = header.next_page_id;
    *row_size = header.row_size;
    return LEGACY_OK;
}

/* ── 행 변환 ── */

static bool grow_rows(legacy_export_t *data, size_t extra)
{
    if (data->rows_bytes + extra <= data->rows_cap) return true;
    size_t next = data->rows_cap == 0 ? 1024 : data->rows_cap;
    while (next < data->rows_bytes + extra) next *= 2;
    uint8_t *bigger = realloc(data->rows, next);
    if (bigger == NULL) return false;
    data->rows = bigger;
    data->rows_cap = next;
    return true;
}

static legacy_status_t append_row(legacy_export_t *data, const uint8_t *row)
{
    record_value_t values[MAX_COLUMNS];
    int64_t id = 0;
    for (uint16_t i = 0; i < data->column_count; i++) {
        const legacy_column_t *column = &data->columns[i];
        const uint8_t *src = row + column->offset;
        values[i] = (record_value_t){RECORD_INTEGER, 0, NULL, 0};
        if (column->type == COL_TYPE_INT) {
            int32_t v;
            memcpy(&v, src, 4);
            values[i].integer = v;
        } else if (column->type == COL_TYPE_BIGINT) {
            int64_t v;
            memcpy(&v, src, 8);
            values[i].integer = v;
        } else {
            size_t length = 0;
            while (length < column->size && src[length] != '\0') length++;
            value_t text = {0};
            /* 옛 형식은 아무 바이트나 담을 수 있었다. 잘못된 UTF-8은 조용히 바꾸지 않고 거절한다. */
            value_status_t checked = value_set_text(&text, src, length);
            value_clear(&text);
            if (checked == VALUE_NOMEM) return LEGACY_NOMEM;
            if (checked != VALUE_OK) return LEGACY_UNCONVERTIBLE;
            values[i] = (record_value_t){RECORD_TEXT, 0, src, length};
        }
        if (i == 0) id = values[i].integer;
    }
    if (id < 1 || (uint64_t)id >= data->next_id) return LEGACY_CORRUPT;
    record_buffer_t encoded;
    record_status_t status = record_encode(values, data->column_count, &encoded);
    if (status == RECORD_NOMEM) return LEGACY_NOMEM;
    if (status != RECORD_OK) return LEGACY_UNCONVERTIBLE;
    size_t need = RECORD_SLOT_BYTES + encoded.size_bytes;
    if (data->record_bytes + need > RECORD_PAGE_BYTES || encoded.size_bytes > UINT32_MAX) {
        free(encoded.bytes);
        return LEGACY_LIMIT;
    }
    if (!grow_rows(data, 4 + encoded.size_bytes)) {
        free(encoded.bytes);
        return LEGACY_NOMEM;
    }
    uint32_t length = (uint32_t)encoded.size_bytes;
    for (unsigned b = 0; b < 4; b++) data->rows[data->rows_bytes + b] = (uint8_t)(length >> (b * 8));
    memcpy(data->rows + data->rows_bytes + 4, encoded.bytes, encoded.size_bytes);
    data->rows_bytes += 4 + encoded.size_bytes;
    data->record_bytes += need;
    free(encoded.bytes);
    return LEGACY_OK;
}

void legacy_export_free(legacy_export_t *data)
{
    if (data == NULL) return;
    free(data->rows);
    free(data->ids);
    free(data);
}

const char *legacy_export_table_name(const legacy_export_t *data)
{
    return data != NULL ? data->table_name : NULL;
}

uint64_t legacy_export_row_count(const legacy_export_t *data)
{
    return data != NULL ? data->row_count : 0;
}

uint64_t legacy_export_next_id(const legacy_export_t *data)
{
    return data != NULL ? data->next_id : 0;
}

static legacy_status_t scan_heap(const reader_t *reader, legacy_export_t *data, uint32_t page_size,
                                 uint32_t first_heap, uint32_t next_page, uint32_t row_size)
{
    uint8_t *page = malloc(page_size);
    uint8_t *seen = calloc(next_page, 1);
    if (page == NULL || seen == NULL) {
        free(page);
        free(seen);
        return LEGACY_NOMEM;
    }
    legacy_status_t status = LEGACY_OK;
    uint64_t live = 0;
    uint32_t pid = first_heap;
    uint32_t visited = 0;
    data->ids_cap = 0;
    while (pid != 0 && status == LEGACY_OK) {
        if (pid >= next_page || seen[pid] != 0 || ++visited > next_page) {
            status = LEGACY_CORRUPT; /* 범위 밖이거나 같은 페이지를 다시 만나는 순환 힙 체인 */
            break;
        }
        seen[pid] = 1;
        status = read_exact(reader, page, page_size, (uint64_t)pid * page_size);
        if (status != LEGACY_OK) break;
        heap_page_header_t header;
        memcpy(&header, page, sizeof(header));
        if (header.page_type != PAGE_TYPE_HEAP ||
            LEGACY_HEAP_HEADER_BYTES + (uint64_t)header.slot_count * LEGACY_SLOT_BYTES > page_size) {
            status = LEGACY_CORRUPT;
            break;
        }
        uint32_t slots_end = LEGACY_HEAP_HEADER_BYTES + header.slot_count * LEGACY_SLOT_BYTES;
        for (uint16_t i = 0; i < header.slot_count && status == LEGACY_OK; i++) {
            slot_t slot;
            memcpy(&slot, page + LEGACY_HEAP_HEADER_BYTES + (size_t)i * LEGACY_SLOT_BYTES, sizeof(slot));
            if (slot.status == SLOT_FREE) continue;
            if (slot.status != SLOT_ALIVE || slot.offset < slots_end ||
                (uint32_t)slot.offset + row_size > page_size) {
                status = LEGACY_CORRUPT;
                break;
            }
            status = append_row(data, page + slot.offset);
            if (status != LEGACY_OK) break;
            int64_t id;
            memcpy(&id, page + slot.offset + data->columns[0].offset, 8);
            if (live >= data->ids_cap) {
                size_t next = data->ids_cap == 0 ? 64 : data->ids_cap * 2;
                int64_t *bigger = realloc(data->ids, next * sizeof(int64_t));
                if (bigger == NULL) {
                    status = LEGACY_NOMEM;
                    break;
                }
                data->ids = bigger;
                data->ids_cap = next;
            }
            for (uint64_t k = 0; k < live; k++) {
                if (data->ids[k] == id) status = LEGACY_UNCONVERTIBLE; /* 중복 id */
            }
            data->ids[live++] = id;
        }
        pid = header.next_heap_page_id;
    }
    free(page);
    free(seen);
    if (status == LEGACY_OK && live != data->row_count) status = LEGACY_CORRUPT;
    return status;
}

legacy_status_t legacy_export(const char *path, const char *v1_table_name, legacy_export_t **out)
{
    if (path == NULL || out == NULL) return LEGACY_INVALID;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return LEGACY_IO;
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        int err = errno;
        close(fd);
        errno = err;
        return err == EWOULDBLOCK ? LEGACY_LOCKED : LEGACY_IO;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int err = errno;
        close(fd);
        errno = err;
        return LEGACY_IO;
    }
    reader_t reader = {fd, page_file_default_io(), (uint64_t)st.st_size};
    legacy_export_t *data = calloc(1, sizeof(*data));
    if (data == NULL) {
        close(fd);
        return LEGACY_NOMEM;
    }
    data->record_bytes = RECORD_PAGE_HEADER;
    uint32_t page_size, first_heap, next_page, row_size;
    legacy_status_t status = parse_header(&reader, v1_table_name, data, &page_size, &first_heap,
                                          &next_page, &row_size);
    if (status == LEGACY_OK) status = scan_heap(&reader, data, page_size, first_heap, next_page, row_size);
    int err = errno;
    close(fd);
    if (status != LEGACY_OK) {
        legacy_export_free(data);
        errno = err;
        return status;
    }
    *out = data;
    return LEGACY_OK;
}

/* ── import ── */

static value_type_t new_type(uint8_t legacy_type)
{
    return legacy_type == COL_TYPE_VARCHAR ? VALUE_TEXT : VALUE_INTEGER;
}

/* 쓴 내용을 읽어 export와 바이트 단위로 대조한다. 게시 전에 호출한다. */
static legacy_status_t verify_written(page_file_t *file, const legacy_export_t *data,
                                      const catalog_t *expected, uint32_t table_id)
{
    catalog_t *loaded = NULL;
    catalog_store_status_t loaded_status = catalog_store_load(file, &loaded);
    if (loaded_status != CATALOG_STORE_OK) return map_store(loaded_status);
    uint8_t *left = NULL, *right = NULL;
    size_t left_size = 0, right_size = 0;
    bool same = catalog_encode(expected, &left, &left_size) == CATALOG_OK &&
                catalog_encode(loaded, &right, &right_size) == CATALOG_OK && left_size == right_size &&
                memcmp(left, right, left_size) == 0;
    free(left);
    free(right);
    catalog_table_view_t view;
    bool described = catalog_describe(loaded, table_id, &view) == CATALOG_OK;
    uint32_t heap_root = described ? view.heap_root : 0;
    catalog_close(loaded);
    if (!same || !described) return LEGACY_CORRUPT;

    uint8_t page[RECORD_PAGE_BYTES];
    page_file_status_t read = page_file_read(file, heap_root, PAGE_KIND_RECORD, table_id, page);
    if (read != PAGE_FILE_OK) return map_page_file(read);
    size_t offset = 0;
    for (uint64_t i = 0; i < data->row_count; i++) {
        uint32_t length = 0;
        for (unsigned b = 0; b < 4; b++) length |= (uint32_t)data->rows[offset + b] << (b * 8);
        record_slice_t slice;
        if (record_page_get(page, (uint16_t)i, &slice) != RECORD_PAGE_OK || slice.size_bytes != length ||
            memcmp(slice.bytes, data->rows + offset + 4, length) != 0) {
            return LEGACY_CORRUPT;
        }
        offset += 4 + length;
    }
    return LEGACY_OK;
}

static legacy_status_t build_and_commit(page_file_t *file, const legacy_export_t *data)
{
    catalog_t *catalog = NULL;
    if (catalog_open_empty(&catalog) != CATALOG_OK) return LEGACY_NOMEM;
    legacy_status_t result = LEGACY_IO;

    catalog_column_spec_t columns[MAX_COLUMNS];
    for (uint16_t i = 0; i < data->column_count; i++) {
        columns[i] = (catalog_column_spec_t){data->columns[i].name, strlen(data->columns[i].name),
                                             new_type(data->columns[i].type), i == 0, NULL};
    }
    uint32_t heap_page;
    page_file_status_t allocated = page_file_alloc(file, &heap_page);
    if (allocated != PAGE_FILE_OK) {
        result = map_page_file(allocated);
        goto done;
    }
    catalog_legacy_table_t legacy = {
        .table = {data->table_name, strlen(data->table_name), columns, data->column_count, 0, true},
        .row_count = data->row_count,
        .next_identity = data->next_id,
        .heap_root = heap_page,
        .primary_root = CATALOG_NO_PAGE,
    };
    catalog_table_id_t table_id;
    catalog_status_t imported = catalog_import_legacy(catalog, &legacy, &table_id);
    if (imported != CATALOG_OK) {
        result = imported == CATALOG_NOMEM ? LEGACY_NOMEM : LEGACY_CORRUPT;
        goto done;
    }

    uint8_t page[RECORD_PAGE_BYTES];
    record_page_init(page);
    size_t offset = 0;
    for (uint64_t i = 0; i < data->row_count; i++) {
        uint32_t length = 0;
        for (unsigned b = 0; b < 4; b++) length |= (uint32_t)data->rows[offset + b] << (b * 8);
        uint16_t slot;
        record_page_status_t inserted = record_page_insert(
            page, (record_slice_t){data->rows + offset + 4, length}, &slot);
        if (inserted != RECORD_PAGE_OK) {
            result = inserted == RECORD_PAGE_FULL ? LEGACY_LIMIT : LEGACY_CORRUPT;
            goto done;
        }
        offset += 4 + length;
    }
    page_file_status_t written = page_file_write(file, heap_page, PAGE_KIND_RECORD, table_id, page);
    if (written != PAGE_FILE_OK) {
        result = map_page_file(written);
        goto done;
    }
    catalog_store_status_t saved = catalog_store_save(file, catalog);
    if (saved != CATALOG_STORE_OK) {
        result = map_store(saved);
        goto done;
    }
    result = verify_written(file, data, catalog, table_id);
done:
    catalog_close(catalog);
    return result;
}

legacy_status_t legacy_import(const legacy_export_t *data, const char *destination)
{
    if (data == NULL || destination == NULL) return LEGACY_INVALID;
    page_file_t *file = NULL;
    page_file_status_t created = page_file_create(destination, &file);
    if (created != PAGE_FILE_OK) return map_page_file(created);

    legacy_status_t status = build_and_commit(file, data);
    int saved_errno = errno;
    if (status == LEGACY_OK) {
        page_file_status_t published = page_file_publish(file);
        saved_errno = errno;
        status = map_page_file(published);
    }
    /* 게시 전에 실패했다면 close가 partial을 지운다. 게시된 파일은 그대로 둔다. */
    page_file_close(file);
    errno = saved_errno;
    return status;
}

legacy_status_t legacy_migrate(const char *source, const char *destination, const char *v1_table_name)
{
    legacy_export_t *data = NULL;
    legacy_status_t status = legacy_export(source, v1_table_name, &data);
    if (status != LEGACY_OK) return status;
    status = legacy_import(data, destination);
    int saved_errno = errno;
    legacy_export_free(data);
    errno = saved_errno;
    return status;
}
