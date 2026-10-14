/*
 * test_io.c -- 파일 I/O 오류 전파 테스트 (#78)
 *
 * pread/pwrite/fsync 자리에 실패·짧은 I/O·EINTR·EOF를 주입해 아래를 검증한다.
 *   1. 짧은 읽기와 EINTR은 이어 읽어 성공한다 (결과가 정상 실행과 같다).
 *   2. 문장 실행 중 읽기가 실패하면 SQL 결과가 오류(status=-1)로 돌아온다.
 *      실패 뒤 pin·래치가 남지 않고, 같은 pager로 다음 요청이 성공한다.
 *   3. 쓰기·fsync 실패는 flush_all의 반환값으로 나오고, 실패한 프레임은 dirty로
 *      남으며, 디스크의 헤더 페이지는 바뀌지 않는다.
 *   4. dirty 프레임 교체(eviction) 중 쓰기가 실패하면 그 프레임을 clean으로
 *      만들거나 재사용하지 않는다.
 *   5. B+ tree 분할·재균형 도중 실패: 변경 전이면 트리가 그대로이고, 변경 후이면
 *      pager가 poison되어 디스크의 마지막 상태가 보존된다.
 *
 * 데이터는 모두 이 파일이 만드는 합성 데이터다. WAL이 없으므로 flush 성공은
 * 트랜잭션 단위 내구성을 뜻하지 않는다. 이 테스트도 그것을 검증하지 않는다.
 *
 * 빌드: 다른 test_* 와 같은 방식으로 tests/test_io.c 와 $(OBJS) 를 함께 링크한다.
 * (Makefile 대상은 조정자가 추가한다)
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "storage/bptree.h"
#include "storage/pager.h"

#define PRISTINE "__test__io_pristine.db"
#define TREE_FULL "__test__io_tree_full.db"
#define TREE_TWO "__test__io_tree_two.db"
#define WORK "__test__io_work.db"
#define ROWS 700
#define SELECT_ALL "SELECT * FROM t WHERE age >= 0"

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

typedef enum { F_NONE, F_ERR, F_SHORT, F_EOF, F_ZERO, F_EINTR } fault_kind_t;

typedef struct {
    fault_kind_t kind;
    int at;       /* 이 번째 호출(1부터)에서 발동 */
    bool sticky;  /* true면 at번째 이후 모든 호출에서 발동 */
    int err;
    int calls;
    int fired;
} fault_t;

static fault_t g_rd, g_wr, g_sync;

static void faults_reset(void)
{
    memset(&g_rd, 0, sizeof(g_rd));
    memset(&g_wr, 0, sizeof(g_wr));
    memset(&g_sync, 0, sizeof(g_sync));
}

static void arm(fault_t *f, fault_kind_t kind, int at, bool sticky, int err)
{
    f->kind = kind;
    f->at = at;
    f->sticky = sticky;
    f->err = err;
    f->calls = 0;
    f->fired = 0;
}

static bool fault_hit(fault_t *f)
{
    f->calls++;
    if (f->kind == F_NONE) return false;
    if (f->calls == f->at || (f->sticky && f->calls > f->at)) {
        f->fired++;
        return true;
    }
    return false;
}

static ssize_t hook_pread(int fd, void *buf, size_t n, off_t off)
{
    if (fault_hit(&g_rd)) {
        switch (g_rd.kind) {
            case F_ERR:   errno = g_rd.err; return -1;
            case F_EINTR: errno = EINTR; return -1;
            case F_EOF:   return 0;
            case F_SHORT: return pread(fd, buf, n > 1 ? n / 2 : n, off);
            default:      break;
        }
    }
    return pread(fd, buf, n, off);
}

static ssize_t hook_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (fault_hit(&g_wr)) {
        switch (g_wr.kind) {
            case F_ERR:   errno = g_wr.err; return -1;
            case F_EINTR: errno = EINTR; return -1;
            case F_ZERO:  return 0;
            case F_SHORT: return pwrite(fd, buf, n > 1 ? n / 2 : n, off);
            default:      break;
        }
    }
    return pwrite(fd, buf, n, off);
}

