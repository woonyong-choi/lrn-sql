#include "sql/sql_eval.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FN_NONE,
    FN_ABS,
    FN_LENGTH,
    FN_LOWER,
    FN_UPPER,
    FN_SUBSTR,
    FN_TRIM,
    FN_LTRIM,
    FN_RTRIM,
    FN_REPLACE,
    FN_COALESCE,
    FN_NULLIF
} fn_t;

typedef struct {
    const char *name;
    fn_t fn;
    size_t min_args;
    size_t max_args;
} fn_spec_t;

static const fn_spec_t FUNCTIONS[] = {
    {"ABS", FN_ABS, 1, 1},           {"LENGTH", FN_LENGTH, 1, 1},     {"LOWER", FN_LOWER, 1, 1},
    {"UPPER", FN_UPPER, 1, 1},       {"SUBSTR", FN_SUBSTR, 2, 3},     {"SUBSTRING", FN_SUBSTR, 2, 3},
    {"TRIM", FN_TRIM, 1, 1},         {"LTRIM", FN_LTRIM, 1, 2},       {"RTRIM", FN_RTRIM, 1, 2},
    {"REPLACE", FN_REPLACE, 3, 3},   {"COALESCE", FN_COALESCE, 1, SIZE_MAX},
    {"NULLIF", FN_NULLIF, 2, 2},
};

typedef struct node {
    const sqlexpr_t *src;
    value_type_t type;
    size_t column;
    fn_t fn;
    struct node **args;
} node_t;

struct sqleval_prog {
    node_t *root;
    node_t *nodes;
    node_t **slots;
    size_t node_count;
};

/* ── 컴파일 ── */

typedef struct {
    sqleval_resolve_fn resolve;
    void *ctx;
    node_t *nodes;
    size_t used;
    node_t **slots;
    size_t slots_used;
    sqleval_error_t *error;
} compiler_t;

static sqleval_status_t failed(sqleval_error_t *error, sqleval_status_t status, const sqlexpr_t *at,
                               const sqlstmt_name_t *name)
{
    if (error != NULL) {
        error->status = status;
        error->offset = at != NULL ? at->offset : 0;
        error->name = name;
    }
    return status;
}

static void count_nodes(const sqlexpr_t *expr, size_t *nodes, size_t *slots)
{
    (*nodes)++;
    *slots += expr->arg_count;
    for (size_t i = 0; i < expr->arg_count; i++) count_nodes(expr->args[i], nodes, slots);
}

static bool is_numeric(value_type_t type) { return type == VALUE_NULL || type == VALUE_INTEGER || type == VALUE_REAL; }
static bool is_boolish(value_type_t type) { return type == VALUE_NULL || type == VALUE_INTEGER; }
static bool is_textish(value_type_t type) { return type == VALUE_NULL || type == VALUE_TEXT; }

/* 두 형식을 하나로 합친다(CASE·COALESCE의 결과). NULL은 다른 쪽을 따른다 */
static bool unify(value_type_t a, value_type_t b, value_type_t *out)
{
    if (a == VALUE_NULL) *out = b;
    else if (b == VALUE_NULL || a == b) *out = a;
    else if (is_numeric(a) && is_numeric(b)) *out = VALUE_REAL;
    else return false;
    return true;
}

static bool comparable(value_type_t a, value_type_t b)
{
    if (a == VALUE_NULL || b == VALUE_NULL) return true;
    if (is_numeric(a) && is_numeric(b)) return true;
    return a == b;
}

static bool name_is(const sqlstmt_name_t *name, const char *text)
{
    size_t length = strlen(text);
    return name->size == length && memcmp(name->bytes, text, length) == 0;
}

static sqleval_status_t compile_node(compiler_t *c, const sqlexpr_t *expr, node_t **out);

static sqleval_status_t compile_function(compiler_t *c, node_t *node)
{
    const sqlexpr_t *expr = node->src;
    const fn_spec_t *spec = NULL;
    for (size_t i = 0; i < sizeof(FUNCTIONS) / sizeof(FUNCTIONS[0]); i++) {
        if (name_is(&expr->name, FUNCTIONS[i].name)) spec = &FUNCTIONS[i];
    }
    if (spec == NULL) return failed(c->error, SQLEVAL_UNKNOWN_FUNCTION, expr, &expr->name);
    if (expr->star || expr->distinct || expr->arg_count < spec->min_args || expr->arg_count > spec->max_args) {
        return failed(c->error, SQLEVAL_ARGUMENT, expr, &expr->name);
    }
    node->fn = spec->fn;
    value_type_t a = node->args[0]->type;
    switch (spec->fn) {
        case FN_ABS:
            if (!is_numeric(a)) return failed(c->error, SQLEVAL_TYPE, expr, &expr->name);
            node->type = a;
            return SQLEVAL_OK;
        case FN_LENGTH:
            if (a != VALUE_NULL && a != VALUE_TEXT && a != VALUE_BLOB) return failed(c->error, SQLEVAL_TYPE, expr, &expr->name);
            node->type = VALUE_INTEGER;
            return SQLEVAL_OK;
        case FN_SUBSTR:
            if (!is_textish(a) || !is_boolish(node->args[1]->type) ||
                (expr->arg_count == 3 && !is_boolish(node->args[2]->type))) {
                return failed(c->error, SQLEVAL_TYPE, expr, &expr->name);
            }
            node->type = VALUE_TEXT;
            return SQLEVAL_OK;
        case FN_LOWER:
        case FN_UPPER:
        case FN_TRIM:
        case FN_LTRIM:
        case FN_RTRIM:
        case FN_REPLACE:
            for (size_t i = 0; i < expr->arg_count; i++) {
                if (!is_textish(node->args[i]->type)) return failed(c->error, SQLEVAL_TYPE, expr, &expr->name);
            }
            node->type = VALUE_TEXT;
            return SQLEVAL_OK;
        case FN_COALESCE: {
            value_type_t type = VALUE_NULL;
            for (size_t i = 0; i < expr->arg_count; i++) {
                if (!unify(type, node->args[i]->type, &type)) return failed(c->error, SQLEVAL_TYPE, expr, &expr->name);
            }
            node->type = type;
            return SQLEVAL_OK;
        }
        case FN_NULLIF:
            if (!comparable(a, node->args[1]->type)) return failed(c->error, SQLEVAL_TYPE, expr, &expr->name);
            node->type = a;
            return SQLEVAL_OK;
        case FN_NONE:
            break;
    }
    return failed(c->error, SQLEVAL_UNKNOWN_FUNCTION, expr, &expr->name);
}

