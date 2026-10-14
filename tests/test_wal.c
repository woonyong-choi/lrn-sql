/*
 * test_wal.c -- WAL 파일·복구 모듈 검사 (#9)
 *
 * 합성 데이터만 쓴다. 로그 파일은 현재 폴더의 __test__wal_*에 만들고 끝에 지운다.
 * 검사 범위: 형식·순서·잘린 꼬리와 중간 손상의 구분, commit fsync 순서, I/O 실패 주입,
 * 두 replay 계약(ALL, COMMITTED_ONLY)의 합성 DB 복구와 반복 복구.
 * 프로세스 종료(kill/restart)는 test_wal_crash.c가 맡는다. 전원 장애 보장은 검사하지 않는다.
 *
 * 빌드(저장소 루트에서):
 *   cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Iinclude \
 *      -o build/test_wal tests/test_wal.c src/storage/wal.c
 * 실행은 build 폴더 안에서 한다: (cd build && ./test_wal)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wal_model.h"

#define LOG "__test__wal_a.log"
#define LOG2 "__test__wal_b.log"
#define DATA "__test__wal_a.dat"

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

/* ---- 파일 도우미 ---- */

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

static uint64_t fnv(const uint8_t *b, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t file_hash(const char *path, size_t *len)
{
    uint8_t *b = read_file(path, len);
    uint64_t h = b != NULL ? fnv(b, *len) : 0;
    free(b);
    return h;
}

static void cleanup(void)
{
    unlink(LOG);
    unlink(LOG2);
    unlink(DATA);
    unlink(DATA ".new");
}

/* ---- 로그 만들기 ---- */

typedef struct {
    uint64_t lsn[256], end[256];
    int kind[256];
    int n;
} layout_t;

static void note(layout_t *lay, wal_t *w, wal_kind_t kind, wal_lsn_t lsn)
{
    if (lay == NULL) return;
    lay->lsn[lay->n] = lsn;
    lay->end[lay->n] = wal_end_lsn(w);
    lay->kind[lay->n] = kind;
    lay->n++;
}

/* txns개 거래(DATA 둘 + COMMIT). tail_data면 마지막에 flush한 DATA 하나(미완 거래 99)를 더한다. */
static void build_log(const char *path, int txns, bool tail_data, layout_t *lay)
{
    unlink(path);
    wal_t *w = NULL;
    if (wal_create(path, &w) != WAL_OK) {
        printf("로그를 만들지 못했습니다\n");
        exit(2);
    }
    if (lay != NULL) lay->n = 0;
    for (int x = 1; x <= txns; x++) {
        for (int j = 0; j < 2; j++) {
            char pl[32];
            snprintf(pl, sizeof(pl), "row-%d-%d", x, j);
            wal_lsn_t lsn;
            wal_append(w, (wal_xid_t)x, (uint32_t)j, pl, (uint32_t)strlen(pl), &lsn);
            note(lay, w, WAL_REC_DATA, lsn);
        }
        wal_lsn_t c;
        wal_commit(w, (wal_xid_t)x, &c);
        note(lay, w, WAL_REC_COMMIT, c);
    }
    if (tail_data) {
        wal_lsn_t lsn;
        wal_append(w, 99, 1, "thirteen-byte", 13, &lsn);
        note(lay, w, WAL_REC_DATA, lsn);
        wal_flush(w, lsn);
    }
    wal_close(w);
}

/* ---- 1. 기본 왕복 ---- */

static int g_redo_count;
static wal_redo_result_t count_redo(void *ctx, const wal_record_t *r)
{
    (void)ctx;
    (void)r;
    g_redo_count++;
    return WAL_REDO_APPLIED;
}

static void test_roundtrip(void)
{
    printf("\n[test_roundtrip]\n");
    unlink(LOG);
    wal_t *w;
    CHECK(wal_create(LOG, &w) == WAL_OK, "create");
    CHECK(wal_create(LOG, &(wal_t *){NULL}) == WAL_EXISTS, "이미 있는 목적지는 EXISTS");
    wal_lsn_t l1, l2, l3, c;
    CHECK(wal_append(w, 1, 7, "aaa", 3, &l1) == WAL_OK, "append1");
    CHECK(wal_append(w, 1, 8, NULL, 0, &l2) == WAL_OK, "빈 페이로드 append");
    CHECK(l1 == WAL_FIRST_LSN && l2 > l1, "LSN은 32에서 시작해 늘어난다 (%llu %llu)", (unsigned long long)l1,
          (unsigned long long)l2);
    CHECK(wal_flushed_lsn(w) == 0, "append만으로는 fsync 되지 않는다");
    CHECK(wal_require_flushed(w, l2) == WAL_NOT_FLUSHED, "fsync 전 페이지 LSN은 거절");
    CHECK(wal_commit(w, 1, &c) == WAL_OK && c > l2, "commit");
    CHECK(wal_require_flushed(w, l2) == WAL_OK && wal_flushed_lsn(w) == c, "commit 뒤 앞선 레코드도 fsync됨");
    CHECK(wal_append(w, 2, 1, "bb", 2, &l3) == WAL_OK, "다른 거래 append");
    CHECK(wal_xid_state(w, 1) == WAL_XID_COMMITTED, "xid1 COMMITTED");
    CHECK(wal_close(w) == WAL_OK, "close(미flush 레코드는 버려진다)");

    wal_open_info_t info;
    CHECK(wal_open(LOG, false, &w, &info) == WAL_OK, "읽기 열기");
    CHECK(info.records == 3 && info.committed == 1 && info.crashed == 0 && info.xids == 1,
          "버려진 xid2는 로그에 없다 (records=%llu)", (unsigned long long)info.records);
    CHECK(wal_xid_state(w, 1) == WAL_XID_COMMITTED && !wal_xid_known(w, 2), "상태 복원");
    wal_replay_ops_t ops = {WAL_REPLAY_ALL, 0, count_redo, NULL, NULL};
    wal_replay_stats_t st;
    g_redo_count = 0;
    CHECK(wal_replay(w, &ops, NULL, &st) == WAL_OK && g_redo_count == 2 && st.applied == 2, "DATA 둘 재생");
    wal_close(w);
    cleanup();
}

/* ---- 2. commit과 fsync 순서 ---- */

static char g_seq[64];
static size_t g_seq_n;
static ssize_t sq_pread(int fd, void *b, size_t n, off_t o) { return pread(fd, b, n, o); }
static ssize_t sq_pwrite(int fd, const void *b, size_t n, off_t o)
{
    if (g_seq_n + 1 < sizeof(g_seq)) g_seq[g_seq_n++] = 'W';
    return pwrite(fd, b, n, o);
}
static int sq_fsync(int fd)
{
    if (g_seq_n + 1 < sizeof(g_seq)) g_seq[g_seq_n++] = 'S';
    return fsync(fd);
}
static int sq_ftruncate(int fd, off_t n) { return ftruncate(fd, n); }

static void test_commit_order(void)
{
    printf("\n[test_commit_order]\n");
    unlink(LOG);
    static const wal_io_t io = {sq_pread, sq_pwrite, sq_fsync, sq_ftruncate};
    wal_set_default_io(&io);
    wal_t *w;
    wal_create(LOG, &w);
    g_seq_n = 0;
    g_seq[0] = 0;
    wal_append(w, 1, 1, "x", 1, NULL);
    wal_append(w, 1, 2, "y", 1, NULL);
    CHECK(g_seq_n == 0, "append는 디스크 I/O를 하지 않는다 (%s)", g_seq);
    CHECK(wal_commit(w, 1, NULL) == WAL_OK, "commit");
    g_seq[g_seq_n] = 0;
    CHECK(strcmp(g_seq, "WS") == 0, "commit은 쓰기 뒤 fsync 한 번 (%s)", g_seq);
    wal_close(w);
    wal_set_default_io(NULL);
    cleanup();
}

/* ---- 3. 호출 규칙 ---- */

static void test_call_rules(void)
{
    printf("\n[test_call_rules]\n");
    unlink(LOG);
    wal_t *w;
    wal_create(LOG, &w);
    wal_lsn_t end0 = wal_end_lsn(w);
    CHECK(wal_append(w, 0, 1, "a", 1, NULL) == WAL_INVALID, "xid 0 거절");
    CHECK(wal_append(w, 1, 1, NULL, 3, NULL) == WAL_INVALID, "NULL 페이로드에 길이");
    CHECK(wal_append(w, 1, 1, "a", WAL_MAX_PAYLOAD + 1, NULL) == WAL_INVALID, "큰 페이로드 거절");
    CHECK(wal_end_lsn(w) == end0, "거절은 상태를 바꾸지 않는다");
    wal_append(w, 1, 1, "a", 1, NULL);
    CHECK(wal_commit(w, 1, NULL) == WAL_OK, "commit");
    CHECK(wal_commit(w, 1, NULL) == WAL_INVALID, "같은 거래 두 번 commit 거절");
    CHECK(wal_append(w, 1, 1, "a", 1, NULL) == WAL_INVALID, "끝난 거래에 DATA 거절");
    CHECK(wal_abort(w, 1, NULL) == WAL_INVALID, "commit된 거래 abort 거절");
    CHECK(wal_abort(w, 2, NULL) == WAL_OK, "DATA 없는 거래도 abort 가능");
    CHECK(wal_append(w, 2, 1, "a", 1, NULL) == WAL_INVALID, "abort된 거래에 DATA 거절");
    CHECK(wal_flush(w, 0) == WAL_INVALID && wal_flush(w, 99999) == WAL_INVALID, "범위 밖 flush");
    CHECK(wal_commit(w, 0, NULL) == WAL_INVALID, "xid 0 commit");
    wal_close(w);
    wal_t *r;
    CHECK(wal_open(LOG, false, &r, NULL) == WAL_OK, "읽기 열기");
    CHECK(wal_append(r, 5, 1, "a", 1, NULL) == WAL_INVALID, "읽기 핸들은 쓰지 못한다");
    wal_close(r);
    cleanup();
}

/* ---- 4. 잘린 꼬리 ---- */

static void test_torn_tail(void)
{
    printf("\n[test_torn_tail]\n");
    for (int variant = 0; variant < 2; variant++) {
        layout_t lay;
        build_log(LOG, 3, variant == 1, &lay);
        size_t len;
        uint8_t *orig = read_file(LOG, &len);
        int last = lay.n - 1;
        uint64_t L = lay.lsn[last], E = lay.end[last];
        CHECK(E == len, "레이아웃과 파일 크기 일치");
        int cuts_ok = 0, cuts = 0;
        for (uint64_t cut = L + 1; cut < E; cut++) {
            write_file(LOG2, orig, (size_t)cut);
            wal_t *w = NULL;
            wal_open_info_t info;
            wal_status_t st = wal_open(LOG2, true, &w, &info);
            size_t after;
            file_hash(LOG2, &after);
            bool ok = st == WAL_OK && info.truncated && info.valid_end == L && info.torn_bytes == cut - L &&
                      after == L && info.records == (uint64_t)last;
            if (ok && variant == 0) ok = wal_xid_state(w, 3) == WAL_XID_CRASHED;
            cuts++;
            cuts_ok += ok;
            if (!ok) printf("  cut=%llu status=%s\n", (unsigned long long)cut, wal_status_name(st));
            if (w != NULL) wal_close(w);
        }
        CHECK(cuts_ok == cuts && cuts > 0, "변형%d: 마지막 레코드의 모든 잘림(%d곳)이 잘린 꼬리로 복구", variant, cuts);

        /* 헤더 일부만 쓰이고 나머지가 0이어도 파일 길이가 56바이트 이상이면 잘림으로 인정하지 않는다 */
        uint8_t *z = calloc(1, len + 200);
        memcpy(z, orig, (size_t)L + 10);
        write_file(LOG2, z, len + 100);
        wal_t *w = NULL;
        wal_open_info_t info;
        size_t pl0, pl1;
        uint64_t ph0 = file_hash(LOG2, &pl0);
        CHECK(wal_open(LOG2, true, &w, &info) == WAL_CORRUPT && info.corrupt_lsn == L && file_hash(LOG2, &pl1) == ph0 &&
                  pl1 == pl0,
              "변형%d: 헤더 일부 + 0 채움(56바이트 이상)은 CORRUPT이고 원본 보존", variant);
        if (w != NULL) wal_close(w);
        /* 완전한 로그 뒤에 0이 56바이트 미만이면 잘림, 이상이면 손상 */
        memcpy(z, orig, len);
        write_file(LOG2, z, len + 20);
        CHECK(wal_open(LOG2, true, &w, &info) == WAL_OK && info.truncated && info.valid_end == E,
              "변형%d: 56바이트 미만의 0 꼬리는 잘림", variant);
        if (w != NULL) wal_close(w);
        size_t zl0, zl1;
        write_file(LOG2, z, len + 100);
        uint64_t zh0 = file_hash(LOG2, &zl0);
        CHECK(wal_open(LOG2, true, &w, &info) == WAL_CORRUPT && info.corrupt_lsn == E &&
                  file_hash(LOG2, &zl1) == zh0 && zl1 == zl0,
              "변형%d: 56바이트 이상의 0 꼬리는 CORRUPT이고 원본 보존", variant);
        free(z);

        /* 읽기 열기는 파일을 바꾸지 않는다 */
        write_file(LOG2, orig, (size_t)L + 20);
        size_t l0, l1;
        uint64_t h0 = file_hash(LOG2, &l0);
        CHECK(wal_open(LOG2, false, &w, &info) == WAL_OK && !info.truncated && info.torn_bytes == 20,
              "변형%d: 읽기 열기는 꼬리를 보고만 한다", variant);
        if (w != NULL) wal_close(w);
        CHECK(file_hash(LOG2, &l1) == h0 && l1 == l0, "변형%d: 읽기 열기 뒤 파일 그대로", variant);

        /* 잘린 꼬리 복구 뒤 같은 핸들로 다음 거래 */
        write_file(LOG2, orig, (size_t)L + 5);
        CHECK(wal_open(LOG2, true, &w, &info) == WAL_OK, "다시 열기");
        wal_lsn_t c;
        CHECK(wal_append(w, 50, 1, "n", 1, NULL) == WAL_OK && wal_commit(w, 50, &c) == WAL_OK && c >= L,
              "변형%d: 잘라 낸 자리에서 다음 거래 commit", variant);
        wal_close(w);
        CHECK(wal_open(LOG2, false, &w, &info) == WAL_OK && wal_xid_state(w, 50) == WAL_XID_COMMITTED,
              "변형%d: 다시 열어도 보임", variant);
        if (w != NULL) wal_close(w);
        free(orig);
    }
    cleanup();
}

/* ---- 5. 완전한 레코드의 불일치: 마지막이어도 거절 ---- */

static void test_bitflip_rejected(void)
{
    printf("\n[test_bitflip_rejected]\n");
    for (int variant = 0; variant < 2; variant++) {
        layout_t lay;
        build_log(LOG, 4, variant == 1, &lay);
        size_t len;
        uint8_t *orig = read_file(LOG, &len);
        int last = lay.n - 1;
        int mid = lay.n / 2;
        const char *what[] = {"마지막이 COMMIT", "마지막이 DATA(패딩 포함)"};
        struct {
            uint64_t from, to, expect_lsn;
            const char *name;
        } spans[] = {
            {lay.lsn[last], lay.end[last], lay.lsn[last], "마지막 레코드"},
            {lay.lsn[mid], lay.end[mid], lay.lsn[mid], "중간 레코드"},
        };
        for (int s = 0; s < 2; s++) {
            int bad = 0, total = 0;
            for (uint64_t off = spans[s].from; off < spans[s].to; off++) {
                for (int bit = 0; bit < 8; bit += 7) { /* 최하위·최상위 비트 */
                    uint8_t *m = malloc(len);
                    memcpy(m, orig, len);
                    m[off] ^= (uint8_t)(1u << bit);
                    write_file(LOG2, m, len);
                    free(m);
                    size_t l0, l1;
                    uint64_t h0 = file_hash(LOG2, &l0);
                    wal_t *w = NULL;
                    wal_open_info_t info;
                    wal_status_t st = wal_open(LOG2, true, &w, &info);
                    wal_status_t st2 = wal_open(LOG2, false, &w, NULL);
                    total++;
                    bool ok = st == WAL_CORRUPT && st2 == WAL_CORRUPT && info.corrupt_lsn == spans[s].expect_lsn &&
                              w == NULL && file_hash(LOG2, &l1) == h0 && l1 == l0;
                    if (!ok) {
                        bad++;
                        printf("  %s off=%llu bit=%d status=%s/%s corrupt_lsn=%llu\n", spans[s].name,
                               (unsigned long long)off, bit, wal_status_name(st), wal_status_name(st2),
                               (unsigned long long)info.corrupt_lsn);
                    }
                    if (w != NULL) wal_close(w);
                }
            }
            CHECK(bad == 0 && total > 0, "%s %s: 모든 비트 반전(%d곳)이 CORRUPT이고 원본 보존", what[variant],
                  spans[s].name, total);
        }
        /* 승인된 레코드의 헤더 CRC 4바이트만 0이 되거나 헤더 뒷부분이 0이 되는 변질 */
        for (int which = 0; which < 2; which++) {
            for (int part = 0; part < 3; part++) {
                int idx = which == 0 ? last : mid;
                size_t from = part == 0 ? 52 : part == 1 ? 48 : 40;
                uint8_t *m = malloc(len);
                memcpy(m, orig, len);
                memset(m + lay.lsn[idx] + from, 0, 56 - from);
                write_file(LOG2, m, len);
                free(m);
                size_t l0, l1;
                uint64_t h0 = file_hash(LOG2, &l0);
                wal_t *w = NULL;
                wal_open_info_t info;
                wal_status_t st = wal_open(LOG2, true, &w, &info);
                CHECK(st == WAL_CORRUPT && info.corrupt_lsn == lay.lsn[idx] && w == NULL && file_hash(LOG2, &l1) == h0 &&
                          l1 == l0,
                      "%s: %s 헤더 [%zu,56)가 0이면 CORRUPT이고 원본 보존 (%s)", what[variant],
                      which == 0 ? "마지막 레코드" : "중간 레코드", from, wal_status_name(st));
                if (w != NULL) wal_close(w);
            }
        }
        /* 승인된 레코드가 통째로 0으로 덮이면 마지막이어도 잘림이 아니라 손상이다 */
        for (int which = 0; which < 2; which++) {
            int idx = which == 0 ? last : mid;
            uint8_t *m = malloc(len);
            memcpy(m, orig, len);
            memset(m + lay.lsn[idx], 0, (size_t)(lay.end[idx] - lay.lsn[idx]));
            write_file(LOG2, m, len);
            free(m);
            size_t l0, l1;
            uint64_t h0 = file_hash(LOG2, &l0);
            wal_t *w = NULL;
            wal_open_info_t info;
            wal_status_t st = wal_open(LOG2, true, &w, &info);
            CHECK(st == WAL_CORRUPT && info.corrupt_lsn == lay.lsn[idx] && w == NULL && file_hash(LOG2, &l1) == h0 &&
                      l1 == l0,
                  "%s: %s 전체가 0이면 CORRUPT이고 원본 보존 (%s)", what[variant],
                  which == 0 ? "마지막 레코드" : "중간 레코드", wal_status_name(st));
            if (w != NULL) wal_close(w);
        }
        /* 파일 헤더 32바이트 */
        int hbad = 0;
        for (int off = 0; off < (int)WAL_HEADER_BYTES; off++) {
            uint8_t *m = malloc(len);
            memcpy(m, orig, len);
            m[off] ^= 0x01;
            write_file(LOG2, m, len);
            free(m);
            wal_t *w = NULL;
            wal_status_t st = wal_open(LOG2, true, &w, NULL);
            if (st != WAL_CORRUPT) hbad++;
            if (w != NULL) wal_close(w);
        }
        CHECK(hbad == 0, "%s: 파일 헤더 바이트 반전은 모두 CORRUPT", what[variant]);
        free(orig);
    }
    /* 거절 뒤 잠금이 남지 않고 다른 로그로 다음 거래가 실행된다 */
    wal_t *w = NULL;
    CHECK(wal_open(LOG2, true, &w, NULL) == WAL_CORRUPT, "거절 뒤 다시 열어도 LOCKED가 아니라 CORRUPT");
    build_log(LOG, 1, false, NULL);
    CHECK(wal_open(LOG, true, &w, NULL) == WAL_OK && wal_append(w, 9, 1, "n", 1, NULL) == WAL_OK &&
              wal_commit(w, 9, NULL) == WAL_OK,
          "정상 로그에서 다음 거래 commit");
    if (w != NULL) wal_close(w);
    cleanup();
}

/* ---- 6. 순서 위반 (checksum은 맞음) ---- */

static uint32_t t_crc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

static void le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    int kind;
    uint64_t xid, lsn_delta, prev_override;
    bool use_prev;
    uint64_t file_id_xor;
    uint32_t plen;
    uint64_t payload_value;
    uint32_t forced_plen; /* 0이 아니면 헤더에 이 길이를 쓴다 */
} raw_t;

