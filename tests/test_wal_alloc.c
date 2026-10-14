/*
 * test_wal_alloc.c -- WAL 힙 예산 감사 검사 (#9)
 *
 * wal.c를 직접 포함하고 malloc·calloc·realloc·free를 감사 함수로 바꿔, WAL이 실제로 받은 힙과
 * WAL 자신의 계상(wal_memory_used·wal_memory_peak)을 독립으로 대조한다. qsort는 숨은 힙을 쓸 수 있으므로
 * 호출하면 즉시 실패한다. realloc은 old와 new가 함께 살아 있는 것으로 센다(보수적 상한).
 *
 *  1. 정지 지점마다 wal_memory_used == 실제 살아 있는 바이트
 *  2. 실제 최대 힙 <= wal_memory_peak, 그리고 지정한 예산 이하
 *  3. 예산 sweep: 어떤 예산에서도 OK 또는 WAL_LIMIT뿐이고, 거절 뒤 파일은 그대로이며 누수가 없고
 *     다음 호출이 정상 동작한다
 *  4. 할당자 실패 주입: n번째 할당 실패마다 NOMEM·LIMIT뿐이고 상태가 불변이며 다음 호출이 정상 동작한다
 *  5. out이 NULL인 공개 함수는 WAL_INVALID
 *
 * 빌드(저장소 루트에서):
 *   cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=undefined -Iinclude \
 *      -o build/test_wal_alloc tests/test_wal_alloc.c
 * 실행은 build 폴더 안에서 한다: (cd build && ./test_wal_alloc)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG "__test__wal_alloc.log"
#define MAX_LIVE 4096

typedef struct {
    void *ptr;
    size_t size;
} live_t;

static live_t g_live[MAX_LIVE];
static size_t g_live_n;
static uint64_t g_live_bytes, g_peak_bytes;
static long g_fail_countdown; /* 0이면 주입 없음. 1이면 다음 할당 실패 */
static long g_alloc_count;
static bool g_forbidden_hit;

static void note_alloc(void *p, size_t n)
{
    for (size_t i = 0; i < MAX_LIVE; i++) {
        if (g_live[i].ptr == NULL) {
            g_live[i].ptr = p;
            g_live[i].size = n;
            g_live_n++;
            g_live_bytes += n;
            if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
            return;
        }
    }
    fprintf(stderr, "감사 표가 가득 찼다\n");
    abort();
}

static size_t note_free(void *p)
{
    for (size_t i = 0; i < MAX_LIVE; i++) {
        if (g_live[i].ptr == p) {
            size_t n = g_live[i].size;
            g_live[i].ptr = NULL;
            g_live_n--;
            g_live_bytes -= n;
            return n;
        }
    }
    fprintf(stderr, "감사하지 않은 포인터를 free했다\n");
    abort();
}

static bool inject_fail(void)
{
    g_alloc_count++;
    if (g_fail_countdown > 0 && --g_fail_countdown == 0) {
        errno = ENOMEM;
        return true;
    }
    return false;
}

static void *audit_malloc(size_t n)
{
    if (inject_fail()) return NULL;
    void *p = malloc(n);
    if (p != NULL) note_alloc(p, n);
    return p;
}

static void *audit_calloc(size_t c, size_t n)
{
    if (inject_fail()) return NULL;
    void *p = calloc(c, n);
    if (p != NULL) note_alloc(p, c * n);
    return p;
}

static void *audit_realloc(void *old, size_t n)
{
    if (inject_fail()) return NULL;
    void *p = malloc(n); /* old와 new가 함께 살아 있는 순간을 센다 */
    if (p == NULL) return NULL;
    note_alloc(p, n);
    if (old != NULL) {
        size_t on = 0;
        for (size_t i = 0; i < MAX_LIVE; i++) {
            if (g_live[i].ptr == old) on = g_live[i].size;
        }
        memcpy(p, old, on < n ? on : n);
        note_free(old);
        free(old);
    }
    return p;
}

static void audit_free(void *p)
{
    if (p == NULL) return;
    note_free(p);
    free(p);
}

