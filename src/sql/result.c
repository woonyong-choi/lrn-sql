#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "sql/result.h"

#define INITIAL_ROW_SLOTS 16u

struct result_cancel {
    atomic_bool is_requested;
};

struct result {
    result_kind_t kind;
    result_state_t state;
    result_cancel_t cancel;
    result_limits_t limits;
    result_column_t *columns;
    size_t column_count;
    result_command_t command;
    result_error_t error;
    result_source_ops_t ops;
    void *ctx;
    bool is_source_closed;
    uint64_t rows_delivered;
    char tag[RESULT_MAX_TAG_BYTES];
    char message[RESULT_MAX_ERROR_MESSAGE_BYTES];
    mem_ledger_t ledger;   /* ops가 NULL이면 계상하지 않는다 */
    mem_scope_t scope;
    size_t accounted;
};

/* builder가 쓰는 메모리 행 공급자. 행마다 [셀 배열][복사한 바이트] 한 덩어리다. */
typedef struct {
    value_view_t **rows;
    size_t count;
    size_t capacity;
    size_t cursor;
    size_t column_count;
    size_t used_bytes;
} mem_rows_t;

struct result_builder {
    result_t *result;
    mem_rows_t *mem;
};

static bool is_type_valid(value_type_t type)
{
    return (unsigned)type <= (unsigned)VALUE_BLOB;
}

static bool is_error_class_valid(result_error_class_t error_class)
{
    return (unsigned)error_class <= (unsigned)RESULT_E_INTERNAL;
}

/* value.h에는 view 검증 함수가 없어 같은 view끼리 비교해 TEXT의 UTF-8 유효성을 확인한다. */
static bool is_text_valid(const uint8_t *bytes, size_t size_bytes)
{
    value_view_t view = {VALUE_TEXT, 0, 0.0, bytes, size_bytes};
    int order;
    return value_compare(&view, &view, &order) == VALUE_OK;
}

static result_status_t check_column(const result_column_t *column)
{
    if (column->name == NULL || column->name_len == 0) return RESULT_INVALID;
    if (column->name_len > RESULT_MAX_NAME_BYTES) return RESULT_LIMIT;
    if (!is_type_valid(column->type)) return RESULT_INVALID;
    return is_text_valid(column->name, column->name_len) ? RESULT_OK : RESULT_INVALID;
}

static result_status_t check_columns(const result_column_t *columns, size_t count,
                                     size_t *names_bytes)
{
    if (columns == NULL || count == 0) return RESULT_INVALID;
    if (count > RESULT_MAX_COLUMNS) return RESULT_LIMIT;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        result_status_t status = check_column(&columns[i]);
        if (status != RESULT_OK) return status;
        total += columns[i].name_len;
    }
    *names_bytes = total;
    return RESULT_OK;
}

static result_status_t resolve_limits(const result_limits_t *input, result_limits_t *out)
{
    out->max_row_bytes = RESULT_DEFAULT_MAX_ROW_BYTES;
    out->max_total_bytes = RESULT_DEFAULT_MAX_TOTAL_BYTES;
    if (input == NULL) return RESULT_OK;
    if (input->max_row_bytes > 0) out->max_row_bytes = input->max_row_bytes;
    if (input->max_total_bytes > 0) out->max_total_bytes = input->max_total_bytes;
    return out->max_row_bytes <= out->max_total_bytes ? RESULT_OK : RESULT_INVALID;
}

static result_t *create_result(result_kind_t kind, size_t column_count, size_t names_bytes)
{
    size_t size = sizeof(result_t) + column_count * sizeof(result_column_t) + names_bytes;
    result_t *result = malloc(size);
    if (result == NULL) return NULL;
    memset(result, 0, sizeof(*result));
    result->kind = kind;
    atomic_init(&result->cancel.is_requested, false);
    return result;
}

