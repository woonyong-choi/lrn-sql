/*
 * ack_queue.c — 선택형 ACK 큐(#140). database의 공개 SQL과 트랜잭션 API만 쓴다.
 *
 * 테이블 ackq_<name>: id(identity), state(0 READY 1 LEASED 2 ACKED 3 FAILED), ready_at, lease_until(밀리초),
 * fence(claim마다 +1), attempts(claim 횟수), idem(idempotency key, 없으면 ''), payload(BLOB), note(마지막 이유).
 * ORDER BY·LIMIT·AND·UPDATE 계산식이 없어서 상태별 메타데이터를 한 행씩 읽어 호출자 쪽에서 id가 가장 작은 k개를 고른다.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ack_queue_internal.h"

#define META_COLS "id, state, ready_at, lease_until, fence, attempts"
#define META_CELLS 6u
#define NOTE_MAX_ATTEMPTS "max attempts exceeded"

typedef struct {
    uint64_t id;
    int64_t state;
    int64_t ready_at;
    int64_t lease_until;
    int64_t fence;
    int64_t attempts;
} meta_t;

typedef bool (*meta_filter_t)(const meta_t *meta, void *ctx);

/* id 오름차순으로 가장 작은 cap개만 보관한다. */
typedef struct {
    meta_t *items;
    size_t count;
    size_t cap;
} topk_t;

static void topk_add(topk_t *top, const meta_t *meta)
{
    size_t pos = top->count;
    while (pos > 0 && top->items[pos - 1].id > meta->id) pos--;
    if (pos >= top->cap) return;
    size_t end = top->count < top->cap ? top->count : top->cap - 1;
    memmove(&top->items[pos + 1], &top->items[pos], (end - pos) * sizeof(meta_t));
    top->items[pos] = *meta;
    if (top->count < top->cap) top->count++;
}

static bool read_meta(const result_row_t *row, meta_t *meta)
{
    if (row->cell_count < META_CELLS) return false;
    for (size_t i = 0; i < META_CELLS; i++) {
        if (row->cells[i].type != VALUE_INTEGER) return false;
    }
    meta->id = (uint64_t)row->cells[0].integer;
    meta->state = row->cells[1].integer;
    meta->ready_at = row->cells[2].integer;
    meta->lease_until = row->cells[3].integer;
    meta->fence = row->cells[4].integer;
    meta->attempts = row->cells[5].integer;
    return true;
}

static bool valid_name(const char *name)
{
    size_t length = name == NULL ? 0 : strlen(name);
    if (length == 0 || length > ACKQ_NAME_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

/* NULL이거나 비어 있으면 길이 0. 길이 제한과 UTF-8을 확인한다. */
static ackq_status_t check_text(const char *text, size_t max_bytes, bool allow_empty, size_t *length)
{
    *length = text == NULL ? 0 : strlen(text);
    if (*length == 0) return allow_empty ? ACKQ_OK : ACKQ_INVALID;
    if (*length > max_bytes) return ACKQ_LIMIT;
    value_view_t view = {VALUE_TEXT, 0, 0.0, (const uint8_t *)text, *length};
    size_t chars;
    return value_text_chars(&view, &chars) == VALUE_OK ? ACKQ_OK : ACKQ_INVALID;
}

static ackq_status_t check_payload(const uint8_t *payload, size_t size)
{
    if (size > ACKQ_MAX_PAYLOAD_BYTES) return ACKQ_LIMIT;
    if (size > 0 && payload == NULL) return ACKQ_INVALID;
    return ACKQ_OK;
}

static ackq_status_t add_ms(uint64_t base, uint64_t delta, int64_t *out)
{
    if (delta > ACKQ_MAX_LEASE_MS) return ACKQ_INVALID;
    uint64_t sum = base + delta;
    if (sum < base || sum > (uint64_t)INT64_MAX) return ACKQ_LIMIT;
    *out = (int64_t)sum;
    return ACKQ_OK;
}

/* 메타데이터 SELECT를 한 행씩 읽어 filter를 통과한 행을 top에 넣는다. */
static ackq_status_t scan_meta(ackq_t *queue, ackq_scope_t *scope, const char *where, const value_view_t *param,
                               meta_filter_t filter, void *ctx, topk_t *top)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    snprintf(sql, sizeof(sql), "SELECT " META_COLS " FROM %s%s", queue->table, where);
    ackq_status_t status = ackq_run(queue, scope, false, sql, param, param != NULL ? 1 : 0, &result, NULL);
    if (status != ACKQ_OK) return status;
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
        meta_t meta;
        if (!read_meta(&row, &meta)) {
            status = ACKQ_INTERNAL;
            continue;
        }
        if (filter == NULL || filter(&meta, ctx)) topk_add(top, &meta);
    }
    ackq_status_t finished = ackq_finish_rows(next, result);
    result_release(&result);
    return status != ACKQ_OK ? status : finished;
}

