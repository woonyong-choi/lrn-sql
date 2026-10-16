#ifndef TOTEDB_H
#define TOTEDB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Python 같은 다른 언어에서 부르는 평탄한(flat) C 인터페이스(#140). 구조체 포인터를 값으로 넘기지 않고 핸들과 바이트
 * 버퍼만 쓴다. ack_queue.h의 큐와 database를 감싸며 database·storage 코어는 이 파일을 모른다. 이 파일은 공유
 * 라이브러리 libtotedb에만 들어가고 DB 단독 사용 빌드(DATABASE_OBJS)에는 들어가지 않는다.
 *
 * 스레드: 모든 호출은 tote_db_t의 mutex로 직렬화된다. 이 라이브러리는 mutex를 쥔 채 다른 writer를 기다리지 않는다.
 * 트랜잭션 시작과 큐의 자기 트랜잭션은 기다리지 않고 TOTE_BUSY를 돌려주므로 호출자가 mutex 밖에서 다시 시도한다.
 *
 * 세션: 문장과 큐 연산은 session.h의 세션으로 실행한다. 트랜잭션 밖의 호출은 연결 하나에 하나인 자동 commit 세션을,
 * 트랜잭션 안의 호출은 트랜잭션마다 따로 연 세션을 쓴다. 연결의 세션 풀은 한도가 없다.
 *
 * 여러 프로세스: 같은 파일을 여러 프로세스가 각자 tote_open으로 연다. 쓰기 트랜잭션은 파일 전체에서 한 번에 하나이고,
 * 다른 프로세스가 쓰는 동안 변경 문장·큐 연산·tote_begin은 기다리지 않고 TOTE_BUSY(문장은 BUSY 오류 결과)다. SELECT는
 * 기다리지 않고 마지막 게시 상태를 읽는다. 게시하는 순간(commit의 마지막 fsync)만 비켜 준다. 핸들은 연
 * 프로세스에서만 쓴다. fork한 자식에서는 tote_close를 포함한 모든 호출이 TOTE_DB_FORKED이며 아무것도 하지 않는다.
 *
 * 반환값: 아래 TOTE_* 값이다. 1~16은 큐 모듈의 상태와 같은 번호다.
 */
#define TOTE_OK 0
#define TOTE_INVALID 1        /* 인자 오류: 이름·길이·개수·시간, 다른 database의 트랜잭션 */
#define TOTE_NOMEM 2
#define TOTE_LIMIT 3          /* payload·결과 크기 한도 */
#define TOTE_BUSY 4           /* 다른 writer(다른 프로세스 포함)가 쓰는 중이다. 반영한 것이 없으며 다시 시도할 수 있다 */
#define TOTE_NOT_INSTALLED 5  /* 큐가 설치되지 않았다 */
#define TOTE_NOT_FOUND 6      /* id가 없다 */
#define TOTE_STALE 7          /* fence가 현재 claim이 아니거나 상태가 맞지 않는다. 아무것도 바꾸지 않았다 */
#define TOTE_CONFLICT 8       /* 같은 idempotency key에 payload가 다르다 */
#define TOTE_UNSUPPORTED 9
#define TOTE_IO 10            /* 저장소 오류. commit 전이라면 상태는 그대로다 */
#define TOTE_NOT_APPLIED 11   /* commit이 확정적으로 적용되지 않았다 */
#define TOTE_UNCERTAIN 12     /* commit 결과를 알 수 없다. 성공으로 취급하면 안 된다. 다시 열어 확인한다 */
#define TOTE_POISONED 13      /* 불확실한 commit 뒤 이 핸들로는 더 쓰지 못한다. 닫고 다시 연다 */
#define TOTE_TXN_FAILED 14    /* 트랜잭션이 이미 실패 상태다. rollback 뒤 다시 한다 */
#define TOTE_INTERNAL 15
#define TOTE_CANCELLED 16     /* tote_cancel로 취소됐다 */
#define TOTE_EMPTY 50         /* claim할 작업이 없다 */
#define TOTE_DB_INVALID 101   /* database 열기 인자 오류 */
#define TOTE_DB_IO 102
#define TOTE_DB_CORRUPT 103
#define TOTE_DB_LOCKED 104
#define TOTE_DB_UNSUPPORTED 105
#define TOTE_DB_NOMEM 106
#define TOTE_DB_NOT_FOUND 107 /* create가 0인데 파일이 없다 */
#define TOTE_DB_FORKED 108    /* fork한 자식이 부모가 연 핸들을 썼다. 아무것도 하지 않았다. 자식은 tote_open으로 새로 연다 */

