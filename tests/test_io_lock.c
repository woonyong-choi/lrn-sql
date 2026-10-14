/*
 * test_io_lock.c -- 같은 파일 단일 pager 잠금과 실패 모델 구분 테스트 (#78)
 *
 * 1. 같은 파일을 두 번 열 수 없다 (같은 프로세스, 다른 프로세스). 잠금은 열린 fd에
 *    묶인 flock이라 프로세스가 SIGKILL로 죽으면 커널이 풀고, pid 숫자는 보지 않는다.
 *    잠긴 파일에 create=true로 열어도 내용을 지우지 않는다.
 * 2. SIGKILL: 이미 flush한 변경은 남고 그 뒤 변경은 사라진다. 파일이 깨지지 않는다.
 * 3. 부분 쓰기(torn): 페이지 일부만 새 내용인 파일을 현재 형식은 탐지하지 못한다.
 *    탐지 못 한다는 사실을 그대로 검사한다 (체크섬 도입은 형식 변경이라 이번 범위 밖).
 * 4. 성공 반환 후 미반영(lost): pwrite·fsync가 성공을 돌려줘도 저장되지 않은 경우를
 *    엔진은 알 수 없다. fsync가 오류를 돌려주면 flush_all 실패로 드러난다.
 *
 * 이 테스트는 WAL 없는 현재 구조의 한계를 확인하는 것이며, 복구 능력을 검증하지 않는다.
 * 데이터는 모두 합성 데이터다.
 *
 * 빌드: 다른 test_* 와 같은 방식으로 tests/test_io_lock.c 와 $(OBJS) 를 함께 링크한다.
 */

#include <assert.h>
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

#include "db.h"
#include "storage/pager.h"

#define PRISTINE "__test__iolock_pristine.db"
#define WORK "__test__iolock_work.db"
#define TORN "__test__iolock_torn.db"
#define PIDFILE "__test__iolock_work.db.pid"
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

/* ── 성공을 돌려주고도 쓰지 않는 저장 장치 흉내 (lost write) ── */
static bool g_drop_writes;
static bool g_fail_fsync;

static ssize_t hook_pread(int fd, void *buf, size_t n, off_t off) { return pread(fd, buf, n, off); }

static ssize_t hook_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (g_drop_writes && off > 0) return (ssize_t)n; /* 헤더(page 0) 밖은 버린다 */
    return pwrite(fd, buf, n, off);
}

static int hook_fsync(int fd)
{
    if (g_fail_fsync) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static const pager_io_t LOSSY_IO = {hook_pread, hook_pwrite, hook_fsync};

/* ── 보조 함수 ── */
static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    assert(in && out);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) assert(fwrite(buf, 1, n, out) == n);
    fclose(in);
    fclose(out);
}

static int exec_status(pager_t *p, const char *sql)
{
    exec_result_t r = db_execute(p, sql);
    free(r.out_buf);
    return r.status;
}

/* SELECT 결과의 행 수 (헤더 두 줄 제외). 실패하면 -1 */
static int count_where(pager_t *p, const char *sql)
{
    exec_result_t r = db_execute(p, sql);
    int lines = 0;
    if (r.out_buf) {
        for (const char *c = r.out_buf; *c; c++) {
            if (*c == '\n') lines++;
        }
    }
    free(r.out_buf);
    if (r.status != 0) return -1;
    return lines > 2 ? lines - 2 : 0;
}

static int g_rows;

/* 힙 페이지 하나를 90% 채우는 행 수로 합성 DB를 만든다 */
static void build_pristine(void)
{
    unlink(PRISTINE);
    pager_t p;
    assert(pager_open(&p, PRISTINE, true) == 0);
    assert(exec_status(&p, "CREATE TABLE t (name VARCHAR(16), age INT)") == 0);
    size_t per_page = (p.page_size - sizeof(heap_page_header_t)) / (sizeof(slot_t) + p.header.row_size);
    g_rows = (int)(per_page * 9 / 10);
    for (int i = 0; i < g_rows; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('n%d', %d)", i, i);
        assert(exec_status(&p, sql) == 0);
    }
    assert(pager_close(&p) == 0);
}

