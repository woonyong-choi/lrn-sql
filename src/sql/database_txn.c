#include "sql/database_txn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "database_internal.h"
#include "sql/result.h"
#include "storage/durable_pair.h"

/* 트랜잭션 계층 하나. database가 쥐고 database_close가 부르는 close에서 해제한다. */
typedef struct {
    database_t *db;
    wal_t *wal;                  /* NULL이면 WAL 없이 열렸다(durable 보증 거절) */
    durable_pair_t pair;         /* file은 db의 page_file을 가리킨다. 닫는 것은 db와 이 계층이 따로 한다 */
    txn_manager_t *manager;
    uint32_t autocommit_timeout_ms;
    /* 현재 writer */
    bool cur_durable;
    wal_xid_t cur_xid;
    uint64_t logged;             /* 이 트랜잭션이 WAL에 쌓은 문장 수 */
    bool wal_committed;          /* COMMIT 레코드가 fsync로 승인됐다 */
} layer_t;

struct database_txn {
    layer_t *layer;
    txn_t *txn;
    bool durable;
};

/* ── 문장 로그 payload ──
 *   u8 version(1), u8 0, u32 sql_len, sql, u32 param_count, 매개변수마다 u8 type + 값
 *   INTEGER i64 8, REAL 8바이트 IEEE754 비트 패턴, TEXT·BLOB u32 길이와 바이트, NULL은 값 없음. 정수는 little-endian.
 */
#define LOG_VERSION 1u

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* 부호화한 크기. 넘치면 false */
static bool payload_size(size_t sql_size, const lexer_binding_t *binding, size_t *size)
{
    size_t total = 2 + 4 + sql_size + 4;
    size_t count = binding != NULL ? binding->count : 0;
    for (size_t i = 0; i < count; i++) {
        const value_view_t *v = &binding->values[i];
        total += 1;
        switch (v->type) {
            case VALUE_NULL: break;
            case VALUE_INTEGER:
            case VALUE_REAL: total += 8; break;
            case VALUE_TEXT:
            case VALUE_BLOB:
                if (v->size_bytes > WAL_MAX_PAYLOAD) return false;
                total += 4 + v->size_bytes;
                break;
            default: return false;
        }
        if (total > WAL_MAX_PAYLOAD) return false;
    }
    if (total > WAL_MAX_PAYLOAD) return false;
    *size = total;
    return true;
}

static void encode_payload(uint8_t *out, const uint8_t *sql, size_t sql_size, const lexer_binding_t *binding)
{
    size_t at = 0;
    out[at++] = LOG_VERSION;
    out[at++] = 0;
    put_u32(out + at, (uint32_t)sql_size);
    at += 4;
    if (sql_size > 0) memcpy(out + at, sql, sql_size);
    at += sql_size;
    size_t count = binding != NULL ? binding->count : 0;
    put_u32(out + at, (uint32_t)count);
    at += 4;
    for (size_t i = 0; i < count; i++) {
        const value_view_t *v = &binding->values[i];
        out[at++] = (uint8_t)v->type;
        switch (v->type) {
            case VALUE_INTEGER:
                put_u64(out + at, (uint64_t)v->integer);
                at += 8;
                break;
            case VALUE_REAL: {
                uint64_t bits;
                memcpy(&bits, &v->real, sizeof(bits));
                put_u64(out + at, bits);
                at += 8;
                break;
            }
            case VALUE_TEXT:
            case VALUE_BLOB:
                put_u32(out + at, (uint32_t)v->size_bytes);
                at += 4;
                if (v->size_bytes > 0) memcpy(out + at, v->bytes, v->size_bytes);
                at += v->size_bytes;
                break;
            default: break;
        }
    }
}

typedef struct {
    const uint8_t *sql;
    size_t sql_size;
    value_view_t *values;   /* 호출자가 free */
    size_t count;
} decoded_t;

