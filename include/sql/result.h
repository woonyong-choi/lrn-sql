#ifndef RESULT_H
#define RESULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/value.h"

/* 고정 한도. 넘으면 RESULT_LIMIT(생성·builder) 또는 RESULT_E_LIMIT(source 행)다. */
#define RESULT_MAX_COLUMNS 1024u
#define RESULT_MAX_NAME_BYTES 256u
#define RESULT_MAX_TAG_BYTES 32u
#define RESULT_MAX_ERROR_MESSAGE_BYTES 1024u
/* 행 하나(셀 배열 + TEXT·BLOB 바이트)와 builder 전체(metadata + 모든 행)의 기본 한도 */
#define RESULT_DEFAULT_MAX_ROW_BYTES ((size_t)64 << 20)
#define RESULT_DEFAULT_MAX_TOTAL_BYTES ((size_t)256 << 20)

/*
 * 생성 함수의 반환값. 실패하면 *out은 NULL이고 호출자가 넘긴 자원(source ctx 포함)은
 * 호출자 소유로 남으며 source의 close는 호출되지 않는다.
 */
typedef enum {
    RESULT_OK,
    RESULT_INVALID,  /* 인자·column·행이 계약에 맞지 않음 */
    RESULT_LIMIT,    /* 고정 한도나 builder 한도 초과 */
    RESULT_NOMEM
} result_status_t;

typedef enum { RESULT_KIND_ROWS, RESULT_KIND_COMMAND, RESULT_KIND_ERROR } result_kind_t;

typedef enum {
    RESULT_E_NONE,
    RESULT_E_SYNTAX,
    RESULT_E_SEMANTIC,
    RESULT_E_CONSTRAINT,
    RESULT_E_LIMIT,
    RESULT_E_NOMEM,
    RESULT_E_IO,
    RESULT_E_BUSY,
    RESULT_E_CANCELLED,
    RESULT_E_INTERNAL
} result_error_class_t;

/*
 * 오류. 입력일 때 message는 호출자가 소유하고 결과가 복사한다(NULL이면 길이 0).
 * RESULT_MAX_ERROR_MESSAGE_BYTES를 넘으면 그 길이에서 잘라 복사하고 is_message_truncated를 켠다.
 * 결과에서 읽을 때 message는 결과가 소유하며 result_release 전까지 유효하다.
 * code는 호출자 정의 세부 코드이며 이 모듈은 해석하지 않는다.
 */
typedef struct {
    result_error_class_t error_class;
    int code;
    const char *message;
    size_t message_len;
    bool is_message_truncated;
} result_error_t;

/*
 * 컬럼 metadata. name은 비어 있지 않은 UTF-8이며 결과가 복사한다. 셀은 type이거나 is_nullable일 때 NULL.
 * 모든 행의 셀은 value.h의 유효한 view여야 한다: TEXT는 UTF-8, REAL은 유한값(NaN·무한대 거절),
 * TEXT·BLOB은 VALUE_MAX_BYTES 이하. 어기면 builder는 INVALID·LIMIT, source 행은 INTERNAL·LIMIT 실패다.
 */
typedef struct {
    const uint8_t *name;
    size_t name_len;
    value_type_t type;
    bool is_nullable;
} result_column_t;

/* tag는 INSERT 같은 명령 이름이다. 1~RESULT_MAX_TAG_BYTES바이트, NUL 없음, 결과가 복사한다. */
typedef struct {
    uint64_t affected_rows;
    const char *tag;
    size_t tag_len;
} result_command_t;

/*
 * 0인 필드는 기본값이다. max_row_bytes <= max_total_bytes여야 한다. SIZE_MAX도 허용하며 합산은 오버플로 없이
 * LIMIT로 거절한다. 셀 하나의 VALUE_MAX_BYTES 상한은 이 한도와 별개로 항상 적용된다.
 * max_total_bytes는 builder 결과에만 적용된다.
 */
typedef struct {
    size_t max_row_bytes;
    size_t max_total_bytes;
} result_limits_t;

typedef struct result result_t;
typedef struct result_cancel result_cancel_t;

/*
 * 빌린 행. cells와 그 안의 bytes는 다음 result_next·result_release 이전의 같은 소비 스레드에서만 유효하다.
 * 오래 쓰려면 value_clone으로 복사한다.
 */
typedef struct {
    const value_view_t *cells;
    size_t cell_count;
} result_row_t;

typedef enum {
    RESULT_NEXT_ROW,        /* row가 채워졌다 */
    RESULT_NEXT_DONE,       /* 모든 행을 성공적으로 전달하고 끝났다. 이것만 성공 완료다 */
    RESULT_NEXT_FAILED,     /* 실패했다. result_outcome의 error를 본다 */
    RESULT_NEXT_CANCELLED   /* 취소가 관측됐다 */
} result_next_t;

/* source가 next 한 번에 채우는 입력·출력. cancel은 읽기 전용 입력이다. */
typedef struct {
    result_row_t row;
    result_error_t error;
    const result_cancel_t *cancel;
} result_step_t;