static void audit_qsort(void *b, size_t n, size_t s, int (*c)(const void *, const void *))
{
    (void)b;
    (void)n;
    (void)s;
    (void)c;
    g_forbidden_hit = true;
}

#define malloc audit_malloc
#define calloc audit_calloc
#define realloc audit_realloc
#define free audit_free
#define qsort audit_qsort
#include "../src/storage/wal.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef qsort

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

static void audit_reset(void)
{
    g_peak_bytes = g_live_bytes;
    g_alloc_count = 0;
    g_fail_countdown = 0;
}

/* 파일을 통째로 읽는다. 실제 malloc을 쓰므로 감사 표에 들어가지 않는다. */
static bool read_file(const char *path, uint8_t **buf, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return false;
    }
    *len = (size_t)sb.st_size;
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

static void write_file(const char *path, const uint8_t *buf, size_t len)
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

static bool file_equals(const char *path, const uint8_t *orig, size_t olen)
{
    uint8_t *cur = NULL;
    size_t clen = 0;
    bool ok = read_file(path, &cur, &clen) && clen == olen && memcmp(cur, orig, olen) == 0;
    free(cur);
    return ok;
}

typedef struct {
    wal_t *w;
    bool mid_ok;
    long calls;
} rctx_t;

static void mid_check(rctx_t *x)
{
    /* 실행 중에는 charge가 free보다 앞서므로 계상 >= 실제 */
    if (wal_memory_used(x->w) < g_live_bytes) x->mid_ok = false;
    x->calls++;
}

static wal_redo_result_t redo_cb(void *c, const wal_record_t *r)
{
    (void)r;
    mid_check(c);
    return WAL_REDO_APPLIED;
}

static bool done_cb(void *c, wal_xid_t xid, wal_lsn_t lsn)
{
    (void)xid;
    (void)lsn;
    mid_check(c);
    return true;
}

static void state_cb(void *c, wal_xid_t xid, wal_xid_state_t st)
{
    (void)xid;
    (void)st;
    mid_check(c);
}

static wal_status_t do_replay(wal_t *w, wal_replay_mode_t mode, rctx_t *x)
{
    wal_replay_ops_t ops = {mode, 0, redo_cb, mode == WAL_REPLAY_COMMITTED_ONLY ? done_cb : NULL, state_cb};
    wal_replay_stats_t st;
    x->w = w;
    return wal_replay(w, &ops, x, &st);
}

/* 거래 200개(xid 표 성장), 150개짜리 큰 거래(쓰기 버퍼 성장), 끝 레코드 없는 거래를 가진 로그 */
static void build_log(void)
{
    unlink(LOG);
    wal_t *w = NULL;
    CHECK(wal_create(LOG, &w) == WAL_OK, "로그 생성");
    if (w == NULL) return;
    wal_lsn_t l = 0;
    char pl[100];
    memset(pl, 'p', sizeof(pl));
    for (wal_xid_t x = 1; x <= 200; x++) {
        for (uint32_t j = 0; j < 2; j++) wal_append(w, x, j, pl, 40, &l);
        if (x % 50 == 0) wal_abort(w, x, &l);
        else wal_commit(w, x, &l);
    }
    CHECK(wal_memory_used(w) == g_live_bytes, "빌드 중 계상 %llu 실제 %llu", (unsigned long long)wal_memory_used(w),
          (unsigned long long)g_live_bytes);
    for (uint32_t i = 0; i < 150; i++) wal_append(w, 201, i, pl, sizeof(pl), &l);
    wal_commit(w, 201, &l);
    wal_append(w, 202, 0, "x", 1, &l);
    wal_flush(w, l);
    CHECK(wal_memory_used(w) == g_live_bytes, "큰 거래 뒤 계상 %llu 실제 %llu", (unsigned long long)wal_memory_used(w),
          (unsigned long long)g_live_bytes);
    wal_close(w);
    CHECK(g_live_bytes == 0 && g_live_n == 0, "닫은 뒤 누수 %llu바이트", (unsigned long long)g_live_bytes);
}

/* ---- 1·2. 계상과 실제 대조 ---- */