typedef struct tote_db tote_db_t;
typedef struct tote_queue tote_queue_t;
typedef struct tote_txn tote_txn_t;
typedef uint64_t (*tote_clock_fn)(void *ctx);

typedef enum { TOTE_PARAM_NULL = 0, TOTE_PARAM_INTEGER = 1, TOTE_PARAM_REAL = 2, TOTE_PARAM_TEXT = 3, TOTE_PARAM_BLOB = 4 } tote_param_type_t;

/* SQL 매개변수 하나. TEXT·BLOB은 bytes와 size를 쓴다. */
typedef struct {
    int32_t type;
    int64_t integer;
    double real;
    const uint8_t *bytes;
    size_t size;
} tote_param_t;

/* 이 헤더의 버전(유의적 버전). 1.0 전에는 파일 형식과 API가 바뀔 수 있다 */
#define TOTE_VERSION "0.1.0"

/* 링크한 라이브러리의 버전. 헤더의 TOTE_VERSION과 다르면 다른 빌드를 섞어 쓴 것이다 */
const char *tote_version(void);
const char *tote_status_name(int status);
void tote_free(void *pointer);

/*
 * tote_exec가 만드는 인코딩 버퍼의 상한(바이트). 기본은 TOTE_DEFAULT_MAX_RESULT_BYTES다. 넘는 결과는 부분 행을 주지 않고
 * TOTE_LIMIT으로 거절하며, 메모리 할당 실패는 TOTE_NOMEM이다. TEXT·BLOB 셀과 열 이름은 u32 길이로 인코딩하므로
 * UINT32_MAX를 넘는 길이도 TOTE_LIMIT이다. 이 값은 프로세스 전역이며 결과를 만들기 전에 정한다.
 */
#define TOTE_DEFAULT_MAX_RESULT_BYTES ((size_t)64 << 20)
void tote_set_max_result_bytes(size_t limit);
size_t tote_max_result_bytes(void);

/* path의 DB를 연다. create가 0이 아니면 없을 때 만든다. commit은 항상 DURABLE이다(database_txn.h). */
int tote_open(const char *path, int create, tote_db_t **out);
/* 살아 있는 큐 핸들(tote_queue_t)이나 트랜잭션(tote_txn_t)이 있으면 TOTE_BUSY이고 핸들은 그대로다. 먼저 닫는다. */
int tote_close(tote_db_t **db);
/*
 * 지금 실행 중인 문장이나 큐 연산 하나를 취소한다. mutex를 쥐지 않으므로 다른 스레드에서 부른다. 취소된 문장은
 * RESULT_E_CANCELLED 오류 결과(큐 연산은 TOTE_CANCELLED)이며 게시를 시작한 문장은 실제 결과로 끝난다. 끝나는 순간과
 * 겹친 요청은 같은 세션의 다음 문장 하나에 적용될 수 있다(session_cancel 계약).
 */
void tote_cancel(tote_db_t *db);

/*
 * SQL 문장 하나를 실행한다. txn이 NULL이 아니면 그 트랜잭션 안에서 실행한다. 성공하면 *out에 결과를 인코딩한 버퍼를
 * 주고(tote_free로 해제) 문장 수준 오류도 인코딩된 오류 결과로 돌아온다. 인코딩:
 *   u8 kind(0 행, 1 명령, 2 오류)
 *   행:   u32 열 수, (u32 이름 길이, 이름, u8 형식, u8 NULL 가능)*열 수, 행마다 [u8 1, 셀*열 수], 끝에 u8 0
 *         형식은 셀 type과 같은 번호이며 0은 식의 형식을 정하지 못했다는 뜻이다(예: SELECT NULL)
 *   명령: u32 tag 길이, tag, u64 영향 행 수
 *   오류: u32 분류, i32 코드, u32 메시지 길이, 메시지
 *   셀:   u8 type(0 NULL,1 INTEGER,2 REAL,3 TEXT,4 BLOB), INTEGER는 i64, REAL은 f64, TEXT·BLOB은 u32 길이와 바이트
 * 모든 정수는 little-endian이다. txn은 이 db의 것이어야 하며 아니면 TOTE_INVALID다(문장을 실행하기 전에 거절한다).
 * 결과를 읽고 해제하는 동안에도 db의 mutex를 놓지 않는다.
 */
int tote_exec(tote_db_t *db, tote_txn_t *txn, const char *sql, const tote_param_t *params, size_t param_count,
               uint8_t **out, size_t *out_size);

