/*
 * test_value_varchar.c — VARCHAR(n) 경계 계약 검사 (#38)
 *
 * n은 문자(code point) 수의 상한이다. n-1·n 문자는 통과하고 n+1 문자는 절단 없이 거절한다. 바이트 수와
 * 문자 수가 다른 한글(3바이트)·이모지(4바이트)·내장 NUL·빈 문자열·잘못된 UTF-8을 함께 본다. 검사 함수는
 * 값을 바꾸지 않고 할당하지 않는다.
 *
 * 범위: value 계층의 독립 계약이다. 실제 DB 저장 경로(parser·executor·schema·database)는 이 파일이 검사하지
 * 않는다. 그 경로의 현재 동작과 수정 위치는 PR 본문에 따로 기록했다.
 *
 * 빌드(value.c만 malloc을 훅으로 바꿔 컴파일한다. 훅이 적용되지 않으면 무할당 검사가 처음에 실패한다):
 *   cc -Wall -Wextra -Werror -g -std=c11 -Iinclude -Dmalloc=value_test_malloc -c src/storage/value.c -o build/value_hooked.o
 *   cc -Wall -Wextra -Werror -g -std=c11 -Iinclude tests/test_value_varchar.c build/value_hooked.o -o build/test_value_varchar
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/value.h"

static int malloc_calls;

void *value_test_malloc(size_t size)
{
    malloc_calls++;
    return malloc(size);
}

static value_view_t text_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_TEXT, 0, 0.0, bytes, size};
}

/* 한 글자의 바이트 수가 다른 문자 집합. 같은 글자를 반복해 정확한 문자 수의 문자열을 만든다. */
typedef struct {
    const char *name;
    const char *glyph;
    size_t glyph_bytes;
} charset_t;

static const charset_t CHARSETS[] = {
    {"ascii", "a", 1},
    {"hangul", "\xea\xb0\x80", 3},        /* U+AC00 */
    {"emoji", "\xf0\x9f\x98\x80", 4},     /* U+1F600 */
    {"two-byte", "\xc3\xa9", 2},          /* U+00E9 */
};

static uint8_t *repeat_glyph(const charset_t *set, size_t chars, size_t *bytes)
{
    *bytes = chars * set->glyph_bytes;
    uint8_t *buffer = calloc(*bytes + 1, 1);
    assert(buffer != NULL);
    for (size_t i = 0; i < chars; i++) {
        memcpy(buffer + i * set->glyph_bytes, set->glyph, set->glyph_bytes);
    }
    return buffer;
}

/* n-1, n, n+1 문자: 통과, 통과, 거절. 문자 수는 정확히 세고 바이트 수와 다르다. */
static void test_boundary(void)
{
    static const size_t WIDTHS[] = {1, 2, 3, 8, 255};
    for (size_t c = 0; c < sizeof(CHARSETS) / sizeof(CHARSETS[0]); c++) {
        for (size_t w = 0; w < sizeof(WIDTHS) / sizeof(WIDTHS[0]); w++) {
            size_t n = WIDTHS[w];
            for (size_t delta = 0; delta < 3; delta++) {
                size_t chars = n - 1 + delta;   /* n-1, n, n+1 */
                size_t bytes;
                uint8_t *data = repeat_glyph(&CHARSETS[c], chars, &bytes);
                value_view_t view = text_view(data, bytes);
                size_t counted = (size_t)-1;
                assert(value_text_chars(&view, &counted) == VALUE_OK && counted == chars);
                value_status_t want = delta == 2 ? VALUE_LIMIT : VALUE_OK;
                value_status_t got = value_check_varchar(&view, n);
                if (got != want) {
                    fprintf(stderr, "%s n=%zu chars=%zu: got %d want %d\n", CHARSETS[c].name, n, chars, got, want);
                }
                assert(got == want);
                free(data);
            }
        }
    }
}

