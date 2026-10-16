#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/database.h"
#include "sql/database_txn.h"
#include "sql/mem_ledger.h"
#include "sql/result.h"

/*
 * 세션과 자원 수명. DB 단독 경로와 큐를 거쳐 문장을 소비하는 경로가 같은 계약을 쓰도록 database의 위에
 * 한 층을 둔다. 이 모듈은 큐도 transaction.h도 알지 못한다. 큐와 transaction은 이 헤더의 공개 API만 쓴다.
 *
 * 예산은 세 단계다.
 *   shared  풀 전체. 열린 데이터베이스의 상주 몫(카탈로그·해제 목록·핸들), 모든 세션의 작업 메모리,
 *           아직 release되지 않은 결과, 풀·세션·계상 객체 구조체의 합이다. 풀 구조체 자신도 shared의 초기 몫이다.
 *   session 세션 하나의 작업 메모리와 이전된 결과의 합.
 *   query   문장 하나가 실행되는 동안의 작업 메모리.
 * 한 번의 reserve는 세 단계를 모두 통과해야 하며 하나라도 넘으면 아무 카운터도 바꾸지 않고 거절한다.
 * reserve는 기다리지 않는다. 대기나 timeout은 호출자(큐)의 몫이다. 단위는 요청한 바이트이며 allocator
 * 오버헤드, 정렬, 단편화, 스택, 프로세스 RSS는 포함하지 않고 한도가 RSS를 보장하지 않는다. 다른 모듈
 * 안의 할당은 그 모듈이 계산한 상한을 호출 전에 잡는다(database.h).
 *
 * 문장의 수명:
 *   session_exec 안에서 쓴 작업 메모리는 성공·오류·취소·메모리 부족·I/O 실패·commit 불확정 어느 쪽으로
 *   끝나도 모두 돌아온다(끝난 뒤 query 사용량은 0). 호출자에게 넘어간 결과의 몫만 query에서 session의
 *   "이전된 결과"로 옮겨 계속 계상하고 result_release에서 반환한다. 옮기는 일은 카운터를 한 번에 바꾸는
 *   것이라 순간에 합이 늘지 않는다. 계상 객체는 참조 계수라 session_close 뒤에도 결과는 유효하며 마지막
 *   결과가 release될 때 정리된다. session_pool_close는 열린 세션, 열린 데이터베이스, 남은 결과(계상 객체)가 있으면
 *   SESSION_BUSY다. 마지막 결과의 release가 끝나 계상 객체가 몫을 돌려준 뒤에야 풀을 닫을 수 있다.
 * 취소는 협력형이다(database.h). session_cancel은 지금 실행 중이거나 다음에 시작하는 문장 하나를 취소하며
 * 문장이 끝날 때 요청은 지워진다. 끝나는 순간과 겹친 요청은 지워지거나 다음 문장 하나에
 * 적용될 수 있다. session_close와 동시에 session_cancel을 부르면 안 된다(호출자가 직렬화한다). 게시를 시작한 문장은 취소와 무관하게 실제 결과로 끝난다.
 * 동시성: 풀의 회계는 mutex로 보호되어 여러 스레드가 각자 세션으로 reserve·release해도 된다. 같은
 * 데이터베이스에 대한 문장 실행은 직렬화된다(동시 writer의 정책은 이 모듈의 범위 밖이다). 한 세션은 한 번에
 * 한 스레드만 쓴다.
 * 수명의 시작과 끝은 호출자가 직렬화한다. 같은 풀 또는 같은 데이터베이스에 대해 session_pool_open_database·
 * session_pool_create_database·session_open 같은 새 핸들을 만드는 호출과 session_close·
 * session_pool_close_database·session_pool_close를 서로 동시에 부르지 않는다(호출 하나가 돌아온 뒤 다음을 부른다).
 * 닫는 호출이 BUSY를 돌려주는 것은 이미 만들어져 쓰이는 대상이 남아 있어서이며, 다른 스레드가 새로 여는 호출과
 * 경합하는 것을 막아 주지 않는다. 안전한 예외는 하나다: 호출자가 이미 쥔 결과의 result_release는 다른 스레드의
 * session_pool_close와 동시에 불러도 된다. 풀은 열린 세션·데이터베이스가 없고 마지막 결과의 반환이 끝나
 * 계상 객체가 사라진 뒤에야 닫히며 그 전에는 SESSION_BUSY다. session_pool_close가 성공해도 release를 부른
 * 스레드가 결과 포인터를 지우는 마지막 쓰기까지 끝났다는 뜻은 아니므로 그 스레드를 join한 뒤에 포인터를 읽는다.
 * 통계 조회(session_pool_stats·session_stats_get)와 session_cancel은 대상 풀·세션이 살아 있도록 호출자가
 * 수명을 보장해야 하며 최종 close와 직렬화한다. 열린 자원이 없을 때 통계 조회와 동시에 session_pool_close가
 * 성공하면 풀이 해제될 수 있다.
 */

