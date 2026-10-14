#ifndef DATABASE_TXN_H
#define DATABASE_TXN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/database.h"
#include "sql/mem_ledger.h"
#include "storage/transaction.h"
#include "storage/wal.h"

/*
 * database 위의 트랜잭션 경계(#8). 여러 문장을 하나의 게시로 확정하거나 모두 버리는 공개 transaction handle과,
 * 데이터 파일과 WAL의 짝(durable_pair) 위의 durable commit과 재시작 복구를 제공한다. 상태 기계·commit 결과·writer
 * 경합·timeout·취소·poison은 storage/transaction.h(#80)의 manager가 맡고, 이 모듈은 그 backend를 database로
 * 구현한다.
 *
 * 모델
 *  - 쓰기 트랜잭션은 한 번에 하나다(단일 writer). 다른 호출은 begin에서 timeout만큼 기다린 뒤 TXN_BUSY·TXN_TIMEOUT이다.
 *  - 선언한 격리는 SNAPSHOT 하나다. 작업이 열려 있는 동안 게시된 상태는 바뀌지 않으므로 이 트랜잭션은 시작 때의
 *    게시된 상태와 자기 변경만 본다. 읽기 전용 트랜잭션 handle은 지원하지 않는다(TXN_UNSUPPORTED). 여러 문장에
 *    걸친 snapshot을 주려면 옛 페이지 회수를 열린 snapshot과 맞춰야 하는데 첫 구현은 문장 단위 직렬화만 보장한다.
 *  - 문장 하나가 오류 결과로 끝나면 그 트랜잭션은 실패 상태다. 이후 문장은 DATABASE_CODE_WORK_FAILED 오류 결과로
 *    거절되고 commit은 TXN_INVALID이므로 명시적 rollback이 필요하다. database_txn_failed로 확인한다.
 *  - 트랜잭션 밖의 database_exec는 문장마다 자동 commit이다. 다른 트랜잭션이 쓰는 중이면 SELECT는 게시된
 *    상태(미확정 변경 없음)를 읽고 변경 문장은 RESULT_E_BUSY 오류 결과다.
 *  - volatile과 durable을 구분한다. guarantee가 TXN_GUARANTEE_DURABLE이면 문장을 WAL에 쌓고 commit에서 COMMIT 레코드를
 *    fsync한 뒤 데이터 파일에 게시한다. 성공은 WAL이 동기화된 뒤에만 돌아온다. 그보다 낮은 보증(SYNCED·VISIBLE)은 WAL
 *    없이 데이터 파일 게시만 하며 재시작 복구를 약속하지 않는다.
 *  - 외부 HTTP·파일 부작용의 원자성은 제공하지 않는다.
 *
 * durable commit 순서와 복구
 *   변경 문장마다 DATA 레코드(SQL과 바인딩)를 WAL 버퍼에 쌓는다. commit은 (1) WAL COMMIT fsync — 승인 지점 —,
 *   (2) 데이터 파일 제어 페이지에 카탈로그와 applied_lsn을 한 번에 게시한다. (1) 뒤 (2) 전에 멈추면 재시작이 WAL의
 *   COMMITTED 거래를 applied_lsn 이후부터 같은 순서로 다시 실행하고 거래마다 applied_lsn과 함께 게시한다. rollback한
 *   거래와 끝나지 않은 거래의 DATA는 실행하지 않는다. 같은 입력의 결정적 재실행을 전제로 하므로 비결정 요소(시계·난수)가
 *   있는 문장이 생기면 이 방식을 다시 정해야 한다(현재 문장에는 없다).
 *   (1)이 실패하면 승인하지 않은 것이다(NOT_APPLIED 또는 UNKNOWN). (1)은 성공했는데 (2)가 실패하면 로그에는 확정이지만
 *   이 핸들은 반영하지 못했으므로 TXN_COMMIT_UNCERTAIN으로 응답하고 manager와 database가 poison이 된다. 다시 열면 복구가
 *   그 거래를 반영한다.
 *   영속 epoch: 열 때 txn_manager가 영속된 floor보다 큰 epoch를 받고 durable_pair_persist_epoch가 두 슬롯에 commit한
 *   뒤에만 첫 트랜잭션을 시작한다. 그래서 WAL 없는 트랜잭션 id도 재시작 뒤 재사용되지 않는다.
 *
 * 한계: 전원 장애·lost write 검증 없음(프로세스 종료와 I/O 실패 주입까지). 여러 스레드가 같은 database를 직접 호출하는
 * 직렬화는 호출자 몫이다(session 계층이 문장 호출을 직렬화한다). 문장 로그 한 건은 WAL_MAX_PAYLOAD(1MiB)를 넘을 수 없다.
 */

