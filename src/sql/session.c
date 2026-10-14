#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "sql/session.h"

/*
 * 모든 카운터는 풀의 mutex 하나로 보호한다. 한 번의 reserve는 query·session·shared를 같은 임계 구역에서
 * 모두 확인하고 모두 올리므로 일부만 반영되는 순간이 없다.
 */

struct session_pool {
    pthread_mutex_t lock;
    session_limits_t limits;
    size_t shared_used;
    size_t shared_peak;
    size_t resident_used;
    size_t result_used;
    size_t sessions_open;
    size_t databases_open;
    size_t accounts_live;  /* 아직 해제되지 않은 계상 객체. 마지막 drop이 풀을 건드린 뒤에만 0이 된다 */
    uint64_t refused;
    uint64_t faults;
};

/* 세션의 계상 객체. 세션과 이전된 결과가 참조 계수로 쥔다. */
typedef struct {
    session_pool_t *pool;
    size_t refs;          /* 풀의 lock으로 보호 */
    size_t query_used;
    size_t result_used;
    size_t peak;
    size_t query_peak;
    size_t last_query_peak;
    uint64_t statements;
    uint64_t refused;
} account_t;

struct session_db {
    session_pool_t *pool;
    database_t *db;
    pthread_mutex_t run;  /* 같은 데이터베이스의 문장 실행을 직렬화 */
    size_t sessions;      /* 풀의 lock으로 보호 */
    size_t overhead;      /* 이 구조체가 shared에 잡은 몫 */
};

struct session {
    session_db_t *db;
    account_t *account;
    atomic_bool cancel;
    bool executing;       /* 풀의 lock으로 보호. session_exec의 마지막 접근 뒤에만 내려간다 */
};

static bool fits(size_t used, size_t bytes, size_t limit)
{
    return bytes <= limit && used <= limit - bytes;
}

static void note_peak(session_pool_t *pool, account_t *account)
{
    if (pool->shared_used > pool->shared_peak) pool->shared_peak = pool->shared_used;
    if (account != NULL) {
        size_t total = account->query_used + account->result_used;
        if (total > account->peak) account->peak = total;
        if (account->query_used > account->query_peak) account->query_peak = account->query_used;
    }
}

/* 카운터를 내린다. 내릴 몫보다 크면 불변식이 깨진 것이라 세고 0으로 맞춘다. */
static void take(session_pool_t *pool, size_t *counter, size_t bytes)
{
    if (bytes > *counter) {
        pool->faults++;
        bytes = *counter;
    }
    *counter -= bytes;
}

/* ── ledger: 세션의 query·result ── */

static bool account_reserve(void *ctx, mem_scope_t scope, size_t bytes)
{
    account_t *account = ctx;
    session_pool_t *pool = account->pool;
    pthread_mutex_lock(&pool->lock);
    size_t session_total = account->query_used + account->result_used;
    bool ok = fits(pool->shared_used, bytes, pool->limits.shared_limit);
    if (ok && scope != MEM_SCOPE_RESIDENT) ok = fits(session_total, bytes, pool->limits.session_limit);
    if (ok && scope == MEM_SCOPE_QUERY) ok = fits(account->query_used, bytes, pool->limits.query_limit);
    if (!ok) {
        pool->refused++;
        account->refused++;
        pthread_mutex_unlock(&pool->lock);
        return false;
    }
    pool->shared_used += bytes;
    switch (scope) {
        case MEM_SCOPE_QUERY: account->query_used += bytes; break;
        case MEM_SCOPE_RESULT:
            account->result_used += bytes;
            pool->result_used += bytes;
            break;
        case MEM_SCOPE_RESIDENT: pool->resident_used += bytes; break;
    }
    note_peak(pool, account);
    pthread_mutex_unlock(&pool->lock);
    return true;
}

