#include <assert.h>
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alloc_audit.h"
#include "sql/session.h"
#include "storage/page_file.h"

/*
 * 세션 계층의 전체 allocator audit와 실패 순번 sweep, 강제 스케줄 검사.
 * 모든 라이브러리 소스와 session.c를 -Dmalloc=audit_malloc -Dcalloc=audit_calloc -Drealloc=audit_realloc
 * -Dfree=audit_free -DSESSION_TEST_HOOK으로 빌드하고 tests/alloc_audit.c와 링크한다(치환하지 않으면 시작할 때 실패).
 *
 * 핵심 불변식: 풀 mutex를 쥔 어느 순간에도 "실제로 살아 있는 요청 바이트(audit_live) <= 풀의 shared_used"이다.
 * 반환이 해제보다 먼저 일어나거나(몫을 먼저 돌려줌) 할당이 reserve보다 먼저 일어나면 어긋난다. 이 검사는
 *   - 모든 reserve·release 직후(session.c의 훅),
 *   - 모든 실제 할당 직후(audit 훅)
 * 에서 한다. 성공 시점의 통계만 보지 않는다. 장부가 0이 된 것은 정리가 끝난 증거가 아니므로 끝에서
 * audit_live == 0(풀 구조체까지 해제됨)도 본다.
 *
 * sweep: 문장 종류마다 n번째 reserve 실패, n번째 pread·pwrite·fsync 실패, n번째 취소 확인을 하나씩 주입하고
 * 자원 회수, 상태 보존, 후속 질의를 확인한다. 강제 스케줄: audit_on_free에서 스레드를 멈춰 "메모리를 해제한
 * 직후 몫을 돌려주기 전"을 고정하고 그 사이의 pool_close·session_close·다음 reserve를 확인한다.
 * 이 검사는 치환해 빌드한 경로에 대한 것이며 큐를 거친 경로나 프로그램 전체의 증명이 아니다.
 */

extern void session_test_with_lock(session_pool_t *pool, void (*fn)(void *ctx, size_t shared_used), void *ctx);

#define DIR_NAME "build/test_session_audit_dir"
#define DB_PATH DIR_NAME "/db.lrdb"

static int g_run, g_fail;
#define CHECK(cond, ...)                              \
    do {                                              \
        g_run++;                                      \
        if (!(cond)) {                                \
            g_fail++;                                 \
            if (g_fail <= 60) {                       \
                printf("  FAIL (line %d): ", __LINE__); \
                printf(__VA_ARGS__);                  \
                printf("\n");                         \
            }                                         \
        }                                             \
    } while (0)

/* ── 불변식 훅 ── */
static session_pool_t *g_pool;
static volatile unsigned long g_violations;
static char g_where[64];

static void note_violation(const char *where)
{
    if (__atomic_fetch_add(&g_violations, 1, __ATOMIC_RELAXED) == 0) {
        snprintf(g_where, sizeof(g_where), "%s", where);
        if (getenv("AUDIT_TRACE") != NULL) {
            /* 첫 위반의 호출 경로. 반환이 해제보다 앞선 곳을 찾는 데 쓴다 */
            void *frames[24];
            int n = backtrace(frames, 24);
            backtrace_symbols_fd(frames, n, 2);
        }
    }
}

void session_test_locked(const char *where, size_t shared_used)
{
    if (audit_live_now() > shared_used) note_violation(where);
}

/* 몫을 돌려주는 객체는 그 전에 해제돼 있어야 한다. 상주 몫의 여유가 작은 구조체의 조기 반환을 가리지 못하게
 * 장부의 부등식과 별개로 주소 기준으로 본다 */
void session_test_returning(const void *object, const char *where)
{
    if (audit_is_live(object)) note_violation(where);
}

static long g_fail_n;
static long g_reserve_calls;

bool session_test_fail_reserve(void)
{
    if (g_fail_n <= 0) return false;
    return __atomic_add_fetch(&g_reserve_calls, 1, __ATOMIC_RELAXED) == g_fail_n;
}

static void check_alloc_locked(void *ctx, size_t shared_used)
{
    (void)ctx;
    if (audit_live_now() > shared_used) note_violation("alloc");
}

static void on_alloc(void *ctx, size_t request, size_t live)
{
    (void)ctx;
    (void)request;
    (void)live;
    session_pool_t *pool = g_pool;
    if (pool != NULL) session_test_with_lock(pool, check_alloc_locked, NULL);
}

/* ── 스레드를 free 직후에 멈추는 문 ── */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool active;        /* 목표 스레드의 free만 센다 */
    pthread_t target;
    long seen;
    long stop_at;       /* seen이 이 값이 되면 멈춘다. 0이면 세기만 한다 */
    bool paused;
    bool resume;
    bool finished;
    bool go;            /* 스레드가 일을 시작해도 된다 */
} gate_t;

static gate_t g_gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, 0, 0, 0, false, false, false, false};

static void gate_wait_go(void)
{
    pthread_mutex_lock(&g_gate.m);
    while (!g_gate.go) pthread_cond_wait(&g_gate.c, &g_gate.m);
    pthread_mutex_unlock(&g_gate.m);
}

static void on_free(void *ctx, size_t size, size_t live)
{
    (void)ctx;
    (void)size;
    (void)live;
    gate_t *g = &g_gate;
    pthread_mutex_lock(&g->m);
    if (g->active && pthread_equal(pthread_self(), g->target)) {
        g->seen++;
        if (g->stop_at > 0 && g->seen == g->stop_at) {
            g->paused = true;
            pthread_cond_broadcast(&g->c);
            while (!g->resume) pthread_cond_wait(&g->c, &g->m);
            g->paused = false;
        }
    }
    pthread_mutex_unlock(&g->m);
}

static void gate_arm(pthread_t target, long stop_at)
{
    pthread_mutex_lock(&g_gate.m);
    g_gate.active = true;
    g_gate.target = target;
    g_gate.seen = 0;
    g_gate.stop_at = stop_at;
    g_gate.paused = g_gate.resume = g_gate.finished = false;
    g_gate.go = true;
    pthread_mutex_unlock(&g_gate.m);
}

