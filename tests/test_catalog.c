#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/catalog.h"

#define HEADER 28u

typedef struct {
    uint8_t *bytes;
    size_t size;
} snapshot_t;

static snapshot_t snapshot(const catalog_t *catalog)
{
    snapshot_t shot = {0};
    assert(catalog_encode(catalog, &shot.bytes, &shot.size) == CATALOG_OK);
    return shot;
}

static void assert_same(const catalog_t *catalog, const snapshot_t *before)
{
    snapshot_t now = snapshot(catalog);
    assert(now.size == before->size && memcmp(now.bytes, before->bytes, now.size) == 0);
    free(now.bytes);
}

static catalog_column_spec_t column(const char *name, value_type_t type, bool not_null)
{
    return (catalog_column_spec_t){name, strlen(name), type, not_null, NULL};
}

static catalog_status_t create(catalog_t *catalog, const char *name,
                               const catalog_column_spec_t *columns, size_t count,
                               uint16_t key, bool identity, catalog_table_id_t *id)
{
    catalog_table_spec_t spec = {name, strlen(name), columns, count, key, identity};
    return catalog_create_table(catalog, &spec, id);
}

static catalog_table_id_t create_users(catalog_t *catalog)
{
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER, true),
                                       column("name", VALUE_TEXT, false),
                                       column("age", VALUE_INTEGER, false)};
    catalog_table_id_t id = 0;
    assert(create(catalog, "users", columns, 3, 0, true, &id) == CATALOG_OK);
    return id;
}

static catalog_table_id_t create_orders(catalog_t *catalog)
{
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER, true),
                                       column("total", VALUE_REAL, true),
                                       column("note", VALUE_BLOB, false)};
    catalog_table_id_t id = 0;
    assert(create(catalog, "orders", columns, 3, 0, false, &id) == CATALOG_OK);
    return id;
}

static catalog_table_view_t describe(const catalog_t *catalog, catalog_table_id_t id)
{
    catalog_table_view_t view;
    assert(catalog_describe(catalog, id, &view) == CATALOG_OK);
    return view;
}

/* #6: 같은 기본 키 위치와 컬럼 이름을 가진 두 테이블의 상태가 서로 영향을 주지 않는다. */
static void tables_are_independent(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_table_id_t users = create_users(catalog), orders = create_orders(catalog);
    assert(users == 1 && orders == 2 && catalog_table_count(catalog) == 2);

    catalog_table_view_t user_view = describe(catalog, users), order_view = describe(catalog, orders);
    assert(user_view.column_count == 3 && order_view.column_count == 3);
    assert(user_view.columns[1].type == VALUE_TEXT && order_view.columns[1].type == VALUE_REAL);
    assert(user_view.columns[2].type == VALUE_INTEGER && order_view.columns[2].type == VALUE_BLOB);
    assert(user_view.primary_key_column == 0 && order_view.primary_key_column == 0);
    assert(user_view.has_identity && !order_view.has_identity);
    assert(user_view.next_identity == 1 && order_view.next_identity == 0);
    assert(user_view.heap_root == CATALOG_NO_PAGE && user_view.primary_root == CATALOG_NO_PAGE);

    assert(catalog_set_row_count(catalog, users, 7) == CATALOG_OK);
    assert(catalog_set_locators(catalog, users, 11, 12) == CATALOG_OK);
    assert(catalog_set_locators(catalog, orders, 21, 22) == CATALOG_OK);
    uint64_t value = 0;
    assert(catalog_reserve_identity(catalog, users, &value) == CATALOG_OK && value == 1);
    assert(catalog_reserve_identity(catalog, users, &value) == CATALOG_OK && value == 2);
    assert(catalog_reserve_identity(catalog, orders, &value) == CATALOG_INVALID && value == 2);

    user_view = describe(catalog, users);
    order_view = describe(catalog, orders);
    assert(user_view.row_count == 7 && user_view.next_identity == 3);
    assert(user_view.heap_root == 11 && user_view.primary_root == 12);
    assert(order_view.row_count == 0 && order_view.next_identity == 0);
    assert(order_view.heap_root == 21 && order_view.primary_root == 22);

    catalog_table_id_t found = 0;
    assert(catalog_find(catalog, "orders", 6, &found) == CATALOG_OK && found == orders);
    assert(catalog_find(catalog, "Orders", 6, &found) == CATALOG_NOT_FOUND && found == orders);
    assert(catalog_find(catalog, "order", 5, &found) == CATALOG_NOT_FOUND);
    catalog_table_view_t at;
    assert(catalog_table_at(catalog, 0, &at) == CATALOG_OK && at.id == users);
    assert(catalog_table_at(catalog, 1, &at) == CATALOG_OK && at.id == orders);
    assert(catalog_table_at(catalog, 2, &at) == CATALOG_NOT_FOUND);
    catalog_close(catalog);
}