static sqleval_status_t type_node(compiler_t *c, node_t *node)
{
    const sqlexpr_t *expr = node->src;
    node_t **args = node->args;
    switch (expr->kind) {
        case SQLEXPR_LITERAL:
            node->type = expr->value.view.type;
            return SQLEVAL_OK;
        case SQLEXPR_COLUMN: {
            if (c->resolve == NULL) return failed(c->error, SQLEVAL_NOT_CONSTANT, expr, &expr->name);
            value_type_t type = VALUE_NULL;
            size_t column = c->resolve(c->ctx, &expr->qualifier, &expr->name, &type);
            if (column == SQLEVAL_NO_COLUMN) return failed(c->error, SQLEVAL_UNKNOWN_COLUMN, expr, &expr->name);
            if (column == SQLEVAL_AMBIGUOUS) return failed(c->error, SQLEVAL_AMBIGUOUS_COLUMN, expr, &expr->name);
            node->column = column;
            node->type = type;
            return SQLEVAL_OK;
        }
        case SQLEXPR_UNARY:
            if (expr->op == SQLEXPR_OP_NOT) {
                if (!is_boolish(args[0]->type)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                node->type = VALUE_INTEGER;
            } else {
                if (!is_numeric(args[0]->type)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                node->type = args[0]->type;
            }
            return SQLEVAL_OK;
        case SQLEXPR_BINARY: {
            value_type_t a = args[0]->type, b = args[1]->type;
            switch (expr->op) {
                case SQLEXPR_OP_ADD:
                case SQLEXPR_OP_SUB:
                case SQLEXPR_OP_MUL:
                case SQLEXPR_OP_DIV:
                    if (!is_numeric(a) || !is_numeric(b)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                    if (!unify(a, b, &node->type)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                    if (node->type == VALUE_NULL) node->type = VALUE_INTEGER;
                    return SQLEVAL_OK;
                case SQLEXPR_OP_MOD:
                    if (!is_boolish(a) || !is_boolish(b)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                    node->type = VALUE_INTEGER;
                    return SQLEVAL_OK;
                case SQLEXPR_OP_CONCAT:
                    if ((!is_textish(a) && a != VALUE_INTEGER) || (!is_textish(b) && b != VALUE_INTEGER)) {
                        return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                    }
                    node->type = VALUE_TEXT;
                    return SQLEVAL_OK;
                case SQLEXPR_OP_AND:
                case SQLEXPR_OP_OR:
                    if (!is_boolish(a) || !is_boolish(b)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                    node->type = VALUE_INTEGER;
                    return SQLEVAL_OK;
                default:
                    if (!comparable(a, b)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
                    node->type = VALUE_INTEGER;
                    return SQLEVAL_OK;
            }
        }
        case SQLEXPR_IS_NULL:
            node->type = VALUE_INTEGER;
            return SQLEVAL_OK;
        case SQLEXPR_BETWEEN:
        case SQLEXPR_IN:
            for (size_t i = 1; i < expr->arg_count; i++) {
                if (!comparable(args[0]->type, args[i]->type)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
            }
            node->type = VALUE_INTEGER;
            return SQLEVAL_OK;
        case SQLEXPR_LIKE:
            for (size_t i = 0; i < expr->arg_count; i++) {
                if (!is_textish(args[i]->type)) return failed(c->error, SQLEVAL_TYPE, expr, NULL);
            }
            node->type = VALUE_INTEGER;
            return SQLEVAL_OK;
        case SQLEXPR_CASE: {
            size_t at = expr->has_operand ? 1 : 0;
            size_t pairs_end = expr->arg_count - (expr->has_else ? 1 : 0);
            value_type_t type = VALUE_NULL;
            for (; at < pairs_end; at += 2) {
                value_type_t when = args[at]->type;
                if (expr->has_operand ? !comparable(args[0]->type, when) : !is_boolish(when)) {
                    return failed(c->error, SQLEVAL_TYPE, args[at]->src, NULL);
                }
                if (!unify(type, args[at + 1]->type, &type)) return failed(c->error, SQLEVAL_TYPE, args[at + 1]->src, NULL);
            }
            if (expr->has_else && !unify(type, args[expr->arg_count - 1]->type, &type)) {
                return failed(c->error, SQLEVAL_TYPE, args[expr->arg_count - 1]->src, NULL);
            }
            node->type = type;
            return SQLEVAL_OK;
        }
        case SQLEXPR_CAST:
            node->type = expr->cast_type;
            return SQLEVAL_OK;
        case SQLEXPR_FUNCTION:
            return compile_function(c, node);
    }
    return failed(c->error, SQLEVAL_TYPE, expr, NULL);
}

static sqleval_status_t compile_node(compiler_t *c, const sqlexpr_t *expr, node_t **out)
{
    node_t *node = &c->nodes[c->used++];
    node->src = expr;
    node->args = &c->slots[c->slots_used];
    c->slots_used += expr->arg_count;
    for (size_t i = 0; i < expr->arg_count; i++) {
        sqleval_status_t status = compile_node(c, expr->args[i], &node->args[i]);
        if (status != SQLEVAL_OK) return status;
    }
    *out = node;
    return type_node(c, node);
}

sqleval_status_t sqleval_compile(const sqlexpr_t *expr, sqleval_resolve_fn resolve, void *ctx,
                                 sqleval_prog_t **out, sqleval_error_t *error)
{
    if (out == NULL) return failed(error, SQLEVAL_ARGUMENT, NULL, NULL);
    *out = NULL;
    if (expr == NULL) return failed(error, SQLEVAL_ARGUMENT, NULL, NULL);
    size_t nodes = 0, slots = 0;
    count_nodes(expr, &nodes, &slots);
    sqleval_prog_t *prog = calloc(1, sizeof(*prog));
    node_t *array = calloc(nodes, sizeof(*array));
    node_t **slot_array = calloc(slots > 0 ? slots : 1, sizeof(*slot_array));
    if (prog == NULL || array == NULL || slot_array == NULL) {
        free(prog);
        free(array);
        free(slot_array);
        return failed(error, SQLEVAL_NOMEM, expr, NULL);
    }
    prog->nodes = array;
    prog->slots = slot_array;
    prog->node_count = nodes;
    compiler_t c = {resolve, ctx, array, 0, slot_array, 0, error};
    sqleval_status_t status = compile_node(&c, expr, &prog->root);
    if (status != SQLEVAL_OK) {
        sqleval_free(prog);
        return status;
    }
    *out = prog;
    return SQLEVAL_OK;
}

void sqleval_free(sqleval_prog_t *prog)
{
    if (prog == NULL) return;
    free(prog->nodes);
    free(prog->slots);
    free(prog);
}

value_type_t sqleval_type(const sqleval_prog_t *prog) { return prog != NULL ? prog->root->type : VALUE_NULL; }

void sqleval_mark_columns(const sqleval_prog_t *prog, bool *needed, size_t column_count)
{
    if (prog == NULL) return;
    for (size_t i = 0; i < prog->node_count; i++) {
        const node_t *node = &prog->nodes[i];
        if (node->src->kind == SQLEXPR_COLUMN && node->column < column_count) needed[node->column] = true;
    }
}

/* ── 실행 ── */

typedef struct {
    const value_t *row;
    sqleval_error_t *error;
} runner_t;

/*
 * node를 계산해 *view에 둔다. 새 값이 필요하면 *tmp에 만들고 view가 그것을 가리킨다(호출자가 tmp를 비운다).
 * 열과 리터럴은 복사하지 않고 빌려 준다.
 */
static sqleval_status_t eval(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view);

static sqleval_status_t from_value(const runner_t *r, value_status_t status, const node_t *node)
{
    switch (status) {
        case VALUE_OK: return SQLEVAL_OK;
        case VALUE_NOMEM: return failed(r->error, SQLEVAL_NOMEM, node->src, NULL);
        case VALUE_OVERFLOW: return failed(r->error, SQLEVAL_OVERFLOW, node->src, NULL);
        case VALUE_LIMIT: return failed(r->error, SQLEVAL_LIMIT, node->src, NULL);
        default: return failed(r->error, SQLEVAL_TYPE, node->src, NULL);
    }
}

static sqleval_status_t set_int(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view, int64_t v)
{
    value_status_t status = value_set_integer(tmp, v);
    if (status != VALUE_OK) return from_value(r, status, node);
    *view = tmp->view;
    return SQLEVAL_OK;
}

static sqleval_status_t set_null(value_t *tmp, value_view_t *view)
{
    value_clear(tmp);
    *view = tmp->view;
    return SQLEVAL_OK;
}

/* 조건 값: 1 참, 0 거짓, -1 NULL */
static sqleval_status_t truth_of(const runner_t *r, const node_t *node, const value_view_t *v, int *truth)
{
    if (v->type == VALUE_NULL) *truth = -1;
    else if (v->type == VALUE_INTEGER) *truth = v->integer != 0;
    else return failed(r->error, SQLEVAL_TYPE, node->src, NULL);
    return SQLEVAL_OK;
}

static sqleval_status_t truth(const runner_t *r, const node_t *node, int *out)
{
    value_t tmp = {0};
    value_view_t view;
    sqleval_status_t status = eval(r, node, &tmp, &view);
    if (status == SQLEVAL_OK) status = truth_of(r, node, &view, out);
    value_clear(&tmp);
    return status;
}

static sqleval_status_t set_truth(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view, int t)
{
    return t < 0 ? set_null(tmp, view) : set_int(r, node, tmp, view, t);
}

/* NULL이 아닌 두 값의 비교. 형식은 컴파일 때 맞췄다 */
static int compare_values(const value_view_t *a, const value_view_t *b)
{
    int order = 0;
    value_compare(a, b, &order);
    return order;
}

static double as_real(const value_view_t *v) { return v->type == VALUE_REAL ? v->real : (double)v->integer; }

static sqleval_status_t arithmetic(const runner_t *r, const node_t *node, const value_view_t *a,
                                   const value_view_t *b, value_t *tmp, value_view_t *view)
{
    sqlexpr_op_t op = node->src->op;
    if (a->type == VALUE_INTEGER && b->type == VALUE_INTEGER && node->type != VALUE_REAL) {
        int64_t x = a->integer, y = b->integer, z = 0;
        bool overflow = false;
        switch (op) {
            case SQLEXPR_OP_ADD: overflow = __builtin_add_overflow(x, y, &z); break;
            case SQLEXPR_OP_SUB: overflow = __builtin_sub_overflow(x, y, &z); break;
            case SQLEXPR_OP_MUL: overflow = __builtin_mul_overflow(x, y, &z); break;
            case SQLEXPR_OP_DIV:
            case SQLEXPR_OP_MOD:
                if (y == 0) return failed(r->error, SQLEVAL_DIVIDE_BY_ZERO, node->src, NULL);
                if (x == INT64_MIN && y == -1) {
                    if (op == SQLEXPR_OP_DIV) overflow = true;
                    else z = 0;
                } else {
                    z = op == SQLEXPR_OP_DIV ? x / y : x % y;
                }
                break;
            default: return failed(r->error, SQLEVAL_TYPE, node->src, NULL);
        }
        if (overflow) return failed(r->error, SQLEVAL_OVERFLOW, node->src, NULL);
        return set_int(r, node, tmp, view, z);
    }
    if (op == SQLEXPR_OP_MOD) return failed(r->error, SQLEVAL_TYPE, node->src, NULL);
    double x = as_real(a), y = as_real(b), z;
    switch (op) {
        case SQLEXPR_OP_ADD: z = x + y; break;
        case SQLEXPR_OP_SUB: z = x - y; break;
        case SQLEXPR_OP_MUL: z = x * y; break;
        case SQLEXPR_OP_DIV:
            if (y == 0.0) return failed(r->error, SQLEVAL_DIVIDE_BY_ZERO, node->src, NULL);
            z = x / y;
            break;
        default: return failed(r->error, SQLEVAL_TYPE, node->src, NULL);
    }
    value_status_t status = value_set_real(tmp, z);
    if (status != VALUE_OK) return from_value(r, status, node);
    *view = tmp->view;
    return SQLEVAL_OK;
}

/* INTEGER면 10진 표기, TEXT면 그대로. buffer는 INTEGER 표기용 */
static void text_of(const value_view_t *v, char buffer[32], const uint8_t **bytes, size_t *size)
{
    if (v->type == VALUE_INTEGER) {
        size_t needed = 0;
        value_format(v, buffer, 32, &needed);
        *bytes = (const uint8_t *)buffer;
        *size = needed;
    } else {
        *bytes = v->bytes;
        *size = v->size_bytes;
    }
}

static sqleval_status_t set_text_bytes(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view,
                                       const uint8_t *bytes, size_t size)
{
    value_status_t status = value_set_text(tmp, bytes, size);
    if (status != VALUE_OK) return from_value(r, status, node);
    *view = tmp->view;
    return SQLEVAL_OK;
}

static sqleval_status_t concat(const runner_t *r, const node_t *node, const value_view_t *a, const value_view_t *b,
                               value_t *tmp, value_view_t *view)
{
    char left_buffer[32], right_buffer[32];
    const uint8_t *left, *right;
    size_t left_size, right_size;
    text_of(a, left_buffer, &left, &left_size);
    text_of(b, right_buffer, &right, &right_size);
    if (left_size > SIZE_MAX - right_size || left_size + right_size > VALUE_MAX_BYTES) {
        return failed(r->error, SQLEVAL_LIMIT, node->src, NULL);
    }
    uint8_t *joined = malloc(left_size + right_size + 1);
    if (joined == NULL) return failed(r->error, SQLEVAL_NOMEM, node->src, NULL);
    if (left_size > 0) memcpy(joined, left, left_size);
    if (right_size > 0) memcpy(joined + left_size, right, right_size);
    sqleval_status_t status = set_text_bytes(r, node, tmp, view, joined, left_size + right_size);
    free(joined);
    return status;
}

/* UTF-8 한 글자의 바이트 수(유효한 TEXT라고 가정한다) */
static size_t char_len(uint8_t lead)
{
    if (lead < 0x80) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    return 4;
}

static bool same_char(const uint8_t *a, const uint8_t *b, size_t len) { return memcmp(a, b, len) == 0; }

/* LIKE: %는 0개 이상, _는 한 글자, escape 뒤의 글자는 그대로. 대소문자를 구분한다 */
static bool like_match(const uint8_t *text, size_t text_size, const uint8_t *pattern, size_t pattern_size,
                       const uint8_t *escape, size_t escape_size)
{
    size_t t = 0, p = 0, star_p = SIZE_MAX, star_t = 0;
    while (t < text_size) {
        if (p < pattern_size) {
            size_t plen = char_len(pattern[p]);
            bool escaped = escape_size > 0 && plen == escape_size && same_char(pattern + p, escape, plen) &&
                           p + plen < pattern_size;
            if (!escaped && pattern[p] == '%') {
                star_p = ++p;
                star_t = t;
                continue;
            }
            size_t at = escaped ? p + plen : p;
            size_t clen = char_len(pattern[at]);
            size_t tlen = char_len(text[t]);
            if ((!escaped && pattern[at] == '_') || (clen == tlen && same_char(pattern + at, text + t, clen))) {
                p = at + clen;
                t += tlen;
                continue;
            }
        }
        if (star_p == SIZE_MAX) return false;
        star_t += char_len(text[star_t]);
        t = star_t;
        p = star_p;
    }
    while (p < pattern_size && pattern[p] == '%') p++;
    return p == pattern_size;
}

static sqleval_status_t eval_like(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view)
{
    value_t parts_tmp[3];
    memset(parts_tmp, 0, sizeof(parts_tmp));
    value_view_t parts[3];
    memset(parts, 0, sizeof(parts));
    size_t count = node->src->arg_count;
    sqleval_status_t status = SQLEVAL_OK;
    bool has_null = false;
    for (size_t i = 0; i < count && status == SQLEVAL_OK; i++) {
        status = eval(r, node->args[i], &parts_tmp[i], &parts[i]);
        if (status == SQLEVAL_OK && parts[i].type == VALUE_NULL) has_null = true;
    }
    if (status == SQLEVAL_OK) {
        if (has_null) {
            status = set_null(tmp, view);
        } else if (count == 3 && (parts[2].size_bytes == 0 || char_len(parts[2].bytes[0]) != parts[2].size_bytes)) {
            status = failed(r->error, SQLEVAL_ARGUMENT, node->src, NULL);
        } else {
            bool matched = like_match(parts[0].bytes, parts[0].size_bytes, parts[1].bytes, parts[1].size_bytes,
                                      count == 3 ? parts[2].bytes : NULL, count == 3 ? parts[2].size_bytes : 0);
            status = set_int(r, node, tmp, view, matched != node->src->negated);
        }
    }
    for (size_t i = 0; i < 3; i++) value_clear(&parts_tmp[i]);
    return status;
}

/* 같은지(NULL이면 unknown -1). 형식은 맞춰져 있다 */
static int equals(const value_view_t *a, const value_view_t *b)
{
    if (a->type == VALUE_NULL || b->type == VALUE_NULL) return -1;
    return compare_values(a, b) == 0;
}

static sqleval_status_t eval_in(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view)
{
    value_t left_tmp = {0};
    value_view_t left;
    sqleval_status_t status = eval(r, node->args[0], &left_tmp, &left);
    int result = 0;
    for (size_t i = 1; i < node->src->arg_count && status == SQLEVAL_OK && result != 1; i++) {
        value_t item_tmp = {0};
        value_view_t item;
        status = eval(r, node->args[i], &item_tmp, &item);
        if (status == SQLEVAL_OK) {
            int eq = equals(&left, &item);
            if (eq == 1) result = 1;
            else if (eq < 0) result = -1;
        }
        value_clear(&item_tmp);
    }
    value_clear(&left_tmp);
    if (status != SQLEVAL_OK) return status;
    if (node->src->negated && result >= 0) result = !result;
    return set_truth(r, node, tmp, view, result);
}

static sqleval_status_t eval_between(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view)
{
    value_t parts_tmp[3];
    memset(parts_tmp, 0, sizeof(parts_tmp));
    value_view_t parts[3];
    memset(parts, 0, sizeof(parts));
    sqleval_status_t status = SQLEVAL_OK;
    for (size_t i = 0; i < 3 && status == SQLEVAL_OK; i++) status = eval(r, node->args[i], &parts_tmp[i], &parts[i]);
    if (status == SQLEVAL_OK) {
        /* x >= lo AND x <= hi 의 3치 논리 */
        int low = parts[0].type == VALUE_NULL || parts[1].type == VALUE_NULL ? -1 : compare_values(&parts[0], &parts[1]) >= 0;
        int high = parts[0].type == VALUE_NULL || parts[2].type == VALUE_NULL ? -1 : compare_values(&parts[0], &parts[2]) <= 0;
        int both = low == 0 || high == 0 ? 0 : (low < 0 || high < 0 ? -1 : 1);
        if (node->src->negated && both >= 0) both = !both;
        status = set_truth(r, node, tmp, view, both);
    }
    for (size_t i = 0; i < 3; i++) value_clear(&parts_tmp[i]);
    return status;
}

static sqleval_status_t eval_case(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view)
{
    const sqlexpr_t *expr = node->src;
    value_t operand_tmp = {0};
    value_view_t operand = {VALUE_NULL, 0, 0.0, NULL, 0};
    sqleval_status_t status = SQLEVAL_OK;
    size_t at = 0;
    if (expr->has_operand) {
        status = eval(r, node->args[0], &operand_tmp, &operand);
        at = 1;
    }
    size_t pairs_end = expr->arg_count - (expr->has_else ? 1 : 0);
    const node_t *chosen = expr->has_else ? node->args[expr->arg_count - 1] : NULL;
    for (; status == SQLEVAL_OK && at < pairs_end; at += 2) {
        int hit;
        if (expr->has_operand) {
            value_t when_tmp = {0};
            value_view_t when;
            status = eval(r, node->args[at], &when_tmp, &when);
            hit = status == SQLEVAL_OK ? equals(&operand, &when) == 1 : 0;
            value_clear(&when_tmp);
        } else {
            status = truth(r, node->args[at], &hit);
            hit = hit == 1;
        }
        if (status == SQLEVAL_OK && hit) {
            chosen = node->args[at + 1];
            break;
        }
    }
    value_clear(&operand_tmp);
    if (status != SQLEVAL_OK) return status;
    if (chosen == NULL) return set_null(tmp, view);
    status = eval(r, chosen, tmp, view);
    if (status == SQLEVAL_OK && node->type == VALUE_REAL && view->type == VALUE_INTEGER) {
        value_status_t cast = value_set_real(tmp, (double)view->integer);
        if (cast != VALUE_OK) return from_value(r, cast, node);
        *view = tmp->view;
    }
    return status;
}

/* 글자 단위 위치. start(1부터)에서 count 글자. 시작이 1보다 작으면 그만큼 길이를 줄인다(PostgreSQL) */
static sqleval_status_t substr(const runner_t *r, const node_t *node, const value_view_t *text, int64_t start,
                               bool has_count, int64_t count, value_t *tmp, value_view_t *view)
{
    if (has_count && count < 0) return failed(r->error, SQLEVAL_ARGUMENT, node->src, NULL);
    int64_t end = INT64_MAX;
    if (has_count) {
        if (__builtin_add_overflow(start, count, &end)) end = INT64_MAX;
    }
    if (start < 1) start = 1;
    size_t from = text->size_bytes, to = text->size_bytes;
    int64_t index = 1;
    for (size_t at = 0; at <= text->size_bytes; index++) {
        if (index == start) from = at;
        if (index == end) {
            to = at;
            break;
        }
        if (at == text->size_bytes) break;
        at += char_len(text->bytes[at]);
    }
    if (from > to) from = to;
    return set_text_bytes(r, node, tmp, view, text->bytes + from, to - from);
}

static bool in_set(const uint8_t *c, size_t len, const value_view_t *set)
{
    for (size_t at = 0; at < set->size_bytes; at += char_len(set->bytes[at])) {
        size_t slen = char_len(set->bytes[at]);
        if (slen == len && same_char(set->bytes + at, c, len)) return true;
    }
    return false;
}

static sqleval_status_t trim(const runner_t *r, const node_t *node, const value_view_t *text, const value_view_t *set,
                             bool left, bool right, value_t *tmp, value_view_t *view)
{
    static const value_view_t SPACE = {VALUE_TEXT, 0, 0.0, (const uint8_t *)" ", 1};
    if (set == NULL) set = &SPACE;
    size_t from = 0, to = text->size_bytes;
    if (left) {
        while (from < to && in_set(text->bytes + from, char_len(text->bytes[from]), set)) from += char_len(text->bytes[from]);
    }
    if (right) {
        while (to > from) {
            size_t start = to - 1;
            while (start > from && (text->bytes[start] & 0xC0) == 0x80) start--;
            if (!in_set(text->bytes + start, to - start, set)) break;
            to = start;
        }
    }
    return set_text_bytes(r, node, tmp, view, text->bytes + from, to - from);
}

static sqleval_status_t replace(const runner_t *r, const node_t *node, const value_view_t *text,
                                const value_view_t *from, const value_view_t *to, value_t *tmp, value_view_t *view)
{
    if (from->size_bytes == 0) return set_text_bytes(r, node, tmp, view, text->bytes, text->size_bytes);
    size_t hits = 0;
    for (size_t at = 0; at + from->size_bytes <= text->size_bytes;) {
        if (memcmp(text->bytes + at, from->bytes, from->size_bytes) == 0) {
            hits++;
            at += from->size_bytes;
        } else at++;
    }
    size_t size = text->size_bytes;
    if (to->size_bytes > from->size_bytes) {
        size_t grow = to->size_bytes - from->size_bytes;
        if (hits > 0 && grow > (VALUE_MAX_BYTES - size) / hits) return failed(r->error, SQLEVAL_LIMIT, node->src, NULL);
        size += grow * hits;
    } else {
        size -= (from->size_bytes - to->size_bytes) * hits;
    }
    uint8_t *out = malloc(size + 1);
    if (out == NULL) return failed(r->error, SQLEVAL_NOMEM, node->src, NULL);
    size_t w = 0;
    for (size_t at = 0; at < text->size_bytes;) {
        if (at + from->size_bytes <= text->size_bytes && memcmp(text->bytes + at, from->bytes, from->size_bytes) == 0) {
            if (to->size_bytes > 0) memcpy(out + w, to->bytes, to->size_bytes);
            w += to->size_bytes;
            at += from->size_bytes;
        } else out[w++] = text->bytes[at++];
    }
    sqleval_status_t status = set_text_bytes(r, node, tmp, view, out, w);
    free(out);
    return status;
}

static sqleval_status_t eval_function(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view)
{
    size_t count = node->src->arg_count;
    if (node->fn == FN_COALESCE) {
        for (size_t i = 0; i < count; i++) {
            sqleval_status_t status = eval(r, node->args[i], tmp, view);
            if (status != SQLEVAL_OK) return status;
            if (view->type != VALUE_NULL) {
                if (node->type == VALUE_REAL && view->type == VALUE_INTEGER) {
                    value_status_t cast = value_set_real(tmp, (double)view->integer);
                    if (cast != VALUE_OK) return from_value(r, cast, node);
                    *view = tmp->view;
                }
                return SQLEVAL_OK;
            }
        }
        return set_null(tmp, view);
    }
    value_t args_tmp[3];
    memset(args_tmp, 0, sizeof(args_tmp));
    value_view_t args[3];
    memset(args, 0, sizeof(args));
    sqleval_status_t status = SQLEVAL_OK;
    bool has_null = false;
    for (size_t i = 0; i < count && status == SQLEVAL_OK; i++) {
        status = eval(r, node->args[i], &args_tmp[i], &args[i]);
        if (status == SQLEVAL_OK && args[i].type == VALUE_NULL) has_null = true;
    }
    if (status == SQLEVAL_OK && node->fn == FN_NULLIF) {
        if (args[0].type != VALUE_NULL && equals(&args[0], &args[1]) == 1) status = set_null(tmp, view);
        else status = value_clone(&args[0], tmp) == VALUE_OK ? (*view = tmp->view, SQLEVAL_OK)
                                                               : failed(r->error, SQLEVAL_NOMEM, node->src, NULL);
    } else if (status == SQLEVAL_OK && has_null) {
        status = set_null(tmp, view);
    } else if (status == SQLEVAL_OK) {
        const value_view_t *a = &args[0];
        switch (node->fn) {
            case FN_ABS:
                if (a->type == VALUE_INTEGER) {
                    if (a->integer == INT64_MIN) status = failed(r->error, SQLEVAL_OVERFLOW, node->src, NULL);
                    else status = set_int(r, node, tmp, view, a->integer < 0 ? -a->integer : a->integer);
                } else {
                    value_status_t set = value_set_real(tmp, a->real < 0 ? -a->real : a->real);
                    status = from_value(r, set, node);
                    if (status == SQLEVAL_OK) *view = tmp->view;
                }
                break;
            case FN_LENGTH:
                if (a->type == VALUE_BLOB) {
                    status = set_int(r, node, tmp, view, (int64_t)a->size_bytes);
                } else {
                    size_t chars = 0;
                    status = from_value(r, value_text_chars(a, &chars), node);
                    if (status == SQLEVAL_OK) status = set_int(r, node, tmp, view, (int64_t)chars);
                }
                break;
            case FN_LOWER:
            case FN_UPPER: {
                uint8_t *out = malloc(a->size_bytes + 1);
                if (out == NULL) {
                    status = failed(r->error, SQLEVAL_NOMEM, node->src, NULL);
                    break;
                }
                for (size_t i = 0; i < a->size_bytes; i++) {
                    uint8_t c = a->bytes[i];
                    if (node->fn == FN_LOWER && c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
                    if (node->fn == FN_UPPER && c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
                    out[i] = c;
                }
                status = set_text_bytes(r, node, tmp, view, out, a->size_bytes);
                free(out);
                break;
            }
            case FN_SUBSTR:
                status = substr(r, node, a, args[1].integer, count == 3, count == 3 ? args[2].integer : 0, tmp, view);
                break;
            case FN_TRIM: status = trim(r, node, a, NULL, true, true, tmp, view); break;
            case FN_LTRIM: status = trim(r, node, a, count == 2 ? &args[1] : NULL, true, false, tmp, view); break;
            case FN_RTRIM: status = trim(r, node, a, count == 2 ? &args[1] : NULL, false, true, tmp, view); break;
            case FN_REPLACE: status = replace(r, node, a, &args[1], &args[2], tmp, view); break;
            default: status = failed(r->error, SQLEVAL_UNKNOWN_FUNCTION, node->src, &node->src->name); break;
        }
    }
    for (size_t i = 0; i < 3; i++) value_clear(&args_tmp[i]);
    return status;
}

static sqleval_status_t eval(const runner_t *r, const node_t *node, value_t *tmp, value_view_t *view)
{
    const sqlexpr_t *expr = node->src;
    switch (expr->kind) {
        case SQLEXPR_LITERAL:
            *view = expr->value.view;
            return SQLEVAL_OK;
        case SQLEXPR_COLUMN:
            *view = r->row[node->column].view;
            return SQLEVAL_OK;
        case SQLEXPR_UNARY: {
            if (expr->op == SQLEXPR_OP_NOT) {
                int t;
                sqleval_status_t status = truth(r, node->args[0], &t);
                if (status != SQLEVAL_OK) return status;
                return set_truth(r, node, tmp, view, t < 0 ? -1 : !t);
            }
            sqleval_status_t status = eval(r, node->args[0], tmp, view);
            if (status != SQLEVAL_OK || expr->op == SQLEXPR_OP_PLUS || view->type == VALUE_NULL) return status;
            if (view->type == VALUE_INTEGER) {
                if (view->integer == INT64_MIN) return failed(r->error, SQLEVAL_OVERFLOW, expr, NULL);
                return set_int(r, node, tmp, view, -view->integer);
            }
            if (view->type != VALUE_REAL) return failed(r->error, SQLEVAL_TYPE, expr, NULL);
            value_status_t set = value_set_real(tmp, -view->real);
            if (set != VALUE_OK) return from_value(r, set, node);
            *view = tmp->view;
            return SQLEVAL_OK;
        }
        case SQLEXPR_BINARY: {
            if (expr->op == SQLEXPR_OP_AND || expr->op == SQLEXPR_OP_OR) {
                int a, b;
                sqleval_status_t status = truth(r, node->args[0], &a);
                if (status != SQLEVAL_OK) return status;
                bool and = expr->op == SQLEXPR_OP_AND;
                if (and && a == 0) return set_int(r, node, tmp, view, 0);
                if (!and && a == 1) return set_int(r, node, tmp, view, 1);
                status = truth(r, node->args[1], &b);
                if (status != SQLEVAL_OK) return status;
                int result = and ? (b == 0 ? 0 : (a < 0 || b < 0 ? -1 : 1)) : (b == 1 ? 1 : (a < 0 || b < 0 ? -1 : 0));
                return set_truth(r, node, tmp, view, result);
            }
            value_t left_tmp = {0}, right_tmp = {0};
            value_view_t left, right;
            sqleval_status_t status = eval(r, node->args[0], &left_tmp, &left);
            if (status == SQLEVAL_OK) status = eval(r, node->args[1], &right_tmp, &right);
            if (status == SQLEVAL_OK) {
                if (left.type == VALUE_NULL || right.type == VALUE_NULL) {
                    status = set_null(tmp, view);
                } else {
                    switch (expr->op) {
                        case SQLEXPR_OP_ADD:
                        case SQLEXPR_OP_SUB:
                        case SQLEXPR_OP_MUL:
                        case SQLEXPR_OP_DIV:
                        case SQLEXPR_OP_MOD: status = arithmetic(r, node, &left, &right, tmp, view); break;
                        case SQLEXPR_OP_CONCAT: status = concat(r, node, &left, &right, tmp, view); break;
                        default: {
                            int order = compare_values(&left, &right);
                            bool result = false;
                            switch (expr->op) {
                                case SQLEXPR_OP_EQ: result = order == 0; break;
                                case SQLEXPR_OP_NE: result = order != 0; break;
                                case SQLEXPR_OP_LT: result = order < 0; break;
                                case SQLEXPR_OP_LE: result = order <= 0; break;
                                case SQLEXPR_OP_GT: result = order > 0; break;
                                case SQLEXPR_OP_GE: result = order >= 0; break;
                                default: break;
                            }
                            status = set_int(r, node, tmp, view, result);
                            break;
                        }
                    }
                }
            }
            value_clear(&left_tmp);
            value_clear(&right_tmp);
            return status;
        }
        case SQLEXPR_IS_NULL: {
            value_t inner = {0};
            value_view_t v;
            sqleval_status_t status = eval(r, node->args[0], &inner, &v);
            bool is_null = v.type == VALUE_NULL;
            value_clear(&inner);
            if (status != SQLEVAL_OK) return status;
            return set_int(r, node, tmp, view, is_null != expr->negated);
        }
        case SQLEXPR_BETWEEN: return eval_between(r, node, tmp, view);
        case SQLEXPR_IN: return eval_in(r, node, tmp, view);
        case SQLEXPR_LIKE: return eval_like(r, node, tmp, view);
        case SQLEXPR_CASE: return eval_case(r, node, tmp, view);
        case SQLEXPR_CAST: {
            value_t inner = {0};
            value_view_t v;
            sqleval_status_t status = eval(r, node->args[0], &inner, &v);
            if (status == SQLEVAL_OK) {
                value_status_t cast = value_cast(&v, expr->cast_type, tmp);
                status = from_value(r, cast, node);
                if (status == SQLEVAL_OK) *view = tmp->view;
            }
            value_clear(&inner);
            return status;
        }
        case SQLEXPR_FUNCTION: return eval_function(r, node, tmp, view);
    }
    return failed(r->error, SQLEVAL_TYPE, expr, NULL);
}

sqleval_status_t sqleval_run(const sqleval_prog_t *prog, const value_t *row, value_t *out, sqleval_error_t *error)
{
    if (prog == NULL || out == NULL) return failed(error, SQLEVAL_ARGUMENT, NULL, NULL);
    runner_t r = {row, error};
    value_t tmp = {0};
    value_view_t view;
    sqleval_status_t status = eval(&r, prog->root, &tmp, &view);
    if (status != SQLEVAL_OK) {
        value_clear(&tmp);
        return status;
    }
    /* 결과 형식을 정적 형식에 맞춘다(INTEGER가 REAL 자리에 온 경우) */
    if (prog->root->type == VALUE_REAL && view.type == VALUE_INTEGER) {
        value_status_t set = value_set_real(out, (double)view.integer);
        value_clear(&tmp);
        return set == VALUE_OK ? SQLEVAL_OK : from_value(&r, set, prog->root);
    }
    if (view.bytes != NULL && view.bytes == tmp.view.bytes) {
        value_clear(out);
        *out = tmp;
        return SQLEVAL_OK;
    }
    value_status_t cloned = value_clone(&view, out);
    value_clear(&tmp);
    return cloned == VALUE_OK ? SQLEVAL_OK : from_value(&r, cloned, prog->root);
}

sqleval_status_t sqleval_test(const sqleval_prog_t *prog, const value_t *row, bool *truth_out, sqleval_error_t *error)
{
    if (prog == NULL || truth_out == NULL) return failed(error, SQLEVAL_ARGUMENT, NULL, NULL);
    runner_t r = {row, error};
    int t = 0;
    sqleval_status_t status = truth(&r, prog->root, &t);
    *truth_out = status == SQLEVAL_OK && t == 1;
    return status;
}

int sqleval_compare(const value_view_t *a, const value_view_t *b)
{
    if (a->type == VALUE_NULL || b->type == VALUE_NULL) return (a->type != VALUE_NULL) - (b->type != VALUE_NULL);
    return compare_values(a, b);
}