typedef struct session_pool session_pool_t;
typedef struct session_db session_db_t;
typedef struct session session_t;

typedef enum {
    SESSION_OK,
    SESSION_INVALID,
    SESSION_BUSY,    /* 닫을 수 없다: 쓰는 것이 남아 있다 */
    SESSION_LIMIT,   /* max_sessions를 넘음 */
    SESSION_NOMEM    /* shared 예산이나 할당이 거절됨 */
} session_status_t;

/* 모든 한도는 0보다 커야 한다. SIZE_MAX는 제한 없음이다. 의미 있는 순서는 query <= session <= shared다. */
typedef struct {
    size_t shared_limit;
    size_t session_limit;
    size_t query_limit;
    size_t max_sessions;
    size_t max_sql_bytes;  /* 0이면 DATABASE_BOUNDED_SQL_BYTES */
} session_limits_t;

typedef struct {
    size_t shared_used;
    size_t shared_peak;
    size_t resident_used;     /* 데이터베이스 상주 몫과 세션 구조체 */
    size_t result_used;       /* 호출자에게 이전된 결과 */
    size_t sessions_open;
    size_t databases_open;
    uint64_t refused;         /* reserve 거절 횟수 */
    uint64_t accounting_faults;  /* 계상 불변식이 깨진 횟수. 0이어야 한다 */
} session_pool_stats_t;

typedef struct {
    size_t query_used;
    size_t result_used;
    size_t peak;              /* 세션의 query+result 합의 최댓값 */
    size_t last_query_peak;   /* 마지막으로 끝난 문장의 query 최댓값 */
    uint64_t statements;
    uint64_t refused;
} session_stats_t;

session_status_t session_pool_open(const session_limits_t *limits, session_pool_t **out);
/* 열린 세션·데이터베이스·결과가 하나라도 있으면 SESSION_BUSY이고 풀은 그대로다. */
session_status_t session_pool_close(session_pool_t *pool);
session_status_t session_pool_stats(const session_pool_t *pool, session_pool_stats_t *out);

/*
 * 풀의 shared 예산 안에서 데이터베이스를 연다. 열기는 admission을 먼저 reserve하는 database_open_bounded를
 * 쓴다(database.h의 한계 포함). 거절되면 SESSION_NOMEM이고 파일도 핸들도 만들지 않았다. database 상태는 out
 * 인자로 돌려준다(DATABASE_OK가 아니면 *out은 NULL이다). 한 풀에서 같은 파일을 두 번 열면 database의 파일 잠금
 * 규칙을 따른다.
 */
session_status_t session_pool_open_database(session_pool_t *pool, const char *path, bool writable,
                                            session_db_t **out, database_status_t *database_status);
session_status_t session_pool_create_database(session_pool_t *pool, const char *path, session_db_t **out,
                                              database_status_t *database_status);
/* 열린 세션이 있으면 SESSION_BUSY다. 닫으면 상주 몫이 shared로 돌아간다. 결과가 남아 있어도 닫을 수 있다. */
session_status_t session_pool_close_database(session_db_t *db, database_status_t *database_status);
/*
 * 트랜잭션 계층(database_txn.h)을 붙여 연다. 상주 몫은 이 풀의 shared 예산으로 계상한다(options->resident는
 * 무시하고 풀의 ledger를 쓴다). 문장과 commit의 임시 메모리, 작업 카탈로그 몫도 같은 예산이며 열린 트랜잭션이 쥔 작업
 * 카탈로그와 페이지 목록은 상주 몫으로 남는다. 한계: 열기 중의 임시 메모리는 열린 뒤에 상주 몫을 붙이는 방식이라
 * database_open_bounded의 사전 admission을 받지 않는다.
 */