static long gate_disarm(void)
{
    pthread_mutex_lock(&g_gate.m);
    g_gate.active = false;
    long seen = g_gate.seen;
    pthread_mutex_unlock(&g_gate.m);
    return seen;
}

/* 목표 스레드가 멈추거나 끝날 때까지 기다린다. 멈췄으면 true */
static bool gate_wait_paused(void)
{
    pthread_mutex_lock(&g_gate.m);
    while (!g_gate.paused && !g_gate.finished) pthread_cond_wait(&g_gate.c, &g_gate.m);
    bool paused = g_gate.paused;
    pthread_mutex_unlock(&g_gate.m);
    return paused;
}

static void gate_finish(void)
{
    pthread_mutex_lock(&g_gate.m);
    g_gate.finished = true;
    pthread_cond_broadcast(&g_gate.c);
    pthread_mutex_unlock(&g_gate.m);
}

static void gate_resume(void)
{
    pthread_mutex_lock(&g_gate.m);
    g_gate.resume = true;
    pthread_cond_broadcast(&g_gate.c);
    pthread_mutex_unlock(&g_gate.m);
}

/* ── 보조 ── */
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

static void fresh_dir(void)
{
    clean_dir();
    mkdir("build", 0755);
    assert(mkdir(DIR_NAME, 0700) == 0);
}

static bool file_exists(void)
{
    struct stat st;
    return stat(DB_PATH, &st) == 0;
}

static session_limits_t big_limits(void)
{
    return (session_limits_t){SIZE_MAX, SIZE_MAX, SIZE_MAX, 16, 0};
}

static session_pool_stats_t pool_stats(session_pool_t *pool)
{
    session_pool_stats_t stats;
    assert(session_pool_stats(pool, &stats) == SESSION_OK);
    return stats;
}

static session_stats_t sess_stats(session_t *session)
{
    session_stats_t stats;
    assert(session_stats_get(session, &stats) == SESSION_OK);
    return stats;
}

typedef struct {
    result_kind_t kind;
    size_t rows;
    result_error_class_t error_class;
    int code;
    uint64_t hash;
} summary_t;

static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

static summary_t consume(result_t *result)
{
    summary_t s = {result_kind(result), 0, RESULT_E_NONE, 0, 1469598103934665603ull};
    result_row_t row;
    while (result_next(result, &row) == RESULT_NEXT_ROW) {
        s.rows++;
        for (size_t i = 0; i < row.cell_count; i++) {
            const value_view_t *v = &row.cells[i];
            s.hash = mix(s.hash, (uint64_t)v->type);
            s.hash = mix(s.hash, (uint64_t)v->integer);
            for (size_t b = 0; b < v->size_bytes; b++) s.hash = mix(s.hash, v->bytes[b]);
        }
    }
    result_outcome_t outcome = result_outcome(result);
    if (outcome.error != NULL) {
        s.error_class = outcome.error->error_class;
        s.code = outcome.error->code;
        s.hash = mix(s.hash, (uint64_t)s.error_class);
    }
    return s;
}

static database_exec_status_t exec_raw(session_t *session, const session_exec_options_t *options, const char *sql,
                                       result_t **out)
{
    *out = NULL;
    return session_exec(session, options, (const uint8_t *)sql, strlen(sql), NULL, out);
}

static summary_t run(session_t *session, const char *sql)
{
    result_t *result = NULL;
    database_exec_status_t status = exec_raw(session, NULL, sql, &result);
    summary_t s = {RESULT_KIND_ERROR, 0, RESULT_E_INTERNAL, 1000 + (int)status, 0};
    if (status == DATABASE_EXEC_OK) s = consume(result);
    result_release(&result);
    return s;
}

/* 풀·DB·세션과 준비된 데이터 */
typedef struct {
    session_pool_t *pool;
    session_db_t *db;
    session_t *session;
    size_t base_shared;
    size_t base_live;
} fixture_t;

static const char *const SETUP[] = {
    "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, note TEXT)",
    "INSERT INTO t (name, note) VALUES ('alpha', 'first row of the table')",
    "INSERT INTO t (name, note) VALUES ('beta', 'second row of the table')",
    "INSERT INTO t (name, note) VALUES ('gamma', NULL)",
    "INSERT INTO t (name, note) VALUES ('delta', 'fourth row of the table, a little longer than the rest')",
    "INSERT INTO t (name, note) VALUES ('epsilon', 'fifth')",
    "INSERT INTO t (name, note) VALUES ('zeta', 'sixth row')",
};

static void audit_begin(void)
{
    audit_reset();
    g_violations = 0;
    audit_on_alloc = on_alloc;
    audit_on_free = on_free;
}

static fixture_t open_fixture(const session_limits_t *limits, bool reopen)
{
    fixture_t f = {0};
    if (!reopen) fresh_dir();
    audit_begin();
    assert(session_pool_open(limits, &f.pool) == SESSION_OK);
    g_pool = f.pool;
    database_status_t ds;
    if (reopen) {
        assert(session_pool_open_database(f.pool, DB_PATH, true, &f.db, &ds) == SESSION_OK);
    } else {
        assert(session_pool_create_database(f.pool, DB_PATH, &f.db, &ds) == SESSION_OK);
    }
    assert(session_open(f.db, &f.session) == SESSION_OK);
    if (!reopen) {
        for (size_t i = 0; i < sizeof(SETUP) / sizeof(SETUP[0]); i++) {
            summary_t s = run(f.session, SETUP[i]);
            assert(s.kind == RESULT_KIND_COMMAND);
        }
    }
    f.base_shared = pool_stats(f.pool).shared_used;
    f.base_live = audit_live_now();
    return f;
}