static void copy_columns(result_t *result, const result_column_t *source, size_t count)
{
    result_column_t *columns = (result_column_t *)(result + 1);
    uint8_t *names = (uint8_t *)(columns + count);
    for (size_t i = 0; i < count; i++) {
        memcpy(names, source[i].name, source[i].name_len);
        columns[i] = source[i];
        columns[i].name = names;
        names += source[i].name_len;
    }
    result->columns = columns;
    result->column_count = count;
}

/* 메시지는 호출 시점에 결과 안의 고정 buffer로 복사한다. 이후 source가 해제돼도 유효하다. */
static void store_error(result_t *result, const result_error_t *error)
{
    size_t len = error->message == NULL ? 0 : error->message_len;
    bool is_truncated = error->is_message_truncated;
    if (len > RESULT_MAX_ERROR_MESSAGE_BYTES) {
        len = RESULT_MAX_ERROR_MESSAGE_BYTES;
        is_truncated = true;
        /* 잘린 위치가 UTF-8 연속 바이트면 문자 경계까지 물린다. */
        while (len > 0 && ((uint8_t)error->message[len] & 0xc0u) == 0x80u) len--;
    }
    if (len > 0) memcpy(result->message, error->message, len);
    result->error = (result_error_t){error->error_class, error->code, result->message, len,
                                     is_truncated};
}

static void store_text_error(result_t *result, result_error_class_t error_class, const char *text)
{
    result_error_t error = {error_class, 0, text, strlen(text), false};
    store_error(result, &error);
}

static void close_source(result_t *result)
{
    if (result->kind != RESULT_KIND_ROWS || result->is_source_closed) return;
    result->is_source_closed = true;
    result->ops.close(result->ctx);
}

/* 종료 상태를 정한 뒤 source를 닫는다. error는 호출 전에 복사돼 있어야 한다. */
static void settle(result_t *result, result_state_t state)
{
    result->state = state;
    close_source(result);
}

static result_next_t settle_failed(result_t *result, result_error_class_t error_class,
                                   const char *text)
{
    store_text_error(result, error_class, text);
    settle(result, RESULT_STATE_FAILED);
    return RESULT_NEXT_FAILED;
}

static result_next_t settle_cancelled(result_t *result)
{
    store_text_error(result, RESULT_E_CANCELLED, "result was cancelled");
    settle(result, RESULT_STATE_CANCELLED);
    return RESULT_NEXT_CANCELLED;
}

static result_next_t settle_source_failure(result_t *result, const result_error_t *error)
{
    bool is_class_usable = is_error_class_valid(error->error_class) &&
                           error->error_class != RESULT_E_NONE &&
                           error->error_class != RESULT_E_CANCELLED;
    if (!is_class_usable) {
        return settle_failed(result, RESULT_E_INTERNAL, "source returned invalid error class");
    }
    store_error(result, error);
    settle(result, RESULT_STATE_FAILED);
    return RESULT_NEXT_FAILED;
}

static bool is_cell_structure_valid(const value_view_t *cell)
{
    switch (cell->type) {
        case VALUE_NULL:
        case VALUE_INTEGER:
            return cell->bytes == NULL && cell->size_bytes == 0;
        case VALUE_REAL:
            return cell->bytes == NULL && cell->size_bytes == 0 && isfinite(cell->real);
        case VALUE_TEXT:
            return cell->size_bytes == 0 || (cell->bytes != NULL &&
                                             is_text_valid(cell->bytes, cell->size_bytes));
        case VALUE_BLOB:
            return cell->size_bytes == 0 || cell->bytes != NULL;
    }
    return false;
}

static bool is_cell_type_allowed(const result_column_t *column, const value_view_t *cell)
{
    if (cell->type == VALUE_NULL) return column->is_nullable;
    return cell->type == column->type;
}

