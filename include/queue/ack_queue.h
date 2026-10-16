#ifndef ACK_QUEUE_H
#define ACK_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/database.h"
#include "sql/database_txn.h"
#include "sql/mem_ledger.h"

/*
 * 선택형 ACK 큐(#140). 독립 DB 위에 따로 설치하는 라이브러리이며 DB 코어(database·storage)는 이 모듈을 모른다.
 * 큐를 설치하지 않으면 객체·worker·전용 테이블이 생기지 않는다. 큐는 database의 공개 SQL과 트랜잭션 API만 쓰고
 * 큐 하나는 테이블 하나(ackq_<name>)다. 큐마다 따로 설치하며 DB와 같은 파일에 산다.
 *
 * 작업(job) 상태: READY -> LEASED -> ACKED | FAILED, 그리고 LEASED -> READY(nack, 만료, 해제).
 *   claim이 READY 작업(또는 만료된 LEASED)을 LEASED로 바꾸고 fence token을 1 올려 돌려준다. fence는 작업별로 단조 증가하며
 *   재사용되지 않는다. ack·nack·fail·update는 (id, fence)를 받아 그 claim이 지금도 유효할 때만 적용한다. 다른 worker가
 *   다시 claim했거나(fence 불일치) 이미 READY로 돌아갔거나 끝난 작업에 대한 오래된 호출은 ACKQ_STALE로 거절한다.
 *   만료된 lease도 아직 아무도 다시 claim하지 않았다면 ack할 수 있다(같은 fence가 여전히 현재이므로 이중 처리가 아니다).
 *
 * 전달 보장: at-least-once다. worker가 죽거나 lease가 만료되면 같은 작업이 다시 전달된다. 외부 부작용(파일·HTTP)의
 * exactly-once나 원자적 commit은 제공하지 않는다. idempotency key가 같고 payload가 같은 enqueue는 기존 작업을
 * 돌려주고(duplicate), payload가 다르면 ACKQ_CONFLICT다.
 *
 * 내구성: 모든 변경은 database의 commit을 따른다(database_txn.h). 성공은 COW 게시의 fsync가 끝난 뒤에만 돌아오므로
 * 승인한 작업은 프로세스·OS 종료 뒤에도 남는다. commit 결과를 알 수 없으면(ACKQ_UNCERTAIN) 성공으로 돌려주지 않는다. 다시 열어 상태를 확인해야 한다.
 *
 * 같은 트랜잭션: 모든 변경 함수는 database_txn_t를 받는다. NULL이면 함수가 자기 트랜잭션을 열고 commit한다(자동 commit).
 * 호출자의 txn을 주면 그 안에서 문장만 실행하고 commit은 호출자가 한다. 그래서 업무 갱신(database_txn_exec)과 ack를
 * 하나의 database_txn_commit으로 함께 확정하거나 함께 버릴 수 있다. ACKQ_STALE·NOT_FOUND·CONFLICT는 변경 없이 거절하는
 * 정상 결과이며 호출자의 트랜잭션을 실패 상태로 만들지 않는다(그 앞에서 호출자가 이미 한 변경은 호출자가 rollback할지
 * 정한다). 그 밖의 비OK 상태(IO·NOMEM·LIMIT·INTERNAL 등)는 문장이 일부만 실행됐을 수 있으므로 호출자는 그 트랜잭션을
 * rollback해야 한다(commit하면 큐 상태가 일부만 반영될 수 있다).
 *
 * SQL로 표현하지 못해 이 모듈이 대신하는 것(database가 제공하지 않음): ORDER BY·LIMIT(claim은 상태별로 행 메타데이터만
 * 읽어 호출자 쪽에서 가장 작은 id k개를 고른다), AND·OR 조건, UPDATE의 계산식(값은 C에서 계산해 상수로 쓴다),
 * UNIQUE 제약과 보조 index(idempotency key는 전체 스캔으로 찾는다), RETURNING, 행 단위 잠금. 그래서 모든 연산이
 * 테이블 전체 스캔을 하며 claim·ack 하나가 O(행 수)다. 메모리는 payload를 제외한 메타데이터 스캔과 claim 개수 k에 비례하며
 * payload는 선택한 작업의 것만 읽는다. 큰 큐의 성능은 목표가 아니다.
 *
 * 스레드: ackq_t와 같은 database를 쓰는 호출은 한 번에 한 스레드만 한다(database 계약). 시간은 now_ms 콜백이 주며
 * 기본은 CLOCK_REALTIME 밀리초다. lease는 절대 시각으로 저장되므로 재시작 뒤에도 유지되고, 벽시계가 뒤로 가면 lease가
 * 길어질 수 있다. 결정적 시험은 콜백으로 시간을 직접 주입한다.
 */

#define ACKQ_NAME_MAX 40u
#define ACKQ_IDEM_KEY_MAX 128u
#define ACKQ_NOTE_MAX 200u
/* 문장 한 건의 WAL 레코드 한도(1MiB)와 SQL·바인딩 몫을 고려한 payload 상한(바이트) */
#define ACKQ_MAX_PAYLOAD_BYTES (512u * 1024u)
#define ACKQ_MAX_LEASE_MS (366ull * 24ull * 3600ull * 1000ull)
#define ACKQ_MAX_CLAIM_BATCH 1024u

