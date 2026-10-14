#ifndef SQL_STATEMENT_H
#define SQL_STATEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/lexer.h"
#include "storage/value.h"

/*
 * lexer 토큰 위에서 동작하는 SQL 문 parser. lexer_statement_t 하나를 받아 문 구조(sqlstmt_t)를
 * 만든다. 기존 legacy parser(parser.h)와 statement_t는 건드리지 않으며 이 모듈은 그것들과
 * 이름도 상태도 공유하지 않는다. 한 번의 호출은 문장 하나만 다루며 입력에 문장이 몇 개인지,
 * 어느 문장을 실행할지는 호출자(database)가 정한다.
 *
 * 문법(키워드는 대소문자를 구분하지 않고 식별자는 바이트 단위로 대소문자를 구분한다):
 *   CREATE TABLE name ( item {, item} )
 *     item       = column | PRIMARY KEY ( column_name )
 *     column     = column_name type {constraint}
 *     type       = INTEGER | REAL | TEXT | BLOB | VARCHAR ( n )
 *     constraint = NOT NULL | PRIMARY KEY [AUTOINCREMENT]
 *   INSERT INTO name [( column_name {, column_name} )] VALUES ( value {, value} )
 *   SELECT [DISTINCT] ( * | column_name {, column_name} ) FROM name [where]
 *     [ORDER BY column_name [ASC | DESC]] [LIMIT nonnegative_integer]
 *   UPDATE name SET column_name = value {, column_name = value} [where]
 *   DELETE FROM name [where]
 *   DROP TABLE name
 *   where      = WHERE column_name ( = | != | <> | < | <= | > | >= ) value
 *              | WHERE column_name IS [NOT] NULL
 *   value      = [+|-] number | 'string' | NULL | X'hex' | ? | ?N
 *   문장 끝의 세미콜론은 lexer가 처리하므로 이 API는 토큰만 본다.
 *
 * 의미:
 *   - VARCHAR(n)의 n은 읽기만 하고 varchar_length로 노출한다. 값을 자르거나 길이를 강제하지
 *     않으며 타입은 TEXT다. n은 1 이상 UINT32_MAX 이하여야 한다.
 *   - AUTOINCREMENT는 INTEGER 컬럼의 컬럼 제약 PRIMARY KEY 바로 뒤에만 쓸 수 있고
 *     has_identity가 된다. 테이블 제약 PRIMARY KEY (...)에는 쓸 수 없다.
 *   - 기본 키는 한 컬럼이다. 컬럼 제약과 테이블 제약을 합쳐 둘 이상이거나 없는 컬럼 이름이면 거절한다.
 *     primary_key_column은 columns 배열의 번호이며 없으면 SQLSTMT_NO_COLUMN이다.
 *   - 정수 리터럴은 INTEGER, 소수점이나 지수가 있으면 REAL이다. int64 범위를 벗어나는 정수와
 *     유한하지 않은 실수는 REAL로 바꾸지 않고 SQLSTMT_LIMIT로 거절한다. 숫자 리터럴은 부호를 포함해
 *     4096바이트 이하여야 한다.
 *   - 매개변수는 lexer가 번호를 매긴 id로 binding에서 값을 찾아 복제한다. 값은 SQL에 이어 붙이지 않는다.
 *   - NULL은 값이며 NULL과의 = 비교도 그대로 구조에 담는다. 3치 논리는 실행기 몫이다.
 *   - X'hex'는 X와 따옴표 문자열이 붙어 있어야 하며 짝수 개의 16진 숫자여야 한다. 빈 X''는 빈 BLOB이다.
 *
 * 명시적으로 거절하는 것(SQLSTMT_UNSUPPORTED): GROUP BY·HAVING·OFFSET·
 * JOIN·UNION 같은 절, AND·OR로 이은 둘 이상의 조건, 별칭(AS), 함수 호출, 테이블.컬럼 형태의 이름,
 * IF [NOT] EXISTS, 여러 행 INSERT, 컬럼 제약 UNIQUE·DEFAULT·CHECK·REFERENCES 등, 테이블 제약 중
 * PRIMARY KEY 외의 것, 복합 기본 키, 위에 없는 타입 이름, CREATE TABLE이 아닌 CREATE.
 *
 * 소유권: 성공한 sqlstmt_t의 모든 이름·값은 이 구조가 복사해 소유하며 입력 SQL과 binding이
 * 사라져도 유효하다. 해제는 sqlstmt_free 한 번이다. 실패하면 *out을 바꾸지 않고 할당을 남기지 않는다.
 * 할당 실패는 SQLSTMT_NOMEM으로 따로 돌려준다. 단일 스레드 계약이다.
 *
 * 보장하지 않는 것: 테이블·컬럼 이름이 실제로 있는지, 값의 타입이 컬럼과 맞는지, INSERT에 값이
 * 모자라거나 남는지(컬럼 목록이 있을 때만 개수를 맞춘다), NOT NULL·기본 키 중복 같은 실행 의미는
 * 검사하지 않는다. 예약어는 따옴표로 감싼 식별자로만 이름에 쓸 수 있다. 어휘·UTF-8 검증은 lexer가 한 뒤라고 가정한다.
 */

