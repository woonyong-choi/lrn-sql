#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/lexer.h"

static const lexer_limits_t LIMITS = {1u << 20, 1u << 20, 1u << 16, 100};

static lexed_t *scan(const void *sql, size_t size)
{
    lexed_t *lexed = NULL;
    lexer_error_t error;
    assert(lexer_scan(sql, size, &LIMITS, &lexed, &error) == LEXER_OK);
    assert(error.reason == LEXER_REASON_NONE && lexed != NULL);
    return lexed;
}

static lexed_t *scan_text(const char *sql)
{
    return scan(sql, strlen(sql));
}

/* 실패하면 *out과 할당은 그대로이고 reason·offset이 맞다. */
static void expect_fail_with(const void *sql, size_t size, const lexer_limits_t *limits,
                             lexer_status_t status, lexer_reason_t reason, size_t offset)
{
    static char marker;
    lexed_t *out = (lexed_t *)&marker;
    lexer_error_t error = {LEXER_REASON_NONE, 12345};
    assert(lexer_scan(sql, size, limits, &out, &error) == status);
    assert(out == (lexed_t *)&marker);
    if (error.reason != reason || error.offset != offset) {
        fprintf(stderr, "input %.40s: reason %d offset %zu, expected reason %d offset %zu\n",
                (const char *)sql, error.reason, error.offset, reason, offset);
    }
    assert(error.reason == reason && error.offset == offset);
}

static void expect_syntax(const char *sql, lexer_reason_t reason, size_t offset)
{
    expect_fail_with(sql, strlen(sql), &LIMITS, LEXER_SYNTAX, reason, offset);
}

static const lexer_token_t *token(const lexed_t *lexed, size_t index)
{
    assert(index < lexed_token_count(lexed));
    return &lexed_tokens(lexed)[index];
}

static void assert_token(const lexed_t *lexed, size_t index, lexer_token_kind_t kind,
                         const char *text)
{
    const lexer_token_t *t = token(lexed, index);
    assert(t->kind == kind);
    assert(t->text_size == strlen(text) && memcmp(t->text, text, t->text_size) == 0);
}

static bool inside(const void *pointer, const void *base, size_t size)
{
    return (const uint8_t *)pointer >= (const uint8_t *)base &&
           (const uint8_t *)pointer <= (const uint8_t *)base + size;
}

/* #37: 토큰 종류·값·위치와 입력 span 수명 */
static void tokens_kinds_and_spans(void)
{
    const char *sql = "SELECT a, \"B é\" FROM t WHERE x <= 1.5e-3 AND y <> 'it''s' AND z != 2;";
    size_t size = strlen(sql);
    lexed_t *lexed = scan_text(sql);
    assert(lexed_statement_count(lexed) == 1);
    const char *expected[] = {"SELECT", "a", ",", "B é", "FROM", "t", "WHERE", "x", "<=", "1.5e-3",
                              "AND", "y", "<>", "it's", "AND", "z", "!=", "2", ";"};
    lexer_token_kind_t kinds[] = {LEXER_WORD, LEXER_WORD, LEXER_PUNCT, LEXER_QUOTED_IDENT,
                                  LEXER_WORD, LEXER_WORD, LEXER_WORD, LEXER_WORD, LEXER_PUNCT,
                                  LEXER_NUMBER, LEXER_WORD, LEXER_WORD, LEXER_PUNCT, LEXER_STRING,
                                  LEXER_WORD, LEXER_WORD, LEXER_PUNCT, LEXER_NUMBER, LEXER_SEMICOLON};
    assert(lexed_token_count(lexed) == sizeof(kinds) / sizeof(kinds[0]));
    for (size_t i = 0; i < lexed_token_count(lexed); i++) {
        assert_token(lexed, i, kinds[i], expected[i]);
        const lexer_token_t *t = token(lexed, i);
        assert(t->bytes == (const uint8_t *)sql + t->offset);
        assert(inside(t->bytes + t->size, sql, size));
    }
    /* escape가 없으면 입력 span, 있으면 소유 arena다. */
    const lexer_token_t *plain = token(lexed, 3), *escaped = token(lexed, 13);
    assert(plain->text == plain->bytes + 1 && plain->text_size + 2 == plain->size);
    assert(escaped->size == 7 && escaped->text_size == 4);
    assert(!inside(escaped->text, sql, size));
    lexer_statement_t statement;
    assert(lexed_statement_at(lexed, 0, &statement) == LEXER_OK);
    assert(statement.token_count == 18 && statement.terminated);
    assert(statement.offset == 0 && statement.end_offset == size);
    assert(lexed_statement_at(lexed, 1, &statement) == LEXER_INVALID);
    lexer_free(lexed);
    lexer_free(NULL);
}

