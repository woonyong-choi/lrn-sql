#ifndef DATABASE_INTERNAL_H
#define DATABASE_INTERNAL_H

/*
 * database의 쓰기 작업(여러 문장을 게시 하나로 묶는 단위)과 transaction 계층이 쓰는 내부 접점. 공개 API가
 * 아니며 src/sql 안의 database_txn.c만 쓴다. database.c는 transaction.h를 알지 못한다.
 *
 * 쓰기 작업의 모양: 작업이 열려 있는 동안 변경 문장은 카탈로그를 게시(commit)하지 않고 작업 카탈로그(staged)에
 * 쌓는다. 데이터 페이지는 이미 COW로 새 자리에 쓰였지만 어느 제어 페이지도 가리키지 않는다. 작업 밖(게시된
 * 카탈로그)에서 읽는 문장은 작업의 변경을 보지 못한다. 작업이 끝날 때 한 번의 게시로 모두 반영하거나 모두 버린다.
 * 옛 페이지의 해제는 게시 성공 뒤로 미룬다. 작업이 만든 새 페이지는 취소 때 돌려준다.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/database.h"
#include "storage/page_file.h"

/* 작업 안의 문장이 이미 실패해 rollback이 필요한 상태에서 받는 오류 결과의 code */
#define DATABASE_CODE_WORK_FAILED 26
/* 다른 작업이 쓰기 중이라 변경 문장을 실행할 수 없을 때 오류 결과의 code */
#define DATABASE_CODE_WRITER_BUSY 29

typedef enum {
    DATABASE_WORK_NOT_APPLIED,  /* 게시된 상태가 그대로다 */
    DATABASE_WORK_UNKNOWN,      /* 제어 쓰기를 시도한 뒤 실패했다. 핸들은 poison이다 */
    DATABASE_WORK_APPLIED
} database_work_outcome_t;

typedef struct {
    /* 작업 밖에서 들어온 문장(autocommit)의 진입점. layer가 작업을 열고 database_exec_in_work로 실행한다 */
    database_exec_status_t (*exec)(void *ctx, database_t *db, const database_exec_options_t *opts,
                                   const uint8_t *sql, size_t size, const lexer_binding_t *binding,
                                   result_t **out);
    /* database_close가 파일을 닫기 전에 부른다. 실패하면 닫지 않는다 */
    database_status_t (*close)(void *ctx, database_t *db);
} database_layer_t;

/*
 * 트랜잭션 제어문(BEGIN·COMMIT·ROLLBACK). 연결(database_conn)이 sink를 걸어 두면 database_exec_ex·
 * database_exec_in_work는 제어문을 실행하지 않고 요청만 sink에 적고 DATABASE_EXEC_CONTROL을 돌려준다(결과 없음).
 * sink가 없으면 제어문은 RESULT_E_SEMANTIC 오류 결과다(연결 밖에서는 의미가 없다).
 */
#define DATABASE_EXEC_CONTROL ((database_exec_status_t)101)
typedef enum { DATABASE_CONTROL_NONE, DATABASE_CONTROL_BEGIN, DATABASE_CONTROL_COMMIT, DATABASE_CONTROL_ROLLBACK } database_control_kind_t;
typedef struct {
    database_control_kind_t kind;
} database_control_t;
void database_set_control_sink(database_t *db, database_control_t *sink);
/* 성공 결과(COMMAND, 영향 행 0)를 만든다. ledger는 opts를 따른다 */
database_exec_status_t database_command_result(database_t *db, const database_exec_options_t *opts, const char *tag,
                                               result_t **out);

/* 계층을 붙인다. 한 번만 붙일 수 있다. layer와 ctx는 database_close까지 살아 있어야 한다 */
database_status_t database_set_layer(database_t *db, const database_layer_t *layer, void *ctx);

page_file_t *database_page_file(database_t *db);
void *database_layer_ctx(const database_t *db);
/* 계층이 만든 오류 결과(예: 트랜잭션 계층 거절). ledger와 취소는 opts를 따른다 */
database_exec_status_t database_error_result(database_t *db, const database_exec_options_t *opts,
                                             result_error_class_t error_class, int code, const char *message,
                                             result_t **out);
/* 게시 결과를 알 수 없어 이 핸들을 더는 쓰지 않는다 */
void database_poison(database_t *db);

/*
 * 작업 밖에서 읽기만 하는 실행. 게시된 카탈로그를 기준으로 SELECT를 실행하고 다른 문장은 RESULT_E_BUSY
 * 오류 결과로 거절한다(다른 작업이 쓰기 중이라 변경 문장은 실행할 수 없다는 뜻). 작업의 미확정 변경은 보이지 않는다.
 */
database_exec_status_t database_exec_read_only(database_t *db, const database_exec_options_t *opts,
                                               const uint8_t *sql, size_t size,
                                               const lexer_binding_t *binding, result_t **out);

/* 작업 시작. writable이고 poison이 아니며 열린 작업이 없어야 한다. */
database_status_t database_work_begin(database_t *db);
bool database_work_active(const database_t *db);
/* 작업 안의 문장이 실패했다. 이후 문장은 거절하고 commit은 하지 않는다 */
bool database_work_failed(const database_t *db);
/* 이 작업이 카탈로그를 바꿨는지(게시할 것이 있는지) */
bool database_work_dirty(const database_t *db);

/*
 * 작업의 문장을 실행한다. SELECT도 작업 카탈로그를 기준으로 읽는다. 문장이 오류 결과로 끝나면(또는 NOMEM이면)
 * 작업은 failed가 된다. failed인 작업에는 문장을 실행하지 않고 DATABASE_CODE_WORK_FAILED 오류 결과를 돌려준다.
 */
database_exec_status_t database_exec_in_work(database_t *db, const database_exec_options_t *opts,
                                             const uint8_t *sql, size_t size,
                                             const lexer_binding_t *binding, result_t **out);

/*
 * 작업을 게시한다. 바꾼 것이 없으면 아무것도 쓰지 않고 APPLIED다. 게시 뒤의 정리는 database_work_end가 한다.
 * errno는 실패 원인을 남긴다. NOT_APPLIED의 nomem이 true이면 메모리 부족(예산 포함)이다.
 */
typedef struct {
    database_work_outcome_t outcome;
    bool nomem;
    int saved_errno;
} database_work_result_t;
database_work_result_t database_work_commit(database_t *db);

/*
 * 작업을 닫는다. APPLIED면 작업 카탈로그가 게시된 카탈로그가 되고 대체된 옛 페이지를 놓는다. NOT_APPLIED면
 * 작업이 새로 만든 페이지를 놓고 변경을 버린다. UNKNOWN이면 메모리만 정리하고 어떤 페이지도 놓지 않으며 핸들은
 * poison이다.
 */
void database_work_end(database_t *db, database_work_outcome_t outcome);

#endif