/* 유효한 CRC로 레코드를 만든다. 반환: 레코드 길이 */
static size_t raw_record(uint8_t *out, uint64_t off, uint64_t prev, uint64_t file_id, const raw_t *r)
{
    uint32_t plen = r->plen;
    size_t span = ((size_t)WAL_RECORD_HEADER_BYTES + plen + 7u) & ~(size_t)7u;
    memset(out, 0, span);
    le(out, 0x5243574Cu, 4);
    le(out + 4, (uint64_t)r->kind, 2);
    le(out + 8, r->forced_plen != 0 ? r->forced_plen : plen, 4);
    le(out + 16, r->xid, 8);
    le(out + 24, off + r->lsn_delta, 8);
    le(out + 32, r->use_prev ? r->prev_override : prev, 8);
    le(out + 40, file_id ^ r->file_id_xor, 8);
    if (plen == 8) le(out + WAL_RECORD_HEADER_BYTES, r->payload_value, 8);
    else if (plen > 0) memset(out + WAL_RECORD_HEADER_BYTES, 0x5A, plen);
    le(out + 48, t_crc(out + WAL_RECORD_HEADER_BYTES, plen), 4);
    le(out + 52, t_crc(out, 52), 4);
    return span;
}

static wal_status_t open_raw(const raw_t *seq, int n, bool expect_unchanged)
{
    unlink(LOG2);
    wal_t *w;
    wal_create(LOG2, &w);
    wal_close(w);
    size_t len;
    uint8_t *hdr = read_file(LOG2, &len);
    uint64_t file_id = 0;
    for (int i = 7; i >= 0; i--) file_id = (file_id << 8) | hdr[16 + i];
    uint8_t *buf = malloc(len + (size_t)n * 128);
    memcpy(buf, hdr, len);
    free(hdr);
    uint64_t off = WAL_FIRST_LSN, prev = 0;
    for (int i = 0; i < n; i++) {
        size_t sp = raw_record(buf + off, off, prev, file_id, &seq[i]);
        prev = off;
        off += sp;
    }
    write_file(LOG2, buf, (size_t)off);
    free(buf);
    size_t l0, l1;
    uint64_t h0 = file_hash(LOG2, &l0);
    wal_t *r = NULL;
    wal_status_t st = wal_open(LOG2, true, &r, NULL);
    if (r != NULL) wal_close(r);
    if (expect_unchanged) {
        CHECK(file_hash(LOG2, &l1) == h0 && l1 == l0, "거절된 파일은 바뀌지 않는다");
    }
    return st;
}

