/*
 * test_legacy_migrate.c -- 옛 MINIDB v1/v2 파일의 명시적 export/import 검사 (#77)
 *
 * 옛 파일은 이 파일이 합성 바이트로 직접 만든 fixture이며 실제 사용 DB를 읽지 않는다.
 * 검증하는 것: 원본이 읽기 전용으로만 쓰이고 hash가 바뀌지 않는다, 어떤 실패 뒤에도 목적지나
 * partial이 성공처럼 남지 않는다, 기존 목적지를 덮어쓰지 않는다, 옛 파일이 새 열기 경로로
 * 자동 해석되지 않는다. 연결 체인·다중 페이지 heap은 #50 범위라 한 페이지를 넘는 행은 거절하는 것만
 * 확인한다. DB 전체 재시작·거래 복구는 검증하지 않는다.
 *
 * 빌드: tests/test_legacy_migrate.c, src/storage/{legacy_migrate,catalog_store,page_file,catalog,
 *       value,record,record_page,overflow}.c
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
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

#define DIR_NAME "__test__legacy_dir"
#define SRC DIR_NAME "/legacy.db"
#define DST DIR_NAME "/new.lrdb"
#define PARTIAL DST ".partial"

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

/* ── 실패 주입 ── */
typedef struct {
    int pread_fail_at, pread_calls;
    int pwrite_fail_at, pwrite_calls;
    int fsync_fail_at, fsync_calls;
    bool link_fail;
    bool link_rival; /* link 직전에 다른 쪽이 목적지를 만든다 */
    bool unlink_fail;
} hooks_t;

static hooks_t g_hooks;

static void hooks_reset(void) { memset(&g_hooks, 0, sizeof(g_hooks)); }

static void spit(const char *path, const uint8_t *bytes, size_t size);

static ssize_t hook_pread(int fd, void *buf, size_t size, off_t offset)
{
    if (size != 8 && g_hooks.pread_fail_at != 0 && ++g_hooks.pread_calls == g_hooks.pread_fail_at) {
        errno = EIO;
        return -1;
    }
    if (size != 8) g_hooks.pread_calls += g_hooks.pread_fail_at == 0 ? 1 : 0;
    return pread(fd, buf, size, offset);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    g_hooks.pwrite_calls += g_hooks.pwrite_fail_at == 0 ? 1 : 0;
    if (g_hooks.pwrite_fail_at != 0 && ++g_hooks.pwrite_calls == g_hooks.pwrite_fail_at) {
        errno = ENOSPC;
        return -1;
    }
    return pwrite(fd, buf, size, offset);
}

static int hook_fsync(int fd)
{
    g_hooks.fsync_calls += g_hooks.fsync_fail_at == 0 ? 1 : 0;
    if (g_hooks.fsync_fail_at != 0 && ++g_hooks.fsync_calls == g_hooks.fsync_fail_at) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int hook_link(const char *from, const char *to)
{
    if (g_hooks.link_rival) spit(to, (const uint8_t *)"rival", 5);
    if (g_hooks.link_fail) {
        errno = EIO;
        return -1;
    }
    return link(from, to);
}

static int hook_unlink(const char *path)
{
    if (g_hooks.unlink_fail) {
        errno = EIO;
        return -1;
    }
    return unlink(path);
}

static const page_file_io_t HOOKED = {hook_pread, hook_pwrite, hook_fsync, hook_link, hook_unlink};

/* ── 보조 함수 ── */
static uint64_t fnv64(const uint8_t *bytes, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1);
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

static uint64_t file_hash(const char *path)
{
    size_t size;
    uint8_t *bytes = slurp(path, &size);
    if (bytes == NULL) return 0;
    uint64_t hash = fnv64(bytes, size);
    free(bytes);
    return hash;
}

static bool exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int next_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    close(fd);
    return fd;
}

static int inventory(void)
{
    int count = 0;
    DIR *dir = opendir(DIR_NAME);
    struct dirent *entry;
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
    }
    if (dir != NULL) closedir(dir);
    return count;
}

static void clean_dir(void)
{
    DIR *dir = opendir(DIR_NAME);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), DIR_NAME "/%s", entry->d_name);
            chmod(path, 0644);
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
    hooks_reset();
}

/* ── 합성 옛 파일 ── */
typedef struct {
    int64_t id;
    const char *name;
    int32_t age;
    bool deleted;
} row_t;

