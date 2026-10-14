#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql/database.h"
#include "sql/result.h"
#include "storage/page_file.h"

/*
 * database의 실패 보존·불확정 결과 검사. 모든 문장에 대해 I/O 실패(pread·pwrite·fsync)와
 * 할당 실패를 한 지점씩 주입하고 다음을 확인한다.
 *   - 성공 응답이면 그 문장이 반영됐고 다시 열어도 같다.
 *   - 오류 응답이고 핸들이 poison되지 않았으면 게시된 상태가 문장 전과 같고 같은 문장을 다시 실행할 수 있다.
 *   - 핸들이 poison됐으면 이후 문장은 모두 오류이고, 다시 열면 문장 전 또는 후 상태다(어느 쪽인지는 불확정).
 *   - 제어 쓰기 전의 fsync 실패는 반드시 문장 전 상태다.
 *   - 제어 쓰기 뒤의 fsync 실패는 오류로 응답하지만 다시 열면 새 상태가 보인다(불확정이라 성공이라 하지 않는다).
 * database.c·heap_store.c는 -Dmalloc=db_test_malloc -Dcalloc=db_test_calloc -Drealloc=db_test_realloc 으로 빌드한다.
 */

#define DIR_NAME "build/test_database_fault_dir"
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

/* ── 주입 ── */
static long g_alloc_fail_at, g_alloc_calls;
static int alloc_fails(void) { return g_alloc_fail_at != 0 && ++g_alloc_calls == g_alloc_fail_at; }

/*
 * 소유권 검사: 치환한 database.c·heap_store.c가 받은 할당만 등록부에 올리고, 해제할 때 내린다.
 * 그 두 모듈 밖에서 만든 할당(catalog·result·lexer·page_file 등)은 등록부에 없으므로 이 검사로는
 * 보지 못한다. 해제된 포인터는 quarantine_flush 전까지 실제로 free하지 않고 보관해, 같은 주소가 다른
 * 모듈의 할당으로 재사용되어 생기는 오탐 없이 이중 해제를 센다. 이 검사는 실행한 경로에 대한 것이며
 * 프로그램 전체에 누수가 없다는 증거가 아니다.
 */
static void **g_live;
static size_t g_live_count, g_live_cap;
static void **g_freed;
static size_t g_freed_count, g_freed_cap;
static long g_double_frees;

static void list_add(void ***list, size_t *count, size_t *cap, void *p)
{
    if (*count == *cap) {
        *cap = *cap == 0 ? 256 : *cap * 2;
        *list = realloc(*list, *cap * sizeof(**list));
        assert(*list != NULL);
    }
    (*list)[(*count)++] = p;
}

static bool list_take(void **list, size_t *count, void *p)
{
    for (size_t i = *count; i > 0; i--) {
        if (list[i - 1] == p) {
            list[i - 1] = list[--*count];
            return true;
        }
    }
    return false;
}

static size_t tracked_live(void) { return g_live_count; }

static void quarantine_flush(void)
{
    for (size_t i = 0; i < g_freed_count; i++) free(g_freed[i]);
    g_freed_count = 0;
}

static void track(void *p)
{
    if (p == NULL) return;
    list_add(&g_live, &g_live_count, &g_live_cap, p);
}

void *db_test_malloc(size_t n)
{
    if (alloc_fails()) return NULL;
    void *p = malloc(n);
    track(p);
    return p;
}

void *db_test_calloc(size_t a, size_t b)
{
    if (alloc_fails()) return NULL;
    void *p = calloc(a, b);
    track(p);
    return p;
}

void *db_test_realloc(void *p, size_t n)
{
    if (alloc_fails()) return NULL;
    bool owned = p != NULL && list_take(g_live, &g_live_count, p);
    void *q = realloc(p, n);
    if (q == NULL) {
        if (owned) list_add(&g_live, &g_live_count, &g_live_cap, p);
        return NULL;
    }
    if (p == NULL || owned) track(q);
    return q;
}

void db_test_free(void *p)
{
    if (p == NULL) return;
    if (list_take(g_live, &g_live_count, p)) {
        list_add(&g_freed, &g_freed_count, &g_freed_cap, p);  /* 실제 해제는 quarantine_flush에서 */
        return;
    }
    for (size_t i = 0; i < g_freed_count; i++) {
        if (g_freed[i] == p) {
            g_double_frees++;
            return;
        }
    }
    free(p);  /* 이 모듈들이 만들지 않은 포인터 */
}

typedef struct {
    int pwrite_fail_at, pwrite_calls;
    int pread_fail_at, pread_calls;
    int fsync_fail_at, fsync_calls;
} hooks_t;