/* 문자 수는 통과하지만 바이트 수는 n을 넘는 값을 바이트 기준으로 잘못 거절하거나 자르지 않는다. */
static void test_chars_not_bytes(void)
{
    size_t bytes;
    uint8_t *hangul = repeat_glyph(&CHARSETS[1], 8, &bytes);   /* 8문자 24바이트 */
    value_view_t view = text_view(hangul, bytes);
    assert(bytes == 24);
    assert(value_check_varchar(&view, 8) == VALUE_OK);
    assert(value_check_varchar(&view, 7) == VALUE_LIMIT);
    assert(view.size_bytes == 24);
    free(hangul);

    uint8_t *emoji = repeat_glyph(&CHARSETS[2], 3, &bytes);    /* 3문자 12바이트 */
    view = text_view(emoji, bytes);
    assert(bytes == 12 && value_check_varchar(&view, 3) == VALUE_OK && value_check_varchar(&view, 2) == VALUE_LIMIT);
    free(emoji);

    /* 서로 다른 크기의 문자가 섞여도 문자 수로 센다: a(1) 가(3) 😀(4) é(2) = 4문자 10바이트 */
    const char mixed[] = "a\xea\xb0\x80\xf0\x9f\x98\x80\xc3\xa9";
    view = text_view(mixed, sizeof(mixed) - 1);
    size_t counted = 0;
    assert(view.size_bytes == 10 && value_text_chars(&view, &counted) == VALUE_OK && counted == 4);
    assert(value_check_varchar(&view, 4) == VALUE_OK && value_check_varchar(&view, 3) == VALUE_LIMIT);
}

static void test_empty_null_nul_and_types(void)
{
    size_t counted = 99;
    value_view_t empty = text_view(NULL, 0);
    assert(value_check_varchar(&empty, 1) == VALUE_OK);
    assert(value_text_chars(&empty, &counted) == VALUE_OK && counted == 0);

    value_view_t null_value = {VALUE_NULL, 0, 0.0, NULL, 0};
    assert(value_check_varchar(&null_value, 1) == VALUE_OK);   /* NULL은 제한을 받지 않는다 */
    counted = 99;
    assert(value_text_chars(&null_value, &counted) == VALUE_MISMATCH && counted == 99);

    value_view_t integer = {VALUE_INTEGER, 7, 0.0, NULL, 0};
    value_view_t blob = {VALUE_BLOB, 0, 0.0, (const uint8_t *)"ab", 2};
    assert(value_check_varchar(&integer, 8) == VALUE_MISMATCH);
    assert(value_check_varchar(&blob, 8) == VALUE_MISMATCH);   /* BLOB은 바이트열이라 문자 제한 대상이 아니다 */

    /* 내장 NUL도 한 문자다 */
    value_view_t nul = text_view("a\0b", 3);
    assert(value_text_chars(&nul, &counted) == VALUE_OK && counted == 3);
    assert(value_check_varchar(&nul, 3) == VALUE_OK && value_check_varchar(&nul, 2) == VALUE_LIMIT);

    /* 선언 자체가 잘못되면 길이 초과와 구분한다 */
    assert(value_check_varchar(&empty, 0) == VALUE_INVALID);
    assert(value_check_varchar(NULL, 1) == VALUE_INVALID);
    assert(value_text_chars(&empty, NULL) == VALUE_INVALID);
    assert(value_text_chars(NULL, &counted) == VALUE_INVALID);
}

/* 경계에서 잘린 다바이트 문자 등 잘못된 UTF-8은 길이가 아니라 입력 오류다. 문자 수는 바꾸지 않는다. */
static void test_invalid_utf8(void)
{
    static const struct {
        const char *name;
        const char *bytes;
        size_t size;
    } CASES[] = {
        {"truncated emoji", "\xf0\x9f\x98", 3},
        {"truncated hangul", "ab\xea\xb0", 4},
        {"lone continuation", "\x80", 1},
        {"overlong nul", "\xc0\x80", 2},
        {"surrogate", "\xed\xa0\x80", 3},
        {"above U+10FFFF", "\xf4\x90\x80\x80", 4},
        {"invalid lead", "\xff", 1},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        value_view_t view = text_view(CASES[i].bytes, CASES[i].size);
        size_t counted = 7;
        if (value_check_varchar(&view, 100) != VALUE_INVALID || value_check_varchar(&view, 1) != VALUE_INVALID ||
            value_text_chars(&view, &counted) != VALUE_INVALID || counted != 7) {
            fprintf(stderr, "invalid utf8 case failed: %s\n", CASES[i].name);
            assert(0);
        }
    }
}

