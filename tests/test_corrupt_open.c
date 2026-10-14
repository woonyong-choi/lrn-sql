/*
 * test_corrupt_open.c -- 손상된 파일 열기 검사 (#130)
 *
 * 정상 합성 DB를 만들어 헤더·힙 체인·빈 페이지 목록·슬롯·컬럼 메타데이터를 하나씩 망가뜨린 파일을
 * pager_open(create=false)로 연다. 계약:
 *   1. 손상된 파일은 제한 시간(WATCHDOG_SECONDS) 안에 -1과 errno=EINVAL로 거절된다.
 *      읽기 자체의 실패(EIO)는 이 검사의 손상 사례에서 나오지 않는다.
 *   2. 실패한 open은 파일을 한 바이트도 바꾸지 않는다.
 *   3. 실패한 open은 fd를 닫고 프레임을 모두 반환한다(pager.fd == -1, 프레임 data == NULL).
 *      반복해도 fd 번호가 늘지 않는다. 반환하지 못한 힙 메모리는 이 검사가 아니라
 *      Linux ASan(LeakSanitizer) 실행이 근거다. maxrss처럼 allocator·OS high-water에
 *      좌우되는 값은 판정에 쓰지 않는다.
 *   4. 정상 파일(빈 DB, 행이 있는 DB, 삭제로 빈 슬롯이 생긴 DB, DROP TABLE로 빈 페이지 목록이
 *      생긴 DB, v1 형식으로 되돌린 DB)은 이전처럼 열리고 같은 행 수를 낸다.
 *      손상 사례를 연 직후에도 정상 파일은 계속 열린다.
 *
 * 데이터는 모두 합성 데이터이며 인자로 받은 새 작업 폴더에만 쓴다. 이미 있는 경로는 거절한다.
 * 빌드: 다른 test_* 와 같은 방식으로 tests/test_corrupt_open.c 와 $(OBJS) 를 함께 링크한다.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "storage/page_format.h"
#include "storage/pager.h"

#define WATCHDOG_SECONDS 20
#define REPEAT 200
#define ROWS 1500
#define PATH_MAX_LEN 512

static int g_run = 0;
static int g_fail = 0;
static const char *g_case = "(시작 전)";
static int g_err_fd = 2; /* open 중 stderr를 /dev/null로 돌려도 정지 보고는 보이게 따로 둔 사본 */

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_run++;                                          \
        if (!(cond)) {                                    \
            g_fail++;                                     \
            printf("  FAIL (line %d, %s): ", __LINE__, g_case); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static void on_alarm(int sig)
{
    (void)sig;
    /* 시그널 핸들러에서는 async-signal-safe 함수만 쓴다 */
    const char head[] = "\n정지: 제한 시간 안에 pager_open이 끝나지 않았습니다: ";
    (void)!write(g_err_fd, head, sizeof(head) - 1);
    (void)!write(g_err_fd, g_case, strlen(g_case));
    (void)!write(g_err_fd, "\n", 1);
    _exit(2);
}

/* ── 바이트 버퍼 보조 ── */
typedef struct {
    uint8_t *data;
    size_t len;
} bytes_t;

static bytes_t read_file(const char *path)
{
    bytes_t b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    if (f == NULL) return b;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = (uint8_t *)malloc((size_t)n + 1);
    b.len = fread(b.data, 1, (size_t)n, f);
    fclose(f);
    return b;
}

static bytes_t clone(const bytes_t *src)
{
    bytes_t b = {(uint8_t *)malloc(src->len + 1), src->len};
    memcpy(b.data, src->data, src->len);
    return b;
}

static void write_file(const char *path, const bytes_t *b)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("쓸 수 없음: %s\n", path);
        exit(2);
    }
    fwrite(b->data, 1, b->len, f);
    fclose(f);
}