static void escapes_and_empty_values(void)
{
    lexed_t *lexed = scan_text("'' '''' 'a''''b' \"a\"\"b\" \"\"\"\"");
    assert_token(lexed, 0, LEXER_STRING, "");
    assert_token(lexed, 1, LEXER_STRING, "'");
    assert_token(lexed, 2, LEXER_STRING, "a''b");
    assert_token(lexed, 3, LEXER_QUOTED_IDENT, "a\"b");
    assert_token(lexed, 4, LEXER_QUOTED_IDENT, "\"");
    assert(token(lexed, 1)->size == 4 && token(lexed, 2)->size == 8);
    lexer_free(lexed);
    expect_syntax("\"\"", LEXER_REASON_EMPTY_IDENT, 0);
    expect_syntax("select \"\" from t", LEXER_REASON_EMPTY_IDENT, 7);
}

/* 페이지보다 긴 문자열은 잘리지 않고 한도를 넘으면 줄이지 않고 거절한다. */
static void long_strings_are_whole(void)
{
    size_t sizes[] = {1, 4095, 4096, 8191, 8192, 8193, 16384, 100000};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t length = sizes[i];
        char *sql = malloc(length + 3);
        sql[0] = '\'';
        for (size_t k = 0; k < length; k++) sql[1 + k] = (char)('a' + k % 26);
        sql[length + 1] = '\'';
        sql[length + 2] = ';';
        lexed_t *lexed = scan(sql, length + 3);
        const lexer_token_t *t = token(lexed, 0);
        assert(t->kind == LEXER_STRING && t->text_size == length && t->size == length + 2);
        assert(memcmp(t->text, sql + 1, length) == 0);
        lexer_free(lexed);
        free(sql);
    }
    /* 8192 경계에 걸친 다중 바이트 문자와 '' escape */
    for (size_t position = 8185; position <= 8195; position++) {
        char *sql = malloc(20000);
        size_t length = 0;
        sql[length++] = '\'';
        while (length - 1 < position) sql[length++] = 'x';
        memcpy(sql + length, "\xea\xb0\x80''\xf0\x9f\x99\x82", 9);
        length += 9;
        while (length < 9000) sql[length++] = 'y';
        sql[length++] = '\'';
        lexed_t *lexed = scan(sql, length);
        const lexer_token_t *t = token(lexed, 0);
        assert(t->text_size == length - 2 - 1);  /* 쌍을 이룬 따옴표 하나가 줄어든다 */
        assert(memcmp(t->text + position, "\xea\xb0\x80'\xf0\x9f\x99\x82", 8) == 0);
        lexer_free(lexed);
        free(sql);
    }
    /* 내장 NUL과 줄바꿈은 문자열 안에서 보존한다. */
    const char with_nul[] = "'a\0b\nc'";
    lexed_t *lexed = scan(with_nul, sizeof(with_nul) - 1);
    assert(token(lexed, 0)->text_size == 5 && memcmp(token(lexed, 0)->text, "a\0b\nc", 5) == 0);
    lexer_free(lexed);
}

static void token_size_limit_never_truncates(void)
{
    char sql[1024];
    memset(sql, 'a', sizeof(sql));
    sql[0] = '\'';
    sql[1023] = '\'';
    lexer_limits_t limits = {4096, 1024, 16, 0};
    lexed_t *lexed = NULL;
    assert(lexer_scan((uint8_t *)sql, 1024, &limits, &lexed, NULL) == LEXER_OK);
    assert(token(lexed, 0)->text_size == 1022);
    lexer_free(lexed);
    limits.max_token_bytes = 1023;
    expect_fail_with(sql, 1024, &limits, LEXER_LIMIT, LEXER_REASON_TOKEN_TOO_LARGE, 0);
    char word[300];
    memset(word, 'w', sizeof(word));
    limits.max_token_bytes = 300;
    assert(lexer_scan((uint8_t *)word, 300, &limits, &lexed, NULL) == LEXER_OK);
    lexer_free(lexed);
    limits.max_token_bytes = 299;
    expect_fail_with(word, 300, &limits, LEXER_LIMIT, LEXER_REASON_TOKEN_TOO_LARGE, 0);
}

