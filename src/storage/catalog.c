#include <stdlib.h>
#include <string.h>

#include "storage/catalog.h"
#include "storage/le.h"

#define FORMAT_VERSION 1u
#define INDEX_FORMAT_VERSION 2u
/* 컬럼 기본값이 있는 카탈로그. 색인 영역은 테이블 뒤에 바이트가 남아 있을 때만 있다 */
#define DEFAULT_FORMAT_VERSION 3u
#define HEADER_BYTES 28u
#define CHECKSUM_BYTES 4u
#define TABLE_FIXED_BYTES 35u
#define COLUMN_FIXED_BYTES 4u
#define INDEX_FIXED_BYTES 13u
#define MIN_TABLE_BYTES (TABLE_FIXED_BYTES + 1u + COLUMN_FIXED_BYTES + 1u)
#define IDENTITY_END ((uint64_t)INT64_MAX + 1u)
#define FLAG_IDENTITY 1u
#define FLAG_NOT_NULL 1u
#define FLAG_DEFAULT 2u
#define INDEX_UNIQUE 1u
#define INDEX_ORDERED 2u

typedef struct {
    catalog_table_id_t id;
    char *names;                 /* 소유: 테이블 이름 뒤에 컬럼 이름, 그 뒤에 기본값 바이트가 이어진다 */
    size_t names_capacity;       /* names 할당 바이트 */
    catalog_column_t *columns;   /* 소유: 이름 포인터는 names 안을 가리킨다 */
    size_t name_bytes;
    size_t column_count;
    uint16_t primary_key_column;
    bool has_identity;
    uint64_t row_count;
    uint64_t next_identity;
    uint32_t heap_root;
    uint32_t primary_root;
    size_t encoded_bytes;
} table_t;

typedef struct {
    char *name;
    size_t name_bytes;
    catalog_table_id_t table_id;
    uint16_t column;
    bool unique;
    bool ordered;   /* 비고유 색인 키의 꼬리가 정수 기본 키다(값이 같으면 기본 키 순서) */
    uint32_t root;
} index_t;

struct catalog {
    table_t *tables;
    size_t count;
    size_t capacity;
    index_t *indexes;
    size_t index_count;
    size_t index_capacity;
    uint32_t next_table_id;
    uint64_t generation;
    size_t encoded_bytes;  /* 지금 encode하면 나올 바이트 수 */
};

typedef struct {
    const char *name;
    size_t size;
} name_ref_t;

static bool name_valid(const char *name, size_t size)
{
    return name != NULL && size >= 1 && size <= CATALOG_NAME_MAX_BYTES &&
           memchr(name, 0, size) == NULL;
}

static int compare_names(const void *left, const void *right)
{
    const name_ref_t *a = left, *b = right;
    size_t common = a->size < b->size ? a->size : b->size;
    int result = memcmp(a->name, b->name, common);
    if (result != 0) return result;
    return (a->size > b->size) - (a->size < b->size);
}

/* 정렬해 이웃을 비교하므로 컬럼 수가 많아도 O(n log n)이다. */
static catalog_status_t check_unique_names(const name_ref_t *refs, size_t count)
{
    name_ref_t *sorted = malloc(count * sizeof(*sorted));
    if (sorted == NULL) return CATALOG_NOMEM;
    memcpy(sorted, refs, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), compare_names);
    catalog_status_t status = CATALOG_OK;
    for (size_t i = 1; i < count && status == CATALOG_OK; i++) {
        if (compare_names(&sorted[i - 1], &sorted[i]) == 0) status = CATALOG_EXISTS;
    }
    free(sorted);
    return status;
}

static bool column_type_valid(value_type_t type)
{
    return type >= VALUE_INTEGER && type <= VALUE_BLOB;
}

/* 기본값 인코딩: 형식 u8, INTEGER·REAL은 8바이트, TEXT·BLOB은 u32 길이와 바이트 */
static size_t default_encoded_bytes(const value_view_t *value)
{
    return 1u + (value->type == VALUE_INTEGER || value->type == VALUE_REAL ? 8u : 4u + value->size_bytes);
}

static size_t default_payload_bytes(const value_view_t *value)
{
    return value->type == VALUE_TEXT || value->type == VALUE_BLOB ? value->size_bytes : 0;
}

static size_t table_encoded_bytes(size_t name_bytes, const catalog_column_t *columns,
                                  size_t column_count)
{
    size_t total = TABLE_FIXED_BYTES + name_bytes;
    for (size_t i = 0; i < column_count; i++) {
        total += COLUMN_FIXED_BYTES + columns[i].name_bytes;
        if (columns[i].has_default) total += default_encoded_bytes(&columns[i].default_value);
    }
    return total;
}