static void set32(bytes_t *b, size_t off, uint32_t v) { memcpy(b->data + off, &v, 4); }
static void set16(bytes_t *b, size_t off, uint16_t v) { memcpy(b->data + off, &v, 2); }
static uint32_t get32(const bytes_t *b, size_t off)
{
    uint32_t v;
    memcpy(&v, b->data + off, 4);
    return v;
}

/* ── 정상 합성 DB 만들기 ── */
static void exec_ok(pager_t *p, const char *sql)
{
    exec_result_t r = db_execute(p, sql);
    free(r.out_buf);
    if (r.status != 0) {
        printf("준비 실패: %s -> %s\n", sql, r.message);
        exit(2);
    }
}

static long count_rows(pager_t *p)
{
    exec_result_t r = db_execute(p, "SELECT COUNT(*) FROM t");
    long n = -1;
    if (r.status == 0 && r.out_buf != NULL) {
        const char *line = strstr(r.out_buf, "----------\n");
        if (line != NULL) n = atol(line + strlen("----------\n"));
    }
    free(r.out_buf);
    return n;
}

static void insert_rows(pager_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('n%d', %d)", i, i);
        exec_ok(p, sql);
    }
}

typedef enum { FX_EMPTY, FX_ROWS, FX_DELETED, FX_DROPPED, FX_COUNT } fixture_t;

static const char *const FX_NAME[FX_COUNT] = {"empty", "rows", "deleted", "dropped"};

static void build_fixture(fixture_t fx, const char *path)
{
    unlink(path);
    pager_t p;
    if (pager_open(&p, path, true) != 0) {
        printf("준비 실패: 새 DB를 만들 수 없음\n");
        exit(2);
    }
    if (fx != FX_EMPTY) {
        exec_ok(&p, "CREATE TABLE t (name VARCHAR(16), age INT)");
        insert_rows(&p, ROWS);
    }
    if (fx == FX_DELETED) {
        for (int i = 2; i < 400; i += 3) {
            char sql[64];
            snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id = %d", i);
            exec_ok(&p, sql);
        }
    }
    if (fx == FX_DROPPED) {
        exec_ok(&p, "DROP TABLE t");
        exec_ok(&p, "CREATE TABLE t (name VARCHAR(16), age INT)");
        insert_rows(&p, 5);
    }
    if (pager_close(&p) != 0) {
        printf("준비 실패: 닫기\n");
        exit(2);
    }
}

/* ── 열기 시도와 자원 확인 ── */
static int probe_fd(void)
{
    int fd = dup(0);
    if (fd >= 0) close(fd);
    return fd;
}

typedef struct {
    int rc;
    int err;
} open_result_t;

static open_result_t try_open(const char *path)
{
    pager_t p;
    int saved = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, 2); /* 손상 문구가 시험 출력을 어지럽히지 않게 한다 */
    errno = 0;
    int rc = pager_open(&p, path, false);
    int err = errno;
    if (saved >= 0) {
        dup2(saved, 2);
        close(saved);
    }
    if (devnull >= 0) close(devnull);
    open_result_t r = {rc, err};
    if (rc == 0) {
        pager_close(&p);
        return r;
    }
    CHECK(p.fd == -1, "실패한 open 뒤 fd가 닫혀 있다 (fd=%d)", p.fd);
    int frames_left = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (p.frames[i].data != NULL) frames_left++;
    }
    CHECK(frames_left == 0, "실패한 open 뒤 프레임 %d개가 남아 있다", frames_left);
    return r;
}

static int same_bytes(const char *path, const bytes_t *expect)
{
    bytes_t now = read_file(path);
    int same = now.len == expect->len && memcmp(now.data, expect->data, now.len) == 0;
    free(now.data);
    return same;
}

/* ── 손상 사례 ── */
typedef struct {
    fixture_t base;
    const char *name;
    void (*mutate)(bytes_t *b, uint32_t ps);
} corrupt_case_t;