static void utf8_is_validated_where_allowed(void)
{
    lexed_t *lexed = scan_text("'é 가 🙂' \"é 가 🙂\" /* é 가 🙂 */ -- é 가 🙂\nx");
    assert_token(lexed, 0, LEXER_STRING, "é 가 🙂");
    assert_token(lexed, 1, LEXER_QUOTED_IDENT, "é 가 🙂");
    assert_token(lexed, 2, LEXER_WORD, "x");
    lexer_free(lexed);

    const char *bad[] = {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80",
                         "\xff", "\xe0\x80\x80", "\xf0\x80\x80\x80", "\xc3", "a\xc3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char sql[32];
        int length = snprintf(sql, sizeof(sql), "'%s'", bad[i]);
        expect_fail_with(sql, (size_t)length, &LIMITS, LEXER_SYNTAX, LEXER_REASON_INVALID_UTF8, 1);
        length = snprintf(sql, sizeof(sql), "\"%s\"", bad[i]);
        expect_fail_with(sql, (size_t)length, &LIMITS, LEXER_SYNTAX, LEXER_REASON_INVALID_UTF8, 1);
        length = snprintf(sql, sizeof(sql), "-- %s\nx", bad[i]);
        expect_fail_with(sql, (size_t)length, &LIMITS, LEXER_SYNTAX, LEXER_REASON_INVALID_UTF8, 2);
        length = snprintf(sql, sizeof(sql), "/*%s*/", bad[i]);
        expect_fail_with(sql, (size_t)length, &LIMITS, LEXER_SYNTAX, LEXER_REASON_INVALID_UTF8, 2);
        length = snprintf(sql, sizeof(sql), "select %s", bad[i]);
        /* "a\xc3"은 앞의 ASCII 단어를 먼저 읽고 그 다음 바이트에서 거절한다. */
        size_t bare = bad[i][0] == 'a' ? 8 : 7;
        expect_fail_with(sql, (size_t)length, &LIMITS, LEXER_SYNTAX, LEXER_REASON_UNEXPECTED_BYTE, bare);
    }
    /* bare 식별자는 ASCII뿐이다. 유효한 UTF-8도 따옴표 밖에서는 거절한다. */
    expect_syntax("select é from t", LEXER_REASON_UNEXPECTED_BYTE, 7);
    expect_syntax("select a가 from t", LEXER_REASON_UNEXPECTED_BYTE, 8);
    /* 따옴표 밖과 식별자·주석 안의 NUL */
    expect_fail_with("select\0 1", 9, &LIMITS, LEXER_SYNTAX, LEXER_REASON_UNEXPECTED_BYTE, 6);
    expect_fail_with("\"a\0b\"", 5, &LIMITS, LEXER_SYNTAX, LEXER_REASON_UNEXPECTED_BYTE, 2);
    expect_fail_with("-- a\0b\nx", 9, &LIMITS, LEXER_SYNTAX, LEXER_REASON_UNEXPECTED_BYTE, 4);
    expect_fail_with("/* a\0b */ x", 12, &LIMITS, LEXER_SYNTAX, LEXER_REASON_UNEXPECTED_BYTE, 4);
}

static void comments_are_skipped_only_outside_strings(void)
{
    lexed_t *lexed = scan_text("a -- comment ; b\n b /* ; 'x */ c '-- not' '/* not */' d -- tail");
    assert(lexed_statement_count(lexed) == 1);
    const char *expected[] = {"a", "b", "c", "-- not", "/* not */", "d"};
    assert(lexed_token_count(lexed) == 6);
    for (size_t i = 0; i < 6; i++) assert(strcmp((const char *)token(lexed, i)->text, expected[i]) == 0 ||
                                          memcmp(token(lexed, i)->text, expected[i], strlen(expected[i])) == 0);
    lexer_statement_t statement;
    assert(lexed_statement_at(lexed, 0, &statement) == LEXER_OK && !statement.terminated);
    lexer_free(lexed);

    lexed = scan_text("a-b a--b\nc 1-2 d/e");
    const char *tokens[] = {"a", "-", "b", "a", "c", "1", "-", "2", "d", "/", "e"};
    assert(lexed_token_count(lexed) == 11);
    for (size_t i = 0; i < 11; i++) {
        assert(token(lexed, i)->text_size == strlen(tokens[i]));
        assert(memcmp(token(lexed, i)->text, tokens[i], strlen(tokens[i])) == 0);
    }
    lexer_free(lexed);

    expect_syntax("a /* never closed", LEXER_REASON_UNTERMINATED_COMMENT, 2);
    expect_syntax("a /*/ still open", LEXER_REASON_UNTERMINATED_COMMENT, 2);
    expect_syntax("a /* x *", LEXER_REASON_UNTERMINATED_COMMENT, 2);
    expect_syntax("-- only a comment", LEXER_REASON_EMPTY_STATEMENT, 17);
    expect_syntax("/* only */", LEXER_REASON_EMPTY_STATEMENT, 10);
}