static void account_release(void *ctx, mem_scope_t scope, size_t bytes)
{
    account_t *account = ctx;
    session_pool_t *pool = account->pool;
    pthread_mutex_lock(&pool->lock);
    take(pool, &pool->shared_used, bytes);
    switch (scope) {
        case MEM_SCOPE_QUERY: take(pool, &account->query_used, bytes); break;
        case MEM_SCOPE_RESULT:
            take(pool, &account->result_used, bytes);
            take(pool, &pool->result_used, bytes);
            break;
        case MEM_SCOPE_RESIDENT: take(pool, &pool->resident_used, bytes); break;
    }
    pthread_mutex_unlock(&pool->lock);
}

static void account_retain(void *ctx)
{
    account_t *account = ctx;
    pthread_mutex_lock(&account->pool->lock);
    account->refs++;
    pthread_mutex_unlock(&account->pool->lock);
}

static void account_drop(void *ctx)
{
    account_t *account = ctx;
    session_pool_t *pool = account->pool;
    pthread_mutex_lock(&pool->lock);
    bool last = --account->refs == 0;
    if (last) {
        /* 계상 객체 자신의 몫은 마지막 참조가 사라질 때 돌려준다. 풀은 accounts_live가 0이 될 때까지 닫히지 않는다 */
        take(pool, &pool->shared_used, sizeof(account_t));
        take(pool, &pool->resident_used, sizeof(account_t));
        pool->accounts_live--;
    }
    pthread_mutex_unlock(&pool->lock);
    if (last) free(account);
}

static const mem_ledger_ops_t ACCOUNT_OPS = {account_reserve, account_release, account_retain, account_drop};

/* ── ledger: 풀의 상주 몫 ── */

static bool pool_reserve(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    session_pool_t *pool = ctx;
    pthread_mutex_lock(&pool->lock);
    bool ok = fits(pool->shared_used, bytes, pool->limits.shared_limit);
    if (!ok) {
        pool->refused++;
    } else {
        pool->shared_used += bytes;
        pool->resident_used += bytes;
        note_peak(pool, NULL);
    }
    pthread_mutex_unlock(&pool->lock);
    return ok;
}

static void pool_release(void *ctx, mem_scope_t scope, size_t bytes)
{
    (void)scope;
    session_pool_t *pool = ctx;
    pthread_mutex_lock(&pool->lock);
    take(pool, &pool->shared_used, bytes);
    take(pool, &pool->resident_used, bytes);
    pthread_mutex_unlock(&pool->lock);
}

static const mem_ledger_ops_t POOL_OPS = {pool_reserve, pool_release, NULL, NULL};

/* ── 풀 ── */

static bool limits_valid(const session_limits_t *limits)
{
    return limits != NULL && limits->shared_limit > 0 && limits->session_limit > 0 &&
           limits->query_limit > 0 && limits->max_sessions > 0;
}

session_status_t session_pool_open(const session_limits_t *limits, session_pool_t **out)
{
    if (out == NULL || !limits_valid(limits)) return SESSION_INVALID;
    session_pool_t *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return SESSION_NOMEM;
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool);
        return SESSION_NOMEM;
    }
    pool->limits = *limits;
    if (!fits(0, sizeof(*pool), limits->shared_limit)) {
        pthread_mutex_destroy(&pool->lock);
        free(pool);
        return SESSION_NOMEM;
    }
    pool->shared_used = sizeof(*pool);
    pool->resident_used = sizeof(*pool);
    pool->shared_peak = sizeof(*pool);
    *out = pool;
    return SESSION_OK;
}

session_status_t session_pool_close(session_pool_t *pool)
{
    if (pool == NULL) return SESSION_INVALID;
    pthread_mutex_lock(&pool->lock);
    bool busy = pool->sessions_open > 0 || pool->databases_open > 0 || pool->accounts_live > 0 ||
                pool->result_used > 0;
    pthread_mutex_unlock(&pool->lock);
    if (busy) return SESSION_BUSY;
    pthread_mutex_destroy(&pool->lock);
    free(pool);
    return SESSION_OK;
}

