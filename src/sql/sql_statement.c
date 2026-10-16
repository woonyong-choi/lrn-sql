#include <stdlib.h>
#include <string.h>

#include "sql/sql_statement.h"
#include "storage/catalog.h"

#define NUMBER_BUFFER_BYTES 4097u
/* 식 트리 높이 한도. 괄호·연산자 사슬이 이보다 깊으면 LIMIT다(파싱·평가·해제가 재귀라 스택을 지킨다) */
#define SQLSTMT_MAX_DEPTH 512u
/* 목록(함수 인자·IN·CASE·여러 행 INSERT의 값) 한도 */
#define SQLSTMT_MAX_LIST 65536u

typedef struct {
    const lexer_token_t *tokens;
    size_t count;
    size_t pos;
    size_t end_offset;
    const lexer_binding_t *binding;
    sqlstmt_status_t status;
    sqlstmt_reason_t reason;
    size_t error_offset;
    value_status_t value_status;
    size_t depth;              /* 지금 파싱 중인 식의 중첩 깊이 */
} parser_t;

/* 이름 자리에 쓸 수 없는 예약어. 따옴표로 감싸면 쓸 수 있다. */
static const char *const RESERVED[] = {
    "CREATE", "TABLE", "INSERT", "INTO", "VALUES", "SELECT", "FROM", "WHERE", "UPDATE",
    "SET", "DELETE", "DROP", "NOT", "NULL", "PRIMARY", "KEY", "AUTOINCREMENT", "IS",
    "INDEX", "UNIQUE", "AND", "OR", "IN", "BETWEEN", "LIKE", "ESCAPE", "CASE", "WHEN", "THEN",
    "ELSE", "END", "CAST", "AS", "ORDER", "BY", "ASC", "DESC", "LIMIT", "OFFSET", "DISTINCT",
    "TRUE", "FALSE", "EXISTS", "GROUP", "HAVING", "JOIN", "INNER", "LEFT", "OUTER", "CROSS", "ON", "RETURNING", "UNION", "ALL", "EXCEPT", "INTERSECT", "WITH", "RIGHT", NULL};

/* 알지만 지원하지 않는 절·연산자. 이름 자리나 문장 끝에서 만나면 UNSUPPORTED다. */
static const char *const UNSUPPORTED_WORDS[] = {
    "FULL", "NATURAL", "USING", "WINDOW", "OVER", "RECURSIVE", NULL};

static bool fail(parser_t *p, sqlstmt_status_t status, sqlstmt_reason_t reason, size_t offset)
{
    p->status = status;
    p->reason = reason;
    p->error_offset = offset;
    return false;
}

static bool nomem(parser_t *p, size_t offset)
{
    return fail(p, SQLSTMT_NOMEM, SQLSTMT_REASON_NONE, offset);
}

static const lexer_token_t *current(const parser_t *p)
{
    return p->pos < p->count ? &p->tokens[p->pos] : NULL;
}

static const lexer_token_t *peek_next(const parser_t *p)
{
    return p->pos + 1 < p->count ? &p->tokens[p->pos + 1] : NULL;
}

static bool word_is(const lexer_token_t *token, const char *keyword)
{
    if (token == NULL || token->kind != LEXER_WORD) return false;
    size_t length = strlen(keyword);
    if (token->text_size != length) return false;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = token->text[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        if (c != (uint8_t)keyword[i]) return false;
    }
    return true;
}

static bool word_in(const lexer_token_t *token, const char *const *set)
{
    for (size_t i = 0; set[i] != NULL; i++) {
        if (word_is(token, set[i])) return true;
    }
    return false;
}

static bool punct_is(const lexer_token_t *token, const char *text)
{
    return token != NULL && token->kind == LEXER_PUNCT && token->text_size == strlen(text) &&
           memcmp(token->text, text, token->text_size) == 0;
}

/*
 * 기대한 토큰이 아닐 때의 오류. 토큰이 없으면 UNEXPECTED_END, 알지만 지원하지 않는 절이면
 * UNSUPPORTED로 분류하고 그 밖에는 호출자가 정한 reason의 SYNTAX다.
 */
static bool syntax_here(parser_t *p, sqlstmt_reason_t reason)
{
    const lexer_token_t *token = current(p);
    if (token == NULL) return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_UNEXPECTED_END, p->end_offset);
    if (word_in(token, UNSUPPORTED_WORDS)) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, token->offset);
    }
    return fail(p, SQLSTMT_SYNTAX, reason, token->offset);
}

static bool take_word(parser_t *p, const char *keyword)
{
    if (!word_is(current(p), keyword)) return false;
    p->pos++;
    return true;
}

static bool expect_word(parser_t *p, const char *keyword)
{
    if (take_word(p, keyword)) return true;
    return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
}

static bool take_punct(parser_t *p, const char *text)
{
    if (!punct_is(current(p), text)) return false;
    p->pos++;
    return true;
}

static bool expect_punct(parser_t *p, const char *text)
{
    if (take_punct(p, text)) return true;
    return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
}

/* 항목 하나를 늘릴 자리를 만든다. 실패하면 기존 배열은 그대로다. 새 칸은 0이다. */
static void *reserve_up_to(parser_t *p, void *array, size_t *capacity, size_t count, size_t element,
                           size_t offset, size_t limit)
{
    if (count >= limit) {
        fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_TOO_MANY_ITEMS, offset);
        return NULL;
    }
    if (count < *capacity) return array;
    size_t grown_capacity = *capacity > 0 ? *capacity * 2 : 4;
    void *grown = malloc(grown_capacity * element);
    if (grown == NULL) {
        nomem(p, offset);
        return NULL;
    }
    memset(grown, 0, grown_capacity * element);
    if (count > 0) memcpy(grown, array, count * element);
    free(array);
    *capacity = grown_capacity;
    return grown;
}

static void *reserve(parser_t *p, void *array, size_t *capacity, size_t count, size_t element, size_t offset)
{
    return reserve_up_to(p, array, capacity, count, element, offset, CATALOG_MAX_COLUMNS);
}

static size_t here_offset(const parser_t *p)
{
    const lexer_token_t *token = current(p);
    return token != NULL ? token->offset : p->end_offset;
}

static void name_clear(sqlstmt_name_t *name)
{
    free(name->bytes);
    name->bytes = NULL;
    name->size = 0;
}

static void select_free(struct sqlstmt_select *query);

static void expr_free(sqlexpr_t *expr)
{
    if (expr == NULL) return;
    select_free(expr->subquery);
    for (size_t i = 0; i < expr->arg_count; i++) expr_free(expr->args[i]);
    free(expr->args);
    value_clear(&expr->value);
    name_clear(&expr->qualifier);
    name_clear(&expr->name);
    free(expr);
}

static void names_clear(sqlstmt_name_t *names, size_t count)
{
    for (size_t i = 0; i < count; i++) name_clear(&names[i]);
    free(names);
}

static void assignments_free(sqlstmt_assignment_t *assignments, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        name_clear(&assignments[i].column);
        expr_free(assignments[i].value);
    }
    free(assignments);
}

static void items_free(sqlstmt_select_item_t *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        expr_free(items[i].expr);
        name_clear(&items[i].qualifier);
        name_clear(&items[i].alias);
    }
    free(items);
}

static void select_fields_clear(sqlstmt_select_t *q)
{
    for (size_t i = 0; i < q->cte_count; i++) {
        name_clear(&q->ctes[i].name);
        names_clear(q->ctes[i].columns, q->ctes[i].column_count);
        select_free(q->ctes[i].query);
    }
    free(q->ctes);
    items_free(q->items, q->item_count);
    for (size_t i = 0; i < q->order_count; i++) expr_free(q->order[i].expr);
    for (size_t i = 0; i < q->group_count; i++) expr_free(q->group_by[i]);
    free(q->group_by);
    expr_free(q->having);
    free(q->order);
    expr_free(q->where);
    expr_free(q->limit);
    expr_free(q->offset);
    for (size_t i = 0; i < q->source_count; i++) {
        name_clear(&q->sources[i].table);
        name_clear(&q->sources[i].alias);
        select_free(q->sources[i].subquery);
        expr_free(q->sources[i].on);
    }
    free(q->sources);
}

/* 첫 SELECT는 문장 구조 안에 있고 집합 연산으로 이어진 뒤 SELECT들은 따로 할당했다 */
static void select_clear(sqlstmt_select_t *head);

static void select_free(sqlstmt_select_t *query)
{
    if (query == NULL) return;
    select_clear(query);
    free(query);
}