static void statements_split_on_real_semicolons(void)
{
    const char *sql = "a; b 'x;y' \"p;q\" /* ; */ -- ;\n; c ;  -- end\n";
    lexed_t *lexed = scan_text(sql);
    assert(lexed_statement_count(lexed) == 3);
    lexer_statement_t statement;
    assert(lexed_statement_at(lexed, 0, &statement) == LEXER_OK);
    assert(statement.token_count == 1 && statement.terminated && statement.offset == 0);
    assert(statement.end_offset == 2);
    assert(lexed_statement_at(lexed, 1, &statement) == LEXER_OK);
    assert(statement.token_count == 3 && statement.terminated);
    assert(statement.tokens[1].kind == LEXER_STRING && statement.tokens[1].text_size == 3);
    assert(lexed_statement_at(lexed, 2, &statement) == LEXER_OK);
    assert(statement.token_count == 1 && statement.terminated);
    lexer_free(lexed);

    lexed = scan_text("select 1; select 2");
    assert(lexed_statement_count(lexed) == 2);
    assert(lexed_statement_at(lexed, 1, &statement) == LEXER_OK && !statement.terminated);
    assert(statement.end_offset == strlen("select 1; select 2"));
    lexer_free(lexed);

    expect_fail_with("", 0, &LIMITS, LEXER_SYNTAX, LEXER_REASON_EMPTY_STATEMENT, 0);
    expect_fail_with(NULL, 0, &LIMITS, LEXER_SYNTAX, LEXER_REASON_EMPTY_STATEMENT, 0);
    expect_syntax("   \n\t ", LEXER_REASON_EMPTY_STATEMENT, 6);
    expect_syntax(";", LEXER_REASON_EMPTY_STATEMENT, 0);
    expect_syntax("a;;", LEXER_REASON_EMPTY_STATEMENT, 2);
    expect_syntax("a; ;b", LEXER_REASON_EMPTY_STATEMENT, 3);
    expect_syntax("; a", LEXER_REASON_EMPTY_STATEMENT, 0);
    expect_syntax("a; /* c */ ;", LEXER_REASON_EMPTY_STATEMENT, 11);
}

