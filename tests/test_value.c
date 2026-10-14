#include <assert.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/value.h"

#define TWO_POW_53 9007199254740992.0
#define TWO_POW_63 9223372036854775808.0

static value_view_t integer_view(int64_t integer)
{
    return (value_view_t){VALUE_INTEGER, integer, 0.0, NULL, 0};
}

static value_view_t real_view(double real)
{
    return (value_view_t){VALUE_REAL, 0, real, NULL, 0};
}

static value_view_t null_view(void)
{
    return (value_view_t){VALUE_NULL, 0, 0.0, NULL, 0};
}

static value_view_t text_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_TEXT, 0, 0.0, bytes, size};
}

static value_view_t blob_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_BLOB, 0, 0.0, bytes, size};
}

static value_t kept_value(void)
{
    value_t value = {0};
    assert(value_set_text(&value, (const uint8_t *)"keep", 4) == VALUE_OK);
    return value;
}

static void assert_kept(const value_t *value)
{
    assert(value->view.type == VALUE_TEXT && value->owns_bytes);
    assert(value->view.size_bytes == 4 && memcmp(value->view.bytes, "keep", 4) == 0);
}

static int order_of(value_view_t left, value_view_t right)
{
    int order = 99;
    assert(value_compare(&left, &right, &order) == VALUE_OK);
    assert(order == -1 || order == 0 || order == 1);
    return order;
}

static void assert_text_form(value_view_t view, const char *expected)
{
    char buffer[128];
    size_t needed = 0;
    assert(value_format(&view, buffer, sizeof(buffer), &needed) == VALUE_OK);
    assert(needed == strlen(expected) && memcmp(buffer, expected, needed + 1) == 0);
}

/* #38: NULL·빈 값·UTF-8·내장 NUL·정수 경계·실수 경계를 정확히 보존한다. */
static void values_roundtrip_exactly(void)
{
    value_t value = {0};
    uint8_t bytes[256];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)i;

    assert(value_set_null(&value) == VALUE_OK && value.view.type == VALUE_NULL);
    assert(value_set_text(&value, NULL, 0) == VALUE_OK);
    assert(value.view.type == VALUE_TEXT && value.view.size_bytes == 0);
    assert(order_of(value_as_view(&value), null_view()) == 1);
    assert(value_set_blob(&value, NULL, 0) == VALUE_OK && value.view.type == VALUE_BLOB);

    const uint8_t korean[] = "한글🙂";
    assert(value_set_text(&value, korean, sizeof(korean) - 1) == VALUE_OK);
    assert(value.view.size_bytes == sizeof(korean) - 1);
    assert(memcmp(value.view.bytes, korean, sizeof(korean) - 1) == 0);

    /* TEXT도 길이를 가진 값이라 U+0000을 보존한다. */
    const uint8_t with_nul[] = {'a', 0, 'b', 0};
    assert(value_set_text(&value, with_nul, sizeof(with_nul)) == VALUE_OK);
    assert(value.view.size_bytes == 4 && memcmp(value.view.bytes, with_nul, 4) == 0);
    value_view_t view = value_as_view(&value);
    assert(value_cast(&view, VALUE_BLOB, &value) == VALUE_OK);
    assert(value.view.type == VALUE_BLOB && value.view.size_bytes == 4);
    assert(memcmp(value.view.bytes, with_nul, 4) == 0);

    assert(value_set_blob(&value, bytes, sizeof(bytes)) == VALUE_OK);
    assert(value.view.size_bytes == 256 && memcmp(value.view.bytes, bytes, 256) == 0);
    assert(value.view.bytes != bytes);

    int64_t integers[] = {INT64_MIN, INT64_MIN + 1, -1, 0, 1, INT64_MAX - 1, INT64_MAX};
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
        value_view_t from = integer_view(integers[i]);
        value_t text = {0}, back = {0};
        assert(value_set_integer(&value, integers[i]) == VALUE_OK);
        assert(value.view.integer == integers[i]);
        assert(value_cast(&from, VALUE_TEXT, &text) == VALUE_OK);
        value_view_t text_as_view = value_as_view(&text);
        assert(value_cast(&text_as_view, VALUE_INTEGER, &back) == VALUE_OK);
        assert(back.view.integer == integers[i]);
        value_clear(&text);
        value_clear(&back);
    }

    double reals[] = {0.0, -0.0, 0.1, -0.1, 1.0, 1.5, 123456789.123456789, DBL_MAX, -DBL_MAX,
                      DBL_MIN, -DBL_MIN, DBL_TRUE_MIN, 1e20, 1e-7, TWO_POW_53, TWO_POW_53 + 2};
    for (size_t i = 0; i < sizeof(reals) / sizeof(reals[0]); i++) {
        value_view_t from = real_view(reals[i]);
        value_t text = {0}, back = {0};
        assert(value_set_real(&value, reals[i]) == VALUE_OK);
        assert(value.view.real == reals[i] && signbit(value.view.real) == signbit(reals[i]));
        assert(value_cast(&from, VALUE_TEXT, &text) == VALUE_OK);
        value_view_t text_as_view = value_as_view(&text);
        assert(value_cast(&text_as_view, VALUE_REAL, &back) == VALUE_OK);
        assert(back.view.real == reals[i] && signbit(back.view.real) == signbit(reals[i]));
        value_clear(&text);
        value_clear(&back);
    }
    value_clear(&value);
    value_clear(&value);
    assert(value.view.type == VALUE_NULL);
}

