/*
 * test_page_file.c -- 새 저장 형식 page_file 검사 (#77)
 *
 * 합성 데이터만 쓴다. 이 검사는 독립 파일 API의 왕복·거절·게시·잠금만 다루며 거래 복구,
 * DB 전체 재시작은 검증하지 않는다. 성공 반환 뒤 저장 장치가 버린 쓰기(lost)는 탐지할 수 없고
 * 가장 최근 제어 페이지의 손실이 "게시되지 않음"과 구분되지 않는 한계를 그대로 확인한다.
 *
 * 빌드: tests/test_page_file.c, src/storage/page_file.c, overflow.c, record_page.c, record.c
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "storage/checksum.h"
#include "storage/overflow.h"
#include "storage/page_file.h"
#include "storage/record_page.h"

#define DIR_NAME "__test__page_file_dir"
#define DB_PATH DIR_NAME "/db.totedb"
#define PARTIAL_PATH DB_PATH ".partial"

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

/* ── I/O 주입: 번호를 센다. 모든 읽기·쓰기가 8192 정렬·크기인지도 확인한다 ── */
typedef struct {
    int pwrite_fail_at;   /* n번째 pwrite를 실패시킨다 (1부터, 0이면 끔) */
    int pwrite_calls;
    int fsync_fail_at;
    int fsync_calls;
    bool link_fail;
    int link_errno;
    bool unlink_fail;
    int misaligned;
    int io_calls;
    int pread_calls;      /* 페이지 단위 pread 수(open의 8바이트 탐색 제외) */
    size_t short_max;     /* 0이 아니면 pread·pwrite 한 번에 이 바이트까지만 옮긴다(짧은 I/O) */
    bool eintr_odd;       /* 홀수 번째 pread·pwrite는 아무것도 옮기지 않고 EINTR로 끝난다 */
    bool eintr_always;    /* pwrite가 매번 EINTR로 끝난다 */
    int pread_zero_at;    /* n번째 페이지 pread가 0바이트(EOF)를 돌려준다 */
    int pwrite_zero_at;   /* n번째 pwrite가 0바이트를 돌려준다 */
    int raw_calls;        /* eintr_odd·zero 판정용 호출 수 */
} hooks_t;

static hooks_t g_hooks;

static void hooks_reset(void) { memset(&g_hooks, 0, sizeof(g_hooks)); }

static void check_aligned(size_t size, off_t offset)
{
    g_hooks.io_calls++;
    if (size != PAGE_FILE_PAGE_BYTES || offset % PAGE_FILE_PAGE_BYTES != 0) g_hooks.misaligned++;
}

/* 짧은 I/O·EINTR 주입. 남은 바이트를 이어 옮기는 호출은 정렬 검사에서 뺀다. */
static bool inject_eintr(void)
{
    if (!g_hooks.eintr_odd || ++g_hooks.raw_calls % 2 == 0) return false;
    errno = EINTR;
    return true;
}

static ssize_t hook_pread(int fd, void *buf, size_t size, off_t offset)
{
    /* open이 첫 8바이트를 보는 탐색(8바이트)은 예외다. */
    if (size != 8) {
        if (g_hooks.short_max == 0) check_aligned(size, offset);
        g_hooks.pread_calls++;
        if (g_hooks.pread_zero_at != 0 && g_hooks.pread_calls == g_hooks.pread_zero_at) return 0;
        if (inject_eintr()) return -1;
        if (g_hooks.short_max != 0 && size > g_hooks.short_max) size = g_hooks.short_max;
    }
    return pread(fd, buf, size, offset);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (g_hooks.short_max == 0) check_aligned(size, offset);
    if (g_hooks.pwrite_fail_at != 0 && ++g_hooks.pwrite_calls == g_hooks.pwrite_fail_at) {
        errno = ENOSPC;
        return -1;
    }
    if (g_hooks.pwrite_zero_at != 0 && ++g_hooks.pwrite_calls == g_hooks.pwrite_zero_at) return 0;
    if (g_hooks.eintr_always) {
        g_hooks.raw_calls++;
        errno = EINTR;
        return -1;
    }
    if (inject_eintr()) return -1;
    if (g_hooks.short_max != 0 && size > g_hooks.short_max) size = g_hooks.short_max;
    return pwrite(fd, buf, size, offset);
}

static int hook_fsync(int fd)
{
    if (g_hooks.fsync_fail_at != 0 && ++g_hooks.fsync_calls == g_hooks.fsync_fail_at) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int hook_link(const char *from, const char *to)
{
    if (g_hooks.link_fail) {
        errno = g_hooks.link_errno;
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
    assert(bytes != NULL);
    assert(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static void spit(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(bytes, 1, size, file) == size);
    fclose(file);
}

/* 파일 내용 hash. 없으면 0. */
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
}

/* 디렉터리 안 파일 이름 수 */
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

/* 구현과 같은 사양(CRC32C, 28..31 제외)으로 다시 계산한다. */
static uint32_t test_checksum(const uint8_t *page)
{
    return crc32c(crc32c(0, page, 28), page + 32, PAGE_FILE_PAGE_BYTES - 32);
}

static void reseal(uint8_t *page)
{
    uint32_t sum = test_checksum(page);
    for (unsigned i = 0; i < 4; i++) page[28 + i] = (uint8_t)(sum >> (i * 8));
}

/*
 * 고정 합성 파일: 레코드 페이지 1장(table 7), overflow 3페이지 체인(table 7), commit 한 번.
 * 반환: 게시된 파일 경로 DB_PATH. 호출 전 fresh_dir 필요.
 */
/* 세대가 큰 제어 슬롯 */
static uint32_t page_file_control_generation_slot(const uint8_t *bytes)
{
    uint64_t gen[2] = {0, 0};
    for (uint32_t slot = 0; slot < 2; slot++) {
        for (int i = 7; i >= 0; i--) gen[slot] = (gen[slot] << 8) | bytes[(size_t)slot * PAGE_FILE_PAGE_BYTES + 32 + 20 + i];
    }
    return gen[1] > gen[0] ? 1 : 0;
}

/* 빈 commit을 한 번 더 해 최신 세대의 manifest가 이전 commit의 페이지를 대조하지 않게 한다 */
static void empty_commit(void)
{
    page_file_t *file;
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_close(file) == PAGE_FILE_OK);
}

static uint8_t g_big[20000];
static overflow_ref_t g_big_ref;
static uint32_t g_record_page;

static void fill_big(void)
{
    for (size_t i = 0; i < sizeof(g_big); i++) g_big[i] = (uint8_t)(i * 31 + 7);
}

static void build_fixture(void)
{
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);

    uint8_t record[RECORD_PAGE_BYTES];
    record_page_init(record);
    uint16_t slot;
    assert(record_page_insert(record, (record_slice_t){(const uint8_t *)"alpha", 5}, &slot) ==
           RECORD_PAGE_OK);
    assert(page_file_alloc(file, &g_record_page) == PAGE_FILE_OK);
    assert(page_file_write(file, g_record_page, PAGE_KIND_RECORD, 7, record) == PAGE_FILE_OK);

    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, file, 7);
    assert(overflow_write(&io, g_big, sizeof(g_big), &g_big_ref) == OVERFLOW_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_publish(file) == PAGE_FILE_OK);
    assert(page_file_close(file) == PAGE_FILE_OK);
}