/* 닫고 audit_live가 0인지(풀 구조체까지 해제됨) 본다 */
static void close_fixture(fixture_t *f, const char *what)
{
    CHECK(session_close(f->session) == SESSION_OK, "%s: 세션 닫기", what);
    database_status_t ds;
    CHECK(session_pool_close_database(f->db, &ds) == SESSION_OK, "%s: DB 닫기", what);
    session_pool_stats_t stats = pool_stats(f->pool);
    CHECK(stats.accounting_faults == 0, "%s: faults %llu", what, (unsigned long long)stats.accounting_faults);
    CHECK(session_pool_close(f->pool) == SESSION_OK, "%s: 풀 닫기", what);
    g_pool = NULL;
    CHECK(audit_live_now() == 0, "%s: 닫은 뒤 살아 있는 할당 %zu바이트", what, audit_live_now());
    CHECK(g_violations == 0, "%s: 반환·할당 순서 위반 %lu (%s)", what, g_violations, g_where);
}

/*
 * 문장 하나가 끝났을 때의 회수. 상태를 바꾸지 않은 문장(거절·취소·실패)은 shared와 살아 있는 할당이 기준으로
 * 돌아와야 한다(strict). 상태를 바꾼 성공은 카탈로그·해제 목록의 상주 몫이 달라질 수 있어 query·result가
 * 비었고 shared가 실제 할당과 같은 방향인지(불변식)만 보고 기준을 새로 잡는다.
 */
static void check_settled(fixture_t *f, const char *label, bool strict)
{
    session_pool_stats_t p = pool_stats(f->pool);
    session_stats_t s = sess_stats(f->session);
    CHECK(p.accounting_faults == 0, "%s: faults", label);
    CHECK(s.query_used == 0 && s.result_used == 0, "%s: 세션 몫 query %zu result %zu", label, s.query_used, s.result_used);
    CHECK(p.shared_used >= audit_live_now(), "%s: shared %zu < 살아 있는 할당 %zu", label, p.shared_used, audit_live_now());
    if (strict) {
        CHECK(p.shared_used == f->base_shared, "%s: shared %zu 기준 %zu", label, p.shared_used, f->base_shared);
        CHECK(audit_live_now() == f->base_live, "%s: 살아 있는 할당 %zu 기준 %zu", label, audit_live_now(), f->base_live);
    } else {
        f->base_shared = p.shared_used;
        f->base_live = audit_live_now();
    }
    CHECK(g_violations == 0, "%s: 순서 위반 %lu (%s)", label, g_violations, g_where);
}

static uint64_t snapshot(session_t *session)
{
    summary_t s = run(session, "SELECT * FROM t");
    return s.hash ^ (s.rows << 56);
}

/* ── 문장 종류 ── */
typedef struct {
    const char *name;
    const char *sql;
} case_t;

static const case_t CASES[] = {
    {"select", "SELECT * FROM t"},
    {"select where", "SELECT name FROM t WHERE id > 2"},
    {"insert", "INSERT INTO t (name, note) VALUES ('eta', 'a freshly inserted row with some text')"},
    {"update", "UPDATE t SET note = 'rewritten note that is longer than before, to force a page rewrite' WHERE id <= 3"},
    {"delete", "DELETE FROM t WHERE id = 2"},
    {"create", "CREATE TABLE extra (x INTEGER, y TEXT)"},
    {"drop", "DROP TABLE t"},
};
#define CASE_COUNT (sizeof(CASES) / sizeof(CASES[0]))

/* 문장을 한 번 깨끗하게 실행했을 때의 결과(기준) */
typedef struct {
    uint64_t before;
    uint64_t after;
    result_kind_t kind;
} reference_t;

static reference_t reference(const case_t *c)
{
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits, false);
    reference_t ref = {snapshot(f.session), 0, RESULT_KIND_ERROR};
    result_t *result = NULL;
    assert(exec_raw(f.session, NULL, c->sql, &result) == DATABASE_EXEC_OK);
    ref.kind = result_kind(result);
    result_release(&result);
    ref.after = snapshot(f.session);
    close_fixture(&f, "기준 실행");
    return ref;
}

/* ── 정상 경로 전체 audit ── */
static void test_full_audit(void)
{
    printf("전체 allocator audit: 정상 경로와 수명 순서\n");
    session_limits_t limits = big_limits();
    fixture_t f = open_fixture(&limits, false);
    for (size_t i = 0; i < CASE_COUNT; i++) {
        /* drop은 마지막 */
        if (strcmp(CASES[i].name, "drop") == 0) continue;
        summary_t s = run(f.session, CASES[i].sql);
        CHECK(s.kind != RESULT_KIND_ERROR, "%s", CASES[i].name);
    }
    CHECK(run(f.session, "SELECT broken FROM").error_class == RESULT_E_SYNTAX, "구문 오류");
    CHECK(g_violations == 0, "순서 위반 %lu (%s)", g_violations, g_where);

    /* 결과를 여럿 쥔 채 세션·DB를 닫고 역순으로 release */
    result_t *held[3] = {0};
    for (int i = 0; i < 3; i++) assert(exec_raw(f.session, NULL, "SELECT * FROM t", &held[i]) == DATABASE_EXEC_OK);
    CHECK(session_close(f.session) == SESSION_OK, "결과를 쥔 채 세션 닫기");
    database_status_t ds;
    CHECK(session_pool_close_database(f.db, &ds) == SESSION_OK, "결과를 쥔 채 DB 닫기");
    CHECK(session_pool_close(f.pool) == SESSION_BUSY, "남은 결과 때문에 풀은 BUSY");
    CHECK(g_violations == 0, "닫은 뒤 순서 위반 %lu (%s)", g_violations, g_where);
    result_release(&held[1]);
    result_release(&held[2]);
    CHECK(session_pool_close(f.pool) == SESSION_BUSY, "결과 하나가 남으면 BUSY");
    result_release(&held[0]);
    CHECK(pool_stats(f.pool).shared_used == pool_stats(f.pool).resident_used, "남은 몫은 풀 구조체뿐");
    CHECK(session_pool_close(f.pool) == SESSION_OK, "마지막 release 뒤 풀 닫기");
    g_pool = NULL;
    CHECK(audit_live_now() == 0, "모두 닫은 뒤 살아 있는 할당 %zu", audit_live_now());
    CHECK(g_violations == 0, "순서 위반 %lu (%s)", g_violations, g_where);
    clean_dir();
}