/* 바이트를 읽기 전에 남은 행 예산과 비교해 거짓 길이를 거른다. */
static result_status_t check_cell(const result_column_t *column, const value_view_t *cell,
                                  size_t *remaining_bytes)
{
    if (!is_type_valid(cell->type) || !is_cell_type_allowed(column, cell)) return RESULT_INVALID;
    if ((uint64_t)cell->size_bytes > VALUE_MAX_BYTES) return RESULT_LIMIT;
    if (cell->size_bytes > *remaining_bytes) return RESULT_LIMIT;
    *remaining_bytes -= cell->size_bytes;
    return is_cell_structure_valid(cell) ? RESULT_OK : RESULT_INVALID;
}

/*
 * 행의 형태·타입·한도를 검사하고 행 비용(셀 배열 + 바이트)을 *row_bytes에 채운다.
 * 셀은 value.h의 유효한 view여야 한다: TEXT는 UTF-8, REAL은 유한값, 바이트는 VALUE_MAX_BYTES 이하.
 */
// cost: time O(c + t), heap O(1), stack O(1)
// vars: c = column_count, t = TEXT 셀 바이트 합
// basis: estimate
static result_status_t check_cells(const result_t *result, const value_view_t *cells,
                                   size_t count, size_t *row_bytes)
{
    if (cells == NULL || count != result->column_count) return RESULT_INVALID;
    size_t array_bytes = count * sizeof(value_view_t);
    if (array_bytes > result->limits.max_row_bytes) return RESULT_LIMIT;
    size_t remaining_bytes = result->limits.max_row_bytes - array_bytes;
    for (size_t i = 0; i < count; i++) {
        result_status_t status = check_cell(&result->columns[i], &cells[i], &remaining_bytes);
        if (status != RESULT_OK) return status;
    }
    *row_bytes = result->limits.max_row_bytes - remaining_bytes;
    return RESULT_OK;
}

static result_next_t settle_bad_row(result_t *result, result_status_t status)
{
    if (status == RESULT_LIMIT) {
        return settle_failed(result, RESULT_E_LIMIT, "row exceeds max_row_bytes");
    }
    return settle_failed(result, RESULT_E_INTERNAL, "source returned invalid row");
}

static result_next_t accept_row(result_t *result, const result_step_t *step, result_row_t *row)
{
    size_t row_bytes;
    result_status_t status = check_cells(result, step->row.cells, step->row.cell_count,
                                         &row_bytes);
    if (status != RESULT_OK) return settle_bad_row(result, status);
    result->rows_delivered++;
    *row = step->row;
    return RESULT_NEXT_ROW;
}

static result_next_t apply_step(result_t *result, result_next_t next, const result_step_t *step,
                                result_row_t *row)
{
    switch (next) {
        case RESULT_NEXT_ROW:
            return accept_row(result, step, row);
        case RESULT_NEXT_DONE:
            settle(result, RESULT_STATE_COMPLETE);
            return RESULT_NEXT_DONE;
        case RESULT_NEXT_FAILED:
            return settle_source_failure(result, &step->error);
        case RESULT_NEXT_CANCELLED:
            return settle_cancelled(result);
    }
    return settle_failed(result, RESULT_E_INTERNAL, "source returned invalid status");
}

static result_next_t terminal_next(const result_t *result)
{
    switch (result->state) {
        case RESULT_STATE_COMPLETE:
            return RESULT_NEXT_DONE;
        case RESULT_STATE_CANCELLED:
            return RESULT_NEXT_CANCELLED;
        case RESULT_STATE_OPEN:
        case RESULT_STATE_FAILED:
            break;
    }
    return RESULT_NEXT_FAILED;
}

static result_status_t check_rows_spec(const result_rows_spec_t *spec, result_limits_t *limits,
                                       size_t *names_bytes)
{
    if (spec == NULL || spec->ops == NULL) return RESULT_INVALID;
    if (spec->ops->next == NULL || spec->ops->close == NULL) return RESULT_INVALID;
    result_status_t status = check_columns(spec->columns, spec->column_count, names_bytes);
    if (status != RESULT_OK) return status;
    return resolve_limits(spec->limits, limits);
}

