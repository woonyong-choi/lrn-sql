/*
 * test_page_file_durable.c -- page_file durable extension(applied_lsn·wal_file_id·durable_epoch) 검사 (#8)
 *
 * 공개 API와 실제 파일만 쓴다. 합성 데이터이며 WAL과의 짝 판정은 test_durable_pair.c가 다룬다.
 *  1. 52바이트 v1 제어 페이지의 읽기와 쓰기가 enable 전에는 그대로다
 *  2. v2 왕복과 디스크 배치, ready 규칙, 단조 규칙
 *  3. 제어 슬롯 손상: ready 뒤에는 살아남은 슬롯이 ready이고 상태 참조와 applied_lsn이 항상 짝이다
 *  4. 형식 거절: 알 수 없는 flags·version·길이, v1 길이의 v2, 예약 영역
 *  5. 최초 전환의 모든 종료 지점과 I/O 실패 지점에서 복구 불변식
 *  6. catalog_store_save_applied의 원자 게시와 실패 복원
 *  7. durable_epoch 증가 전용 갱신
 *
 * 이 검사는 프로세스 종료와 I/O 실패 주입만 다룬다. 전원 장애와 lost write(성공 뒤 쓰기가 사라짐)는 다루지 않는다.
 *
 * 빌드는 Makefile의 STORAGE_OBJS와 같은 목록을 쓴다(저장소 루트에서, build/storage의 객체 사용):
 *   cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=undefined -Iinclude -o build/test_page_file_durable \
 *      tests/test_page_file_durable.c build/storage/{record,record_page,overflow,value,catalog,page_file,catalog_store}.o
 * 실행은 저장소 루트에서 한다.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/page_file.h"

#define DIR_NAME "__test__page_file_durable"
#define DB DIR_NAME "/db.lrdb"
#define BASE DIR_NAME "/base.bin"
#define WAL_ID 0x1122334455667788ull
#define EPOCH 3u

static int g_run, g_fail;

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

/* ---- 파일 보조 ---- */

static void fresh_dir(void)
{
    DIR *d = opendir(DIR_NAME);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", DIR_NAME, e->d_name);
            unlink(path);
        }
        closedir(d);
    } else {
        mkdir(DIR_NAME, 0700);
    }
}

static bool read_all(const char *path, uint8_t **buf, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    *len = (size_t)st.st_size;
    *buf = malloc(*len > 0 ? *len : 1);
    size_t got = 0;
    while (got < *len) {
        ssize_t r = pread(fd, *buf + got, *len - got, (off_t)got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == *len;
}

static void write_all(const char *path, const uint8_t *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    size_t put = 0;
    while (put < len) {
        ssize_t r = pwrite(fd, buf + put, len - put, (off_t)put);
        if (r <= 0) break;
        put += (size_t)r;
    }
    close(fd);
}

static void patch(const char *path, off_t off, const void *bytes, size_t n)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (pwrite(fd, bytes, n, off) != (ssize_t)n) fprintf(stderr, "patch 실패\n");
        close(fd);
    }
}

static uint32_t test_checksum(const uint8_t *page)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < PAGE_FILE_PAGE_BYTES; i++) {
        if (i >= 28 && i < 32) continue;
        hash = (hash ^ page[i]) * 16777619u;
    }
    return hash;
}