/* 검사는 입력 바이트와 크기를 바꾸지 않는다. 거절한 값도 그대로 남는다(조용한 절단 없음). */
static void test_no_mutation_no_allocation(void)
{
    assert(malloc_calls == 0);
    value_t owned = {0};
    assert(value_set_text(&owned, (const uint8_t *)"\xea\xb0\x80\xea\xb0\x80\xea\xb0\x80", 9) == VALUE_OK);
    assert(malloc_calls == 1);   /* -Dmalloc=value_test_malloc가 적용됐는지 확인한다 */
    malloc_calls = 0;

    uint8_t before[9];
    memcpy(before, owned.view.bytes, 9);
    value_view_t view = value_as_view(&owned);
    for (size_t n = 1; n <= 4; n++) {
        value_status_t status = value_check_varchar(&view, n);
        assert(status == (n >= 3 ? VALUE_OK : VALUE_LIMIT));
        size_t counted;
        assert(value_text_chars(&view, &counted) == VALUE_OK && counted == 3);
    }
    assert(malloc_calls == 0);   /* 검사는 할당하지 않는다 */
    assert(owned.view.size_bytes == 9 && memcmp(owned.view.bytes, before, 9) == 0);
    value_clear(&owned);
}

/*
 * 저장 왕복: 경계 값의 바이트를 길이와 함께 파일에 쓰고 처음부터 다시 읽어 같은 판정이 나오는지 본다.
 * 같은 프로세스의 파일 왕복이며 실제 DB 재시작(pager 재열기)이 아니다.
 */
static void test_file_round_trip(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    size_t counts[] = {7, 8, 9};   /* VARCHAR(8)의 n-1, n, n+1 */
    for (size_t c = 0; c < sizeof(CHARSETS) / sizeof(CHARSETS[0]); c++) {
        for (size_t i = 0; i < 3; i++) {
            size_t bytes;
            uint8_t *data = repeat_glyph(&CHARSETS[c], counts[i], &bytes);
            uint32_t length = (uint32_t)bytes;
            assert(fwrite(&length, sizeof(length), 1, file) == 1);
            assert(fwrite(data, 1, bytes, file) == bytes);
            free(data);
        }
    }
    assert(fflush(file) == 0);
    rewind(file);
    for (size_t c = 0; c < sizeof(CHARSETS) / sizeof(CHARSETS[0]); c++) {
        for (size_t i = 0; i < 3; i++) {
            uint32_t length;
            assert(fread(&length, sizeof(length), 1, file) == 1);
            uint8_t *data = calloc((size_t)length + 1, 1);
            assert(data != NULL && fread(data, 1, length, file) == length);
            value_t restored = {0};
            assert(value_set_text(&restored, data, length) == VALUE_OK);
            assert(restored.view.size_bytes == counts[i] * CHARSETS[c].glyph_bytes);
            value_view_t view = value_as_view(&restored);
            size_t counted;
            assert(value_text_chars(&view, &counted) == VALUE_OK && counted == counts[i]);
            assert(value_check_varchar(&view, 8) == (i == 2 ? VALUE_LIMIT : VALUE_OK));
            value_clear(&restored);
            free(data);
        }
    }
    assert(fclose(file) == 0);
}

int main(void)
{
    test_boundary();
    test_chars_not_bytes();
    test_empty_null_nul_and_types();
    test_invalid_utf8();
    test_no_mutation_no_allocation();
    test_file_round_trip();
    puts("value/varchar: n-1/n/n+1 by characters, UTF-8 chars vs bytes, no truncation, no allocation");
    return 0;
}
