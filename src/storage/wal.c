#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/wal.h"

#define WAL_MAGIC "LRWALOG1"
#define WAL_VERSION 1u
#define REC_MAGIC 0x5243574Cu /* "LWCR" little-endian */
#define EINTR_RETRY_MAX 100
#define WINDOW_BYTES (64u * 1024u)
#define INITIAL_XIDS 64u

typedef struct {
    wal_xid_t xid;
    uint8_t state;
    bool used;
    bool pending;          /* replay 중 반영 대상으로 고른 거래 */
    wal_lsn_t first_lsn;
    wal_lsn_t commit_lsn;
    uint64_t *lsns;        /* replay 중 이 거래의 DATA LSN. 끝나면 해제 */
    size_t lsn_len, lsn_cap;
} xid_entry_t;

struct wal {
    int fd;
    bool writable;
    bool poisoned;
    wal_io_t io;
    uint64_t file_id;
    wal_lsn_t written;     /* 파일에 쓴 끝 오프셋 */
    wal_lsn_t end;         /* 버퍼를 포함한 끝 오프셋 */
    wal_lsn_t last_lsn;    /* 마지막으로 쌓은 레코드 */
    wal_lsn_t flushed_lsn; /* fsync된 마지막 레코드 */
    wal_lsn_t redo_lsn;
    uint8_t *buf;
    size_t buf_len, buf_cap;
    xid_entry_t *xids;
    size_t xid_cap, xid_count;
    wal_xid_t max_xid;
    uint64_t budget, mem_used, mem_peak; /* WAL이 쥔 힙 바이트(재할당 중 old+new 포함)와 상한 */
};

/* 예산 검사. n을 더해도 상한을 넘지 않을 때만 쓴다(덧셈 대신 뺄셈으로 비교해 넘침을 막는다). */
static bool mem_charge(wal_t *w, uint64_t n)
{
    if (w->mem_used > w->budget || n > w->budget - w->mem_used) return false;
    w->mem_used += n;
    if (w->mem_used > w->mem_peak) w->mem_peak = w->mem_used;
    return true;
}

static void mem_release(wal_t *w, uint64_t n)
{
    w->mem_used = n > w->mem_used ? 0 : w->mem_used - n;
}

static wal_status_t errno_status(void)
{
    return errno == ENOMEM ? WAL_NOMEM : errno == ENOBUFS ? WAL_LIMIT : WAL_IO;
}

static ssize_t libc_pread(int fd, void *buf, size_t size, off_t offset)
{
    return pread(fd, buf, size, offset);
}

static ssize_t libc_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    return pwrite(fd, buf, size, offset);
}

static const wal_io_t LIBC_IO = {libc_pread, libc_pwrite, fsync, ftruncate};
static wal_io_t g_default_io = {libc_pread, libc_pwrite, fsync, ftruncate};

void wal_set_default_io(const wal_io_t *io)
{
    g_default_io = io != NULL ? *io : LIBC_IO;
}

