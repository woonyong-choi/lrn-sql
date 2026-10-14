#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 트랜잭션 상태 코어. 저장 방식을 모르는 backend 위에서 가시성(snapshot), commit 결과(성공·실패·불확실),
 * writer 경합·timeout·취소, 자원 회수 시점을 한 곳에서 정한다. 큐·SQL·WAL에 의존하지 않는다.
 *
 * 모델: writer는 한 번에 하나, 읽기 snapshot은 여러 개다. 읽기는 writer를 막지 않는다. 선언 격리는
 * SNAPSHOT 하나다(commit 전 변경은 다른 snapshot에 보이지 않고 rollback 뒤 사라진다). 이 모듈은 가시성
 * 판정 자체를 하지 않는다. backend가 snapshot_seq 이하의 commit만 보여 주는 책임을 지고, 이 모듈은 그
 * 순번과 회수 하한(horizon)을 정확히 관리한다.
 *
 * 스레드: manager는 스레드 안전하다. txn_t 하나는 한 번에 한 스레드만 쓴다. 다른 스레드는 txn_cancel_t로만
 * 취소를 건다. 실제 DB(page_file·database)와의 연결은 이 모듈 밖이며 여기서 검증한 것은 backend 계약이다.
 */

typedef enum {
    TXN_OK,
    TXN_INVALID,
    TXN_NOMEM,
    TXN_LIMIT,             /* 동시 reader 한도 */
    TXN_BUSY,              /* 다른 writer가 있고 기다리지 않음, 또는 조회 대상이 진행 중 */
    TXN_TIMEOUT,
    TXN_CANCELLED,
    TXN_UNSUPPORTED,       /* 격리 수준·보증 단계·조회 범위를 지원하지 않음 */
    TXN_POISONED,          /* manager가 불확실한 commit 또는 backend 오류 뒤 새 트랜잭션을 거절 */
    TXN_NOT_ACTIVE,
    TXN_NOT_FOUND,         /* 조회 링에서 밀려났거나 기록이 없음 */
    TXN_REENTRANT,         /* backend callback 안에서 manager의 쓰기 API를 호출함 */
    TXN_IO,                /* backend가 시작 단계에서 확정 실패를 보고 */
    TXN_COMMIT_FAILED,     /* 확정된 미적용. 저장 상태는 begin 전과 같다 */
    TXN_COMMIT_UNCERTAIN   /* 적용 여부를 알 수 없다. manager는 poison이 된다 */
} txn_status_t;

typedef enum { TXN_ACCESS_READ_WRITE, TXN_ACCESS_READ_ONLY } txn_access_t;

/* SNAPSHOT 외에는 TXN_UNSUPPORTED다. */
typedef enum {
    TXN_ISOLATION_SNAPSHOT,
    TXN_ISOLATION_READ_COMMITTED,
    TXN_ISOLATION_SERIALIZABLE
} txn_isolation_t;

/*
 * commit이 약속하는 보증. 0은 가장 약한 단계라 기본값이 된다.
 * VISIBLE: 게시만 하고 재시작 뒤 보존을 주장하지 않는다.
 * SYNCED:  데이터와 제어 정보를 장치에 동기화했지만 로그가 없어 부분 쓰기·lost write를 복구하지 않는다.
 * DURABLE: WAL 같은 복구 가능한 로그가 동기화된 뒤다. WAL 없는 backend는 선언하면 안 된다.
 * backend가 선언한 max_guarantee보다 높은 요청은 begin에서 TXN_UNSUPPORTED이며 낮춰 처리하지 않는다.
 */
typedef enum { TXN_GUARANTEE_VISIBLE, TXN_GUARANTEE_SYNCED, TXN_GUARANTEE_DURABLE } txn_guarantee_t;

typedef enum {
    TXN_STATE_NONE,        /* NULL 핸들 */
    TXN_STATE_ACTIVE,
    TXN_STATE_COMMITTED,
    TXN_STATE_ROLLED_BACK, /* rollback, 취소, 암묵 release */
    TXN_STATE_FAILED,      /* commit이 확정 미적용 */
    TXN_STATE_UNCERTAIN    /* commit 결과 불명 */
} txn_state_t;

