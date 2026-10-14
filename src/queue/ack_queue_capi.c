/*
 * ack_queue_capi.c — 다른 언어에서 부르는 평탄한 C 인터페이스(#140). 설명은 include/queue/ack_queue_capi.h.
 *
 * 불변 조건: 핸들을 쓰는 모든 호출은 lrndb_t의 mutex 안에서 database를 건드린다. 결과(result_t)는 읽는 동안 database의
 * 상태를 참조하므로 실행부터 인코딩·해제까지 같은 잠금 구간에서 끝낸다. 다른 database의 트랜잭션은 잠그거나 실행하기
 * 전에 거절한다. db가 살아 있는 큐·트랜잭션 핸들을 세어 두고 하나라도 있으면 닫지 않는다.
 */
#define _POSIX_C_SOURCE 200809L

#include "queue/ack_queue_capi.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "queue/ack_queue.h"
#include "sql/database_txn.h"
#include "sql/result.h"

struct lrndb {
    database_t *db;
    pthread_mutex_t mutex;
    size_t live_queues;   /* mutex 안에서만 바꾼다 */
    size_t live_txns;
};

struct lrnq {
    lrndb_t *db;
    ackq_t *queue;
};

struct lrntxn {
    lrndb_t *db;
    database_txn_t *txn;
};

static _Atomic size_t g_max_result_bytes = LRN_DEFAULT_MAX_RESULT_BYTES;

void lrn_set_max_result_bytes(size_t limit)
{
    /* 버퍼 용량을 두 배로 키울 때 size_t가 넘치지 않도록 절반으로 제한한다 */
    atomic_store(&g_max_result_bytes, limit > SIZE_MAX / 2 ? SIZE_MAX / 2 : limit);
}

size_t lrn_max_result_bytes(void)
{
    return atomic_load(&g_max_result_bytes);
}

const char *lrn_status_name(int status)
{
    switch (status) {
        case LRN_EMPTY: return "EMPTY";
        case LRN_DB_INVALID: return "DB_INVALID";
        case LRN_DB_IO: return "DB_IO";
        case LRN_DB_CORRUPT: return "DB_CORRUPT";
        case LRN_DB_LOCKED: return "DB_LOCKED";
        case LRN_DB_UNSUPPORTED: return "DB_UNSUPPORTED";
        case LRN_DB_NOMEM: return "DB_NOMEM";
        case LRN_DB_NOT_FOUND: return "DB_NOT_FOUND";
        default: return ackq_status_name((ackq_status_t)status);
    }
}

void lrn_free(void *pointer)
{
    free(pointer);
}

static int map_database_status(database_status_t status)
{
    switch (status) {
        case DATABASE_OK: return LRN_OK;
        case DATABASE_IO: return LRN_DB_IO;
        case DATABASE_CORRUPT: return LRN_DB_CORRUPT;
        case DATABASE_LOCKED: return LRN_DB_LOCKED;
        case DATABASE_UNSUPPORTED: return LRN_DB_UNSUPPORTED;
        case DATABASE_NOMEM: return LRN_DB_NOMEM;
        default: return LRN_DB_INVALID;
    }
}

int lrndb_open(const char *path, const char *wal_path, int create, lrndb_t **out)
{
    if (out == NULL) return LRN_DB_INVALID;
    *out = NULL;
    if (path == NULL) return LRN_DB_INVALID;
    if (access(path, F_OK) != 0) {
        if (!create) return LRN_DB_NOT_FOUND;
        database_t *fresh = NULL;
        database_status_t created = database_create(path, &fresh);
        if (created != DATABASE_OK) return map_database_status(created);
        if (database_close(fresh) != DATABASE_OK) return LRN_DB_IO;
    }
    lrndb_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) return LRN_DB_NOMEM;
    database_txn_options_t options = {0};
    options.wal_path = wal_path;
    options.convert = wal_path != NULL;
    database_txn_info_t info;
    database_status_t status = database_open_transactional(path, &options, &handle->db, &info);
    if (status != DATABASE_OK) {
        free(handle);
        return map_database_status(status);
    }
    pthread_mutex_init(&handle->mutex, NULL);
    *out = handle;
    return LRN_OK;
}