static int hook_fsync(int fd)
{
    if (fault_hit(&g_sync) && g_sync.kind == F_ERR) {
        errno = g_sync.err;
        return -1;
    }
    return fsync(fd);
}

static const pager_io_t HOOKED_IO = {hook_pread, hook_pwrite, hook_fsync};

/* ── 보조 함수 ── */

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    assert(in && out);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        assert(fwrite(buf, 1, n, out) == n);
    }
    fclose(in);
    fclose(out);
}

/* 파일 앞 n바이트를 읽는다. 짧으면 나머지는 0 */
static void read_prefix(const char *path, uint8_t *buf, size_t n)
{
    memset(buf, 0, n);
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t got = fread(buf, 1, n, f);
    (void)got;
    fclose(f);
}

static int next_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    close(fd);
    return fd;
}

/* 모든 프레임에서 pin이 0이고 쓰기 래치를 잡을 수 있는지. 래치만 볼 때는 check_pins=false */
static bool frames_quiescent(pager_t *p, bool check_pins)
{
    for (int i = 0; i < MAX_FRAMES; i++) {
        frame_t *f = &p->frames[i];
        if (check_pins && f->is_valid && f->pin_count != 0) return false;
        if (pthread_rwlock_trywrlock(&f->latch) != 0) return false;
        pthread_rwlock_unlock(&f->latch);
    }
    return true;
}

static int dirty_frames(pager_t *p)
{
    int n = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (p->frames[i].is_valid && p->frames[i].is_dirty) n++;
    }
    return n;
}

/* 깨끗한 프레임을 모두 비워 다음 접근이 디스크 읽기가 되게 한다 */
static void drop_cache(pager_t *p)
{
    pthread_mutex_lock(&p->pager_mutex);
    for (int i = 0; i < MAX_FRAMES; i++) {
        assert(!p->frames[i].is_dirty && p->frames[i].pin_count == 0);
        p->frames[i].is_valid = false;
        p->frames[i].hash_next = -1;
    }
    for (int b = 0; b < FRAME_HASH_BUCKETS; b++) p->frame_buckets[b] = -1;
    pthread_mutex_unlock(&p->pager_mutex);
}

/* SQL을 실행해 status와 결과 본문(복사본)을 돌려준다. 반환값은 호출자가 free */
static char *run_sql(pager_t *p, const char *sql, int *status, char *msg, size_t msg_len)
{
    exec_result_t r = db_execute(p, sql);
    if (status) *status = r.status;
    if (msg) snprintf(msg, msg_len, "%s", r.message);
    char *body = strdup(r.out_buf ? r.out_buf : "");
    free(r.out_buf);
    return body;
}

static void build_pristine(void)
{
    unlink(PRISTINE);
    faults_reset();
    pager_t p;
    assert(pager_open(&p, PRISTINE, true) == 0);
    int st;
    free(run_sql(&p, "CREATE TABLE t (name VARCHAR(16), age INT)", &st, NULL, 0));
    assert(st == 0);
    for (int i = 0; i < ROWS; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('n%d', %d)", i, i);
        free(run_sql(&p, sql, &st, NULL, 0));
        assert(st == 0);
    }
    assert(pager_close(&p) == 0);
}

/* WORK를 pristine으로 되돌려 열고 캐시를 비운다. 실패하면 -1 */
static int open_work_cold(pager_t *p, const char *src)
{
    copy_file(src, WORK);
    if (pager_open(p, WORK, false) != 0) return -1;
    drop_cache(p);
    return 0;
}