static ackq_status_t fetch_meta(ackq_t *queue, ackq_scope_t *scope, uint64_t id, meta_t *meta)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    int64_t key;
    if (!ackq_to_i64(id, &key)) return ACKQ_NOT_FOUND;
    snprintf(sql, sizeof(sql), "SELECT " META_COLS " FROM %s WHERE id = ?", queue->table);
    value_view_t param = ackq_int(key);
    ackq_status_t status = ackq_run(queue, scope, false, sql, &param, 1, &result, NULL);
    if (status != ACKQ_OK) return status;
    bool found = false;
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
        if (read_meta(&row, meta)) found = true;
        else status = ACKQ_INTERNAL;
    }
    ackq_status_t finished = ackq_finish_rows(next, result);
    result_release(&result);
    if (status != ACKQ_OK) return status;
    if (finished != ACKQ_OK) return finished;
    return found ? ACKQ_OK : ACKQ_NOT_FOUND;
}

/* WHERE id = ?로 한 행을 고치거나 지우는 문장. 영향 행이 정확히 하나여야 한다. */
static ackq_status_t run_one_row(ackq_t *queue, ackq_scope_t *scope, const char *sql, const value_view_t *params,
                                 size_t count)
{
    result_t *result = NULL;
    ackq_status_t status = ackq_run(queue, scope, true, sql, params, count, &result, NULL);
    if (status != ACKQ_OK) return status;
    status = ackq_expect_affected(result, 1);
    result_release(&result);
    return status;
}

/* 한 행의 컬럼 하나(note·payload)를 읽어 복사한다. TEXT면 NUL로 끝내 돌려준다. 빈 값은 *bytes가 NULL이다. */
static ackq_status_t fetch_column(ackq_t *queue, ackq_scope_t *scope, const char *column, uint64_t id,
                                  uint8_t **bytes, size_t *size)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE id = ?", column, queue->table);
    value_view_t param = ackq_int((int64_t)id);
    ackq_status_t status = ackq_run(queue, scope, false, sql, &param, 1, &result, NULL);
    if (status != ACKQ_OK) return status;
    *bytes = NULL;
    *size = 0;
    bool found = false;
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
        const value_view_t *cell = row.cell_count > 0 ? &row.cells[0] : NULL;
        if (cell == NULL || (cell->type != VALUE_BLOB && cell->type != VALUE_TEXT)) {
            status = ACKQ_INTERNAL;
            continue;
        }
        found = true;
        if (cell->size_bytes > 0 && status == ACKQ_OK) {
            *bytes = malloc(cell->size_bytes + 1);
            if (*bytes == NULL) {
                status = ACKQ_NOMEM;
                continue;
            }
            memcpy(*bytes, cell->bytes, cell->size_bytes);
            (*bytes)[cell->size_bytes] = '\0';
            *size = cell->size_bytes;
        }
    }
    ackq_status_t finished = ackq_finish_rows(next, result);
    result_release(&result);
    if (status == ACKQ_OK) status = finished;
    if (status == ACKQ_OK && !found) status = ACKQ_NOT_FOUND;
    if (status != ACKQ_OK) {
        free(*bytes);
        *bytes = NULL;
        *size = 0;
    }
    return status;
}

static ackq_status_t fetch_created(ackq_t *queue, ackq_scope_t *scope, uint64_t id, uint64_t *created)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    snprintf(sql, sizeof(sql), "SELECT created_at FROM %s WHERE id = ?", queue->table);
    value_view_t param = ackq_int((int64_t)id);
    ackq_status_t status = ackq_run(queue, scope, false, sql, &param, 1, &result, NULL);
    if (status != ACKQ_OK) return status;
    bool found = false;
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
        if (row.cell_count < 1 || row.cells[0].type != VALUE_INTEGER) {
            status = ACKQ_INTERNAL;
            continue;
        }
        *created = (uint64_t)row.cells[0].integer;
        found = true;
    }
    ackq_status_t finished = ackq_finish_rows(next, result);
    result_release(&result);
    if (status == ACKQ_OK) status = finished;
    return status == ACKQ_OK && !found ? ACKQ_NOT_FOUND : status;
}

/* ── 설치·열기 ── */

