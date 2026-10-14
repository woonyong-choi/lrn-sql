#ifndef WAL_MODEL_H
#define WAL_MODEL_H

/*
 * WAL 검사용 합성 DB. 실제 pager·page_file과 무관한 최소 모형이며 이 모형에 대한 결과를 실제 DB의
 * 행·카탈로그 재시작 보장으로 읽으면 안 된다. 페이지는 4096바이트 MODEL_PAGES장이고 page LSN(u64),
 * 행 수(u32), CRC(u32)와 행(row_id, xid, value 각 u64)을 담는다. 페이지 쓰기는 원자적이라고 가정한다.
 *
 * 두 소비자 계약을 모두 모형으로 만든다.
 *  MODEL_MVCC: 페이지를 제자리에 쓴다. 미확정 행은 페이지에 남을 수 있고 가시성은 xid 상태로 정한다.
 *              WAL_REPLAY_ALL + page LSN 비교로 중복을 막는다. steal(commit 전 게시)을 허용한다.
 *  MODEL_COW:  MVCC가 없는 DB. 거래의 변경은 거래 안에만 두었다가 COMMIT fsync 뒤에 새 이미지 파일을
 *              쓰고 rename으로 한 번에 게시하며 이미지 머리에 applied_lsn(마지막 반영 COMMIT LSN)을 둔다.
 *              WAL_REPLAY_COMMITTED_ONLY가 applied_lsn보다 큰 COMMIT만 거래 단위로 넘긴다.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/wal.h"

#define MODEL_PAGES 16u
#define MODEL_PAGE_BYTES 4096u
#define MODEL_ROW_BYTES 24u
#define MODEL_MAX_ROWS ((MODEL_PAGE_BYTES - 16u) / MODEL_ROW_BYTES)
#define MODEL_MAX_TXN 2048u
#define MODEL_PATH_MAX 256u

typedef enum { MODEL_MVCC = 0, MODEL_COW = 1 } model_mode_t;

/* 자식 프로세스와 부모가 공유하는 기록. 비어 있으면(NULL) 기록하지 않는다. */
typedef struct {
    uint64_t synced;                  /* WAL 파일에서 마지막으로 fsync가 성공한 시점의 크기 */
    uint64_t acked_n;
    uint64_t acked[MODEL_MAX_TXN];    /* wal_commit이 WAL_OK를 돌려준 xid */
    uint64_t attempted_n;
    uint64_t attempted[MODEL_MAX_TXN];/* wal_commit을 부른 xid */
} model_shared_t;

static model_shared_t *g_shared;
static long g_points_left = -1;
static long g_points_seen;

/* 종료 지점. g_points_left번째 지점에서 프로세스를 즉시 끝낸다(kill -9와 같은 효과). */
static inline void crash_point(void)
{
    g_points_seen++;
    if (g_points_left >= 0 && g_points_left-- == 0) _exit(137);
}

static ssize_t hk_pread(int fd, void *buf, size_t size, off_t off) { return pread(fd, buf, size, off); }

static ssize_t hk_pwrite(int fd, const void *buf, size_t size, off_t off)
{
    crash_point();
    ssize_t r = pwrite(fd, buf, size, off);
    crash_point();
    return r;
}

static int hk_fsync(int fd)
{
    crash_point();
    int r = fsync(fd);
    if (r == 0 && g_shared != NULL) {
        struct stat sb;
        if (fstat(fd, &sb) == 0) g_shared->synced = (uint64_t)sb.st_size;
    }
    crash_point();
    return r;
}

static int hk_ftruncate(int fd, off_t len)
{
    crash_point();
    int r = ftruncate(fd, len);
    crash_point();
    return r;
}

static inline void model_install_hooks(void)
{
    static const wal_io_t io = {hk_pread, hk_pwrite, hk_fsync, hk_ftruncate};
    wal_set_default_io(&io);
}

typedef struct {
    int fd;                       /* MVCC 데이터 파일 */
    wal_t *wal;
    model_mode_t mode;
    char path[MODEL_PATH_MAX];
    bool disable_page_lsn_guard;  /* true면 MVCC redo가 page LSN을 무시한다(음성 대조용) */
    bool ignore_applied;          /* true면 COW replay가 applied_lsn을 0으로 둔다(음성 대조용) */
    bool wal_first;               /* false면 flush 없이 MVCC 페이지를 쓴다(음성 대조용) */
    int wal_first_refused;
    uint64_t applied;             /* COW 이미지 머리의 applied_lsn */
    uint8_t cache[MODEL_PAGES][MODEL_PAGE_BYTES];
    bool dirty[MODEL_PAGES];
} model_t;

