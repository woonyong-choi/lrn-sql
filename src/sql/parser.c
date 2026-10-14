/*
 * parser.c -- 재귀 하강 SQL 파서
 *
 * 지원: CREATE TABLE, INSERT, SELECT, DELETE, UPDATE, DROP TABLE, EXPLAIN
 *       WHERE (=, !=, <, >, <=, >=), COUNT(*), ORDER BY, LIMIT
 */

#include "sql/parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

static void trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (isspace((unsigned char)s[len-1]) || s[len-1] == ';')) {
        s[--len] = '\0';
    }
}

static int strcasecmp_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d != 0) return d;
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int at_end(const char *p)
{
    return *skip_ws(p) == '\0' ? 0 : -1;
}

static int consume_word(const char **input, const char *word)
{
    const char *p = skip_ws(*input);
    size_t len = strlen(word);
    if (strlen(p) < len || strcasecmp_n(p, word, len) != 0
        || isalnum((unsigned char)p[len]) || p[len] == '_') return -1;
    *input = p + len;
    return 0;
}

/* 공백·NUL·stops 앞까지 읽어 out에 담는다. 길이를 반환하고, capacity를 넘으면 -1 */
static int take_token(const char **input, char *out, size_t capacity, const char *stops)
{
    const char *p = *input;
    size_t len = 0;
    while (*p && !isspace((unsigned char)*p) && strchr(stops, *p) == NULL) {
        if (len + 1 >= capacity) return -1;
        out[len++] = *p++;
    }
    out[len] = '\0';
    *input = p;
    return (int)len;
}

static int parse_string(const char **input, char *out, size_t capacity)
{
    const char *p = *input;
    size_t len = 0;
    if (*p++ != '\'') return -1;
    while (*p) {
        if (*p == '\'') {
            if (p[1] != '\'') {
                out[len] = '\0';
                *input = p + 1;
                return 0;
            }
            p++;
        }
        if (len + 1 >= capacity) return -1;
        out[len++] = *p++;
    }
    return -1;
}