static ackq_status_t make_queue(database_t *db, const ackq_options_t *options, ackq_t **out)
{
    *out = NULL;
    if (db == NULL || options == NULL || !valid_name(options->name)) return ACKQ_INVALID;
    if (options->durability != ACKQ_DURABLE && options->durability != ACKQ_VOLATILE) return ACKQ_INVALID;
    ackq_t *queue = calloc(1, sizeof(*queue));
    if (queue == NULL) return ACKQ_NOMEM;
    queue->db = db;
    snprintf(queue->name, sizeof(queue->name), "%s", options->name);
    snprintf(queue->table, sizeof(queue->table), "ackq_%s", options->name);
    queue->max_attempts = options->max_attempts;
    queue->begin_timeout_ms = options->begin_timeout_ms;
    queue->guarantee = options->durability == ACKQ_DURABLE ? TXN_GUARANTEE_DURABLE : TXN_GUARANTEE_SYNCED;
    queue->now_ms = options->now_ms;
    queue->clock_ctx = options->clock_ctx;
    if (options->ledger != NULL) {
        queue->exec_opts.ledger = options->ledger;
        queue->has_exec_opts = true;
    }
    *out = queue;
    return ACKQ_OK;
}

/* 테이블과 컬럼이 있는지만 본다. 없으면 NOT_INSTALLED(의미 오류), 그 밖의 오류는 그대로 돌려준다. */
static ackq_status_t probe_table(ackq_t *queue)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    result_error_class_t error_class = RESULT_E_NONE;
    ackq_scope_t scope = {NULL, false, false};
    snprintf(sql, sizeof(sql), "SELECT " META_COLS ", idem, payload, note, created_at FROM %s WHERE id = 0",
             queue->table);
    ackq_status_t status = ackq_run(queue, &scope, false, sql, NULL, 0, &result, &error_class);
    if (status == ACKQ_OK) {
        result_row_t row;
        result_next_t next;
        while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {}
        status = ackq_finish_rows(next, result);
        result_release(&result);
        return status;
    }
    return error_class == RESULT_E_SEMANTIC ? ACKQ_NOT_INSTALLED : status;
}

ackq_status_t ackq_open(database_t *db, const ackq_options_t *options, ackq_t **out)
{
    ackq_t *queue;
    if (out == NULL) return ACKQ_INVALID;
    ackq_status_t status = make_queue(db, options, &queue);
    if (status != ACKQ_OK) return status;
    status = probe_table(queue);
    if (status != ACKQ_OK) {
        free(queue);
        return status;
    }
    *out = queue;
    return ACKQ_OK;
}

static ackq_status_t create_table(ackq_t *queue)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    ackq_scope_t scope;
    ackq_status_t status = ackq_scope_open(queue, NULL, true, &scope);
    if (status != ACKQ_OK) return status;
    snprintf(sql, sizeof(sql),
             "CREATE TABLE %s (id INTEGER PRIMARY KEY AUTOINCREMENT, state INTEGER NOT NULL, "
             "ready_at INTEGER NOT NULL, lease_until INTEGER NOT NULL, fence INTEGER NOT NULL, "
             "attempts INTEGER NOT NULL, idem TEXT NOT NULL, payload BLOB NOT NULL, note TEXT NOT NULL, "
             "created_at INTEGER NOT NULL)",
             queue->table);
    status = ackq_run(queue, &scope, true, sql, NULL, 0, &result, NULL);
    if (status == ACKQ_OK) result_release(&result);
    return ackq_scope_close(&scope, status);
}

ackq_status_t ackq_install(database_t *db, const ackq_options_t *options, ackq_t **out)
{
    ackq_t *queue;
    if (out == NULL) return ACKQ_INVALID;
    ackq_status_t status = make_queue(db, options, &queue);
    if (status != ACKQ_OK) return status;
    status = probe_table(queue);
    if (status == ACKQ_NOT_INSTALLED) {
        status = create_table(queue);
        /* 다른 설치가 먼저 만들었을 수 있다. 같은 이름의 테이블이 있으면 그것을 쓴다. */
        if (status != ACKQ_OK) status = probe_table(queue) == ACKQ_OK ? ACKQ_OK : status;
    }
    if (status != ACKQ_OK) {
        free(queue);
        return status;
    }
    *out = queue;
    return ACKQ_OK;
}

void ackq_close(ackq_t **queue)
{
    if (queue == NULL || *queue == NULL) return;
    free(*queue);
    *queue = NULL;
}

/* ── enqueue ── */

static ackq_status_t find_idem(ackq_t *queue, ackq_scope_t *scope, const char *key, const uint8_t *payload,
                               size_t size, uint64_t *id, bool *found)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    snprintf(sql, sizeof(sql), "SELECT id, payload FROM %s WHERE idem = ?", queue->table);
    value_view_t param = ackq_text(key);
    ackq_status_t status = ackq_run(queue, scope, false, sql, &param, 1, &result, NULL);
    if (status != ACKQ_OK) return status;
    bool same = true;
    *found = false;
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
        if (row.cell_count < 2 || row.cells[0].type != VALUE_INTEGER || row.cells[1].type != VALUE_BLOB) {
            status = ACKQ_INTERNAL;
            continue;
        }
        *found = true;
        *id = (uint64_t)row.cells[0].integer;
        same = row.cells[1].size_bytes == size && (size == 0 || memcmp(row.cells[1].bytes, payload, size) == 0);
    }
    ackq_status_t finished = ackq_finish_rows(next, result);
    result_release(&result);
    if (status != ACKQ_OK) return status;
    if (finished != ACKQ_OK) return finished;
    return *found && !same ? ACKQ_CONFLICT : ACKQ_OK;
}

