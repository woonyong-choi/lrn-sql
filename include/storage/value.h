#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 값 하나의 TEXT·BLOB 최대 바이트. 호출자가 정하는 길이 필드가 4바이트일 때의 한계다. */
#define VALUE_MAX_BYTES UINT32_MAX

typedef enum {
    VALUE_OK,
    VALUE_INVALID,   /* 인자·view 오류, 잘못된 UTF-8, 형식이 틀린 문자열 */
    VALUE_MISMATCH,  /* 허용하지 않는 변환 조합 */
    VALUE_OVERFLOW,  /* int64 범위 밖, 정밀도를 잃는 변환, 유한하지 않은 실수 */
    VALUE_LIMIT,     /* VALUE_MAX_BYTES 초과 또는 호출자 buffer 부족 */
    VALUE_NOMEM
} value_status_t;

typedef enum {
    VALUE_NULL,
    VALUE_INTEGER,
    VALUE_REAL,
    VALUE_TEXT,
    VALUE_BLOB
} value_type_t;

/*
 * 빌린 값(view). 소유하지 않으며 bytes가 가리키는 원본이 살아 있는 동안만 유효하다.
 * 유효한 view: NULL·INTEGER·REAL은 bytes가 NULL이고 size_bytes가 0, REAL은 유한값,
 * TEXT·BLOB은 size_bytes > 0이면 bytes가 NULL이 아님, TEXT는 유효한 UTF-8.
 * 이 모듈의 모든 함수는 잘못된 view를 조용히 처리하지 않고 VALUE_INVALID로 거절한다.
 * NULL은 빈 TEXT·BLOB과 다르다. TEXT·BLOB의 길이는 size_bytes이며 NUL 바이트(U+0000)를
 * 포함해도 그대로 보존한다. C 문자열 종료 문자는 어디서도 길이를 정하지 않는다.
 */
typedef struct {
    value_type_t type;
    int64_t integer;
    double real;
    const uint8_t *bytes;
    size_t size_bytes;
} value_view_t;

/*
 * 소유 값. TEXT·BLOB 바이트는 value가 복사해 보관하며 value_clear가 해제한다.
 * 호출자는 value_t를 0으로 초기화해서 쓰며 0 값은 NULL이다. value_clear는 반복 호출해도 안전하다.
 * 생성·변환 함수는 새 값을 먼저 만든 뒤 성공했을 때만 out의 이전 값을 해제하고 교체한다.
 * 실패하면 out은 변경하지 않는다. 원본 view가 out의 바이트를 가리켜도 안전하다.
 */
typedef struct {
    value_view_t view;
    bool owns_bytes;
} value_t;

void value_clear(value_t *value);
value_view_t value_as_view(const value_t *value);

value_status_t value_set_null(value_t *out);
value_status_t value_set_integer(value_t *out, int64_t integer);
/* NaN·무한대는 저장하지 않고 VALUE_OVERFLOW다. -0.0은 그대로 보존한다. */
value_status_t value_set_real(value_t *out, double real);
/*
 * 유효한 UTF-8이어야 한다(과대 인코딩·서로게이트·U+10FFFF 초과는 VALUE_INVALID).
 * U+0000을 포함해도 보존한다. 길이 선언은 없고 VARCHAR 제한은 저장 폭과 분리한다.
 */
value_status_t value_set_text(value_t *out, const uint8_t *bytes, size_t size_bytes);
/* 임의 바이트를 허용한다. */
value_status_t value_set_blob(value_t *out, const uint8_t *bytes, size_t size_bytes);
value_status_t value_clone(const value_view_t *from, value_t *out);

/*
 * 변환은 조용한 절단·반올림·정밀도 손실 없이 정확히 되거나 거절한다.
 * NULL은 어떤 대상으로도 NULL이 된다. 그 밖의 허용 조합:
 *   INTEGER -> REAL   double로 정확히 표현되는 값만(2^53 초과도 정확하면 허용). 아니면 VALUE_OVERFLOW
 *   REAL -> INTEGER   정수값이고 int64 범위 안일 때만. 소수부가 있거나 범위 밖이면 VALUE_OVERFLOW
 *   TEXT -> INTEGER   [+-]?[0-9]+ 전체. 공백·NUL·꼬리 문자는 VALUE_INVALID, 범위 밖은 VALUE_OVERFLOW
 *   TEXT -> REAL      [+-]?[0-9]+(.[0-9]+)?([eE][+-]?[0-9]+)? 전체를 가장 가까운 double로. 형식 오류는 VALUE_INVALID,
 *                     무한대로 넘치거나 0이 아닌 값이 0으로 사라지면 VALUE_OVERFLOW
 *   INTEGER/REAL -> TEXT  value_format과 같은 문자열
 *   TEXT -> BLOB      같은 바이트
 *   BLOB -> TEXT      유효한 UTF-8일 때만(NUL 허용), 아니면 VALUE_INVALID
 *   같은 타입          복제
 *   그 밖(BLOB<->숫자 등) VALUE_MISMATCH
 * SQLite와 다른 점(통과 범위가 아님): SQLite는 '12abc'·소수부 있는 REAL·잘못된 UTF-8을 허용하거나
 * 잘라서 변환하지만 여기서는 거절한다.
 */
value_status_t value_cast(const value_view_t *from, value_type_t target, value_t *out);

/*
 * 정렬용 전순서를 *order(-1, 0, 1)에 채운다. NULL < 숫자 < TEXT < BLOB이다.
 * INTEGER와 REAL은 변환 없이 수학적으로 정확히 비교한다(2^53 초과 정수 포함, -0.0 == 0.0).
 * TEXT·BLOB은 길이를 가진 바이트열로 memcmp 후 길이 순이다(UTF-8 바이트 순).
 * 잘못된 view면 VALUE_INVALID(비유한 실수는 VALUE_OVERFLOW)이며 *order는 변경하지 않는다.
 * SQL의 NULL 3치 논리는 이 함수가 아니라 실행기가 정한다.
 */
value_status_t value_compare(const value_view_t *left, const value_view_t *right, int *order);

/*
 * 출력 문자열. 같은 값은 항상 같은 바이트를 만들며 로캘과 무관하다. 문자열 길이는 *needed 바이트
 * (NUL 제외)이고 capacity가 *needed보다 크면 끝에 종료 NUL을 하나 덧붙인다.
 *   NULL "NULL" / INTEGER 10진 / TEXT 원문 바이트 그대로(내장 NUL 포함)
 *   REAL "%.17g" 형식: 유한값을 왕복할 수 있는 17자리 표현이며 최단 표현은 약속하지 않는다.
 *        소수점과 지수가 없는 결과에는 ".0"을 붙인다(예 "1.0", "-0.0")
 *   BLOB "X'ab01'" 소문자 16진
 * buffer가 모자라면 VALUE_LIMIT이며 *needed만 채우고 buffer는 변경하지 않는다.
 * 그 밖의 실패에서는 buffer와 *needed를 변경하지 않는다.
 */
value_status_t value_format(const value_view_t *value, char *buffer, size_t capacity,
                            size_t *needed);

#endif