static void select_clear(sqlstmt_select_t *head)
{
    select_fields_clear(head);
    for (sqlstmt_select_t *next = head->next; next != NULL;) {
        sqlstmt_select_t *after = next->next;
        select_fields_clear(next);
        free(next);
        next = after;
    }
}

void sqlstmt_free(sqlstmt_t *statement)
{
    if (statement == NULL) return;
    switch (statement->kind) {
        case SQLSTMT_CREATE_TABLE:
            for (size_t i = 0; i < statement->create.column_count; i++) {
                name_clear(&statement->create.columns[i].name);
                expr_free(statement->create.columns[i].default_value);
            }
            free(statement->create.columns);
            name_clear(&statement->create.table);
            break;
        case SQLSTMT_CREATE_INDEX:
            name_clear(&statement->index.name);
            name_clear(&statement->index.table);
            name_clear(&statement->index.column);
            break;
        case SQLSTMT_INSERT:
            names_clear(statement->insert.columns, statement->insert.column_count);
            for (size_t i = 0; i < statement->insert.value_count * statement->insert.row_count; i++) {
                expr_free(statement->insert.values[i]);
            }
            free(statement->insert.values);
            name_clear(&statement->insert.table);
            name_clear(&statement->insert.conflict.target);
            assignments_free(statement->insert.conflict.assignments, statement->insert.conflict.assignment_count);
            expr_free(statement->insert.conflict.where);
            items_free(statement->insert.returning.items, statement->insert.returning.count);
            break;
        case SQLSTMT_SELECT:
            select_clear(&statement->select);
            break;
        case SQLSTMT_UPDATE:
            assignments_free(statement->update.assignments, statement->update.assignment_count);
            expr_free(statement->update.where);
            name_clear(&statement->update.table);
            items_free(statement->update.returning.items, statement->update.returning.count);
            break;
        case SQLSTMT_DELETE:
            expr_free(statement->del.where);
            name_clear(&statement->del.table);
            items_free(statement->del.returning.items, statement->del.returning.count);
            break;
        case SQLSTMT_DROP_TABLE:
            name_clear(&statement->drop.table);
            break;
        case SQLSTMT_BEGIN:
        case SQLSTMT_COMMIT:
        case SQLSTMT_ROLLBACK:
            break;
    }
    free(statement);
}

/*
 * 이름 하나. 예약어와 알려진 미지원 단어는 거절하고 따옴표 식별자는 어떤 UTF-8이든 받는다.
 * 뒤에 '.'이 오면 한정 이름이라 거절하며 reject_call이면 '('도 함수 호출로 거절한다.
 */
static bool parse_name(parser_t *p, sqlstmt_name_t *out, bool reject_call)
{
    const lexer_token_t *token = current(p);
    if (token == NULL || (token->kind != LEXER_WORD && token->kind != LEXER_QUOTED_IDENT) ||
        (token->kind == LEXER_WORD && word_in(token, RESERVED))) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_IDENTIFIER);
    }
    if (token->kind == LEXER_WORD && (word_is(token, "AND") || word_is(token, "OR") ||
                                      word_in(token, UNSUPPORTED_WORDS))) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_IDENTIFIER);
    }
    if (token->text_size > CATALOG_NAME_MAX_BYTES) {
        return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NAME_TOO_LONG, token->offset);
    }
    const lexer_token_t *next = peek_next(p);
    if (punct_is(next, ".") || (reject_call && punct_is(next, "("))) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, next->offset);
    }
    uint8_t *copy = malloc(token->text_size);
    if (copy == NULL) return nomem(p, token->offset);
    memcpy(copy, token->text, token->text_size);
    out->bytes = copy;
    out->size = token->text_size;
    p->pos++;
    return true;
}

/* 한정 이름의 한 부분(table 또는 column). 뒤의 '.'·'('는 호출자가 본다 */
static bool parse_name_part(parser_t *p, sqlstmt_name_t *out)
{
    const lexer_token_t *token = current(p);
    if (token == NULL || (token->kind != LEXER_WORD && token->kind != LEXER_QUOTED_IDENT) ||
        (token->kind == LEXER_WORD && (word_in(token, RESERVED) || word_in(token, UNSUPPORTED_WORDS)))) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_IDENTIFIER);
    }
    if (token->text_size > CATALOG_NAME_MAX_BYTES) {
        return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NAME_TOO_LONG, token->offset);
    }
    uint8_t *copy = malloc(token->text_size);
    if (copy == NULL) return nomem(p, token->offset);
    memcpy(copy, token->text, token->text_size);
    out->bytes = copy;
    out->size = token->text_size;
    p->pos++;
    return true;
}

static bool names_equal(const sqlstmt_name_t *left, const sqlstmt_name_t *right)
{
    return left->size == right->size && memcmp(left->bytes, right->bytes, left->size) == 0;
}

static bool is_float_literal(const lexer_token_t *token)
{
    for (size_t i = 0; i < token->text_size; i++) {
        uint8_t c = token->text[i];
        if (c == '.' || c == 'e' || c == 'E') return true;
    }
    return false;
}

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 값 변환 실패를 오류로 옮긴다. 리터럴의 범위 오류는 LIMIT, 그 밖의 형식 오류는 SYNTAX다. */
static bool value_failed(parser_t *p, value_status_t status, size_t offset)
{
    p->value_status = status;
    if (status == VALUE_NOMEM) return nomem(p, offset);
    if (status == VALUE_OVERFLOW || status == VALUE_LIMIT) {
        return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, offset);
    }
    return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_BAD_NUMBER, offset);
}

static bool parse_number(parser_t *p, value_t *out)
{
    size_t start = here_offset(p);
    bool negative = false;
    if (punct_is(current(p), "-") || punct_is(current(p), "+")) {
        negative = current(p)->text[0] == '-';
        p->pos++;
    }
    const lexer_token_t *token = current(p);
    if (token == NULL || token->kind != LEXER_NUMBER) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_VALUE);
    }
    size_t sign_bytes = negative ? 1 : 0;
    if (token->text_size + sign_bytes >= NUMBER_BUFFER_BYTES) {
        return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, start);
    }
    uint8_t buffer[NUMBER_BUFFER_BYTES];
    if (negative) buffer[0] = '-';
    memcpy(buffer + sign_bytes, token->text, token->text_size);
    value_view_t text = {VALUE_TEXT, 0, 0.0, buffer, token->text_size + sign_bytes};
    value_type_t target = is_float_literal(token) ? VALUE_REAL : VALUE_INTEGER;
    value_status_t status = value_cast(&text, target, out);
    if (status != VALUE_OK) return value_failed(p, status, start);
    p->pos++;
    return true;
}

static bool parse_blob_literal(parser_t *p, value_t *out)
{
    const lexer_token_t *quote = peek_next(p);
    if (quote == NULL || quote->kind != LEXER_STRING || quote->offset != current(p)->offset + 1) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_VALUE);
    }
    if (quote->text_size % 2 != 0) {
        return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_BAD_BLOB_LITERAL, quote->offset);
    }
    size_t size = quote->text_size / 2;
    uint8_t *bytes = size > 0 ? malloc(size) : NULL;
    if (size > 0 && bytes == NULL) return nomem(p, quote->offset);
    for (size_t i = 0; i < size; i++) {
        int high = hex_digit(quote->text[2 * i]);
        int low = hex_digit(quote->text[2 * i + 1]);
        if (high < 0 || low < 0) {
            free(bytes);
            return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_BAD_BLOB_LITERAL, quote->offset);
        }
        bytes[i] = (uint8_t)(high << 4 | low);
    }
    value_status_t status = value_set_blob(out, bytes, size);
    free(bytes);
    if (status != VALUE_OK) return value_failed(p, status, quote->offset);
    p->pos += 2;
    return true;
}

static bool parse_parameter(parser_t *p, value_t *out)
{
    const lexer_token_t *token = current(p);
    if (p->binding == NULL) {
        return fail(p, SQLSTMT_INVALID, SQLSTMT_REASON_MISSING_BINDING, token->offset);
    }
    const value_view_t *view = NULL;
    if (lexer_binding_value(p->binding, token->parameter_id, &view) != LEXER_OK) {
        return fail(p, SQLSTMT_INVALID, SQLSTMT_REASON_BAD_BINDING, token->offset);
    }
    value_status_t status = value_clone(view, out);
    if (status == VALUE_NOMEM) return nomem(p, token->offset);
    if (status != VALUE_OK) {
        p->value_status = status;
        return fail(p, SQLSTMT_INVALID, SQLSTMT_REASON_BAD_BINDING, token->offset);
    }
    p->pos++;
    return true;
}