/* 방금 넣은 행의 id. identity 카운터는 단조라 새 id가 모든 기존 id보다 크므로 최댓값이다. */
static ackq_status_t newest_id(ackq_t *queue, ackq_scope_t *scope, uint64_t *id)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    snprintf(sql, sizeof(sql), "SELECT id FROM %s", queue->table);
    ackq_status_t status = ackq_run(queue, scope, false, sql, NULL, 0, &result, NULL);
    if (status != ACKQ_OK) return status;
    uint64_t best = 0;
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
        if (row.cell_count < 1 || row.cells[0].type != VALUE_INTEGER) {
            status = ACKQ_INTERNAL;
            continue;
        }
        if ((uint64_t)row.cells[0].integer > best) best = (uint64_t)row.cells[0].integer;
    }
    ackq_status_t finished = ackq_finish_rows(next, result);
    result_release(&result);
    if (status == ACKQ_OK) status = finished;
    if (status == ACKQ_OK) {
        if (best == 0) status = ACKQ_INTERNAL;
        else *id = best;
    }
    return status;
}

static ackq_status_t enqueue_in(ackq_t *queue, ackq_scope_t *scope, const uint8_t *payload, size_t size,
                                const char *key, uint64_t *id, bool *duplicate)
{
    char sql[ACKQ_SQL_MAX];
    int64_t now = (int64_t)ackq_now(queue);
    bool found = false;
    uint64_t existing = 0;
    ackq_status_t status = ACKQ_OK;
    if (key != NULL) {
        status = find_idem(queue, scope, key, payload, size, &existing, &found);
        if (status != ACKQ_OK) return status;
        if (found) {
            *id = existing;
            if (duplicate != NULL) *duplicate = true;
            return ACKQ_OK;
        }
    }
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s (state, ready_at, lease_until, fence, attempts, idem, payload, note, created_at) "
             "VALUES (0, ?, 0, 0, 0, ?, ?, '', ?)",
             queue->table);
    value_view_t params[4] = {ackq_int(now), ackq_text(key != NULL ? key : ""), ackq_blob(payload, size),
                              ackq_int(now)};
    status = run_one_row(queue, scope, sql, params, 4);
    if (status != ACKQ_OK) return status;
    status = newest_id(queue, scope, id);
    if (status == ACKQ_OK && duplicate != NULL) *duplicate = false;
    return status;
}

ackq_status_t ackq_enqueue(ackq_t *queue, database_txn_t *txn, const uint8_t *payload, size_t size,
                           const char *idem_key, uint64_t *id, bool *duplicate)
{
    size_t key_length = 0;
    if (queue == NULL || id == NULL) return ACKQ_INVALID;
    *id = 0;   /* 성공이 아니면 id를 주지 않는다 */
    if (duplicate != NULL) *duplicate = false;
    ackq_status_t status = check_payload(payload, size);
    if (status != ACKQ_OK) return status;
    if (idem_key != NULL) {
        status = check_text(idem_key, ACKQ_IDEM_KEY_MAX, false, &key_length);
        if (status != ACKQ_OK) return status;
    }
    ackq_scope_t scope;
    status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) return status;
    status = enqueue_in(queue, &scope, payload, size, idem_key, id, duplicate);
    status = ackq_scope_close(&scope, status);
    if (status != ACKQ_OK) {
        *id = 0;
        if (duplicate != NULL) *duplicate = false;
    }
    return status;
}

/* ── claim ── */

typedef struct {
    int64_t now;
    uint32_t max_attempts;
} claim_filter_t;

/* READY는 시도 횟수와 무관하게 때가 되면 claim한다. 횟수를 다 쓴 작업은 nack·만료 때 이미 FAILED가 된다. */
static bool ready_due(const meta_t *meta, void *ctx)
{
    const claim_filter_t *filter = ctx;
    return meta->ready_at <= filter->now;
}

static bool lease_expired(const meta_t *meta, void *ctx)
{
    const claim_filter_t *filter = ctx;
    bool attempts_left = filter->max_attempts == 0 || meta->attempts < (int64_t)filter->max_attempts;
    return meta->lease_until <= filter->now && attempts_left;
}

