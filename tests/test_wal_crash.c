/*
 * test_wal_crash.c -- WAL kill/restart 합성 DB 검사 (#9)
 *
 * 자식 프로세스가 합성 DB(tests/wal_model.h)에서 거래를 실행하다 죽고, 부모가 로그를 다시 열어
 * "승인한 commit 집합"과 복구 결과를 대조한다.
 *
 *  1. sweep: WAL의 pwrite·fsync·ftruncate 앞뒤와 모형의 게시 단계마다 하나씩 _exit(137)로 끝낸다.
 *     종료 지점 k를 처음부터 끝까지 모두 훑는다. 같은 종료 결과에 세 가지 저장 장치 모형을 적용한다.
 *       v0 프로세스 종료: 파일이 그대로(커널 캐시의 쓰기는 남는다)
 *       v1 fsync되지 않은 꼬리 소실: 마지막 fsync 성공 시점의 크기로 자른다
 *       v2 꼬리 일부 소실: 마지막 fsync 성공 크기 + 나머지의 절반으로 자른다(잘린 레코드가 생긴다)
 *  2. kill: 무한 거래 루프를 도는 자식에 임의 시점에 SIGKILL을 보낸다(v0).
 *
 * 각 복구 뒤 확인: 승인한(acked) 거래 ⊆ 로그의 COMMITTED ⊆ commit을 시도한(attempted) 거래,
 * 합성 DB 불변식, 같은 로그를 다시 복구해도 같은 상태, 복구 뒤 다음 거래의 commit과 재열기.
 * MVCC(ALL replay + page LSN)와 COW(COMMITTED_ONLY replay + applied_lsn) 두 소비자 계약 모두 돌린다.
 *
 * 이 검사는 "프로세스 종료"와 "fsync되지 않은 꼬리를 버리는 저장 장치" 모형만 다룬다. 전원 장애,
 * 페이지 내부 부분 쓰기, 저장 장치가 fsync 성공 뒤 데이터를 잃는 경우는 검사하지 않는다.
 *
 * 빌드(저장소 루트에서):
 *   cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=undefined -Iinclude \
 *      -o build/test_wal_crash tests/test_wal_crash.c src/storage/wal.c
 * 실행은 build 폴더 안에서 한다: (cd build && ./test_wal_crash)
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

#include "wal_model.h"

#define LOG "__test__walc.log"
#define DATA "__test__walc.dat"
#define SWEEP_TXNS 16

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

static uint8_t *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat sb;
    fstat(fd, &sb);
    uint8_t *b = malloc((size_t)sb.st_size + 1);
    size_t got = 0;
    while (got < (size_t)sb.st_size) {
        ssize_t n = pread(fd, b + got, (size_t)sb.st_size - got, (off_t)got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    *len = got;
    return b;
}

static void write_file(const char *path, const uint8_t *b, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t done = 0;
    while (fd >= 0 && done < len) {
        ssize_t n = write(fd, b + done, len - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    if (fd >= 0) close(fd);
}

static void clean_files(void)
{
    unlink(LOG);
    unlink(DATA);
    unlink(DATA ".new");
}

typedef struct {
    uint8_t *bytes;
    size_t len;
    bool present;
} snap_t;

static void snap_take(snap_t *s, const char *path)
{
    s->bytes = read_file(path, &s->len);
    s->present = s->bytes != NULL;
}

static void snap_restore(const snap_t *s, const char *path)
{
    if (s->present) write_file(path, s->bytes, s->len);
    else unlink(path);
}

static void snap_free(snap_t *s)
{
    free(s->bytes);
    s->bytes = NULL;
}

/* 자식이 하는 일: 로그를 만들고 거래를 규칙대로 실행한다. */
static void child_workload(model_mode_t mode, uint64_t txns)
{
    model_t *m = calloc(1, sizeof(*m));
    if (model_open(m, DATA, mode) != 0) _exit(90);
    crash_point();
    if (wal_create(LOG, &m->wal) != WAL_OK) _exit(91);
    for (uint64_t x = 1; x <= txns; x++) {
        if (model_run_txn(m, x) < 0) _exit(92);
    }
    wal_close(m->wal);
    model_close(m);
    _exit(0);
}

typedef struct {
    int acked_not_committed;
    int committed_not_attempted;
} counts_t;