/* 앞쪽 힙 페이지와 리프 페이지 번호를 파일에서 찾는다 */
static uint32_t nth_page_of_type(const bytes_t *b, uint32_t ps, uint32_t type, int n)
{
    for (uint32_t i = 1; (size_t)(i + 1) * ps <= b->len; i++) {
        if (get32(b, (size_t)i * ps) == type && n-- == 0) return i;
    }
    return 0;
}

#define HEAP1(b, ps) nth_page_of_type((b), (ps), PAGE_TYPE_HEAP, 0)
#define HEAP2(b, ps) nth_page_of_type((b), (ps), PAGE_TYPE_HEAP, 1)

static void m_self_cycle(bytes_t *b, uint32_t ps) { set32(b, (size_t)HEAP1(b, ps) * ps + 4, HEAP1(b, ps)); }
static void m_two_cycle(bytes_t *b, uint32_t ps)
{
    uint32_t h1 = HEAP1(b, ps), h2 = HEAP2(b, ps);
    set32(b, (size_t)h1 * ps + 4, h2);
    set32(b, (size_t)h2 * ps + 4, h1);
}
static void m_next_oob(bytes_t *b, uint32_t ps) { set32(b, (size_t)HEAP1(b, ps) * ps + 4, 99999); }
static void m_next_leaf(bytes_t *b, uint32_t ps)
{
    set32(b, (size_t)HEAP1(b, ps) * ps + 4, nth_page_of_type(b, ps, PAGE_TYPE_LEAF, 0));
}
static void m_next_header(bytes_t *b, uint32_t ps)
{
    (void)ps;
    set32(b, (size_t)HEAP1(b, ps) * ps + 4, 0);
    set32(b, 20, 0); /* first_heap = 0 */
}
static void m_first_heap_oob(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 20, 99999); }
static void m_trunc_100(bytes_t *b, uint32_t ps) { (void)ps; b->len = 100; }
static void m_trunc_400(bytes_t *b, uint32_t ps) { (void)ps; b->len = 400; }
static void m_header_only(bytes_t *b, uint32_t ps) { b->len = ps; }
static void m_trunc_mid_heap(bytes_t *b, uint32_t ps) { b->len = (size_t)HEAP1(b, ps) * ps + 10; }
static void m_empty_file(bytes_t *b, uint32_t ps) { (void)ps; b->len = 0; }
static void m_ps_0(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 12, 0); }
static void m_ps_8(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 12, 8); }
static void m_ps_3000(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 12, 3000); }
static void m_ps_1gib(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 12, 1u << 30); }
static void m_ps_1mib(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 12, 1u << 20); }
static void m_ps_half(bytes_t *b, uint32_t ps) { set32(b, 12, ps / 2); }
static void m_colcount_40000(bytes_t *b, uint32_t ps) { (void)ps; set16(b, 48, 40000); }
static void m_colcount_17(bytes_t *b, uint32_t ps) { (void)ps; set16(b, 48, 17); }
static void m_rowsize_0(bytes_t *b, uint32_t ps) { (void)ps; set16(b, 50, 0); }
static void m_rowsize_60000(bytes_t *b, uint32_t ps) { (void)ps; set16(b, 50, 60000); }
static void m_coltype_99(bytes_t *b, uint32_t ps) { (void)ps; b->data[52 + 32] = 99; }
static void m_colsize_0(bytes_t *b, uint32_t ps) { (void)ps; set16(b, 52 + 33, 0); }
static void m_colsize_300(bytes_t *b, uint32_t ps) { (void)ps; set16(b, 52 + 33, 300); }
static void m_colname_unterminated(bytes_t *b, uint32_t ps) { (void)ps; memset(b->data + 52, 'x', 32); }
static void m_slotcount_60000(bytes_t *b, uint32_t ps) { set16(b, (size_t)HEAP1(b, ps) * ps + 8, 60000); }
static void m_slot_offset_oob(bytes_t *b, uint32_t ps) { set16(b, (size_t)HEAP1(b, ps) * ps + 16, 60000); }
static void m_slot_status_bad(bytes_t *b, uint32_t ps) { set16(b, (size_t)HEAP1(b, ps) * ps + 18, 9); }
static void m_heap_type_leaf(bytes_t *b, uint32_t ps) { set32(b, (size_t)HEAP1(b, ps) * ps, PAGE_TYPE_LEAF); }
static void m_free_slot_head_oob(bytes_t *b, uint32_t ps) { set16(b, (size_t)HEAP1(b, ps) * ps + 10, 50000); }
static void m_free_space_oob(bytes_t *b, uint32_t ps) { set16(b, (size_t)HEAP1(b, ps) * ps + 12, 60000); }
static void m_next_page_id_0(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 24, 0); }
static void m_root_oob(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 16, 99999); }
static void m_root_is_heap(bytes_t *b, uint32_t ps) { set32(b, 16, HEAP1(b, ps)); }
static void m_free_head_oob(bytes_t *b, uint32_t ps) { (void)ps; set32(b, 28, 99999); }
static void m_free_head_heap(bytes_t *b, uint32_t ps) { set32(b, 28, HEAP1(b, ps)); }
static void m_free_self_cycle(bytes_t *b, uint32_t ps)
{
    uint32_t f = get32(b, 28);
    set32(b, (size_t)f * ps + 4, f);
}
static void m_free_next_oob(bytes_t *b, uint32_t ps)
{
    uint32_t f = get32(b, 28);
    set32(b, (size_t)f * ps + 4, 99999);
}
static void m_free_slot_cycle(bytes_t *b, uint32_t ps)
{
    /* 빈 슬롯 체인의 첫 슬롯이 자기 자신을 다시 가리키게 한다 */
    uint32_t h = HEAP1(b, ps);
    uint16_t head;
    memcpy(&head, b->data + (size_t)h * ps + 10, 2);
    size_t slot_off = (size_t)h * ps + sizeof(heap_page_header_t) + (size_t)head * sizeof(slot_t);
    set16(b, slot_off + 4, head);
}

