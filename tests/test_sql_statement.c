#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/lexer.h"
#include "sql/sql_statement.h"
#include "storage/catalog.h"

/*
 * sql_statement.c와 value.c는 -Dmalloc=sqlstmt_test_malloc -Dcalloc=sqlstmt_test_calloc
 * -Dfree=sqlstmt_test_free로 빌드한다. 이 파일과 lexer.c는 그대로 빌드하므로 아래 함수가
 * parser가 하는 할당만 세고 실패시킨다. 치환 없이 빌드하면 main이 곧바로 실패한다.
 */
static size_t live_allocations;
static size_t allocation_calls;
static size_t fail_at;

static void *counted_alloc(void *memory)
{
    if (memory != NULL) live_allocations++;
    return memory;
}

void *sqlstmt_test_malloc(size_t size)
{
    allocation_calls++;
    if (fail_at != 0 && allocation_calls == fail_at) return NULL;
    return counted_alloc(malloc(size));
}

void *sqlstmt_test_calloc(size_t count, size_t size)
{
    allocation_calls++;
    if (fail_at != 0 && allocation_calls == fail_at) return NULL;
    return counted_alloc(calloc(count, size));
}

void sqlstmt_test_free(void *memory)
{
    if (memory != NULL) live_allocations--;
    free(memory);
}

static const lexer_limits_t LIMITS = {1u << 22, 1u << 20, 1u << 16, 100};

static sqlstmt_status_t run(const void *sql, size_t size, const value_view_t *values,
                            size_t value_count, sqlstmt_t **out, sqlstmt_error_t *error)
{
    lexed_t *lexed = NULL;
    lexer_error_t lexer_error;
    assert(lexer_scan(sql, size, &LIMITS, &lexed, &lexer_error) == LEXER_OK);
    lexer_statement_t statement;
    assert(lexed_statement_at(lexed, 0, &statement) == LEXER_OK);
    lexer_binding_t binding = {values, value_count};
    sqlstmt_status_t status =
        sqlstmt_parse(&statement, value_count > 0 ? &binding : NULL, out, error);
    /* 문 구조는 입력과 토큰이 사라진 뒤에도 유효해야 한다. */
    lexer_free(lexed);
    return status;
}

static sqlstmt_t *parse_bound(const char *sql, const value_view_t *values, size_t value_count)
{
    sqlstmt_t *statement = NULL;
    sqlstmt_error_t error = {SQLSTMT_REASON_BAD_ARGUMENT, 99, VALUE_INVALID};
    sqlstmt_status_t status = run(sql, strlen(sql), values, value_count, &statement, &error);
    if (status != SQLSTMT_OK) {
        fprintf(stderr, "input %s: status %d reason %d offset %zu\n", sql, status, error.reason,
                error.offset);
    }
    assert(status == SQLSTMT_OK && statement != NULL);
    assert(error.reason == SQLSTMT_REASON_NONE && error.offset == 0 && error.value_status == VALUE_OK);
    return statement;
}

static sqlstmt_t *parse_ok(const char *sql)
{
    return parse_bound(sql, NULL, 0);
}

static size_t at(const char *sql, const char *needle)
{
    const char *found = strstr(sql, needle);
    assert(found != NULL);
    return (size_t)(found - sql);
}

/* 기대와 다른 입력은 모두 모아 출력한 뒤 main이 한꺼번에 실패시킨다. */
static size_t mismatches;

/* 실패하면 *out과 할당은 그대로이고 상태·reason·offset이 맞다. */
static void expect_fail_bound(const char *sql, const value_view_t *values, size_t value_count,
                              sqlstmt_status_t status, sqlstmt_reason_t reason, size_t offset)
{
    static char marker;
    sqlstmt_t *out = (sqlstmt_t *)&marker;
    sqlstmt_error_t error = {SQLSTMT_REASON_NONE, 12345, VALUE_OK};
    size_t before = live_allocations;
    sqlstmt_status_t actual = run(sql, strlen(sql), values, value_count, &out, &error);
    if (actual != status || error.reason != reason || error.offset != offset) {
        fprintf(stderr, "input %s: status %d reason %d offset %zu, expected %d %d %zu\n", sql,
                actual, error.reason, error.offset, status, reason, offset);
        mismatches++;
        return;
    }
    if (out != (sqlstmt_t *)&marker || live_allocations != before) {
        fprintf(stderr, "input %s: 실패했는데 출력이 바뀌었거나 할당 %zu개가 남음\n", sql, live_allocations - before);
        mismatches++;
        return;
    }
    /* error가 NULL이어도 같은 상태를 돌려준다. */
    assert(run(sql, strlen(sql), values, value_count, &out, NULL) == status);
    assert(out == (sqlstmt_t *)&marker && live_allocations == before);
}

static void expect_fail(const char *sql, sqlstmt_status_t status, sqlstmt_reason_t reason,
                        size_t offset)
{
    expect_fail_bound(sql, NULL, 0, status, reason, offset);
}

static void expect_syntax(const char *sql, sqlstmt_reason_t reason, size_t offset)
{
    expect_fail(sql, SQLSTMT_SYNTAX, reason, offset);
}

static void expect_unsupported(const char *sql, sqlstmt_reason_t reason, size_t offset)
{
    expect_fail(sql, SQLSTMT_UNSUPPORTED, reason, offset);
}

static bool name_is(const sqlstmt_name_t *name, const char *text)
{
    return name->size == strlen(text) && memcmp(name->bytes, text, name->size) == 0;
}

static bool is_integer(const sqlexpr_t *expr, int64_t expected)
{
    return expr->kind == SQLEXPR_LITERAL && expr->value.view.type == VALUE_INTEGER && expr->value.view.integer == expected;
}

static bool is_real(const sqlexpr_t *expr, double expected)
{
    return expr->kind == SQLEXPR_LITERAL && expr->value.view.type == VALUE_REAL && expr->value.view.real == expected;
}

static bool is_text(const sqlexpr_t *expr, const void *bytes, size_t size)
{
    const value_view_t *v = &expr->value.view;
    return expr->kind == SQLEXPR_LITERAL && v->type == VALUE_TEXT && v->size_bytes == size &&
           (size == 0 || memcmp(v->bytes, bytes, size) == 0);
}

static bool is_blob(const sqlexpr_t *expr, const void *bytes, size_t size)
{
    const value_view_t *v = &expr->value.view;
    return expr->kind == SQLEXPR_LITERAL && v->type == VALUE_BLOB && v->size_bytes == size &&
           (size == 0 || memcmp(v->bytes, bytes, size) == 0);
}

static bool is_null(const sqlexpr_t *expr) { return expr->kind == SQLEXPR_LITERAL && expr->value.view.type == VALUE_NULL; }

static bool is_column(const sqlexpr_t *expr, const char *name)
{
    return expr->kind == SQLEXPR_COLUMN && expr->qualifier.size == 0 && name_is(&expr->name, name);
}

/* column op 리터럴 꼴의 이항 비교 */
static bool is_compare(const sqlexpr_t *expr, const char *column, sqlexpr_op_t op)
{
    return expr != NULL && expr->kind == SQLEXPR_BINARY && expr->op == op && is_column(expr->args[0], column);
}