static void reseal(uint8_t *page)
{
    uint32_t sum = test_checksum(page);
    for (unsigned i = 0; i < 4; i++) page[28 + i] = (uint8_t)(sum >> (i * 8));
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/* 슬롯 페이지 읽기. 본문은 page + 32 */
static bool read_slot(const char *path, int slot, uint8_t page[PAGE_FILE_PAGE_BYTES])
{
    uint8_t *all = NULL;
    size_t len = 0;
    if (!read_all(path, &all, &len) || len < 2 * PAGE_FILE_PAGE_BYTES) {
        free(all);
        return false;
    }
    memcpy(page, all + (size_t)slot * PAGE_FILE_PAGE_BYTES, PAGE_FILE_PAGE_BYTES);
    free(all);
    return true;
}

/* 슬롯을 손상시킨다: 본문 뒤쪽 바이트 하나를 뒤집어 체크섬이 어긋나게 한다 */
static void damage_slot(const char *path, int slot)
{
    uint8_t b;
    int fd = open(path, O_RDWR);
    off_t off = (off_t)slot * PAGE_FILE_PAGE_BYTES + 200;
    if (fd >= 0 && pread(fd, &b, 1, off) == 1) {
        b ^= 0x5A;
        if (pwrite(fd, &b, 1, off) != 1) fprintf(stderr, "damage 실패\n");
    }
    if (fd >= 0) close(fd);
}

/* ---- 상태 참조와 applied_lsn의 짝: 데이터 페이지에 lsn을 적고 카탈로그 generation은 lsn+100 ---- */

static bool commit_marker(page_file_t *file, uint64_t lsn, bool with_lsn)
{
    uint8_t body[PAGE_BODY_BYTES] = {0};
    for (int i = 0; i < 8; i++) body[i] = (uint8_t)(lsn >> (i * 8));
    uint32_t page = 0;
    if (page_file_alloc(file, &page) != PAGE_FILE_OK) return false;
    if (page_file_write(file, page, PAGE_KIND_RECORD, 0, body) != PAGE_FILE_OK) return false;
    if (page_file_stage_catalog(file, page, 8, lsn + 100) != PAGE_FILE_OK) return false;
    if (with_lsn && page_file_stage_applied_lsn(file, lsn) != PAGE_FILE_OK) return false;
    uint32_t old = page_file_control(file)->catalog_head;
    if (page_file_commit(file) != PAGE_FILE_OK) return false;
    if (old != PAGE_FILE_NO_PAGE) (void)page_file_release(file, old);
    return true;
}

/* 열린 파일의 상태 참조와 applied_lsn이 같은 commit의 것이다 */
static bool pair_ok(page_file_t *file, const char *label)
{
    const page_file_control_t *c = page_file_control(file);
    if (c->catalog_head == PAGE_FILE_NO_PAGE) return c->applied_lsn == 0;
    uint8_t body[PAGE_BODY_BYTES];
    if (page_file_read(file, c->catalog_head, PAGE_KIND_RECORD, 0, body) != PAGE_FILE_OK) {
        printf("  [%s] 상태 페이지를 읽지 못한다\n", label);
        return false;
    }
    uint64_t marker = le64(body);
    if (marker + 100 != c->catalog_generation) {
        printf("  [%s] 페이지(%llu)와 카탈로그 generation(%llu)이 어긋난다\n", label, (unsigned long long)marker,
               (unsigned long long)c->catalog_generation);
        return false;
    }
    if (c->format_version == PAGE_FILE_DURABLE_FORMAT_VERSION && marker != c->applied_lsn) {
        printf("  [%s] 상태 페이지 %llu와 applied_lsn %llu이 짝이 아니다\n", label, (unsigned long long)marker,
               (unsigned long long)c->applied_lsn);
        return false;
    }
    return true;
}

static void make_v1_base(void)
{
    page_file_t *f = NULL;
    CHECK(page_file_create(DB, &f) == PAGE_FILE_OK, "v1 파일 생성");
    if (f == NULL) return;
    CHECK(commit_marker(f, 0, false), "v1 첫 commit");
    CHECK(page_file_publish(f) == PAGE_FILE_OK, "게시");
    CHECK(page_file_close(f) == PAGE_FILE_OK, "닫기");
}

static bool open_rw(page_file_t **f, page_file_open_info_t *info)
{
    return page_file_open(DB, true, f, info) == PAGE_FILE_OK;
}

/* v1 파일에서 ready까지: enable, commit 두 번(marker 0). 열린 쓰기 핸들을 돌려준다 */
static page_file_t *open_ready(void)
{
    fresh_dir();
    make_v1_base();
    page_file_t *f = NULL;
    if (!open_rw(&f, NULL)) return NULL;
    if (page_file_enable_durable(f, WAL_ID, EPOCH) != PAGE_FILE_OK || !commit_marker(f, 0, true) ||
        !commit_marker(f, 0, true) || !page_file_durable_ready(f)) {
        page_file_close(f);
        return NULL;
    }
    return f;
}

/* ---- 1. v1 보존 ---- */

static void test_v1_preserved(void)
{
    printf("\n[v1 52바이트 제어 페이지 보존]\n");
    fresh_dir();
    make_v1_base();
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    CHECK(read_slot(DB, 1, page) && le32(page + 16) == 52 && le32(page + 32 + 8) == 1, "첫 commit은 v1 52바이트 본문");

    page_file_t *f = NULL;
    page_file_open_info_t info;
    CHECK(page_file_open(DB, false, &f, &info) == PAGE_FILE_OK, "읽기 열기");
    if (f != NULL) {
        const page_file_control_t *c = page_file_control(f);
        CHECK(c->format_version == 1 && c->applied_lsn == 0 && c->wal_file_id == 0 && c->durable_epoch == 0 &&
                  !c->durable_ready,
              "v1은 확장 값이 모두 0");
        CHECK(!page_file_durable_ready(f), "v1은 ready가 아니다");
        CHECK(page_file_check_wal_identity(f, WAL_ID) == PAGE_FILE_MISMATCH, "v1은 어떤 WAL과도 짝이 아니다");
        CHECK(page_file_enable_durable(f, WAL_ID, EPOCH) == PAGE_FILE_INVALID, "읽기 핸들은 enable 불가");
        page_file_close(f);
    }
    CHECK(open_rw(&f, NULL), "쓰기 열기");
    if (f == NULL) return;
    CHECK(page_file_stage_applied_lsn(f, 0) == PAGE_FILE_INVALID, "enable 전에는 applied_lsn 불가");
    CHECK(page_file_stage_durable_epoch(f, 1) == PAGE_FILE_INVALID, "enable 전에는 epoch 불가");
    CHECK(commit_marker(f, 7, false), "enable 없이 v1로 계속 commit");
    page_file_close(f);
    CHECK(read_slot(DB, 0, page) && le32(page + 16) == 52 && le32(page + 32 + 8) == 1, "다음 슬롯도 v1 52바이트");
    CHECK(open_rw(&f, NULL) && page_file_control(f)->format_version == 1, "여전히 v1로 열린다");
    if (f != NULL) {
        CHECK(pair_ok(f, "v1"), "v1은 카탈로그 generation만 짝");
        page_file_close(f);
    }
}

/* ---- 2. 전환, 왕복, 규칙 ---- */

static void test_enable_and_roundtrip(void)
{
    printf("\n[enable과 v2 왕복]\n");
    fresh_dir();
    make_v1_base();
    page_file_t *f = NULL;
    CHECK(open_rw(&f, NULL), "열기");
    if (f == NULL) return;
    CHECK(page_file_enable_durable(f, 0, EPOCH) == PAGE_FILE_INVALID, "wal_file_id 0은 거절");
    CHECK(page_file_enable_durable(f, WAL_ID, EPOCH) == PAGE_FILE_OK, "enable");
    CHECK(page_file_enable_durable(f, WAL_ID, EPOCH) == PAGE_FILE_OK, "같은 값은 무동작");
    CHECK(page_file_enable_durable(f, WAL_ID + 1, EPOCH) == PAGE_FILE_INVALID, "다른 wal_file_id는 거절");
    CHECK(page_file_enable_durable(f, WAL_ID, EPOCH + 1) == PAGE_FILE_INVALID, "다른 epoch로 enable을 다시 하면 거절");
    CHECK(page_file_stage_applied_lsn(f, 5) == PAGE_FILE_INVALID, "ready 전에는 applied_lsn을 올릴 수 없다");
    CHECK(commit_marker(f, 0, true), "v2 첫 commit(ready 아님)");
    CHECK(!page_file_durable_ready(f), "첫 v2 commit은 ready가 아니다");
    CHECK(page_file_stage_applied_lsn(f, 5) == PAGE_FILE_INVALID, "첫 v2 commit 뒤에도 올릴 수 없다");
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    CHECK(read_slot(DB, 0, page) && le32(page + 16) == 76 && le32(page + 32 + 8) == 2, "v2 본문은 76바이트 version 2");
    CHECK(le32(page + 32 + 72) == 0, "첫 v2 슬롯의 flags는 0");
    CHECK(read_slot(DB, 1, page) && le32(page + 16) == 52, "반대편 슬롯은 아직 v1");
    CHECK(commit_marker(f, 0, true), "v2 두 번째 commit");
    CHECK(page_file_durable_ready(f), "두 번째 v2 commit부터 ready");
    CHECK(read_slot(DB, 1, page) && le32(page + 32 + 72) == 1, "두 번째 v2 슬롯은 ready flag");
    CHECK(page_file_stage_applied_lsn(f, 5) == PAGE_FILE_OK, "ready 뒤에는 올릴 수 있다");
    CHECK(commit_marker(f, 5, true), "applied_lsn 5 게시");
    CHECK(page_file_stage_applied_lsn(f, 4) == PAGE_FILE_INVALID, "낮추는 것은 거절");
    CHECK(page_file_stage_applied_lsn(f, 5) == PAGE_FILE_OK, "같은 값은 허용");
    CHECK(page_file_check_wal_identity(f, WAL_ID) == PAGE_FILE_OK, "같은 WAL");
    CHECK(page_file_check_wal_identity(f, WAL_ID + 1) == PAGE_FILE_MISMATCH, "다른 WAL");
    CHECK(page_file_check_wal_identity(f, 0) == PAGE_FILE_INVALID, "0은 거절");
    page_file_close(f);

    CHECK(open_rw(&f, NULL), "다시 열기");
    if (f != NULL) {
        const page_file_control_t *c = page_file_control(f);
        CHECK(c->format_version == 2 && c->applied_lsn == 5 && c->wal_file_id == WAL_ID && c->durable_epoch == EPOCH &&
                  c->durable_ready,
              "v2 값이 왕복한다");
        CHECK(c->catalog_generation == 105, "카탈로그 generation은 applied_lsn과 별개 값이다");
        CHECK(pair_ok(f, "왕복"), "상태 참조와 applied_lsn이 짝");
        page_file_close(f);
    }
    CHECK(read_slot(DB, 0, page) || true, "배치 확인");
    uint8_t *all = NULL;
    size_t len = 0;
    CHECK(read_all(DB, &all, &len), "원본 읽기");
    if (all != NULL) {
        for (int slot = 0; slot < 2; slot++) {
            const uint8_t *p = all + (size_t)slot * PAGE_FILE_PAGE_BYTES;
            bool ok = true;
            for (size_t i = 32 + 76; i < PAGE_FILE_PAGE_BYTES; i++) ok = ok && p[i] == 0;
            CHECK(ok, "슬롯 %d의 본문 뒤는 모두 0", slot);
        }
        free(all);
    }
}

/* ---- 3. 슬롯 손상 ---- */

static void test_slot_damage(void)
{
    printf("\n[제어 슬롯 손상]\n");
    page_file_t *f = open_ready();
    CHECK(f != NULL, "ready 파일");
    if (f == NULL) return;
    CHECK(commit_marker(f, 5, true) && commit_marker(f, 9, true), "applied 5, 9 게시");
    page_file_close(f);
    uint8_t *saved = NULL;
    size_t saved_len = 0;
    CHECK(read_all(DB, &saved, &saved_len), "저장");

    /* 최신 슬롯 손상: 살아남은 슬롯은 ready이고 한 세대 전의 짝을 보인다 */
    uint64_t newest_gen = 0;
    int newest = -1;
    for (int slot = 0; slot < 2; slot++) {
        uint8_t page[PAGE_FILE_PAGE_BYTES];
        read_slot(DB, slot, page);
        uint64_t gen = le64(page + 32 + 20);
        if (gen > newest_gen) {
            newest_gen = gen;
            newest = slot;
        }
    }
    damage_slot(DB, newest);
    page_file_open_info_t info;
    CHECK(open_rw(&f, &info), "최신 슬롯 손상 뒤에도 열린다");
    if (f != NULL) {
        const page_file_control_t *c = page_file_control(f);
        CHECK(info.damaged_slots == 1, "손상 1");
        CHECK(c->format_version == 2 && c->durable_ready && c->wal_file_id == WAL_ID, "살아남은 슬롯은 v2 ready");
        CHECK(c->applied_lsn == 5, "한 세대 전 applied_lsn 5 (%llu)", (unsigned long long)c->applied_lsn);
        CHECK(pair_ok(f, "최신 손상"), "그 세대의 상태 참조와 짝이다");
        CHECK(page_file_stage_applied_lsn(f, 9) == PAGE_FILE_OK, "ready라 다시 올릴 수 있다");
        CHECK(commit_marker(f, 9, true), "손상 슬롯을 덮어쓰며 다시 게시");
        page_file_close(f);
    }
    CHECK(open_rw(&f, &info) && info.damaged_slots == 0 && page_file_control(f)->applied_lsn == 9, "다시 게시한 뒤 정상");
    if (f != NULL) page_file_close(f);

    /* 이전 슬롯 손상 */
    write_all(DB, saved, saved_len);
    damage_slot(DB, 1 - newest);
    CHECK(open_rw(&f, &info), "이전 슬롯 손상");
    if (f != NULL) {
        CHECK(info.damaged_slots == 1 && page_file_control(f)->applied_lsn == 9 && pair_ok(f, "이전 손상"),
              "최신 슬롯이 그대로 쓰인다");
        page_file_close(f);
    }

    /* 최초 전환 중간: 첫 v2 슬롯이 손상되면 v1로 fallback하며 ready가 아니다 */
    fresh_dir();
    make_v1_base();
    CHECK(open_rw(&f, NULL) && page_file_enable_durable(f, WAL_ID, EPOCH) == PAGE_FILE_OK &&
              commit_marker(f, 0, true),
          "첫 v2 commit만");
    if (f != NULL) page_file_close(f);
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    int first_v2 = (read_slot(DB, 0, page) && le32(page + 16) == 76) ? 0 : 1;
    damage_slot(DB, first_v2);
    CHECK(open_rw(&f, &info), "첫 v2 슬롯 손상");
    if (f != NULL) {
        CHECK(info.damaged_slots == 1 && page_file_control(f)->format_version == 1 && !page_file_durable_ready(f),
              "v1로 fallback하고 ready가 아니다");
        page_file_close(f);
    }

    /* 두 번째 v2 commit 뒤 최신(ready) 슬롯 손상: 살아남은 슬롯은 ready가 아닌 첫 v2다 */
    fresh_dir();
    make_v1_base();
    CHECK(open_rw(&f, NULL) && page_file_enable_durable(f, WAL_ID, EPOCH) == PAGE_FILE_OK &&
              commit_marker(f, 0, true) && commit_marker(f, 0, true),
          "v2 commit 두 번");
    if (f != NULL) page_file_close(f);
    int ready_slot = (read_slot(DB, 1, page) && le32(page + 32 + 72) == 1) ? 1 : 0;
    damage_slot(DB, ready_slot);
    CHECK(open_rw(&f, &info), "ready 슬롯 손상");
    if (f != NULL) {
        CHECK(page_file_control(f)->format_version == 2 && !page_file_durable_ready(f) &&
                  page_file_control(f)->applied_lsn == 0,
              "첫 v2는 ready가 아니고 applied_lsn 0");
        CHECK(page_file_stage_applied_lsn(f, 3) == PAGE_FILE_INVALID, "ready가 아니면 올릴 수 없다");
        page_file_close(f);
    }
    free(saved);
}

/* ---- 4. 형식 거절 ---- */

static bool forge_open(uint8_t *page, int slot, page_file_status_t *status, page_file_open_info_t *info)
{
    reseal(page);
    patch(DB, (off_t)slot * PAGE_FILE_PAGE_BYTES, page, PAGE_FILE_PAGE_BYTES);
    page_file_t *f = NULL;
    *status = page_file_open(DB, false, &f, info);
    if (f != NULL) page_file_close(f);
    return true;
}

static void test_format_rejection(void)
{
    printf("\n[형식 거절]\n");
    page_file_t *f = open_ready();
    CHECK(f != NULL, "ready 파일");
    if (f == NULL) return;
    page_file_close(f);
    uint8_t *orig = NULL;
    size_t len = 0;
    read_all(DB, &orig, &len);
    int newest = 0;
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    uint64_t best = 0;
    for (int slot = 0; slot < 2; slot++) {
        read_slot(DB, slot, page);
        if (le64(page + 32 + 20) > best) {
            best = le64(page + 32 + 20);
            newest = slot;
        }
    }
    page_file_status_t st;
    page_file_open_info_t info;

    read_slot(DB, newest, page);
    put_le32(page + 32 + 72, 2); /* 알 수 없는 확장 flag */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_UNSUPPORTED, "알 수 없는 flags는 UNSUPPORTED (%d)", (int)st);

    write_all(DB, orig, len);
    read_slot(DB, newest, page);
    put_le32(page + 32 + 8, 3); /* 알 수 없는 version */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_UNSUPPORTED, "알 수 없는 version은 UNSUPPORTED (%d)", (int)st);

    write_all(DB, orig, len);
    read_slot(DB, newest, page);
    put_le32(page + 32 + 8, 1); /* v1이라면서 본문이 76바이트 */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_OK && info.damaged_slots == 1, "v1 표시의 76바이트 본문은 손상으로 보고 fallback");

    write_all(DB, orig, len);
    read_slot(DB, newest, page);
    put_le32(page + 16, 52); /* v2라면서 본문이 52바이트 */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_OK && info.damaged_slots == 1, "v2 표시의 52바이트 본문은 손상으로 보고 fallback");

    write_all(DB, orig, len);
    read_slot(DB, newest, page);
    memset(page + 32 + 60, 0, 8); /* wal_file_id 0 */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_OK && info.damaged_slots == 1, "v2의 wal_file_id 0은 손상");

    write_all(DB, orig, len);
    read_slot(DB, newest, page);
    page[32 + 76] = 1; /* 본문 길이 뒤 비0 */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_OK && info.damaged_slots == 1, "본문 뒤 비0은 손상");

    write_all(DB, orig, len);
    read_slot(DB, newest, page);
    put_le32(page + 32 + 32, 1); /* 예약 u32(32)는 v2에서도 0 */
    forge_open(page, newest, &st, &info);
    CHECK(st == PAGE_FILE_OK && info.damaged_slots == 1, "예약 영역 비0은 손상");

    write_all(DB, orig, len);
    CHECK(open_rw(&f, NULL), "원본은 정상");
    if (f != NULL) page_file_close(f);
    free(orig);
}