static const corrupt_case_t CASES[] = {
    {FX_ROWS, "self_cycle", m_self_cycle},
    {FX_ROWS, "two_page_cycle", m_two_cycle},
    {FX_ROWS, "next_out_of_range", m_next_oob},
    {FX_ROWS, "next_points_to_leaf", m_next_leaf},
    {FX_ROWS, "first_heap_zero", m_next_header},
    {FX_ROWS, "first_heap_out_of_range", m_first_heap_oob},
    {FX_ROWS, "truncated_header_100", m_trunc_100},
    {FX_ROWS, "truncated_header_400", m_trunc_400},
    {FX_ROWS, "header_page_only", m_header_only},
    {FX_ROWS, "truncated_mid_heap", m_trunc_mid_heap},
    {FX_ROWS, "empty_file", m_empty_file},
    {FX_ROWS, "page_size_0", m_ps_0},
    {FX_ROWS, "page_size_8", m_ps_8},
    {FX_ROWS, "page_size_3000", m_ps_3000},
    {FX_ROWS, "page_size_1GiB", m_ps_1gib},
    {FX_ROWS, "page_size_1MiB", m_ps_1mib},
    {FX_ROWS, "page_size_half", m_ps_half},
    {FX_ROWS, "column_count_40000", m_colcount_40000},
    {FX_ROWS, "column_count_17", m_colcount_17},
    {FX_ROWS, "row_size_0", m_rowsize_0},
    {FX_ROWS, "row_size_60000", m_rowsize_60000},
    {FX_ROWS, "column_type_99", m_coltype_99},
    {FX_ROWS, "column_size_0", m_colsize_0},
    {FX_ROWS, "column_size_300", m_colsize_300},
    {FX_ROWS, "column_name_unterminated", m_colname_unterminated},
    {FX_ROWS, "slot_count_60000", m_slotcount_60000},
    {FX_ROWS, "slot_offset_out_of_page", m_slot_offset_oob},
    {FX_ROWS, "slot_status_invalid", m_slot_status_bad},
    {FX_ROWS, "heap_page_type_leaf", m_heap_type_leaf},
    {FX_ROWS, "free_space_offset_out_of_page", m_free_space_oob},
    {FX_ROWS, "next_page_id_zero", m_next_page_id_0},
    {FX_ROWS, "root_out_of_range", m_root_oob},
    {FX_ROWS, "root_is_heap_page", m_root_is_heap},
    {FX_ROWS, "free_head_out_of_range", m_free_head_oob},
    {FX_ROWS, "free_head_is_heap_page", m_free_head_heap},
    {FX_DELETED, "free_slot_head_out_of_range", m_free_slot_head_oob},
    {FX_DELETED, "free_slot_chain_cycle", m_free_slot_cycle},
    {FX_DROPPED, "free_list_self_cycle", m_free_self_cycle},
    {FX_DROPPED, "free_list_next_out_of_range", m_free_next_oob},
};
#define CASE_COUNT ((int)(sizeof(CASES) / sizeof(CASES[0])))