static void free_clean(sqlstmt_t *statement)
{
    sqlstmt_free(statement);
    assert(live_allocations == 0);
}

static void create_all_types(void)
{
    sqlstmt_t *statement = parse_ok(
        "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(20) NOT NULL,"
        " score REAL, data BLOB NOT NULL, bio TEXT);");
    assert(statement->kind == SQLSTMT_CREATE_TABLE);
    sqlstmt_create_t *create = &statement->create;
    assert(name_is(&create->table, "users") && create->column_count == 5);
    assert(name_is(&create->columns[0].name, "id") && create->columns[0].type == VALUE_INTEGER);
    assert(create->primary_key_column == 0 && create->has_identity);
    assert(name_is(&create->columns[1].name, "name") && create->columns[1].type == VALUE_TEXT);
    assert(create->columns[1].not_null && create->columns[1].has_varchar_length);
    assert(create->columns[1].varchar_length == 20);
    assert(create->columns[2].type == VALUE_REAL && !create->columns[2].not_null);
    assert(create->columns[3].type == VALUE_BLOB && create->columns[3].not_null);
    assert(create->columns[4].type == VALUE_TEXT && !create->columns[4].has_varchar_length);
    free_clean(statement);
}

/* #65: 단일 컬럼 보조 색인과 unique 선언만 AST로 보존한다. */
static void create_index_forms(void)
{
    sqlstmt_t *statement = parse_ok("CREATE INDEX by_name ON users (name)");
    assert(statement->kind == SQLSTMT_CREATE_INDEX);
    assert(!statement->index.unique && name_is(&statement->index.name, "by_name"));
    assert(name_is(&statement->index.table, "users") && name_is(&statement->index.column, "name"));
    free_clean(statement);

    statement = parse_ok("CREATE UNIQUE INDEX unique_email ON users (email)");
    assert(statement->kind == SQLSTMT_CREATE_INDEX && statement->index.unique);
    free_clean(statement);
    expect_unsupported("CREATE INDEX pair ON users (a, b)", SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 29);
}

static void create_primary_key_forms(void)
{
    sqlstmt_t *statement = parse_ok("create table t (a integer, b text, primary key (b))");
    assert(statement->create.primary_key_column == 1 && !statement->create.has_identity);
    free_clean(statement);

    /* 제약은 컬럼보다 먼저 와도 컬럼을 CREATE 전체에서 찾는다. */
    statement = parse_ok("CREATE TABLE t (PRIMARY KEY (a), a INTEGER)");
    assert(statement->create.primary_key_column == 0 && !statement->create.has_identity);
    free_clean(statement);

    statement = parse_ok("CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT)");
    assert(statement->create.primary_key_column == 0 && !statement->create.has_identity);
    free_clean(statement);

    statement = parse_ok("CREATE TABLE t (a INTEGER NOT NULL, b TEXT)");
    assert(statement->create.primary_key_column == SQLSTMT_NO_COLUMN);
    assert(statement->create.columns[0].not_null);
    free_clean(statement);

    /* 제약 순서와 NOT NULL 중복은 그대로 받는다. */
    statement = parse_ok("CREATE TABLE t (a INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT NOT NULL)");
    assert(statement->create.has_identity && statement->create.columns[0].not_null);
    free_clean(statement);
}

static void identifiers_and_case(void)
{
    sqlstmt_t *statement = parse_ok("CrEaTe TaBlE \"T \xc3\xa9\" (\"a b\" InTeGeR, \"select\" tExT)");
    assert(statement->create.table.size == 4 && name_is(&statement->create.table, "T \xc3\xa9"));
    assert(name_is(&statement->create.columns[0].name, "a b"));
    assert(name_is(&statement->create.columns[1].name, "select"));
    free_clean(statement);

    /* 식별자는 대소문자를 구분하고 키워드만 구분하지 않는다. */
    statement = parse_ok("SeLeCt A, a FrOm T wHeRe B Is NoT NuLl");
    assert(statement->select.item_count == 2 && is_column(statement->select.items[0].expr, "A"));
    assert(is_column(statement->select.items[1].expr, "a") && name_is(&statement->select.table, "T"));
    assert(statement->select.where->kind == SQLEXPR_IS_NULL && statement->select.where->negated);
    free_clean(statement);

    statement = parse_ok("SELECT \"from\", \"where\" FROM \"table\"");
    assert(is_column(statement->select.items[0].expr, "from") && name_is(&statement->select.table, "table"));
    free_clean(statement);

    /* 예약어 아닌 단어는 맨 이름으로 쓸 수 있다. */
    statement = parse_ok("SELECT integer, x, count, if FROM limited");
    assert(statement->select.item_count == 4 && name_is(&statement->select.table, "limited"));
    free_clean(statement);
}

static void insert_values(void)
{
    sqlstmt_t *statement = parse_ok(
        "INSERT INTO t VALUES (1, -2, +3, 1.5, -0.25e1, 'it''s', '', NULL, X'00ff', x'')");
    assert(statement->kind == SQLSTMT_INSERT);
    sqlstmt_insert_t *insert = &statement->insert;
    assert(name_is(&insert->table, "t") && !insert->has_columns && insert->value_count == 10 && insert->row_count == 1);
    assert(is_integer(insert->values[0], 1) && is_integer(insert->values[1], -2));
    assert(is_integer(insert->values[2], 3) && is_real(insert->values[3], 1.5));
    assert(is_real(insert->values[4], -2.5) && is_text(insert->values[5], "it's", 4));
    assert(is_text(insert->values[6], "", 0) && is_null(insert->values[7]));
    assert(is_blob(insert->values[8], "\x00\xff", 2) && is_blob(insert->values[9], "", 0));
    free_clean(statement);

    statement = parse_ok("INSERT INTO t (b, \"a c\") VALUES (NULL, 'x')");
    assert(statement->insert.has_columns && statement->insert.column_count == 2);
    assert(name_is(&statement->insert.columns[0], "b") && name_is(&statement->insert.columns[1], "a c"));
    free_clean(statement);

    /* 컬럼 목록이 없으면 값 개수는 database가 정한다. */
    statement = parse_ok("INSERT INTO t VALUES (1)");
    assert(statement->insert.value_count == 1);
    free_clean(statement);

    /* 여러 행과 식 값 */
    statement = parse_ok("INSERT INTO t (a, b) VALUES (1, 'x' || 'y'), (2 * 3, NULL), (-4, lower('Z'))");
    assert(statement->insert.row_count == 3 && statement->insert.value_count == 2);
    assert(is_integer(statement->insert.values[0], 1) && statement->insert.values[1]->op == SQLEXPR_OP_CONCAT);
    assert(statement->insert.values[2]->kind == SQLEXPR_BINARY && statement->insert.values[2]->op == SQLEXPR_OP_MUL);
    assert(is_integer(statement->insert.values[4], -4) && statement->insert.values[5]->kind == SQLEXPR_FUNCTION);
    assert(name_is(&statement->insert.values[5]->name, "LOWER"));
    free_clean(statement);
}