typedef struct {
    uint32_t version;
    uint32_t page_size;
    const char *table_name;     /* v2 */
    uint64_t next_id;
    uint64_t header_row_count;  /* UINT64_MAX면 살아 있는 행 수 */
    size_t rows_per_page;       /* 0이면 한 페이지에 전부 */
} legacy_opts_t;

#define LEGACY_ROW_SIZE 28u

static void put_row(uint8_t *at, const row_t *row)
{
    memset(at, 0, LEGACY_ROW_SIZE);
    memcpy(at, &row->id, 8);
    memcpy(at + 8, row->name, strlen(row->name) < 16 ? strlen(row->name) : 16);
    memcpy(at + 24, &row->age, 4);
}

static uint8_t *build_legacy(const legacy_opts_t *opts, const row_t *rows, size_t count, size_t *size_out)
{
    uint32_t ps = opts->page_size;
    size_t per_page = opts->rows_per_page != 0 ? opts->rows_per_page : (count > 0 ? count : 1);
    size_t heap_pages = (count + per_page - 1) / per_page;
    if (heap_pages == 0) heap_pages = 1;
    uint32_t total_pages = 3 + (uint32_t)(heap_pages - 1); /* 0 헤더, 1 첫 힙, 2 리프, 3.. 추가 힙 */
    uint8_t *file = calloc(total_pages, ps);
    assert(file != NULL);

    db_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, DB_MAGIC, 8);
    header.version = opts->version;
    header.page_size = ps;
    header.root_index_page_id = 2;
    header.first_heap_page_id = 1;
    header.next_page_id = total_pages;
    uint64_t live = 0;
    for (size_t i = 0; i < count; i++) live += !rows[i].deleted;
    header.next_id = opts->next_id;
    header.row_count = opts->header_row_count == UINT64_MAX ? live : opts->header_row_count;
    header.column_count = 3;
    header.row_size = LEGACY_ROW_SIZE;
    memcpy(header.columns[0].name, "id", 2);
    header.columns[0].type = COL_TYPE_BIGINT;
    header.columns[0].size = 8;
    header.columns[0].offset = 0;
    header.columns[0].is_system = 1;
    memcpy(header.columns[1].name, "name", 4);
    header.columns[1].type = COL_TYPE_VARCHAR;
    header.columns[1].size = 16;
    header.columns[1].offset = 8;
    memcpy(header.columns[2].name, "age", 3);
    header.columns[2].type = COL_TYPE_INT;
    header.columns[2].size = 4;
    header.columns[2].offset = 24;
    if (opts->version == 2 && opts->table_name != NULL) {
        memcpy(header.table_name, opts->table_name, strlen(opts->table_name));
    }
    memcpy(file, &header, sizeof(header));

    leaf_page_header_t leaf = {.page_type = PAGE_TYPE_LEAF};
    memcpy(file + (size_t)2 * ps, &leaf, sizeof(leaf));

    for (size_t page = 0; page < heap_pages; page++) {
        uint32_t pid = page == 0 ? 1 : 2 + (uint32_t)page;
        uint8_t *at = file + (size_t)pid * ps;
        size_t first = page * per_page;
        size_t n = count - first < per_page ? count - first : per_page;
        heap_page_header_t hph = {
            .page_type = PAGE_TYPE_HEAP,
            .next_heap_page_id = page + 1 < heap_pages ? 3 + (uint32_t)page : 0,
            .slot_count = (uint16_t)n,
            .free_slot_head = SLOT_NONE,
            .free_space_offset = (uint16_t)(n * LEGACY_ROW_SIZE),
        };
        memcpy(at, &hph, sizeof(hph));
        for (size_t i = 0; i < n; i++) {
            slot_t slot = {.offset = (uint16_t)(ps - (i + 1) * LEGACY_ROW_SIZE),
                           .status = rows[first + i].deleted ? SLOT_FREE : SLOT_ALIVE,
                           .next_free = SLOT_NONE};
            memcpy(at + sizeof(hph) + i * sizeof(slot), &slot, sizeof(slot));
            put_row(at + slot.offset, &rows[first + i]);
        }
    }
    *size_out = (size_t)total_pages * ps;
    return file;
}

static const row_t ROWS[] = {
    {1, "alice", 31, false}, {2, "gone", 99, true}, {3, "한글 이름", 7, false}, {4, "", 0, false},
};
#define ROW_COUNT (sizeof(ROWS) / sizeof(ROWS[0]))

