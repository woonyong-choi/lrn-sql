/*
 * test_ack_queue_fault.c — durable ACK 큐 연산의 I/O 실패와 프로세스 종료 주입(#140). 합성 데이터만 쓴다.
 *
 * 연산마다 WAL·데이터 파일의 pwrite·fsync·ftruncate 호출 지점을 하나씩 실패시키거나(오류 주입) 그 지점에서 프로세스를
 * 끝낸 뒤(fork) 다시 열어, 호출자에게 성공(ACKQ_OK)으로 알린 변경은 반드시 남고 실패·불확실로 알린 변경은 이전이거나
 * 새 상태 중 하나이며 중간 상태가 없다는 것을 확인한다. 불확실한 결과를 성공으로 돌려주지 않는지도 같은 검사로 본다.
 * 다루지 않는 것: 전원 장애·lost write(database_txn.h의 한계와 같다), 여러 프로세스 동시 사용.
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "ack_queue_test_util.h"
#include "storage/page_file.h"
#include "storage/wal.h"

#define DIR "__test__ack_queue_fault"

/* ── 주입 hook ── */

static long g_points_left = -1, g_points_seen, g_fail_at, g_fail_seen;
static bool g_fail_mode, g_active;

static bool point(void)
{
    if (!g_active) return false;
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

/* ── 파일 스냅샷 ── */

typedef struct {
    uint8_t *data[2];
    size_t len[2];
    bool present[2];
} snap_t;

static bool read_file(const char *path, uint8_t **buf, size_t *len)
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

static void snap_take(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->present[0] = read_file(g_db_path, &s->data[0], &s->len[0]);
    s->present[1] = read_file(g_wal_path, &s->data[1], &s->len[1]);
}

static void snap_restore(const snap_t *s)
{
    if (s->present[0]) write_file(g_db_path, s->data[0], s->len[0]);
    if (s->present[1]) write_file(g_wal_path, s->data[1], s->len[1]);
    else unlink(g_wal_path);
}

static void snap_free(snap_t *s)
{
    free(s->data[0]);
    free(s->data[1]);
}

/* ── 장면: 준비, 실행, 재시작 뒤 검증 ── */

typedef struct {
    const char *name;
    void (*prepare)(void);
    ackq_status_t (*run)(ackq_t *queue);
    /* 재시작 뒤(주입 없음) 상태 검사. acknowledged는 호출자에게 성공으로 알렸다는 뜻이다 */
    void (*verify)(database_t *db, ackq_t *queue, ackq_status_t status, bool acknowledged);
} scenario_t;

static uint64_t g_id, g_fence;

static ackq_state_t job_state(ackq_t *q, uint64_t id, uint64_t *fence, bool *found)
{
    ackq_item_t items[16];
    size_t count = 0;
    *found = false;
    if (ackq_list(q, NULL, 0, 16, false, items, &count) != ACKQ_OK) return ACKQ_STATE_FAILED;
    ackq_state_t state = ACKQ_STATE_READY;
    for (size_t i = 0; i < count; i++) {
        if (items[i].id == id) {
            *found = true;
            state = items[i].state;
            if (fence != NULL) *fence = items[i].fence;
        }
    }
    ackq_list_release(items, count);
    return state;
}

static ackq_t *open_queue(database_t *db)
{
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q = NULL;
    if (ackq_open(db, &o, &q) != ACKQ_OK) return NULL;
    return q;
}

/* 장면 1: ack */
static void prepare_ack(void)
{
    make_db();
    database_t *db = open_db(true);
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 100;
    assert(enq(q, NULL, "job", NULL, &g_id, NULL) == ACKQ_OK);
    ackq_claim_t item[1];
    size_t n = 0;
    assert(ackq_claim(q, NULL, 1, 100000, item, &n) == ACKQ_OK && n == 1);
    g_fence = item[0].fence;
    ackq_claims_release(item, n);
    ackq_close(&q);
    assert(database_close(db) == DATABASE_OK);
}

static ackq_status_t run_ack(ackq_t *q)
{
    return ackq_ack(q, NULL, g_id, g_fence);
}

static void verify_ack(database_t *db, ackq_t *q, ackq_status_t st, bool acknowledged)
{
    (void)db;
    bool found;
    uint64_t fence = 0;
    ackq_state_t state = job_state(q, g_id, &fence, &found);
    CHECK(found, "작업이 사라지지 않았다");
    CHECK(state == ACKQ_STATE_ACKED || (state == ACKQ_STATE_LEASED && fence == g_fence),
          "중간 상태 없음: ACKED 또는 원래 lease (state=%d fence=%llu)", (int)state, (unsigned long long)fence);
    if (acknowledged) CHECK(state == ACKQ_STATE_ACKED, "성공으로 알린 ack는 재시작 뒤에도 남는다");
    if (st == ACKQ_NOT_APPLIED) CHECK(state == ACKQ_STATE_LEASED, "확정 미적용이면 이전 상태다");
    CHECK(ackq_ack(q, NULL, g_id, g_fence) == ACKQ_OK, "재시작 뒤 같은 ack를 다시 해도 결과가 같다");
    CHECK(job_state(q, g_id, NULL, &found) == ACKQ_STATE_ACKED, "다시 한 ack 뒤 ACKED");
}

/* 장면 2: enqueue(idempotency key) */
static void prepare_enqueue(void)
{
    make_db();
    database_t *db = open_db(true);
    ackq_options_t o = qopts("jobs", 0);
    ackq_t *q;
    assert(ackq_install(db, &o, &q) == ACKQ_OK);
    g_now = 100;
    ackq_close(&q);
    assert(database_close(db) == DATABASE_OK);
}

static ackq_status_t run_enqueue(ackq_t *q)
{
    uint64_t id = 99;
    ackq_status_t st = ackq_enqueue(q, NULL, (const uint8_t *)"payload", 7, "key-1", &id, NULL);
    CHECK(st == ACKQ_OK ? id != 0 : id == 0, "성공이 아니면 id를 주지 않는다 (st=%s)", ackq_status_name(st));
    return st;
}

static void verify_enqueue(database_t *db, ackq_t *q, ackq_status_t st, bool acknowledged)
{
    (void)db;
    (void)st;
    ackq_stats_t stats;
    CHECK(ackq_stats(q, NULL, &stats) == ACKQ_OK, "stats");
    uint64_t total = stats.ready + stats.leased + stats.acked + stats.failed;
    CHECK(total <= 1, "작업은 0개 또는 1개 (total=%llu)", (unsigned long long)total);
    if (acknowledged) CHECK(total == 1, "성공으로 알린 enqueue는 재시작 뒤에도 남는다");
    uint64_t id = 0;
    bool dup = false;
    CHECK(ackq_enqueue(q, NULL, (const uint8_t *)"payload", 7, "key-1", &id, &dup) == ACKQ_OK, "같은 key로 재시도");
    CHECK(dup == (total == 1), "불확실했던 enqueue는 재시도가 같은 작업으로 풀어 준다");
    CHECK(ackq_stats(q, NULL, &stats) == ACKQ_OK && stats.ready == 1, "결국 작업은 정확히 하나");
}

/* 장면 3: claim 2개(배치는 원자적) */
static void prepare_claim(void)
{
    prepare_enqueue();
    database_t *db = open_db(true);
    ackq_t *q = open_queue(db);
    for (int i = 0; i < 3; i++) {
        uint64_t id;
        assert(enq(q, NULL, "p", NULL, &id, NULL) == ACKQ_OK);
    }
    ackq_close(&q);
    assert(database_close(db) == DATABASE_OK);
}

static ackq_status_t run_claim(ackq_t *q)
{
    ackq_claim_t items[2];
    size_t n = 0;
    ackq_status_t st = ackq_claim(q, NULL, 2, 1000, items, &n);
    CHECK(st == ACKQ_OK ? n == 2 : n == 0, "성공이 아니면 claim한 항목을 주지 않는다 (st=%s n=%zu)", ackq_status_name(st), n);
    ackq_claims_release(items, n);
    return st;
}

static void verify_claim(database_t *db, ackq_t *q, ackq_status_t st, bool acknowledged)
{
    (void)db;
    (void)st;
    ackq_stats_t stats;
    CHECK(ackq_stats(q, NULL, &stats) == ACKQ_OK, "stats");
    CHECK(stats.leased == 0 || stats.leased == 2, "배치는 전부 또는 전혀 (leased=%llu)", (unsigned long long)stats.leased);
    CHECK(stats.ready + stats.leased == 3, "작업이 사라지지 않았다");
    if (acknowledged) CHECK(stats.leased == 2, "성공으로 알린 claim은 재시작 뒤에도 lease다");
    g_now = 100000;   /* 모든 lease가 만료된 뒤 */
    ackq_claim_t items[3];
    size_t n = 0;
    CHECK(ackq_claim(q, NULL, 3, 1000, items, &n) == ACKQ_OK && n == 3, "만료 뒤 셋 모두 다시 전달된다 (n=%zu)", n);
    ackq_claims_release(items, n);
    g_now = 100;
}

static const scenario_t SCENARIOS[] = {
    {"ack", prepare_ack, run_ack, verify_ack},
    {"enqueue", prepare_enqueue, run_enqueue, verify_enqueue},
    {"claim", prepare_claim, run_claim, verify_claim},
};

/* 주입 없이 한 번 돌려 연산이 지나는 I/O 지점 수를 센다 */
static long count_points(const scenario_t *sc, const snap_t *base)
{
    snap_restore(base);
    hooks_on();
    database_t *db = open_db(true);
    ackq_t *q = open_queue(db);
    g_fail_mode = true;
    g_fail_at = 0;
    g_points_seen = 0;
    g_active = true;
    ackq_status_t st = sc->run(q);
    g_active = false;
    CHECK(st == ACKQ_OK, "%s: 주입 없는 실행은 성공", sc->name);
    ackq_close(&q);
    database_close(db);
    hooks_off();
    return g_points_seen;
}

static void reopen_and_verify(const scenario_t *sc, ackq_status_t st, bool acknowledged)
{
    database_t *db = open_db(true);
    CHECK(db != NULL, "%s: 재시작 열기", sc->name);
    if (db == NULL) return;
    ackq_t *q = open_queue(db);
    CHECK(q != NULL, "%s: 재시작 뒤 큐 열기", sc->name);
    if (q != NULL) sc->verify(db, q, st, acknowledged);
    ackq_close(&q);
    database_close(db);
}

/* I/O 실패 주입: k번째 지점이 실패한다 */
static void sweep_io_failures(const scenario_t *sc)
{
    snap_t base;
    sc->prepare();
    snap_take(&base);
    long points = count_points(sc, &base);
    CHECK(points > 0, "%s: I/O 지점이 있다", sc->name);
    int ok = 0, uncertain = 0, other_error = 0;
    for (long k = 1; k <= points; k++) {
        snap_restore(&base);
        hooks_on();
        database_t *db = open_db(true);
        ackq_t *q = db != NULL ? open_queue(db) : NULL;
        if (q == NULL) {
            CHECK(false, "%s k=%ld: 열기 실패", sc->name, k);
            hooks_off();
            if (db != NULL) database_close(db);
            continue;
        }
        g_fail_mode = true;
        g_fail_at = k;
        g_fail_seen = 0;
        g_points_seen = 0;
        g_active = true;
        ackq_status_t st = sc->run(q);
        g_active = false;
        ackq_close(&q);
        database_close(db);
        hooks_off();
        if (st == ACKQ_OK) ok++;
        else if (st == ACKQ_UNCERTAIN || st == ACKQ_POISONED) uncertain++;
        else other_error++;
        reopen_and_verify(sc, st, st == ACKQ_OK);
    }
    printf("  %s: I/O 실패 %ld곳 -> 성공 %d, 불확실 %d, 그 밖의 오류 %d\n", sc->name, points, ok, uncertain, other_error);
    CHECK(ok + uncertain + other_error == (int)points, "%s: 모든 지점을 분류했다", sc->name);
    CHECK(uncertain + other_error > 0, "%s: 주입이 실제로 실패를 만들었다", sc->name);
    snap_free(&base);
}

/* 프로세스 종료 주입: k번째 지점에서 _exit. 성공을 알렸다면 그 변경은 남아야 한다 */
static void sweep_kills(const scenario_t *sc)
{
    snap_t base;
    sc->prepare();
    snap_take(&base);
    long points = count_points(sc, &base);
    int survived_acknowledged = 0, died_before_ack = 0;
    for (long k = 1; k <= points; k++) {
        snap_restore(&base);
        int pipefd[2];
        assert(pipe(pipefd) == 0);
        fflush(stdout);
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            close(pipefd[0]);
            hooks_on();
            database_t *db = open_db(true);
            ackq_t *q = db != NULL ? open_queue(db) : NULL;
            if (q == NULL) _exit(4);
            g_fail_mode = false;
            g_points_left = k - 1;
            g_active = true;
            ackq_status_t st = sc->run(q);
            g_active = false;
            char tag = st == ACKQ_OK ? 'O' : 'E';
            if (write(pipefd[1], &tag, 1) != 1) _exit(5);
            _exit(0);
        }
        close(pipefd[1]);
        char tag = 0;
        ssize_t got = read(pipefd[0], &tag, 1);
        close(pipefd[0]);
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);
        bool acknowledged = got == 1 && tag == 'O';
        if (acknowledged) survived_acknowledged++;
        else died_before_ack++;
        reopen_and_verify(sc, acknowledged ? ACKQ_OK : ACKQ_UNCERTAIN, acknowledged);
    }
    printf("  %s: 종료 %ld곳 -> 성공을 알린 뒤 종료 %d, 알리기 전 종료 %d\n", sc->name, points, survived_acknowledged,
           died_before_ack);
    CHECK(died_before_ack > 0, "%s: 종료 주입이 실제로 동작했다", sc->name);
    snap_free(&base);
}

int main(void)
{
    set_dir(DIR);
    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); i++) {
        printf("\n[%s: I/O 실패 주입과 재시작]\n", SCENARIOS[i].name);
        sweep_io_failures(&SCENARIOS[i]);
        printf("[%s: 프로세스 종료 주입과 재시작]\n", SCENARIOS[i].name);
        sweep_kills(&SCENARIOS[i]);
    }
    printf("\ntest_ack_queue_fault: %d checks, %d failed\n", g_run, g_fail);
    if (g_fail == 0) remove_dir();
    return g_fail == 0 ? 0 : 1;
}