static void text_values_keep_every_byte(void)
{
    const char sql[] = "INSERT INTO t VALUES ('a\0b', '\xed\x95\x9c\xea\xb8\x80 \xf0\x9f\x98\x80')";
    sqlstmt_t *statement = NULL;
    sqlstmt_error_t error;
    assert(run(sql, sizeof(sql) - 1, NULL, 0, &statement, &error) == SQLSTMT_OK);
    assert(is_text(statement->insert.values[0], "a\0b", 3));
    assert(is_text(statement->insert.values[1], "\xed\x95\x9c\xea\xb8\x80 \xf0\x9f\x98\x80", 11));
    free_clean(statement);

    /* 페이지보다 긴 문자열도 잘리지 않는다. */
    size_t length = 20000;
    char *long_sql = malloc(length + 40);
    assert(long_sql != NULL);
    size_t used = (size_t)sprintf(long_sql, "INSERT INTO t VALUES ('");
    memset(long_sql + used, 'q', length);
    memcpy(long_sql + used + length, "')", 3);
    assert(run(long_sql, used + length + 2, NULL, 0, &statement, &error) == SQLSTMT_OK);
    assert(statement->insert.values[0]->value.view.size_bytes == length);
    assert(statement->insert.values[0]->value.view.bytes[length - 1] == 'q');
    free_clean(statement);
    free(long_sql);
}

static void numeric_literals(void)
{
    sqlstmt_t *statement = parse_ok(
        "INSERT INTO t VALUES (9223372036854775807, -9223372036854775808, 007, - 5, 1e3, "
        "1.0, 2.5E-1, -0.0, 4.9e-324)");
    sqlstmt_insert_t *insert = &statement->insert;
    assert(is_integer(insert->values[0], INT64_MAX) && is_integer(insert->values[1], INT64_MIN));
    assert(is_integer(insert->values[2], 7) && is_integer(insert->values[3], -5));
    assert(is_real(insert->values[4], 1000.0) && is_real(insert->values[5], 1.0));
    assert(is_real(insert->values[6], 0.25));
    assert(insert->values[7]->value.view.type == VALUE_REAL && insert->values[7]->value.view.real == 0.0 &&
           signbit(insert->values[7]->value.view.real));
    assert(is_real(insert->values[8], 4.9e-324));
    free_clean(statement);

    const char *prefix = "INSERT INTO t VALUES (";
    expect_fail("INSERT INTO t VALUES (9223372036854775808)", SQLSTMT_LIMIT,
                SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, strlen(prefix));
    expect_fail("INSERT INTO t VALUES (-9223372036854775809)", SQLSTMT_LIMIT,
                SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, strlen(prefix));
    expect_fail("INSERT INTO t VALUES (1e999)", SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE,
                strlen(prefix));
    expect_fail("INSERT INTO t VALUES (1e-999)", SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE,
                strlen(prefix));
    expect_syntax("INSERT INTO t VALUES (-)", SQLSTMT_REASON_EXPECTED_VALUE, at("INSERT INTO t VALUES (-)", ")"));
    /* 문자열·NULL의 단항 부호는 문법상 식이다. 형식 검사는 실행기 몫이다 */
    statement = parse_ok("INSERT INTO t VALUES (- 'a', -NULL, - - 3)");
    assert(statement->insert.values[0]->kind == SQLEXPR_UNARY && statement->insert.values[0]->op == SQLEXPR_OP_NEG);
    assert(statement->insert.values[1]->kind == SQLEXPR_UNARY && is_null(statement->insert.values[1]->args[0]));
    assert(statement->insert.values[2]->kind == SQLEXPR_UNARY && is_integer(statement->insert.values[2]->args[0], -3));
    free_clean(statement);

    /* 숫자 리터럴은 4096바이트까지다. 앞의 0이 길어도 값이 작으면 같은 한도로 거절한다. */
    char digits[5000];
    char sql[5200];
    memset(digits, '0', sizeof(digits) - 1);
    digits[sizeof(digits) - 1] = '\0';
    digits[4095] = '1';
    digits[4096] = '\0';
    sprintf(sql, "INSERT INTO t VALUES (%s)", digits);
    statement = parse_ok(sql);
    assert(is_integer(statement->insert.values[0], 1));
    free_clean(statement);
    digits[4096] = '0';
    digits[4097] = '\0';
    sprintf(sql, "INSERT INTO t VALUES (%s)", digits);
    expect_fail(sql, SQLSTMT_LIMIT, SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, strlen(prefix));
}

static void parameters(void)
{
    uint8_t text[] = {'a', 0, 'b'};
    uint8_t blob[] = {0xff, 0x00};
    value_view_t values[] = {
        {VALUE_INTEGER, 42, 0.0, NULL, 0},
        {VALUE_TEXT, 0, 0.0, text, sizeof(text)},
        {VALUE_BLOB, 0, 0.0, blob, sizeof(blob)},
        {VALUE_NULL, 0, 0.0, NULL, 0},
        {VALUE_REAL, 0, -0.0, NULL, 0},
    };
    sqlstmt_t *statement = parse_bound("INSERT INTO t VALUES (?, ?, ?, ?, ?)", values, 5);
    sqlstmt_insert_t *insert = &statement->insert;
    /* binding이 바뀌어도 복사한 값은 그대로다. */
    memset(text, 'Z', sizeof(text));
    memset(blob, 'Z', sizeof(blob));
    assert(is_integer(insert->values[0], 42) && is_text(insert->values[1], "a\0b", 3));
    assert(is_blob(insert->values[2], "\xff\x00", 2) && is_null(insert->values[3]));
    assert(insert->values[4]->value.view.type == VALUE_REAL && signbit(insert->values[4]->value.view.real));
    free_clean(statement);

    text[0] = 'x';
    text[1] = 'y';
    text[2] = 'z';
    /* ?N은 번호로 찾고 같은 번호를 다시 써도 된다. */
    statement = parse_bound("UPDATE t SET a = ?2, b = ?1 WHERE c >= ?2", values, 2);
    assert(is_text(statement->update.assignments[0].value, "xyz", 3));
    assert(is_integer(statement->update.assignments[1].value, 42));
    assert(is_compare(statement->update.where, "c", SQLEXPR_OP_GE));
    assert(is_text(statement->update.where->args[1], "xyz", 3));
    free_clean(statement);

    expect_fail_bound("INSERT INTO t VALUES (?)", NULL, 0, SQLSTMT_INVALID,
                      SQLSTMT_REASON_MISSING_BINDING, at("INSERT INTO t VALUES (?)", "?"));
    /* 개수 검사는 lexer_check_binding 몫이지만 범위를 벗어난 id는 거절한다. */
    expect_fail_bound("INSERT INTO t VALUES (?1, ?2)", values, 1, SQLSTMT_INVALID,
                      SQLSTMT_REASON_BAD_BINDING, at("INSERT INTO t VALUES (?1, ?2)", "?2"));

    value_view_t bad[] = {{VALUE_TEXT, 0, 0.0, NULL, 3}};
    sqlstmt_t *out = NULL;
    sqlstmt_error_t error;
    assert(run("INSERT INTO t VALUES (?)", 24, bad, 1, &out, &error) == SQLSTMT_INVALID);
    assert(error.reason == SQLSTMT_REASON_BAD_BINDING && error.value_status == VALUE_INVALID);
    assert(out == NULL && live_allocations == 0);
}