/* ══════════════════════════════════════════════════════════════ */
static void test_fixture_roundtrip_is_page_size_independent(void)
{
    printf("\n[test_fixture_roundtrip_is_page_size_independent]\n");
    fresh_dir();
    hooks_reset();
    build_fixture();
    CHECK(g_hooks.misaligned == 0 && g_hooks.io_calls > 0,
          "모든 I/O가 8192 크기·정렬이다 (어긋난 호출 %d / %d)", g_hooks.misaligned, g_hooks.io_calls);

    size_t size;
    uint8_t *bytes = slurp(DB_PATH, &size);
    CHECK(bytes != NULL && size % PAGE_FILE_PAGE_BYTES == 0, "파일 길이가 8192의 배수 (%zu)", size);
    /* 제어 페이지 envelope: 고정 magic·버전·종류·page ID·길이와 본문 magic */
    CHECK(memcmp(bytes + PAGE_FILE_PAGE_BYTES, "LRPG", 4) == 0, "envelope magic");
    CHECK(bytes[PAGE_FILE_PAGE_BYTES + 4] == 2 && bytes[PAGE_FILE_PAGE_BYTES + 5] == 0, "envelope 버전 2(CRC32C) LE");
    CHECK(crc32c(0, "123456789", 9) == 0xe3069283u, "CRC32C 표준 검사값");
    CHECK(bytes[PAGE_FILE_PAGE_BYTES + 6] == PAGE_KIND_CONTROL, "종류 제어");
    CHECK(bytes[PAGE_FILE_PAGE_BYTES + 8] == 1, "제어 slot 1의 page ID");
    uint32_t manifest = bytes[PAGE_FILE_PAGE_BYTES + 32 + 68];
    CHECK(manifest > 0 && bytes[PAGE_FILE_PAGE_BYTES + 16] + 256u * bytes[PAGE_FILE_PAGE_BYTES + 17] == 72 + 8 * manifest,
          "빈 목록이 없는 제어 본문은 머리 72바이트와 manifest %u개", manifest);
    CHECK(bytes[PAGE_FILE_PAGE_BYTES + 40] == PAGE_FILE_FORMAT_VERSION, "형식 버전 3");
    CHECK(memcmp(bytes + PAGE_FILE_PAGE_BYTES + 32, "TOTEDB\0\0", 8) == 0, "파일 magic");
    CHECK(bytes[PAGE_FILE_PAGE_BYTES + 48] == 4 && bytes[PAGE_FILE_PAGE_BYTES + 49] == 3 &&
              bytes[PAGE_FILE_PAGE_BYTES + 50] == 2 && bytes[PAGE_FILE_PAGE_BYTES + 51] == 1,
          "byte order 표식은 little-endian 0x01020304");
    uint8_t *page0 = bytes;
    bool blank = true;
    for (size_t i = 0; i < PAGE_FILE_PAGE_BYTES; i++) blank = blank && page0[i] == 0;
    CHECK(blank, "첫 commit 뒤 제어 slot 0은 아직 쓰이지 않음");
    free(bytes);

    page_file_t *file;
    page_file_open_info_t info;
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK, "다시 열기");
    CHECK(info.damaged_slots == 0 && info.blank_slots == 1, "손상 없음, 빈 슬롯 1");
    const page_file_control_t *control = page_file_control(file);
    CHECK(control->generation == 1, "세대 1");
    uint8_t record[RECORD_PAGE_BYTES];
    CHECK(page_file_read(file, g_record_page, PAGE_KIND_RECORD, 7, record) == PAGE_FILE_OK,
          "레코드 페이지 읽기");
    record_slice_t value;
    CHECK(record_page_get(record, 0, &value) == RECORD_PAGE_OK && value.size_bytes == 5 &&
              memcmp(value.bytes, "alpha", 5) == 0,
          "LRP2 본문이 envelope 안에서 왕복");
    uint8_t wrong[PAGE_BODY_BYTES];
    CHECK(page_file_read(file, g_record_page, PAGE_KIND_RECORD, 8, wrong) == PAGE_FILE_CORRUPT,
          "다른 table ID는 거절");
    CHECK(page_file_read(file, g_record_page, PAGE_KIND_OVERFLOW, 7, wrong) == PAGE_FILE_CORRUPT,
          "다른 종류는 거절");
    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, file, 7);
    uint8_t *big = NULL;
    CHECK(overflow_read(&io, g_big_ref, &big) == OVERFLOW_OK && memcmp(big, g_big, sizeof(g_big)) == 0,
          "여러 페이지 overflow 값이 왕복");
    free(big);
    CHECK(page_file_close(file) == PAGE_FILE_OK, "닫기");
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
typedef void (*mutate_t)(uint8_t *bytes, size_t *size);

static void mut_control_version(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 32 + 8] = 9;
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_page_bytes(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 32 + 12 + 1] = 0x10; /* 4096+... 의미 없는 크기 */
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_byte_order(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 32 + 16] = 1;
    b[PAGE_FILE_PAGE_BYTES + 32 + 19] = 4;
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_envelope_version(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 4] = 3;
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_file_magic(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 32] = 'X';
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_checksum(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 40] ^= 1; /* 본문 한 비트, 체크섬은 그대로 */
}
static void mut_body_length(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 16] = 200;
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_wrong_page_id(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 8] = 0;
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_trailing_garbage(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 5000] = 1; /* 본문 길이 뒤 패딩 */
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_truncate_pages(uint8_t *b, size_t *s)
{
    (void)b;
    *s -= PAGE_FILE_PAGE_BYTES; /* 약속한 page_count보다 짧음 */
}
static void mut_truncate_header(uint8_t *b, size_t *s)
{
    (void)b;
    *s = 100;
}
static void mut_empty(uint8_t *b, size_t *s)
{
    (void)b;
    *s = 0;
}
/* 다른 형식의 파일: 두 제어 페이지 어디에도 ToteDB magic이 없다 */
static void mut_foreign_file(uint8_t *b, size_t *s)
{
    for (size_t i = 0; i < *s; i++) b[i] = (uint8_t)(i * 13u + 5u);
    memcpy(b, "SQLite format 3", 16);
}
static void mut_short_text_file(uint8_t *b, size_t *s)
{
    static const char text[] = "hello, this is not a database\n";
    memcpy(b, text, sizeof(text) - 1);
    *s = sizeof(text) - 1;
}
static void mut_both_controls_bad(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 40] ^= 1;
    b[100] = 0x5a; /* slot 0은 비어 있지 않은 쓰레기 */
}
static void mut_page_count_too_small(uint8_t *b, size_t *s)
{
    (void)s;
    b[PAGE_FILE_PAGE_BYTES + 32 + 28] = 1;
    reseal(b + PAGE_FILE_PAGE_BYTES);
}
static void mut_old_lrp1_only(uint8_t *b, size_t *s)
{
    (void)s;
    memset(b, 0, 2 * PAGE_FILE_PAGE_BYTES); /* 제어 페이지 없이 옛 LRP1 페이지가 앞에 있는 파일 */
    memcpy(b, "LRP1", 4);
}

static void test_invalid_files_are_rejected_without_change(void)
{
    printf("\n[test_invalid_files_are_rejected_without_change]\n");
    struct {
        const char *name;
        mutate_t mutate;
        page_file_status_t expect;
    } cases[] = {
        {"제어 버전", mut_control_version, PAGE_FILE_UNSUPPORTED},
        {"물리 페이지 크기", mut_page_bytes, PAGE_FILE_UNSUPPORTED},
        {"byte order", mut_byte_order, PAGE_FILE_UNSUPPORTED},
        {"envelope 버전", mut_envelope_version, PAGE_FILE_UNSUPPORTED},
        {"파일 magic", mut_file_magic, PAGE_FILE_UNSUPPORTED},
        {"체크섬", mut_checksum, PAGE_FILE_CORRUPT},
        {"본문 길이", mut_body_length, PAGE_FILE_CORRUPT},
        {"page ID", mut_wrong_page_id, PAGE_FILE_CORRUPT},
        {"본문 뒤 패딩", mut_trailing_garbage, PAGE_FILE_CORRUPT},
        {"page_count 초과 길이", mut_truncate_pages, PAGE_FILE_CORRUPT},
        {"잘린 header", mut_truncate_header, PAGE_FILE_CORRUPT},
        {"빈 파일", mut_empty, PAGE_FILE_NO_CONTROL},
        {"다른 형식 파일", mut_foreign_file, PAGE_FILE_UNSUPPORTED},
        {"짧은 텍스트 파일", mut_short_text_file, PAGE_FILE_UNSUPPORTED},
        {"제어 둘 다 손상", mut_both_controls_bad, PAGE_FILE_CORRUPT},
        {"page_count가 너무 작음", mut_page_count_too_small, PAGE_FILE_CORRUPT},
        {"옛 LRP1 페이지만", mut_old_lrp1_only, PAGE_FILE_UNSUPPORTED},
    };
    fresh_dir();
    hooks_reset();
    build_fixture();
    size_t original_size;
    uint8_t *original = slurp(DB_PATH, &original_size);
    assert(original != NULL);
    int files_before = inventory();  /* DB 파일과 게시 때 생긴 writer 잠금 파일 */

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t *bytes = malloc(original_size);
        assert(bytes != NULL);
        memcpy(bytes, original, original_size);
        size_t size = original_size;
        cases[i].mutate(bytes, &size);
        spit(DB_PATH, bytes, size);
        free(bytes);
        uint64_t before = file_hash(DB_PATH);
        int fd_before = next_fd();

        for (int writable = 0; writable < 2; writable++) {
            page_file_t *file = NULL;
            page_file_status_t status = page_file_open(DB_PATH, writable != 0, &file, NULL);
            CHECK(status == cases[i].expect && file == NULL,
                  "%s: 거절 (상태 %d, 기대 %d, writable=%d)", cases[i].name, status,
                  cases[i].expect, writable);
        }
        CHECK(file_hash(DB_PATH) == before, "%s: 원본 hash 변경 없음", cases[i].name);
        CHECK(next_fd() == fd_before, "%s: fd 누수 없음", cases[i].name);
        CHECK(inventory() == files_before, "%s: 디렉터리에 다른 파일이 생기지 않음", cases[i].name);
    }
    free(original);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void flip_slot(const char *path, uint32_t slot, size_t at)
{
    size_t size;
    uint8_t *bytes = slurp(path, &size);
    bytes[(size_t)slot * PAGE_FILE_PAGE_BYTES + at] ^= 0x40;
    spit(path, bytes, size);
    free(bytes);
}

