#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sql/result.h"

#define MAX_SCRIPT_ROWS 8u
#define BIG_BYTES ((size_t)1 << 20)

typedef struct {
    value_view_t rows[MAX_SCRIPT_ROWS * 3];
    size_t row_count;
    size_t index;
    result_next_t end;
    result_error_t error;
    char message[32];
    int next_calls;
    int close_calls;
    atomic_int is_waiting;
} script_t;

static const result_column_t columns[3] = {
    {(const uint8_t *)"id", 2, VALUE_INTEGER, false},
    {(const uint8_t *)"note", 4, VALUE_TEXT, true},
    {(const uint8_t *)"data", 4, VALUE_BLOB, true},
};

static value_view_t null_view(void)
{
    return (value_view_t){VALUE_NULL, 0, 0.0, NULL, 0};
}

static value_view_t integer_view(int64_t integer)
{
    return (value_view_t){VALUE_INTEGER, integer, 0.0, NULL, 0};
}

static value_view_t text_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_TEXT, 0, 0.0, bytes, size};
}

static value_view_t blob_view(const void *bytes, size_t size)
{
    return (value_view_t){VALUE_BLOB, 0, 0.0, bytes, size};
}

static void add_script_row(script_t *script, value_view_t id, value_view_t note, value_view_t data)
{
    value_view_t *row = &script->rows[script->row_count * 3];
    row[0] = id;
    row[1] = note;
    row[2] = data;
    script->row_count++;
}

static result_next_t script_next(void *ctx, result_step_t *step)
{
    script_t *script = ctx;
    script->next_calls++;
    if (script->index < script->row_count) {
        step->row = (result_row_t){&script->rows[script->index++ * 3], 3};
        return RESULT_NEXT_ROW;
    }
    if (script->end == RESULT_NEXT_FAILED) {
        step->error = script->error;
        step->error.message = script->message;
    }
    return script->end;
}

/* close가 호출되면 source가 가진 메시지를 지운다. 결과가 먼저 복사하지 않았다면 검사가 잡는다. */
static void script_close(void *ctx)
{
    script_t *script = ctx;
    script->close_calls++;
    memset(script->message, 'X', sizeof(script->message));
}

static const result_source_ops_t script_ops = {script_next, script_close};

static void script_set_error(script_t *script, result_error_class_t error_class, int code,
                             const char *message)
{
    script->end = RESULT_NEXT_FAILED;
    script->error = (result_error_t){error_class, code, NULL, strlen(message), false};
    memcpy(script->message, message, strlen(message));
}

static result_t *open_script(script_t *script, const result_limits_t *limits)
{
    result_rows_spec_t spec = {columns, 3, &script_ops, script, limits};
    result_t *result = NULL;
    assert(result_open_rows(&spec, &result) == RESULT_OK && result != NULL);
    return result;
}

static void assert_bytes(const value_view_t *view, const void *expected, size_t size)
{
    assert(view->size_bytes == size);
    assert(size == 0 || memcmp(view->bytes, expected, size) == 0);
}

/* 근거: #64 완료 조건 1. NULL·빈값·내장 NUL·정수가 구별돼 그대로 전달된다. */
static void builder_preserves_null_empty_and_nul_bytes(void)
{
    result_builder_spec_t spec = {columns, 3, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    value_view_t first[3] = {integer_view(-7), null_view(), blob_view("", 0)};
    value_view_t second[3] = {integer_view(2), text_view("a\0b", 3), blob_view("\0\xff", 2)};
    value_view_t third[3] = {integer_view(3), text_view("", 0), null_view()};
    assert(result_builder_add_row(builder, first, 3) == RESULT_OK);
    assert(result_builder_add_row(builder, second, 3) == RESULT_OK);
    assert(result_builder_add_row(builder, third, 3) == RESULT_OK);
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK && builder == NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW && row.cell_count == 3);
    assert(row.cells[0].integer == -7 && row.cells[1].type == VALUE_NULL);
    assert(row.cells[2].type == VALUE_BLOB && row.cells[2].size_bytes == 0);
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    assert_bytes(&row.cells[1], "a\0b", 3);
    assert_bytes(&row.cells[2], "\0\xff", 2);
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    assert(row.cells[1].type == VALUE_TEXT && row.cells[1].size_bytes == 0);
    assert(row.cells[2].type == VALUE_NULL);
    assert(result_next(result, &row) == RESULT_NEXT_DONE && row.cells == NULL);
    result_outcome_t outcome = result_outcome(result);
    assert(outcome.state == RESULT_STATE_COMPLETE && outcome.rows_delivered == 3);
    assert(!outcome.is_partial && outcome.error == NULL);
    result_release(&result);
    assert(result == NULL);
}

