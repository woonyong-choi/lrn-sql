#ifndef DATABASE_TXN_H
#define DATABASE_TXN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/database.h"
#include "sql/mem_ledger.h"
#include "storage/transaction.h"

/*
 * database 위의 트랜잭션 경계(#8). 여러 문장을 하나의 게시로 확정하거나 모두 버리는 공개 transaction handle을
 * 제공한다. 상태 기계·commit 결과·writer 경합·timeout·취소·poison은 storage/transaction.h(#80)의 manager가 맡고,
 * 이 모듈은 그 backend를 database로 구현한다.
 *
 * 모델
 *  - 쓰기 트랜잭션은 한 번에 하나다(단일 writer). 다른 호출은 begin에서 timeout만큼 기다린 뒤 TXN_BUSY·TXN_TIMEOUT이다.
 *  - 선언한 격리는 SNAPSHOT 하나다. 작업이 열려 있는 동안 게시된 상태는 바뀌지 않으므로 이 트랜잭션은 시작 때의
 *    게시된 상태와 자기 변경만 본다. 읽기 전용 트랜잭션 handle은 지원하지 않는다(TXN_UNSUPPORTED).
 *  - 문장 하나가 오류 결과로 끝나면 그 트랜잭션은 실패 상태다. 이후 문장은 DATABASE_CODE_WORK_FAILED 오류 결과로
 *    거절되고 commit은 TXN_INVALID이므로 명시적 rollback이 필요하다. database_txn_failed로 확인한다.
 *  - 트랜잭션 밖의 database_exec는 문장마다 자동 commit이다. 다른 트랜잭션이 쓰는 중이면 SELECT는 게시된
 *    상태(미확정 변경 없음)를 읽고 변경 문장은 RESULT_E_BUSY 오류 결과다.
 *  - 외부 HTTP·파일 부작용의 원자성은 제공하지 않는다.
 *
 * commit과 재시작
 *   commit은 COW 게시다. 바뀐 데이터 페이지와 카탈로그를 새 자리에 쓰고 제어 페이지 한 장으로 세대를 바꾼 뒤
 *   fsync한다(순서는 page_file_commit). 성공 응답은 제어 쓰기 뒤의 fsync가 끝난 뒤에만 돌아오므로 승인된 거래는 프로세스·OS
 *   종료 뒤에도 남고, 승인 전에 멈추면 다시 열었을 때 이전 세대 또는 새 세대 중 하나다(부분 반영 없음). 그래서
 *   달성 보증은 항상 TXN_GUARANTEE_DURABLE이다. 제어 쓰기를 시도한 뒤 실패하면 TXN_COMMIT_UNCERTAIN이고 핸들은
 *   poison이 된다. 다시 열면 이전 또는 새 상태다.
 *
 * 한계: 성공 반환 뒤 저장 장치가 버린 쓰기(lost write)는 탐지하지 못한다. 전원 장애 검증은 하지 않았다(프로세스
 * 종료와 I/O 실패 주입까지). 여러 스레드가 같은 database를 직접 호출하는 직렬화는 호출자 몫이다(session 계층이
 * 문장 호출을 직렬화한다). epoch(트랜잭션 번호의 상위 부분)는 열 때마다 정하며 재시작 뒤 같은 번호가 다시 나올 수
 * 있다. 그래서 database_txn_query_commit은 같은 열기 안에서만 의미가 있다.
 */
typedef struct database_txn database_txn_t;

typedef struct {
    /* 상주 몫을 계상할 ledger. NULL이면 계상하지 않는다. 열린 뒤에 database_attach_ledger와 같게 붙인다 */
    const mem_ledger_t *resident;
    /* 트랜잭션 밖 변경 문장이 다른 쓰기를 기다리는 시간. 0이면 기다리지 않고 BUSY */
    uint32_t autocommit_timeout_ms;
} database_txn_options_t;

/* 쓰기로 열고 트랜잭션 계층을 붙인다. options가 NULL이면 기본값이다. 실패하면 *out은 NULL이다. */
database_status_t database_open_transactional(const char *path, const database_txn_options_t *options,
                                              database_t **out);
/*
 * 트랜잭션을 시작한다. options는 transaction.h의 txn_begin_options_t이며 NULL이면 기본값(READ_WRITE, SNAPSHOT,
 * VISIBLE, 기다리지 않음)이다. READ_ONLY와 SNAPSHOT 외의 격리는 TXN_UNSUPPORTED다. 어떤 보증을 요청해도
 * commit은 DURABLE을 달성한다. 성공한 *out은 database_txn_release로 해제한다.
 */
txn_status_t database_txn_begin(database_t *db, const txn_begin_options_t *options, database_txn_t **out);