static void duplicate_and_missing_keep_catalog(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_table_id_t users = create_users(catalog);
    assert(catalog_set_row_count(catalog, users, 3) == CATALOG_OK);
    snapshot_t before = snapshot(catalog);
    uint64_t generation = catalog_generation(catalog);

    catalog_column_spec_t one[] = {column("x", VALUE_INTEGER, false)};
    catalog_table_id_t id = 99;
    assert(create(catalog, "users", one, 1, CATALOG_NO_COLUMN, false, &id) == CATALOG_EXISTS);
    assert(id == 99);
    assert_same(catalog, &before);
    assert(catalog_generation(catalog) == generation);

    catalog_table_view_t view = {0};
    assert(catalog_describe(catalog, 0, &view) == CATALOG_NOT_FOUND);
    assert(catalog_describe(catalog, 2, &view) == CATALOG_NOT_FOUND);
    assert(catalog_drop_table(catalog, 2) == CATALOG_NOT_FOUND);
    assert(catalog_drop_table(catalog, CATALOG_NO_TABLE) == CATALOG_NOT_FOUND);
    assert(catalog_set_row_count(catalog, 2, 1) == CATALOG_NOT_FOUND);
    assert(catalog_set_locators(catalog, 2, 1, 1) == CATALOG_NOT_FOUND);
    uint64_t value = 77;
    assert(catalog_reserve_identity(catalog, 2, &value) == CATALOG_NOT_FOUND && value == 77);
    assert_same(catalog, &before);
    assert(catalog_generation(catalog) == generation);

    /* 실패한 create는 ID를 소비하지 않는다. */
    catalog_table_id_t next = 0;
    assert(create(catalog, "next", one, 1, CATALOG_NO_COLUMN, false, &next) == CATALOG_OK);
    assert(next == users + 1);
    free(before.bytes);
    catalog_close(catalog);
}

static void invalid_specs_keep_catalog(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    create_users(catalog);
    snapshot_t before = snapshot(catalog);
    catalog_column_spec_t good[] = {column("id", VALUE_INTEGER, true), column("v", VALUE_TEXT, false)};
    catalog_column_spec_t null_type[] = {column("id", VALUE_NULL, false)};
    catalog_column_spec_t bad_type[] = {column("id", (value_type_t)9, false)};
    catalog_column_spec_t dup[] = {column("a", VALUE_INTEGER, false), column("a", VALUE_TEXT, false)};
    catalog_column_spec_t real_key[] = {column("k", VALUE_REAL, false)};
    catalog_column_spec_t empty_name[] = {{"", 0, VALUE_INTEGER, false, NULL}};
    catalog_column_spec_t nul_name[] = {{"a\0b", 3, VALUE_INTEGER, false, NULL}};
    char long_name[257];
    memset(long_name, 'n', 256);
    long_name[256] = '\0';
    catalog_column_spec_t long_column[] = {{long_name, 256, VALUE_INTEGER, false, NULL}};
    catalog_column_spec_t too_many[1025];
    for (size_t i = 0; i < 1025; i++) {
        static char names[1025][8];
        snprintf(names[i], sizeof(names[i]), "c%zu", i);
        too_many[i] = column(names[i], VALUE_INTEGER, false);
    }

    struct { const char *name; size_t name_bytes; const catalog_column_spec_t *columns;
             size_t count; uint16_t key; bool identity; } cases[] = {
        {"t", 1, good, 2, 0, false},  /* 기준: 아래 변형이 이 하나만 다르다 */
        {"", 0, good, 2, 0, false}, {"a\0b", 3, good, 2, 0, false}, {long_name, 256, good, 2, 0, false},
        {NULL, 1, good, 2, 0, false}, {"t", 1, NULL, 2, 0, false}, {"t", 1, good, 0, 0, false},
        {"t", 1, too_many, 1025, 0, false}, {"t", 1, null_type, 1, 0, false},
        {"t", 1, bad_type, 1, 0, false}, {"t", 1, dup, 2, 0, false},
        {"t", 1, empty_name, 1, 0, false}, {"t", 1, nul_name, 1, 0, false},
        {"t", 1, long_column, 1, 0, false}, {"t", 1, good, 2, 2, false},
        {"t", 1, good, 2, 1, true}, {"t", 1, real_key, 1, 0, true},
        {"t", 1, good, 2, CATALOG_NO_COLUMN, true}};
    for (size_t i = 1; i < sizeof(cases) / sizeof(cases[0]); i++) {
        catalog_table_spec_t spec = {cases[i].name, cases[i].name_bytes, cases[i].columns,
                                     cases[i].count, cases[i].key, cases[i].identity};
        catalog_table_id_t id = 99;
        assert(catalog_create_table(catalog, &spec, &id) == CATALOG_INVALID);
        assert(id == 99);
        assert_same(catalog, &before);
    }
    catalog_table_id_t id = 99;
    assert(catalog_create_table(catalog, NULL, &id) == CATALOG_INVALID);
    assert(catalog_create_table(NULL, &(catalog_table_spec_t){0}, &id) == CATALOG_INVALID);
    catalog_table_spec_t spec = {"t", 1, good, 2, 0, false};
    assert(catalog_create_table(catalog, &spec, NULL) == CATALOG_INVALID);
    assert_same(catalog, &before);

    /* 이름 255바이트와 컬럼 1024개 경계는 허용한다. */
    static catalog_column_spec_t many[1024];
    static char names[1024][8];
    for (size_t i = 0; i < 1024; i++) {
        snprintf(names[i], sizeof(names[i]), "c%zu", i);
        many[i] = column(names[i], VALUE_BLOB, true);
    }
    assert(create(catalog, "wide", many, 1024, 1023, false, &id) == CATALOG_OK);
    assert(catalog_describe(catalog, id, &(catalog_table_view_t){0}) == CATALOG_OK);
    assert(create(catalog, long_name + 1, good, 2, 0, false, &id) == CATALOG_OK);
    free(before.bytes);
    catalog_close(catalog);
}

