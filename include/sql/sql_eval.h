#ifndef SQL_EVAL_H
#define SQL_EVAL_H

#include <stdbool.h>
#include <stddef.h>

#include "sql/sql_statement.h"
#include "storage/value.h"

/*
 * 식 평가기. sqlexpr_t 트리를 열 번호·함수가 결정된 프로그램으로 컴파일하고 한 행에 대해 계산한다.
 * 컴파일은 PostgreSQL처럼 정적 형식을 정한다. 결과 열 형식(sqleval_type)은 모든 행에서 같다.
 *
 * 의미(SQLite·PostgreSQL이 같게 답하는 부분을 따르고, 둘이 갈리면 숨기지 않고 오류로 거절한다):
 *   - NULL은 3치 논리를 따른다. 비교·산술·연결·LIKE·함수는 인자 중 NULL이 있으면 NULL이다(COALESCE·NULLIF·
 *     IS NULL·AND·OR·CASE·IN 제외).
 *   - 참·거짓은 INTEGER 1·0이다. 조건 자리에는 INTEGER(0이 아니면 참)나 NULL만 온다. TEXT 조건은 형식 오류.
 *   - 산술: INTEGER끼리는 INTEGER(넘침은 OVERFLOW), REAL이 섞이면 REAL. / 의 정수 나눗셈은 0 쪽으로 자른다.
 *     0으로 나누기와 % 는 DIVIDE_BY_ZERO다. % 는 INTEGER만. TEXT·BLOB 산술은 형식 오류.
 *   - 비교: 숫자끼리(INTEGER·REAL 섞어도 정확히), TEXT끼리(바이트 순서), BLOB끼리. 그 밖의 조합은 형식 오류.
 *   - || 는 TEXT·INTEGER를 받아 TEXT를 만든다(INTEGER는 10진 표기). REAL·BLOB은 형식 오류.
 *   - LIKE는 대소문자를 구분한다(%·_, ESCAPE 한 글자). SQLite 기본(ASCII 대소문자 무시)과 다르다.
 *   - CAST는 value_cast의 정확한 변환만 받는다(잃는 변환은 형식 오류). 문자열을 숫자로 바꿀 때만 앞뒤 ASCII 공백을
 *     무시하고, 정수를 실수로 바꿀 때는 가장 가까운 double로 반올림한다(SQLite·PostgreSQL과 같음). 저장할 때의 변환은
 *     공백과 정밀도 손실을 거절한다.
 *   - 최상위 AND로 나뉜 WHERE 조건은 앞에서부터 보고 참이 아닌 조건을 만나면 뒤 조건을 평가하지 않는다. 그래서 뒤 조건의
 *     평가 오류(0으로 나누기 등)는 앞 조건이 참인 행에서만 난다. SELECT 목록의 AND는 NULL AND x에서 x를 평가한다.
 *     평가 순서는 SQL 표준·PostgreSQL도 약속하지 않으므로 오류가 날 수 있는 조건은 CASE로 막아야 한다.
 *   - 집계 COUNT·SUM·AVG·MIN·MAX는 이 모듈이 계산하지 않는다. 실행기가 그룹마다 접어 match로 넘긴다.
 *   - 서브쿼리 결과는 subquery 훅이 준다. 바깥 열을 보는 서브쿼리는 평가할 때마다 refresh로 다시 계산한다.
 *   - 함수: ABS, LENGTH(글자 수, BLOB은 바이트 수), LOWER, UPPER(ASCII), SUBSTR(x, 시작[, 길이])(글자 단위,
 *     시작이 1보다 작으면 그만큼 길이를 줄인다), TRIM·LTRIM·RTRIM(x[, 문자들]), REPLACE, COALESCE, NULLIF.
 */

typedef enum {
    SQLEVAL_OK,
    SQLEVAL_NOMEM,
    SQLEVAL_TYPE,             /* 형식이 맞지 않음 */
    SQLEVAL_DIVIDE_BY_ZERO,
    SQLEVAL_OVERFLOW,
    SQLEVAL_UNKNOWN_COLUMN,
    SQLEVAL_AMBIGUOUS_COLUMN,
    SQLEVAL_UNKNOWN_FUNCTION,
    SQLEVAL_ARGUMENT,         /* 함수 인자 수·인자 값 */
    SQLEVAL_NOT_CONSTANT,     /* 상수 자리에 열이 나옴 */
    SQLEVAL_AGGREGATE,        /* 집계 함수를 쓸 수 없는 자리(WHERE·GROUP BY·집계 안의 집계 등) */
    SQLEVAL_NOT_GROUPED,      /* 그룹 질의에서 GROUP BY에 없는 열을 집계 밖에서 씀 */
    SQLEVAL_SUBQUERY,         /* 서브쿼리 실행이 실패함(원인은 subquery 훅을 준 쪽이 기록했다) */
    SQLEVAL_SUBQUERY_SHAPE,   /* 식 자리 서브쿼리가 열 하나가 아니거나 스칼라 서브쿼리가 두 행 이상 */
    SQLEVAL_LIMIT             /* 값 크기 한도 */
} sqleval_status_t;

typedef struct {
    sqleval_status_t status;
    size_t offset;            /* 원인 식의 입력 위치 */
    const sqlstmt_name_t *name;  /* 열·함수 이름 오류일 때 그 이름(문장이 소유) */
} sqleval_error_t;

