/*
 * test_catalog_store.c -- catalog(#6)·가변 값(#81)과 page_file(#77) 연결 검사
 *
 * 합성 데이터만 쓴다. 이 검사는 "새 파일을 닫고 다시 열어 같은 카탈로그·레코드·큰 값을 읽는다"와
 * "저장 실패·손상에서 이전 세대를 보존한다"만 다룬다. WAL·거래 복구·DB 전체 재시작은 검증하지
 * 않는다. 가장 최근 제어 쓰기가 사라진 경우는 정상 미반영과 구분되지 않는다.
 *
 * 빌드: tests/test_catalog_store.c, src/storage/{catalog_store,page_file,catalog,value,record,
 *       record_page,overflow}.c
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/catalog_store.h"
#include "storage/record.h"
#include "storage/record_page.h"

#define DIR_NAME "__test__catalog_store_dir"
#define DB_PATH DIR_NAME "/db.lrdb"

static int g_run = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_run++;                                          \
        if (!(cond)) {                                    \
            g_fail++;                                     \
            printf("  FAIL (line %d): ", __LINE__);       \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ── 주입: 제어 페이지 쓰기 때 보류 해제 수를 기록하고 원하는 n번째 pwrite를 실패시킨다 ── */
static page_file_t *g_watched;
static int g_pending_at_control_write = -1;
static int g_fail_pwrite_at;
static int g_pwrite_calls;

static ssize_t hook_pread(int fd, void *buf, size_t size, off_t offset)
{
    return pread(fd, buf, size, offset);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (offset < (off_t)(PAGE_FILE_CONTROL_PAGES * PAGE_FILE_PAGE_BYTES) && g_watched != NULL) {
        g_pending_at_control_write = (int)page_file_pending_release_count(g_watched);
    }
    if (g_fail_pwrite_at != 0 && ++g_pwrite_calls == g_fail_pwrite_at) {
        errno = ENOSPC;
        return -1;
    }
    return pwrite(fd, buf, size, offset);
}

static const page_file_io_t HOOKED = {hook_pread, hook_pwrite, fsync, link, unlink};

/* ── 보조 함수 ── */
static void clean_dir(void)
{
    DIR *dir = opendir(DIR_NAME);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), DIR_NAME "/%s", entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(DIR_NAME);
}

static void fresh_dir(void)
{
    clean_dir();
    assert(mkdir(DIR_NAME, 0755) == 0);
    g_watched = NULL;
    g_pending_at_control_write = -1;
    g_fail_pwrite_at = 0;
    g_pwrite_calls = 0;
}

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes != NULL && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static void spit(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fwrite(bytes, 1, size, file) == size);
    fclose(file);
}

static catalog_column_spec_t column(const char *name, value_type_t type, bool not_null)
{
    return (catalog_column_spec_t){name, strlen(name), type, not_null};
}

static catalog_table_id_t add_table(catalog_t *catalog, const char *name, bool identity)
{
    catalog_column_spec_t columns[] = {column("id", VALUE_INTEGER, true),
                                       column("name", VALUE_TEXT, false),
                                       column("bio", VALUE_BLOB, false)};
    catalog_table_spec_t spec = {name, strlen(name), columns, 3, 0, identity};
    catalog_table_id_t id = 0;
    assert(catalog_create_table(catalog, &spec, &id) == CATALOG_OK);
    return id;
}

static void encode_bytes(const catalog_t *catalog, uint8_t **bytes, size_t *size)
{
    assert(catalog_encode(catalog, bytes, size) == CATALOG_OK);
}

static bool same_catalog(const catalog_t *a, const catalog_t *b)
{
    uint8_t *left, *right;
    size_t left_size, right_size;
    encode_bytes(a, &left, &left_size);
    encode_bytes(b, &right, &right_size);
    bool same = left_size == right_size && memcmp(left, right, left_size) == 0;
    free(left);
    free(right);
    return same;
}

/* LRO1 체인의 페이지 번호를 순서대로 모은다. 본문 8..11이 next다. 반환: 개수 */
static size_t chain_pages(page_file_t *file, uint32_t head, uint32_t table_id, uint32_t *ids, size_t cap)
{
    size_t count = 0;
    uint8_t body[PAGE_BODY_BYTES];
    while (head != 0 && count < cap) {
        assert(page_file_read(file, head, PAGE_KIND_OVERFLOW, table_id, body) == PAGE_FILE_OK);
        ids[count++] = head;
        head = (uint32_t)body[8] | (uint32_t)body[9] << 8 | (uint32_t)body[10] << 16 |
               (uint32_t)body[11] << 24;
    }
    return count;
}