/* 근거: #64 완료 조건 1. 1MiB TEXT·BLOB이 손실 없이 복사되고 입력 변경과 독립이다. */
static void builder_copies_large_text_and_blob(void)
{
    char *text = malloc(BIG_BYTES);
    unsigned char *blob = malloc(BIG_BYTES);
    assert(text != NULL && blob != NULL);
    memset(text, 'k', BIG_BYTES);
    for (size_t i = 0; i < BIG_BYTES; i++) blob[i] = (unsigned char)(i * 31u);
    result_builder_spec_t spec = {columns, 3, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    value_view_t cells[3] = {integer_view(1), text_view(text, BIG_BYTES), blob_view(blob, BIG_BYTES)};
    assert(result_builder_add_row(builder, cells, 3) == RESULT_OK);
    memset(text, 'z', BIG_BYTES);
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    assert(row.cells[1].size_bytes == BIG_BYTES && ((const char *)row.cells[1].bytes)[BIG_BYTES - 1] == 'k');
    assert(row.cells[2].size_bytes == BIG_BYTES);
    assert(row.cells[2].bytes[12345] == (unsigned char)(12345u * 31u));
    result_release(&result);
    free(text);
    free(blob);
}

/* 근거: #64 완료 조건 1. 컬럼 이름·타입·nullable이 그대로 전달되고 범위 밖 index는 NULL이다. */
static void result_exposes_column_metadata(void)
{
    result_builder_spec_t spec = {columns, 3, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK);
    assert(result_kind(result) == RESULT_KIND_ROWS && result_column_count(result) == 3);
    const result_column_t *note = result_column(result, 1);
    assert(note->name != columns[1].name && note->name_len == 4);
    assert(memcmp(note->name, "note", 4) == 0 && note->type == VALUE_TEXT && note->is_nullable);
    assert(!result_column(result, 0)->is_nullable && result_column(result, 3) == NULL);
    assert(result_command(result) == NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_DONE);
    assert(result_outcome(result).rows_delivered == 0);
    result_release(&result);
}

/* 근거: #64 계약(영향행수). COMMAND는 만들자마자 COMPLETE이고 행이 없다. */
static void command_result_reports_affected_rows(void)
{
    result_command_t command = {42, "INSERT", 6};
    result_t *result = NULL;
    assert(result_open_command(&command, &result) == RESULT_OK);
    assert(result_kind(result) == RESULT_KIND_COMMAND && result_column_count(result) == 0);
    assert(result_command(result)->affected_rows == 42 && result_command(result)->tag_len == 6);
    assert(memcmp(result_command(result)->tag, "INSERT", 6) == 0);
    assert(result_outcome(result).state == RESULT_STATE_COMPLETE);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_DONE);
    result_release(&result);
    command.tag_len = RESULT_MAX_TAG_BYTES + 1;
    assert(result_open_command(&command, &result) == RESULT_LIMIT && result == NULL);
    command.tag_len = 0;
    assert(result_open_command(&command, &result) == RESULT_INVALID && result == NULL);
}

/* 근거: #64 완료 조건 2. 오류 결과는 성공과 구별되고 긴 메시지는 UTF-8 경계에서 잘린다. */
static void error_result_is_failed_and_truncates_message(void)
{
    char message[RESULT_MAX_ERROR_MESSAGE_BYTES + 100];
    for (size_t i = 0; i + 1 < sizeof(message); i += 2) memcpy(&message[i], "\xc3\xa9", 2);
    result_error_t error = {RESULT_E_CONSTRAINT, 23505, message, sizeof(message) - 1, false};
    result_t *result = NULL;
    assert(result_open_error(&error, &result) == RESULT_OK);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_FAILED);
    result_outcome_t outcome = result_outcome(result);
    assert(outcome.state == RESULT_STATE_FAILED && !outcome.is_partial);
    assert(outcome.error->error_class == RESULT_E_CONSTRAINT && outcome.error->code == 23505);
    assert(outcome.error->is_message_truncated);
    assert(outcome.error->message_len <= RESULT_MAX_ERROR_MESSAGE_BYTES);
    assert(outcome.error->message_len % 2 == 0);
    result_release(&result);
    error.error_class = RESULT_E_NONE;
    assert(result_open_error(&error, &result) == RESULT_INVALID && result == NULL);
}

