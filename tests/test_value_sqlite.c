#include <assert.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/value.h"

/*
 * #38: SQLite와 의미가 같은 부분(값 왕복·크기 비교·정확한 정수 변환)만 대조한다.
 * SQLite가 허용하지만 여기서 거절하는 엄격 변환은 known_differences에서 따로 확인하며
 * 공통 의미의 통과 개수에 넣지 않는다. 빌드는 -lsqlite3가 필요하다.
 */
static const uint8_t EMPTY_BLOB_ANCHOR[1];

static value_view_t integer_view(int64_t integer)
{
    return (value_view_t){VALUE_INTEGER, integer, 0.0, NULL, 0};
}

static value_view_t real_view(double real)
{
    return (value_view_t){VALUE_REAL, 0, real, NULL, 0};
}

static value_view_t text_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_TEXT, 0, 0.0, bytes, size};
}

static value_view_t blob_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_BLOB, 0, 0.0, bytes, size};
}

static void bind_view(sqlite3_stmt *statement, int index, const value_view_t *view)
{
    int result = SQLITE_OK;
    switch (view->type) {
        case VALUE_NULL:
            result = sqlite3_bind_null(statement, index);
            break;
        case VALUE_INTEGER:
            result = sqlite3_bind_int64(statement, index, view->integer);
            break;
        case VALUE_REAL:
            result = sqlite3_bind_double(statement, index, view->real);
            break;
        case VALUE_TEXT:
            result = sqlite3_bind_text64(statement, index,
                                         view->bytes != NULL ? (const char *)view->bytes : "",
                                         view->size_bytes, SQLITE_STATIC, SQLITE_UTF8);
            break;
        case VALUE_BLOB:
            result = sqlite3_bind_blob64(statement, index,
                                         view->bytes != NULL ? view->bytes : EMPTY_BLOB_ANCHOR,
                                         view->size_bytes, SQLITE_STATIC);
            break;
    }
    assert(result == SQLITE_OK);
}

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK);
    return statement;
}

static value_type_t our_type(int sqlite_type)
{
    switch (sqlite_type) {
        case SQLITE_INTEGER: return VALUE_INTEGER;
        case SQLITE_FLOAT: return VALUE_REAL;
        case SQLITE_TEXT: return VALUE_TEXT;
        case SQLITE_BLOB: return VALUE_BLOB;
        default: return VALUE_NULL;
    }
}

static int our_order(const value_view_t *left, const value_view_t *right)
{
    int order = 99;
    assert(value_compare(left, right, &order) == VALUE_OK);
    return order;
}

/* 컬럼 타입 없는 테이블에서 저장 뒤 읽은 값이 같은 타입·바이트·크기다. */
static void stored_values_match(sqlite3 *db, const value_view_t *values, size_t count)
{
    sqlite3_stmt *insert = prepare(db, "INSERT INTO t(v) VALUES (?1)");
    sqlite3_stmt *select = prepare(db, "SELECT v FROM t WHERE rowid = ?1");
    for (size_t i = 0; i < count; i++) {
        sqlite3_reset(insert);
        bind_view(insert, 1, &values[i]);
        assert(sqlite3_step(insert) == SQLITE_DONE);
        sqlite3_reset(select);
        sqlite3_bind_int64(select, 1, sqlite3_last_insert_rowid(db));
        assert(sqlite3_step(select) == SQLITE_ROW);
        assert(our_type(sqlite3_column_type(select, 0)) == values[i].type);
        switch (values[i].type) {
            case VALUE_INTEGER:
                assert(sqlite3_column_int64(select, 0) == values[i].integer);
                break;
            case VALUE_REAL: {
                double stored = sqlite3_column_double(select, 0);
                assert(stored == values[i].real && signbit(stored) == signbit(values[i].real));
                break;
            }
            case VALUE_TEXT:
                assert((size_t)sqlite3_column_bytes(select, 0) == values[i].size_bytes);
                assert(values[i].size_bytes == 0 ||
                       memcmp(sqlite3_column_text(select, 0), values[i].bytes,
                              values[i].size_bytes) == 0);
                break;
            case VALUE_BLOB:
                assert((size_t)sqlite3_column_bytes(select, 0) == values[i].size_bytes);
                assert(values[i].size_bytes == 0 ||
                       memcmp(sqlite3_column_blob(select, 0), values[i].bytes,
                              values[i].size_bytes) == 0);
                break;
            case VALUE_NULL:
                break;
        }
    }
    sqlite3_finalize(insert);
    sqlite3_finalize(select);
}

