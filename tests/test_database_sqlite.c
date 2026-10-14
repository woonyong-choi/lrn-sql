#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql/database.h"
#include "sql/result.h"

/*
 * 같은 SQL 입력을 database와 sqlite3 CLI에 주고 CRUD 결과와 재시작 뒤 결과를 대조한다.
 * 이 저장소에는 sqlite3 헤더가 없어 CLI(.mode quote)의 출력을 읽는다. CLI가 없으면 통과가 아니라
 * SKIPPED로 끝난다(종료 코드 77).
 *
 * 비교 범위는 두 쪽이 같은 의미를 갖는 문장뿐이다. 다른 점(통과 범위가 아님): sqlite는 타입 친화성으로
 * 'abc'를 INTEGER 컬럼에 그대로 넣고 여기서는 오류다. 행 순서는 SELECT에 ORDER BY가 없으므로 정렬해서 비교한다.
 * 실수는 sqlite CLI가 15자리로 출력하므로 15자리로 맞춰 비교한다.
 */

#define DIR_NAME "build/test_database_sqlite_dir"
#define DB_PATH DIR_NAME "/db.lrdb"
#define SCRIPT_PATH DIR_NAME "/script.sql"

typedef enum { STEP_OK, STEP_ERROR, STEP_SELECT, STEP_REOPEN } step_kind_t;

typedef struct {
    step_kind_t kind;
    const char *sql;
} step_t;

static char g_big_literal[9400];
static char g_bigger_literal[30000];

static const step_t STEPS[] = {
    {STEP_OK, "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(5) NOT NULL, score REAL, bio TEXT, avatar BLOB)"},
    {STEP_OK, "CREATE TABLE logs (ts INTEGER, msg TEXT NOT NULL)"},
    {STEP_ERROR, "CREATE TABLE logs (x INTEGER)"},
    {STEP_OK, "INSERT INTO users (name, score) VALUES ('alice', 1.5)"},
    {STEP_OK, "INSERT INTO users (name, score, bio) VALUES ('bobbybobbybobby', 2, 'hello')"},
    {STEP_OK, "INSERT INTO users (name, avatar) VALUES ('carol', X'00ff10')"},
    {STEP_OK, "INSERT INTO users (name, bio) VALUES ('dave', '')"},
    {STEP_OK, "INSERT INTO users (name, bio) VALUES ('eve', 'h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80 ''quoted''')"},
    {STEP_OK, "INSERT INTO logs VALUES (1, 'boot')"},
    {STEP_OK, "INSERT INTO logs VALUES (NULL, 'no timestamp')"},
    {STEP_OK, "INSERT INTO logs (msg) VALUES ('only msg')"},
    {STEP_ERROR, "INSERT INTO users (score) VALUES (1.0)"},
    {STEP_ERROR, "INSERT INTO users (name) VALUES (NULL)"},
    {STEP_ERROR, "INSERT INTO nothing VALUES (1)"},
    {STEP_ERROR, "INSERT INTO users (id, name) VALUES (1, 'dup')"},
    {STEP_SELECT, "SELECT * FROM users"},
    {STEP_SELECT, "SELECT * FROM logs"},
    {STEP_REOPEN, NULL},
    {STEP_SELECT, "SELECT * FROM users"},
    {STEP_SELECT, "SELECT name, bio FROM users WHERE bio IS NULL"},
    {STEP_SELECT, "SELECT name FROM users WHERE bio IS NOT NULL"},
    {STEP_SELECT, "SELECT name FROM users WHERE score >= 1.5"},
    {STEP_SELECT, "SELECT name FROM users WHERE score > 1"},
    {STEP_SELECT, "SELECT name FROM users WHERE name <> 'alice'"},
    {STEP_SELECT, "SELECT name FROM users WHERE name = NULL"},
    {STEP_SELECT, "SELECT msg FROM logs WHERE ts < 5"},
    {STEP_SELECT, "SELECT id, name FROM users WHERE id >= 3"},
    {STEP_OK, "UPDATE users SET score = 9.25, bio = 'updated' WHERE id = 1"},
    {STEP_OK, "UPDATE users SET bio = NULL WHERE name = 'carol'"},
    {STEP_OK, "UPDATE logs SET msg = 'changed' WHERE ts IS NULL"},
    {STEP_ERROR, "UPDATE users SET name = NULL WHERE id = 1"},
    {STEP_ERROR, "UPDATE users SET id = 2 WHERE id = 1"},
    {STEP_OK, "DELETE FROM users WHERE id = 4"},
    {STEP_OK, "DELETE FROM logs WHERE ts = 1"},
    {STEP_OK, "DELETE FROM logs WHERE ts = 12345"},
    {STEP_SELECT, "SELECT * FROM users"},
    {STEP_SELECT, "SELECT * FROM logs"},
    {STEP_OK, "INSERT INTO users (name) VALUES ('after delete')"},
    {STEP_OK, "INSERT INTO users (id, name) VALUES (40, 'explicit')"},
    {STEP_OK, "INSERT INTO users (name) VALUES ('after explicit')"},
    {STEP_OK, "UPDATE users SET id = 99 WHERE id = 40"},
    {STEP_OK, "INSERT INTO users (name) VALUES ('after update')"},
    {STEP_SELECT, "SELECT id, name FROM users"},
    {STEP_REOPEN, NULL},
    {STEP_OK, NULL},  /* 큰 값 삽입: 아래에서 채운다(첫 번째 9000바이트대, 두 번째 30000바이트) */
    {STEP_OK, NULL},
    {STEP_SELECT, "SELECT id, bio FROM users WHERE name = 'big'"},
    {STEP_SELECT, "SELECT id, bio FROM users WHERE name = 'bigger'"},
    {STEP_OK, "DROP TABLE logs"},
    {STEP_ERROR, "SELECT * FROM logs"},
    {STEP_SELECT, "SELECT * FROM users"},
    {STEP_REOPEN, NULL},
    {STEP_SELECT, "SELECT * FROM users"},
    {STEP_OK, "CREATE TABLE logs (q REAL, r TEXT)"},
    {STEP_OK, "INSERT INTO logs VALUES (0.5, 'new schema')"},
    {STEP_SELECT, "SELECT * FROM logs"},
};