/* 값 하나. 실패하면 *out은 그대로다. */
static bool parse_value(parser_t *p, value_t *out)
{
    const lexer_token_t *token = current(p);
    if (token == NULL) return syntax_here(p, SQLSTMT_REASON_EXPECTED_VALUE);
    if (token->kind == LEXER_PARAMETER) return parse_parameter(p, out);
    if (token->kind == LEXER_STRING) {
        value_status_t status = value_set_text(out, token->text, token->text_size);
        if (status != VALUE_OK) return value_failed(p, status, token->offset);
        p->pos++;
        return true;
    }
    if (token->kind == LEXER_NUMBER || punct_is(token, "-") || punct_is(token, "+")) {
        return parse_number(p, out);
    }
    if (word_is(token, "NULL")) {
        value_status_t status = value_set_null(out);
        if (status != VALUE_OK) return value_failed(p, status, token->offset);
        p->pos++;
        return true;
    }
    if (word_is(token, "X")) return parse_blob_literal(p, out);
    return syntax_here(p, SQLSTMT_REASON_EXPECTED_VALUE);
}

static bool parse_type(parser_t *p, sqlstmt_column_def_t *column);

/* ── 식 ── */

static sqlexpr_t *new_expr(parser_t *p, sqlexpr_kind_t kind, size_t offset)
{
    sqlexpr_t *expr = calloc(1, sizeof(*expr));
    if (expr == NULL) {
        nomem(p, offset);
        return NULL;
    }
    expr->kind = kind;
    expr->offset = offset;
    return expr;
}

/* child를 parent의 인자로 붙인다. 실패하면 child를 해제한다(parent는 그대로 호출자 소유). */
static bool push_arg(parser_t *p, sqlexpr_t *parent, sqlexpr_t *child, size_t *capacity)
{
    void *grown = reserve_up_to(p, parent->args, capacity, parent->arg_count, sizeof(*parent->args),
                                child->offset, SQLSTMT_MAX_LIST);
    if (grown == NULL) {
        expr_free(child);
        return false;
    }
    parent->args = grown;
    parent->args[parent->arg_count++] = child;
    return true;
}

/* 인자를 정해진 수만큼 가진 노드. 실패하면 인자를 모두 해제한다. */
static sqlexpr_t *make_node(parser_t *p, sqlexpr_kind_t kind, size_t offset, sqlexpr_t **args, size_t count)
{
    sqlexpr_t *node = new_expr(p, kind, offset);
    size_t capacity = 0;
    for (size_t i = 0; i < count; i++) {
        if (node == NULL || !push_arg(p, node, args[i], &capacity)) {
            for (size_t j = node == NULL ? i : i + 1; j < count; j++) expr_free(args[j]);
            expr_free(node);
            return NULL;
        }
    }
    return node;
}

static bool parse_expr(parser_t *p, sqlexpr_t **out);
static bool parse_subquery(parser_t *p, sqlstmt_select_t **out);

/* 현재 토큰이 괄호 안 질의의 시작인지 */
static bool starts_query(const lexer_token_t *token) { return word_is(token, "SELECT") || word_is(token, "WITH"); }

/* 깊이를 하나 들어간다. 한도를 넘으면 LIMIT다 */
static bool enter(parser_t *p)
{
    if (++p->depth > SQLSTMT_MAX_DEPTH) return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_TOO_MANY_ITEMS, here_offset(p));
    return true;
}

static bool literal(parser_t *p, sqlexpr_t **out, size_t offset, bool (*read)(parser_t *, value_t *))
{
    sqlexpr_t *expr = new_expr(p, SQLEXPR_LITERAL, offset);
    if (expr == NULL) return false;
    if (!read(p, &expr->value)) {
        expr_free(expr);
        return false;
    }
    *out = expr;
    return true;
}

static bool parse_type_name(parser_t *p, value_type_t *type)
{
    sqlstmt_column_def_t column = {0};
    if (!parse_type(p, &column)) return false;
    *type = column.type;
    return true;
}

/* 이름(args) 또는 이름(*) 또는 이름(DISTINCT x) */
static bool parse_call(parser_t *p, sqlexpr_t **out)
{
    const lexer_token_t *token = current(p);
    sqlexpr_t *call = new_expr(p, SQLEXPR_FUNCTION, token->offset);
    if (call == NULL) return false;
    if (token->text_size > CATALOG_NAME_MAX_BYTES) {
        expr_free(call);
        return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NAME_TOO_LONG, token->offset);
    }
    call->name.bytes = malloc(token->text_size);
    if (call->name.bytes == NULL) {
        expr_free(call);
        return nomem(p, token->offset);
    }
    /* 함수 이름은 대소문자를 구분하지 않는다. 대문자로 맞춰 둔다 */
    for (size_t i = 0; i < token->text_size; i++) {
        uint8_t c = token->text[i];
        call->name.bytes[i] = c >= 'a' && c <= 'z' ? (uint8_t)(c - 'a' + 'A') : c;
    }
    call->name.size = token->text_size;
    p->pos += 2; /* 이름과 ( */
    size_t capacity = 0;
    if (take_punct(p, "*")) {
        call->star = true;
    } else if (!punct_is(current(p), ")")) {
        call->distinct = take_word(p, "DISTINCT");
        do {
            sqlexpr_t *arg = NULL;
            if (!parse_expr(p, &arg) || !push_arg(p, call, arg, &capacity)) {
                expr_free(call);
                return false;
            }
        } while (take_punct(p, ","));
    }
    if (!expect_punct(p, ")")) {
        expr_free(call);
        return false;
    }
    *out = call;
    return true;
}

static bool parse_case(parser_t *p, sqlexpr_t **out, size_t offset)
{
    sqlexpr_t *node = new_expr(p, SQLEXPR_CASE, offset);
    if (node == NULL) return false;
    size_t capacity = 0;
    sqlexpr_t *part = NULL;
    if (!word_is(current(p), "WHEN")) {
        if (!parse_expr(p, &part) || !push_arg(p, node, part, &capacity)) goto failed;
        node->has_operand = true;
    }
    if (!word_is(current(p), "WHEN")) {
        syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
        goto failed;
    }
    while (take_word(p, "WHEN")) {
        if (!parse_expr(p, &part) || !push_arg(p, node, part, &capacity)) goto failed;
        if (!expect_word(p, "THEN")) goto failed;
        if (!parse_expr(p, &part) || !push_arg(p, node, part, &capacity)) goto failed;
    }
    if (take_word(p, "ELSE")) {
        if (!parse_expr(p, &part) || !push_arg(p, node, part, &capacity)) goto failed;
        node->has_else = true;
    }
    if (!expect_word(p, "END")) goto failed;
    *out = node;
    return true;
failed:
    expr_free(node);
    return false;
}

static bool parse_primary(parser_t *p, sqlexpr_t **out)
{
    const lexer_token_t *token = current(p);
    if (token == NULL) return syntax_here(p, SQLSTMT_REASON_EXPECTED_VALUE);
    size_t offset = token->offset;
    if (punct_is(token, "(") && starts_query(peek_next(p))) {
        p->pos++;
        sqlexpr_t *node = new_expr(p, SQLEXPR_SUBQUERY, offset);
        if (node == NULL) return false;
        if (!parse_subquery(p, &node->subquery)) {
            expr_free(node);
            return false;
        }
        *out = node;
        return true;
    }
    if (punct_is(token, "(")) {
        p->pos++;
        sqlexpr_t *inner = NULL;
        if (!parse_expr(p, &inner)) return false;
        if (!expect_punct(p, ")")) {
            expr_free(inner);
            return false;
        }
        *out = inner;
        return true;
    }
    if (token->kind == LEXER_PARAMETER || token->kind == LEXER_STRING || token->kind == LEXER_NUMBER ||
        word_is(token, "NULL") || (word_is(token, "X") && peek_next(p) != NULL &&
                                    peek_next(p)->kind == LEXER_STRING)) {
        return literal(p, out, offset, parse_value);
    }
    if (word_is(token, "TRUE") || word_is(token, "FALSE")) {
        sqlexpr_t *expr = new_expr(p, SQLEXPR_LITERAL, offset);
        if (expr == NULL) return false;
        value_status_t set = value_set_integer(&expr->value, word_is(token, "TRUE") ? 1 : 0);
        if (set != VALUE_OK) {
            expr_free(expr);
            return value_failed(p, set, offset);
        }
        p->pos++;
        *out = expr;
        return true;
    }
    if (take_word(p, "CASE")) return parse_case(p, out, offset);
    if (word_is(token, "CAST") && punct_is(peek_next(p), "(")) {
        p->pos += 2;
        sqlexpr_t *inner = NULL;
        if (!parse_expr(p, &inner)) return false;
        sqlexpr_t *cast = make_node(p, SQLEXPR_CAST, offset, &inner, 1);
        if (cast == NULL) return false;
        if (!expect_word(p, "AS") || !parse_type_name(p, &cast->cast_type) || !expect_punct(p, ")")) {
            expr_free(cast);
            return false;
        }
        *out = cast;
        return true;
    }
    if (word_is(token, "EXISTS")) {
        p->pos++;
        if (!expect_punct(p, "(")) return false;
        if (!starts_query(current(p))) return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
        sqlexpr_t *node = new_expr(p, SQLEXPR_EXISTS, offset);
        if (node == NULL) return false;
        if (!parse_subquery(p, &node->subquery)) {
            expr_free(node);
            return false;
        }
        *out = node;
        return true;
    }
    if ((token->kind != LEXER_WORD && token->kind != LEXER_QUOTED_IDENT) ||
        (token->kind == LEXER_WORD && (word_in(token, RESERVED) || word_in(token, UNSUPPORTED_WORDS)))) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_VALUE);
    }
    if (token->kind == LEXER_WORD && punct_is(peek_next(p), "(") && !word_in(token, RESERVED)) {
        return parse_call(p, out);
    }
    sqlexpr_t *column = new_expr(p, SQLEXPR_COLUMN, offset);
    if (column == NULL) return false;
    bool qualified = punct_is(peek_next(p), ".");
    if (qualified) {
        if (!parse_name_part(p, &column->qualifier) || !expect_punct(p, ".")) {
            expr_free(column);
            return false;
        }
    }
    if (!parse_name_part(p, &column->name)) {
        expr_free(column);
        return false;
    }
    *out = column;
    return true;
}