/* 쓰기 트랜잭션 시작. 다른 writer가 있으면 기다리지 않고 TOTE_BUSY다. */
int tote_begin(tote_db_t *db, tote_txn_t **out);
/* commit·rollback은 성공하든 실패하든 트랜잭션을 해제하고 *txn을 NULL로 만든다. */
int tote_commit(tote_txn_t **txn);
int tote_rollback(tote_txn_t **txn);
/* 문장 오류로 실패 상태인지(commit하면 안 되고 rollback만 된다) */
int tote_txn_failed(const tote_txn_t *txn);

/* 큐 옵션. 모두 0이면 기본값이다(ack_queue.h의 ackq_options_t와 같은 뜻). */
typedef struct {
    uint32_t max_attempts;        /* 0이면 제한 없음 */
    size_t max_payload_bytes;     /* 0이면 1 MiB */
    uint64_t idem_retention_ms;   /* 0이면 7일, UINT64_MAX면 purge 뒤 key를 남기지 않음 */
    tote_clock_fn clock;          /* NULL이면 실시간(밀리초) */
    void *clock_ctx;
} tote_queue_options_t;

/* 큐 이름 name을 연다. install이 0이 아니면 없을 때 만든다. options가 NULL이면 모두 기본값이다. */
int tote_queue_open(tote_db_t *db, const char *name, int install, const tote_queue_options_t *options,
                    tote_queue_t **out);
void tote_queue_close(tote_queue_t **queue);

typedef struct {
    uint64_t id;
    uint64_t fence;
    uint32_t attempts;
    uint64_t lease_until_ms;
    uint8_t *payload;   /* tote_queue_item_free로 해제 */
    size_t payload_size;
} tote_queue_item_t;

int tote_queue_put(tote_queue_t *queue, tote_txn_t *txn, const uint8_t *payload, size_t size, const char *idem_key, uint64_t *id,
             int *duplicate);
/* id가 0이면 다음 작업, 아니면 그 작업을 claim한다. 가져올 것이 없으면 TOTE_EMPTY(id 지정이 불가하면 TOTE_STALE). */
int tote_queue_get(tote_queue_t *queue, tote_txn_t *txn, uint64_t id, uint64_t lease_ms, tote_queue_item_t *item);
void tote_queue_item_free(tote_queue_item_t *item);
int tote_queue_ack(tote_queue_t *queue, tote_txn_t *txn, uint64_t id, uint64_t fence);
/* 같은 claim의 lease를 지금 + lease_ms로 늘린다. fence는 그대로다. 성공하면 새 만료 시각을 *lease_until에 준다(NULL 가능). */
int tote_queue_extend(tote_queue_t *queue, tote_txn_t *txn, uint64_t id, uint64_t fence, uint64_t lease_ms,
                      uint64_t *lease_until);
int tote_queue_fail(tote_queue_t *queue, tote_txn_t *txn, uint64_t id, uint64_t fence, const char *reason);
int tote_queue_nack(tote_queue_t *queue, tote_txn_t *txn, uint64_t id, uint64_t fence, uint64_t retry_delay_ms, const char *reason,
              int *terminal);
int tote_queue_update(tote_queue_t *queue, tote_txn_t *txn, uint64_t id, uint64_t fence, const uint8_t *payload, size_t size);
int tote_queue_recover(tote_queue_t *queue, tote_txn_t *txn, size_t limit, size_t *recovered);
int tote_queue_release_leases(tote_queue_t *queue, tote_txn_t *txn, size_t limit, size_t *released);
int tote_queue_purge(tote_queue_t *queue, tote_txn_t *txn, int include_failed, size_t limit, size_t *purged);
/* counts: ready, leased, acked, failed 순서의 u64 네 개 */
int tote_queue_stats(tote_queue_t *queue, tote_txn_t *txn, uint64_t counts[4]);

typedef struct {
    uint64_t id;
    int32_t state;
    uint64_t fence;
    uint32_t attempts;
    uint64_t ready_at_ms;
    uint64_t lease_until_ms;
    uint64_t created_at_ms;
    char note[201];
    uint8_t *payload;   /* with_payload일 때만. tote_queue_list_free로 해제 */
    size_t payload_size;
} tote_queue_list_item_t;

/* items는 limit개 이상의 배열이다. 채운 수를 *count에 준다. 해제는 tote_queue_list_free(items, *count). */
int tote_queue_list(tote_queue_t *queue, tote_txn_t *txn, uint64_t after_id, size_t limit, int with_payload, tote_queue_list_item_t *items,
              size_t *count);
void tote_queue_list_free(tote_queue_list_item_t *items, size_t count);

#endif
