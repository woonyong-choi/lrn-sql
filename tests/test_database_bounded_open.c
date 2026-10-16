#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alloc_audit.h"
#include "sql/database.h"
#include "sql/mem_ledger.h"
#include "storage/page_file.h"
#include "../src/sql/database_internal.h"

/*
 * 상한을 지키는 열기(database_open_bounded·database_create_bounded) 검사.
 * 라이브러리 소스는 -Dmalloc=audit_malloc -Dcalloc=audit_calloc -Drealloc=audit_realloc -Dfree=audit_free로
 * 빌드하고 tests/alloc_audit.c와 함께 링크한다. 치환하지 않고 빌드하면 시작할 때 실패한다.
 * 확인하는 것:
 *   - 모든 힙 할당 시점에 이미 reserve한 몫이 그 시점까지 살아 있는 요청 바이트 이상이다(할당 전 reserve).
 *   - 한도 직전(peak-1)은 NOMEM이고 파일도 핸들도 남기지 않으며 reserve를 모두 돌려준다.
 *   - 열기와 admission 사이에 경로의 파일이 교체되거나 열린 파일이 자라도 열린 fd의 파일을 기준으로 한다.
 *   - 제어 페이지가 약속한 카탈로그 크기가 비정상으로 크면 그 크기만큼 할당하기 전에 거절한다.
 * 이 검사는 치환해 빌드한 모듈이 실행한 경로에 대한 것이며 프로그램 전체의 한도 준수 증명이 아니다.
 */

#define DIR_NAME "build/test_bounded_open_dir"

static int g_run, g_fail;
#define CHECK(cond, ...)                              \
    do {                                              \
        g_run++;                                      \
        if (!(cond)) {                                \
            g_fail++;                                 \
            printf("  FAIL (line %d): ", __LINE__);   \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
        }                                             \
    } while (0)

/* ── 추적 ledger ── */
typedef struct {
    size_t limit;
    size_t used;
    size_t peak;
    unsigned long reserves;
    unsigned long refused;
    unsigned long violations;   /* reserve 몫보다 많은 요청 바이트가 살아 있던 할당 */
    bool watching;
    void (*on_first_reserve)(void *);
    void *first_ctx;
    bool fired;
} tracker_t;

static bool track_reserve(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    tracker_t *t = ctx;
    t->reserves++;
    if (!t->fired && t->on_first_reserve != NULL) {
        t->fired = true;
        t->on_first_reserve(t->first_ctx);
    }
    if (bytes > t->limit || t->used > t->limit - bytes) {
        t->refused++;
        return false;
    }
    t->used += bytes;
    if (t->used > t->peak) t->peak = t->used;
    return true;
}

static void track_release(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    tracker_t *t = ctx;
    assert(bytes <= t->used);
    t->used -= bytes;
}

static void track_retain(void *ctx) { (void)ctx; }
static void track_drop(void *ctx) { (void)ctx; }

static const mem_ledger_ops_t TRACK_OPS = {track_reserve, track_release, track_retain, track_drop};

static void on_alloc(void *ctx, size_t request, size_t live)
{
    (void)request;
    tracker_t *t = ctx;
    if (t->watching && live > t->used) t->violations++;
}

static void tracker_init(tracker_t *t, size_t limit)
{
    memset(t, 0, sizeof(*t));
    t->limit = limit;
}

static mem_ledger_t ledger_of(tracker_t *t) { return (mem_ledger_t){&TRACK_OPS, t}; }

static void watch(tracker_t *t)
{
    audit_reset();
    audit_on_alloc = on_alloc;
    audit_ctx = t;
    t->watching = true;
}

static void unwatch(tracker_t *t)
{
    t->watching = false;
    audit_on_alloc = NULL;
}

/* ── 파일 보조 ── */
static void clean_dir(void)
{
    DIR *dir = opendir(DIR_NAME);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", DIR_NAME, entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    mkdir("build", 0755);
    mkdir(DIR_NAME, 0755);
}

static void run_sql(database_t *db, const char *sql)
{
    result_t *result = NULL;
    database_exec_status_t status = database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result);
    assert(status == DATABASE_EXEC_OK && result != NULL);
    assert(result_kind(result) != RESULT_KIND_ERROR);
    result_release(&result);
}