#define FIXED_STEP_COUNT (sizeof(STEPS) / sizeof(STEPS[0]))

/* 고정 단계 뒤에 기본 키 색인용으로 만든 단계를 붙인 전체 목록 */
static step_t *g_steps;
static size_t g_step_count;

/* ── 표준 표기 ── */
typedef struct {
    char *text;
    size_t len, cap;
} buffer_t;

static void append(buffer_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->text = realloc(b->text, b->cap);
        assert(b->text != NULL);
    }
    memcpy(b->text + b->len, s, n);
    b->len += n;
    b->text[b->len] = '\0';
}

static void append_str(buffer_t *b, const char *s) { append(b, s, strlen(s)); }

static void append_hex(buffer_t *b, const uint8_t *bytes, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char two[2] = {digits[bytes[i] >> 4], digits[bytes[i] & 15]};
        append(b, two, 2);
    }
}

static void canon_cell(buffer_t *line, const value_view_t *cell)
{
    char text[64];
    switch (cell->type) {
        case VALUE_NULL: append_str(line, "NULL"); break;
        case VALUE_INTEGER:
            snprintf(text, sizeof(text), "I:%lld", (long long)cell->integer);
            append_str(line, text);
            break;
        case VALUE_REAL:
            snprintf(text, sizeof(text), "R:%.15g", cell->real);
            append_str(line, text);
            break;
        case VALUE_TEXT:
            append_str(line, "T:");
            append_hex(line, cell->bytes, cell->size_bytes);
            break;
        default:
            append_str(line, "B:");
            append_hex(line, cell->bytes, cell->size_bytes);
            break;
    }
}

static int compare_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *join_sorted(char **lines, size_t count)
{
    if (count > 1) qsort(lines, count, sizeof(*lines), compare_lines);
    buffer_t out = {NULL, 0, 0};
    char head[32];
    snprintf(head, sizeof(head), "[%zu]", count);
    append_str(&out, head);
    for (size_t i = 0; i < count; i++) {
        append_str(&out, "\n");
        append_str(&out, lines[i]);
        free(lines[i]);
    }
    free(lines);
    return out.text;
}

