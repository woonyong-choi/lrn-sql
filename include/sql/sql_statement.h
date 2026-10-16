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
 *   CREATE [UNIQUE] INDEX name ON table ( column_name )
 *     item       = column | PRIMARY KEY ( column_name )
 *     column     = column_name type {constraint}
 *     type       = INTEGER | REAL | TEXT | BLOB | VARCHAR ( n )
 *     constraint = NOT NULL | PRIMARY KEY [AUTOINCREMENT] | DEFAULT 상수 | DEFAULT -숫자 | DEFAULT ( expr )
 *   INSERT INTO name [( column_name {, column_name} )] VALUES ( expr {, expr} ) {, ( expr {, expr} )} [conflict] [returning]
 *   INSERT INTO name DEFAULT VALUES [conflict] [returning]
 *     conflict   = ON CONFLICT [( column_name )] DO NOTHING
 *                | ON CONFLICT ( column_name ) DO UPDATE SET column_name = expr {, ...} [WHERE expr]
 *   [WITH cte {, cte}] SELECT core {set_op core} [ORDER BY expr [ASC|DESC] [NULLS FIRST|LAST] {, ...}] [LIMIT expr] [OFFSET expr]
 *     cte        = name [( column {, column} )] AS ( query )
 *     core       = [DISTINCT] item {, item} [FROM source {join}] [WHERE expr] [GROUP BY expr {, expr}] [HAVING expr]
 *     set_op     = UNION [ALL] SELECT | EXCEPT SELECT | INTERSECT SELECT
 *     item       = * | name.* | expr [[AS] alias]
 *     source     = name [[AS] alias] | ( query ) [[AS] alias]
 *     join       = , source | CROSS JOIN source | [INNER] JOIN source ON expr | LEFT [OUTER] JOIN source ON expr |
 *                  RIGHT [OUTER] JOIN source ON expr (첫 결합에서만)
 *   UPDATE name SET column_name = expr {, column_name = expr} [WHERE expr] [returning]
 *   DELETE FROM name [WHERE expr] [returning]
 *     returning  = RETURNING item {, item}   (item은 SELECT 목록과 같다)
 *   DROP TABLE name
 *   BEGIN [TRANSACTION] | START TRANSACTION
 *   COMMIT [TRANSACTION] | ROLLBACK [TRANSACTION]
 *   expr(우선순위 낮은 것부터): OR, AND, NOT, 비교(= == != <> < <= > >=)·IS [NOT] NULL·[NOT] BETWEEN·[NOT] IN (목록)·
 *     [NOT] LIKE [ESCAPE], + -, * / %, ||, 단항 + -, 기본식
 *   기본식      = 숫자 | 'string' | NULL | TRUE | FALSE | X'hex' | ? | ?N | [name.]column | name(args) | name(*) |
 *                name(DISTINCT expr) | CASE [expr] WHEN expr THEN expr {...} [ELSE expr] END | CAST(expr AS type) | ( expr ) |
 *                ( query ) | EXISTS ( query ),  그리고 expr [NOT] IN ( query )
 *   query      = [WITH ...] SELECT ... (괄호 안의 SELECT 문 전체)
 *   문장 끝의 세미콜론은 lexer가 처리하므로 이 API는 토큰만 본다.
 *
 * 의미:
 *   - VARCHAR(n)의 n은 읽기만 하고 varchar_length로 노출한다. 값을 자르거나 길이를 강제하지
 *     않으며 타입은 TEXT다. n은 1 이상 UINT32_MAX 이하여야 한다.
 *   - AUTOINCREMENT는 INTEGER 컬럼의 컬럼 제약 PRIMARY KEY 바로 뒤에만 쓸 수 있고
 *     has_identity가 된다. 테이블 제약 PRIMARY KEY (...)에는 쓸 수 없다.
 *   - 기본 키는 한 컬럼이다. 컬럼 제약과 테이블 제약을 합쳐 둘 이상이거나 없는 컬럼 이름이면 거절한다.
 *     primary_key_column은 columns 배열의 번호이며 없으면 SQLSTMT_NO_COLUMN이다.
 *   - 정수 리터럴은 INTEGER, 소수점이나 지수가 있으면 REAL이다. 부호가 붙은 숫자는 리터럴 하나로 읽는다(INT64_MIN).
 *     int64 범위를 벗어나는 정수와 유한하지 않은 실수는 SQLSTMT_LIMIT로 거절한다. 숫자 리터럴은 부호를 포함해
 *     4096바이트 이하여야 한다. TRUE·FALSE는 INTEGER 1·0이다.
 *   - 매개변수는 lexer가 번호를 매긴 id로 binding에서 값을 찾아 리터럴로 복제한다. 값은 SQL에 이어 붙이지 않는다.
 *   - 함수 이름은 대문자로 맞춰 둔다. 어떤 함수가 있는지, 열 이름이 맞는지, 형식이 맞는지는 실행기(sql_eval) 몫이다.
 *   - X'hex'는 X와 따옴표 문자열이 붙어 있어야 하며 짝수 개의 16진 숫자여야 한다. 빈 X''는 빈 BLOB이다.
 *   - 식 트리 높이는 512, 목록(인자·IN·여러 행 INSERT의 값)은 65536개까지다. 넘으면 SQLSTMT_LIMIT다.
 *   - FROM 원본은 SQLSTMT_MAX_SOURCES개까지다. 넘으면 SQLSTMT_LIMIT다.
 *
 * 명시적으로 거절하는 것(SQLSTMT_UNSUPPORTED): 첫 결합이 아닌 RIGHT JOIN, FULL·NATURAL JOIN, JOIN ... USING, 괄호로 묶은 JOIN,
 * INTERSECT와 UNION·EXCEPT를 괄호 없이 섞기, EXCEPT ALL·INTERSECT ALL, 집합 연산 자리의 괄호 SELECT,
 * WITH RECURSIVE, SELECT가 아닌 문장 앞의 WITH, 창 함수, INSERT ... SELECT,
 * CREATE TABLE ... AS,
 * IF [NOT] EXISTS, 컬럼 제약 UNIQUE·DEFAULT·CHECK·REFERENCES 등, 테이블 제약 중 PRIMARY KEY 외의 것,
 * 복합 기본 키·복합 인덱스, 위에 없는 타입 이름, 위에 없는 CREATE, 테이블 이름의 한정(schema.table).
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
    SQLSTMT_CREATE_INDEX,
    SQLSTMT_INSERT,
    SQLSTMT_SELECT,
    SQLSTMT_UPDATE,
    SQLSTMT_DELETE,
    SQLSTMT_DROP_TABLE,
    SQLSTMT_BEGIN,     /* 트랜잭션 제어문. 실행은 연결(session·database_conn)이 한다 */
    SQLSTMT_COMMIT,
    SQLSTMT_ROLLBACK
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
    struct sqlexpr *default_value;  /* DEFAULT 식. 없으면 NULL */
} sqlstmt_column_def_t;