/* ── n번째 reserve 실패 sweep ── */
static void test_reserve_sweep(void)
{
    printf("n번째 reserve 실패 sweep\n");
    long total_points = 0;
    for (size_t ci = 0; ci < CASE_COUNT; ci++) {
        const case_t *c = &CASES[ci];
        reference_t ref = reference(c);
        long nomem = 0, ok = 0;
        for (long n = 1; n < 4000; n++) {
            session_limits_t limits = big_limits();
            fixture_t f = open_fixture(&limits, false);
            g_reserve_calls = 0;
            g_fail_n = n;
            result_t *result = NULL;
            database_exec_status_t status = exec_raw(f.session, NULL, c->sql, &result);
            bool hit = g_reserve_calls >= n;
            g_fail_n = 0;
            char label[96];
            snprintf(label, sizeof(label), "%s reserve %ld", c->name, n);
            if (!hit) {
                CHECK(status == DATABASE_EXEC_OK && result != NULL && result_kind(result) == ref.kind, "%s: 주입 없이 성공", label);
                result_release(&result);
                check_settled(&f, label, false);
                CHECK(snapshot(f.session) == ref.after, "%s: 성공 뒤 상태", label);
                close_fixture(&f, label);
                ok++;
                break;
            }
            total_points++;
            bool refused = status == DATABASE_EXEC_NOMEM;
            if (status == DATABASE_EXEC_OK && result != NULL && result_kind(result) == RESULT_KIND_ERROR) {
                summary_t s = consume(result);
                refused = s.error_class == RESULT_E_NOMEM;
            }
            CHECK(refused, "%s: 거절은 NOMEM이어야 함 (status %d)", label, (int)status);
            nomem++;
            result_release(&result);
            check_settled(&f, label, true);
            CHECK(snapshot(f.session) == ref.before, "%s: 거절된 문장은 아무것도 반영하지 않음", label);
            /* 후속 질의: 같은 문장이 다시 정상 실행된다 */
            result_t *again = NULL;
            CHECK(exec_raw(f.session, NULL, c->sql, &again) == DATABASE_EXEC_OK && result_kind(again) == ref.kind, "%s: 후속 실행", label);
            result_release(&again);
            check_settled(&f, label, false);
            CHECK(snapshot(f.session) == ref.after, "%s: 후속 실행 뒤 상태", label);
            close_fixture(&f, label);
        }
        printf("  %-13s 주입 지점 %ld곳, 정상 %ld\n", c->name, nomem, ok);
        CHECK(nomem > 0 && ok == 1, "%s: sweep이 끝났다", c->name);
    }
    CHECK(total_points > 100, "총 주입 지점 %ld", total_points);
    clean_dir();
}

/* ── I/O 실패 sweep ── */
typedef struct {
    int pread_at, pwrite_at, fsync_at;
    int pread_calls, pwrite_calls, fsync_calls;
} io_hooks_t;

static io_hooks_t g_io;

static ssize_t hook_pread(int fd, void *buf, size_t size, off_t offset)
{
    if (g_io.pread_at != 0 && ++g_io.pread_calls == g_io.pread_at) return -1;
    return pread(fd, buf, size, offset);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (g_io.pwrite_at != 0 && ++g_io.pwrite_calls == g_io.pwrite_at) return -1;
    return pwrite(fd, buf, size, offset);
}

static int hook_fsync(int fd)
{
    if (g_io.fsync_at != 0 && ++g_io.fsync_calls == g_io.fsync_at) return -1;
    return fsync(fd);
}

static const page_file_io_t HOOKED_IO = {hook_pread, hook_pwrite, hook_fsync, link, unlink};

