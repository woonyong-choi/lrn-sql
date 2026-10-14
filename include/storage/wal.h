#ifndef WAL_H
#define WAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * 자체 WAL 파일과 재시작 복구. pager·page_file과 독립이며 어느 쪽의 형식도 바꾸지 않는다.
 *
 * 파일 배치(정수는 little-endian):
 *   0..31   파일 헤더: magic "LRWALOG1" 8, version u32, 예약 u32, file_id u64, 예약 u32, CRC32C u32
 *   32..    레코드. 모두 8바이트 정렬이며 레코드 하나는 56바이트 헤더와 페이로드다.
 *     0 magic u32, 4 kind u16, 6 예약 u16, 8 payload_len u32, 12 page_id u32, 16 xid u64,
 *     24 lsn u64, 32 prev_lsn u64, 40 file_id u64, 48 payload CRC32C u32, 52 header CRC32C u32
 * LSN은 레코드 시작의 파일 오프셋이다(0은 없음). prev_lsn은 바로 앞 레코드의 LSN이다.
 *
 * 약속하는 것
 * - wal_commit()은 COMMIT 레코드와 그 앞의 모든 레코드를 fsync한 뒤에만 WAL_OK를 돌려준다.
 * - 쓰기·fsync가 실패하면 그 commit의 운명을 알 수 없으므로 WAL_COMMIT_UNKNOWN을 돌려주고
 *   핸들을 쓸 수 없게 만든다. 재열기와 복구가 로그에 실제 남은 것으로 결정한다.
 * - 데이터 페이지는 wal_require_flushed(그 페이지의 마지막 LSN)가 WAL_OK일 때만 써야 한다(WAL 먼저).
 * - 로그 끝의 "불완전한 길이"와 "완전한 레코드의 불일치"를 구분한다. 이 구현은 파일을 미리 늘리지
 *   않으므로 잘린 꼬리로 인정하는 것은 물리적으로 파일이 짧은 두 경우뿐이다.
 *     1) 레코드 헤더(56바이트)가 다 들어오기 전에 파일이 끝남
 *     2) 헤더 magic·CRC·LSN·file_id가 모두 맞는데 그 헤더가 선언한 길이만큼 파일이 모자람
 *   쓰기 열기는 잘린 꼬리를 ftruncate+fsync로 잘라 낸다. 그 밖의 모든 불일치는 WAL_CORRUPT로 거절하고
 *   파일을 바꾸지 않는다: 헤더가 다 있는데 magic·CRC가 틀림(CRC 필드만 0이 된 경우, 레코드 전체가 0으로
 *   덮인 경우 포함), 길이가 찼는데 payload CRC·패딩이 틀림. 이런 레코드는 이미 승인된 마지막 commit일
 *   수 있고 변질과 미완성을 구분할 방법이 없기 때문이다.
 * - 열기는 순서(LSN=오프셋, prev_lsn 연쇄, 끝난 거래에 DATA·COMMIT·ABORT 금지)도 검증한다.
 *
 * replay 계약 (wal_replay_mode_t)
 * - WAL_REPLAY_ALL: MVCC가 있는 소비자용. 모든 DATA를 LSN 순서로 넘기고 가시성은 xid 상태로 정한다.
 *   page LSN 비교(wal_redo_needed)로 중복 적용을 막는다. 미확정 이미지를 같은 자리의 가시 페이지에
 *   덮어쓰는 소비자(MVCC 없음)에는 쓰면 안 된다.
 * - WAL_REPLAY_COMMITTED_ONLY: MVCC가 없는 소비자용. COMMITTED 거래의 DATA만, 거래 단위로 묶어
 *   COMMIT LSN 순서(applied_lsn보다 큰 것만)로 넘기고 거래마다 txn_done을 부른다. ABORTED·CRASHED
 *   거래의 DATA는 넘기지 않는다. 소비자는 (a) 거래의 변경을 COMMIT 전에 가시 페이지에 쓰지 않거나
 *   COW로 새 자리에만 쓰고, (b) txn_done에서 그 거래의 변경과 "applied_lsn = commit_lsn"을 한 번에
 *   원자적으로 게시해야 한다. 그러면 중복 적용은 applied_lsn 하나로 막히고 복구 중 중단도 안전하다.
 *
 * 메모리 예산
 * - WAL이 쥐는 힙(거래 상태 표, 쓰기 버퍼, 읽기 창, COMMITTED_ONLY replay의 DATA LSN 배열과 후보·정렬
 *   배열)은 wal_limits_t.max_memory_bytes(기본 WAL_DEFAULT_MEMORY_BYTES)를 넘지 않는다. 표·버퍼·배열을
 *   키울 때 새 크기를 예산에 올린 뒤에야 옛 크기를 놓으므로 재해시·재할당 동안은 old+new를 함께 센다.
 *   예산을 넘기는 요청은 상태를 바꾸지 않고 WAL_LIMIT이다. 열기가 WAL_LIMIT이면 핸들은 없고 파일은
 *   바뀌지 않으며 info.memory_peak가 필요했던 양을 알려 준다. 소비자(DB)의 메모리는 세지 않는다.
 * - 열기 비용은 거래 수와 로그 크기에 비례하고, replay는 반영할 DATA 레코드 하나에 8바이트를 더 쓴다.
 *   실제 DB 연결 전 이 예산은 세션·결과 메모리 장부(#79)와 합산 규칙을 맞춰야 한다.
 *
 * 약속하지 않는 것
 * - 전원 장애 보장. fsync가 거짓말을 하는 저장 장치와 이미 fsync된 뒤 중간 로그의 변질은 막지 못한다
 *   (변질은 CORRUPT로 알릴 뿐 복구하지 않는다). 검사는 프로세스 종료(SIGKILL·_exit)와
 *   fsync되지 않은 꼬리를 버리는 모형까지만 다룬다.
 * - 쓰기 순서가 바뀌어 중간에 0 구멍이 생긴 로그는 CORRUPT로 거절한다(안전 쪽 오탐).
 * - 로그 파일은 줄어들지 않는다(세그먼트 회수 없음). 열기 비용은 파일 크기에 비례한다.
 * - 한 핸들은 한 스레드만 쓴다. 같은 파일은 flock으로 한 쓰기 핸들만 연다.
 */

#define WAL_HEADER_BYTES 32u
#define WAL_RECORD_HEADER_BYTES 56u
#define WAL_MAX_PAYLOAD (1u << 20)
#define WAL_FIRST_LSN WAL_HEADER_BYTES
#define WAL_INVALID_XID 0u

typedef uint64_t wal_lsn_t;
typedef uint64_t wal_xid_t;

typedef enum {
    WAL_REC_DATA = 1,       /* redo 대상. 페이로드는 호출자가 해석한다 */
    WAL_REC_COMMIT = 2,
    WAL_REC_ABORT = 3,
    WAL_REC_CHECKPOINT = 4  /* 페이로드 8바이트: redo 시작 LSN. xid는 WAL_INVALID_XID */
} wal_kind_t;

typedef enum {
    WAL_OK,
    WAL_INVALID,            /* 인자·크기 한계·순서 위반 호출 */
    WAL_IO,                 /* 열기·읽기·쓰기·fsync 실패. errno를 남긴다 */
    WAL_CORRUPT,            /* 중간 손상, 순서 위반, 잘못된 헤더 */
    WAL_UNSUPPORTED,        /* 알 수 없는 version */
    WAL_NOMEM,
    WAL_LOCKED,             /* 다른 핸들이 잠금을 쥐고 있음 */
    WAL_EXISTS,             /* wal_create 목적지가 이미 있음 */
    WAL_COMMIT_UNKNOWN,     /* 쓰기·fsync 실패. commit이 로그에 남았는지 모른다. 핸들 사용 불가 */
    WAL_POISONED,           /* 이전 I/O 실패로 닫힌 핸들 */
    WAL_NOT_FLUSHED,        /* wal_require_flushed: 요청한 LSN이 아직 디스크에 없음 */
    WAL_REDO_FAILED,        /* redo 콜백이 실패를 돌려줌 */
    WAL_LIMIT               /* WAL 메모리 예산 또는 크기 한계를 넘음. 상태는 바뀌지 않는다 */
} wal_status_t;

typedef enum {
    WAL_XID_IN_PROGRESS = 0,  /* 로그에 끝 레코드 없음. 재시작 뒤에는 CRASHED로 본다 */
    WAL_XID_COMMITTED = 1,
    WAL_XID_ABORTED = 2,
    WAL_XID_CRASHED = 3       /* 끝 레코드 없이 로그가 끝남. 가시 상태로 노출하지 않는다 */
} wal_xid_state_t;

/* 테스트가 실패를 주입하는 자리. 기본값은 libc다. */
typedef struct {
    ssize_t (*pread)(int fd, void *buf, size_t size, off_t offset);
    ssize_t (*pwrite)(int fd, const void *buf, size_t size, off_t offset);
    int (*fsync)(int fd);
    int (*ftruncate)(int fd, off_t length);
} wal_io_t;

/* 이후 열리는 핸들이 쓰는 I/O 진입점을 바꾼다. NULL이면 libc로 복원한다. 테스트 전용. */
void wal_set_default_io(const wal_io_t *io);

typedef struct wal wal_t;

typedef struct {
    wal_kind_t kind;
    wal_xid_t xid;
    uint32_t page_id;
    wal_lsn_t lsn;
    wal_lsn_t prev_lsn;
    wal_lsn_t commit_lsn;     /* COMMITTED_ONLY에서 이 레코드 거래의 COMMIT LSN. ALL에서는 0 */
    uint32_t payload_len;
    const uint8_t *payload;   /* 콜백 동안만 유효 */
} wal_record_t;

typedef struct {
    uint64_t records;         /* 유효 레코드 수 */
    wal_lsn_t valid_end;      /* 유효 로그의 끝 오프셋(다음 레코드가 놓일 자리) */
    uint64_t torn_bytes;      /* 잘린 꼬리 바이트 수. 쓰기 열기에서는 이미 잘라 낸 양 */
    bool truncated;           /* 쓰기 열기가 꼬리를 잘랐다 */
    wal_lsn_t corrupt_lsn;    /* WAL_CORRUPT일 때 첫 불량 위치. 아니면 0 */
    wal_lsn_t redo_lsn;       /* 마지막 유효 CHECKPOINT의 redo 시작. 없으면 WAL_FIRST_LSN */
    wal_lsn_t last_checkpoint;
    uint64_t xids;            /* 로그에 나타난 거래 수 */
    uint64_t committed;
    uint64_t aborted;
    uint64_t crashed;
    uint64_t memory_peak;     /* 열기 동안 WAL이 쥔 힙의 최대(old+new 포함). 실패해도 채운다 */
} wal_open_info_t;

/*
 * 새 로그를 만든다. 목적지가 이미 있으면 WAL_EXISTS이며 바꾸지 않는다. 헤더를 쓰고 파일과 디렉터리를
 * fsync한 뒤 쓰기 핸들을 돌려준다. 헤더가 durable해지기 전에 멈춘 파일(32바이트 이하이고 헤더 영역이
 * 전부 0이거나, 32바이트 미만이고 magic으로 시작함)은 쓰기 wal_open이 다시 초기화한다. 그 시점에는 어떤 레코드도 승인되지 않았다.
 */
wal_status_t wal_create(const char *path, wal_t **out);

#define WAL_DEFAULT_MEMORY_BYTES (64ull << 20)
typedef struct {
    uint64_t max_memory_bytes; /* 0이면 WAL_DEFAULT_MEMORY_BYTES */
} wal_limits_t;
wal_status_t wal_create_with(const char *path, const wal_limits_t *limits, wal_t **out);

/*
 * 기존 로그를 열고 처음부터 끝까지 검증한다. writable이면 배타, 아니면 공유 잠금을 non-blocking으로
 * 잡는다. 쓰기 열기는 잘린 꼬리를 자르고 fsync한 뒤 돌려준다. 읽기 열기는 파일을 바꾸지 않는다.
 * 실패하면 *out은 NULL이고 info(NULL 허용)에 판정 근거가 남는다.
 */
wal_status_t wal_open(const char *path, bool writable, wal_t **out, wal_open_info_t *info);
wal_status_t wal_open_with(const char *path, bool writable, const wal_limits_t *limits, wal_t **out,
                           wal_open_info_t *info);
wal_status_t wal_close(wal_t *wal);

/*
 * DATA 레코드를 메모리 버퍼에 쌓고 LSN을 돌려준다. 디스크에는 닿지 않는다. xid는 0이 아니어야 하고
 * 이미 끝났거나(COMMITTED·ABORTED·CRASHED) 그 거래에 DATA를 더 쌓을 수 없다. 실패하면 상태를 바꾸지 않는다.
 */
wal_status_t wal_append(wal_t *wal, wal_xid_t xid, uint32_t page_id, const void *payload,
                        uint32_t payload_len, wal_lsn_t *lsn);

/* lsn까지(그 레코드 포함) 쓰고 fsync한다. 실패하면 핸들을 쓸 수 없게 만들고 WAL_IO다. */
wal_status_t wal_flush(wal_t *wal, wal_lsn_t lsn);

/* 데이터 페이지를 쓰기 전에 부른다. 그 페이지의 마지막 LSN이 이미 fsync됐을 때만 WAL_OK. */
wal_status_t wal_require_flushed(const wal_t *wal, wal_lsn_t page_lsn);

/*
 * COMMIT 레코드를 쌓고 fsync한 뒤에만 WAL_OK다. xid의 DATA를 이미 append했어야 한다.
 * 쓰기·fsync 실패는 WAL_COMMIT_UNKNOWN(핸들 사용 불가, 호출자는 응답을 보내면 안 된다).
 * 레코드를 쌓지 못한 실패(INVALID·NOMEM·POISONED)는 로그에 아무것도 남기지 않았다.
 */
wal_status_t wal_commit(wal_t *wal, wal_xid_t xid, wal_lsn_t *commit_lsn);
/* ABORT 레코드를 쌓는다. fsync는 하지 않는다(승인 응답이 없으므로 잃어도 CRASHED와 같은 결과). */
wal_status_t wal_abort(wal_t *wal, wal_xid_t xid, wal_lsn_t *abort_lsn);

/* redo 시작 위치를 정하는 CHECKPOINT를 쓰고 fsync한다. redo_lsn 이전 변경이 데이터 파일에 fsync된 뒤에 부른다. */
wal_status_t wal_checkpoint(wal_t *wal, wal_lsn_t redo_lsn, wal_lsn_t *checkpoint_lsn);

wal_lsn_t wal_appended_lsn(const wal_t *wal);   /* 마지막으로 쌓은 레코드의 LSN. 없으면 0 */
wal_lsn_t wal_flushed_lsn(const wal_t *wal);    /* fsync된 마지막 레코드의 LSN. 없으면 0 */
wal_lsn_t wal_end_lsn(const wal_t *wal);        /* 다음 레코드가 놓일 오프셋 */
/* 로그에서 본 가장 큰 xid. 새 xid는 이보다 커야 한다. 거래가 없으면 0 */
wal_xid_t wal_max_xid(const wal_t *wal);
/*
 * 이 로그 파일의 식별자(헤더의 file_id, 만들 때 무작위로 정해지며 0이 아니다). 읽기 전용이다. LSN은 이 파일 안의
 * 오프셋이라 다른 로그에는 의미가 없으므로, 데이터 파일이 짝인 로그를 기억하는 데 쓴다. wal을 NULL로 주면 0이다.
 */
uint64_t wal_file_id(const wal_t *wal);
bool wal_is_poisoned(const wal_t *wal);
uint64_t wal_memory_used(const wal_t *wal);  /* 지금 WAL이 쥔 힙 바이트 */
uint64_t wal_memory_peak(const wal_t *wal);

/*
 * 열기 때 스캔한 로그와 이 핸들이 쌓은 레코드에서 본 거래 상태. 열기 때 끝 레코드가 없던 거래는
 * WAL_XID_CRASHED다. 모르는 xid는 WAL_XID_IN_PROGRESS와 false. COMMITTED는 COMMIT이 fsync된 뒤에만 된다.
 */
bool wal_xid_known(const wal_t *wal, wal_xid_t xid);
wal_xid_state_t wal_xid_state(const wal_t *wal, wal_xid_t xid);

typedef enum {
    WAL_REDO_APPLIED = 0,
    WAL_REDO_SKIPPED = 1,   /* page LSN이 이미 이 레코드를 반영함 */
    WAL_REDO_ERROR = 2
} wal_redo_result_t;

/* page LSN이 레코드보다 오래됐을 때만 적용한다. */
static inline bool wal_redo_needed(wal_lsn_t record_lsn, wal_lsn_t page_lsn)
{
    return page_lsn < record_lsn;
}

typedef enum {
    WAL_REPLAY_ALL = 0,
    WAL_REPLAY_COMMITTED_ONLY = 1
} wal_replay_mode_t;

typedef struct {
    wal_replay_mode_t mode;
    /* COMMITTED_ONLY: 이 LSN 이하 COMMIT의 거래는 이미 반영됐으므로 넘기지 않는다 */
    wal_lsn_t applied_lsn;
    /* 각 DATA 레코드마다. ALL은 page LSN과 비교해 적용 여부를 정하고 결과를 돌려준다. */
    wal_redo_result_t (*redo)(void *ctx, const wal_record_t *record);
    /* COMMITTED_ONLY: 거래의 마지막 DATA 뒤. 여기서 원자적으로 게시한다. 실패하면 false. NULL 허용 */
    bool (*txn_done)(void *ctx, wal_xid_t xid, wal_lsn_t commit_lsn);
    /* redo가 끝난 뒤 로그에 나타난 거래마다 한 번. CRASHED는 끝 레코드가 없는 거래다. NULL 허용 */
    void (*xid_state)(void *ctx, wal_xid_t xid, wal_xid_state_t state);
} wal_replay_ops_t;

typedef struct {
    uint64_t applied;
    uint64_t skipped;
    uint64_t scanned;
    uint64_t txns;            /* COMMITTED_ONLY: txn_done을 부른 거래 수 */
} wal_replay_stats_t;

/*
 * ALL: info.redo_lsn부터 쓴 끝까지 읽으며 DATA를 redo에 넘긴다.
 * COMMITTED_ONLY: 위 replay 계약대로 거래 단위로 넘긴다. checkpoint의 redo_lsn은 쓰지 않는다.
 * 레코드는 다시 CRC를 검증한다. redo가 ERROR를, txn_done이 false를 돌려주면 즉시 WAL_REDO_FAILED이며
 * 이후는 건드리지 않는다. 버퍼에 아직 쓰지 않은 레코드가 있으면 COMMITTED_ONLY는 WAL_INVALID다.
 */
wal_status_t wal_replay(wal_t *wal, const wal_replay_ops_t *ops, void *ctx,
                        wal_replay_stats_t *stats);

const char *wal_status_name(wal_status_t status);

#endif
