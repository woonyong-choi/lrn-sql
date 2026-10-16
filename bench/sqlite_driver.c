/* #137: 동일 C API 경계에서 전체 결과를 소비한 뒤 시간을 측정한다. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "sql/database.h"

typedef struct {
    char *data;
    size_t size, capacity;
} buffer_t;

typedef struct {
    char **rows;
    size_t count, capacity, columns, encoded_bytes;
    uint64_t affected;
    int error;
    char message[1024];
} response_t;

typedef struct {
    database_t *mini;
    sqlite3 *sqlite;
    const char *path;
    int is_sqlite;
} engine_t;

static void *resize(void *data, size_t bytes)
{
    void *out = realloc(data, bytes);
    if (out == NULL) { fputs("allocation failed\n", stderr); exit(2); }
    return out;
}

static void append(buffer_t *buffer, const char *data, size_t size)
{
    if (buffer->size + size + 1 > buffer->capacity) {
        buffer->capacity = (buffer->size + size + 1) * 2;
        buffer->data = resize(buffer->data, buffer->capacity);
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
}

static void append_cell(buffer_t *buffer, const value_view_t *cell)
{
    char number[96];
    switch (cell->type) {
        case VALUE_NULL: append(buffer, "N;", 2); return;
        case VALUE_INTEGER:
            snprintf(number, sizeof(number), "I%" PRId64 ";", cell->integer);
            append(buffer, number, strlen(number)); return;
        case VALUE_REAL:
            snprintf(number, sizeof(number), "R%a;", cell->real);
            append(buffer, number, strlen(number)); return;
        case VALUE_TEXT: append(buffer, "T", 1); break;
        case VALUE_BLOB: append(buffer, "B", 1); break;
    }
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < cell->size_bytes; i++) {
        char pair[] = {hex[cell->bytes[i] >> 4], hex[cell->bytes[i] & 15]};
        append(buffer, pair, 2);
    }
    append(buffer, ";", 1);
}

static void append_row(response_t *response, char *row)
{
    response->encoded_bytes += strlen(row) + 1;
    if (response->encoded_bytes > ((size_t)512 << 20)) {
        fputs("encoded result exceeds 512 MiB\n", stderr); exit(2);
    }
    if (response->count == response->capacity) {
        response->capacity = response->capacity == 0 ? 16 : response->capacity * 2;
        response->rows = resize(response->rows, response->capacity * sizeof(char *));
    }
    response->rows[response->count++] = row;
}

static void consume_mini(result_t *result, response_t *response)
{
    if (result_kind(result) == RESULT_KIND_COMMAND) {
        response->affected = result_command(result)->affected_rows;
        return;
    }
    response->columns = result_column_count(result);
    result_row_t row;
    result_next_t state;
    while ((state = result_next(result, &row)) == RESULT_NEXT_ROW) {
        buffer_t buffer = {0};
        for (size_t i = 0; i < row.cell_count; i++) append_cell(&buffer, &row.cells[i]);
        append_row(response, buffer.data);
    }
    if (state == RESULT_NEXT_DONE) return;
    result_outcome_t outcome = result_outcome(result);
    response->error = outcome.error == NULL ? -1 : (int)outcome.error->error_class;
    if (outcome.error != NULL) {
        snprintf(response->message, sizeof(response->message), "%.*s",
                 (int)outcome.error->message_len, outcome.error->message);
    }
}

/* cost: time O(q + b), heap O(b), stack O(1), io O(p) */
/* vars: q = SQL 실행 비용, b = 반환 결과 바이트, p = 엔진이 접근하는 페이지 수 */
/* basis: estimate */
static void execute_mini(engine_t *engine, const char *sql, response_t *response)
{
    result_t *result = NULL;
    database_exec_status_t status = database_exec(engine->mini,
        (const uint8_t *)sql, strlen(sql), NULL, &result);
    if (status != DATABASE_EXEC_OK || result == NULL) {
        response->error = -1;
        snprintf(response->message, sizeof(response->message), "database_exec status %d", status);
        return;
    }
    consume_mini(result, response);
    result_release(&result);
}

static value_view_t sqlite_cell(sqlite3_stmt *statement, int column)
{
    value_view_t cell = {0};
    switch (sqlite3_column_type(statement, column)) {
        case SQLITE_NULL: cell.type = VALUE_NULL; break;
        case SQLITE_INTEGER:
            cell.type = VALUE_INTEGER; cell.integer = sqlite3_column_int64(statement, column); break;
        case SQLITE_FLOAT:
            cell.type = VALUE_REAL; cell.real = sqlite3_column_double(statement, column); break;
        case SQLITE_TEXT:
            cell.type = VALUE_TEXT; cell.bytes = sqlite3_column_text(statement, column);
            cell.size_bytes = (size_t)sqlite3_column_bytes(statement, column); break;
        case SQLITE_BLOB:
            cell.type = VALUE_BLOB; cell.bytes = sqlite3_column_blob(statement, column);
            cell.size_bytes = (size_t)sqlite3_column_bytes(statement, column); break;
    }
    return cell;
}