/* database 쪽 SELECT 결과 */
static char *ours_rows(result_t *result)
{
    char **lines = NULL;
    size_t count = 0, cap = 0;
    result_row_t row;
    while (result_next(result, &row) == RESULT_NEXT_ROW) {
        buffer_t line = {NULL, 0, 0};
        append_str(&line, "");
        for (size_t i = 0; i < row.cell_count; i++) {
            if (i > 0) append_str(&line, "|");
            canon_cell(&line, &row.cells[i]);
        }
        if (count == cap) {
            cap = cap == 0 ? 16 : cap * 2;
            lines = realloc(lines, cap * sizeof(*lines));
            assert(lines != NULL);
        }
        lines[count++] = line.text;
    }
    return join_sorted(lines, count);
}

/* sqlite .mode quote 한 줄을 표준 표기로 */
static char *sqlite_line(const char *s)
{
    buffer_t line = {NULL, 0, 0};
    append_str(&line, "");
    bool first = true;
    while (*s != '\0') {
        if (!first) append_str(&line, "|");
        first = false;
        if (*s == '\'') {
            buffer_t bytes = {NULL, 0, 0};
            append_str(&bytes, "");
            s++;
            for (;;) {
                if (*s == '\'' && s[1] == '\'') {
                    append(&bytes, "'", 1);
                    s += 2;
                } else if (*s == '\'') {
                    s++;
                    break;
                } else {
                    append(&bytes, s, 1);
                    s++;
                }
            }
            append_str(&line, "T:");
            append_hex(&line, (const uint8_t *)bytes.text, bytes.len);
            free(bytes.text);
        } else if ((s[0] == 'X' || s[0] == 'x') && s[1] == '\'') {
            s += 2;
            append_str(&line, "B:");
            while (*s != '\'') {
                char c = (char)tolower((unsigned char)*s);
                append(&line, &c, 1);
                s++;
            }
            s++;
        } else {
            const char *end = strchr(s, ',');
            size_t n = end != NULL ? (size_t)(end - s) : strlen(s);
            char token[64];
            assert(n < sizeof(token));
            memcpy(token, s, n);
            token[n] = '\0';
            if (strcmp(token, "NULL") == 0) {
                append_str(&line, "NULL");
            } else if (strpbrk(token, ".eE") != NULL) {
                char out[64];
                snprintf(out, sizeof(out), "R:%.15g", strtod(token, NULL));
                append_str(&line, out);
            } else {
                /* "I:" + token(최대 sizeof(token)-1바이트) + NUL이 모두 들어가는 크기 */
                char out[2 + sizeof(token)];
                snprintf(out, sizeof(out), "I:%s", token);
                append_str(&line, out);
            }
            s += n;
        }
        if (*s == ',') s++;
    }
    return line.text;
}

/* ── 파일 보조 ── */
static void clean_dir(void)
{
    DIR *dir = opendir(DIR_NAME);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", DIR_NAME, entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(DIR_NAME);
}