/* 테이블 tables개를 가진 파일을 만든다. 테이블이 많을수록 카탈로그가 커진다. */
static void make_fixture(const char *path, int tables)
{
    unlink(path);
    database_t *db = NULL;
    assert(database_create(path, &db) == DATABASE_OK);
    for (int i = 0; i < tables; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "CREATE TABLE t%d (id INTEGER PRIMARY KEY, name TEXT, note TEXT)", i);
        run_sql(db, sql);
    }
    run_sql(db, "INSERT INTO t0 VALUES (1, 'a', 'b')");
    assert(database_close(db) == DATABASE_OK);
}

static bool exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

/* ── 검사 ── */

static void test_success_accounts_resident_and_reserves_before_allocating(void)
{
    printf("[success]\n");
    const char *path = DIR_NAME "/a.totedb";
    make_fixture(path, 6);
    tracker_t t;
    tracker_init(&t, SIZE_MAX);
    mem_ledger_t ledger = ledger_of(&t);
    watch(&t);
    database_t *db = NULL;
    database_status_t status = database_open_bounded(path, true, &ledger, &db);
    unwatch(&t);
    CHECK(status == DATABASE_OK && db != NULL, "open_bounded=%d", (int)status);
    if (db == NULL) return;
    CHECK(t.violations == 0, "할당 시점에 reserve가 모자란 경우 %lu번", t.violations);
    CHECK(t.used == database_resident_bytes(db), "열린 뒤 남은 reserve %zu != 상주 %zu", t.used,
          database_resident_bytes(db));
    CHECK(t.peak >= t.used, "peak");
    CHECK(database_table_count(db) == 6, "테이블 %zu", database_table_count(db));
    /* 열린 핸들로 실제 질의가 되고 닫으면 모두 돌려준다 */
    run_sql(db, "SELECT * FROM t0");
    CHECK(database_close(db) == DATABASE_OK, "close");
    CHECK(t.used == 0, "닫은 뒤 남은 reserve %zu", t.used);
    CHECK(audit_live == 0, "닫은 뒤 살아 있는 할당 %zu바이트", audit_live);
}

static void test_refusal_is_exact_and_leaves_nothing(void)
{
    printf("[refusal]\n");
    const char *path = DIR_NAME "/b.totedb";
    make_fixture(path, 6);
    tracker_t probe;
    tracker_init(&probe, SIZE_MAX);
    mem_ledger_t probe_ledger = ledger_of(&probe);
    database_t *db = NULL;
    assert(database_open_bounded(path, true, &probe_ledger, &db) == DATABASE_OK);
    size_t admission_peak = probe.peak;
    assert(database_close(db) == DATABASE_OK);
    CHECK(admission_peak > 0, "admission peak");

    /* 한도 = peak면 열리고 peak-1이면 NOMEM이다 */
    tracker_t fits;
    tracker_init(&fits, admission_peak);
    mem_ledger_t fits_ledger = ledger_of(&fits);
    watch(&fits);
    db = NULL;
    database_status_t status = database_open_bounded(path, true, &fits_ledger, &db);
    unwatch(&fits);
    CHECK(status == DATABASE_OK, "한도=peak에서 열려야 한다: %d", (int)status);
    CHECK(fits.violations == 0, "한도=peak: reserve보다 많이 할당 %lu번", fits.violations);
    if (db != NULL) database_close(db);
    CHECK(fits.used == 0, "한도=peak에서 닫은 뒤 reserve %zu", fits.used);

    tracker_t tight;
    tracker_init(&tight, admission_peak - 1);
    mem_ledger_t tight_ledger = ledger_of(&tight);
    watch(&tight);
    db = NULL;
    status = database_open_bounded(path, true, &tight_ledger, &db);
    unwatch(&tight);
    CHECK(status == DATABASE_NOMEM && db == NULL, "한도=peak-1은 NOMEM이어야 한다: %d", (int)status);
    CHECK(tight.violations == 0, "거절 경로에서 reserve보다 많이 할당 %lu번", tight.violations);
    CHECK(tight.used == 0, "거절 뒤 남은 reserve %zu", tight.used);
    CHECK(audit_live == 0, "거절 뒤 살아 있는 할당 %zu바이트", audit_live);

    /* 한도를 더 줄여 첫 단계(핸들)에서 거절되면 아무것도 할당하지 않았다 */
    tracker_t none;
    tracker_init(&none, 1);
    mem_ledger_t none_ledger = ledger_of(&none);
    watch(&none);
    status = database_open_bounded(path, true, &none_ledger, &db);
    unwatch(&none);
    CHECK(status == DATABASE_NOMEM, "한도=1: %d", (int)status);
    CHECK(audit_calls == 0, "핸들 단계에서 거절됐는데 할당 %zu번", audit_calls);
    CHECK(none.used == 0, "한도=1 뒤 reserve %zu", none.used);
}