static void test_io_sweep(void)
{
    printf("n번째 pread·pwrite·fsync 실패 sweep\n");
    /* 문장마다 파일 읽기 실패 경로를 모두 지나가게 하려고 읽기 캐시를 끈다(캐시가 있으면 적중한 읽기는 pread가 없다) */
    page_file_set_default_cache_pages(0);
    static const char *const KINDS[] = {"pread", "pwrite", "fsync"};
    long total_points = 0;
    for (size_t ci = 0; ci < CASE_COUNT; ci++) {
        const case_t *c = &CASES[ci];
        if (strcmp(c->name, "select") == 0 || strcmp(c->name, "select where") == 0) {
            /* 읽기 문장도 pread 실패는 있다. 같은 틀로 본다 */
        }
        page_file_set_default_io(NULL);
        reference_t ref = reference(c);
        long points = 0, poisoned_total = 0;
        for (int kind = 0; kind < 3; kind++) {
            for (int n = 1; n < 400; n++) {
                page_file_set_default_io(&HOOKED_IO);
                memset(&g_io, 0, sizeof(g_io));
                session_limits_t limits = big_limits();
                fixture_t f = open_fixture(&limits, false);
                database_t *dbh = session_db_handle(f.db);
                if (kind == 0) g_io.pread_at = n;
                if (kind == 1) g_io.pwrite_at = n;
                if (kind == 2) g_io.fsync_at = n;
                result_t *result = NULL;
                database_exec_status_t status = exec_raw(f.session, NULL, c->sql, &result);
                bool hit = (kind == 0 && g_io.pread_calls >= n) || (kind == 1 && g_io.pwrite_calls >= n) ||
                           (kind == 2 && g_io.fsync_calls >= n);
                memset(&g_io, 0, sizeof(g_io));
                char label[96];
                snprintf(label, sizeof(label), "%s %s %d", c->name, KINDS[kind], n);
                if (!hit) {
                    CHECK(status == DATABASE_EXEC_OK && result_kind(result) == ref.kind, "%s: 주입 없이 성공", label);
                    result_release(&result);
                    check_settled(&f, label, false);
                    close_fixture(&f, label);
                    break;
                }
                points++;
                bool poisoned = database_is_poisoned(dbh);
                poisoned_total += poisoned;
                bool succeeded = false;
                if (status == DATABASE_EXEC_OK && result != NULL && result_kind(result) == ref.kind) succeeded = true;
                result_release(&result);
                check_settled(&f, label, !succeeded);  /* 오류든 불확정이든 자원은 모두 돌아온다 */
                if (!poisoned) {
                    /* 후속 질의: 상태는 문장 전이거나(오류) 문장 후(성공)이고 같은 문장을 다시 실행할 수 있다 */
                    uint64_t now = snapshot(f.session);
                    CHECK(now == (succeeded ? ref.after : ref.before), "%s: 핸들이 정상이면 응답과 상태가 일치", label);
                    if (!succeeded) {
                        result_t *again = NULL;
                        CHECK(exec_raw(f.session, NULL, c->sql, &again) == DATABASE_EXEC_OK && result_kind(again) == ref.kind,
                              "%s: 같은 문장을 다시 실행", label);
                        result_release(&again);
                        check_settled(&f, label, false);
                        CHECK(snapshot(f.session) == ref.after, "%s: 다시 실행한 뒤 상태", label);
                    }
                    close_fixture(&f, label);
                } else {
                    /* poison: 이후 모든 문장은 오류이고 자원은 여전히 돌아온다. 다시 열면 문장 전 또는 후 */
                    summary_t next = run(f.session, "SELECT * FROM t");
                    CHECK(next.kind == RESULT_KIND_ERROR, "%s: poison된 핸들은 문장을 거절", label);
                    check_settled(&f, label, false);
                    CHECK(session_close(f.session) == SESSION_OK, "%s: 세션 닫기", label);
                    database_status_t ds;
                    CHECK(session_pool_close_database(f.db, &ds) == SESSION_OK, "%s: poison된 DB 닫기", label);
                    page_file_set_default_io(NULL);
                    session_db_t *db2;
                    session_t *s2;
                    CHECK(session_pool_open_database(f.pool, DB_PATH, true, &db2, &ds) == SESSION_OK, "%s: 다시 열기", label);
                    if (db2 != NULL) {
                        CHECK(session_open(db2, &s2) == SESSION_OK, "%s: 세션", label);
                        uint64_t now = snapshot(s2);
                        CHECK(now == ref.before || now == ref.after, "%s: 다시 열면 문장 전 또는 후", label);
                        CHECK(session_close(s2) == SESSION_OK, "%s: 세션 닫기", label);
                        CHECK(session_pool_close_database(db2, &ds) == SESSION_OK, "%s: DB 닫기", label);
                    }
                    CHECK(pool_stats(f.pool).accounting_faults == 0 && g_violations == 0, "%s: 회계", label);
                    CHECK(session_pool_close(f.pool) == SESSION_OK, "%s: 풀 닫기", label);
                    g_pool = NULL;
                    CHECK(audit_live_now() == 0, "%s: poison 경로 뒤 살아 있는 할당 %zu", label, audit_live_now());
                }
            }
        }
        total_points += points;
        printf("  %-13s 주입 지점 %ld곳 (poison %ld)\n", c->name, points, poisoned_total);
    }
    page_file_set_default_io(NULL);
    page_file_set_default_cache_pages(PAGE_FILE_CACHE_PAGES);
    CHECK(total_points > 50, "총 주입 지점 %ld", total_points);
    clean_dir();
}

/* ── n번째 취소 확인 sweep ── */
typedef struct {
    long calls;
    long fire_at;
} cancel_probe_t;

static bool cancel_probe(void *ctx)
{
    cancel_probe_t *p = ctx;
    return ++p->calls == p->fire_at;
}

static void test_cancel_sweep(void)
{
    printf("n번째 취소 확인 sweep\n");
    for (size_t ci = 0; ci < CASE_COUNT; ci++) {
        const case_t *c = &CASES[ci];
        reference_t ref = reference(c);
        long points = 0;
        for (long n = 1; n < 500; n++) {
            session_limits_t limits = big_limits();
            fixture_t f = open_fixture(&limits, false);
            cancel_probe_t probe = {0, n};
            database_cancel_t cancel = {cancel_probe, &probe};
            session_exec_options_t options = {&cancel};
            result_t *result = NULL;
            database_exec_status_t status = exec_raw(f.session, &options, c->sql, &result);
            bool fired = probe.calls >= n;
            char label[96];
            snprintf(label, sizeof(label), "%s cancel %ld", c->name, n);
            CHECK(status == DATABASE_EXEC_OK && result != NULL, "%s: 결과가 있다", label);
            if (!fired) {
                CHECK(result_kind(result) == ref.kind, "%s: 취소 없이 성공", label);
                result_release(&result);
                check_settled(&f, label, false);
                CHECK(snapshot(f.session) == ref.after, "%s: 성공 뒤 상태", label);
                close_fixture(&f, label);
                break;
            }
            points++;
            summary_t s = consume(result);
            CHECK(s.kind == RESULT_KIND_ERROR && s.error_class == RESULT_E_CANCELLED && s.code == DATABASE_CODE_CANCELLED,
                  "%s: 취소 결과 (kind %d class %d)", label, s.kind, s.error_class);
            result_release(&result);
            check_settled(&f, label, true);
            CHECK(snapshot(f.session) == ref.before, "%s: 취소된 문장은 아무것도 반영하지 않음", label);
            result_t *again = NULL;
            CHECK(exec_raw(f.session, NULL, c->sql, &again) == DATABASE_EXEC_OK && result_kind(again) == ref.kind, "%s: 후속 실행", label);
            result_release(&again);
            check_settled(&f, label, false);
            CHECK(snapshot(f.session) == ref.after, "%s: 후속 실행 뒤 상태", label);
            close_fixture(&f, label);
        }
        printf("  %-13s 취소 지점 %ld곳\n", c->name, points);
    }
    clean_dir();
}