result_status_t result_open_rows(const result_rows_spec_t *spec, result_t **out)
{
    if (out == NULL) return RESULT_INVALID;
    *out = NULL;
    result_limits_t limits;
    size_t names_bytes;
    result_status_t status = check_rows_spec(spec, &limits, &names_bytes);
    if (status != RESULT_OK) return status;
    result_t *result = create_result(RESULT_KIND_ROWS, spec->column_count, names_bytes);
    if (result == NULL) return RESULT_NOMEM;
    copy_columns(result, spec->columns, spec->column_count);
    result->limits = limits;
    result->ops = *spec->ops;
    result->ctx = spec->ctx;
    result->state = RESULT_STATE_OPEN;
    *out = result;
    return RESULT_OK;
}

static result_status_t check_command(const result_command_t *command)
{
    if (command == NULL || command->tag == NULL || command->tag_len == 0) return RESULT_INVALID;
    if (command->tag_len > RESULT_MAX_TAG_BYTES) return RESULT_LIMIT;
    return memchr(command->tag, '\0', command->tag_len) == NULL ? RESULT_OK : RESULT_INVALID;
}

/* ledger가 있으면 결과 구조체 크기를 먼저 reserve한다. 만든 결과가 그 계상과 ledger를 쥔다. */
static result_t *create_accounted(result_kind_t kind, const mem_ledger_t *ledger)
{
    size_t size = sizeof(result_t);
    if (!mem_reserve(ledger, MEM_SCOPE_QUERY, size)) return NULL;
    result_t *result = create_result(kind, 0, 0);
    if (result == NULL) {
        mem_release(ledger, MEM_SCOPE_QUERY, size);
        return NULL;
    }
    if (mem_ledger_enabled(ledger)) {
        result->ledger = *ledger;
        result->scope = MEM_SCOPE_QUERY;
        result->accounted = size;
        mem_ledger_retain(ledger);
    }
    return result;
}

result_status_t result_open_command_ex(const result_command_t *command, const mem_ledger_t *ledger,
                                       result_t **out)
{
    if (out == NULL) return RESULT_INVALID;
    *out = NULL;
    result_status_t status = check_command(command);
    if (status != RESULT_OK) return status;
    result_t *result = create_accounted(RESULT_KIND_COMMAND, ledger);
    if (result == NULL) return RESULT_NOMEM;
    memcpy(result->tag, command->tag, command->tag_len);
    result->command = (result_command_t){command->affected_rows, result->tag, command->tag_len};
    result->state = RESULT_STATE_COMPLETE;
    *out = result;
    return RESULT_OK;
}

result_status_t result_open_command(const result_command_t *command, result_t **out)
{
    return result_open_command_ex(command, NULL, out);
}

static result_status_t check_error(const result_error_t *error)
{
    if (error == NULL || error->error_class == RESULT_E_NONE) return RESULT_INVALID;
    if (!is_error_class_valid(error->error_class)) return RESULT_INVALID;
    if (error->message == NULL && error->message_len > 0) return RESULT_INVALID;
    return RESULT_OK;
}

result_status_t result_open_error_ex(const result_error_t *error, const mem_ledger_t *ledger,
                                     result_t **out)
{
    if (out == NULL) return RESULT_INVALID;
    *out = NULL;
    result_status_t status = check_error(error);
    if (status != RESULT_OK) return status;
    result_t *result = create_accounted(RESULT_KIND_ERROR, ledger);
    if (result == NULL) return RESULT_NOMEM;
    store_error(result, error);
    result->state = RESULT_STATE_FAILED;
    *out = result;
    return RESULT_OK;
}

result_status_t result_open_error(const result_error_t *error, result_t **out)
{
    return result_open_error_ex(error, NULL, out);
}