static void test_failed_opens_release_everything(void)
{
    printf("[failures]\n");
    tracker_t t;
    tracker_init(&t, SIZE_MAX);
    mem_ledger_t ledger = ledger_of(&t);
    database_t *db = NULL;

    watch(&t);
    database_status_t missing = database_open_bounded(DIR_NAME "/none.totedb", true, &ledger, &db);
    CHECK(missing != DATABASE_OK && db == NULL, "없는 파일");
    CHECK(t.used == 0 && audit_calls == 0, "없는 파일: reserve %zu 할당 %zu번", t.used, audit_calls);

    const char *garbage = DIR_NAME "/garbage.totedb";
    FILE *file = fopen(garbage, "wb");
    assert(file != NULL);
    for (int i = 0; i < 3 * 8192; i++) fputc(i % 251, file);
    fclose(file);
    database_status_t bad = database_open_bounded(garbage, true, &ledger, &db);
    unwatch(&t);
    CHECK(bad != DATABASE_OK && db == NULL, "손상 파일 거절 %d", (int)bad);
    CHECK(t.used == 0, "손상 파일 뒤 reserve %zu", t.used);
    CHECK(audit_live == 0, "손상 파일 뒤 살아 있는 할당 %zu", audit_live);

    /* 다른 핸들이 쓰기 작업(배타 잠금) 중이면 대기 상한 뒤 LOCKED이고 reserve는 돌아온다 */
    const char *path = DIR_NAME "/lock.totedb";
    make_fixture(path, 2);
    database_t *first = NULL;
    assert(database_open(path, true, &first) == DATABASE_OK);
    assert(database_work_begin(first) == DATABASE_OK);
    page_file_set_default_lock_timeout(0);
    watch(&t);
    database_status_t locked = database_open_bounded(path, true, &ledger, &db);
    unwatch(&t);
    page_file_set_default_lock_timeout(PAGE_FILE_LOCK_TIMEOUT_MS);
    CHECK(locked == DATABASE_LOCKED && db == NULL, "잠긴 파일 %d", (int)locked);
    CHECK(t.used == 0, "잠긴 파일 뒤 reserve %zu", t.used);
    database_work_end(first, DATABASE_WORK_NOT_APPLIED);
    /* 작업이 끝나면 쓰기 핸들이 열려 있어도 두 번째 핸들이 열린다(#199) */
    database_t *second = NULL;
    CHECK(database_open_bounded(path, true, &ledger, &second) == DATABASE_OK, "작업이 없는 파일의 두 번째 열기");
    database_close(second);
    database_close(first);
}

/* 제어 페이지에 터무니없이 큰 카탈로그 크기를 써 넣은 파일 */
static void test_huge_catalog_size_is_rejected_before_allocating(void)
{
    printf("[huge catalog size]\n");
    const char *path = DIR_NAME "/huge.totedb";
    make_fixture(path, 2);
    page_file_t *file = NULL;
    assert(page_file_open(path, true, &file, NULL) == PAGE_FILE_OK);
    const page_file_control_t *control = page_file_control(file);
    assert(page_file_stage_catalog(file, control->catalog_head, 0xFFFFFFF0u, control->catalog_generation) == PAGE_FILE_OK);
    assert(page_file_commit(file) == PAGE_FILE_OK);
    assert(page_file_close(file) == PAGE_FILE_OK);

    tracker_t t;
    tracker_init(&t, (size_t)1 << 40);  /* 한도가 아주 커도 한도 초과 크기는 거절한다 */
    mem_ledger_t ledger = ledger_of(&t);
    watch(&t);
    database_t *db = NULL;
    database_status_t status = database_open_bounded(path, false, &ledger, &db);
    unwatch(&t);
    CHECK(status == DATABASE_CORRUPT && db == NULL, "거대한 카탈로그 크기: %d", (int)status);
    CHECK(audit_max_single < ((size_t)1 << 20), "거절 전에 %zu바이트를 한 번에 요청했다", audit_max_single);
    CHECK(t.used == 0, "거절 뒤 reserve %zu", t.used);
    CHECK(audit_live == 0, "거절 뒤 살아 있는 할당 %zu", audit_live);

    /* 한도 안 크기지만 예산을 넘으면 읽기 buffer를 할당하기 전에 NOMEM이다 */
    make_fixture(path, 40);
    tracker_t probe;
    tracker_init(&probe, SIZE_MAX);
    mem_ledger_t probe_ledger = ledger_of(&probe);
    database_t *opened = NULL;
    assert(database_open_bounded(path, false, &probe_ledger, &opened) == DATABASE_OK);
    size_t resident = database_resident_bytes(opened);
    database_close(opened);
    tracker_t small;
    tracker_init(&small, resident);  /* 상주 몫은 들어가도 읽기 임시까지는 모자란 예산 */
    mem_ledger_t small_ledger = ledger_of(&small);
    watch(&small);
    status = database_open_bounded(path, false, &small_ledger, &db);
    unwatch(&small);
    CHECK(status == DATABASE_NOMEM && db == NULL, "임시를 못 잡으면 NOMEM이어야 한다: %d", (int)status);
    CHECK(small.violations == 0, "읽기 단계에서 reserve보다 많이 할당 %lu번", small.violations);
    CHECK(small.used == 0 && audit_live == 0, "거절 뒤 reserve %zu 할당 %zu", small.used, audit_live);
}