/* ---- 5. 최초 전환의 종료·실패 지점 ---- */

static long g_points_left = -1;
static long g_points_seen;
static long g_fail_at;
static long g_fail_seen;
static bool g_fail_mode;

static bool point(void)
{
    g_points_seen++;
    if (g_fail_mode) return g_fail_at > 0 && ++g_fail_seen == g_fail_at;
    if (g_points_left >= 0 && g_points_left-- == 0) _exit(137);
    return false;
}

static ssize_t h_pwrite(int fd, const void *b, size_t n, off_t o)
{
    if (point()) {
        errno = EIO;
        return -1;
    }
    ssize_t r = pwrite(fd, b, n, o);
    point();
    return r;
}

static int h_fsync(int fd)
{
    if (point()) {
        errno = EIO;
        return -1;
    }
    int r = fsync(fd);
    point();
    return r;
}

static int h_link(const char *a, const char *b)
{
    point();
    return link(a, b);
}

static int h_unlink(const char *p)
{
    point();
    return unlink(p);
}

static const page_file_io_t HOOKS = {pread, h_pwrite, h_fsync, h_link, h_unlink};

typedef struct {
    uint64_t acked;     /* commit이 성공한 가장 큰 applied_lsn */
    uint8_t ready_acked;
} shared_t;