typedef enum { TXN_OUTCOME_NOT_APPLIED, TXN_OUTCOME_UNKNOWN, TXN_OUTCOME_APPLIED } txn_outcome_t;

typedef struct txn txn_t;
typedef struct txn_manager txn_manager_t;

/* 다른 스레드가 begin 대기와 commit 시작 전을 취소하는 플래그. 사용하는 동안 살아 있어야 한다. */
typedef struct {
    atomic_bool requested;
} txn_cancel_t;

void txn_cancel_init(txn_cancel_t *cancel);
void txn_cancel_request(txn_cancel_t *cancel);
bool txn_cancel_requested(const txn_cancel_t *cancel);

/* publish가 돌려주는 결과. achieved는 APPLIED일 때만 의미가 있다. */
typedef struct {
    txn_outcome_t outcome;
    txn_guarantee_t achieved;
} txn_publish_result_t;

/*
 * backend callback. manager 락을 잡지 않은 채 호출되므로 callback 안에서 manager의 읽기 accessor
 * (txn_manager_last_committed, txn_manager_oldest_snapshot, txn_manager_is_poisoned)는 불러도 되지만
 * begin·commit·rollback·release·close·query는 불러서는 안 되며 TXN_REENTRANT로 거절된다.
 *
 * write_begin: writer 시작. 성공하면 *write에 쓰기 핸들(NULL 가능)을 채운다. 실패하면 backend는 아무것도
 *   남기지 않아야 하며 write_finish는 호출되지 않는다. TXN_OK 외 값은 그대로 begin 결과가 된다(COMMIT_*는 TXN_IO).
 * publish: commit 지점. 같은 writer에서 최대 한 번, 취소가 먼저 확정되면 호출되지 않는다. commit_seq 순번으로
 *   requested 보증 이상을 달성하면 APPLIED, 아무것도 바꾸지 못했음이 확실하면 NOT_APPLIED, 모르면 UNKNOWN을
 *   돌려준다. 이미 적용했는데 보증 미달이거나 알 수 없는 값은 manager가 UNKNOWN으로 취급하고 poison한다.
 * write_finish: write_begin이 성공한 모든 writer마다 정확히 한 번, 실패할 수 없다.
 *   APPLIED: 새 상태를 확정한다. 이번 commit이 대체한 옛 자원은 즉시 해제하지 말고 commit_seq와 함께
 *     보관했다가 reclaim의 horizon이 commit_seq 이상이 된 뒤에 해제한다(열린 snapshot이 쓸 수 있다).
 *   NOT_APPLIED: begin 때 만든 새 자원을 회수한다.
 *   UNKNOWN: 메모리만 정리하고 저장 장치의 자원은 건드리지 않는다(어느 쪽이 유효한지 모르므로).
 * reclaim(선택): horizon(가장 오래된 열린 snapshot 순번, 없으면 마지막 commit 순번)이 오르면 호출되며
 *   서로 직렬화된다. commit_seq <= horizon로 보관한 옛 자원만 해제한다.
 * is_poisoned(선택): backend가 스스로 poison이면 true. 모든 commit 뒤에 확인하고 true면 manager도 poison이다.
 * close(선택): manager close에서 한 번 호출된다. manager 생성에 실패하면 호출되지 않고 ctx는 호출자 소유다.
 */
typedef struct {
    txn_status_t (*write_begin)(void *ctx, uint64_t base_seq, void **write);
    txn_publish_result_t (*publish)(void *ctx, void *write, uint64_t commit_seq,
                                    txn_guarantee_t requested);
    void (*write_finish)(void *ctx, void *write, txn_outcome_t outcome, uint64_t commit_seq);
    void (*reclaim)(void *ctx, uint64_t horizon);
    bool (*is_poisoned)(void *ctx);
    void (*close)(void *ctx);
} txn_backend_ops_t;

/*
 * 세대 선택 방식. DEFAULT는 txn_manager_spec_t.epoch를 따른다(0이면 자동, 아니면 지정).
 * ABOVE_FLOOR는 epoch_floor보다 크고 이 프로세스가 이미 발급한 모든 세대보다 큰 값을 한 번에 자동으로 고른다.
 */
typedef enum { TXN_EPOCH_DEFAULT, TXN_EPOCH_ABOVE_FLOOR } txn_epoch_mode_t;