/* ══════════════════════════════════════════════════════════════ */
/*  1. 짧은 읽기·EINTR는 이어서 성공한다                          */
/* ══════════════════════════════════════════════════════════════ */
static void test_short_read_and_eintr_resume(void)
{
    printf("\n[test_short_read_and_eintr_resume]\n");
    faults_reset();
    pager_t p;
    CHECK(open_work_cold(&p, PRISTINE) == 0, "정상 열기");
    arm(&g_rd, F_NONE, 0, false, 0);
    int st;
    char *expect = run_sql(&p, SELECT_ALL, &st, NULL, 0);
    int reads = g_rd.calls;
    CHECK(st == 0 && strlen(expect) > 0, "기준 SELECT 성공");
    CHECK(reads > 0, "차가운 캐시에서 읽기 발생 (%d)", reads);
    pager_close(&p);

    fault_kind_t kinds[] = {F_SHORT, F_EINTR};
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        for (int at = 1; at <= reads; at++) {
            CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
            arm(&g_rd, kinds[k], at, false, 0);
            char *got = run_sql(&p, SELECT_ALL, &st, NULL, 0);
            CHECK(g_rd.fired == 1, "주입 발동 kind=%d at=%d", kinds[k], at);
            CHECK(st == 0 && strcmp(got, expect) == 0,
                  "짧은 읽기/EINTR 뒤에도 결과 동일 kind=%d at=%d", kinds[k], at);
            free(got);
            faults_reset();
            pager_close(&p);
        }
    }
    free(expect);
}

/* ══════════════════════════════════════════════════════════════ */
/*  2. 문장 실행 중 읽기 실패 → 오류 결과, 정리, 다음 요청 성공      */
/* ══════════════════════════════════════════════════════════════ */
static void test_read_fault_sweep(void)
{
    printf("\n[test_read_fault_sweep]\n");
    const char *stmts[] = {
        SELECT_ALL,
        "SELECT * FROM t WHERE id = 7",
        "SELECT * FROM t WHERE id > 500",
        "UPDATE t SET age = 99 WHERE id = 7",
        "UPDATE t SET age = 99 WHERE age >= 0",
        "DELETE FROM t WHERE id = 7",
        "DELETE FROM t WHERE age >= 650",
        "INSERT INTO t VALUES ('zz', 1)",
    };
    fault_kind_t kinds[] = {F_ERR, F_EOF};
    int silent_success = 0;
    int failures_seen = 0;

    for (size_t s = 0; s < sizeof(stmts) / sizeof(stmts[0]); s++) {
        pager_t p;
        faults_reset();
        CHECK(open_work_cold(&p, PRISTINE) == 0, "기준 열기");
        arm(&g_rd, F_NONE, 0, false, 0);
        int st;
        free(run_sql(&p, stmts[s], &st, NULL, 0));
        int reads = g_rd.calls;
        CHECK(st == 0, "기준 실행 성공: %s", stmts[s]);
        pager_close(&p);

        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            for (int at = 1; at <= reads; at++) {
                faults_reset();
                CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
                arm(&g_rd, kinds[k], at, false, EIO);
                char msg[512];
                free(run_sql(&p, stmts[s], &st, msg, sizeof(msg)));
                if (g_rd.fired > 0) {
                    failures_seen++;
                    if (st != -1) silent_success++;
                    CHECK(st == -1, "읽기 실패가 오류로 전달됨: %s (kind=%d at=%d)",
                          stmts[s], kinds[k], at);
                    CHECK(strstr(msg, "디스크 입출력") != NULL, "오류 메시지: %s", msg);
                }
                faults_reset();
                bool poisoned = pager_is_poisoned(&p);
                CHECK(!poisoned, "읽기 한 번 실패로 poison되지 않음: %s", stmts[s]);
                CHECK(frames_quiescent(&p, true), "실패 뒤 pin·래치 정리: %s (at=%d)",
                      stmts[s], at);
                char *again = run_sql(&p, SELECT_ALL, &st, NULL, 0);
                CHECK(st == 0, "같은 pager의 다음 요청 성공: %s (at=%d)", stmts[s], at);
                free(again);
                pager_close(&p);
            }
        }
    }
    CHECK(failures_seen > 0, "실패 주입이 실제로 발동함 (%d)", failures_seen);
    CHECK(silent_success == 0, "실패를 숨긴 성공 응답 없음");
}