static void ownership_copies_replaces_and_aliases(void)
{
    uint8_t source[] = "abc";
    value_t value = {0};
    assert(value_set_text(&value, source, 3) == VALUE_OK);
    source[0] = 'z';
    assert(memcmp(value.view.bytes, "abc", 3) == 0 && value.owns_bytes);

    /* 자기 바이트를 가리키는 view로 복제·변환해도 새 값을 먼저 만들어 안전하다. */
    value_view_t self = value_as_view(&value);
    assert(value_clone(&self, &value) == VALUE_OK);
    assert(memcmp(value.view.bytes, "abc", 3) == 0);
    self = value_as_view(&value);
    assert(value_cast(&self, VALUE_BLOB, &value) == VALUE_OK);
    assert(value.view.type == VALUE_BLOB && memcmp(value.view.bytes, "abc", 3) == 0);

    assert(value_set_integer(&value, 5) == VALUE_OK);
    assert(!value.owns_bytes && value.view.bytes == NULL);
    value_clear(&value);
}

static void invalid_text_is_rejected_and_output_kept(void)
{
    static const uint8_t overlong[] = {0xc0, 0x80};
    static const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
    static const uint8_t above_max[] = {0xf4, 0x90, 0x80, 0x80};
    static const uint8_t truncated[] = {0xe2, 0x82};
    static const uint8_t stray[] = {0x80};
    static const uint8_t forbidden[] = {0xff};
    static const uint8_t overlong3[] = {0xe0, 0x80, 0x80};
    static const uint8_t overlong4[] = {0xf0, 0x80, 0x80, 0x80};
    static const uint8_t max_code_point[] = {0xf4, 0x8f, 0xbf, 0xbf};
    static const uint8_t del_and_nul[] = {0x7f, 0x00};
    struct { const uint8_t *bytes; size_t size; } bad[] = {
        {overlong, 2}, {surrogate, 3}, {above_max, 4}, {truncated, 2}, {stray, 1},
        {forbidden, 1}, {overlong3, 3}, {overlong4, 4}};
    value_t value = kept_value();
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(value_set_text(&value, bad[i].bytes, bad[i].size) == VALUE_INVALID);
        assert_kept(&value);
        value_view_t view = text_view(bad[i].bytes, bad[i].size);
        value_view_t other = integer_view(1);
        int order = 77;
        assert(value_compare(&view, &other, &order) == VALUE_INVALID && order == 77);
        assert(value_clone(&view, &value) == VALUE_INVALID);
        assert_kept(&value);
        /* BLOB은 같은 바이트를 허용하지만 TEXT로 바꿀 수는 없다. */
        value_view_t blob = blob_view(bad[i].bytes, bad[i].size);
        assert(value_cast(&blob, VALUE_TEXT, &value) == VALUE_INVALID);
        assert_kept(&value);
    }
    assert(value_set_text(&value, max_code_point, 4) == VALUE_OK);
    assert(value_set_text(&value, del_and_nul, 2) == VALUE_OK && value.view.size_bytes == 2);
    value_clear(&value);
}