typedef struct {
    sqlstmt_name_t table;
    sqlstmt_column_def_t *columns;
    size_t column_count;
    uint16_t primary_key_column; /* 없으면 SQLSTMT_NO_COLUMN */
    bool has_identity;
} sqlstmt_create_t;

typedef struct {
    sqlstmt_name_t name;
    sqlstmt_name_t table;
    sqlstmt_name_t column;
    bool unique;
} sqlstmt_index_t;

/* ── 식 ── */

typedef enum {
    SQLEXPR_LITERAL,   /* value */
    SQLEXPR_COLUMN,    /* [qualifier.]name */
    SQLEXPR_UNARY,     /* op(NEG·PLUS·NOT) args[0] */
    SQLEXPR_BINARY,    /* args[0] op args[1] */
    SQLEXPR_IS_NULL,   /* args[0] IS [NOT] NULL (negated) */
    SQLEXPR_BETWEEN,   /* args[0] [NOT] BETWEEN args[1] AND args[2] */
    SQLEXPR_IN,        /* args[0] [NOT] IN (args[1..]) | args[0] [NOT] IN ( query ) (subquery) */
    SQLEXPR_LIKE,      /* args[0] [NOT] LIKE args[1] [ESCAPE args[2]] */
    SQLEXPR_CASE,      /* has_operand면 args[0]이 비교 대상. 이어 WHEN·THEN 쌍, has_else면 마지막이 ELSE */
    SQLEXPR_FUNCTION,  /* name(args) | name(*) (star) | name(DISTINCT x) (distinct) */
    SQLEXPR_SUBQUERY,  /* ( query ): 한 행 한 열 결과(행이 없으면 NULL) */
    SQLEXPR_EXISTS,    /* EXISTS ( query ) */
    SQLEXPR_CAST       /* CAST(args[0] AS cast_type) */
} sqlexpr_kind_t;