static ackq_status_t claim_one(ackq_t *queue, ackq_scope_t *scope, const meta_t *meta, int64_t lease_until,
                               ackq_claim_t *item)
{
    char sql[ACKQ_SQL_MAX];
    if (meta->fence == INT64_MAX || meta->attempts == INT64_MAX) return ACKQ_LIMIT;
    int64_t fence = meta->fence + 1;
    int64_t attempts = meta->attempts + 1;
    snprintf(sql, sizeof(sql), "UPDATE %s SET state = 1, lease_until = ?, fence = ?, attempts = ? WHERE id = ?",
             queue->table);
    value_view_t params[4] = {ackq_int(lease_until), ackq_int(fence), ackq_int(attempts), ackq_int((int64_t)meta->id)};
    ackq_status_t status = run_one_row(queue, scope, sql, params, 4);
    if (status != ACKQ_OK) return status;
    uint8_t *payload = NULL;
    size_t size = 0;
    status = fetch_column(queue, scope, "payload", meta->id, &payload, &size);
    if (status != ACKQ_OK) return status;
    *item = (ackq_claim_t){meta->id, (uint64_t)fence, (uint32_t)attempts, (uint64_t)lease_until, payload, size};
    return ACKQ_OK;
}

static ackq_status_t claim_in(ackq_t *queue, ackq_scope_t *scope, size_t max_items, int64_t now, int64_t lease_until,
                              ackq_claim_t *items, size_t *count)
{
    claim_filter_t filter = {now, queue->max_attempts};
    topk_t top = {calloc(max_items, sizeof(meta_t)), 0, max_items};
    if (top.items == NULL) return ACKQ_NOMEM;
    value_view_t ready = ackq_int(ACKQ_STATE_READY);
    value_view_t leased = ackq_int(ACKQ_STATE_LEASED);
    ackq_status_t status = scan_meta(queue, scope, " WHERE state = ?", &ready, ready_due, &filter, &top);
    if (status == ACKQ_OK) status = scan_meta(queue, scope, " WHERE state = ?", &leased, lease_expired, &filter, &top);
    for (size_t i = 0; status == ACKQ_OK && i < top.count; i++) {
        status = claim_one(queue, scope, &top.items[i], lease_until, &items[i]);
        if (status == ACKQ_OK) *count = i + 1;
    }
    free(top.items);
    return status;
}

ackq_status_t ackq_claim(ackq_t *queue, database_txn_t *txn, size_t max_items, uint64_t lease_ms,
                         ackq_claim_t *items, size_t *count)
{
    if (count != NULL) *count = 0;
    if (queue == NULL || items == NULL || count == NULL) return ACKQ_INVALID;
    if (max_items == 0 || max_items > ACKQ_MAX_CLAIM_BATCH || lease_ms == 0) return ACKQ_INVALID;
    uint64_t now = ackq_now(queue);
    int64_t lease_until, now_value;
    ackq_status_t status = add_ms(now, lease_ms, &lease_until);
    if (status != ACKQ_OK || !ackq_to_i64(now, &now_value)) return status != ACKQ_OK ? status : ACKQ_LIMIT;
    ackq_scope_t scope;
    status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) return status;
    status = claim_in(queue, &scope, max_items, now_value, lease_until, items, count);
    if (status == ACKQ_OK) status = ackq_scope_close(&scope, status);
    else ackq_scope_close(&scope, status);
    /* commit이 실패하거나 불확실하면 claim은 없던 일이다. 가져온 항목을 호출자에게 주지 않는다. */
    if (status != ACKQ_OK) {
        ackq_claims_release(items, *count);
        *count = 0;
    }
    return status;
}

ackq_status_t ackq_claim_id(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t lease_ms, ackq_claim_t *item)
{
    if (queue == NULL || item == NULL || id == 0 || lease_ms == 0) return ACKQ_INVALID;
    *item = (ackq_claim_t){0};
    uint64_t now = ackq_now(queue);
    int64_t lease_until, now_value;
    ackq_status_t status = add_ms(now, lease_ms, &lease_until);
    if (status != ACKQ_OK || !ackq_to_i64(now, &now_value)) return status != ACKQ_OK ? status : ACKQ_LIMIT;
    ackq_scope_t scope;
    status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) return status;
    meta_t meta;
    status = fetch_meta(queue, &scope, id, &meta);
    if (status == ACKQ_OK) {
        claim_filter_t filter = {now_value, queue->max_attempts};
        bool due = meta.state == ACKQ_STATE_READY ? ready_due(&meta, &filter)
                                                  : meta.state == ACKQ_STATE_LEASED && lease_expired(&meta, &filter);
        status = due ? claim_one(queue, &scope, &meta, lease_until, item) : ACKQ_STALE;
    }
    status = ackq_scope_close(&scope, status);
    if (status != ACKQ_OK) ackq_claims_release(item, 1);
    return status;
}