/* open이 읽기·쓰기·fsync 실패에서 fd를 새지 않고 -1을 돌려준다 */
static void test_open_failures_do_not_leak_fd(void)
{
    printf("\n[test_open_failures_do_not_leak_fd]\n");
    int base = next_fd();
    pager_t p;

    copy_file(PRISTINE, WORK);
    faults_reset();
    arm(&g_rd, F_ERR, 1, false, EIO);
    CHECK(pager_open(&p, WORK, false) == -1, "헤더 읽기 실패 → -1");
    CHECK(next_fd() == base, "헤더 읽기 실패 뒤 fd 누수 없음");

    faults_reset();
    arm(&g_rd, F_EOF, 1, false, 0);
    CHECK(pager_open(&p, WORK, false) == -1, "헤더 EOF → -1");
    CHECK(next_fd() == base, "헤더 EOF 뒤 fd 누수 없음");

    faults_reset();
    arm(&g_wr, F_ERR, 2, false, ENOSPC);
    CHECK(pager_open(&p, WORK, true) == -1, "새 DB 초기 쓰기 실패 → -1");
    CHECK(errno == ENOSPC, "errno 보존: %d", errno);
    CHECK(next_fd() == base, "새 DB 쓰기 실패 뒤 fd 누수 없음");

    faults_reset();
    arm(&g_sync, F_ERR, 1, false, EIO);
    CHECK(pager_open(&p, WORK, true) == -1, "새 DB fsync 실패 → -1");
    CHECK(next_fd() == base, "새 DB fsync 실패 뒤 fd 누수 없음");

    faults_reset();
    copy_file(PRISTINE, WORK);
    CHECK(truncate(WORK, 10) == 0, "잘린 파일 준비");
    CHECK(pager_open(&p, WORK, false) == -1, "잘린 헤더 → -1");
    CHECK(next_fd() == base, "잘린 헤더 뒤 fd 누수 없음");
    unlink(WORK);
}

/* ══════════════════════════════════════════════════════════════ */
/*  3. 쓰기·fsync 실패                                            */
/* ══════════════════════════════════════════════════════════════ */
static int count_rows(pager_t *p)
{
    int st;
    char *body = run_sql(p, SELECT_ALL, &st, NULL, 0);
    int lines = 0;
    for (char *c = body; *c; c++) {
        if (*c == '\n') lines++;
    }
    free(body);
    return st == 0 ? lines : -1;
}

static void insert_rows(pager_t *p, int from, int to)
{
    for (int i = from; i < to; i++) {
        char sql[128];
        int st;
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('w%d', %d)", i, i);
        free(run_sql(p, sql, &st, NULL, 0));
        assert(st == 0);
    }
}

static void test_flush_write_failure_keeps_dirty_and_header(void)
{
    printf("\n[test_flush_write_failure_keeps_dirty_and_header]\n");
    faults_reset();
    pager_t p;
    CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
    int before_rows = count_rows(&p);
    p.dirty_high_watermark = 0; /* 선제 flush를 꺼서 쓰기 위치를 통제한다 */
    insert_rows(&p, 1000, 1400);
    int dirty = dirty_frames(&p);
    CHECK(dirty >= 2, "dirty 프레임 여러 개 (%d)", dirty);

    uint8_t header_before[256];
    read_prefix(WORK, header_before, sizeof(header_before));

    fault_kind_t kinds[] = {F_ERR, F_ZERO};
    int errs[] = {EIO, ENOSPC};
    for (size_t k = 0; k < 2; k++) {
        arm(&g_wr, kinds[k], 2, false, errs[k]);
        errno = 0;
        CHECK(pager_flush_all(&p) == -1, "쓰기 실패 → flush_all -1 (kind=%d)", kinds[k]);
        CHECK(errno == ENOSPC || errno == EIO, "errno 전달: %d", errno);
        CHECK(dirty_frames(&p) == dirty, "실패 뒤에도 dirty 유지 (%d → %d)", dirty,
              dirty_frames(&p));
        uint8_t header_after[256];
        read_prefix(WORK, header_after, sizeof(header_after));
        CHECK(memcmp(header_before, header_after, sizeof(header_after)) == 0,
              "쓰기 실패 시 디스크 헤더 페이지는 그대로");
        CHECK(pager_last_io_errno(&p) != 0, "마지막 오류 기록");
    }

    faults_reset();
    CHECK(pager_flush_all(&p) == 0, "실패가 사라지면 같은 프레임으로 flush 성공");
    CHECK(dirty_frames(&p) == 0, "성공 뒤 dirty 없음");
    CHECK(pager_close(&p) == 0, "닫기 성공");

    CHECK(pager_open(&p, WORK, false) == 0, "다시 열기");
    CHECK(count_rows(&p) == before_rows + 400, "복구한 flush의 데이터가 디스크에 있음");
    pager_close(&p);
}

