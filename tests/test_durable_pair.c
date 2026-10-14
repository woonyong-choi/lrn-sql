/*
 * test_durable_pair.c -- 데이터 파일과 WAL의 짝, 최초 durable 전환의 중단 복구 검사 (#8)
 *
 * 공개 API(durable_pair, page_file, wal)와 실제 파일만 쓴다. 합성 데이터다.
 *  1. 전환 순서(WAL 생성, v2 첫 commit, v2 ready commit)와 재열기
 *  2. 전환 중 모든 종료 지점과 I/O 실패 지점: 열 수 있고(복구 또는 NEEDS_CONVERSION), 이어서 전환이 끝나며,
 *     그 뒤 승인한 거래가 재시작 뒤에도 보인다
 *  3. 승인된 durable 거래를 조용히 durable이 아닌 것으로 여는 길이 없다(DOWNGRADED), 슬롯 손상 뒤 판정
 *  4. 뒤늦은 WAL 누락과 불일치는 복구가 아니라 오류이며 어느 파일도 바꾸지 않는다
 *  5. epoch 영속화, 예산(admission, WAL 한도) 거절은 파일을 만들지도 바꾸지도 않는다
 *  6. epoch는 두 슬롯에 commit한 뒤에만 노출한다(종료·I/O 실패 지점과 슬롯 손상 뒤 floor), 손상 v1은 PLAIN으로 열지 않는다
 *
 * 다루지 않는 것: 전원 장애, lost write, 실제 database·catalog·heap의 replay.
 *
 * 빌드(저장소 루트에서, build/storage 객체 사용):
 *   cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=undefined -Iinclude -o build/test_durable_pair \
 *      tests/test_durable_pair.c src/storage/durable_pair.c build/storage/{wal,page_file}.o
 * 실행은 저장소 루트에서 한다.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "storage/durable_pair.h"

#define DIR_NAME "__test__durable_pair"
#define DB DIR_NAME "/db.lrdb"
#define WAL DIR_NAME "/db.wal"
#define EPOCH 5u

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

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
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

typedef struct {
    uint8_t *db, *wal;
    size_t db_len, wal_len;
    bool has_wal;
} snap_t;

static void snap_take(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    read_all(DB, &s->db, &s->db_len);
    s->has_wal = read_all(WAL, &s->wal, &s->wal_len);
}

static void snap_restore(const snap_t *s)
{
    write_all(DB, s->db, s->db_len);
    if (s->has_wal) write_all(WAL, s->wal, s->wal_len);
    else unlink(WAL);
}

static bool snap_same(const snap_t *s)
{
    uint8_t *db = NULL, *wal = NULL;
    size_t dl = 0, wl = 0;
    bool ok = read_all(DB, &db, &dl) && dl == s->db_len && memcmp(db, s->db, dl) == 0;
    bool has = read_all(WAL, &wal, &wl);
    if (has != s->has_wal) ok = false;
    else if (has) ok = ok && wl == s->wal_len && memcmp(wal, s->wal, wl) == 0;
    free(db);
    free(wal);
    return ok;
}

static void snap_free(snap_t *s)
{
    free(s->db);
    free(s->wal);
}

static void damage_slot(int slot)
{
    uint8_t b;
    int fd = open(DB, O_RDWR);
    off_t off = (off_t)slot * PAGE_FILE_PAGE_BYTES + 200;
    if (fd >= 0 && pread(fd, &b, 1, off) == 1) {
        b ^= 0x5A;
        if (pwrite(fd, &b, 1, off) != 1) fprintf(stderr, "damage 실패\n");
    }
    if (fd >= 0) close(fd);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* 슬롯의 ready flag. v1 슬롯이면 -1 */
static int slot_ready_flag(int slot)
{
    uint8_t *all = NULL;
    size_t len = 0;
    int result = -1;
    if (read_all(DB, &all, &len) && len >= 2 * PAGE_FILE_PAGE_BYTES) {
        const uint8_t *p = all + (size_t)slot * PAGE_FILE_PAGE_BYTES;
        if (le32(p + 16) == 76) result = (int)le32(p + 32 + 72);
    }
    free(all);
    return result;
}