static void non_finite_reals_are_rejected(void)
{
    double bad[] = {NAN, INFINITY, -INFINITY};
    value_t value = kept_value();
    for (size_t i = 0; i < 3; i++) {
        assert(value_set_real(&value, bad[i]) == VALUE_OVERFLOW);
        assert_kept(&value);
        value_view_t view = real_view(bad[i]), other = integer_view(0);
        int order = 77;
        char buffer[8];
        size_t needed = 55;
        assert(value_compare(&view, &other, &order) == VALUE_OVERFLOW && order == 77);
        assert(value_compare(&other, &view, &order) == VALUE_OVERFLOW && order == 77);
        assert(value_format(&view, buffer, sizeof(buffer), &needed) == VALUE_OVERFLOW);
        assert(needed == 55);
        assert(value_cast(&view, VALUE_TEXT, &value) == VALUE_OVERFLOW);
        assert(value_clone(&view, &value) == VALUE_OVERFLOW);
        assert_kept(&value);
    }
    value_clear(&value);
}

static void expect_cast(value_view_t from, value_type_t target, value_status_t expected)
{
    value_t value = kept_value();
    assert(value_cast(&from, target, &value) == expected);
    if (expected == VALUE_OK) {
        assert(value.view.type == target || from.type == VALUE_NULL);
    } else {
        assert_kept(&value);
    }
    value_clear(&value);
}

static void expect_integer_from_text(const char *text, size_t size, value_status_t expected,
                                     int64_t integer)
{
    value_t value = {0};
    value_view_t view = text_view(text, size);
    assert(value_cast(&view, VALUE_INTEGER, &value) == expected);
    if (expected == VALUE_OK) assert(value.view.integer == integer);
    value_clear(&value);
}

static void expect_real_from_text(const char *text, size_t size, value_status_t expected,
                                  double real)
{
    value_t value = {0};
    value_view_t view = text_view(text, size);
    assert(value_cast(&view, VALUE_REAL, &value) == expected);
    if (expected == VALUE_OK) {
        assert(value.view.real == real && signbit(value.view.real) == signbit(real));
    }
    value_clear(&value);
}

/* 정확히 표현되는 값은 2^53 초과도 허용하고 실제 손실만 거절한다. UB 없이 int64 경계를 검사한다. */
static void numeric_casts_are_exact_or_rejected(void)
{
    value_t value = {0};
    value_view_t from = integer_view(9007199254740993LL);
    assert(value_cast(&from, VALUE_REAL, &value) == VALUE_OVERFLOW && value.view.type == VALUE_NULL);
    int64_t exact[] = {0, -1, 9007199254740992LL, 9007199254740994LL, INT64_MIN,
                       INT64_MAX - 1023, -9007199254740992LL};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        from = integer_view(exact[i]);
        assert(value_cast(&from, VALUE_REAL, &value) == VALUE_OK);
        assert(value.view.real == (double)exact[i]);
    }
    int64_t lossy[] = {INT64_MAX, INT64_MAX - 1, INT64_MAX - 1024, 9007199254740993LL,
                       -9007199254740993LL, INT64_MIN + 1};
    for (size_t i = 0; i < sizeof(lossy) / sizeof(lossy[0]); i++) {
        from = integer_view(lossy[i]);
        assert(value_cast(&from, VALUE_REAL, &value) == VALUE_OVERFLOW);
    }

    struct { double real; value_status_t status; int64_t integer; } reals[] = {
        {0.0, VALUE_OK, 0}, {-0.0, VALUE_OK, 0}, {42.0, VALUE_OK, 42}, {-42.0, VALUE_OK, -42},
        {TWO_POW_63 - 1024.0, VALUE_OK, INT64_MAX - 1023}, {-TWO_POW_63, VALUE_OK, INT64_MIN},
        {TWO_POW_63, VALUE_OVERFLOW, 0}, {-TWO_POW_63 - 2048.0, VALUE_OVERFLOW, 0},
        {2.5, VALUE_OVERFLOW, 0}, {-0.5, VALUE_OVERFLOW, 0}, {1e300, VALUE_OVERFLOW, 0},
        {-1e300, VALUE_OVERFLOW, 0}, {DBL_TRUE_MIN, VALUE_OVERFLOW, 0}};
    for (size_t i = 0; i < sizeof(reals) / sizeof(reals[0]); i++) {
        from = real_view(reals[i].real);
        assert(value_cast(&from, VALUE_INTEGER, &value) == reals[i].status);
        if (reals[i].status == VALUE_OK) assert(value.view.integer == reals[i].integer);
    }
    value_clear(&value);
}