#define SQLSTMT_NO_COLUMN UINT16_MAX

typedef enum {
    SQLSTMT_OK,
    SQLSTMT_INVALID,      /* 인자 오류, binding 없음·잘못된 binding 값 */
    SQLSTMT_SYNTAX,       /* 문법 오류(reason·offset 참고) */
    SQLSTMT_UNSUPPORTED,  /* 문법은 알지만 지원하지 않음 */
    SQLSTMT_LIMIT,        /* 이름 길이·항목 수·숫자 크기 한도 초과 */
    SQLSTMT_NOMEM
} sqlstmt_status_t;

typedef enum {
    SQLSTMT_REASON_NONE,
    SQLSTMT_REASON_BAD_ARGUMENT,
    SQLSTMT_REASON_MISSING_BINDING,        /* 매개변수가 있는데 binding이 없음 */
    SQLSTMT_REASON_BAD_BINDING,            /* binding 값이 잘못됨(value_status 참고) */
    SQLSTMT_REASON_UNKNOWN_STATEMENT,
    SQLSTMT_REASON_UNEXPECTED_END,         /* 토큰이 모자람, offset은 문장 마지막 토큰 끝 */
    SQLSTMT_REASON_EXPECTED_KEYWORD,       /* 기대한 키워드·구두점이 아님 */
    SQLSTMT_REASON_EXPECTED_IDENTIFIER,    /* 이름 자리에 이름이 아님(예약어 포함) */
    SQLSTMT_REASON_EXPECTED_VALUE,
    SQLSTMT_REASON_TRAILING_TOKENS,        /* 문장이 끝난 뒤 토큰이 남음 */
    SQLSTMT_REASON_BAD_NUMBER,
    SQLSTMT_REASON_NUMBER_OUT_OF_RANGE,
    SQLSTMT_REASON_BAD_BLOB_LITERAL,
    SQLSTMT_REASON_BAD_VARCHAR_LENGTH,
    SQLSTMT_REASON_NAME_TOO_LONG,
    SQLSTMT_REASON_TOO_MANY_ITEMS,         /* 컬럼·값·SET 항목이 CATALOG_MAX_COLUMNS 초과 */
    SQLSTMT_REASON_NO_COLUMNS,
    SQLSTMT_REASON_DUPLICATE_COLUMN,
    SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY,
    SQLSTMT_REASON_UNKNOWN_PRIMARY_KEY_COLUMN,
    SQLSTMT_REASON_AUTOINCREMENT_NOT_INTEGER_KEY,
    SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH,
    SQLSTMT_REASON_UNSUPPORTED_CREATE,
    SQLSTMT_REASON_UNSUPPORTED_CLAUSE,
    SQLSTMT_REASON_UNSUPPORTED_CONDITION,  /* AND·OR */
    SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, /* 함수 호출, 한정 이름 */
    SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT,
    SQLSTMT_REASON_UNSUPPORTED_TYPE
} sqlstmt_reason_t;

