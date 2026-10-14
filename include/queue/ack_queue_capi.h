#ifndef ACK_QUEUE_CAPI_H
#define ACK_QUEUE_CAPI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Python 같은 다른 언어에서 부르는 평탄한(flat) C 인터페이스(#140). 구조체 포인터를 값으로 넘기지 않고 핸들과 바이트
 * 버퍼만 쓴다. ack_queue.h의 큐와 database를 감싸며 database·storage 코어는 이 파일을 모른다. 이 파일은 공유
 * 라이브러리 liblrnackq에만 들어가고 DB 단독 사용 빌드(DATABASE_OBJS)에는 들어가지 않는다.
 *
 * 스레드: 모든 호출은 lrndb_t의 mutex로 직렬화된다. 이 라이브러리는 mutex를 쥔 채 다른 writer를 기다리지 않는다.
 * 트랜잭션 시작과 큐의 자기 트랜잭션은 기다리지 않고 LRN_BUSY를 돌려주므로 호출자가 mutex 밖에서 다시 시도한다.
 *
 * 반환값: 0(LRN_OK)과 ackq_status_t의 숫자(1~15)가 그대로 쓰인다. 그 밖의 LRN_* 값은 아래에 있다.
 */
#define LRN_OK 0
#define LRN_EMPTY 50         /* claim할 작업이 없다 */
#define LRN_DB_INVALID 101   /* database 열기 인자 오류 */
#define LRN_DB_IO 102
#define LRN_DB_CORRUPT 103
#define LRN_DB_LOCKED 104
#define LRN_DB_UNSUPPORTED 105
#define LRN_DB_NOMEM 106
#define LRN_DB_NOT_FOUND 107 /* create가 0인데 파일이 없다 */

typedef struct lrndb lrndb_t;
typedef struct lrnq lrnq_t;
typedef struct lrntxn lrntxn_t;
typedef uint64_t (*lrn_clock_fn)(void *ctx);

typedef enum { LRN_PARAM_NULL = 0, LRN_PARAM_INTEGER = 1, LRN_PARAM_REAL = 2, LRN_PARAM_TEXT = 3, LRN_PARAM_BLOB = 4 } lrn_param_type_t;

/* SQL 매개변수 하나. TEXT·BLOB은 bytes와 size를 쓴다. */
typedef struct {
    int32_t type;
    int64_t integer;
    double real;
    const uint8_t *bytes;
    size_t size;
} lrn_param_t;

const char *lrn_status_name(int status);
void lrn_free(void *pointer);

/*
 * lrndb_exec가 만드는 인코딩 버퍼의 상한(바이트). 기본은 LRN_DEFAULT_MAX_RESULT_BYTES다. 넘는 결과는 부분 행을 주지 않고
 * ACKQ_LIMIT으로 거절하며, 메모리 할당 실패는 ACKQ_NOMEM이다. TEXT·BLOB 셀과 열 이름은 u32 길이로 인코딩하므로
 * UINT32_MAX를 넘는 길이도 ACKQ_LIMIT이다. 이 값은 프로세스 전역이며 결과를 만들기 전에 정한다.
 */
#define LRN_DEFAULT_MAX_RESULT_BYTES ((size_t)64 << 20)
void lrn_set_max_result_bytes(size_t limit);
size_t lrn_max_result_bytes(void);

/* path의 DB를 연다. wal_path가 NULL이 아니면 짝 WAL과 함께 연다(처음이면 durable로 전환). create가 0이 아니면 없을 때 만든다. */
int lrndb_open(const char *path, const char *wal_path, int create, lrndb_t **out);
/* 살아 있는 큐 핸들(lrnq_t)이나 트랜잭션(lrntxn_t)이 있으면 ACKQ_BUSY이고 핸들은 그대로다. 먼저 닫는다. */
int lrndb_close(lrndb_t **db);

/*
 * SQL 문장 하나를 실행한다. txn이 NULL이 아니면 그 트랜잭션 안에서 실행한다. 성공하면 *out에 결과를 인코딩한 버퍼를
 * 주고(lrn_free로 해제) 문장 수준 오류도 인코딩된 오류 결과로 돌아온다. 인코딩:
 *   u8 kind(0 행, 1 명령, 2 오류)
 *   행:   u32 열 수, (u32 이름 길이, 이름)*열 수, 행마다 [u8 1, 셀*열 수], 끝에 u8 0
 *   명령: u32 tag 길이, tag, u64 영향 행 수
 *   오류: u32 분류, i32 코드, u32 메시지 길이, 메시지
 *   셀:   u8 type(0 NULL,1 INTEGER,2 REAL,3 TEXT,4 BLOB), INTEGER는 i64, REAL은 f64, TEXT·BLOB은 u32 길이와 바이트
 * 모든 정수는 little-endian이다. txn은 이 db의 것이어야 하며 아니면 ACKQ_INVALID다(문장을 실행하기 전에 거절한다).
 * 결과를 읽고 해제하는 동안에도 db의 mutex를 놓지 않는다.
 */