/* ── 정상 파일 ── */
static void check_good_opens(const char *path, fixture_t fx, long expect_rows, const char *label)
{
    g_case = label;
    pager_t p;
    int rc = pager_open(&p, path, false);
    CHECK(rc == 0, "%s: 정상 파일이 열린다 (errno=%d)", label, rc == 0 ? 0 : errno);
    if (rc != 0) return;
    if (fx != FX_EMPTY) {
        long n = count_rows(&p);
        CHECK(n == expect_rows, "%s: 행 수 %ld (기대 %ld)", label, n, expect_rows);
        insert_rows(&p, 1);
        CHECK(count_rows(&p) == expect_rows + 1, "%s: 열린 뒤 쓰기가 이어진다", label);
    }
    CHECK(pager_close(&p) == 0, "%s: 닫기", label);
}

static long expected_rows(fixture_t fx)
{
    switch (fx) {
        case FX_ROWS: return ROWS;
        case FX_DELETED: return ROWS - 133; /* id 2부터 3씩 400 미만: 133개 */
        case FX_DROPPED: return 5;
        default: return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s NEW_WORKDIR\n", argv[0]);
        return 2;
    }
    if (mkdir(argv[1], 0700) != 0) {
        fprintf(stderr, "거절: 작업 폴더 %s 를 새로 만들 수 없습니다 (%s). 이미 있는 경로는 쓰지 않습니다.\n",
                argv[1], strerror(errno));
        return 2;
    }
    g_err_fd = dup(2);
    signal(SIGALRM, on_alarm);
    db_init();

    char good[FX_COUNT][PATH_MAX_LEN];
    char work[PATH_MAX_LEN];
    snprintf(work, sizeof(work), "%s/case.db", argv[1]);
    bytes_t pristine[FX_COUNT];
    for (int fx = 0; fx < FX_COUNT; fx++) {
        snprintf(good[fx], sizeof(good[fx]), "%s/good_%s.db", argv[1], FX_NAME[fx]);
        build_fixture((fixture_t)fx, good[fx]);
        pristine[fx] = read_file(good[fx]);
    }
    uint32_t ps = get32(&pristine[FX_ROWS], 12);

    printf("[정상 파일은 이전처럼 열린다]\n");
    alarm(WATCHDOG_SECONDS);
    for (int fx = 0; fx < FX_COUNT; fx++) {
        char label[64];
        snprintf(label, sizeof(label), "정상:%s", FX_NAME[fx]);
        check_good_opens(good[fx], (fixture_t)fx, expected_rows((fixture_t)fx), label);
        write_file(good[fx], &pristine[fx]); /* 쓰기 검사 흔적을 되돌린다 */
    }
    {
        /* v1 형식: version=1, table_name 영역을 비운다. 열리고 table_name은 비어 있다 */
        bytes_t v1 = clone(&pristine[FX_ROWS]);
        set32(&v1, 8, 1);
        memset(v1.data + 676, 0, 32);
        write_file(work, &v1);
        free(v1.data);
        g_case = "정상:v1";
        pager_t p;
        CHECK(pager_open(&p, work, false) == 0, "v1 파일이 열린다");
        CHECK(p.header.version == 1 && p.header.table_name[0] == '\0', "v1은 이름 없는 상태로 열린다");
        exec_result_t blocked = db_execute(&p, "SELECT COUNT(*) FROM t");
        CHECK(blocked.status == -1 && strstr(blocked.message, "--adopt-table") != NULL,
              "v1은 명시적 이름 등록 전까지 쿼리를 막는다 (자동 형식 전환 없음)");
        free(blocked.out_buf);
        /* main.c의 --adopt-table과 같은 명시적 등록: 이름을 쓰고 v2로 올려 닫는다 */
        memcpy(p.header.table_name, "t", 2);
        p.header.version = DB_VERSION;
        p.header_dirty = true;
        CHECK(pager_close(&p) == 0, "명시적 등록 뒤 닫기");
        g_case = "정상:v1 등록 뒤";
        CHECK(pager_open(&p, work, false) == 0, "등록한 파일이 다시 열린다");
        CHECK(p.header.version == DB_VERSION, "등록 뒤에는 v2");
        CHECK(count_rows(&p) == ROWS, "등록 뒤 행 수");
        pager_close(&p);
    }

    printf("[손상된 파일은 EINVAL로 거절되고 원본·자원이 유지된다]\n");
    for (int i = 0; i < CASE_COUNT; i++) {
        const corrupt_case_t *c = &CASES[i];
        g_case = c->name;
        bytes_t b = clone(&pristine[c->base]);
        c->mutate(&b, ps);
        write_file(work, &b);
        bytes_t written = read_file(work);
        free(b.data);

        alarm(WATCHDOG_SECONDS);
        int fd_before = probe_fd();
        open_result_t r = try_open(work);
        CHECK(r.rc == -1, "손상된 파일은 열리지 않는다");
        CHECK(r.err == EINVAL, "손상은 EINVAL이다 (errno=%d: %s)", r.err, strerror(r.err));
        CHECK(same_bytes(work, &written), "실패한 open이 파일을 바꾸지 않는다");
        CHECK(probe_fd() == fd_before, "fd 번호가 늘지 않는다");
        free(written.data);

        /* 손상 사례 직후에도 정상 파일이 열린다 */
        check_good_opens(good[c->base], c->base, expected_rows(c->base), c->name);
        write_file(good[c->base], &pristine[c->base]);
    }

    printf("[반복 거절 %d회: fd 번호가 늘지 않고 매번 같은 결과]\n", REPEAT);
    {
        bytes_t b = clone(&pristine[FX_ROWS]);
        m_self_cycle(&b, ps);
        write_file(work, &b);
        free(b.data);
        g_case = "repeat_self_cycle";
        alarm(WATCHDOG_SECONDS);
        int fd_before = probe_fd();
        int bad = 0;
        for (int i = 0; i < REPEAT; i++) {
            open_result_t r = try_open(work);
            if (r.rc != -1 || r.err != EINVAL) bad++;
        }
        CHECK(bad == 0, "반복 중 달라진 결과 %d회", bad);
        CHECK(probe_fd() == fd_before, "반복 뒤 fd 번호가 늘지 않는다");
    }
    alarm(0);

    for (int fx = 0; fx < FX_COUNT; fx++) {
        free(pristine[fx].data);
        unlink(good[fx]);
    }
    unlink(work);
    rmdir(argv[1]);
    db_destroy();

    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
