/*
 * session_probe.c -- #103 차등 검사의 이 DB 쪽 어댑터. 표준 입력의 한 줄 명령을 받아 연결(session)을 유지한 채
 * 실행하고 결과를 줄 단위로 돌려준다. 프로토콜은 tests/sqlite_session_protocol.py와 같다.
 *
 * 명령: HELLO | OPEN 경로hex wal|nowal new|existing | SESSION 이름 | EXEC 이름 SQLhex 개수 값... | DROP 이름
 *       | CLOSE | QUIT. 값은 N, I10진수, R실수비트hex16, T바이트hex, B바이트hex.
 * 응답은 END 줄로 끝난다. SQL 오류 결과는 ERR, 프로브 자체의 실패는 FAULT다. 합성 데이터만 다룬다.
 *
 * 빌드: Makefile의 build/session_probe (session·database·database_txn과 저장 계층 전체)
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/session.h"

#define MAX_SESSIONS 8
#define MAX_NAME 16
#define POOL_BYTES ((size_t)1 << 30)

typedef struct {
    char name[MAX_NAME];
    session_t *session;
} named_session_t;

static session_pool_t *g_pool;
static session_db_t *g_db;
static named_session_t g_sessions[MAX_SESSIONS];

static const char *const ERROR_CLASS_NAMES[] = {"none",  "syntax", "semantic", "constraint", "limit",
                                                "nomem", "io",     "busy",     "cancelled",  "internal"};

static void reply_fault(const char *what, int first, int second)
{
    printf("FAULT %s %d %d\nEND\n", what, first, second);
}

static void put_hex(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) printf("%02x", bytes[i]);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* 소문자 hex 문자열을 바이트로 바꾼다. 홀수 길이나 hex가 아닌 글자는 거절한다 */
static bool hex_decode(const char *hex, uint8_t **out, size_t *size)
{
    size_t length = strlen(hex);
    if (length % 2 != 0) return false;
    uint8_t *bytes = malloc(length / 2 + 1);
    if (bytes == NULL) return false;
    for (size_t i = 0; i < length / 2; i++) {
        int high = hex_digit(hex[2 * i]), low = hex_digit(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            free(bytes);
            return false;
        }
        bytes[i] = (uint8_t)(high * 16 + low);
    }
    *out = bytes;
    *size = length / 2;
    return true;
}

static void put_cell(const value_view_t *cell)
{
    switch (cell->type) {
        case VALUE_NULL: putchar('N'); break;
        case VALUE_INTEGER: printf("I%lld", (long long)cell->integer); break;
        case VALUE_REAL: {
            uint64_t bits;
            memcpy(&bits, &cell->real, sizeof(bits));
            printf("R%016llx", (unsigned long long)bits);
            break;
        }
        case VALUE_TEXT:
            putchar('T');
            put_hex(cell->bytes, cell->size_bytes);
            break;
        case VALUE_BLOB:
            putchar('B');
            put_hex(cell->bytes, cell->size_bytes);
            break;
    }
}

static char type_letter(value_type_t type)
{
    static const char LETTERS[] = {'N', 'I', 'R', 'T', 'B'};
    return LETTERS[type];
}

static void emit_rows(result_t *result)
{
    size_t columns = result_column_count(result);
    printf("ROWS %zu\n", columns);
    for (size_t i = 0; i < columns; i++) {
        const result_column_t *column = result_column(result, i);
        printf("COL ");
        put_hex(column->name, column->name_len);
        printf(" %c\n", type_letter(column->type));
    }
    result_row_t row;
    result_next_t step;
    while ((step = result_next(result, &row)) == RESULT_NEXT_ROW) {
        printf("ROW");
        for (size_t i = 0; i < row.cell_count; i++) {
            putchar(' ');
            put_cell(&row.cells[i]);
        }
        putchar('\n');
    }
    if (step != RESULT_NEXT_DONE) {
        result_outcome_t outcome = result_outcome(result);
        printf("INCOMPLETE %d %llu\n", (int)outcome.state, (unsigned long long)outcome.rows_delivered);
    }
}

static void emit_result(result_t *result)
{
    switch (result_kind(result)) {
        case RESULT_KIND_ROWS: emit_rows(result); break;
        case RESULT_KIND_COMMAND: {
            const result_command_t *command = result_command(result);
            printf("CMD ");
            put_hex((const uint8_t *)command->tag, command->tag_len);
            printf(" %llu\n", (unsigned long long)command->affected_rows);
            break;
        }
        case RESULT_KIND_ERROR: {
            const result_error_t *error = result_outcome(result).error;
            printf("ERR %s %d ", ERROR_CLASS_NAMES[error->error_class], error->code);
            put_hex((const uint8_t *)error->message, error->message_len);
            putchar('\n');
            break;
        }
    }
}