/* 열 이름을 번호로 바꾼다. 없으면 SQLEVAL_NO_COLUMN, 둘 이상과 맞으면 SQLEVAL_AMBIGUOUS. *type은 열 형식 */
#define SQLEVAL_NO_COLUMN ((size_t)-1)
#define SQLEVAL_AMBIGUOUS ((size_t)-2)
#define SQLEVAL_UNGROUPED ((size_t)-3)
/*
 * 부분식 치환. 컴파일은 각 노드에서 먼저 match를 부르고, 번호를 돌려주면 그 부분식 전체를 row[번호]로 읽는다
 * (그룹 질의에서 GROUP BY 식과 집계 호출을 그룹 행의 칸으로 바꿀 때 쓴다). 맞지 않으면 SQLEVAL_NO_COLUMN.
 */
typedef size_t (*sqleval_match_fn)(void *ctx, const sqlexpr_t *expr, value_type_t *type);
typedef size_t (*sqleval_resolve_fn)(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name,
                                     value_type_t *type);

/*
 * 서브쿼리 결과(바깥 행에 기대지 않는 서브쿼리만). 훅을 준 쪽이 만들어 프로그램보다 오래 쥔다.
 *   (query): 행이 없으면 NULL, 한 행이면 그 값(first). 두 행 이상이면 SUBQUERY_SHAPE.
 *   EXISTS (query): row_count > 0.
 *   x IN (query): set은 NULL 아닌 값을 sqleval_compare 순서로 정렬·중복 제거한 것. 3치 논리는 IN 목록과 같다
 *     (맞으면 참, 아니고 NULL이 있으면 NULL, 아니면 거짓. 결과가 비었으면 x가 NULL이어도 거짓).
 */
typedef struct {
    value_type_t type;            /* 첫 열의 형식 */
    size_t column_count;
    size_t row_count;
    const value_view_t *first;    /* 첫 행 첫 열. row_count가 0이면 NULL */
    const value_view_t *set;      /* IN일 때만 */
    size_t set_count;
    bool set_has_null;
    /*
     * 바깥 열을 보는 서브쿼리면 refresh가 NULL이 아니다. 평가기는 값을 읽기 전에 refresh(refresh_ctx, 바깥 행)를
     * 불러 위 필드를 그 행에 맞게 다시 채우게 한다. outer_columns는 바깥 행에서 읽는 칸이다(열 표시용).
     */
    sqleval_status_t (*refresh)(void *ctx, const value_t *row);
    void *refresh_ctx;
    const size_t *outer_columns;
    size_t outer_count;
} sqleval_subquery_t;

/* expr(SUBQUERY·EXISTS·IN ( query ))의 결과를 준다. 실패하면 SQLEVAL_SUBQUERY나 SQLEVAL_NOMEM */
typedef sqleval_status_t (*sqleval_subquery_fn)(void *ctx, const sqlexpr_t *expr, const sqleval_subquery_t **out);

/*
 * 바깥 질의의 열. resolve가 찾지 못한 이름을 바깥 범위에서 찾아 그 값이 놓일 칸을 준다(칸은 프로그램보다 오래
 * 산다). 찾으면 true다.
 */
typedef bool (*sqleval_outer_fn)(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name,
                                 value_type_t *type, const value_t **cell);

/* 컴파일 환경. resolve·match는 ctx를, subquery·outer는 subquery_ctx를 받는다. 없는 훅은 NULL */
typedef struct {
    sqleval_resolve_fn resolve;   /* NULL이면 열을 쓸 수 없다(상수식) */
    sqleval_match_fn match;
    void *ctx;
    sqleval_subquery_fn subquery; /* NULL이면 서브쿼리를 쓸 수 없다 */
    void *subquery_ctx;
    sqleval_outer_fn outer;       /* NULL이면 바깥 열을 볼 수 없다 */
} sqleval_env_t;

typedef struct sqleval_prog sqleval_prog_t;

/*
 * expr를 컴파일한다. 성공하면 *out을 sqleval_free로 해제한다.
 * 프로그램은 expr와 서브쿼리 결과를 빌려 쓰므로 그것들보다 오래 살면 안 된다.
 */
sqleval_status_t sqleval_compile(const sqlexpr_t *expr, const sqleval_env_t *env, sqleval_prog_t **out,
                                 sqleval_error_t *error);

/* COUNT·SUM·AVG·MIN·MAX(인자 하나) 호출인지 */
bool sqleval_is_aggregate(const sqlexpr_t *expr);
void sqleval_free(sqleval_prog_t *prog);

/* 정적 결과 형식. 모든 인자가 NULL 리터럴이면 VALUE_NULL이다 */
value_type_t sqleval_type(const sqleval_prog_t *prog);
/* 쓰는 열을 needed[0..column_count)에 표시한다 */
void sqleval_mark_columns(const sqleval_prog_t *prog, bool *needed, size_t column_count);

/*
 * row[i]가 열 i의 값인 행에 대해 계산한다. 결과는 *out에 담기며 호출자가 value_clear한다(실패하면 *out은 NULL).
 * TEXT·BLOB 결과 크기가 max_bytes를 넘으면 LIMIT이다.
 */
sqleval_status_t sqleval_run(const sqleval_prog_t *prog, const value_t *row, value_t *out, sqleval_error_t *error);
/* 조건 판정. 참이면 *truth가 true, 거짓·NULL이면 false */
sqleval_status_t sqleval_test(const sqleval_prog_t *prog, const value_t *row, bool *truth, sqleval_error_t *error);

/* 정렬·DISTINCT용 비교. NULL은 가장 작다. 형식이 다른 숫자도 값으로 비교한다 */
int sqleval_compare(const value_view_t *a, const value_view_t *b);

#endif