/* 잘못된 payload면 false. 값의 바이트는 payload를 가리키므로 payload보다 오래 쓰면 안 된다 */
static bool decode_payload(const uint8_t *p, size_t size, decoded_t *out)
{
    *out = (decoded_t){NULL, 0, NULL, 0};
    if (size < 2 + 4 + 4 || p[0] != LOG_VERSION || p[1] != 0) return false;
    size_t at = 2;
    uint32_t sql_size = get_u32(p + at);
    at += 4;
    if (sql_size > size - at || size - at - sql_size < 4) return false;
    out->sql = p + at;
    out->sql_size = sql_size;
    at += sql_size;
    uint32_t count = get_u32(p + at);
    at += 4;
    if (count > size - at) return false;  /* 값 하나는 1바이트 이상이다 */
    if (count > 0) {
        out->values = calloc(count, sizeof(value_view_t));
        if (out->values == NULL) return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (at >= size) goto bad;
        value_view_t v = {(value_type_t)p[at++], 0, 0, NULL, 0};
        switch (v.type) {
            case VALUE_NULL: break;
            case VALUE_INTEGER:
                if (size - at < 8) goto bad;
                v.integer = (int64_t)get_u64(p + at);
                at += 8;
                break;
            case VALUE_REAL: {
                if (size - at < 8) goto bad;
                uint64_t bits = get_u64(p + at);
                memcpy(&v.real, &bits, sizeof(bits));
                at += 8;
                break;
            }
            case VALUE_TEXT:
            case VALUE_BLOB: {
                if (size - at < 4) goto bad;
                uint32_t n = get_u32(p + at);
                at += 4;
                if (n > size - at) goto bad;
                v.bytes = n > 0 ? p + at : NULL;
                v.size_bytes = n;
                at += n;
                break;
            }
            default: goto bad;
        }
        out->values[i] = v;
    }
    out->count = count;
    if (at != size) goto bad;
    return true;
bad:
    free(out->values);
    *out = (decoded_t){NULL, 0, NULL, 0};
    return false;
}

/* ── database 계층 접점 ── */

static bool layer_log_statement(void *ctx, const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                bool *limit_exceeded)
{
    layer_t *l = ctx;
    *limit_exceeded = false;
    if (!l->cur_durable || l->wal == NULL) return true;  /* volatile 트랜잭션은 로그를 쌓지 않는다 */
    size_t bytes = 0;
    if (!payload_size(size, binding, &bytes)) {
        *limit_exceeded = true;
        return false;
    }
    uint8_t *payload = malloc(bytes);
    if (payload == NULL) return false;
    encode_payload(payload, sql, size, binding);
    wal_lsn_t lsn = 0;
    wal_status_t st = wal_append(l->wal, l->cur_xid, 0, payload, (uint32_t)bytes, &lsn);
    free(payload);
    if (st != WAL_OK) {
        *limit_exceeded = st == WAL_LIMIT || st == WAL_INVALID;
        return false;
    }
    l->logged++;
    return true;
}

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
    txn_begin_options_t begin = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_SNAPSHOT,
                                 l->wal != NULL ? TXN_GUARANTEE_DURABLE : TXN_GUARANTEE_SYNCED,
                                 l->autocommit_timeout_ms, NULL};
    txn_t *txn = NULL;
    txn_status_t st = txn_begin(l->manager, &begin, &txn);
    if (st == TXN_BUSY || st == TXN_TIMEOUT) {
        /* 다른 트랜잭션이 쓰는 중: 게시된 상태만 읽는다. 변경 문장은 BUSY 오류 결과로 끝난다 */
        return database_exec_read_only(db, opts, sql, size, binding, out);
    }
    if (st != TXN_OK) return error_from_txn(db, opts, st, out);

    l->cur_durable = l->wal != NULL;
    l->cur_xid = txn_id(txn);
    database_exec_status_t status = database_exec_in_work(db, opts, sql, size, binding, false, out);
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
    database_status_t result = DATABASE_OK;
    if (l->wal != NULL && wal_close(l->wal) != WAL_OK) result = DATABASE_IO;
    free(l);
    return result;
}

static const database_layer_t LAYER = {layer_log_statement, layer_exec, layer_close};