static bool intersects(const uint32_t *a, size_t na, const uint32_t *b, size_t nb)
{
    for (size_t i = 0; i < na; i++) {
        for (size_t j = 0; j < nb; j++) {
            if (a[i] == b[j]) return true;
        }
    }
    return false;
}

static void flip(const char *path, uint32_t slot, size_t at)
{
    size_t size;
    uint8_t *bytes = slurp(path, &size);
    bytes[(size_t)slot * PAGE_FILE_PAGE_BYTES + at] ^= 0x40;
    spit(path, bytes, size);
    free(bytes);
}

/* ══════════════════════════════════════════════════════════════ */
/* 두 테이블의 카탈로그·레코드 페이지·큰 값이 닫고 다시 연 뒤에도 같다 (#6 + #81 + #77) */
static uint8_t g_big[30000];

static void pack_ref(overflow_ref_t ref, uint8_t out[8])
{
    for (unsigned i = 0; i < 4; i++) {
        out[i] = (uint8_t)(ref.head >> (i * 8));
        out[4 + i] = (uint8_t)(ref.size_bytes >> (i * 8));
    }
}

static overflow_ref_t unpack_ref(const uint8_t in[8])
{
    overflow_ref_t ref = {0, 0};
    for (unsigned i = 0; i < 4; i++) {
        ref.head |= (uint32_t)in[i] << (i * 8);
        ref.size_bytes |= (uint32_t)in[4 + i] << (i * 8);
    }
    return ref;
}

/* 행 하나를 레코드 페이지에 넣는다. 큰 값은 overflow 체인에 두고 8바이트 참조만 행에 담는다. */
static uint32_t store_row(page_file_t *file, uint32_t table_id, int64_t id, const char *name,
                          const uint8_t *bio, size_t bio_size)
{
    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, file, table_id);
    overflow_ref_t ref;
    assert(overflow_write(&io, bio, bio_size, &ref) == OVERFLOW_OK);
    uint8_t packed[8];
    pack_ref(ref, packed);
    record_value_t values[3] = {
        {RECORD_INTEGER, id, NULL, 0},
        {RECORD_TEXT, 0, (const uint8_t *)name, strlen(name)},
        {RECORD_BLOB, 0, packed, sizeof(packed)},
    };
    record_buffer_t encoded;
    assert(record_encode(values, 3, &encoded) == RECORD_OK);
    uint8_t page[RECORD_PAGE_BYTES];
    record_page_init(page);
    uint16_t slot;
    assert(record_page_insert(page, (record_slice_t){encoded.bytes, encoded.size_bytes}, &slot) ==
           RECORD_PAGE_OK);
    free(encoded.bytes);
    uint32_t page_id;
    assert(page_file_alloc(file, &page_id) == PAGE_FILE_OK);
    assert(page_file_write(file, page_id, PAGE_KIND_RECORD, table_id, page) == PAGE_FILE_OK);
    return page_id;
}