static int parse_id_value(const char *input, uint64_t *value)
{
    if (!isdigit((unsigned char)*input)) return -1;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(input, &end, 10);
    if (*end != '\0' || errno == ERANGE || parsed > INT64_MAX) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

/* ── WHERE 절 파서 ── */
static int parse_where(const char *p, statement_t *stmt, const char **end)
{
    p = skip_ws(p);
    *end = p;
    if (*p == '\0') return 0;
    if (strcasecmp_n(p, "WHERE", 5) != 0) return 0;
    p = skip_ws(p + 5);

    int i = take_token(&p, stmt->pred_field, sizeof(stmt->pred_field), "=!<>");
    if (i <= 0) return -1;
    p = skip_ws(p);

    if (strcasecmp_n(p, "BETWEEN", 7) == 0 && isspace((unsigned char)p[7])) {
        if (strcasecmp_n(stmt->pred_field, "id", 2) != 0
            || strlen(stmt->pred_field) != 2) return -1;
        p = skip_ws(p + 7);
        char lo_buf[64];
        if (take_token(&p, lo_buf, sizeof(lo_buf), ";") <= 0) return -1;
        p = skip_ws(p);
        if (strcasecmp_n(p, "AND", 3) != 0) return -1;
        p = skip_ws(p + 3);
        char hi_buf[64];
        if (take_token(&p, hi_buf, sizeof(hi_buf), ";") <= 0) return -1;
        stmt->predicate_kind = PREDICATE_ID_RANGE;
        if (parse_id_value(lo_buf, &stmt->range_lo) != 0
            || parse_id_value(hi_buf, &stmt->range_hi) != 0) return -1;
        stmt->has_lo = true;
        stmt->has_hi = true;
        stmt->lo_inclusive = true;
        stmt->hi_inclusive = true;
        *end = p;
        return 0;
    }

    if (*p == '!' && p[1] == '=') {
        stmt->pred_op = OP_NE; p += 2;
    } else if (*p == '<' && p[1] == '=') {
        stmt->pred_op = OP_LE; p += 2;
    } else if (*p == '>' && p[1] == '=') {
        stmt->pred_op = OP_GE; p += 2;
    } else if (*p == '<') {
        stmt->pred_op = OP_LT; p++;
    } else if (*p == '>') {
        stmt->pred_op = OP_GT; p++;
    } else if (*p == '=') {
        stmt->pred_op = OP_EQ; p++;
    } else {
        return -1;
    }
    p = skip_ws(p);

    if (*p == '\'') {
        if (parse_string(&p, stmt->pred_value, sizeof(stmt->pred_value)) != 0)
            return -1;
    } else if (take_token(&p, stmt->pred_value, sizeof(stmt->pred_value), ";") <= 0) {
        return -1;
    }

    bool is_id = (strcasecmp_n(stmt->pred_field, "id", 2) == 0
                  && strlen(stmt->pred_field) == 2);
    if (is_id) {
        if (parse_id_value(stmt->pred_value, &stmt->pred_id) != 0) return -1;
        if (stmt->pred_op == OP_EQ) {
            stmt->predicate_kind = PREDICATE_ID_EQ;
        } else if (stmt->pred_op == OP_NE) {
            stmt->predicate_kind = PREDICATE_FIELD_CMP;
        } else {
            stmt->predicate_kind = PREDICATE_ID_RANGE;
            uint64_t v = stmt->pred_id;
            switch (stmt->pred_op) {
                case OP_GE: stmt->range_lo = v; stmt->has_lo = true; stmt->lo_inclusive = true;  break;
                case OP_GT: stmt->range_lo = v; stmt->has_lo = true; stmt->lo_inclusive = false; break;
                case OP_LE: stmt->range_hi = v; stmt->has_hi = true; stmt->hi_inclusive = true;  break;
                case OP_LT: stmt->range_hi = v; stmt->has_hi = true; stmt->hi_inclusive = false; break;
                default: break;
            }
        }
    } else if (stmt->pred_op == OP_EQ) {
        stmt->predicate_kind = PREDICATE_FIELD_EQ;
    } else {
        stmt->predicate_kind = PREDICATE_FIELD_CMP;
    }
    *end = p;
    return 0;
}

static int parse_order_limit(const char *p, statement_t *stmt)
{
    p = skip_ws(p);
    if (*p && strcasecmp_n(p, "ORDER", 5) == 0) {
        p = skip_ws(p + 5);
        if (strcasecmp_n(p, "BY", 2) != 0) return -1;
        p = skip_ws(p + 2);
        if (take_token(&p, stmt->order_by_field, sizeof(stmt->order_by_field), "") <= 0)
            return -1;
        stmt->has_order_by = true;
        p = skip_ws(p);
        if (*p && strcasecmp_n(p, "DESC", 4) == 0) {
            stmt->order_desc = true;
            p = skip_ws(p + 4);
        } else if (*p && strcasecmp_n(p, "ASC", 3) == 0) {
            p = skip_ws(p + 3);
        }
    }
    if (*p && strcasecmp_n(p, "LIMIT", 5) == 0) {
        p = skip_ws(p + 5);
        if (!isdigit((unsigned char)*p)) return -1;
        uint32_t count = 0;
        while (isdigit((unsigned char)*p)) {
            uint32_t digit = (uint32_t)(*p++ - '0');
            if (count > (UINT32_MAX - digit) / 10) return -1;
            count = count * 10 + digit;
        }
        stmt->limit_count = count;
        stmt->has_limit = true;
    }
    return at_end(p);
}

/* "(n)" 형태의 VARCHAR 크기. 숫자가 아니거나 0·uint16 범위 초과면 -1 */
static int parse_varchar_size(const char **input, uint16_t *size)
{
    const char *p = skip_ws(*input + 1);
    unsigned long value = 0;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) {
        value = value * 10 + (unsigned long)(*p++ - '0');
        if (value > UINT16_MAX) return -1;
    }
    p = skip_ws(p);
    if (*p != ')' || value == 0) return -1;
    *size = (uint16_t)value;
    *input = p + 1;
    return 0;
}