/* 0인 필드는 기본값이다. */
#define TXN_DEFAULT_MAX_READERS 256u
#define TXN_MAX_MAX_READERS 65536u
#define TXN_DEFAULT_COMMIT_RING 64u
#define TXN_MAX_COMMIT_RING 65536u
#define TXN_WAIT_FOREVER UINT32_MAX

typedef struct {
    const txn_backend_ops_t *ops;
    void *ctx;
    txn_guarantee_t max_guarantee;   /* backend가 실제로 달성할 수 있는 최대 보증 */
    uint64_t initial_seq;            /* backend가 가진 마지막 commit 순번 */
    /*
     * manager 세대. txn_id 상위 32비트에 들어가 재open 전의 id와 새 id를 구분한다.
     * 프로세스마다 고정 공간의 상한(high-water) 하나로 단조 발급한다. 0이면 자동 발급이며 상한 + 1(처음은 1)이다.
     * 0이 아니면 상한보다 클 때만 채택하고 상한을 그 값으로 올린다. 상한 이하의 지정은 TXN_INVALID이므로 이 프로세스에서
     * 쓴 번호와 그보다 작은 번호는 다시 쓸 수 없다. 자동 발급이 UINT32_MAX를 넘으면 TXN_LIMIT이다. open이 실패하면
     * 번호를 소모하지 않는다.
     * 호출자 조율: 이 상한은 프로세스를 넘는 유일성을 주지 않는다(재시작하면 자동 발급이 1부터 다시 시작해 이전
     * 프로세스의 id와 겹칠 수 있다). 재시작 경계에서는 호출자가 backend에 저장한 증가하는 세대 번호를 넘겨야 하며
     * 이 모듈은 그 값을 저장하지도 검증하지도 않는다. 같은 프로세스에서 자동 발급과 지정을 섞으면 큰 번호를 지정한 뒤
     * 작은 번호는 거절되므로 번호 범위를 호출자가 조율한다. 같은 세대 번호를 재시작 뒤에 다시 넘기는 것은 이전 id와
     * 구분되지 않으므로 안전하다고 가정하면 안 된다. */
    uint32_t epoch;
    /*
     * 재시작을 넘는 세대 할당. epoch_mode가 ABOVE_FLOOR이면 epoch는 0이어야 하고(아니면 TXN_INVALID) manager는
     * max(이 프로세스의 상한, epoch_floor) + 1을 세대로 받으며 상한이 그 값으로 오른다. 선택과 상한 갱신은 한 번의
     * 잠금 안에서 일어나므로 동시 open끼리, 그리고 서로 다른 floor를 가진 저장소끼리 겹치지 않는다. floor는 호출자가
     * backend에 저장해 둔 가장 큰 세대이고 처음 만든 저장소는 0이다. max가 UINT32_MAX이면 TXN_LIMIT이며 상한은
     * 그대로다. 지정 epoch의 전역 중복 거절은 DEFAULT 방식에서 그대로 유지된다.
     * 영속화 책임: 이 모듈은 값을 저장하지 않는다. 호출자는 open 직후 txn_manager_epoch로 받은 값을 backend에
     * 저장해 성공을 확인한 뒤에만 begin을 불러야 한다. 저장이 실패하면 manager를 close하고 그 저장소를 쓰지 않아야
     * 하며, 이미 발급한 세대는 프로세스 상한에 남아 다시 쓰이지 않는다(저장소의 floor가 그대로여도 다음 open은 더
     * 큰 값을 받는다). 재시작 뒤에는 상한이 사라지므로 저장된 floor가 유일성을 맡는다. */
    txn_epoch_mode_t epoch_mode;
    uint32_t epoch_floor;
    size_t max_readers;
    size_t commit_ring;              /* 응답 손실 조회용으로 기억하는 commit 수 */
    /* 단조 시계(밀리초). NULL이면 CLOCK_MONOTONIC이다. manager를 호출하면 안 된다. */
    uint64_t (*now_ms)(void *clock_ctx);
    void *clock_ctx;
} txn_manager_spec_t;

/* 실패하면 *out은 NULL이고 spec의 ctx는 호출자 소유로 남으며 close는 호출되지 않는다. */
txn_status_t txn_manager_open(const txn_manager_spec_t *spec, txn_manager_t **out);