/* 새 v1 데이터 파일(commit 한 번, 게시됨) */
static void make_v1_db(void)
{
    page_file_t *f = NULL;
    CHECK(page_file_create(DB, &f) == PAGE_FILE_OK, "v1 생성");
    if (f == NULL) return;
    CHECK(page_file_commit(f) == PAGE_FILE_OK, "v1 commit");
    CHECK(page_file_publish(f) == PAGE_FILE_OK, "게시");
    page_file_close(f);
}

static durable_pair_options_t opts(bool convert)
{
    durable_pair_options_t o = {0};
    o.convert = convert;
    o.durable_epoch = EPOCH;
    return o;
}

static durable_pair_status_t open_pair(bool convert, durable_pair_t *pair, durable_pair_info_t *info)
{
    durable_pair_options_t o = opts(convert);
    return durable_pair_open(DB, WAL, &o, pair, info);
}

/* 거래 하나를 승인한다: DATA와 COMMIT을 fsync하고 applied_lsn 게시는 publish가 true일 때만 한다 */
static bool approve(durable_pair_t *pair, wal_xid_t xid, bool publish, wal_lsn_t *commit_lsn)
{
    wal_lsn_t l = 0, cl = 0;
    if (wal_append(pair->wal, xid, 0, "p", 1, &l) != WAL_OK) return false;
    if (wal_commit(pair->wal, xid, &cl) != WAL_OK) return false;
    if (commit_lsn != NULL) *commit_lsn = cl;
    if (!publish) return true;
    if (page_file_stage_applied_lsn(pair->file, cl) != PAGE_FILE_OK) return false;
    return page_file_commit(pair->file) == PAGE_FILE_OK;
}

static bool is_ready_pair(const durable_pair_t *pair)
{
    const page_file_control_t *c = page_file_control(pair->file);
    return pair->wal != NULL && c->format_version == 2 && c->durable_ready &&
           c->wal_file_id == wal_file_id(pair->wal) && c->durable_epoch == EPOCH;
}

/* ---- 1. 전환 순서와 재열기 ---- */

static void test_conversion_basic(void)
{
    printf("\n[최초 전환과 재열기]\n");
    fresh_dir();
    make_v1_db();
    durable_pair_t pair;
    durable_pair_info_t info;
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_OK, "v1과 WAL 없음은 convert 없이 PLAIN으로 열린다");
    CHECK(pair.wal == NULL && !info.converted && !exists(WAL), "PLAIN은 WAL을 만들지 않는다");
    durable_pair_close(&pair);

    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_OK, "convert로 전환");
    CHECK(info.converted && !info.resumed, "새 전환이다");
    CHECK(is_ready_pair(&pair), "ready이고 식별자가 WAL과 같다");
    CHECK(wal_file_id(pair.wal) != 0, "WAL file_id가 있다");
    wal_lsn_t cl = 0;
    CHECK(approve(&pair, 1, true, &cl), "첫 거래 승인과 applied_lsn 게시");
    CHECK(page_file_control(pair.file)->applied_lsn == cl, "applied_lsn은 COMMIT LSN");
    CHECK(durable_pair_close(&pair) == DURABLE_PAIR_OK, "닫기");

    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_OK, "재열기");
    CHECK(!info.converted && is_ready_pair(&pair), "그대로 ready");
    CHECK(page_file_control(pair.file)->applied_lsn == cl && wal_xid_state(pair.wal, 1) == WAL_XID_COMMITTED,
          "applied_lsn과 승인한 거래가 재시작 뒤에도 있다");
    CHECK(info.wal_info.records == 2, "WAL 기록 2 (DATA, COMMIT)");
    durable_pair_close(&pair);
}

/* ---- 2. 전환 중 종료·실패 지점 ---- */

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