/* ---- 바이트·체크섬 ---- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t g_crc_table[256];
static bool g_crc_ready;

static void crc_init(void)
{
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

/* 비용: O(n) 시간, 바이트당 표 조회 1번 */
static uint32_t crc32c(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint64_t span_of(uint32_t payload_len)
{
    return ((uint64_t)WAL_RECORD_HEADER_BYTES + payload_len + 7u) & ~(uint64_t)7u;
}

/* ---- I/O ---- */

/* 1: 읽음, 0: 파일 끝이 먼저 옴, -1: 오류(errno) */
static int read_full(const wal_io_t *io, int fd, uint8_t *buf, size_t len, uint64_t off)
{
    size_t done = 0;
    int retries = 0;
    while (done < len) {
        ssize_t n = io->pread(fd, buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR && retries++ < EINTR_RETRY_MAX) continue;
            return -1;
        }
        if (n == 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

static int write_full(const wal_io_t *io, int fd, const uint8_t *buf, size_t len, uint64_t off)
{
    size_t done = 0;
    int retries = 0;
    while (done < len) {
        ssize_t n = io->pwrite(fd, buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR && retries++ < EINTR_RETRY_MAX) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int sync_fd(const wal_io_t *io, int fd)
{
    int retries = 0;
    while (io->fsync(fd) != 0) {
        if (errno == EINTR && retries++ < EINTR_RETRY_MAX) continue;
        return -1;
    }
    return 0;
}

static int sync_parent_dir(const wal_io_t *io, const char *path)
{
    char dir[PATH_MAX + 2];
    size_t n = strlen(path);
    if (n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
    if (slash == NULL) {
        strcpy(dir, ".");
    } else if (slash == dir) {
        dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return -1;
    int rc = sync_fd(io, fd);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc;
}

/* 파일 읽기 창. 요청한 구간이 창 안에 있으면 포인터를 돌려주고 아니면 다시 채운다. */
typedef struct {
    const wal_io_t *io;
    int fd;
    uint64_t file_size;
    uint8_t *buf;
    size_t cap;
    uint64_t base;
    size_t len;
    wal_t *acct; /* 창 버퍼를 예산에 올린다 */
} reader_t;

static void rd_free(reader_t *rd)
{
    mem_release(rd->acct, rd->cap);
    free(rd->buf);
    rd->buf = NULL;
    rd->cap = 0;
}

/* 비용: 창 밖이면 pread 1번(최대 max(n, 64KiB)바이트) */
static int rd_peek(reader_t *rd, uint64_t off, size_t n, const uint8_t **out)
{
    if (off + n > rd->file_size) return 0;
    if (!(rd->len > 0 && off >= rd->base && off + n <= rd->base + rd->len)) {
        size_t want = n > WINDOW_BYTES ? n : WINDOW_BYTES;
        if (off + want > rd->file_size) want = (size_t)(rd->file_size - off);
        if (want > rd->cap) {
            if (!mem_charge(rd->acct, want)) { /* old가 아직 있으므로 old+new를 함께 센다 */
                errno = ENOBUFS;
                return -1;
            }
            uint8_t *nb = realloc(rd->buf, want);
            if (nb == NULL) {
                mem_release(rd->acct, want);
                errno = ENOMEM;
                return -1;
            }
            mem_release(rd->acct, rd->cap);
            rd->buf = nb;
            rd->cap = want;
        }
        rd->len = 0;
        int rc = read_full(rd->io, rd->fd, rd->buf, want, off);
        if (rc < 0) return -1;
        if (rc == 0) {
            errno = EIO; /* 크기를 확인한 뒤 파일이 줄었다 */
            return -1;
        }
        rd->base = off;
        rd->len = want;
    }
    *out = rd->buf + (off - rd->base);
    return 1;
}

/* ---- 레코드 해석 ---- */

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) return false;
    }
    return true;
}

typedef struct {
    wal_kind_t kind;
    wal_xid_t xid;
    uint32_t page_id;
    wal_lsn_t lsn, prev_lsn;
    uint32_t payload_len;
    const uint8_t *payload;
    uint64_t span;
} rec_t;

enum {
    REC_IO = -1,
    REC_INCOMPLETE = 0, /* 잘린 꼬리: 불완전한 길이이거나 쓰이지 않은 영역 */
    REC_OK = 1,
    REC_BAD = 2         /* 완전한 길이인데 내용이 틀림. 승인된 레코드일 수 있다 */
};

static int rec_parse(reader_t *rd, uint64_t off, uint64_t file_id, rec_t *rec)
{
    const uint8_t *h;
    int rc = rd_peek(rd, off, WAL_RECORD_HEADER_BYTES, &h);
    if (rc < 0) return REC_IO;
    if (rc == 0) return REC_INCOMPLETE;
    /* 헤더가 다 들어왔는데 magic·CRC가 틀리면 어떤 경우에도 잘림이 아니다. 마지막 승인 레코드의
     * 변질(CRC 필드만 0이 된 경우 포함)과 구분할 수 없으므로 조건 없이 거절한다. */
    if (get32(h) != REC_MAGIC || crc32c(h, 52) != get32(h + 52)) return REC_BAD;
    uint16_t kind = get16(h + 4);
    uint32_t plen = get32(h + 8);
    if (kind < WAL_REC_DATA || kind > WAL_REC_CHECKPOINT || get16(h + 6) != 0) return REC_BAD;
    if (plen > WAL_MAX_PAYLOAD || get64(h + 24) != off || get64(h + 40) != file_id) return REC_BAD;
    rec->kind = (wal_kind_t)kind;
    rec->payload_len = plen;
    rec->page_id = get32(h + 12);
    rec->xid = get64(h + 16);
    rec->lsn = get64(h + 24);
    rec->prev_lsn = get64(h + 32);
    uint32_t payload_crc = get32(h + 48);
    rec->span = span_of(plen);
    if (off + rec->span > rd->file_size) return REC_INCOMPLETE;
    rc = rd_peek(rd, off, (size_t)rec->span, &h);
    if (rc < 0) return REC_IO;
    if (rc == 0) return REC_INCOMPLETE;
    rec->payload = h + WAL_RECORD_HEADER_BYTES;
    if (crc32c(rec->payload, plen) != payload_crc) return REC_BAD;
    if (!all_zero(h + WAL_RECORD_HEADER_BYTES + plen, (size_t)rec->span - WAL_RECORD_HEADER_BYTES - plen)) {
        return REC_BAD;
    }
    return REC_OK;
}

/* ---- 거래 상태 표 ---- */

static size_t xid_slot(const wal_t *w, wal_xid_t xid)
{
    size_t mask = w->xid_cap - 1;
    size_t i = (size_t)((xid * 0x9E3779B97F4A7C15ull) >> 32) & mask;
    while (w->xids[i].used && w->xids[i].xid != xid) i = (i + 1) & mask;
    return i;
}

static xid_entry_t *xid_find(const wal_t *w, wal_xid_t xid)
{
    if (w->xid_cap == 0) return NULL;
    xid_entry_t *e = &w->xids[xid_slot(w, xid)];
    return e->used ? e : NULL;
}

/* 한 개를 더 넣을 자리를 만든다. 재해시 동안 old와 new를 모두 예산에 센다. 실패하면 표는 그대로다. */
static wal_status_t xid_reserve(wal_t *w)
{
    if (w->xid_cap != 0 && (w->xid_count + 1) * 10 <= w->xid_cap * 7) return WAL_OK;
    if (w->xid_cap > SIZE_MAX / 2 / sizeof(xid_entry_t)) return WAL_LIMIT;
    size_t ncap = w->xid_cap == 0 ? INITIAL_XIDS : w->xid_cap * 2;
    uint64_t nbytes = (uint64_t)ncap * sizeof(xid_entry_t);
    if (!mem_charge(w, nbytes)) return WAL_LIMIT;
    xid_entry_t *nt = calloc(ncap, sizeof(*nt));
    if (nt == NULL) {
        mem_release(w, nbytes);
        return WAL_NOMEM;
    }
    xid_entry_t *old = w->xids;
    size_t ocap = w->xid_cap;
    w->xids = nt;
    w->xid_cap = ncap;
    for (size_t i = 0; i < ocap; i++) {
        if (old[i].used) w->xids[xid_slot(w, old[i].xid)] = old[i];
    }
    free(old);
    mem_release(w, (uint64_t)ocap * sizeof(xid_entry_t));
    return WAL_OK;
}

/* 이미 있는 xid는 자리가 필요 없다. 없을 때만 한 개 자리를 만든다. */
static wal_status_t xid_reserve_for(wal_t *w, wal_xid_t xid)
{
    return xid_find(w, xid) != NULL ? WAL_OK : xid_reserve(w);
}

/* xid_reserve가 성공한 뒤에만 부른다. */
static void xid_set(wal_t *w, wal_xid_t xid, wal_xid_state_t state)
{
    xid_entry_t *e = &w->xids[xid_slot(w, xid)];
    if (!e->used) {
        e->used = true;
        e->xid = xid;
        w->xid_count++;
    }
    e->state = (uint8_t)state;
    if (xid > w->max_xid) w->max_xid = xid;
}

static void xid_note(wal_t *w, wal_xid_t xid, wal_lsn_t lsn, wal_kind_t kind)
{
    xid_entry_t *e = xid_find(w, xid);
    if (e == NULL) return;
    if (e->first_lsn == 0) e->first_lsn = lsn;
    if (kind == WAL_REC_COMMIT) e->commit_lsn = lsn;
}

static bool xid_open_for_write(const wal_t *w, wal_xid_t xid)
{
    const xid_entry_t *e = xid_find(w, xid);
    return e == NULL || e->state == WAL_XID_IN_PROGRESS;
}

/* ---- 열기 ---- */

static void wal_free(wal_t *w)
{
    if (w == NULL) return;
    if (w->fd >= 0) close(w->fd);
    free(w->buf);
    for (size_t i = 0; i < w->xid_cap; i++) free(w->xids[i].lsns);
    free(w->xids);
    free(w);
}

static uint64_t budget_of(const wal_limits_t *limits)
{
    return limits != NULL && limits->max_memory_bytes != 0 ? limits->max_memory_bytes
                                                           : WAL_DEFAULT_MEMORY_BYTES;
}

/* 핸들 자체도 예산에 올린다. 예산이 핸들보다 작으면 할당하지 않고 WAL_LIMIT이다. */
static wal_t *wal_alloc(int fd, bool writable, const wal_limits_t *limits, wal_status_t *st)
{
    crc_init();
    uint64_t budget = budget_of(limits);
    if (budget < sizeof(wal_t)) {
        *st = WAL_LIMIT;
        return NULL;
    }
    wal_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        *st = WAL_NOMEM;
        return NULL;
    }
    w->budget = budget;
    w->mem_used = w->mem_peak = sizeof(*w);
    w->fd = fd;
    w->writable = writable;
    w->io = g_default_io;
    w->redo_lsn = WAL_FIRST_LSN;
    return w;
}

static int random_file_id(uint64_t *id)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    uint8_t b[8];
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n != (ssize_t)sizeof(b)) {
        errno = EIO;
        return -1;
    }
    *id = get64(b);
    if (*id == 0) *id = 1;
    return 0;
}

static void header_encode(uint8_t h[WAL_HEADER_BYTES], uint64_t file_id)
{
    memset(h, 0, WAL_HEADER_BYTES);
    memcpy(h, WAL_MAGIC, 8);
    put32(h + 8, WAL_VERSION);
    put64(h + 16, file_id);
    put32(h + 28, crc32c(h, 28));
}

static wal_status_t write_header(wal_t *w, const char *path)
{
    uint8_t h[WAL_HEADER_BYTES];
    if (random_file_id(&w->file_id) != 0) return WAL_IO;
    header_encode(h, w->file_id);
    if (write_full(&w->io, w->fd, h, sizeof(h), 0) != 0) return WAL_IO;
    if (sync_fd(&w->io, w->fd) != 0) return WAL_IO;
    if (sync_parent_dir(&w->io, path) != 0) return WAL_IO;
    w->written = w->end = WAL_FIRST_LSN;
    return WAL_OK;
}

wal_status_t wal_create(const char *path, wal_t **out)
{
    return wal_create_with(path, NULL, out);
}

wal_status_t wal_create_with(const char *path, const wal_limits_t *limits, wal_t **out)
{
    if (out == NULL) return WAL_INVALID;
    *out = NULL;
    if (path == NULL) return WAL_INVALID;
    if (budget_of(limits) < sizeof(wal_t)) return WAL_LIMIT; /* 파일을 만들기 전에 거절한다 */
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return errno == EEXIST ? WAL_EXISTS : WAL_IO;
    wal_status_t ast = WAL_OK;
    wal_t *w = wal_alloc(fd, true, limits, &ast);
    if (w == NULL) {
        close(fd);
        unlink(path);
        return ast;
    }
    wal_status_t st = WAL_OK;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        st = WAL_LOCKED;
    } else {
        st = write_header(w, path);
    }
    if (st != WAL_OK) {
        int saved = errno;
        wal_free(w);
        unlink(path);
        errno = saved;
        return st;
    }
    *out = w;
    return WAL_OK;
}

