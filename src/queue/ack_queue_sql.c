/*
 * ack_queue_sql.c — 큐 연산이 database에 문장을 실행하고 트랜잭션을 여닫는 공통 부분.
 */
#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <time.h>

#include "ack_queue_internal.h"

uint64_t ackq_now(const ackq_t *queue)
{
    if (queue->now_ms != NULL) return queue->now_ms(queue->clock_ctx);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

value_view_t ackq_int(int64_t value)
{
    return (value_view_t){VALUE_INTEGER, value, 0.0, NULL, 0};
}

value_view_t ackq_text(const char *text)
{
    size_t size = strlen(text);
    return (value_view_t){VALUE_TEXT, 0, 0.0, size > 0 ? (const uint8_t *)text : NULL, size};
}

value_view_t ackq_blob(const uint8_t *bytes, size_t size)
{
    return (value_view_t){VALUE_BLOB, 0, 0.0, size > 0 ? bytes : NULL, size};
}

bool ackq_to_i64(uint64_t value, int64_t *out)
{
    if (value > (uint64_t)INT64_MAX) return false;
    *out = (int64_t)value;
    return true;
}

static ackq_status_t map_error_class(result_error_class_t error_class)
{
    switch (error_class) {
        case RESULT_E_NOMEM: return ACKQ_NOMEM;
        case RESULT_E_LIMIT: return ACKQ_LIMIT;
        case RESULT_E_IO: return ACKQ_IO;
        case RESULT_E_BUSY: return ACKQ_BUSY;
        default: return ACKQ_INTERNAL;
    }
}

static ackq_status_t map_txn_status(txn_status_t status)
{
    switch (status) {
        case TXN_OK: return ACKQ_OK;
        case TXN_BUSY:
        case TXN_TIMEOUT: return ACKQ_BUSY;
        case TXN_NOMEM: return ACKQ_NOMEM;
        case TXN_LIMIT: return ACKQ_LIMIT;
        case TXN_POISONED: return ACKQ_POISONED;
        case TXN_UNSUPPORTED: return ACKQ_UNSUPPORTED;
        case TXN_IO: return ACKQ_IO;
        case TXN_COMMIT_FAILED: return ACKQ_NOT_APPLIED;
        case TXN_COMMIT_UNCERTAIN: return ACKQ_UNCERTAIN;
        default: return ACKQ_INTERNAL;
    }
}

ackq_status_t ackq_scope_open(ackq_t *queue, database_txn_t *given, bool write, ackq_scope_t *scope)
{
    *scope = (ackq_scope_t){NULL, false, false};
    if (given != NULL) {
        /* 다른 database의 트랜잭션으로 이 큐의 SQL을 실행하지 않는다. 어떤 문장도 실행하기 전에 거절한다. */
        if (!database_txn_belongs_to(given, queue->db)) return ACKQ_INVALID;
        if (database_txn_state(given) != TXN_STATE_ACTIVE) return ACKQ_INVALID;
        if (database_txn_failed(given)) return ACKQ_TXN_FAILED;
        scope->txn = given;
        return ACKQ_OK;
    }
    if (database_is_poisoned(queue->db)) return ACKQ_POISONED;
    if (!write) return ACKQ_OK;
    txn_begin_options_t options = {.access = TXN_ACCESS_READ_WRITE,
                                   .isolation = TXN_ISOLATION_SNAPSHOT,
                                   .guarantee = queue->guarantee,
                                   .timeout_ms = queue->begin_timeout_ms};
    txn_status_t status = database_txn_begin(queue->db, &options, &scope->txn);
    /* 트랜잭션 계층이 없는 database(읽기 전용으로 열었거나 database_open으로 연 것)는 쓰기를 할 수 없다 */
    if (status == TXN_INVALID) return ACKQ_UNSUPPORTED;
    if (status != TXN_OK) return map_txn_status(status);
    scope->own = true;
    return ACKQ_OK;
}

ackq_status_t ackq_scope_close(ackq_scope_t *scope, ackq_status_t result)
{
    if (!scope->own || scope->txn == NULL) return result;
    ackq_status_t final = result;
    if (result == ACKQ_OK && scope->dirty) {
        txn_status_t status = database_txn_commit(scope->txn, NULL);
        final = map_txn_status(status);   /* TXN_OK면 승인된 commit이므로 그 결과를 그대로 돌려준다 */
    } else {
        database_txn_rollback(scope->txn);
    }
    database_txn_release(&scope->txn);
    scope->own = false;
    return final;
}

ackq_status_t ackq_run(ackq_t *queue, ackq_scope_t *scope, bool write, const char *sql, const value_view_t *params,
                       size_t param_count, result_t **out, result_error_class_t *sql_class)
{
    lexer_binding_t binding = {params, param_count};
    const lexer_binding_t *bound = param_count > 0 ? &binding : NULL;
    const database_exec_options_t *options = queue->has_exec_opts ? &queue->exec_opts : NULL;
    result_t *result = NULL;
    database_exec_status_t status;

    *out = NULL;
    if (sql_class != NULL) *sql_class = RESULT_E_NONE;
    if (write) scope->dirty = true;
    if (scope->txn != NULL) {
        status = database_txn_exec(scope->txn, options, (const uint8_t *)sql, strlen(sql), bound, &result);
    } else {
        status = database_exec_ex(queue->db, options, (const uint8_t *)sql, strlen(sql), bound, &result);
    }
    if (status == DATABASE_EXEC_NOMEM) return ACKQ_NOMEM;
    if (status != DATABASE_EXEC_OK || result == NULL) return ACKQ_INVALID;
    if (result_kind(result) == RESULT_KIND_ERROR) {
        const result_outcome_t outcome = result_outcome(result);
        result_error_class_t error_class = outcome.error != NULL ? outcome.error->error_class : RESULT_E_INTERNAL;
        if (sql_class != NULL) *sql_class = error_class;
        result_release(&result);
        if (database_is_poisoned(queue->db)) return ACKQ_POISONED;
        return map_error_class(error_class);
    }
    *out = result;
    return ACKQ_OK;
}

ackq_status_t ackq_finish_rows(result_next_t last, result_t *result)
{
    if (last == RESULT_NEXT_DONE) return ACKQ_OK;
    const result_outcome_t outcome = result_outcome(result);
    return map_error_class(outcome.error != NULL ? outcome.error->error_class : RESULT_E_INTERNAL);
}

ackq_status_t ackq_expect_affected(const result_t *result, uint64_t expected)
{
    const result_command_t *command = result_command(result);
    return command != NULL && command->affected_rows == expected ? ACKQ_OK : ACKQ_INTERNAL;
}

const char *ackq_status_name(ackq_status_t status)
{
    static const char *const names[] = {"OK", "INVALID", "NOMEM", "LIMIT", "BUSY", "NOT_INSTALLED", "NOT_FOUND",
                                        "STALE", "CONFLICT", "UNSUPPORTED", "IO", "NOT_APPLIED", "UNCERTAIN",
                                        "POISONED", "TXN_FAILED", "INTERNAL"};
    return (size_t)status < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}