static void test_order_violations(void)
{
    printf("\n[test_order_violations]\n");
    enum { D = WAL_REC_DATA, C = WAL_REC_COMMIT, A = WAL_REC_ABORT, K = WAL_REC_CHECKPOINT };
    raw_t ok[] = {{D, 1, 0, 0, false, 0, 4, 0, 0}, {C, 1, 0, 0, false, 0, 0, 0, 0}};
    CHECK(open_raw(ok, 2, false) == WAL_OK, "대조: 올바른 순서는 열린다(인코더 검증)");
    raw_t a[] = {{D, 1, 0, 0, false, 0, 4, 0, 0}, {C, 1, 0, 0, false, 0, 0, 0, 0}, {D, 1, 0, 0, false, 0, 4, 0, 0}};
    CHECK(open_raw(a, 3, true) == WAL_CORRUPT, "COMMIT 뒤 같은 거래의 DATA");
    raw_t b[] = {{C, 1, 0, 0, false, 0, 0, 0, 0}, {C, 1, 0, 0, false, 0, 0, 0, 0}};
    CHECK(open_raw(b, 2, true) == WAL_CORRUPT, "COMMIT 두 번");
    raw_t c[] = {{D, 1, 0, 0, false, 0, 4, 0, 0}, {A, 1, 0, 0, false, 0, 0, 0, 0}, {C, 1, 0, 0, false, 0, 0, 0, 0}};
    CHECK(open_raw(c, 3, true) == WAL_CORRUPT, "ABORT 뒤 COMMIT");
    raw_t d[] = {{D, 1, 0, 0, false, 0, 4, 0, 0}, {D, 1, 0, 999, true, 0, 4, 0, 0}};
    CHECK(open_raw(d, 2, true) == WAL_CORRUPT, "prev_lsn 연쇄 어긋남");
    raw_t e[] = {{D, 1, 8, 0, false, 0, 4, 0, 0}};
    CHECK(open_raw(e, 1, true) == WAL_CORRUPT, "lsn이 오프셋과 다름(복사된 레코드)");
    raw_t f[] = {{D, 1, 0, 0, false, 5, 4, 0, 0}};
    CHECK(open_raw(f, 1, true) == WAL_CORRUPT, "다른 파일의 file_id");
    raw_t g[] = {{D, 0, 0, 0, false, 0, 4, 0, 0}};
    CHECK(open_raw(g, 1, true) == WAL_CORRUPT, "xid 0의 DATA");
    raw_t h[] = {{K, 0, 0, 0, false, 0, 8, 4096, 0}};
    CHECK(open_raw(h, 1, true) == WAL_CORRUPT, "redo가 checkpoint 자신보다 뒤");
    raw_t i2[] = {{K, 0, 0, 0, false, 0, 8, 37, 0}};
    CHECK(open_raw(i2, 1, true) == WAL_CORRUPT, "정렬되지 않은 redo");
    raw_t i3[] = {{K, 5, 0, 0, false, 0, 8, 32, 0}};
    CHECK(open_raw(i3, 1, true) == WAL_CORRUPT, "xid가 있는 checkpoint");
    raw_t i4[] = {{K, 0, 0, 0, false, 0, 4, 0, 0}};
    CHECK(open_raw(i4, 1, true) == WAL_CORRUPT, "길이가 8이 아닌 checkpoint");
    raw_t j[] = {{9, 1, 0, 0, false, 0, 4, 0, 0}};
    CHECK(open_raw(j, 1, true) == WAL_CORRUPT, "알 수 없는 kind");
    raw_t k[] = {{D, 1, 0, 0, false, 0, 4, 0, WAL_MAX_PAYLOAD + 8}};
    CHECK(open_raw(k, 1, true) == WAL_CORRUPT, "선언 길이가 한도를 넘음");
    cleanup();
}