void ackq_claims_release(ackq_claim_t *items, size_t count)
{
    if (items == NULL) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].payload);
        items[i] = (ackq_claim_t){0};
    }
}

/* ── ack · fail · nack · update ── */

static bool fits_i64(uint64_t value)
{
    return value <= (uint64_t)INT64_MAX;
}

static ackq_status_t set_terminal(ackq_t *queue, ackq_scope_t *scope, uint64_t id, ackq_state_t state,
                                  const char *note)
{
    char sql[ACKQ_SQL_MAX];
    snprintf(sql, sizeof(sql), "UPDATE %s SET state = ?, lease_until = 0, note = ? WHERE id = ?", queue->table);
    value_view_t params[3] = {ackq_int(state), ackq_text(note), ackq_int((int64_t)id)};
    return run_one_row(queue, scope, sql, params, 3);
}

/* ack·fail 공통: 같은 claim이 LEASED면 target으로 바꾸고, 이미 target이면 중복 요청이라 OK다. */
static ackq_status_t finish_claim(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence,
                                  ackq_state_t target, const char *note)
{
    if (queue == NULL || id == 0 || fence == 0 || !fits_i64(fence)) return ACKQ_INVALID;
    ackq_scope_t scope;
    ackq_status_t status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) return status;
    meta_t meta;
    status = fetch_meta(queue, &scope, id, &meta);
    if (status == ACKQ_OK) {
        if (meta.fence != (int64_t)fence) status = ACKQ_STALE;
        else if (meta.state == (int64_t)target) status = ACKQ_OK;
        else if (meta.state != ACKQ_STATE_LEASED) status = ACKQ_STALE;
        else status = set_terminal(queue, &scope, id, target, note);
    }
    return ackq_scope_close(&scope, status);
}

ackq_status_t ackq_ack(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence)
{
    return finish_claim(queue, txn, id, fence, ACKQ_STATE_ACKED, "");
}

ackq_status_t ackq_fail(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence, const char *reason)
{
    size_t length;
    ackq_status_t status = check_text(reason, ACKQ_NOTE_MAX, true, &length);
    if (status != ACKQ_OK) return status;
    return finish_claim(queue, txn, id, fence, ACKQ_STATE_FAILED, reason != NULL ? reason : "");
}

static ackq_status_t nack_in(ackq_t *queue, ackq_scope_t *scope, const meta_t *meta, uint64_t fence,
                             uint64_t retry_delay_ms, const char *note, bool *terminal)
{
    char sql[ACKQ_SQL_MAX];
    if (meta->fence != (int64_t)fence) return ACKQ_STALE;
    if (meta->state == ACKQ_STATE_READY || meta->state == ACKQ_STATE_FAILED) {
        if (terminal != NULL) *terminal = meta->state == ACKQ_STATE_FAILED;
        return ACKQ_OK;   /* 같은 claim의 중복 nack */
    }
    if (meta->state != ACKQ_STATE_LEASED) return ACKQ_STALE;
    if (queue->max_attempts > 0 && meta->attempts >= (int64_t)queue->max_attempts) {
        if (terminal != NULL) *terminal = true;
        return set_terminal(queue, scope, meta->id, ACKQ_STATE_FAILED, note[0] != '\0' ? note : NOTE_MAX_ATTEMPTS);
    }
    int64_t ready_at;
    ackq_status_t status = add_ms(ackq_now(queue), retry_delay_ms, &ready_at);
    if (status != ACKQ_OK) return status;
    snprintf(sql, sizeof(sql), "UPDATE %s SET state = 0, ready_at = ?, lease_until = 0, note = ? WHERE id = ?",
             queue->table);
    value_view_t params[3] = {ackq_int(ready_at), ackq_text(note), ackq_int((int64_t)meta->id)};
    if (terminal != NULL) *terminal = false;
    return run_one_row(queue, scope, sql, params, 3);
}

ackq_status_t ackq_nack(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence, uint64_t retry_delay_ms,
                        const char *reason, bool *terminal)
{
    size_t length;
    if (terminal != NULL) *terminal = false;
    if (queue == NULL || id == 0 || fence == 0 || !fits_i64(fence)) return ACKQ_INVALID;
    ackq_status_t status = check_text(reason, ACKQ_NOTE_MAX, true, &length);
    if (status != ACKQ_OK) return status;
    ackq_scope_t scope;
    status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) return status;
    meta_t meta;
    status = fetch_meta(queue, &scope, id, &meta);
    if (status == ACKQ_OK) {
        status = nack_in(queue, &scope, &meta, fence, retry_delay_ms, reason != NULL ? reason : "", terminal);
    }
    return ackq_scope_close(&scope, status);
}