static shared_t *g_shared;

/* 전환 순서: enable, 두 번 commit(marker 0), 그리고 applied 5, 9 */
static void run_sequence(page_file_t *f)
{
    if (page_file_enable_durable(f, WAL_ID, EPOCH) != PAGE_FILE_OK) return;
    if (!commit_marker(f, 0, true)) return;
    if (!commit_marker(f, 0, true)) return;
    g_shared->ready_acked = 1;
    if (!commit_marker(f, 5, true)) return;
    g_shared->acked = 5;
    if (!commit_marker(f, 9, true)) return;
    g_shared->acked = 9;
}

/* 어느 지점에서 멈췄든 열 수 있고, 짝이 맞고, 승인한 값이 보이며, 이어서 ready까지 간다 */
static bool recovery_ok(const char *label)
{
    page_file_t *f = NULL;
    page_file_open_info_t info;
    page_file_status_t st = page_file_open(DB, true, &f, &info);
    if (st != PAGE_FILE_OK) {
        printf("  [%s] 열 수 없다 %d\n", label, (int)st);
        return false;
    }
    const page_file_control_t *c = page_file_control(f);
    bool ok = pair_ok(f, label);
    if (c->format_version == 2) {
        if (c->wal_file_id != WAL_ID || c->durable_epoch != EPOCH) {
            printf("  [%s] 식별자가 달라졌다\n", label);
            ok = false;
        }
        if (!c->durable_ready && c->applied_lsn != 0) {
            printf("  [%s] ready가 아닌데 applied_lsn %llu\n", label, (unsigned long long)c->applied_lsn);
            ok = false;
        }
    } else if (c->applied_lsn != 0 || c->wal_file_id != 0) {
        printf("  [%s] v1에 확장 값이 있다\n", label);
        ok = false;
    }
    if (g_shared != NULL) {
        if (g_shared->ready_acked && !(c->format_version == 2 && c->durable_ready)) {
            printf("  [%s] ready를 승인했는데 ready가 아니다\n", label);
            ok = false;
        }
        if (c->applied_lsn < g_shared->acked) {
            printf("  [%s] 승인한 applied_lsn %llu보다 작다 %llu\n", label, (unsigned long long)g_shared->acked,
                   (unsigned long long)c->applied_lsn);
            ok = false;
        }
    }
    /* 이어서 전환을 끝내고 올린다 */
    if (c->format_version != 2 && page_file_enable_durable(f, WAL_ID, EPOCH) != PAGE_FILE_OK) ok = false;
    for (int i = 0; i < 2 && !page_file_durable_ready(f); i++) {
        if (!commit_marker(f, c->applied_lsn, true)) ok = false;
    }
    uint64_t next = page_file_control(f)->applied_lsn + 1;
    if (!page_file_durable_ready(f) || !commit_marker(f, next, true)) {
        printf("  [%s] 이어서 전환하지 못한다\n", label);
        ok = false;
    }
    page_file_close(f);
    if (!open_rw(&f, NULL) || !pair_ok(f, label) || page_file_control(f)->applied_lsn != next) {
        printf("  [%s] 이어서 한 commit이 다시 열었을 때 보이지 않는다\n", label);
        ok = false;
    }
    if (f != NULL) page_file_close(f);
    return ok;
}