static void test_short_and_interrupted_writes_resume(void)
{
    printf("\n[test_short_and_interrupted_writes_resume]\n");
    fault_kind_t kinds[] = {F_SHORT, F_EINTR};
    for (size_t k = 0; k < 2; k++) {
        faults_reset();
        pager_t p;
        CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
        int before_rows = count_rows(&p);
        p.dirty_high_watermark = 0;
        insert_rows(&p, 2000, 2100);
        arm(&g_wr, kinds[k], 2, false, 0);
        CHECK(pager_flush_all(&p) == 0, "짧은 쓰기/EINTR는 이어 써서 성공 (kind=%d)", kinds[k]);
        CHECK(g_wr.fired == 1, "주입 발동");
        faults_reset();
        pager_close(&p);
        CHECK(pager_open(&p, WORK, false) == 0, "다시 열기");
        CHECK(count_rows(&p) == before_rows + 100, "짧은 쓰기 뒤에도 데이터 온전");
        pager_close(&p);
    }
}

static void test_watermark_flush_failure_reports_and_keeps_dirty(void)
{
    printf("\n[test_watermark_flush_failure_reports_and_keeps_dirty]\n");
    faults_reset();
    pager_t p;
    CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
    int before_rows = count_rows(&p);
    p.dirty_high_watermark = 2;
    p.dirty_low_watermark = 0;

    int acked = 0;
    int failed_statements = 0;
    arm(&g_wr, F_ERR, 1, true, ENOSPC);
    for (int i = 0; i < 10; i++) {
        char sql[128];
        int st;
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('m%d', %d)", i, i);
        free(run_sql(&p, sql, &st, NULL, 0));
        if (st == 0) acked++;
        else failed_statements++;
    }
    CHECK(failed_statements > 0, "워터마크 flush 실패가 INSERT 오류로 전달됨 (%d)", failed_statements);
    CHECK(dirty_frames(&p) >= 2, "쓰지 못한 프레임은 dirty 유지 (%d)", dirty_frames(&p));
    CHECK(frames_quiescent(&p, true), "pin·래치 정리");

    faults_reset();
    p.dirty_high_watermark = 0;
    CHECK(pager_flush_all(&p) == 0, "실패가 사라진 뒤 flush 성공");
    pager_close(&p);
    CHECK(pager_open(&p, WORK, false) == 0, "다시 열기");
    CHECK(count_rows(&p) >= before_rows + acked, "확인 응답을 받은 행은 flush 뒤 모두 존재");
    pager_close(&p);
}

static void test_fsync_failure_is_sticky(void)
{
    printf("\n[test_fsync_failure_is_sticky]\n");
    faults_reset();
    pager_t p;
    CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
    p.dirty_high_watermark = 0;
    insert_rows(&p, 3000, 3050);
    int dirty = dirty_frames(&p);

    arm(&g_sync, F_ERR, 1, false, EIO);
    CHECK(pager_flush_all(&p) == -1, "fsync 실패 → flush_all -1");
    CHECK(dirty_frames(&p) == dirty, "fsync 실패 뒤에도 dirty 유지");
    CHECK(p.fsync_failed, "fsync 실패 기록");

    faults_reset();
    CHECK(pager_flush_all(&p) == -1, "fsync가 한 번 실패한 pager는 다시 열기 전까지 flush 성공을 주장하지 않음");
    CHECK(dirty_frames(&p) == dirty, "여전히 dirty");

    int fd = p.fd;
    CHECK(pager_close(&p) == -1, "닫기는 실패를 알림");
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "실패해도 fd는 닫힘");
}