static uint64_t test_audit_equality(void)
{
    printf("\n[계상과 실제 힙 대조]\n");
    build_log();
    audit_reset();
    wal_t *w = NULL;
    wal_open_info_t info;
    CHECK(wal_open(LOG, true, &w, &info) == WAL_OK, "쓰기 열기");
    if (w == NULL) return 0;
    CHECK(wal_memory_used(w) == g_live_bytes, "열기 뒤 계상 %llu 실제 %llu", (unsigned long long)wal_memory_used(w),
          (unsigned long long)g_live_bytes);
    CHECK(wal_memory_used(w) >= sizeof(wal_t), "핸들이 계상에 들어 있다");
    rctx_t rc = {w, true, 0};
    CHECK(do_replay(w, WAL_REPLAY_COMMITTED_ONLY, &rc) == WAL_OK, "COMMITTED_ONLY replay");
    CHECK(rc.mid_ok && rc.calls > 0, "replay 중 계상 >= 실제 (호출 %ld)", rc.calls);
    CHECK(wal_memory_used(w) == g_live_bytes, "replay 뒤 계상 %llu 실제 %llu", (unsigned long long)wal_memory_used(w),
          (unsigned long long)g_live_bytes);
    rc = (rctx_t){w, true, 0};
    CHECK(do_replay(w, WAL_REPLAY_ALL, &rc) == WAL_OK, "ALL replay");
    CHECK(rc.mid_ok && rc.calls > 0, "ALL 중 계상 >= 실제");
    CHECK(wal_memory_used(w) == g_live_bytes, "ALL 뒤 계상과 실제 일치");
    uint64_t wal_peak = wal_memory_peak(w);
    CHECK(g_peak_bytes <= wal_peak, "실제 최대 %llu <= 계상 최대 %llu", (unsigned long long)g_peak_bytes,
          (unsigned long long)wal_peak);
    CHECK(info.memory_peak <= wal_peak, "info.memory_peak <= 핸들 최대");
    CHECK(wal_close(w) == WAL_OK && g_live_bytes == 0, "닫은 뒤 누수 없음");
    CHECK(!g_forbidden_hit, "qsort를 부르지 않는다");

    audit_reset();
    CHECK(wal_open(LOG, false, &w, NULL) == WAL_OK, "읽기 열기");
    if (w != NULL) {
        CHECK(wal_memory_used(w) == g_live_bytes, "읽기 열기 뒤 계상과 실제 일치");
        wal_close(w);
    }
    CHECK(g_live_bytes == 0, "읽기 핸들 누수 없음");
    return wal_peak;
}

/* ---- 3. 예산 sweep ---- */

/* 거래 하나에 DATA 2000개: 열기 때 xid 표가 작아 replay의 lsn 배열이 가장 큰 할당이 된다 */
static uint64_t build_log2_peak(void)
{
    unlink(LOG);
    wal_t *w = NULL;
    CHECK(wal_create(LOG, &w) == WAL_OK, "두 번째 로그 생성");
    if (w == NULL) return 0;
    wal_lsn_t l = 0;
    for (uint32_t i = 0; i < 2000; i++) wal_append(w, 1, i, "datadata", 8, &l);
    CHECK(wal_commit(w, 1, &l) == WAL_OK, "두 번째 로그 commit");
    wal_close(w);
    audit_reset();
    CHECK(wal_open(LOG, true, &w, NULL) == WAL_OK, "두 번째 로그 열기");
    if (w == NULL) return 0;
    rctx_t rc = {w, true, 0};
    CHECK(do_replay(w, WAL_REPLAY_COMMITTED_ONLY, &rc) == WAL_OK, "두 번째 로그 replay");
    uint64_t peak = wal_memory_peak(w);
    CHECK(g_peak_bytes <= peak, "두 번째 로그 실제 최대 <= 계상 최대");
    wal_close(w);
    return peak;
}

