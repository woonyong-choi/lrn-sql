#ifndef LEXER_H
#define LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/value.h"

/*
 * 길이 기반 SQL lexer. 입력 전체를 한 번에 검증하며 토큰화하고 성공하거나 아무것도 돌려주지 않는다.
 * 그래서 문장의 앞부분만 토큰으로 넘어가 실행되는 경로가 없고, 한도를 넘는 토큰을 줄여서
 * 돌려주는 일도 없다. 입력은 (bytes, size)이며 NUL 종료를 가정하지 않는다.
 *
 * 이 모듈은 어휘만 검사한다. 입력 전체가 어휘상 유효해도 SQL 문법, 식별자 해석, 실행 안전은
 * 보장하지 않으며 #50의 통합 단계(parser·planner·executor)가 따로 검사해야 한다.
 */

/* 매개변수 ID 상한(비트맵 크기를 제한한다). */
#define LEXER_PARAMETER_CEILING 65535u

typedef enum {
    LEXER_OK,
    LEXER_INVALID,          /* 인자 오류, 0인 한도 */
    LEXER_LIMIT,            /* 호출자 한도 초과(reason 참고) */
    LEXER_NOMEM,
    LEXER_SYNTAX,           /* 어휘 오류(reason·offset 참고) */
    LEXER_PARAMETER_COUNT   /* 바인딩 값 개수가 매개변수 개수와 다름 */
} lexer_status_t;

typedef enum {
    LEXER_REASON_NONE,
    LEXER_REASON_INPUT_TOO_LARGE,
    LEXER_REASON_TOKEN_TOO_LARGE,
    LEXER_REASON_TOO_MANY_TOKENS,
    LEXER_REASON_TOO_MANY_PARAMETERS,
    LEXER_REASON_UNEXPECTED_BYTE,     /* 알 수 없는 바이트, 따옴표 밖 NUL·비ASCII */
    LEXER_REASON_INVALID_UTF8,
    LEXER_REASON_UNTERMINATED_STRING,
    LEXER_REASON_UNTERMINATED_IDENT,
    LEXER_REASON_UNTERMINATED_COMMENT,
    LEXER_REASON_EMPTY_IDENT,         /* "" */
    LEXER_REASON_BAD_NUMBER,
    LEXER_REASON_BAD_PARAMETER,       /* ?0, ?01, ?1a, ?a */
    LEXER_REASON_PARAMETER_MIX,       /* ?와 ?N 혼용 */
    LEXER_REASON_PARAMETER_GAP,       /* 사용한 ?N의 고유 ID가 1..최대 ID를 채우지 않음 */
    LEXER_REASON_EMPTY_STATEMENT      /* 빈 입력, 선행·연속 세미콜론 */
} lexer_reason_t;

/* 모든 한도는 호출자가 정한다. max_parameters만 0(매개변수 금지)을 허용한다. */
typedef struct {
    size_t max_input_bytes;
    size_t max_token_bytes;  /* 따옴표를 포함한 원문 span 기준 */
    size_t max_tokens;       /* 세미콜론 토큰 포함 */
    uint32_t max_parameters; /* LEXER_PARAMETER_CEILING 이하 */
} lexer_limits_t;

typedef struct {
    lexer_reason_t reason;
    size_t offset;  /* 문제가 된 입력 바이트 위치 */
} lexer_error_t;

typedef enum {
    LEXER_WORD,          /* [A-Za-z_][A-Za-z0-9_]* ASCII bare 식별자·키워드(구분은 parser 몫) */
    LEXER_QUOTED_IDENT,  /* "..." 안은 UTF-8, "" escape */
    LEXER_STRING,        /* '...' 안은 UTF-8, '' escape, 내장 NUL 허용 */
    LEXER_NUMBER,        /* 부호 없는 10진 정수·실수. 값 변환은 value_cast 몫 */
    LEXER_PARAMETER,
    LEXER_PUNCT,
    LEXER_SEMICOLON
} lexer_token_kind_t;