/* spec 모양과 identity 규칙을 검사한다. 이름 중복 검사는 할당이 필요해 따로 한다. */
static catalog_status_t check_spec(const catalog_table_spec_t *spec)
{
    if (spec == NULL || !name_valid(spec->name, spec->name_bytes)) return CATALOG_INVALID;
    if (spec->column_count < 1 || spec->column_count > CATALOG_MAX_COLUMNS) return CATALOG_INVALID;
    if (spec->columns == NULL) return CATALOG_INVALID;
    for (size_t i = 0; i < spec->column_count; i++) {
        const catalog_column_spec_t *column = &spec->columns[i];
        if (!name_valid(column->name, column->name_bytes) || !column_type_valid(column->type)) {
            return CATALOG_INVALID;
        }
        const value_view_t *value = column->default_value;
        if (value != NULL && (value->type != column->type ||
                              ((value->type == VALUE_TEXT || value->type == VALUE_BLOB) &&
                               (value->size_bytes > CATALOG_DEFAULT_MAX_BYTES ||
                                (value->size_bytes > 0 && value->bytes == NULL))))) {
            return CATALOG_INVALID;
        }
    }
    uint16_t key = spec->primary_key_column;
    if (key != CATALOG_NO_COLUMN && key >= spec->column_count) return CATALOG_INVALID;
    if (spec->has_identity &&
        (key == CATALOG_NO_COLUMN || spec->columns[key].type != VALUE_INTEGER)) {
        return CATALOG_INVALID;
    }
    return CATALOG_OK;
}

static catalog_status_t check_spec_names(const catalog_table_spec_t *spec)
{
    name_ref_t *refs = malloc(spec->column_count * sizeof(*refs));
    if (refs == NULL) return CATALOG_NOMEM;
    for (size_t i = 0; i < spec->column_count; i++) {
        refs[i] = (name_ref_t){spec->columns[i].name, spec->columns[i].name_bytes};
    }
    catalog_status_t status = check_unique_names(refs, spec->column_count);
    free(refs);
    /* 컬럼 이름 중복은 카탈로그의 이름 충돌이 아니라 schema 오류다. */
    return status == CATALOG_EXISTS ? CATALOG_INVALID : status;
}

static void release_table(table_t *table)
{
    free(table->names);
    free(table->columns);
    memset(table, 0, sizeof(*table));
}

static catalog_status_t build_table(table_t *out, catalog_table_id_t id,
                                    const catalog_table_spec_t *spec)
{
    size_t names_bytes = spec->name_bytes;
    for (size_t i = 0; i < spec->column_count; i++) {
        names_bytes += spec->columns[i].name_bytes;
        if (spec->columns[i].default_value != NULL) names_bytes += default_payload_bytes(spec->columns[i].default_value);
    }
    table_t table = {0};
    table.names = malloc(names_bytes);
    table.names_capacity = names_bytes;
    table.columns = malloc(spec->column_count * sizeof(*table.columns));
    if (table.names == NULL || table.columns == NULL) {
        release_table(&table);
        return CATALOG_NOMEM;
    }
    memcpy(table.names, spec->name, spec->name_bytes);
    size_t offset = spec->name_bytes;
    for (size_t i = 0; i < spec->column_count; i++) {
        const catalog_column_spec_t *column = &spec->columns[i];
        memcpy(table.names + offset, column->name, column->name_bytes);
        table.columns[i] = (catalog_column_t){table.names + offset, column->name_bytes,
                                              column->type, column->not_null, false, {0}};
        offset += column->name_bytes;
    }
    for (size_t i = 0; i < spec->column_count; i++) {
        const value_view_t *value = spec->columns[i].default_value;
        if (value == NULL) continue;
        table.columns[i].has_default = true;
        table.columns[i].default_value = *value;
        size_t payload = default_payload_bytes(value);
        if (payload > 0) {
            memcpy(table.names + offset, value->bytes, payload);
            table.columns[i].default_value.bytes = (const uint8_t *)table.names + offset;
            offset += payload;
        } else if (value->type == VALUE_TEXT || value->type == VALUE_BLOB) {
            table.columns[i].default_value.bytes = NULL;
        }
    }
    table.id = id;
    table.name_bytes = spec->name_bytes;
    table.column_count = spec->column_count;
    table.primary_key_column = spec->primary_key_column;
    table.has_identity = spec->has_identity;
    table.next_identity = spec->has_identity ? 1 : 0;
    table.encoded_bytes = table_encoded_bytes(spec->name_bytes, table.columns, spec->column_count);
    *out = table;
    return CATALOG_OK;
}

static void fill_view(const table_t *table, catalog_table_view_t *view)
{
    *view = (catalog_table_view_t){table->id, table->names, table->name_bytes, table->columns,
                                   table->column_count, table->primary_key_column,
                                   table->has_identity, table->row_count, table->next_identity,
                                   table->heap_root, table->primary_root};
}

catalog_status_t catalog_open_empty(catalog_t **out)
{
    if (out == NULL) return CATALOG_INVALID;
    catalog_t *catalog = malloc(sizeof(*catalog));
    if (catalog == NULL) return CATALOG_NOMEM;
    *catalog = (catalog_t){.next_table_id = 1, .encoded_bytes = HEADER_BYTES + CHECKSUM_BYTES};
    *out = catalog;
    return CATALOG_OK;
}

void catalog_close(catalog_t *catalog)
{
    if (catalog == NULL) return;
    for (size_t i = 0; i < catalog->count; i++) release_table(&catalog->tables[i]);
    for (size_t i = 0; i < catalog->index_count; i++) free(catalog->indexes[i].name);
    free(catalog->indexes);
    free(catalog->tables);
    free(catalog);
}