static int h_ftruncate(int fd, off_t len)
{
    if (point()) {
        errno = EIO;
        return -1;
    }
    int r = ftruncate(fd, len);
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

static void hooks_on(void)
{
    page_file_io_t pio = {pread, h_pwrite, h_fsync, h_link, h_unlink};
    page_file_set_default_io(&pio);
    wal_io_t wio = {pread, h_pwrite, h_fsync, h_ftruncate};
    wal_set_default_io(&wio);
}

static void hooks_off(void)
{
    page_file_set_default_io(NULL);
    wal_set_default_io(NULL);
}

/* 어느 지점에서 멈췄든: 열린다(복구 또는 NEEDS_CONVERSION), 이어서 전환이 끝나고, 승인한 거래가 재시작 뒤에도 있다 */
static bool recover_conversion(const char *label, bool completed)
{
    durable_pair_t pair;
    durable_pair_info_t info;
    durable_pair_status_t st = open_pair(false, &pair, &info);
    bool ok = true;
    if (st == DURABLE_PAIR_OK) {
        if (completed && !is_ready_pair(&pair)) {
            printf("  [%s] 전환이 끝났다고 응답했는데 ready가 아니다\n", label);
            ok = false;
        }
        durable_pair_close(&pair);
    } else if (st != DURABLE_PAIR_NEEDS_CONVERSION) {
        printf("  [%s] 허용되지 않는 상태 %s\n", label, durable_pair_status_name(st));
        return false;
    } else if (completed) {
        printf("  [%s] 전환이 끝났다고 응답했는데 NEEDS_CONVERSION\n", label);
        ok = false;
    }
    st = open_pair(true, &pair, &info);
    if (st != DURABLE_PAIR_OK || !is_ready_pair(&pair)) {
        printf("  [%s] 이어서 전환하지 못한다 %s\n", label, durable_pair_status_name(st));
        if (st == DURABLE_PAIR_OK) durable_pair_close(&pair);
        return false;
    }
    wal_lsn_t cl = 0;
    wal_xid_t xid = wal_max_xid(pair.wal) + 1;
    if (!approve(&pair, xid, true, &cl)) {
        printf("  [%s] 전환 뒤 거래를 승인하지 못한다\n", label);
        ok = false;
    }
    durable_pair_close(&pair);
    st = open_pair(false, &pair, &info);
    if (st != DURABLE_PAIR_OK || !is_ready_pair(&pair) || wal_xid_state(pair.wal, xid) != WAL_XID_COMMITTED ||
        page_file_control(pair.file)->applied_lsn != cl) {
        printf("  [%s] 승인한 거래가 재시작 뒤에 보이지 않는다 %s\n", label, durable_pair_status_name(st));
        ok = false;
    }
    if (st == DURABLE_PAIR_OK) durable_pair_close(&pair);
    return ok;
}

static void test_conversion_points(void)
{
    printf("\n[최초 전환의 종료·실패 지점]\n");
    fresh_dir();
    make_v1_db();
    snap_t base;
    snap_take(&base);

    /* 지점 수 */
    hooks_on();
    g_fail_mode = true;
    g_fail_at = 0;
    g_points_seen = 0;
    durable_pair_t pair;
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK, "기준 전환");
    long total = g_points_seen;
    durable_pair_close(&pair);
    hooks_off();
    g_fail_mode = false;
    CHECK(total > 10, "지점 수 %ld", total);

    int bad = 0, crashed = 0;
    for (long k = 0; k <= total; k++) {
        snap_restore(&base);
        pid_t pid = fork();
        if (pid == 0) {
            hooks_on();
            g_points_left = k;
            durable_pair_t p;
            durable_pair_status_t st = open_pair(true, &p, NULL);
            _exit(st == DURABLE_PAIR_OK ? 0 : 3);
        }
        int status;
        waitpid(pid, &status, 0);
        bool died = WIFEXITED(status) && WEXITSTATUS(status) == 137;
        bool done = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!died && !done) {
            printf("  k=%ld 자식이 예상 밖으로 끝남 status=%d\n", k, status);
            bad++;
            continue;
        }
        crashed += died;
        char label[48];
        snprintf(label, sizeof(label), "종료 k=%ld", k);
        if (!recover_conversion(label, done)) bad++;
    }
    CHECK(bad == 0 && crashed > 0, "종료 지점 %ld곳 어긋남 %d (비정상 종료 %d)", total + 1, bad, crashed);

    bad = 0;
    long failed = 0;
    for (long k = 1; k <= total; k++) {
        snap_restore(&base);
        hooks_on();
        g_fail_mode = true;
        g_fail_at = k;
        g_fail_seen = 0;
        durable_pair_status_t st = open_pair(true, &pair, NULL);
        bool hit = g_fail_seen >= k;
        g_fail_mode = false;
        hooks_off();
        failed += hit;
        if (st == DURABLE_PAIR_OK) durable_pair_close(&pair);
        char label[48];
        snprintf(label, sizeof(label), "실패 k=%ld", k);
        if (!recover_conversion(label, st == DURABLE_PAIR_OK)) bad++;
    }
    CHECK(bad == 0 && failed > 0, "실패 지점 어긋남 %d (주입 %ld)", bad, failed);
    snap_free(&base);
}