/* 파일 헤더를 확인한다. needs_init은 "헤더가 durable해지기 전에 멈춘 빈 파일"이다. */
static wal_status_t header_check(wal_t *w, uint64_t size, bool *needs_init)
{
    uint8_t h[WAL_HEADER_BYTES];
    *needs_init = false;
    size_t have = size < WAL_HEADER_BYTES ? (size_t)size : WAL_HEADER_BYTES;
    memset(h, 0, sizeof(h));
    if (have > 0) {
        int rc = read_full(&w->io, w->fd, h, have, 0);
        if (rc < 0) return WAL_IO;
        if (rc == 0) {
            errno = EIO;
            return WAL_IO;
        }
    }
    if (size <= WAL_HEADER_BYTES && all_zero(h, sizeof(h))) {
        *needs_init = true;
        return WAL_OK;
    }
    if (size < WAL_HEADER_BYTES) {
        /* wal_create는 헤더 fsync 뒤에야 반환하므로, magic으로 시작하는 짧은 파일은 승인 commit이 없는 생성 중단이다 */
        size_t mlen = size < 8 ? (size_t)size : 8;
        if (size > 0 && memcmp(h, WAL_MAGIC, mlen) == 0) {
            *needs_init = true;
            return WAL_OK;
        }
        return WAL_CORRUPT;
    }
    if (memcmp(h, WAL_MAGIC, 8) != 0 || crc32c(h, 28) != get32(h + 28)) return WAL_CORRUPT;
    if (get32(h + 8) != WAL_VERSION) return WAL_UNSUPPORTED;
    w->file_id = get64(h + 16);
    return WAL_OK;
}