static void write_legacy(const legacy_opts_t *opts, const row_t *rows, size_t count)
{
    size_t size;
    uint8_t *bytes = build_legacy(opts, rows, count, &size);
    spit(SRC, bytes, size);
    free(bytes);
}

static legacy_opts_t v2_opts(uint32_t page_size)
{
    return (legacy_opts_t){2, page_size, "people", 10, UINT64_MAX, 0};
}

/* 새 파일에서 첫 테이블의 행을 읽어 기대와 대조한다 */
static bool new_file_has_rows(const char *path, const row_t *rows, size_t count, uint64_t next_identity)
{
    page_file_t *file;
    if (page_file_open(path, false, &file, NULL) != PAGE_FILE_OK) return false;
    catalog_t *catalog = NULL;
    bool ok = catalog_store_load(file, &catalog) == CATALOG_STORE_OK && catalog_table_count(catalog) == 1;
    catalog_table_view_t view;
    ok = ok && catalog_table_at(catalog, 0, &view) == CATALOG_OK;
    ok = ok && view.name_bytes == 6 && memcmp(view.name, "people", 6) == 0 && view.column_count == 3 &&
         view.columns[0].type == VALUE_INTEGER && view.columns[1].type == VALUE_TEXT &&
         view.columns[2].type == VALUE_INTEGER && view.has_identity && view.primary_key_column == 0 &&
         view.next_identity == next_identity && view.primary_root == CATALOG_NO_PAGE;
    uint8_t page[RECORD_PAGE_BYTES];
    ok = ok && page_file_read(file, view.heap_root, PAGE_KIND_RECORD, view.id, page) == PAGE_FILE_OK;
    uint16_t slot = 0;
    uint64_t live = 0;
    for (size_t i = 0; ok && i < count; i++) {
        if (rows[i].deleted) continue;
        live++;
        record_slice_t slice;
        record_value_t values[3];
        size_t found = 0;
        record_buffer_t input;
        ok = record_page_get(page, slot++, &slice) == RECORD_PAGE_OK;
        if (!ok) break;
        input = (record_buffer_t){(uint8_t *)slice.bytes, slice.size_bytes};
        ok = record_decode(&input, values, 3, &found) == RECORD_OK && found == 3 &&
             values[0].integer == rows[i].id && values[1].size_bytes == strlen(rows[i].name) &&
             memcmp(values[1].bytes, rows[i].name, values[1].size_bytes) == 0 &&
             values[2].integer == rows[i].age;
    }
    ok = ok && view.row_count == live;
    ok = ok && record_page_get(page, slot, &(record_slice_t){0}) == RECORD_PAGE_NOT_FOUND;
    if (catalog != NULL) catalog_close(catalog);
    page_file_close(file);
    return ok;
}

/* ══════════════════════════════════════════════════════════════ */
static void test_migrate_is_read_only_and_page_size_independent(void)
{
    printf("\n[test_migrate_is_read_only_and_page_size_independent]\n");
    uint64_t hashes[2] = {0, 0};
    uint32_t sizes[2] = {4096, 16384};
    for (int i = 0; i < 2; i++) {
        fresh_dir();
        legacy_opts_t opts = v2_opts(sizes[i]);
        write_legacy(&opts, ROWS, ROW_COUNT);
        chmod(SRC, 0444); /* 쓰기 권한이 없어도 변환되어야 한다: 원본을 읽기 전용으로만 연다 */
        uint64_t before = file_hash(SRC);
        int fd_before = next_fd();
        CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_OK, "page_size=%u 변환 성공", sizes[i]);
        CHECK(file_hash(SRC) == before, "원본 hash 변경 없음 (page_size=%u)", sizes[i]);
        CHECK(inventory() == 2 && exists(DST) && !exists(PARTIAL), "원본과 새 파일만 존재");
        CHECK(next_fd() == fd_before, "fd 누수 없음");
        CHECK(new_file_has_rows(DST, ROWS, ROW_COUNT, 10), "행·schema·identity가 새 파일에 보존");
        hashes[i] = file_hash(DST);

        legacy_status_t again = legacy_migrate(SRC, DST, NULL);
        CHECK(again == LEGACY_EXISTS, "같은 목적지에 다시 변환하면 거절");
        CHECK(file_hash(DST) == hashes[i] && inventory() == 2, "기존 목적지 그대로, partial 없음");
    }
    CHECK(hashes[0] != 0 && hashes[0] == hashes[1],
          "OS 페이지 크기(4096/16384)가 다른 원본에서 같은 논리 내용이면 새 파일 바이트가 같음");
    clean_dir();
}

