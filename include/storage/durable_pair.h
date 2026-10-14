#ifndef DURABLE_PAIR_H
#define DURABLE_PAIR_H

#include <stdbool.h>
#include <stdint.h>

#include "storage/page_file.h"
#include "storage/wal.h"

/*
 * 데이터 파일(page_file)과 WAL의 짝을 여는 순서와 첫 durable 전환. 두 파일의 관계를 정하는 곳이며 replay와
 * 거래는 호출자가 한다. database·session·heap을 알지 못한다.
 *
 * 짝의 근거: v2 제어 페이지가 WAL 헤더의 file_id(wal_file_id)를 담는다. 이 값은 카탈로그 generation과 무관한
 * 별도 필드이고 한 번 정하면 바꿀 수 없다.
 *
 * 첫 durable 전환 순서(승인된 거래는 마지막 단계 뒤에만 생긴다):
 *   A. WAL 파일을 만든다(wal_create가 파일과 디렉터리를 fsync한 뒤 돌려준다)
 *   B. enable_durable과 commit 1: WAL의 file_id를 담은 v2 제어 페이지(ready 아님)를 한 슬롯에 쓴다
 *   C. commit 2: 두 번째 v2 제어 페이지(ready)를 다른 슬롯에 쓴다
 * 어느 단계에서 멈춰도 WAL에 기록이 없고 승인한 거래가 없으므로 같은 호출을 다시 하면 이어서 끝낸다.
 *
 * 열 때의 판정(제어 페이지 c, WAL 상태). 오류는 복구하지 않고 파일을 바꾸지 않는다.
 *   c가 v1, WAL 없음            PLAIN(로그 없이 연다). convert면 A, B, C를 한다
 *   c가 v1, WAL에 기록 0        중단된 전환. convert면 B, C를 한다. 아니면 NEEDS_CONVERSION
 *   c가 v1, WAL에 기록 있음     오류 DOWNGRADED. v2 제어 페이지가 손상돼 v1 슬롯으로 fallback했을 수 있다.
 *                               승인한 거래가 있을 수 있으므로 로그 없는 파일로 조용히 여는 길은 없다
 *   c가 v2, WAL 없음            오류 WAL_MISSING(새로 만들지 않는다)
 *   c가 v2, WAL id 다름         오류 MISMATCH
 *   c가 v2 ready 아님, 기록 0   중단된 전환. convert면 C를 한다. 아니면 NEEDS_CONVERSION
 *   c가 v2 ready 아님, 기록 있음 오류 DOWNGRADED. 기록은 ready 뒤에만 생기므로 더 새로운 슬롯이 사라졌다
 *   c가 v2 ready                정상. 한 슬롯이 손상돼도 살아남은 슬롯이 ready이면 정상이다
 * 판정할 수 없는 한계: WAL 파일이 지워지고 v2 슬롯도 손상돼 v1 fallback이 된 경우는 로그 없는 v1 파일과 같다.
 * 이때는 file_info.damaged_slots가 0이 아니므로 호출자가 구별해 거절할 수 있다.
 */

typedef enum {
    DURABLE_PAIR_OK,
    DURABLE_PAIR_INVALID,
    DURABLE_PAIR_IO,                 /* errno를 남긴다 */
    DURABLE_PAIR_NOMEM,
    DURABLE_PAIR_CORRUPT,            /* 데이터 파일 또는 WAL 손상 */
    DURABLE_PAIR_UNSUPPORTED,
    DURABLE_PAIR_LOCKED,
    DURABLE_PAIR_LIMIT,              /* WAL 메모리 예산 */
    DURABLE_PAIR_NEEDS_CONVERSION,   /* 복구할 수 있는 미완료 전환. convert가 아니라 하지 않았다 */
    DURABLE_PAIR_WAL_MISSING,        /* 오류: durable 파일의 WAL이 없다 */
    DURABLE_PAIR_MISMATCH,           /* 오류: 다른 WAL이다 */
    DURABLE_PAIR_DOWNGRADED          /* 오류: 승인된 거래가 있을 수 있는데 durable로 보이지 않는다 */
} durable_pair_status_t;

typedef struct {
    bool convert;                    /* 첫 전환과 중단된 전환의 마무리를 허용한다 */
    uint32_t durable_epoch;          /* 전환 때 제어 페이지에 기록한다 */
    const wal_limits_t *wal_limits;  /* NULL이면 WAL 기본 예산 */
    page_file_admit_fn admit;        /* NULL 가능. 데이터 파일 열기의 admission */
    void *admit_ctx;
} durable_pair_options_t;

typedef struct {
    page_file_t *file;
    wal_t *wal;                      /* PLAIN이면 NULL */
} durable_pair_t;

typedef struct {
    bool converted;                  /* 이번 호출이 전환 단계를 하나 이상 수행했다 */
    bool resumed;                    /* 그중 이전에 중단된 전환을 이어서 끝냈다 */
    page_file_open_info_t file_info;
    wal_open_info_t wal_info;
} durable_pair_info_t;

/*
 * 두 파일을 쓰기로 열고 위 표대로 판정한다. 성공하면 *out에 두 핸들이 있다(PLAIN이면 wal이 NULL).
 * 실패하면 둘 다 닫고 *out은 모두 NULL이다. info는 NULL 가능이며 가능한 만큼 채운다.
 * replay와 applied_lsn 게시는 호출자가 이 짝으로 한다.
 */
durable_pair_status_t durable_pair_open(const char *db_path, const char *wal_path,
                                        const durable_pair_options_t *options, durable_pair_t *out,
                                        durable_pair_info_t *info);

/*
 * epoch를 영속화한다. page_file_stage_durable_epoch와 page_file_commit을 한 번에 하며, 성공을 돌려받은 뒤에만
 * 호출자가 그 epoch를 세션에 보여 줄 수 있다. 실패하면 보여 주면 안 된다. 제어 쓰기를 시도한 뒤의 실패는 영속 여부를
 * 알 수 없으므로 DURABLE_PAIR_IO이고 핸들은 poison이다(다시 열면 영속된 값이 floor다).
 */
durable_pair_status_t durable_pair_persist_epoch(durable_pair_t *pair, uint32_t epoch);

/* WAL을 먼저 닫고 데이터 파일을 닫는다. 처음 실패를 돌려준다. 핸들은 NULL이 된다. */
durable_pair_status_t durable_pair_close(durable_pair_t *pair);

const char *durable_pair_status_name(durable_pair_status_t status);

#endif