static void test_budget_sweep(uint64_t full_peak, bool need_replay_limit)
{
    printf("\n[예산 sweep] 최대 %llu바이트\n", (unsigned long long)full_peak);
    uint8_t *orig = NULL;
    size_t olen = 0;
    CHECK(read_file(LOG, &orig, &olen), "원본 읽기");
    uint64_t limit_opens = 0, ok_opens = 0, limit_replays = 0, last_limit = 0;
    int bad = 0;
    for (uint64_t b = 1; b <= full_peak + 64; b += 7) {
        wal_limits_t lim = {b};
        audit_reset();
        wal_t *w = NULL;
        wal_open_info_t info;
        wal_status_t st = wal_open_with(LOG, true, &lim, &w, &info);
        if (st != WAL_OK && st != WAL_LIMIT) {
            printf("  budget=%llu 열기 상태 %s\n", (unsigned long long)b, wal_status_name(st));
            bad++;
        }
        if (st == WAL_LIMIT) {
            limit_opens++;
            last_limit = b;
            if (w != NULL || g_live_bytes != 0 || !file_equals(LOG, orig, olen) || g_peak_bytes > b) {
                printf("  budget=%llu 열기 거절 뒤 핸들·누수·파일·최대 불일치(최대 %llu)\n", (unsigned long long)b,
                       (unsigned long long)g_peak_bytes);
                bad++;
            }
            continue;
        }
        if (st != WAL_OK) continue;
        ok_opens++;
        wal_replay_mode_t modes[2] = {WAL_REPLAY_COMMITTED_ONLY, WAL_REPLAY_ALL};
        for (int m = 0; m < 2; m++) {
            rctx_t rc = {w, true, 0};
            wal_status_t rs = do_replay(w, modes[m], &rc);
            if (rs != WAL_OK && rs != WAL_LIMIT) {
                printf("  budget=%llu replay 상태 %s\n", (unsigned long long)b, wal_status_name(rs));
                bad++;
            }
            if (rs == WAL_LIMIT) limit_replays++;
            if (!rc.mid_ok || wal_memory_used(w) != g_live_bytes) {
                printf("  budget=%llu replay %d 뒤 계상 %llu 실제 %llu\n", (unsigned long long)b, m,
                       (unsigned long long)wal_memory_used(w), (unsigned long long)g_live_bytes);
                bad++;
            }
        }
        if (g_peak_bytes > b) {
            printf("  budget=%llu 인데 실제 최대 %llu\n", (unsigned long long)b, (unsigned long long)g_peak_bytes);
            bad++;
        }
        wal_close(w);
        if (g_live_bytes != 0) {
            printf("  budget=%llu 닫은 뒤 누수 %llu\n", (unsigned long long)b, (unsigned long long)g_live_bytes);
            bad++;
        }
    }
    CHECK(bad == 0, "예산 sweep 어긋남 %d", bad);
    CHECK(limit_opens > 0 && ok_opens > 0 && (!need_replay_limit || limit_replays > 0), "경로가 관측됐다 (열기 거절 %llu, 열기 성공 %llu, replay 거절 %llu)",
          (unsigned long long)limit_opens, (unsigned long long)ok_opens, (unsigned long long)limit_replays);
    CHECK(!g_forbidden_hit, "sweep 중 qsort를 부르지 않는다");

    /* 거절된 뒤 다음 호출이 정상이다 */
    wal_limits_t lim = {last_limit};
    wal_t *w = NULL;
    CHECK(wal_open_with(LOG, true, &lim, &w, NULL) == WAL_LIMIT, "마지막 거절 예산은 다시 해도 거절");
    CHECK(wal_open(LOG, true, &w, NULL) == WAL_OK, "거절 뒤 기본 예산으로 열기");
    if (w != NULL) {
        wal_lsn_t l;
        CHECK(wal_append(w, 5000, 0, "n", 1, &l) == WAL_OK && wal_commit(w, 5000, &l) == WAL_OK,
              "거절 뒤 다음 거래가 commit된다");
        wal_close(w);
    }
    write_file(LOG, orig, olen);

    /* 만들기: 거절되면 파일이 생기지 않는다 */
    int cbad = 0;
    uint64_t climit = 0, cok = 0;
    for (uint64_t b = 1; b <= 4096; b += 3) {
        unlink(LOG);
        wal_limits_t cl = {b};
        wal_t *cw = NULL;
        audit_reset();
        wal_status_t st = wal_create_with(LOG, &cl, &cw);
        if (st == WAL_LIMIT) {
            climit++;
            if (cw != NULL || access(LOG, F_OK) == 0 || g_live_bytes != 0) cbad++;
        } else if (st == WAL_OK) {
            cok++;
            if (wal_memory_used(cw) != g_live_bytes || g_peak_bytes > b) cbad++;
            wal_close(cw);
            if (g_live_bytes != 0) cbad++;
        } else {
            cbad++;
        }
    }
    CHECK(cbad == 0 && climit > 0 && cok > 0, "wal_create_with 예산 sweep 어긋남 %d (거절 %llu, 성공 %llu)", cbad,
          (unsigned long long)climit, (unsigned long long)cok);
    write_file(LOG, orig, olen);
    free(orig);
}