/* ── CREATE TABLE ── */
static int parse_create_table(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    if (take_token(&p, stmt->table_name, sizeof(stmt->table_name), "(") < 0) return -1;

    p = skip_ws(p);
    if (*p != '(') return -1;
    p++;

    stmt->col_count = 0;
    while (*p && *p != ')') {
        if (stmt->col_count >= MAX_COLUMNS - 1) return -1;
        p = skip_ws(p);
        if (*p == ')') break;

        column_def_t *cd = &stmt->col_defs[stmt->col_count];
        memset(cd, 0, sizeof(*cd));
        if (take_token(&p, cd->name, sizeof(cd->name), ",)") < 0) return -1;
        p = skip_ws(p);

        char type_str[32] = {0};
        if (take_token(&p, type_str, sizeof(type_str), ",()") < 0) return -1;

        if (strcasecmp_n(type_str, "INT", 3) == 0 && strlen(type_str) == 3) {
            cd->type = COL_TYPE_INT; cd->size = 4;
        } else if (strcasecmp_n(type_str, "BIGINT", 6) == 0 && strlen(type_str) == 6) {
            cd->type = COL_TYPE_BIGINT; cd->size = 8;
        } else if (strcasecmp_n(type_str, "VARCHAR", 7) == 0 && strlen(type_str) == 7) {
            cd->type = COL_TYPE_VARCHAR; cd->size = 32;
            p = skip_ws(p);
            if (*p == '(' && parse_varchar_size(&p, &cd->size) != 0) return -1;
        } else {
            return -1;
        }

        p = skip_ws(p);
        if (strcmp(cd->name, "id") == 0 && *p != ',' && *p != ')') {
            if (stmt->col_count != 0 || cd->type != COL_TYPE_BIGINT
                || consume_word(&p, "GENERATED") != 0
                || consume_word(&p, "ALWAYS") != 0
                || consume_word(&p, "AS") != 0
                || consume_word(&p, "IDENTITY") != 0
                || consume_word(&p, "PRIMARY") != 0
                || consume_word(&p, "KEY") != 0) return -1;
        }

        stmt->col_count++;
        p = skip_ws(p);
        if (*p == ',') p++;
        else if (*p != ')') return -1;
    }
    if (*p != ')') return -1;
    return at_end(p + 1);
}

/* ── INSERT ── */
static int parse_insert(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    if (strcasecmp_n(p, "INTO", 4) != 0) return -1;
    p = skip_ws(p + 4);

    if (take_token(&p, stmt->table_name, sizeof(stmt->table_name), "(") <= 0) return -1;

    p = skip_ws(p);
    if (*p == '(') {
        stmt->has_insert_columns = true;
        p++;
        while (*p) {
            p = skip_ws(p);
            if (stmt->insert_column_count >= MAX_COLUMNS - 1) return -1;
            char *column = stmt->insert_columns[stmt->insert_column_count];
            int i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                if (i + 1 >= (int)sizeof(stmt->insert_columns[0])) return -1;
                column[i++] = *p++;
            }
            column[i] = '\0';
            if (i == 0) return -1;
            stmt->insert_column_count++;
            p = skip_ws(p);
            if (*p == ')') {
                p++;
                break;
            }
            if (*p != ',') return -1;
            p++;
        }
        if (stmt->insert_column_count == 0) return -1;
    }

    p = skip_ws(p);
    if (strcasecmp_n(p, "VALUES", 6) != 0) return -1;
    p = skip_ws(p + 6);
    if (*p != '(') return -1;
    p++;

    stmt->insert_value_count = 0;
    while (*p && *p != ')') {
        p = skip_ws(p);
        if (*p == ')') break;
        if (stmt->insert_value_count >= MAX_COLUMNS - 1) return -1;
        char *val = stmt->insert_values[stmt->insert_value_count];
        if (*p == '\'') {
            if (parse_string(&p, val, sizeof(stmt->insert_values[0])) != 0)
                return -1;
        } else if (take_token(&p, val, sizeof(stmt->insert_values[0]), ",);") <= 0) {
            return -1;
        }
        stmt->insert_value_count++;
        p = skip_ws(p);
        if (*p == ',') p++;
        else if (*p != ')') return -1;
    }
    if (*p != ')') return -1;
    return at_end(p + 1);
}

/* ── SELECT ── */
static int parse_select(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    if (strcasecmp_n(p, "COUNT", 5) == 0) {
        p = skip_ws(p + 5);
        if (*p++ != '(') return -1;
        p = skip_ws(p);
        if (*p++ != '*') return -1;
        p = skip_ws(p);
        if (*p++ != ')') return -1;
        stmt->select_count = true;
    } else if (*p == '*') {
        stmt->select_all = true;
        p++;
    } else {
        return -1;
    }
    p = skip_ws(p);
    if (strcasecmp_n(p, "FROM", 4) != 0) return -1;
    p = skip_ws(p + 4);

    if (take_token(&p, stmt->table_name, sizeof(stmt->table_name), "") <= 0) return -1;

    const char *after_where;
    if (parse_where(p, stmt, &after_where) != 0) return -1;
    return parse_order_limit(after_where, stmt);
}

