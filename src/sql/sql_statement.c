#include <stdlib.h>
#include <string.h>

#include "sql/sql_statement.h"
#include "storage/catalog.h"

#define NUMBER_BUFFER_BYTES 4097u

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
} parser_t;

/* 이름 자리에 쓸 수 없는 예약어. 따옴표로 감싸면 쓸 수 있다. */
static const char *const RESERVED[] = {
    "CREATE", "TABLE", "INSERT", "INTO", "VALUES", "SELECT", "FROM", "WHERE", "UPDATE",
    "SET", "DELETE", "DROP", "NOT", "NULL", "PRIMARY", "KEY", "AUTOINCREMENT", "IS",
    "ORDER", "BY", "ASC", "DESC", "LIMIT", NULL};

/* 알지만 지원하지 않는 절·연산자. 이름 자리나 문장 끝에서 만나면 UNSUPPORTED다. */
static const char *const UNSUPPORTED_WORDS[] = {
    "DISTINCT", "ALL", "ORDER", "GROUP", "HAVING", "LIMIT", "OFFSET", "JOIN", "INNER", "LEFT",
    "RIGHT", "FULL", "CROSS", "NATURAL", "UNION", "EXCEPT", "INTERSECT", "RETURNING", "AS",
    "ON", "USING", NULL};

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
    if (word_is(token, "AND") || word_is(token, "OR")) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CONDITION, token->offset);
    }
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
static void *reserve(parser_t *p, void *array, size_t *capacity, size_t count, size_t element,
                     size_t offset)
{
    if (count >= CATALOG_MAX_COLUMNS) {
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

static void where_clear(sqlstmt_where_t *where)
{
    name_clear(&where->column);
    value_clear(&where->value);
}

static void names_clear(sqlstmt_name_t *names, size_t count)
{
    for (size_t i = 0; i < count; i++) name_clear(&names[i]);
    free(names);
}

void sqlstmt_free(sqlstmt_t *statement)
{
    if (statement == NULL) return;
    switch (statement->kind) {
        case SQLSTMT_CREATE_TABLE:
            for (size_t i = 0; i < statement->create.column_count; i++) {
                name_clear(&statement->create.columns[i].name);
            }
            free(statement->create.columns);
            name_clear(&statement->create.table);
            break;
        case SQLSTMT_INSERT:
            names_clear(statement->insert.columns, statement->insert.column_count);
            for (size_t i = 0; i < statement->insert.value_count; i++) {
                value_clear(&statement->insert.values[i]);
            }
            free(statement->insert.values);
            name_clear(&statement->insert.table);
            break;
        case SQLSTMT_SELECT:
            names_clear(statement->select.columns, statement->select.column_count);
            where_clear(&statement->select.where);
            name_clear(&statement->select.order_column);
            name_clear(&statement->select.table);
            break;
        case SQLSTMT_UPDATE:
            for (size_t i = 0; i < statement->update.assignment_count; i++) {
                name_clear(&statement->update.assignments[i].column);
                value_clear(&statement->update.assignments[i].value);
            }
            free(statement->update.assignments);
            where_clear(&statement->update.where);
            name_clear(&statement->update.table);
            break;
        case SQLSTMT_DELETE:
            where_clear(&statement->del.where);
            name_clear(&statement->del.table);
            break;
        case SQLSTMT_DROP_TABLE:
            name_clear(&statement->drop.table);
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

static bool parse_where(parser_t *p, sqlstmt_where_t *where)
{
    if (!take_word(p, "WHERE")) return true;
    if (!parse_name(p, &where->column, true)) return false;
    where->present = true;
    const lexer_token_t *token = current(p);
    static const char *const OPERATORS[] = {"=", "!=", "<>", "<", "<=", ">", ">="};
    static const sqlstmt_op_t OPS[] = {SQLSTMT_OP_EQ, SQLSTMT_OP_NE, SQLSTMT_OP_NE, SQLSTMT_OP_LT,
                                       SQLSTMT_OP_LE, SQLSTMT_OP_GT, SQLSTMT_OP_GE};
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (punct_is(token, OPERATORS[i])) {
            p->pos++;
            where->op = OPS[i];
            return parse_value(p, &where->value);
        }
    }
    if (take_word(p, "IS")) {
        bool negated = take_word(p, "NOT");
        if (!expect_word(p, "NULL")) return false;
        where->op = negated ? SQLSTMT_OP_IS_NOT_NULL : SQLSTMT_OP_IS_NULL;
        return true;
    }
    return syntax_here(p, SQLSTMT_REASON_EXPECTED_KEYWORD);
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

static bool parse_drop(parser_t *p, sqlstmt_t *statement)
{
    statement->kind = SQLSTMT_DROP_TABLE;
    if (!expect_word(p, "TABLE") || !reject_if_exists(p)) return false;
    if (!parse_name(p, &statement->drop.table, false)) return false;
    return parse_end(p);
}

static bool parse_delete(parser_t *p, sqlstmt_t *statement)
{
    statement->kind = SQLSTMT_DELETE;
    if (!expect_word(p, "FROM")) return false;
    if (!parse_name(p, &statement->del.table, false)) return false;
    if (!parse_where(p, &statement->del.where)) return false;
    return parse_end(p);
}

static bool parse_select(parser_t *p, sqlstmt_t *statement)
{
    sqlstmt_select_t *query = &statement->select;
    statement->kind = SQLSTMT_SELECT;
    size_t capacity = 0;
    if (take_punct(p, "*")) {
        query->select_all = true;
    } else {
        do {
            void *grown = reserve(p, query->columns, &capacity, query->column_count,
                                  sizeof(*query->columns), here_offset(p));
            if (grown == NULL) return false;
            query->columns = grown;
            if (!parse_name(p, &query->columns[query->column_count], true)) return false;
            query->column_count++;
        } while (take_punct(p, ","));
    }
    if (!expect_word(p, "FROM")) return false;
    if (!parse_name(p, &query->table, false)) return false;
    if (!parse_where(p, &query->where)) return false;
    if (take_word(p, "ORDER")) {
        query->has_order = true;
        if (!expect_word(p, "BY") || !parse_name(p, &query->order_column, true)) return false;
        if (take_word(p, "DESC")) query->order_desc = true;
        else take_word(p, "ASC");
    }
    if (take_word(p, "LIMIT")) {
        size_t offset = here_offset(p);
        value_t count = {0};
        if (!parse_value(p, &count)) return false;
        if (count.view.type != VALUE_INTEGER || count.view.integer < 0 ||
            (uint64_t)count.view.integer > SIZE_MAX) {
            value_clear(&count);
            return fail(p, SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, offset);
        }
        query->has_limit = true;
        query->limit = (size_t)count.view.integer;
        value_clear(&count);
    }
    return parse_end(p);
}

static bool parse_update(parser_t *p, sqlstmt_t *statement)
{
    sqlstmt_update_t *update = &statement->update;
    statement->kind = SQLSTMT_UPDATE;
    if (!parse_name(p, &update->table, false) || !expect_word(p, "SET")) return false;
    size_t capacity = 0;
    do {
        size_t offset = here_offset(p);
        void *grown = reserve(p, update->assignments, &capacity, update->assignment_count,
                              sizeof(*update->assignments), offset);
        if (grown == NULL) return false;
        update->assignments = grown;
        sqlstmt_assignment_t *assignment = &update->assignments[update->assignment_count];
        if (!parse_name(p, &assignment->column, false)) return false;
        update->assignment_count++;
        for (size_t i = 0; i + 1 < update->assignment_count; i++) {
            if (names_equal(&update->assignments[i].column, &assignment->column)) {
                return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_DUPLICATE_COLUMN, offset);
            }
        }
        if (!expect_punct(p, "=") || !parse_value(p, &assignment->value)) return false;
    } while (take_punct(p, ","));
    if (!parse_where(p, &update->where)) return false;
    return parse_end(p);
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
    if (!expect_word(p, "VALUES") || !expect_punct(p, "(")) return false;
    capacity = 0;
    do {
        void *grown = reserve(p, insert->values, &capacity, insert->value_count,
                              sizeof(*insert->values), here_offset(p));
        if (grown == NULL) return false;
        insert->values = grown;
        if (!parse_value(p, &insert->values[insert->value_count])) return false;
        insert->value_count++;
    } while (take_punct(p, ","));
    size_t close = here_offset(p);
    if (!expect_punct(p, ")")) return false;
    if (insert->has_columns && insert->column_count != insert->value_count) {
        return fail(p, SQLSTMT_SYNTAX, SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH, close);
    }
    if (punct_is(current(p), ",")) {
        return fail(p, SQLSTMT_UNSUPPORTED, SQLSTMT_REASON_UNSUPPORTED_CLAUSE, here_offset(p));
    }
    return parse_end(p);
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
        } else if (word_is(token, "UNIQUE") || word_is(token, "DEFAULT") ||
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
    return parse_end(p);
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
    } else if (word_is(first, "SELECT")) {
        parser.pos++;
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