session_status_t session_pool_open_transactional(session_pool_t *pool, const char *path,
                                                 const database_txn_options_t *options, session_db_t **out,
                                                 database_status_t *database_status);
/* 상태 확인용 핸들. 호출자가 닫으면 안 된다. 세션 밖에서 직접 exec하면 직렬화·회계 계약을 우회한다. */
database_t *session_db_handle(session_db_t *db);
/* 세션이 속한 데이터베이스. 같은 데이터베이스의 세션인지 확인하는 데 쓴다. */
session_db_t *session_database(const session_t *session);

/* max_sessions를 넘으면 SESSION_LIMIT, 세션 구조체를 shared에 reserve하지 못하면 SESSION_NOMEM이다. */
session_status_t session_open(session_db_t *db, session_t **out);
/* 실행 중이면 SESSION_BUSY. 남은 결과는 그대로 유효하며 release가 몫을 돌려준다. */
session_status_t session_close(session_t *session);
session_status_t session_stats_get(const session_t *session, session_stats_t *out);
/* 다른 스레드에서 불러도 된다. 지금 또는 다음 문장 하나를 취소하도록 요청한다. */
void session_cancel(session_t *session);

typedef struct {
    const database_cancel_t *cancel;  /* 호출자 취소 조건(선택). session_cancel과 함께 본다 */
} session_exec_options_t;

/*
 * 트랜잭션 계층이 붙은 데이터베이스에서 BEGIN 문장이 다른 세션의 writer를 기다리는 시간(ms). 기본 0은 기다리지 않고
 * RESULT_E_BUSY 오류 결과다. 기다리는 동안 데이터베이스 실행 잠금을 쥐지 않으므로 쓰고 있는 세션이 commit·rollback할 수
 * 있다. session_cancel이 오면 기다림을 멈추고 BUSY 결과를 돌려준다. 실행 중이 아닐 때 호출한다.
 */
void session_set_begin_timeout(session_t *session, uint32_t timeout_ms);

/*
 * 문장 하나를 실행한다(database_exec_ex와 같은 반환 계약). 반환된 결과는 session에 이전된 몫으로 계상되며
 * 호출자가 result_release한다. 예산을 넘거나 할당이 거절되면 RESULT_E_NOMEM 오류 결과이고 그 결과조차 만들 수
 * 없으면 DATABASE_EXEC_NOMEM이다. 이미 실행 중인 세션이면 DATABASE_EXEC_INVALID다.
 */
database_exec_status_t session_exec(session_t *session, const session_exec_options_t *options,
                                    const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                    result_t **out);

/*
 * 세션의 트랜잭션(C API). BEGIN·COMMIT·ROLLBACK 문장과 같은 database_conn의 트랜잭션을 쓴다. 세션은 열린 트랜잭션을
 * 최대 하나 쥐고, 세션 안의 session_exec는 열린 트랜잭션 안에서 실행된다(없으면 자동 commit). 트랜잭션 계층이 없는
 * 데이터베이스의 세션은 TXN_UNSUPPORTED다. session_close는 열린 트랜잭션을 rollback한다. 실행 중인 세션은 TXN_INVALID다.
 *   begin: timeout_ms만큼 다른 세션의 writer를 기다린다. 기다리는 동안 실행 잠금을 쥐지 않는다. commit은 항상
 *          DURABLE을 달성한다(database_txn.h)
 *   commit: 실패 상태(session_txn_failed)면 TXN_INVALID이고 그대로다. 그 밖에는 결과와 무관하게 트랜잭션이 끝난다
 *   rollback: 열린 트랜잭션이 없으면 TXN_NOT_ACTIVE
 */
typedef struct {
    txn_guarantee_t guarantee;
    uint32_t timeout_ms;
} session_txn_options_t;

txn_status_t session_txn_begin(session_t *session, const session_txn_options_t *options);
txn_status_t session_txn_commit(session_t *session, txn_commit_info_t *info);
txn_status_t session_txn_rollback(session_t *session);
bool session_txn_active(const session_t *session);
bool session_txn_failed(const session_t *session);

#endif