/* NULL이 없는 쌍은 SQLite의 <·>와, NULL이 낀 쌍은 ORDER BY 순서와 같아야 한다. */
static size_t orders_match(sqlite3 *db, const value_view_t *values, size_t count)
{
    sqlite3_stmt *compare = prepare(db, "SELECT (?1 > ?2) - (?1 < ?2)");
    sqlite3_stmt *sorted = prepare(db, "SELECT column1 FROM (VALUES (?1), (?2)) ORDER BY column1 LIMIT 1");
    size_t checked = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            int expected = our_order(&values[i], &values[j]);
            if (values[i].type != VALUE_NULL && values[j].type != VALUE_NULL) {
                sqlite3_reset(compare);
                bind_view(compare, 1, &values[i]);
                bind_view(compare, 2, &values[j]);
                assert(sqlite3_step(compare) == SQLITE_ROW);
                assert(sqlite3_column_int(compare, 0) == expected);
            } else if (expected != 0) {
                sqlite3_reset(sorted);
                bind_view(sorted, 1, &values[i]);
                bind_view(sorted, 2, &values[j]);
                assert(sqlite3_step(sorted) == SQLITE_ROW);
                /* 더 작은 쪽이 NULL이면 첫 행이 NULL이다. NULL끼리가 아니므로 정확히 하나만 NULL이다. */
                bool first_is_null = sqlite3_column_type(sorted, 0) == SQLITE_NULL;
                assert(first_is_null == ((expected < 0) == (values[i].type == VALUE_NULL)));
            }
            checked++;
        }
    }
    sqlite3_finalize(compare);
    sqlite3_finalize(sorted);
    return checked;
}

static void integer_text_casts_match(sqlite3 *db)
{
    const char *texts[] = {"0", "-1", "42", "9223372036854775807", "-9223372036854775808"};
    sqlite3_stmt *cast = prepare(db, "SELECT CAST(?1 AS INTEGER)");
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        value_view_t text = text_view(texts[i], strlen(texts[i]));
        value_t ours = {0};
        assert(value_cast(&text, VALUE_INTEGER, &ours) == VALUE_OK);
        sqlite3_reset(cast);
        bind_view(cast, 1, &text);
        assert(sqlite3_step(cast) == SQLITE_ROW);
        assert(sqlite3_column_int64(cast, 0) == ours.view.integer);
        value_clear(&ours);
    }
    sqlite3_finalize(cast);

    sqlite3_stmt *to_text = prepare(db, "SELECT CAST(?1 AS TEXT)");
    int64_t integers[] = {0, -1, INT64_MAX, INT64_MIN, 1234567890123LL};
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
        value_view_t integer = integer_view(integers[i]);
        value_t ours = {0};
        assert(value_cast(&integer, VALUE_TEXT, &ours) == VALUE_OK);
        sqlite3_reset(to_text);
        bind_view(to_text, 1, &integer);
        assert(sqlite3_step(to_text) == SQLITE_ROW);
        assert((size_t)sqlite3_column_bytes(to_text, 0) == ours.view.size_bytes);
        assert(memcmp(sqlite3_column_text(to_text, 0), ours.view.bytes, ours.view.size_bytes) == 0);
        value_clear(&ours);
    }
    sqlite3_finalize(to_text);

    /* 정확히 표현되는 정수와 실수 변환은 SQLite와 같은 값이다. */
    sqlite3_stmt *real_cast = prepare(db, "SELECT CAST(?1 AS REAL)");
    const char *real_texts[] = {"1.5", "-2.5e3", "0.1", "100"};
    for (size_t i = 0; i < sizeof(real_texts) / sizeof(real_texts[0]); i++) {
        value_view_t text = text_view(real_texts[i], strlen(real_texts[i]));
        value_t ours = {0};
        assert(value_cast(&text, VALUE_REAL, &ours) == VALUE_OK);
        sqlite3_reset(real_cast);
        bind_view(real_cast, 1, &text);
        assert(sqlite3_step(real_cast) == SQLITE_ROW);
        assert(sqlite3_column_double(real_cast, 0) == ours.view.real);
        value_clear(&ours);
    }
    sqlite3_finalize(real_cast);
}