/* 로그 전체를 읽으며 순서와 체크섬을 검증하고 거래 상태를 만든다. */
static wal_status_t scan(wal_t *w, uint64_t size, wal_open_info_t *info)
{
    reader_t rd = {&w->io, w->fd, size, NULL, 0, 0, 0, w};
    uint64_t off = WAL_FIRST_LSN;
    wal_lsn_t prev = 0;
    wal_status_t st = WAL_OK;
    while (off < size) {
        rec_t r;
        int rc = rec_parse(&rd, off, w->file_id, &r);
        if (rc == REC_IO) {
            st = errno_status();
            goto done;
        }
        if (rc == REC_INCOMPLETE) {
            info->torn_bytes = size - off;
            goto done;
        }
        if (rc == REC_BAD) {
            info->corrupt_lsn = off;
            st = WAL_CORRUPT;
            goto done;
        }
        if (r.prev_lsn != prev) {
            info->corrupt_lsn = off;
            st = WAL_CORRUPT;
            goto done;
        }
        if (r.kind == WAL_REC_CHECKPOINT) {
            wal_lsn_t redo = r.payload_len == 8 ? get64(r.payload) : 0;
            if (r.xid != WAL_INVALID_XID || r.payload_len != 8 || redo < WAL_FIRST_LSN ||
                redo > r.lsn || (redo & 7u) != 0 || redo < w->redo_lsn) {
                info->corrupt_lsn = off;
                st = WAL_CORRUPT;
                goto done;
            }
            w->redo_lsn = redo;
            info->last_checkpoint = r.lsn;
        } else {
            if (r.xid == WAL_INVALID_XID || !xid_open_for_write(w, r.xid)) {
                info->corrupt_lsn = off;
                st = WAL_CORRUPT;
                goto done;
            }
            st = xid_reserve_for(w, r.xid);
            if (st != WAL_OK) goto done;
            wal_xid_state_t s = r.kind == WAL_REC_COMMIT ? WAL_XID_COMMITTED
                                : r.kind == WAL_REC_ABORT ? WAL_XID_ABORTED
                                                          : WAL_XID_IN_PROGRESS;
            xid_set(w, r.xid, s);
            xid_note(w, r.xid, r.lsn, r.kind);
        }
        info->records++;
        prev = r.lsn;
        off += r.span;
    }
done:
    rd_free(&rd);
    info->valid_end = off;
    w->last_lsn = prev;
    w->flushed_lsn = prev;
    return st;
}

wal_status_t wal_open(const char *path, bool writable, wal_t **out, wal_open_info_t *info)
{
    return wal_open_with(path, writable, NULL, out, info);
}