static void text_conversions_are_strict(void)
{
    expect_integer_from_text("0", 1, VALUE_OK, 0);
    expect_integer_from_text("-0", 2, VALUE_OK, 0);
    expect_integer_from_text("+7", 2, VALUE_OK, 7);
    expect_integer_from_text("0042", 4, VALUE_OK, 42);
    expect_integer_from_text("9223372036854775807", 19, VALUE_OK, INT64_MAX);
    expect_integer_from_text("-9223372036854775808", 20, VALUE_OK, INT64_MIN);
    expect_integer_from_text("9223372036854775808", 19, VALUE_OVERFLOW, 0);
    expect_integer_from_text("-9223372036854775809", 20, VALUE_OVERFLOW, 0);
    expect_integer_from_text("99999999999999999999999", 23, VALUE_OVERFLOW, 0);
    expect_integer_from_text("999999999999999999999x", 22, VALUE_INVALID, 0);
    const char *bad_integers[] = {"", "+", "-", " 1", "1 ", "1x", "0x10", "1.0", "--1", "1e2"};
    for (size_t i = 0; i < sizeof(bad_integers) / sizeof(bad_integers[0]); i++) {
        expect_integer_from_text(bad_integers[i], strlen(bad_integers[i]), VALUE_INVALID, 0);
    }
    expect_integer_from_text("1\0", 2, VALUE_INVALID, 0);
    expect_integer_from_text("\0001", 2, VALUE_INVALID, 0);

    expect_real_from_text("1.5", 3, VALUE_OK, 1.5);
    expect_real_from_text("-2.5e3", 6, VALUE_OK, -2500.0);
    expect_real_from_text("1E2", 3, VALUE_OK, 100.0);
    expect_real_from_text("7", 1, VALUE_OK, 7.0);
    expect_real_from_text("0.1", 3, VALUE_OK, 0.1);
    expect_real_from_text("-0.0", 4, VALUE_OK, -0.0);
    expect_real_from_text("0e-400", 6, VALUE_OK, 0.0);
    expect_real_from_text("1.7976931348623157e308", 22, VALUE_OK, DBL_MAX);
    expect_real_from_text("4.9406564584124654e-324", 23, VALUE_OK, DBL_TRUE_MIN);
    expect_real_from_text("1e400", 5, VALUE_OVERFLOW, 0);
    expect_real_from_text("-1e400", 6, VALUE_OVERFLOW, 0);
    expect_real_from_text("1e-400", 6, VALUE_OVERFLOW, 0);
    expect_real_from_text("1.8e308", 7, VALUE_OVERFLOW, 0);
    const char *bad_reals[] = {"", ".5", "5.", "1e", "1e+", "nan", "inf", "-inf", "infinity",
                               "0x1p3", " 1", "1 ", "1,5", "1.5.5", "e5", "+", "1_0"};
    for (size_t i = 0; i < sizeof(bad_reals) / sizeof(bad_reals[0]); i++) {
        expect_real_from_text(bad_reals[i], strlen(bad_reals[i]), VALUE_INVALID, 0);
    }
    expect_real_from_text("1.5\0", 4, VALUE_INVALID, 0);

    uint8_t blob[] = {0xff, 0x00};
    expect_cast(blob_view(blob, 2), VALUE_INTEGER, VALUE_MISMATCH);
    expect_cast(blob_view(blob, 2), VALUE_REAL, VALUE_MISMATCH);
    expect_cast(integer_view(1), VALUE_BLOB, VALUE_MISMATCH);
    expect_cast(real_view(1.0), VALUE_BLOB, VALUE_MISMATCH);
    expect_cast(blob_view(blob, 2), VALUE_TEXT, VALUE_INVALID);
    expect_cast(integer_view(1), (value_type_t)99, VALUE_INVALID);
    expect_cast((value_view_t){(value_type_t)99, 0, 0.0, NULL, 0}, VALUE_TEXT, VALUE_INVALID);
    for (value_type_t target = VALUE_NULL; target <= VALUE_BLOB; target++) {
        expect_cast(null_view(), target, VALUE_OK);
    }
    /* 같은 타입은 복제하고 NUL을 포함한 BLOB은 유효한 UTF-8일 때 TEXT가 된다. */
    uint8_t nul_blob[] = {'o', 'k', 0, 'x'};
    value_t value = {0};
    value_view_t view = blob_view(nul_blob, 4);
    assert(value_cast(&view, VALUE_TEXT, &value) == VALUE_OK);
    assert(value.view.type == VALUE_TEXT && value.view.size_bytes == 4);
    assert(memcmp(value.view.bytes, nul_blob, 4) == 0);
    value_clear(&value);
}

