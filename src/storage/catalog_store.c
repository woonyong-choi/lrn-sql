#include <errno.h>
#include <stdlib.h>

#include "storage/catalog_store.h"

#define CATALOG_TABLE_ID 0u

static catalog_store_status_t from_page_file(page_file_status_t status)
{
    switch (status) {
        case PAGE_FILE_OK: return CATALOG_STORE_OK;
        case PAGE_FILE_INVALID: return CATALOG_STORE_INVALID;
        case PAGE_FILE_CORRUPT:
        case PAGE_FILE_NO_CONTROL: return CATALOG_STORE_CORRUPT;
        case PAGE_FILE_UNSUPPORTED: return CATALOG_STORE_UNSUPPORTED;
        case PAGE_FILE_NOMEM: return CATALOG_STORE_NOMEM;
        case PAGE_FILE_LIMIT: return CATALOG_STORE_LIMIT;
        default: return CATALOG_STORE_IO;
    }
}

/* overflow는 어댑터 실패를 IO 하나로만 알리므로 마지막 page_file 상태로 원인을 가른다. */
static catalog_store_status_t from_overflow(overflow_status_t status,
                                            const page_file_overflow_ctx_t *ctx)
{
    switch (status) {
        case OVERFLOW_OK: return CATALOG_STORE_OK;
        case OVERFLOW_INVALID: return CATALOG_STORE_INVALID;
        case OVERFLOW_LIMIT: return CATALOG_STORE_LIMIT;
        case OVERFLOW_NOMEM: return CATALOG_STORE_NOMEM;
        case OVERFLOW_CORRUPT: return CATALOG_STORE_CORRUPT;
        case OVERFLOW_UNRECLAIMED: return CATALOG_STORE_UNRECLAIMED;
        case OVERFLOW_IO:
        case OVERFLOW_ROLLBACK_FAILED:
            return ctx->last_status == PAGE_FILE_OK ? CATALOG_STORE_IO : from_page_file(ctx->last_status);
        default: return CATALOG_STORE_IO;
    }
}

static catalog_store_status_t from_catalog(catalog_status_t status)
{
    switch (status) {
        case CATALOG_OK: return CATALOG_STORE_OK;
        case CATALOG_UNSUPPORTED: return CATALOG_STORE_UNSUPPORTED;
        case CATALOG_NOMEM: return CATALOG_STORE_NOMEM;
        case CATALOG_LIMIT: return CATALOG_STORE_LIMIT;
        case CATALOG_CORRUPT: return CATALOG_STORE_CORRUPT;
        default: return CATALOG_STORE_INVALID;
    }
}

catalog_store_status_t catalog_store_save(page_file_t *file, const catalog_t *catalog)
{
    if (file == NULL || catalog == NULL) return CATALOG_STORE_INVALID;
    if (page_file_is_poisoned(file)) {
        errno = EIO;
        return CATALOG_STORE_IO;
    }
    const page_file_control_t *control = page_file_control(file);
    uint32_t old_head = control->catalog_head;
    uint32_t old_size = control->catalog_size;
    uint64_t old_generation = control->catalog_generation;
    uint64_t generation = catalog_generation(catalog);
    if (old_head != PAGE_FILE_NO_PAGE && generation < old_generation) return CATALOG_STORE_STALE;

    uint8_t *bytes = NULL;
    size_t size = 0;
    catalog_status_t encoded = catalog_encode(catalog, &bytes, &size);
    if (encoded != CATALOG_OK) return from_catalog(encoded);

    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, file, CATALOG_TABLE_ID);
    overflow_ref_t created;
    overflow_status_t written = overflow_write(&io, bytes, size, &created);
    free(bytes);
    if (written != OVERFLOW_OK) return from_overflow(written, &ctx);

    int saved_errno = 0;
    page_file_status_t staged = page_file_stage_catalog(file, created.head, created.size_bytes, generation);
    page_file_status_t committed = staged == PAGE_FILE_OK ? page_file_commit(file) : staged;
    if (committed != PAGE_FILE_OK) {
        saved_errno = errno;
        if (staged == PAGE_FILE_OK && page_file_last_commit_outcome(file) == PAGE_FILE_COMMIT_UNKNOWN) {
            /*
             * 제어 쓰기를 시도했으므로 디스크가 새 체인을 가리킬 수 있다. 어느 체인도 놓지 않는다.
             * 놓으면 다시 열었을 때 보이는 쪽의 페이지가 재사용될 수 있다.
             */
            errno = saved_errno;
            return CATALOG_STORE_OUTCOME_UNKNOWN;
        }
        /* 제어 쓰기 전의 실패라 새 체인은 어떤 제어 페이지도 가리키지 않아 보류 해제해도 안전하다. 옛 참조를 되돌려 둔다. */
        overflow_free(&io, created);
        page_file_stage_catalog(file, old_head, old_size, old_generation);
        errno = saved_errno;
        return from_page_file(committed);
    }

    /* commit이 끝난 뒤에만 옛 체인을 놓는다. 놓은 페이지는 다음 commit 뒤에야 재사용된다. */
    if (old_head != PAGE_FILE_NO_PAGE) {
        overflow_status_t freed = overflow_free(&io, (overflow_ref_t){old_head, old_size});
        if (freed != OVERFLOW_OK) return CATALOG_STORE_UNRECLAIMED;
    }
    return CATALOG_STORE_OK;
}

catalog_store_status_t catalog_store_load(page_file_t *file, catalog_t **out)
{
    return catalog_store_load_admitted(file, NULL, NULL, out);
}

catalog_store_status_t catalog_store_load_admitted(page_file_t *file, catalog_store_admit_fn admit,
                                                   void *admit_ctx, catalog_t **out)
{
    if (file == NULL || out == NULL) return CATALOG_STORE_INVALID;
    const page_file_control_t *control = page_file_control(file);
    if (control->catalog_head == PAGE_FILE_NO_PAGE) return CATALOG_STORE_EMPTY;
    if (admit != NULL) {
        if (control->catalog_size > CATALOG_MAX_ENCODED_BYTES) return CATALOG_STORE_LIMIT;
        if (!admit(admit_ctx, control->catalog_size)) return CATALOG_STORE_NOMEM;
    }

    page_file_overflow_ctx_t ctx;
    overflow_io_t io = page_file_overflow_io(&ctx, file, CATALOG_TABLE_ID);
    uint8_t *bytes = NULL;
    overflow_status_t read = overflow_read(
        &io, (overflow_ref_t){control->catalog_head, control->catalog_size}, &bytes);
    if (read != OVERFLOW_OK) return from_overflow(read, &ctx);

    catalog_t *loaded = NULL;
    catalog_status_t decoded = catalog_decode(bytes, control->catalog_size, &loaded);
    free(bytes);
    if (decoded != CATALOG_OK) return from_catalog(decoded);
    if (catalog_generation(loaded) != control->catalog_generation) {
        /* 제어 페이지와 체인 내용의 세대가 다르면 서로 다른 시점의 조각이다. */
        catalog_close(loaded);
        return CATALOG_STORE_CORRUPT;
    }
    *out = loaded;
    return CATALOG_STORE_OK;
}