typedef struct database_txn database_txn_t;

typedef struct {
    /* NULL이면 WAL 없이 연다(durable 보증 거절). 있으면 데이터 파일과 짝으로 판정한다(durable_pair.h) */
    const char *wal_path;
    /* 로그 없는 파일을 durable로 전환하거나 중단된 전환을 마무리하는 것을 허용한다 */
    bool convert;
    /* 전환 때 제어 페이지에 기록하는 첫 epoch floor. 보통 0 */
    uint32_t initial_epoch;
    const wal_limits_t *wal_limits;
    /* 상주 몫을 계상할 ledger. NULL이면 계상하지 않는다. 열린 뒤에 database_attach_ledger와 같게 붙인다 */
    const mem_ledger_t *resident;
    /* 트랜잭션 밖 변경 문장이 다른 쓰기를 기다리는 시간. 0이면 기다리지 않고 BUSY */
    uint32_t autocommit_timeout_ms;
} database_txn_options_t;

typedef struct {
    bool wal_present;            /* WAL과 짝으로 열렸다 */
    bool converted;              /* 이번 열기가 durable 전환 단계를 수행했다 */
    uint64_t replayed_txns;      /* 재시작 복구가 다시 반영한 거래 수 */
    uint64_t replayed_statements;
    uint32_t epoch;              /* 이번 열기가 영속화한 epoch(0이면 영속화 없음) */
    int pair_status;             /* durable_pair_status_t. 열기 실패 원인 */
} database_txn_info_t;

/*
 * 쓰기로 열고 트랜잭션 계층을 붙인다. wal_path가 있으면 짝을 판정해(DOWNGRADED·MISMATCH·WAL_MISSING는 거절) 복구를
 * 마친 뒤에만 돌려준다. 실패하면 *out은 NULL이고 두 파일은 판정이 허용한 만큼만 바뀐다. 상태는 database_status_t로
 * 돌려주며 짝 판정 실패는 info->pair_status에 자세히 있다(CORRUPT·UNSUPPORTED·IO·LOCKED·NOMEM으로 환산).
 */
database_status_t database_open_transactional(const char *path, const database_txn_options_t *options,
                                              database_t **out, database_txn_info_t *info);

/*
 * 트랜잭션을 시작한다. options는 transaction.h의 txn_begin_options_t이며 NULL이면 기본값(READ_WRITE, SNAPSHOT,
 * VISIBLE, 기다리지 않음)이다. READ_ONLY와 SNAPSHOT 외의 격리는 TXN_UNSUPPORTED, WAL 없는 열기에서 DURABLE은
 * TXN_UNSUPPORTED다. 성공한 *out은 database_txn_release로 해제한다.
 */
txn_status_t database_txn_begin(database_t *db, const txn_begin_options_t *options, database_txn_t **out);

/* 트랜잭션 안에서 문장 하나를 실행한다(database_exec_ex와 같은 반환 계약). ACTIVE가 아니면 DATABASE_EXEC_INVALID. */
database_exec_status_t database_txn_exec(database_txn_t *txn, const database_exec_options_t *opts,
                                         const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                         result_t **out);

/* 문장 오류 뒤 rollback이 필요한 상태인지 */
bool database_txn_failed(const database_txn_t *txn);

/* transaction.h와 같은 의미. 실패 상태의 commit은 TXN_INVALID이고 트랜잭션은 ACTIVE로 남아 rollback을 기다린다 */
txn_status_t database_txn_commit(database_txn_t *txn, txn_commit_info_t *info);
txn_status_t database_txn_rollback(database_txn_t *txn);
txn_status_t database_txn_release(database_txn_t **txn);
txn_state_t database_txn_state(const database_txn_t *txn);
uint64_t database_txn_id(const database_txn_t *txn);

/* 응답을 잃은 호출자가 id의 commit 결과를 조회한다(transaction.h의 txn_query_commit). */
txn_status_t database_txn_query_commit(database_t *db, uint64_t txn_id, txn_commit_info_t *info);

/* manager 상태 조회 */
bool database_txn_layer_poisoned(const database_t *db);
uint32_t database_txn_epoch(const database_t *db);

#endif