static void test_v1_needs_explicit_name_and_v2_name_must_match(void)
{
    printf("\n[test_v1_needs_explicit_name_and_v2_name_must_match]\n");
    fresh_dir();
    legacy_opts_t v1 = {1, 4096, NULL, 10, UINT64_MAX, 0};
    write_legacy(&v1, ROWS, ROW_COUNT);
    uint64_t before = file_hash(SRC);
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_UNSUPPORTED, "v1은 이름 없이는 변환하지 않음");
    CHECK(!exists(DST) && inventory() == 1, "목적지·partial 없음");
    CHECK(legacy_migrate(SRC, DST, "people") == LEGACY_OK, "이름을 명시하면 변환");
    CHECK(file_hash(SRC) == before && new_file_has_rows(DST, ROWS, ROW_COUNT, 10), "원본 보존, 행 보존");

    fresh_dir();
    legacy_opts_t v2 = v2_opts(4096);
    write_legacy(&v2, ROWS, ROW_COUNT);
    CHECK(legacy_migrate(SRC, DST, "other") == LEGACY_INVALID, "v2 이름과 다른 인자는 거절");
    CHECK(legacy_migrate(SRC, DST, "people") == LEGACY_OK, "같은 이름이면 허용");
    clean_dir();
}

static void test_heap_chain_rows_are_converted_in_order(void)
{
    printf("\n[test_heap_chain_rows_are_converted_in_order]\n");
    fresh_dir();
    row_t rows[6];
    for (int i = 0; i < 6; i++) rows[i] = (row_t){i + 1, "r", i * 10, false};
    legacy_opts_t opts = v2_opts(4096);
    opts.rows_per_page = 2;
    write_legacy(&opts, rows, 6);
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_OK, "힙 페이지 3장 변환");
    CHECK(new_file_has_rows(DST, rows, 6, 10), "여러 힙 페이지의 행이 순서대로 보존");
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
typedef void (*corrupt_t)(uint8_t *bytes, size_t *size);

static void c_bad_magic(uint8_t *b, size_t *s) { (void)s; b[0] = 'X'; }
static void c_version(uint8_t *b, size_t *s) { (void)s; b[8] = 9; }
static void c_page_size(uint8_t *b, size_t *s) { (void)s; b[12] = 0xE8; b[13] = 3; b[14] = 0; b[15] = 0; }
static void c_truncated(uint8_t *b, size_t *s) { (void)b; *s -= 4096; }
static void c_short_header(uint8_t *b, size_t *s) { (void)b; *s = 100; }
static void c_row_count(uint8_t *b, size_t *s) { (void)s; b[40] = 99; }
static void c_next_id_small(uint8_t *b, size_t *s) { (void)s; b[32] = 3; } /* id 3,4가 next_id 이상 */
static void c_invalid_utf8(uint8_t *b, size_t *s) { (void)s; b[4096 + 4096 - 28 + 8] = 0xC3; b[4096 + 4096 - 28 + 9] = 0x28; }
static void c_duplicate_id(uint8_t *b, size_t *s) { (void)s; b[4096 + 4096 - 3 * 28] = 1; } /* 세 번째 슬롯 행의 id를 1로 */
static void c_slot_offset(uint8_t *b, size_t *s)
{
    (void)s;
    slot_t slot;
    memcpy(&slot, b + 4096 + sizeof(heap_page_header_t), sizeof(slot));
    slot.offset = 4090; /* 행이 페이지 밖으로 나감 */
    memcpy(b + 4096 + sizeof(heap_page_header_t), &slot, sizeof(slot));
}
static void c_slot_status(uint8_t *b, size_t *s)
{
    (void)s;
    b[4096 + sizeof(heap_page_header_t) + 2] = 0x77;
}
static void c_heap_type(uint8_t *b, size_t *s) { (void)s; b[4096] = 0x09; }
static void c_cycle(uint8_t *b, size_t *s)
{
    (void)s;
    b[4096 + 4] = 1; /* 첫 힙 페이지의 next가 자기 자신 */
}
static void c_slot_count(uint8_t *b, size_t *s)
{
    (void)s;
    b[4096 + 8] = 0xFF;
    b[4096 + 9] = 0x7F; /* 슬롯 디렉터리가 페이지를 넘음 */
}
static void c_column_type(uint8_t *b, size_t *s)
{
    (void)s;
    b[sizeof(db_header_t) - 1] = 0; /* 사용 안 함 */
    b[52 + 38 + 32] = 9; /* 두 번째 컬럼 type */
}
static void c_column_overlap(uint8_t *b, size_t *s)
{
    (void)s;
    b[52 + 38 * 2 + 35] = 4; /* 세 번째 컬럼 offset을 4로: id와 겹침 */
}
static void c_first_heap(uint8_t *b, size_t *s) { (void)s; b[20] = 0; }
static void c_empty_name(uint8_t *b, size_t *s)
{
    (void)s;
    memset(b + offsetof(db_header_t, table_name), 0, 32);
}