/* ── 열기·만들기·세션 열기의 reserve 실패 sweep ── */
static void test_open_sweep(void)
{
    printf("열기·만들기·세션 열기의 n번째 reserve 실패 sweep\n");
    long create_points = 0, open_points = 0, session_points = 0;
    for (int mode = 0; mode < 3; mode++) {
        for (long n = 1; n < 400; n++) {
            fresh_dir();
            session_limits_t limits = big_limits();
            if (mode == 1) {
                /* 열 파일을 만들어 둔다 */
                fixture_t f = open_fixture(&limits, false);
                close_fixture(&f, "준비");
            }
            audit_begin();
            session_pool_t *pool;
            assert(session_pool_open(&limits, &pool) == SESSION_OK);
            g_pool = pool;
            size_t pool_shared = pool_stats(pool).shared_used;
            size_t pool_live = audit_live_now();
            session_db_t *db = NULL;
            database_status_t ds;
            g_reserve_calls = 0;
            g_fail_n = n;
            session_status_t st;
            if (mode == 0) st = session_pool_create_database(pool, DB_PATH, &db, &ds);
            else if (mode == 1) st = session_pool_open_database(pool, DB_PATH, true, &db, &ds);
            else {
                g_fail_n = 0;
                fresh_dir();
                st = session_pool_create_database(pool, DB_PATH, &db, &ds);
                assert(st == SESSION_OK);
                pool_shared = pool_stats(pool).shared_used;
                pool_live = audit_live_now();
                g_reserve_calls = 0;
                g_fail_n = n;
            }
            session_t *sess = NULL;
            bool hit;
            char label[64];
            snprintf(label, sizeof(label), "mode %d reserve %ld", mode, n);
            if (mode == 2) {
                st = session_open(db, &sess);
                hit = g_reserve_calls >= n;
                g_fail_n = 0;
                if (!hit) {
                    CHECK(st == SESSION_OK, "%s: 정상", label);
                    CHECK(session_close(sess) == SESSION_OK, "%s", label);
                    CHECK(session_pool_close_database(db, &ds) == SESSION_OK, "%s", label);
                    CHECK(session_pool_close(pool) == SESSION_OK, "%s", label);
                    g_pool = NULL;
                    CHECK(audit_live_now() == 0 && g_violations == 0, "%s: 정리", label);
                    break;
                }
                session_points++;
                CHECK(st == SESSION_NOMEM && sess == NULL, "%s: 세션 거절", label);
                CHECK(pool_stats(pool).shared_used == pool_shared && pool_stats(pool).sessions_open == 0, "%s: 몫 복구", label);
                CHECK(audit_live_now() == pool_live, "%s: 할당 복구", label);
                CHECK(session_pool_close_database(db, &ds) == SESSION_OK, "%s", label);
            } else {
                hit = g_reserve_calls >= n;
                g_fail_n = 0;
                if (!hit) {
                    CHECK(st == SESSION_OK, "%s: 정상", label);
                    CHECK(session_pool_close_database(db, &ds) == SESSION_OK, "%s", label);
                    CHECK(session_pool_close(pool) == SESSION_OK, "%s", label);
                    g_pool = NULL;
                    CHECK(audit_live_now() == 0 && g_violations == 0, "%s: 정리", label);
                    break;
                }
                if (mode == 0) create_points++;
                else open_points++;
                CHECK(st == SESSION_NOMEM && db == NULL, "%s: 거절 (st %d, ds %d)", label, (int)st, (int)ds);
                if (mode == 0) CHECK(!file_exists(), "%s: 거절된 create는 파일을 남기지 않는다", label);
                CHECK(pool_stats(pool).shared_used == pool_shared && pool_stats(pool).databases_open == 0, "%s: 몫 복구", label);
                CHECK(audit_live_now() == pool_live, "%s: 할당 복구", label);
            }
            CHECK(g_violations == 0, "%s: 순서 위반 %lu (%s)", label, g_violations, g_where);
            CHECK(session_pool_close(pool) == SESSION_OK, "%s: 풀 닫기", label);
            g_pool = NULL;
            CHECK(audit_live_now() == 0, "%s: 닫은 뒤 할당 %zu", label, audit_live_now());
        }
    }
    printf("  create %ld곳, open %ld곳, session %ld곳\n", create_points, open_points, session_points);
    CHECK(create_points > 1 && open_points > 1 && session_points >= 1, "주입 지점 수");
    clean_dir();
}

/* ── 강제 스케줄 ── */
typedef enum { OP_RESULT_RELEASE, OP_SESSION_CLOSE, OP_DB_CLOSE, OP_EXEC } op_t;

typedef struct {
    op_t op;
    fixture_t *f;
    result_t *held;
    session_db_t *db;
    session_t *session;
    database_exec_status_t exec_status;
    result_t *exec_result;
    session_status_t close_status;
} job_t;

static void *job_main(void *arg)
{
    job_t *j = arg;
    gate_wait_go();
    switch (j->op) {
        case OP_RESULT_RELEASE: result_release(&j->held); break;
        case OP_SESSION_CLOSE: j->close_status = session_close(j->session); break;
        case OP_DB_CLOSE: {
            database_status_t ds;
            j->close_status = session_pool_close_database(j->db, &ds);
            break;
        }
        case OP_EXEC: j->exec_status = exec_raw(j->session, NULL, "SELECT * FROM t", &j->exec_result); break;
    }
    gate_disarm();
    gate_finish();
    return NULL;
}

/* 시나리오를 만든다 */
static void build_job(job_t *j, fixture_t *f, op_t op)
{
    memset(j, 0, sizeof(*j));
    j->op = op;
    j->f = f;
    j->session = f->session;
    j->db = f->db;
    switch (op) {
        case OP_RESULT_RELEASE:
            assert(exec_raw(f->session, NULL, "SELECT * FROM t", &j->held) == DATABASE_EXEC_OK);
            assert(session_close(f->session) == SESSION_OK);
            f->session = NULL;
            {
                database_status_t ds;
                assert(session_pool_close_database(f->db, &ds) == SESSION_OK);
                f->db = NULL;
            }
            break;
        case OP_SESSION_CLOSE: break;
        case OP_DB_CLOSE:
            assert(session_close(f->session) == SESSION_OK);
            f->session = NULL;
            break;
        case OP_EXEC: break;
    }
}

