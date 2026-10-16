#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/page_file.h"

#include "page_cache.h"
#include "storage/record_page.h"

_Static_assert(RECORD_PAGE_BYTES == PAGE_BODY_BYTES,
               "record page body must equal the page_file body");

#define EINTR_RETRY_MAX 100
#define CONTROL_HEADER_BYTES 68u
#define CONTROL_INLINE_IDS ((PAGE_BODY_BYTES - CONTROL_HEADER_BYTES) / 4u)
#define CHAIN_HEADER_BYTES 8u
#define CHAIN_IDS ((PAGE_BODY_BYTES - CHAIN_HEADER_BYTES) / 4u)
#define BYTE_ORDER_MARK 0x01020304u
#define PARTIAL_SUFFIX ".partial"

/*
 * 제어 본문 배치: magic 8, 형식 u32, 페이지 크기 u32, byte order u32, 세대 u64, page_count u32, 예약 u32(0),
 * 카탈로그 head u32, size u32, 카탈로그 세대 u64, 재사용 수 u32, 대기 수 u32, 사슬 head u32, 사슬 길이 u32,
 * 그 뒤에 빈 페이지 번호(재사용 다음 대기 순서)를 CONTROL_INLINE_IDS개까지. 나머지는 사슬 페이지에
 * 이어 쓴다. 사슬 본문: 다음 사슬 u32, 이 페이지의 번호 수 u32, 번호들.
 */
static const uint8_t FILE_MAGIC[8] = {'L', 'R', 'S', 'Q', 'L', 'D', 'B', 0};

/* 쓰기 핸들이 아는 페이지 하나의 상태 */
typedef enum {
    PS_USED,       /* 게시된 상태가 가리킬 수 있다 */
    PS_FRESH,      /* 마지막 commit 뒤에 받았다. 어느 제어 페이지도 가리킨 적 없다 */
    PS_PENDING,    /* 이번 commit에서 놓았다 */
    PS_AGING,      /* 직전 commit에서 놓았다. fallback 세대가 가리킬 수 있다 */
    PS_REUSABLE,   /* 두 제어 슬롯 모두 가리키지 않는다 */
    PS_DISCARDED   /* 새 페이지였다가 놓았다. 재사용 목록과 새 페이지 목록에 함께 있다 */
} page_state_t;

typedef struct {
    uint32_t reusable;
    uint32_t aging;
    uint32_t chain_head;
    uint32_t chain_count;
} free_header_t;

typedef struct {
    uint32_t *items;
    size_t len, cap;
} id_list_t;

struct page_file {
    int fd;
    bool writable;
    bool published;
    bool poisoned;
    page_file_commit_outcome_t commit_outcome;
    char *path;
    char *partial_path;
    page_file_io_t io;
    page_file_control_t committed;
    uint32_t page_count;
    uint64_t file_bytes;
    uint32_t staged_head;
    uint32_t staged_size;
    uint64_t staged_generation;
    uint8_t *state;         /* 쓰기 핸들만. 페이지마다 page_state_t. 길이 state_cap >= page_count */
    size_t state_cap;
    id_list_t pending;
    id_list_t aging;        /* 직전 commit이 더는 가리키지 않게 된 페이지. 한 번 더 commit해야 재사용된다 */
    id_list_t reusable;
    id_list_t fresh;        /* 마지막 commit 뒤에 받은 페이지(PS_FRESH·PS_DISCARDED) */
    id_list_t chain;        /* 현재 제어 페이지가 가리키는 빈 목록 사슬 페이지 */
    size_t cache_pages;     /* 이 핸들의 캐시 상한. 계상은 이 값으로 한다 */
    page_cache_t *cache;    /* 처음 쓰일 때 만든다. 만들지 못하면 NULL로 두고 캐시 없이 동작한다 */
    bool cache_tried;
};

static ssize_t libc_pread(int fd, void *buf, size_t size, off_t offset)
{
    return pread(fd, buf, size, offset);
}

static ssize_t libc_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    return pwrite(fd, buf, size, offset);
}

static const page_file_io_t LIBC_IO = {libc_pread, libc_pwrite, fsync, link, unlink};
static page_file_io_t g_default_io = {libc_pread, libc_pwrite, fsync, link, unlink};
static size_t g_default_cache_pages = PAGE_FILE_CACHE_PAGES;

void page_file_set_default_cache_pages(size_t pages) { g_default_cache_pages = pages; }

void page_file_set_default_io(const page_file_io_t *io)
{
    g_default_io = io != NULL ? *io : LIBC_IO;
}

page_file_io_t page_file_default_io(void)
{
    return g_default_io;
}

static void put_u16(uint8_t *at, uint16_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *at, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) at[i] = (uint8_t)(value >> (i * 8));
}

static void put_u64(uint8_t *at, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) at[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t get_u16(const uint8_t *at)
{
    return (uint16_t)((uint16_t)at[0] | (uint16_t)at[1] << 8);
}

static uint32_t get_u32(const uint8_t *at)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value |= (uint32_t)at[i] << (i * 8);
    return value;
}

static uint64_t get_u64(const uint8_t *at)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)at[i] << (i * 8);
    return value;
}

/* 체크섬 필드(28..31)를 뺀 페이지 전체를 덮어 패딩 손상도 잡는다. */
static uint32_t page_checksum(const uint8_t *page)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < PAGE_FILE_PAGE_BYTES; i++) {
        if (i >= 28 && i < 32) continue;
        hash = (hash ^ page[i]) * 16777619u;
    }
    return hash;
}

static void encode_page(uint8_t *page, page_kind_t kind, uint32_t page_id, uint32_t table_id,
                        uint32_t body_len, const uint8_t *body)
{
    memset(page, 0, PAGE_FILE_PAGE_BYTES);
    memcpy(page, "LRPG", 4);
    put_u16(page + 4, 1);
    put_u16(page + 6, (uint16_t)kind);
    put_u32(page + 8, page_id);
    put_u32(page + 12, table_id);
    put_u32(page + 16, body_len);
    memcpy(page + PAGE_ENVELOPE_BYTES, body, body_len);
    put_u32(page + 28, page_checksum(page));
}

typedef enum {
    SLOT_BLANK,
    SLOT_VALID,
    SLOT_DAMAGED,
    SLOT_UNSUPPORTED
} slot_state_t;

typedef struct {
    page_kind_t kind;
    uint32_t table_id;
    uint32_t body_len;
} envelope_t;