/* 첫 reserve(admit 직후, fd를 연 다음)에서 경로의 파일을 바꾸거나 키운다 */
typedef struct {
    const char *path;
    const char *replacement;
    int grow_fd;
} race_t;

static void race_hook(void *ctx)
{
    race_t *race = ctx;
    if (race->grow_fd >= 0) {
        /* 열린 파일을 8MiB 키운다. 잠금은 권고라 막지 못한다 */
        assert(ftruncate(race->grow_fd, (off_t)8 << 20) == 0);
    }
    if (race->replacement != NULL) assert(rename(race->replacement, race->path) == 0);
}

static void test_replacement_and_growth_after_open_do_not_bypass_admission(void)
{
    printf("[race]\n");
    const char *path = DIR_NAME "/race.totedb";
    const char *big = DIR_NAME "/race_big.totedb";
    make_fixture(path, 2);
    make_fixture(big, 60);

    /* 열린 fd의 크기로 admission을 계산하므로 경로를 바꿔치기해도 원래 파일(테이블 2개)이 열린다 */
    race_t race = {path, big, -1};
    tracker_t t;
    tracker_init(&t, SIZE_MAX);
    t.on_first_reserve = race_hook;
    t.first_ctx = &race;
    mem_ledger_t ledger = ledger_of(&t);
    watch(&t);
    database_t *db = NULL;
    database_status_t status = database_open_bounded(path, false, &ledger, &db);
    unwatch(&t);
    CHECK(status == DATABASE_OK && db != NULL, "교체 중 열기 %d", (int)status);
    CHECK(db != NULL && database_table_count(db) == 2, "열린 fd의 파일이어야 한다: 테이블 %zu",
          db != NULL ? database_table_count(db) : 0);
    CHECK(t.violations == 0, "교체 중 reserve보다 많이 할당 %lu번", t.violations);
    if (db != NULL) database_close(db);
    CHECK(t.used == 0, "교체 뒤 reserve %zu", t.used);

    /* 열고 잠근 뒤 같은 파일이 8MiB로 자라도 admission은 제어 페이지의 페이지 수와 카탈로그 크기를 따른다 */
    make_fixture(path, 2);
    int grow_fd = open(path, O_RDWR);
    assert(grow_fd >= 0);
    race_t grow = {path, NULL, grow_fd};
    tracker_t g;
    tracker_init(&g, SIZE_MAX);
    g.on_first_reserve = race_hook;
    g.first_ctx = &grow;
    mem_ledger_t grow_ledger = ledger_of(&g);
    watch(&g);
    db = NULL;
    status = database_open_bounded(path, false, &grow_ledger, &db);
    unwatch(&g);
    close(grow_fd);
    CHECK(status == DATABASE_OK && db != NULL, "성장 중 열기 %d", (int)status);
    CHECK(g.violations == 0, "성장 중 reserve보다 많이 할당 %lu번", g.violations);
    if (db != NULL) database_close(db);
    CHECK(g.used == 0 && audit_live == 0, "성장 뒤 reserve %zu 할당 %zu", g.used, audit_live);

    /* 이미 자란 파일(8MiB)은 페이지 수 상한이 커져 작은 파일에 맞춘 예산으로는 거절된다. 크기는 열린 fd의 것이다 */
    const char *small_path = DIR_NAME "/race_small.totedb";
    make_fixture(small_path, 2);
    tracker_t small_probe;
    tracker_init(&small_probe, SIZE_MAX);
    mem_ledger_t small_probe_ledger = ledger_of(&small_probe);
    database_t *small_db = NULL;
    assert(database_open_bounded(small_path, false, &small_probe_ledger, &small_db) == DATABASE_OK);
    database_close(small_db);
    tracker_t grown_probe;
    tracker_init(&grown_probe, SIZE_MAX);
    mem_ledger_t grown_probe_ledger = ledger_of(&grown_probe);
    assert(database_open_bounded(path, false, &grown_probe_ledger, &small_db) == DATABASE_OK);
    database_close(small_db);
    CHECK(grown_probe.peak > small_probe.peak, "자란 파일의 admission(%zu)이 작은 파일(%zu)보다 커야 한다",
          grown_probe.peak, small_probe.peak);

    tracker_t small;
    tracker_init(&small, small_probe.peak);
    mem_ledger_t small_ledger = ledger_of(&small);
    watch(&small);
    db = NULL;
    status = database_open_bounded(path, false, &small_ledger, &db);
    unwatch(&small);
    CHECK(status == DATABASE_NOMEM && db == NULL, "자란 파일은 작은 예산에서 NOMEM: %d", (int)status);
    CHECK(small.violations == 0, "자란 파일 거절 경로에서 reserve보다 많이 할당 %lu번", small.violations);
    CHECK(small.used == 0 && audit_live == 0, "자란 파일 거절 뒤 reserve %zu 할당 %zu", small.used, audit_live);
}