wal_status_t wal_open_with(const char *path, bool writable, const wal_limits_t *limits, wal_t **out,
                           wal_open_info_t *info)
{
    wal_open_info_t local;
    if (info == NULL) info = &local;
    memset(info, 0, sizeof(*info));
    info->redo_lsn = WAL_FIRST_LSN;
    if (out == NULL) return WAL_INVALID;
    *out = NULL;
    if (path == NULL) return WAL_INVALID;
    if (budget_of(limits) < sizeof(wal_t)) return WAL_LIMIT; /* 파일을 열기 전에 거절한다 */
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return WAL_IO;
    wal_status_t ast = WAL_OK;
    wal_t *w = wal_alloc(fd, writable, limits, &ast);
    if (w == NULL) {
        close(fd);
        return ast;
    }
    wal_status_t st = WAL_OK;
    struct stat sb;
    bool needs_init = false;
    if (flock(fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        st = errno == EWOULDBLOCK ? WAL_LOCKED : WAL_IO;
    } else if (fstat(fd, &sb) != 0) {
        st = WAL_IO;
    } else if ((st = header_check(w, (uint64_t)sb.st_size, &needs_init)) != WAL_OK) {
        /* st 그대로 */
    } else if (needs_init) {
        if (!writable) {
            st = WAL_CORRUPT;
        } else if ((st = write_header(w, path)) == WAL_OK) {
            if (sb.st_size > WAL_HEADER_BYTES) st = WAL_CORRUPT;
            info->valid_end = WAL_FIRST_LSN;
        }
    } else {
        st = scan(w, (uint64_t)sb.st_size, info);
        if (st == WAL_OK) {
            w->written = w->end = info->valid_end;
            if (info->torn_bytes > 0 && writable) {
                if (w->io.ftruncate(fd, (off_t)info->valid_end) != 0 || sync_fd(&w->io, fd) != 0) {
                    st = WAL_IO;
                } else {
                    info->truncated = true;
                }
            }
        }
    }
    if (st == WAL_OK) {
        /* 열기 때 끝 레코드가 없는 거래는 CRASHED로 고정한다. */
        for (size_t i = 0; i < w->xid_cap; i++) {
            xid_entry_t *e = &w->xids[i];
            if (!e->used) continue;
            if (e->state == WAL_XID_IN_PROGRESS) e->state = WAL_XID_CRASHED;
            info->xids++;
            if (e->state == WAL_XID_COMMITTED) info->committed++;
            else if (e->state == WAL_XID_ABORTED) info->aborted++;
            else info->crashed++;
        }
        info->redo_lsn = w->redo_lsn;
        info->memory_peak = w->mem_peak;
        *out = w;
        return WAL_OK;
    }
    int saved = errno;
    info->memory_peak = w->mem_peak;
    wal_free(w);
    errno = saved;
    return st;
}

wal_status_t wal_close(wal_t *wal)
{
    if (wal == NULL) return WAL_INVALID;
    int rc = close(wal->fd);
    wal->fd = -1;
    wal_free(wal);
    return rc == 0 ? WAL_OK : WAL_IO;
}

/* ---- 쓰기 ---- */

/* 쓰기 버퍼를 키운다. 새 버퍼를 예산에 올린 뒤에야 old를 놓는다(old+new). */
static wal_status_t buf_reserve(wal_t *w, size_t extra)
{
    if (extra > SIZE_MAX - w->buf_len) return WAL_LIMIT;
    size_t need = w->buf_len + extra;
    if (need <= w->buf_cap) return WAL_OK;
    size_t ncap = w->buf_cap == 0 ? 4096 : w->buf_cap;
    while (ncap < need && ncap <= SIZE_MAX / 2) ncap *= 2;
    if (ncap < need) ncap = need;
    if (!mem_charge(w, ncap)) {
        if (ncap == need || !mem_charge(w, need)) return WAL_LIMIT;
        ncap = need;
    }
    uint8_t *nb = realloc(w->buf, ncap);
    if (nb == NULL) {
        mem_release(w, ncap);
        return WAL_NOMEM;
    }
    mem_release(w, w->buf_cap);
    w->buf = nb;
    w->buf_cap = ncap;
    return WAL_OK;
}

static wal_status_t append_rec(wal_t *w, wal_kind_t kind, wal_xid_t xid, uint32_t page_id,
                               const void *payload, uint32_t plen, wal_lsn_t *lsn_out)
{
    if (w == NULL) return WAL_INVALID;
    if (w->poisoned) return WAL_POISONED;
    if (!w->writable || plen > WAL_MAX_PAYLOAD || (plen > 0 && payload == NULL)) return WAL_INVALID;
    if (kind == WAL_REC_CHECKPOINT) {
        if (xid != WAL_INVALID_XID || plen != 8) return WAL_INVALID;
    } else if (xid == WAL_INVALID_XID || !xid_open_for_write(w, xid)) {
        return WAL_INVALID;
    }
    uint64_t span = span_of(plen);
    wal_status_t st = buf_reserve(w, (size_t)span);
    if (st != WAL_OK) return st;
    if (kind != WAL_REC_CHECKPOINT) {
        st = xid_reserve_for(w, xid);
        if (st != WAL_OK) return st;
    }

    uint8_t *r = w->buf + w->buf_len;
    memset(r, 0, (size_t)span);
    put32(r, REC_MAGIC);
    put16(r + 4, (uint16_t)kind);
    put32(r + 8, plen);
    put32(r + 12, page_id);
    put64(r + 16, xid);
    put64(r + 24, w->end);
    put64(r + 32, w->last_lsn);
    put64(r + 40, w->file_id);
    if (plen > 0) memcpy(r + WAL_RECORD_HEADER_BYTES, payload, plen);
    put32(r + 48, crc32c(r + WAL_RECORD_HEADER_BYTES, plen));
    put32(r + 52, crc32c(r, 52));

    wal_lsn_t lsn = w->end;
    w->buf_len += (size_t)span;
    w->end += span;
    w->last_lsn = lsn;
    if (kind == WAL_REC_DATA) {
        xid_set(w, xid, WAL_XID_IN_PROGRESS);
    } else if (kind == WAL_REC_ABORT) {
        xid_set(w, xid, WAL_XID_ABORTED);
    } else if (kind == WAL_REC_COMMIT) {
        xid_set(w, xid, WAL_XID_IN_PROGRESS);
    }
    if (kind != WAL_REC_CHECKPOINT) xid_note(w, xid, lsn, kind);
    if (lsn_out != NULL) *lsn_out = lsn;
    return WAL_OK;
}

wal_status_t wal_append(wal_t *wal, wal_xid_t xid, uint32_t page_id, const void *payload,
                        uint32_t payload_len, wal_lsn_t *lsn)
{
    return append_rec(wal, WAL_REC_DATA, xid, page_id, payload, payload_len, lsn);
}

/* 비용: pwrite 1번(버퍼 전체) + fsync 1번 */
wal_status_t wal_flush(wal_t *wal, wal_lsn_t lsn)
{
    if (wal == NULL) return WAL_INVALID;
    if (lsn != 0 && lsn <= wal->flushed_lsn) return WAL_OK;
    if (wal->poisoned) return WAL_POISONED;
    if (lsn == 0 || lsn > wal->last_lsn) return WAL_INVALID;
    if (write_full(&wal->io, wal->fd, wal->buf, wal->buf_len, wal->written) != 0) {
        wal->poisoned = true;
        return WAL_IO;
    }
    wal->written = wal->end;
    wal->buf_len = 0;
    if (sync_fd(&wal->io, wal->fd) != 0) {
        wal->poisoned = true;
        return WAL_IO;
    }
    wal->flushed_lsn = wal->last_lsn;
    return WAL_OK;
}

wal_status_t wal_require_flushed(const wal_t *wal, wal_lsn_t page_lsn)
{
    if (wal == NULL) return WAL_INVALID;
    if (page_lsn == 0 || page_lsn <= wal->flushed_lsn) return WAL_OK;
    return WAL_NOT_FLUSHED;
}

wal_status_t wal_commit(wal_t *wal, wal_xid_t xid, wal_lsn_t *commit_lsn)
{
    wal_lsn_t lsn = 0;
    wal_status_t st = append_rec(wal, WAL_REC_COMMIT, xid, 0, NULL, 0, &lsn);
    if (st != WAL_OK) return st;
    st = wal_flush(wal, lsn);
    if (st != WAL_OK) return WAL_COMMIT_UNKNOWN;
    xid_set(wal, xid, WAL_XID_COMMITTED);
    if (commit_lsn != NULL) *commit_lsn = lsn;
    return WAL_OK;
}

wal_status_t wal_abort(wal_t *wal, wal_xid_t xid, wal_lsn_t *abort_lsn)
{
    return append_rec(wal, WAL_REC_ABORT, xid, 0, NULL, 0, abort_lsn);
}

wal_status_t wal_checkpoint(wal_t *wal, wal_lsn_t redo_lsn, wal_lsn_t *checkpoint_lsn)
{
    if (wal == NULL) return WAL_INVALID;
    if (redo_lsn < wal->redo_lsn || redo_lsn > wal->end || (redo_lsn & 7u) != 0) return WAL_INVALID;
    uint8_t p[8];
    put64(p, redo_lsn);
    wal_lsn_t lsn = 0;
    wal_status_t st = append_rec(wal, WAL_REC_CHECKPOINT, WAL_INVALID_XID, 0, p, 8, &lsn);
    if (st != WAL_OK) return st;
    st = wal_flush(wal, lsn);
    if (st != WAL_OK) return st;
    wal->redo_lsn = redo_lsn;
    if (checkpoint_lsn != NULL) *checkpoint_lsn = lsn;
    return WAL_OK;
}

wal_lsn_t wal_appended_lsn(const wal_t *wal) { return wal->last_lsn; }
wal_lsn_t wal_flushed_lsn(const wal_t *wal) { return wal->flushed_lsn; }
wal_lsn_t wal_end_lsn(const wal_t *wal) { return wal->end; }
wal_xid_t wal_max_xid(const wal_t *wal) { return wal->max_xid; }
bool wal_is_poisoned(const wal_t *wal) { return wal->poisoned; }
uint64_t wal_file_id(const wal_t *wal) { return wal == NULL ? 0 : wal->file_id; }
uint64_t wal_memory_used(const wal_t *wal) { return wal->mem_used; }
uint64_t wal_memory_peak(const wal_t *wal) { return wal->mem_peak; }

bool wal_xid_known(const wal_t *wal, wal_xid_t xid)
{
    return xid_find(wal, xid) != NULL;
}

wal_xid_state_t wal_xid_state(const wal_t *wal, wal_xid_t xid)
{
    const xid_entry_t *e = xid_find(wal, xid);
    return e != NULL ? (wal_xid_state_t)e->state : WAL_XID_IN_PROGRESS;
}

/* ---- 복구 ---- */

/* qsort는 구현이 임시 힙을 쓸 수 있어 예산 밖이 된다. 추가 공간 없는 제자리 heapsort를 쓴다. */
typedef int (*elem_cmp_t)(const void *, const void *);

static void elem_swap(unsigned char *a, unsigned char *b, size_t size)
{
    while (size-- > 0) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(unsigned char *base, size_t size, size_t root, size_t n, elem_cmp_t cmp)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        elem_swap(base + root * size, base + child * size, size);
        root = child;
    }
}

