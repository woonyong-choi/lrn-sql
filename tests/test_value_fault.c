#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/value.h"

static int malloc_calls;
static int fail_at;

void *value_test_malloc(size_t size)
{
    return ++malloc_calls == fail_at ? NULL : malloc(size);
}

static value_status_t op_set_text(value_t *out)
{
    return value_set_text(out, (const uint8_t *)"a\0b", 3);
}

static value_status_t op_set_blob(value_t *out)
{
    return value_set_blob(out, (const uint8_t *)"\0\xff", 2);
}

static value_status_t op_clone_text(value_t *out)
{
    value_view_t view = {VALUE_TEXT, 0, 0.0, (const uint8_t *)"clone", 5};
    return value_clone(&view, out);
}

static value_status_t op_text_to_blob(value_t *out)
{
    value_view_t view = {VALUE_TEXT, 0, 0.0, (const uint8_t *)"abc", 3};
    return value_cast(&view, VALUE_BLOB, out);
}

static value_status_t op_blob_to_text(value_t *out)
{
    value_view_t view = {VALUE_BLOB, 0, 0.0, (const uint8_t *)"abc", 3};
    return value_cast(&view, VALUE_TEXT, out);
}

static value_status_t op_integer_to_text(value_t *out)
{
    value_view_t view = {VALUE_INTEGER, -42, 0.0, NULL, 0};
    return value_cast(&view, VALUE_TEXT, out);
}

static value_status_t op_real_to_text(value_t *out)
{
    value_view_t view = {VALUE_REAL, 0, 0.5, NULL, 0};
    return value_cast(&view, VALUE_TEXT, out);
}

static value_status_t op_text_to_real(value_t *out)
{
    value_view_t view = {VALUE_TEXT, 0, 0.0, (const uint8_t *)"2.5e1", 5};
    return value_cast(&view, VALUE_REAL, out);
}

static void assert_kept(const value_t *value)
{
    assert(value->view.type == VALUE_TEXT && value->owns_bytes);
    assert(value->view.size_bytes == 4 && memcmp(value->view.bytes, "keep", 4) == 0);
}

/* 할당 실패는 몇 번째 malloc이든 NOMEM이고 기존 소유 값을 바꾸지 않으며 다음 호출은 성공한다. */
static void run_operation(value_status_t (*operation)(value_t *))
{
    value_t value = {0};
    assert(value_set_text(&value, (const uint8_t *)"keep", 4) == VALUE_OK);
    int failures = 0;
    for (int step = 1; step <= 4; step++) {
        fail_at = step;
        malloc_calls = 0;
        value_status_t status = operation(&value);
        if (status == VALUE_OK) {
            /* 이 호출의 malloc 횟수가 fail_at보다 적었다. 새 값으로 바뀌었으므로 되돌린다. */
            assert(malloc_calls < step);
            fail_at = 0;
            assert(value_set_text(&value, (const uint8_t *)"keep", 4) == VALUE_OK);
            continue;
        }
        assert(status == VALUE_NOMEM);
        assert_kept(&value);
        failures++;
    }
    assert(failures >= 1);
    fail_at = 0;
    assert(operation(&value) == VALUE_OK);
    value_clear(&value);
}

int main(void)
{
    value_status_t (*operations[])(value_t *) = {
        op_set_text, op_set_blob, op_clone_text, op_text_to_blob,
        op_blob_to_text, op_integer_to_text, op_real_to_text, op_text_to_real};
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        run_operation(operations[i]);
    }
    puts("value: allocation failures keep owned output; next operation succeeds");
    return 0;
}