/* ID는 drop 뒤에도, 인코딩을 거쳐서도 재사용하지 않는다. */
static void ids_are_stable_and_never_reused(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_column_spec_t one[] = {column("x", VALUE_INTEGER, false)};
    catalog_table_id_t a, b, c, d, e;
    assert(create(catalog, "a", one, 1, 0, false, &a) == CATALOG_OK);
    assert(create(catalog, "b", one, 1, 0, false, &b) == CATALOG_OK);
    assert(create(catalog, "c", one, 1, 0, false, &c) == CATALOG_OK);
    assert(catalog_set_row_count(catalog, c, 5) == CATALOG_OK);
    assert(catalog_drop_table(catalog, b) == CATALOG_OK);
    assert(catalog_drop_table(catalog, b) == CATALOG_NOT_FOUND);
    assert(describe(catalog, a).id == 1 && describe(catalog, c).id == 3);
    assert(describe(catalog, c).row_count == 5);
    catalog_table_id_t found = 0;
    assert(catalog_find(catalog, "b", 1, &found) == CATALOG_NOT_FOUND);
    assert(create(catalog, "d", one, 1, 0, false, &d) == CATALOG_OK && d == 4);
    assert(create(catalog, "b", one, 1, 0, false, &e) == CATALOG_OK && e == 5);
    assert(catalog_drop_table(catalog, e) == CATALOG_OK);
    assert(catalog_drop_table(catalog, d) == CATALOG_OK);

    snapshot_t shot = snapshot(catalog);
    catalog_t *copy;
    assert(catalog_decode(shot.bytes, shot.size, &copy) == CATALOG_OK);
    catalog_table_id_t next;
    assert(create(copy, "z", one, 1, 0, false, &next) == CATALOG_OK && next == 6);
    assert(catalog_table_count(copy) == 3);
    free(shot.bytes);
    catalog_close(copy);
    catalog_close(catalog);
}

