#include <stdlib.h>
#include <string.h>

#include "sql/lexer.h"

struct lexed {
    lexer_token_t *tokens;
    size_t token_count;
    lexer_statement_t *statements;
    size_t statement_count;
    uint8_t *arena;
    uint32_t parameter_count;
};

/* 같은 scan을 두 번 돈다. 첫 패스(출력 포인터 NULL)는 검증과 크기 계산만, 둘째 패스는 채우기만 한다. */
typedef struct {
    const uint8_t *in;
    size_t size;
    const lexer_limits_t *limits;
    lexer_token_t *tokens;
    lexer_statement_t *statements;
    uint8_t *arena;
    uint8_t *seen;
    size_t token_count;
    size_t statement_count;
    size_t arena_bytes;
    size_t statement_first;
    size_t statement_offset;
    bool statement_open;
    uint32_t auto_count;
    uint32_t unique_ids;
    uint32_t max_id;
    size_t max_id_offset;
    bool auto_mode;
    bool explicit_mode;
    lexer_status_t status;
    lexer_reason_t reason;
    size_t error_offset;
} scan_t;

static bool fail(scan_t *s, lexer_status_t status, lexer_reason_t reason, size_t offset)
{
    s->status = status;
    s->reason = reason;
    s->error_offset = offset;
    return false;
}

static bool syntax(scan_t *s, lexer_reason_t reason, size_t offset)
{
    return fail(s, LEXER_SYNTAX, reason, offset);
}

static bool utf8_valid(const uint8_t *bytes, size_t size)
{
    size_t i = 0;
    while (i < size) {
        uint8_t lead = bytes[i];
        size_t extra;
        uint32_t code;
        if (lead < 0x80) {
            i++;
            continue;
        }
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
            code = lead & 0x1fu;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2;
            code = lead & 0x0fu;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (size - i <= extra) return false;
        for (size_t k = 1; k <= extra; k++) {
            if ((bytes[i + k] & 0xc0u) != 0x80u) return false;
            code = code << 6 | (bytes[i + k] & 0x3fu);
        }
        if (extra == 2 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff))) return false;
        if (extra == 3 && (code < 0x10000 || code > 0x10ffff)) return false;
        i += extra + 1;
    }
    return true;
}