static void inplace_sort(void *base, size_t n, size_t size, elem_cmp_t cmp)
{
    unsigned char *b = base;
    if (n < 2) return;
    for (size_t i = n / 2; i-- > 0;) sift_down(b, size, i, n, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        elem_swap(b, b + end * size, size);
        sift_down(b, size, 0, end, cmp);
    }
}

static int xid_cmp(const void *a, const void *b)
{
    wal_xid_t x = *(const wal_xid_t *)a, y = *(const wal_xid_t *)b;
    return x < y ? -1 : x > y;
}

static wal_status_t finish_xid_states(wal_t *wal, const wal_replay_ops_t *ops, void *ctx)
{
    if (ops->xid_state == NULL || wal->xid_count == 0) return WAL_OK;
    uint64_t bytes = (uint64_t)wal->xid_count * sizeof(wal_xid_t);
    if (!mem_charge(wal, bytes)) return WAL_LIMIT;
    wal_xid_t *ids = malloc(wal->xid_count * sizeof(*ids));
    if (ids == NULL) {
        mem_release(wal, bytes);
        return WAL_NOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < wal->xid_cap; i++) {
        if (wal->xids[i].used) ids[n++] = wal->xids[i].xid;
    }
    inplace_sort(ids, n, sizeof(*ids), xid_cmp);
    for (size_t i = 0; i < n; i++) {
        wal_xid_state_t s = wal_xid_state(wal, ids[i]);
        ops->xid_state(ctx, ids[i], s == WAL_XID_IN_PROGRESS ? WAL_XID_CRASHED : s);
    }
    free(ids);
    mem_release(wal, bytes);
    return WAL_OK;
}