/* ---- 4. 할당자 실패 주입 ---- */

static void test_alloc_failure(void)
{
    printf("\n[할당자 실패 주입]\n");
    uint8_t *orig = NULL;
    size_t olen = 0;
    CHECK(read_file(LOG, &orig, &olen), "원본 읽기");

    /* 열기와 두 replay를 거치는 전체 할당 수를 센다 */
    audit_reset();
    wal_t *w = NULL;
    CHECK(wal_open(LOG, true, &w, NULL) == WAL_OK, "기준 열기");
    rctx_t rc = {w, true, 0};
    do_replay(w, WAL_REPLAY_COMMITTED_ONLY, &rc);
    do_replay(w, WAL_REPLAY_ALL, &rc);
    wal_close(w);
    long total = g_alloc_count;
    CHECK(total > 4, "열기·replay의 할당 수 %ld", total);

    int bad = 0;
    long failed_opens = 0, failed_replays = 0;
    for (long k = 1; k <= total + 1; k++) {
        audit_reset();
        g_fail_countdown = k;
        w = NULL;
        wal_status_t st = wal_open(LOG, true, &w, NULL);
        if (st != WAL_OK) {
            failed_opens++;
            if ((st != WAL_NOMEM && st != WAL_LIMIT) || w != NULL || g_live_bytes != 0 || !file_equals(LOG, orig, olen)) {
                printf("  k=%ld 열기 실패 상태 %s 누수 %llu\n", k, wal_status_name(st), (unsigned long long)g_live_bytes);
                bad++;
            }
            continue;
        }
        wal_replay_mode_t modes[2] = {WAL_REPLAY_COMMITTED_ONLY, WAL_REPLAY_ALL};
        for (int m = 0; m < 2; m++) {
            rctx_t x = {w, true, 0};
            wal_status_t rs = do_replay(w, modes[m], &x);
            if (rs != WAL_OK) {
                failed_replays++;
                if (rs != WAL_NOMEM && rs != WAL_LIMIT) {
                    printf("  k=%ld replay %d 실패 상태 %s\n", k, m, wal_status_name(rs));
                    bad++;
                }
            }
            if (wal_memory_used(w) != g_live_bytes) {
                printf("  k=%ld replay %d 뒤 계상 %llu 실제 %llu\n", k, m, (unsigned long long)wal_memory_used(w),
                       (unsigned long long)g_live_bytes);
                bad++;
            }
        }
        g_fail_countdown = 0;
        /* 실패 뒤에도 같은 핸들로 replay가 다시 성공한다 */
        rctx_t again = {w, true, 0};
        if (do_replay(w, WAL_REPLAY_COMMITTED_ONLY, &again) != WAL_OK) {
            printf("  k=%ld 실패 뒤 replay가 성공하지 않는다\n", k);
            bad++;
        }
        wal_close(w);
        if (g_live_bytes != 0) {
            printf("  k=%ld 닫은 뒤 누수 %llu\n", k, (unsigned long long)g_live_bytes);
            bad++;
        }
    }
    CHECK(bad == 0 && failed_opens > 0 && failed_replays > 0,
          "열기·replay 실패 주입 어긋남 %d (열기 실패 %ld, replay 실패 %ld)", bad, failed_opens, failed_replays);

    /* 쓰기 경로: append·commit의 할당 실패는 상태를 바꾸지 않고, 주입을 풀면 같은 호출이 성공한다 */
    int wbad = 0;
    long wfail = 0;
    for (long k = 1; k <= 12; k++) {
        write_file(LOG, orig, olen);
        audit_reset();
        w = NULL;
        if (wal_open(LOG, true, &w, NULL) != WAL_OK) {
            wbad++;
            continue;
        }
        char pl[100];
        memset(pl, 'q', sizeof(pl));
        g_fail_countdown = k;
        wal_xid_t x = 1000;
        wal_lsn_t l;
        bool done_all = true;
        for (uint32_t i = 0; i < 150 && done_all; i++) {
            wal_status_t st = wal_append(w, x, i, pl, sizeof(pl), &l);
            if (st == WAL_OK) continue;
            wfail++;
            if ((st != WAL_NOMEM && st != WAL_LIMIT) || wal_memory_used(w) != g_live_bytes) {
                printf("  k=%ld append 실패 상태 %s\n", k, wal_status_name(st));
                wbad++;
            }
            g_fail_countdown = 0;
            if (wal_append(w, x, i, pl, sizeof(pl), &l) != WAL_OK) {
                wbad++;
                done_all = false;
            }
        }
        g_fail_countdown = k;
        wal_status_t cs = wal_commit(w, x, &l);
        if (cs != WAL_OK) {
            wfail++;
            if ((cs != WAL_NOMEM && cs != WAL_LIMIT) || wal_memory_used(w) != g_live_bytes) {
                printf("  k=%ld commit 실패 상태 %s\n", k, wal_status_name(cs));
                wbad++;
            }
            g_fail_countdown = 0;
            cs = wal_commit(w, x, &l);
        }
        g_fail_countdown = 0;
        if (cs != WAL_OK || wal_xid_state(w, x) != WAL_XID_COMMITTED) wbad++;
        wal_close(w);
        if (g_live_bytes != 0) wbad++;
        w = NULL;
        if (wal_open(LOG, false, &w, NULL) != WAL_OK || wal_xid_state(w, x) != WAL_XID_COMMITTED) {
            printf("  k=%ld 재열기에서 commit이 보이지 않는다\n", k);
            wbad++;
        }
        if (w != NULL) wal_close(w);
    }
    CHECK(wbad == 0 && wfail > 0, "쓰기 경로 실패 주입 어긋남 %d (실패 %ld)", wbad, wfail);
    write_file(LOG, orig, olen);
    free(orig);
}