/* ---- 3. silent downgrade 금지, 슬롯 손상 ---- */

static void test_downgrade_paths(void)
{
    printf("\n[승인된 거래의 silent downgrade 금지]\n");
    durable_pair_t pair;
    durable_pair_info_t info;
    wal_lsn_t cl1 = 0, cl2 = 0;

    /* a) 전환 전 v1 파일을 되돌려 놓음(v2 슬롯이 모두 사라진 fallback): WAL에 승인 기록이 있다 */
    fresh_dir();
    make_v1_db();
    snap_t v1;
    snap_take(&v1);
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK && approve(&pair, 1, true, &cl1), "전환과 승인");
    durable_pair_close(&pair);
    snap_t with_records;
    snap_take(&with_records);
    write_all(DB, v1.db, v1.db_len);
    snap_t restored;
    snap_take(&restored);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_DOWNGRADED, "v1 + 기록 있는 WAL은 DOWNGRADED (convert 없음)");
    CHECK(snap_same(&restored), "오류는 어느 파일도 바꾸지 않는다");
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_DOWNGRADED, "convert여도 DOWNGRADED");
    CHECK(snap_same(&restored) && pair.file == NULL && pair.wal == NULL, "여전히 바뀌지 않고 핸들도 없다");
    snap_free(&restored);

    /* b) ready 직후 승인했지만 게시 전에 멈춤 + 최신 슬롯(ready) 손상 -> 살아남은 슬롯은 ready가 아닌 첫 v2 */
    snap_restore(&with_records);
    fresh_dir();
    make_v1_db();
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK && approve(&pair, 1, false, &cl1), "승인했으나 게시 전");
    durable_pair_close(&pair);
    int ready_slot = slot_ready_flag(1) == 1 ? 1 : 0;
    CHECK(slot_ready_flag(ready_slot) == 1 && slot_ready_flag(1 - ready_slot) == 0, "슬롯 배치 확인");
    damage_slot(ready_slot);
    snap_t damaged;
    snap_take(&damaged);
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_DOWNGRADED, "ready 슬롯 손상과 승인 기록은 DOWNGRADED");
    CHECK(info.file_info.damaged_slots == 1 && snap_same(&damaged), "손상 1을 알리고 파일은 그대로");
    snap_free(&damaged);

    /* c) 게시한 뒤 최신 슬롯 손상: 살아남은 슬롯은 ready라 열리고 replay가 필요한 상태다 */
    fresh_dir();
    make_v1_db();
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK && approve(&pair, 1, true, &cl1) &&
              approve(&pair, 2, true, &cl2),
          "두 거래를 게시");
    durable_pair_close(&pair);
    uint8_t *all = NULL;
    size_t len = 0;
    read_all(DB, &all, &len);
    uint64_t best = 0;
    int newest = 0;
    for (int slot = 0; slot < 2; slot++) {
        uint64_t gen = 0;
        for (int i = 7; i >= 0; i--) gen = (gen << 8) | all[(size_t)slot * PAGE_FILE_PAGE_BYTES + 32 + 20 + i];
        if (gen > best) {
            best = gen;
            newest = slot;
        }
    }
    free(all);
    damage_slot(newest);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_OK, "게시 뒤 최신 슬롯 손상은 열린다");
    if (pair.file != NULL) {
        const page_file_control_t *c = page_file_control(pair.file);
        CHECK(is_ready_pair(&pair) && info.file_info.damaged_slots == 1, "살아남은 슬롯은 ready이고 손상 1을 알린다");
        CHECK(c->applied_lsn == cl1 && c->applied_lsn < cl2, "applied_lsn이 WAL보다 뒤라 replay 대상이 남는다");
        CHECK(wal_xid_state(pair.wal, 2) == WAL_XID_COMMITTED, "승인한 거래는 WAL에 있다");
        durable_pair_close(&pair);
    }

    /* d) 두 슬롯 모두 손상 */
    damage_slot(1 - newest);
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_CORRUPT, "두 슬롯 손상은 CORRUPT");
    snap_free(&v1);
    snap_free(&with_records);
}