/*
 * 해제하지 않은 txn_t가 있으면 TXN_BUSY이고 manager는 그대로다. 성공하면 backend close를 한 번 부르고
 * *manager를 NULL로 만든다. *manager가 NULL이면 무동작이다. begin 등 다른 호출과 동시에 부르면 안 된다.
 */
txn_status_t txn_manager_close(txn_manager_t **manager);

typedef struct {
    txn_access_t access;
    txn_isolation_t isolation;
    txn_guarantee_t guarantee;       /* READ_WRITE의 commit이 요구하는 보증 */
    /* writer 대기 시간. 0이면 기다리지 않고 BUSY, TXN_WAIT_FOREVER면 무기한. READ_ONLY는 기다리지 않는다. */
    uint32_t timeout_ms;
    const txn_cancel_t *cancel;      /* NULL 가능 */
} txn_begin_options_t;

/* options가 NULL이면 모두 0 기본값이다. 실패하면 *out은 NULL이다. */
txn_status_t txn_begin(txn_manager_t *manager, const txn_begin_options_t *options, txn_t **out);

typedef struct {
    uint64_t txn_id;
    txn_outcome_t outcome;
    uint64_t commit_seq;             /* APPLIED면 이 commit의 순번, 아니면 마지막 commit 순번 */
    txn_guarantee_t guarantee;       /* APPLIED일 때 달성한 보증 */
} txn_commit_info_t;

/*
 * ACTIVE가 아니면 NOT_ACTIVE. info는 NULL 가능이며 TXN_OK, COMMIT_FAILED, COMMIT_UNCERTAIN, 취소 때 채운다.
 * 취소가 publish 전에 확인되면 TXN_CANCELLED이고 변경은 사라진다. publish 시작 뒤의 취소는 무시한다.
 * READ_ONLY는 backend를 부르지 않고 COMMITTED가 된다.
 */
txn_status_t txn_commit(txn_t *txn, txn_commit_info_t *info);
txn_status_t txn_rollback(txn_t *txn);

/* ACTIVE면 rollback한 뒤 핸들을 해제하고 *txn을 NULL로 만든다. *txn이 NULL이면 무동작이다. */
txn_status_t txn_release(txn_t **txn);

/* open이 발급한 세대. txn_id의 상위 32비트와 같다. NULL이면 0이다. */
uint32_t txn_manager_epoch(const txn_manager_t *manager);

txn_state_t txn_state(const txn_t *txn);
uint64_t txn_id(const txn_t *txn);
uint64_t txn_snapshot_seq(const txn_t *txn);

/*
 * txn_id는 (세대 << 32 | 일련번호)인 불투명 값이며 한 세대는 2^32-1개를 발급하고 끝나면 begin이 LIMIT다.
 * 응답을 잃은 호출자가 txn_id의 commit 결과를 조회한다. 최근 commit_ring개의 writer 트랜잭션(rollback 포함)을
 * 기억한다. 진행 중이면 BUSY, 링에서 밀려났거나 읽기 전용이면 NOT_FOUND, 이 세대가 아직 발급하지 않은 번호는
 * NOT_FOUND, 다른 세대의 id(같은 프로세스에서 이전에 발급)는 UNSUPPORTED다. 재시작 전 id를 구분하려면 호출자가 영속 세대 번호를
 * 줘야 하고, 재시작 뒤 commit 결과 조회는 WAL이 필요하므로 capability에 없다.
 */
txn_status_t txn_query_commit(txn_manager_t *manager, uint64_t txn_id, txn_commit_info_t *info);

uint64_t txn_manager_last_committed(txn_manager_t *manager);
/* 열린 snapshot(writer 포함) 중 가장 작은 순번. 없으면 마지막 commit 순번이다. */
uint64_t txn_manager_oldest_snapshot(txn_manager_t *manager);
bool txn_manager_is_poisoned(txn_manager_t *manager);

#define TXN_CAP_DURABLE 1u  /* max_guarantee가 DURABLE */
unsigned txn_manager_capabilities(const txn_manager_t *manager);

/* writer 대기가 플랫폼 단조 대기(macOS 상대 대기, Linux CLOCK_MONOTONIC 조건 변수)를 쓰는지. false면 벽시계 후퇴 경로다. */
bool txn_wait_is_monotonic(void);

#endif