/* 트랜잭션 안에서 문장 하나를 실행한다(database_exec_ex와 같은 반환 계약). ACTIVE가 아니면 DATABASE_EXEC_INVALID. */
database_exec_status_t database_txn_exec(database_txn_t *txn, const database_exec_options_t *opts,
                                         const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                         result_t **out);

/* 문장 오류 뒤 rollback이 필요한 상태인지 */
bool database_txn_failed(const database_txn_t *txn);
/* 큐처럼 호출자 거래를 받는 모듈이 다른 database의 거래를 거절할 때 쓴다. */
bool database_txn_belongs_to(const database_txn_t *txn, const database_t *db);

/* transaction.h와 같은 의미. 실패 상태의 commit은 TXN_INVALID이고 트랜잭션은 ACTIVE로 남아 rollback을 기다린다 */
txn_status_t database_txn_commit(database_txn_t *txn, txn_commit_info_t *info);
txn_status_t database_txn_rollback(database_txn_t *txn);
txn_status_t database_txn_release(database_txn_t **txn);
txn_state_t database_txn_state(const database_txn_t *txn);
uint64_t database_txn_id(const database_txn_t *txn);

/* 응답을 잃은 호출자가 id의 commit 결과를 조회한다(transaction.h의 txn_query_commit). */
txn_status_t database_txn_query_commit(database_t *db, uint64_t txn_id, txn_commit_info_t *info);

/*
 * 연결(connection). 한 연결은 열린 트랜잭션을 최대 하나 쥔다. BEGIN·COMMIT·ROLLBACK 문장과 C API가 같은
 * database_txn_t를 쓴다. 연결 안에서 문장은 열린 트랜잭션이 있으면 그 안에서, 없으면 자동 commit으로 실행된다.
 * 한 연결은 한 번에 한 스레드만 쓴다. 같은 database를 쓰는 여러 연결의 호출 직렬화는 호출자 몫이다(session 계층이 한다).
 *
 * SQL 제어문(정의: sql_statement.h)
 *   BEGIN [TRANSACTION], START TRANSACTION -> COMMAND "BEGIN"
 *   COMMIT [TRANSACTION]  -> COMMAND "COMMIT",  ROLLBACK [TRANSACTION] -> COMMAND "ROLLBACK"
 *   이미 트랜잭션 안에서 BEGIN하거나 밖에서 COMMIT·ROLLBACK하면 오류 결과다(트랜잭션은 그대로). 다른 연결이 쓰는 중이면
 *   BEGIN은 RESULT_E_BUSY 오류 결과이고 database_conn_begin_busy가 true다(호출자가 기다렸다 다시 시도할 수 있다).
 *   문장 오류로 실패한 트랜잭션의 COMMIT은 거절되고(DATABASE_CODE_WORK_FAILED) ROLLBACK만 받는다.
 *   연결 밖(database_exec)에서 실행한 제어문은 RESULT_E_SEMANTIC 오류 결과다.
 */
typedef struct database_conn database_conn_t;

typedef struct {
    uint32_t begin_timeout_ms;   /* BEGIN 문장이 다른 writer를 기다리는 시간. 0이면 기다리지 않고 BUSY */
} database_conn_options_t;

/* 트랜잭션 계층이 붙은 database에서만 연다. 아니면 DATABASE_INVALID */
database_status_t database_conn_open(database_t *db, const database_conn_options_t *options, database_conn_t **out);
/* 열린 트랜잭션은 rollback한다. *conn은 NULL이 된다 */
database_status_t database_conn_close(database_conn_t **conn);
database_exec_status_t database_conn_exec(database_conn_t *conn, const database_exec_options_t *opts,
                                          const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                          result_t **out);
/* C API. 열린 트랜잭션이 있으면 TXN_INVALID. 나머지는 transaction.h의 begin과 같다 */
txn_status_t database_conn_begin(database_conn_t *conn, const txn_begin_options_t *options);
/*
 * 실패한 트랜잭션(database_conn_failed)이면 TXN_INVALID이고 그대로다. 그 밖에는 결과와 무관하게 트랜잭션이 끝나
 * 연결에는 열린 트랜잭션이 남지 않는다(TXN_OK, TXN_COMMIT_FAILED, TXN_COMMIT_UNCERTAIN 등).
 */
txn_status_t database_conn_commit(database_conn_t *conn, txn_commit_info_t *info);
txn_status_t database_conn_rollback(database_conn_t *conn);
bool database_conn_in_txn(const database_conn_t *conn);
bool database_conn_failed(const database_conn_t *conn);
/* 마지막 BEGIN이 다른 writer 때문에 거절됐다(재시도 가능) */
bool database_conn_begin_busy(const database_conn_t *conn);
database_txn_t *database_conn_txn(database_conn_t *conn);

/* manager 상태 조회 */
bool database_txn_layer_poisoned(const database_t *db);
uint32_t database_txn_epoch(const database_t *db);

#endif
