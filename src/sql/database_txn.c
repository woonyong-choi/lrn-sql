#include "sql/database_txn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "database_internal.h"
#include "sql/result.h"

/* 트랜잭션 계층 하나. database가 쥐고 database_close가 부르는 close에서 해제한다. */
typedef struct {
    database_t *db;
    txn_manager_t *manager;
    uint32_t autocommit_timeout_ms;
} layer_t;

struct database_txn {
    layer_t *layer;
    txn_t *txn;
};

/* ── database 계층 접점 ── */

static database_exec_status_t error_from_txn(database_t *db, const database_exec_options_t *opts, txn_status_t st,
                                             result_t **out)
{
    switch (st) {
        case TXN_POISONED:
            return database_error_result(db, opts, RESULT_E_IO, 19,
                                         "트랜잭션 계층이 불확정한 commit 또는 I/O 실패로 더 쓸 수 없습니다. 다시 여세요", out);
        case TXN_LIMIT:
            return database_error_result(db, opts, RESULT_E_LIMIT, 30, "트랜잭션 번호 한도를 넘었습니다", out);
        case TXN_NOMEM:
            return DATABASE_EXEC_NOMEM;
        default:
            return database_error_result(db, opts, RESULT_E_INTERNAL, 31, "트랜잭션을 시작하지 못했습니다", out);
    }
}

/* 트랜잭션 밖에서 들어온 문장. 문장마다 자동 commit이다 */
static database_exec_status_t layer_exec(void *ctx, database_t *db, const database_exec_options_t *opts,
                                         const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                         result_t **out)
{
    layer_t *l = ctx;
    txn_begin_options_t begin = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_SNAPSHOT, TXN_GUARANTEE_DURABLE,
                                 l->autocommit_timeout_ms, NULL};
    txn_t *txn = NULL;
    txn_status_t st = txn_begin(l->manager, &begin, &txn);
    if (st == TXN_BUSY || st == TXN_TIMEOUT) {
        /* 다른 트랜잭션이 쓰는 중: 게시된 상태만 읽는다. 변경 문장은 BUSY 오류 결과로 끝난다 */
        return database_exec_read_only(db, opts, sql, size, binding, out);
    }
    if (st != TXN_OK) return error_from_txn(db, opts, st, out);

    database_exec_status_t status = database_exec_in_work(db, opts, sql, size, binding, out);
    bool ok = status == DATABASE_EXEC_OK && *out != NULL && result_kind(*out) != RESULT_KIND_ERROR;
    if (!ok) {
        txn_release(&txn);
        return status;
    }
    txn_commit_info_t info;
    st = txn_commit(txn, &info);
    txn_release(&txn);
    if (st == TXN_OK) return DATABASE_EXEC_OK;
    /* 성공 결과를 만들었지만 반영되지 않았거나 알 수 없다. 성공으로 응답하지 않는다 */
    result_release(out);
    *out = NULL;
    if (st == TXN_COMMIT_UNCERTAIN) {
        return database_error_result(db, opts, RESULT_E_IO, 32,
                                     "커밋 결과를 알 수 없습니다. 데이터베이스를 닫고 다시 열어 확인하세요", out);
    }
    if (st == TXN_COMMIT_FAILED || st == TXN_CANCELLED) {
        return database_error_result(db, opts, RESULT_E_IO, 33, "커밋하지 못했습니다. 아무것도 반영하지 않았습니다", out);
    }
    return error_from_txn(db, opts, st, out);
}

static database_status_t layer_close(void *ctx, database_t *db)
{
    layer_t *l = ctx;
    (void)db;
    txn_status_t closed = txn_manager_close(&l->manager);
    if (closed == TXN_BUSY) return DATABASE_BUSY;
    free(l);
    return DATABASE_OK;
}

static const database_layer_t LAYER = {layer_exec, layer_close};

/* ── transaction manager backend ── */

static txn_status_t be_write_begin(void *ctx, uint64_t base_seq, void **write)
{
    layer_t *l = ctx;
    (void)base_seq;
    database_status_t st = database_work_begin(l->db);
    switch (st) {
        case DATABASE_OK: break;
        case DATABASE_LOCKED: return TXN_BUSY;  /* 다른 핸들(다른 프로세스)이 쓰는 중 */
        case DATABASE_NOMEM: return TXN_NOMEM;
        case DATABASE_IO:
        case DATABASE_CORRUPT: return TXN_IO;
        default: return TXN_INVALID;
    }
    *write = l;
    return TXN_OK;
}