typedef enum {
    ACKQ_OK,
    ACKQ_INVALID,        /* 인자 오류: 이름·길이·개수·시간 */
    ACKQ_NOMEM,
    ACKQ_LIMIT,          /* payload·메모리 예산 한도 */
    ACKQ_BUSY,           /* 다른 writer가 있고 기다린 시간이 끝났다. 재시도할 수 있다 */
    ACKQ_NOT_INSTALLED,  /* 큐 테이블이 없다. 열기는 만들지 않는다 */
    ACKQ_NOT_FOUND,      /* id가 없다 */
    ACKQ_STALE,          /* fence가 현재 claim이 아니거나 상태가 맞지 않는다. 아무것도 바꾸지 않았다 */
    ACKQ_CONFLICT,       /* 같은 idempotency key에 payload가 다르다 */
    ACKQ_UNSUPPORTED,    /* 트랜잭션 계층 없이 연 database에 쓰기 요청 */
    ACKQ_IO,             /* 저장소 오류. commit 전이라면 상태는 그대로다 */
    ACKQ_NOT_APPLIED,    /* commit이 확정적으로 적용되지 않았다. 상태는 그대로다 */
    ACKQ_UNCERTAIN,      /* commit 결과를 알 수 없다. 성공으로 취급하면 안 된다. 다시 열어 확인한다 */
    ACKQ_POISONED,       /* database가 불확실한 commit 뒤 더는 쓸 수 없다. 닫고 다시 연다 */
    ACKQ_TXN_FAILED,     /* 호출자의 트랜잭션이 이미 실패 상태다. rollback 뒤 다시 한다 */
    ACKQ_INTERNAL        /* 이 모듈이 기대하지 못한 database 응답 */
} ackq_status_t;

typedef enum {
    ACKQ_STATE_READY = 0,
    ACKQ_STATE_LEASED = 1,
    ACKQ_STATE_ACKED = 2,
    ACKQ_STATE_FAILED = 3  /* terminal: 명시적 fail이나 시도 횟수 초과 */
} ackq_state_t;

typedef struct ackq ackq_t;

typedef struct {
    const char *name;                /* [A-Za-z0-9_]{1..ACKQ_NAME_MAX}. 테이블은 ackq_<name> */
    /* 0이면 제한 없음. 시도(claim) 횟수가 이 값에 닿은 작업의 nack·만료는 FAILED로 끝난다 */
    uint32_t max_attempts;
    uint32_t begin_timeout_ms;       /* 자기 트랜잭션이 다른 writer를 기다리는 시간. 0이면 기다리지 않고 BUSY */
    uint64_t (*now_ms)(void *ctx);   /* NULL이면 CLOCK_REALTIME 밀리초 */
    void *clock_ctx;
    /* NULL이 아니면 문장마다 이 ledger로 메모리를 계상하고 넘으면 ACKQ_LIMIT이다 */
    const mem_ledger_t *ledger;
} ackq_options_t;

/* 큐 테이블이 없으면 만든다(자기 트랜잭션). 있으면 열기와 같다. */
ackq_status_t ackq_install(database_t *db, const ackq_options_t *options, ackq_t **out);
/* 테이블이 있어야 한다. 없으면 ACKQ_NOT_INSTALLED이고 아무것도 만들지 않는다. */
ackq_status_t ackq_open(database_t *db, const ackq_options_t *options, ackq_t **out);
void ackq_close(ackq_t **queue);

typedef struct {
    uint64_t id;
    uint64_t fence;            /* ack·nack·fail·update에 그대로 넘긴다 */
    uint32_t attempts;         /* 이 claim을 포함한 시도 횟수 */
    uint64_t lease_until_ms;
    uint8_t *payload;          /* ackq_claims_release로 해제. 빈 payload면 NULL */
    size_t payload_size;
} ackq_claim_t;

/*
 * enqueue. idem_key는 NULL이거나 1~ACKQ_IDEM_KEY_MAX바이트의 UTF-8이다. 같은 key와 같은 payload는 새 작업을 만들지
 * 않고 기존 id와 *duplicate=true를 돌려준다(작업이 이미 끝났어도 그대로). payload가 다르면 ACKQ_CONFLICT다.
 * payload는 ACKQ_MAX_PAYLOAD_BYTES 이하여야 하며 넘으면 자르지 않고 ACKQ_LIMIT이다. duplicate는 NULL 가능.
 */
ackq_status_t ackq_enqueue(ackq_t *queue, database_txn_t *txn, const uint8_t *payload, size_t size,
                           const char *idem_key, uint64_t *id, bool *duplicate);

