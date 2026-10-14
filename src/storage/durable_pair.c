#include "storage/durable_pair.h"

#include <errno.h>
#include <string.h>

static durable_pair_status_t from_page_file(page_file_status_t status)
{
    switch (status) {
        case PAGE_FILE_OK: return DURABLE_PAIR_OK;
        case PAGE_FILE_INVALID: return DURABLE_PAIR_INVALID;
        case PAGE_FILE_CORRUPT:
        case PAGE_FILE_NO_CONTROL: return DURABLE_PAIR_CORRUPT;
        case PAGE_FILE_UNSUPPORTED: return DURABLE_PAIR_UNSUPPORTED;
        case PAGE_FILE_LOCKED: return DURABLE_PAIR_LOCKED;
        case PAGE_FILE_NOMEM: return DURABLE_PAIR_NOMEM;
        case PAGE_FILE_MISMATCH: return DURABLE_PAIR_MISMATCH;
        default: return DURABLE_PAIR_IO;
    }
}

static durable_pair_status_t from_wal(wal_status_t status)
{
    switch (status) {
        case WAL_OK: return DURABLE_PAIR_OK;
        case WAL_INVALID: return DURABLE_PAIR_INVALID;
        case WAL_CORRUPT: return DURABLE_PAIR_CORRUPT;
        case WAL_UNSUPPORTED: return DURABLE_PAIR_UNSUPPORTED;
        case WAL_LOCKED: return DURABLE_PAIR_LOCKED;
        case WAL_NOMEM: return DURABLE_PAIR_NOMEM;
        case WAL_LIMIT: return DURABLE_PAIR_LIMIT;
        default: return DURABLE_PAIR_IO;
    }
}

static void abandon(durable_pair_t *pair)
{
    int saved = errno;
    if (pair->wal != NULL) wal_close(pair->wal);
    if (pair->file != NULL) page_file_close(pair->file);
    pair->wal = NULL;
    pair->file = NULL;
    errno = saved;
}

/* WAL을 연다. 파일이 없으면 *absent를 true로 하고 OK다. */
static durable_pair_status_t open_wal(const char *path, const durable_pair_options_t *options, wal_t **wal,
                                      wal_open_info_t *info, bool *absent)
{
    *absent = false;
    wal_status_t st = wal_open_with(path, true, options->wal_limits, wal, info);
    if (st == WAL_IO && errno == ENOENT) {
        *absent = true;
        return DURABLE_PAIR_OK;
    }
    return from_wal(st);
}

/* B와 C. 제어 페이지가 아직 v1이면 enable 뒤 commit 두 번, v2이고 ready가 아니면 commit 한 번 */
static durable_pair_status_t finish_conversion(durable_pair_t *pair, uint32_t epoch)
{
    const page_file_control_t *c = page_file_control(pair->file);
    if (c->format_version != PAGE_FILE_DURABLE_FORMAT_VERSION) {
        page_file_status_t st = page_file_enable_durable(pair->file, wal_file_id(pair->wal), epoch);
        if (st != PAGE_FILE_OK) return from_page_file(st);
        st = page_file_commit(pair->file);
        if (st != PAGE_FILE_OK) return from_page_file(st);
    }
    if (!page_file_durable_ready(pair->file)) {
        page_file_status_t st = page_file_commit(pair->file);
        if (st != PAGE_FILE_OK) return from_page_file(st);
    }
    return page_file_durable_ready(pair->file) ? DURABLE_PAIR_OK : DURABLE_PAIR_IO;
}

