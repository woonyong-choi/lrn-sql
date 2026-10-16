#ifndef ACK_QUEUE_INTERNAL_H
#define ACK_QUEUE_INTERNAL_H

#include "queue/ack_queue.h"
#include "sql/session.h"
#include "sql/result.h"
#include "storage/value.h"

/* 문장 하나의 최대 SQL 길이. 이름은 ACKQ_NAME_MAX 이하라 가장 긴 문장도 들어간다. */
#define ACKQ_SQL_MAX 512u

struct ackq {
    session_db_t *db;               /* 설치한 데이터베이스. 연산의 세션은 이 데이터베이스의 것이어야 한다 */
    char name[ACKQ_NAME_MAX + 1];
    char table[ACKQ_NAME_MAX + 8];
    uint32_t max_attempts;
    uint32_t begin_timeout_ms;
    uint64_t (*now_ms)(void *ctx);
    void *clock_ctx;
};

/* 한 연산이 문장을 실행하는 세션. 세션에 열려 있던 트랜잭션(호출자의 것)이면 own이 false이고 commit하지 않는다. */
typedef struct {
    session_t *session;
    bool own;
    bool dirty;   /* 쓰기 문장을 실행했다. 자기 트랜잭션은 dirty일 때만 commit한다 */
} ackq_scope_t;

uint64_t ackq_now(const ackq_t *queue);

/*
 * 세션에 트랜잭션이 열려 있으면 그 안에서, 없으면 write일 때 세션에 자기 트랜잭션을 연다. write가 false이고 열린
 * 트랜잭션이 없으면 트랜잭션 없이(문장마다 자동 commit) 읽는다.
 */
ackq_status_t ackq_scope_open(ackq_t *queue, session_t *session, bool write, ackq_scope_t *scope);
/* result가 ACKQ_OK이고 dirty이면 commit해 그 결과를 돌려준다. 아니면 rollback하고 result를 그대로 돌려준다. */
ackq_status_t ackq_scope_close(ackq_scope_t *scope, ackq_status_t result);

/*
 * 문장을 실행한다. 성공(ROWS·COMMAND)이면 *out에 결과를 주고 ACKQ_OK다. 오류 결과이면 결과를 해제하고 매핑한 상태를
 * 돌려주며 *sql_class에 RESULT 오류 분류를 준다(NULL 가능).
 */
ackq_status_t ackq_run(ackq_t *queue, ackq_scope_t *scope, bool write, const char *sql, const value_view_t *params,
                       size_t param_count, result_t **out, result_error_class_t *sql_class);

/* 결과의 행을 다 읽었는지(DONE) 확인한다. 중간 실패는 매핑한 상태로 돌려준다. */
ackq_status_t ackq_finish_rows(result_next_t last, result_t *result);
/* COMMAND 결과의 영향 행 수가 expected와 같은지 확인한다. 다르면 ACKQ_INTERNAL */
ackq_status_t ackq_expect_affected(const result_t *result, uint64_t expected);

value_view_t ackq_int(int64_t value);
value_view_t ackq_text(const char *text);
value_view_t ackq_blob(const uint8_t *bytes, size_t size);
bool ackq_to_i64(uint64_t value, int64_t *out);

#endif