/* ══════════════════════════════════════════════════════════════ */
static void test_second_open_in_process_is_refused(void)
{
    printf("\n[test_second_open_in_process_is_refused]\n");
    copy_file(PRISTINE, WORK);
    pager_t a, b;
    CHECK(pager_open(&a, WORK, false) == 0, "첫 열기");
    struct stat before, after;
    stat(WORK, &before);

    errno = 0;
    CHECK(pager_open(&b, WORK, false) == -1, "같은 파일 두 번째 열기는 거부");
    CHECK(errno == EWOULDBLOCK || errno == EAGAIN, "errno = 이미 열림 (%d)", errno);
    errno = 0;
    CHECK(pager_open(&b, WORK, true) == -1, "잠긴 파일에 create=true도 거부");
    stat(WORK, &after);
    CHECK(before.st_size == after.st_size && before.st_size > 0, "거부된 create가 파일을 비우지 않음");
    CHECK(count_where(&a, SELECT_ALL) == g_rows, "첫 pager는 영향 없이 동작");

    CHECK(pager_close(&a) == 0, "첫 pager 닫기");
    CHECK(pager_open(&b, WORK, false) == 0, "닫은 뒤에는 열림");
    CHECK(count_where(&b, SELECT_ALL) == g_rows, "데이터 온전");
    pager_close(&b);
}

/* 자식이 파일을 열고 신호를 보낸 뒤 대기한다. 반환: 자식 pid. ready_fd로 한 바이트 수신 */
static pid_t spawn_holder(int mode)
{
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready[0]);
        pager_t p;
        if (pager_open(&p, WORK, false) != 0) _exit(2);
        if (mode == 1) { /* flush한 변경 50행 + flush하지 않은 변경 50행 */
            p.dirty_high_watermark = 0;
            for (int i = 0; i < 50; i++) {
                char sql[128];
                snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('a%d', %d)", i, 5000 + i);
                if (exec_status(&p, sql) != 0) _exit(3);
            }
            if (pager_flush_all(&p) != 0) _exit(4);
            for (int i = 0; i < 50; i++) {
                char sql[128];
                snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ('b%d', %d)", i, 6000 + i);
                if (exec_status(&p, sql) != 0) _exit(3);
            }
        }
        char ok = 1;
        if (write(ready[1], &ok, 1) != 1) _exit(5);
        for (;;) pause();
    }
    close(ready[1]);
    char ok = 0;
    assert(read(ready[0], &ok, 1) == 1 && ok == 1);
    close(ready[0]);
    return pid;
}