static durable_pair_status_t decide(durable_pair_t *pair, const char *wal_path,
                                    const durable_pair_options_t *options, durable_pair_info_t *info)
{
    const page_file_control_t *c = page_file_control(pair->file);
    bool durable = c->format_version == PAGE_FILE_DURABLE_FORMAT_VERSION;
    bool absent = false;
    durable_pair_status_t st = open_wal(wal_path, options, &pair->wal, &info->wal_info, &absent);
    if (st != DURABLE_PAIR_OK) return st;

    if (absent) {
        if (durable) return DURABLE_PAIR_WAL_MISSING;
        if (!options->convert) return DURABLE_PAIR_OK; /* PLAIN */
        st = from_wal(wal_create_with(wal_path, options->wal_limits, &pair->wal));
        if (st != DURABLE_PAIR_OK) return st;
        info->converted = true;
        return finish_conversion(pair, options->durable_epoch);
    }
    if (durable && c->wal_file_id != wal_file_id(pair->wal)) return DURABLE_PAIR_MISMATCH;
    if (durable && c->durable_ready) return DURABLE_PAIR_OK;
    /* v1이거나 ready가 아니다. 기록이 있으면 승인된 거래가 있을 수 있어 전환을 이어 가지 않는다. */
    if (info->wal_info.records != 0) return DURABLE_PAIR_DOWNGRADED;
    if (!options->convert) return DURABLE_PAIR_NEEDS_CONVERSION;
    info->converted = true;
    info->resumed = true;
    return finish_conversion(pair, options->durable_epoch);
}

durable_pair_status_t durable_pair_open(const char *db_path, const char *wal_path,
                                        const durable_pair_options_t *options, durable_pair_t *out,
                                        durable_pair_info_t *info)
{
    if (out == NULL) return DURABLE_PAIR_INVALID;
    out->file = NULL;
    out->wal = NULL;
    durable_pair_info_t local;
    if (info == NULL) info = &local;
    memset(info, 0, sizeof(*info));
    static const durable_pair_options_t defaults = {0};
    if (options == NULL) options = &defaults;
    if (db_path == NULL || wal_path == NULL) return DURABLE_PAIR_INVALID;

    durable_pair_t pair = {NULL, NULL};
    page_file_status_t fs = page_file_open_admitted(db_path, true, &pair.file, &info->file_info, options->admit,
                                                    options->admit_ctx);
    if (fs != PAGE_FILE_OK) return from_page_file(fs);
    durable_pair_status_t st = decide(&pair, wal_path, options, info);
    if (st != DURABLE_PAIR_OK) {
        abandon(&pair);
        return st;
    }
    *out = pair;
    return DURABLE_PAIR_OK;
}

durable_pair_status_t durable_pair_persist_epoch(durable_pair_t *pair, uint32_t epoch)
{
    if (pair == NULL || pair->file == NULL) return DURABLE_PAIR_INVALID;
    page_file_status_t st = page_file_stage_durable_epoch(pair->file, epoch);
    if (st != PAGE_FILE_OK) return from_page_file(st);
    st = page_file_commit(pair->file);
    if (st != PAGE_FILE_OK) {
        /* 실패한 commit이 디스크에 남았는지 모를 수 있다. 같은 값을 다시 staged로 두지 않고 호출자가 다시 연다. */
        return from_page_file(st);
    }
    return DURABLE_PAIR_OK;
}

durable_pair_status_t durable_pair_close(durable_pair_t *pair)
{
    if (pair == NULL) return DURABLE_PAIR_INVALID;
    durable_pair_status_t result = DURABLE_PAIR_OK;
    if (pair->wal != NULL) {
        durable_pair_status_t st = from_wal(wal_close(pair->wal));
        if (result == DURABLE_PAIR_OK) result = st;
    }
    if (pair->file != NULL) {
        durable_pair_status_t st = from_page_file(page_file_close(pair->file));
        if (result == DURABLE_PAIR_OK) result = st;
    }
    pair->wal = NULL;
    pair->file = NULL;
    return result;
}

const char *durable_pair_status_name(durable_pair_status_t status)
{
    static const char *const names[] = {"OK", "INVALID", "IO", "NOMEM", "CORRUPT", "UNSUPPORTED", "LOCKED",
                                        "LIMIT", "NEEDS_CONVERSION", "WAL_MISSING", "MISMATCH", "DOWNGRADED"};
    return (size_t)status < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}