static void identity_counters_are_monotonic_and_bounded(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER, true)};
    catalog_legacy_table_t legacy = {{"old", 3, columns, 1, 0, true}, 40, INT64_MAX, 9, 10};
    catalog_table_id_t id;
    assert(catalog_import_legacy(catalog, &legacy, &id) == CATALOG_OK);
    catalog_table_view_t view = describe(catalog, id);
    assert(view.row_count == 40 && view.next_identity == (uint64_t)INT64_MAX);
    assert(view.heap_root == 9 && view.primary_root == 10);
    uint64_t value = 0;
    assert(catalog_reserve_identity(catalog, id, &value) == CATALOG_OK && value == INT64_MAX);
    snapshot_t before = snapshot(catalog);
    value = 5;
    assert(catalog_reserve_identity(catalog, id, &value) == CATALOG_LIMIT && value == 5);
    assert_same(catalog, &before);
    free(before.bytes);

    /* 명시적 import만 허용한다. 잘못된 카운터와 중복 이름은 카탈로그를 바꾸지 않는다. */
    snapshot_t shot = snapshot(catalog);
    catalog_table_id_t other = 99;
    legacy.table.name = "fresh";
    legacy.table.name_bytes = 5;
    legacy.next_identity = 0;
    assert(catalog_import_legacy(catalog, &legacy, &other) == CATALOG_INVALID && other == 99);
    legacy.next_identity = (uint64_t)INT64_MAX + 2;
    assert(catalog_import_legacy(catalog, &legacy, &other) == CATALOG_INVALID);
    legacy.next_identity = 3;
    legacy.table.name = "old";
    legacy.table.name_bytes = 3;
    assert(catalog_import_legacy(catalog, &legacy, &other) == CATALOG_EXISTS && other == 99);
    assert(catalog_import_legacy(catalog, NULL, &other) == CATALOG_INVALID);
    assert_same(catalog, &shot);
    legacy.table.name = "edge";
    legacy.table.name_bytes = 4;
    legacy.next_identity = (uint64_t)INT64_MAX + 1;
    assert(catalog_import_legacy(catalog, &legacy, &other) == CATALOG_OK);
    assert(catalog_reserve_identity(catalog, other, &value) == CATALOG_LIMIT);
    free(shot.bytes);
    catalog_close(catalog);
}

static void generation_counts_successful_changes_only(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    assert(catalog_generation(catalog) == 0);
    catalog_table_id_t id = create_users(catalog);
    assert(catalog_generation(catalog) == 1);
    assert(catalog_set_row_count(catalog, id, 1) == CATALOG_OK);
    assert(catalog_set_locators(catalog, id, 2, 3) == CATALOG_OK);
    uint64_t value;
    assert(catalog_reserve_identity(catalog, id, &value) == CATALOG_OK);
    assert(catalog_generation(catalog) == 4);
    assert(catalog_drop_table(catalog, id) == CATALOG_OK);
    assert(catalog_generation(catalog) == 5);
    assert(catalog_drop_table(catalog, id) == CATALOG_NOT_FOUND);
    assert(catalog_generation(catalog) == 5);
    catalog_close(catalog);
}

static void assert_views_equal(const catalog_table_view_t *left, const catalog_table_view_t *right)
{
    assert(left->id == right->id && left->name_bytes == right->name_bytes);
    assert(memcmp(left->name, right->name, left->name_bytes) == 0);
    assert(left->column_count == right->column_count);
    for (size_t i = 0; i < left->column_count; i++) {
        assert(left->columns[i].type == right->columns[i].type);
        assert(left->columns[i].not_null == right->columns[i].not_null);
        assert(left->columns[i].name_bytes == right->columns[i].name_bytes);
        assert(memcmp(left->columns[i].name, right->columns[i].name, left->columns[i].name_bytes) == 0);
    }
    assert(left->primary_key_column == right->primary_key_column);
    assert(left->has_identity == right->has_identity);
    assert(left->row_count == right->row_count && left->next_identity == right->next_identity);
    assert(left->heap_root == right->heap_root && left->primary_root == right->primary_root);
}