typedef enum {
    SQLEXPR_OP_NEG, SQLEXPR_OP_PLUS, SQLEXPR_OP_NOT,
    SQLEXPR_OP_ADD, SQLEXPR_OP_SUB, SQLEXPR_OP_MUL, SQLEXPR_OP_DIV, SQLEXPR_OP_MOD, SQLEXPR_OP_CONCAT,
    SQLEXPR_OP_EQ, SQLEXPR_OP_NE, SQLEXPR_OP_LT, SQLEXPR_OP_LE, SQLEXPR_OP_GT, SQLEXPR_OP_GE,
    SQLEXPR_OP_AND, SQLEXPR_OP_OR
} sqlexpr_op_t;

typedef struct sqlexpr {
    sqlexpr_kind_t kind;
    sqlexpr_op_t op;
    bool negated;
    bool has_operand;
    bool has_else;
    bool star;
    bool distinct;
    value_type_t cast_type;
    size_t offset;               /* 식이 시작한 입력 바이트 위치(오류 보고용) */
    value_t value;
    sqlstmt_name_t qualifier;    /* COLUMN의 테이블 이름. 없으면 size 0 */
    sqlstmt_name_t name;         /* COLUMN·FUNCTION 이름 */
    struct sqlexpr **args;
    size_t arg_count;
    struct sqlstmt_select *subquery;  /* SUBQUERY·EXISTS·IN ( query )의 질의 */
} sqlexpr_t;

typedef struct {
    sqlexpr_t *expr;             /* star면 NULL */
    bool star;                   /* * 또는 table.* */
    sqlstmt_name_t qualifier;    /* table.*의 table */
    sqlstmt_name_t alias;        /* [AS] alias. 없으면 size 0 */
} sqlstmt_select_item_t;

typedef struct {
    sqlstmt_name_t column;
    sqlexpr_t *value;
} sqlstmt_assignment_t;

/*
 * INSERT ... ON CONFLICT [( column )] DO NOTHING | DO UPDATE SET ... [WHERE expr].
 * target이 없으면 기본 키와 모든 유일 색인이 대상이다(DO NOTHING만). DO UPDATE의 식에서 excluded.column은
 * 넣으려던 값, 한정 없는 이름과 테이블 이름 한정은 이미 있는 행이다.
 */
typedef struct {
    bool present;
    bool has_target;
    sqlstmt_name_t target;
    bool do_update;
    sqlstmt_assignment_t *assignments;
    size_t assignment_count;
    sqlexpr_t *where;
} sqlstmt_conflict_t;

/* INSERT·UPDATE·DELETE의 RETURNING 목록. 없으면 count 0 */
typedef struct {
    sqlstmt_select_item_t *items;
    size_t count;
} sqlstmt_returning_t;

typedef struct {
    sqlstmt_name_t table;
    bool has_columns;
    sqlstmt_name_t *columns;     /* has_columns일 때 column_count개 */
    size_t column_count;
    sqlexpr_t **values;          /* row_count × value_count, 행 순서 */
    size_t value_count;          /* 한 행의 값 수. has_columns이면 column_count와 같다 */
    size_t row_count;            /* DEFAULT VALUES면 1행, 값 0개, has_columns이고 컬럼 0개 */
    sqlstmt_conflict_t conflict;
    sqlstmt_returning_t returning;
} sqlstmt_insert_t;

typedef enum { SQLSTMT_NULLS_DEFAULT, SQLSTMT_NULLS_FIRST, SQLSTMT_NULLS_LAST } sqlstmt_nulls_t;

typedef struct {
    sqlexpr_t *expr;
    bool desc;
    sqlstmt_nulls_t nulls;
} sqlstmt_order_t;