static void trailing_and_invalid_input_is_rejected(void)
{
    expect_syntax("select 'abc", LEXER_REASON_UNTERMINATED_STRING, 7);
    expect_syntax("select 'abc'' ", LEXER_REASON_UNTERMINATED_STRING, 7);
    expect_syntax("select \"abc", LEXER_REASON_UNTERMINATED_IDENT, 7);
    expect_syntax("select 1; select 'x", LEXER_REASON_UNTERMINATED_STRING, 17);
    const char *unexpected[] = {"@", "#", "$1", "`a`", "\\", "[a]", "{", "|", "&", ":a", "a ! b",
                                "~", "\x01", "\x7f", "\x0b", "\x1b"};
    for (size_t i = 0; i < sizeof(unexpected) / sizeof(unexpected[0]); i++) {
        char sql[32];
        snprintf(sql, sizeof(sql), "a %s", unexpected[i]);
        size_t offset = strcmp(unexpected[i], "a ! b") == 0 ? 4 : 2;
        expect_syntax(sql, LEXER_REASON_UNEXPECTED_BYTE, offset);
    }
    expect_syntax("select 1e", LEXER_REASON_BAD_NUMBER, 7);
    expect_syntax("select 1e+", LEXER_REASON_BAD_NUMBER, 7);
    expect_syntax("select 1.", LEXER_REASON_BAD_NUMBER, 7);
    expect_syntax("select 12abc", LEXER_REASON_BAD_NUMBER, 7);
    expect_syntax("select 1.5.5", LEXER_REASON_BAD_NUMBER, 7);
    expect_syntax("select 0x10", LEXER_REASON_BAD_NUMBER, 7);
    expect_syntax("select 1_000", LEXER_REASON_BAD_NUMBER, 7);
    /* 줄바꿈을 포함한 공백은 모두 허용하고 \f도 공백이다. */
    lexed_t *lexed = scan_text("a\f\r\n\tb");
    assert(lexed_token_count(lexed) == 2);
    lexer_free(lexed);
    /* 숫자 모양: 부호 없는 10진. 앞의 .은 별도 PUNCT이며 문법 검사는 parser 몫이다. */
    lexed = scan_text("0 007 1.5 1e10 1E+10 2.5e-3 .5");
    const char *numbers[] = {"0", "007", "1.5", "1e10", "1E+10", "2.5e-3", ".", "5"};
    assert(lexed_token_count(lexed) == 8);
    for (size_t i = 0; i < 8; i++) {
        assert(token(lexed, i)->text_size == strlen(numbers[i]));
        assert(token(lexed, i)->kind == (i == 6 ? LEXER_PUNCT : LEXER_NUMBER));
    }
    lexer_free(lexed);
    lexed = scan_text("< <= > >= <> != = + - * / % ( ) , .");
    assert(lexed_token_count(lexed) == 16);
    for (size_t i = 0; i < 16; i++) assert(token(lexed, i)->kind == LEXER_PUNCT);
    assert(token(lexed, 1)->size == 2 && token(lexed, 4)->size == 2 && token(lexed, 5)->size == 2);
    lexer_free(lexed);
}

static void limits_are_explicit_and_exact(void)
{
    const char *sql = "a b c d;";
    lexer_limits_t limits = {8, 8, 8, 0};
    lexed_t *lexed = NULL;
    assert(lexer_scan((const uint8_t *)sql, 8, &limits, &lexed, NULL) == LEXER_OK);
    lexer_free(lexed);
    limits.max_input_bytes = 7;
    expect_fail_with(sql, 8, &limits, LEXER_LIMIT, LEXER_REASON_INPUT_TOO_LARGE, 7);
    limits.max_input_bytes = 8;
    limits.max_tokens = 5;
    assert(lexer_scan((const uint8_t *)sql, 8, &limits, &lexed, NULL) == LEXER_OK);
    lexer_free(lexed);
    limits.max_tokens = 4;
    expect_fail_with(sql, 8, &limits, LEXER_LIMIT, LEXER_REASON_TOO_MANY_TOKENS, 7);

    lexer_limits_t zero = LIMITS;
    zero.max_input_bytes = 0;
    expect_fail_with(sql, 8, &zero, LEXER_INVALID, LEXER_REASON_NONE, 0);
    zero = LIMITS;
    zero.max_token_bytes = 0;
    expect_fail_with(sql, 8, &zero, LEXER_INVALID, LEXER_REASON_NONE, 0);
    zero = LIMITS;
    zero.max_tokens = 0;
    expect_fail_with(sql, 8, &zero, LEXER_INVALID, LEXER_REASON_NONE, 0);
    zero = LIMITS;
    zero.max_parameters = LEXER_PARAMETER_CEILING + 1;
    expect_fail_with(sql, 8, &zero, LEXER_INVALID, LEXER_REASON_NONE, 0);
    expect_fail_with(NULL, 3, &LIMITS, LEXER_INVALID, LEXER_REASON_NONE, 0);
    expect_fail_with(sql, 8, NULL, LEXER_INVALID, LEXER_REASON_NONE, 0);
    assert(lexer_scan((const uint8_t *)sql, 8, &LIMITS, NULL, NULL) == LEXER_INVALID);
    zero = LIMITS;
    zero.max_parameters = LEXER_PARAMETER_CEILING;
    lexed = NULL;
    assert(lexer_scan((const uint8_t *)"?", 1, &zero, &lexed, NULL) == LEXER_OK);
    lexer_free(lexed);
}

static void assert_ids(const lexed_t *lexed, const uint32_t *ids, size_t count)
{
    size_t seen = 0;
    for (size_t i = 0; i < lexed_token_count(lexed); i++) {
        if (token(lexed, i)->kind != LEXER_PARAMETER) continue;
        assert(seen < count && token(lexed, i)->parameter_id == ids[seen]);
        seen++;
    }
    assert(seen == count);
}