int lrndb_close(lrndb_t **db)
{
    if (db == NULL || *db == NULL) return LRN_OK;
    lrndb_t *handle = *db;
    pthread_mutex_lock(&handle->mutex);
    if (handle->live_queues > 0 || handle->live_txns > 0) {
        pthread_mutex_unlock(&handle->mutex);
        return ACKQ_BUSY;   /* 큐나 트랜잭션이 이 database를 쥐고 있다. 핸들은 그대로다 */
    }
    database_status_t status = database_close(handle->db);
    pthread_mutex_unlock(&handle->mutex);
    if (status == DATABASE_BUSY) return ACKQ_BUSY;
    pthread_mutex_destroy(&handle->mutex);
    free(handle);
    *db = NULL;
    return map_database_status(status);
}

/* ── 결과 인코딩: 상한을 넘거나 할당이 실패하면 error를 남기고 부분 결과를 주지 않는다 ── */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    ackq_status_t error;   /* ACKQ_OK가 아니면 이후 쓰기는 무시한다 */
} buffer_t;

static void put_bytes(buffer_t *b, const void *bytes, size_t size)
{
    if (b->error != ACKQ_OK) return;
    size_t limit = lrn_max_result_bytes();
    if (size > limit || b->size > limit - size) {
        b->error = ACKQ_LIMIT;
        return;
    }
    size_t need = b->size + size;   /* limit 이하라 넘치지 않는다 */
    if (need > b->capacity) {
        /* 선언한 상한보다 크게 할당하지 않는다. 작은 상한에서는 처음부터 상한 크기로 잡는다. */
        size_t capacity = b->capacity != 0 ? b->capacity : (limit < 256 ? limit : 256);
        while (capacity < need) {
            if (capacity > limit / 2) {
                capacity = limit;   /* need <= limit이므로 충분하다 */
                break;
            }
            capacity *= 2;
        }
        uint8_t *grown = realloc(b->data, capacity);
        if (grown == NULL) {
            b->error = ACKQ_NOMEM;
            return;
        }
        b->data = grown;
        b->capacity = capacity;
    }
    if (size > 0) memcpy(b->data + b->size, bytes, size);
    b->size = need;
}

static void put_u8(buffer_t *b, uint8_t v) { put_bytes(b, &v, 1); }

static void put_u32(buffer_t *b, uint32_t v)
{
    uint8_t raw[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put_bytes(b, raw, 4);
}

static void put_u64(buffer_t *b, uint64_t v)
{
    uint8_t raw[8];
    for (int i = 0; i < 8; i++) raw[i] = (uint8_t)(v >> (8 * i));
    put_bytes(b, raw, 8);
}

/* 길이는 u32로 인코딩한다. 넘으면 자르지 않고 거절한다. */
static void put_length_prefixed(buffer_t *b, const uint8_t *bytes, size_t size)
{
    if (size > UINT32_MAX) {
        if (b->error == ACKQ_OK) b->error = ACKQ_LIMIT;
        return;
    }
    put_u32(b, (uint32_t)size);
    put_bytes(b, bytes, size);
}

static void put_cell(buffer_t *b, const value_view_t *cell)
{
    put_u8(b, (uint8_t)cell->type);
    switch (cell->type) {
        case VALUE_INTEGER: put_u64(b, (uint64_t)cell->integer); break;
        case VALUE_REAL: {
            uint64_t bits;
            memcpy(&bits, &cell->real, sizeof(bits));
            put_u64(b, bits);
            break;
        }
        case VALUE_TEXT:
        case VALUE_BLOB: put_length_prefixed(b, cell->bytes, cell->size_bytes); break;
        default: break;
    }
}

static void encode_error(buffer_t *b, const result_error_t *error)
{
    put_u8(b, 2);
    put_u32(b, error != NULL ? (uint32_t)error->error_class : (uint32_t)RESULT_E_INTERNAL);
    put_u32(b, error != NULL ? (uint32_t)error->code : 0);
    put_length_prefixed(b, error != NULL ? (const uint8_t *)error->message : NULL, error != NULL ? error->message_len : 0);
}

static void encode_rows(buffer_t *b, result_t *result)
{
    size_t columns = result_column_count(result);
    put_u8(b, 0);
    if (columns > UINT32_MAX) {
        b->error = ACKQ_LIMIT;
        return;
    }
    put_u32(b, (uint32_t)columns);
    for (size_t i = 0; i < columns; i++) {
        const result_column_t *column = result_column(result, i);
        put_length_prefixed(b, column->name, column->name_len);
    }
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW && b->error == ACKQ_OK) {
        put_u8(b, 1);
        for (size_t i = 0; i < row.cell_count; i++) put_cell(b, &row.cells[i]);
    }
    if (b->error != ACKQ_OK) return;   /* 상한이나 할당 실패면 부분 행을 주지 않는다 */
    if (next != RESULT_NEXT_DONE) {
        /* 읽는 도중 실패는 오류 결과로 바꿔 부분 행을 성공처럼 돌려주지 않는다 */
        b->size = 0;
        encode_error(b, result_outcome(result).error);
        return;
    }
    put_u8(b, 0);
}