static const table_t *find_by_id(const catalog_t *catalog, catalog_table_id_t id)
{
    size_t low = 0, high = catalog->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (catalog->tables[middle].id < id) low = middle + 1;
        else high = middle;
    }
    if (low < catalog->count && catalog->tables[low].id == id) return &catalog->tables[low];
    return NULL;
}

static table_t *find_mutable(catalog_t *catalog, catalog_table_id_t id)
{
    return (table_t *)find_by_id(catalog, id);
}

static const table_t *find_by_name(const catalog_t *catalog, const char *name, size_t size)
{
    for (size_t i = 0; i < catalog->count; i++) {
        const table_t *table = &catalog->tables[i];
        if (table->name_bytes == size && memcmp(table->names, name, size) == 0) return table;
    }
    return NULL;
}

static catalog_status_t reserve_slot(catalog_t *catalog)
{
    if (catalog->count < catalog->capacity) return CATALOG_OK;
    size_t capacity = catalog->capacity == 0 ? 4 : catalog->capacity * 2;
    if (capacity > SIZE_MAX / sizeof(table_t)) return CATALOG_LIMIT;
    table_t *grown = realloc(catalog->tables, capacity * sizeof(*grown));
    if (grown == NULL) return CATALOG_NOMEM;
    catalog->tables = grown;
    catalog->capacity = capacity;
    return CATALOG_OK;
}

static catalog_status_t check_can_mutate(const catalog_t *catalog)
{
    return catalog->generation == UINT64_MAX ? CATALOG_LIMIT : CATALOG_OK;
}

/* 모든 검사와 할당을 끝낸 뒤에만 상태를 바꾼다. */
static catalog_status_t add_table(catalog_t *catalog, const catalog_table_spec_t *spec,
                                  const catalog_legacy_table_t *legacy, catalog_table_id_t *id)
{
    if (id == NULL) return CATALOG_INVALID;
    catalog_status_t status = check_spec(spec);
    if (status != CATALOG_OK) return status;
    if (legacy != NULL && spec->has_identity &&
        (legacy->next_identity < 1 || legacy->next_identity > IDENTITY_END)) {
        return CATALOG_INVALID;
    }
    status = check_spec_names(spec);
    if (status != CATALOG_OK) return status;
    if (find_by_name(catalog, spec->name, spec->name_bytes) != NULL) return CATALOG_EXISTS;
    if (catalog->next_table_id == UINT32_MAX) return CATALOG_LIMIT;
    status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;

    table_t table;
    status = build_table(&table, catalog->next_table_id, spec);
    if (status != CATALOG_OK) return status;
    if (table.encoded_bytes > CATALOG_MAX_ENCODED_BYTES - catalog->encoded_bytes) {
        release_table(&table);
        return CATALOG_LIMIT;
    }
    status = reserve_slot(catalog);
    if (status != CATALOG_OK) {
        release_table(&table);
        return status;
    }
    if (legacy != NULL) {
        table.row_count = legacy->row_count;
        if (spec->has_identity) table.next_identity = legacy->next_identity;
        table.heap_root = legacy->heap_root;
        table.primary_root = legacy->primary_root;
    }
    catalog->tables[catalog->count++] = table;
    catalog->next_table_id++;
    catalog->generation++;
    catalog->encoded_bytes += table.encoded_bytes;
    *id = table.id;
    return CATALOG_OK;
}

catalog_status_t catalog_create_table(catalog_t *catalog, const catalog_table_spec_t *spec,
                                      catalog_table_id_t *id)
{
    if (catalog == NULL) return CATALOG_INVALID;
    return add_table(catalog, spec, NULL, id);
}

catalog_status_t catalog_import_legacy(catalog_t *catalog, const catalog_legacy_table_t *legacy,
                                       catalog_table_id_t *id)
{
    if (catalog == NULL || legacy == NULL) return CATALOG_INVALID;
    return add_table(catalog, &legacy->table, legacy, id);
}

catalog_status_t catalog_drop_table(catalog_t *catalog, catalog_table_id_t id)
{
    if (catalog == NULL) return CATALOG_INVALID;
    table_t *table = find_mutable(catalog, id);
    if (table == NULL) return CATALOG_NOT_FOUND;
    catalog_status_t status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;
    size_t index = (size_t)(table - catalog->tables);
    catalog->encoded_bytes -= table->encoded_bytes;
    bool had_indexes = catalog->index_count > 0;
    for (size_t i = 0; i < catalog->index_count;) {
        if (catalog->indexes[i].table_id != id) {
            i++;
            continue;
        }
        catalog->encoded_bytes -= INDEX_FIXED_BYTES + catalog->indexes[i].name_bytes;
        free(catalog->indexes[i].name);
        memmove(&catalog->indexes[i], &catalog->indexes[i + 1],
                (catalog->index_count - i - 1) * sizeof(*catalog->indexes));
        catalog->index_count--;
    }
    if (had_indexes && catalog->index_count == 0) catalog->encoded_bytes -= 4u;
    release_table(table);
    memmove(table, table + 1, (catalog->count - index - 1) * sizeof(*table));
    catalog->count--;
    catalog->generation++;
    return CATALOG_OK;
}