static session_t *find_session(const char *name)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].session != NULL && strcmp(g_sessions[i].name, name) == 0) return g_sessions[i].session;
    }
    return NULL;
}

/* 값 토큰 하나를 value_view_t로 바꾼다. 바이트 값은 *owned에 할당해 돌려주고 호출자가 free한다 */
static bool decode_param(const char *token, value_view_t *view, uint8_t **owned)
{
    *view = (value_view_t){VALUE_NULL, 0, 0.0, NULL, 0};
    *owned = NULL;
    switch (token[0]) {
        case 'N': return token[1] == '\0';
        case 'I':
            view->type = VALUE_INTEGER;
            view->integer = strtoll(token + 1, NULL, 10);
            return true;
        case 'R': {
            uint64_t bits = strtoull(token + 1, NULL, 16);
            view->type = VALUE_REAL;
            memcpy(&view->real, &bits, sizeof(bits));
            return strlen(token + 1) == 16;
        }
        case 'T':
        case 'B':
            view->type = token[0] == 'T' ? VALUE_TEXT : VALUE_BLOB;
            if (!hex_decode(token + 1, owned, &view->size_bytes)) return false;
            view->bytes = *owned;
            return true;
        default: return false;
    }
}

static void free_params(uint8_t **owned, size_t count)
{
    for (size_t i = 0; i < count; i++) free(owned[i]);
    free(owned);
}

/* 남은 토큰에서 개수만큼 값을 읽는다. 실패하면 할당을 모두 돌려주고 false다 */
static bool decode_params(char **cursor, size_t count, value_view_t *views, uint8_t ***owned_out)
{
    uint8_t **owned = calloc(count + 1, sizeof(*owned));
    if (owned == NULL) return false;
    for (size_t i = 0; i < count; i++) {
        char *token = strtok_r(NULL, " ", cursor);
        if (token == NULL || !decode_param(token, &views[i], &owned[i])) {
            free_params(owned, count);
            return false;
        }
    }
    *owned_out = owned;
    return true;
}

// cost: time O(n) for sql and values, heap O(n), stack O(1), io 1
// vars: n = request bytes
// basis: estimate
static void command_exec(char *cursor)
{
    char *name = strtok_r(NULL, " ", &cursor);
    char *sql_hex = strtok_r(NULL, " ", &cursor);
    char *count_text = strtok_r(NULL, " ", &cursor);
    session_t *session = name == NULL ? NULL : find_session(name);
    uint8_t *sql = NULL;
    size_t sql_size = 0, count = count_text == NULL ? 0 : strtoul(count_text, NULL, 10);
    if (session == NULL || sql_hex == NULL || !hex_decode(sql_hex, &sql, &sql_size)) {
        reply_fault("exec arguments", 0, 0);
        return;
    }
    value_view_t *views = calloc(count + 1, sizeof(*views));
    uint8_t **owned = NULL;
    if (views == NULL || !decode_params(&cursor, count, views, &owned)) {
        free(views);
        free(sql);
        reply_fault("exec values", 0, 0);
        return;
    }
    lexer_binding_t binding = {views, count};
    result_t *result = NULL;
    database_exec_status_t status = session_exec(session, NULL, sql, sql_size, count > 0 ? &binding : NULL, &result);
    if (status == DATABASE_EXEC_OK && result != NULL) {
        emit_result(result);
        result_release(&result);
    } else {
        printf("EXECSTATUS %d\n", (int)status);
    }
    printf("TXN %d %d\nEND\n", session_txn_active(session) ? 1 : 0, session_txn_failed(session) ? 1 : 0);
    free_params(owned, count);
    free(views);
    free(sql);
}