static const char *OP_NAMES[] = {"result_release", "session_close", "database_close", "session_exec"};

static void test_forced_schedule(void)
{
    printf("강제 스케줄: free 직후 멈춘 스레드와 pool_close·session_close·후속 reserve\n");
    for (int op = 0; op < 4; op++) {
        /* 건수 세기 */
        long frees = 0;
        {
            session_limits_t limits = big_limits();
            fixture_t f = open_fixture(&limits, false);
            job_t job;
            build_job(&job, &f, (op_t)op);
            gate_arm(pthread_self(), 0);
            job_main(&job);  /* 같은 스레드에서 직접 실행하며 센다 */
            frees = g_gate.seen;
            pthread_mutex_lock(&g_gate.m);
            frees = g_gate.seen;
            pthread_mutex_unlock(&g_gate.m);
            if (op == OP_EXEC) result_release(&job.exec_result);
            if (f.session != NULL && op != OP_SESSION_CLOSE) assert(session_close(f.session) == SESSION_OK);
            if (op == OP_SESSION_CLOSE) f.session = NULL;
            if (f.db != NULL && op != OP_DB_CLOSE) {
                database_status_t ds;
                assert(session_pool_close_database(f.db, &ds) == SESSION_OK);
            }
            assert(session_pool_close(f.pool) == SESSION_OK);
            g_pool = NULL;
            CHECK(audit_live_now() == 0, "%s: 건수 세기 뒤 할당 %zu", OP_NAMES[op], audit_live_now());
        }
        printf("  %-15s free %ld회마다 멈춤\n", OP_NAMES[op], frees);
        CHECK(frees > 0, "%s: free 지점", OP_NAMES[op]);

        for (long k = 1; k <= frees; k++) {
            session_limits_t limits = big_limits();
            fixture_t f = open_fixture(&limits, false);
            job_t job;
            build_job(&job, &f, (op_t)op);
            char label[96];
            snprintf(label, sizeof(label), "%s k=%ld/%ld", OP_NAMES[op], k, frees);
            pthread_t thread;
            pthread_mutex_lock(&g_gate.m);
            g_gate.active = false;
            g_gate.go = false;
            g_gate.stop_at = k;
            g_gate.seen = 0;
            g_gate.paused = g_gate.resume = g_gate.finished = false;
            pthread_mutex_unlock(&g_gate.m);
            assert(pthread_create(&thread, NULL, job_main, &job) == 0);
            pthread_mutex_lock(&g_gate.m);
            g_gate.target = thread;
            g_gate.active = true;
            g_gate.go = true;
            pthread_cond_broadcast(&g_gate.c);
            pthread_mutex_unlock(&g_gate.m);
            bool paused = gate_wait_paused();
            if (!paused) {
                /* 목표 스레드가 문을 켜기 전에 끝났거나 지점이 줄었다. 끝까지 기다리고 정리 */
                pthread_join(thread, NULL);
                CHECK(false, "%s: 멈추지 못했다", label);
                if (op == OP_EXEC) result_release(&job.exec_result);
                goto cleanup;
            }

            /* ── 멈춘 순간(A는 어떤 메모리를 막 해제했고 몫은 아직 돌려주지 않았을 수 있다) ── */
            {
                session_pool_stats_t stats = pool_stats(f.pool);
                CHECK(stats.shared_used >= audit_live_now(), "%s: 멈춘 순간 shared %zu < 살아 있는 할당 %zu", label,
                      stats.shared_used, audit_live_now());
                CHECK(session_pool_close(f.pool) == SESSION_BUSY, "%s: 정리가 끝나기 전 pool_close는 BUSY", label);
                if (op == OP_EXEC) {
                    CHECK(session_close(f.session) == SESSION_BUSY, "%s: 실행 중인 세션은 닫히지 않는다", label);
                }
                if (op == OP_SESSION_CLOSE || op == OP_DB_CLOSE) {
                    /* 닫는 중인 대상 밖의 자원으로 다음 reserve를 시도한다 */
                }
            }
            gate_resume();
            pthread_join(thread, NULL);

            cleanup:
            switch (op) {
                case OP_RESULT_RELEASE: break;
                case OP_SESSION_CLOSE:
                    CHECK(job.close_status == SESSION_OK, "%s: close 결과", label);
                    f.session = NULL;
                    break;
                case OP_DB_CLOSE:
                    CHECK(job.close_status == SESSION_OK, "%s: DB close 결과", label);
                    f.db = NULL;
                    break;
                case OP_EXEC:
                    CHECK(job.exec_status == DATABASE_EXEC_OK && job.exec_result != NULL, "%s: 실행 결과", label);
                    if (job.exec_result != NULL) {
                        CHECK(consume(job.exec_result).rows == 6, "%s: 행", label);
                        result_release(&job.exec_result);
                    }
                    break;
            }
            CHECK(g_violations == 0, "%s: 순서 위반 %lu (%s)", label, g_violations, g_where);
            if (f.session != NULL) assert(session_close(f.session) == SESSION_OK);
            if (f.db != NULL) {
                database_status_t ds;
                assert(session_pool_close_database(f.db, &ds) == SESSION_OK);
            }
            CHECK(pool_stats(f.pool).accounting_faults == 0, "%s: faults", label);
            CHECK(session_pool_close(f.pool) == SESSION_OK, "%s: 끝난 뒤 풀 닫기", label);
            g_pool = NULL;
            CHECK(audit_live_now() == 0, "%s: 끝난 뒤 할당 %zu", label, audit_live_now());
        }
    }
    clean_dir();
}