static hooks_t g_hooks;

static ssize_t hook_pread(int fd, void *buf, size_t size, off_t offset)
{
    if (g_hooks.pread_fail_at != 0 && ++g_hooks.pread_calls == g_hooks.pread_fail_at) {
        errno = EIO;
        return -1;
    }
    return pread(fd, buf, size, offset);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (g_hooks.pwrite_fail_at != 0 && ++g_hooks.pwrite_calls == g_hooks.pwrite_fail_at) {
        errno = ENOSPC;
        return -1;
    }
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

static const page_file_io_t HOOKED = {hook_pread, hook_pwrite, hook_fsync, link, unlink};

typedef enum { KIND_PWRITE, KIND_PREAD, KIND_FSYNC, KIND_ALLOC } kind_t;
static const char *KIND_NAMES[] = {"pwrite", "pread", "fsync", "alloc"};

static void arm(kind_t kind, int n)
{
    memset(&g_hooks, 0, sizeof(g_hooks));
    g_alloc_fail_at = 0;
    g_alloc_calls = 0;
    if (kind == KIND_PWRITE) g_hooks.pwrite_fail_at = n;
    if (kind == KIND_PREAD) g_hooks.pread_fail_at = n;
    if (kind == KIND_FSYNC) g_hooks.fsync_fail_at = n;
    if (kind == KIND_ALLOC) g_alloc_fail_at = n;
}

static void disarm(void)
{
    memset(&g_hooks, 0, sizeof(g_hooks));
    g_alloc_fail_at = 0;
}

static bool triggered(kind_t kind, int n)
{
    switch (kind) {
        case KIND_PWRITE: return g_hooks.pwrite_calls >= n;
        case KIND_PREAD: return g_hooks.pread_calls >= n;
        case KIND_FSYNC: return g_hooks.fsync_calls >= n;
        default: return g_alloc_calls >= n;
    }
}

/* ── 파일·결과 보조 ── */
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
    rmdir(DIR_NAME);
}

static uint64_t fnv(const uint8_t *bytes, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

typedef struct {
    char *text;
    size_t len, cap;
} buffer_t;

static void put(buffer_t *b, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void put(buffer_t *b, const char *format, ...)
{
    char piece[128];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(piece, sizeof(piece), format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(piece));
    if (b->len + (size_t)n + 1 > b->cap) {
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->text = realloc(b->text, b->cap);
        assert(b->text != NULL);
    }
    memcpy(b->text + b->len, piece, (size_t)n + 1);
    b->len += (size_t)n;
}

/* SELECT 결과를 정렬 없이 순서대로 한 문자열로. 텍스트·BLOB은 길이와 해시만 쓴다. */
static char *select_dump(database_t *db, const char *sql)
{
    result_t *result = NULL;
    database_exec_status_t status = database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result);
    assert(status == DATABASE_EXEC_OK);
    buffer_t out = {NULL, 0, 0};
    put(&out, "%s", "");
    if (result_kind(result) != RESULT_KIND_ROWS) {
        put(&out, "NOT_ROWS(%d)", (int)result_kind(result));
        result_release(&result);
        return out.text;
    }
    result_row_t row;
    size_t rows = 0;
    uint64_t digest = 1469598103934665603ull;
    while (result_next(result, &row) == RESULT_NEXT_ROW) {
        rows++;
        for (size_t i = 0; i < row.cell_count; i++) {
            const value_view_t *c = &row.cells[i];
            uint64_t part = (uint64_t)c->type * 1000003u;
            if (c->type == VALUE_INTEGER) part ^= (uint64_t)c->integer;
            if (c->type == VALUE_REAL) {
                uint64_t bits;
                memcpy(&bits, &c->real, sizeof(bits));
                part ^= bits;
            }
            if (c->type == VALUE_TEXT || c->type == VALUE_BLOB) part ^= fnv(c->bytes, c->size_bytes) ^ c->size_bytes;
            digest = (digest ^ part) * 1099511628211ull;
        }
        digest = (digest ^ 0xff) * 1099511628211ull;
    }
    put(&out, "%zu:%016llx", rows, (unsigned long long)digest);
    result_release(&result);
    return out.text;
}

/* 모든 테이블 + 테이블 목록(존재 여부) */
static char *snapshot(database_t *db)
{
    static const char *tables[] = {"users", "logs", "extra"};
    buffer_t out = {NULL, 0, 0};
    put(&out, "%s", "");
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "SELECT * FROM %s", tables[i]);
        char *dump = select_dump(db, sql);
        put(&out, "%s=%s;", tables[i], dump);
        free(dump);
    }
    put(&out, "n=%zu", database_table_count(db));
    return out.text;
}