static result_next_t next_memory_row(void *ctx, result_step_t *step)
{
    mem_rows_t *mem = ctx;
    if (mem->cursor >= mem->count) return RESULT_NEXT_DONE;
    step->row.cells = mem->rows[mem->cursor++];
    step->row.cell_count = mem->column_count;
    return RESULT_NEXT_ROW;
}

static void close_memory_rows(void *ctx)
{
    mem_rows_t *mem = ctx;
    for (size_t i = 0; i < mem->count; i++) free(mem->rows[i]);
    free(mem->rows);
    free(mem);
}

static const result_source_ops_t memory_ops = {next_memory_row, close_memory_rows};

/* 결과 구조, builder 보조 구조, 컬럼 metadata처럼 행 수와 무관한 고정 할당 크기 */
static size_t fixed_bytes(const result_t *result)
{
    size_t total = sizeof(result_t) + sizeof(mem_rows_t) + sizeof(result_builder_t);
    total += result->column_count * sizeof(result_column_t);
    for (size_t i = 0; i < result->column_count; i++) total += result->columns[i].name_len;
    return total;
}

result_status_t result_builder_create(const result_builder_spec_t *spec, result_builder_t **out)
{
    return result_builder_create_ex(spec, NULL, out);
}

result_status_t result_builder_create_ex(const result_builder_spec_t *spec, const mem_ledger_t *ledger,
                                         result_builder_t **out)
{
    if (out == NULL) return RESULT_INVALID;
    *out = NULL;
    if (spec == NULL) return RESULT_INVALID;
    /* 검사는 할당과 reserve보다 먼저 한다. 고정 할당 크기는 결과를 열고 나서 fixed_bytes와 같다. */
    result_limits_t limits;
    size_t names_bytes;
    result_status_t checked = check_columns(spec->columns, spec->column_count, &names_bytes);
    if (checked != RESULT_OK) return checked;
    checked = resolve_limits(spec->limits, &limits);
    if (checked != RESULT_OK) return checked;
    size_t fixed = sizeof(result_t) + spec->column_count * sizeof(result_column_t) + names_bytes +
                   sizeof(mem_rows_t) + sizeof(result_builder_t);
    if (fixed > limits.max_total_bytes) return RESULT_LIMIT;
    if (!mem_reserve(ledger, MEM_SCOPE_QUERY, fixed)) return RESULT_NOMEM;

    mem_rows_t *mem = calloc(1, sizeof(*mem));
    if (mem == NULL) {
        mem_release(ledger, MEM_SCOPE_QUERY, fixed);
        return RESULT_NOMEM;
    }
    mem->column_count = spec->column_count;
    result_rows_spec_t rows = {spec->columns, spec->column_count, &memory_ops, mem, spec->limits};
    result_t *result;
    result_status_t status = result_open_rows(&rows, &result);
    if (status != RESULT_OK) {
        free(mem);
        mem_release(ledger, MEM_SCOPE_QUERY, fixed);
        return status;
    }
    mem->used_bytes = fixed_bytes(result);
    result_builder_t *builder = malloc(sizeof(*builder));
    if (builder == NULL) {
        result_release(&result);  /* 아직 계상을 쥐지 않았다 */
        mem_release(ledger, MEM_SCOPE_QUERY, fixed);
        return RESULT_NOMEM;
    }
    if (mem_ledger_enabled(ledger)) {
        result->ledger = *ledger;
        result->scope = MEM_SCOPE_QUERY;
        result->accounted = fixed;
        mem_ledger_retain(ledger);
    }
    *builder = (result_builder_t){result, mem};
    *out = builder;
    return RESULT_OK;
}