/* ---- 5. out이 NULL ---- */

static void test_null_out(void)
{
    printf("\n[out이 NULL]\n");
    CHECK(wal_create(LOG, NULL) == WAL_INVALID, "wal_create(out=NULL)");
    CHECK(wal_create_with(LOG, NULL, NULL) == WAL_INVALID, "wal_create_with(out=NULL)");
    CHECK(wal_open(LOG, true, NULL, NULL) == WAL_INVALID, "wal_open(out=NULL)");
    CHECK(wal_open_with(LOG, false, NULL, NULL, NULL) == WAL_INVALID, "wal_open_with(out=NULL)");
    wal_t *w = (wal_t *)0x1;
    CHECK(wal_open_with(NULL, true, NULL, &w, NULL) == WAL_INVALID && w == NULL, "path=NULL은 out을 NULL로 둔다");
    CHECK(wal_create_with(NULL, NULL, &w) == WAL_INVALID && w == NULL, "create path=NULL");
}

int main(void)
{
    (void)audit_qsort; /* wal.c가 qsort를 쓰면 g_forbidden_hit로 잡힌다 */
    unlink(LOG);
    uint64_t full_peak = test_audit_equality();
    test_budget_sweep(full_peak, false);
    test_budget_sweep(build_log2_peak(), true);
    test_alloc_failure();
    test_null_out();
    unlink(LOG);
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
