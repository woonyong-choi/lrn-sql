/*
 * ack_queue_sql.c — 큐 연산이 세션으로 문장을 실행하고 세션의 트랜잭션을 여닫는 공통 부분.
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
        case RESULT_E_CANCELLED: return ACKQ_CANCELLED;
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

ackq_status_t ackq_scope_open(ackq_t *queue, session_t *session, bool write, ackq_scope_t *scope)
{
    *scope = (ackq_scope_t){session, false, false};
    /* 다른 데이터베이스의 세션으로 이 큐의 SQL을 실행하지 않는다. 어떤 문장도 실행하기 전에 거절한다. */
    if (session == NULL || session_database(session) != queue->db) return ACKQ_INVALID;
    if (session_txn_active(session)) return session_txn_failed(session) ? ACKQ_TXN_FAILED : ACKQ_OK;
    if (database_is_poisoned(session_db_handle(queue->db))) return ACKQ_POISONED;
    if (!write) return ACKQ_OK;
    session_txn_options_t options = {TXN_GUARANTEE_DURABLE, queue->begin_timeout_ms};
    txn_status_t status = session_txn_begin(session, &options);
    if (status != TXN_OK) return map_txn_status(status);
    scope->own = true;
    return ACKQ_OK;
}

ackq_status_t ackq_scope_close(ackq_scope_t *scope, ackq_status_t result)
{
    if (!scope->own) return result;
    ackq_status_t final = result;
    if (result == ACKQ_OK && scope->dirty) {
        txn_status_t status = session_txn_commit(scope->session, NULL);
        final = map_txn_status(status);   /* TXN_OK면 승인된 commit이므로 그 결과를 그대로 돌려준다 */
        /* 실패 상태라 commit하지 못한 트랜잭션은 세션에 남기지 않는다 */
        if (session_txn_active(scope->session)) session_txn_rollback(scope->session);
    } else {
        session_txn_rollback(scope->session);
    }
    scope->own = false;
    return final;
}

ackq_status_t ackq_run(ackq_t *queue, ackq_scope_t *scope, bool write, const char *sql, const value_view_t *params,
                       size_t param_count, result_t **out, result_error_class_t *sql_class)
{
    lexer_binding_t binding = {params, param_count};
    const lexer_binding_t *bound = param_count > 0 ? &binding : NULL;
    result_t *result = NULL;

    *out = NULL;
    if (sql_class != NULL) *sql_class = RESULT_E_NONE;
    if (write) scope->dirty = true;
    database_exec_status_t status = session_exec(scope->session, NULL, (const uint8_t *)sql, strlen(sql), bound, &result);
    if (status == DATABASE_EXEC_NOMEM) return ACKQ_NOMEM;
    if (status != DATABASE_EXEC_OK || result == NULL) return ACKQ_INVALID;
    if (result_kind(result) == RESULT_KIND_ERROR) {
        const result_outcome_t outcome = result_outcome(result);
        result_error_class_t error_class = outcome.error != NULL ? outcome.error->error_class : RESULT_E_INTERNAL;
        if (sql_class != NULL) *sql_class = error_class;
        result_release(&result);
        if (database_is_poisoned(session_db_handle(queue->db))) return ACKQ_POISONED;
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
                                        "POISONED", "TXN_FAILED", "INTERNAL", "CANCELLED"};
    return (size_t)status < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}