catalog_status_t catalog_find(const catalog_t *catalog, const char *name, size_t name_bytes,
                              catalog_table_id_t *id)
{
    if (catalog == NULL || id == NULL || name == NULL) return CATALOG_INVALID;
    const table_t *table = find_by_name(catalog, name, name_bytes);
    if (table == NULL) return CATALOG_NOT_FOUND;
    *id = table->id;
    return CATALOG_OK;
}

catalog_status_t catalog_describe(const catalog_t *catalog, catalog_table_id_t id,
                                  catalog_table_view_t *view)
{
    if (catalog == NULL || view == NULL) return CATALOG_INVALID;
    const table_t *table = find_by_id(catalog, id);
    if (table == NULL) return CATALOG_NOT_FOUND;
    fill_view(table, view);
    return CATALOG_OK;
}

size_t catalog_table_count(const catalog_t *catalog)
{
    return catalog == NULL ? 0 : catalog->count;
}

catalog_status_t catalog_table_at(const catalog_t *catalog, size_t index,
                                  catalog_table_view_t *view)
{
    if (catalog == NULL || view == NULL) return CATALOG_INVALID;
    if (index >= catalog->count) return CATALOG_NOT_FOUND;
    fill_view(&catalog->tables[index], view);
    return CATALOG_OK;
}

catalog_status_t catalog_set_row_count(catalog_t *catalog, catalog_table_id_t id,
                                       uint64_t row_count)
{
    if (catalog == NULL) return CATALOG_INVALID;
    table_t *table = find_mutable(catalog, id);
    if (table == NULL) return CATALOG_NOT_FOUND;
    catalog_status_t status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;
    table->row_count = row_count;
    catalog->generation++;
    return CATALOG_OK;
}

catalog_status_t catalog_set_locators(catalog_t *catalog, catalog_table_id_t id,
                                      uint32_t heap_root, uint32_t primary_root)
{
    if (catalog == NULL) return CATALOG_INVALID;
    table_t *table = find_mutable(catalog, id);
    if (table == NULL) return CATALOG_NOT_FOUND;
    catalog_status_t status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;
    table->heap_root = heap_root;
    table->primary_root = primary_root;
    catalog->generation++;
    return CATALOG_OK;
}

static index_t *find_index(catalog_t *catalog, const char *name, size_t name_bytes)
{
    for (size_t i = 0; i < catalog->index_count; i++) {
        index_t *index = &catalog->indexes[i];
        if (index->name_bytes == name_bytes && memcmp(index->name, name, name_bytes) == 0) return index;
    }
    return NULL;
}

catalog_status_t catalog_add_index(catalog_t *catalog, catalog_table_id_t table_id,
                                   const char *name, size_t name_bytes, uint16_t column,
                                   bool unique, bool ordered)
{
    if (catalog == NULL || !name_valid(name, name_bytes)) return CATALOG_INVALID;
    const table_t *table = find_by_id(catalog, table_id);
    if (table == NULL) return CATALOG_NOT_FOUND;
    if (column >= table->column_count || (unique && ordered)) return CATALOG_INVALID;
    if (find_index(catalog, name, name_bytes) != NULL || find_by_name(catalog, name, name_bytes) != NULL) {
        return CATALOG_EXISTS;
    }
    if (catalog->index_count == CATALOG_MAX_INDEXES) return CATALOG_LIMIT;
    size_t extra = INDEX_FIXED_BYTES + name_bytes + (catalog->index_count == 0 ? 4u : 0u);
    if (extra > CATALOG_MAX_ENCODED_BYTES - catalog->encoded_bytes) return CATALOG_LIMIT;
    catalog_status_t status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;
    char *copied = malloc(name_bytes);
    if (copied == NULL) return CATALOG_NOMEM;
    if (catalog->index_count == catalog->index_capacity) {
        size_t capacity = catalog->index_capacity == 0 ? 4 : catalog->index_capacity * 2;
        index_t *grown = realloc(catalog->indexes, capacity * sizeof(*grown));
        if (grown == NULL) {
            free(copied);
            return CATALOG_NOMEM;
        }
        catalog->indexes = grown;
        catalog->index_capacity = capacity;
    }
    memcpy(copied, name, name_bytes);
    catalog->indexes[catalog->index_count++] =
        (index_t){copied, name_bytes, table_id, column, unique, ordered, CATALOG_NO_PAGE};
    catalog->encoded_bytes += extra;
    catalog->generation++;
    return CATALOG_OK;
}

catalog_status_t catalog_set_index_root(catalog_t *catalog, const char *name, size_t name_bytes,
                                        uint32_t root)
{
    if (catalog == NULL || name == NULL) return CATALOG_INVALID;
    index_t *index = find_index(catalog, name, name_bytes);
    if (index == NULL) return CATALOG_NOT_FOUND;
    catalog_status_t status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;
    index->root = root;
    catalog->generation++;
    return CATALOG_OK;
}

size_t catalog_index_count(const catalog_t *catalog, catalog_table_id_t table_id)
{
    if (catalog == NULL) return 0;
    size_t count = 0;
    for (size_t i = 0; i < catalog->index_count; i++) count += catalog->indexes[i].table_id == table_id;
    return count;
}