static void command_open(char *cursor)
{
    char *path_hex = strtok_r(NULL, " ", &cursor);
    char *wal = strtok_r(NULL, " ", &cursor);
    char *fresh = strtok_r(NULL, " ", &cursor);
    uint8_t *path_bytes = NULL;
    size_t path_size = 0;
    if (g_pool != NULL || fresh == NULL || !hex_decode(path_hex, &path_bytes, &path_size)) {
        reply_fault("open arguments", 0, 0);
        return;
    }
    char *path = strndup((const char *)path_bytes, path_size);
    char *wal_path = NULL;
    bool use_wal = strcmp(wal, "wal") == 0, is_new = strcmp(fresh, "new") == 0;
    if (use_wal && asprintf(&wal_path, "%s.wal", path) < 0) wal_path = NULL;
    database_status_t db_status = DATABASE_OK;
    if (is_new) {
        database_t *created = NULL;
        db_status = database_create(path, &created);
        if (db_status == DATABASE_OK) db_status = database_close(created);
    }
    session_limits_t limits = {POOL_BYTES, POOL_BYTES, POOL_BYTES, MAX_SESSIONS, 0};
    session_status_t status = db_status == DATABASE_OK ? session_pool_open(&limits, &g_pool) : SESSION_INVALID;
    if (status == SESSION_OK) {
        database_txn_options_t options = {0};
        options.wal_path = wal_path;
        options.convert = use_wal && is_new;
        status = session_pool_open_transactional(g_pool, path, &options, &g_db, &db_status, NULL);
        if (status != SESSION_OK) {
            session_pool_close(g_pool);
            g_pool = NULL;
        }
    }
    if (status == SESSION_OK) printf("OK\nEND\n");
    else reply_fault("open", (int)status, (int)db_status);
    free(wal_path);
    free(path);
    free(path_bytes);
}

static void command_session(char *cursor)
{
    char *name = strtok_r(NULL, " ", &cursor);
    named_session_t *slot = NULL;
    for (size_t i = 0; i < MAX_SESSIONS && slot == NULL; i++) {
        if (g_sessions[i].session == NULL) slot = &g_sessions[i];
    }
    if (g_db == NULL || name == NULL || strlen(name) >= MAX_NAME || slot == NULL || find_session(name) != NULL) {
        reply_fault("session arguments", 0, 0);
        return;
    }
    session_status_t status = session_open(g_db, &slot->session);
    if (status != SESSION_OK) {
        slot->session = NULL;
        reply_fault("session", (int)status, 0);
        return;
    }
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    printf("OK\nEND\n");
}

static void command_drop(char *cursor)
{
    char *name = strtok_r(NULL, " ", &cursor);
    for (size_t i = 0; i < MAX_SESSIONS && name != NULL; i++) {
        if (g_sessions[i].session != NULL && strcmp(g_sessions[i].name, name) == 0) {
            session_status_t status = session_close(g_sessions[i].session);
            g_sessions[i].session = NULL;
            if (status == SESSION_OK) printf("OK\nEND\n");
            else reply_fault("drop", (int)status, 0);
            return;
        }
    }
    reply_fault("drop arguments", 0, 0);
}

static void command_close(void)
{
    session_status_t status = SESSION_OK;
    database_status_t db_status = DATABASE_OK;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].session != NULL) status = session_close(g_sessions[i].session);
        g_sessions[i].session = NULL;
    }
    if (g_db != NULL) status = status == SESSION_OK ? session_pool_close_database(g_db, &db_status) : status;
    g_db = NULL;
    if (g_pool != NULL && status == SESSION_OK) status = session_pool_close(g_pool);
    g_pool = NULL;
    if (status == SESSION_OK && db_status == DATABASE_OK) printf("OK\nEND\n");
    else reply_fault("close", (int)status, (int)db_status);
}

static bool dispatch(char *line)
{
    char *cursor = NULL;
    char *verb = strtok_r(line, " ", &cursor);
    if (verb == NULL) return true;
    if (strcmp(verb, "HELLO") == 0) printf("HELLO session_probe 1\nEND\n");
    else if (strcmp(verb, "OPEN") == 0) command_open(cursor);
    else if (strcmp(verb, "SESSION") == 0) command_session(cursor);
    else if (strcmp(verb, "EXEC") == 0) command_exec(cursor);
    else if (strcmp(verb, "DROP") == 0) command_drop(cursor);
    else if (strcmp(verb, "CLOSE") == 0) command_close();
    else if (strcmp(verb, "QUIT") == 0) return false;
    else reply_fault("unknown command", 0, 0);
    fflush(stdout);
    return true;
}

// cost: time O(n) per request, heap O(n), stack O(1), io 1 read + 1 write per request
// vars: n = request bytes
// basis: estimate
int main(void)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, stdin)) >= 0) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        if (!dispatch(line)) break;
    }
    free(line);
    printf("OK\nEND\n");
    return 0;
}