static void encode_decode_roundtrips_and_fixes_layout(void)
{
    catalog_t *empty;
    assert(catalog_open_empty(&empty) == CATALOG_OK);
    snapshot_t empty_shot = snapshot(empty);
    assert(empty_shot.size == 32 && memcmp(empty_shot.bytes, "LRCT\x01\x00\x00\x00\x20\x00\x00\x00", 12) == 0);
    catalog_t *back;
    assert(catalog_decode(empty_shot.bytes, empty_shot.size, &back) == CATALOG_OK);
    assert(catalog_table_count(back) == 0 && catalog_generation(back) == 0);
    catalog_close(back);
    free(empty_shot.bytes);
    catalog_close(empty);

    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_table_id_t users = create_users(catalog), orders = create_orders(catalog);
    catalog_column_spec_t one[] = {column("한글", VALUE_BLOB, true)};
    catalog_table_id_t unicode;
    assert(create(catalog, "표", one, 1, CATALOG_NO_COLUMN, false, &unicode) == CATALOG_OK);
    assert(catalog_set_row_count(catalog, users, UINT64_MAX) == CATALOG_OK);
    assert(catalog_set_locators(catalog, orders, UINT32_MAX, 1) == CATALOG_OK);
    uint64_t value;
    assert(catalog_reserve_identity(catalog, users, &value) == CATALOG_OK);
    snapshot_t shot = snapshot(catalog);

    assert(catalog_decode(shot.bytes, shot.size, &back) == CATALOG_OK);
    assert(catalog_table_count(back) == 3 && catalog_generation(back) == catalog_generation(catalog));
    for (size_t i = 0; i < 3; i++) {
        catalog_table_view_t left, right;
        assert(catalog_table_at(catalog, i, &left) == CATALOG_OK);
        assert(catalog_table_at(back, i, &right) == CATALOG_OK);
        assert_views_equal(&left, &right);
    }
    assert_same(back, &shot);
    /* 첫 테이블의 ID와 이름 길이가 고정 endian 위치에 있다. */
    assert(shot.bytes[HEADER] == 1 && shot.bytes[HEADER + 1] == 0 && shot.bytes[HEADER + 4] == 5);
    catalog_close(back);
    free(shot.bytes);
    catalog_close(catalog);
}

static uint32_t fnv(const uint8_t *bytes, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static void seal(uint8_t *bytes, size_t size)
{
    uint32_t sum = fnv(bytes, size - 4);
    for (unsigned i = 0; i < 4; i++) bytes[size - 4 + i] = (uint8_t)(sum >> (i * 8));
}

static void poke(uint8_t *bytes, size_t offset, unsigned width, uint64_t value)
{
    for (unsigned i = 0; i < width; i++) bytes[offset + i] = (uint8_t)(value >> (i * 8));
}

static catalog_status_t decode_marker(const uint8_t *bytes, size_t size)
{
    static char sentinel_storage;
    catalog_t *out = (catalog_t *)&sentinel_storage;
    catalog_status_t status = catalog_decode(bytes, size, &out);
    if (status == CATALOG_OK) {
        catalog_close(out);
    } else {
        assert(out == (catalog_t *)&sentinel_storage);  /* 실패하면 *out은 그대로 */
    }
    return status;
}

static void truncation_and_bit_flips_are_rejected(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    create_users(catalog);
    create_orders(catalog);
    snapshot_t shot = snapshot(catalog);
    for (size_t length = 0; length < shot.size; length++) {
        assert(decode_marker(shot.bytes, length) == CATALOG_CORRUPT);
    }
    uint8_t *copy = malloc(shot.size + 1);
    for (size_t offset = 0; offset < shot.size; offset++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            memcpy(copy, shot.bytes, shot.size);
            copy[offset] ^= (uint8_t)(1u << bit);
            catalog_status_t status = decode_marker(copy, shot.size);
            assert(status == CATALOG_CORRUPT || status == CATALOG_UNSUPPORTED);
        }
    }
    /* 끝에 붙은 바이트는 길이 필드와 맞지 않는다. */
    memcpy(copy, shot.bytes, shot.size);
    copy[shot.size] = 0;
    assert(decode_marker(copy, shot.size + 1) == CATALOG_CORRUPT);
    free(copy);
    free(shot.bytes);
    catalog_close(catalog);
}

typedef struct {
    size_t offset;
    unsigned width;
    uint64_t value;
    catalog_status_t expected;
} tamper_t;

static void run_tampers(const uint8_t *original, size_t size, const tamper_t *cases, size_t count)
{
    uint8_t *copy = malloc(size);
    for (size_t i = 0; i < count; i++) {
        memcpy(copy, original, size);
        poke(copy, cases[i].offset, cases[i].width, cases[i].value);
        seal(copy, size);  /* checksum은 맞지만 의미가 틀린 입력 */
        assert(decode_marker(copy, size) == cases[i].expected);
    }
    free(copy);
}