static void encode_result(buffer_t *b, result_t *result)
{
    switch (result_kind(result)) {
        case RESULT_KIND_ROWS: encode_rows(b, result); break;
        case RESULT_KIND_COMMAND: {
            const result_command_t *command = result_command(result);
            put_u8(b, 1);
            put_length_prefixed(b, (const uint8_t *)command->tag, command->tag_len);
            put_u64(b, command->affected_rows);
            break;
        }
        default: encode_error(b, result_outcome(result).error);
    }
}

/* 입력 매개변수를 view로 바꾼다. 실패하면 상태를 돌려주고 *views는 NULL이다. */
static int build_views(const lrn_param_t *params, size_t count, value_view_t **views)
{
    *views = NULL;
    if (count == 0) return LRN_OK;
    value_view_t *built = calloc(count, sizeof(*built));
    if (built == NULL) return ACKQ_NOMEM;
    for (size_t i = 0; i < count; i++) {
        const lrn_param_t *p = &params[i];
        switch (p->type) {
            case LRN_PARAM_NULL: built[i] = (value_view_t){VALUE_NULL, 0, 0.0, NULL, 0}; break;
            case LRN_PARAM_INTEGER: built[i] = (value_view_t){VALUE_INTEGER, p->integer, 0.0, NULL, 0}; break;
            case LRN_PARAM_REAL: built[i] = (value_view_t){VALUE_REAL, 0, p->real, NULL, 0}; break;
            case LRN_PARAM_TEXT:
                built[i] = (value_view_t){VALUE_TEXT, 0, 0.0, p->size > 0 ? p->bytes : NULL, p->size};
                break;
            case LRN_PARAM_BLOB:
                built[i] = (value_view_t){VALUE_BLOB, 0, 0.0, p->size > 0 ? p->bytes : NULL, p->size};
                break;
            default:
                free(built);
                return ACKQ_INVALID;
        }
    }
    *views = built;
    return LRN_OK;
}

int lrndb_exec(lrndb_t *db, lrntxn_t *txn, const char *sql, const lrn_param_t *params, size_t param_count,
               uint8_t **out, size_t *out_size)
{
    if (db == NULL || sql == NULL || out == NULL || out_size == NULL || (param_count > 0 && params == NULL)) {
        return ACKQ_INVALID;
    }
    *out = NULL;
    *out_size = 0;
    if (txn != NULL && txn->db != db) return ACKQ_INVALID;   /* 다른 database의 트랜잭션은 실행 전에 거절 */
    value_view_t *views;
    int status = build_views(params, param_count, &views);
    if (status != LRN_OK) return status;
    lexer_binding_t binding = {views, param_count};
    result_t *result = NULL;
    buffer_t buffer = {0};
    /* 실행, 인코딩, 해제를 한 잠금 구간에서 끝낸다. 행은 result_next가 database 상태를 읽어 만든다. */
    pthread_mutex_lock(&db->mutex);
    database_exec_status_t exec_status;
    if (txn != NULL) {
        exec_status = database_txn_exec(txn->txn, NULL, (const uint8_t *)sql, strlen(sql),
                                        param_count > 0 ? &binding : NULL, &result);
    } else {
        exec_status = database_exec(db->db, (const uint8_t *)sql, strlen(sql), param_count > 0 ? &binding : NULL,
                                    &result);
    }
    if (exec_status == DATABASE_EXEC_OK && result != NULL) {
        encode_result(&buffer, result);
        result_release(&result);
    }
    pthread_mutex_unlock(&db->mutex);
    free(views);
    if (exec_status == DATABASE_EXEC_NOMEM) return ACKQ_NOMEM;
    if (exec_status != DATABASE_EXEC_OK) return ACKQ_INVALID;
    if (buffer.error != ACKQ_OK) {
        free(buffer.data);
        return buffer.error;
    }
    *out = buffer.data;
    *out_size = buffer.size;
    return LRN_OK;
}