session_status_t session_pool_stats(const session_pool_t *pool, session_pool_stats_t *out)
{
    if (pool == NULL || out == NULL) return SESSION_INVALID;
    session_pool_t *mutable_pool = (session_pool_t *)pool;
    pthread_mutex_lock(&mutable_pool->lock);
    *out = (session_pool_stats_t){pool->shared_used, pool->shared_peak, pool->resident_used,
                                  pool->result_used, pool->sessions_open, pool->databases_open,
                                  pool->refused, pool->faults};
    pthread_mutex_unlock(&mutable_pool->lock);
    return SESSION_OK;
}

/* ── 데이터베이스 ── */

/*
 * 래퍼 구조체와 몫을 파일을 만들거나 열기 전에 먼저 확보한다. 그래야 래퍼 때문에 실패해도 만들어진 파일이
 * 남지 않는다(database.h의 실패 시 파일 없음 계약). databases_open도 이때 올려 열기 도중 풀이 닫히지 않게 한다.
 */
static session_db_t *wrapper_acquire(session_pool_t *pool)
{
    mem_ledger_t ledger = {&POOL_OPS, pool};
    if (!mem_reserve(&ledger, MEM_SCOPE_RESIDENT, sizeof(session_db_t))) return NULL;
    session_db_t *wrapper = calloc(1, sizeof(*wrapper));
    if (wrapper == NULL || pthread_mutex_init(&wrapper->run, NULL) != 0) {
        free(wrapper);
        mem_release(&ledger, MEM_SCOPE_RESIDENT, sizeof(session_db_t));
        return NULL;
    }
    wrapper->pool = pool;
    wrapper->overhead = sizeof(session_db_t);
    pthread_mutex_lock(&pool->lock);
    pool->databases_open++;
    pthread_mutex_unlock(&pool->lock);
    return wrapper;
}

/* 풀을 건드리는 마지막 일이 databases_open 감소이므로 몫을 먼저 돌려준다 */
static void wrapper_discard(session_db_t *wrapper)
{
    session_pool_t *pool = wrapper->pool;
    mem_ledger_t ledger = {&POOL_OPS, pool};
    mem_release(&ledger, MEM_SCOPE_RESIDENT, wrapper->overhead);
    pthread_mutex_destroy(&wrapper->run);
    pthread_mutex_lock(&pool->lock);
    pool->databases_open--;
    pthread_mutex_unlock(&pool->lock);
    free(wrapper);
}

static session_status_t finish_open(session_db_t *wrapper, database_t *db, database_status_t status,
                                    database_status_t *database_status, session_db_t **out)
{
    if (database_status != NULL) *database_status = status;
    if (status != DATABASE_OK) {
        wrapper_discard(wrapper);
        return status == DATABASE_NOMEM ? SESSION_NOMEM : SESSION_INVALID;
    }
    wrapper->db = db;
    *out = wrapper;
    return SESSION_OK;
}

session_status_t session_pool_open_database(session_pool_t *pool, const char *path, bool writable,
                                            session_db_t **out, database_status_t *database_status)
{
    if (pool == NULL || path == NULL || out == NULL) return SESSION_INVALID;
    *out = NULL;
    session_db_t *wrapper = wrapper_acquire(pool);
    if (wrapper == NULL) {
        if (database_status != NULL) *database_status = DATABASE_NOMEM;
        return SESSION_NOMEM;
    }
    mem_ledger_t ledger = {&POOL_OPS, pool};
    database_t *db = NULL;
    database_status_t status = database_open_bounded(path, writable, &ledger, &db);
    return finish_open(wrapper, db, status, database_status, out);
}