/* ?와 ?N, 고유 ID 수와 placeholder 출현 수는 다르다. */
static void parameters_are_numbered_and_dense(void)
{
    lexed_t *lexed = scan_text("select ?, ?, ? from t where a = ?");
    assert(lexed_parameter_count(lexed) == 4);
    assert_ids(lexed, (uint32_t[]){1, 2, 3, 4}, 4);
    lexer_free(lexed);

    lexed = scan_text("select ?2, ?1, ?2 where x = ?1 and y = ?2");
    assert(lexed_parameter_count(lexed) == 2);  /* 출현은 5번이지만 고유 ID는 둘 */
    assert_ids(lexed, (uint32_t[]){2, 1, 2, 1, 2}, 5);
    lexer_free(lexed);

    lexed = scan_text("?1 ?1 ?1");
    assert(lexed_parameter_count(lexed) == 1);
    lexer_free(lexed);
    lexed = scan_text("?3 ?1 ?2");
    assert(lexed_parameter_count(lexed) == 3);
    lexer_free(lexed);
    lexed = scan_text("select 1");
    assert(lexed_parameter_count(lexed) == 0);
    lexer_free(lexed);
    lexed = scan_text("select ?; select ?");
    assert(lexed_parameter_count(lexed) == 2);  /* 입력 전체에서 한 번 번호를 매긴다 */
    assert_ids(lexed, (uint32_t[]){1, 2}, 2);
    lexer_free(lexed);
    lexed = scan_text("select '?' \"?1\" -- ?2\n, /* ? */ ?");
    assert(lexed_parameter_count(lexed) == 1);
    lexer_free(lexed);

    expect_syntax("select ?1, ?3", LEXER_REASON_PARAMETER_GAP, 11);
    expect_syntax("select ?2", LEXER_REASON_PARAMETER_GAP, 7);
    expect_syntax("?1 ?2 ?4 ?2", LEXER_REASON_PARAMETER_GAP, 6);
    expect_syntax("select ?, ?2", LEXER_REASON_PARAMETER_MIX, 10);
    expect_syntax("select ?1, ?", LEXER_REASON_PARAMETER_MIX, 11);
    expect_syntax("select ?0", LEXER_REASON_BAD_PARAMETER, 7);
    expect_syntax("select ?01", LEXER_REASON_BAD_PARAMETER, 7);
    expect_syntax("select ?1a", LEXER_REASON_BAD_PARAMETER, 7);
    expect_syntax("select ?a", LEXER_REASON_BAD_PARAMETER, 7);
    expect_syntax("select ?_x", LEXER_REASON_BAD_PARAMETER, 7);

    lexer_limits_t limits = LIMITS;
    limits.max_parameters = 3;
    lexed_t *out = NULL;
    assert(lexer_scan((const uint8_t *)"?3 ?2 ?1", 8, &limits, &out, NULL) == LEXER_OK);
    lexer_free(out);
    expect_fail_with("?1 ?2 ?4", 8, &limits, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, 6);
    expect_fail_with("? ? ? ?", 7, &limits, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, 6);
    expect_fail_with("?99999999999999999999", 21, &limits, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, 0);
    limits.max_parameters = 0;
    expect_fail_with("select ?", 8, &limits, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, 7);
    expect_fail_with("select ?1", 9, &limits, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, 7);

    /* 100개까지 빈틈 없이 */
    char big[1024];
    size_t length = 0;
    for (int i = 1; i <= 100; i++) length += (size_t)snprintf(big + length, sizeof(big) - length, "?%d ", i);
    lexed = scan(big, length);
    assert(lexed_parameter_count(lexed) == 100);
    lexer_free(lexed);
    length += (size_t)snprintf(big + length, sizeof(big) - length, "?101");
    expect_fail_with(big, length, &LIMITS, LEXER_LIMIT, LEXER_REASON_TOO_MANY_PARAMETERS, length - 4);
}