static void exec_sql(database_t *db, const char *sql)
{
    result_t *result = NULL;
    assert(database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result) == DATABASE_EXEC_OK);
    assert(result_kind(result) == RESULT_KIND_COMMAND);
    result_release(&result);
}

static void build_baseline(void)
{
    clean_dir();
    assert(mkdir(DIR_NAME, 0700) == 0);
    database_t *db;
    assert(database_create(DB_PATH, &db) == DATABASE_OK);
    exec_sql(db, "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, bio TEXT)");
    exec_sql(db, "CREATE TABLE logs (ts INTEGER, msg TEXT NOT NULL)");
    for (int i = 0; i < 6; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO users (name, bio) VALUES ('user%d', 'bio %d')", i, i);
        exec_sql(db, sql);
        snprintf(sql, sizeof(sql), "INSERT INTO logs VALUES (%d, 'log %d')", i, i);
        exec_sql(db, sql);
    }
    assert(database_close(db) == DATABASE_OK);
}

static uint8_t *g_big;
static size_t g_big_size = 20000;

typedef struct {
    const char *name;
    const char *sql;
    bool bound_big;   /* 첫 매개변수로 큰 TEXT를 넘긴다 */
} statement_case_t;

static const statement_case_t CASES[] = {
    {"create", "CREATE TABLE extra (x INTEGER, y TEXT)", false},
    {"insert big", "INSERT INTO users (name, bio) VALUES ('zed', ?)", true},
    {"update grow", "UPDATE users SET bio = ? WHERE id <= 3", true},
    {"update shrink", "UPDATE users SET bio = 'x'", false},
    {"delete", "DELETE FROM users WHERE id = 2", false},
    {"drop", "DROP TABLE logs", false},
};

static database_exec_status_t run_case(database_t *db, const statement_case_t *c, result_t **result)
{
    value_view_t param = {VALUE_TEXT, 0, 0, g_big, g_big_size};
    lexer_binding_t binding = {&param, 1};
    return database_exec(db, (const uint8_t *)c->sql, strlen(c->sql), c->bound_big ? &binding : NULL, result);
}

