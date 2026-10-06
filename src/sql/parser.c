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

    int i = 0;
    while (*p && *p != '=' && *p != '!' && *p != '<' && *p != '>'
           && !isspace((unsigned char)*p) && i < 31) {
        stmt->pred_field[i++] = *p++;
    }
    stmt->pred_field[i] = '\0';
    if (i == 0) return -1;
    p = skip_ws(p);

    if (strcasecmp_n(p, "BETWEEN", 7) == 0 && isspace((unsigned char)p[7])) {
        if (strcasecmp_n(stmt->pred_field, "id", 2) != 0
            || strlen(stmt->pred_field) != 2) return -1;
        p = skip_ws(p + 7);
        char lo_buf[64];
        int j = 0;
        while (*p && *p != ';' && !isspace((unsigned char)*p) && j < 63)
            lo_buf[j++] = *p++;
        lo_buf[j] = '\0';
        if (j == 0) return -1;
        p = skip_ws(p);
        if (strcasecmp_n(p, "AND", 3) != 0) return -1;
        p = skip_ws(p + 3);
        char hi_buf[64];
        j = 0;
        while (*p && *p != ';' && !isspace((unsigned char)*p) && j < 63)
            hi_buf[j++] = *p++;
        hi_buf[j] = '\0';
        if (j == 0) return -1;
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

    i = 0;
    if (*p == '\'') {
        if (parse_string(&p, stmt->pred_value, sizeof(stmt->pred_value)) != 0)
            return -1;
    } else {
        while (*p && *p != ';' && !isspace((unsigned char)*p) && i < 255)
            stmt->pred_value[i++] = *p++;
        stmt->pred_value[i] = '\0';
        if (i == 0) return -1;
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
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < 31)
            stmt->order_by_field[i++] = *p++;
        stmt->order_by_field[i] = '\0';
        if (i == 0) return -1;
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

/* ── CREATE TABLE ── */
static int parse_create_table(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    char tname[32] = {0};
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '(' && i < 31)
        tname[i++] = *p++;
    tname[i] = '\0';
    strncpy(stmt->table_name, tname, 31);

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
        i = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')' && i < 31)
            cd->name[i++] = *p++;
        cd->name[i] = '\0';
        p = skip_ws(p);

        char type_str[32] = {0};
        i = 0;
        while (*p && *p != ',' && *p != ')' && *p != '(' && !isspace((unsigned char)*p) && i < 31)
            type_str[i++] = *p++;
        type_str[i] = '\0';

        if (strcasecmp_n(type_str, "INT", 3) == 0 && strlen(type_str) == 3) {
            cd->type = COL_TYPE_INT; cd->size = 4;
        } else if (strcasecmp_n(type_str, "BIGINT", 6) == 0) {
            cd->type = COL_TYPE_BIGINT; cd->size = 8;
        } else if (strcasecmp_n(type_str, "VARCHAR", 7) == 0) {
            cd->type = COL_TYPE_VARCHAR; cd->size = 32;
            p = skip_ws(p);
            if (*p == '(') {
                p++;
                cd->size = (uint16_t)atoi(p);
                while (*p && *p != ')') p++;
                if (*p == ')') p++;
            }
        } else {
            return -1;
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

    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';

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
        i = 0;
        if (*p == '\'') {
            if (parse_string(&p, val, sizeof(stmt->insert_values[0])) != 0)
                return -1;
        } else {
            while (*p && *p != ',' && *p != ')' && *p != ';'
                   && !isspace((unsigned char)*p)
                   && i < 255)
                val[i++] = *p++;
            val[i] = '\0';
            if (i == 0) return -1;
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

    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';
    if (i == 0) return -1;

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

    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';

    const char *end;
    if (parse_where(p, stmt, &end) != 0) return -1;
    return at_end(end);
}

/* ── UPDATE ── */
static int parse_update(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);

    /* 테이블 이름 */
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';

    p = skip_ws(p);
    if (strcasecmp_n(p, "SET", 3) != 0) return -1;
    p = skip_ws(p + 3);

    /* SET col = val */
    i = 0;
    while (*p && *p != '=' && !isspace((unsigned char)*p) && i < 31)
        stmt->update_field[i++] = *p++;
    stmt->update_field[i] = '\0';

    p = skip_ws(p);
    if (*p != '=') return -1;
    p = skip_ws(p + 1);

    i = 0;
    if (*p == '\'') {
        if (parse_string(&p, stmt->update_value, sizeof(stmt->update_value)) != 0)
            return -1;
    } else {
        while (*p && *p != ';' && !isspace((unsigned char)*p) && i < 255)
            stmt->update_value[i++] = *p++;
        stmt->update_value[i] = '\0';
        if (i == 0) return -1;
    }
    const char *end;
    if (parse_where(p, stmt, &end) != 0) return -1;
    return at_end(end);
}

/* ── DROP TABLE ── */
static int parse_drop_table(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';
    if (i == 0) return -1;
    return at_end(p);
}

/* ── 메인 파서 (진입점) ── */
int parse(const char *input, statement_t *stmt)
{
    memset(stmt, 0, sizeof(*stmt));
    char buf[4096];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
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
        statement_t inner;
        if (parse(p, &inner) != 0) return -1;
        /* EXPLAIN EXPLAIN ... 은 의미가 없고, 중첩을 허용하면 계획 수립이
         * 자기 자신을 무한히 호출한다. */
        if (inner.type == STMT_EXPLAIN) return -1;

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