/* ---- 3b. 복구할 수 있는 미완료 전환 ---- */

static void test_recoverable_states(void)
{
    printf("\n[승인 전 중단된 전환]\n");
    durable_pair_t pair;
    durable_pair_info_t info;

    /* A만 끝남: v1 + 빈 WAL */
    fresh_dir();
    make_v1_db();
    wal_t *w = NULL;
    CHECK(wal_create(WAL, &w) == WAL_OK, "WAL 생성");
    uint64_t id_a = wal_file_id(w);
    wal_close(w);
    snap_t a;
    snap_take(&a);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_NEEDS_CONVERSION, "convert 없이는 NEEDS_CONVERSION");
    CHECK(snap_same(&a), "아무것도 바꾸지 않는다");
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_OK, "convert가 이어서 끝낸다");
    CHECK(info.converted && info.resumed && is_ready_pair(&pair), "중단된 전환을 이어서 끝냈다");
    CHECK(wal_file_id(pair.wal) == id_a && page_file_control(pair.file)->wal_file_id == id_a,
          "만들어 둔 WAL을 그대로 쓴다");
    durable_pair_close(&pair);
    snap_free(&a);

    /* B까지 끝남: v2(ready 아님) + 빈 WAL */
    fresh_dir();
    make_v1_db();
    CHECK(wal_create(WAL, &w) == WAL_OK, "WAL 생성");
    uint64_t id_b = wal_file_id(w);
    wal_close(w);
    page_file_t *f = NULL;
    CHECK(page_file_open(DB, true, &f, NULL) == PAGE_FILE_OK && page_file_enable_durable(f, id_b, EPOCH) == PAGE_FILE_OK &&
              page_file_commit(f) == PAGE_FILE_OK,
          "v2 첫 commit만");
    if (f != NULL) page_file_close(f);
    snap_t b;
    snap_take(&b);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_NEEDS_CONVERSION, "v2 ready 아님 + 빈 WAL은 NEEDS_CONVERSION");
    CHECK(snap_same(&b), "바뀌지 않는다");
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_OK && info.resumed && is_ready_pair(&pair), "이어서 끝낸다");
    durable_pair_close(&pair);
    snap_free(&b);

    /* WAL을 만들다 멈춘 헤더 접두 파일(magic 일부)도 기록 0으로 보고 이어간다 */
    fresh_dir();
    make_v1_db();
    write_all(WAL, (const uint8_t *)"LRWAL", 5);
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_OK && is_ready_pair(&pair), "헤더 접두 WAL에서 전환");
    if (pair.file != NULL) durable_pair_close(&pair);
}

/* ---- 4. 뒤늦은 누락과 불일치 ---- */

static void test_late_missing_and_mismatch(void)
{
    printf("\n[WAL 누락과 불일치]\n");
    durable_pair_t pair;
    durable_pair_info_t info;
    wal_lsn_t cl = 0;
    fresh_dir();
    make_v1_db();
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK && approve(&pair, 1, true, &cl), "전환과 승인");
    durable_pair_close(&pair);
    snap_t ok;
    snap_take(&ok);

    unlink(WAL);
    snap_t no_wal;
    snap_take(&no_wal);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_WAL_MISSING, "WAL이 없으면 WAL_MISSING");
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_WAL_MISSING, "convert여도 WAL_MISSING");
    CHECK(snap_same(&no_wal) && !exists(WAL), "WAL을 새로 만들지 않고 데이터 파일도 그대로");
    snap_free(&no_wal);

    wal_t *other = NULL;
    CHECK(wal_create(WAL, &other) == WAL_OK, "같은 경로에 다른 WAL");
    if (other != NULL) wal_close(other);
    snap_t swapped;
    snap_take(&swapped);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_MISMATCH, "다른 WAL은 MISMATCH");
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_MISMATCH, "convert여도 MISMATCH");
    CHECK(snap_same(&swapped), "파일이 바뀌지 않는다");
    snap_free(&swapped);

    snap_restore(&ok);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_OK && is_ready_pair(&pair), "원래 짝으로 돌리면 정상");
    if (pair.file != NULL) durable_pair_close(&pair);
    snap_free(&ok);
}