static void test_corrupt_sources_are_rejected_without_output(void)
{
    printf("\n[test_corrupt_sources_are_rejected_without_output]\n");
    struct {
        const char *name;
        corrupt_t corrupt;
        legacy_status_t expect;
    } cases[] = {
        {"magic", c_bad_magic, LEGACY_UNSUPPORTED},
        {"version", c_version, LEGACY_UNSUPPORTED},
        {"page_size", c_page_size, LEGACY_UNSUPPORTED},
        {"잘린 파일", c_truncated, LEGACY_CORRUPT},
        {"잘린 header", c_short_header, LEGACY_CORRUPT},
        {"header 행 수 불일치", c_row_count, LEGACY_CORRUPT},
        {"next_id 이상의 id", c_next_id_small, LEGACY_CORRUPT},
        {"잘못된 UTF-8", c_invalid_utf8, LEGACY_UNCONVERTIBLE},
        {"중복 id", c_duplicate_id, LEGACY_UNCONVERTIBLE},
        {"슬롯 offset 범위 밖", c_slot_offset, LEGACY_CORRUPT},
        {"슬롯 상태", c_slot_status, LEGACY_CORRUPT},
        {"힙 페이지 종류", c_heap_type, LEGACY_CORRUPT},
        {"힙 체인 순환", c_cycle, LEGACY_CORRUPT},
        {"슬롯 수 과다", c_slot_count, LEGACY_CORRUPT},
        {"컬럼 종류", c_column_type, LEGACY_UNSUPPORTED},
        {"컬럼 범위 겹침", c_column_overlap, LEGACY_CORRUPT},
        {"첫 힙 페이지 번호", c_first_heap, LEGACY_CORRUPT},
        {"v2 빈 테이블 이름", c_empty_name, LEGACY_CORRUPT},
    };
    legacy_opts_t opts = v2_opts(4096);
    size_t good_size;
    uint8_t *good = build_legacy(&opts, ROWS, ROW_COUNT, &good_size);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh_dir();
        uint8_t *bytes = malloc(good_size);
        memcpy(bytes, good, good_size);
        size_t size = good_size;
        cases[i].corrupt(bytes, &size);
        spit(SRC, bytes, size);
        free(bytes);
        uint64_t before = file_hash(SRC);
        int fd_before = next_fd();
        legacy_status_t status = legacy_migrate(SRC, DST, NULL);
        CHECK(status == cases[i].expect, "%s: 상태 %d (기대 %d)", cases[i].name, status, cases[i].expect);
        CHECK(file_hash(SRC) == before, "%s: 원본 hash 변경 없음", cases[i].name);
        CHECK(!exists(DST) && !exists(PARTIAL) && inventory() == 1, "%s: 목적지·partial 없음", cases[i].name);
        CHECK(next_fd() == fd_before, "%s: fd 누수 없음", cases[i].name);
    }
    free(good);
    clean_dir();
}

static void test_too_many_rows_are_refused_not_truncated(void)
{
    printf("\n[test_too_many_rows_are_refused_not_truncated]\n");
    fresh_dir();
    enum { MANY = 1500 };
    row_t *rows = malloc(MANY * sizeof(row_t));
    for (int i = 0; i < MANY; i++) rows[i] = (row_t){i + 1, "row", i, false};
    legacy_opts_t opts = v2_opts(16384);
    opts.next_id = MANY + 1;
    opts.rows_per_page = 400;
    write_legacy(&opts, rows, MANY);
    free(rows);
    uint64_t before = file_hash(SRC);
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_LIMIT, "레코드 페이지 한 장을 넘으면 일부만 옮기지 않고 거절");
    CHECK(file_hash(SRC) == before && !exists(DST) && inventory() == 1, "원본 보존, 새 파일 없음");
    clean_dir();
}