int lrndb_exec(lrndb_t *db, lrntxn_t *txn, const char *sql, const lrn_param_t *params, size_t param_count,
               uint8_t **out, size_t *out_size);

/* 쓰기 트랜잭션 시작. 다른 writer가 있으면 기다리지 않고 LRN_BUSY(ACKQ_BUSY)다. durable이 0이 아니면 WAL 보증이다. */
int lrndb_begin(lrndb_t *db, int durable, lrntxn_t **out);
/* commit·rollback은 성공하든 실패하든 트랜잭션을 해제하고 *txn을 NULL로 만든다. */
int lrndb_commit(lrntxn_t **txn);
int lrndb_rollback(lrntxn_t **txn);
/* 문장 오류로 실패 상태인지(commit하면 안 되고 rollback만 된다) */
int lrndb_txn_failed(const lrntxn_t *txn);

/* 큐 이름 name을 연다. install이 0이 아니면 없을 때 만든다. durable이 0이면 VOLATILE이다. clock이 NULL이면 실시간이다. */
int lrnq_open(lrndb_t *db, const char *name, int install, int durable, uint32_t max_attempts, lrn_clock_fn clock,
              void *clock_ctx, lrnq_t **out);
void lrnq_close(lrnq_t **queue);

typedef struct {
    uint64_t id;
    uint64_t fence;
    uint32_t attempts;
    uint64_t lease_until_ms;
    uint8_t *payload;   /* lrnq_item_free로 해제 */
    size_t payload_size;
} lrnq_item_t;

int lrnq_put(lrnq_t *queue, lrntxn_t *txn, const uint8_t *payload, size_t size, const char *idem_key, uint64_t *id,
             int *duplicate);
/* id가 0이면 다음 작업, 아니면 그 작업을 claim한다. 가져올 것이 없으면 LRN_EMPTY(id 지정이 불가하면 ACKQ_STALE). */
int lrnq_get(lrnq_t *queue, lrntxn_t *txn, uint64_t id, uint64_t lease_ms, lrnq_item_t *item);
void lrnq_item_free(lrnq_item_t *item);
int lrnq_ack(lrnq_t *queue, lrntxn_t *txn, uint64_t id, uint64_t fence);
int lrnq_fail(lrnq_t *queue, lrntxn_t *txn, uint64_t id, uint64_t fence, const char *reason);
int lrnq_nack(lrnq_t *queue, lrntxn_t *txn, uint64_t id, uint64_t fence, uint64_t retry_delay_ms, const char *reason,
              int *terminal);
int lrnq_update(lrnq_t *queue, lrntxn_t *txn, uint64_t id, uint64_t fence, const uint8_t *payload, size_t size);
int lrnq_recover(lrnq_t *queue, lrntxn_t *txn, size_t limit, size_t *recovered);
int lrnq_release_leases(lrnq_t *queue, lrntxn_t *txn, size_t limit, size_t *released);
int lrnq_purge(lrnq_t *queue, lrntxn_t *txn, int include_failed, size_t limit, size_t *purged);
/* counts: ready, leased, acked, failed 순서의 u64 네 개 */
int lrnq_stats(lrnq_t *queue, lrntxn_t *txn, uint64_t counts[4]);

typedef struct {
    uint64_t id;
    int32_t state;
    uint64_t fence;
    uint32_t attempts;
    uint64_t ready_at_ms;
    uint64_t lease_until_ms;
    uint64_t created_at_ms;
    char note[201];
    uint8_t *payload;   /* with_payload일 때만. lrnq_list_free로 해제 */
    size_t payload_size;
} lrnq_listitem_t;

/* items는 limit개 이상의 배열이다. 채운 수를 *count에 준다. 해제는 lrnq_list_free(items, *count). */
int lrnq_list(lrnq_t *queue, lrntxn_t *txn, uint64_t after_id, size_t limit, int with_payload, lrnq_listitem_t *items,
              size_t *count);
void lrnq_list_free(lrnq_listitem_t *items, size_t count);

#endif