/* 복구해 대조한다. 실패하면 이유를 출력한다. */
static bool verify(model_mode_t mode, const model_shared_t *sh, const char *label)
{
    bool ok = true;
    wal_set_default_io(NULL);
    if (access(LOG, F_OK) != 0) {
        /* 로그를 만들기 전에 끝났다 */
        if (sh->acked_n != 0) {
            printf("  [%s] 로그가 없는데 승인한 commit이 %llu개\n", label, (unsigned long long)sh->acked_n);
            return false;
        }
        return true;
    }
    model_t *m = calloc(1, sizeof(*m));
    wal_open_info_t info;
    wal_replay_stats_t st;
    int rc = model_recover(m, LOG, DATA, mode, &info, &st);
    if (rc != 0) {
        printf("  [%s] 복구 실패 rc=%d\n", label, rc);
        free(m);
        return false;
    }
    for (uint64_t i = 0; i < sh->acked_n; i++) {
        uint64_t x = sh->acked[i];
        if (!wal_xid_known(m->wal, x) || wal_xid_state(m->wal, x) != WAL_XID_COMMITTED) {
            printf("  [%s] 승인한 commit xid=%llu이 복구 뒤 COMMITTED가 아니다\n", label, (unsigned long long)x);
            ok = false;
        }
    }
    for (uint64_t x = 1; x <= wal_max_xid(m->wal); x++) {
        if (!wal_xid_known(m->wal, x) || wal_xid_state(m->wal, x) != WAL_XID_COMMITTED) continue;
        bool attempted = false;
        for (uint64_t i = 0; i < sh->attempted_n; i++) attempted |= sh->attempted[i] == x;
        if (!attempted) {
            printf("  [%s] commit을 시도하지 않은 xid=%llu이 COMMITTED다\n", label, (unsigned long long)x);
            ok = false;
        }
        if (txn_fate(x) != FATE_COMMIT) {
            printf("  [%s] commit하지 않는 운명의 xid=%llu이 COMMITTED다\n", label, (unsigned long long)x);
            ok = false;
        }
    }
    ok = model_check_invariants(m, label) && ok;
    uint64_t fp1 = model_fingerprint(m);
    wal_close(m->wal);
    model_close(m);

    /* 같은 로그를 다시 복구해도 같다 */
    wal_replay_stats_t st2;
    int rc2 = model_recover(m, LOG, DATA, mode, &info, &st2);
    if (rc2 != 0 || model_fingerprint(m) != fp1) {
        printf("  [%s] 반복 복구 결과가 다르다 rc=%d\n", label, rc2);
        ok = false;
    }
    if (rc2 == 0) {
        ok = model_check_invariants(m, label) && ok;
        /* 복구 뒤 다음 거래가 실행되고 다시 열어도 보인다 */
        uint64_t nx = wal_max_xid(m->wal) + 1000;
        model_txn_t t;
        txn_begin(&t, nx);
        for (unsigned j = 0; j < txn_rows(nx); j++) model_insert(m, &t, txn_row_id(nx, j), txn_value(nx, j));
        if (model_commit(m, &t) != WAL_OK) {
            printf("  [%s] 복구 뒤 다음 거래가 commit되지 않는다\n", label);
            ok = false;
        }
        wal_close(m->wal);
        model_close(m);
        int rc3 = model_recover(m, LOG, DATA, mode, &info, &st2);
        if (rc3 != 0 || wal_xid_state(m->wal, nx) != WAL_XID_COMMITTED || !model_check_invariants(m, label)) {
            printf("  [%s] 다음 거래를 다시 열었을 때 보이지 않는다 rc=%d\n", label, rc3);
            ok = false;
        }
        if (rc3 == 0) wal_close(m->wal);
    }
    model_close(m);
    free(m);
    return ok;
}

