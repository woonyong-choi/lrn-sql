#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "storage/page_file.h"
#include "storage/le.h"

#include "page_cache.h"
#include "storage/checksum.h"
#include "storage/record_page.h"

_Static_assert(RECORD_PAGE_BYTES == PAGE_BODY_BYTES,
               "record page body must equal the page_file body");

#define EINTR_RETRY_MAX 100
#define CONTROL_HEADER_BYTES 72u
#define MANIFEST_MAX 256u
#define MANIFEST_BYTES (MANIFEST_MAX * 8u)
#define CONTROL_INLINE_IDS ((PAGE_BODY_BYTES - CONTROL_HEADER_BYTES - MANIFEST_BYTES) / 4u)
#define CHAIN_HEADER_BYTES 8u
#define CHAIN_IDS ((PAGE_BODY_BYTES - CHAIN_HEADER_BYTES) / 4u)
#define BYTE_ORDER_MARK 0x01020304u
#define ENVELOPE_VERSION 2u   /* 2: 체크섬 CRC32C */
#define PARTIAL_SUFFIX ".partial"
#define WRITER_LOCK_SUFFIX "-lock"

/*
 * 제어 본문 배치: magic 8, 형식 u32, 페이지 크기 u32, byte order u32, 세대 u64, page_count u32, 예약 u32(0),
 * 카탈로그 head u32, size u32, 카탈로그 세대 u64, 재사용 수 u32, 대기 수 u32, 사슬 head u32, 사슬 길이 u32,
 * manifest 수 u32, 그 뒤에 manifest(페이지 번호 u32, 페이지 체크섬 u32)를 MANIFEST_MAX개까지, 그 뒤에 빈 페이지
 * 번호(재사용 다음 대기 순서)를 CONTROL_INLINE_IDS개까지. 나머지 번호는 사슬 페이지에 이어 쓴다.
 * 사슬 본문: 다음 사슬 u32, 이 페이지의 번호 수 u32, 번호들.
 *
 * commit 순서. manifest가 있으면(이 commit이 쓴 새 페이지가 MANIFEST_MAX장 이하이고 게시된 페이지를 덮어쓰지
 * 않았으면) 데이터 쓰기 → 제어 쓰기 → fsync 한 번이다. 끝나지 않은 commit은 열 때 최신 제어 페이지의 manifest와
 * 실제 페이지, 파일 길이를 대조해 알아보고 이전 세대로 연다. manifest가 없으면 데이터 fsync → 제어 쓰기 → fsync다.
 * 어느 쪽이든 새 페이지는 두 제어 슬롯이 가리키지 않는 자리에만 쓰므로 이전 세대는 그대로다.
 */
static const uint8_t FILE_MAGIC[8] = {'T', 'O', 'T', 'E', 'D', 'B', 0, 0};

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
    uint32_t manifest;
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
    uint32_t manifest_ids[MANIFEST_MAX];   /* 마지막 commit 뒤에 쓴 새 페이지와 그 체크섬 */
    uint32_t manifest_sums[MANIFEST_MAX];
    uint32_t manifest_len;
    bool needs_barrier;     /* manifest가 넘쳤거나 새 페이지가 아닌 페이지를 썼다. 다음 commit은 fsync 두 번 */
    size_t cache_pages;     /* 이 핸들의 캐시 상한. 계상은 이 값으로 한다 */
    page_cache_t *cache;    /* 처음 쓰일 때 만든다. 만들지 못하면 NULL로 두고 캐시 없이 동작한다 */
    bool cache_tried;
    pid_t owner;            /* 연 프로세스. fork한 자식은 이 핸들을 쓰지 않는다 */
    page_file_lock_t lock;  /* 지금 쥔 잠금 */
    uint32_t lock_timeout_ms;     /* writer 잠금과 열기의 대기 상한 */
    uint32_t publish_timeout_ms;  /* 게시 순간의 배타 잠금과 reader 공유 잠금의 대기 상한 */
    int writer_fd;          /* writer 잠금 파일(<path>-lock). 쓰기 핸들만, 없으면 -1 */
    int gate_fd;            /* 읽기 핸들이 게시 대기 표시를 보는 잠금 파일 fd. 없으면 -1(쓰기 핸들은 writer_fd를 쓴다) */
    page_file_t *next_open; /* 열린 핸들 목록(fork한 자식에서 fd 사본을 닫는다) */
    page_file_t *prev_open;
    bool stale;             /* 버린 변경이 남았다. 다음 잠금에서 디스크의 게시 상태로 다시 읽는다 */
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
static uint32_t g_default_lock_timeout_ms = PAGE_FILE_LOCK_TIMEOUT_MS;
static uint32_t g_default_publish_timeout_ms = PAGE_FILE_PUBLISH_TIMEOUT_MS;

void page_file_set_default_cache_pages(size_t pages) { g_default_cache_pages = pages; }

void page_file_set_default_lock_timeout(uint32_t timeout_ms) { g_default_lock_timeout_ms = timeout_ms; }

void page_file_set_default_publish_timeout(uint32_t timeout_ms) { g_default_publish_timeout_ms = timeout_ms; }

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

/* 체크섬 필드(28..31)를 뺀 페이지 전체의 CRC32C. 패딩 손상도 잡는다. */
static uint32_t page_checksum(const uint8_t *page)
{
    return crc32c(crc32c(0, page, 28), page + 32, PAGE_FILE_PAGE_BYTES - 32);
}