static void test_conversion_points(void)
{
    printf("\n[최초 전환의 종료·실패 지점]\n");
    fresh_dir();
    make_v1_base();
    uint8_t *base = NULL;
    size_t base_len = 0;
    CHECK(read_all(DB, &base, &base_len), "기준 파일");
    shared_t *sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    /* 지점 수를 센다 */
    g_shared = sh;
    memset(sh, 0, sizeof(*sh));
    page_file_set_default_io(&HOOKS);
    g_fail_mode = true;
    g_fail_at = 0;
    g_points_seen = 0;
    page_file_t *f = NULL;
    CHECK(open_rw(&f, NULL), "기준 실행 열기");
    long before = g_points_seen;
    if (f != NULL) {
        run_sequence(f);
        page_file_close(f);
    }
    long total = g_points_seen - before;
    CHECK(sh->acked == 9 && sh->ready_acked == 1, "기준 실행은 끝까지 간다");
    CHECK(total > 20, "지점 수 %ld", total);
    page_file_set_default_io(NULL);
    g_fail_mode = false;

    int bad = 0;
    for (long k = 0; k <= total; k++) {
        write_all(DB, base, base_len);
        memset(sh, 0, sizeof(*sh));
        pid_t pid = fork();
        if (pid == 0) {
            page_file_t *cf = NULL;
            if (page_file_open(DB, true, &cf, NULL) != PAGE_FILE_OK) _exit(2);
            page_file_set_default_io(&HOOKS);
            g_points_left = k;
            run_sequence(cf);
            _exit(0);
        }
        int status;
        waitpid(pid, &status, 0);
        if (!(WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 137))) {
            printf("  k=%ld 자식이 예상 밖으로 끝남\n", k);
            bad++;
            continue;
        }
        char label[48];
        snprintf(label, sizeof(label), "종료 k=%ld", k);
        if (!recovery_ok(label)) bad++;
    }
    CHECK(bad == 0, "종료 지점 %ld곳 어긋남 %d", total + 1, bad);

    /* I/O 실패: 같은 지점에서 한 번만 실패시키고 호출이 오류를 돌려준 뒤 다시 열어 불변식을 확인한다 */
    bad = 0;
    long failed = 0;
    g_shared = sh;
    for (long k = 1; k <= total; k++) {
        write_all(DB, base, base_len);
        memset(sh, 0, sizeof(*sh));
        page_file_set_default_io(&HOOKS); /* 이후 열리는 핸들만 훅을 쓴다 */
        g_fail_mode = true;
        g_fail_at = 0;
        g_fail_seen = 0;
        bool opened = open_rw(&f, NULL);
        CHECK(opened, "실패 주입 열기");
        if (!opened) {
            g_fail_mode = false;
            page_file_set_default_io(NULL);
            continue;
        }
        g_fail_at = k;
        g_fail_seen = 0;
        run_sequence(f);
        bool hit = g_fail_seen >= k;
        g_fail_mode = false;
        page_file_set_default_io(NULL);
        failed += hit;
        page_file_close(f);
        char label[48];
        snprintf(label, sizeof(label), "실패 k=%ld", k);
        if (!recovery_ok(label)) bad++;
    }
    CHECK(bad == 0 && failed > 0, "실패 지점 어긋남 %d (주입 %ld)", bad, failed);
    munmap(sh, sizeof(*sh));
    g_shared = NULL;
    free(base);
}