/* ── 트랜잭션 ── */

static int map_txn(txn_status_t status)
{
    switch (status) {
        case TXN_OK: return LRN_OK;
        case TXN_BUSY:
        case TXN_TIMEOUT: return ACKQ_BUSY;
        case TXN_NOMEM: return ACKQ_NOMEM;
        case TXN_POISONED: return ACKQ_POISONED;
        case TXN_UNSUPPORTED: return ACKQ_UNSUPPORTED;
        case TXN_COMMIT_FAILED: return ACKQ_NOT_APPLIED;
        case TXN_COMMIT_UNCERTAIN: return ACKQ_UNCERTAIN;
        case TXN_INVALID: return ACKQ_TXN_FAILED;
        default: return ACKQ_IO;
    }
}

int lrndb_begin(lrndb_t *db, int durable, lrntxn_t **out)
{
    if (db == NULL || out == NULL) return ACKQ_INVALID;
    *out = NULL;
    lrntxn_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) return ACKQ_NOMEM;
    txn_begin_options_t options = {.access = TXN_ACCESS_READ_WRITE,
                                   .isolation = TXN_ISOLATION_SNAPSHOT,
                                   .guarantee = durable ? TXN_GUARANTEE_DURABLE : TXN_GUARANTEE_SYNCED,
                                   .timeout_ms = 0};
    pthread_mutex_lock(&db->mutex);
    txn_status_t status = database_txn_begin(db->db, &options, &handle->txn);
    if (status == TXN_OK) db->live_txns++;
    pthread_mutex_unlock(&db->mutex);
    if (status != TXN_OK) {
        free(handle);
        return status == TXN_INVALID ? ACKQ_UNSUPPORTED : map_txn(status);
    }
    handle->db = db;
    *out = handle;
    return LRN_OK;
}

static void forget_txn(lrntxn_t **txn)
{
    lrntxn_t *handle = *txn;
    handle->db->live_txns--;
    free(handle);
    *txn = NULL;
}

int lrndb_commit(lrntxn_t **txn)
{
    if (txn == NULL || *txn == NULL) return ACKQ_INVALID;
    lrndb_t *db = (*txn)->db;
    pthread_mutex_lock(&db->mutex);
    txn_status_t status = database_txn_commit((*txn)->txn, NULL);
    /* 실패 상태(INVALID)이면 트랜잭션이 남으므로 rollback으로 정리한다 */
    if (status == TXN_INVALID) database_txn_rollback((*txn)->txn);
    database_txn_release(&(*txn)->txn);
    forget_txn(txn);
    pthread_mutex_unlock(&db->mutex);
    return map_txn(status);
}

int lrndb_rollback(lrntxn_t **txn)
{
    if (txn == NULL || *txn == NULL) return ACKQ_INVALID;
    lrndb_t *db = (*txn)->db;
    pthread_mutex_lock(&db->mutex);
    txn_status_t status = database_txn_rollback((*txn)->txn);
    database_txn_release(&(*txn)->txn);
    forget_txn(txn);
    pthread_mutex_unlock(&db->mutex);
    return map_txn(status);
}