static void test_evict_write_failure_never_reuses_dirty_frame(void)
{
    printf("\n[test_evict_write_failure_never_reuses_dirty_frame]\n");
    faults_reset();
    pager_t p;
    CHECK(open_work_cold(&p, PRISTINE) == 0, "열기");
    p.dirty_high_watermark = 0;

    /* 캐시를 dirty 프레임으로 가득 채운다 */
    uint32_t first = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        uint32_t pid = pager_alloc_page(&p);
        CHECK(pid != 0, "페이지 할당 %d", i);
        if (first == 0) first = pid;
    }

    /* 가장 오래된 프레임을 찾는다 (LRU 교체 대상) */
    int victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (p.frames[i].is_valid && p.frames[i].used_tick < oldest) {
            oldest = p.frames[i].used_tick;
            victim = i;
        }
    }
    CHECK(victim >= 0 && p.frames[victim].is_dirty, "교체 대상은 dirty");
    uint32_t victim_pid = p.frames[victim].page_id;

    uint32_t next_before = p.header.next_page_id;
    arm(&g_wr, F_ERR, 1, true, ENOSPC);
    CHECK(pager_alloc_page(&p) == 0, "교체 쓰기 실패 → 할당 실패(0)");
    CHECK(p.header.next_page_id == next_before, "실패한 할당은 헤더를 바꾸지 않음");
    CHECK(p.frames[victim].is_valid && p.frames[victim].is_dirty &&
              p.frames[victim].page_id == victim_pid,
          "쓰지 못한 프레임은 유효·dirty 그대로, 다른 페이지로 재사용되지 않음");
    CHECK(pager_last_io_errno(&p) == ENOSPC, "errno 기록");
    CHECK(frames_quiescent(&p, true), "pin·래치 정리");

    faults_reset();
    CHECK(pager_alloc_page(&p) != 0, "실패가 사라지면 같은 요청이 성공");
    pager_close(&p);
}

/* ══════════════════════════════════════════════════════════════ */
/*  4. B+ tree 분할·재균형 도중 실패                                */
/* ══════════════════════════════════════════════════════════════ */
static uint32_t leaf_capacity(const pager_t *p)
{
    return (p->page_size - sizeof(leaf_page_header_t)) / sizeof(leaf_entry_t);
}

static const row_ref_t FAKE_REF = {1, 0};

static void build_tree_full(void)
{
    unlink(TREE_FULL);
    faults_reset();
    pager_t p;
    assert(pager_open(&p, TREE_FULL, true) == 0);
    uint32_t cap = leaf_capacity(&p);
    for (uint32_t k = 1; k <= cap; k++) assert(bptree_insert(&p, k, FAKE_REF) == 0);
    assert(pager_close(&p) == 0);
}

static uint32_t left_leaf_count(pager_t *p, uint32_t *left_pid)
{
    uint8_t *root = pager_get_page(p, p->header.root_index_page_id);
    assert(root);
    internal_page_header_t iph;
    memcpy(&iph, root, sizeof(iph));
    pager_unpin(p, p->header.root_index_page_id);
    uint8_t *leaf = pager_get_page(p, iph.leftmost_child_page_id);
    assert(leaf);
    leaf_page_header_t lph;
    memcpy(&lph, leaf, sizeof(lph));
    pager_unpin(p, iph.leftmost_child_page_id);
    if (left_pid) *left_pid = iph.leftmost_child_page_id;
    return lph.key_count;
}

/* 루트가 분할된 두 리프 트리에서, 다음 삭제가 왼쪽 리프의 재균형을 일으키는 상태 */
static uint64_t build_tree_two(void)
{
    unlink(TREE_TWO);
    faults_reset();
    pager_t p;
    assert(pager_open(&p, TREE_TWO, true) == 0);
    uint32_t cap = leaf_capacity(&p);
    for (uint32_t k = 1; k <= cap + 1; k++) assert(bptree_insert(&p, k, FAKE_REF) == 0);
    uint64_t next_key = 1;
    while (left_leaf_count(&p, NULL) > cap / 2) {
        assert(bptree_delete(&p, next_key) == 0);
        next_key++;
    }
    /* 한 번 더 지우면 왼쪽이 최소치 아래로 내려가 오른쪽 형제를 읽어야 한다 */
    assert(pager_close(&p) == 0);
    return next_key;
}

static bool tree_has_all(pager_t *p, uint64_t from, uint64_t to)
{
    for (uint64_t k = from; k <= to; k++) {
        row_ref_t ref;
        if (!bptree_search(p, k, &ref)) return false;
    }
    return true;
}

