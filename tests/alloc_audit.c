#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alloc_audit.h"

size_t audit_live;
size_t audit_peak;
size_t audit_calls;
size_t audit_max_single;
void (*audit_on_alloc)(void *ctx, size_t request, size_t live);
void *audit_ctx;

/* 요청 크기를 블록 앞에 적는다. 16바이트는 max_align_t 정렬을 지킨다 */
typedef struct {
    size_t size;
    size_t unused;
} header_t;

void audit_reset(void)
{
    audit_live = audit_peak = audit_calls = audit_max_single = 0;
    audit_on_alloc = NULL;
    audit_ctx = NULL;
}

static void note(size_t request)
{
    audit_calls++;
    audit_live += request;
    if (audit_live > audit_peak) audit_peak = audit_live;
    if (request > audit_max_single) audit_max_single = request;
    if (audit_on_alloc != NULL) audit_on_alloc(audit_ctx, request, audit_live);
}

void *audit_malloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(header_t)) return NULL;
    header_t *header = malloc(sizeof(header_t) + size);
    if (header == NULL) return NULL;
    header->size = size;
    note(size);
    return header + 1;
}

void *audit_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    void *block = audit_malloc(bytes);
    if (block != NULL) memset(block, 0, bytes);
    return block;
}

void audit_free(void *block)
{
    if (block == NULL) return;
    header_t *header = (header_t *)block - 1;
    audit_live -= header->size;
    free(header);
}

/* 실패하면 옛 블록은 그대로다. 성공하면 옛 크기를 돌려주고 새 크기를 센다(순간 최대치는 old+new). */
void *audit_realloc(void *block, size_t size)
{
    if (block == NULL) return audit_malloc(size);
    header_t *old = (header_t *)block - 1;
    void *fresh = audit_malloc(size);
    if (fresh == NULL) return NULL;
    memcpy(fresh, block, old->size < size ? old->size : size);
    audit_free(block);
    return fresh;
}