static bool parse_unary(parser_t *p, sqlexpr_t **out)
{
    const lexer_token_t *token = current(p);
    if ((punct_is(token, "-") || punct_is(token, "+"))) {
        const lexer_token_t *next = peek_next(p);
        /* 부호 붙은 숫자는 리터럴로 읽는다(INT64_MIN을 표현하기 위해서다) */
        if (next != NULL && next->kind == LEXER_NUMBER) {
            return literal(p, out, token->offset, parse_value);
        }
        if (!enter(p)) return false;
        p->pos++;
        sqlexpr_t *operand = NULL;
        bool ok = parse_unary(p, &operand);
        p->depth--;
        if (!ok) return false;
        sqlexpr_t *node = make_node(p, SQLEXPR_UNARY, token->offset, &operand, 1);
        if (node == NULL) return false;
        node->op = punct_is(token, "-") ? SQLEXPR_OP_NEG : SQLEXPR_OP_PLUS;
        *out = node;
        return true;
    }
    return parse_primary(p, out);
}

/* 왼쪽 결합 이항 연산 한 단계. ops·kinds는 같은 길이다 */
typedef bool (*level_fn)(parser_t *, sqlexpr_t **);

static bool binary_level(parser_t *p, sqlexpr_t **out, level_fn next, const char *const *puncts,
                         const sqlexpr_op_t *ops, size_t count)
{
    sqlexpr_t *left = NULL;
    if (!next(p, &left)) return false;
    size_t chained = 0;
    for (;;) {
        size_t which = count;
        for (size_t i = 0; i < count; i++) {
            if (punct_is(current(p), puncts[i])) which = i;
        }
        if (which == count) break;
        if (++chained + p->depth > SQLSTMT_MAX_DEPTH) {
            expr_free(left);
            return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_TOO_MANY_ITEMS, here_offset(p));
        }
        size_t offset = here_offset(p);
        p->pos++;
        sqlexpr_t *right = NULL;
        if (!next(p, &right)) {
            expr_free(left);
            return false;
        }
        sqlexpr_t *pair[2] = {left, right};
        left = make_node(p, SQLEXPR_BINARY, offset, pair, 2);
        if (left == NULL) return false;
        left->op = ops[which];
    }
    *out = left;
    return true;
}

static bool parse_concat(parser_t *p, sqlexpr_t **out)
{
    static const char *const P[] = {"||"};
    static const sqlexpr_op_t O[] = {SQLEXPR_OP_CONCAT};
    return binary_level(p, out, parse_unary, P, O, 1);
}

static bool parse_mul(parser_t *p, sqlexpr_t **out)
{
    static const char *const P[] = {"*", "/", "%"};
    static const sqlexpr_op_t O[] = {SQLEXPR_OP_MUL, SQLEXPR_OP_DIV, SQLEXPR_OP_MOD};
    return binary_level(p, out, parse_concat, P, O, 3);
}

static bool parse_add(parser_t *p, sqlexpr_t **out)
{
    static const char *const P[] = {"+", "-"};
    static const sqlexpr_op_t O[] = {SQLEXPR_OP_ADD, SQLEXPR_OP_SUB};
    return binary_level(p, out, parse_mul, P, O, 2);
}

/* 비교와 술어: = == != <> < <= > >=, IS [NOT] NULL, [NOT] BETWEEN, [NOT] IN, [NOT] LIKE */
static bool parse_predicate(parser_t *p, sqlexpr_t **out)
{
    sqlexpr_t *left = NULL;
    if (!parse_add(p, &left)) return false;
    static const char *const P[] = {"=", "==", "!=", "<>", "<", "<=", ">", ">="};
    static const sqlexpr_op_t O[] = {SQLEXPR_OP_EQ, SQLEXPR_OP_EQ, SQLEXPR_OP_NE, SQLEXPR_OP_NE,
                                     SQLEXPR_OP_LT, SQLEXPR_OP_LE, SQLEXPR_OP_GT, SQLEXPR_OP_GE};
    const lexer_token_t *token = current(p);
    size_t offset = here_offset(p);
    for (size_t i = 0; i < sizeof(O) / sizeof(O[0]); i++) {
        if (!punct_is(token, P[i])) continue;
        p->pos++;
        sqlexpr_t *right = NULL;
        if (!parse_add(p, &right)) {
            expr_free(left);
            return false;
        }
        sqlexpr_t *pair[2] = {left, right};
        sqlexpr_t *node = make_node(p, SQLEXPR_BINARY, offset, pair, 2);
        if (node == NULL) return false;
        node->op = O[i];
        *out = node;
        return true;
    }
    if (take_word(p, "IS")) {
        bool negated = take_word(p, "NOT");
        if (!expect_word(p, "NULL")) {
            expr_free(left);
            return false;
        }
        sqlexpr_t *node = make_node(p, SQLEXPR_IS_NULL, offset, &left, 1);
        if (node == NULL) return false;
        node->negated = negated;
        *out = node;
        return true;
    }
    bool negated = false;
    if (word_is(token, "NOT") && (word_is(peek_next(p), "BETWEEN") || word_is(peek_next(p), "IN") ||
                                  word_is(peek_next(p), "LIKE"))) {
        negated = true;
        p->pos++;
    }
    if (take_word(p, "BETWEEN")) {
        sqlexpr_t *parts[3] = {left, NULL, NULL};
        if (!parse_add(p, &parts[1]) || !expect_word(p, "AND") || !parse_add(p, &parts[2])) {
            expr_free(parts[0]);
            expr_free(parts[1]);
            return false;
        }
        sqlexpr_t *node = make_node(p, SQLEXPR_BETWEEN, offset, parts, 3);
        if (node == NULL) return false;
        node->negated = negated;
        *out = node;
        return true;
    }
    if (take_word(p, "IN")) {
        sqlexpr_t *node = make_node(p, SQLEXPR_IN, offset, &left, 1);
        if (node == NULL) return false;
        node->negated = negated;
        size_t capacity = 1;
        if (!expect_punct(p, "(")) {
            expr_free(node);
            return false;
        }
        if (starts_query(current(p))) {
            if (!parse_subquery(p, &node->subquery)) {
                expr_free(node);
                return false;
            }
            *out = node;
            return true;
        }
        do {
            sqlexpr_t *item = NULL;
            if (!parse_expr(p, &item) || !push_arg(p, node, item, &capacity)) {
                expr_free(node);
                return false;
            }
        } while (take_punct(p, ","));
        if (!expect_punct(p, ")")) {
            expr_free(node);
            return false;
        }
        *out = node;
        return true;
    }
    if (take_word(p, "LIKE")) {
        sqlexpr_t *parts[3] = {left, NULL, NULL};
        size_t count = 2;
        if (!parse_add(p, &parts[1])) {
            expr_free(left);
            return false;
        }
        if (take_word(p, "ESCAPE")) {
            if (!parse_add(p, &parts[2])) {
                expr_free(parts[0]);
                expr_free(parts[1]);
                return false;
            }
            count = 3;
        }
        sqlexpr_t *node = make_node(p, SQLEXPR_LIKE, offset, parts, count);
        if (node == NULL) return false;
        node->negated = negated;
        *out = node;
        return true;
    }
    if (negated) {
        expr_free(left);
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
    }
    *out = left;
    return true;
}