static void format_follows_lengths_and_buffer_contract(void)
{
    assert_text_form(null_view(), "NULL");
    assert_text_form(integer_view(0), "0");
    assert_text_form(integer_view(INT64_MIN), "-9223372036854775808");
    assert_text_form(integer_view(INT64_MAX), "9223372036854775807");
    assert_text_form(real_view(1.0), "1.0");
    assert_text_form(real_view(-0.0), "-0.0");
    assert_text_form(real_view(0.0), "0.0");
    assert_text_form(real_view(0.1), "0.10000000000000001");
    assert_text_form(real_view(1e20), "1e+20");
    assert_text_form(real_view(TWO_POW_63), "9.2233720368547758e+18");
    assert_text_form(text_view("한글", 6), "한글");
    assert_text_form(text_view("", 0), "");
    uint8_t blob[] = {0x00, 0xff, 0xab};
    assert_text_form(blob_view(blob, 3), "X'00ffab'");
    assert_text_form(blob_view(NULL, 0), "X''");

    /* 내장 NUL은 문자열 끝이 아니라 길이 안의 바이트다. */
    char buffer[16];
    size_t needed = 0;
    value_view_t with_nul = text_view("a\0b", 3);
    memset(buffer, 'z', sizeof(buffer));
    assert(value_format(&with_nul, buffer, sizeof(buffer), &needed) == VALUE_OK);
    assert(needed == 3 && memcmp(buffer, "a\0b\0", 4) == 0);

    /* 정확히 맞는 buffer는 종료 NUL 없이, 모자란 buffer는 그대로 두고 필요 길이만 알린다. */
    memset(buffer, 'z', sizeof(buffer));
    assert(value_format(&with_nul, buffer, 3, &needed) == VALUE_OK);
    assert(memcmp(buffer, "a\0bzzz", 6) == 0);
    memset(buffer, 'z', sizeof(buffer));
    needed = 0;
    assert(value_format(&with_nul, buffer, 2, &needed) == VALUE_LIMIT && needed == 3);
    assert(memcmp(buffer, "zzzz", 4) == 0);
    value_view_t empty = text_view(NULL, 0);
    assert(value_format(&empty, NULL, 0, &needed) == VALUE_OK && needed == 0);
    assert(value_format(&with_nul, NULL, 0, &needed) == VALUE_LIMIT && needed == 3);
    assert(value_format(&with_nul, NULL, 3, &needed) == VALUE_INVALID);
    assert(value_format(&with_nul, buffer, 8, NULL) == VALUE_INVALID);
    needed = 55;
    value_view_t bad = text_view(NULL, 4);
    assert(value_format(&bad, buffer, sizeof(buffer), &needed) == VALUE_INVALID && needed == 55);
}