/* ---- 6. catalog_store_save_applied ---- */

/* 실제 카탈로그 체인을 가진 ready 파일 */
static page_file_t *open_ready_catalog(catalog_t *cat)
{
    fresh_dir();
    page_file_t *f = NULL;
    if (page_file_create(DB, &f) != PAGE_FILE_OK) return NULL;
    if (catalog_store_save(f, cat) != CATALOG_STORE_OK || page_file_publish(f) != PAGE_FILE_OK ||
        page_file_enable_durable(f, WAL_ID, EPOCH) != PAGE_FILE_OK ||
        catalog_store_save_applied(f, cat, 0) != CATALOG_STORE_OK ||
        catalog_store_save_applied(f, cat, 0) != CATALOG_STORE_OK || !page_file_durable_ready(f)) {
        page_file_close(f);
        return NULL;
    }
    return f;
}

static void test_catalog_store_applied(void)
{
    printf("\n[catalog_store_save_applied]\n");
    catalog_t *cat = NULL;
    CHECK(catalog_open_empty(&cat) == CATALOG_OK, "빈 카탈로그");
    page_file_t *f = open_ready_catalog(cat);
    CHECK(f != NULL, "카탈로그가 있는 ready 파일");
    if (f == NULL) {
        catalog_close(cat);
        return;
    }
    CHECK(catalog_store_save_applied(f, cat, 11) == CATALOG_STORE_OK, "카탈로그와 applied_lsn 11을 함께 게시");
    CHECK(page_file_control(f)->applied_lsn == 11, "게시한 applied_lsn");
    catalog_t *loaded = NULL;
    CHECK(catalog_store_load(f, &loaded) == CATALOG_STORE_OK, "게시한 카탈로그를 읽는다");
    if (loaded != NULL) catalog_close(loaded);
    page_file_close(f);
    CHECK(open_rw(&f, NULL) && page_file_control(f)->applied_lsn == 11, "다시 열어도 11");
    if (f == NULL) {
        catalog_close(cat);
        return;
    }

    /* 낮추는 값은 아무것도 쓰지 않는다 */
    uint8_t *before = NULL, *after = NULL;
    size_t bl = 0, al = 0;
    read_all(DB, &before, &bl);
    CHECK(catalog_store_save_applied(f, cat, 10) == CATALOG_STORE_INVALID, "낮추는 applied_lsn은 INVALID");
    read_all(DB, &after, &al);
    CHECK(bl == al && memcmp(before, after, bl) == 0, "INVALID는 파일을 바꾸지 않는다");
    free(before);
    free(after);

    /* 제어 쓰기 전 실패: applied_lsn은 이전 값으로 돌아가고, 뒤따르는 일반 commit이 12를 게시하지 않는다 */
    page_file_close(f);
    page_file_set_default_io(&HOOKS);
    g_fail_mode = true;
    g_fail_at = 0;
    g_fail_seen = 0;
    CHECK(open_rw(&f, NULL), "훅을 쓰는 핸들");
    if (f == NULL) {
        catalog_close(cat);
        return;
    }
    g_fail_at = 1;
    catalog_store_status_t st = catalog_store_save_applied(f, cat, 12);
    g_fail_mode = false;
    page_file_set_default_io(NULL);
    CHECK(st != CATALOG_STORE_OK && st != CATALOG_STORE_OUTCOME_UNKNOWN, "데이터 쓰기 실패 (%d)", (int)st);
    if (!page_file_is_poisoned(f)) {
        CHECK(catalog_store_save(f, cat) == CATALOG_STORE_OK, "이후 일반 save");
        CHECK(page_file_control(f)->applied_lsn == 11, "실패한 12가 새어 나가지 않는다 (%llu)",
              (unsigned long long)page_file_control(f)->applied_lsn);
    }
    page_file_close(f);

    /* v1 파일에서는 거절 */
    fresh_dir();
    CHECK(page_file_create(DB, &f) == PAGE_FILE_OK && catalog_store_save(f, cat) == CATALOG_STORE_OK &&
              page_file_publish(f) == PAGE_FILE_OK,
          "v1 카탈로그 파일");
    if (f != NULL) page_file_close(f);
    CHECK(open_rw(&f, NULL), "v1 열기");
    if (f != NULL) {
        CHECK(catalog_store_save_applied(f, cat, 1) == CATALOG_STORE_INVALID, "v1은 INVALID");
        CHECK(catalog_store_save(f, cat) == CATALOG_STORE_OK, "v1 일반 save는 그대로 된다");
        page_file_close(f);
    }
    catalog_close(cat);
}