typedef struct {
    uint64_t xid;
    unsigned n;
    uint32_t page[4];
    uint64_t row[4], value[4];
} model_txn_t;

static inline uint32_t model_crc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static inline uint64_t m_get64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline void m_put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static inline uint32_t m_get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void m_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static inline uint32_t page_crc(const uint8_t *pg)
{
    uint8_t tmp[MODEL_PAGE_BYTES];
    memcpy(tmp, pg, sizeof(tmp));
    m_put32(tmp + 12, 0);
    return model_crc(tmp, sizeof(tmp));
}

static inline bool page_is_zero(const uint8_t *pg)
{
    for (size_t k = 0; k < MODEL_PAGE_BYTES; k++) {
        if (pg[k] != 0) return false;
    }
    return true;
}

static inline void image_header(uint8_t *h, uint64_t applied)
{
    memset(h, 0, MODEL_PAGE_BYTES);
    m_put64(h, applied);
    m_put32(h + 12, 0);
    m_put32(h + 12, model_crc(h, MODEL_PAGE_BYTES));
}

/* 모형 저장소를 읽는다. 없는 파일은 빈 상태다. 0 성공, -1 I/O, -2 손상 */
static inline int model_load(model_t *m)
{
    memset(m->cache, 0, sizeof(m->cache));
    m->applied = 0;
    m->fd = -1;
    if (m->mode == MODEL_MVCC) {
        m->fd = open(m->path, O_RDWR | O_CREAT, 0600);
        if (m->fd < 0) return -1;
        for (uint32_t i = 0; i < MODEL_PAGES; i++) {
            ssize_t n = pread(m->fd, m->cache[i], MODEL_PAGE_BYTES, (off_t)i * MODEL_PAGE_BYTES);
            if (n < 0) return -1;
            if (n == 0) continue;
            if (n != (ssize_t)MODEL_PAGE_BYTES) return -2;
            if (!page_is_zero(m->cache[i]) && page_crc(m->cache[i]) != m_get32(m->cache[i] + 12)) return -2;
        }
        return 0;
    }
    int fd = open(m->path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    int rc = 0;
    uint8_t h[MODEL_PAGE_BYTES];
    if (pread(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h) || page_crc(h) != m_get32(h + 12)) {
        rc = -2;
    } else {
        m->applied = m_get64(h);
        for (uint32_t i = 0; i < MODEL_PAGES && rc == 0; i++) {
            if (pread(fd, m->cache[i], MODEL_PAGE_BYTES, (off_t)(i + 1) * MODEL_PAGE_BYTES) !=
                (ssize_t)MODEL_PAGE_BYTES) {
                rc = -2;
            } else if (!page_is_zero(m->cache[i]) && page_crc(m->cache[i]) != m_get32(m->cache[i] + 12)) {
                rc = -2;
            }
        }
    }
    close(fd);
    return rc;
}

static inline int model_open(model_t *m, const char *path, model_mode_t mode)
{
    memset(m, 0, sizeof(*m));
    m->mode = mode;
    m->wal_first = true;
    snprintf(m->path, sizeof(m->path), "%s", path);
    return model_load(m);
}

static inline void model_close(model_t *m)
{
    if (m->fd >= 0) close(m->fd);
    m->fd = -1;
}

static inline uint32_t model_page_of(uint64_t row_id) { return (uint32_t)(row_id % MODEL_PAGES); }

static inline bool model_apply(model_t *m, uint32_t page, wal_lsn_t lsn, uint64_t row_id,
                               uint64_t xid, uint64_t value)
{
    uint8_t *pg = m->cache[page];
    uint32_t n = m_get32(pg + 8);
    if (n >= MODEL_MAX_ROWS) return false;
    uint8_t *row = pg + 16 + (size_t)n * MODEL_ROW_BYTES;
    m_put64(row, row_id);
    m_put64(row + 8, xid);
    m_put64(row + 16, value);
    m_put32(pg + 8, n + 1);
    if (m->mode == MODEL_MVCC) m_put64(pg, lsn);
    m->dirty[page] = true;
    return true;
}

/* MVCC: 페이지 하나를 제자리에 쓴다. WAL 먼저 규칙: 마지막 LSN이 fsync 전이면 거절한다. */
static inline int model_publish_page(model_t *m, uint32_t page)
{
    if (!m->dirty[page]) return 0;
    uint8_t *pg = m->cache[page];
    if (m->wal_first && wal_require_flushed(m->wal, m_get64(pg)) != WAL_OK) {
        m->wal_first_refused++;
        return -3;
    }
    m_put32(pg + 12, 0);
    m_put32(pg + 12, model_crc(pg, MODEL_PAGE_BYTES));
    crash_point();
    if (pwrite(m->fd, pg, MODEL_PAGE_BYTES, (off_t)page * MODEL_PAGE_BYTES) != (ssize_t)MODEL_PAGE_BYTES) {
        return -1;
    }
    crash_point();
    m->dirty[page] = false;
    return 0;
}

/* COW: 이미지 전체를 새 파일에 쓰고 fsync 뒤 rename한다. 거래의 변경과 applied_lsn이 함께 보인다. */
static inline bool model_publish_image(model_t *m)
{
    char tmp[MODEL_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", m->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = true;
    uint8_t h[MODEL_PAGE_BYTES];
    image_header(h, m->applied);
    crash_point();
    ok = pwrite(fd, h, sizeof(h), 0) == (ssize_t)sizeof(h);
    for (uint32_t i = 0; i < MODEL_PAGES && ok; i++) {
        uint8_t *pg = m->cache[i];
        m_put32(pg + 12, 0);
        if (!page_is_zero(pg)) m_put32(pg + 12, model_crc(pg, MODEL_PAGE_BYTES));
        ok = pwrite(fd, pg, MODEL_PAGE_BYTES, (off_t)(i + 1) * MODEL_PAGE_BYTES) == (ssize_t)MODEL_PAGE_BYTES;
    }
    crash_point();
    ok = ok && fsync(fd) == 0;
    close(fd);
    crash_point();
    ok = ok && rename(tmp, m->path) == 0;
    crash_point();
    return ok;
}

static inline int model_publish_all(model_t *m)
{
    if (m->mode == MODEL_COW) return model_publish_image(m) ? 0 : -1;
    int rc = 0;
    for (uint32_t i = 0; i < MODEL_PAGES; i++) {
        int r = model_publish_page(m, i);
        if (r != 0) rc = r;
    }
    return rc;
}

static inline void txn_begin(model_txn_t *t, uint64_t xid)
{
    memset(t, 0, sizeof(*t));
    t->xid = xid;
}

/* 한 행: DATA를 append하고 MVCC는 캐시에 바로 적용, COW는 거래 안에만 둔다. 실패하면 -1 */
static inline int model_insert(model_t *m, model_txn_t *t, uint64_t row_id, uint64_t value)
{
    if (t->n >= 4) return -1;
    uint8_t payload[16];
    m_put64(payload, row_id);
    m_put64(payload + 8, value);
    uint32_t page = model_page_of(row_id);
    wal_lsn_t lsn = 0;
    if (wal_append(m->wal, t->xid, page, payload, sizeof(payload), &lsn) != WAL_OK) return -1;
    t->page[t->n] = page;
    t->row[t->n] = row_id;
    t->value[t->n] = value;
    t->n++;
    if (m->mode == MODEL_MVCC) return model_apply(m, page, lsn, row_id, t->xid, value) ? 0 : -1;
    return 0;
}

/*
 * commit 한 번. wal_commit이 WAL_OK를 돌려준 뒤에만 게시하고 응답(acked)을 기록한다.
 * 반환은 wal_commit의 상태. 게시 실패는 acked 뒤라도 복구가 같은 결과를 만든다.
 */
static inline wal_status_t model_commit(model_t *m, model_txn_t *t)
{
    wal_lsn_t lsn = 0;
    if (g_shared != NULL) g_shared->attempted[g_shared->attempted_n++] = t->xid;
    crash_point();
    wal_status_t st = wal_commit(m->wal, t->xid, &lsn);
    crash_point();
    if (st != WAL_OK) return st;
    if (m->mode == MODEL_COW) {
        for (unsigned i = 0; i < t->n; i++) model_apply(m, t->page[i], lsn, t->row[i], t->xid, t->value[i]);
        m->applied = lsn;
        (void)model_publish_image(m);
    } else {
        (void)model_publish_all(m);
    }
    if (g_shared != NULL) g_shared->acked[g_shared->acked_n++] = t->xid;
    return WAL_OK;
}

/* ---- 복구 ---- */

static inline wal_redo_result_t model_redo(void *ctx, const wal_record_t *rec)
{
    model_t *m = ctx;
    if (rec->payload_len != 16 || rec->page_id >= MODEL_PAGES) return WAL_REDO_ERROR;
    if (m->mode == MODEL_MVCC && !m->disable_page_lsn_guard &&
        !wal_redo_needed(rec->lsn, m_get64(m->cache[rec->page_id]))) {
        return WAL_REDO_SKIPPED;
    }
    return model_apply(m, rec->page_id, rec->lsn, m_get64(rec->payload), rec->xid, m_get64(rec->payload + 8))
               ? WAL_REDO_APPLIED
               : WAL_REDO_ERROR;
}

static inline bool model_txn_done(void *ctx, wal_xid_t xid, wal_lsn_t commit_lsn)
{
    model_t *m = ctx;
    (void)xid;
    m->applied = commit_lsn;
    return model_publish_image(m);
}

/* 열기+복구. 0 성공이며 m->wal이 쓰기 핸들이다. 음수면 실패 */
static inline int model_recover(model_t *m, const char *wal_path, const char *data_path, model_mode_t mode,
                                wal_open_info_t *info, wal_replay_stats_t *stats)
{
    wal_status_t st = wal_open(wal_path, true, &m->wal, info);
    if (st != WAL_OK) return -(int)st - 100;
    wal_t *w = m->wal;
    bool noguard = m->disable_page_lsn_guard, ign = m->ignore_applied;
    int rc = model_open(m, data_path, mode);
    m->wal = w;
    m->disable_page_lsn_guard = noguard;
    m->ignore_applied = ign;
    if (rc != 0) return rc;
    wal_replay_ops_t ops = {mode == MODEL_COW ? WAL_REPLAY_COMMITTED_ONLY : WAL_REPLAY_ALL,
                            (mode == MODEL_COW && !ign) ? m->applied : 0, model_redo,
                            mode == MODEL_COW ? model_txn_done : NULL, NULL};
    st = wal_replay(m->wal, &ops, m, stats);
    if (st != WAL_OK) return -(int)st - 200;
    return model_publish_all(m);
}

/* ---- 결정적 거래 규칙 ---- */

static inline unsigned txn_rows(uint64_t xid) { return 1u + (unsigned)(xid % 3u); }
static inline uint64_t txn_row_id(uint64_t xid, unsigned j) { return xid * 10u + j; }
static inline uint64_t txn_value(uint64_t xid, unsigned j) { return xid * 1000u + j; }
typedef enum { FATE_COMMIT, FATE_ABORT, FATE_OPEN } fate_t;
static inline fate_t txn_fate(uint64_t xid)
{
    if (xid % 5u == 0) return FATE_ABORT;
    if (xid % 7u == 0) return FATE_OPEN;
    return FATE_COMMIT;
}

/*
 * 거래 하나를 규칙대로 실행한다. 반환: 0 정상, 음수면 이 핸들을 더 쓰면 안 된다(실패·UNKNOWN).
 * MVCC는 xid%4==0이면 commit 전에 WAL을 flush하고 페이지를 먼저 게시한다(steal).
 */
static inline int model_run_txn(model_t *m, uint64_t xid)
{
    model_txn_t t;
    txn_begin(&t, xid);
    for (unsigned j = 0; j < txn_rows(xid); j++) {
        if (model_insert(m, &t, txn_row_id(xid, j), txn_value(xid, j)) != 0) return -1;
    }
    switch (txn_fate(xid)) {
    case FATE_ABORT:
        return wal_abort(m->wal, xid, NULL) == WAL_OK ? 0 : -1;
    case FATE_OPEN:
        return 0;
    case FATE_COMMIT:
        if (m->mode == MODEL_MVCC && xid % 4u == 0) {
            if (wal_flush(m->wal, wal_appended_lsn(m->wal)) != WAL_OK) return -2;
            (void)model_publish_all(m);
        }
        return model_commit(m, &t) == WAL_OK ? 0 : -3;
    }
    return -1;
}

/* ---- 불변식 ---- */

/* 같은 row_id가 몇 번 있는지. value·xid가 규칙과 다르면 -1 */
static inline int model_find_row(const model_t *m, uint64_t row_id, uint64_t xid, uint64_t value)
{
    const uint8_t *pg = m->cache[model_page_of(row_id)];
    int found = 0;
    for (uint32_t i = 0; i < m_get32(pg + 8); i++) {
        const uint8_t *r = pg + 16 + (size_t)i * MODEL_ROW_BYTES;
        if (m_get64(r) != row_id) continue;
        if (m_get64(r + 8) != xid || m_get64(r + 16) != value) return -1;
        found++;
    }
    return found;
}

/*
 * 복구 뒤 상태 검사. 어긋나면 이유를 출력하고 false.
 *  - COMMITTED 거래의 모든 행이 정확히 한 번 있다(중복·누락 없음)
 *  - COW: COMMITTED가 아닌 거래의 행은 저장소에 없다(미확정 이미지가 게시되지 않음)
 *  - MVCC: 가시 행 수가 COMMITTED 거래의 행 수와 같다(가시성은 xid 상태)
 *  - 모든 행은 거래 규칙에 맞고 applied·page LSN이 로그 끝을 넘지 않는다
 */
static inline bool model_check_invariants(const model_t *m, const char *label)
{
    bool ok = true;
    wal_lsn_t end = wal_end_lsn(m->wal);
    uint64_t visible = 0, expected = 0, total = 0;
    if (m->mode == MODEL_COW && m->applied > end) {
        printf("  [%s] applied_lsn %llu이 로그 끝 %llu보다 앞선다\n", label, (unsigned long long)m->applied,
               (unsigned long long)end);
        ok = false;
    }
    for (uint32_t p = 0; p < MODEL_PAGES; p++) {
        const uint8_t *pg = m->cache[p];
        if (m->mode == MODEL_MVCC && m_get64(pg) > end) {
            printf("  [%s] page %u의 LSN이 로그 끝보다 앞선다(데이터가 로그보다 먼저 나갔다)\n", label, p);
            ok = false;
        }
        for (uint32_t i = 0; i < m_get32(pg + 8); i++) {
            const uint8_t *r = pg + 16 + (size_t)i * MODEL_ROW_BYTES;
            uint64_t xid = m_get64(r + 8), row_id = m_get64(r);
            total++;
            bool rule = xid > 0 && row_id / 10u == xid && m_get64(r + 16) == txn_value(xid, (unsigned)(row_id % 10u));
            if (!rule) {
                printf("  [%s] 규칙에 맞지 않는 행 row=%llu xid=%llu\n", label, (unsigned long long)row_id,
                       (unsigned long long)xid);
                ok = false;
                continue;
            }
            bool committed = wal_xid_known(m->wal, xid) && wal_xid_state(m->wal, xid) == WAL_XID_COMMITTED;
            if (committed) visible++;
            if (m->mode == MODEL_COW && !committed) {
                printf("  [%s] COMMITTED가 아닌 거래 %llu의 행이 게시됐다\n", label, (unsigned long long)xid);
                ok = false;
            }
        }
    }
    for (uint64_t xid = 1; xid <= wal_max_xid(m->wal); xid++) {
        if (!wal_xid_known(m->wal, xid) || wal_xid_state(m->wal, xid) != WAL_XID_COMMITTED) continue;
        for (unsigned j = 0; j < txn_rows(xid); j++) {
            int c = model_find_row(m, txn_row_id(xid, j), xid, txn_value(xid, j));
            if (c != 1) {
                printf("  [%s] 커밋된 거래 %llu의 행 %u가 %d번 있다\n", label, (unsigned long long)xid, j, c);
                ok = false;
            }
            expected++;
        }
    }
    if (visible != expected) {
        printf("  [%s] 가시 행 %llu과 커밋 거래 행 수 %llu이 다르다\n", label, (unsigned long long)visible,
               (unsigned long long)expected);
        ok = false;
    }
    if (m->mode == MODEL_COW && total != expected) {
        printf("  [%s] 저장소 행 %llu과 커밋 거래 행 수 %llu이 다르다\n", label, (unsigned long long)total,
               (unsigned long long)expected);
        ok = false;
    }
    return ok;
}

static inline uint64_t model_fingerprint(const model_t *m)
{
    uint64_t h = 1469598103934665603ull ^ m->applied;
    for (uint32_t p = 0; p < MODEL_PAGES; p++) {
        for (uint32_t i = 0; i < MODEL_PAGE_BYTES; i++) {
            h ^= m->cache[p][i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

#endif