static void compare_orders_types_and_exact_numbers(void)
{
    uint8_t high[] = {0xc3, 0xa9}, low[] = {'z'};
    assert(order_of(null_view(), null_view()) == 0);
    assert(order_of(null_view(), integer_view(INT64_MIN)) == -1);
    assert(order_of(real_view(-DBL_MAX), text_view("", 0)) == -1);
    assert(order_of(text_view("zzz", 3), blob_view(NULL, 0)) == -1);
    assert(order_of(blob_view(NULL, 0), null_view()) == 1);

    assert(order_of(integer_view(1), integer_view(2)) == -1);
    assert(order_of(real_view(2.5), real_view(2.5)) == 0);
    assert(order_of(real_view(-0.0), real_view(0.0)) == 0);
    assert(order_of(real_view(-0.0), integer_view(0)) == 0);
    assert(order_of(integer_view(9007199254740993LL), real_view(TWO_POW_53)) == 1);
    assert(order_of(real_view(TWO_POW_53), integer_view(9007199254740993LL)) == -1);
    assert(order_of(integer_view(9007199254740992LL), real_view(TWO_POW_53)) == 0);
    assert(order_of(integer_view(INT64_MAX), real_view(TWO_POW_63)) == -1);
    assert(order_of(integer_view(INT64_MIN), real_view(-TWO_POW_63)) == 0);
    assert(order_of(integer_view(INT64_MIN), real_view(-TWO_POW_63 - 2048.0)) == 1);
    assert(order_of(integer_view(INT64_MAX), real_view(DBL_MAX)) == -1);
    assert(order_of(integer_view(INT64_MIN), real_view(-DBL_MAX)) == 1);
    assert(order_of(integer_view(0), real_view(0.5)) == -1);
    assert(order_of(integer_view(0), real_view(-0.5)) == 1);
    assert(order_of(integer_view(-1), real_view(-0.5)) == -1);
    assert(order_of(integer_view(1), real_view(0.5)) == 1);
    assert(order_of(integer_view(-1), real_view(-1.5)) == 1);
    assert(order_of(integer_view(0), real_view(DBL_TRUE_MIN)) == -1);

    assert(order_of(text_view("a\0b", 3), text_view("a", 1)) == 1);
    assert(order_of(text_view("a\0b", 3), text_view("a\0c", 3)) == -1);
    assert(order_of(text_view("a", 1), text_view("ab", 2)) == -1);
    assert(order_of(text_view("", 0), text_view("a", 1)) == -1);
    assert(order_of(text_view("", 0), text_view(NULL, 0)) == 0);
    assert(order_of(text_view(high, 2), text_view(low, 1)) == 1);
    assert(order_of(blob_view("\0", 1), blob_view("", 0)) == 1);
    assert(order_of(blob_view("\xff", 1), blob_view("\0\0", 2)) == 1);
    assert(order_of(blob_view("ab", 2), blob_view("ab", 2)) == 0);

    value_view_t set[] = {null_view(), integer_view(INT64_MIN), integer_view(-1), real_view(-0.5),
                          real_view(-0.0), integer_view(0), real_view(0.5), integer_view(1),
                          integer_view(9007199254740993LL), real_view(TWO_POW_53 + 2),
                          integer_view(INT64_MAX), real_view(TWO_POW_63), real_view(DBL_MAX),
                          text_view("", 0), text_view("a", 1), text_view("a\0", 2),
                          text_view(high, 2), blob_view("", 0), blob_view("\0", 1)};
    size_t count = sizeof(set) / sizeof(set[0]);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            int forward = order_of(set[i], set[j]), backward = order_of(set[j], set[i]);
            assert(forward == -backward);
            for (size_t k = 0; k < count; k++) {
                if (forward <= 0 && order_of(set[j], set[k]) <= 0) {
                    assert(order_of(set[i], set[k]) <= 0);
                }
            }
        }
    }
    /* 위 목록은 오름차순으로 적었으므로 인접 쌍은 같거나 작다. */
    for (size_t i = 0; i + 1 < count; i++) assert(order_of(set[i], set[i + 1]) <= 0);
}