static void test_control_generations_and_fallback_limits(void)
{
    printf("\n[test_control_generations_and_fallback_limits]\n");
    fresh_dir();
    hooks_reset();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK); /* gen 1 -> slot 1 */
    assert(page_file_publish(file) == PAGE_FILE_OK);
    size_t size_g1;
    uint8_t *after_g1 = slurp(DB_PATH, &size_g1);
    assert(page_file_commit(file) == PAGE_FILE_OK); /* gen 2 -> slot 0 */
    size_t size_g2;
    uint8_t *after_g2 = slurp(DB_PATH, &size_g2);
    assert(page_file_commit(file) == PAGE_FILE_OK); /* gen 3 -> slot 1 */
    CHECK(page_file_control(file)->generation == 3, "세 번 commit 뒤 세대 3");
    assert(page_file_close(file) == PAGE_FILE_OK);

    page_file_open_info_t info;
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK &&
              page_file_control(file)->generation == 3 && info.damaged_slots == 0,
          "정상: 세대 3, 손상 없음");
    page_file_close(file);

    /* 최신 제어 페이지 손상: 한 세대 전으로 열리고 damaged_slots로 알린다 */
    flip_slot(DB_PATH, 1, 60);
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK &&
              page_file_control(file)->generation == 2 && info.damaged_slots == 1,
          "최신 슬롯 손상: 세대 2로 열리고 damaged_slots=1");
    page_file_close(file);

    /*
     * 가장 최근 제어 쓰기가 통째로 사라진 경우(옛 페이지가 그대로 남음)는 정상 상태와 같아 보인다.
     * 슬롯 1에 세대 1 내용을 되돌리면 세대 2(슬롯 0)와 1(슬롯 1)로 열리며 손상 표시가 없다.
     */
    spit(DB_PATH, after_g2, size_g2);
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK &&
              page_file_control(file)->generation == 2 && info.damaged_slots == 0,
          "한계: 최신 쓰기가 사라져도 세대 2로 정상 열림 (손상 표시 없음)");
    page_file_close(file);

    /* 세대 1 파일의 슬롯 1을 세대 3처럼 고쳐 쓴다. 반대 슬롯이 비어 있으면 손상으로 표시한다. */
    {
        uint8_t *mixed = malloc(size_g1);
        memcpy(mixed, after_g1, size_g1);
        /* 슬롯 0에 세대 3처럼 보이는 페이지를 직접 만든다: 슬롯 1의 세대 1 본문을 세대 3으로 바꿔 슬롯 1에 */
        uint8_t *slot1 = mixed + PAGE_FILE_PAGE_BYTES;
        slot1[32 + 20] = 3; /* 세대 3, 슬롯 1 */
        reseal(slot1);
        /* 슬롯 0은 비어 있음: 세대 3인데 반대편이 비면 손상으로 취급되어 damaged_slots=1로 열린다 */
        spit(DB_PATH, mixed, size_g1);
        free(mixed);
        CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK &&
                  info.damaged_slots == 1 && page_file_control(file)->generation == 3,
              "세대 2 이상인데 반대 슬롯이 0이면 손상으로 표시");
        page_file_close(file);
    }
    free(after_g1);
    free(after_g2);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_commit_failures_keep_current_generation(void)
{
    printf("\n[test_commit_failures_keep_current_generation]\n");
    fresh_dir();
    hooks_reset();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint32_t first;
    assert(page_file_alloc(file, &first) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES];
    memset(body, 0x11, sizeof(body));
    assert(page_file_write(file, first, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_publish(file) == PAGE_FILE_OK);
    uint64_t good_hash_control = file_hash(DB_PATH);
    (void)good_hash_control;

    /* 해제 후 commit이 제어 쓰기에서 실패 */
    CHECK(page_file_release(file, first) == PAGE_FILE_OK, "해제 보류");
    CHECK(page_file_pending_release_count(file) == 1 && page_file_reusable_count(file) == 0,
          "commit 전에는 재사용 불가");
    CHECK(page_file_last_commit_outcome(file) == PAGE_FILE_COMMIT_APPLIED, "직전 commit은 적용됨");
    g_hooks.pwrite_fail_at = 1;
    g_hooks.pwrite_calls = 0;
    errno = 0;
    CHECK(page_file_commit(file) == PAGE_FILE_IO && errno == ENOSPC, "제어 쓰기 실패는 IO와 errno");
    CHECK(page_file_control(file)->generation == 1, "이 핸들이 아는 세대는 그대로");
    CHECK(page_file_last_commit_outcome(file) == PAGE_FILE_COMMIT_UNKNOWN,
          "제어 쓰기를 시도한 뒤의 실패는 미반영이라고 단정할 수 없음");
    CHECK(page_file_is_poisoned(file), "제어 쓰기 실패는 핸들을 poison");
    CHECK(page_file_pending_release_count(file) == 1 && page_file_reusable_count(file) == 0,
          "실패한 commit은 보류 해제를 재사용 목록으로 옮기지 않음");
    uint32_t again;
    CHECK(page_file_alloc(file, &again) == PAGE_FILE_IO && page_file_write(file, first, PAGE_KIND_OVERFLOW, 0, body) ==
              PAGE_FILE_IO, "poison된 핸들은 어느 세대가 가리킬지 모르는 페이지를 받거나 쓰지 않음");
    hooks_reset();
    errno = 0;
    CHECK(page_file_commit(file) == PAGE_FILE_IO && errno == EIO, "poison된 핸들은 이후 commit도 거절");
    CHECK(page_file_last_commit_outcome(file) == PAGE_FILE_COMMIT_UNKNOWN,
          "거절한 commit이 poison을 일으킨 결과를 덮어쓰지 않음");
    page_file_close(file);

    /* 새 핸들로 해제 -> commit -> 재사용 */
    CHECK(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK, "다시 열기");
    CHECK(page_file_control(file)->generation == 1, "쓰이지 않은 제어 쓰기는 보이지 않음");
    assert(page_file_release(file, first) == PAGE_FILE_OK);
    CHECK(page_file_commit(file) == PAGE_FILE_OK, "다시 commit 성공");
    CHECK(page_file_pending_release_count(file) == 0 && page_file_aging_release_count(file) == 1 &&
              page_file_reusable_count(file) == 0,
          "commit 한 번 뒤에는 fallback 세대가 아직 가리킬 수 있어 재사용 불가");
    uint32_t held;
    assert(page_file_alloc(file, &held) == PAGE_FILE_OK);
    CHECK(held != first, "commit 한 번 뒤에도 해제한 페이지를 돌려주지 않음");
    CHECK(page_file_commit(file) == PAGE_FILE_OK, "두 번째 commit 성공");
    CHECK(page_file_aging_release_count(file) == 0 && page_file_reusable_count(file) == 1,
          "commit이 두 번 성공한 뒤에야 재사용 가능");
    uint32_t reused;
    CHECK(page_file_alloc(file, &reused) == PAGE_FILE_OK && reused == first, "해제한 페이지를 재사용");
    CHECK(page_file_release(file, 9999) == PAGE_FILE_INVALID, "범위 밖 해제 거절");
    CHECK(page_file_release(file, 0) == PAGE_FILE_INVALID, "제어 페이지 해제 거절");
    CHECK(page_file_release(file, reused) == PAGE_FILE_OK &&
              page_file_release(file, reused) == PAGE_FILE_INVALID, "이중 해제 거절");
    page_file_close(file);

    /* manifest가 있는 commit은 fsync가 제어 쓰기 뒤 한 번뿐이라 그 실패는 불확정이다 */
    CHECK(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK, "다시 열기");
    CHECK(page_file_control(file)->generation == 3, "마지막으로 성공한 세대 3");
    hooks_reset();
    g_hooks.fsync_fail_at = 1;
    CHECK(page_file_commit(file) == PAGE_FILE_IO, "fsync 실패");
    CHECK(page_file_last_commit_outcome(file) == PAGE_FILE_COMMIT_UNKNOWN && page_file_is_poisoned(file) &&
              page_file_control(file)->generation == 3,
          "불확정 결과, 핸들 poison, 핸들이 아는 세대는 그대로");
    hooks_reset();
    errno = 0;
    CHECK(page_file_commit(file) == PAGE_FILE_IO && errno == EIO, "fsync가 실패한 핸들은 이후 commit도 거절");
    page_file_close(file);
    CHECK(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK, "불확정 뒤에도 열림");
    CHECK(page_file_control(file)->generation == 4,
          "실패를 돌려준 commit의 새 세대가 다시 열면 보임(불확정 결과)");
    page_file_close(file);

    /* 게시된 페이지를 제자리에서 덮어쓴 commit은 장벽(데이터 fsync → 제어 쓰기 → fsync)이라 데이터 fsync 실패는 미반영 */
    CHECK(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK, "다시 열기");
    uint32_t in_place;
    assert(page_file_alloc(file, &in_place) == PAGE_FILE_OK);
    assert(page_file_write(file, in_place, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_write(file, in_place, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    hooks_reset();
    g_hooks.fsync_fail_at = 1;
    CHECK(page_file_commit(file) == PAGE_FILE_IO, "데이터 fsync 실패");
    CHECK(page_file_last_commit_outcome(file) == PAGE_FILE_COMMIT_NOT_APPLIED && page_file_is_poisoned(file),
          "제어 쓰기 전의 실패는 미반영이고 핸들은 poison");
    hooks_reset();
    page_file_close(file);
    CHECK(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK, "다른 핸들로는 다시 열림");
    CHECK(page_file_control(file)->generation == 5, "미반영: 디스크도 세대 5");
    page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_publish_never_clobbers_destination(void)
{
    printf("\n[test_publish_never_clobbers_destination]\n");
    int fd_base = next_fd();
    page_file_t *file, *other;

    /* a) 목적지가 이미 있으면 만들지 않는다 */
    fresh_dir();
    hooks_reset();
    spit(DB_PATH, (const uint8_t *)"existing", 8);
    uint64_t existing = file_hash(DB_PATH);
    CHECK(page_file_create(DB_PATH, &file) == PAGE_FILE_EXISTS, "목적지가 있으면 create 거절");
    CHECK(file_hash(DB_PATH) == existing && inventory() == 1, "기존 목적지·partial 없음");

    /* b) create 뒤 다른 쪽이 먼저 목적지를 만들면 publish가 덮어쓰지 않는다 */
    unlink(DB_PATH);
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    CHECK(exists(PARTIAL_PATH) && !exists(DB_PATH), "partial만 존재");
    CHECK(page_file_publish(file) == PAGE_FILE_INVALID, "commit 전 publish 거절");
    assert(page_file_commit(file) == PAGE_FILE_OK);
    spit(DB_PATH, (const uint8_t *)"rival", 5);
    uint64_t rival = file_hash(DB_PATH);
    CHECK(page_file_publish(file) == PAGE_FILE_EXISTS, "경합한 목적지는 EXISTS");
    CHECK(file_hash(DB_PATH) == rival, "경합한 목적지 내용이 그대로");
    CHECK(!page_file_is_published(file) && exists(PARTIAL_PATH), "핸들은 게시되지 않았고 partial 유지");
    CHECK(page_file_close(file) == PAGE_FILE_OK, "닫기");
    CHECK(!exists(PARTIAL_PATH) && inventory() == 1, "닫으면 partial이 지워짐");

    /* c) partial이 남아 있으면 새로 만들지 않는다 (진행 중이거나 남은 흔적을 덮지 않음) */
    unlink(DB_PATH);
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    CHECK(page_file_create(DB_PATH, &other) == PAGE_FILE_EXISTS, "같은 목적지의 두 번째 create는 거절");
    CHECK(exists(PARTIAL_PATH), "첫 partial은 영향 없음");
    page_file_close(file);
    CHECK(inventory() == 0, "정리 뒤 디렉터리가 비어 있음");

    /* d) link 실패: 목적지를 만들지 않고 닫으면 partial도 사라진다 */
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    g_hooks.link_fail = true;
    g_hooks.link_errno = EIO;
    CHECK(page_file_publish(file) == PAGE_FILE_IO && !exists(DB_PATH), "link 실패는 IO, 목적지 없음");
    g_hooks.link_errno = ENOTSUP;
    CHECK(page_file_publish(file) == PAGE_FILE_UNSUPPORTED, "hard link 미지원은 UNSUPPORTED (덮어쓰는 대체 경로 없음)");
    g_hooks.link_fail = false;
    page_file_close(file);
    CHECK(inventory() == 0, "실패한 게시 뒤 partial 없음");

    /* e) 게시 뒤 partial 삭제가 실패하면 PUBLISHED_DIRTY, 목적지는 정상으로 열린다 */
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    g_hooks.unlink_fail = true;
    CHECK(page_file_publish(file) == PAGE_FILE_PUBLISHED_DIRTY, "정리 실패는 PUBLISHED_DIRTY");
    g_hooks.unlink_fail = false;
    CHECK(page_file_is_published(file) && exists(DB_PATH) && exists(PARTIAL_PATH),
          "목적지는 보이고 partial이 남음 (인벤토리로 확인)");
    page_file_close(file);
    CHECK(page_file_open(DB_PATH, false, &other, NULL) == PAGE_FILE_OK, "남은 partial이 있어도 목적지는 열림");
    page_file_close(other);
    unlink(PARTIAL_PATH);
    CHECK(next_fd() == fd_base, "모든 경로 뒤 fd 누수 없음");
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_locking_and_unpublished_files(void)
{
    printf("\n[test_locking_and_unpublished_files]\n");
    fresh_dir();
    hooks_reset();
    int fd_base = next_fd();
    page_file_t *a, *b;
    assert(page_file_create(DB_PATH, &a) == PAGE_FILE_OK);
    CHECK(page_file_open(PARTIAL_PATH, false, &b, NULL) == PAGE_FILE_NO_CONTROL, "작성 중인 partial은 게시 전이라 열리지 않음");
    page_file_close(a);

    /* 게시하지 않고 닫으면 partial이 지워진다 */
    assert(page_file_create(DB_PATH, &a) == PAGE_FILE_OK);
    CHECK(page_file_close(a) == PAGE_FILE_OK && !exists(PARTIAL_PATH), "게시하지 않고 닫으면 partial 제거");

    /* 첫 commit 전의 partial 내용은 열기를 통과하지 못한다 */
    assert(page_file_create(DB_PATH, &a) == PAGE_FILE_OK);
    size_t size;
    uint8_t *raw = slurp(PARTIAL_PATH, &size);
    spit(DB_PATH, raw, size); /* 미완성 새 DB를 목적지 이름으로 복사해 본다 */
    free(raw);
    page_file_close(a);
    CHECK(page_file_open(DB_PATH, false, &b, NULL) == PAGE_FILE_NO_CONTROL,
          "제어 페이지가 비어 있는 미완성 파일은 열리지 않음");
    unlink(DB_PATH);

    /* 게시된 파일: writer는 하나, reader는 writer와 함께 열린다(#200) */
    fresh_dir();
    build_fixture();
    CHECK(page_file_open(DB_PATH, true, &a, NULL) == PAGE_FILE_OK, "쓰기 핸들");
    CHECK(page_file_open(DB_PATH, true, &b, NULL) == PAGE_FILE_LOCKED, "두 번째 쓰기 핸들 거절");
    CHECK(exists(DB_PATH "-lock"), "writer 잠금 파일");
    page_file_t *r1 = NULL, *r2 = NULL;
    CHECK(page_file_open(DB_PATH, false, &r1, NULL) == PAGE_FILE_OK &&
              page_file_open(DB_PATH, false, &r2, NULL) == PAGE_FILE_OK, "쓰기 핸들이 열려 있어도 읽기 핸들은 공유");
    CHECK(page_file_write(r1, 2, PAGE_KIND_RECORD, 7, (uint8_t[PAGE_BODY_BYTES]){0}) == PAGE_FILE_INVALID,
          "읽기 핸들은 쓰지 못함");
    /* reader가 읽는 동안(공유 잠금)은 게시하지 않는다. 그 세대의 페이지가 읽는 도중 다시 쓰이지 않게 하기 위해서다 */
    uint64_t before = page_file_control(a)->generation;
    page_file_set_default_publish_timeout(0);
    page_file_t *quick = NULL;
    page_file_close(a);
    assert(page_file_open(DB_PATH, true, &quick, NULL) == PAGE_FILE_OK);
    page_file_set_default_publish_timeout(PAGE_FILE_PUBLISH_TIMEOUT_MS);
    CHECK(page_file_commit(quick) == PAGE_FILE_LOCKED &&
              page_file_last_commit_outcome(quick) == PAGE_FILE_COMMIT_NOT_APPLIED,
          "reader가 읽는 동안 게시는 기다린다(대기 상한 0이면 LOCKED, 미적용)");
    page_file_close(r1);
    page_file_close(r2);
    CHECK(page_file_commit(quick) == PAGE_FILE_OK && page_file_control(quick)->generation == before + 1,
          "reader가 끝나면 게시한다");
    page_file_close(quick);

    /* 다른 프로세스가 열고 SIGKILL로 죽으면 잠금이 풀린다 (pid 숫자는 보지 않는다) */
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        page_file_t *held;
        if (page_file_open(DB_PATH, true, &held, NULL) != PAGE_FILE_OK) _exit(2);
        char ok = 1;
        if (write(ready[1], &ok, 1) != 1) _exit(3);
        for (;;) pause();
    }
    char ok = 0;
    assert(read(ready[0], &ok, 1) == 1 && ok == 1);
    CHECK(page_file_open(DB_PATH, true, &a, NULL) == PAGE_FILE_LOCKED, "다른 프로세스가 쥐고 있으면 거절");
    kill(child, SIGKILL);
    int wait_status;
    waitpid(child, &wait_status, 0);
    CHECK(page_file_open(DB_PATH, true, &a, NULL) == PAGE_FILE_OK, "SIGKILL 뒤에는 열림");

    /* 읽던 reader 프로세스가 SIGKILL로 죽으면 게시가 다시 진행된다(#200) */
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        page_file_t *reader;
        if (page_file_open(DB_PATH, false, &reader, NULL) != PAGE_FILE_OK) _exit(2);
        char ok = 1;
        if (write(ready[1], &ok, 1) != 1) _exit(3);
        for (;;) pause();
    }
    assert(read(ready[0], &ok, 1) == 1 && ok == 1);
    page_file_close(a);
    page_file_set_default_publish_timeout(0);
    page_file_status_t reopened = page_file_open(DB_PATH, true, &a, NULL);
    page_file_set_default_publish_timeout(PAGE_FILE_PUBLISH_TIMEOUT_MS);
    /* 자식은 부모가 쓰기로 연 핸들을 물려받았지만 fork 직후 그 fd 사본을 닫았으므로 부모가 닫은 writer 잠금이 남지 않는다 */
    CHECK(reopened == PAGE_FILE_OK, "자식이 물려받은 fd가 부모의 writer 잠금을 붙잡지 않음 (%d)", (int)reopened);
    assert(reopened == PAGE_FILE_OK);
    CHECK(page_file_commit(a) == PAGE_FILE_LOCKED, "reader가 읽는 동안 게시 대기");
    kill(child, SIGKILL);
    waitpid(child, &wait_status, 0);
    CHECK(page_file_commit(a) == PAGE_FILE_OK, "reader가 죽은 뒤 게시");
    page_file_close(a);
    close(ready[0]);
    close(ready[1]);
    CHECK(next_fd() == fd_base, "fd 누수 없음");
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_corrupt_data_pages_are_detected(void)
{
    printf("\n[test_corrupt_data_pages_are_detected]\n");
    fresh_dir();
    hooks_reset();
    build_fixture();
    page_file_t *file;
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    /* 최신 세대의 manifest가 이 페이지를 대조하지 않게 빈 commit을 한 번 더 한다(열기가 아니라 읽기에서 탐지) */
    assert(page_file_commit(file) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES];

    flip_slot(DB_PATH, g_record_page, 3000); /* 열린 뒤 디스크에서 한 비트 변경 */
    CHECK(page_file_read(file, g_record_page, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_CORRUPT &&
              errno == EBADMSG,
          "한 비트 손상은 체크섬이 잡음 (EBADMSG)");
    uint8_t keep[PAGE_BODY_BYTES];
    memset(keep, 0xab, sizeof(keep));
    memcpy(body, keep, sizeof(body));
    page_file_read(file, g_record_page, PAGE_KIND_RECORD, 7, body);
    CHECK(memcmp(body, keep, sizeof(body)) == 0, "실패한 읽기는 출력을 바꾸지 않음");

    /* 부분 쓰기: 페이지 앞 절반만 새 내용 */
    memset(body, 0x22, sizeof(body));
    assert(page_file_write(file, g_record_page, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    page_file_close(file);
    size_t size;
    uint8_t *bytes = slurp(DB_PATH, &size);
    size_t at = (size_t)g_record_page * PAGE_FILE_PAGE_BYTES;
    memset(bytes + at + 4096, 0x33, 4096); /* 뒷 절반은 다른 내용 */
    spit(DB_PATH, bytes, size);
    free(bytes);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    CHECK(page_file_read(file, g_record_page, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_CORRUPT,
          "부분 쓰기(torn) 페이지는 체크섬으로 탐지");

    /* 위치가 어긋난 쓰기: 다른 page ID의 페이지를 복사해 넣으면 거절 */
    page_file_close(file);
    bytes = slurp(DB_PATH, &size);
    memcpy(bytes + at, bytes + (size_t)(g_record_page + 1) * PAGE_FILE_PAGE_BYTES, PAGE_FILE_PAGE_BYTES);
    spit(DB_PATH, bytes, size);
    free(bytes);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    CHECK(page_file_read(file, g_record_page, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_CORRUPT,
          "다른 위치의 유효한 페이지를 복사해도 page ID가 달라 거절");

    /* 범위 */
    CHECK(page_file_read(file, 0, PAGE_KIND_RECORD, 0, body) == PAGE_FILE_INVALID, "제어 페이지는 읽기 대상이 아님");
    CHECK(page_file_read(file, 100000, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_INVALID, "할당 범위 밖");
    page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_released_pages_survive_for_fallback_generation(void)
{
    printf("\n[test_released_pages_survive_for_fallback_generation]\n");
    fresh_dir();
    hooks_reset();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint32_t old_page;
    assert(page_file_alloc(file, &old_page) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES], got[PAGE_BODY_BYTES];
    memset(body, 0x33, sizeof(body));
    assert(page_file_write(file, old_page, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);   /* 세대 1: old_page를 참조한다고 본다 */
    assert(page_file_publish(file) == PAGE_FILE_OK);

    /* 세대 2는 old_page를 더는 가리키지 않는다 */
    assert(page_file_release(file, old_page) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    uint32_t next_page;
    assert(page_file_alloc(file, &next_page) == PAGE_FILE_OK);
    CHECK(next_page != old_page, "세대 2 직후에도 fallback(세대 1)이 가리키는 페이지는 재사용 안 함");
    memset(body, 0x77, sizeof(body));
    assert(page_file_write(file, next_page, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    page_file_close(file);    /* 세대 3 commit 전에 중단 */

    /* 최신 제어 쓰기(세대 2, slot 0)를 잃은 상황: 세대 1로 열리고 그 페이지는 온전하다 */
    flip_slot(DB_PATH, 0, 100);
    page_file_open_info_t open_info;
    assert(page_file_open(DB_PATH, false, &file, &open_info) == PAGE_FILE_OK);
    CHECK(page_file_control(file)->generation == 1 && open_info.damaged_slots == 1,
          "한 세대 전으로 열림");
    CHECK(page_file_read(file, old_page, PAGE_KIND_OVERFLOW, 0, got) == PAGE_FILE_OK &&
              got[0] == 0x33 && got[PAGE_BODY_BYTES - 1] == 0x33,
          "fallback 세대의 페이지는 덮어쓰이지 않음");
    page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_page_info_reports_verified_kind(void)
{
    printf("\n[test_page_info_reports_verified_kind]\n");
    fresh_dir();
    hooks_reset();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint32_t rec, dir, ovf;
    assert(page_file_alloc(file, &rec) == PAGE_FILE_OK);
    assert(page_file_alloc(file, &dir) == PAGE_FILE_OK);
    assert(page_file_alloc(file, &ovf) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES];
    memset(body, 0x5a, sizeof(body));
    assert(page_file_write(file, rec, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    assert(page_file_write(file, dir, PAGE_KIND_DIRECTORY, 7, body) == PAGE_FILE_OK);
    assert(page_file_write(file, ovf, PAGE_KIND_OVERFLOW, 9, body) == PAGE_FILE_OK);
    page_file_page_info_t info = {PAGE_KIND_CONTROL, 0};
    CHECK(page_file_page_info(file, rec, &info) == PAGE_FILE_OK && info.kind == PAGE_KIND_RECORD &&
              info.table_id == 7, "record 종류와 table ID");
    CHECK(page_file_page_info(file, dir, &info) == PAGE_FILE_OK && info.kind == PAGE_KIND_DIRECTORY &&
              info.table_id == 7, "directory 종류와 table ID");
    CHECK(page_file_page_info(file, ovf, &info) == PAGE_FILE_OK && info.kind == PAGE_KIND_OVERFLOW &&
              info.table_id == 9, "overflow 종류와 table ID");
    CHECK(page_file_read(file, dir, PAGE_KIND_RECORD, 7, body) == PAGE_FILE_CORRUPT,
          "directory를 record로 읽으면 거절");
    CHECK(page_file_read(file, rec, PAGE_KIND_DIRECTORY, 7, body) == PAGE_FILE_CORRUPT,
          "record를 directory로 읽으면 거절");
    page_file_page_info_t before = info;
    CHECK(page_file_page_info(file, 0, &info) == PAGE_FILE_INVALID &&
              page_file_page_info(file, 999, &info) == PAGE_FILE_INVALID &&
              page_file_page_info(NULL, rec, &info) == PAGE_FILE_INVALID &&
              page_file_page_info(file, rec, NULL) == PAGE_FILE_INVALID &&
              info.kind == before.kind && info.table_id == before.table_id,
          "범위 밖·제어 페이지·NULL은 INVALID이고 출력 불변");
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_publish(file) == PAGE_FILE_OK);
    page_file_close(file);
    empty_commit();

    /* 손상된 envelope와 알 수 없는 종류 */
    size_t size;
    uint8_t *bytes = slurp(DB_PATH, &size);
    assert(bytes != NULL);
    bytes[(size_t)dir * PAGE_FILE_PAGE_BYTES + 100] ^= 1;   /* 본문 손상은 checksum으로 탐지 */
    uint8_t *ovf_page = bytes + (size_t)ovf * PAGE_FILE_PAGE_BYTES;
    ovf_page[6] = 9;                                         /* 알 수 없는 종류 */
    ovf_page[7] = 0;
    reseal(ovf_page);
    spit(DB_PATH, bytes, size);
    free(bytes);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    CHECK(page_file_page_info(file, dir, &info) == PAGE_FILE_CORRUPT, "손상된 페이지는 CORRUPT");
    CHECK(page_file_page_info(file, ovf, &info) == PAGE_FILE_UNSUPPORTED, "알 수 없는 종류는 UNSUPPORTED");
    CHECK(page_file_page_info(file, rec, &info) == PAGE_FILE_OK && info.kind == PAGE_KIND_RECORD,
          "다른 페이지는 영향 없음");
    page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
/* #83: 읽기 캐시는 검증했거나 디스크에 쓴 바이트만 담고 언제나 디스크와 같다. */

#define CACHE_TEST_PAGES 20

static void cache_body(uint8_t *body, unsigned seed)
{
    for (size_t i = 0; i < PAGE_BODY_BYTES; i++) body[i] = (uint8_t)(seed * 31u + i * 7u);
}

/* 서로 다른 본문을 가진 record 페이지 CACHE_TEST_PAGES장을 게시하고 page ID를 돌려준다 */
static void build_cache_fixture(uint32_t ids[CACHE_TEST_PAGES])
{
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES];
    for (unsigned i = 0; i < CACHE_TEST_PAGES; i++) {
        assert(page_file_alloc(file, &ids[i]) == PAGE_FILE_OK);
        cache_body(body, i);
        assert(page_file_write(file, ids[i], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    }
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_publish(file) == PAGE_FILE_OK);
    assert(page_file_close(file) == PAGE_FILE_OK);
}

static void test_read_cache_matches_disk(void)
{
    printf("\n[test_read_cache_matches_disk]\n");
    fresh_dir();
    hooks_reset();
    uint32_t ids[CACHE_TEST_PAGES];
    build_cache_fixture(ids);
    empty_commit();  /* 아래에서 게시된 페이지를 제자리에서 덮어써 본다. 최신 manifest가 대조하지 않게 한다 */
    uint8_t body[PAGE_BODY_BYTES], expected[PAGE_BODY_BYTES];
    page_file_t *file;

    /* 적중한 읽기는 pread를 부르지 않는다 */
    page_file_set_default_cache_pages(CACHE_TEST_PAGES);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    hooks_reset();
    assert(page_file_read(file, ids[3], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    int after_miss = g_hooks.pread_calls;
    page_file_page_info_t info;
    CHECK(page_file_read(file, ids[3], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK &&
              page_file_page_info(file, ids[3], &info) == PAGE_FILE_OK && info.kind == PAGE_KIND_RECORD &&
              info.table_id == 7,
          "적중한 읽기와 종류 조회 성공");
    CHECK(after_miss == 1 && g_hooks.pread_calls == 1, "적중한 읽기는 pread 없음: miss %d, 합계 %d", after_miss,
          g_hooks.pread_calls);
    cache_body(expected, 3);
    CHECK(memcmp(body, expected, sizeof(body)) == 0, "적중한 본문이 디스크 본문과 같음");
    CHECK(page_file_read(file, ids[3], PAGE_KIND_INDEX, 7, body) == PAGE_FILE_CORRUPT &&
              page_file_read(file, ids[3], PAGE_KIND_RECORD, 8, body) == PAGE_FILE_CORRUPT,
          "적중해도 종류·table ID가 다르면 거절");
    page_file_cache_stats_t stats = page_file_cache_stats(file);
    CHECK(stats.hits == 4 && stats.misses == 1, "지표: 적중 %llu, miss %llu", (unsigned long long)stats.hits,
          (unsigned long long)stats.misses);
    page_file_close(file);

    /* 용량보다 많은 페이지를 두 번 훑어도 결과가 같다 */
    page_file_set_default_cache_pages(4);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    int mismatches = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned k = 0; k < CACHE_TEST_PAGES; k++) {
            unsigned i = (k * 7u + pass) % CACHE_TEST_PAGES;
            cache_body(expected, i);
            if (page_file_read(file, ids[i], PAGE_KIND_RECORD, 7, body) != PAGE_FILE_OK ||
                memcmp(body, expected, sizeof(body)) != 0) {
                mismatches++;
            }
        }
    }
    stats = page_file_cache_stats(file);
    CHECK(mismatches == 0, "작은 캐시에서 40번 읽기 결과 불일치 %d", mismatches);
    CHECK(stats.evictions > 0, "용량을 넘으면 내보냄: %llu", (unsigned long long)stats.evictions);
    page_file_close(file);

    /* pwrite 실패 뒤 다시 읽은 값은 디스크 값이다. 성공한 쓰기는 캐시와 디스크가 같다 */
    page_file_set_default_cache_pages(CACHE_TEST_PAGES);
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    assert(page_file_read(file, ids[5], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    hooks_reset();
    g_hooks.pwrite_fail_at = 1;
    cache_body(body, 99);
    CHECK(page_file_write(file, ids[5], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_IO, "주입한 pwrite 실패");
    int reads_before = g_hooks.pread_calls;
    assert(page_file_read(file, ids[5], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    cache_body(expected, 5);
    CHECK(memcmp(body, expected, sizeof(body)) == 0 && g_hooks.pread_calls == reads_before + 1,
          "실패한 쓰기 뒤에는 파일에서 다시 읽은 옛 본문");
    g_hooks.pwrite_fail_at = 0;
    cache_body(expected, 42);
    assert(page_file_write(file, ids[6], PAGE_KIND_RECORD, 7, expected) == PAGE_FILE_OK);
    reads_before = g_hooks.pread_calls;
    assert(page_file_read(file, ids[6], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK);
    CHECK(memcmp(body, expected, sizeof(body)) == 0 && g_hooks.pread_calls == reads_before,
          "성공한 쓰기는 캐시에서 새 본문");
    page_file_close(file);
    page_file_set_default_cache_pages(0);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    CHECK(page_file_read(file, ids[6], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_OK &&
              memcmp(body, expected, sizeof(body)) == 0,
          "캐시 없이 다시 열어도 디스크에 같은 본문");
    page_file_close(file);

    /* 손상 페이지는 캐시에 들어가지 않고 읽을 때마다 거절된다 */
    page_file_set_default_cache_pages(CACHE_TEST_PAGES);
    flip_slot(DB_PATH, ids[8], 3000);
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    hooks_reset();
    CHECK(page_file_read(file, ids[8], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_CORRUPT &&
              page_file_read(file, ids[8], PAGE_KIND_RECORD, 7, body) == PAGE_FILE_CORRUPT &&
              page_file_page_info(file, ids[8], &info) == PAGE_FILE_CORRUPT,
          "손상 페이지는 매번 거절");
    CHECK(g_hooks.pread_calls == 3, "손상 페이지는 캐시하지 않아 매번 파일을 읽음: %d", g_hooks.pread_calls);
    page_file_close(file);
    page_file_set_default_cache_pages(PAGE_FILE_CACHE_PAGES);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
static void test_free_list_survives_reopen(void)
{
    printf("\n[test_free_list_survives_reopen]\n");
    fresh_dir();
    hooks_reset();
    enum { N = 5000 };
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES];
    memset(body, 0x5A, sizeof(body));
    static uint32_t ids[N];
    for (int i = 0; i < N; i++) {
        assert(page_file_alloc(file, &ids[i]) == PAGE_FILE_OK);
        assert(page_file_write(file, ids[i], PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    }
    assert(page_file_commit(file) == PAGE_FILE_OK && page_file_publish(file) == PAGE_FILE_OK);
    uint32_t high = page_file_allocated_pages(file);

    /* 새 페이지는 놓는 즉시 재사용된다 */
    uint32_t fresh, again;
    assert(page_file_alloc(file, &fresh) == PAGE_FILE_OK);
    CHECK(page_file_is_fresh(file, fresh) && !page_file_is_fresh(file, ids[0]), "새 페이지 표시");
    CHECK(page_file_release(file, fresh) == PAGE_FILE_OK && page_file_pending_release_count(file) == 0 &&
              page_file_reusable_count(file) == 1, "새 페이지는 보류 없이 재사용 목록으로");
    CHECK(page_file_alloc(file, &again) == PAGE_FILE_OK && again == fresh, "놓은 새 페이지를 곧바로 다시 받음");
    page_file_release_fresh(file);
    CHECK(page_file_reusable_count(file) == 1 && !page_file_is_fresh(file, fresh), "rollback은 새 페이지를 모두 놓음");

    /* 게시된 페이지 N장을 놓으면 제어 페이지에 다 들어가지 않아 사슬로 넘친다 */
    for (int i = 0; i < N; i++) assert(page_file_release(file, ids[i]) == PAGE_FILE_OK);
    CHECK(page_file_release(file, ids[7]) == PAGE_FILE_INVALID, "이중 해제 거절");
    CHECK(page_file_commit(file) == PAGE_FILE_OK, "commit");
    size_t aging = page_file_aging_release_count(file), reusable = page_file_reusable_count(file);
    CHECK(aging == N && reusable == 0, "대기 %zu 재사용 %zu (사슬 페이지는 재사용 목록에서 받음)", aging, reusable);
    assert(page_file_close(file) == PAGE_FILE_OK);

    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    CHECK(page_file_aging_release_count(file) == N && page_file_reusable_count(file) == 0,
          "다시 열어도 대기 목록이 남음 (%zu)", page_file_aging_release_count(file));
    CHECK(page_file_commit(file) == PAGE_FILE_OK, "빈 commit");
    reusable = page_file_reusable_count(file);
    aging = page_file_aging_release_count(file);
    assert(page_file_close(file) == PAGE_FILE_OK);

    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    CHECK(page_file_reusable_count(file) == reusable && page_file_aging_release_count(file) == aging &&
              reusable + aging >= N, "다시 열어도 재사용 %zu 대기 %zu 그대로", reusable, aging);
    uint32_t before = page_file_allocated_pages(file);
    bool in_old_range = true;
    for (size_t i = 0; i < reusable; i++) {
        uint32_t id;
        assert(page_file_alloc(file, &id) == PAGE_FILE_OK);
        in_old_range = in_old_range && id < before;
    }
    CHECK(in_old_range && page_file_allocated_pages(file) == before && before <= high + 4,
          "재사용 페이지로 할당하고 파일이 자라지 않음 (%u -> %u)", high, before);
    page_file_release_fresh(file);
    assert(page_file_close(file) == PAGE_FILE_OK);

    /* 읽기 핸들은 빈 목록을 읽지 않는다 */
    CHECK(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK && page_file_reusable_count(file) == 0,
          "읽기 열기");
    page_file_close(file);

    /* 빈 목록의 중복 번호는 쓰기 열기에서 손상이다 */
    size_t size;
    uint8_t *bytes = slurp(DB_PATH, &size);
    uint32_t slot = page_file_control_generation_slot(bytes);
    uint8_t *control = bytes + (size_t)slot * PAGE_FILE_PAGE_BYTES;
    uint32_t manifest = (uint32_t)control[32 + 68] | (uint32_t)control[32 + 69] << 8;
    uint8_t *ids_at = control + 32 + 72 + 8 * manifest;
    memcpy(ids_at + 4, ids_at, 4);
    reseal(control);
    spit(DB_PATH, bytes, size);
    free(bytes);
    file = NULL;
    CHECK(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_CORRUPT, "중복 번호 거절");
    if (file != NULL) page_file_close(file);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
/* 사슬로 넘치는 빈 목록을 쓰는 commit의 모든 pwrite·fsync 실패 지점에서 다시 열면 이전 또는 새 빈 목록이다 */
static void free_list_op(page_file_t *file)
{
    uint8_t body[PAGE_BODY_BYTES];
    memset(body, 0x33, sizeof(body));
    for (int i = 0; i < 3; i++) {
        uint32_t id;
        assert(page_file_alloc(file, &id) == PAGE_FILE_OK);
        assert(page_file_write(file, id, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    }
}

static void test_free_list_commit_failures(void)
{
    printf("\n[test_free_list_commit_failures]\n");
    fresh_dir();
    hooks_reset();
    enum { N = 5000 };
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES];
    memset(body, 0x44, sizeof(body));
    static uint32_t ids[N];
    for (int i = 0; i < N; i++) {
        assert(page_file_alloc(file, &ids[i]) == PAGE_FILE_OK);
        assert(page_file_write(file, ids[i], PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    }
    assert(page_file_commit(file) == PAGE_FILE_OK && page_file_publish(file) == PAGE_FILE_OK);
    for (int i = 0; i < N; i++) assert(page_file_release(file, ids[i]) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK && page_file_close(file) == PAGE_FILE_OK);
    size_t size;
    uint8_t *base = slurp(DB_PATH, &size);

    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    size_t old_r = page_file_reusable_count(file), old_a = page_file_aging_release_count(file);
    free_list_op(file);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    size_t new_r = page_file_reusable_count(file), new_a = page_file_aging_release_count(file);
    assert(page_file_close(file) == PAGE_FILE_OK);
    CHECK(old_a >= N && new_r >= N - 10, "기준: 이전 재사용 %zu 대기 %zu, 새 재사용 %zu 대기 %zu", old_r, old_a, new_r,
          new_a);

    int injected = 0, bad = 0, old_seen = 0, new_seen = 0;
    for (int which = 0; which < 2; which++) {
        for (int k = 1; k <= 40; k++) {
            spit(DB_PATH, base, size);
            hooks_reset();
            assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
            free_list_op(file);
            if (which == 0) g_hooks.pwrite_fail_at = k;
            else g_hooks.fsync_fail_at = k;
            page_file_status_t status = page_file_commit(file);
            hooks_reset();
            page_file_close(file);
            if (status == PAGE_FILE_OK) break;
            injected++;
            page_file_t *again = NULL;
            if (page_file_open(DB_PATH, true, &again, NULL) != PAGE_FILE_OK) {
                printf("  %s %d: 다시 열기 실패\n", which == 0 ? "pwrite" : "fsync", k);
                bad++;
                continue;
            }
            size_t r = page_file_reusable_count(again), a = page_file_aging_release_count(again);
            bool is_old = r == old_r && a == old_a, is_new = r == new_r && a == new_a;
            old_seen += is_old;
            new_seen += is_new;
            /* 재사용 목록의 페이지는 모두 받을 수 있고 파일 범위 안이다 */
            uint32_t high = page_file_allocated_pages(again);
            bool in_range = true;
            for (size_t i = 0; i < r; i++) {
                uint32_t id;
                in_range = in_range && page_file_alloc(again, &id) == PAGE_FILE_OK && id < high;
            }
            if (!(is_old || is_new) || !in_range) {
                printf("  %s %d: 재사용 %zu 대기 %zu\n", which == 0 ? "pwrite" : "fsync", k, r, a);
                bad++;
            }
            page_file_close(again);
        }
    }
    CHECK(bad == 0 && injected > 0 && old_seen > 0 && new_seen > 0,
          "주입 %d곳에서 어긋남 %d (이전 %d, 새 %d)", injected, bad, old_seen, new_seen);
    free(base);
    clean_dir();
}

/* ══════════════════════════════════════════════════════════════ */
/* fsync 한 번 commit: 제어 페이지는 남았지만 데이터 쓰기가 사라졌으면(전원 장애 모사) 이전 세대로 연다 */
static void test_incomplete_commit_falls_back(void)
{
    printf("\n[test_incomplete_commit_falls_back]\n");
    fresh_dir();
    hooks_reset();
    page_file_t *file;
    assert(page_file_create(DB_PATH, &file) == PAGE_FILE_OK);
    uint8_t body[PAGE_BODY_BYTES], got[PAGE_BODY_BYTES];
    uint32_t p1, p2;
    memset(body, 0x61, sizeof(body));
    assert(page_file_alloc(file, &p1) == PAGE_FILE_OK && page_file_write(file, p1, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK && page_file_publish(file) == PAGE_FILE_OK);
    assert(page_file_close(file) == PAGE_FILE_OK);
    size_t size1;
    uint8_t *gen1 = slurp(DB_PATH, &size1);

    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    memset(body, 0x62, sizeof(body));
    assert(page_file_alloc(file, &p2) == PAGE_FILE_OK && page_file_write(file, p2, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK);
    hooks_reset();
    assert(page_file_commit(file) == PAGE_FILE_OK);
    CHECK(g_hooks.io_calls > 0, "commit");
    assert(page_file_close(file) == PAGE_FILE_OK);
    size_t size2;
    uint8_t *gen2 = slurp(DB_PATH, &size2);
    CHECK(size2 > size1, "새 페이지가 파일을 늘림 (%zu -> %zu)", size1, size2);

    /* 1. 파일 길이가 늘지 않았다(새 페이지 쓰기가 사라짐) */
    uint8_t *mixed = malloc(size2);
    memcpy(mixed, gen1, size1);
    memcpy(mixed, gen2, 2 * PAGE_FILE_PAGE_BYTES);   /* 제어 페이지 두 장은 세대 2의 것 */
    spit(DB_PATH, mixed, size1);
    page_file_open_info_t info;
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK && page_file_control(file)->generation == 1 &&
              info.damaged_slots == 1, "짧은 파일: 세대 1로 열고 손상 슬롯 1");
    page_file_close(file);

    /* 2. 길이는 늘었지만 새 페이지 내용이 0이다 */
    memcpy(mixed, gen2, size2);
    memset(mixed + (size_t)p2 * PAGE_FILE_PAGE_BYTES, 0, PAGE_FILE_PAGE_BYTES);
    spit(DB_PATH, mixed, size2);
    CHECK(page_file_open(DB_PATH, true, &file, &info) == PAGE_FILE_OK && page_file_control(file)->generation == 1 &&
              info.damaged_slots == 1, "사라진 데이터 쓰기: 세대 1로 엶");
    CHECK(page_file_read(file, p1, PAGE_KIND_OVERFLOW, 0, got) == PAGE_FILE_OK && got[0] == 0x61, "세대 1의 페이지");
    /* 이어서 commit하면 끝나지 않은 세대의 슬롯을 덮어쓴다 */
    CHECK(page_file_commit(file) == PAGE_FILE_OK && page_file_control(file)->generation == 2, "이어서 commit");
    page_file_close(file);
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK && page_file_control(file)->generation == 2 &&
              info.damaged_slots == 0, "새 세대 2는 온전함");
    page_file_close(file);

    /* 3. 온전한 세대 2는 그대로 열린다 */
    spit(DB_PATH, gen2, size2);
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_OK && page_file_control(file)->generation == 2 &&
              page_file_read(file, p2, PAGE_KIND_OVERFLOW, 0, got) == PAGE_FILE_OK && got[0] == 0x62,
          "온전한 commit은 새 세대");
    page_file_close(file);

    /* 4. 첫 세대가 끝나지 않았으면 돌아갈 세대가 없어 손상이다 */
    memcpy(mixed, gen1, size1);
    memset(mixed + (size_t)p1 * PAGE_FILE_PAGE_BYTES, 0, PAGE_FILE_PAGE_BYTES);
    spit(DB_PATH, mixed, size1);
    CHECK(page_file_open(DB_PATH, false, &file, &info) == PAGE_FILE_CORRUPT, "첫 세대가 끝나지 않으면 손상");
    free(mixed);
    free(gen1);
    free(gen2);
    clean_dir();
}

/*
 * #78: 짧은 읽기·쓰기와 EINTR은 이어서 끝까지 옮기고, 진행 없는 0바이트 반환과 끝없는 EINTR은 성공으로 끝나지
 * 않는다. 0바이트 pread는 잘린 파일, 0바이트 pwrite는 공간 부족과 같게 본다.
 */
static void test_short_io_and_eintr(void)
{
    printf("\n[test_short_io_and_eintr]\n");
    fresh_dir();
    hooks_reset();
    page_file_set_default_cache_pages(0);
    uint8_t body[PAGE_BODY_BYTES], got[PAGE_BODY_BYTES];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = (uint8_t)(i * 7 + 3);

    /* 1000바이트씩 끊기고 두 번에 한 번 EINTR이어도 commit과 다시 읽기가 같은 바이트다 */
    g_hooks.short_max = 1000;
    g_hooks.eintr_odd = true;
    page_file_t *file = NULL;
    uint32_t id = 0;
    bool created = page_file_create(DB_PATH, &file) == PAGE_FILE_OK;
    CHECK(created && page_file_alloc(file, &id) == PAGE_FILE_OK &&
              page_file_write(file, id, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_OK &&
              page_file_commit(file) == PAGE_FILE_OK && page_file_publish(file) == PAGE_FILE_OK,
          "짧은 쓰기·EINTR 속 생성과 commit 성공");
    if (created) page_file_close(file);
    bool opened = page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK;
    CHECK(opened && page_file_read(file, id, PAGE_KIND_OVERFLOW, 0, got) == PAGE_FILE_OK &&
              memcmp(body, got, sizeof(body)) == 0,
          "짧은 읽기·EINTR 속 다시 읽은 바이트가 같음");
    if (opened) page_file_close(file);
    if (!created || !opened) {
        g_hooks.short_max = 0;
        g_hooks.eintr_odd = false;
        clean_dir();
        return;
    }

    /* 페이지 중간의 0바이트 pread는 잘린 파일이다. 읽은 앞부분을 성공으로 돌려주지 않는다 */
    hooks_reset();
    g_hooks.short_max = 1000;
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    g_hooks.pread_calls = 0;
    g_hooks.pread_zero_at = 3;
    errno = 0;
    CHECK(page_file_read(file, id, PAGE_KIND_OVERFLOW, 0, got) == PAGE_FILE_IO && errno == EIO,
          "중간 0바이트 pread는 IO와 EIO");
    page_file_close(file);

    /* 0바이트 pwrite는 진행이 없으므로 공간 부족이다. 실패한 commit은 이전 세대를 남긴다 */
    hooks_reset();
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    memset(body, 0x5a, sizeof(body));
    uint32_t fresh;
    assert(page_file_alloc(file, &fresh) == PAGE_FILE_OK);
    g_hooks.pwrite_zero_at = 1;
    errno = 0;
    CHECK(page_file_write(file, fresh, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_IO && errno == ENOSPC,
          "0바이트 pwrite는 IO와 ENOSPC");
    page_file_close(file);

    /* 끝없는 EINTR은 재시도 한도 뒤 실패다 */
    hooks_reset();
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    assert(page_file_alloc(file, &fresh) == PAGE_FILE_OK);
    g_hooks.eintr_always = true;
    errno = 0;
    CHECK(page_file_write(file, fresh, PAGE_KIND_OVERFLOW, 0, body) == PAGE_FILE_IO && errno == EINTR &&
              g_hooks.raw_calls <= 1000,
          "끝없는 EINTR은 한도 뒤 IO");
    g_hooks.eintr_always = false;
    page_file_close(file);

    hooks_reset();
    page_file_set_default_cache_pages(PAGE_FILE_CACHE_PAGES);
    CHECK(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK &&
              page_file_read(file, id, PAGE_KIND_OVERFLOW, 0, got) == PAGE_FILE_OK && got[0] == (uint8_t)3,
          "실패한 쓰기들 뒤에도 게시된 세대는 처음 그대로");
    page_file_close(file);
    clean_dir();
}

int main(void)
{
    /* 잠금 충돌은 기다리지 않고 곧바로 LOCKED로 확인한다 */
    page_file_set_default_lock_timeout(0);
    page_file_set_default_io(&HOOKED);
    fill_big();
    test_fixture_roundtrip_is_page_size_independent();
    test_invalid_files_are_rejected_without_change();
    test_control_generations_and_fallback_limits();
    test_commit_failures_keep_current_generation();
    test_publish_never_clobbers_destination();
    test_locking_and_unpublished_files();
    test_corrupt_data_pages_are_detected();
    test_released_pages_survive_for_fallback_generation();
    test_page_info_reports_verified_kind();
    test_read_cache_matches_disk();
    test_free_list_survives_reopen();
    test_free_list_commit_failures();
    test_incomplete_commit_falls_back();
    test_short_io_and_eintr();
    page_file_set_default_io(NULL);
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