static void test_btree_split_read_fault_leaves_tree_unchanged(void)
{
    printf("\n[test_btree_split_read_fault_leaves_tree_unchanged]\n");
    pager_t p;
    faults_reset();
    CHECK(open_work_cold(&p, TREE_FULL) == 0, "기준 열기");
    uint32_t cap = leaf_capacity(&p);
    arm(&g_rd, F_NONE, 0, false, 0);
    CHECK(bptree_insert(&p, cap + 1, FAKE_REF) == 0, "기준 분할 삽입 성공");
    int reads = g_rd.calls;
    pager_close(&p);
    CHECK(reads > 0, "분할 삽입에 읽기 포함 (%d)", reads);

    int failures = 0;
    for (int at = 1; at <= reads; at++) {
        faults_reset();
        CHECK(open_work_cold(&p, TREE_FULL) == 0, "열기");
        arm(&g_rd, F_ERR, at, false, EIO);
        int rc = bptree_insert(&p, cap + 1, FAKE_REF);
        if (g_rd.fired) {
            failures++;
            CHECK(rc == BPTREE_ERR_IO, "읽기 실패 → BPTREE_ERR_IO (rc=%d at=%d)", rc, at);
            CHECK(!pager_is_poisoned(&p), "분할 전 실패는 poison 아님");
            CHECK(frames_quiescent(&p, true), "pin·래치 정리 (at=%d)", at);
            faults_reset();
            row_ref_t ref;
            CHECK(tree_has_all(&p, 1, cap), "기존 키 모두 그대로");
            CHECK(!bptree_search(&p, cap + 1, &ref), "실패한 키는 들어가지 않음");
            CHECK(bptree_insert(&p, cap + 1, FAKE_REF) == 0, "같은 pager에서 재시도 성공");
            CHECK(tree_has_all(&p, 1, cap + 1), "재시도 뒤 모든 키 존재");
        }
        faults_reset();
        CHECK(pager_close(&p) == 0, "닫기 성공");
    }
    CHECK(failures > 0, "분할 삽입 중 읽기 실패가 실제로 발동함");
}

static void test_btree_split_poison_preserves_disk_state(void)
{
    printf("\n[test_btree_split_poison_preserves_disk_state]\n");
    pager_t p;
    faults_reset();
    copy_file(TREE_FULL, WORK);
    size_t span = 3 * (size_t)sysconf(_SC_PAGESIZE); /* 헤더·힙·루트 페이지 */
    uint8_t *original = malloc(span);
    uint8_t *after = malloc(span);
    assert(original && after);
    read_prefix(WORK, original, span);

    CHECK(pager_open(&p, WORK, false) == 0, "열기");
    uint32_t cap = leaf_capacity(&p);
    p.dirty_high_watermark = 0;
    for (int i = 0; i < 300; i++) CHECK(pager_alloc_page(&p) != 0, "채우기 %d", i);
    uint8_t *root = pager_get_page(&p, p.header.root_index_page_id); /* 루트 리프를 최신으로 */
    CHECK(root != NULL, "루트 읽기");
    pager_unpin(&p, p.header.root_index_page_id);

    /* 첫 교체 쓰기(분할용 새 리프)는 성공하고, 두 번째(새 루트)에서 실패한다 */
    arm(&g_wr, F_ERR, 2, true, ENOSPC);
    int rc = bptree_insert(&p, cap + 1, FAKE_REF);
    CHECK(rc == BPTREE_ERR_IO, "분할 도중 실패 → BPTREE_ERR_IO (rc=%d)", rc);
    CHECK(pager_is_poisoned(&p), "되돌릴 수 없는 실패는 pager를 poison");
    CHECK(frames_quiescent(&p, false), "다른 스레드가 기다리지 않도록 래치는 모두 풀림");
    CHECK(pager_get_page(&p, 1) == NULL, "poison 뒤 페이지 접근은 실패");
    CHECK(pager_flush_all(&p) == -1, "poison 뒤 flush는 디스크를 건드리지 않고 실패");
    faults_reset();
    CHECK(pager_flush_all(&p) == -1, "실패 원인이 사라져도 poison은 유지");

    read_prefix(WORK, after, span);
    CHECK(memcmp(original, after, span) == 0, "헤더·힙·루트 페이지가 원본 그대로");
    free(original);
    free(after);
    CHECK(pager_close(&p) == -1, "닫기는 실패를 알리고 자원은 해제");

    CHECK(pager_open(&p, WORK, false) == 0, "다시 열기");
    CHECK(tree_has_all(&p, 1, cap), "다시 열면 원본 키가 모두 보임");
    CHECK(bptree_insert(&p, cap + 1, FAKE_REF) == 0, "다시 연 pager에서 삽입 성공");
    CHECK(pager_close(&p) == 0, "닫기 성공");
}