/* checksum을 다시 맞춘 의미 오류도 거절한다(길이·개수·ID·타입·identity·중복). */
static void semantic_corruption_is_rejected(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER, true), column("v", VALUE_TEXT, false)};
    catalog_table_id_t id;
    assert(create(catalog, "t", columns, 2, 0, true, &id) == CATALOG_OK);
    snapshot_t shot = snapshot(catalog);
    assert(shot.size == 79);
    const tamper_t cases[] = {
        {4, 2, 2, CATALOG_CORRUPT}, {4, 2, 3, CATALOG_OK}, {4, 2, 4, CATALOG_UNSUPPORTED},
        {4, 2, 0, CATALOG_UNSUPPORTED}, {6, 2, 1, CATALOG_CORRUPT},
        {0, 1, 'X', CATALOG_CORRUPT}, {8, 4, 78, CATALOG_CORRUPT}, {8, 4, 80, CATALOG_CORRUPT},
        {20, 4, 0, CATALOG_CORRUPT}, {20, 4, 1, CATALOG_CORRUPT},
        {24, 4, 0, CATALOG_CORRUPT}, {24, 4, 2, CATALOG_CORRUPT}, {24, 4, 0xffffffffu, CATALOG_CORRUPT},
        {28, 4, 0, CATALOG_CORRUPT}, {28, 4, 2, CATALOG_CORRUPT},
        {32, 2, 0, CATALOG_CORRUPT}, {32, 2, 256, CATALOG_CORRUPT}, {32, 2, 65535, CATALOG_CORRUPT},
        {34, 2, 2, CATALOG_CORRUPT}, {34, 2, CATALOG_NO_COLUMN, CATALOG_CORRUPT}, {34, 2, 1, CATALOG_CORRUPT},
        {36, 2, 0, CATALOG_CORRUPT}, {36, 2, 1025, CATALOG_CORRUPT}, {36, 2, 65535, CATALOG_CORRUPT},
        {36, 2, 3, CATALOG_CORRUPT}, {36, 2, 1, CATALOG_CORRUPT},
        {38, 1, 2, CATALOG_CORRUPT}, {38, 1, 0, CATALOG_CORRUPT},
        {47, 8, 0, CATALOG_CORRUPT}, {47, 8, (uint64_t)INT64_MAX + 2, CATALOG_CORRUPT},
        {64, 1, 0, CATALOG_CORRUPT}, {64, 1, 5, CATALOG_CORRUPT}, {64, 1, 255, CATALOG_CORRUPT},
        {65, 1, 2, CATALOG_CORRUPT}, {66, 2, 0, CATALOG_CORRUPT}, {66, 2, 255, CATALOG_CORRUPT},
        {66, 2, 65535, CATALOG_CORRUPT}, {68, 1, 0, CATALOG_CORRUPT}, {63, 1, 0, CATALOG_CORRUPT}};
    run_tampers(shot.bytes, shot.size, cases, sizeof(cases) / sizeof(cases[0]));

    free(shot.bytes);
    catalog_close(catalog);

    /* 같은 이름의 컬럼 둘 */
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_column_spec_t twin[] = {column("x", VALUE_INTEGER, false), column("y", VALUE_TEXT, false)};
    assert(create(catalog, "t", twin, 2, CATALOG_NO_COLUMN, false, &id) == CATALOG_OK);
    shot = snapshot(catalog);
    assert(shot.size == 78 && shot.bytes[68] == 'x' && shot.bytes[73] == 'y');
    const tamper_t twins[] = {{73, 1, 'x', CATALOG_CORRUPT}};
    run_tampers(shot.bytes, shot.size, twins, 1);
    free(shot.bytes);
    catalog_close(catalog);

    /* 테이블 둘: ID 순서·중복, 같은 이름 */
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_column_spec_t one[] = {column("c", VALUE_INTEGER, false)};
    assert(create(catalog, "a", one, 1, 0, false, &id) == CATALOG_OK);
    assert(create(catalog, "b", one, 1, 0, false, &id) == CATALOG_OK);
    shot = snapshot(catalog);
    assert(shot.size == 114 && shot.bytes[63] == 'a' && shot.bytes[104] == 'b');
    const tamper_t pair[] = {
        {69, 4, 1, CATALOG_CORRUPT}, {69, 4, 0, CATALOG_CORRUPT}, {69, 4, 3, CATALOG_CORRUPT},
        {28, 4, 2, CATALOG_CORRUPT}, {104, 1, 'a', CATALOG_CORRUPT}};
    run_tampers(shot.bytes, shot.size, pair, sizeof(pair) / sizeof(pair[0]));
    free(shot.bytes);
    catalog_close(catalog);
}

static void decode_argument_and_size_limits(void)
{
    catalog_t *out = NULL;
    uint8_t byte = 0;
    assert(catalog_decode(&byte, 1, NULL) == CATALOG_INVALID);
    assert(catalog_decode(NULL, 5, &out) == CATALOG_INVALID);
    assert(catalog_decode(NULL, 0, &out) == CATALOG_CORRUPT && out == NULL);
    /* 상한을 넘는 크기는 입력을 읽기 전에 거절한다. */
    assert(catalog_decode(&byte, (size_t)CATALOG_MAX_ENCODED_BYTES + 1, &out) == CATALOG_LIMIT);
    assert(out == NULL);
    uint8_t *bytes = NULL;
    size_t size = 0;
    assert(catalog_encode(NULL, &bytes, &size) == CATALOG_INVALID);
}

