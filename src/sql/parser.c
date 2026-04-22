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

/* ── WHERE 절 파서 (비교 연산자 확장) ── */
static int parse_where(const char *p, statement_t *stmt)
{
    p = skip_ws(p);
    if (*p == '\0' || *p == ';') {
        stmt->predicate_kind = PREDICATE_NONE;
        return 0;
    }

    /* WHERE 외 키워드는 무시 (ORDER BY, LIMIT 등) */
    if (strcasecmp_n(p, "WHERE", 5) != 0) {
        stmt->predicate_kind = PREDICATE_NONE;
        return 0;
    }
    p = skip_ws(p + 5);

    /* 필드 이름 */
    int i = 0;
    while (*p && *p != '=' && *p != '!' && *p != '<' && *p != '>'
           && !isspace((unsigned char)*p) && i < 31) {
        stmt->pred_field[i++] = *p++;
    }
    stmt->pred_field[i] = '\0';

    p = skip_ws(p);

    /* 연산자 파싱 */
    if (*p == '!' && *(p+1) == '=') {
        stmt->pred_op = OP_NE; p += 2;
    } else if (*p == '<' && *(p+1) == '=') {
        stmt->pred_op = OP_LE; p += 2;
    } else if (*p == '>' && *(p+1) == '=') {
        stmt->pred_op = OP_GE; p += 2;
    } else if (*p == '<') {
        stmt->pred_op = OP_LT; p += 1;
    } else if (*p == '>') {
        stmt->pred_op = OP_GT; p += 1;
    } else if (*p == '=') {
        stmt->pred_op = OP_EQ; p += 1;
    } else {
        return -1;
    }

    p = skip_ws(p);

    /* 값 추출 */
    i = 0;
    if (*p == '\'') {
        p++;
        while (*p && *p != '\'' && i < 255) {
            stmt->pred_value[i++] = *p++;
        }
        if (*p == '\'') p++;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 255) {
            stmt->pred_value[i++] = *p++;
        }
    }
    stmt->pred_value[i] = '\0';

    /* 조건 분류 */
    if (strcasecmp_n(stmt->pred_field, "id", 2) == 0
        && strlen(stmt->pred_field) == 2 && stmt->pred_op == OP_EQ) {
        stmt->predicate_kind = PREDICATE_ID_EQ;
        stmt->pred_id = (uint64_t)atoll(stmt->pred_value);
    } else if (stmt->pred_op == OP_EQ) {
        stmt->predicate_kind = PREDICATE_FIELD_EQ;
    } else {
        stmt->predicate_kind = PREDICATE_FIELD_CMP;
    }
    return 0;
}

/* WHERE 이후의 나머지를 파싱 (ORDER BY, LIMIT) */
static const char *find_after_where(const char *p)
{
    p = skip_ws(p);
    if (strcasecmp_n(p, "WHERE", 5) != 0) return p;
    p += 5;
    /* WHERE 절 끝: ORDER, LIMIT, 또는 문자열 끝 */
    while (*p) {
        const char *q = skip_ws(p);
        if (strcasecmp_n(q, "ORDER", 5) == 0 || strcasecmp_n(q, "LIMIT", 5) == 0)
            return q;
        p++;
    }
    return p;
}

static int parse_order_limit(const char *p, statement_t *stmt)
{
    p = skip_ws(p);

    /* ORDER BY */
    if (*p && strcasecmp_n(p, "ORDER", 5) == 0) {
        p = skip_ws(p + 5);
        if (strcasecmp_n(p, "BY", 2) != 0) return -1;
        p = skip_ws(p + 2);

        int i = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 31) {
            stmt->order_by_field[i++] = *p++;
        }
        stmt->order_by_field[i] = '\0';
        stmt->has_order_by = true;
        stmt->order_desc = false;

        p = skip_ws(p);
        if (*p && strcasecmp_n(p, "DESC", 4) == 0) {
            stmt->order_desc = true;
            p = skip_ws(p + 4);
        } else if (*p && strcasecmp_n(p, "ASC", 3) == 0) {
            p = skip_ws(p + 3);
        }
    }

    p = skip_ws(p);

    /* LIMIT */
    if (*p && strcasecmp_n(p, "LIMIT", 5) == 0) {
        p = skip_ws(p + 5);
        stmt->limit_count = (uint32_t)atoi(p);
        stmt->has_limit = true;
    }

    return 0;
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
    }
    return 0;
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

        char *val = stmt->insert_values[stmt->insert_value_count];
        i = 0;
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'' && i < 255) val[i++] = *p++;
            if (*p == '\'') p++;
        } else {
            while (*p && *p != ',' && *p != ')' && !isspace((unsigned char)*p) && i < 255)
                val[i++] = *p++;
        }
        val[i] = '\0';
        stmt->insert_value_count++;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    return 0;
}

/* ── SELECT ── */
static int parse_select(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);

    /* COUNT(*) */
    if (strcasecmp_n(p, "COUNT", 5) == 0) {
        p = skip_ws(p + 5);
        if (*p == '(') {
            p++;
            p = skip_ws(p);
            if (*p == '*') p++;
            p = skip_ws(p);
            if (*p == ')') p++;
        }
        stmt->select_count = true;
        p = skip_ws(p);
        if (strcasecmp_n(p, "FROM", 4) != 0) return -1;
        p = skip_ws(p + 4);

        int i = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 31)
            stmt->table_name[i++] = *p++;
        stmt->table_name[i] = '\0';

        /* WHERE + ORDER BY + LIMIT */
        const char *rest = skip_ws(p);
        const char *after_where = find_after_where(rest);
        if (parse_where(rest, stmt) != 0) return -1;
        return parse_order_limit(after_where, stmt);
    }

    /* SELECT * */
    if (*p == '*') {
        stmt->select_all = true;
        p = skip_ws(p + 1);
    }
    if (strcasecmp_n(p, "FROM", 4) != 0) return -1;
    p = skip_ws(p + 4);

    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';

    const char *rest = skip_ws(p);
    const char *after_where = find_after_where(rest);
    if (parse_where(rest, stmt) != 0) return -1;
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

    return parse_where(skip_ws(p), stmt);
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
        p++;
        while (*p && *p != '\'' && i < 255)
            stmt->update_value[i++] = *p++;
        if (*p == '\'') p++;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 255)
            stmt->update_value[i++] = *p++;
    }
    stmt->update_value[i] = '\0';

    return parse_where(skip_ws(p), stmt);
}

/* ── DROP TABLE ── */
static int parse_drop_table(const char *input, statement_t *stmt)
{
    const char *p = skip_ws(input);
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ';' && i < 31)
        stmt->table_name[i++] = *p++;
    stmt->table_name[i] = '\0';
    return 0;
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
        stmt->type = STMT_EXPLAIN;
        p = skip_ws(p + 7);
        statement_t inner;
        if (parse(p, &inner) != 0) return -1;
        stmt->inner_type = inner.type;
        stmt->inner_predicate = inner.predicate_kind;
        memcpy(stmt->table_name, inner.table_name, 32);
        memcpy(stmt->pred_field, inner.pred_field, 32);
        memcpy(stmt->pred_value, inner.pred_value, 256);
        stmt->pred_id = inner.pred_id;
        stmt->predicate_kind = inner.predicate_kind;
        return 0;
    }

    return -1;
}
