#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/catalog.h"

static int calls;
static int fail_at;
static long live;

void *catalog_test_malloc(size_t size)
{
    if (++calls == fail_at) return NULL;
    void *memory = malloc(size);
    if (memory != NULL) live++;
    return memory;
}

void *catalog_test_realloc(void *old, size_t size)
{
    if (++calls == fail_at) return NULL;
    void *memory = realloc(old, size);
    if (memory != NULL && old == NULL) live++;
    return memory;
}

void catalog_test_free(void *memory)
{
    if (memory != NULL) live--;
    free(memory);
}

static catalog_column_spec_t column(const char *name, value_type_t type)
{
    return (catalog_column_spec_t){name, strlen(name), type, false};
}

static catalog_table_id_t add(catalog_t *catalog, const char *name)
{
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER), column("payload", VALUE_BLOB)};
    catalog_table_spec_t spec = {name, strlen(name), columns, 2, 0, true};
    catalog_table_id_t id = 0;
    assert(catalog_create_table(catalog, &spec, &id) == CATALOG_OK);
    return id;
}

static catalog_t *four_tables(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    const char *names[] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < 4; i++) assert(add(catalog, names[i]) == i + 1);
    return catalog;
}

typedef struct {
    uint8_t *bytes;
    size_t size;
} snapshot_t;

static snapshot_t snapshot(const catalog_t *catalog)
{
    snapshot_t shot;
    int saved = fail_at;
    fail_at = 0;
    assert(catalog_encode(catalog, &shot.bytes, &shot.size) == CATALOG_OK);
    fail_at = saved;
    return shot;
}

static void assert_same(const catalog_t *catalog, const snapshot_t *before)
{
    snapshot_t now = snapshot(catalog);
    assert(now.size == before->size && memcmp(now.bytes, before->bytes, now.size) == 0);
    catalog_test_free(now.bytes);
}

/*
 * #6: 어느 할당이 실패해도 NOMEM이고 카탈로그·카운터·출력 인자가 그대로이며 새어 나간 메모리가 없다.
 * 각 호출이 필요한 할당 횟수보다 큰 fail_at에서 처음 성공한다.
 */
static void create_and_import_preserve_catalog(bool legacy)
{
    catalog_t *catalog = four_tables();
    snapshot_t before = snapshot(catalog);
    uint64_t generation = catalog_generation(catalog);
    long baseline = live;
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER), column("payload", VALUE_BLOB)};
    catalog_legacy_table_t entry = {{"e", 1, columns, 2, 0, true}, 9, 4, 8, 7};
    int failures = 0;
    for (int step = 1; step <= 40; step++) {
        catalog_table_id_t id = 99;
        calls = 0;
        fail_at = step;
        catalog_status_t status = legacy ? catalog_import_legacy(catalog, &entry, &id)
                                         : catalog_create_table(catalog, &entry.table, &id);
        fail_at = 0;
        if (status == CATALOG_OK) {
            assert(calls < step && id == 5);
            break;
        }
        assert(status == CATALOG_NOMEM && id == 99);
        assert(live == baseline && catalog_generation(catalog) == generation);
        assert_same(catalog, &before);
        failures++;
    }
    assert(failures >= 4);
    catalog_table_view_t view;
    assert(catalog_describe(catalog, 5, &view) == CATALOG_OK);
    assert(view.row_count == (legacy ? 9u : 0u));
    catalog_test_free(before.bytes);
    catalog_close(catalog);
    assert(live == 0);
}

static void open_empty_failure_keeps_output(void)
{
    static char marker;
    for (int step = 1; step <= 4; step++) {
        catalog_t *out = (catalog_t *)&marker;
        calls = 0;
        fail_at = step;
        catalog_status_t status = catalog_open_empty(&out);
        fail_at = 0;
        if (status == CATALOG_OK) {
            catalog_close(out);
            break;
        }
        assert(status == CATALOG_NOMEM && out == (catalog_t *)&marker && live == 0);
    }
    assert(live == 0);
}

static void encode_failure_keeps_output(void)
{
    catalog_t *catalog = four_tables();
    uint8_t *bytes = (uint8_t *)&calls;
    size_t size = 123;
    calls = 0;
    fail_at = 1;
    assert(catalog_encode(catalog, &bytes, &size) == CATALOG_NOMEM);
    fail_at = 0;
    assert(bytes == (uint8_t *)&calls && size == 123);
    catalog_close(catalog);
    assert(live == 0);
}

static void decode_failure_keeps_output_and_frees_partial_work(void)
{
    catalog_t *catalog = four_tables();
    snapshot_t shot = snapshot(catalog);
    long baseline = live;
    static char marker;
    int failures = 0;
    for (int step = 1; step <= 200; step++) {
        catalog_t *out = (catalog_t *)&marker;
        calls = 0;
        fail_at = step;
        catalog_status_t status = catalog_decode(shot.bytes, shot.size, &out);
        fail_at = 0;
        if (status == CATALOG_OK) {
            assert(calls < step);
            assert_same(out, &shot);
            catalog_close(out);
            break;
        }
        assert(status == CATALOG_NOMEM && out == (catalog_t *)&marker);
        assert(live == baseline);
        failures++;
    }
    assert(failures >= 8);
    catalog_test_free(shot.bytes);
    catalog_close(catalog);
    assert(live == 0);
}

int main(void)
{
    create_and_import_preserve_catalog(false);
    create_and_import_preserve_catalog(true);
    open_empty_failure_keeps_output();
    encode_failure_keeps_output();
    decode_failure_keeps_output_and_frees_partial_work();
    puts("catalog: allocation failures keep catalog, counters and outputs; no leaks");
    return 0;
}