/* 카탈로그는 encode 상한을 넘기는 create를 LIMIT으로 거절하고 상한 크기도 왕복한다. */
static void encoded_size_cap_is_enforced(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    static catalog_column_spec_t columns[1024];
    static char names[1024][256];
    for (size_t i = 0; i < 1024; i++) {
        memset(names[i], 'x', 255);
        snprintf(names[i], 8, "c%04zu", i);
        names[i][strlen(names[i])] = 'x';
        columns[i] = (catalog_column_spec_t){names[i], 255, VALUE_TEXT, false, NULL};
    }
    size_t created = 0;
    catalog_status_t status = CATALOG_OK;
    for (; status == CATALOG_OK; created++) {
        char table[32];
        snprintf(table, sizeof(table), "table%zu", created);
        catalog_table_id_t id = 0;
        snapshot_t before = snapshot(catalog);
        status = create(catalog, table, columns, 1024, 0, false, &id);
        if (status != CATALOG_OK) {
            assert(status == CATALOG_LIMIT && id == 0);
            assert_same(catalog, &before);
        }
        assert(before.size <= CATALOG_MAX_ENCODED_BYTES);
        free(before.bytes);
    }
    assert(created > 10 && catalog_table_count(catalog) == created - 1);
    snapshot_t shot = snapshot(catalog);
    assert(shot.size <= CATALOG_MAX_ENCODED_BYTES);
    catalog_t *back;
    assert(catalog_decode(shot.bytes, shot.size, &back) == CATALOG_OK);
    assert(catalog_table_count(back) == created - 1);
    assert_same(back, &shot);
    free(shot.bytes);
    catalog_close(back);
    catalog_close(catalog);
}

/* #65: v1 테이블 파일을 읽은 뒤 보조 색인 메타데이터를 v2로 게시하고 다시 읽는다. */
static void index_metadata_survives_restart(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_table_id_t users = create_users(catalog);
    catalog_table_id_t orders = create_orders(catalog);
    snapshot_t legacy = snapshot(catalog);
    catalog_t *upgraded;
    assert(catalog_decode(legacy.bytes, legacy.size, &upgraded) == CATALOG_OK);
    free(legacy.bytes);
    catalog_close(catalog);

    assert(catalog_add_index(upgraded, users, "by_name", 7, 1, false, true) == CATALOG_OK);
    assert(catalog_add_index(upgraded, orders, "order_name", 10, 1, true, false) == CATALOG_OK);
    /* 유일 색인은 꼬리가 없어 ordered 표시를 받지 않는다 */
    assert(catalog_add_index(upgraded, orders, "both", 4, 1, true, true) == CATALOG_INVALID);
    assert(catalog_add_index(upgraded, users, "by_name", 7, 1, false, true) == CATALOG_EXISTS);
    assert(catalog_set_index_root(upgraded, "by_name", 7, 123) == CATALOG_OK);
    assert(catalog_set_index_root(upgraded, "order_name", 10, 456) == CATALOG_OK);
    snapshot_t indexed = snapshot(upgraded);
    assert(indexed.bytes[4] == 2 && catalog_encoded_bytes(upgraded) == indexed.size);
    catalog_t *reopened;
    assert(catalog_decode(indexed.bytes, indexed.size, &reopened) == CATALOG_OK);
    assert(catalog_index_count(reopened, users) == 1);
    assert(catalog_index_count(reopened, orders) == 1);
    catalog_index_view_t view;
    assert(catalog_index_at(reopened, users, 0, &view) == CATALOG_OK);
    assert(view.column == 1 && !view.unique && view.ordered && view.root == 123);
    assert(catalog_index_at(reopened, orders, 0, &view) == CATALOG_OK);
    assert(view.unique && !view.ordered && view.root == 456);
    assert_same(reopened, &indexed);
    assert(catalog_drop_table(reopened, users) == CATALOG_OK);
    assert(catalog_index_count(reopened, users) == 0);
    assert(catalog_drop_table(reopened, orders) == CATALOG_OK);
    snapshot_t empty = snapshot(reopened);
    assert(empty.bytes[4] == 1 && empty.size == 32);
    free(empty.bytes);
    free(indexed.bytes);
    catalog_close(reopened);
    catalog_close(upgraded);
}