/* cost: time O(q + b), heap O(b), stack O(1), io O(p) */
/* vars: q = SQL 실행 비용, b = 반환 결과 바이트, p = 엔진이 접근하는 페이지 수 */
/* basis: estimate */
static void execute_sqlite(engine_t *engine, const char *sql, response_t *response)
{
    sqlite3_stmt *statement = NULL;
    int status = sqlite3_prepare_v2(engine->sqlite, sql, -1, &statement, NULL);
    if (status == SQLITE_OK) {
        response->columns = (size_t)sqlite3_column_count(statement);
        while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
            buffer_t buffer = {0};
            for (size_t i = 0; i < response->columns; i++) {
                value_view_t cell = sqlite_cell(statement, (int)i);
                append_cell(&buffer, &cell);
            }
            append_row(response, buffer.data);
        }
    }
    if (status != SQLITE_DONE) {
        response->error = status;
        snprintf(response->message, sizeof(response->message), "%s", sqlite3_errmsg(engine->sqlite));
    } else if (strncmp(sql, "INSERT", 6) == 0 || strncmp(sql, "UPDATE", 6) == 0 || strncmp(sql, "DELETE", 6) == 0) {
        response->affected = (uint64_t)sqlite3_changes(engine->sqlite);
    }
    int finalized = sqlite3_finalize(statement);
    if (finalized != SQLITE_OK && response->error == 0) response->error = finalized;
}

/* cost: time O(p), heap O(p), stack O(1), io O(p) */
/* vars: p = 카탈로그 또는 연결 초기화에 접근하는 페이지 수 */
/* basis: estimate */
static int open_engine(engine_t *engine, int is_new)
{
    if (!engine->is_sqlite) {
        return is_new ? database_create(engine->path, &engine->mini) :
                        database_open(engine->path, true, &engine->mini);
    }
    int status = sqlite3_open(engine->path, &engine->sqlite);
    if (status != SQLITE_OK) return status;
    return sqlite3_exec(engine->sqlite,
        "PRAGMA page_size=4096; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
        "PRAGMA fullfsync=OFF; PRAGMA cache_size=-2048; PRAGMA mmap_size=0; PRAGMA temp_store=FILE;",
        NULL, NULL, NULL);
}

/* cost: time O(p), heap O(1), stack O(1), io O(p) */
/* vars: p = 닫을 때 남은 엔진 페이지 수 */
/* basis: estimate */
static int close_engine(engine_t *engine)
{
    return engine->is_sqlite ? sqlite3_close(engine->sqlite) : database_close(engine->mini);
}

static uint64_t now_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) exit(2);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

/*
 * 이 프로세스의 최고 RSS. Linux의 ru_maxrss는 fork 전 부모(측정 Python)의 최고치를 이어받으므로
 * exec 뒤 새로 시작하는 /proc/self/status의 VmHWM을 쓴다.
 */
static uint64_t peak_rss_bytes(void)
{
#ifdef __linux__
    FILE *status = fopen("/proc/self/status", "r");
    if (status == NULL) exit(2);
    char line[256];
    unsigned long long kib = 0;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (sscanf(line, "VmHWM: %llu kB", &kib) == 1) break;
    }
    fclose(status);
    if (kib == 0) exit(2);
    return (uint64_t)kib * 1024;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) exit(2);
    return (uint64_t)usage.ru_maxrss;  /* macOS는 바이트 단위다 */
#endif
}

/* cost: time O(b), heap O(1), stack O(1), io O(b) */
/* vars: b = 결과 프로토콜 바이트 수 */
/* basis: estimate */
static void emit_response(response_t *response, uint64_t elapsed, const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0) exit(2);
    uint64_t rss = peak_rss_bytes();
    printf("%d %" PRIu64 " %zu %zu %" PRIu64 " %" PRIu64 " %lld\n",
           response->error, elapsed, response->count, response->columns,
           response->affected, rss, (long long)info.st_size);
    buffer_t message = {0};
    value_view_t error = {.type = VALUE_TEXT, .bytes = (const uint8_t *)response->message,
                         .size_bytes = strlen(response->message)};
    append_cell(&message, &error);
    puts(message.data);
    free(message.data);
    for (size_t i = 0; i < response->count; i++) { puts(response->rows[i]); free(response->rows[i]); }
    free(response->rows);
    fflush(stdout);
}

/* cost: time O(s + q + b), heap O(b), stack O(1), io O(s + p + b) */
/* vars: s = SQL 입력 바이트, q = 엔진 실행 비용, b = 결과 바이트, p = 접근 페이지 */
/* basis: estimate */
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { puts(sqlite3_libversion()); return 0; }
    if (argc != 3) return 2;
    engine_t engine = {.path = argv[2], .is_sqlite = strcmp(argv[1], "sqlite") == 0};
    int opened = open_engine(&engine, access(engine.path, F_OK) != 0);  /* 있는 파일은 다시 연다(복구 측정) */
    if (opened != 0) { fprintf(stderr, "open status %d\n", opened); return 2; }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, stdin)) >= 0) {
        if (size > 0 && line[size - 1] == '\n') line[size - 1] = '\0';
        response_t response = {0};
        uint64_t start = now_ns();
        if (strcmp(line, "@reopen") == 0) {
            response.error = close_engine(&engine);
            if (response.error == 0) response.error = open_engine(&engine, 0);
        } else if (engine.is_sqlite) execute_sqlite(&engine, line, &response);
        else execute_mini(&engine, line, &response);
        uint64_t elapsed = now_ns() - start;
        emit_response(&response, elapsed, engine.path);
    }
    free(line);
    if (ferror(stdin)) return 2;
    return close_engine(&engine) == 0 ? 0 : 2;
}