int lrndb_txn_failed(const lrntxn_t *txn)
{
    return txn != NULL && database_txn_failed(txn->txn) ? 1 : 0;
}

/* ── 큐 ── */

int lrnq_open(lrndb_t *db, const char *name, int install, int durable, uint32_t max_attempts, lrn_clock_fn clock,
              void *clock_ctx, lrnq_t **out)
{
    if (db == NULL || out == NULL) return ACKQ_INVALID;
    *out = NULL;
    lrnq_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) return ACKQ_NOMEM;
    ackq_options_t options = {0};
    options.name = name;
    options.durability = durable ? ACKQ_DURABLE : ACKQ_VOLATILE;
    options.max_attempts = max_attempts;
    options.begin_timeout_ms = 0;
    options.now_ms = clock;
    options.clock_ctx = clock_ctx;
    pthread_mutex_lock(&db->mutex);
    int status = install ? ackq_install(db->db, &options, &handle->queue) : ackq_open(db->db, &options, &handle->queue);
    if (status == ACKQ_OK) db->live_queues++;
    pthread_mutex_unlock(&db->mutex);
    if (status != ACKQ_OK) {
        free(handle);
        return status;
    }
    handle->db = db;
    *out = handle;
    return LRN_OK;
}

void lrnq_close(lrnq_t **queue)
{
    if (queue == NULL || *queue == NULL) return;
    lrndb_t *db = (*queue)->db;
    pthread_mutex_lock(&db->mutex);
    ackq_close(&(*queue)->queue);
    db->live_queues--;
    pthread_mutex_unlock(&db->mutex);
    free(*queue);
    *queue = NULL;
}

/* 큐 연산 공통: txn이 이 큐의 database 것이 아니면 잠그기 전에 거절하고, 아니면 mutex 안에서 call을 실행한다. */
#define QUEUE_CALL(q, txn, call)                                              \
    do {                                                                      \
        if ((q) == NULL || ((txn) != NULL && (txn)->db != (q)->db)) return ACKQ_INVALID; \
        pthread_mutex_lock(&(q)->db->mutex);                                  \
        status = (call);                                                      \
        pthread_mutex_unlock(&(q)->db->mutex);                                \
    } while (0)

static database_txn_t *raw(lrntxn_t *txn)
{
    return txn != NULL ? txn->txn : NULL;
}

int lrnq_put(lrnq_t *q, lrntxn_t *txn, const uint8_t *payload, size_t size, const char *idem_key, uint64_t *id,
             int *duplicate)
{
    int status;
    bool dup = false;
    QUEUE_CALL(q, txn, ackq_enqueue(q->queue, raw(txn), payload, size, idem_key, id, &dup));
    if (duplicate != NULL) *duplicate = dup ? 1 : 0;
    return status;
}

int lrnq_get(lrnq_t *q, lrntxn_t *txn, uint64_t id, uint64_t lease_ms, lrnq_item_t *item)
{
    int status;
    ackq_claim_t claim = {0};
    size_t count = 0;
    if (item == NULL) return ACKQ_INVALID;
    *item = (lrnq_item_t){0};
    if (id == 0) {
        QUEUE_CALL(q, txn, ackq_claim(q->queue, raw(txn), 1, lease_ms, &claim, &count));
        if (status == ACKQ_OK && count == 0) return LRN_EMPTY;
    } else {
        QUEUE_CALL(q, txn, ackq_claim_id(q->queue, raw(txn), id, lease_ms, &claim));
    }
    if (status != ACKQ_OK) return status;
    *item = (lrnq_item_t){claim.id, claim.fence, claim.attempts, claim.lease_until_ms, claim.payload, claim.payload_size};
    return LRN_OK;
}

void lrnq_item_free(lrnq_item_t *item)
{
    if (item == NULL) return;
    free(item->payload);
    *item = (lrnq_item_t){0};
}

int lrnq_ack(lrnq_t *q, lrntxn_t *txn, uint64_t id, uint64_t fence)
{
    int status;
    QUEUE_CALL(q, txn, ackq_ack(q->queue, raw(txn), id, fence));
    return status;
}