catalog_status_t catalog_index_at(const catalog_t *catalog, catalog_table_id_t table_id,
                                  size_t position, catalog_index_view_t *view)
{
    if (catalog == NULL || view == NULL) return CATALOG_INVALID;
    for (size_t i = 0; i < catalog->index_count; i++) {
        const index_t *index = &catalog->indexes[i];
        if (index->table_id != table_id) continue;
        if (position-- > 0) continue;
        *view = (catalog_index_view_t){index->name, index->name_bytes, index->table_id,
                                       index->column, index->unique, index->ordered, index->root};
        return CATALOG_OK;
    }
    return CATALOG_NOT_FOUND;
}

catalog_status_t catalog_reserve_identity(catalog_t *catalog, catalog_table_id_t id,
                                          uint64_t *value)
{
    if (catalog == NULL || value == NULL) return CATALOG_INVALID;
    table_t *table = find_mutable(catalog, id);
    if (table == NULL) return CATALOG_NOT_FOUND;
    if (!table->has_identity) return CATALOG_INVALID;
    if (table->next_identity >= IDENTITY_END) return CATALOG_LIMIT;
    catalog_status_t status = check_can_mutate(catalog);
    if (status != CATALOG_OK) return status;
    *value = table->next_identity++;
    catalog->generation++;
    return CATALOG_OK;
}