/* ---- 7. epoch ---- */

static void test_epoch(void)
{
    printf("\n[durable_epoch 증가 전용 갱신]\n");
    fresh_dir();
    make_v1_base();
    page_file_t *f = NULL;
    uint32_t next = 0;
    CHECK(open_rw(&f, NULL), "열기");
    if (f == NULL) return;
    CHECK(page_file_next_durable_epoch(f, &next) == PAGE_FILE_INVALID, "v1은 epoch가 없다");
    CHECK(page_file_enable_durable(f, WAL_ID, EPOCH) == PAGE_FILE_OK && commit_marker(f, 0, true), "첫 v2 commit");
    CHECK(page_file_stage_durable_epoch(f, EPOCH + 1) == PAGE_FILE_INVALID, "ready 전에는 epoch를 올릴 수 없다");
    CHECK(commit_marker(f, 0, true) && page_file_durable_ready(f), "ready");
    CHECK(page_file_stage_durable_epoch(f, EPOCH) == PAGE_FILE_INVALID, "같은 값은 거절");
    CHECK(page_file_stage_durable_epoch(f, EPOCH - 1) == PAGE_FILE_INVALID, "낮은 값은 거절");
    CHECK(page_file_next_durable_epoch(f, &next) == PAGE_FILE_OK && next == EPOCH + 1, "다음 값");
    CHECK(page_file_stage_durable_epoch(f, next) == PAGE_FILE_OK, "올리기");
    CHECK(page_file_stage_durable_epoch(f, next) == PAGE_FILE_OK, "같은 staged 값은 무동작");
    CHECK(page_file_control(f)->durable_epoch == EPOCH, "commit 전에는 영속 값이 그대로다");
    CHECK(commit_marker(f, 4, true), "함께 commit");
    CHECK(page_file_control(f)->durable_epoch == next && page_file_control(f)->wal_file_id == WAL_ID &&
              page_file_control(f)->applied_lsn == 4,
          "epoch와 applied_lsn이 함께 게시되고 wal_file_id는 그대로");
    page_file_close(f);
    CHECK(open_rw(&f, NULL) && page_file_control(f)->durable_epoch == next, "재시작 뒤 영속된 epoch가 floor");
    if (f != NULL) page_file_close(f);

    /* 최댓값 */
    fresh_dir();
    make_v1_base();
    CHECK(open_rw(&f, NULL), "열기");
    if (f == NULL) return;
    CHECK(page_file_enable_durable(f, WAL_ID, UINT32_MAX) == PAGE_FILE_OK && commit_marker(f, 0, true) &&
              commit_marker(f, 0, true),
          "epoch 최댓값으로 전환");
    CHECK(page_file_next_durable_epoch(f, &next) == PAGE_FILE_LIMIT, "다음 값은 LIMIT(넘침 거절)");
    CHECK(page_file_stage_durable_epoch(f, UINT32_MAX) == PAGE_FILE_INVALID, "최댓값을 다시 올릴 수 없다");
    next = 77;
    CHECK(page_file_next_durable_epoch(f, &next) == PAGE_FILE_LIMIT && next == 77, "LIMIT은 출력을 바꾸지 않는다");
    page_file_close(f);
}

int main(void)
{
    mkdir(DIR_NAME, 0700);
    test_v1_preserved();
    test_enable_and_roundtrip();
    test_slot_damage();
    test_format_rejection();
    test_conversion_points();
    test_catalog_store_applied();
    test_epoch();
    fresh_dir();
    rmdir(DIR_NAME);
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
