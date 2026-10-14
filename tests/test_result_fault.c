#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/result.h"

#define SLOT_GROWTH_ROWS 16u

static int alloc_calls;
static int fail_at;
static int live_blocks;
static int close_calls;

/* result.c를 -Dmalloc=result_test_malloc 등으로 빌드해 할당을 세고 N번째를 실패시킨다. */
void *result_test_malloc(size_t size)
{
    if (++alloc_calls == fail_at) return NULL;
    void *block = malloc(size);
    if (block != NULL) live_blocks++;
    return block;
}

void *result_test_calloc(size_t count, size_t size)
{
    if (++alloc_calls == fail_at) return NULL;
    void *block = calloc(count, size);
    if (block != NULL) live_blocks++;
    return block;
}

void *result_test_realloc(void *block, size_t size)
{
    if (++alloc_calls == fail_at) return NULL;
    void *grown = realloc(block, size);
    if (grown != NULL && block == NULL) live_blocks++;
    return grown;
}

void result_test_free(void *block)
{
    if (block != NULL) live_blocks--;
    free(block);
}

static const result_column_t columns[2] = {
    {(const uint8_t *)"id", 2, VALUE_INTEGER, false},
    {(const uint8_t *)"note", 4, VALUE_TEXT, true},
};

static result_next_t empty_next(void *ctx, result_step_t *step)
{
    (void)ctx;
    (void)step;
    return RESULT_NEXT_DONE;
}

static void count_close(void *ctx)
{
    (void)ctx;
    close_calls++;
}

static const result_source_ops_t ops = {empty_next, count_close};

static result_status_t open_rows(result_t **out)
{
    result_rows_spec_t spec = {columns, 2, &ops, NULL, NULL};
    return result_open_rows(&spec, out);
}

static result_status_t open_command(result_t **out)
{
    result_command_t command = {3, "UPDATE", 6};
    return result_open_command(&command, out);
}

static result_status_t open_error(result_t **out)
{
    result_error_t error = {RESULT_E_IO, 1, "read failed", 11, false};
    return result_open_error(&error, out);
}

static result_status_t open_builder_result(result_t **out)
{
    result_builder_spec_t spec = {columns, 2, NULL};
    result_builder_t *builder = NULL;
    *out = NULL;
    result_status_t status = result_builder_create(&spec, &builder);
    if (status != RESULT_OK) return status;
    return result_builder_finish(&builder, out);
}

/* 근거: #64 계약(생성 실패). 몇 번째 할당이 실패해도 NOMEM·out NULL·누수 없음·close 미호출이다. */
static void run_open(result_status_t (*open)(result_t **), int expected_close_calls)
{
    int failures = 0;
    for (int step = 1; step <= 6; step++) {
        result_t *result = (result_t *)&failures;
        alloc_calls = 0;
        fail_at = step;
        close_calls = 0;
        result_status_t status = open(&result);
        fail_at = 0;
        if (status == RESULT_OK) {
            assert(alloc_calls < step);
            result_release(&result);
            assert(live_blocks == 0 && close_calls == expected_close_calls);
            continue;
        }
        assert(status == RESULT_NOMEM && result == NULL);
        assert(live_blocks == 0 && close_calls == 0);
        failures++;
    }
    assert(failures >= 1);
}

static value_view_t numbered_cells[2];

static result_status_t add_numbered_row(result_builder_t *builder, int64_t id)
{
    numbered_cells[0] = (value_view_t){VALUE_INTEGER, id, 0.0, NULL, 0};
    numbered_cells[1] = (value_view_t){VALUE_TEXT, 0, 0.0, (const uint8_t *)"row", 3};
    return result_builder_add_row(builder, numbered_cells, 2);
}

static result_builder_t *builder_with_rows(int count)
{
    result_builder_spec_t spec = {columns, 2, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    for (int i = 0; i < count; i++) assert(add_numbered_row(builder, i) == RESULT_OK);
    return builder;
}

static void assert_rows_numbered(result_builder_t *builder, int count)
{
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK);
    result_row_t row;
    for (int i = 0; i < count; i++) {
        assert(result_next(result, &row) == RESULT_NEXT_ROW);
        assert(row.cells[0].integer == i && row.cells[1].size_bytes == 3);
        assert(memcmp(row.cells[1].bytes, "row", 3) == 0);
    }
    assert(result_next(result, &row) == RESULT_NEXT_DONE);
    result_release(&result);
}

/* 근거: #64 계약(builder 한도). add_row 할당 실패는 기존 행을 보존하고 누수 없이 재시도가 성공한다. */
static void run_add_row_failures(int existing_rows)
{
    result_builder_t *builder = builder_with_rows(existing_rows);
    int added = 0;
    int failures = 0;
    for (int step = 1; step <= 3; step++) {
        int before = live_blocks;
        alloc_calls = 0;
        fail_at = step;
        result_status_t status = add_numbered_row(builder, existing_rows + added);
        fail_at = 0;
        if (status == RESULT_OK) {
            assert(alloc_calls < step);
            added++;
            continue;
        }
        assert(status == RESULT_NOMEM && live_blocks == before);
        failures++;
    }
    assert(failures >= 1);
    assert(add_numbered_row(builder, existing_rows + added) == RESULT_OK);
    assert_rows_numbered(builder, existing_rows + added + 1);
    assert(live_blocks == 0);
}

/* 근거: #64 계약(builder 수명). 행을 담은 채 destroy해도 모든 블록이 해제된다. */
static void destroy_with_rows_frees_everything(void)
{
    result_builder_t *builder = builder_with_rows(SLOT_GROWTH_ROWS + 1);
    result_builder_destroy(&builder);
    assert(builder == NULL && live_blocks == 0);
}

int main(void)
{
    run_open(open_rows, 1);
    run_open(open_command, 0);
    run_open(open_error, 0);
    run_open(open_builder_result, 0);
    run_add_row_failures(0);
    run_add_row_failures(SLOT_GROWTH_ROWS - 1);
    run_add_row_failures(SLOT_GROWTH_ROWS);
    run_add_row_failures(SLOT_GROWTH_ROWS * 2);
    destroy_with_rows_frees_everything();
    puts("result: allocation failures keep prior state; release frees exactly once");
    return 0;
}