/* ---- 7. 파일 헤더, 초기화, 잠금 ---- */

static void test_header_and_lock(void)
{
    printf("\n[test_header_and_lock]\n");
    layout_t lay;
    build_log(LOG, 1, false, &lay);
    size_t len;
    uint8_t *orig = read_file(LOG, &len);
    uint8_t *m = malloc(len);
    wal_t *w = NULL;

    memcpy(m, orig, len);
    memcpy(m, "XXXXXXXX", 8);
    write_file(LOG2, m, len);
    CHECK(wal_open(LOG2, true, &w, NULL) == WAL_CORRUPT, "잘못된 magic");

    memcpy(m, orig, len);
    le(m + 8, 2, 4);
    le(m + 28, t_crc(m, 28), 4);
    write_file(LOG2, m, len);
    CHECK(wal_open(LOG2, true, &w, NULL) == WAL_UNSUPPORTED, "CRC가 맞는 다른 version은 UNSUPPORTED");

    memset(m, 7, 10);
    write_file(LOG2, m, 10);
    CHECK(wal_open(LOG2, true, &w, NULL) == WAL_CORRUPT, "0이 아닌 짧은 파일");

    /* wal_create가 헤더 fsync 전에 멈춰 magic 접두만 남은 파일은 다시 초기화한다 */
    for (size_t plen = 1; plen < 32; plen += 7) {
        write_file(LOG2, orig, plen);
        wal_open_info_t pinfo;
        CHECK(wal_open(LOG2, true, &w, &pinfo) == WAL_OK && pinfo.valid_end == WAL_FIRST_LSN,
              "헤더 접두(%zu바이트)는 생성 중단으로 보고 다시 초기화", plen);
        if (w != NULL) {
            wal_close(w);
            w = NULL;
        }
        write_file(LOG2, orig, plen);
        CHECK(wal_open(LOG2, false, &w, NULL) == WAL_CORRUPT, "헤더 접두는 읽기 열기에서 거절 (%zu)", plen);
    }

    uint8_t zeros[64] = {0};
    for (size_t zlen = 0; zlen <= 32; zlen += 16) {
        write_file(LOG2, zeros, zlen);
        wal_open_info_t info;
        wal_status_t st = wal_open(LOG2, true, &w, &info);
        CHECK(st == WAL_OK && info.valid_end == WAL_FIRST_LSN, "0바이트·0 헤더(%zu)는 다시 초기화", zlen);
        if (w != NULL) {
            CHECK(wal_append(w, 1, 1, "a", 1, NULL) == WAL_OK && wal_commit(w, 1, NULL) == WAL_OK,
                  "초기화한 로그에서 commit (%zu)", zlen);
            wal_close(w);
            w = NULL;
        }
        write_file(LOG2, zeros, zlen);
        CHECK(wal_open(LOG2, false, &w, NULL) == WAL_CORRUPT, "읽기 열기는 초기화하지 않고 거절 (%zu)", zlen);
    }
    memset(m, 0, len);
    write_file(LOG2, m, len);
    CHECK(wal_open(LOG2, true, &w, NULL) == WAL_CORRUPT, "32바이트보다 큰 0 파일");

    CHECK(wal_open("__test__wal_none.log", true, &w, NULL) == WAL_IO, "없는 파일은 IO");
    wal_t *a = NULL, *b = NULL;
    CHECK(wal_open(LOG, true, &a, NULL) == WAL_OK, "쓰기 열기");
    CHECK(wal_open(LOG, true, &b, NULL) == WAL_LOCKED, "두 번째 쓰기 열기는 LOCKED");
    CHECK(wal_open(LOG, false, &b, NULL) == WAL_LOCKED, "쓰는 중 읽기 열기는 LOCKED");
    wal_close(a);
    CHECK(wal_open(LOG, false, &a, NULL) == WAL_OK && wal_open(LOG, false, &b, NULL) == WAL_OK,
          "읽기끼리는 공유");
    wal_close(a);
    wal_close(b);
    free(m);
    free(orig);
    cleanup();
}