/* envelope만 검사한다. 손상이면 SLOT_DAMAGED, 알 수 없는 envelope 버전이면 SLOT_UNSUPPORTED. */
static slot_state_t decode_envelope(const uint8_t *page, uint32_t expect_id, envelope_t *out)
{
    if (memcmp(page, "LRPG", 4) != 0) return SLOT_DAMAGED;
    if (get_u32(page + 28) != page_checksum(page)) return SLOT_DAMAGED;
    if (get_u16(page + 4) != 1) return SLOT_UNSUPPORTED;
    uint32_t body_len = get_u32(page + 16);
    if (get_u32(page + 8) != expect_id || body_len > PAGE_BODY_BYTES) return SLOT_DAMAGED;
    for (size_t i = 20; i < 28; i++) {
        if (page[i] != 0) return SLOT_DAMAGED;
    }
    for (size_t i = PAGE_ENVELOPE_BYTES + body_len; i < PAGE_FILE_PAGE_BYTES; i++) {
        if (page[i] != 0) return SLOT_DAMAGED;
    }
    out->kind = (page_kind_t)get_u16(page + 6);
    out->table_id = get_u32(page + 12);
    out->body_len = body_len;
    return SLOT_VALID;
}

static bool is_blank(const uint8_t *page)
{
    for (size_t i = 0; i < PAGE_FILE_PAGE_BYTES; i++) {
        if (page[i] != 0) return false;
    }
    return true;
}

static uint32_t inline_ids(uint64_t total)
{
    return total < CONTROL_INLINE_IDS ? (uint32_t)total : CONTROL_INLINE_IDS;
}

static uint32_t chain_pages_for(uint64_t total)
{
    if (total <= CONTROL_INLINE_IDS) return 0;
    return (uint32_t)((total - CONTROL_INLINE_IDS + CHAIN_IDS - 1) / CHAIN_IDS);
}