ackq_status_t ackq_update(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence, const uint8_t *payload,
                          size_t size)
{
    char sql[ACKQ_SQL_MAX];
    if (queue == NULL || id == 0 || !fits_i64(fence)) return ACKQ_INVALID;
    ackq_status_t status = check_payload(payload, size);
    if (status != ACKQ_OK) return status;
    ackq_scope_t scope;
    status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) return status;
    meta_t meta;
    status = fetch_meta(queue, &scope, id, &meta);
    if (status == ACKQ_OK) {
        bool allowed = fence == 0 ? meta.state == ACKQ_STATE_READY
                                  : meta.state == ACKQ_STATE_LEASED && meta.fence == (int64_t)fence;
        if (!allowed) {
            status = ACKQ_STALE;
        } else {
            snprintf(sql, sizeof(sql), "UPDATE %s SET payload = ? WHERE id = ?", queue->table);
            value_view_t params[2] = {ackq_blob(payload, size), ackq_int((int64_t)id)};
            status = run_one_row(queue, &scope, sql, params, 2);
        }
    }
    return ackq_scope_close(&scope, status);
}

/* ── 회복·정리·조회 ── */

static bool any_lease(const meta_t *meta, void *ctx)
{
    (void)meta;
    (void)ctx;
    return true;
}

static ackq_status_t return_one(ackq_t *queue, ackq_scope_t *scope, const meta_t *meta)
{
    char sql[ACKQ_SQL_MAX];
    if (queue->max_attempts > 0 && meta->attempts >= (int64_t)queue->max_attempts) {
        return set_terminal(queue, scope, meta->id, ACKQ_STATE_FAILED, NOTE_MAX_ATTEMPTS);
    }
    snprintf(sql, sizeof(sql), "UPDATE %s SET state = 0, lease_until = 0 WHERE id = ?", queue->table);
    value_view_t param = ackq_int((int64_t)meta->id);
    return run_one_row(queue, scope, sql, &param, 1);
}

static ackq_status_t return_leases(ackq_t *queue, database_txn_t *txn, size_t limit, bool only_expired,
                                   size_t *processed)
{
    if (processed != NULL) *processed = 0;
    if (queue == NULL || limit == 0 || limit > ACKQ_MAX_CLAIM_BATCH) return ACKQ_INVALID;
    uint64_t now = ackq_now(queue);
    claim_filter_t filter = {(int64_t)now, 0};   /* 시도 횟수 조건 없이 만료만 본다 */
    topk_t top = {calloc(limit, sizeof(meta_t)), 0, limit};
    if (top.items == NULL) return ACKQ_NOMEM;
    ackq_scope_t scope;
    ackq_status_t status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) {
        free(top.items);
        return status;
    }
    value_view_t leased = ackq_int(ACKQ_STATE_LEASED);
    status = scan_meta(queue, &scope, " WHERE state = ?", &leased, only_expired ? lease_expired : any_lease, &filter,
                       &top);
    size_t done = 0;
    for (size_t i = 0; status == ACKQ_OK && i < top.count; i++) {
        status = return_one(queue, &scope, &top.items[i]);
        if (status == ACKQ_OK) done++;
    }
    free(top.items);
    if (status == ACKQ_OK && processed != NULL) *processed = done;
    return ackq_scope_close(&scope, status);
}

ackq_status_t ackq_recover(ackq_t *queue, database_txn_t *txn, size_t limit, size_t *recovered)
{
    return return_leases(queue, txn, limit, true, recovered);
}

ackq_status_t ackq_release_leases(ackq_t *queue, database_txn_t *txn, size_t limit, size_t *released)
{
    return return_leases(queue, txn, limit, false, released);
}

ackq_status_t ackq_purge(ackq_t *queue, database_txn_t *txn, bool include_failed, size_t limit, size_t *purged)
{
    char sql[ACKQ_SQL_MAX];
    if (purged != NULL) *purged = 0;
    if (queue == NULL || limit == 0 || limit > ACKQ_MAX_CLAIM_BATCH) return ACKQ_INVALID;
    topk_t top = {calloc(limit, sizeof(meta_t)), 0, limit};
    if (top.items == NULL) return ACKQ_NOMEM;
    ackq_scope_t scope;
    ackq_status_t status = ackq_scope_open(queue, txn, true, &scope);
    if (status != ACKQ_OK) {
        free(top.items);
        return status;
    }
    value_view_t acked = ackq_int(ACKQ_STATE_ACKED);
    value_view_t failed = ackq_int(ACKQ_STATE_FAILED);
    status = scan_meta(queue, &scope, " WHERE state = ?", &acked, NULL, NULL, &top);
    if (status == ACKQ_OK && include_failed) status = scan_meta(queue, &scope, " WHERE state = ?", &failed, NULL, NULL, &top);
    size_t done = 0;
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id = ?", queue->table);
    for (size_t i = 0; status == ACKQ_OK && i < top.count; i++) {
        value_view_t param = ackq_int((int64_t)top.items[i].id);
        status = run_one_row(queue, &scope, sql, &param, 1);
        if (status == ACKQ_OK) done++;
    }
    free(top.items);
    if (status == ACKQ_OK && purged != NULL) *purged = done;
    return ackq_scope_close(&scope, status);
}