static bool is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_word_start(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_word_part(uint8_t c)
{
    return is_word_start(c) || is_digit(c);
}

static bool emit(scan_t *s, lexer_token_kind_t kind, size_t offset, size_t size,
                 const uint8_t *text, size_t text_size, uint32_t parameter_id)
{
    if (size > s->limits->max_token_bytes) {
        return fail(s, LEXER_LIMIT, LEXER_REASON_TOKEN_TOO_LARGE, offset);
    }
    if (s->token_count >= s->limits->max_tokens) {
        return fail(s, LEXER_LIMIT, LEXER_REASON_TOO_MANY_TOKENS, offset);
    }
    if (s->tokens != NULL) {
        s->tokens[s->token_count] = (lexer_token_t){
            kind, offset, s->in + offset, size, text != NULL ? text : s->in + offset,
            text != NULL ? text_size : size, parameter_id};
    }
    s->token_count++;
    return true;
}

static void open_statement(scan_t *s, size_t offset)
{
    if (s->statement_open) return;
    s->statement_open = true;
    s->statement_first = s->token_count;
    s->statement_offset = offset;
}

static void close_statement(scan_t *s, size_t end_offset, bool terminated)
{
    if (s->statements != NULL) {
        s->statements[s->statement_count] = (lexer_statement_t){
            s->tokens + s->statement_first, s->token_count - s->statement_first,
            s->statement_offset, end_offset, terminated};
    }
    s->statement_count++;
    s->statement_open = false;
}

static bool skip_line_comment(scan_t *s, size_t *at)
{
    size_t end = *at + 2;
    while (end < s->size && s->in[end] != '\n') end++;
    const uint8_t *body = s->in + *at + 2;
    size_t length = end - (*at + 2);
    if (length > 0 && memchr(body, 0, length) != NULL) {
        return syntax(s, LEXER_REASON_UNEXPECTED_BYTE, *at + 2 + (size_t)((const uint8_t *)memchr(body, 0, length) - body));
    }
    if (!utf8_valid(body, length)) return syntax(s, LEXER_REASON_INVALID_UTF8, *at + 2);
    *at = end;
    return true;
}

static bool skip_block_comment(scan_t *s, size_t *at)
{
    size_t end = *at + 2;
    while (end + 1 < s->size && !(s->in[end] == '*' && s->in[end + 1] == '/')) end++;
    if (end + 1 >= s->size) return syntax(s, LEXER_REASON_UNTERMINATED_COMMENT, *at);
    const uint8_t *body = s->in + *at + 2;
    size_t length = end - (*at + 2);
    if (length > 0 && memchr(body, 0, length) != NULL) {
        return syntax(s, LEXER_REASON_UNEXPECTED_BYTE, *at + 2 + (size_t)((const uint8_t *)memchr(body, 0, length) - body));
    }
    if (!utf8_valid(body, length)) return syntax(s, LEXER_REASON_INVALID_UTF8, *at + 2);
    *at = end + 2;
    return true;
}

/* 따옴표로 감싼 문자열·식별자. 같은 따옴표 두 개는 하나로 풀며 escape가 있을 때만 arena에 복사한다. */
static bool scan_quoted(scan_t *s, size_t *at, uint8_t quote, lexer_token_kind_t kind)
{
    size_t start = *at, end = start + 1, doubled = 0;
    lexer_reason_t unterminated = kind == LEXER_STRING ? LEXER_REASON_UNTERMINATED_STRING
                                                       : LEXER_REASON_UNTERMINATED_IDENT;
    for (;;) {
        if (end >= s->size) return syntax(s, unterminated, start);
        if (s->in[end] == quote) {
            if (end + 1 < s->size && s->in[end + 1] == quote) {
                doubled++;
                end += 2;
                continue;
            }
            break;
        }
        end++;
    }
    const uint8_t *body = s->in + start + 1;
    size_t length = end - start - 1;
    if (!utf8_valid(body, length)) return syntax(s, LEXER_REASON_INVALID_UTF8, start + 1);
    if (kind == LEXER_QUOTED_IDENT) {
        if (length == 0) return syntax(s, LEXER_REASON_EMPTY_IDENT, start);
        const uint8_t *nul = memchr(body, 0, length);
        if (nul != NULL) return syntax(s, LEXER_REASON_UNEXPECTED_BYTE, (size_t)(nul - s->in));
    }
    size_t text_size = length - doubled;
    const uint8_t *text = body;
    if (doubled > 0) {
        if (s->arena != NULL) {
            uint8_t *copy = s->arena + s->arena_bytes;
            for (size_t i = 0, o = 0; i < length; i++) {
                copy[o++] = body[i];
                if (body[i] == quote) i++;
            }
            text = copy;
        } else {
            text = NULL;
        }
        s->arena_bytes += text_size;
    }
    open_statement(s, start);
    if (!emit(s, kind, start, end + 1 - start, text, text_size, 0)) return false;
    *at = end + 1;
    return true;
}

static bool scan_number(scan_t *s, size_t *at)
{
    const uint8_t *in = s->in;
    size_t start = *at, end = start, n = s->size;
    while (end < n && is_digit(in[end])) end++;
    if (end < n && in[end] == '.') {
        size_t fraction = end + 1;
        if (fraction >= n || !is_digit(in[fraction])) return syntax(s, LEXER_REASON_BAD_NUMBER, start);
        while (fraction < n && is_digit(in[fraction])) fraction++;
        end = fraction;
    }
    if (end < n && (in[end] == 'e' || in[end] == 'E')) {
        size_t exponent = end + 1;
        if (exponent < n && (in[exponent] == '+' || in[exponent] == '-')) exponent++;
        if (exponent >= n || !is_digit(in[exponent])) return syntax(s, LEXER_REASON_BAD_NUMBER, start);
        while (exponent < n && is_digit(in[exponent])) exponent++;
        end = exponent;
    }
    if (end < n && (is_word_part(in[end]) || in[end] == '.')) {
        return syntax(s, LEXER_REASON_BAD_NUMBER, start);
    }
    open_statement(s, start);
    if (!emit(s, LEXER_NUMBER, start, end - start, NULL, 0, 0)) return false;
    *at = end;
    return true;
}

static bool note_explicit_id(scan_t *s, uint32_t id, size_t offset)
{
    if (s->auto_mode) return syntax(s, LEXER_REASON_PARAMETER_MIX, offset);
    s->explicit_mode = true;
    if ((s->seen[id / 8] & (1u << (id % 8))) == 0) {
        s->seen[id / 8] |= (uint8_t)(1u << (id % 8));
        s->unique_ids++;
    }
    if (id > s->max_id) {
        s->max_id = id;
        s->max_id_offset = offset;
    }
    return true;
}

static bool scan_parameter(scan_t *s, size_t *at)
{
    const uint8_t *in = s->in;
    size_t start = *at, end = start + 1, n = s->size;
    uint32_t id;
    if (end < n && is_digit(in[end])) {
        if (in[end] == '0') return syntax(s, LEXER_REASON_BAD_PARAMETER, start);
        uint64_t value = 0;
        for (; end < n && is_digit(in[end]); end++) {
            value = value * 10 + (uint64_t)(in[end] - '0');
            if (value > s->limits->max_parameters) {
                return fail(s, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, start);
            }
        }
        if (end < n && is_word_part(in[end])) return syntax(s, LEXER_REASON_BAD_PARAMETER, start);
        id = (uint32_t)value;
        if (!note_explicit_id(s, id, start)) return false;
    } else {
        if (end < n && is_word_start(in[end])) return syntax(s, LEXER_REASON_BAD_PARAMETER, start);
        if (s->explicit_mode) return syntax(s, LEXER_REASON_PARAMETER_MIX, start);
        if (s->auto_count >= s->limits->max_parameters) {
            return fail(s, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, start);
        }
        s->auto_mode = true;
        id = ++s->auto_count;
        s->unique_ids = id;
        s->max_id = id;
    }
    open_statement(s, start);
    if (!emit(s, LEXER_PARAMETER, start, end - start, NULL, 0, id)) return false;
    *at = end;
    return true;
}

static bool scan_punct(scan_t *s, size_t *at)
{
    const uint8_t *in = s->in;
    size_t start = *at, length = 1;
    uint8_t next = start + 1 < s->size ? in[start + 1] : 0;
    switch (in[start]) {
        case '(': case ')': case ',': case '.': case '*': case '=':
        case '+': case '-': case '/': case '%':
            break;
        case '<':
            if (next == '=' || next == '>') length = 2;
            break;
        case '>':
            if (next == '=') length = 2;
            break;
        case '!':
            if (next != '=') return syntax(s, LEXER_REASON_UNEXPECTED_BYTE, start);
            length = 2;
            break;
        default:
            return syntax(s, LEXER_REASON_UNEXPECTED_BYTE, start);
    }
    open_statement(s, start);
    if (!emit(s, LEXER_PUNCT, start, length, NULL, 0, 0)) return false;
    *at = start + length;
    return true;
}

static bool scan_semicolon(scan_t *s, size_t *at)
{
    if (!s->statement_open) return syntax(s, LEXER_REASON_EMPTY_STATEMENT, *at);
    close_statement(s, *at + 1, true);
    if (!emit(s, LEXER_SEMICOLON, *at, 1, NULL, 0, 0)) return false;
    (*at)++;
    return true;
}

static bool scan_word(scan_t *s, size_t *at)
{
    size_t start = *at, end = start + 1;
    while (end < s->size && is_word_part(s->in[end])) end++;
    open_statement(s, start);
    if (!emit(s, LEXER_WORD, start, end - start, NULL, 0, 0)) return false;
    *at = end;
    return true;
}

static bool scan_all(scan_t *s)
{
    const uint8_t *in = s->in;
    size_t n = s->size, at = 0;
    while (at < n) {
        uint8_t c = in[at];
        bool ok;
        if (is_space(c)) {
            at++;
            continue;
        }
        if (c == '-' && at + 1 < n && in[at + 1] == '-') ok = skip_line_comment(s, &at);
        else if (c == '/' && at + 1 < n && in[at + 1] == '*') ok = skip_block_comment(s, &at);
        else if (c == '\'') ok = scan_quoted(s, &at, '\'', LEXER_STRING);
        else if (c == '"') ok = scan_quoted(s, &at, '"', LEXER_QUOTED_IDENT);
        else if (is_word_start(c)) ok = scan_word(s, &at);
        else if (is_digit(c)) ok = scan_number(s, &at);
        else if (c == '?') ok = scan_parameter(s, &at);
        else if (c == ';') ok = scan_semicolon(s, &at);
        else ok = scan_punct(s, &at);
        if (!ok) return false;
    }
    if (s->statement_open) close_statement(s, n, false);
    if (s->statement_count == 0) return syntax(s, LEXER_REASON_EMPTY_STATEMENT, n);
    if (s->explicit_mode && s->unique_ids != s->max_id) {
        return syntax(s, LEXER_REASON_PARAMETER_GAP, s->max_id_offset);
    }
    return true;
}

static void reset_pass(scan_t *s, size_t seen_bytes)
{
    s->token_count = 0;
    s->statement_count = 0;
    s->arena_bytes = 0;
    s->statement_open = false;
    s->auto_count = 0;
    s->unique_ids = 0;
    s->max_id = 0;
    s->auto_mode = false;
    s->explicit_mode = false;
    memset(s->seen, 0, seen_bytes);
}

static lexer_status_t finish_error(const scan_t *s, lexer_error_t *error)
{
    if (error != NULL) *error = (lexer_error_t){s->reason, s->error_offset};
    return s->status;
}

lexer_status_t lexer_scan(const uint8_t *input, size_t size, const lexer_limits_t *limits,
                          lexed_t **out, lexer_error_t *error)
{
    if (error != NULL) *error = (lexer_error_t){LEXER_REASON_NONE, 0};
    if (out == NULL || limits == NULL || (input == NULL && size > 0)) return LEXER_INVALID;
    if (limits->max_input_bytes == 0 || limits->max_token_bytes == 0 || limits->max_tokens == 0 ||
        limits->max_parameters > LEXER_PARAMETER_CEILING) {
        return LEXER_INVALID;
    }
    if (size > limits->max_input_bytes) {
        if (error != NULL) *error = (lexer_error_t){LEXER_REASON_INPUT_TOO_LARGE, limits->max_input_bytes};
        return LEXER_LIMIT;
    }
    size_t seen_bytes = limits->max_parameters / 8 + 1;
    scan_t scan = {.in = input, .size = size, .limits = limits};
    scan.seen = malloc(seen_bytes);
    if (scan.seen == NULL) return LEXER_NOMEM;
    reset_pass(&scan, seen_bytes);
    if (!scan_all(&scan)) {
        free(scan.seen);
        return finish_error(&scan, error);
    }

    lexed_t *lexed = malloc(sizeof(*lexed));
    lexer_token_t *tokens = malloc(scan.token_count * sizeof(*tokens));
    lexer_statement_t *statements = malloc(scan.statement_count * sizeof(*statements));
    uint8_t *arena = scan.arena_bytes > 0 ? malloc(scan.arena_bytes) : NULL;
    if (lexed == NULL || tokens == NULL || statements == NULL ||
        (scan.arena_bytes > 0 && arena == NULL)) {
        free(lexed);
        free(tokens);
        free(statements);
        free(arena);
        free(scan.seen);
        return LEXER_NOMEM;
    }
    scan.tokens = tokens;
    scan.statements = statements;
    scan.arena = arena;
    reset_pass(&scan, seen_bytes);
    bool filled = scan_all(&scan);
    free(scan.seen);
    if (!filled) {
        free(lexed);
        free(tokens);
        free(statements);
        free(arena);
        return LEXER_INVALID;
    }
    *lexed = (lexed_t){tokens, scan.token_count, statements, scan.statement_count, arena,
                       scan.explicit_mode ? scan.max_id : scan.auto_count};
    *out = lexed;
    return LEXER_OK;
}

void lexer_free(lexed_t *lexed)
{
    if (lexed == NULL) return;
    free(lexed->tokens);
    free(lexed->statements);
    free(lexed->arena);
    free(lexed);
}

size_t lexed_token_count(const lexed_t *lexed)
{
    return lexed == NULL ? 0 : lexed->token_count;
}

const lexer_token_t *lexed_tokens(const lexed_t *lexed)
{
    return lexed == NULL ? NULL : lexed->tokens;
}

size_t lexed_statement_count(const lexed_t *lexed)
{
    return lexed == NULL ? 0 : lexed->statement_count;
}

lexer_status_t lexed_statement_at(const lexed_t *lexed, size_t index, lexer_statement_t *out)
{
    if (lexed == NULL || out == NULL || index >= lexed->statement_count) return LEXER_INVALID;
    *out = lexed->statements[index];
    return LEXER_OK;
}

uint32_t lexed_parameter_count(const lexed_t *lexed)
{
    return lexed == NULL ? 0 : lexed->parameter_count;
}

lexer_status_t lexer_check_binding(const lexed_t *lexed, const lexer_binding_t *binding)
{
    if (lexed == NULL || binding == NULL) return LEXER_INVALID;
    if (binding->count != lexed->parameter_count) return LEXER_PARAMETER_COUNT;
    if (binding->count > 0 && binding->values == NULL) return LEXER_INVALID;
    return LEXER_OK;
}

lexer_status_t lexer_binding_value(const lexer_binding_t *binding, uint32_t id,
                                   const value_view_t **view)
{
    if (binding == NULL || view == NULL || binding->values == NULL) return LEXER_INVALID;
    if (id == 0 || id > binding->count) return LEXER_INVALID;
    *view = &binding->values[id - 1];
    return LEXER_OK;
}