/*
 * 게시는 COW다. 새 페이지와 카탈로그를 쓰고 fsync한 뒤 제어 페이지 한 장으로 세대를 바꾸고 다시 fsync한다.
 * 성공을 돌려준 게시는 프로세스·OS 종료 뒤에도 남으므로 요청한 보증과 무관하게 DURABLE을 달성한다.
 */
static txn_publish_result_t be_publish(void *ctx, void *write, uint64_t commit_seq, txn_guarantee_t requested)
{
    layer_t *l = ctx;
    (void)write;
    (void)commit_seq;
    (void)requested;
    if (database_work_failed(l->db)) return (txn_publish_result_t){TXN_OUTCOME_NOT_APPLIED, TXN_GUARANTEE_VISIBLE};
    database_work_result_t r = database_work_commit(l->db);
    if (r.outcome == DATABASE_WORK_APPLIED) return (txn_publish_result_t){TXN_OUTCOME_APPLIED, TXN_GUARANTEE_DURABLE};
    txn_outcome_t outcome = r.outcome == DATABASE_WORK_UNKNOWN ? TXN_OUTCOME_UNKNOWN : TXN_OUTCOME_NOT_APPLIED;
    return (txn_publish_result_t){outcome, TXN_GUARANTEE_VISIBLE};
}

static void be_write_finish(void *ctx, void *write, txn_outcome_t outcome, uint64_t commit_seq)
{
    layer_t *l = ctx;
    (void)write;
    (void)commit_seq;
    database_work_outcome_t end = outcome == TXN_OUTCOME_APPLIED ? DATABASE_WORK_APPLIED
                                  : outcome == TXN_OUTCOME_UNKNOWN ? DATABASE_WORK_UNKNOWN
                                                                   : DATABASE_WORK_NOT_APPLIED;
    database_work_end(l->db, end);
}

static bool be_is_poisoned(void *ctx)
{
    layer_t *l = ctx;
    return database_is_poisoned(l->db);
}

static const txn_backend_ops_t BACKEND = {be_write_begin, be_publish, be_write_finish, NULL, be_is_poisoned, NULL};

/* ── 공개 트랜잭션 ── */

txn_status_t database_txn_begin(database_t *db, const txn_begin_options_t *options, database_txn_t **out)
{
    if (db == NULL || out == NULL) return TXN_INVALID;
    *out = NULL;
    layer_t *l = database_layer_ctx(db);
    if (l == NULL) return TXN_UNSUPPORTED;
    txn_begin_options_t o = options != NULL ? *options : (txn_begin_options_t){0};
    if (o.access != TXN_ACCESS_READ_WRITE) return TXN_UNSUPPORTED;
    database_txn_t *t = calloc(1, sizeof(*t));
    if (t == NULL) return TXN_NOMEM;
    txn_status_t st = txn_begin(l->manager, &o, &t->txn);
    if (st != TXN_OK) {
        free(t);
        return st;
    }
    t->layer = l;
    *out = t;
    return TXN_OK;
}

database_exec_status_t database_txn_exec(database_txn_t *txn, const database_exec_options_t *opts,
                                         const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                         result_t **out)
{
    if (txn == NULL || out == NULL || txn_state(txn->txn) != TXN_STATE_ACTIVE) return DATABASE_EXEC_INVALID;
    return database_exec_in_work(txn->layer->db, opts, sql, size, binding, out);
}

bool database_txn_failed(const database_txn_t *txn)
{
    return txn != NULL && txn_state(txn->txn) == TXN_STATE_ACTIVE && database_work_failed(txn->layer->db);
}

bool database_txn_belongs_to(const database_txn_t *txn, const database_t *db)
{
    return txn != NULL && db != NULL && txn->layer == database_layer_ctx(db);
}

txn_status_t database_txn_commit(database_txn_t *txn, txn_commit_info_t *info)
{
    if (txn == NULL) return TXN_INVALID;
    if (database_txn_failed(txn)) return TXN_INVALID;
    return txn_commit(txn->txn, info);
}

txn_status_t database_txn_rollback(database_txn_t *txn)
{
    return txn != NULL ? txn_rollback(txn->txn) : TXN_INVALID;
}

txn_status_t database_txn_release(database_txn_t **txn)
{
    if (txn == NULL || *txn == NULL) return TXN_OK;
    txn_status_t st = txn_release(&(*txn)->txn);
    free(*txn);
    *txn = NULL;
    return st;
}

txn_state_t database_txn_state(const database_txn_t *txn)
{
    return txn != NULL ? txn_state(txn->txn) : TXN_STATE_NONE;
}