static void compare_rejects_bad_views_and_keeps_order(void)
{
    uint8_t byte = 1;
    value_view_t good = integer_view(1);
    value_view_t bad[] = {
        {(value_type_t)99, 0, 0.0, NULL, 0},
        {VALUE_NULL, 0, 0.0, &byte, 1},
        {VALUE_INTEGER, 1, 0.0, &byte, 0},
        {VALUE_REAL, 0, 1.0, NULL, 3},
        {VALUE_TEXT, 0, 0.0, NULL, 3},
        {VALUE_BLOB, 0, 0.0, NULL, 1}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int order = 77;
        assert(value_compare(&bad[i], &good, &order) == VALUE_INVALID && order == 77);
        assert(value_compare(&good, &bad[i], &order) == VALUE_INVALID && order == 77);
        assert(value_compare(&bad[i], &bad[i], &order) == VALUE_INVALID && order == 77);
    }
    int order = 77;
    assert(value_compare(NULL, &good, &order) == VALUE_INVALID && order == 77);
    assert(value_compare(&good, NULL, &order) == VALUE_INVALID && order == 77);
    assert(value_compare(&good, &good, NULL) == VALUE_INVALID);
    if (sizeof(size_t) > 4) {
        value_view_t huge = {VALUE_BLOB, 0, 0.0, &byte, (size_t)VALUE_MAX_BYTES + 1};
        assert(value_compare(&huge, &good, &order) == VALUE_LIMIT && order == 77);
        value_t value = kept_value();
        assert(value_set_blob(&value, &byte, (size_t)VALUE_MAX_BYTES + 1) == VALUE_LIMIT);
        assert_kept(&value);
        value_clear(&value);
    }
}

static void real_text_is_locale_independent(void)
{
    const char *locale = setlocale(LC_NUMERIC, "de_DE.UTF-8");
    if (locale == NULL) {
        puts("value: de_DE locale unavailable, locale independence not verified");
        return;
    }
    char probe[16];
    snprintf(probe, sizeof(probe), "%g", 1.5);
    assert(strcmp(probe, "1,5") == 0);  /* 로캘이 실제로 소수점을 바꿨는지 확인 */
    assert_text_form(real_view(1.5), "1.5");
    assert_text_form(real_view(-0.0), "-0.0");
    assert_text_form(real_view(1e20), "1e+20");
    expect_real_from_text("1.5", 3, VALUE_OK, 1.5);
    expect_real_from_text("-2.5e3", 6, VALUE_OK, -2500.0);
    expect_real_from_text("1,5", 3, VALUE_INVALID, 0);
    value_t value = {0};
    value_view_t from = real_view(0.1);
    assert(value_cast(&from, VALUE_TEXT, &value) == VALUE_OK);
    assert(value.view.size_bytes == 19 && memcmp(value.view.bytes, "0.10000000000000001", 19) == 0);
    value_clear(&value);
    assert(setlocale(LC_NUMERIC, "C") != NULL);
}

int main(void)
{
    values_roundtrip_exactly();
    ownership_copies_replaces_and_aliases();
    invalid_text_is_rejected_and_output_kept();
    non_finite_reals_are_rejected();
    numeric_casts_are_exact_or_rejected();
    text_conversions_are_strict();
    format_follows_lengths_and_buffer_contract();
    compare_orders_types_and_exact_numbers();
    compare_rejects_bad_views_and_keeps_order();
    real_text_is_locale_independent();
    puts("value: roundtrip, strict casts, exact compare, format and failure preservation");
    return 0;
}