ackq_status_t ackq_stats(ackq_t *queue, database_txn_t *txn, ackq_stats_t *stats)
{
    char sql[ACKQ_SQL_MAX];
    result_t *result = NULL;
    if (queue == NULL || stats == NULL) return ACKQ_INVALID;
    *stats = (ackq_stats_t){0};
    ackq_scope_t scope;
    ackq_status_t status = ackq_scope_open(queue, txn, false, &scope);
    if (status != ACKQ_OK) return status;
    snprintf(sql, sizeof(sql), "SELECT state FROM %s", queue->table);
    ackq_stats_t counted = {0};
    status = ackq_run(queue, &scope, false, sql, NULL, 0, &result, NULL);
    if (status == ACKQ_OK) {
        result_row_t row;
        result_next_t next;
        while ((next = result_next(result, &row)) == RESULT_NEXT_ROW) {
            int64_t state = row.cell_count > 0 && row.cells[0].type == VALUE_INTEGER ? row.cells[0].integer : -1;
            if (state == ACKQ_STATE_READY) counted.ready++;
            else if (state == ACKQ_STATE_LEASED) counted.leased++;
            else if (state == ACKQ_STATE_ACKED) counted.acked++;
            else if (state == ACKQ_STATE_FAILED) counted.failed++;
            else status = ACKQ_INTERNAL;
        }
        ackq_status_t finished = ackq_finish_rows(next, result);
        result_release(&result);
        if (status == ACKQ_OK) status = finished;
    }
    if (status == ACKQ_OK) *stats = counted;
    return ackq_scope_close(&scope, status);
}

static ackq_status_t fill_item(ackq_t *queue, ackq_scope_t *scope, const meta_t *meta, bool with_payload,
                               ackq_item_t *item)
{
    uint8_t *note = NULL;
    size_t note_size = 0;
    *item = (ackq_item_t){.id = meta->id,
                          .state = (ackq_state_t)meta->state,
                          .fence = (uint64_t)meta->fence,
                          .attempts = (uint32_t)meta->attempts,
                          .ready_at_ms = (uint64_t)meta->ready_at,
                          .lease_until_ms = (uint64_t)meta->lease_until};
    ackq_status_t status = fetch_column(queue, scope, "note", meta->id, &note, &note_size);
    if (status != ACKQ_OK) return status;
    if (note_size > ACKQ_NOTE_MAX) note_size = ACKQ_NOTE_MAX;
    if (note != NULL) memcpy(item->note, note, note_size);
    item->note[note_size] = '\0';
    free(note);
    status = fetch_created(queue, scope, meta->id, &item->created_at_ms);
    if (status == ACKQ_OK && with_payload) {
        status = fetch_column(queue, scope, "payload", meta->id, &item->payload, &item->payload_size);
    }
    return status;
}

ackq_status_t ackq_list(ackq_t *queue, database_txn_t *txn, uint64_t after_id, size_t limit, bool with_payload,
                        ackq_item_t *items, size_t *count)
{
    if (count != NULL) *count = 0;
    if (queue == NULL || items == NULL || count == NULL || limit == 0 || limit > ACKQ_MAX_CLAIM_BATCH ||
        !fits_i64(after_id)) {
        return ACKQ_INVALID;
    }
    topk_t top = {calloc(limit, sizeof(meta_t)), 0, limit};
    if (top.items == NULL) return ACKQ_NOMEM;
    ackq_scope_t scope;
    ackq_status_t status = ackq_scope_open(queue, txn, false, &scope);
    if (status != ACKQ_OK) {
        free(top.items);
        return status;
    }
    value_view_t after = ackq_int((int64_t)after_id);
    status = scan_meta(queue, &scope, " WHERE id > ?", &after, NULL, NULL, &top);
    size_t filled = 0;
    for (size_t i = 0; status == ACKQ_OK && i < top.count; i++) {
        status = fill_item(queue, &scope, &top.items[i], with_payload, &items[i]);
        if (status == ACKQ_OK) filled = i + 1;
        else free(items[i].payload);
    }
    free(top.items);
    if (status != ACKQ_OK) {
        ackq_list_release(items, filled);
        filled = 0;
    }
    *count = filled;
    return ackq_scope_close(&scope, status);
}

void ackq_list_release(ackq_item_t *items, size_t count)
{
    if (items == NULL) return;
    for (size_t i = 0; i < count; i++) {
        free(items[i].payload);
        items[i] = (ackq_item_t){0};
    }
}