/* ── DELETE ── */
static int parse_delete(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    if (strcasecmp_n(p, "FROM", 4) != 0) return -1;
    p = skip_ws(p + 4);

    if (take_token(&p, stmt->table_name, sizeof(stmt->table_name), ";") < 0) return -1;

    const char *end;
    if (parse_where(p, stmt, &end) != 0) return -1;
    return at_end(end);
}

/* ── UPDATE ── */
static int parse_update(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);

    /* 테이블 이름 */
    if (take_token(&p, stmt->table_name, sizeof(stmt->table_name), "") < 0) return -1;

    p = skip_ws(p);
    if (strcasecmp_n(p, "SET", 3) != 0) return -1;
    p = skip_ws(p + 3);

    /* SET col = val */
    if (take_token(&p, stmt->update_field, sizeof(stmt->update_field), "=") < 0) return -1;

    p = skip_ws(p);
    if (*p != '=') return -1;
    p = skip_ws(p + 1);

    if (*p == '\'') {
        if (parse_string(&p, stmt->update_value, sizeof(stmt->update_value)) != 0)
            return -1;
    } else if (take_token(&p, stmt->update_value, sizeof(stmt->update_value), ";") <= 0) {
        return -1;
    }
    const char *end;
    if (parse_where(p, stmt, &end) != 0) return -1;
    return at_end(end);
}

/* ── DROP TABLE ── */
static int parse_drop_table(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    if (take_token(&p, stmt->table_name, sizeof(stmt->table_name), ";") <= 0) return -1;
    return at_end(p);
}

/* ── 메인 파서 (진입점) ── */
int parse(const char *input, statement_t *stmt)
{
    memset(stmt, 0, sizeof(*stmt));
    char buf[PARSER_MAX_INPUT_LEN + 1];
    if (strnlen(input, sizeof(buf)) >= sizeof(buf)) return -1;
    memcpy(buf, input, strlen(input) + 1);
    trim(buf);

    const char *p = skip_ws(buf);
    if (*p == '\0') return -1;

    if (strcasecmp_n(p, "CREATE", 6) == 0) {
        p = skip_ws(p + 6);
        if (strcasecmp_n(p, "TABLE", 5) != 0) return -1;
        stmt->type = STMT_CREATE_TABLE;
        return parse_create_table(skip_ws(p + 5), stmt);
    }
    if (strcasecmp_n(p, "INSERT", 6) == 0) {
        stmt->type = STMT_INSERT;
        return parse_insert(skip_ws(p + 6), stmt);
    }
    if (strcasecmp_n(p, "SELECT", 6) == 0) {
        stmt->type = STMT_SELECT;
        return parse_select(skip_ws(p + 6), stmt);
    }
    if (strcasecmp_n(p, "DELETE", 6) == 0) {
        stmt->type = STMT_DELETE;
        return parse_delete(skip_ws(p + 6), stmt);
    }
    if (strcasecmp_n(p, "UPDATE", 6) == 0) {
        stmt->type = STMT_UPDATE;
        return parse_update(skip_ws(p + 6), stmt);
    }
    if (strcasecmp_n(p, "DROP", 4) == 0) {
        p = skip_ws(p + 4);
        if (strcasecmp_n(p, "TABLE", 5) != 0) return -1;
        stmt->type = STMT_DROP_TABLE;
        return parse_drop_table(skip_ws(p + 5), stmt);
    }
    if (strcasecmp_n(p, "EXPLAIN", 7) == 0) {
        p = skip_ws(p + 7);
        /* EXPLAIN EXPLAIN ... 은 의미가 없고, 중첩을 허용하면 계획 수립이
         * 자기 자신을 무한히 호출한다. 재귀 전에 막아 스택을 쓰지 않는다. */
        if (strcasecmp_n(p, "EXPLAIN", 7) == 0) return -1;
        statement_t inner;
        if (parse(p, &inner) != 0) return -1;

        /*
         * 안쪽 문장을 통째로 들고 간다.
         *
         * 예전에는 계획에 필요할 것 같은 필드만 골라 복사했는데, 그때
         * select_count 와 has_order_by 가 빠져 있었다. 그 둘은 접근 경로를
         * 바꾸는 조건이라, EXPLAIN 이 실행기와 다른 계획을 보고했다.
         * 필드가 늘어날 때마다 같은 실수가 반복되므로 선별 복사를 버린다.
         */
        *stmt = inner;
        stmt->type = STMT_EXPLAIN;
        stmt->inner_type = inner.type;
        stmt->inner_predicate = inner.predicate_kind;
        return 0;
    }

    return -1;
}