static void kill_and_reap(pid_t pid)
{
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static void test_other_process_lock_and_sigkill_release(void)
{
    printf("\n[test_other_process_lock_and_sigkill_release]\n");
    copy_file(PRISTINE, WORK);

    /* pid 숫자 기록이 있어도 열림 여부는 잠금만 본다 (살아 있는 pid를 적어 둔다) */
    FILE *pf = fopen(PIDFILE, "w");
    assert(pf);
    fprintf(pf, "%d\n", (int)getpid());
    fclose(pf);

    pid_t child = spawn_holder(0);
    pager_t p;
    errno = 0;
    CHECK(pager_open(&p, WORK, false) == -1, "다른 프로세스가 열고 있으면 거부");
    CHECK(errno == EWOULDBLOCK || errno == EAGAIN, "errno = 이미 열림 (%d)", errno);

    kill_and_reap(child);
    CHECK(pager_open(&p, WORK, false) == 0, "SIGKILL로 죽은 뒤에는 pid 기록 없이도 열림");
    CHECK(count_where(&p, SELECT_ALL) == g_rows, "데이터 온전");
    pager_close(&p);
    unlink(PIDFILE);
}

static void test_sigkill_keeps_flushed_and_drops_unflushed(void)
{
    printf("\n[test_sigkill_keeps_flushed_and_drops_unflushed]\n");
    copy_file(PRISTINE, WORK);
    pid_t child = spawn_holder(1);
    kill_and_reap(child);

    pager_t p;
    CHECK(pager_open(&p, WORK, false) == 0, "죽은 프로세스의 파일을 다시 열 수 있음");
    CHECK(count_where(&p, SELECT_ALL) == g_rows + 50,
          "flush 이전 변경(50행)은 남고 이후 변경(50행)은 사라짐 (현재 %d)",
          count_where(&p, SELECT_ALL));
    CHECK(count_where(&p, "SELECT * FROM t WHERE age >= 6000") == 0, "flush되지 않은 행은 없음");
    CHECK(count_where(&p, "SELECT * FROM t WHERE age >= 5000") == 50, "flush된 행은 모두 있음");
    CHECK(exec_status(&p, "INSERT INTO t VALUES ('after', 1)") == 0, "복구한 파일에 계속 쓸 수 있음");
    CHECK(pager_close(&p) == 0, "닫기 성공");
}

static void test_torn_page_is_not_detected(void)
{
    printf("\n[test_torn_page_is_not_detected]\n");
    /* 새 값으로 갱신해 flush한 파일 */
    copy_file(PRISTINE, WORK);
    pager_t p;
    CHECK(pager_open(&p, WORK, false) == 0, "열기");
    CHECK(exec_status(&p, "UPDATE t SET age = 1000 WHERE age >= 0") == 0, "전체 갱신");
    CHECK(pager_close(&p) == 0, "갱신 flush");

    /* 힙 페이지(page 1)의 앞 절반만 새 내용이고 뒷 절반은 옛 내용인 상태를 만든다 */
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    copy_file(WORK, TORN);
    FILE *old_f = fopen(PRISTINE, "rb");
    FILE *torn_f = fopen(TORN, "r+b");
    assert(old_f && torn_f);
    uint8_t *half = malloc(ps / 2);
    assert(half);
    assert(fseek(old_f, (long)(ps + ps / 2), SEEK_SET) == 0);
    assert(fread(half, 1, ps / 2, old_f) == ps / 2);
    assert(fseek(torn_f, (long)(ps + ps / 2), SEEK_SET) == 0);
    assert(fwrite(half, 1, ps / 2, torn_f) == ps / 2);
    fclose(old_f);
    fclose(torn_f);
    free(half);

    CHECK(pager_open(&p, TORN, false) == 0, "찢어진 페이지를 가진 파일이 오류 없이 열림 (탐지 불가)");
    int total = count_where(&p, SELECT_ALL);
    int updated = count_where(&p, "SELECT * FROM t WHERE age >= 1000");
    CHECK(total == g_rows, "행 수는 정상으로 보임 (%d)", total);
    CHECK(updated > 0 && updated < g_rows,
          "새 값과 옛 값이 섞여 보이지만 오류 없이 성공 응답 (갱신된 %d/%d)", updated, g_rows);
    pager_close(&p);
}

static void test_lost_write_is_invisible_unless_fsync_fails(void)
{
    printf("\n[test_lost_write_is_invisible_unless_fsync_fails]\n");
    pager_set_default_io(&LOSSY_IO);

    copy_file(PRISTINE, WORK);
    pager_t p;
    g_drop_writes = true;
    g_fail_fsync = false;
    CHECK(pager_open(&p, WORK, false) == 0, "열기");
    CHECK(exec_status(&p, "UPDATE t SET age = 1000 WHERE age >= 0") == 0, "갱신");
    CHECK(pager_flush_all(&p) == 0, "저장 장치가 쓰기를 버려도 성공 반환 → 엔진은 알 수 없음");
    CHECK(pager_close(&p) == 0, "닫기도 성공");
    g_drop_writes = false;
    CHECK(pager_open(&p, WORK, false) == 0, "다시 열기");
    CHECK(count_where(&p, "SELECT * FROM t WHERE age >= 1000") == 0,
          "성공을 돌려받았지만 변경은 디스크에 없음");
    pager_close(&p);

    copy_file(PRISTINE, WORK);
    g_drop_writes = true;
    g_fail_fsync = false;
    CHECK(pager_open(&p, WORK, false) == 0, "열기");
    CHECK(exec_status(&p, "UPDATE t SET age = 1000 WHERE age >= 0") == 0, "갱신");
    g_fail_fsync = true;
    CHECK(pager_flush_all(&p) == -1, "fsync가 오류를 돌려주면 flush 실패로 드러남");
    CHECK(pager_close(&p) == -1, "닫기도 실패를 알림");

    g_drop_writes = false;
    g_fail_fsync = false;
    pager_set_default_io(NULL);
}

int main(void)
{
    db_init();
    build_pristine();

    test_second_open_in_process_is_refused();
    test_other_process_lock_and_sigkill_release();
    test_sigkill_keeps_flushed_and_drops_unflushed();
    test_torn_page_is_not_detected();
    test_lost_write_is_invisible_unless_fsync_fails();

    unlink(PRISTINE);
    unlink(WORK);
    unlink(TORN);
    unlink(PIDFILE);
    db_destroy();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