static bool parse_not(parser_t *p, sqlexpr_t **out)
{
    const lexer_token_t *token = current(p);
    if (!word_is(token, "NOT")) return parse_predicate(p, out);
    if (!enter(p)) return false;
    p->pos++;
    sqlexpr_t *operand = NULL;
    bool ok = parse_not(p, &operand);
    p->depth--;
    if (!ok) return false;
    sqlexpr_t *node = make_node(p, SQLEXPR_UNARY, token->offset, &operand, 1);
    if (node == NULL) return false;
    node->op = SQLEXPR_OP_NOT;
    *out = node;
    return true;
}

static bool word_level(parser_t *p, sqlexpr_t **out, level_fn next, const char *word, sqlexpr_op_t op)
{
    sqlexpr_t *left = NULL;
    if (!next(p, &left)) return false;
    size_t chained = 0;
    while (word_is(current(p), word)) {
        if (++chained + p->depth > SQLSTMT_MAX_DEPTH) {
            expr_free(left);
            return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_TOO_MANY_ITEMS, here_offset(p));
        }
        size_t offset = here_offset(p);
        p->pos++;
        sqlexpr_t *right = NULL;
        if (!next(p, &right)) {
            expr_free(left);
            return false;
        }
        sqlexpr_t *pair[2] = {left, right};
        left = make_node(p, SQLEXPR_BINARY, offset, pair, 2);
        if (left == NULL) return false;
        left->op = op;
    }
    *out = left;
    return true;
}

static bool parse_and(parser_t *p, sqlexpr_t **out) { return word_level(p, out, parse_not, "AND", SQLEXPR_OP_AND); }

static bool parse_expr(parser_t *p, sqlexpr_t **out)
{
    if (!enter(p)) return false;
    bool ok = word_level(p, out, parse_and, "OR", SQLEXPR_OP_OR);
    p->depth--;
    return ok;
}

static bool parse_where(parser_t *p, sqlexpr_t **where)
{
    if (!take_word(p, "WHERE")) return true;
    return parse_expr(p, where);
}

/* 문장은 여기서 끝나야 한다. 남은 토큰이 알려진 절이면 UNSUPPORTED로 구분한다. */
static bool parse_end(parser_t *p)
{
    if (current(p) == NULL) return true;
    return syntax_here(p, SQLSTMT_REASON_TRAILING_TOKENS);
}

/* IF [NOT] EXISTS를 만나면 지원하지 않는다고 알린다. */
static bool reject_if_exists(parser_t *p)
{
    const lexer_token_t *token = current(p);
    const lexer_token_t *next = peek_next(p);
    if (word_is(token, "IF") && (word_is(next, "EXISTS") || word_is(next, "NOT"))) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, token->offset);
    }
    return true;
}

/* BEGIN [TRANSACTION], START TRANSACTION */
static bool parse_begin(parser_t *p, sqlstmt_t *statement, bool start)
{
    statement->kind = SQLSTMT_BEGIN;
    if (start) {
        if (!expect_word(p, "TRANSACTION")) return false;
    } else {
        take_word(p, "TRANSACTION");
    }
    return parse_end(p);
}

/* COMMIT [TRANSACTION], ROLLBACK [TRANSACTION] */
static bool parse_finish(parser_t *p, sqlstmt_t *statement, sqlstmt_kind_t kind)
{
    statement->kind = kind;
    take_word(p, "TRANSACTION");
    return parse_end(p);
}

static bool parse_drop(parser_t *p, sqlstmt_t *statement)
{
    statement->kind = SQLSTMT_DROP_TABLE;
    if (!expect_word(p, "TABLE") || !reject_if_exists(p)) return false;
    if (!parse_name(p, &statement->drop.table, false)) return false;
    return parse_end(p);
}

static bool parse_returning(parser_t *p, sqlstmt_returning_t *returning);

static bool parse_delete(parser_t *p, sqlstmt_t *statement)
{
    statement->kind = SQLSTMT_DELETE;
    if (!expect_word(p, "FROM")) return false;
    if (!parse_name(p, &statement->del.table, false)) return false;
    if (!parse_where(p, &statement->del.where)) return false;
    return parse_returning(p, &statement->del.returning) && parse_end(p);
}

/* [AS] alias. AS가 없으면 예약어가 아닌 이름만 별칭이다 */
static bool parse_alias(parser_t *p, sqlstmt_name_t *alias)
{
    if (take_word(p, "AS")) return parse_name(p, alias, true);
    const lexer_token_t *token = current(p);
    if (token != NULL && (token->kind == LEXER_QUOTED_IDENT ||
                          (token->kind == LEXER_WORD && !word_in(token, RESERVED) &&
                           !word_in(token, UNSUPPORTED_WORDS)))) {
        return parse_name(p, alias, true);
    }
    return true;
}

static bool parse_select_item(parser_t *p, sqlstmt_select_item_t *item)
{
    if (take_punct(p, "*")) {
        item->star = true;
        return true;
    }
    const lexer_token_t *token = current(p);
    if (token != NULL && (token->kind == LEXER_WORD || token->kind == LEXER_QUOTED_IDENT) &&
        punct_is(peek_next(p), ".") && p->pos + 2 < p->count && punct_is(&p->tokens[p->pos + 2], "*")) {
        item->star = true;
        if (!parse_name_part(p, &item->qualifier)) return false;
        p->pos += 2;
        return true;
    }
    return parse_expr(p, &item->expr) && parse_alias(p, &item->alias);
}

static bool parse_order(parser_t *p, sqlstmt_select_t *query)
{
    size_t capacity = 0;
    do {
        void *grown = reserve(p, query->order, &capacity, query->order_count, sizeof(*query->order), here_offset(p));
        if (grown == NULL) return false;
        query->order = grown;
        sqlstmt_order_t *key = &query->order[query->order_count];
        if (!parse_expr(p, &key->expr)) return false;
        query->order_count++;
        if (take_word(p, "DESC")) key->desc = true;
        else take_word(p, "ASC");
        if (word_is(current(p), "NULLS")) {
            p->pos++;
            if (word_is(current(p), "FIRST")) key->nulls = SQLSTMT_NULLS_FIRST;
            else if (word_is(current(p), "LAST")) key->nulls = SQLSTMT_NULLS_LAST;
            else return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
            p->pos++;
        }
    } while (take_punct(p, ","));
    return true;
}

static bool parse_returning(parser_t *p, sqlstmt_returning_t *returning)
{
    if (!take_word(p, "RETURNING")) return true;
    size_t capacity = 0;
    do {
        void *grown = reserve(p, returning->items, &capacity, returning->count, sizeof(*returning->items),
                              here_offset(p));
        if (grown == NULL) return false;
        returning->items = grown;
        if (!parse_select_item(p, &returning->items[returning->count++])) return false;
    } while (take_punct(p, ","));
    return true;
}

static bool parse_source(parser_t *p, sqlstmt_select_t *query, size_t *capacity, sqlstmt_join_t join)
{
    size_t offset = here_offset(p);
    if (query->source_count == SQLSTMT_MAX_SOURCES) return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_TOO_MANY_ITEMS, offset);
    void *grown = reserve(p, query->sources, capacity, query->source_count, sizeof(*query->sources), offset);
    if (grown == NULL) return false;
    query->sources = grown;
    sqlstmt_source_t *source = &query->sources[query->source_count++];
    memset(source, 0, sizeof(*source));
    source->join = join;
    if (punct_is(current(p), "(")) {
        if (!starts_query(peek_next(p))) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
        }
        p->pos++;
        if (!parse_subquery(p, &source->subquery) || !parse_alias(p, &source->alias)) return false;
    } else if (!parse_name(p, &source->table, false) || !parse_alias(p, &source->alias)) {
        return false;
    }
    if (join != SQLSTMT_JOIN_INNER && join != SQLSTMT_JOIN_LEFT && join != SQLSTMT_JOIN_RIGHT) return true;
    if (word_is(current(p), "USING")) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
    }
    return expect_word(p, "ON") && parse_expr(p, &source->on);
}