/* 근거: #64 완료 조건 3. 정상 완료에서 close는 DONE 시점에 한 번만 호출된다. */
static void source_completes_and_closes_exactly_once(void)
{
    script_t script = {.end = RESULT_NEXT_DONE};
    add_script_row(&script, integer_view(1), null_view(), null_view());
    add_script_row(&script, integer_view(2), text_view("x", 1), null_view());
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW && script.close_calls == 0);
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    assert(result_outcome(result).state == RESULT_STATE_OPEN);
    assert(result_next(result, &row) == RESULT_NEXT_DONE && script.close_calls == 1);
    assert(result_next(result, &row) == RESULT_NEXT_DONE && script.next_calls == 3);
    assert(result_outcome(result).state == RESULT_STATE_COMPLETE && !result_outcome(result).is_partial);
    result_release(&result);
    result_release(&result);
    assert(script.close_calls == 1 && result == NULL);
}

/* 근거: #64 완료 조건 2. 행 전달 뒤 실패는 부분 결과이고 메시지는 close 전에 복사된다. */
static void source_failure_after_rows_is_partial_and_keeps_message(void)
{
    script_t script = {0};
    add_script_row(&script, integer_view(1), null_view(), null_view());
    script_set_error(&script, RESULT_E_IO, 5, "disk read failed");
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    assert(result_next(result, &row) == RESULT_NEXT_FAILED && script.close_calls == 1);
    result_outcome_t outcome = result_outcome(result);
    assert(outcome.state == RESULT_STATE_FAILED && outcome.is_partial && outcome.rows_delivered == 1);
    assert(outcome.error->error_class == RESULT_E_IO && outcome.error->code == 5);
    assert(outcome.error->message_len == 16 && memcmp(outcome.error->message, "disk read failed", 16) == 0);
    assert(result_next(result, &row) == RESULT_NEXT_FAILED && script.next_calls == 2);
    result_release(&result);
    assert(script.close_calls == 1);
}

/* 근거: #64 완료 조건 2. 행 없이 실패하면 부분 결과가 아니다. */
static void source_failure_without_rows_is_not_partial(void)
{
    script_t script = {0};
    script_set_error(&script, RESULT_E_BUSY, 0, "table is locked");
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_FAILED);
    assert(!result_outcome(result).is_partial && result_outcome(result).rows_delivered == 0);
    result_release(&result);
}

/* 근거: #64 완료 조건 2. 취소는 성공과 구별되고 부분 결과로 표시되며 이후 source를 부르지 않는다. */
static void cancel_after_rows_is_partial_cancelled(void)
{
    script_t script = {.end = RESULT_NEXT_DONE};
    add_script_row(&script, integer_view(1), null_view(), null_view());
    add_script_row(&script, integer_view(2), null_view(), null_view());
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    result_cancel(result);
    assert(script.close_calls == 0);
    assert(result_next(result, &row) == RESULT_NEXT_CANCELLED && script.close_calls == 1);
    result_outcome_t outcome = result_outcome(result);
    assert(outcome.state == RESULT_STATE_CANCELLED && outcome.is_partial);
    assert(outcome.error->error_class == RESULT_E_CANCELLED);
    assert(result_next(result, &row) == RESULT_NEXT_CANCELLED && script.next_calls == 1);
    result_release(&result);
    assert(script.close_calls == 1);
}

/* 근거: #64 완료 조건 2. 이미 완료된 결과의 취소 요청은 상태를 바꾸지 않는다. */
static void cancel_after_done_keeps_complete(void)
{
    script_t script = {.end = RESULT_NEXT_DONE};
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_DONE);
    result_cancel(result);
    assert(result_next(result, &row) == RESULT_NEXT_DONE);
    assert(result_outcome(result).state == RESULT_STATE_COMPLETE);
    result_release(&result);
}

/* 근거: #64 완료 조건 3. 소비를 끝내지 않고 release해도 close는 정확히 한 번이다. */
static void release_before_end_closes_once(void)
{
    script_t script = {.end = RESULT_NEXT_DONE};
    add_script_row(&script, integer_view(1), null_view(), null_view());
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW && script.close_calls == 0);
    result_release(&result);
    assert(result == NULL && script.close_calls == 1);
    result_release(&result);
    result_release(NULL);
    assert(script.close_calls == 1);
}