static void test_source_locking_and_no_auto_interpretation(void)
{
    printf("\n[test_source_locking_and_no_auto_interpretation]\n");
    fresh_dir();
    legacy_opts_t opts = v2_opts(4096);
    write_legacy(&opts, ROWS, ROW_COUNT);
    int holder = open(SRC, O_RDWR);
    assert(holder >= 0 && flock(holder, LOCK_EX | LOCK_NB) == 0); /* 옛 pager가 쓰는 중이라고 본다 */
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_LOCKED, "쓰기 중인 옛 파일은 변환하지 않음");
    CHECK(!exists(DST) && inventory() == 1, "새 파일·partial 없음");
    close(holder);
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_OK, "잠금이 풀리면 변환");

    /* 새 열기 경로는 옛 파일을 해석하지 않는다. 반대 방향도 마찬가지다. */
    uint64_t legacy_before = file_hash(SRC), new_before = file_hash(DST);
    page_file_t *file = NULL;
    CHECK(page_file_open(SRC, true, &file, NULL) == PAGE_FILE_UNSUPPORTED && file == NULL,
          "page_file_open은 옛 파일을 열지 않음");
    legacy_export_t *data = NULL;
    CHECK(legacy_export(DST, NULL, &data) == LEGACY_UNSUPPORTED && data == NULL,
          "legacy_export는 새 형식 파일을 옛 파일로 읽지 않음");
    CHECK(file_hash(SRC) == legacy_before && file_hash(DST) == new_before, "두 파일 모두 변경 없음");

    /* 읽기 공유: 동시에 두 번 export해도 서로 막지 않는다 */
    legacy_export_t *a = NULL, *b = NULL;
    int reader = open(SRC, O_RDONLY);
    assert(reader >= 0 && flock(reader, LOCK_SH | LOCK_NB) == 0);
    CHECK(legacy_export(SRC, NULL, &a) == LEGACY_OK && legacy_export(SRC, NULL, &b) == LEGACY_OK,
          "읽기 잠금은 공유");
    CHECK(legacy_export_row_count(a) == 3 && legacy_export_next_id(a) == 10 &&
              strcmp(legacy_export_table_name(a), "people") == 0,
          "export 값의 행 수·next_id·이름");
    legacy_export_free(a);
    legacy_export_free(b);
    close(reader);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void expect_clean_failure(const char *label, legacy_status_t status, legacy_status_t expect,
                                 uint64_t src_hash, int fd_base)
{
    CHECK(status == expect, "%s: 상태 %d (기대 %d)", label, status, expect);
    CHECK(file_hash(SRC) == src_hash, "%s: 원본 hash 변경 없음", label);
    CHECK(!exists(DST) && !exists(PARTIAL) && inventory() == 1, "%s: 새 DB·partial이 남지 않음", label);
    CHECK(next_fd() == fd_base, "%s: fd 누수 없음", label);
    page_file_t *probe = NULL;
    page_file_status_t probed = page_file_open(DST, false, &probe, NULL);
    CHECK(probed != PAGE_FILE_OK, "%s: 미완성 새 DB가 성공으로 열리지 않음", label);
    if (probed == PAGE_FILE_OK) page_file_close(probe);
}