static bool parse_from(parser_t *p, sqlstmt_select_t *query)
{
    size_t capacity = 0;
    if (!parse_source(p, query, &capacity, SQLSTMT_JOIN_NONE)) return false;
    for (;;) {
        sqlstmt_join_t join;
        if (take_punct(p, ",")) {
            join = SQLSTMT_JOIN_CROSS;
        } else if (take_word(p, "CROSS")) {
            if (!expect_word(p, "JOIN")) return false;
            join = SQLSTMT_JOIN_CROSS;
        } else if (take_word(p, "INNER")) {
            if (!expect_word(p, "JOIN")) return false;
            join = SQLSTMT_JOIN_INNER;
        } else if (take_word(p, "JOIN")) {
            join = SQLSTMT_JOIN_INNER;
        } else if (take_word(p, "LEFT")) {
            take_word(p, "OUTER");
            if (!expect_word(p, "JOIN")) return false;
            join = SQLSTMT_JOIN_LEFT;
        } else if (word_is(current(p), "RIGHT")) {
            /* 결합은 왼쪽부터 쌓으므로 RIGHT는 첫 결합에서만 뒤집어 실행할 수 있다 */
            if (query->source_count != 1) {
                return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
            }
            p->pos++;
            take_word(p, "OUTER");
            if (!expect_word(p, "JOIN")) return false;
            join = SQLSTMT_JOIN_RIGHT;
        } else {
            return true;
        }
        if (!parse_source(p, query, &capacity, join)) return false;
    }
}

/* SELECT 핵심: 목록·FROM·WHERE·GROUP BY·HAVING */
static bool parse_select_core(parser_t *p, sqlstmt_select_t *query)
{
    query->distinct = take_word(p, "DISTINCT");
    size_t capacity = 0;
    do {
        void *grown = reserve(p, query->items, &capacity, query->item_count, sizeof(*query->items), here_offset(p));
        if (grown == NULL) return false;
        query->items = grown;
        if (!parse_select_item(p, &query->items[query->item_count++])) return false;
    } while (take_punct(p, ","));
    if (take_word(p, "FROM") && !parse_from(p, query)) return false;
    if (!parse_where(p, &query->where)) return false;
    if (take_word(p, "GROUP")) {
        if (!expect_word(p, "BY")) return false;
        size_t group_capacity = 0;
        do {
            void *grown = reserve(p, query->group_by, &group_capacity, query->group_count, sizeof(*query->group_by),
                                  here_offset(p));
            if (grown == NULL) return false;
            query->group_by = grown;
            if (!parse_expr(p, &query->group_by[query->group_count])) return false;
            query->group_count++;
        } while (take_punct(p, ","));
    }
    if (take_word(p, "HAVING")) {
        if (!parse_expr(p, &query->having)) return false;
    }
    return true;
}

/* UNION [ALL] | EXCEPT | INTERSECT. 아니면 false */
static bool take_set_op(parser_t *p, sqlstmt_set_op_t *op)
{
    if (take_word(p, "UNION")) {
        *op = take_word(p, "ALL") ? SQLSTMT_SET_UNION_ALL : SQLSTMT_SET_UNION;
    } else if (take_word(p, "EXCEPT")) {
        *op = SQLSTMT_SET_EXCEPT;
    } else if (take_word(p, "INTERSECT")) {
        *op = SQLSTMT_SET_INTERSECT;
    } else {
        return false;
    }
    return true;
}

/* SELECT 키워드 뒤의 질의: 핵심·집합 연산·ORDER BY·LIMIT·OFFSET */
static bool parse_query_body(parser_t *p, sqlstmt_select_t *query)
{
    if (!parse_select_core(p, query)) return false;
    sqlstmt_select_t *tail = query;
    bool has_intersect = false, has_other = false;
    sqlstmt_set_op_t op;
    for (size_t offset = here_offset(p); take_set_op(p, &op); offset = here_offset(p)) {
        if ((op == SQLSTMT_SET_EXCEPT || op == SQLSTMT_SET_INTERSECT) && word_is(current(p), "ALL")) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
        }
        if (op == SQLSTMT_SET_INTERSECT) has_intersect = true;
        else has_other = true;
        if (has_intersect && has_other) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, offset);
        }
        if (punct_is(current(p), "(")) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
        }
        if (!expect_word(p, "SELECT")) return false;
        sqlstmt_select_t *next = calloc(1, sizeof(*next));
        if (next == NULL) return nomem(p, offset);
        tail->next = next;
        tail->next_op = op;
        tail = next;
        if (!parse_select_core(p, next)) return false;
    }
    if (take_word(p, "ORDER")) {
        if (!expect_word(p, "BY") || !parse_order(p, query)) return false;
    }
    if (take_word(p, "LIMIT")) {
        if (!parse_expr(p, &query->limit)) return false;
    }
    if (take_word(p, "OFFSET")) {
        if (!parse_expr(p, &query->offset)) return false;
    }
    return true;
}

/* WITH 목록. WITH 키워드 뒤에서 부른다 */
static bool parse_ctes(parser_t *p, sqlstmt_select_t *query)
{
    if (word_is(current(p), "RECURSIVE")) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
    }
    size_t capacity = 0;
    do {
        size_t offset = here_offset(p);
        void *grown = reserve(p, query->ctes, &capacity, query->cte_count, sizeof(*query->ctes), offset);
        if (grown == NULL) return false;
        query->ctes = grown;
        sqlstmt_cte_t *cte = &query->ctes[query->cte_count++];
        if (!parse_name(p, &cte->name, false)) return false;
        for (size_t i = 0; i + 1 < query->cte_count; i++) {
            if (names_equal(&query->ctes[i].name, &cte->name)) {
                return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_COLUMN, offset);
            }
        }
        if (take_punct(p, "(")) {
            size_t column_capacity = 0;
            do {
                size_t at = here_offset(p);
                grown = reserve(p, cte->columns, &column_capacity, cte->column_count, sizeof(*cte->columns), at);
                if (grown == NULL) return false;
                cte->columns = grown;
                if (!parse_name(p, &cte->columns[cte->column_count++], false)) return false;
                for (size_t i = 0; i + 1 < cte->column_count; i++) {
                    if (names_equal(&cte->columns[i], &cte->columns[cte->column_count - 1])) {
                        return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_COLUMN, at);
                    }
                }
            } while (take_punct(p, ","));
            if (!expect_punct(p, ")")) return false;
        }
        if (!expect_word(p, "AS") || !expect_punct(p, "(")) return false;
        if (!starts_query(current(p))) return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
        if (!parse_subquery(p, &cte->query)) return false;
    } while (take_punct(p, ","));
    return true;
}

/* [WITH ...] SELECT ... */
static bool parse_query(parser_t *p, sqlstmt_select_t *query)
{
    if (take_word(p, "WITH") && !parse_ctes(p, query)) return false;
    if (!expect_word(p, "SELECT")) {
        if (query->cte_count > 0 && current(p) != NULL && current(p)->kind == LEXER_WORD) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
        }
        return false;
    }
    return parse_query_body(p, query);
}

/* ( 다음의 질의와 닫는 ). 여는 괄호는 호출자가 읽었다 */
static bool parse_subquery(parser_t *p, sqlstmt_select_t **out)
{
    if (!enter(p)) return false;
    sqlstmt_select_t *query = calloc(1, sizeof(*query));
    bool ok = query != NULL ? parse_query(p, query) && expect_punct(p, ")") : nomem(p, here_offset(p));
    p->depth--;
    *out = query;
    return ok;
}

static bool parse_select(parser_t *p, sqlstmt_t *statement)
{
    statement->kind = SQLSTMT_SELECT;
    return parse_query(p, &statement->select) && parse_end(p);
}

/* column = expr {, column = expr}. SET 뒤에서 부른다 */
static bool parse_assignments(parser_t *p, sqlstmt_assignment_t **assignments, size_t *count)
{
    size_t capacity = 0;
    do {
        size_t offset = here_offset(p);
        void *grown = reserve(p, *assignments, &capacity, *count, sizeof(**assignments), offset);
        if (grown == NULL) return false;
        *assignments = grown;
        sqlstmt_assignment_t *assignment = &(*assignments)[*count];
        if (!parse_name(p, &assignment->column, false)) return false;
        (*count)++;
        for (size_t i = 0; i + 1 < *count; i++) {
            if (names_equal(&(*assignments)[i].column, &assignment->column)) {
                return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_COLUMN, offset);
            }
        }
        if (!expect_punct(p, "=") || !parse_expr(p, &assignment->value)) return false;
    } while (take_punct(p, ","));
    return true;
}

static bool parse_update(parser_t *p, sqlstmt_t *statement)
{
    sqlstmt_update_t *update = &statement->update;
    statement->kind = SQLSTMT_UPDATE;
    if (!parse_name(p, &update->table, false) || !expect_word(p, "SET")) return false;
    if (!parse_assignments(p, &update->assignments, &update->assignment_count)) return false;
    if (!parse_where(p, &update->where)) return false;
    return parse_returning(p, &update->returning) && parse_end(p);
}