session_status_t session_pool_create_database(session_pool_t *pool, const char *path, session_db_t **out,
                                              database_status_t *database_status)
{
    if (pool == NULL || path == NULL || out == NULL) return SESSION_INVALID;
    *out = NULL;
    session_db_t *wrapper = wrapper_acquire(pool);
    if (wrapper == NULL) {
        if (database_status != NULL) *database_status = DATABASE_NOMEM;
        return SESSION_NOMEM;
    }
    mem_ledger_t ledger = {&POOL_OPS, pool};
    database_t *db = NULL;
    database_status_t status = database_create_bounded(path, &ledger, &db);
    return finish_open(wrapper, db, status, database_status, out);
}

session_status_t session_pool_close_database(session_db_t *wrapper, database_status_t *database_status)
{
    if (wrapper == NULL) return SESSION_INVALID;
    session_pool_t *pool = wrapper->pool;
    pthread_mutex_lock(&pool->lock);
    bool busy = wrapper->sessions > 0;
    pthread_mutex_unlock(&pool->lock);
    if (busy) return SESSION_BUSY;
    database_status_t closed = database_close(wrapper->db);  /* 상주 몫을 돌려준다 */
    if (database_status != NULL) *database_status = closed;
    wrapper_discard(wrapper);
    return SESSION_OK;
}

database_t *session_db_handle(session_db_t *db) { return db != NULL ? db->db : NULL; }

/* ── 세션 ── */

session_status_t session_open(session_db_t *db, session_t **out)
{
    if (db == NULL || out == NULL) return SESSION_INVALID;
    session_pool_t *pool = db->pool;
    size_t overhead = sizeof(session_t) + sizeof(account_t);
    pthread_mutex_lock(&pool->lock);
    if (pool->sessions_open >= pool->limits.max_sessions) {
        pthread_mutex_unlock(&pool->lock);
        return SESSION_LIMIT;
    }
    if (!fits(pool->shared_used, overhead, pool->limits.shared_limit)) {
        pool->refused++;
        pthread_mutex_unlock(&pool->lock);
        return SESSION_NOMEM;
    }
    pool->shared_used += overhead;
    pool->resident_used += overhead;
    note_peak(pool, NULL);
    pool->sessions_open++;
    pool->accounts_live++;
    db->sessions++;
    pthread_mutex_unlock(&pool->lock);

    session_t *session = calloc(1, sizeof(*session));
    account_t *account = calloc(1, sizeof(*account));
    if (session == NULL || account == NULL) {
        free(session);
        free(account);
        pthread_mutex_lock(&pool->lock);
        take(pool, &pool->shared_used, overhead);
        take(pool, &pool->resident_used, overhead);
        pool->sessions_open--;
        pool->accounts_live--;
        db->sessions--;
        pthread_mutex_unlock(&pool->lock);
        return SESSION_NOMEM;
    }
    account->pool = pool;
    account->refs = 1;  /* 세션이 쥔다 */
    session->db = db;
    session->account = account;
    atomic_init(&session->cancel, false);
    *out = session;
    return SESSION_OK;
}

session_status_t session_close(session_t *session)
{
    if (session == NULL) return SESSION_INVALID;
    session_pool_t *pool = session->db->pool;
    account_t *account = session->account;
    pthread_mutex_lock(&pool->lock);
    if (session->executing) {
        pthread_mutex_unlock(&pool->lock);
        return SESSION_BUSY;
    }
    /* 세션 구조체 몫만 돌려준다. account 몫은 마지막 참조가 사라질 때 account_drop이 돌려준다 */
    take(pool, &pool->shared_used, sizeof(session_t));
    take(pool, &pool->resident_used, sizeof(session_t));
    session->db->sessions--;
    /* 남은 query 몫이 있으면 불변식 위반이다. 세고 돌려준다 */
    if (account->query_used > 0) {
        pool->faults++;
        take(pool, &pool->shared_used, account->query_used);
        account->query_used = 0;
    }
    pool->sessions_open--;
    pthread_mutex_unlock(&pool->lock);
    free(session);
    account_drop(account);  /* 이전된 결과가 남아 있으면 그쪽이 마지막 참조다 */
    return SESSION_OK;
}