static wal_redo_result_t deliver(const wal_replay_ops_t *ops, void *ctx, const rec_t *r, wal_lsn_t commit_lsn)
{
    wal_record_t out = {r->kind, r->xid, r->page_id, r->lsn, r->prev_lsn, commit_lsn, r->payload_len, r->payload};
    return ops->redo(ctx, &out);
}

static int commit_cmp(const void *a, const void *b)
{
    const xid_entry_t *x = *(const xid_entry_t *const *)a, *y = *(const xid_entry_t *const *)b;
    return x->commit_lsn < y->commit_lsn ? -1 : x->commit_lsn > y->commit_lsn;
}

/* 비용: COMMITTED_ONLY는 대상 거래의 첫 레코드부터 쓴 끝까지 한 번 읽고, 대상 DATA마다 한 번 더 읽는다.
 * 메모리는 대상 DATA 하나에 8바이트와 대상 거래 수 */
static wal_status_t replay_committed(wal_t *wal, const wal_replay_ops_t *ops, void *ctx,
                                     wal_replay_stats_t *stats)
{
    if (wal->buf_len > 0) return WAL_INVALID;
    uint64_t cand_bytes = ((uint64_t)wal->xid_count + 1) * sizeof(xid_entry_t *);
    if (!mem_charge(wal, cand_bytes)) return WAL_LIMIT;
    xid_entry_t **cand = malloc((wal->xid_count + 1) * sizeof(*cand));
    if (cand == NULL) {
        mem_release(wal, cand_bytes);
        return WAL_NOMEM;
    }
    size_t n = 0;
    uint64_t min_first = UINT64_MAX;
    for (size_t i = 0; i < wal->xid_cap; i++) {
        xid_entry_t *e = &wal->xids[i];
        if (!e->used || e->state != WAL_XID_COMMITTED || e->commit_lsn <= ops->applied_lsn) continue;
        e->pending = true;
        cand[n++] = e;
        if (e->first_lsn != 0 && e->first_lsn < min_first) min_first = e->first_lsn;
    }
    wal_status_t st = WAL_OK;
    reader_t rd = {&wal->io, wal->fd, wal->written, NULL, 0, 0, 0, wal};
    if (n > 0 && min_first != UINT64_MAX) {
        for (uint64_t off = min_first; off < wal->written;) {
            rec_t r;
            int rc = rec_parse(&rd, off, wal->file_id, &r);
            if (rc != REC_OK) {
                st = rc == REC_IO ? errno_status() : WAL_CORRUPT;
                goto done;
            }
            stats->scanned++;
            xid_entry_t *e = r.kind == WAL_REC_DATA ? xid_find(wal, r.xid) : NULL;
            if (e != NULL && e->pending) {
                if (e->lsn_len == e->lsn_cap) {
                    if (e->lsn_cap > SIZE_MAX / 2 / sizeof(uint64_t)) {
                        st = WAL_LIMIT;
                        goto done;
                    }
                    size_t nc = e->lsn_cap == 0 ? 4 : e->lsn_cap * 2;
                    if (!mem_charge(wal, (uint64_t)nc * sizeof(uint64_t))) { /* old+new */
                        st = WAL_LIMIT;
                        goto done;
                    }
                    uint64_t *nl = realloc(e->lsns, nc * sizeof(*nl));
                    if (nl == NULL) {
                        mem_release(wal, (uint64_t)nc * sizeof(uint64_t));
                        st = WAL_NOMEM;
                        goto done;
                    }
                    mem_release(wal, (uint64_t)e->lsn_cap * sizeof(uint64_t));
                    e->lsns = nl;
                    e->lsn_cap = nc;
                }
                e->lsns[e->lsn_len++] = r.lsn;
            }
            off += r.span;
        }
    }
    inplace_sort(cand, n, sizeof(*cand), commit_cmp);
    for (size_t i = 0; i < n; i++) {
        xid_entry_t *e = cand[i];
        for (size_t k = 0; k < e->lsn_len; k++) {
            rec_t r;
            int rc = rec_parse(&rd, e->lsns[k], wal->file_id, &r);
            if (rc != REC_OK || r.kind != WAL_REC_DATA || r.xid != e->xid) {
                st = rc == REC_IO ? errno_status() : WAL_CORRUPT;
                goto done;
            }
            wal_redo_result_t res = deliver(ops, ctx, &r, e->commit_lsn);
            if (res == WAL_REDO_ERROR) {
                st = WAL_REDO_FAILED;
                goto done;
            }
            if (res == WAL_REDO_APPLIED) stats->applied++;
            else stats->skipped++;
        }
        if (ops->txn_done != NULL && !ops->txn_done(ctx, e->xid, e->commit_lsn)) {
            st = WAL_REDO_FAILED;
            goto done;
        }
        stats->txns++;
    }
done:
    for (size_t i = 0; i < wal->xid_cap; i++) {
        wal->xids[i].pending = false;
        mem_release(wal, (uint64_t)wal->xids[i].lsn_cap * sizeof(uint64_t));
        free(wal->xids[i].lsns);
        wal->xids[i].lsns = NULL;
        wal->xids[i].lsn_len = wal->xids[i].lsn_cap = 0;
    }
    rd_free(&rd);
    free(cand);
    mem_release(wal, cand_bytes);
    return st;
}