/* ---- 5. epoch, 예산 ---- */

static bool admit_none(void *ctx, uint64_t bytes)
{
    (void)ctx;
    (void)bytes;
    return false;
}

static void test_epoch_and_budgets(void)
{
    printf("\n[epoch 영속화와 예산 거절]\n");
    durable_pair_t pair;
    durable_pair_info_t info;
    fresh_dir();
    make_v1_db();
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK, "전환");
    uint64_t id = page_file_control(pair.file)->wal_file_id;
    CHECK(durable_pair_persist_epoch(&pair, EPOCH) == DURABLE_PAIR_INVALID, "같은 epoch는 거절");
    CHECK(durable_pair_persist_epoch(&pair, EPOCH - 1) == DURABLE_PAIR_INVALID, "낮은 epoch는 거절");
    CHECK(durable_pair_persist_epoch(&pair, EPOCH + 1) == DURABLE_PAIR_OK, "epoch 영속화");
    durable_pair_close(&pair);
    durable_pair_options_t o = opts(false);
    o.durable_epoch = 99; /* 전환 때만 쓰는 값이라 열기에 영향이 없다 */
    CHECK(durable_pair_open(DB, WAL, &o, &pair, &info) == DURABLE_PAIR_OK, "재열기");
    if (pair.file != NULL) {
        CHECK(page_file_control(pair.file)->durable_epoch == EPOCH + 1 && page_file_control(pair.file)->wal_file_id == id,
              "재시작 뒤 영속된 epoch가 floor이고 wal_file_id는 그대로");
        durable_pair_close(&pair);
    }

    /* admission 거절: 아무것도 만들지 않는다 */
    fresh_dir();
    make_v1_db();
    snap_t before;
    snap_take(&before);
    o = opts(true);
    o.admit = admit_none;
    CHECK(durable_pair_open(DB, WAL, &o, &pair, &info) == DURABLE_PAIR_NOMEM, "데이터 파일 admission 거절은 NOMEM");
    CHECK(snap_same(&before) && !exists(WAL), "파일을 바꾸지도 WAL을 만들지도 않았다");

    /* WAL 예산 거절: 전환 시작 시 WAL을 만들기 전에 거절한다 */
    wal_limits_t tiny = {1};
    o = opts(true);
    o.wal_limits = &tiny;
    CHECK(durable_pair_open(DB, WAL, &o, &pair, &info) == DURABLE_PAIR_LIMIT, "WAL 예산 거절은 LIMIT");
    CHECK(snap_same(&before) && !exists(WAL), "파일을 바꾸지도 WAL을 만들지도 않았다(전환 시작)");
    snap_free(&before);

    /* 기존 짝에 대한 WAL 예산 거절도 바꾸지 않는다 */
    fresh_dir();
    make_v1_db();
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK, "전환");
    durable_pair_close(&pair);
    snap_take(&before);
    o = opts(false);
    o.wal_limits = &tiny;
    CHECK(durable_pair_open(DB, WAL, &o, &pair, &info) == DURABLE_PAIR_LIMIT, "기존 짝의 WAL 예산 거절");
    CHECK(snap_same(&before), "바뀌지 않는다");
    snap_free(&before);
}

/* ---- 6. epoch 이중 슬롯, v1 손상 거절 ---- */

/* 가장 높은 generation의 제어 슬롯 */
static int newest_slot(void)
{
    uint8_t *all = NULL;
    size_t len = 0;
    uint64_t best = 0;
    int newest = 0;
    if (read_all(DB, &all, &len) && len >= 2 * PAGE_FILE_PAGE_BYTES) {
        for (int slot = 0; slot < 2; slot++) {
            uint64_t gen = 0;
            for (int i = 7; i >= 0; i--) gen = (gen << 8) | all[(size_t)slot * PAGE_FILE_PAGE_BYTES + 32 + 20 + i];
            if (gen > best) {
                best = gen;
                newest = slot;
            }
        }
    }
    free(all);
    return newest;
}