static model_shared_t *shared_new(void)
{
    void *p = mmap(NULL, sizeof(model_shared_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        printf("mmap 실패\n");
        exit(2);
    }
    memset(p, 0, sizeof(model_shared_t));
    return p;
}

static void apply_variant(int v, const snap_t *log0, const model_shared_t *sh)
{
    if (!log0->present) {
        unlink(LOG);
        return;
    }
    size_t len = log0->len;
    if (v == 1 && sh->synced < len) len = (size_t)sh->synced;
    if (v == 2 && sh->synced < len) len = (size_t)sh->synced + (len - (size_t)sh->synced) / 2;
    write_file(LOG, log0->bytes, len);
}

static void sweep(model_mode_t mode, const char *name)
{
    printf("\n[sweep %s]\n", name);
    clean_files();
    model_install_hooks();
    g_shared = NULL;
    g_points_left = -1;
    g_points_seen = 0;
    pid_t pid = fork();
    if (pid == 0) child_workload(mode, SWEEP_TXNS);
    int status;
    waitpid(pid, &status, 0);
    /* 종료 지점 수는 자식이 센 값을 알 수 없으므로 부모가 같은 작업을 직접 센다 */
    clean_files();
    {
        long seen0 = g_points_seen;
        model_t *m = calloc(1, sizeof(*m));
        model_open(m, DATA, mode);
        crash_point();
        wal_create(LOG, &m->wal);
        for (uint64_t x = 1; x <= SWEEP_TXNS; x++) model_run_txn(m, x);
        wal_close(m->wal);
        model_close(m);
        free(m);
        g_points_seen -= seen0;
    }
    long total = g_points_seen;
    clean_files();
    wal_set_default_io(NULL);
    printf("  종료 지점 %ld곳 x 저장 장치 모형 3가지\n", total);
    CHECK(total > SWEEP_TXNS * 4, "종료 지점이 충분하다 (%ld)", total);

    int bad = 0, runs = 0, crashed = 0;
    model_shared_t *sh = shared_new();
    for (long k = 0; k <= total; k++) {
        clean_files();
        memset(sh, 0, sizeof(*sh));
        pid = fork();
        if (pid == 0) {
            model_install_hooks();
            g_shared = sh;
            g_points_left = k;
            child_workload(mode, SWEEP_TXNS);
        }
        waitpid(pid, &status, 0);
        bool died = WIFEXITED(status) && WEXITSTATUS(status) == 137;
        if (!(WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 137))) {
            printf("  k=%ld 자식이 예상 밖으로 끝남 status=%d\n", k, status);
            bad++;
            continue;
        }
        crashed += died;
        snap_t log0, data0, new0;
        snap_take(&log0, LOG);
        snap_take(&data0, DATA);
        snap_take(&new0, DATA ".new");
        for (int v = 0; v < 3; v++) {
            apply_variant(v, &log0, sh);
            snap_restore(&data0, DATA);
            snap_restore(&new0, DATA ".new");
            char label[96];
            snprintf(label, sizeof(label), "%s k=%ld v%d", name, k, v);
            runs++;
            if (!verify(mode, sh, label)) bad++;
        }
        snap_free(&log0);
        snap_free(&data0);
        snap_free(&new0);
    }
    munmap(sh, sizeof(*sh));
    CHECK(bad == 0 && crashed > 0, "%s: %d회 복구 대조 중 어긋남 %d (비정상 종료 %d회)", name, runs, bad, crashed);
    clean_files();
}

static void kill_test(model_mode_t mode, const char *name, int rounds)
{
    printf("\n[kill -9 %s]\n", name);
    model_shared_t *sh = shared_new();
    unsigned long seed = (unsigned long)time(NULL) ^ 0x9E3779B9ul;
    int bad = 0, killed = 0, with_acks = 0;
    for (int r = 0; r < rounds; r++) {
        clean_files();
        memset(sh, 0, sizeof(*sh));
        pid_t pid = fork();
        if (pid == 0) {
            model_install_hooks();
            g_shared = sh;
            g_points_left = -1;
            child_workload(mode, 1500);
        }
        /* 홀수 라운드는 첫 승인 commit 뒤에 죽인다. fsync가 느린 저장소에서도 승인 있는 종료를 보장한다 */
        if (r % 2 == 1) {
            for (int spin = 0; spin < 5000 && ((volatile model_shared_t *)sh)->acked_n == 0; spin++) usleep(1000);
        }
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        usleep((useconds_t)(300 + (seed >> 33) % 9000));
        kill(pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        killed += WIFSIGNALED(status);
        with_acks += sh->acked_n > 0;
        char label[96];
        snprintf(label, sizeof(label), "%s kill#%d acked=%llu", name, r, (unsigned long long)sh->acked_n);
        if (!verify(mode, sh, label)) bad++;
    }
    munmap(sh, sizeof(*sh));
    CHECK(bad == 0, "%s: SIGKILL %d회 복구 대조 어긋남 %d (실제 종료 %d회, 승인 있는 라운드 %d)", name, rounds, bad,
          killed, with_acks);
    CHECK(killed > 0 && with_acks > 0, "%s: 승인된 commit이 있는 상태에서 실제로 죽였다", name);
    clean_files();
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 30;
    clean_files();
    sweep(MODEL_COW, "COW·COMMITTED_ONLY");
    sweep(MODEL_MVCC, "MVCC·ALL");
    kill_test(MODEL_COW, "COW·COMMITTED_ONLY", rounds);
    kill_test(MODEL_MVCC, "MVCC·ALL", rounds);
    clean_files();
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