static void test_two_tables_survive_reopen_with_large_values(void)
{
    printf("\n[test_two_tables_survive_reopen_with_large_values]\n");
    fresh_dir();
    for (size_t i = 0; i < sizeof(g_big); i++) g_big[i] = (uint8_t)(i * 17 + 3);

    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    catalog_table_id_t users = add_table(catalog, "users", true);
    catalog_table_id_t orders = add_table(catalog, "orders", false);
    CHECK(users != orders, "서로 다른 table ID");

    uint8_t small[16];
    memset(small, 'q', sizeof(small));
    /* heap_root는 첫 레코드 페이지 하나만 가리킨다. 연결 체인·다중 페이지 CRUD는 이 범위가 아니다. */
    uint32_t users_page = store_row(file, users, 1, "alice", g_big, sizeof(g_big));
    uint32_t orders_page = store_row(file, orders, 1, "pen", small, sizeof(small));
    uint64_t next_identity;
    assert(catalog_reserve_identity(catalog, users, &next_identity) == CATALOG_OK);
    assert(catalog_set_locators(catalog, users, users_page, CATALOG_NO_PAGE) == CATALOG_OK);
    assert(catalog_set_locators(catalog, orders, orders_page, CATALOG_NO_PAGE) == CATALOG_OK);
    assert(catalog_set_row_count(catalog, users, 1) == CATALOG_OK);
    assert(catalog_set_row_count(catalog, orders, 1) == CATALOG_OK);

    CHECK(catalog_store_load(file, &(catalog_t *){NULL}) == CATALOG_STORE_EMPTY, "저장 전에는 EMPTY");
    CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_OK, "카탈로그 저장");
    CHECK(page_file_publish(file) == PAGE_FILE_OK, "게시");
    page_file_close(file);

    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    catalog_t *loaded = NULL;
    CHECK(catalog_store_load(file, &loaded) == CATALOG_STORE_OK, "다시 열어 카탈로그 읽기");
    CHECK(loaded != NULL && same_catalog(catalog, loaded), "schema·행 수·카운터·locator가 동일");
    catalog_table_view_t view;
    assert(catalog_describe(loaded, users, &view) == CATALOG_OK);
    CHECK(view.row_count == 1 && view.heap_root == users_page && view.next_identity == next_identity + 1,
          "users 상태 보존");
    CHECK(catalog_find(loaded, "orders", 6, &(catalog_table_id_t){0}) == CATALOG_OK, "이름으로 찾기");

    /* users 행과 큰 값을 읽는다 */
    uint8_t page[RECORD_PAGE_BYTES];
    CHECK(page_file_read(file, view.heap_root, PAGE_KIND_RECORD, users, page) == PAGE_FILE_OK,
          "users 레코드 페이지");
    record_slice_t slice;
    CHECK(record_page_get(page, 0, &slice) == RECORD_PAGE_OK, "slot 0");
    record_value_t values[3];
    size_t count = 0;
    record_buffer_t input = {(uint8_t *)slice.bytes, slice.size_bytes};
    CHECK(record_decode(&input, values, 3, &count) == RECORD_OK && count == 3 &&
              values[0].integer == 1 && values[1].size_bytes == 5 &&
              memcmp(values[1].bytes, "alice", 5) == 0 && values[2].size_bytes == 8,
          "행 필드 복원");
    overflow_ref_t ref = unpack_ref(values[2].bytes);
    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, file, users);
    uint8_t *big = NULL;
    CHECK(overflow_read(&io, ref, &big) == OVERFLOW_OK && ref.size_bytes == sizeof(g_big) &&
              memcmp(big, g_big, sizeof(g_big)) == 0,
          "페이지보다 큰 값이 table ID와 함께 왕복");
    free(big);

    /* 다른 테이블의 table ID로는 읽히지 않는다 */
    CHECK(page_file_read(file, users_page, PAGE_KIND_RECORD, orders, page) == PAGE_FILE_CORRUPT,
          "users 페이지를 orders로 읽으면 거절");
    page_file_overflow_ctx_t wrong;
    overflow_io_t wrong_io = page_file_overflow_io(&wrong, file, orders);
    CHECK(overflow_read(&wrong_io, ref, &big) != OVERFLOW_OK, "다른 table ID의 overflow 읽기는 거절");

    catalog_close(loaded);
    catalog_close(catalog);
    page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_save_orders_commit_before_release_and_failure_keeps_old(void)
{
    printf("\n[test_save_orders_commit_before_release_and_failure_keeps_old]\n");
    fresh_dir();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    add_table(catalog, "t1", false);
    CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_OK, "첫 저장");
    assert(page_file_publish(file) == PAGE_FILE_OK);
    page_file_close(file);

    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    g_watched = file;
    catalog_t *first_copy;
    assert(catalog_store_load(file, &first_copy) == CATALOG_STORE_OK);
    uint32_t old_head = page_file_control(file)->catalog_head;
    uint32_t old_pages[8];
    size_t old_count = chain_pages(file, old_head, 0, old_pages, 8);

    add_table(catalog, "t2", false);
    g_pending_at_control_write = -1;
    CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_OK, "두 번째 저장");
    CHECK(g_pending_at_control_write == 0, "제어 페이지를 쓰는 시점에는 옛 체인이 아직 놓이지 않음 (%d)",
          g_pending_at_control_write);
    CHECK(page_file_pending_release_count(file) == old_count, "commit 뒤에야 옛 체인이 보류 해제됨");
    CHECK(page_file_reusable_count(file) == 0, "옛 체인은 곧바로 재사용되지 않음");
    uint32_t new_pages[8];
    size_t new_count = chain_pages(file, page_file_control(file)->catalog_head, 0, new_pages, 8);
    CHECK(!intersects(old_pages, old_count, new_pages, new_count), "새 체인은 옛 체인과 겹치지 않음");

    /* 제어 쓰기 실패: 이전 세대(두 번째 저장)와 그 체인이 그대로 읽힌다 */
    add_table(catalog, "t3", false);
    catalog_t *before_fail;
    assert(catalog_store_load(file, &before_fail) == CATALOG_STORE_OK);
    uint64_t generation_before = page_file_control(file)->generation;
    size_t pending_before = page_file_pending_release_count(file);
    g_fail_pwrite_at = 0;
    g_pwrite_calls = 0;
    /* 새 체인 쓰기 1페이지 + 제어 쓰기 1회: 두 번째 pwrite가 제어 페이지다 */
    g_fail_pwrite_at = 2;
    errno = 0;
    CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_IO && errno == ENOSPC,
          "제어 쓰기 실패는 IO와 errno");
    g_fail_pwrite_at = 0;
    CHECK(page_file_control(file)->generation == generation_before, "세대 그대로");
    catalog_t *after_fail = NULL;
    CHECK(catalog_store_load(file, &after_fail) == CATALOG_STORE_OK && same_catalog(before_fail, after_fail),
          "실패한 저장 뒤에도 이전 카탈로그가 같은 핸들에서 읽힘");
    CHECK(page_file_pending_release_count(file) >= pending_before, "새 체인만 보류로 추가됨 (옛 체인은 놓지 않음)");
    catalog_close(after_fail);

    page_file_close(file);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    catalog_t *reopened;
    CHECK(catalog_store_load(file, &reopened) == CATALOG_STORE_OK && same_catalog(before_fail, reopened),
          "다시 열어도 이전 카탈로그");
    catalog_close(reopened);
    page_file_close(file);

    /* 실패 뒤 같은 카탈로그를 다시 저장하면 성공한다 */
    g_watched = NULL;
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_OK, "재시도 성공");
    page_file_close(file);

    catalog_close(first_copy);
    catalog_close(before_fail);
    catalog_close(catalog);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_generations_reuse_pages_without_exposing_previous_chain(void)
{
    printf("\n[test_generations_reuse_pages_without_exposing_previous_chain]\n");
    fresh_dir();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);

    catalog_t *saved[9] = {NULL};
    uint32_t prev_pages[64];
    size_t prev_count = 0;
    uint32_t max_page_count = 0;
    for (int round = 1; round <= 8; round++) {
        char name[16];
        snprintf(name, sizeof(name), "table_%d", round);
        /* 큰 카탈로그: 여러 페이지 체인이 되도록 이름이 긴 테이블을 늘린다 */
        for (int extra = 0; extra < 40; extra++) {
            char big_name[64];
            snprintf(big_name, sizeof(big_name), "%s_%03d_with_a_longer_name", name, extra);
            add_table(catalog, big_name, false);
        }
        CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_OK, "저장 %d", round);
        if (round == 1) assert(page_file_publish(file) == PAGE_FILE_OK);

        uint32_t pages[64];
        size_t count = chain_pages(file, page_file_control(file)->catalog_head, 0, pages, 64);
        CHECK(round > 1 || count >= 1, "체인 페이지 수 %zu", count);
        CHECK(!intersects(pages, count, prev_pages, prev_count),
              "라운드 %d: 새 체인이 직전 세대 체인(제어 슬롯이 아직 가리킴)과 겹치지 않음", round);
        memcpy(prev_pages, pages, count * sizeof(uint32_t));
        prev_count = count;
        assert(catalog_store_load(file, &saved[round]) == CATALOG_STORE_OK);
        if (page_file_control(file)->page_count > max_page_count) {
            max_page_count = page_file_control(file)->page_count;
        }

        /* 최신 제어 페이지가 손상돼도 직전 세대 카탈로그가 온전히 읽힌다 (다음 commit 전까지) */
        if (round >= 2) {
            uint32_t newest_slot = (uint32_t)(page_file_control(file)->generation % 2);
            page_file_close(file);
            size_t size;
            uint8_t *snapshot = slurp(DB_PATH, &size);
            flip(DB_PATH, newest_slot, 60);
            page_file_t *fallback;
            page_file_open_info_t info;
            assert(page_file_open(DB_PATH, false, &fallback, &info) == PAGE_FILE_OK);
            catalog_t *previous = NULL;
            CHECK(info.damaged_slots == 1 && catalog_store_load(fallback, &previous) == CATALOG_STORE_OK &&
                      same_catalog(previous, saved[round - 1]),
                  "라운드 %d: 최신 슬롯 손상 시 직전 세대 카탈로그로 열림", round);
            catalog_close(previous);
            page_file_close(fallback);
            spit(DB_PATH, snapshot, size);
            free(snapshot);
            assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
        }
    }
    CHECK(max_page_count <= 40, "해제한 체인이 재사용되어 파일이 계속 자라지 않음 (최대 %u)", max_page_count);

    for (int i = 1; i <= 8; i++) catalog_close(saved[i]);
    catalog_close(catalog);
    page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_stale_and_inconsistent_catalogs_are_rejected(void)
{
    printf("\n[test_stale_and_inconsistent_catalogs_are_rejected]\n");
    fresh_dir();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    add_table(catalog, "t1", false);
    catalog_t *old_state;
    assert(catalog_open_empty(&old_state) == CATALOG_OK);
    CHECK(catalog_store_save(file, old_state) == CATALOG_STORE_OK, "빈 카탈로그 저장 (generation 0)");
    add_table(catalog, "t2", false);
    CHECK(catalog_store_save(file, catalog) == CATALOG_STORE_OK, "저장");
    assert(page_file_publish(file) == PAGE_FILE_OK);
    uint64_t generation = page_file_control(file)->generation;
    CHECK(catalog_store_save(file, old_state) == CATALOG_STORE_STALE, "더 낮은 generation의 카탈로그는 거절");
    CHECK(page_file_control(file)->generation == generation, "거절된 저장은 commit하지 않음");

    /* 제어 페이지의 세대와 체인 내용의 세대가 다르면 조각난 상태로 보고 거절 */
    const page_file_control_t *control = page_file_control(file);
    assert(page_file_stage_catalog(file, control->catalog_head, control->catalog_size,
                                   control->catalog_generation + 5) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    catalog_t *loaded = NULL;
    CHECK(catalog_store_load(file, &loaded) == CATALOG_STORE_CORRUPT && loaded == NULL,
          "제어와 체인의 catalog generation 불일치는 CORRUPT");

    /* 체인 페이지 한 비트 손상 */
    uint32_t head = control->catalog_head;
    page_file_close(file);
    flip(DB_PATH, head, 4000);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    CHECK(catalog_store_load(file, &loaded) == CATALOG_STORE_CORRUPT, "체인 손상은 CORRUPT");
    page_file_close(file);
    catalog_close(old_state);
    catalog_close(catalog);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_lost_latest_control_is_indistinguishable(void)
{
    printf("\n[test_lost_latest_control_is_indistinguishable]\n");
    fresh_dir();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_open_empty(&catalog) == CATALOG_OK);
    add_table(catalog, "first", false);
    assert(catalog_store_save(file, catalog) == CATALOG_STORE_OK);
    assert(page_file_publish(file) == PAGE_FILE_OK);
    catalog_t *first_state;
    assert(catalog_store_load(file, &first_state) == CATALOG_STORE_OK);
    size_t size_first;
    uint8_t *file_first = slurp(DB_PATH, &size_first);
    add_table(catalog, "second", false);
    assert(catalog_store_save(file, catalog) == CATALOG_STORE_OK);
    page_file_close(file);

    /* 두 번째 저장의 제어 페이지 쓰기만 사라진 상태를 만든다: 제어 슬롯 0만 첫 저장 시점으로 되돌린다 */
    size_t size_now;
    uint8_t *now = slurp(DB_PATH, &size_now);
    memcpy(now, file_first, PAGE_FILE_PAGE_BYTES); /* 슬롯 0 */
    spit(DB_PATH, now, size_now);
    free(now);
    free(file_first);

    page_file_open_info_t info;
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK, "열림");
    catalog_t *loaded = NULL;
    CHECK(catalog_store_load(file, &loaded) == CATALOG_STORE_OK, "읽힘");
    /* 슬롯 1(세대 1, 첫 저장)과 슬롯 0 되돌림(내용 없음)이 남는다. 첫 저장 상태로 정상처럼 열린다. */
    CHECK(info.damaged_slots == 0 && same_catalog(loaded, first_state),
          "한계: 최신 제어 쓰기가 사라져도 손상 표시 없이 한 세대 전 카탈로그로 열림");
    catalog_close(loaded);
    catalog_close(first_state);
    catalog_close(catalog);
    page_file_close(file);
    clean_dir();
}

int main(void)
{
    page_file_set_default_io(&HOOKED);
    test_two_tables_survive_reopen_with_large_values();
    test_save_orders_commit_before_release_and_failure_keeps_old();
    test_generations_reuse_pages_without_exposing_previous_chain();
    test_stale_and_inconsistent_catalogs_are_rejected();
    test_lost_latest_control_is_indistinguishable();
    page_file_set_default_io(NULL);
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