/* ── 멈춘 사이의 다음 reserve ──
 * 결과 하나를 쥔 세션 A의 release가 free 직후 멈춘 동안 같은 풀의 세션 B가 질의한다. 한도는 결과를 쥔 상태에
 * 질의 peak의 절반만 더한 값이라 A의 몫이 돌아오기 전에는 B의 질의가 다 들어가지 못한다. 반납이 해제보다
 * 앞서면 B가 아직 살아 있는 A의 메모리 위에 reserve해 audit 훅이 잡는다. */
typedef struct {
    session_t *a;
    result_t *held;
} release_job_t;

static void *release_main(void *arg)
{
    release_job_t *j = arg;
    gate_wait_go();
    result_release(&j->held);
    gate_disarm();
    gate_finish();
    return NULL;
}

static void test_next_reserve_during_release(void)
{
    printf("반납과 다음 reserve 사이의 역전 검사\n");
    session_limits_t big = big_limits();
    size_t base, held, query_peak, open_peak;
    {
        fixture_t f = open_fixture(&big, false);
        close_fixture(&f, "준비");
        f = open_fixture(&big, true);
        open_peak = pool_stats(f.pool).shared_peak;
        session_t *other;
        assert(session_open(f.db, &other) == SESSION_OK);
        base = pool_stats(f.pool).shared_used;
        run(f.session, "SELECT * FROM t");
        query_peak = sess_stats(f.session).last_query_peak;
        result_t *keep = NULL;
        assert(exec_raw(f.session, NULL, "SELECT * FROM t", &keep) == DATABASE_EXEC_OK);
        held = sess_stats(f.session).result_used;
        result_release(&keep);
        assert(session_close(other) == SESSION_OK);
        close_fixture(&f, "측정");
    }
    CHECK(held > 0 && query_peak >= held, "측정 result %zu peak %zu", held, query_peak);
    /* base + peak <= 한도 < base + held + peak: 결과를 쥐면 B의 질의(peak)가 들어가지 못하고 반납하면 들어간다 */
    session_limits_t tight = big;
    tight.shared_limit = base + query_peak + held / 2;
    if (tight.shared_limit < open_peak) tight.shared_limit = open_peak;
    long frees = 0;
    for (long k = 0; k <= frees; k++) {
        fixture_t f = open_fixture(&tight, true);
        session_t *b;
        /* B의 구조체 몫이 들어가야 한다. 한도는 그만큼 여유가 있다고 보고 거절되면 시험 가정이 깨진 것이다 */
        session_status_t opened = session_open(f.db, &b);
        CHECK(opened == SESSION_OK, "B 세션 열기 (%d)", (int)opened);
        if (opened != SESSION_OK) {
            close_fixture(&f, "B 실패");
            break;
        }
        release_job_t job = {f.session, NULL};
        assert(exec_raw(f.session, NULL, "SELECT * FROM t", &job.held) == DATABASE_EXEC_OK);
        char label[64];
        snprintf(label, sizeof(label), "release k=%ld/%ld", k, frees);
        if (k == 0) {
            /* 건수 세기 */
            gate_arm(pthread_self(), 0);
            release_main(&job);
            pthread_mutex_lock(&g_gate.m);
            frees = g_gate.seen;
            pthread_mutex_unlock(&g_gate.m);
            CHECK(frees > 0, "release의 free 지점 %ld", frees);
            printf("  result_release free %ld회마다 멈춤\n", frees);
        } else {
            pthread_t thread;
            pthread_mutex_lock(&g_gate.m);
            g_gate.active = false;
            g_gate.go = false;
            g_gate.stop_at = k;
            g_gate.seen = 0;
            g_gate.paused = g_gate.resume = g_gate.finished = false;
            pthread_mutex_unlock(&g_gate.m);
            assert(pthread_create(&thread, NULL, release_main, &job) == 0);
            pthread_mutex_lock(&g_gate.m);
            g_gate.target = thread;
            g_gate.active = true;
            g_gate.go = true;
            pthread_cond_broadcast(&g_gate.c);
            pthread_mutex_unlock(&g_gate.m);
            bool paused = gate_wait_paused();
            CHECK(paused, "%s: 멈춤", label);
            if (paused) {
                /* 멈춘 동안 B가 질의한다. 실행 또는 NOMEM 거절뿐이고 순서 위반이 없어야 한다 */
                result_t *second = NULL;
                database_exec_status_t status = exec_raw(b, NULL, "SELECT * FROM t", &second);
                if (status == DATABASE_EXEC_OK && second != NULL) {
                    summary_t s = consume(second);
                    CHECK((s.kind == RESULT_KIND_ROWS && s.rows == 6) ||
                              (s.kind == RESULT_KIND_ERROR && s.error_class == RESULT_E_NOMEM),
                          "%s: B의 결과는 실행 또는 NOMEM (kind %d class %d)", label, s.kind, s.error_class);
                } else {
                    CHECK(status == DATABASE_EXEC_NOMEM, "%s: B 상태 %d", label, (int)status);
                }
                result_release(&second);
                CHECK(g_violations == 0, "%s: 멈춘 사이 순서 위반 %lu (%s)", label, g_violations, g_where);
                gate_resume();
            }
            pthread_join(thread, NULL);
        }
        CHECK(sess_stats(f.session).result_used == 0, "%s: A의 결과 몫 반환", label);
        summary_t after = run(b, "SELECT * FROM t");
        CHECK(after.kind == RESULT_KIND_ROWS && after.rows == 6, "%s: 반납 뒤 B 질의가 돈다", label);
        CHECK(g_violations == 0, "%s: 순서 위반 %lu (%s)", label, g_violations, g_where);
        CHECK(session_close(b) == SESSION_OK, "%s: B 닫기", label);
        close_fixture(&f, label);
    }
    clean_dir();
}

int main(void)
{
    /* 치환하지 않고 빌드했다면 첫 풀이 audit를 지나지 않아 audit_live가 0으로 남아 시작할 때 실패한다 */
    test_full_audit();
    test_reserve_sweep();
    test_io_sweep();
    test_cancel_sweep();
    test_open_sweep();
    test_forced_schedule();
    test_next_reserve_during_release();
    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