/* ---- 8. I/O 실패 주입 ---- */

static struct {
    long fail_at, count;
    int mode; /* 0 오류, 1 쓰기 짧게 하다 오류 */
    int err;
    int chop;   /* >0이면 모든 pread·pwrite를 이 크기로 쪼갠다 */
    bool eintr; /* 호출마다 한 번씩 EINTR */
    bool pending_eintr;
} fi;

static bool fi_hit(void)
{
    fi.count++;
    return fi.fail_at > 0 && fi.count == fi.fail_at;
}

static bool fi_eintr(void)
{
    if (!fi.eintr) return false;
    fi.pending_eintr = !fi.pending_eintr;
    if (fi.pending_eintr) {
        errno = EINTR;
        return true;
    }
    return false;
}

static ssize_t fi_pread(int fd, void *b, size_t n, off_t o)
{
    if (fi_eintr()) return -1;
    if (fi_hit()) {
        errno = fi.err;
        return -1;
    }
    if (fi.chop > 0 && n > (size_t)fi.chop) n = (size_t)fi.chop;
    return pread(fd, b, n, o);
}

static ssize_t fi_pwrite(int fd, const void *b, size_t n, off_t o)
{
    if (fi_eintr()) return -1;
    if (fi_hit()) {
        if (fi.mode == 1 && n > 4) {
            ssize_t part = pwrite(fd, b, n / 2, o);
            (void)part;
        }
        errno = fi.err;
        return -1;
    }
    if (fi.chop > 0 && n > (size_t)fi.chop) n = (size_t)fi.chop;
    return pwrite(fd, b, n, o);
}

static int fi_fsync(int fd)
{
    if (fi_eintr()) return -1;
    if (fi_hit()) {
        errno = fi.err;
        return -1;
    }
    return fsync(fd);
}

static int fi_ftruncate(int fd, off_t n)
{
    if (fi_hit()) {
        errno = fi.err;
        return -1;
    }
    return ftruncate(fd, n);
}

static void fi_install(long fail_at, int mode)
{
    static const wal_io_t io = {fi_pread, fi_pwrite, fi_fsync, fi_ftruncate};
    memset(&fi, 0, sizeof(fi));
    fi.fail_at = fail_at;
    fi.mode = mode;
    fi.err = EIO;
    wal_set_default_io(&io);
}

typedef struct {
    int acked[8];
    int attempted[8];
    wal_status_t first_fail;
    bool create_failed;
} wl_t;

/* create + 거래 1~3 commit + 거래 4 abort. 첫 실패에서 멈춘다. */
static void run_wl(wl_t *r)
{
    memset(r, 0, sizeof(*r));
    unlink(LOG);
    wal_t *w = NULL;
    wal_status_t st = wal_create(LOG, &w);
    if (st != WAL_OK) {
        r->create_failed = true;
        r->first_fail = st;
        return;
    }
    for (int x = 1; x <= 3; x++) {
        wal_append(w, (wal_xid_t)x, 1, "aa", 2, NULL);
        wal_append(w, (wal_xid_t)x, 2, "bb", 2, NULL);
        r->attempted[x] = 1;
        st = wal_commit(w, (wal_xid_t)x, NULL);
        if (st != WAL_OK) {
            r->first_fail = st;
            CHECK(st == WAL_COMMIT_UNKNOWN, "쓰기·fsync 실패는 COMMIT_UNKNOWN (%s)", wal_status_name(st));
            CHECK(wal_is_poisoned(w), "실패한 핸들은 사용 불가");
            CHECK(wal_append(w, 9, 1, "z", 1, NULL) == WAL_POISONED, "실패 뒤 append는 POISONED");
            CHECK(wal_commit(w, (wal_xid_t)x, NULL) == WAL_POISONED, "실패 뒤 같은 commit 재시도는 POISONED");
            CHECK(wal_flush(w, wal_appended_lsn(w)) == WAL_POISONED, "실패 뒤 flush 재시도는 POISONED");
            CHECK(wal_xid_state(w, (wal_xid_t)x) != WAL_XID_COMMITTED, "실패한 commit을 COMMITTED로 보이지 않는다");
            break;
        }
        r->acked[x] = 1;
    }
    wal_close(w);
}

static void test_io_faults(void)
{
    printf("\n[test_io_faults]\n");
    fi_install(0, 0);
    wl_t base;
    run_wl(&base);
    long total = fi.count;
    CHECK(total >= 9 && base.acked[3] == 1, "기준 실행의 I/O 호출 수 %ld", total);
    int modes_ok = 0, runs = 0;
    for (int mode = 0; mode < 2; mode++) {
        for (long k = 1; k <= total; k++) {
            fi_install(k, mode);
            wl_t r;
            run_wl(&r);
            wal_set_default_io(NULL);
            runs++;
            bool ok = true;
            wal_t *w = NULL;
            wal_open_info_t info;
            if (r.create_failed) {
                /* 만들다 실패한 파일은 남기지 않고 새로 만들 수 있다 */
                ok = access(LOG, F_OK) != 0;
                wal_t *n = NULL;
                ok = ok && wal_create(LOG, &n) == WAL_OK;
                if (n != NULL) wal_close(n);
            } else {
                wal_status_t st = wal_open(LOG, true, &w, &info);
                ok = st == WAL_OK;
                if (ok) {
                    for (int x = 1; x <= 3; x++) {
                        bool committed = wal_xid_known(w, (wal_xid_t)x) && wal_xid_state(w, (wal_xid_t)x) == WAL_XID_COMMITTED;
                        if (r.acked[x] && !committed) {
                            printf("  승인한 commit %d이 사라짐 (mode=%d k=%ld)\n", x, mode, k);
                            ok = false;
                        }
                        if (committed && !r.attempted[x]) {
                            printf("  시도하지 않은 commit %d이 보임\n", x);
                            ok = false;
                        }
                    }
                    wal_lsn_t c;
                    ok = ok && wal_append(w, 20, 1, "n", 1, NULL) == WAL_OK && wal_commit(w, 20, &c) == WAL_OK;
                    wal_close(w);
                    w = NULL;
                    ok = ok && wal_open(LOG, false, &w, NULL) == WAL_OK && wal_xid_state(w, 20) == WAL_XID_COMMITTED;
                    if (w != NULL) wal_close(w);
                } else {
                    printf("  재열기 실패 %s (mode=%d k=%ld)\n", wal_status_name(st), mode, k);
                }
            }
            modes_ok += ok;
        }
    }
    wal_set_default_io(NULL);
    CHECK(modes_ok == runs, "모든 실패 지점(%d회: 오류·짧은 쓰기 뒤 오류)에서 승인 commit 보존·다음 거래 실행 (%d)", runs, modes_ok);

    /* 짧은 읽기·쓰기와 EINTR */
    fi_install(0, 0);
    fi.chop = 7;
    fi.eintr = true;
    wl_t r;
    run_wl(&r);
    wal_t *w = NULL;
    bool ok = r.acked[3] == 1 && wal_open(LOG, true, &w, NULL) == WAL_OK;
    ok = ok && wal_xid_state(w, 3) == WAL_XID_COMMITTED;
    if (w != NULL) wal_close(w);
    wal_set_default_io(NULL);
    CHECK(ok, "7바이트씩 쪼갠 읽기·쓰기와 EINTR에서도 같은 결과");

    /* 열기 중 읽기 실패는 IO이며 파일을 바꾸지 않는다 */
    build_log(LOG, 6, true, NULL);
    size_t l0, l1;
    uint64_t h0 = file_hash(LOG, &l0);
    fi_install(0, 0);
    wal_open(LOG, true, &w, NULL);
    long opens = fi.count;
    if (w != NULL) wal_close(w);
    CHECK(opens > 0, "열기의 I/O 호출 수 %ld", opens);
    int open_ok = 0;
    for (long k = 1; k <= opens; k++) {
        fi_install(k, 0);
        wal_status_t st = wal_open(LOG, true, &w, NULL);
        wal_set_default_io(NULL);
        bool good = st == WAL_IO && w == NULL && file_hash(LOG, &l1) == h0 && l1 == l0;
        good = good && wal_open(LOG, true, &w, NULL) == WAL_OK;
        if (w != NULL) wal_close(w);
        w = NULL;
        open_ok += good;
    }
    CHECK(open_ok == opens, "열기 중 모든 I/O 실패(%ld곳)가 IO이고 파일 보존·재열기 성공 (%d)", opens, open_ok);
    cleanup();
}