/* 노출한 epoch를 어느 한 슬롯의 손상 뒤에도 floor가 넘는지: 손상 없음, 최신 슬롯 손상, 이전 슬롯 손상 */
static bool floor_survives(const char *label, uint32_t exposed)
{
    snap_t s;
    snap_take(&s);
    int newest = newest_slot();
    bool ok = true;
    for (int variant = 0; variant < 3; variant++) {
        snap_restore(&s);
        if (variant == 1) damage_slot(newest);
        if (variant == 2) damage_slot(1 - newest);
        durable_pair_t pair;
        durable_pair_info_t info;
        durable_pair_status_t st = open_pair(false, &pair, &info);
        if (st != DURABLE_PAIR_OK) {
            printf("  [%s] 변형 %d: 열리지 않는다 %s\n", label, variant, durable_pair_status_name(st));
            ok = false;
            continue;
        }
        uint32_t floor_epoch = page_file_control(pair.file)->durable_epoch;
        if (floor_epoch < EPOCH || (exposed != 0 && floor_epoch < exposed)) {
            printf("  [%s] 변형 %d: floor %u < 노출 %u\n", label, variant, floor_epoch, exposed);
            ok = false;
        }
        durable_pair_close(&pair);
    }
    snap_free(&s);
    return ok;
}

/* 전환을 마치고 거래 하나를 게시해 두 제어 슬롯이 모두 ready인 짝 */
static void make_two_ready_slots(void)
{
    durable_pair_t pair;
    fresh_dir();
    make_v1_db();
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_OK && approve(&pair, 1, true, NULL), "전환과 게시");
    durable_pair_close(&pair);
    CHECK(slot_ready_flag(0) == 1 && slot_ready_flag(1) == 1, "두 슬롯이 모두 ready");
}

static void test_epoch_two_slots(void)
{
    printf("\n[epoch 이중 슬롯 영속화]\n");
    durable_pair_t pair;

    /* 성공: 두 슬롯 모두 같은 epoch를 담는다 */
    make_two_ready_slots();
    CHECK(open_pair(false, &pair, NULL) == DURABLE_PAIR_OK, "열기");
    CHECK(durable_pair_persist_epoch(&pair, EPOCH + 1) == DURABLE_PAIR_OK, "epoch 영속화");
    durable_pair_close(&pair);
    CHECK(floor_survives("성공", EPOCH + 1), "한 슬롯이 손상돼도 floor가 노출 epoch 이상");

    /* 지점 수 */
    make_two_ready_slots();
    snap_t base;
    snap_take(&base);
    hooks_on();
    CHECK(open_pair(false, &pair, NULL) == DURABLE_PAIR_OK, "열기");
    g_fail_mode = true;
    g_fail_at = 0;
    g_points_seen = 0;
    CHECK(durable_pair_persist_epoch(&pair, EPOCH + 1) == DURABLE_PAIR_OK, "기준 영속화");
    long total = g_points_seen;
    durable_pair_close(&pair);
    hooks_off();
    g_fail_mode = false;
    CHECK(total >= 4, "지점 수 %ld", total);

    /* 종료: 자식이 OK를 돌려받기 전까지 노출하지 않는다. OK(0)로 끝난 자식만 노출한 것이다 */
    int bad = 0, crashed = 0;
    for (long k = 0; k <= total; k++) {
        snap_restore(&base);
        pid_t pid = fork();
        if (pid == 0) {
            durable_pair_t p;
            hooks_on();
            if (open_pair(false, &p, NULL) != DURABLE_PAIR_OK) _exit(4);
            g_points_left = k;
            durable_pair_status_t st = durable_pair_persist_epoch(&p, EPOCH + 1);
            _exit(st == DURABLE_PAIR_OK ? 0 : 3);
        }
        int status;
        waitpid(pid, &status, 0);
        bool died = WIFEXITED(status) && WEXITSTATUS(status) == 137;
        bool done = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!died && !done) {
            printf("  k=%ld 자식이 예상 밖으로 끝남 status=%d\n", k, status);
            bad++;
            continue;
        }
        crashed += died;
        char label[48];
        snprintf(label, sizeof(label), "종료 k=%ld", k);
        if (!floor_survives(label, done ? EPOCH + 1 : 0)) bad++;
    }
    CHECK(bad == 0 && crashed > 0, "종료 지점 %ld곳 어긋남 %d (비정상 종료 %d)", total + 1, bad, crashed);

    /* I/O 실패: OK가 아니면 노출하지 않고, 같은 핸들은 새 epoch를 내주지 않는다(재열기뿐) */
    bad = 0;
    long failed = 0;
    for (long k = 1; k <= total; k++) {
        snap_restore(&base);
        hooks_on();
        CHECK(open_pair(false, &pair, NULL) == DURABLE_PAIR_OK, "열기");
        g_fail_mode = true;
        g_fail_at = k;
        g_fail_seen = 0;
        durable_pair_status_t st = durable_pair_persist_epoch(&pair, EPOCH + 1);
        failed += g_fail_seen >= k;
        g_fail_mode = false;
        hooks_off();
        char label[48];
        snprintf(label, sizeof(label), "실패 k=%ld", k);
        if (st != DURABLE_PAIR_OK) {
            if (durable_pair_persist_epoch(&pair, EPOCH + 1) == DURABLE_PAIR_OK ||
                durable_pair_persist_epoch(&pair, EPOCH + 2) == DURABLE_PAIR_OK) {
                printf("  [%s] 실패한 핸들이 새 epoch를 내준다\n", label);
                bad++;
            }
        }
        durable_pair_close(&pair);
        if (!floor_survives(label, st == DURABLE_PAIR_OK ? EPOCH + 1 : 0)) bad++;
    }
    CHECK(bad == 0 && failed > 0, "실패 지점 어긋남 %d (주입 %ld)", bad, failed);

    /* 두 슬롯 모두 손상은 기존대로 fail-closed */
    snap_restore(&base);
    damage_slot(0);
    damage_slot(1);
    CHECK(open_pair(true, &pair, NULL) == DURABLE_PAIR_CORRUPT, "이중 슬롯 손상은 CORRUPT");
    snap_free(&base);
}

