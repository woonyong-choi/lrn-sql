#include <pthread.h>
#include <stdbool.h>
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
void (*audit_on_free)(void *ctx, size_t size, size_t live);
void *audit_free_ctx;
static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;

size_t audit_live_now(void)
{
    pthread_mutex_lock(&audit_lock);
    size_t live = audit_live;
    pthread_mutex_unlock(&audit_lock);
    return live;
}

/* 요청 크기를 블록 앞에 적는다. 16바이트는 max_align_t 정렬을 지킨다 */
typedef struct header {
    size_t size;
    struct header *prev, *next;
    size_t unused;  /* 32바이트로 맞춰 max_align_t 정렬을 지킨다 */
} header_t;

static header_t *audit_blocks;  /* 살아 있는 블록. audit_lock으로 보호 */

bool audit_is_live(const void *block)
{
    pthread_mutex_lock(&audit_lock);
    bool found = false;
    for (const header_t *h = audit_blocks; h != NULL && !found; h = h->next) found = (const void *)(h + 1) == block;
    pthread_mutex_unlock(&audit_lock);
    return found;
}

void audit_reset(void)
{
    pthread_mutex_lock(&audit_lock);
    audit_live = audit_peak = audit_calls = audit_max_single = 0;
    audit_on_alloc = NULL;
    audit_ctx = NULL;
    audit_on_free = NULL;
    audit_free_ctx = NULL;
    pthread_mutex_unlock(&audit_lock);
}

static void note(size_t request)
{
    pthread_mutex_lock(&audit_lock);
    audit_calls++;
    audit_live += request;
    if (audit_live > audit_peak) audit_peak = audit_live;
    if (request > audit_max_single) audit_max_single = request;
    size_t live = audit_live;
    void (*hook)(void *, size_t, size_t) = audit_on_alloc;
    void *ctx = audit_ctx;
    pthread_mutex_unlock(&audit_lock);
    if (hook != NULL) hook(ctx, request, live);
}

void *audit_malloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(header_t)) return NULL;
    header_t *header = malloc(sizeof(header_t) + size);
    if (header == NULL) return NULL;
    header->size = size;
    pthread_mutex_lock(&audit_lock);
    header->prev = NULL;
    header->next = audit_blocks;
    if (audit_blocks != NULL) audit_blocks->prev = header;
    audit_blocks = header;
    pthread_mutex_unlock(&audit_lock);
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
    size_t size = header->size;
    pthread_mutex_lock(&audit_lock);
    if (header->prev != NULL) header->prev->next = header->next;
    else audit_blocks = header->next;
    if (header->next != NULL) header->next->prev = header->prev;
    pthread_mutex_unlock(&audit_lock);
    free(header);  /* 실제 해제가 끝난 뒤에 live를 줄인다: live가 줄면 그 메모리는 이미 없다 */
    pthread_mutex_lock(&audit_lock);
    audit_live -= size;
    size_t live = audit_live;
    void (*hook)(void *, size_t, size_t) = audit_on_free;
    void *ctx = audit_free_ctx;
    pthread_mutex_unlock(&audit_lock);
    if (hook != NULL) hook(ctx, size, live);
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