session_status_t session_stats_get(const session_t *session, session_stats_t *out)
{
    if (session == NULL || out == NULL) return SESSION_INVALID;
    session_pool_t *pool = session->db->pool;
    account_t *account = session->account;
    pthread_mutex_lock(&pool->lock);
    *out = (session_stats_t){account->query_used, account->result_used, account->peak,
                             account->last_query_peak, account->statements, account->refused};
    pthread_mutex_unlock(&pool->lock);
    return SESSION_OK;
}

void session_cancel(session_t *session)
{
    if (session != NULL) atomic_store(&session->cancel, true);
}

/* session_cancel과 호출자 조건을 함께 본다 */
typedef struct {
    session_t *session;
    const database_cancel_t *user;
} cancel_ctx_t;

static bool cancel_requested(void *ctx)
{
    const cancel_ctx_t *cancel = ctx;
    if (atomic_load(&cancel->session->cancel)) return true;
    return cancel->user != NULL && cancel->user->requested != NULL && cancel->user->requested(cancel->user->ctx);
}

database_exec_status_t session_exec(session_t *session, const session_exec_options_t *options,
                                    const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                    result_t **out)
{
    if (session == NULL || out == NULL) return DATABASE_EXEC_INVALID;
    session_pool_t *pool = session->db->pool;
    account_t *account = session->account;
    pthread_mutex_lock(&pool->lock);
    if (session->executing) {
        pthread_mutex_unlock(&pool->lock);
        return DATABASE_EXEC_INVALID;
    }
    session->executing = true;
    account->query_peak = 0;
    pthread_mutex_unlock(&pool->lock);

    cancel_ctx_t cancel_ctx = {session, options != NULL ? options->cancel : NULL};
    database_cancel_t cancel = {cancel_requested, &cancel_ctx};
    mem_ledger_t ledger = {&ACCOUNT_OPS, account};
    database_exec_options_t exec_options = {&ledger, &cancel, pool->limits.max_sql_bytes};

    pthread_mutex_lock(&session->db->run);
    database_exec_status_t status = database_exec_ex(session->db->db, &exec_options, sql, size, binding, out);
    pthread_mutex_unlock(&session->db->run);

    /* 결과를 호출자에게 넘긴다: query 몫을 session의 이전된 결과로 옮기고 결과가 반환할 곳을 바꾼다.
     * 옮기는 일은 카운터 이동이라 shared_used는 변하지 않는다. 먼저 옮겨야 rebind 뒤 release가 맞는 곳을 본다 */
    if (status == DATABASE_EXEC_OK && *out != NULL) {
        size_t moved = result_accounted_bytes(*out);
        mem_ledger_t owner = {&ACCOUNT_OPS, account};
        pthread_mutex_lock(&pool->lock);
        take(pool, &account->query_used, moved);
        account->result_used += moved;
        pool->result_used += moved;
        pthread_mutex_unlock(&pool->lock);
        result_rebind_ledger(*out, &owner, MEM_SCOPE_RESULT);
    }

    /* 이 문장의 취소 요청은 세션 구조체를 마지막으로 만지기 전에 지운다. 실행 상태를 내리는 것이 마지막 접근이며
     * 그 뒤에는 session_close가 세션을 해제해도 된다 */
    atomic_store(&session->cancel, false);
    pthread_mutex_lock(&pool->lock);
    /* 끝난 문장의 query 몫은 0이어야 한다. 남았다면 불변식 위반이므로 세고 돌려준다 */
    if (account->query_used > 0) {
        pool->faults++;
        take(pool, &pool->shared_used, account->query_used);
        account->query_used = 0;
    }
    account->last_query_peak = account->query_peak;
    account->statements++;
    session->executing = false;
    pthread_mutex_unlock(&pool->lock);
    return status;
}