/* ON CONFLICT ... (ON은 호출자가 읽었다) */
static bool parse_conflict(parser_t *p, sqlstmt_conflict_t *conflict)
{
    conflict->present = true;
    if (!expect_word(p, "CONFLICT")) return false;
    if (take_punct(p, "(")) {
        conflict->has_target = true;
        if (!parse_name(p, &conflict->target, false)) return false;
        if (punct_is(current(p), ",")) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
        }
        if (!expect_punct(p, ")")) return false;
    }
    if (!expect_word(p, "DO")) return false;
    if (take_word(p, "NOTHING")) return true;
    size_t offset = here_offset(p);
    if (!expect_word(p, "UPDATE")) return false;
    /* 어느 행과 충돌했는지 알아야 갱신할 수 있다(PostgreSQL과 같음) */
    if (!conflict->has_target) return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_EXPECTED_KEYWORD, offset);
    conflict->do_update = true;
    if (!expect_word(p, "SET") || !parse_assignments(p, &conflict->assignments, &conflict->assignment_count)) {
        return false;
    }
    return parse_where(p, &conflict->where);
}

static bool parse_insert(parser_t *p, sqlstmt_t *statement)
{
    sqlstmt_insert_t *insert = &statement->insert;
    statement->kind = SQLSTMT_INSERT;
    if (!expect_word(p, "INTO") || !parse_name(p, &insert->table, false)) return false;
    size_t capacity = 0;
    if (take_punct(p, "(")) {
        insert->has_columns = true;
        do {
            size_t offset = here_offset(p);
            void *grown = reserve(p, insert->columns, &capacity, insert->column_count,
                                  sizeof(*insert->columns), offset);
            if (grown == NULL) return false;
            insert->columns = grown;
            sqlstmt_name_t *column = &insert->columns[insert->column_count];
            if (!parse_name(p, column, false)) return false;
            insert->column_count++;
            for (size_t i = 0; i + 1 < insert->column_count; i++) {
                if (names_equal(&insert->columns[i], column)) {
                    return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_COLUMN, offset);
                }
            }
        } while (take_punct(p, ","));
        if (!expect_punct(p, ")")) return false;
    }
    if (word_is(current(p), "SELECT")) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
    }
    if (!insert->has_columns && take_word(p, "DEFAULT")) {
        if (!expect_word(p, "VALUES")) return false;
        insert->has_columns = true;
        insert->row_count = 1;
        if (take_word(p, "ON") && !parse_conflict(p, &insert->conflict)) return false;
        return parse_returning(p, &insert->returning) && parse_end(p);
    }
    if (!expect_word(p, "VALUES")) return false;
    capacity = 0;
    size_t total = 0;
    do {
        if (!expect_punct(p, "(")) goto failed;
        size_t in_row = 0;
        do {
            void *grown = reserve_up_to(p, insert->values, &capacity, total, sizeof(*insert->values),
                                        here_offset(p), SQLSTMT_MAX_LIST);
            if (grown == NULL) goto failed;
            insert->values = grown;
            if (!parse_expr(p, &insert->values[total])) goto failed;
            total++;
            in_row++;
            if (insert->row_count == 0) insert->value_count = in_row;
        } while (take_punct(p, ","));
        size_t close = here_offset(p);
        if (!expect_punct(p, ")")) goto failed;
        if (in_row != insert->value_count || (insert->has_columns && insert->column_count != in_row)) {
            fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH, close);
            goto failed;
        }
        insert->row_count++;
    } while (take_punct(p, ","));
    if (take_word(p, "ON") && !parse_conflict(p, &insert->conflict)) return false;
    return parse_returning(p, &insert->returning) && parse_end(p);
failed:
    /* 해제는 row_count × value_count를 따르므로 끝나지 않은 행의 값은 여기서 지운다 */
    for (size_t i = insert->row_count * insert->value_count; i < total; i++) {
        expr_free(insert->values[i]);
        insert->values[i] = NULL;
    }
    return false;
}

/* VARCHAR ( n ). n은 자르거나 강제하지 않고 기록만 한다. */
static bool parse_varchar_length(parser_t *p, sqlstmt_column_def_t *column)
{
    if (!expect_punct(p, "(")) return false;
    const lexer_token_t *token = current(p);
    if (token == NULL || token->kind != LEXER_NUMBER) {
        return syntax_here(p, SQLSTMT_REASON_BAD_VARCHAR_LENGTH);
    }
    uint64_t length = 0;
    for (size_t i = 0; i < token->text_size; i++) {
        uint8_t c = token->text[i];
        if (c < '0' || c > '9') {
            return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_BAD_VARCHAR_LENGTH, token->offset);
        }
        length = length * 10 + (uint64_t)(c - '0');
        if (length > UINT32_MAX) {
            return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, token->offset);
        }
    }
    if (length == 0) {
        return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_BAD_VARCHAR_LENGTH, token->offset);
    }
    p->pos++;
    column->has_varchar_length = true;
    column->varchar_length = (uint32_t)length;
    return expect_punct(p, ")");
}

static bool parse_type(parser_t *p, sqlstmt_column_def_t *column)
{
    const lexer_token_t *token = current(p);
    if (token == NULL || token->kind != LEXER_WORD) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
    }
    if (word_is(token, "INTEGER")) {
        column->type = VALUE_INTEGER;
    } else if (word_is(token, "REAL")) {
        column->type = VALUE_REAL;
    } else if (word_is(token, "TEXT")) {
        column->type = VALUE_TEXT;
    } else if (word_is(token, "BLOB")) {
        column->type = VALUE_BLOB;
    } else if (word_is(token, "VARCHAR")) {
        column->type = VALUE_TEXT;
        p->pos++;
        return parse_varchar_length(p, column);
    } else if (word_in(token, RESERVED)) {
        return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
    } else {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_TYPE, token->offset);
    }
    p->pos++;
    return true;
}

/* 컬럼 제약. 기본 키가 이미 정해졌으면 중복이다. */
static bool parse_constraints(parser_t *p, sqlstmt_create_t *create, size_t index)
{
    sqlstmt_column_def_t *column = &create->columns[index];
    for (;;) {
        const lexer_token_t *token = current(p);
        if (token == NULL || punct_is(token, ",") || punct_is(token, ")")) return true;
        if (take_word(p, "NOT")) {
            if (!expect_word(p, "NULL")) return false;
            column->not_null = true;
        } else if (word_is(token, "PRIMARY")) {
            p->pos++;
            if (!expect_word(p, "KEY")) return false;
            if (create->primary_key_column != SQLSTMT_NO_COLUMN) {
                return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY,
                            token->offset);
            }
            create->primary_key_column = (uint16_t)index;
            const lexer_token_t *auto_token = current(p);
            if (word_is(auto_token, "AUTOINCREMENT")) {
                if (column->type != VALUE_INTEGER) {
                    return fail(p, SQLSTMT_SYNTAX,
                                SQLSTMT_REASON_AUTOINCREMENT_NOT_INTEGER_KEY, auto_token->offset);
                }
                create->has_identity = true;
                p->pos++;
            }
        } else if (word_is(token, "AUTOINCREMENT")) {
            return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_AUTOINCREMENT_NOT_INTEGER_KEY,
                        token->offset);
        } else if (word_is(token, "DEFAULT")) {
            p->pos++;
            if (column->default_value != NULL) return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_EXPECTED_KEYWORD, token->offset);
            /* 상수·부호 붙은 숫자·괄호 식(SQLite와 PostgreSQL이 함께 받는 꼴) */
            if (!parse_unary(p, &column->default_value)) return false;
        } else if (word_is(token, "UNIQUE") ||
                   word_is(token, "CHECK") || word_is(token, "REFERENCES") ||
                   word_is(token, "COLLATE") || word_is(token, "GENERATED") ||
                   word_is(token, "CONSTRAINT") || word_is(token, "NULL")) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT,
                        token->offset);
        } else {
            return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
        }
    }
}

/* 테이블 제약 PRIMARY KEY ( name ). 컬럼은 CREATE 전체를 읽은 뒤 찾는다. */
static bool parse_table_primary_key(parser_t *p, sqlstmt_name_t *name, size_t *offset)
{
    if (!expect_word(p, "KEY") || !expect_punct(p, "(")) return false;
    *offset = here_offset(p);
    if (!parse_name(p, name, false)) return false;
    if (punct_is(current(p), ",")) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT,
                    here_offset(p));
    }
    return expect_punct(p, ")");
}