typedef struct {
    sqlstmt_reason_t reason;
    size_t offset;                /* 문제가 된 입력 바이트 위치 */
    value_status_t value_status;  /* 값 변환·복제가 원인일 때만 VALUE_OK가 아님 */
} sqlstmt_error_t;

typedef enum {
    SQLSTMT_CREATE_TABLE,
    SQLSTMT_INSERT,
    SQLSTMT_SELECT,
    SQLSTMT_UPDATE,
    SQLSTMT_DELETE,
    SQLSTMT_DROP_TABLE
} sqlstmt_kind_t;

/* 소유한 이름 바이트. NUL 종료가 아니며 길이는 size다. */
typedef struct {
    uint8_t *bytes;
    size_t size;
} sqlstmt_name_t;

typedef struct {
    sqlstmt_name_t name;
    value_type_t type;           /* INTEGER·REAL·TEXT·BLOB. VARCHAR는 TEXT */
    bool not_null;
    bool has_varchar_length;
    uint32_t varchar_length;     /* has_varchar_length일 때만 의미가 있고 강제하지 않는다 */
} sqlstmt_column_def_t;

typedef struct {
    sqlstmt_name_t table;
    sqlstmt_column_def_t *columns;
    size_t column_count;
    uint16_t primary_key_column; /* 없으면 SQLSTMT_NO_COLUMN */
    bool has_identity;
} sqlstmt_create_t;

typedef struct {
    sqlstmt_name_t table;
    bool has_columns;
    sqlstmt_name_t *columns;     /* has_columns일 때 column_count개 */
    size_t column_count;
    value_t *values;
    size_t value_count;          /* has_columns이면 column_count와 같다 */
} sqlstmt_insert_t;

typedef enum {
    SQLSTMT_OP_EQ,
    SQLSTMT_OP_NE,        /* != 와 <> */
    SQLSTMT_OP_LT,
    SQLSTMT_OP_LE,
    SQLSTMT_OP_GT,
    SQLSTMT_OP_GE,
    SQLSTMT_OP_IS_NULL,
    SQLSTMT_OP_IS_NOT_NULL
} sqlstmt_op_t;

typedef struct {
    bool present;
    sqlstmt_name_t column;
    sqlstmt_op_t op;
    value_t value;               /* IS [NOT] NULL이면 NULL 값 */
} sqlstmt_where_t;

typedef struct {
    sqlstmt_name_t table;
    bool distinct;
    bool select_all;
    sqlstmt_name_t *columns;     /* select_all이 아닐 때 column_count개 */
    size_t column_count;
    sqlstmt_where_t where;
    bool has_order;
    sqlstmt_name_t order_column;
    bool order_desc;
    bool has_limit;
    size_t limit;
} sqlstmt_select_t;

typedef struct {
    sqlstmt_name_t column;
    value_t value;
} sqlstmt_assignment_t;

typedef struct {
    sqlstmt_name_t table;
    sqlstmt_assignment_t *assignments;
    size_t assignment_count;
    sqlstmt_where_t where;
} sqlstmt_update_t;

typedef struct {
    sqlstmt_name_t table;
    sqlstmt_where_t where;
} sqlstmt_delete_t;

typedef struct {
    sqlstmt_name_t table;
} sqlstmt_drop_t;

typedef struct {
    sqlstmt_kind_t kind;
    union {
        sqlstmt_create_t create;
        sqlstmt_insert_t insert;
        sqlstmt_select_t select;
        sqlstmt_update_t update;
        sqlstmt_delete_t del;
        sqlstmt_drop_t drop;
    };
} sqlstmt_t;

/*
 * statement의 토큰은 호출 동안만 빌린다. binding은 매개변수가 없으면 NULL이어도 된다. error는 NULL이어도 되며
 * 성공하면 reason이 NONE이다. 성공한 *out은 호출자가 sqlstmt_free한다.
 */
sqlstmt_status_t sqlstmt_parse(const lexer_statement_t *statement, const lexer_binding_t *binding,
                               sqlstmt_t **out, sqlstmt_error_t *error);
void sqlstmt_free(sqlstmt_t *statement);

#endif