/* FROM 원본의 결합 방식. 첫 원본은 NONE, 쉼표와 CROSS JOIN은 CROSS다 */
typedef enum {
    SQLSTMT_JOIN_NONE,
    SQLSTMT_JOIN_CROSS,
    SQLSTMT_JOIN_INNER,
    SQLSTMT_JOIN_LEFT,
    SQLSTMT_JOIN_RIGHT   /* 둘째 원본에만(첫 결합). 그 뒤 자리는 UNSUPPORTED */
} sqlstmt_join_t;

typedef struct {
    sqlstmt_name_t table;        /* subquery면 size 0 */
    sqlstmt_name_t alias;        /* 없으면 size 0 */
    struct sqlstmt_select *subquery;  /* FROM ( query ) */
    sqlstmt_join_t join;
    sqlexpr_t *on;               /* INNER·LEFT·RIGHT의 ON 조건. 그 밖에는 NULL */
} sqlstmt_source_t;

#define SQLSTMT_MAX_SOURCES 64

/* 집합 연산. 왼쪽부터 결합한다. INTERSECT를 다른 연산과 섞으면 거절한다(엔진마다 우선순위가 다르다) */
typedef enum { SQLSTMT_SET_UNION, SQLSTMT_SET_UNION_ALL, SQLSTMT_SET_EXCEPT, SQLSTMT_SET_INTERSECT } sqlstmt_set_op_t;

/* WITH name [( column {, column} )] AS ( query ). columns가 없으면 질의의 결과 열 이름을 쓴다 */
typedef struct {
    sqlstmt_name_t name;
    sqlstmt_name_t *columns;
    size_t column_count;
    struct sqlstmt_select *query;
} sqlstmt_cte_t;

typedef struct sqlstmt_select {
    sqlstmt_cte_t *ctes;         /* WITH 목록(첫 SELECT에만). 뒤 이름은 앞 이름을 볼 수 있다 */
    size_t cte_count;
    bool distinct;
    sqlstmt_select_item_t *items;
    size_t item_count;
    sqlstmt_source_t *sources;   /* FROM 원본 source_count개. FROM이 없으면 0개 */
    size_t source_count;
    sqlexpr_t *where;            /* 없으면 NULL */
    sqlexpr_t **group_by;        /* GROUP BY 식 group_count개 */
    size_t group_count;
    sqlexpr_t *having;           /* 없으면 NULL */
    sqlstmt_order_t *order;
    size_t order_count;
    sqlexpr_t *limit;            /* 없으면 NULL */
    sqlexpr_t *offset;
    /* 집합 연산으로 이어지는 다음 SELECT. 있으면 ORDER BY·LIMIT·OFFSET은 첫 SELECT에 두며 결합한 결과에 쓴다 */
    struct sqlstmt_select *next;
    sqlstmt_set_op_t next_op;
} sqlstmt_select_t;

typedef struct {
    sqlstmt_name_t table;
    sqlstmt_assignment_t *assignments;
    size_t assignment_count;
    sqlexpr_t *where;
    sqlstmt_returning_t returning;
} sqlstmt_update_t;

typedef struct {
    sqlstmt_name_t table;
    sqlexpr_t *where;
    sqlstmt_returning_t returning;
} sqlstmt_delete_t;

typedef struct {
    sqlstmt_name_t table;
} sqlstmt_drop_t;

typedef struct {
    sqlstmt_kind_t kind;
    union {
        sqlstmt_create_t create;
        sqlstmt_index_t index;
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

/*
 * 메모리 회계용 상한. 이 statement를 sqlstmt_parse하는 동안 요청 바이트의 순간 최대치 상한이며 성공한
 * 결과가 sqlstmt_free까지 쥐는 몫도 이 값 이하다. 구조체와 항목 배열(두 배씩 늘어 old+new가 최대 3배),
 * 이름·문자열·BLOB의 복사본(토큰마다 최대 두 번), 숫자 변환 임시 buffer, 매개변수 값의 복제를 합한다.
 * 항목 수는 토큰 수와 CATALOG_MAX_COLUMNS로 제한된다. 요청 바이트 기준이며 allocator 오버헤드는 제외한다.
 */
size_t sqlstmt_parse_peak_bytes(const lexer_statement_t *statement, const lexer_binding_t *binding);

#endif