static void test_import_failures_preserve_source_and_never_publish(void)
{
    printf("\n[test_import_failures_preserve_source_and_never_publish]\n");
    fresh_dir();
    legacy_opts_t opts = v2_opts(4096);
    write_legacy(&opts, ROWS, ROW_COUNT);
    uint64_t src_hash = file_hash(SRC);
    int fd_base = next_fd();

    /* 기준 실행: 호출 횟수를 센다 */
    hooks_reset();
    page_file_set_default_io(&HOOKED);
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_OK, "기준 변환");
    int writes = g_hooks.pwrite_calls, syncs = g_hooks.fsync_calls, reads = g_hooks.pread_calls;
    CHECK(writes > 0 && syncs > 0 && reads > 0, "호출 수 확인: pwrite %d fsync %d pread %d", writes, syncs, reads);
    unlink(DST);

    for (int at = 1; at <= writes; at++) {
        char label[64];
        snprintf(label, sizeof(label), "pwrite %d번째 실패", at);
        hooks_reset();
        g_hooks.pwrite_fail_at = at;
        expect_clean_failure(label, legacy_migrate(SRC, DST, NULL), LEGACY_IO, src_hash, fd_base);
    }
    for (int at = 1; at <= syncs; at++) {
        char label[64];
        snprintf(label, sizeof(label), "fsync %d번째 실패", at);
        hooks_reset();
        g_hooks.fsync_fail_at = at;
        if (at < syncs) {
            expect_clean_failure(label, legacy_migrate(SRC, DST, NULL), LEGACY_IO, src_hash, fd_base);
        } else {
            /* 마지막 fsync는 게시 뒤 디렉터리 fsync다. 이미 목적지가 보이므로 DIRTY로 알려야 한다. */
            CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_PUBLISHED_DIRTY, "%s: 이름 fsync 실패는 PUBLISHED_DIRTY", label);
            CHECK(exists(DST) && !exists(PARTIAL) && new_file_has_rows(DST, ROWS, ROW_COUNT, 10),
                  "%s: 목적지는 완전하고 partial은 정리됨", label);
            CHECK(file_hash(SRC) == src_hash, "%s: 원본 보존", label);
            unlink(DST);
        }
    }
    for (int at = 1; at <= reads; at++) {
        char label[64];
        snprintf(label, sizeof(label), "원본 pread %d번째 실패", at);
        hooks_reset();
        g_hooks.pread_fail_at = at;
        expect_clean_failure(label, legacy_migrate(SRC, DST, NULL), LEGACY_IO, src_hash, fd_base);
    }
    hooks_reset();
    g_hooks.link_fail = true;
    expect_clean_failure("link 실패", legacy_migrate(SRC, DST, NULL), LEGACY_IO, src_hash, fd_base);

    /* 게시 직전에 다른 쪽이 목적지를 만들면 그 내용을 덮어쓰지 않는다 */
    hooks_reset();
    g_hooks.link_rival = true;
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_EXISTS, "경합한 목적지는 EXISTS");
    size_t rival_size;
    uint8_t *rival = slurp(DST, &rival_size);
    CHECK(rival != NULL && rival_size == 5 && memcmp(rival, "rival", 5) == 0, "경합한 목적지 내용 보존");
    free(rival);
    CHECK(!exists(PARTIAL) && inventory() == 2 && file_hash(SRC) == src_hash, "partial 정리, 원본 보존");
    unlink(DST);

    /* 게시 뒤 partial 정리 실패: 새 파일은 완전하며 PUBLISHED_DIRTY로 알린다 */
    hooks_reset();
    g_hooks.unlink_fail = true;
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_PUBLISHED_DIRTY, "정리 실패는 PUBLISHED_DIRTY");
    CHECK(exists(DST) && exists(PARTIAL), "목적지와 남은 partial을 인벤토리로 확인");
    CHECK(new_file_has_rows(DST, ROWS, ROW_COUNT, 10), "목적지는 완전한 새 파일");
    CHECK(file_hash(SRC) == src_hash, "원본 보존");
    hooks_reset();
    unlink(DST);
    unlink(PARTIAL);

    /* 목적지가 이미 있으면 처음부터 시작하지 않는다 */
    spit(DST, (const uint8_t *)"mine", 4);
    uint64_t mine = file_hash(DST);
    CHECK(legacy_migrate(SRC, DST, NULL) == LEGACY_EXISTS, "기존 목적지 거절");
    CHECK(file_hash(DST) == mine && !exists(PARTIAL) && inventory() == 2, "기존 목적지·원본만 남음");
    CHECK(next_fd() == fd_base, "fd 누수 없음");

    page_file_set_default_io(NULL);
    clean_dir();
}

int main(void)
{
    test_migrate_is_read_only_and_page_size_independent();
    test_v1_needs_explicit_name_and_v2_name_must_match();
    test_heap_chain_rows_are_converted_in_order();
    test_corrupt_sources_are_rejected_without_output();
    test_too_many_rows_are_refused_not_truncated();
    test_source_locking_and_no_auto_interpretation();
    test_import_failures_preserve_source_and_never_publish();
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