static void test_create_bounded(void)
{
    printf("[create]\n");
    const char *path = DIR_NAME "/created.totedb";
    unlink(path);
    tracker_t probe;
    tracker_init(&probe, SIZE_MAX);
    mem_ledger_t probe_ledger = ledger_of(&probe);
    database_t *db = NULL;
    watch(&probe);
    assert(database_create_bounded(path, &probe_ledger, &db) == DATABASE_OK);
    unwatch(&probe);
    CHECK(probe.violations == 0, "만들기 중 reserve보다 많이 할당 %lu번", probe.violations);
    CHECK(probe.used == database_resident_bytes(db), "만든 뒤 reserve %zu != 상주 %zu", probe.used,
          database_resident_bytes(db));
    size_t admission_peak = probe.peak;
    run_sql(db, "CREATE TABLE t (id INTEGER PRIMARY KEY)");
    database_close(db);
    CHECK(probe.used == 0 && audit_live == 0, "닫은 뒤 reserve %zu 할당 %zu", probe.used, audit_live);

    /* 한도 직전은 파일을 만들지 않고 NOMEM이다 */
    unlink(path);
    tracker_t tight;
    tracker_init(&tight, admission_peak - 1);
    mem_ledger_t tight_ledger = ledger_of(&tight);
    watch(&tight);
    database_status_t status = database_create_bounded(path, &tight_ledger, &db);
    unwatch(&tight);
    CHECK(status == DATABASE_NOMEM, "한도 직전 만들기 %d", (int)status);
    CHECK(!exists(path), "거절됐는데 파일이 만들어졌다");
    CHECK(tight.used == 0 && audit_calls == 0, "거절 뒤 reserve %zu 할당 %zu번", tight.used, audit_calls);

    /* 이미 있으면 EXISTS이고 reserve는 돌아오며 기존 파일은 그대로다 */
    make_fixture(path, 3);
    tracker_t t;
    tracker_init(&t, SIZE_MAX);
    mem_ledger_t ledger = ledger_of(&t);
    status = database_create_bounded(path, &ledger, &db);
    CHECK(status == DATABASE_EXISTS, "이미 있는 파일 %d", (int)status);
    CHECK(t.used == 0, "EXISTS 뒤 reserve %zu", t.used);
    database_t *check = NULL;
    assert(database_open(path, false, &check) == DATABASE_OK);
    CHECK(database_table_count(check) == 3, "기존 파일이 바뀌었다");
    database_close(check);
}

int main(void)
{
    /* 치환 없이 빌드했는지 확인한다: 치환한 database.c의 할당은 audit을 지난다 */
    clean_dir();
    audit_reset();
    database_t *db = NULL;
    assert(database_create(DIR_NAME "/probe.totedb", &db) == DATABASE_OK);
    size_t calls = audit_calls;
    database_close(db);
    if (calls == 0) {
        printf("할당 치환 없이 빌드됐다\n");
        return 2;
    }
    clean_dir();
    test_success_accounts_resident_and_reserves_before_allocating();
    test_refusal_is_exact_and_leaves_nothing();
    test_failed_opens_release_everything();
    test_huge_catalog_size_is_rejected_before_allocating();
    test_replacement_and_growth_after_open_do_not_bypass_admission();
    test_create_bounded();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