/* ---- 8b. 메모리 예산 ---- */

static wal_redo_result_t pass_redo(void *ctx, const wal_record_t *r)
{
    (void)r;
    if (ctx != NULL) ++*(long *)ctx;
    return WAL_REDO_APPLIED;
}

static long g_fail_after;
static wal_redo_result_t failing_redo(void *ctx, const wal_record_t *r)
{
    (void)ctx;
    (void)r;
    return --g_fail_after < 0 ? WAL_REDO_ERROR : WAL_REDO_APPLIED;
}

static bool done_false(void *ctx, wal_xid_t xid, wal_lsn_t lsn)
{
    (void)ctx;
    (void)xid;
    (void)lsn;
    return false;
}

static void test_memory_budget(void)
{
    printf("\n[test_memory_budget]\n");
    build_log(LOG, 300, false, NULL); /* 거래 300개: 상태 표가 여러 번 커진다 */
    wal_t *w = NULL;
    wal_open_info_t info;
    CHECK(wal_open(LOG, false, &w, &info) == WAL_OK, "기본 예산으로 열기");
    uint64_t peak = info.memory_peak, used = wal_memory_used(w);
    CHECK(peak > used && used > 0, "재해시·읽기 창의 old+new가 정상 상태보다 크게 잡힌다 (peak=%llu used=%llu)",
          (unsigned long long)peak, (unsigned long long)used);
    wal_close(w);

    size_t l0, l1;
    uint64_t h0 = file_hash(LOG, &l0);
    wal_limits_t lim = {peak};
    CHECK(wal_open_with(LOG, false, &lim, &w, &info) == WAL_OK, "예산이 필요량(peak)과 같으면 열린다");
    if (w != NULL) {
        CHECK(wal_memory_peak(w) == peak, "peak는 결정적이다");
        wal_close(w);
    }
    lim.max_memory_bytes = peak - 1;
    CHECK(wal_open_with(LOG, true, &lim, &w, &info) == WAL_LIMIT && w == NULL && info.memory_peak < peak &&
              file_hash(LOG, &l1) == h0 && l1 == l0,
          "예산이 1바이트 모자라면 LIMIT이고 핸들 없음·파일 보존");
    int ok_count = 0, limit_count = 0;
    bool monotone = true, within = true;
    bool seen_ok = false;
    for (uint64_t b = 512; b <= 2 * peak; b += 512) {
        lim.max_memory_bytes = b;
        wal_status_t st = wal_open_with(LOG, false, &lim, &w, &info);
        if (info.memory_peak > b) within = false;
        if (st == WAL_OK) {
            ok_count++;
            seen_ok = true;
            wal_close(w);
        } else {
            limit_count++;
            if (st != WAL_LIMIT || seen_ok) monotone = false;
        }
    }
    CHECK(ok_count > 0 && limit_count > 0 && monotone && within,
          "예산을 키우며 훑으면 LIMIT 뒤 OK로만 바뀌고 어느 경우에도 peak가 예산을 넘지 않는다 (ok=%d limit=%d)",
          ok_count, limit_count);
    lim.max_memory_bytes = UINT64_MAX;
    CHECK(wal_open_with(LOG, false, &lim, &w, &info) == WAL_OK && wal_memory_peak(w) == peak,
          "예산이 UINT64_MAX여도 계산이 넘치지 않는다");
    if (w != NULL) wal_close(w);

    /* append 승인 상한: 버퍼가 예산을 넘기면 상태를 바꾸지 않고 LIMIT */
    unlink(LOG);
    lim.max_memory_bytes = 16 * 1024;
    CHECK(wal_create_with(LOG, &lim, &w) == WAL_OK, "예산 16KiB로 만들기");
    static uint8_t big[1000];
    memset(big, 'x', sizeof(big));
    int n = 0;
    wal_status_t st = WAL_OK;
    while (n < 1000 && (st = wal_append(w, 1, 1, big, sizeof(big), NULL)) == WAL_OK) n++;
    wal_lsn_t end_before = wal_end_lsn(w);
    CHECK(st == WAL_LIMIT && n > 4 && n < 20, "버퍼 증가가 예산에서 멈춘다 (n=%d, %s)", n, wal_status_name(st));
    CHECK(wal_append(w, 1, 1, big, sizeof(big), NULL) == WAL_LIMIT && wal_end_lsn(w) == end_before &&
              wal_memory_used(w) <= 16 * 1024,
          "LIMIT은 상태를 바꾸지 않고 사용량은 예산 이하");
    CHECK(wal_flush(w, wal_appended_lsn(w)) == WAL_OK, "쌓인 만큼은 flush된다");
    CHECK(wal_append(w, 1, 1, big, sizeof(big), NULL) == WAL_OK && wal_commit(w, 1, NULL) == WAL_OK,
          "flush 뒤에는 버퍼를 다시 쓸 수 있고 commit된다");
    wal_close(w);

    /* 거래 수 상한 */
    unlink(LOG);
    lim.max_memory_bytes = 12 * 1024;
    CHECK(wal_create_with(LOG, &lim, &w) == WAL_OK, "예산 12KiB로 만들기");
    n = 0;
    st = WAL_OK;
    while (n < 100000 && (st = wal_append(w, (wal_xid_t)(n + 1), 1, "a", 1, NULL)) == WAL_OK) n++;
    CHECK(st == WAL_LIMIT && n > 10 && wal_memory_used(w) <= 12 * 1024, "거래 수도 예산에서 멈춘다 (n=%d, %s)", n,
          wal_status_name(st));
    CHECK(wal_flush(w, wal_appended_lsn(w)) == WAL_OK && wal_commit(w, 1, NULL) == WAL_OK,
          "flush 뒤에는 이미 있는 거래가 commit된다");
    wal_close(w);

    /* replay 실패 뒤 정리 */
    unlink(LOG);
    wal_create(LOG, &w);
    for (int i = 0; i < 5000; i++) wal_append(w, 1, 1, "payload8", 8, NULL);
    wal_commit(w, 1, NULL);
    wal_close(w);
    wal_open(LOG, true, &w, &info);
    uint64_t open_peak = info.memory_peak;
    wal_close(w);
    lim.max_memory_bytes = open_peak;
    CHECK(wal_open_with(LOG, true, &lim, &w, &info) == WAL_OK, "열기 예산으로 열린다");
    uint64_t base = wal_memory_used(w);
    wal_replay_ops_t ops = {WAL_REPLAY_COMMITTED_ONLY, 0, pass_redo, NULL, NULL};
    wal_replay_stats_t rs;
    wal_status_t rst = wal_replay(w, &ops, NULL, &rs);
    CHECK(rst == WAL_LIMIT && wal_memory_used(w) == base,
          "replay 배열이 예산을 넘으면 LIMIT이고 사용량이 되돌아온다 (%s used=%llu base=%llu peak=%llu budget=%llu)",
          wal_status_name(rst), (unsigned long long)wal_memory_used(w), (unsigned long long)base,
          (unsigned long long)wal_memory_peak(w), (unsigned long long)open_peak);
    CHECK(wal_replay(w, &ops, NULL, &rs) == WAL_LIMIT && wal_memory_used(w) == base, "반복해도 같다");
    wal_close(w);

    wal_open(LOG, true, &w, &info);
    base = wal_memory_used(w);
    g_fail_after = 1234;
    ops.redo = failing_redo;
    CHECK(wal_replay(w, &ops, NULL, &rs) == WAL_REDO_FAILED && wal_memory_used(w) == base,
          "redo 실패 뒤에도 사용량이 되돌아온다");
    ops.redo = pass_redo;
    ops.txn_done = done_false;
    CHECK(wal_replay(w, &ops, NULL, &rs) == WAL_REDO_FAILED && wal_memory_used(w) == base,
          "txn_done 실패 뒤에도 사용량이 되돌아온다");
    ops.txn_done = NULL;
    long delivered = 0;
    CHECK(wal_replay(w, &ops, &delivered, &rs) == WAL_OK && delivered == 5000 && rs.txns == 1 &&
              wal_memory_used(w) == base,
          "실패 뒤 다시 replay하면 모든 DATA 5000개를 한 번씩 넘긴다 (%ld)", delivered);
    wal_close(w);
    cleanup();
}