/* 비용: ALL은 redo 시작부터 쓴 끝까지 한 번 읽는다 */
static wal_status_t replay_all(wal_t *wal, const wal_replay_ops_t *ops, void *ctx,
                               wal_replay_stats_t *stats)
{
    reader_t rd = {&wal->io, wal->fd, wal->written, NULL, 0, 0, 0, wal};
    wal_status_t st = WAL_OK;
    for (uint64_t off = wal->redo_lsn; off < wal->written;) {
        rec_t r;
        int rc = rec_parse(&rd, off, wal->file_id, &r);
        if (rc != REC_OK) {
            st = rc == REC_IO ? errno_status() : WAL_CORRUPT;
            break;
        }
        stats->scanned++;
        if (r.kind == WAL_REC_DATA) {
            wal_redo_result_t res = deliver(ops, ctx, &r, 0);
            if (res == WAL_REDO_ERROR) {
                st = WAL_REDO_FAILED;
                break;
            }
            if (res == WAL_REDO_APPLIED) stats->applied++;
            else stats->skipped++;
        }
        off += r.span;
    }
    rd_free(&rd);
    return st;
}

wal_status_t wal_replay(wal_t *wal, const wal_replay_ops_t *ops, void *ctx,
                        wal_replay_stats_t *stats)
{
    wal_replay_stats_t local;
    if (stats == NULL) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (wal == NULL || ops == NULL || ops->redo == NULL) return WAL_INVALID;
    wal_status_t st;
    if (ops->mode == WAL_REPLAY_COMMITTED_ONLY) st = replay_committed(wal, ops, ctx, stats);
    else if (ops->mode == WAL_REPLAY_ALL) st = replay_all(wal, ops, ctx, stats);
    else return WAL_INVALID;
    if (st != WAL_OK) return st;
    return finish_xid_states(wal, ops, ctx);
}

const char *wal_status_name(wal_status_t status)
{
    static const char *const names[] = {
        "OK", "INVALID", "IO", "CORRUPT", "UNSUPPORTED", "NOMEM", "LOCKED", "EXISTS",
        "COMMIT_UNKNOWN", "POISONED", "NOT_FLUSHED", "REDO_FAILED", "LIMIT"};
    return (size_t)status < sizeof(names) / sizeof(names[0]) ? names[status] : "?";
}