/* 기본값은 형식 버전 3으로 저장되고 그대로 돌아오며, 잘림·비트 뒤집힘은 거절된다 */
static void defaults_round_trip(void)
{
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    value_view_t int_default = {.type = VALUE_INTEGER, .integer = -7};
    value_view_t real_default = {.type = VALUE_REAL, .real = 2.5};
    value_view_t text_default = {.type = VALUE_TEXT, .bytes = (const uint8_t *)"ready", .size_bytes = 5};
    value_view_t blob_default = {.type = VALUE_BLOB, .bytes = NULL, .size_bytes = 0};
    value_view_t wrong_type = {.type = VALUE_TEXT, .bytes = (const uint8_t *)"1", .size_bytes = 1};
    catalog_column_spec_t columns[] = {
        {"i", 1, VALUE_INTEGER, true, &int_default}, {"r", 1, VALUE_REAL, false, &real_default},
        {"t", 1, VALUE_TEXT, false, &text_default},  {"b", 1, VALUE_BLOB, false, &blob_default},
        {"n", 1, VALUE_TEXT, false, NULL},
    };
    catalog_table_spec_t spec = {"jobs", 4, columns, 5, CATALOG_NO_COLUMN, false};
    catalog_table_id_t id;
    catalog_column_spec_t bad[] = {{"x", 1, VALUE_INTEGER, false, &wrong_type}};
    catalog_table_spec_t bad_spec = {"bad", 3, bad, 1, CATALOG_NO_COLUMN, false};
    assert(catalog_create_table(catalog, &bad_spec, &id) == CATALOG_INVALID);
    assert(catalog_create_table(catalog, &spec, &id) == CATALOG_OK);
    snapshot_t shot = snapshot(catalog);
    assert(shot.bytes[4] == 3 && shot.bytes[5] == 0);
    catalog_t *decoded;
    assert(catalog_decode(shot.bytes, shot.size, &decoded) == CATALOG_OK);
    catalog_table_view_t view;
    assert(catalog_describe(decoded, id, &view) == CATALOG_OK && view.column_count == 5);
    assert(view.columns[0].has_default && view.columns[0].default_value.integer == -7 && view.columns[0].not_null);
    assert(view.columns[1].has_default && view.columns[1].default_value.real == 2.5);
    assert(view.columns[2].has_default && view.columns[2].default_value.size_bytes == 5 &&
           memcmp(view.columns[2].default_value.bytes, "ready", 5) == 0);
    assert(view.columns[3].has_default && view.columns[3].default_value.size_bytes == 0);
    assert(!view.columns[4].has_default);
    snapshot_t again = snapshot(decoded);
    assert(again.size == shot.size && memcmp(again.bytes, shot.bytes, shot.size) == 0);
    for (size_t length = 0; length < shot.size; length++) assert(decode_marker(shot.bytes, length) == CATALOG_CORRUPT);
    uint8_t *copy = malloc(shot.size);
    for (size_t offset = 0; offset < shot.size; offset++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            memcpy(copy, shot.bytes, shot.size);
            copy[offset] ^= (uint8_t)(1u << bit);
            catalog_status_t status = decode_marker(copy, shot.size);
            assert(status == CATALOG_CORRUPT || status == CATALOG_UNSUPPORTED);
        }
    }
    /* 기본값 플래그는 버전 1·2에서 쓸 수 없다 */
    memcpy(copy, shot.bytes, shot.size);
    copy[4] = 1;
    seal(copy, shot.size);
    assert(decode_marker(copy, shot.size) == CATALOG_CORRUPT);
    free(copy);
    free(again.bytes);
    free(shot.bytes);
    catalog_close(decoded);
    catalog_close(catalog);
}

int main(void)
{
    defaults_round_trip();
    tables_are_independent();
    duplicate_and_missing_keep_catalog();
    invalid_specs_keep_catalog();
    ids_are_stable_and_never_reused();
    identity_counters_are_monotonic_and_bounded();
    generation_counts_successful_changes_only();
    encode_decode_roundtrips_and_fixes_layout();
    truncation_and_bit_flips_are_rejected();
    semantic_corruption_is_rejected();
    decode_argument_and_size_limits();
    encoded_size_cap_is_enforced();
    index_metadata_survives_restart();
    puts("catalog: independent tables, stable IDs, preserved failures, strict encode/decode");
    return 0;
}