uint64_t database_txn_id(const database_txn_t *txn)
{
    return txn != NULL ? txn_id(txn->txn) : 0;
}

txn_status_t database_txn_query_commit(database_t *db, uint64_t id, txn_commit_info_t *info)
{
    layer_t *l = database_layer_ctx(db);
    return l != NULL ? txn_query_commit(l->manager, id, info) : TXN_UNSUPPORTED;
}

bool database_txn_layer_poisoned(const database_t *db)
{
    layer_t *l = database_layer_ctx(db);
    return l != NULL && txn_manager_is_poisoned(l->manager);
}

uint32_t database_txn_epoch(const database_t *db)
{
    layer_t *l = database_layer_ctx(db);
    return l != NULL ? txn_manager_epoch(l->manager) : 0;
}

/* ── 연결 ── */

struct database_conn {
    database_t *db;
    layer_t *layer;
    database_txn_t *txn;
    uint32_t begin_timeout_ms;
    bool begin_busy;
};

database_status_t database_conn_open(database_t *db, const database_conn_options_t *options, database_conn_t **out)
{
    if (db == NULL || out == NULL) return DATABASE_INVALID;
    *out = NULL;
    layer_t *l = database_layer_ctx(db);
    if (l == NULL) return DATABASE_INVALID;
    database_conn_t *conn = calloc(1, sizeof(*conn));
    if (conn == NULL) return DATABASE_NOMEM;
    conn->db = db;
    conn->layer = l;
    conn->begin_timeout_ms = options != NULL ? options->begin_timeout_ms : 0;
    *out = conn;
    return DATABASE_OK;
}

database_status_t database_conn_close(database_conn_t **conn)
{
    if (conn == NULL || *conn == NULL) return DATABASE_OK;
    database_txn_release(&(*conn)->txn);  /* 열린 트랜잭션은 rollback이다 */
    free(*conn);
    *conn = NULL;
    return DATABASE_OK;
}

bool database_conn_in_txn(const database_conn_t *conn)
{
    return conn != NULL && conn->txn != NULL && database_txn_state(conn->txn) == TXN_STATE_ACTIVE;
}

bool database_conn_failed(const database_conn_t *conn) { return conn != NULL && database_txn_failed(conn->txn); }
bool database_conn_begin_busy(const database_conn_t *conn) { return conn != NULL && conn->begin_busy; }
database_txn_t *database_conn_txn(database_conn_t *conn) { return conn != NULL ? conn->txn : NULL; }

txn_status_t database_conn_begin(database_conn_t *conn, const txn_begin_options_t *options)
{
    if (conn == NULL) return TXN_INVALID;
    conn->begin_busy = false;
    if (conn->txn != NULL) return TXN_INVALID;
    txn_status_t st = database_txn_begin(conn->db, options, &conn->txn);
    if (st == TXN_BUSY || st == TXN_TIMEOUT) conn->begin_busy = true;
    return st;
}

txn_status_t database_conn_commit(database_conn_t *conn, txn_commit_info_t *info)
{
    if (conn == NULL || conn->txn == NULL) return TXN_NOT_ACTIVE;
    txn_status_t st = database_txn_commit(conn->txn, info);
    if (st == TXN_INVALID && database_txn_failed(conn->txn)) return st;  /* rollback을 기다린다 */
    database_txn_release(&conn->txn);
    return st;
}

txn_status_t database_conn_rollback(database_conn_t *conn)
{
    if (conn == NULL || conn->txn == NULL) return TXN_NOT_ACTIVE;
    txn_status_t st = database_txn_rollback(conn->txn);
    database_txn_release(&conn->txn);
    return st;
}

static database_exec_status_t control_error(database_conn_t *conn, const database_exec_options_t *opts,
                                            result_error_class_t error_class, int code, const char *message,
                                            result_t **out)
{
    return database_error_result(conn->db, opts, error_class, code, message, out);
}

static database_exec_status_t control_begin(database_conn_t *conn, const database_exec_options_t *opts,
                                            result_t **out)
{
    if (conn->txn != NULL) {
        return control_error(conn, opts, RESULT_E_SEMANTIC, 35, "이미 트랜잭션 안입니다", out);
    }
    txn_begin_options_t o = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_SNAPSHOT, TXN_GUARANTEE_DURABLE,
                             conn->begin_timeout_ms, NULL};
    txn_status_t st = database_conn_begin(conn, &o);
    if (st == TXN_OK) return database_command_result(conn->db, opts, "BEGIN", out);
    if (st == TXN_BUSY || st == TXN_TIMEOUT) {
        return control_error(conn, opts, RESULT_E_BUSY, DATABASE_CODE_WRITER_BUSY,
                             "다른 트랜잭션이 쓰는 중이라 시작하지 못했습니다", out);
    }
    return error_from_txn(conn->db, opts, st, out);
}