/* 바인딩은 값 개수와 고유 ID 개수를 비교하며 SQL 문자열을 다시 만들지 않는다. */
static void binding_checks_count_not_occurrences(void)
{
    lexed_t *lexed = scan_text("select ?1, ?2, ?1 where a = ?2");
    const uint8_t blob[] = {0, 1, 2};
    value_view_t values[] = {{VALUE_INTEGER, 7, 0.0, NULL, 0},
                             {VALUE_BLOB, 0, 0.0, blob, sizeof(blob)},
                             {VALUE_NULL, 0, 0.0, NULL, 0}};
    lexer_binding_t binding = {values, 2};
    assert(lexed_parameter_count(lexed) == 2);
    assert(lexer_check_binding(lexed, &binding) == LEXER_OK);
    binding.count = 4;  /* placeholder 출현 수와 같은 값으로 속일 수 없다 */
    assert(lexer_check_binding(lexed, &binding) == LEXER_PARAMETER_COUNT);
    binding.count = 3;
    assert(lexer_check_binding(lexed, &binding) == LEXER_PARAMETER_COUNT);
    binding.count = 1;
    assert(lexer_check_binding(lexed, &binding) == LEXER_PARAMETER_COUNT);
    binding.count = 0;
    assert(lexer_check_binding(lexed, &binding) == LEXER_PARAMETER_COUNT);
    binding = (lexer_binding_t){NULL, 2};
    assert(lexer_check_binding(lexed, &binding) == LEXER_INVALID);
    assert(lexer_check_binding(NULL, &binding) == LEXER_INVALID);
    assert(lexer_check_binding(lexed, NULL) == LEXER_INVALID);

    binding = (lexer_binding_t){values, 2};
    const value_view_t *view = NULL;
    assert(lexer_binding_value(&binding, 1, &view) == LEXER_OK && view == &values[0]);
    assert(lexer_binding_value(&binding, 2, &view) == LEXER_OK && view == &values[1]);
    assert(view->bytes == blob && view->size_bytes == 3);  /* 복사·재조합 없이 같은 view */
    const value_view_t *before = view;
    assert(lexer_binding_value(&binding, 0, &view) == LEXER_INVALID && view == before);
    assert(lexer_binding_value(&binding, 3, &view) == LEXER_INVALID && view == before);
    assert(lexer_binding_value(&binding, 1, NULL) == LEXER_INVALID);
    lexer_free(lexed);

    lexed = scan_text("select 1");
    binding = (lexer_binding_t){NULL, 0};
    assert(lexer_check_binding(lexed, &binding) == LEXER_OK);
    binding = (lexer_binding_t){values, 1};
    assert(lexer_check_binding(lexed, &binding) == LEXER_PARAMETER_COUNT);
    lexer_free(lexed);
}

/* 입력의 모든 접두부는 입력 밖을 읽지 않고, 성공하면 모든 span이 접두부 안에 있다. */
static void every_prefix_stays_inside_input(void)
{
    const char script[] = "SELECT 'it''s', \"q\"\"x\", ?1, 12.5e+3 /* c */ FROM t; -- tail\n"
                          "INSERT INTO u VALUES ('가🙂', ?2); select ?1 <> ?2 -- x";
    size_t total = sizeof(script) - 1;
    for (size_t length = 0; length <= total; length++) {
        uint8_t *copy = malloc(length ? length : 1);  /* 접두부 뒤를 읽으면 sanitizer가 잡는다 */
        memcpy(copy, script, length);
        lexed_t *lexed = NULL;
        lexer_error_t error;
        lexer_status_t status = lexer_scan(copy, length, &LIMITS, &lexed, &error);
        if (status == LEXER_OK) {
            for (size_t i = 0; i < lexed_token_count(lexed); i++) {
                const lexer_token_t *t = token(lexed, i);
                assert(t->offset + t->size <= length && t->bytes == copy + t->offset);
            }
            lexer_free(lexed);
        } else {
            assert(status == LEXER_SYNTAX && lexed == NULL && error.offset <= length);
        }
        free(copy);
    }
}

int main(void)
{
    tokens_kinds_and_spans();
    escapes_and_empty_values();
    long_strings_are_whole();
    token_size_limit_never_truncates();
    utf8_is_validated_where_allowed();
    comments_are_skipped_only_outside_strings();
    statements_split_on_real_semicolons();
    trailing_and_invalid_input_is_rejected();
    limits_are_explicit_and_exact();
    parameters_are_numbered_and_dense();
    binding_checks_count_not_occurrences();
    every_prefix_stays_inside_input();
    puts("lexer: whole-input tokens, escapes, UTF-8, comments, statements, limits, parameters");
    return 0;
}
