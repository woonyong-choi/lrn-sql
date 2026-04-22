/*
 * statement.h -- SQL 문 파싱 결과 구조체
 */

#ifndef STATEMENT_H
#define STATEMENT_H

#include "../storage/page_format.h"
#include "../storage/schema.h"
#include <stdint.h>
#include <stdbool.h>

/* SQL 문 유형 */
typedef enum {
    STMT_CREATE_TABLE,
    STMT_INSERT,
    STMT_SELECT,
    STMT_DELETE,
    STMT_UPDATE,
    STMT_DROP_TABLE,
    STMT_EXPLAIN
} statement_type_t;

/* WHERE 절 조건 종류 */
typedef enum {
    PREDICATE_NONE,
    PREDICATE_ID_EQ,
    PREDICATE_FIELD_EQ,
    PREDICATE_FIELD_CMP   /* >, <, >=, <=, != */
} predicate_kind_t;

/* 비교 연��자 */
typedef enum {
    OP_EQ,    /* = */
    OP_NE,    /* != */
    OP_LT,    /* < */
    OP_GT,    /* > */
    OP_LE,    /* <= */
    OP_GE     /* >= */
} compare_op_t;

/* CREATE TABLE에서 파싱된 컬럼 정의 */
typedef struct {
    char     name[32];
    uint8_t  type;
    uint16_t size;
} column_def_t;

typedef struct {
    statement_type_t  type;
    predicate_kind_t  predicate_kind;
    compare_op_t      pred_op;              /* 비교 연산자 */
    char              table_name[32];

    /* INSERT */
    char              insert_values[MAX_COLUMNS][256];
    uint16_t          insert_value_count;

    /* UPDATE: SET col = val */
    char              update_field[32];
    char              update_value[256];

    /* WHERE */
    char              pred_field[32];
    char              pred_value[256];
    uint64_t          pred_id;

    /* CREATE TABLE */
    column_def_t      col_defs[MAX_COLUMNS];
    uint16_t          col_count;

    /* SELECT */
    bool              select_all;
    bool              select_count;         /* SELECT COUNT(*) */

    /* ORDER BY */
    char              order_by_field[32];
    bool              order_desc;           /* true=DESC, false=ASC */
    bool              has_order_by;

    /* LIMIT */
    uint32_t          limit_count;
    bool              has_limit;

    /* EXPLAIN */
    statement_type_t  inner_type;
    predicate_kind_t  inner_predicate;
} statement_t;

#endif /* STATEMENT_H */