/* ── transaction manager backend ── */

static txn_status_t be_write_begin(void *ctx, uint64_t base_seq, void **write)
{
    layer_t *l = ctx;
    (void)base_seq;
    database_status_t st = database_work_begin(l->db);
    if (st != DATABASE_OK) return st == DATABASE_IO ? TXN_IO : TXN_INVALID;
    l->cur_durable = false;
    l->cur_xid = 0;
    l->logged = 0;
    l->wal_committed = false;
    *write = l;
    return TXN_OK;
}

static txn_publish_result_t be_publish(void *ctx, void *write, uint64_t commit_seq, txn_guarantee_t requested)
{
    layer_t *l = ctx;
    (void)write;
    (void)commit_seq;
    txn_publish_result_t unknown = {TXN_OUTCOME_UNKNOWN, TXN_GUARANTEE_VISIBLE};
    txn_publish_result_t not_applied = {TXN_OUTCOME_NOT_APPLIED, TXN_GUARANTEE_VISIBLE};
    database_t *db = l->db;
    if (database_work_failed(db)) return not_applied;

    if (requested == TXN_GUARANTEE_DURABLE && l->logged > 0) {
        wal_lsn_t lsn = 0;
        wal_status_t ws = wal_commit(l->wal, l->cur_xid, &lsn);
        if (ws == WAL_COMMIT_UNKNOWN) return unknown;      /* 로그에 남았는지 모른다 */
        if (ws != WAL_OK) return not_applied;              /* 아무것도 승인되지 않았다 */
        l->wal_committed = true;
        /* 여기서부터 거래는 승인됐다. 데이터 파일에 반영하지 못하면 재시작 복구가 반영한다 */
        database_work_result_t r = database_work_commit(db, &lsn);
        if (r.outcome == DATABASE_WORK_APPLIED) return (txn_publish_result_t){TXN_OUTCOME_APPLIED, TXN_GUARANTEE_DURABLE};
        database_poison(db);
        return unknown;
    }
    database_work_result_t r = database_work_commit(db, NULL);
    if (r.outcome == DATABASE_WORK_APPLIED) {
        txn_guarantee_t achieved = requested == TXN_GUARANTEE_DURABLE ? TXN_GUARANTEE_DURABLE : TXN_GUARANTEE_SYNCED;
        return (txn_publish_result_t){TXN_OUTCOME_APPLIED, achieved};
    }
    return r.outcome == DATABASE_WORK_UNKNOWN ? unknown : not_applied;
}

static void be_write_finish(void *ctx, void *write, txn_outcome_t outcome, uint64_t commit_seq)
{
    layer_t *l = ctx;
    (void)write;
    (void)commit_seq;
    database_work_outcome_t end = outcome == TXN_OUTCOME_APPLIED ? DATABASE_WORK_APPLIED
                                  : outcome == TXN_OUTCOME_UNKNOWN ? DATABASE_WORK_UNKNOWN
                                                                   : DATABASE_WORK_NOT_APPLIED;
    if (outcome == TXN_OUTCOME_NOT_APPLIED && l->logged > 0 && !l->wal_committed && l->wal != NULL) {
        wal_lsn_t lsn = 0;
        wal_abort(l->wal, l->cur_xid, &lsn);  /* 승인 응답이 없으므로 잃어도 CRASHED와 같다 */
    }
    database_work_end(l->db, end);
    l->cur_durable = false;
    l->logged = 0;
    l->wal_committed = false;
}

static bool be_is_poisoned(void *ctx)
{
    layer_t *l = ctx;
    return database_is_poisoned(l->db) || (l->wal != NULL && wal_is_poisoned(l->wal));
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
    t->durable = o.guarantee == TXN_GUARANTEE_DURABLE;
    *out = t;
    return TXN_OK;
}