/* ---- 9. checkpoint ---- */

static uint64_t g_first_redo_lsn;
static wal_redo_result_t first_redo(void *ctx, const wal_record_t *r)
{
    (void)ctx;
    if (g_first_redo_lsn == 0) g_first_redo_lsn = r->lsn;
    g_redo_count++;
    return WAL_REDO_APPLIED;
}

static void test_checkpoint(void)
{
    printf("\n[test_checkpoint]\n");
    unlink(LOG);
    wal_t *w;
    wal_create(LOG, &w);
    for (int x = 1; x <= 2; x++) {
        wal_append(w, (wal_xid_t)x, 1, "aa", 2, NULL);
        wal_commit(w, (wal_xid_t)x, NULL);
    }
    wal_lsn_t redo = wal_end_lsn(w), ck;
    CHECK(wal_checkpoint(w, redo + 8, NULL) == WAL_INVALID, "아직 쓰지 않은 위치는 거절");
    CHECK(wal_checkpoint(w, redo + 1, NULL) == WAL_INVALID, "정렬되지 않은 위치는 거절");
    CHECK(wal_checkpoint(w, redo, &ck) == WAL_OK, "checkpoint");
    CHECK(wal_checkpoint(w, WAL_FIRST_LSN, NULL) == WAL_INVALID, "redo는 뒤로 갈 수 없다");
    wal_append(w, 3, 1, "bb", 2, NULL);
    wal_append(w, 3, 2, "cc", 2, NULL);
    wal_commit(w, 3, NULL);
    wal_close(w);
    wal_open_info_t info;
    CHECK(wal_open(LOG, true, &w, &info) == WAL_OK && info.redo_lsn == redo && info.last_checkpoint == ck,
          "열기가 마지막 checkpoint의 redo 위치를 찾는다");
    wal_replay_ops_t ops = {WAL_REPLAY_ALL, 0, first_redo, NULL, NULL};
    g_redo_count = 0;
    g_first_redo_lsn = 0;
    wal_replay_stats_t st;
    CHECK(wal_replay(w, &ops, NULL, &st) == WAL_OK && g_redo_count == 2 && g_first_redo_lsn > ck,
          "redo 시작 위치 이전의 DATA는 재생하지 않는다 (%d)", g_redo_count);
    wal_close(w);
    cleanup();
}

/* ---- 10. 합성 DB: 두 replay 계약 ---- */

static void run_workload(model_mode_t mode, int txns)
{
    unlink(LOG);
    unlink(DATA);
    unlink(DATA ".new");
    model_t *m = calloc(1, sizeof(*m));
    model_open(m, DATA, mode);
    wal_create(LOG, &m->wal);
    for (int x = 1; x <= txns; x++) {
        if (model_run_txn(m, (uint64_t)x) < 0) {
            printf("거래 %d 실패\n", x);
            break;
        }
    }
    wal_close(m->wal);
    model_close(m);
    free(m);
}

static void recover_and_check(model_mode_t mode, bool drop_data, const char *label, uint64_t *fp_out,
                              wal_replay_stats_t *stats_out)
{
    if (drop_data) {
        unlink(DATA);
        unlink(DATA ".new");
    }
    model_t *m = calloc(1, sizeof(*m));
    wal_open_info_t info;
    int rc = model_recover(m, LOG, DATA, mode, &info, stats_out);
    CHECK(rc == 0, "%s: 복구 (rc=%d)", label, rc);
    if (rc == 0) {
        CHECK(model_check_invariants(m, label), "%s: 불변식", label);
        if (fp_out != NULL) *fp_out = model_fingerprint(m);
        wal_close(m->wal);
    }
    model_close(m);
    free(m);
}