/*
 * 최대 max_items(1..ACKQ_MAX_CLAIM_BATCH)개를 id 순서로 claim한다. 대상은 ready_at이 지난 READY와 lease가 만료된
 * LEASED(시도 횟수가 max_attempts에 닿은 것은 제외, ackq_recover가 FAILED로 끝낸다)다. 각 작업의 fence가 1 오르고
 * 시도 횟수가 늘며 lease는 now + lease_ms다. 가져올 것이 없으면 *count가 0이고 ACKQ_OK다. items 배열은 호출자가
 * max_items 크기로 준비하며 반환된 *count개를 ackq_claims_release로 해제한다. 실패하면 *count는 0이다.
 */
ackq_status_t ackq_claim(ackq_t *queue, database_txn_t *txn, size_t max_items, uint64_t lease_ms,
                         ackq_claim_t *items, size_t *count);
void ackq_claims_release(ackq_claim_t *items, size_t count);
/*
 * 특정 id 하나를 claim한다(persist-queue의 get(id=...)에 대응). claim할 수 있는 상태(때가 된 READY 또는 만료된
 * LEASED)가 아니면 ACKQ_STALE, id가 없으면 ACKQ_NOT_FOUND다. 성공하면 item을 ackq_claims_release(item, 1)로 해제한다.
 */
ackq_status_t ackq_claim_id(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t lease_ms, ackq_claim_t *item);

/*
 * claim이 지금도 유효할 때만 적용한다(state=LEASED, fence 일치). 같은 claim의 중복 ack·fail은 이미 적용됐으므로
 * ACKQ_OK다(응답 소실 뒤 재시도). 다른 fence나 상태면 ACKQ_STALE, id가 없으면 ACKQ_NOT_FOUND다.
 */
ackq_status_t ackq_ack(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence);
/* reason은 NULL 가능, ACKQ_NOTE_MAX바이트 이하의 UTF-8이며 넘으면 ACKQ_LIMIT이다. */
ackq_status_t ackq_fail(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence, const char *reason);
/*
 * 다시 claim할 수 있게 READY로 돌린다. retry_delay_ms 뒤부터 claim할 수 있다. 시도 횟수가 max_attempts에 닿았으면
 * 재시도하지 않고 FAILED로 끝내며 *terminal(NULL 가능)이 true가 된다.
 */
ackq_status_t ackq_nack(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence, uint64_t retry_delay_ms,
                        const char *reason, bool *terminal);
/*
 * payload를 바꾼다. fence가 0이면 READY 작업만(claim 전 수정), 0이 아니면 그 claim이 유효한 LEASED 작업만 바꾼다.
 * 끝난 작업(ACKED·FAILED)은 ACKQ_STALE다.
 */
ackq_status_t ackq_update(ackq_t *queue, database_txn_t *txn, uint64_t id, uint64_t fence, const uint8_t *payload,
                          size_t size);

/*
 * lease가 만료된 작업을 READY로(시도 횟수가 max_attempts에 닿았으면 FAILED로) 돌린다. 한 번에 최대 limit개를 처리하고
 * 처리한 수를 *recovered에 준다. claim도 만료된 lease를 직접 가져가므로 호출은 선택이다(통계와 FAILED 정리용).
 */
ackq_status_t ackq_recover(ackq_t *queue, database_txn_t *txn, size_t limit, size_t *recovered);
/* 모든 LEASED를 만료와 무관하게 READY로 돌린다(worker가 모두 죽은 것을 호출자가 안다는 약속). 최대 limit개. */
ackq_status_t ackq_release_leases(ackq_t *queue, database_txn_t *txn, size_t limit, size_t *released);
/* ACKED 작업을 삭제한다(include_failed면 FAILED도). 최대 limit개를 지우고 지운 수를 준다. */
ackq_status_t ackq_purge(ackq_t *queue, database_txn_t *txn, bool include_failed, size_t limit, size_t *purged);

typedef struct {
    uint64_t ready;
    uint64_t leased;
    uint64_t acked;
    uint64_t failed;
} ackq_stats_t;

/* 상태별 개수. 메타데이터만 읽고 payload를 복사하지 않는다. */
ackq_status_t ackq_stats(ackq_t *queue, database_txn_t *txn, ackq_stats_t *stats);

typedef struct {
    uint64_t id;
    ackq_state_t state;
    uint64_t fence;
    uint32_t attempts;
    uint64_t ready_at_ms;
    uint64_t lease_until_ms;
    uint64_t created_at_ms;        /* enqueue 시각 */
    char note[ACKQ_NOTE_MAX + 1];  /* 마지막 nack·fail 이유 */
    uint8_t *payload;              /* with_payload일 때만. ackq_list_release로 해제 */
    size_t payload_size;
} ackq_item_t;

/*
 * after_id보다 큰 id를 오름차순으로 최대 limit개 읽는 bounded cursor. 다음 호출은 마지막 id를 after_id로 준다.
 * 메타데이터는 한 번에 한 행씩 읽어 가장 작은 limit개만 보관하고 payload는 with_payload일 때 반환할 항목의 것만 읽는다.
 */
ackq_status_t ackq_list(ackq_t *queue, database_txn_t *txn, uint64_t after_id, size_t limit, bool with_payload,
                        ackq_item_t *items, size_t *count);
void ackq_list_release(ackq_item_t *items, size_t count);

const char *ackq_status_name(ackq_status_t status);

#endif