static void test_btree_delete_rebalance_fault(void)
{
    printf("\n[test_btree_delete_rebalance_fault]\n");
    uint64_t key = build_tree_two();
    pager_t p;
    faults_reset();
    CHECK(open_work_cold(&p, TREE_TWO) == 0, "기준 열기");
    uint32_t cap = leaf_capacity(&p);
    arm(&g_rd, F_NONE, 0, false, 0);
    CHECK(bptree_delete(&p, key) == 0, "기준 삭제 성공");
    int reads = g_rd.calls;
    pager_close(&p);

    uint8_t original[256];
    read_prefix(TREE_TWO, original, sizeof(original));
    int clean_failures = 0;
    int poisoned_failures = 0;

    for (int at = 1; at <= reads; at++) {
        faults_reset();
        CHECK(open_work_cold(&p, TREE_TWO) == 0, "열기");
        arm(&g_rd, F_ERR, at, false, EIO);
        int rc = bptree_delete(&p, key);
        if (!g_rd.fired) {
            pager_close(&p);
            continue;
        }
        CHECK(rc == BPTREE_ERR_IO, "삭제 중 읽기 실패 → BPTREE_ERR_IO (rc=%d at=%d)", rc, at);
        CHECK(frames_quiescent(&p, false), "래치 정리 (at=%d)", at);
        faults_reset();
        if (pager_is_poisoned(&p)) {
            poisoned_failures++;
            CHECK(pager_get_page(&p, 1) == NULL, "poison 뒤 접근 실패");
            CHECK(pager_flush_all(&p) == -1, "poison 뒤 flush 실패");
            uint8_t after[256];
            read_prefix(WORK, after, sizeof(after));
            CHECK(memcmp(original, after, sizeof(after)) == 0, "디스크 헤더 보존");
            CHECK(pager_close(&p) == -1, "닫기는 실패를 알림");
        } else {
            clean_failures++;
            CHECK(frames_quiescent(&p, true), "pin 정리 (at=%d)", at);
            row_ref_t ref;
            CHECK(bptree_search(&p, key, &ref), "삭제 전 실패는 키를 지우지 않음");
            CHECK(bptree_delete(&p, key) == 0, "같은 pager에서 재시도 성공");
            CHECK(pager_close(&p) == 0, "닫기 성공");
        }
    }
    CHECK(clean_failures > 0, "변경 전 실패 경로를 거침");
    CHECK(poisoned_failures > 0, "재균형 도중 실패(poison) 경로를 거침");
    (void)cap;
}

int main(void)
{
    db_init();
    pager_set_default_io(&HOOKED_IO);
    build_pristine();
    build_tree_full();

    test_short_read_and_eintr_resume();
    test_read_fault_sweep();
    test_open_failures_do_not_leak_fd();
    test_flush_write_failure_keeps_dirty_and_header();
    test_short_and_interrupted_writes_resume();
    test_watermark_flush_failure_reports_and_keeps_dirty();
    test_fsync_failure_is_sticky();
    test_evict_write_failure_never_reuses_dirty_frame();
    test_btree_split_read_fault_leaves_tree_unchanged();
    test_btree_split_poison_preserves_disk_state();
    test_btree_delete_rebalance_fault();

    pager_set_default_io(NULL);
    unlink(PRISTINE);
    unlink(TREE_FULL);
    unlink(TREE_TWO);
    unlink(WORK);
    db_destroy();

    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