static void test_replay_contracts(void)
{
    printf("\n[test_replay_contracts]\n");
    for (int md = 0; md < 2; md++) {
        model_mode_t mode = md == 0 ? MODEL_MVCC : MODEL_COW;
        const char *name = md == 0 ? "MVCC·ALL" : "COW·COMMITTED_ONLY";
        run_workload(mode, 40);
        uint64_t fp1 = 0, fp2 = 0, fp3 = 0;
        wal_replay_stats_t s1, s2, s3;
        recover_and_check(mode, true, name, &fp1, &s1);
        CHECK(s1.applied > 0, "%s: 데이터가 없으면 로그에서 만든다 (applied=%llu)", name, (unsigned long long)s1.applied);
        /* 같은 로그를 다시 복구해도 같다 */
        recover_and_check(mode, false, name, &fp2, &s2);
        recover_and_check(mode, false, name, &fp3, &s3);
        CHECK(fp1 == fp2 && fp2 == fp3, "%s: 반복 복구의 결과가 같다", name);
        if (mode == MODEL_COW) {
            CHECK(s2.txns == 0 && s3.txns == 0 && s2.applied == 0, "COW: 반영된 거래는 다시 넘기지 않는다 (%llu)",
                  (unsigned long long)s2.txns);
        } else {
            CHECK(s2.applied == 0 && s2.skipped > 0, "MVCC: page LSN이 중복 적용을 건너뛴다 (applied=%llu skipped=%llu)",
                  (unsigned long long)s2.applied, (unsigned long long)s2.skipped);
        }
        /* 데이터를 지우고 처음부터 다시 만들어도 같다 */
        uint64_t fp4 = 0;
        wal_replay_stats_t s4;
        recover_and_check(mode, true, name, &fp4, &s4);
        CHECK(fp4 == fp1, "%s: 로그만으로 다시 만든 상태가 같다", name);
    }

    /* 음성 대조: 보호 장치를 빼면 검사가 실패해야 한다 */
    printf("  -- 음성 대조 (아래 출력은 의도된 실패 확인)\n");
    for (int md = 0; md < 2; md++) {
        model_mode_t mode = md == 0 ? MODEL_MVCC : MODEL_COW;
        run_workload(mode, 12);
        recover_and_check(mode, true, "대조 준비", NULL, NULL);
        model_t *m = calloc(1, sizeof(*m));
        m->disable_page_lsn_guard = true;
        m->ignore_applied = true;
        wal_open_info_t info;
        wal_replay_stats_t st;
        /* model_recover는 model_open으로 모형을 다시 만들므로 플래그를 보존한다 */
        int rc = model_recover(m, LOG, DATA, mode, &info, &st);
        bool inv = rc == 0 && model_check_invariants(m, "대조");
        CHECK(!inv, "%s: %s 없이는 중복 적용이 불변식에 걸린다", md == 0 ? "MVCC" : "COW",
              md == 0 ? "page LSN 비교" : "applied_lsn");
        if (rc == 0) wal_close(m->wal);
        model_close(m);
        free(m);
    }
    cleanup();
}

/* WAL 먼저: 로그보다 앞서 나간 페이지를 불변식이 잡는다 */
static void test_wal_first(void)
{
    printf("\n[test_wal_first]\n");
    unlink(LOG);
    unlink(DATA);
    model_t *m = calloc(1, sizeof(*m));
    model_open(m, DATA, MODEL_MVCC);
    wal_create(LOG, &m->wal);
    model_txn_t t;
    txn_begin(&t, 1);
    model_insert(m, &t, txn_row_id(1, 0), txn_value(1, 0));
    model_insert(m, &t, txn_row_id(1, 0) + 160, txn_value(1, 0) + 0); /* 같은 페이지의 두 번째 행: page LSN이 32보다 커진다 */
    CHECK(model_publish_all(m) == -3 && m->wal_first_refused == 1, "flush 전 페이지 쓰기는 거절된다");
    m->wal_first = false;
    CHECK(model_publish_all(m) == 0, "대조: 규칙을 끄면 쓰기가 나간다");
    wal_close(m->wal); /* flush하지 않았으므로 로그에는 아무것도 없다 */
    model_close(m);
    free(m);
    printf("  -- 음성 대조 (아래 출력은 의도된 실패 확인)\n");
    m = calloc(1, sizeof(*m));
    wal_open_info_t info;
    int rc = model_recover(m, LOG, DATA, MODEL_MVCC, &info, NULL);
    CHECK(rc == 0 && !model_check_invariants(m, "WAL 먼저 위반"), "로그보다 앞선 데이터 페이지를 불변식이 잡는다 (rc=%d)", rc);
    if (rc == 0) wal_close(m->wal);
    model_close(m);
    free(m);
    cleanup();
}

/* 가시 페이지를 미확정 이미지가 덮는 소비자는 COMMITTED_ONLY로도 복구할 수 없다 */
static bool inplace_run(bool leak)
{
    unlink(LOG);
    unlink(DATA);
    unlink(DATA ".new");
    model_t *m = calloc(1, sizeof(*m));
    model_open(m, DATA, MODEL_COW);
    wal_create(LOG, &m->wal);
    model_txn_t t1, t2;
    txn_begin(&t1, 1);
    for (unsigned j = 0; j < txn_rows(1); j++) model_insert(m, &t1, txn_row_id(1, j), txn_value(1, j));
    model_commit(m, &t1);
    txn_begin(&t2, 2);
    for (unsigned j = 0; j < txn_rows(2); j++) {
        model_insert(m, &t2, txn_row_id(2, j), txn_value(2, j));
        if (leak) model_apply(m, model_page_of(txn_row_id(2, j)), 0, txn_row_id(2, j), 2, txn_value(2, j));
    }
    if (leak) (void)model_publish_image(m); /* 잘못된 소비자: commit 전에 가시 이미지에 게시 */
    wal_close(m->wal);                      /* 거래 2는 끝나지 않고 종료 */
    model_close(m);
    free(m);
    m = calloc(1, sizeof(*m));
    wal_open_info_t info;
    int rc = model_recover(m, LOG, DATA, MODEL_COW, &info, NULL);
    bool inv = rc == 0 && model_check_invariants(m, leak ? "제자리 미확정" : "COW 대조");
    if (rc == 0) wal_close(m->wal);
    model_close(m);
    free(m);
    return inv;
}

static void test_inplace_uncommitted_is_unsafe(void)
{
    printf("\n[test_inplace_uncommitted_is_unsafe]\n");
    CHECK(inplace_run(false), "대조: 거래 안에만 두는 COW 소비자는 미확정 거래 2가 복구 뒤에도 보이지 않는다");
    printf("  -- 음성 대조 (아래 출력은 의도된 실패 확인)\n");
    CHECK(!inplace_run(true), "미확정 이미지를 가시 이미지에 게시하면 committed-only replay가 지우지 못해 불변식에 걸린다");
    cleanup();
}

/* ---- wal_file_id: 읽기 전용 식별자 접근자 ---- */

static void test_file_id_accessor(void)
{
    printf("\n[wal_file_id]\n");
    cleanup();
    CHECK(wal_file_id(NULL) == 0, "NULL은 0");
    wal_t *a = NULL, *b = NULL;
    CHECK(wal_create(LOG, &a) == WAL_OK && wal_create(LOG2, &b) == WAL_OK, "로그 둘 생성");
    if (a == NULL || b == NULL) return;
    uint64_t ida = wal_file_id(a), idb = wal_file_id(b);
    CHECK(ida != 0 && idb != 0 && ida != idb, "0이 아니고 로그마다 다르다");
    wal_lsn_t l = 0;
    CHECK(wal_append(a, 1, 0, "x", 1, &l) == WAL_OK && wal_commit(a, 1, &l) == WAL_OK, "쓰기는 식별자를 바꾸지 않는다");
    CHECK(wal_file_id(a) == ida, "쓴 뒤에도 같다");
    wal_close(a);
    wal_close(b);
    CHECK(wal_open(LOG, false, &a, NULL) == WAL_OK && wal_file_id(a) == ida, "읽기 열기도 같은 식별자");
    if (a != NULL) wal_close(a);
    CHECK(wal_open(LOG, true, &a, NULL) == WAL_OK && wal_file_id(a) == ida, "다시 열어도 같은 식별자");
    if (a != NULL) wal_close(a);
    cleanup();
}

int main(void)
{
    cleanup();
    test_file_id_accessor();
    test_roundtrip();
    test_commit_order();
    test_call_rules();
    test_torn_tail();
    test_bitflip_rejected();
    test_order_violations();
    test_header_and_lock();
    test_io_faults();
    test_memory_budget();
    test_checkpoint();
    test_replay_contracts();
    test_wal_first();
    test_inplace_uncommitted_is_unsafe();
    cleanup();
    printf("\n%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