static void select_forms(void)
{
    sqlstmt_t *statement = parse_ok("SELECT * FROM t;");
    assert(statement->kind == SQLSTMT_SELECT && statement->select.item_count == 1 && statement->select.items[0].star);
    assert(statement->select.where == NULL && statement->select.has_from);
    free_clean(statement);

    statement = parse_ok("SELECT a, b, a FROM t WHERE c <> 'x'");
    assert(statement->select.item_count == 3 && is_compare(statement->select.where, "c", SQLEXPR_OP_NE));
    assert(is_text(statement->select.where->args[1], "x", 1));
    free_clean(statement);

    struct {
        const char *sql;
        sqlexpr_op_t op;
    } cases[] = {
        {"SELECT * FROM t WHERE a = 1", SQLEXPR_OP_EQ},  {"SELECT * FROM t WHERE a == 1", SQLEXPR_OP_EQ},
        {"SELECT * FROM t WHERE a != 1", SQLEXPR_OP_NE}, {"SELECT * FROM t WHERE a <> 1", SQLEXPR_OP_NE},
        {"SELECT * FROM t WHERE a < 1", SQLEXPR_OP_LT},  {"SELECT * FROM t WHERE a <= 1", SQLEXPR_OP_LE},
        {"SELECT * FROM t WHERE a > 1", SQLEXPR_OP_GT},  {"SELECT * FROM t WHERE a >= 1", SQLEXPR_OP_GE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        statement = parse_ok(cases[i].sql);
        assert(is_compare(statement->select.where, "a", cases[i].op) && is_integer(statement->select.where->args[1], 1));
        free_clean(statement);
    }

    /* NULL과의 = 비교도 그대로 구조에 담는다. */
    statement = parse_ok("SELECT * FROM t WHERE a = NULL");
    assert(is_compare(statement->select.where, "a", SQLEXPR_OP_EQ) && is_null(statement->select.where->args[1]));
    free_clean(statement);

    /* 우선순위: OR < AND < NOT < 비교 < +,- < *,/,% < || < 단항 */
    statement = parse_ok("SELECT * FROM t WHERE a = 1 OR b = 2 AND NOT c = 3");
    const sqlexpr_t *w = statement->select.where;
    assert(w->op == SQLEXPR_OP_OR && is_compare(w->args[0], "a", SQLEXPR_OP_EQ));
    assert(w->args[1]->op == SQLEXPR_OP_AND && w->args[1]->args[1]->kind == SQLEXPR_UNARY);
    assert(w->args[1]->args[1]->op == SQLEXPR_OP_NOT && is_compare(w->args[1]->args[1]->args[0], "c", SQLEXPR_OP_EQ));
    free_clean(statement);

    statement = parse_ok("SELECT 1 + 2 * 3 - 4 % 5 || 'x' AS v");
    assert(!statement->select.has_from && name_is(&statement->select.items[0].alias, "v"));
    const sqlexpr_t *e = statement->select.items[0].expr;
    assert(e->op == SQLEXPR_OP_SUB && e->args[0]->op == SQLEXPR_OP_ADD && e->args[0]->args[1]->op == SQLEXPR_OP_MUL);
    assert(e->args[1]->op == SQLEXPR_OP_MOD && e->args[1]->args[1]->op == SQLEXPR_OP_CONCAT);
    free_clean(statement);

    statement = parse_ok("SELECT * FROM t WHERE a BETWEEN 1 AND 2 AND b NOT IN (1, 2, 3) AND c LIKE 'x%' ESCAPE '!'"
                         " AND d IS NULL AND e NOT BETWEEN 0 AND 9 AND f NOT LIKE '_'");
    w = statement->select.where;
    assert(w->op == SQLEXPR_OP_AND && w->args[1]->kind == SQLEXPR_LIKE && w->args[1]->negated);
    const sqlexpr_t *chain = w;
    size_t conjuncts = 1;
    while (chain->kind == SQLEXPR_BINARY && chain->op == SQLEXPR_OP_AND) {
        conjuncts++;
        chain = chain->args[0];
    }
    assert(conjuncts == 6 && chain->kind == SQLEXPR_BETWEEN && !chain->negated && chain->arg_count == 3);
    free_clean(statement);

    statement = parse_ok("SELECT CASE WHEN a > 1 THEN 'big' WHEN a = 1 THEN 'one' ELSE 'small' END, "
                         "CASE a WHEN 1 THEN 2 END, CAST(a AS TEXT), coalesce(a, b, 0), t.a, u.* FROM t AS u");
    const sqlstmt_select_t *q = &statement->select;
    assert(q->item_count == 6 && q->items[0].expr->kind == SQLEXPR_CASE && q->items[0].expr->has_else);
    assert(!q->items[0].expr->has_operand && q->items[0].expr->arg_count == 5);
    assert(q->items[1].expr->has_operand && !q->items[1].expr->has_else && q->items[1].expr->arg_count == 3);
    assert(q->items[2].expr->kind == SQLEXPR_CAST && q->items[2].expr->cast_type == VALUE_TEXT);
    assert(q->items[3].expr->kind == SQLEXPR_FUNCTION && name_is(&q->items[3].expr->name, "COALESCE"));
    assert(name_is(&q->items[4].expr->qualifier, "t") && name_is(&q->items[4].expr->name, "a"));
    assert(q->items[5].star && name_is(&q->items[5].qualifier, "u") && name_is(&q->table_alias, "u"));
    free_clean(statement);

    statement = parse_ok("SELECT DISTINCT a x, b FROM t u WHERE TRUE ORDER BY 1 DESC, b NULLS LAST, a + 1 ASC NULLS FIRST "
                         "LIMIT 10 OFFSET 2");
    q = &statement->select;
    assert(q->distinct && name_is(&q->items[0].alias, "x") && name_is(&q->table_alias, "u"));
    assert(is_integer(q->where, 1) && q->order_count == 3 && q->order[0].desc && is_integer(q->order[0].expr, 1));
    assert(!q->order[1].desc && q->order[1].nulls == SQLSTMT_NULLS_LAST);
    assert(q->order[2].nulls == SQLSTMT_NULLS_FIRST && q->order[2].expr->op == SQLEXPR_OP_ADD);
    assert(is_integer(q->limit, 10) && q->offset != NULL);
    free_clean(statement);

    statement = parse_ok("SELECT a, sum(b) FROM t WHERE c > 0 GROUP BY a, c + 1 HAVING count(*) > 1 ORDER BY 2");
    q = &statement->select;
    assert(q->group_count == 2 && is_column(q->group_by[0], "a") && q->group_by[1]->op == SQLEXPR_OP_ADD);
    assert(q->having != NULL && q->having->op == SQLEXPR_OP_GT && q->order_count == 1);
    free_clean(statement);

    statement = parse_ok("SELECT count(*), count(DISTINCT a), f() FROM t");
    q = &statement->select;
    assert(q->items[0].expr->star && q->items[1].expr->distinct && q->items[2].expr->arg_count == 0);
    free_clean(statement);
}

static void update_delete_drop(void)
{
    sqlstmt_t *statement = parse_ok("UPDATE t SET a = 1, \"b c\" = NULL, d = 'x' WHERE id = 7");
    assert(statement->kind == SQLSTMT_UPDATE && statement->update.assignment_count == 3);
    assert(name_is(&statement->update.assignments[1].column, "b c"));
    assert(is_null(statement->update.assignments[1].value));
    assert(is_compare(statement->update.where, "id", SQLEXPR_OP_EQ) && is_integer(statement->update.where->args[1], 7));
    free_clean(statement);

    statement = parse_ok("UPDATE t SET a = a + 1, b = upper(b)");
    assert(statement->update.where == NULL && statement->update.assignments[0].value->op == SQLEXPR_OP_ADD);
    free_clean(statement);

    statement = parse_ok("DELETE FROM t WHERE a < -1.5");
    assert(statement->kind == SQLSTMT_DELETE && name_is(&statement->del.table, "t"));
    assert(is_compare(statement->del.where, "a", SQLEXPR_OP_LT) && is_real(statement->del.where->args[1], -1.5));
    free_clean(statement);

    statement = parse_ok("DELETE FROM t");
    assert(statement->del.where == NULL);
    free_clean(statement);

    statement = parse_ok("DROP TABLE \"t u\";");
    assert(statement->kind == SQLSTMT_DROP_TABLE && name_is(&statement->drop.table, "t u"));
    free_clean(statement);
}

static void transaction_control(void)
{
    sqlstmt_t *statement = parse_ok("BEGIN");
    assert(statement->kind == SQLSTMT_BEGIN);
    free_clean(statement);
    statement = parse_ok("begin transaction;");
    assert(statement->kind == SQLSTMT_BEGIN);
    free_clean(statement);
    statement = parse_ok("Start Transaction");
    assert(statement->kind == SQLSTMT_BEGIN);
    free_clean(statement);
    statement = parse_ok("COMMIT");
    assert(statement->kind == SQLSTMT_COMMIT);
    free_clean(statement);
    statement = parse_ok("commit transaction");
    assert(statement->kind == SQLSTMT_COMMIT);
    free_clean(statement);
    statement = parse_ok("ROLLBACK");
    assert(statement->kind == SQLSTMT_ROLLBACK);
    free_clean(statement);
    statement = parse_ok("ROLLBACK TRANSACTION;");
    assert(statement->kind == SQLSTMT_ROLLBACK);
    free_clean(statement);

    expect_syntax("START", SQLSTMT_REASON_UNEXPECTED_END, 5);
    expect_syntax("START DURABLE", SQLSTMT_REASON_EXPECTED_KEYWORD, 6);
    expect_syntax("BEGIN DURABLE", SQLSTMT_REASON_TRAILING_TOKENS, 6);
    expect_syntax("BEGIN TRANSACTION VOLATILE", SQLSTMT_REASON_TRAILING_TOKENS, 18);
    expect_syntax("BEGIN x", SQLSTMT_REASON_TRAILING_TOKENS, 6);
    expect_syntax("COMMIT x", SQLSTMT_REASON_TRAILING_TOKENS, 7);
    expect_syntax("COMMIT TRANSACTION TRANSACTION", SQLSTMT_REASON_TRAILING_TOKENS, 19);
    expect_syntax("ROLLBACK TO x", SQLSTMT_REASON_TRAILING_TOKENS, 9);
}

static void create_errors(void)
{
    expect_syntax("FROBNICATE x", SQLSTMT_REASON_UNKNOWN_STATEMENT, 0);
    expect_syntax("x", SQLSTMT_REASON_UNKNOWN_STATEMENT, 0);
    expect_syntax("\"select\" * FROM t", SQLSTMT_REASON_UNKNOWN_STATEMENT, 0);
    expect_syntax("CREATE", SQLSTMT_REASON_UNEXPECTED_END, 6);
    expect_syntax("CREATE TABLE t (", SQLSTMT_REASON_UNEXPECTED_END, 16);
    expect_syntax("CREATE TABLE t (a INTEGER", SQLSTMT_REASON_UNEXPECTED_END, 25);
    expect_syntax("CREATE TABLE (a INTEGER)", SQLSTMT_REASON_EXPECTED_IDENTIFIER, 13);
    expect_syntax("CREATE TABLE select (a INTEGER)", SQLSTMT_REASON_EXPECTED_IDENTIFIER, 13);
    expect_syntax("CREATE TABLE t ()", SQLSTMT_REASON_EXPECTED_IDENTIFIER, 16);
    expect_syntax("CREATE TABLE t (a)", SQLSTMT_REASON_EXPECTED_KEYWORD, 17);
    expect_syntax("CREATE TABLE t (a INTEGER b)", SQLSTMT_REASON_EXPECTED_KEYWORD, 26);
    expect_syntax("CREATE TABLE t (a INTEGER) x", SQLSTMT_REASON_TRAILING_TOKENS, 27);
    expect_syntax("CREATE TABLE t (PRIMARY KEY (a))", SQLSTMT_REASON_NO_COLUMNS, 31);
    expect_unsupported("CREATE TABLE t (a FLOAT)", SQLSTMT_REASON_UNSUPPORTED_TYPE, 18);
    expect_unsupported("CREATE TABLE t (a DATETIME)", SQLSTMT_REASON_UNSUPPORTED_TYPE, 18);
    expect_syntax("CREATE TABLE t (a NULL)", SQLSTMT_REASON_EXPECTED_KEYWORD, 18);
    expect_syntax("CREATE TABLE t (a INTEGER, a TEXT)", SQLSTMT_REASON_DUPLICATE_COLUMN, 27);
    expect_syntax("CREATE TABLE t (a INTEGER PRIMARY KEY, b INTEGER PRIMARY KEY)",
                  SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY, 49);
    expect_syntax("CREATE TABLE t (a INTEGER PRIMARY KEY PRIMARY KEY)",
                  SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY, 38);
    expect_syntax("CREATE TABLE t (a INTEGER PRIMARY KEY, PRIMARY KEY (a))",
                  SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY, 52);
    expect_syntax("CREATE TABLE t (a INTEGER, PRIMARY KEY (a), PRIMARY KEY (a))",
                  SQLSTMT_REASON_DUPLICATE_PRIMARY_KEY, 44);
    expect_syntax("CREATE TABLE t (a INTEGER, PRIMARY KEY (z))",
                  SQLSTMT_REASON_UNKNOWN_PRIMARY_KEY_COLUMN, 40);
    expect_syntax("CREATE TABLE t (a TEXT PRIMARY KEY AUTOINCREMENT)",
                  SQLSTMT_REASON_AUTOINCREMENT_NOT_INTEGER_KEY, 35);
    expect_syntax("CREATE TABLE t (a INTEGER AUTOINCREMENT)",
                  SQLSTMT_REASON_AUTOINCREMENT_NOT_INTEGER_KEY, 26);
    expect_syntax("CREATE TABLE t (a INTEGER, PRIMARY KEY (a) AUTOINCREMENT)",
                  SQLSTMT_REASON_EXPECTED_KEYWORD, 43);
    expect_unsupported("CREATE TABLE t (a INTEGER, b INTEGER, PRIMARY KEY (a, b))",
                       SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 52);
    expect_unsupported("CREATE TABLE t (a INTEGER UNIQUE)", SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 26);
    expect_unsupported("CREATE TABLE t (a INTEGER DEFAULT 1)", SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 26);
    expect_unsupported("CREATE TABLE t (a INTEGER CHECK (a > 0))", SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 26);
    expect_unsupported("CREATE TABLE t (a INTEGER REFERENCES u (b))", SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 26);
    expect_unsupported("CREATE TABLE t (a INTEGER, UNIQUE (a))", SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 27);
    expect_unsupported("CREATE TABLE t (a INTEGER, FOREIGN KEY (a) REFERENCES u (b))",
                       SQLSTMT_REASON_UNSUPPORTED_CONSTRAINT, 27);
    expect_unsupported("CREATE TABLE IF NOT EXISTS t (a INTEGER)", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 13);
    expect_unsupported("CREATE TABLE t (a INTEGER) AS SELECT 1", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 27);
    expect_unsupported("CREATE TABLE t.u (a INTEGER)", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 14);
}

static void varchar_errors(void)
{
    sqlstmt_t *statement = parse_ok("CREATE TABLE t (a VARCHAR(4294967295), b varchar ( 007 ))");
    assert(statement->create.columns[0].varchar_length == UINT32_MAX);
    assert(statement->create.columns[1].varchar_length == 7);
    free_clean(statement);
    expect_syntax("CREATE TABLE t (a VARCHAR)", SQLSTMT_REASON_EXPECTED_KEYWORD, 25);
    expect_syntax("CREATE TABLE t (a VARCHAR(0))", SQLSTMT_REASON_BAD_VARCHAR_LENGTH, 26);
    expect_syntax("CREATE TABLE t (a VARCHAR(1.5))", SQLSTMT_REASON_BAD_VARCHAR_LENGTH, 26);
    expect_syntax("CREATE TABLE t (a VARCHAR(1e2))", SQLSTMT_REASON_BAD_VARCHAR_LENGTH, 26);
    expect_syntax("CREATE TABLE t (a VARCHAR(x))", SQLSTMT_REASON_BAD_VARCHAR_LENGTH, 26);
    expect_syntax("CREATE TABLE t (a VARCHAR(-1))", SQLSTMT_REASON_BAD_VARCHAR_LENGTH, 26);
    expect_syntax("CREATE TABLE t (a VARCHAR(5 6))", SQLSTMT_REASON_EXPECTED_KEYWORD, 28);
    expect_fail("CREATE TABLE t (a VARCHAR(4294967296))", SQLSTMT_LIMIT,
                SQLSTMT_REASON_NUMBER_OUT_OF_RANGE, 26);
}

static void insert_errors(void)
{
    expect_syntax("INSERT t VALUES (1)", SQLSTMT_REASON_EXPECTED_KEYWORD, 7);
    expect_syntax("INSERT INTO", SQLSTMT_REASON_UNEXPECTED_END, 11);
    expect_syntax("INSERT INTO t", SQLSTMT_REASON_UNEXPECTED_END, 13);
    expect_syntax("INSERT INTO t DEFAULT VALUES", SQLSTMT_REASON_EXPECTED_KEYWORD, 14);
    expect_unsupported("INSERT INTO t SELECT 1", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 14);
    expect_syntax("INSERT INTO t VALUES ()", SQLSTMT_REASON_EXPECTED_VALUE, 22);
    expect_syntax("INSERT INTO t VALUES (1", SQLSTMT_REASON_UNEXPECTED_END, 23);
    expect_syntax("INSERT INTO t VALUES (1,", SQLSTMT_REASON_UNEXPECTED_END, 24);
    expect_syntax("INSERT INTO t VALUES (1 2)", SQLSTMT_REASON_EXPECTED_KEYWORD, 24);
    expect_syntax("INSERT INTO t VALUES (X 'ab')", SQLSTMT_REASON_EXPECTED_VALUE, 22);
    expect_syntax("INSERT INTO t VALUES (X'0')", SQLSTMT_REASON_BAD_BLOB_LITERAL, 23);
    expect_syntax("INSERT INTO t VALUES (X'zz')", SQLSTMT_REASON_BAD_BLOB_LITERAL, 23);
    expect_syntax("INSERT INTO t () VALUES ()", SQLSTMT_REASON_EXPECTED_IDENTIFIER, 15);
    expect_syntax("INSERT INTO t (a, b) VALUES (1)", SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH, 30);
    expect_syntax("INSERT INTO t (a) VALUES (1, 2)", SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH, 30);
    expect_syntax("INSERT INTO t (a, a) VALUES (1, 2)", SQLSTMT_REASON_DUPLICATE_COLUMN, 18);
    expect_syntax("INSERT INTO t (a, b VALUES (1, 2)", SQLSTMT_REASON_EXPECTED_KEYWORD, 20);
    expect_syntax("INSERT INTO t VALUES (1) x", SQLSTMT_REASON_TRAILING_TOKENS, 25);
    expect_syntax("INSERT INTO t VALUES (1), (2, 3)", SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH, 31);
    expect_syntax("INSERT INTO t VALUES (1, 2), (3)", SQLSTMT_REASON_COLUMN_VALUE_COUNT_MISMATCH, 31);
    expect_syntax("INSERT INTO t VALUES (1),", SQLSTMT_REASON_UNEXPECTED_END, 25);
    expect_unsupported("INSERT INTO t.u VALUES (1)", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 13);
}

static void select_and_where_errors(void)
{
    expect_syntax("SELECT", SQLSTMT_REASON_UNEXPECTED_END, 6);
    expect_syntax("SELECT FROM t", SQLSTMT_REASON_EXPECTED_VALUE, 7);
    expect_syntax("SELECT a, FROM t", SQLSTMT_REASON_EXPECTED_VALUE, 10);
    expect_syntax("SELECT a b c FROM t", SQLSTMT_REASON_TRAILING_TOKENS, 11);
    expect_syntax("SELECT * a FROM t", SQLSTMT_REASON_TRAILING_TOKENS, 9);
    expect_syntax("SELECT * FROM", SQLSTMT_REASON_UNEXPECTED_END, 13);
    expect_syntax("SELECT * FROM t u v", SQLSTMT_REASON_TRAILING_TOKENS, 18);
    expect_syntax("SELECT * FROM t WHERE", SQLSTMT_REASON_UNEXPECTED_END, 21);
    expect_syntax("SELECT * FROM t WHERE a =", SQLSTMT_REASON_UNEXPECTED_END, 25);
    expect_syntax("SELECT * FROM t WHERE a IS 5", SQLSTMT_REASON_EXPECTED_KEYWORD, 27);
    expect_syntax("SELECT * FROM t WHERE a IS NOT 5", SQLSTMT_REASON_EXPECTED_KEYWORD, 31);
    expect_syntax("SELECT * FROM t WHERE a NOT 5", SQLSTMT_REASON_TRAILING_TOKENS, 24);
    expect_syntax("SELECT * FROM t WHERE a BETWEEN 1", SQLSTMT_REASON_UNEXPECTED_END, 33);
    expect_syntax("SELECT * FROM t WHERE a IN ()", SQLSTMT_REASON_EXPECTED_VALUE, 28);
    expect_syntax("SELECT * FROM t WHERE (a = 1", SQLSTMT_REASON_UNEXPECTED_END, 28);
    expect_syntax("SELECT * FROM t WHERE a = 1 WHERE b = 2", SQLSTMT_REASON_TRAILING_TOKENS, 28);
    expect_syntax("SELECT CASE END FROM t", SQLSTMT_REASON_EXPECTED_VALUE, 12);
    expect_syntax("SELECT CASE WHEN 1 THEN 2 FROM t", SQLSTMT_REASON_EXPECTED_KEYWORD, 26);
    expect_syntax("SELECT CAST(a TEXT) FROM t", SQLSTMT_REASON_EXPECTED_KEYWORD, 14);
    expect_syntax("SELECT * FROM t ORDER a", SQLSTMT_REASON_EXPECTED_KEYWORD, 22);
    expect_syntax("SELECT * FROM t ORDER BY a NULLS", SQLSTMT_REASON_UNEXPECTED_END, 32);
    expect_syntax("SELECT * FROM t LIMIT", SQLSTMT_REASON_UNEXPECTED_END, 21);
    expect_syntax("SELECT a FROM t OFFSET 1 LIMIT 2", SQLSTMT_REASON_TRAILING_TOKENS, 25);

    expect_syntax("SELECT a FROM t GROUP a", SQLSTMT_REASON_EXPECTED_KEYWORD, 22);
    expect_syntax("SELECT a FROM t HAVING", SQLSTMT_REASON_UNEXPECTED_END, 22);
    expect_unsupported("SELECT * FROM t JOIN u", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 16);
    expect_unsupported("SELECT * FROM t, u", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 15);
    expect_unsupported("SELECT * FROM t UNION SELECT * FROM u", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 16);
    expect_unsupported("SELECT * FROM t WHERE a IN (SELECT b FROM u)", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 28);
    expect_unsupported("SELECT (SELECT 1)", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 7);
    expect_unsupported("SELECT * FROM t WHERE EXISTS (SELECT 1)", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 22);
    expect_unsupported("SELECT * FROM t.u", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 15);
    expect_syntax("DELETE FROM t ORDER BY a", SQLSTMT_REASON_TRAILING_TOKENS, 14);
    expect_unsupported("UPDATE t SET a = 1 RETURNING a", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 19);
}

static void update_delete_drop_errors(void)
{
    expect_syntax("UPDATE", SQLSTMT_REASON_UNEXPECTED_END, 6);
    expect_syntax("UPDATE t", SQLSTMT_REASON_UNEXPECTED_END, 8);
    expect_syntax("UPDATE t a = 1", SQLSTMT_REASON_EXPECTED_KEYWORD, 9);
    expect_syntax("UPDATE t SET", SQLSTMT_REASON_UNEXPECTED_END, 12);
    expect_syntax("UPDATE t SET a 1", SQLSTMT_REASON_EXPECTED_KEYWORD, 15);
    expect_syntax("UPDATE t SET a =", SQLSTMT_REASON_UNEXPECTED_END, 16);
    expect_syntax("UPDATE t SET a = 1,", SQLSTMT_REASON_UNEXPECTED_END, 19);
    expect_syntax("UPDATE t SET a = 1, a = 2", SQLSTMT_REASON_DUPLICATE_COLUMN, 20);
    expect_syntax("UPDATE t SET a = 1 b", SQLSTMT_REASON_TRAILING_TOKENS, 19);
    expect_syntax("DELETE t", SQLSTMT_REASON_EXPECTED_KEYWORD, 7);
    expect_syntax("DELETE FROM", SQLSTMT_REASON_UNEXPECTED_END, 11);
    expect_syntax("DELETE FROM t WHERE", SQLSTMT_REASON_UNEXPECTED_END, 19);
    expect_syntax("DELETE FROM t x", SQLSTMT_REASON_TRAILING_TOKENS, 14);
    expect_syntax("DROP t", SQLSTMT_REASON_EXPECTED_KEYWORD, 5);
    expect_syntax("DROP TABLE", SQLSTMT_REASON_UNEXPECTED_END, 10);
    expect_syntax("DROP TABLE t u", SQLSTMT_REASON_TRAILING_TOKENS, 13);
    expect_syntax("DROP TABLE t, u", SQLSTMT_REASON_TRAILING_TOKENS, 12);
    expect_syntax("DROP TABLE select", SQLSTMT_REASON_EXPECTED_IDENTIFIER, 11);
    expect_unsupported("DROP TABLE IF EXISTS t", SQLSTMT_REASON_UNSUPPORTED_CLAUSE, 11);
    expect_unsupported("DROP TABLE t.u", SQLSTMT_REASON_UNSUPPORTED_EXPRESSION, 12);
}

static void limits(void)
{
    char name[400];
    char sql[800];
    memset(name, 'n', 255);
    name[255] = '\0';
    sprintf(sql, "DROP TABLE %s", name);
    sqlstmt_t *statement = parse_ok(sql);
    assert(statement->drop.table.size == 255);
    free_clean(statement);
    name[255] = 'n';
    name[256] = '\0';
    sprintf(sql, "DROP TABLE %s", name);
    expect_fail(sql, SQLSTMT_LIMIT, SQLSTMT_REASON_NAME_TOO_LONG, 11);
    sprintf(sql, "DROP TABLE \"%s\"", name);
    expect_fail(sql, SQLSTMT_LIMIT, SQLSTMT_REASON_NAME_TOO_LONG, 11);

    /* 컬럼·선택 항목·SET 항목은 CATALOG_MAX_COLUMNS개까지다(INSERT 값은 행 수를 곱해 더 많을 수 있다). */
    size_t max = CATALOG_MAX_COLUMNS;
    for (int kind = 0; kind < 4; kind++) {
        if (kind == 1) continue;
        for (size_t count = max; count <= max + 1; count++) {
            size_t capacity = 64 * (count + 4);
            char *big = malloc(capacity);
            assert(big != NULL);
            size_t used = 0;
            size_t offending = 0;
            const char *head[] = {"CREATE TABLE t (", "INSERT INTO t VALUES (", "SELECT ",
                                  "UPDATE t SET "};
            used += (size_t)sprintf(big + used, "%s", head[kind]);
            for (size_t i = 0; i < count; i++) {
                if (i > 0) used += (size_t)sprintf(big + used, ", ");
                offending = used;
                if (kind == 0) used += (size_t)sprintf(big + used, "c%zu INTEGER", i);
                if (kind == 1) used += (size_t)sprintf(big + used, "%zu", i);
                if (kind == 2) used += (size_t)sprintf(big + used, "c%zu", i);
                if (kind == 3) used += (size_t)sprintf(big + used, "c%zu = %zu", i, i);
            }
            if (kind == 0 || kind == 1) used += (size_t)sprintf(big + used, ")");
            if (kind == 2) used += (size_t)sprintf(big + used, " FROM t");
            sqlstmt_t *out = NULL;
            sqlstmt_error_t error;
            sqlstmt_status_t status = run(big, used, NULL, 0, &out, &error);
            if (count == max) {
                assert(status == SQLSTMT_OK && out != NULL);
                free_clean(out);
            } else {
                assert(status == SQLSTMT_LIMIT && out == NULL);
                assert(error.reason == SQLSTMT_REASON_TOO_MANY_ITEMS && error.offset == offending);
                assert(live_allocations == 0);
            }
            free(big);
        }
    }
}

static void bad_arguments(void)
{
    sqlstmt_t *out = NULL;
    sqlstmt_error_t error = {SQLSTMT_REASON_NONE, 5, VALUE_OK};
    lexer_statement_t empty = {NULL, 0, 0, 0, false};
    assert(sqlstmt_parse(NULL, NULL, &out, &error) == SQLSTMT_INVALID);
    assert(error.reason == SQLSTMT_REASON_BAD_ARGUMENT && out == NULL);
    assert(sqlstmt_parse(&empty, NULL, &out, &error) == SQLSTMT_INVALID);
    assert(error.reason == SQLSTMT_REASON_BAD_ARGUMENT);
    assert(sqlstmt_parse(&empty, NULL, NULL, NULL) == SQLSTMT_INVALID);
    sqlstmt_free(NULL);
}

static void statement_picks_one_from_script(void)
{
    const char *sql = "DROP TABLE a; SELECT * FROM b WHERE x = 1; INSERT INTO c VALUES (2);";
    lexed_t *lexed = NULL;
    lexer_error_t lexer_error;
    assert(lexer_scan((const uint8_t *)sql, strlen(sql), &LIMITS, &lexed, &lexer_error) == LEXER_OK);
    assert(lexed_statement_count(lexed) == 3);
    sqlstmt_kind_t kinds[] = {SQLSTMT_DROP_TABLE, SQLSTMT_SELECT, SQLSTMT_INSERT};
    for (size_t i = 0; i < 3; i++) {
        lexer_statement_t statement;
        assert(lexed_statement_at(lexed, i, &statement) == LEXER_OK);
        sqlstmt_t *parsed = NULL;
        assert(sqlstmt_parse(&statement, NULL, &parsed, NULL) == SQLSTMT_OK);
        assert(parsed->kind == kinds[i]);
        free_clean(parsed);
    }
    lexer_free(lexed);
}

/*
 * 모든 할당 지점에서 한 번씩 실패시킨다. 실패하면 NOMEM이고 *out은 그대로이며 이번 호출이 할당한
 * 것은 하나도 남지 않는다. 실패가 사라질 때까지 지점을 올리면 성공한 결과도 정확히 해제된다.
 */
static void allocation_failures(void)
{
    uint8_t blob[] = {1, 2, 3};
    value_view_t values[] = {
        {VALUE_TEXT, 0, 0.0, (const uint8_t *)"abc", 3},
        {VALUE_BLOB, 0, 0.0, blob, sizeof(blob)},
    };
    const char *inputs[] = {
        "CREATE TABLE t (a INTEGER PRIMARY KEY AUTOINCREMENT, b VARCHAR(5) NOT NULL, c REAL)",
        "CREATE TABLE t (a INTEGER, b TEXT, PRIMARY KEY (b))",
        "INSERT INTO t (a, b, c, d, e, f) VALUES (1, 'x', X'0a0b', 2.5, ?1, ?2)",
        "INSERT INTO t VALUES (1, -2, 'long text value', NULL, X'ff')",
        "SELECT a, b, c, d, e FROM t WHERE a >= 'x'",
        "SELECT * FROM t WHERE a IS NOT NULL",
        "SELECT DISTINCT a + 1 AS x, CASE WHEN b IN (1, 2) THEN 'y' ELSE lower(c) END FROM t u "
        "WHERE a BETWEEN 1 AND 9 AND c LIKE 'a%' OR NOT d IS NULL ORDER BY 1 DESC, x LIMIT 3 OFFSET 1",
        "INSERT INTO t VALUES (1, 'a' || 'b'), (2, CAST(?1 AS BLOB))",
        "UPDATE t SET a = 1, b = ?1, c = X'01', d = -1.5 WHERE e <> ?2",
        "DELETE FROM t WHERE \"column name\" = 'v'",
        "DROP TABLE t",
    };
    size_t injected = 0;
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        for (size_t point = 1;; point++) {
            static char marker;
            sqlstmt_t *out = (sqlstmt_t *)&marker;
            sqlstmt_error_t error = {SQLSTMT_REASON_BAD_ARGUMENT, 7, VALUE_OK};
            size_t before = live_allocations;
            allocation_calls = 0;
            fail_at = point;
            sqlstmt_status_t status =
                run(inputs[i], strlen(inputs[i]), values, 2, &out, &error);
            fail_at = 0;
            if (status == SQLSTMT_OK) {
                assert(out != (sqlstmt_t *)&marker && allocation_calls < point);
                sqlstmt_free(out);
                assert(live_allocations == before);
                break;
            }
            if (status != SQLSTMT_NOMEM) {
                fprintf(stderr, "input %s point %zu: status %d reason %d\n", inputs[i], point,
                        status, error.reason);
            }
            assert(status == SQLSTMT_NOMEM);
            assert(out == (sqlstmt_t *)&marker);
            assert(live_allocations == before);
            injected++;
        }
    }
    /* 치환이 빠진 빌드라면 실패를 한 번도 주입하지 못한다. */
    assert(injected >= 60);
    printf("sqlstmt: %zu allocation failures injected and cleaned\n", injected);
}

int main(void)
{
    /* 치환이 적용된 빌드인지 확인한다. */
    sqlstmt_t *probe = parse_ok("DROP TABLE t");
    assert(allocation_calls > 0 && live_allocations > 0);
    free_clean(probe);

    create_all_types();
    create_index_forms();
    create_primary_key_forms();
    identifiers_and_case();
    insert_values();
    text_values_keep_every_byte();
    numeric_literals();
    parameters();
    select_forms();
    update_delete_drop();
    transaction_control();
    create_errors();
    varchar_errors();
    insert_errors();
    select_and_where_errors();
    update_delete_drop_errors();
    limits();
    bad_arguments();
    statement_picks_one_from_script();
    allocation_failures();
    assert(live_allocations == 0);
    if (mismatches != 0) {
        fprintf(stderr, "sqlstmt: %zu mismatched failure expectations\n", mismatches);
        return 1;
    }
    puts("sqlstmt: all checks passed");
    return 0;
}