/* 행 비용과 같은 크기의 한 덩어리를 만들어 셀 배열 뒤에 TEXT·BLOB 바이트를 복사한다. */
// cost: time O(c + b), heap O(c + b), stack O(1), alloc 1
// vars: c = cell_count, b = 복사할 TEXT·BLOB 바이트 합
// basis: estimate
static value_view_t *copy_row(const value_view_t *cells, size_t count, size_t row_bytes)
{
    value_view_t *row = malloc(row_bytes);
    if (row == NULL) return NULL;
    uint8_t *bytes = (uint8_t *)(row + count);
    for (size_t i = 0; i < count; i++) {
        row[i] = cells[i];
        row[i].bytes = NULL;
        if (cells[i].size_bytes == 0) continue;
        memcpy(bytes, cells[i].bytes, cells[i].size_bytes);
        row[i].bytes = bytes;
        bytes += cells[i].size_bytes;
    }
    return row;
}

static size_t growth_slots(const mem_rows_t *mem)
{
    if (mem->count < mem->capacity) return 0;
    return mem->capacity == 0 ? INITIAL_ROW_SLOTS : mem->capacity;
}

/* realloc이 실패하면 기존 배열은 그대로다. */
static bool grow_rows(mem_rows_t *mem, size_t slots)
{
    size_t capacity = mem->capacity + slots;
    if (capacity > SIZE_MAX / sizeof(*mem->rows)) return false;
    value_view_t **rows = realloc(mem->rows, capacity * sizeof(*rows));
    if (rows == NULL) return false;
    mem->rows = rows;
    mem->capacity = capacity;
    return true;
}

/* 행 슬롯 배열 증가분도 한도에 포함한다. 실패하면 builder는 호출 전 상태 그대로다. */
result_status_t result_builder_add_row(result_builder_t *builder, const value_view_t *cells,
                                       size_t cell_count)
{
    if (builder == NULL) return RESULT_INVALID;
    result_t *result = builder->result;
    mem_rows_t *mem = builder->mem;
    size_t row_bytes;
    result_status_t status = check_cells(result, cells, cell_count, &row_bytes);
    if (status != RESULT_OK) return status;
    size_t slots = growth_slots(mem);
    if (slots > (SIZE_MAX - row_bytes) / sizeof(value_view_t *)) return RESULT_LIMIT;
    size_t charge = row_bytes + slots * sizeof(value_view_t *);
    if (charge > result->limits.max_total_bytes - mem->used_bytes) return RESULT_LIMIT;

    /* 행과, 배열을 늘릴 때는 새 배열 전체를 먼저 reserve한다. 옛 배열은 이미 계상돼 있어 순간 최대치가 old+new다. */
    const mem_ledger_t *ledger = &result->ledger;
    size_t old_array = mem->capacity * sizeof(*mem->rows);
    size_t new_array = (mem->capacity + slots) * sizeof(*mem->rows);
    if (!mem_reserve(ledger, MEM_SCOPE_QUERY, row_bytes)) return RESULT_NOMEM;
    if (slots > 0 && !mem_reserve(ledger, MEM_SCOPE_QUERY, new_array)) {
        mem_release(ledger, MEM_SCOPE_QUERY, row_bytes);
        return RESULT_NOMEM;
    }
    value_view_t *row = copy_row(cells, cell_count, row_bytes);
    if (row == NULL) goto fail;
    if (slots > 0 && !grow_rows(mem, slots)) {
        free(row);
        goto fail;
    }
    if (slots > 0) mem_release(ledger, MEM_SCOPE_QUERY, old_array);
    mem->rows[mem->count++] = row;
    mem->used_bytes += charge;
    if (mem_ledger_enabled(ledger)) result->accounted += row_bytes + new_array - old_array;
    return RESULT_OK;
fail:
    mem_release(ledger, MEM_SCOPE_QUERY, row_bytes);
    if (slots > 0) mem_release(ledger, MEM_SCOPE_QUERY, new_array);
    return RESULT_NOMEM;
}