static int g_run = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_run++;                                          \
        if (!(cond)) {                                    \
            g_fail++;                                     \
            printf("  FAIL (line %d): ", __LINE__);       \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* sql이 NULL인 STEP_OK는 큰 값 삽입이다. 앞에서부터 순서대로 a, b를 쓴다. */
static const char *step_sql(size_t i, char *storage_a, char *storage_b)
{
    if (g_steps[i].sql != NULL) return g_steps[i].sql;
    size_t ordinal = 0;
    for (size_t k = 0; k < i; k++) {
        if (g_steps[k].kind == STEP_OK && g_steps[k].sql == NULL) ordinal++;
    }
    return ordinal == 0 ? storage_a : storage_b;
}

/*
 * 기본 키 색인 단계. 키 -1000..1000에 대한 삽입(중복 포함)·조회·값 갱신·키 갱신·삭제를 정해진 난수열로 섞는다.
 * 성공·실패 기대는 두 엔진이 아니라 이 모델(key -> 존재 여부)에서 얻는다. 키가 한 leaf(508개)를 넘게 쌓여
 * 색인이 쪼개지고, 중간에 다시 열어 재시작 뒤 색인을 읽는다.
 */
#define KEY_LOW (-1000)
#define KEY_SPAN 2001
#define GENERATED_INSERTS 1100
#define GENERATED_OPERATIONS 900

static unsigned g_seed = 20260715u;

static unsigned next_random(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static void add_step(step_kind_t kind, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void add_step(step_kind_t kind, const char *format, ...)
{
    char *sql = NULL;
    if (format != NULL) {
        sql = malloc(160);
        assert(sql != NULL);
        va_list args;
        va_start(args, format);
        vsnprintf(sql, 160, format, args);
        va_end(args);
    }
    g_steps = realloc(g_steps, (g_step_count + 1) * sizeof(*g_steps));
    assert(g_steps != NULL);
    g_steps[g_step_count++] = (step_t){kind, sql};
}

static void build_steps(void)
{
    g_steps = malloc(FIXED_STEP_COUNT * sizeof(*g_steps));
    assert(g_steps != NULL);
    memcpy(g_steps, STEPS, sizeof(STEPS));
    g_step_count = FIXED_STEP_COUNT;

    bool exists[KEY_SPAN] = {false};
    add_step(STEP_OK, "CREATE TABLE kv (k INTEGER PRIMARY KEY, v TEXT NOT NULL)");
    for (int i = 0; i < GENERATED_INSERTS; i++) {
        int key = KEY_LOW + (int)(next_random() % KEY_SPAN);
        add_step(exists[key - KEY_LOW] ? STEP_ERROR : STEP_OK, "INSERT INTO kv VALUES (%d, 'v%d')", key, i);
        exists[key - KEY_LOW] = true;
    }
    add_step(STEP_SELECT, "SELECT * FROM kv");
    add_step(STEP_REOPEN, NULL);
    add_step(STEP_SELECT, "SELECT * FROM kv");
    for (int i = 0; i < GENERATED_OPERATIONS; i++) {
        int key = KEY_LOW + (int)(next_random() % KEY_SPAN);
        int other = KEY_LOW + (int)(next_random() % KEY_SPAN);
        bool has_key = exists[key - KEY_LOW];
        switch (next_random() % 5) {
            case 0:
                add_step(STEP_SELECT, "SELECT k, v FROM kv WHERE k = %d", key);
                break;
            case 1:
                add_step(has_key ? STEP_ERROR : STEP_OK, "INSERT INTO kv VALUES (%d, 'again%d')", key, i);
                exists[key - KEY_LOW] = true;
                break;
            case 2:
                add_step(STEP_OK, "UPDATE kv SET v = 'u%d' WHERE k = %d", i, key);
                break;
            case 3: {
                bool is_conflict = has_key && other != key && exists[other - KEY_LOW];
                add_step(is_conflict ? STEP_ERROR : STEP_OK, "UPDATE kv SET k = %d WHERE k = %d", other, key);
                if (has_key && !is_conflict) {
                    exists[key - KEY_LOW] = key == other;
                    exists[other - KEY_LOW] = true;
                }
                break;
            }
            default:
                add_step(STEP_OK, "DELETE FROM kv WHERE k = %d", key);
                exists[key - KEY_LOW] = false;
                break;
        }
        if (i % 300 == 299) {
            add_step(STEP_REOPEN, NULL);
            add_step(STEP_SELECT, "SELECT * FROM kv");
        }
    }
    for (int key = KEY_LOW; key < KEY_LOW + KEY_SPAN; key += 7) add_step(STEP_SELECT, "SELECT k, v FROM kv WHERE k = %d", key);
}

int main(void)
{
    if (system("sqlite3 --version > /dev/null 2>&1") != 0) {
        printf("SKIPPED: sqlite3 CLI가 없어 비교하지 않았다\n");
        return 77;
    }
    build_steps();
    memset(g_big_literal, 'b', sizeof(g_big_literal) - 1);
    memset(g_bigger_literal, 'z', sizeof(g_bigger_literal) - 1);
    for (size_t i = 0; i < sizeof(g_big_literal) - 1; i += 97) g_big_literal[i] = (char)('A' + i % 26);
    char *step44 = malloc(sizeof(g_big_literal) + 64);
    char *step45 = malloc(sizeof(g_bigger_literal) + 64);
    assert(step44 != NULL && step45 != NULL);
    snprintf(step44, sizeof(g_big_literal) + 64, "INSERT INTO users (name, bio) VALUES ('big', '%s')", g_big_literal);
    snprintf(step45, sizeof(g_bigger_literal) + 64, "INSERT INTO users (name, bio) VALUES ('bigger', '%s')", g_bigger_literal);

    clean_dir();
    assert(mkdir(DIR_NAME, 0700) == 0);

    /* sqlite3에 줄 스크립트: 각 문장 뒤에 표지 */
    FILE *script = fopen(SCRIPT_PATH, "wb");
    assert(script != NULL);
    fprintf(script, ".mode quote\n.headers off\n");
    for (size_t i = 0; i < g_step_count; i++) {
        if (g_steps[i].kind == STEP_REOPEN) continue;
        fprintf(script, "%s;\nSELECT '@@%zu';\n", step_sql(i, step44, step45), i);
    }
    fclose(script);
    FILE *pipe = popen("sqlite3 -batch :memory: < " SCRIPT_PATH " 2>&1", "r");
    assert(pipe != NULL);

    /* 출력을 표지 단위로 나눈다 */
    bool *errored = calloc(g_step_count, sizeof(*errored));
    char **rows = calloc(g_step_count, sizeof(*rows));
    assert(errored != NULL && rows != NULL);
    size_t cap = 0;
    char *line = NULL;
    ssize_t got;
    char **pending = NULL;
    size_t pending_count = 0, pending_cap = 0;
    bool pending_error = false;
    while ((got = getline(&line, &cap, pipe)) >= 0) {
        while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) line[--got] = '\0';
        if (strncmp(line, "'@@", 3) == 0) {
            size_t index = (size_t)strtoul(line + 3, NULL, 10);
            errored[index] = pending_error;
            rows[index] = join_sorted(pending, pending_count);
            pending = NULL;
            pending_count = pending_cap = 0;
            pending_error = false;
            continue;
        }
        if (strncmp(line, "Error", 5) == 0 || strncmp(line, "Parse error", 11) == 0 ||
            strncmp(line, "Runtime error", 13) == 0) {
            pending_error = true;
            continue;
        }
        if (pending_count == pending_cap) {
            pending_cap = pending_cap == 0 ? 16 : pending_cap * 2;
            pending = realloc(pending, pending_cap * sizeof(*pending));
            assert(pending != NULL);
        }
        pending[pending_count++] = sqlite_line(line);
    }
    free(line);
    pclose(pipe);

    database_t *db;
    assert(database_create(DB_PATH, &db) == DATABASE_OK);
    size_t selects = 0, compared_rows = 0;
    for (size_t i = 0; i < g_step_count; i++) {
        if (g_steps[i].kind == STEP_REOPEN) {
            assert(database_close(db) == DATABASE_OK);
            assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
            continue;
        }
        const char *sql = step_sql(i, step44, step45);
        result_t *result = NULL;
        database_exec_status_t status = database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result);
        assert(status == DATABASE_EXEC_OK && result != NULL);
        bool ours_error = result_kind(result) == RESULT_KIND_ERROR;
        CHECK(ours_error == errored[i], "단계 %zu: %.60s -> 이쪽 오류 %d, sqlite 오류 %d", i, sql, ours_error, errored[i]);
        if (g_steps[i].kind == STEP_ERROR) CHECK(ours_error && errored[i], "단계 %zu: 둘 다 오류여야 함", i);
        if (g_steps[i].kind == STEP_OK) CHECK(!ours_error && !errored[i], "단계 %zu: 둘 다 성공이어야 함: %.60s", i, sql);
        if (g_steps[i].kind == STEP_SELECT && !ours_error) {
            selects++;
            char *mine = ours_rows(result);
            CHECK(strcmp(mine, rows[i]) == 0, "단계 %zu: %s\n--- 이쪽\n%.600s\n--- sqlite\n%.600s", i, sql, mine, rows[i]);
            compared_rows += (size_t)atoi(mine + 1);
            free(mine);
        }
        result_release(&result);
    }
    assert(database_close(db) == DATABASE_OK);
    for (size_t i = 0; i < g_step_count; i++) free(rows[i]);
    free(rows);
    free(errored);
    free(step44);
    free(step45);
    for (size_t i = FIXED_STEP_COUNT; i < g_step_count; i++) free((char *)g_steps[i].sql);
    free(g_steps);
    clean_dir();
    printf("sqlite 비교: SELECT %zu개, 비교한 행 %zu개\n", selects, compared_rows);
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