static slot_state_t decode_control(const uint8_t *page, uint32_t slot, page_file_control_t *out,
                                   free_header_t *free_out)
{
    if (is_blank(page)) return SLOT_BLANK;
    envelope_t envelope;
    slot_state_t state = decode_envelope(page, slot, &envelope);
    if (state != SLOT_VALID) return state;
    if (envelope.kind != PAGE_KIND_CONTROL || envelope.table_id != 0) return SLOT_DAMAGED;
    const uint8_t *body = page + PAGE_ENVELOPE_BYTES;
    if (envelope.body_len < 12 || memcmp(body, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0) return SLOT_UNSUPPORTED;
    if (get_u32(body + 8) != PAGE_FILE_FORMAT_VERSION) return SLOT_UNSUPPORTED;
    if (envelope.body_len < CONTROL_HEADER_BYTES) return SLOT_DAMAGED;
    if (get_u32(body + 12) != PAGE_FILE_PAGE_BYTES) return SLOT_UNSUPPORTED;
    if (get_u32(body + 16) != BYTE_ORDER_MARK) return SLOT_UNSUPPORTED;
    page_file_control_t control = {
        .generation = get_u64(body + 20),
        .page_count = get_u32(body + 28),
        .catalog_head = get_u32(body + 36),
        .catalog_size = get_u32(body + 40),
        .catalog_generation = get_u64(body + 44),
    };
    if (get_u32(body + 32) != 0) return SLOT_DAMAGED;
    if (control.generation == 0 || control.generation % 2 != slot) return SLOT_DAMAGED;
    if (control.page_count < PAGE_FILE_CONTROL_PAGES || control.page_count > PAGE_FILE_MAX_PAGES) {
        return SLOT_DAMAGED;
    }
    if ((control.catalog_head == PAGE_FILE_NO_PAGE) != (control.catalog_size == 0)) {
        return SLOT_DAMAGED;
    }
    if (control.catalog_head != PAGE_FILE_NO_PAGE &&
        (control.catalog_head < PAGE_FILE_CONTROL_PAGES ||
         control.catalog_head >= control.page_count)) {
        return SLOT_DAMAGED;
    }
    free_header_t free_list = {get_u32(body + 52), get_u32(body + 56), get_u32(body + 60), get_u32(body + 64)};
    uint64_t total = (uint64_t)free_list.reusable + free_list.aging;
    if (total > control.page_count - PAGE_FILE_CONTROL_PAGES ||
        envelope.body_len != CONTROL_HEADER_BYTES + 4u * inline_ids(total) ||
        free_list.chain_count != chain_pages_for(total)) {
        return SLOT_DAMAGED;
    }
    if ((free_list.chain_count == 0) != (free_list.chain_head == PAGE_FILE_NO_PAGE)) return SLOT_DAMAGED;
    if (free_list.chain_head != PAGE_FILE_NO_PAGE &&
        (free_list.chain_head < PAGE_FILE_CONTROL_PAGES || free_list.chain_head >= control.page_count)) {
        return SLOT_DAMAGED;
    }
    *out = control;
    if (free_out != NULL) *free_out = free_list;
    return SLOT_VALID;
}

/* 본문 앞 CONTROL_HEADER_BYTES를 채운다. 번호는 호출자가 뒤에 쓴다 */
static void encode_control_header(uint8_t *body, const page_file_control_t *control, const free_header_t *free_list)
{
    memset(body, 0, CONTROL_HEADER_BYTES);
    memcpy(body, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_u32(body + 8, PAGE_FILE_FORMAT_VERSION);
    put_u32(body + 12, PAGE_FILE_PAGE_BYTES);
    put_u32(body + 16, BYTE_ORDER_MARK);
    put_u64(body + 20, control->generation);
    put_u32(body + 28, control->page_count);
    put_u32(body + 36, control->catalog_head);
    put_u32(body + 40, control->catalog_size);
    put_u64(body + 44, control->catalog_generation);
    put_u32(body + 52, free_list->reusable);
    put_u32(body + 56, free_list->aging);
    put_u32(body + 60, free_list->chain_head);
    put_u32(body + 64, free_list->chain_count);
}

/* ── 저수준 I/O: EINTR·짧은 I/O·EOF·ENOSPC를 처리한다 ── */

static page_file_status_t read_full(const page_file_t *file, uint8_t *buf, size_t size, uint64_t offset)
{
    size_t done = 0;
    int eintr = 0;
    while (done < size) {
        ssize_t got = file->io.pread(file->fd, buf + done, size - done, (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR && ++eintr <= EINTR_RETRY_MAX) continue;
            return PAGE_FILE_IO;
        }
        if (got == 0) {
            errno = EIO;
            return PAGE_FILE_IO;
        }
        eintr = 0;
        done += (size_t)got;
    }
    return PAGE_FILE_OK;
}

static page_file_status_t write_full(const page_file_t *file, const uint8_t *buf, size_t size,
                                     uint64_t offset)
{
    size_t done = 0;
    int eintr = 0;
    while (done < size) {
        ssize_t put = file->io.pwrite(file->fd, buf + done, size - done, (off_t)(offset + done));
        if (put < 0) {
            if (errno == EINTR && ++eintr <= EINTR_RETRY_MAX) continue;
            return PAGE_FILE_IO;
        }
        if (put == 0) {
            errno = ENOSPC;
            return PAGE_FILE_IO;
        }
        eintr = 0;
        done += (size_t)put;
    }
    return PAGE_FILE_OK;
}

static page_file_status_t sync_file(page_file_t *file)
{
    int eintr = 0;
    while (file->io.fsync(file->fd) != 0) {
        if (errno == EINTR && ++eintr <= EINTR_RETRY_MAX) continue;
        /* 커널이 더러운 페이지를 버렸을 수 있어 이 핸들로는 더 이상 성공을 주장하지 않는다. */
        file->poisoned = true;
        return PAGE_FILE_IO;
    }
    return PAGE_FILE_OK;
}

static page_file_t *new_handle(int fd, bool writable)
{
    page_file_t *file = calloc(1, sizeof(*file));
    if (file == NULL) return NULL;
    file->fd = fd;
    file->writable = writable;
    file->io = g_default_io;
    file->cache_pages = g_default_cache_pages;
    return file;
}

static void free_handle(page_file_t *file)
{
    free(file->path);
    free(file->partial_path);
    free(file->state);
    free(file->pending.items);
    free(file->aging.items);
    free(file->reusable.items);
    free(file->fresh.items);
    free(file->chain.items);
    page_cache_destroy(file->cache);
    free(file);
}

static char *copy_string(const char *text, const char *suffix)
{
    size_t length = strlen(text), extra = suffix != NULL ? strlen(suffix) : 0;
    char *copy = malloc(length + extra + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    if (extra > 0) memcpy(copy + length, suffix, extra);
    copy[length + extra] = '\0';
    return copy;
}

static page_file_status_t lock_fd(int fd, bool exclusive)
{
    if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) return PAGE_FILE_OK;
    return errno == EWOULDBLOCK ? PAGE_FILE_LOCKED : PAGE_FILE_IO;
}

/* ── 생성·게시 ── */

page_file_status_t page_file_create(const char *path, page_file_t **out)
{
    if (path == NULL || out == NULL || path[0] == '\0') return PAGE_FILE_INVALID;
    if (strlen(path) + sizeof(PARTIAL_SUFFIX) >= PATH_MAX) return PAGE_FILE_INVALID;
    struct stat existing;
    if (lstat(path, &existing) == 0) {
        errno = EEXIST;
        return PAGE_FILE_EXISTS;
    }
    if (errno != ENOENT) return PAGE_FILE_IO;

    char *partial = copy_string(path, PARTIAL_SUFFIX);
    char *final_path = copy_string(path, NULL);
    if (partial == NULL || final_path == NULL) {
        free(partial);
        free(final_path);
        return PAGE_FILE_NOMEM;
    }
    int fd = open(partial, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        int err = errno;
        free(partial);
        free(final_path);
        errno = err;
        return err == EEXIST ? PAGE_FILE_EXISTS : PAGE_FILE_IO;
    }
    page_file_t *file = new_handle(fd, true);
    if (file == NULL) {
        close(fd);
        unlink(partial);
        free(partial);
        free(final_path);
        return PAGE_FILE_NOMEM;
    }
    file->path = final_path;
    file->partial_path = partial;
    page_file_status_t status = lock_fd(fd, true);
    uint8_t zero[PAGE_FILE_PAGE_BYTES] = {0};
    for (uint32_t slot = 0; status == PAGE_FILE_OK && slot < PAGE_FILE_CONTROL_PAGES; slot++) {
        status = write_full(file, zero, sizeof(zero), (uint64_t)slot * PAGE_FILE_PAGE_BYTES);
    }
    if (status != PAGE_FILE_OK) {
        int err = errno;
        close(fd);
        file->io.unlink(partial);
        free_handle(file);
        errno = err;
        return status;
    }
    file->page_count = PAGE_FILE_CONTROL_PAGES;
    file->file_bytes = (uint64_t)PAGE_FILE_CONTROL_PAGES * PAGE_FILE_PAGE_BYTES;
    *out = file;
    return PAGE_FILE_OK;
}

static page_file_status_t sync_directory(const page_file_t *file, const char *path)
{
    char *directory = copy_string(path, NULL);
    if (directory == NULL) return PAGE_FILE_NOMEM;
    char *slash = strrchr(directory, '/');
    if (slash == NULL) {
        free(directory);
        directory = copy_string(".", NULL);
        if (directory == NULL) return PAGE_FILE_NOMEM;
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int fd = open(directory, O_RDONLY);
    free(directory);
    if (fd < 0) return PAGE_FILE_IO;
    page_file_status_t status = PAGE_FILE_OK;
    int eintr = 0;
    while (file->io.fsync(fd) != 0) {
        if (errno == EINTR && ++eintr <= EINTR_RETRY_MAX) continue;
        status = PAGE_FILE_IO;
        break;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return status;
}

page_file_status_t page_file_publish(page_file_t *file)
{
    if (file == NULL || !file->writable || file->published || file->partial_path == NULL ||
        file->committed.generation == 0) {
        return PAGE_FILE_INVALID;
    }
    page_file_status_t status = sync_file(file);
    if (status != PAGE_FILE_OK) return status;
    /* link는 목적지가 이미 있으면 실패한다. rename은 조용히 덮어쓰므로 쓰지 않는다. */
    if (file->io.link(file->partial_path, file->path) != 0) {
        int err = errno;
        if (err == EEXIST) return PAGE_FILE_EXISTS;
        if (err == ENOTSUP || err == EPERM || err == EXDEV || err == ENOSYS) {
            errno = err;
            return PAGE_FILE_UNSUPPORTED;
        }
        errno = err;
        return PAGE_FILE_IO;
    }
    file->published = true;
    page_file_status_t result = PAGE_FILE_OK;
    if (file->io.unlink(file->partial_path) != 0) result = PAGE_FILE_PUBLISHED_DIRTY;
    if (sync_directory(file, file->path) != PAGE_FILE_OK) result = PAGE_FILE_PUBLISHED_DIRTY;
    free(file->partial_path);
    file->partial_path = NULL;
    return result;
}

/* ── 열기·닫기 ── */

static page_file_status_t load_free_list(page_file_t *file, const uint8_t *control_page, const free_header_t *header);

static page_file_status_t pick_control(const uint8_t pages[2][PAGE_FILE_PAGE_BYTES],
                                       page_file_control_t *chosen, free_header_t *chosen_free,
                                       page_file_open_info_t *info)
{
    page_file_control_t controls[2];
    free_header_t frees[2];
    slot_state_t states[2];
    for (uint32_t slot = 0; slot < 2; slot++) {
        states[slot] = decode_control(pages[slot], slot, &controls[slot], &frees[slot]);
    }
    memset(info, 0, sizeof(*info));
    if (states[0] == SLOT_UNSUPPORTED || states[1] == SLOT_UNSUPPORTED) return PAGE_FILE_UNSUPPORTED;
    int valid = (states[0] == SLOT_VALID) + (states[1] == SLOT_VALID);
    if (valid == 0) {
        if (states[0] == SLOT_BLANK && states[1] == SLOT_BLANK) return PAGE_FILE_NO_CONTROL;
        return PAGE_FILE_CORRUPT;
    }
    if (valid == 2) {
        uint64_t a = controls[0].generation, b = controls[1].generation;
        /* 세대는 두 슬롯에 번갈아 쓰이므로 차이가 1이 아니면 한쪽 쓰기가 사라진 것이다. */
        if ((a > b ? a - b : b - a) != 1) return PAGE_FILE_CORRUPT;
        *chosen = a > b ? controls[0] : controls[1];
        *chosen_free = a > b ? frees[0] : frees[1];
        return PAGE_FILE_OK;
    }
    uint32_t good = states[0] == SLOT_VALID ? 0 : 1;
    uint32_t other = 1 - good;
    *chosen = controls[good];
    *chosen_free = frees[good];
    if (states[other] == SLOT_BLANK) {
        info->blank_slots = 1;
        /* 두 번째 세대부터는 반대편 슬롯에도 내용이 있어야 한다. 0으로 지워졌다면 손상이다. */
        if (chosen->generation >= 2) {
            info->blank_slots = 0;
            info->damaged_slots = 1;
        }
    } else {
        info->damaged_slots = 1;
    }
    return PAGE_FILE_OK;
}

page_file_status_t page_file_open(const char *path, bool writable, page_file_t **out,
                                  page_file_open_info_t *info)
{
    return page_file_open_admitted(path, writable, out, info, NULL, NULL);
}

page_file_status_t page_file_open_admitted(const char *path, bool writable, page_file_t **out,
                                           page_file_open_info_t *info, page_file_admit_fn admit,
                                           void *ctx)
{
    if (path == NULL || out == NULL) return PAGE_FILE_INVALID;
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) return PAGE_FILE_IO;
    page_file_status_t status = lock_fd(fd, writable);
    if (status != PAGE_FILE_OK) {
        int err = errno;
        close(fd);
        errno = err;
        return status;
    }
    if (admit != NULL) {
        /* 잠근 fd의 크기로 상한을 정한다. 이 뒤의 할당은 모두 admit가 허락한 몫 안에서만 일어난다 */
        struct stat sized;
        if (fstat(fd, &sized) != 0) {
            int err = errno;
            close(fd);
            errno = err;
            return PAGE_FILE_IO;
        }
        if (!admit(ctx, sized.st_size > 0 ? (uint64_t)sized.st_size : 0)) {
            close(fd);
            return PAGE_FILE_NOMEM;
        }
    }
    page_file_t *file = new_handle(fd, writable);
    if (file == NULL) {
        close(fd);
        return PAGE_FILE_NOMEM;
    }
    file->path = copy_string(path, NULL);
    if (file->path == NULL) {
        close(fd);
        free_handle(file);
        return PAGE_FILE_NOMEM;
    }

    uint8_t pages[2][PAGE_FILE_PAGE_BYTES];
    struct stat st;
    page_file_control_t chosen;
    free_header_t chosen_free;
    page_file_open_info_t local_info;
    status = PAGE_FILE_OK;
    if (fstat(fd, &st) != 0) {
        status = PAGE_FILE_IO;
    } else {
        /* 옛 MINIDB 파일은 길이와 상관없이 첫 8바이트로 알아보고 거절한다. */
        uint8_t magic[8] = {0};
        ssize_t got = pread(fd, magic, sizeof(magic), 0);
        if (got == (ssize_t)sizeof(magic) && memcmp(magic, "MINIDB\0", 8) == 0) {
            status = PAGE_FILE_UNSUPPORTED;
        } else if (st.st_size < (off_t)(PAGE_FILE_CONTROL_PAGES * PAGE_FILE_PAGE_BYTES)) {
            status = st.st_size == 0 ? PAGE_FILE_NO_CONTROL : PAGE_FILE_CORRUPT;
        }
    }
    for (uint32_t slot = 0; status == PAGE_FILE_OK && slot < 2; slot++) {
        status = read_full(file, pages[slot], PAGE_FILE_PAGE_BYTES,
                           (uint64_t)slot * PAGE_FILE_PAGE_BYTES);
    }
    if (status == PAGE_FILE_OK) status = pick_control(pages, &chosen, &chosen_free, &local_info);
    if (status == PAGE_FILE_OK && (uint64_t)st.st_size < (uint64_t)chosen.page_count * PAGE_FILE_PAGE_BYTES) {
        status = PAGE_FILE_CORRUPT; /* 파일이 제어 페이지가 약속한 길이보다 짧다 */
    }
    if (status == PAGE_FILE_OK && writable) {
        /* 빈 목록은 쓰기 핸들만 쓴다. 읽기 핸들은 읽지 않는다 */
        file->page_count = chosen.page_count;
        status = load_free_list(file, pages[chosen.generation % 2], &chosen_free);
    }
    if (status != PAGE_FILE_OK) {
        int err = errno;
        close(fd);
        free_handle(file);
        errno = err;
        return status;
    }
    file->committed = chosen;
    file->page_count = chosen.page_count;
    file->file_bytes = (uint64_t)st.st_size;
    file->published = true;
    file->staged_head = chosen.catalog_head;
    file->staged_size = chosen.catalog_size;
    file->staged_generation = chosen.catalog_generation;
    if (info != NULL) *info = local_info;
    *out = file;
    return PAGE_FILE_OK;
}

page_file_status_t page_file_close(page_file_t *file)
{
    if (file == NULL) return PAGE_FILE_INVALID;
    page_file_status_t status = PAGE_FILE_OK;
    if (!file->published && file->partial_path != NULL) file->io.unlink(file->partial_path);
    if (close(file->fd) != 0) status = PAGE_FILE_IO;
    free_handle(file);
    return status;
}

const page_file_control_t *page_file_control(const page_file_t *file)
{
    return file != NULL ? &file->committed : NULL;
}

uint32_t page_file_allocated_pages(const page_file_t *file)
{
    return file != NULL ? file->page_count : 0;
}

bool page_file_is_published(const page_file_t *file)
{
    return file != NULL && file->published;
}

const char *page_file_partial_path(const page_file_t *file)
{
    return file != NULL ? file->partial_path : NULL;
}

/* ── 페이지 읽기·쓰기 ── */

static page_cache_t *cache_of(page_file_t *file)
{
    if (!file->cache_tried) {
        file->cache_tried = true;
        file->cache = page_cache_create(file->cache_pages);
    }
    return file->cache;
}

/*
 * 데이터 페이지 한 장을 파일에서 읽어 envelope와 체크섬을 검증한다. 검증에 성공한 본문만 캐시에 넣는다.
 * 종류와 table ID의 기대값 대조는 호출자가 한다(캐시 적중 경로와 같은 검사를 쓰기 위해서다).
 */
static page_file_status_t read_verified(page_file_t *file, uint32_t page_id, uint8_t *page, envelope_t *envelope)
{
    page_file_status_t status = read_full(file, page, PAGE_FILE_PAGE_BYTES, (uint64_t)page_id * PAGE_FILE_PAGE_BYTES);
    if (status != PAGE_FILE_OK) return status;
    slot_state_t state = decode_envelope(page, page_id, envelope);
    if (state == SLOT_UNSUPPORTED) return PAGE_FILE_UNSUPPORTED;
    if (state != SLOT_VALID || envelope->body_len != PAGE_BODY_BYTES) {
        errno = EBADMSG;
        return PAGE_FILE_CORRUPT;
    }
    page_cache_put(cache_of(file), page_id, (uint16_t)envelope->kind, envelope->table_id, page + PAGE_ENVELOPE_BYTES);
    return PAGE_FILE_OK;
}

static bool data_page_in_range(const page_file_t *file, uint32_t page_id)
{
    return page_id >= PAGE_FILE_CONTROL_PAGES && page_id < file->page_count;
}

page_file_status_t page_file_read(page_file_t *file, uint32_t page_id, page_kind_t kind,
                                  uint32_t table_id, uint8_t body[PAGE_BODY_BYTES])
{
    if (file == NULL || body == NULL || kind == PAGE_KIND_CONTROL) return PAGE_FILE_INVALID;
    if (!data_page_in_range(file, page_id)) return PAGE_FILE_INVALID;
    uint16_t cached_kind;
    uint32_t cached_table;
    if (page_cache_get(cache_of(file), page_id, &cached_kind, &cached_table, body)) {
        if (cached_kind != kind || cached_table != table_id) {
            errno = EBADMSG;
            return PAGE_FILE_CORRUPT;
        }
        return PAGE_FILE_OK;
    }
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    envelope_t envelope;
    page_file_status_t status = read_verified(file, page_id, page, &envelope);
    if (status != PAGE_FILE_OK) return status;
    if (envelope.kind != kind || envelope.table_id != table_id) {
        errno = EBADMSG;
        return PAGE_FILE_CORRUPT;
    }
    memcpy(body, page + PAGE_ENVELOPE_BYTES, PAGE_BODY_BYTES);
    return PAGE_FILE_OK;
}

page_file_status_t page_file_write(page_file_t *file, uint32_t page_id, page_kind_t kind,
                                   uint32_t table_id, const uint8_t body[PAGE_BODY_BYTES])
{
    if (file == NULL || body == NULL || !file->writable || kind == PAGE_KIND_CONTROL || kind == PAGE_KIND_FREE_LIST) {
        return PAGE_FILE_INVALID;
    }
    if (!data_page_in_range(file, page_id)) return PAGE_FILE_INVALID;
    if (file->poisoned) {
        /* 게시 결과를 모르는 핸들은 어느 세대가 가리킬지 모르는 페이지를 덮어쓰지 않는다 */
        errno = EIO;
        return PAGE_FILE_IO;
    }
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    encode_page(page, kind, page_id, table_id, PAGE_BODY_BYTES, body);
    uint64_t offset = (uint64_t)page_id * PAGE_FILE_PAGE_BYTES;
    page_file_status_t status = write_full(file, page, sizeof(page), offset);
    if (status != PAGE_FILE_OK) {
        /* 일부만 쓰였을 수 있어 디스크 내용을 알 수 없다. 다음 읽기는 파일에서 검증한다 */
        page_cache_drop(file->cache, page_id);
        return status;
    }
    if (offset + PAGE_FILE_PAGE_BYTES > file->file_bytes) file->file_bytes = offset + PAGE_FILE_PAGE_BYTES;
    page_cache_put(cache_of(file), page_id, (uint16_t)kind, table_id, body);
    return PAGE_FILE_OK;
}

page_file_status_t page_file_page_info(page_file_t *file, uint32_t page_id,
                                       page_file_page_info_t *info)
{
    if (file == NULL || info == NULL || !data_page_in_range(file, page_id)) {
        return PAGE_FILE_INVALID;
    }
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    envelope_t envelope;
    uint16_t cached_kind;
    if (page_cache_get(cache_of(file), page_id, &cached_kind, &envelope.table_id, page + PAGE_ENVELOPE_BYTES)) {
        envelope.kind = (page_kind_t)cached_kind;
    } else {
        page_file_status_t status = read_verified(file, page_id, page, &envelope);
        if (status != PAGE_FILE_OK) return status;
    }
    if (envelope.kind != PAGE_KIND_RECORD && envelope.kind != PAGE_KIND_OVERFLOW &&
        envelope.kind != PAGE_KIND_DIRECTORY && envelope.kind != PAGE_KIND_INDEX) {
        return PAGE_FILE_UNSUPPORTED;
    }
    info->kind = envelope.kind;
    info->table_id = envelope.table_id;
    return PAGE_FILE_OK;
}

/* ── 할당·해제 ── */

static bool list_grow(id_list_t *list, size_t needed)
{
    if (needed <= list->cap) return true;
    size_t next = list->cap == 0 ? 8 : list->cap;
    while (next < needed) next *= 2;
    uint32_t *bigger = realloc(list->items, next * sizeof(uint32_t));
    if (bigger == NULL) return false;
    list->items = bigger;
    list->cap = next;
    return true;
}

/* 공간은 ensure_capacity가 미리 잡는다 */
static void list_push(id_list_t *list, uint32_t id) { list->items[list->len++] = id; }

/*
 * 페이지 상태 배열과 목록들이 페이지 n장을 담을 공간을 미리 잡는다. 서로 다른 페이지만 담으므로 어느 목록도 n을
 * 넘지 않고, 그래서 release와 commit 성공 뒤의 이동은 할당하지 않는다.
 */
static bool ensure_capacity(page_file_t *file, size_t n)
{
    if (n > file->state_cap) {
        size_t next = file->state_cap == 0 ? 8 : file->state_cap;
        while (next < n) next *= 2;
        uint8_t *bigger = realloc(file->state, next);
        if (bigger == NULL) return false;
        memset(bigger + file->state_cap, PS_USED, next - file->state_cap);
        file->state = bigger;
        file->state_cap = next;
    }
    return list_grow(&file->pending, n) && list_grow(&file->aging, n) && list_grow(&file->reusable, n) &&
           list_grow(&file->fresh, n);
}

page_file_status_t page_file_alloc(page_file_t *file, uint32_t *page_id)
{
    if (file == NULL || page_id == NULL || !file->writable) return PAGE_FILE_INVALID;
    if (file->poisoned) {
        errno = EIO;
        return PAGE_FILE_IO;
    }
    if (file->reusable.len > 0) {
        uint32_t id = file->reusable.items[--file->reusable.len];
        if (file->state[id] != PS_DISCARDED) list_push(&file->fresh, id);  /* 버린 새 페이지는 이미 목록에 있다 */
        file->state[id] = PS_FRESH;
        *page_id = id;
        return PAGE_FILE_OK;
    }
    if (file->page_count >= PAGE_FILE_MAX_PAGES) return PAGE_FILE_LIMIT;
    if (!ensure_capacity(file, (size_t)file->page_count + 1)) return PAGE_FILE_NOMEM;
    uint32_t id = file->page_count++;
    file->state[id] = PS_FRESH;
    list_push(&file->fresh, id);
    *page_id = id;
    return PAGE_FILE_OK;
}

page_file_status_t page_file_release(page_file_t *file, uint32_t page_id)
{
    if (file == NULL || !file->writable || !data_page_in_range(file, page_id)) {
        return PAGE_FILE_INVALID;
    }
    switch ((page_state_t)file->state[page_id]) {
        case PS_USED:
            file->state[page_id] = PS_PENDING;
            list_push(&file->pending, page_id);
            return PAGE_FILE_OK;
        case PS_FRESH:
            /* 어느 제어 페이지도 가리킨 적 없어 곧바로 다시 쓸 수 있다 */
            file->state[page_id] = PS_DISCARDED;
            list_push(&file->reusable, page_id);
            return PAGE_FILE_OK;
        default:
            return PAGE_FILE_INVALID; /* 이중 해제 */
    }
}

bool page_file_is_fresh(const page_file_t *file, uint32_t page_id)
{
    return file != NULL && file->writable && data_page_in_range(file, page_id) && file->state[page_id] == PS_FRESH;
}

void page_file_restore_pending(page_file_t *file)
{
    if (file == NULL || !file->writable) return;
    for (size_t i = 0; i < file->pending.len; i++) file->state[file->pending.items[i]] = PS_USED;
    file->pending.len = 0;
}

void page_file_release_fresh(page_file_t *file)
{
    if (file == NULL || !file->writable) return;
    for (size_t i = 0; i < file->fresh.len; i++) {
        uint32_t id = file->fresh.items[i];
        if (file->state[id] != PS_FRESH) continue;
        file->state[id] = PS_DISCARDED;
        list_push(&file->reusable, id);
    }
}

size_t page_file_pending_release_count(const page_file_t *file)
{
    return file != NULL ? file->pending.len : 0;
}

size_t page_file_aging_release_count(const page_file_t *file)
{
    return file != NULL ? file->aging.len : 0;
}

size_t page_file_reusable_count(const page_file_t *file)
{
    return file != NULL ? file->reusable.len : 0;
}

/*
 * 열 때 제어 페이지와 사슬에서 빈 목록을 읽는다. 번호마다 범위와 중복을 확인한다(사슬 페이지는 읽는 동안
 * PS_FRESH로 표시해 빈 목록과 겹치면 거절하고 끝나면 PS_USED로 돌린다).
 */
static page_file_status_t load_free_list(page_file_t *file, const uint8_t *control_page, const free_header_t *header)
{
    if (!ensure_capacity(file, file->page_count) || !list_grow(&file->chain, header->chain_count)) {
        return PAGE_FILE_NOMEM;
    }
    uint64_t total = (uint64_t)header->reusable + header->aging;
    uint64_t at = 0;
    const uint8_t *ids = control_page + PAGE_ENVELOPE_BYTES + CONTROL_HEADER_BYTES;
    uint32_t inline_count = inline_ids(total);
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    uint32_t next = header->chain_head;
    page_file_status_t status = PAGE_FILE_OK;
    for (uint32_t chain_index = 0; status == PAGE_FILE_OK && at < total; chain_index++) {
        uint32_t count = inline_count;
        if (chain_index > 0) {
            envelope_t envelope;
            if (chain_index > header->chain_count || !data_page_in_range(file, next) ||
                file->state[next] != PS_USED) {
                status = PAGE_FILE_CORRUPT;
                break;
            }
            status = read_full(file, page, PAGE_FILE_PAGE_BYTES, (uint64_t)next * PAGE_FILE_PAGE_BYTES);
            if (status != PAGE_FILE_OK) break;
            if (decode_envelope(page, next, &envelope) != SLOT_VALID || envelope.kind != PAGE_KIND_FREE_LIST ||
                envelope.table_id != 0 || envelope.body_len != PAGE_BODY_BYTES) {
                status = PAGE_FILE_CORRUPT;
                break;
            }
            file->state[next] = PS_FRESH;
            list_push(&file->chain, next);
            const uint8_t *body = page + PAGE_ENVELOPE_BYTES;
            next = get_u32(body);
            count = get_u32(body + 4);
            ids = body + CHAIN_HEADER_BYTES;
            if (count == 0 || count > CHAIN_IDS || count > total - at) {
                status = PAGE_FILE_CORRUPT;
                break;
            }
        }
        for (uint32_t i = 0; i < count; i++, at++) {
            uint32_t id = get_u32(ids + 4u * i);
            if (!data_page_in_range(file, id) || file->state[id] != PS_USED) {
                status = PAGE_FILE_CORRUPT;
                break;
            }
            bool reusable = at < header->reusable;
            file->state[id] = reusable ? PS_REUSABLE : PS_AGING;
            list_push(reusable ? &file->reusable : &file->aging, id);
        }
    }
    if (status == PAGE_FILE_OK && (file->chain.len != header->chain_count || next != PAGE_FILE_NO_PAGE)) {
        status = PAGE_FILE_CORRUPT;
    }
    for (size_t i = 0; i < file->chain.len; i++) file->state[file->chain.items[i]] = PS_USED;
    if (status == PAGE_FILE_CORRUPT) errno = EBADMSG;
    return status;
}

/* ── commit ── */

page_file_status_t page_file_stage_catalog(page_file_t *file, uint32_t head, uint32_t size,
                                           uint64_t catalog_generation)
{
    if (file == NULL || !file->writable) return PAGE_FILE_INVALID;
    if ((head == PAGE_FILE_NO_PAGE) != (size == 0)) return PAGE_FILE_INVALID;
    if (head != PAGE_FILE_NO_PAGE && !data_page_in_range(file, head)) return PAGE_FILE_INVALID;
    file->staged_head = head;
    file->staged_size = size;
    file->staged_generation = catalog_generation;
    return PAGE_FILE_OK;
}

/* commit 뒤의 빈 목록을 차례로 읽는다: 재사용(지금 재사용 + 지금 대기) 다음 대기(지금 보류 + 옛 사슬) */
typedef struct {
    const id_list_t *parts[4];
    int part;
    size_t index;
} free_cursor_t;

static uint32_t cursor_next(free_cursor_t *cursor)
{
    while (cursor->index >= cursor->parts[cursor->part]->len) {
        cursor->part++;
        cursor->index = 0;
    }
    return cursor->parts[cursor->part]->items[cursor->index++];
}

static void put_ids(uint8_t *at, free_cursor_t *cursor, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) put_u32(at + 4u * i, cursor_next(cursor));
}

/* commit이 받은 사슬 페이지를 돌려준다(새 페이지라 곧바로 재사용 목록으로 간다) */
static void undo_chain(page_file_t *file, const id_list_t *chain)
{
    for (size_t i = chain->len; i > 0; i--) page_file_release(file, chain->items[i - 1]);
}

page_file_status_t page_file_commit(page_file_t *file)
{
    if (file == NULL || !file->writable) return PAGE_FILE_INVALID;
    if (file->poisoned) {
        /* 거절한 호출이 poison을 일으킨 commit의 결과를 덮어쓰지 않는다. */
        errno = EIO;
        return PAGE_FILE_IO;
    }
    file->commit_outcome = PAGE_FILE_COMMIT_NOT_APPLIED;
    if (file->committed.generation >= UINT64_MAX - 1) return PAGE_FILE_LIMIT;

    /*
     * 빈 목록이 제어 페이지에 다 들어가지 않으면 사슬 페이지를 받는다. 재사용 목록에서 받으면 적을 번호도 하나 준다.
     * 받은 사슬 페이지는 새 페이지라 실패하면 그대로 돌려준다.
     */
    id_list_t next_chain = {NULL, 0, 0};
    for (;;) {
        uint64_t total = (uint64_t)file->reusable.len + file->aging.len + file->pending.len + file->chain.len;
        if (next_chain.len >= chain_pages_for(total)) break;
        uint32_t id;
        page_file_status_t taken = list_grow(&next_chain, next_chain.len + 1) ? page_file_alloc(file, &id)
                                                                               : PAGE_FILE_NOMEM;
        if (taken != PAGE_FILE_OK) {
            undo_chain(file, &next_chain);
            free(next_chain.items);
            return taken;
        }
        list_push(&next_chain, id);
    }
    free_header_t header = {(uint32_t)(file->reusable.len + file->aging.len),
                            (uint32_t)(file->pending.len + file->chain.len), PAGE_FILE_NO_PAGE,
                            (uint32_t)next_chain.len};
    if (next_chain.len > 0) header.chain_head = next_chain.items[0];
    free_cursor_t cursor = {{&file->reusable, &file->aging, &file->pending, &file->chain}, 0, 0};
    uint64_t total = (uint64_t)header.reusable + header.aging;

    page_file_control_t next = {
        .generation = file->committed.generation + 1,
        .page_count = file->page_count,
        .catalog_head = file->staged_head,
        .catalog_size = file->staged_size,
        .catalog_generation = file->staged_generation,
    };
    uint8_t body[PAGE_BODY_BYTES];
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    uint32_t inline_count = inline_ids(total);
    encode_control_header(body, &next, &header);
    put_ids(body + CONTROL_HEADER_BYTES, &cursor, inline_count);
    uint32_t control_bytes = CONTROL_HEADER_BYTES + 4u * inline_count;
    uint32_t slot = (uint32_t)(next.generation % 2);

    page_file_status_t status = PAGE_FILE_OK;
    uint64_t left = total - inline_count;
    uint8_t chain_body[PAGE_BODY_BYTES];
    for (size_t i = 0; i < next_chain.len && status == PAGE_FILE_OK; i++) {
        uint32_t count = left < CHAIN_IDS ? (uint32_t)left : CHAIN_IDS;
        memset(chain_body, 0, sizeof(chain_body));
        put_u32(chain_body, i + 1 < next_chain.len ? next_chain.items[i + 1] : PAGE_FILE_NO_PAGE);
        put_u32(chain_body + 4, count);
        put_ids(chain_body + CHAIN_HEADER_BYTES, &cursor, count);
        left -= count;
        uint32_t id = next_chain.items[i];
        encode_page(page, PAGE_KIND_FREE_LIST, id, 0, PAGE_BODY_BYTES, chain_body);
        page_cache_drop(file->cache, id);
        status = write_full(file, page, sizeof(page), (uint64_t)id * PAGE_FILE_PAGE_BYTES);
        uint64_t end = ((uint64_t)id + 1) * PAGE_FILE_PAGE_BYTES;
        if (status == PAGE_FILE_OK && end > file->file_bytes) file->file_bytes = end;
    }

    uint64_t needed = (uint64_t)file->page_count * PAGE_FILE_PAGE_BYTES;
    if (status == PAGE_FILE_OK && needed > file->file_bytes) {
        /* 할당만 하고 쓰지 않은 페이지도 파일 길이 안에 두어 다시 열 때 잘린 파일로 보이지 않게 한다. */
        if (ftruncate(file->fd, (off_t)needed) != 0) status = PAGE_FILE_IO;
        else file->file_bytes = needed;
    }
    if (status == PAGE_FILE_OK) status = sync_file(file);
    if (status != PAGE_FILE_OK) {
        int err = errno;
        if (!file->poisoned) undo_chain(file, &next_chain);
        free(next_chain.items);
        errno = err;
        return status;
    }

    encode_page(page, PAGE_KIND_CONTROL, slot, 0, control_bytes, body);
    /* 여기서부터는 실패해도 디스크에 새 세대가 남았는지 알 수 없다. */
    file->commit_outcome = PAGE_FILE_COMMIT_UNKNOWN;
    status = write_full(file, page, sizeof(page), (uint64_t)slot * PAGE_FILE_PAGE_BYTES);
    if (status != PAGE_FILE_OK) file->poisoned = true;
    if (status == PAGE_FILE_OK) status = sync_file(file);
    if (status != PAGE_FILE_OK) {
        free(next_chain.items);
        return status;
    }

    file->commit_outcome = PAGE_FILE_COMMIT_APPLIED;
    file->committed = next;
    /*
     * 새 페이지는 이제 게시된 상태의 일부다. 이전 commit이 놓은 페이지는 방금 commit으로 두 제어 슬롯(현재와
     * fallback) 모두 더는 가리키지 않으므로 재사용할 수 있다. 이번에 놓은 페이지와 옛 사슬은 fallback 세대가
     * 아직 가리킬 수 있어 한 번 더 commit한 뒤에야 재사용한다. 목록 순서는 방금 쓴 빈 목록과 같다.
     */
    for (size_t i = 0; i < file->fresh.len; i++) {
        uint32_t id = file->fresh.items[i];
        file->state[id] = file->state[id] == PS_FRESH ? PS_USED : PS_REUSABLE;
    }
    file->fresh.len = 0;
    for (size_t i = 0; i < file->aging.len; i++) {
        file->state[file->aging.items[i]] = PS_REUSABLE;
        list_push(&file->reusable, file->aging.items[i]);
    }
    file->aging.len = 0;
    for (size_t i = 0; i < file->pending.len; i++) {
        file->state[file->pending.items[i]] = PS_AGING;
        list_push(&file->aging, file->pending.items[i]);
    }
    file->pending.len = 0;
    for (size_t i = 0; i < file->chain.len; i++) {
        file->state[file->chain.items[i]] = PS_AGING;
        list_push(&file->aging, file->chain.items[i]);
    }
    free(file->chain.items);
    file->chain = next_chain;
    return PAGE_FILE_OK;
}

page_file_commit_outcome_t page_file_last_commit_outcome(const page_file_t *file)
{
    return file != NULL ? file->commit_outcome : PAGE_FILE_COMMIT_NONE;
}

bool page_file_is_poisoned(const page_file_t *file)
{
    return file != NULL && file->poisoned;
}

/* ── overflow 어댑터 ── */

static bool adapter_allocate(void *context, uint32_t *page_id)
{
    page_file_overflow_ctx_t *ctx = context;
    ctx->last_status = page_file_alloc(ctx->file, page_id);
    return ctx->last_status == PAGE_FILE_OK;
}

static bool adapter_release(void *context, uint32_t page_id)
{
    page_file_overflow_ctx_t *ctx = context;
    ctx->last_status = page_file_release(ctx->file, page_id);
    return ctx->last_status == PAGE_FILE_OK;
}

static bool adapter_read(void *context, uint32_t page_id, uint8_t *page)
{
    page_file_overflow_ctx_t *ctx = context;
    ctx->last_status = page_file_read(ctx->file, page_id, PAGE_KIND_OVERFLOW, ctx->table_id, page);
    return ctx->last_status == PAGE_FILE_OK;
}

static bool adapter_write(void *context, uint32_t page_id, const uint8_t *page)
{
    page_file_overflow_ctx_t *ctx = context;
    ctx->last_status = page_file_write(ctx->file, page_id, PAGE_KIND_OVERFLOW, ctx->table_id, page);
    return ctx->last_status == PAGE_FILE_OK;
}

overflow_io_t page_file_overflow_io(page_file_overflow_ctx_t *ctx, page_file_t *file,
                                    uint32_t table_id)
{
    ctx->file = file;
    ctx->table_id = table_id;
    ctx->last_status = PAGE_FILE_OK;
    return (overflow_io_t){ctx, PAGE_BODY_BYTES, adapter_allocate, adapter_release,
                           adapter_read, adapter_write};
}

size_t page_file_handle_bytes(const page_file_t *file)
{
    if (file == NULL) return 0;
    size_t total = sizeof(*file) + page_cache_bytes(file->cache_pages);
    if (file->path != NULL) total += strlen(file->path) + 1;
    if (file->partial_path != NULL) total += strlen(file->partial_path) + 1;
    return total;
}

size_t page_file_list_peak_bytes(uint32_t page_count)
{
    size_t cap = 8;
    while (cap < page_count) cap *= 2;
    /* 상태 배열과 목록 다섯이 각각 cap. 늘 때는 하나씩 옛 배열(cap/2)이 함께 있다 */
    return cap + cap / 2 + 5 * cap * sizeof(uint32_t) + cap / 2 * sizeof(uint32_t);
}

size_t page_file_open_peak_bytes(const char *path)
{
    size_t length = path != NULL ? strlen(path) : 0;
    /* 핸들, create의 partial·final·directory 사본(접미사 포함)과 open의 path 사본 */
    return sizeof(page_file_t) + page_cache_bytes(g_default_cache_pages) + 4 * (length + sizeof(PARTIAL_SUFFIX) + 1);
}

page_file_cache_stats_t page_file_cache_stats(const page_file_t *file)
{
    page_cache_stats_t stats = page_cache_stats(file != NULL ? file->cache : NULL);
    return (page_file_cache_stats_t){stats.hits, stats.misses, stats.evictions};
}