result_status_t result_builder_finish(result_builder_t **builder, result_t **out)
{
    if (out == NULL) return RESULT_INVALID;
    *out = NULL;
    if (builder == NULL || *builder == NULL) return RESULT_INVALID;
    result_t *result = (*builder)->result;
    /* builder 구조체는 여기서 사라지므로 해제한 뒤 그 몫을 돌려주고, 나머지는 결과가 쥔다 */
    bool return_builder_bytes = result->accounted >= sizeof(result_builder_t);
    if (return_builder_bytes) result->accounted -= sizeof(result_builder_t);
    *out = result;
    free(*builder);
    *builder = NULL;
    if (return_builder_bytes) mem_release(&result->ledger, result->scope, sizeof(result_builder_t));
    return RESULT_OK;
}

void result_builder_destroy(result_builder_t **builder)
{
    if (builder == NULL || *builder == NULL) return;
    /* builder 구조체를 먼저 해제한다. 그 몫은 결과의 계상에 들어 있어 result_release가 결과를 해제한 뒤 함께 돌려준다 */
    result_t *result = (*builder)->result;
    free(*builder);
    *builder = NULL;
    result_release(&result);
}

result_kind_t result_kind(const result_t *result)
{
    assert(result != NULL);
    return result->kind;
}

size_t result_column_count(const result_t *result)
{
    assert(result != NULL);
    return result->column_count;
}

const result_column_t *result_column(const result_t *result, size_t index)
{
    assert(result != NULL);
    return index < result->column_count ? &result->columns[index] : NULL;
}

const result_command_t *result_command(const result_t *result)
{
    assert(result != NULL);
    return result->kind == RESULT_KIND_COMMAND ? &result->command : NULL;
}

result_next_t result_next(result_t *result, result_row_t *row)
{
    assert(result != NULL && row != NULL);
    *row = (result_row_t){NULL, 0};
    if (result->state != RESULT_STATE_OPEN) return terminal_next(result);
    if (atomic_load(&result->cancel.is_requested)) return settle_cancelled(result);
    result_step_t step = {.cancel = &result->cancel};
    result_next_t next = result->ops.next(result->ctx, &step);
    return apply_step(result, next, &step, row);
}

result_outcome_t result_outcome(const result_t *result)
{
    assert(result != NULL);
    bool is_ended_early = result->state == RESULT_STATE_FAILED ||
                          result->state == RESULT_STATE_CANCELLED;
    return (result_outcome_t){result->state, result->rows_delivered,
                              is_ended_early && result->rows_delivered > 0,
                              is_ended_early ? &result->error : NULL};
}

void result_cancel(result_t *result)
{
    assert(result != NULL);
    atomic_store(&result->cancel.is_requested, true);
}

bool result_cancel_requested(const result_cancel_t *cancel)
{
    assert(cancel != NULL);
    return atomic_load(&cancel->is_requested);
}

void result_release(result_t **result)
{
    if (result == NULL || *result == NULL) return;
    close_source(*result);
    mem_ledger_t ledger = (*result)->ledger;
    mem_scope_t scope = (*result)->scope;
    size_t accounted = (*result)->accounted;
    free(*result);
    /* 몫은 메모리를 해제한 뒤에 돌려준다. 먼저 돌려주면 다른 곳이 그 몫으로 할당해 아직 살아 있는 이 결과와 겹친다 */
    mem_release(&ledger, scope, accounted);
    mem_ledger_drop(&ledger);
    *result = NULL;
}

size_t result_accounted_bytes(const result_t *result)
{
    assert(result != NULL);
    return result->accounted;
}

void result_rebind_ledger(result_t *result, const mem_ledger_t *ledger, mem_scope_t scope)
{
    assert(result != NULL);
    if (!mem_ledger_enabled(&result->ledger) || !mem_ledger_enabled(ledger)) return;
    mem_ledger_retain(ledger);
    mem_ledger_t old = result->ledger;
    result->ledger = *ledger;
    result->scope = scope;
    mem_ledger_drop(&old);
}