static void expect_ours(value_view_t from, value_type_t target, value_status_t status)
{
    value_t out = {0};
    assert(value_cast(&from, target, &out) == status);
    value_clear(&out);
}

/* SQLite는 허용하지만 이 모듈이 거절하는 변환. 공통 의미 개수에 포함하지 않는다. */
static size_t known_differences(sqlite3 *db)
{
    size_t differences = 0;
    sqlite3_stmt *as_integer = prepare(db, "SELECT CAST(?1 AS INTEGER)");
    struct { const char *text; int64_t sqlite; } texts[] = {
        {"12abc", 12}, {" 12", 12}, {"1e3", 1}, {"1.9", 1}, {"abc", 0}, {"", 0}};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        value_view_t text = text_view(texts[i].text, strlen(texts[i].text));
        sqlite3_reset(as_integer);
        bind_view(as_integer, 1, &text);
        assert(sqlite3_step(as_integer) == SQLITE_ROW);
        assert(sqlite3_column_int64(as_integer, 0) == texts[i].sqlite);
        expect_ours(text, VALUE_INTEGER, VALUE_INVALID);
        differences++;
    }
    sqlite3_finalize(as_integer);

    /* 소수부가 있는 REAL을 SQLite는 잘라내고, 큰 정수를 REAL로 바꿀 때 조용히 반올림한다. */
    sqlite3_stmt *numeric = prepare(db, "SELECT CAST(?1 AS INTEGER), CAST(?1 AS REAL)");
    value_view_t fractional = real_view(2.5);
    sqlite3_reset(numeric);
    bind_view(numeric, 1, &fractional);
    assert(sqlite3_step(numeric) == SQLITE_ROW && sqlite3_column_int64(numeric, 0) == 2);
    expect_ours(fractional, VALUE_INTEGER, VALUE_OVERFLOW);
    differences++;
    value_view_t lossy = integer_view(9007199254740993LL);
    sqlite3_reset(numeric);
    bind_view(numeric, 1, &lossy);
    assert(sqlite3_step(numeric) == SQLITE_ROW);
    assert(sqlite3_column_double(numeric, 1) == 9007199254740992.0);
    expect_ours(lossy, VALUE_REAL, VALUE_OVERFLOW);
    differences++;
    sqlite3_finalize(numeric);

    /* SQLite는 NaN을 NULL로 저장하고 잘못된 UTF-8 TEXT도 그대로 저장한다. */
    sqlite3_stmt *echo = prepare(db, "SELECT ?1");
    sqlite3_bind_double(echo, 1, NAN);
    assert(sqlite3_step(echo) == SQLITE_ROW && sqlite3_column_type(echo, 0) == SQLITE_NULL);
    value_t out = {0};
    assert(value_set_real(&out, NAN) == VALUE_OVERFLOW);
    differences++;
    static const uint8_t bad_utf8[] = {0xff, 0xfe};
    sqlite3_reset(echo);
    bind_view(echo, 1, &(value_view_t){VALUE_TEXT, 0, 0.0, bad_utf8, 2});
    assert(sqlite3_step(echo) == SQLITE_ROW && sqlite3_column_bytes(echo, 0) == 2);
    assert(value_set_text(&out, bad_utf8, 2) == VALUE_INVALID);
    differences++;
    sqlite3_finalize(echo);

    /* REAL 텍스트: SQLite는 15자리 표현이라 왕복이 보장되지 않고 이 모듈은 17자리를 쓴다. */
    sqlite3_stmt *to_text = prepare(db, "SELECT CAST(?1 AS TEXT)");
    value_view_t tenth = real_view(0.1);
    sqlite3_reset(to_text);
    bind_view(to_text, 1, &tenth);
    assert(sqlite3_step(to_text) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(to_text, 0), "0.1") == 0);
    expect_ours(tenth, VALUE_TEXT, VALUE_OK);
    char buffer[40];
    size_t needed = 0;
    assert(value_format(&tenth, buffer, sizeof(buffer), &needed) == VALUE_OK);
    assert(strcmp(buffer, "0.10000000000000001") == 0);
    differences++;
    sqlite3_finalize(to_text);

    /*
     * VARCHAR(n): SQLite는 n을 강제하지 않아 n+1 문자도 절단 없이 저장한다. 이 모듈은 n+1 문자를 거절하고
     * n 문자는 통과시킨다. 문자 수 기준이라 바이트 수가 n을 넘는 한글 n 문자도 통과한다.
     */
    assert(sqlite3_exec(db, "CREATE TABLE narrow(v VARCHAR(2))", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *insert = prepare(db, "INSERT INTO narrow VALUES (?1)");
    sqlite3_stmt *sizes = prepare(db, "SELECT length(v), length(CAST(v AS BLOB)) FROM narrow WHERE rowid = ?1");
    static const char hangul3[] = "\xea\xb0\x80\xea\xb0\x80\xea\xb0\x80";   /* 3문자 9바이트 */
    value_view_t within = text_view(hangul3, 6);                           /* 2문자 6바이트 */
    value_view_t beyond = text_view(hangul3, 9);
    assert(value_check_varchar(&within, 2) == VALUE_OK);
    assert(value_check_varchar(&beyond, 2) == VALUE_LIMIT);
    bind_view(insert, 1, &beyond);
    assert(sqlite3_step(insert) == SQLITE_DONE);
    sqlite3_bind_int64(sizes, 1, sqlite3_last_insert_rowid(db));
    assert(sqlite3_step(sizes) == SQLITE_ROW);
    assert(sqlite3_column_int(sizes, 0) == 3 && sqlite3_column_int(sizes, 1) == 9);   /* 절단 없음 */
    differences++;
    sqlite3_finalize(sizes);
    sqlite3_finalize(insert);
    return differences;
}

int main(void)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE t(v)", NULL, NULL, NULL) == SQLITE_OK);

    static const uint8_t binary[] = {0x00, 0xff, 0x80, 0x00};
    static const uint8_t high[] = {0xc3, 0xa9};
    value_view_t values[] = {
        {VALUE_NULL, 0, 0.0, NULL, 0},
        integer_view(INT64_MIN), integer_view(-1), integer_view(0), integer_view(1),
        integer_view(9007199254740993LL), integer_view(INT64_MAX),
        real_view(-1e300), real_view(-0.5), real_view(-0.0), real_view(0.0), real_view(0.5),
        real_view(9007199254740992.0), real_view(9007199254740994.0),
        real_view(9223372036854775808.0), real_view(1e300), real_view(5e-324),
        text_view("", 0), text_view("a", 1), text_view("a\0b", 3), text_view("a\0c", 3),
        text_view("ab", 2), text_view(high, 2), text_view("한글🙂", 10),
        blob_view("", 0), blob_view(binary, 1), blob_view(binary, 4), blob_view("a", 1)};
    size_t count = sizeof(values) / sizeof(values[0]);

    stored_values_match(db, values, count);
    size_t pairs = orders_match(db, values, count);
    integer_text_casts_match(db);
    size_t differences = known_differences(db);
    assert(sqlite3_close(db) == SQLITE_OK);
    printf("value/sqlite %s: %zu stored values and %zu ordered pairs agree; "
           "%zu stricter conversions differ by design (not counted as agreement)\n",
           sqlite3_libversion(), count, pairs, differences);
    return 0;
}