database_exec_status_t database_txn_exec(database_txn_t *txn, const database_exec_options_t *opts,
                                         const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                         result_t **out)
{
    if (txn == NULL || out == NULL || txn_state(txn->txn) != TXN_STATE_ACTIVE) return DATABASE_EXEC_INVALID;
    layer_t *l = txn->layer;
    l->cur_durable = txn->durable && l->wal != NULL;
    l->cur_xid = txn_id(txn->txn);
    return database_exec_in_work(l->db, opts, sql, size, binding, false, out);
}

bool database_txn_failed(const database_txn_t *txn)
{
    return txn != NULL && txn_state(txn->txn) == TXN_STATE_ACTIVE && database_work_failed(txn->layer->db);
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
                                            database_control_mode_t mode, result_t **out)
{
    if (conn->txn != NULL) {
        return control_error(conn, opts, RESULT_E_SEMANTIC, 35, "이미 트랜잭션 안입니다", out);
    }
    bool has_wal = conn->layer->wal != NULL;
    if (mode == DATABASE_CONTROL_DURABLE && !has_wal) {
        return control_error(conn, opts, RESULT_E_SEMANTIC, 37,
                             "WAL 없이 열린 데이터베이스에서는 DURABLE 트랜잭션을 시작할 수 없습니다", out);
    }
    bool durable = mode == DATABASE_CONTROL_DURABLE || (mode == DATABASE_CONTROL_DEFAULT && has_wal);
    txn_begin_options_t o = {TXN_ACCESS_READ_WRITE, TXN_ISOLATION_SNAPSHOT,
                             durable ? TXN_GUARANTEE_DURABLE : TXN_GUARANTEE_SYNCED, conn->begin_timeout_ms, NULL};
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
        case DATABASE_CONTROL_BEGIN: return control_begin(conn, opts, control.mode, out);
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

/* ── 열기와 복구 ── */

typedef struct {
    database_t *db;
    bool in_work;
    bool failed;
    uint64_t txns;
    uint64_t statements;
} replay_t;

static wal_redo_result_t replay_redo(void *ctx, const wal_record_t *record)
{
    replay_t *r = ctx;
    if (record->kind != WAL_REC_DATA) return WAL_REDO_SKIPPED;
    if (!r->in_work) {
        if (database_work_begin(r->db) != DATABASE_OK) {
            r->failed = true;
            return WAL_REDO_ERROR;
        }
        r->in_work = true;
    }
    decoded_t d;
    if (!decode_payload(record->payload, record->payload_len, &d)) {
        r->failed = true;
        return WAL_REDO_ERROR;
    }
    lexer_binding_t binding = {d.values, d.count};
    result_t *result = NULL;
    database_exec_status_t st = database_exec_in_work(r->db, NULL, d.sql, d.sql_size, &binding, true, &result);
    bool bad = st != DATABASE_EXEC_OK || result == NULL || result_kind(result) == RESULT_KIND_ERROR;
    result_release(&result);
    free(d.values);
    if (bad) {
        r->failed = true;
        return WAL_REDO_ERROR;
    }
    r->statements++;
    return WAL_REDO_APPLIED;
}

static bool replay_done(void *ctx, wal_xid_t xid, wal_lsn_t commit_lsn)
{
    replay_t *r = ctx;
    (void)xid;
    database_work_result_t res = database_work_commit(r->db, &commit_lsn);
    database_work_end(r->db, res.outcome);
    r->in_work = false;
    if (res.outcome != DATABASE_WORK_APPLIED) {
        r->failed = true;
        return false;
    }
    r->txns++;
    return true;
}

static database_status_t from_pair(durable_pair_status_t st)
{
    switch (st) {
        case DURABLE_PAIR_OK: return DATABASE_OK;
        case DURABLE_PAIR_INVALID: return DATABASE_INVALID;
        case DURABLE_PAIR_NOMEM:
        case DURABLE_PAIR_LIMIT: return DATABASE_NOMEM;
        case DURABLE_PAIR_UNSUPPORTED: return DATABASE_UNSUPPORTED;
        case DURABLE_PAIR_LOCKED: return DATABASE_LOCKED;
        case DURABLE_PAIR_IO: return DATABASE_IO;
        default: return DATABASE_CORRUPT;  /* CORRUPT, NEEDS_CONVERSION, WAL_MISSING, MISMATCH, DOWNGRADED */
    }
}

static void abandon(layer_t *l, database_t *db)
{
    int saved = errno;
    if (l != NULL) {
        if (l->manager != NULL) txn_manager_close(&l->manager);
        if (l->wal != NULL) wal_close(l->wal);
        free(l);
    }
    if (db != NULL) database_close(db);
    errno = saved;
}

database_status_t database_open_transactional(const char *path, const database_txn_options_t *options,
                                              database_t **out, database_txn_info_t *info)
{
    database_txn_info_t local;
    if (info == NULL) info = &local;
    memset(info, 0, sizeof(*info));
    if (path == NULL || out == NULL) return DATABASE_INVALID;
    *out = NULL;
    static const database_txn_options_t defaults = {0};
    if (options == NULL) options = &defaults;

    database_t *db = NULL;
    wal_t *wal = NULL;
    if (options->wal_path == NULL) {
        database_status_t st = database_open(path, true, &db);
        if (st != DATABASE_OK) return st;
    } else {
        durable_pair_options_t po = {0};
        po.convert = options->convert;
        po.durable_epoch = options->initial_epoch;
        po.wal_limits = options->wal_limits;
        durable_pair_t pair;
        durable_pair_info_t pinfo;
        durable_pair_status_t ps = durable_pair_open(path, options->wal_path, &po, &pair, &pinfo);
        info->pair_status = (int)ps;
        if (ps != DURABLE_PAIR_OK) return from_pair(ps);
        info->converted = pinfo.converted;
        database_status_t st = database_adopt_file(pair.file, true, &db);
        if (st != DATABASE_OK) {
            int saved = errno;
            durable_pair_close(&pair);
            errno = saved;
            return st;
        }
        wal = pair.wal;
    }

    layer_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        if (wal != NULL) wal_close(wal);
        database_close(db);
        return DATABASE_NOMEM;
    }
    l->db = db;
    l->wal = wal;
    l->pair = (durable_pair_t){database_page_file(db), wal, false};
    l->autocommit_timeout_ms = options->autocommit_timeout_ms;
    info->wal_present = wal != NULL;

    if (wal != NULL) {
        replay_t replay = {db, false, false, 0, 0};
        wal_replay_ops_t ops = {WAL_REPLAY_COMMITTED_ONLY, page_file_control(l->pair.file)->applied_lsn,
                                replay_redo, replay_done, NULL};
        wal_replay_stats_t stats;
        wal_status_t ws = wal_replay(wal, &ops, &replay, &stats);
        if (replay.in_work) database_work_end(db, DATABASE_WORK_NOT_APPLIED);
        info->replayed_txns = replay.txns;
        info->replayed_statements = replay.statements;
        if (ws != WAL_OK || replay.failed) {
            abandon(l, db);
            return ws == WAL_IO ? DATABASE_IO : DATABASE_CORRUPT;
        }
    }

    txn_manager_spec_t spec = {0};
    spec.ops = &BACKEND;
    spec.ctx = l;
    spec.max_guarantee = wal != NULL ? TXN_GUARANTEE_DURABLE : TXN_GUARANTEE_SYNCED;
    if (wal != NULL) {
        spec.epoch_mode = TXN_EPOCH_ABOVE_FLOOR;
        spec.epoch_floor = page_file_control(l->pair.file)->durable_epoch;
    }
    if (txn_manager_open(&spec, &l->manager) != TXN_OK) {
        abandon(l, db);
        return DATABASE_NOMEM;
    }
    if (wal != NULL) {
        uint32_t epoch = txn_manager_epoch(l->manager);
        /* 영속화가 성공한 뒤에만 첫 트랜잭션을 시작한다. 실패하면 이 저장소를 쓰지 않는다 */
        if (durable_pair_persist_epoch(&l->pair, epoch) != DURABLE_PAIR_OK) {
            abandon(l, db);
            return DATABASE_IO;
        }
        info->epoch = epoch;
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