/* 근거: #64 계약(오류 종류). 잘못된 source 출력은 INTERNAL·LIMIT 실패이며 close는 한 번이다. */
static void invalid_source_output_fails_closed(void)
{
    script_t script = {0};
    script_set_error(&script, RESULT_E_CANCELLED, 0, "bad class");
    result_t *result = open_script(&script, NULL);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_FAILED);
    assert(result_outcome(result).error->error_class == RESULT_E_INTERNAL);
    result_release(&result);
    assert(script.close_calls == 1);

    script_t mismatch = {.end = RESULT_NEXT_DONE};
    add_script_row(&mismatch, null_view(), null_view(), null_view());
    result = open_script(&mismatch, NULL);
    assert(result_next(result, &row) == RESULT_NEXT_FAILED);
    assert(result_outcome(result).error->error_class == RESULT_E_INTERNAL);
    assert(result_outcome(result).rows_delivered == 0 && mismatch.close_calls == 1);
    result_release(&result);

    script_t huge = {.end = RESULT_NEXT_DONE};
    add_script_row(&huge, integer_view(1), text_view("abcdef", 6), null_view());
    result_limits_t limits = {100, 200};
    result = open_script(&huge, &limits);
    assert(result_next(result, &row) == RESULT_NEXT_FAILED);
    assert(result_outcome(result).error->error_class == RESULT_E_LIMIT);
    result_release(&result);
}

/* 근거: #64 계약(생성 실패). 실패하면 *out은 NULL이고 source ctx는 호출자 소유로 남는다. */
static void failed_open_leaves_source_with_caller(void)
{
    script_t script = {0};
    result_t *result = (result_t *)&script;
    result_rows_spec_t spec = {columns, 0, &script_ops, &script, NULL};
    assert(result_open_rows(&spec, &result) == RESULT_INVALID && result == NULL);
    spec.column_count = RESULT_MAX_COLUMNS + 1;
    assert(result_open_rows(&spec, &result) == RESULT_LIMIT && result == NULL);
    spec.column_count = 3;
    spec.ops = NULL;
    assert(result_open_rows(&spec, &result) == RESULT_INVALID && result == NULL);
    result_column_t bad_type[1] = {{(const uint8_t *)"id", 2, (value_type_t)99, false}};
    result_column_t bad_name[1] = {{(const uint8_t *)"\xff", 1, VALUE_TEXT, false}};
    spec = (result_rows_spec_t){bad_type, 1, &script_ops, &script, NULL};
    assert(result_open_rows(&spec, &result) == RESULT_INVALID);
    spec.columns = bad_name;
    assert(result_open_rows(&spec, &result) == RESULT_INVALID);
    result_limits_t limits = {200, 100};
    spec = (result_rows_spec_t){columns, 3, &script_ops, &script, &limits};
    assert(result_open_rows(&spec, &result) == RESULT_INVALID && result == NULL);
    assert(script.close_calls == 0 && script.next_calls == 0);
}

/* 근거: #64 계약(메모리 한도). 한도 초과는 builder를 바꾸지 않고, 맞는 행은 계속 추가된다. */
static void builder_limits_reject_without_changing_rows(void)
{
    result_limits_t limits = {400, 1200};
    result_builder_spec_t spec = {columns, 3, &limits};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    char big[512] = {0};
    value_view_t small[3] = {integer_view(1), text_view("ok", 2), null_view()};
    value_view_t over_row[3] = {integer_view(2), text_view(big, sizeof(big)), null_view()};
    assert(result_builder_add_row(builder, small, 3) == RESULT_OK);
    assert(result_builder_add_row(builder, over_row, 3) == RESULT_LIMIT);
    value_view_t wrong_count[2] = {integer_view(3), null_view()};
    assert(result_builder_add_row(builder, wrong_count, 2) == RESULT_INVALID);
    value_view_t null_id[3] = {null_view(), null_view(), null_view()};
    assert(result_builder_add_row(builder, null_id, 3) == RESULT_INVALID);
    int accepted = 1;
    while (result_builder_add_row(builder, small, 3) == RESULT_OK) accepted++;
    assert(accepted >= 2 && accepted < 20);
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK);
    result_row_t row;
    int seen = 0;
    while (result_next(result, &row) == RESULT_NEXT_ROW) {
        assert(row.cells[0].integer == 1 && row.cells[1].size_bytes == 2);
        seen++;
    }
    assert(seen == accepted);
    result_release(&result);
    limits = (result_limits_t){4096, 100};
    assert(result_builder_create(&spec, &builder) == RESULT_INVALID && builder == NULL);
    limits = (result_limits_t){10, 20};
    assert(result_builder_create(&spec, &builder) == RESULT_LIMIT && builder == NULL);
}