static void encode_page(uint8_t *page, page_kind_t kind, uint32_t page_id, uint32_t table_id,
                        uint32_t body_len, const uint8_t *body)
{
    memset(page, 0, PAGE_FILE_PAGE_BYTES);
    memcpy(page, "LRPG", 4);
    put_u16(page + 4, ENVELOPE_VERSION);
    put_u16(page + 6, (uint16_t)kind);
    le_put_u32(page + 8, page_id);
    le_put_u32(page + 12, table_id);
    le_put_u32(page + 16, body_len);
    memcpy(page + PAGE_ENVELOPE_BYTES, body, body_len);
    le_put_u32(page + 28, page_checksum(page));
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

static const uint8_t ZERO_PAGE[PAGE_FILE_PAGE_BYTES];

/* 0 영역 검사. 바이트 단위 반복 대신 벡터화된 memcmp를 쓴다 */
static bool all_zero(const uint8_t *bytes, size_t size) { return memcmp(bytes, ZERO_PAGE, size) == 0; }

/* envelope만 검사한다. 손상이면 SLOT_DAMAGED, 알 수 없는 envelope 버전이면 SLOT_UNSUPPORTED. */
static slot_state_t decode_envelope(const uint8_t *page, uint32_t expect_id, envelope_t *out)
{
    if (memcmp(page, "LRPG", 4) != 0) return SLOT_DAMAGED;
    if (le_get_u32(page + 28) != page_checksum(page)) return SLOT_DAMAGED;
    if (le_get_u16(page + 4) != ENVELOPE_VERSION) return SLOT_UNSUPPORTED;
    uint32_t body_len = le_get_u32(page + 16);
    if (le_get_u32(page + 8) != expect_id || body_len > PAGE_BODY_BYTES) return SLOT_DAMAGED;
    if (!all_zero(page + 20, 8) || !all_zero(page + PAGE_ENVELOPE_BYTES + body_len,
                                             PAGE_FILE_PAGE_BYTES - PAGE_ENVELOPE_BYTES - body_len)) {
        return SLOT_DAMAGED;
    }
    out->kind = (page_kind_t)le_get_u16(page + 6);
    out->table_id = le_get_u32(page + 12);
    out->body_len = body_len;
    return SLOT_VALID;
}

static bool is_blank(const uint8_t *page) { return all_zero(page, PAGE_FILE_PAGE_BYTES); }

static uint32_t inline_ids(uint64_t total)
{
    return total < CONTROL_INLINE_IDS ? (uint32_t)total : CONTROL_INLINE_IDS;
}

static uint32_t chain_pages_for(uint64_t total)
{
    if (total <= CONTROL_INLINE_IDS) return 0;
    return (uint32_t)((total - CONTROL_INLINE_IDS + CHAIN_IDS - 1) / CHAIN_IDS);
}

/* 제어 페이지 본문 앞에 ToteDB 파일 magic이 있는지(체크섬과 무관하게) */
static bool has_file_magic(const uint8_t *page)
{
    return memcmp(page + PAGE_ENVELOPE_BYTES, FILE_MAGIC, sizeof(FILE_MAGIC)) == 0;
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
    if (le_get_u32(body + 8) != PAGE_FILE_FORMAT_VERSION) return SLOT_UNSUPPORTED;
    if (envelope.body_len < CONTROL_HEADER_BYTES) return SLOT_DAMAGED;
    if (le_get_u32(body + 12) != PAGE_FILE_PAGE_BYTES) return SLOT_UNSUPPORTED;
    if (le_get_u32(body + 16) != BYTE_ORDER_MARK) return SLOT_UNSUPPORTED;
    page_file_control_t control = {
        .generation = le_get_u64(body + 20),
        .page_count = le_get_u32(body + 28),
        .catalog_head = le_get_u32(body + 36),
        .catalog_size = le_get_u32(body + 40),
        .catalog_generation = le_get_u64(body + 44),
    };
    if (le_get_u32(body + 32) != 0) return SLOT_DAMAGED;
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
    free_header_t free_list = {le_get_u32(body + 52), le_get_u32(body + 56), le_get_u32(body + 60), le_get_u32(body + 64),
                               le_get_u32(body + 68)};
    uint64_t total = (uint64_t)free_list.reusable + free_list.aging;
    if (total > control.page_count - PAGE_FILE_CONTROL_PAGES || free_list.manifest > MANIFEST_MAX ||
        envelope.body_len != CONTROL_HEADER_BYTES + 8u * free_list.manifest + 4u * inline_ids(total) ||
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
    le_put_u32(body + 8, PAGE_FILE_FORMAT_VERSION);
    le_put_u32(body + 12, PAGE_FILE_PAGE_BYTES);
    le_put_u32(body + 16, BYTE_ORDER_MARK);
    le_put_u64(body + 20, control->generation);
    le_put_u32(body + 28, control->page_count);
    le_put_u32(body + 36, control->catalog_head);
    le_put_u32(body + 40, control->catalog_size);
    le_put_u64(body + 44, control->catalog_generation);
    le_put_u32(body + 52, free_list->reusable);
    le_put_u32(body + 56, free_list->aging);
    le_put_u32(body + 60, free_list->chain_head);
    le_put_u32(body + 64, free_list->chain_count);
    le_put_u32(body + 68, free_list->manifest);
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

/*
 * 열린 핸들 목록. flock은 열린 파일 설명마다 걸리고 그 설명을 가리키는 fd가 모두 닫혀야 풀린다. fork한 자식이 fd 사본을
 * 쥐고 있으면 부모가 죽거나 닫아도 잠금이 남는다. 그래서 fork 직후 자식에서 모든 핸들의 fd 사본을 닫는다. 자식의
 * 핸들은 어차피 PAGE_FILE_FORKED로 쓰지 못한다. 사본을 닫아도 부모의 잠금은 그대로다.
 */
static pthread_mutex_t g_open_lock = PTHREAD_MUTEX_INITIALIZER;
static page_file_t *g_open_handles;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

static void before_fork(void) { pthread_mutex_lock(&g_open_lock); }

static void after_fork_parent(void) { pthread_mutex_unlock(&g_open_lock); }

static void after_fork_child(void)
{
    for (page_file_t *file = g_open_handles; file != NULL; file = file->next_open) {
        if (file->fd >= 0) close(file->fd);
        if (file->writer_fd >= 0) close(file->writer_fd);
        if (file->gate_fd >= 0) close(file->gate_fd);
        file->fd = -1;
        file->writer_fd = -1;
        file->gate_fd = -1;
    }
    pthread_mutex_init(&g_open_lock, NULL);
}

static void install_fork_handler(void) { pthread_atfork(before_fork, after_fork_parent, after_fork_child); }

static void remember_handle(page_file_t *file)
{
    pthread_once(&g_atfork_once, install_fork_handler);
    pthread_mutex_lock(&g_open_lock);
    file->next_open = g_open_handles;
    if (g_open_handles != NULL) g_open_handles->prev_open = file;
    g_open_handles = file;
    pthread_mutex_unlock(&g_open_lock);
}

static void forget_handle(page_file_t *file)
{
    pthread_mutex_lock(&g_open_lock);
    if (file->prev_open != NULL) file->prev_open->next_open = file->next_open;
    else if (g_open_handles == file) g_open_handles = file->next_open;
    if (file->next_open != NULL) file->next_open->prev_open = file->prev_open;
    pthread_mutex_unlock(&g_open_lock);
}

static page_file_t *new_handle(int fd, bool writable)
{
    page_file_t *file = calloc(1, sizeof(*file));
    if (file == NULL) return NULL;
    file->fd = fd;
    file->writable = writable;
    file->io = g_default_io;
    file->cache_pages = g_default_cache_pages;
    file->owner = getpid();
    file->lock_timeout_ms = g_default_lock_timeout_ms;
    file->publish_timeout_ms = g_default_publish_timeout_ms;
    file->writer_fd = -1;
    file->gate_fd = -1;
    remember_handle(file);
    return file;
}

static void free_handle(page_file_t *file)
{
    forget_handle(file);
    if (file->writer_fd >= 0) close(file->writer_fd);
    if (file->gate_fd >= 0) close(file->gate_fd);
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

static uint64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* 바쁘면 100µs에서 2ms까지 늘려 쉬며 다시 시도한다. 대기 상한을 넘으면 LOCKED다. operation은 LOCK_SH·LOCK_EX */
static page_file_status_t lock_fd(int fd, int operation, uint32_t timeout_ms)
{
    uint64_t deadline = monotonic_us() + (uint64_t)timeout_ms * 1000u;
    long pause_us = 100;
    for (;;) {
        if (flock(fd, operation | LOCK_NB) == 0) return PAGE_FILE_OK;
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK) return PAGE_FILE_IO;
        if (monotonic_us() >= deadline) {
            errno = EWOULDBLOCK;
            return PAGE_FILE_LOCKED;
        }
        struct timespec pause = {0, pause_us * 1000};
        nanosleep(&pause, NULL);
        if (pause_us < 2000) pause_us *= 2;
    }
}

/*
 * writer 잠금 파일(<db_path>-lock)을 열어 배타 잠금을 잡는다. 쓰기 거래를 파일 전체에서 하나로 줄인다(#200). 잠근 뒤 그
 * 경로가 여전히 같은 파일인지 확인하고, 누가 지우거나 바꿨으면 새로 열어 다시 잡는다. 두 writer가 서로 다른 잠금 파일을
 * 잡지 않게 하기 위해서다. *writer_fd가 -1이면 연다. 실패하면 잠금을 쥐지 않는다(fd는 남겨 다음에 쓴다).
 */
static page_file_status_t lock_writer(const char *db_path, int *writer_fd, uint32_t timeout_ms)
{
    size_t length = strlen(db_path);
    char lock_path[PATH_MAX];
    if (length + sizeof(WRITER_LOCK_SUFFIX) > sizeof(lock_path)) return PAGE_FILE_INVALID;
    memcpy(lock_path, db_path, length);
    memcpy(lock_path + length, WRITER_LOCK_SUFFIX, sizeof(WRITER_LOCK_SUFFIX));
    for (;;) {
        if (*writer_fd < 0) {
            *writer_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (*writer_fd < 0) return PAGE_FILE_IO;
        }
        page_file_status_t status = lock_fd(*writer_fd, LOCK_EX, timeout_ms);
        if (status != PAGE_FILE_OK) return status;
        struct stat held, named;
        if (fstat(*writer_fd, &held) == 0 && stat(lock_path, &named) == 0 && held.st_dev == named.st_dev &&
            held.st_ino == named.st_ino) {
            return PAGE_FILE_OK;
        }
        close(*writer_fd);
        *writer_fd = -1;
    }
}

/*
 * 게시 대기 표시(writer 우선). writer는 게시 잠금을 기다리기 전에 잠금 파일 첫 바이트를 1로, 게시가 끝나면 0으로 쓴다.
 * reader는 공유 잠금을 잡기 전에 표시를 보고 1이면 기다린다. 쉬지 않고 읽는 reader들 사이에서 게시할 틈이 없어 writer가
 * 굶지 않게 하기 위해서다. 표시를 남긴 writer가 죽었으면 writer 잠금이 비어 있으므로 그 표시는 무시한다.
 */
static void set_publish_intent(const page_file_t *file, bool pending)
{
    uint8_t mark = pending ? 1 : 0;
    if (file->writer_fd >= 0 && pwrite(file->writer_fd, &mark, 1, 0) != 1) {
        /* 표시는 공정성만 돕는다. 쓰지 못해도 게시 잠금이 정확성을 지킨다 */
    }
}

static int gate_of(page_file_t *file)
{
    if (file->writer_fd >= 0) return file->writer_fd;
    if (file->gate_fd < 0 && file->path != NULL) {
        char lock_path[PATH_MAX];
        if (snprintf(lock_path, sizeof(lock_path), "%s%s", file->path, WRITER_LOCK_SUFFIX) < (int)sizeof(lock_path)) {
            file->gate_fd = open(lock_path, O_RDONLY | O_CLOEXEC);
        }
    }
    return file->gate_fd;
}

/* 게시 대기 표시가 있으면 deadline까지 기다린다. 표시를 쓴 writer가 없으면(잠금이 비어 있으면) 곧바로 돌아간다 */
static void wait_for_publisher(page_file_t *file, uint32_t timeout_ms)
{
    int gate = gate_of(file);
    if (gate < 0) return;
    uint64_t deadline = monotonic_us() + (uint64_t)timeout_ms * 1000u;
    long pause_us = 50;
    for (;;) {
        uint8_t mark = 0;
        if (pread(gate, &mark, 1, 0) != 1 || mark == 0) return;
        if (gate != file->writer_fd && flock(gate, LOCK_SH | LOCK_NB) == 0) {
            flock(gate, LOCK_UN);  /* writer 잠금이 비어 있다. 죽은 writer가 남긴 표시다 */
            return;
        }
        if (monotonic_us() >= deadline) return;
        struct timespec pause = {0, pause_us * 1000};
        nanosleep(&pause, NULL);
        if (pause_us < 1000) pause_us *= 2;
    }
}

static bool forked(const page_file_t *file) { return file->owner != getpid(); }

/* 연산 앞 공통 검사. 연 프로세스이고, 읽기는 잠금, 변경은 배타 잠금을 쥐어야 한다 */
static page_file_status_t check_access(const page_file_t *file, page_file_lock_t needed)
{
    if (forked(file)) return PAGE_FILE_FORKED;
    if (needed == PAGE_FILE_LOCK_EXCLUSIVE && (!file->writable || file->lock != PAGE_FILE_LOCK_EXCLUSIVE)) {
        return PAGE_FILE_INVALID;
    }
    if (needed == PAGE_FILE_LOCK_SHARED && file->lock == PAGE_FILE_LOCK_NONE) return PAGE_FILE_INVALID;
    return PAGE_FILE_OK;
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
    int fd = open(partial, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
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
    /* 게시 전 파일은 이 핸들만 안다. writer 잠금은 게시 직전에 잡는다(page_file_publish) */
    file->lock = PAGE_FILE_LOCK_EXCLUSIVE;
    page_file_status_t status = PAGE_FILE_OK;
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
    if (forked(file)) return PAGE_FILE_FORKED;
    page_file_status_t status = sync_file(file);
    if (status != PAGE_FILE_OK) return status;
    /*
     * 이름이 보이는 순간부터 이 핸들이 writer다. 다른 writer가 그 사이 끼어들지 않게 link 전에 writer 잠금을 잡는다.
     * 게시에 실패하면 이번에 만든 잠금 파일은 지운다. 그 사이 다른 핸들이 열었어도 lock_writer가 경로를 다시 확인한다.
     */
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s%s", file->path, WRITER_LOCK_SUFFIX);
    bool made_lock = access(lock_path, F_OK) != 0;
    status = lock_writer(file->path, &file->writer_fd, file->lock_timeout_ms);
    if (status != PAGE_FILE_OK) return status;
    /* link는 목적지가 이미 있으면 실패한다. rename은 조용히 덮어쓰므로 쓰지 않는다. */
    if (file->io.link(file->partial_path, file->path) != 0) {
        int err = errno;
        if (made_lock) unlink(lock_path);
        close(file->writer_fd);
        file->writer_fd = -1;
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

/*
 * 고른 제어 페이지의 commit이 끝났는지 본다. 파일이 page_count만큼 길고 manifest의 페이지가 모두 적힌 체크섬과
 * 같아야 한다. 끝나지 않았으면 CORRUPT, 읽기 실패는 IO다.
 */
static page_file_status_t commit_complete(const page_file_t *file, const uint8_t *control_page,
                                          const page_file_control_t *control, const free_header_t *header,
                                          uint64_t file_bytes)
{
    if (file_bytes < (uint64_t)control->page_count * PAGE_FILE_PAGE_BYTES) return PAGE_FILE_CORRUPT;
    const uint8_t *entries = control_page + PAGE_ENVELOPE_BYTES + CONTROL_HEADER_BYTES;
    uint8_t page[PAGE_FILE_PAGE_BYTES];
    for (uint32_t i = 0; i < header->manifest; i++) {
        uint32_t id = le_get_u32(entries + 8u * i), sum = le_get_u32(entries + 8u * i + 4);
        if (id < PAGE_FILE_CONTROL_PAGES || id >= control->page_count) return PAGE_FILE_CORRUPT;
        page_file_status_t status = read_full(file, page, PAGE_FILE_PAGE_BYTES, (uint64_t)id * PAGE_FILE_PAGE_BYTES);
        if (status != PAGE_FILE_OK) return status;
        envelope_t envelope;
        if (le_get_u32(page + 28) != sum || decode_envelope(page, id, &envelope) != SLOT_VALID) return PAGE_FILE_CORRUPT;
    }
    return PAGE_FILE_OK;
}

static page_file_status_t load_free_list(page_file_t *file, const uint8_t *control_page, const free_header_t *header);

/* exclude가 0 또는 1이면 그 슬롯을 손상으로 본다(끝나지 않은 commit) */
static page_file_status_t pick_control(const uint8_t pages[2][PAGE_FILE_PAGE_BYTES], int exclude,
                                       page_file_control_t *chosen, free_header_t *chosen_free,
                                       page_file_open_info_t *info)
{
    page_file_control_t controls[2];
    free_header_t frees[2];
    slot_state_t states[2];
    for (uint32_t slot = 0; slot < 2; slot++) {
        states[slot] = decode_control(pages[slot], slot, &controls[slot], &frees[slot]);
    }
    if (exclude >= 0) states[exclude] = SLOT_DAMAGED;
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

/*
 * 두 제어 페이지를 읽어 세대가 큰 쪽을 고른다. 열기와 page_file_lock이 함께 쓴다. 그 commit이 끝났는지는 아직 보지
 * 않는다(verify_published). pages에는 두 제어 페이지, file_bytes에는 파일 길이가 남는다.
 */
static page_file_status_t read_controls(page_file_t *file, uint8_t pages[2][PAGE_FILE_PAGE_BYTES],
                                        page_file_control_t *chosen, free_header_t *chosen_free,
                                        page_file_open_info_t *info, uint64_t *file_bytes)
{
    struct stat st;
    page_file_status_t status = PAGE_FILE_OK;
    if (fstat(file->fd, &st) != 0) {
        status = PAGE_FILE_IO;
    } else if (st.st_size == 0) {
        status = PAGE_FILE_NO_CONTROL;
    } else if (st.st_size < (off_t)(PAGE_FILE_CONTROL_PAGES * PAGE_FILE_PAGE_BYTES)) {
        /* 제어 페이지 두 장보다 짧다. 읽은 앞부분이 0뿐이거나 파일 magic이 있으면 잘린 ToteDB 파일, 다른 내용이면 다른 형식이다 */
        memset(pages[0], 0, PAGE_FILE_PAGE_BYTES);
        size_t head = st.st_size < (off_t)PAGE_FILE_PAGE_BYTES ? (size_t)st.st_size : PAGE_FILE_PAGE_BYTES;
        status = read_full(file, pages[0], head, 0);
        if (status == PAGE_FILE_OK) {
            status = is_blank(pages[0]) || has_file_magic(pages[0]) ? PAGE_FILE_CORRUPT : PAGE_FILE_UNSUPPORTED;
        }
    }
    for (uint32_t slot = 0; status == PAGE_FILE_OK && slot < 2; slot++) {
        status = read_full(file, pages[slot], PAGE_FILE_PAGE_BYTES, (uint64_t)slot * PAGE_FILE_PAGE_BYTES);
    }
    /* 두 제어 페이지 어디에도 파일 magic이 없고 비어 있지도 않으면 ToteDB 파일이 아니다. 바꾸지 않고 거절한다 */
    if (status == PAGE_FILE_OK && !has_file_magic(pages[0]) && !has_file_magic(pages[1]) &&
        !(is_blank(pages[0]) && is_blank(pages[1]))) {
        status = PAGE_FILE_UNSUPPORTED;
    }
    if (status == PAGE_FILE_OK) status = pick_control(pages, -1, chosen, chosen_free, info);
    if (status == PAGE_FILE_OK) *file_bytes = (uint64_t)st.st_size;
    return status;
}

/* 고른 commit이 끝나지 않았으면(fsync 전에 멈춤) 이전 세대로 바꾼다. 이전 세대도 끝나지 않았으면 손상이다 */
static page_file_status_t verify_published(page_file_t *file, const uint8_t pages[2][PAGE_FILE_PAGE_BYTES],
                                           page_file_control_t *chosen, free_header_t *chosen_free,
                                           page_file_open_info_t *info, uint64_t file_bytes)
{
    int newest = (int)(chosen->generation % 2);
    page_file_status_t status = commit_complete(file, pages[newest], chosen, chosen_free, file_bytes);
    if (status != PAGE_FILE_CORRUPT) return status;
    status = pick_control(pages, newest, chosen, chosen_free, info);
    if (status == PAGE_FILE_NO_CONTROL) return PAGE_FILE_CORRUPT;
    if (status != PAGE_FILE_OK) return status;
    return commit_complete(file, pages[chosen->generation % 2], chosen, chosen_free, file_bytes);
}

static void adopt_published(page_file_t *file, const page_file_control_t *chosen, uint64_t file_bytes)
{
    file->committed = *chosen;
    file->page_count = chosen->page_count;
    file->file_bytes = file_bytes;
    file->staged_head = chosen->catalog_head;
    file->staged_size = chosen->catalog_size;
    file->staged_generation = chosen->catalog_generation;
}

/* 쓰기로 열기 전 확인: 열기와 같은 규칙으로 게시된 ToteDB 파일인지만 본다. 임시 핸들로 읽고 아무것도 바꾸지 않는다 */
static page_file_status_t check_published_file(int fd)
{
    page_file_t probe = {.fd = fd, .io = g_default_io, .writer_fd = -1};
    uint8_t pages[2][PAGE_FILE_PAGE_BYTES];
    page_file_control_t chosen;
    free_header_t chosen_free;
    page_file_open_info_t info;
    uint64_t file_bytes = 0;
    page_file_status_t status = read_controls(&probe, pages, &chosen, &chosen_free, &info, &file_bytes);
    if (status == PAGE_FILE_OK) status = verify_published(&probe, pages, &chosen, &chosen_free, &info, file_bytes);
    return status;
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
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return PAGE_FILE_IO;
    /*
     * 읽기 핸들은 파일 공유 잠금을, 쓰기 핸들은 writer 잠금을 쥔 채 게시 상태를 읽는다. 어느 쪽이든 그동안 게시가 없다.
     * 쓰기 핸들은 먼저 공유 잠금으로 ToteDB 파일인지 확인하고 놓은 뒤 writer 잠금을 잡는다. 다른 형식 파일 옆에 잠금
     * 파일을 만들지 않고, 공유 잠금을 쥔 채 writer 잠금을 기다리다 게시하려는 writer와 서로 막히지 않게 하기 위해서다.
     */
    int writer_fd = -1;
    page_file_status_t status = lock_fd(fd, LOCK_SH, g_default_lock_timeout_ms);
    if (status == PAGE_FILE_OK && writable) {
        status = check_published_file(fd);
        flock(fd, LOCK_UN);
        if (status == PAGE_FILE_OK) status = lock_writer(path, &writer_fd, g_default_lock_timeout_ms);
    }
    if (status != PAGE_FILE_OK) {
        int err = errno;
        if (writer_fd >= 0) close(writer_fd);
        close(fd);
        errno = err;
        return status;
    }
    if (admit != NULL) {
        /* 잠근 fd의 크기로 상한을 정한다. 이 뒤의 할당은 모두 admit가 허락한 몫 안에서만 일어난다 */
        struct stat sized;
        if (fstat(fd, &sized) != 0) {
            int err = errno;
            if (writer_fd >= 0) close(writer_fd);
            close(fd);
            errno = err;
            return PAGE_FILE_IO;
        }
        if (!admit(ctx, sized.st_size > 0 ? (uint64_t)sized.st_size : 0)) {
            if (writer_fd >= 0) close(writer_fd);
            close(fd);
            return PAGE_FILE_NOMEM;
        }
    }
    page_file_t *file = new_handle(fd, writable);
    if (file == NULL) {
        if (writer_fd >= 0) close(writer_fd);
        close(fd);
        return PAGE_FILE_NOMEM;
    }
    file->writer_fd = writer_fd;
    file->path = copy_string(path, NULL);
    if (file->path == NULL) {
        close(fd);
        free_handle(file);
        return PAGE_FILE_NOMEM;
    }

    uint8_t pages[2][PAGE_FILE_PAGE_BYTES];
    page_file_control_t chosen;
    free_header_t chosen_free;
    page_file_open_info_t local_info;
    uint64_t file_bytes = 0;
    status = read_controls(file, pages, &chosen, &chosen_free, &local_info, &file_bytes);
    if (status == PAGE_FILE_OK) status = verify_published(file, pages, &chosen, &chosen_free, &local_info, file_bytes);
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
    adopt_published(file, &chosen, file_bytes);
    file->published = true;
    file->lock = writable ? PAGE_FILE_LOCK_EXCLUSIVE : PAGE_FILE_LOCK_SHARED;
    if (info != NULL) *info = local_info;
    *out = file;
    return PAGE_FILE_OK;
}

page_file_status_t page_file_close(page_file_t *file)
{
    if (file == NULL) return PAGE_FILE_INVALID;
    page_file_status_t status = PAGE_FILE_OK;
    /* fork한 자식의 close는 부모가 만드는 partial을 지우지 않는다. fd만 닫아 부모의 잠금도 그대로다 */
    if (!file->published && file->partial_path != NULL && !forked(file)) file->io.unlink(file->partial_path);
    if (file->fd >= 0 && close(file->fd) != 0) status = PAGE_FILE_IO;
    free_handle(file);
    return status;
}

/* 쓰기 핸들의 빈 목록·페이지 상태를 비운다. 디스크에서 다시 읽기 전에 쓴다 */
static void reset_free_state(page_file_t *file)
{
    if (file->state != NULL) memset(file->state, PS_USED, file->state_cap);
    file->pending.len = 0;
    file->aging.len = 0;
    file->reusable.len = 0;
    file->fresh.len = 0;
    file->chain.len = 0;
    file->manifest_len = 0;
    file->needs_barrier = false;
}

/*
 * 잠금을 얻은 직후 게시 상태를 디스크와 맞춘다. 세대가 같고 버린 변경이 없으면 그대로 둔다. 다르면 제어 상태와
 * (쓰기 핸들은) 빈 목록을 다시 읽고 캐시를 비운다. 실패하면 stale로 남겨 다음 잠금이 다시 시도한다.
 */
static page_file_status_t sync_published(page_file_t *file, bool *changed)
{
    uint8_t pages[2][PAGE_FILE_PAGE_BYTES];
    page_file_control_t chosen;
    free_header_t chosen_free;
    page_file_open_info_t info;
    uint64_t file_bytes = 0;
    page_file_status_t status = read_controls(file, pages, &chosen, &chosen_free, &info, &file_bytes);
    if (status != PAGE_FILE_OK) return status;
    /* 이 핸들이 쓰거나 확인한 세대 그대로면 manifest 대조 없이 쓴다. 문장마다 manifest 페이지를 다시 읽지 않기 위해서다 */
    if (!file->stale && chosen.generation == file->committed.generation) {
        file->file_bytes = file_bytes;
        return PAGE_FILE_OK;
    }
    status = verify_published(file, pages, &chosen, &chosen_free, &info, file_bytes);
    if (status != PAGE_FILE_OK) return status;
    if (!file->stale && chosen.generation == file->committed.generation) {
        /* 더 새 세대는 끝나지 않은 commit이었다(쓰던 핸들이 멈춤). 아는 세대가 여전히 게시 상태다 */
        file->file_bytes = file_bytes;
        return PAGE_FILE_OK;
    }
    file->stale = true;
    if (file->writable) {
        reset_free_state(file);
        file->page_count = chosen.page_count;
        status = load_free_list(file, pages[chosen.generation % 2], &chosen_free);
        if (status != PAGE_FILE_OK) return status;
    }
    adopt_published(file, &chosen, file_bytes);
    page_cache_destroy(file->cache);
    file->cache = NULL;
    file->cache_tried = false;
    file->stale = false;
    *changed = true;
    return PAGE_FILE_OK;
}

void page_file_set_lock_timeout(page_file_t *file, uint32_t timeout_ms)
{
    if (file != NULL) file->lock_timeout_ms = timeout_ms;
}

page_file_lock_t page_file_lock_mode(const page_file_t *file)
{
    return file != NULL ? file->lock : PAGE_FILE_LOCK_NONE;
}

page_file_status_t page_file_lock(page_file_t *file, page_file_lock_t mode, bool *changed)
{
    bool ignored = false;
    if (changed == NULL) changed = &ignored;
    *changed = false;
    if (file == NULL || !file->published || mode == PAGE_FILE_LOCK_NONE) return PAGE_FILE_INVALID;
    if (forked(file)) return PAGE_FILE_FORKED;
    if (file->lock != PAGE_FILE_LOCK_NONE || (mode == PAGE_FILE_LOCK_EXCLUSIVE && !file->writable)) {
        return PAGE_FILE_INVALID;
    }
    /* reader는 게시 순간(짧다)만 비켜 주므로 게시 대기 상한까지 기다린다. writer는 다른 쓰기 거래 전체를 기다리므로 핸들 상한이다 */
    if (mode == PAGE_FILE_LOCK_SHARED) wait_for_publisher(file, file->publish_timeout_ms);
    page_file_status_t status = mode == PAGE_FILE_LOCK_SHARED
                                    ? lock_fd(file->fd, LOCK_SH, file->publish_timeout_ms)
                                    : lock_writer(file->path, &file->writer_fd, file->lock_timeout_ms);
    if (status != PAGE_FILE_OK) return status;
    file->lock = mode;
    status = sync_published(file, changed);
    if (status != PAGE_FILE_OK) {
        int err = errno;
        flock(mode == PAGE_FILE_LOCK_SHARED ? file->fd : file->writer_fd, LOCK_UN);
        file->lock = PAGE_FILE_LOCK_NONE;
        errno = err;
    }
    return status;
}

page_file_status_t page_file_unlock(page_file_t *file)
{
    if (file == NULL || !file->published || file->lock == PAGE_FILE_LOCK_NONE) return PAGE_FILE_INVALID;
    if (forked(file)) return PAGE_FILE_FORKED;
    /* commit하지 않은 변경(새 페이지, 보류 해제, 카탈로그 지정)은 버린다. 다음 잠금이 디스크 상태로 다시 읽는다 */
    if (file->lock == PAGE_FILE_LOCK_EXCLUSIVE &&
        (file->fresh.len > 0 || file->pending.len > 0 || file->manifest_len > 0 || file->needs_barrier ||
         file->page_count != file->committed.page_count || file->staged_head != file->committed.catalog_head ||
         file->staged_size != file->committed.catalog_size ||
         file->staged_generation != file->committed.catalog_generation)) {
        file->stale = true;
    }
    if (flock(file->lock == PAGE_FILE_LOCK_SHARED ? file->fd : file->writer_fd, LOCK_UN) != 0) return PAGE_FILE_IO;
    file->lock = PAGE_FILE_LOCK_NONE;
    return PAGE_FILE_OK;
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

/* 쓴 페이지를 manifest에 적는다. 새 페이지가 아니거나 자리가 없으면 다음 commit은 fsync 장벽을 쓴다 */
static void note_write(page_file_t *file, uint32_t page_id, const uint8_t *page)
{
    if (file->state == NULL || file->state[page_id] != PS_FRESH) {
        file->needs_barrier = true;
        return;
    }
    uint32_t sum = le_get_u32(page + 28);
    for (uint32_t i = 0; i < file->manifest_len; i++) {
        if (file->manifest_ids[i] != page_id) continue;
        file->manifest_sums[i] = sum;
        return;
    }
    if (file->manifest_len == MANIFEST_MAX) {
        file->needs_barrier = true;
        return;
    }
    file->manifest_ids[file->manifest_len] = page_id;
    file->manifest_sums[file->manifest_len++] = sum;
}

/* 놓은 새 페이지는 manifest에서 뺀다. 다음 commit이 가리키지 않는 페이지라 대조할 까닭이 없다 */
static void forget_write(page_file_t *file, uint32_t page_id)
{
    for (uint32_t i = 0; i < file->manifest_len; i++) {
        if (file->manifest_ids[i] != page_id) continue;
        file->manifest_len--;
        file->manifest_ids[i] = file->manifest_ids[file->manifest_len];
        file->manifest_sums[i] = file->manifest_sums[file->manifest_len];
        return;
    }
}

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
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_SHARED);
    if (access != PAGE_FILE_OK) return access;
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
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_EXCLUSIVE);
    if (access != PAGE_FILE_OK) return access;
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
    note_write(file, page_id, page);
    page_cache_put(cache_of(file), page_id, (uint16_t)kind, table_id, body);
    return PAGE_FILE_OK;
}

page_file_status_t page_file_page_info(page_file_t *file, uint32_t page_id,
                                       page_file_page_info_t *info)
{
    if (file == NULL || info == NULL) return PAGE_FILE_INVALID;
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_SHARED);
    if (access != PAGE_FILE_OK) return access;
    if (!data_page_in_range(file, page_id)) return PAGE_FILE_INVALID;
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
    if (file == NULL || page_id == NULL) return PAGE_FILE_INVALID;
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_EXCLUSIVE);
    if (access != PAGE_FILE_OK) return access;
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
    if (file == NULL) return PAGE_FILE_INVALID;
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_EXCLUSIVE);
    if (access != PAGE_FILE_OK) return access;
    if (!data_page_in_range(file, page_id)) return PAGE_FILE_INVALID;
    switch ((page_state_t)file->state[page_id]) {
        case PS_USED:
            file->state[page_id] = PS_PENDING;
            list_push(&file->pending, page_id);
            return PAGE_FILE_OK;
        case PS_FRESH:
            /* 어느 제어 페이지도 가리킨 적 없어 곧바로 다시 쓸 수 있다 */
            file->state[page_id] = PS_DISCARDED;
            list_push(&file->reusable, page_id);
            forget_write(file, page_id);
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
    if (file == NULL || check_access(file, PAGE_FILE_LOCK_EXCLUSIVE) != PAGE_FILE_OK) return;
    for (size_t i = 0; i < file->pending.len; i++) file->state[file->pending.items[i]] = PS_USED;
    file->pending.len = 0;
}

void page_file_release_fresh(page_file_t *file)
{
    if (file == NULL || check_access(file, PAGE_FILE_LOCK_EXCLUSIVE) != PAGE_FILE_OK) return;
    for (size_t i = 0; i < file->fresh.len; i++) {
        uint32_t id = file->fresh.items[i];
        if (file->state[id] != PS_FRESH) continue;
        file->state[id] = PS_DISCARDED;
        list_push(&file->reusable, id);
    }
    file->manifest_len = 0;
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
    const uint8_t *ids = control_page + PAGE_ENVELOPE_BYTES + CONTROL_HEADER_BYTES + 8u * header->manifest;
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
            next = le_get_u32(body);
            count = le_get_u32(body + 4);
            ids = body + CHAIN_HEADER_BYTES;
            if (count == 0 || count > CHAIN_IDS || count > total - at) {
                status = PAGE_FILE_CORRUPT;
                break;
            }
        }
        for (uint32_t i = 0; i < count; i++, at++) {
            uint32_t id = le_get_u32(ids + 4u * i);
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
    if (file == NULL) return PAGE_FILE_INVALID;
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_EXCLUSIVE);
    if (access != PAGE_FILE_OK) return access;
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
    for (uint32_t i = 0; i < count; i++) le_put_u32(at + 4u * i, cursor_next(cursor));
}

/* commit이 받은 사슬 페이지를 돌려준다(새 페이지라 곧바로 재사용 목록으로 간다) */
static void undo_chain(page_file_t *file, const id_list_t *chain)
{
    for (size_t i = chain->len; i > 0; i--) page_file_release(file, chain->items[i - 1]);
}

page_file_status_t page_file_commit(page_file_t *file)
{
    if (file == NULL) return PAGE_FILE_INVALID;
    page_file_status_t access = check_access(file, PAGE_FILE_LOCK_EXCLUSIVE);
    if (access != PAGE_FILE_OK) return access;
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
                            (uint32_t)next_chain.len, 0};
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
    uint32_t slot = (uint32_t)(next.generation % 2);

    page_file_status_t status = PAGE_FILE_OK;
    uint64_t left = total - inline_count;
    uint8_t chain_body[PAGE_BODY_BYTES];
    /* 앞 inline_count개는 제어 페이지에 쓴다(사슬을 쓴 뒤 manifest와 함께). 사슬은 그 다음 번호부터다 */
    for (uint32_t i = 0; i < inline_count; i++) cursor_next(&cursor);
    for (size_t i = 0; i < next_chain.len && status == PAGE_FILE_OK; i++) {
        uint32_t count = left < CHAIN_IDS ? (uint32_t)left : CHAIN_IDS;
        memset(chain_body, 0, sizeof(chain_body));
        le_put_u32(chain_body, i + 1 < next_chain.len ? next_chain.items[i + 1] : PAGE_FILE_NO_PAGE);
        le_put_u32(chain_body + 4, count);
        put_ids(chain_body + CHAIN_HEADER_BYTES, &cursor, count);
        left -= count;
        uint32_t id = next_chain.items[i];
        encode_page(page, PAGE_KIND_FREE_LIST, id, 0, PAGE_BODY_BYTES, chain_body);
        page_cache_drop(file->cache, id);
        status = write_full(file, page, sizeof(page), (uint64_t)id * PAGE_FILE_PAGE_BYTES);
        uint64_t end = ((uint64_t)id + 1) * PAGE_FILE_PAGE_BYTES;
        if (status == PAGE_FILE_OK && end > file->file_bytes) file->file_bytes = end;
        if (status == PAGE_FILE_OK) note_write(file, id, page);
    }

    uint64_t needed = (uint64_t)file->page_count * PAGE_FILE_PAGE_BYTES;
    if (status == PAGE_FILE_OK && needed > file->file_bytes) {
        /* 할당만 하고 쓰지 않은 페이지도 파일 길이 안에 두어 다시 열 때 잘린 파일로 보이지 않게 한다. */
        if (ftruncate(file->fd, (off_t)needed) != 0) status = PAGE_FILE_IO;
        else file->file_bytes = needed;
    }
    /* manifest로 끝남을 확인할 수 있으면 데이터 fsync를 제어 쓰기 뒤의 fsync 하나로 합친다 */
    bool barrier = file->needs_barrier;
    if (status == PAGE_FILE_OK && barrier) status = sync_file(file);
    if (status != PAGE_FILE_OK) {
        int err = errno;
        if (!file->poisoned) undo_chain(file, &next_chain);
        free(next_chain.items);
        errno = err;
        return status;
    }

    /*
     * 게시 순간에만 파일 배타 잠금을 잡는다. 문장을 읽는 reader(공유 잠금)가 끝나기를 기다리고, 그동안 새 reader는
     * 기다린다. 읽는 문장 도중에 세대가 바뀌지 않으므로 그 세대가 가리키는 페이지는 문장이 끝날 때까지 다시 쓰이지 않는다.
     */
    set_publish_intent(file, true);
    status = lock_fd(file->fd, LOCK_EX, file->publish_timeout_ms);
    if (status != PAGE_FILE_OK) {
        int err = errno;
        set_publish_intent(file, false);
        undo_chain(file, &next_chain);
        free(next_chain.items);
        errno = err;
        return status;
    }
    if (!barrier) header.manifest = file->manifest_len;
    encode_control_header(body, &next, &header);
    for (uint32_t i = 0; i < header.manifest; i++) {
        le_put_u32(body + CONTROL_HEADER_BYTES + 8u * i, file->manifest_ids[i]);
        le_put_u32(body + CONTROL_HEADER_BYTES + 8u * i + 4, file->manifest_sums[i]);
    }
    uint32_t ids_at = CONTROL_HEADER_BYTES + 8u * header.manifest;
    cursor = (free_cursor_t){{&file->reusable, &file->aging, &file->pending, &file->chain}, 0, 0};
    put_ids(body + ids_at, &cursor, inline_count);
    uint32_t control_bytes = ids_at + 4u * inline_count;
    encode_page(page, PAGE_KIND_CONTROL, slot, 0, control_bytes, body);
    /* 여기서부터는 실패해도 디스크에 새 세대가 남았는지 알 수 없다. */
    file->commit_outcome = PAGE_FILE_COMMIT_UNKNOWN;
    status = write_full(file, page, sizeof(page), (uint64_t)slot * PAGE_FILE_PAGE_BYTES);
    if (status != PAGE_FILE_OK) file->poisoned = true;
    if (status == PAGE_FILE_OK) status = sync_file(file);
    int err = errno;
    flock(file->fd, LOCK_UN);
    set_publish_intent(file, false);
    errno = err;
    if (status != PAGE_FILE_OK) {
        free(next_chain.items);
        return status;
    }

    file->commit_outcome = PAGE_FILE_COMMIT_APPLIED;
    file->committed = next;
    file->manifest_len = 0;
    file->needs_barrier = false;
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

// cost: time O(c), heap O(1), stack O(1), io 0
// vars: c = 빈 목록 사슬 페이지 수
// basis: estimate
page_file_status_t page_file_page_role(const page_file_t *file, uint32_t page_id, page_file_role_t *role)
{
    if (file == NULL || role == NULL || !file->writable || file->state == NULL || !data_page_in_range(file, page_id)) {
        return PAGE_FILE_INVALID;
    }
    for (size_t i = 0; i < file->chain.len; i++) {
        if (file->chain.items[i] != page_id) continue;
        *role = PAGE_FILE_ROLE_FREE_LIST;
        return PAGE_FILE_OK;
    }
    page_state_t state = (page_state_t)file->state[page_id];
    *role = state == PS_USED || state == PS_FRESH ? PAGE_FILE_ROLE_IN_USE : PAGE_FILE_ROLE_FREE;
    return PAGE_FILE_OK;
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