int lrnq_fail(lrnq_t *q, lrntxn_t *txn, uint64_t id, uint64_t fence, const char *reason)
{
    int status;
    QUEUE_CALL(q, txn, ackq_fail(q->queue, raw(txn), id, fence, reason));
    return status;
}

int lrnq_nack(lrnq_t *q, lrntxn_t *txn, uint64_t id, uint64_t fence, uint64_t retry_delay_ms, const char *reason,
              int *terminal)
{
    int status;
    bool end = false;
    QUEUE_CALL(q, txn, ackq_nack(q->queue, raw(txn), id, fence, retry_delay_ms, reason, &end));
    if (terminal != NULL) *terminal = end ? 1 : 0;
    return status;
}

int lrnq_update(lrnq_t *q, lrntxn_t *txn, uint64_t id, uint64_t fence, const uint8_t *payload, size_t size)
{
    int status;
    QUEUE_CALL(q, txn, ackq_update(q->queue, raw(txn), id, fence, payload, size));
    return status;
}

int lrnq_recover(lrnq_t *q, lrntxn_t *txn, size_t limit, size_t *recovered)
{
    int status;
    QUEUE_CALL(q, txn, ackq_recover(q->queue, raw(txn), limit, recovered));
    return status;
}

int lrnq_release_leases(lrnq_t *q, lrntxn_t *txn, size_t limit, size_t *released)
{
    int status;
    QUEUE_CALL(q, txn, ackq_release_leases(q->queue, raw(txn), limit, released));
    return status;
}

int lrnq_purge(lrnq_t *q, lrntxn_t *txn, int include_failed, size_t limit, size_t *purged)
{
    int status;
    QUEUE_CALL(q, txn, ackq_purge(q->queue, raw(txn), include_failed != 0, limit, purged));
    return status;
}

int lrnq_stats(lrnq_t *q, lrntxn_t *txn, uint64_t counts[4])
{
    int status;
    ackq_stats_t stats = {0};
    if (counts == NULL) return ACKQ_INVALID;
    QUEUE_CALL(q, txn, ackq_stats(q->queue, raw(txn), &stats));
    counts[0] = stats.ready;
    counts[1] = stats.leased;
    counts[2] = stats.acked;
    counts[3] = stats.failed;
    return status;
}

int lrnq_list(lrnq_t *q, lrntxn_t *txn, uint64_t after_id, size_t limit, int with_payload, lrnq_listitem_t *items,
              size_t *count)
{
    int status;
    if (count != NULL) *count = 0;
    if (items == NULL || count == NULL || limit == 0 || limit > ACKQ_MAX_CLAIM_BATCH) return ACKQ_INVALID;
    ackq_item_t *raw_items = calloc(limit, sizeof(*raw_items));
    if (raw_items == NULL) return ACKQ_NOMEM;
    size_t n = 0;
    if (q == NULL || (txn != NULL && txn->db != q->db)) {
        free(raw_items);
        return ACKQ_INVALID;
    }
    pthread_mutex_lock(&q->db->mutex);
    status = ackq_list(q->queue, raw(txn), after_id, limit, with_payload != 0, raw_items, &n);
    pthread_mutex_unlock(&q->db->mutex);
    if (status == ACKQ_OK) {
        for (size_t i = 0; i < n; i++) {
            const ackq_item_t *src = &raw_items[i];
            items[i] = (lrnq_listitem_t){.id = src->id,
                                         .state = (int32_t)src->state,
                                         .fence = src->fence,
                                         .attempts = src->attempts,
                                         .ready_at_ms = src->ready_at_ms,
                                         .lease_until_ms = src->lease_until_ms,
                                         .created_at_ms = src->created_at_ms,
                                         .payload = src->payload,
                                         .payload_size = src->payload_size};
            memcpy(items[i].note, src->note, sizeof(items[i].note));
            raw_items[i].payload = NULL;   /* 소유권을 넘겼다 */
        }
        *count = n;
    }
    free(raw_items);
    return status;
}

void lrnq_list_free(lrnq_listitem_t *items, size_t count)
{
    if (items == NULL) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].payload);
        items[i].payload = NULL;
    }
}