/* 근거: #64 계약(builder 수명). finish는 builder를 소비하고 destroy는 한 번만 해제한다. */
static void builder_finish_and_destroy_are_single_use(void)
{
    result_builder_spec_t spec = {columns, 3, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    result_t *result = (result_t *)&builder;
    assert(result_builder_finish(NULL, &result) == RESULT_INVALID && result == NULL);
    assert(result_builder_finish(&builder, NULL) == RESULT_INVALID && builder != NULL);
    result_builder_destroy(&builder);
    assert(builder == NULL);
    result_builder_destroy(&builder);
    assert(result_builder_finish(&builder, &result) == RESULT_INVALID && result == NULL);
}

static result_next_t wait_for_cancel_next(void *ctx, result_step_t *step)
{
    script_t *script = ctx;
    atomic_store(&script->is_waiting, 1);
    for (int i = 0; i < 3000; i++) {
        if (result_cancel_requested(step->cancel)) return RESULT_NEXT_CANCELLED;
        usleep(1000);
    }
    return RESULT_NEXT_DONE;
}

static void *consume_until_end(void *arg)
{
    result_row_t row;
    return (void *)(intptr_t)result_next(arg, &row);
}

/* 근거: #64 완료 조건 2. 다른 스레드의 cancel이 next 안의 source에게 관측된다. */
static void cancel_from_other_thread_reaches_blocked_source(void)
{
    script_t script = {0};
    result_source_ops_t ops = {wait_for_cancel_next, script_close};
    result_rows_spec_t spec = {columns, 3, &ops, &script, NULL};
    result_t *result = NULL;
    assert(result_open_rows(&spec, &result) == RESULT_OK);
    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, consume_until_end, result) == 0);
    while (atomic_load(&script.is_waiting) == 0) usleep(1000);
    result_cancel(result);
    void *outcome;
    assert(pthread_join(consumer, &outcome) == 0);
    assert((result_next_t)(intptr_t)outcome == RESULT_NEXT_CANCELLED);
    assert(result_outcome(result).state == RESULT_STATE_CANCELLED && script.close_calls == 1);
    result_release(&result);
}

/* 근거: #64 완료 조건 1·3. 서로 다른 소비자가 문자열 파싱 없이 같은 결과를 읽고 복사본은 release 뒤에도 남는다. */
static void consumers_read_typed_cells_and_clone_survives_release(void)
{
    result_builder_spec_t spec = {columns, 3, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create(&spec, &builder) == RESULT_OK);
    value_view_t cells[3] = {integer_view(9), text_view("a\0b", 3), null_view()};
    assert(result_builder_add_row(builder, cells, 3) == RESULT_OK);
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK);
    result_row_t row;
    assert(result_next(result, &row) == RESULT_NEXT_ROW);
    value_t kept[3];
    memset(kept, 0, sizeof(kept));
    for (size_t i = 0; i < 3; i++) {
        assert(row.cells[i].type == cells[i].type);
        assert(value_clone(&row.cells[i], &kept[i]) == VALUE_OK);
    }
    result_release(&result);
    assert(kept[0].view.integer == 9 && kept[2].view.type == VALUE_NULL);
    assert_bytes(&kept[1].view, "a\0b", 3);
    for (size_t i = 0; i < 3; i++) value_clear(&kept[i]);
}

int main(void)
{
    builder_preserves_null_empty_and_nul_bytes();
    builder_copies_large_text_and_blob();
    result_exposes_column_metadata();
    command_result_reports_affected_rows();
    error_result_is_failed_and_truncates_message();
    source_completes_and_closes_exactly_once();
    source_failure_after_rows_is_partial_and_keeps_message();
    source_failure_without_rows_is_not_partial();
    cancel_after_rows_is_partial_cancelled();
    cancel_after_done_keeps_complete();
    release_before_end_closes_once();
    invalid_source_output_fails_closed();
    failed_open_leaves_source_with_caller();
    builder_limits_reject_without_changing_rows();
    builder_finish_and_destroy_are_single_use();
    cancel_from_other_thread_reaches_blocked_source();
    consumers_read_typed_cells_and_clone_survives_release();
    puts("result: contract checks passed");
    return 0;
}