static void test_statement(const statement_case_t *c)
{
    printf("\n[%s]\n", c->name);
    page_file_set_default_io(NULL);
    build_baseline();
    char *before, *after;
    {
        database_t *db;
        assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
        before = snapshot(db);
        result_t *result = NULL;
        assert(run_case(db, c, &result) == DATABASE_EXEC_OK && result_kind(result) == RESULT_KIND_COMMAND);
        result_release(&result);
        after = snapshot(db);
        assert(database_close(db) == DATABASE_OK);
    }
    CHECK(strcmp(before, after) != 0, "문장이 상태를 바꿔야 함");

    page_file_set_default_io(&HOOKED);
    int errors = 0, unknown = 0, successes = 0, nomem = 0, poisoned_total = 0;
    for (int kind = KIND_PWRITE; kind <= KIND_ALLOC; kind++) {
        for (int n = 1; n <= 3000; n++) {
            quarantine_flush();
            page_file_set_default_io(NULL);
            build_baseline();
            page_file_set_default_io(&HOOKED);
            disarm();
            database_t *db;
            assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
            size_t live_baseline = tracked_live();
            long doubles_before = g_double_frees;
            result_t *result = NULL;
            arm((kind_t)kind, n);
            database_exec_status_t status = run_case(db, c, &result);
            bool hit = triggered((kind_t)kind, n);
            disarm();
            if (!hit && status == DATABASE_EXEC_OK && result != NULL && result_kind(result) == RESULT_KIND_COMMAND) {
                result_release(&result);
                CHECK(tracked_live() == live_baseline && g_double_frees == doubles_before,
                      "%s %s: 주입 없이 성공한 문장도 할당을 모두 해제함 (남은 %zu, 기준 %zu)", c->name, KIND_NAMES[kind],
                      tracked_live(), live_baseline);
                assert(database_close(db) == DATABASE_OK);
                quarantine_flush();
                break;  /* 이 종류의 주입 지점을 모두 지났다 */
            }
            bool poisoned = database_is_poisoned(db);
            bool answered_error = false;
            poisoned_total += poisoned;
            char label[96];
            snprintf(label, sizeof(label), "%s %s %d", c->name, KIND_NAMES[kind], n);

            if (status == DATABASE_EXEC_NOMEM) {
                nomem++;
                CHECK(result == NULL, "%s: NOMEM이면 결과 없음", label);
                char *now = snapshot(db);
                CHECK(strcmp(now, before) == 0 || poisoned, "%s: NOMEM 뒤 상태 불변", label);
                free(now);
            } else {
                CHECK(status == DATABASE_EXEC_OK && result != NULL, "%s: 결과가 있음 (%d)", label, (int)status);
                if (result != NULL && result_kind(result) == RESULT_KIND_COMMAND) {
                    successes++;
                    char *now = snapshot(db);
                    CHECK(strcmp(now, after) == 0, "%s: 성공 응답이면 반영됨", label);
                    free(now);
                } else if (result != NULL) {
                    errors++;
                    answered_error = true;
                    CHECK(result_kind(result) == RESULT_KIND_ERROR, "%s: 오류 결과", label);
                    if (!poisoned) {
                        char *now = snapshot(db);
                        CHECK(strcmp(now, before) == 0, "%s: 오류 응답, 핸들 정상이면 문장 전 상태", label);
                        free(now);
                        /* 같은 핸들로 다시 실행할 수 있다 */
                        result_t *again = NULL;
                        CHECK(run_case(db, c, &again) == DATABASE_EXEC_OK && again != NULL &&
                                  result_kind(again) == RESULT_KIND_COMMAND,
                              "%s: 같은 문장을 다시 실행할 수 있음", label);
                        result_release(&again);
                        char *redone = snapshot(db);
                        CHECK(strcmp(redone, after) == 0, "%s: 다시 실행하면 문장 후 상태", label);
                        free(redone);
                    } else {
                        /* poison: 이후 모든 문장이 오류 */
                        result_t *next = NULL;
                        CHECK(run_case(db, c, &next) == DATABASE_EXEC_OK && next != NULL &&
                                  result_kind(next) == RESULT_KIND_ERROR,
                              "%s: poison된 핸들은 모든 문장을 거절", label);
                        result_release(&next);
                    }
                }
            }
            if (kind == KIND_FSYNC && n == 2) {
                CHECK(answered_error && poisoned, "%s: 제어 쓰기 뒤 fsync 실패는 오류 응답이고 핸들 poison", label);
            }
            result_release(&result);
            CHECK(tracked_live() == live_baseline && g_double_frees == doubles_before,
                  "%s: 문장 하나가 끝나면 database.c·heap_store.c의 할당이 모두 정확히 한 번 해제됨 (남은 %zu, 기준 %zu, 이중 해제 %ld)",
                  label, tracked_live(), live_baseline, g_double_frees - doubles_before);
            assert(database_close(db) == DATABASE_OK);
            quarantine_flush();

            /* 다시 열었을 때의 상태 */
            page_file_set_default_io(NULL);
            database_t *reopened;
            database_status_t opened = database_open(DB_PATH, true, &reopened);
            CHECK(opened == DATABASE_OK, "%s: 다시 열기 (%d)", label, (int)opened);
            if (opened == DATABASE_OK) {
                char *now = snapshot(reopened);
                bool is_before = strcmp(now, before) == 0, is_after = strcmp(now, after) == 0;
                CHECK(is_before || is_after, "%s: 다시 열면 문장 전 또는 후 상태", label);
                if (kind == KIND_FSYNC && n == 1) {
                    CHECK(is_before, "%s: 제어 쓰기 전의 fsync 실패는 반드시 문장 전 상태", label);
                }
                if (answered_error && poisoned && is_after) {
                    unknown++;  /* 오류로 응답했지만 새 상태가 보임: 불확정 결과를 성공이라 하지 않았다 */
                }
                free(now);
                assert(database_close(reopened) == DATABASE_OK);
            }
            page_file_set_default_io(&HOOKED);
        }
    }
    page_file_set_default_io(NULL);
    CHECK(errors > 0, "I/O 실패 지점을 지남");
    CHECK(unknown > 0, "불확정 결과(오류 응답, 새 상태가 보임)가 실제로 관측됨");
    printf("  (오류 응답 %d, NOMEM %d, 성공 %d, poison %d, 오류 응답이지만 새 상태가 보인 불확정 %d)\n",
           errors, nomem, successes, poisoned_total, unknown);
    free(before);
    free(after);
}

int main(void)
{
    g_big = malloc(g_big_size);
    assert(g_big != NULL);
    for (size_t i = 0; i < g_big_size; i++) g_big[i] = (uint8_t)('a' + i % 26);
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) test_statement(&CASES[i]);
    free(g_big);
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