uint64_t catalog_generation(const catalog_t *catalog)
{
    return catalog == NULL ? 0 : catalog->generation;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    for (unsigned i = 0; i < 2; i++) bytes[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t checksum(const uint8_t *bytes, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static uint8_t *encode_table(uint8_t *at, const table_t *table)
{
    le_put_u32(at, table->id);
    put_u16(at + 4, (uint16_t)table->name_bytes);
    put_u16(at + 6, table->primary_key_column);
    put_u16(at + 8, (uint16_t)table->column_count);
    at[10] = table->has_identity ? FLAG_IDENTITY : 0;
    le_put_u64(at + 11, table->row_count);
    le_put_u64(at + 19, table->next_identity);
    le_put_u32(at + 27, table->heap_root);
    le_put_u32(at + 31, table->primary_root);
    at += TABLE_FIXED_BYTES;
    memcpy(at, table->names, table->name_bytes);
    at += table->name_bytes;
    for (size_t i = 0; i < table->column_count; i++) {
        const catalog_column_t *column = &table->columns[i];
        at[0] = (uint8_t)column->type;
        at[1] = (uint8_t)((column->not_null ? FLAG_NOT_NULL : 0) | (column->has_default ? FLAG_DEFAULT : 0));
        put_u16(at + 2, (uint16_t)column->name_bytes);
        memcpy(at + COLUMN_FIXED_BYTES, column->name, column->name_bytes);
        at += COLUMN_FIXED_BYTES + column->name_bytes;
        if (!column->has_default) continue;
        const value_view_t *value = &column->default_value;
        *at++ = (uint8_t)value->type;
        if (value->type == VALUE_INTEGER) {
            le_put_u64(at, (uint64_t)value->integer);
            at += 8;
        } else if (value->type == VALUE_REAL) {
            uint64_t bits;
            memcpy(&bits, &value->real, sizeof(bits));
            le_put_u64(at, bits);
            at += 8;
        } else {
            le_put_u32(at, (uint32_t)value->size_bytes);
            if (value->size_bytes > 0) memcpy(at + 4, value->bytes, value->size_bytes);
            at += 4 + value->size_bytes;
        }
    }
    return at;
}

static bool has_defaults(const catalog_t *catalog)
{
    for (size_t t = 0; t < catalog->count; t++) {
        for (size_t i = 0; i < catalog->tables[t].column_count; i++) {
            if (catalog->tables[t].columns[i].has_default) return true;
        }
    }
    return false;
}

catalog_status_t catalog_encode(const catalog_t *catalog, uint8_t **bytes, size_t *size)
{
    if (catalog == NULL || bytes == NULL || size == NULL) return CATALOG_INVALID;
    size_t total = catalog->encoded_bytes;
    uint8_t *buffer = malloc(total);
    if (buffer == NULL) return CATALOG_NOMEM;
    memcpy(buffer, "LRCT", 4);
    put_u16(buffer + 4, has_defaults(catalog)          ? DEFAULT_FORMAT_VERSION
                        : catalog->index_count > 0 ? INDEX_FORMAT_VERSION
                                                   : FORMAT_VERSION);
    put_u16(buffer + 6, 0);
    le_put_u32(buffer + 8, (uint32_t)total);
    le_put_u64(buffer + 12, catalog->generation);
    le_put_u32(buffer + 20, catalog->next_table_id);
    le_put_u32(buffer + 24, (uint32_t)catalog->count);
    uint8_t *at = buffer + HEADER_BYTES;
    for (size_t i = 0; i < catalog->count; i++) at = encode_table(at, &catalog->tables[i]);
    if (catalog->index_count > 0) {
        le_put_u32(at, (uint32_t)catalog->index_count);
        at += 4;
        for (size_t i = 0; i < catalog->index_count; i++) {
            const index_t *index = &catalog->indexes[i];
            le_put_u32(at, index->table_id);
            put_u16(at + 4, index->column);
            put_u16(at + 6, (uint16_t)index->name_bytes);
            at[8] = (uint8_t)((index->unique ? INDEX_UNIQUE : 0) | (index->ordered ? INDEX_ORDERED : 0));
            le_put_u32(at + 9, index->root);
            memcpy(at + INDEX_FIXED_BYTES, index->name, index->name_bytes);
            at += INDEX_FIXED_BYTES + index->name_bytes;
        }
    }
    le_put_u32(at, checksum(buffer, (size_t)(at - buffer)));
    *bytes = buffer;
    *size = total;
    return CATALOG_OK;
}

typedef struct {
    const uint8_t *at;
    size_t left;
    bool allow_default;          /* 형식 버전 3 */
} reader_t;

static bool take(reader_t *reader, size_t count, const uint8_t **out)
{
    if (reader->left < count) return false;
    *out = reader->at;
    reader->at += count;
    reader->left -= count;
    return true;
}

static uint64_t get_le(const uint8_t *bytes, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++) value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

/* 컬럼 하나를 읽어 spec에 옮긴다. 이름·기본값 바이트는 입력 buffer를 빌리고 기본값은 *value에 둔다. */
static catalog_status_t read_column(reader_t *reader, catalog_column_spec_t *column, value_view_t *value)
{
    const uint8_t *fixed, *name;
    if (!take(reader, COLUMN_FIXED_BYTES, &fixed)) return CATALOG_CORRUPT;
    size_t name_bytes = (size_t)get_le(fixed + 2, 2);
    uint8_t allowed = reader->allow_default ? (FLAG_NOT_NULL | FLAG_DEFAULT) : FLAG_NOT_NULL;
    if ((fixed[1] & ~allowed) != 0 || !column_type_valid((value_type_t)fixed[0])) {
        return CATALOG_CORRUPT;
    }
    if (!take(reader, name_bytes, &name) || !name_valid((const char *)name, name_bytes)) {
        return CATALOG_CORRUPT;
    }
    *column = (catalog_column_spec_t){(const char *)name, name_bytes, (value_type_t)fixed[0],
                                      (fixed[1] & FLAG_NOT_NULL) != 0, NULL};
    if ((fixed[1] & FLAG_DEFAULT) == 0) return CATALOG_OK;
    const uint8_t *type, *payload;
    if (!take(reader, 1, &type) || *type != fixed[0]) return CATALOG_CORRUPT;
    *value = (value_view_t){0};
    value->type = (value_type_t)*type;
    if (value->type == VALUE_INTEGER || value->type == VALUE_REAL) {
        if (!take(reader, 8, &payload)) return CATALOG_CORRUPT;
        uint64_t bits = get_le(payload, 8);
        if (value->type == VALUE_INTEGER) value->integer = (int64_t)bits;
        else memcpy(&value->real, &bits, sizeof(bits));
    } else {
        const uint8_t *length;
        if (!take(reader, 4, &length)) return CATALOG_CORRUPT;
        value->size_bytes = (size_t)get_le(length, 4);
        if (value->size_bytes > CATALOG_DEFAULT_MAX_BYTES || !take(reader, value->size_bytes, &payload)) {
            return CATALOG_CORRUPT;
        }
        value->bytes = value->size_bytes > 0 ? payload : NULL;
    }
    column->default_value = value;
    return CATALOG_OK;
}

/* 한 테이블을 검증하며 catalog에 붙인다. 길이는 남은 바이트와 대조한 뒤에만 할당에 쓴다. */
static catalog_status_t read_table(reader_t *reader, catalog_t *catalog)
{
    const uint8_t *fixed, *name;
    if (!take(reader, TABLE_FIXED_BYTES, &fixed)) return CATALOG_CORRUPT;
    catalog_table_id_t id = (catalog_table_id_t)get_le(fixed, 4);
    size_t name_bytes = (size_t)get_le(fixed + 4, 2);
    size_t column_count = (size_t)get_le(fixed + 8, 2);
    if (fixed[10] > FLAG_IDENTITY) return CATALOG_CORRUPT;
    bool has_identity = fixed[10] == FLAG_IDENTITY;
    uint64_t next_identity = get_le(fixed + 19, 8);
    if (id == CATALOG_NO_TABLE || id >= catalog->next_table_id) return CATALOG_CORRUPT;
    if (catalog->count > 0 && catalog->tables[catalog->count - 1].id >= id) return CATALOG_CORRUPT;
    if (column_count < 1 || column_count > CATALOG_MAX_COLUMNS) return CATALOG_CORRUPT;
    if (reader->left / COLUMN_FIXED_BYTES < column_count) return CATALOG_CORRUPT;
    if (!take(reader, name_bytes, &name) || !name_valid((const char *)name, name_bytes)) {
        return CATALOG_CORRUPT;
    }
    catalog_column_spec_t *columns = malloc(column_count * sizeof(*columns));
    value_view_t *defaults = malloc(column_count * sizeof(*defaults));
    if (columns == NULL || defaults == NULL) {
        free(columns);
        free(defaults);
        return CATALOG_NOMEM;
    }
    catalog_status_t status = CATALOG_OK;
    for (size_t i = 0; i < column_count && status == CATALOG_OK; i++) {
        status = read_column(reader, &columns[i], &defaults[i]);
    }
    catalog_table_spec_t spec = {(const char *)name, name_bytes, columns, column_count,
                                 (uint16_t)get_le(fixed + 6, 2), has_identity};
    if (status == CATALOG_OK) status = check_spec(&spec) == CATALOG_OK ? CATALOG_OK : CATALOG_CORRUPT;
    if (status == CATALOG_OK) {
        bool identity_ok = has_identity ? next_identity >= 1 && next_identity <= IDENTITY_END
                                        : next_identity == 0;
        if (!identity_ok) status = CATALOG_CORRUPT;
    }
    if (status == CATALOG_OK) {
        status = check_spec_names(&spec);
        if (status == CATALOG_INVALID) status = CATALOG_CORRUPT;
    }
    if (status == CATALOG_OK) status = reserve_slot(catalog);
    table_t table = {0};
    if (status == CATALOG_OK) status = build_table(&table, id, &spec);
    free(columns);
    free(defaults);
    if (status != CATALOG_OK) return status;
    table.row_count = get_le(fixed + 11, 8);
    table.next_identity = next_identity;
    table.heap_root = (uint32_t)get_le(fixed + 27, 4);
    table.primary_root = (uint32_t)get_le(fixed + 31, 4);
    catalog->tables[catalog->count++] = table;
    catalog->encoded_bytes += table.encoded_bytes;
    return CATALOG_OK;
}

static catalog_status_t check_table_names_unique(const catalog_t *catalog)
{
    if (catalog->count < 2) return CATALOG_OK;
    name_ref_t *refs = malloc(catalog->count * sizeof(*refs));
    if (refs == NULL) return CATALOG_NOMEM;
    for (size_t i = 0; i < catalog->count; i++) {
        refs[i] = (name_ref_t){catalog->tables[i].names, catalog->tables[i].name_bytes};
    }
    catalog_status_t status = check_unique_names(refs, catalog->count);
    free(refs);
    return status == CATALOG_EXISTS ? CATALOG_CORRUPT : status;
}

static catalog_status_t check_envelope(const uint8_t *bytes, size_t size)
{
    if (size < HEADER_BYTES + CHECKSUM_BYTES) return CATALOG_CORRUPT;
    if (size > CATALOG_MAX_ENCODED_BYTES) return CATALOG_LIMIT;
    if (memcmp(bytes, "LRCT", 4) != 0) return CATALOG_CORRUPT;
    uint64_t version = get_le(bytes + 4, 2);
    if (version != FORMAT_VERSION && version != INDEX_FORMAT_VERSION && version != DEFAULT_FORMAT_VERSION) {
        return CATALOG_UNSUPPORTED;
    }
    if (get_le(bytes + 6, 2) != 0) return CATALOG_CORRUPT;
    if (get_le(bytes + 8, 4) != size) return CATALOG_CORRUPT;
    if (get_le(bytes + size - CHECKSUM_BYTES, 4) != checksum(bytes, size - CHECKSUM_BYTES)) {
        return CATALOG_CORRUPT;
    }
    return CATALOG_OK;
}

catalog_status_t catalog_decode(const uint8_t *bytes, size_t size, catalog_t **out)
{
    if (out == NULL || (bytes == NULL && size > 0)) return CATALOG_INVALID;
    catalog_status_t status = check_envelope(bytes, size);
    if (status != CATALOG_OK) return status;
    size_t table_count = (size_t)get_le(bytes + 24, 4);
    size_t payload = size - HEADER_BYTES - CHECKSUM_BYTES;
    if (table_count > payload / MIN_TABLE_BYTES) return CATALOG_CORRUPT;
    catalog_t *catalog;
    status = catalog_open_empty(&catalog);
    if (status != CATALOG_OK) return status;
    catalog->generation = 0;
    catalog->next_table_id = (uint32_t)get_le(bytes + 20, 4);
    if (catalog->next_table_id == CATALOG_NO_TABLE) status = CATALOG_CORRUPT;
    if (status == CATALOG_OK && table_count > 0) {
        catalog->tables = malloc(table_count * sizeof(*catalog->tables));
        if (catalog->tables == NULL) status = CATALOG_NOMEM;
        else catalog->capacity = table_count;
    }
    uint64_t version = get_le(bytes + 4, 2);
    reader_t reader = {bytes + HEADER_BYTES, payload, version == DEFAULT_FORMAT_VERSION};
    for (size_t i = 0; i < table_count && status == CATALOG_OK; i++) {
        status = read_table(&reader, catalog);
    }
    if (status == CATALOG_OK &&
        (version == INDEX_FORMAT_VERSION || (version == DEFAULT_FORMAT_VERSION && reader.left > 0))) {
        const uint8_t *count_bytes;
        if (!take(&reader, 4u, &count_bytes)) status = CATALOG_CORRUPT;
        else {
            size_t index_count = (size_t)get_le(count_bytes, 4);
            if (index_count == 0 || index_count > CATALOG_MAX_INDEXES ||
                index_count > reader.left / (INDEX_FIXED_BYTES + 1u)) status = CATALOG_CORRUPT;
            for (size_t i = 0; i < index_count && status == CATALOG_OK; i++) {
                const uint8_t *fixed, *name;
                if (!take(&reader, INDEX_FIXED_BYTES, &fixed)) {
                    status = CATALOG_CORRUPT;
                    break;
                }
                size_t name_bytes = (size_t)get_le(fixed + 6, 2);
                if (fixed[8] > (INDEX_UNIQUE | INDEX_ORDERED) || !take(&reader, name_bytes, &name)) {
                    status = CATALOG_CORRUPT;
                    break;
                }
                status = catalog_add_index(catalog, (catalog_table_id_t)get_le(fixed, 4),
                                           (const char *)name, name_bytes,
                                           (uint16_t)get_le(fixed + 4, 2), (fixed[8] & INDEX_UNIQUE) != 0,
                                           (fixed[8] & INDEX_ORDERED) != 0);
                if (status == CATALOG_OK) {
                    catalog->indexes[catalog->index_count - 1].root = (uint32_t)get_le(fixed + 9, 4);
                } else if (status != CATALOG_NOMEM) status = CATALOG_CORRUPT;
            }
        }
    }
    if (status == CATALOG_OK && (reader.left != 0 || catalog->encoded_bytes != size)) {
        status = CATALOG_CORRUPT;
    }
    if (status == CATALOG_OK) catalog->generation = get_le(bytes + 12, 8);
    if (status == CATALOG_OK) status = check_table_names_unique(catalog);
    if (status != CATALOG_OK) {
        catalog_close(catalog);
        return status;
    }
    *out = catalog;
    return CATALOG_OK;
}

/* 테이블 하나가 쥔 이름(과 기본값) 배열과 컬럼 배열 */
static size_t table_memory_bytes(const table_t *table)
{
    return table->names_capacity + table->column_count * sizeof(catalog_column_t);
}

size_t catalog_memory_bytes(const catalog_t *catalog)
{
    if (catalog == NULL) return 0;
    size_t total = sizeof(*catalog) + catalog->capacity * sizeof(table_t);
    for (size_t i = 0; i < catalog->count; i++) total += table_memory_bytes(&catalog->tables[i]);
    total += catalog->index_capacity * sizeof(index_t);
    for (size_t i = 0; i < catalog->index_count; i++) total += catalog->indexes[i].name_bytes;
    return total;
}

size_t catalog_clone_peak_bytes(const catalog_t *catalog)
{
    if (catalog == NULL) return 0;
    size_t decoded = sizeof(*catalog) + catalog->count * sizeof(table_t);
    size_t widest = 0;
    for (size_t i = 0; i < catalog->count; i++) {
        decoded += table_memory_bytes(&catalog->tables[i]);
        if (catalog->tables[i].column_count > widest) widest = catalog->tables[i].column_count;
    }
    decoded += catalog->index_capacity * sizeof(index_t);
    /* 복제 중 realloc은 이전 배열과 새 배열이 동시에 살아 있을 수 있다. */
    if (catalog->index_capacity > 4) decoded += (catalog->index_capacity / 2u) * sizeof(index_t);
    for (size_t i = 0; i < catalog->index_count; i++) decoded += catalog->indexes[i].name_bytes;
    /* read_table의 임시 spec 배열과 이름 검사 refs, 마지막의 테이블 이름 refs */
    size_t per_table = widest * (sizeof(catalog_column_spec_t) + sizeof(value_view_t) + sizeof(name_ref_t));
    size_t names = catalog->count * sizeof(name_ref_t);
    size_t scratch = per_table > names ? per_table : names;
    return catalog->encoded_bytes + decoded + scratch;
}

size_t catalog_create_peak_bytes(const catalog_t *catalog, size_t column_count, size_t names_bytes)
{
    if (catalog == NULL) return 0;
    size_t refs = column_count * sizeof(name_ref_t);
    size_t built = names_bytes + column_count * sizeof(catalog_column_t);
    size_t grow = 0;
    if (catalog->count == catalog->capacity) {
        size_t next = catalog->capacity == 0 ? 4 : catalog->capacity * 2;
        grow = (catalog->capacity + next) * sizeof(table_t);
    }
    return refs + built + grow;
}

size_t catalog_add_index_peak_bytes(const catalog_t *catalog, size_t name_bytes)
{
    if (catalog == NULL) return 0;
    size_t grow = 0;
    if (catalog->index_count == catalog->index_capacity) {
        size_t next = catalog->index_capacity == 0 ? 4 : catalog->index_capacity * 2;
        grow = (catalog->index_capacity + next) * sizeof(index_t);
    }
    return name_bytes + grow;
}

size_t catalog_encoded_bytes(const catalog_t *catalog)
{
    return catalog == NULL ? 0 : catalog->encoded_bytes;
}

size_t catalog_decode_peak_bytes(size_t encoded_bytes)
{
    size_t tables = encoded_bytes / MIN_TABLE_BYTES;
    size_t columns = encoded_bytes / (COLUMN_FIXED_BYTES + 1u);
    size_t memory = sizeof(catalog_t) + tables * sizeof(table_t) + encoded_bytes +
                    columns * sizeof(catalog_column_t);
    size_t indexes = encoded_bytes / (INDEX_FIXED_BYTES + 1u);
    if (indexes > CATALOG_MAX_INDEXES) indexes = CATALOG_MAX_INDEXES;
    memory += indexes * sizeof(index_t) * 3u + encoded_bytes;
    size_t scratch = columns * (sizeof(catalog_column_spec_t) + sizeof(value_view_t) + sizeof(name_ref_t)) +
                     tables * sizeof(name_ref_t);
    return memory + scratch;
}