static database_exec_status_t control_commit(database_conn_t *conn, const database_exec_options_t *opts,
                                             result_t **out)
{
    if (conn->txn == NULL) return control_error(conn, opts, RESULT_E_SEMANTIC, 36, "진행 중인 트랜잭션이 없습니다", out);
    txn_status_t st = database_conn_commit(conn, NULL);
    switch (st) {
        case TXN_OK: return database_command_result(conn->db, opts, "COMMIT", out);
        case TXN_INVALID:
            return control_error(conn, opts, RESULT_E_SEMANTIC, DATABASE_CODE_WORK_FAILED,
                                 "이 트랜잭션은 이전 문장 오류로 실패한 상태입니다. rollback해야 합니다", out);
        case TXN_COMMIT_UNCERTAIN:
            return control_error(conn, opts, RESULT_E_IO, 32,
                                 "커밋 결과를 알 수 없습니다. 데이터베이스를 닫고 다시 열어 확인하세요", out);
        case TXN_COMMIT_FAILED:
        case TXN_CANCELLED:
            return control_error(conn, opts, RESULT_E_IO, 33, "커밋하지 못했습니다. 아무것도 반영하지 않았습니다", out);
        default: return error_from_txn(conn->db, opts, st, out);
    }
}

database_exec_status_t database_conn_exec(database_conn_t *conn, const database_exec_options_t *opts,
                                          const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                          result_t **out)
{
    if (conn == NULL || out == NULL) return DATABASE_EXEC_INVALID;
    database_t *db = conn->db;
    conn->begin_busy = false;
    database_control_t control;
    database_set_control_sink(db, &control);
    database_exec_status_t status = conn->txn != NULL ? database_txn_exec(conn->txn, opts, sql, size, binding, out)
                                                      : database_exec_ex(db, opts, sql, size, binding, out);
    database_set_control_sink(db, NULL);
    if (status != DATABASE_EXEC_CONTROL) return status;
    *out = NULL;
    switch (control.kind) {
        case DATABASE_CONTROL_BEGIN: return control_begin(conn, opts, out);
        case DATABASE_CONTROL_COMMIT: return control_commit(conn, opts, out);
        case DATABASE_CONTROL_ROLLBACK:
            if (conn->txn == NULL) {
                return control_error(conn, opts, RESULT_E_SEMANTIC, 36, "진행 중인 트랜잭션이 없습니다", out);
            }
            database_conn_rollback(conn);
            return database_command_result(db, opts, "ROLLBACK", out);
        default: return control_error(conn, opts, RESULT_E_INTERNAL, 21, "문장을 실행하지 못했습니다", out);
    }
}

/* ── 열기 ── */

static void abandon(layer_t *l, database_t *db)
{
    int saved = errno;
    if (l != NULL) {
        if (l->manager != NULL) txn_manager_close(&l->manager);
        free(l);
    }
    if (db != NULL) database_close(db);
    errno = saved;
}

database_status_t database_open_transactional(const char *path, const database_txn_options_t *options,
                                              database_t **out)
{
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    *out = NULL;
    static const database_txn_options_t defaults = {0};
    if (options == NULL) options = &defaults;

    database_t *db = NULL;
    database_status_t st = database_open(path, true, &db);
    if (st != DATABASE_OK) return st;
    layer_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        database_close(db);
        return DATABASE_NOMEM;
    }
    l->db = db;
    l->autocommit_timeout_ms = options->autocommit_timeout_ms;

    txn_manager_spec_t spec = {0};
    spec.ops = &BACKEND;
    spec.ctx = l;
    spec.max_guarantee = TXN_GUARANTEE_DURABLE;
    if (txn_manager_open(&spec, &l->manager) != TXN_OK) {
        abandon(l, db);
        return DATABASE_NOMEM;
    }
    if (database_set_layer(db, &LAYER, l) != DATABASE_OK) {
        abandon(l, db);
        return DATABASE_INVALID;
    }
    if (options->resident != NULL && database_attach_ledger(db, options->resident) != DATABASE_OK) {
        /* 계층을 붙인 뒤라 database_close가 계층까지 닫는다 */
        database_close(db);
        return DATABASE_NOMEM;
    }
    *out = db;
    return DATABASE_OK;
}