static void test_v1_damage_rejected(void)
{
    printf("\n[v1 + WAL 없음 + 손상 슬롯]\n");
    durable_pair_t pair;
    durable_pair_info_t info;
    fresh_dir();
    make_v1_db();
    {
        /* v1 commit을 한 번 더 해서 두 슬롯 모두 v1이 되게 한다 */
        page_file_t *f = NULL;
        CHECK(page_file_open(DB, true, &f, NULL) == PAGE_FILE_OK, "v1 열기");
        if (f != NULL) {
            CHECK(page_file_commit(f) == PAGE_FILE_OK, "v1 commit");
            page_file_close(f);
        }
    }
    snap_t clean;
    snap_take(&clean);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_OK && pair.wal == NULL && info.file_info.damaged_slots == 0,
          "손상 없는 v1은 PLAIN");
    durable_pair_close(&pair);

    damage_slot(newest_slot());
    snap_t damaged;
    snap_take(&damaged);
    CHECK(open_pair(false, &pair, &info) == DURABLE_PAIR_CORRUPT, "손상 v1은 PLAIN으로 열지 않는다");
    CHECK(info.file_info.damaged_slots == 1 && pair.file == NULL && pair.wal == NULL, "손상 1을 알리고 핸들 없음");
    CHECK(open_pair(true, &pair, &info) == DURABLE_PAIR_CORRUPT, "convert여도 자동 전환하지 않는다");
    CHECK(snap_same(&damaged) && !exists(WAL), "어느 파일도 바꾸거나 WAL을 만들지 않는다");
    snap_free(&clean);
    snap_free(&damaged);
}

int main(void)
{
    mkdir(DIR_NAME, 0700);
    test_conversion_basic();
    test_recoverable_states();
    test_conversion_points();
    test_downgrade_paths();
    test_late_missing_and_mismatch();
    test_epoch_and_budgets();
    test_epoch_two_slots();
    test_v1_damage_rejected();
    fresh_dir();
    rmdir(DIR_NAME);
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