/*
 * 행 공급자. next는 ROW면 step->row를, FAILED면 step->error를 채운다.
 * FAILED의 error_class는 NONE·CANCELLED가 아니어야 하며 어기면 INTERNAL로 처리한다.
 * 오래 걸리는 next는 result_cancel_requested(step->cancel)를 주기적으로 보고 CANCELLED를 돌려줄 수 있다.
 * close는 생성에 성공한 결과마다 정확히 한 번 호출된다: 종료 상태(DONE·FAILED·CANCELLED)가 정해지는
 * 즉시, 그 전에 release되면 release에서. close 뒤에는 이전에 빌려준 행·error.message가 무효가 되므로
 * 결과는 close 전에 error.message를 복사한다. next와 close는 같은 소비 스레드에서만 호출된다.
 */
typedef struct {
    result_next_t (*next)(void *ctx, result_step_t *step);
    void (*close)(void *ctx);
} result_source_ops_t;

/* 성공하면 ctx의 소유권이 결과로 넘어간다(close가 해제를 책임진다). limits는 NULL이면 기본값이다. */
typedef struct {
    const result_column_t *columns;
    size_t column_count;
    const result_source_ops_t *ops;
    void *ctx;
    const result_limits_t *limits;
} result_rows_spec_t;

typedef struct {
    const result_column_t *columns;
    size_t column_count;
    const result_limits_t *limits;
} result_builder_spec_t;

typedef enum {
    RESULT_STATE_OPEN,       /* ROWS 결과가 아직 끝나지 않음. 성공이 아니다 */
    RESULT_STATE_COMPLETE,
    RESULT_STATE_FAILED,
    RESULT_STATE_CANCELLED
} result_state_t;

/*
 * is_partial은 FAILED·CANCELLED인데 rows_delivered가 0보다 큰 경우다. 호출자가 이미 소비한 행이 있으므로
 * 그 행들을 완전한 결과로 취급하면 안 된다. CANCELLED의 error_class는 RESULT_E_CANCELLED다.
 * error는 COMPLETE·OPEN이면 NULL이고 아니면 결과가 소유하며 release 전까지 유효하다.
 */
typedef struct {
    result_state_t state;
    uint64_t rows_delivered;
    bool is_partial;
    const result_error_t *error;
} result_outcome_t;

typedef struct result_builder result_builder_t;

/*
 * 결과를 만든다. 세 종류 모두 실패 계약은 result_status_t와 같다.
 * COMMAND는 만들자마자 COMPLETE, ERROR는 만들자마자 FAILED(rows_delivered 0)이며 result_next는
 * 각각 DONE·FAILED를 돌려준다.
 */
result_status_t result_open_rows(const result_rows_spec_t *spec, result_t **out);
result_status_t result_open_command(const result_command_t *command, result_t **out);
result_status_t result_open_error(const result_error_t *error, result_t **out);

/*
 * 메모리 결과 builder. max_total_bytes는 결과·builder 구조체와 컬럼 metadata 같은 고정 할당, 셀 배열,
 * 복사한 TEXT/BLOB 바이트, 행 슬롯 배열 증가분을 합산한다. 고정 할당만으로 한도를 넘으면 create가 LIMIT다.
 * source 기반 결과의 고정 할당(결과 구조체, 컬럼, 이름)은 한도 밖이며 RESULT_MAX_COLUMNS와
 * RESULT_MAX_NAME_BYTES로 상한이 정해진 상수 크기다.
 * add_row와 create가 실패하면 builder는 이전 상태 그대로 계속 쓸 수 있다.
 * finish는 실패하지 않는 경우에만 builder를 소비하고 *builder를 NULL로 만든다. 인자 오류(INVALID)면 builder는 그대로다.
 * destroy는 *builder가 NULL이면 아무것도 하지 않고, 아니면 정확히 한 번 해제한 뒤 NULL로 만든다.
 */
result_status_t result_builder_create(const result_builder_spec_t *spec, result_builder_t **out);
result_status_t result_builder_add_row(result_builder_t *builder, const value_view_t *cells,
                                       size_t cell_count);
result_status_t result_builder_finish(result_builder_t **builder, result_t **out);
void result_builder_destroy(result_builder_t **builder);

result_kind_t result_kind(const result_t *result);
size_t result_column_count(const result_t *result);
/* index가 범위 밖이면 NULL이다. 반환 포인터는 result_release 전까지 유효하다. */
const result_column_t *result_column(const result_t *result, size_t index);
/* COMMAND가 아니면 NULL이다. */
const result_command_t *result_command(const result_t *result);

/*
 * 다음 행으로 진행한다. 종료 상태는 한 번 정해지면 바뀌지 않으며 이후 호출은 같은 값을 돌려주고
 * source를 다시 부르지 않는다. 취소는 result_cancel 이후 첫 result_next에서 CANCELLED로 확정된다(이미 끝났으면 무시).
 */
result_next_t result_next(result_t *result, result_row_t *row);
result_outcome_t result_outcome(const result_t *result);

/*
 * 취소를 요청한다. result_next·result_outcome·result_release와 달리 다른 스레드에서 호출해도 된다.
 * result_release와 동시에 호출하거나 release 뒤에 호출하면 안 된다. 호출하는 쪽이 release 전에
 * 호출이 끝났음을 보장해야 한다.
 */
void result_cancel(result_t *result);
bool result_cancel_requested(const result_cancel_t *cancel);

/* 단일 소비 스레드 전용. *result가 NULL이면 무동작이고 아니면 한 번 해제하고 NULL로 만든다. */
void result_release(result_t **result);

#endif