static bool parse_create(parser_t *p, sqlstmt_t *statement)
{
    if (word_is(current(p), "UNIQUE") || word_is(current(p), "INDEX")) {
        sqlstmt_index_t *index = &statement->index;
        statement->kind = SQLSTMT_CREATE_INDEX;
        index->unique = take_word(p, "UNIQUE");
        if (!expect_word(p, "INDEX")) return false;
        if (!reject_if_exists(p)) return false;
        if (!parse_name(p, &index->name, false) || !expect_word(p, "ON") ||
            !parse_name(p, &index->table, false) || !expect_punct(p, "(")) return false;
        if (!parse_name(p, &index->column, false)) return false;
        if (punct_is(current(p), ",")) {
            return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT,
                        here_offset(p));
        }
        return expect_punct(p, ")");
    }
    sqlstmt_create_t *create = &statement->create;
    statement->kind = SQLSTMT_CREATE_TABLE;
    create->primary_key_column = SQLSTMT_NO_COLUMN;
    if (!word_is(current(p), "TABLE")) {
        if (current(p) == NULL) return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CREATE, current(p)->offset);
    }
    p->pos++;
    if (!reject_if_exists(p)) return false;
    if (!parse_name(p, &create->table, false) || !expect_punct(p, "(")) return false;

    sqlstmt_name_t table_key = {NULL, 0};
    size_t table_key_offset = 0;
    bool has_table_key = false;
    size_t capacity = 0;
    bool ok = true;
    do {
        const lexer_token_t *token = current(p);
        if (word_is(token, "PRIMARY")) {
            size_t constraint_offset = token->offset;
            p->pos++;
            if (has_table_key) {
                ok = fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY,
                          constraint_offset);
                break;
            }
            if (!parse_table_primary_key(p, &table_key, &table_key_offset)) {
                ok = false;
                break;
            }
            has_table_key = true;
            continue;
        }
        if (word_is(token, "UNIQUE") || word_is(token, "FOREIGN") || word_is(token, "CHECK") ||
            word_is(token, "CONSTRAINT")) {
            ok = fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT,
                      token->offset);
            break;
        }
        size_t offset = here_offset(p);
        void *grown = reserve(p, create->columns, &capacity, create->column_count,
                              sizeof(*create->columns), offset);
        if (grown == NULL) {
            ok = false;
            break;
        }
        create->columns = grown;
        sqlstmt_column_def_t *column = &create->columns[create->column_count];
        if (!parse_name(p, &column->name, false)) {
            ok = false;
            break;
        }
        size_t index = create->column_count++;
        for (size_t i = 0; i < index; i++) {
            if (names_equal(&create->columns[i].name, &column->name)) {
                ok = fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_COLUMN, offset);
                break;
            }
        }
        if (!ok) break;
        if (!parse_type(p, column) || !parse_constraints(p, create, index)) {
            ok = false;
            break;
        }
    } while (take_punct(p, ","));
    size_t close = here_offset(p);
    if (ok) ok = expect_punct(p, ")");
    if (ok && create->column_count == 0) {
        ok = fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_NO_COLUMNS, close);
    }
    if (ok && has_table_key) {
        if (create->primary_key_column != SQLSTMT_NO_COLUMN) {
            ok = fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY, table_key_offset);
        } else {
            for (size_t i = 0; i < create->column_count; i++) {
                if (names_equal(&create->columns[i].name, &table_key)) {
                    create->primary_key_column = (uint16_t)i;
                    break;
                }
            }
            if (create->primary_key_column == SQLSTMT_NO_COLUMN) {
                ok = fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_UNKNOWN_PRIMARY_KEY_COLUMN,
                          table_key_offset);
            }
        }
    }
    name_clear(&table_key);
    if (!ok) return false;
    if (word_is(current(p), "AS")) return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
    return parse_end(p);
}

size_t sqlstmt_parse_peak_bytes(const lexer_statement_t *statement, const lexer_binding_t *binding)
{
    if (statement == NULL || statement->tokens == NULL) return 0;
    size_t items = statement->token_count < CATALOG_MAX_COLUMNS ? statement->token_count : CATALOG_MAX_COLUMNS;
    if (items < 4) items = 4;
    /* INSERT는 이름 배열과 값 배열을 함께 쥔다. 다른 문장은 이 합보다 작다 */
    size_t element = sizeof(sqlstmt_name_t) + sizeof(sqlstmt_select_item_t) + sizeof(sqlstmt_order_t) +
                     sizeof(sqlstmt_source_t) + sizeof(sqlstmt_cte_t);
    if (sizeof(sqlstmt_column_def_t) > element) element = sizeof(sqlstmt_column_def_t);
    if (sizeof(sqlstmt_assignment_t) > element) element = sizeof(sqlstmt_assignment_t);
    size_t total = sizeof(sqlstmt_t) + 3 * items * element;
    /* 토큰마다 식 노드 하나와, 인자·값 목록의 포인터 자리(두 배씩 늘어 old+new가 최대 3배) */
    total += statement->token_count * (sizeof(sqlexpr_t) + 3 * sizeof(sqlexpr_t *));
    for (size_t i = 0; i < statement->token_count; i++) {
        const lexer_token_t *token = &statement->tokens[i];
        switch (token->kind) {
            case LEXER_WORD:
                /* 집합 연산 뒤의 SELECT마다 핵심 구조 하나 */
                if (word_is(token, "SELECT")) total += sizeof(sqlstmt_select_t);
                total += 2 * token->text_size;
                break;
            case LEXER_QUOTED_IDENT:
            case LEXER_STRING: total += 2 * token->text_size; break;
            case LEXER_NUMBER: total += token->text_size + 16; break;
            case LEXER_PARAMETER:
                if (binding != NULL && binding->values != NULL && token->parameter_id >= 1 &&
                    token->parameter_id <= binding->count) {
                    total += binding->values[token->parameter_id - 1].size_bytes;
                }
                break;
            default: break;
        }
    }
    return total;
}

sqlstmt_status_t sqlstmt_parse(const lexer_statement_t *statement, const lexer_binding_t *binding,
                               sqlstmt_t **out, sqlstmt_error_t *error)
{
    parser_t parser = {0};
    sqlstmt_status_t status;
    sqlstmt_t *built = NULL;
    if (statement == NULL || out == NULL || statement->tokens == NULL ||
        statement->token_count == 0) {
        parser.status = SQLSTMT_INVALID;
        parser.reason = SQLSTMT_REASON_BAD_ARGUMENT;
        goto done;
    }
    const lexer_token_t *last = &statement->tokens[statement->token_count - 1];
    parser.tokens = statement->tokens;
    parser.count = statement->token_count;
    parser.end_offset = last->offset + last->size;
    parser.binding = binding;

    built = calloc(1, sizeof(*built));
    if (built == NULL) {
        nomem(&parser, statement->offset);
        goto done;
    }
    const lexer_token_t *first = current(&parser);
    bool ok;
    if (word_is(first, "CREATE")) {
        parser.pos++;
        ok = parse_create(&parser, built);
    } else if (word_is(first, "INSERT")) {
        parser.pos++;
        ok = parse_insert(&parser, built);
    } else if (word_is(first, "SELECT") || word_is(first, "WITH")) {
        ok = parse_select(&parser, built);
    } else if (word_is(first, "UPDATE")) {
        parser.pos++;
        ok = parse_update(&parser, built);
    } else if (word_is(first, "DELETE")) {
        parser.pos++;
        ok = parse_delete(&parser, built);
    } else if (word_is(first, "DROP")) {
        parser.pos++;
        ok = parse_drop(&parser, built);
    } else if (word_is(first, "BEGIN") || word_is(first, "START")) {
        bool start = word_is(first, "START");
        parser.pos++;
        ok = parse_begin(&parser, built, start);
    } else if (word_is(first, "COMMIT")) {
        parser.pos++;
        ok = parse_finish(&parser, built, SQLSTMT_COMMIT);
    } else if (word_is(first, "ROLLBACK")) {
        parser.pos++;
        ok = parse_finish(&parser, built, SQLSTMT_ROLLBACK);
    } else {
        ok = fail(&parser, SQLSTMT_SYNTAX, SQLSTMT_REASON_UNKNOWN_STATEMENT, first->offset);
    }
    if (ok) {
        *out = built;
        built = NULL;
        parser.status = SQLSTMT_OK;
        parser.reason = SQLSTMT_REASON_NONE;
        parser.error_offset = 0;
    }

done:
    sqlstmt_free(built);
    status = parser.status;
    if (error != NULL) {
        error->reason = parser.reason;
        error->offset = parser.error_offset;
        error->value_status = parser.value_status;
    }
    return status;
}