/*
 * bytes는 입력 buffer의 span이라 입력이 살아 있고 바뀌지 않는 동안만 유효하다.
 * STRING·QUOTED_IDENT의 text는 따옴표를 벗기고 escape를 푼 내용이다. escape가 없으면 입력 span을
 * 가리키고, 있으면 lexed_t가 소유한 arena를 가리켜 lexer_free 전까지 유효하다.
 * 그 밖의 토큰은 text가 bytes와 같다. parameter_id는 PARAMETER에서만 의미가 있다.
 */
typedef struct {
    lexer_token_kind_t kind;
    size_t offset;
    const uint8_t *bytes;
    size_t size;
    const uint8_t *text;
    size_t text_size;
    uint32_t parameter_id;
} lexer_token_t;

/* 세미콜론을 제외한 토큰 구간. terminated는 뒤에 세미콜론이 있었는지다. */
typedef struct {
    const lexer_token_t *tokens;
    size_t token_count;
    size_t offset;      /* 첫 토큰 위치 */
    size_t end_offset;  /* 문장 끝(세미콜론 포함) 다음 위치 */
    bool terminated;
} lexer_statement_t;

typedef struct lexed lexed_t;

/*
 * 성공하면 *out을 채우며 lexer_free로 해제한다. 실패하면 *out을 바꾸지 않고 할당을 남기지 않는다.
 * error는 NULL이어도 된다. 주석은 건너뛰고 닫히지 않은 따옴표·주석, 잘못된 UTF-8, 알 수 없는
 * 바이트, 빈 문장은 거절한다. 문장은 문자열·주석 밖의 세미콜론으로 나뉘며 마지막 세미콜론은 선택이다.
 * 매개변수는 입력 전체에서 한 번 번호를 매긴다.
 */
lexer_status_t lexer_scan(const uint8_t *input, size_t size, const lexer_limits_t *limits,
                          lexed_t **out, lexer_error_t *error);
void lexer_free(lexed_t *lexed);

/*
 * 메모리 회계용 상한. 길이 input_size인 입력을 limits로 lexer_scan하는 동안 요청 바이트의 순간 최대치 상한이며
 * 성공한 lexed_t가 lexer_free까지 쥐는 몫도 이 값 이하다. 모든 토큰은 입력 바이트를 하나 이상 덮으므로
 * 토큰과 문장 수는 min(input_size, max_tokens) 이하이고 arena는 input_size 이하다. 요청 바이트 기준이며
 * allocator 오버헤드는 포함하지 않는다. 입력이 한도를 넘어 scan이 할당 전에 거절하는 경우에도 같은 식이다.
 */
size_t lexer_scan_peak_bytes(size_t input_size, const lexer_limits_t *limits);

size_t lexed_token_count(const lexed_t *lexed);
const lexer_token_t *lexed_tokens(const lexed_t *lexed);
size_t lexed_statement_count(const lexed_t *lexed);
lexer_status_t lexed_statement_at(const lexed_t *lexed, size_t index, lexer_statement_t *out);

/*
 * 매개변수 개수는 고유 ID의 수이며 ?N 반복이나 같은 ID의 재사용은 한 개로 센다. `?`는 나타난
 * 순서대로 1부터 번호를 받는다. ?N만 쓸 때는 고유 ID 집합이 정확히 1..최대 ID여야 한다.
 * placeholder가 나타난 횟수와 바인딩 값 개수는 다른 값이며 비교는 이 개수로 한다.
 */
uint32_t lexed_parameter_count(const lexed_t *lexed);

/* 바인딩 값은 호출자가 소유하고 SQL 문자열에 다시 이어 붙이지 않는다. 배열이 살아 있는 동안 유효하다. */
typedef struct {
    const value_view_t *values;
    size_t count;
} lexer_binding_t;

/* count가 lexed_parameter_count와 같고(0이면 values는 NULL도 가능) 아니면 LEXER_PARAMETER_COUNT. */
lexer_status_t lexer_check_binding(const lexed_t *lexed, const lexer_binding_t *binding);
/* id는 1부터이며 범위 밖이면 LEXER_INVALID이고 *view는 바뀌지 않는다. view 내용 검증은 value_* 호출 몫이다. */
lexer_status_t lexer_binding_value(const lexer_binding_t *binding, uint32_t id,
                                   const value_view_t **view);

#endif
