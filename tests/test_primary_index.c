/*
 * test_primary_index.c -- 기본 키 색인(#158) database 수준 검사
 *
 * 다루는 것: 기본 키 등가 조회·중복 검사·갱신·삭제가 전체 스캔 없이 같은 결과를 내는지(스캔 행 수 계수기),
 * 행이 옮겨지거나 지워진 뒤와 재시작 뒤에 낡은 locator가 없는지, 색인이 없거나 heap과 어긋난 표가 스캔으로
 * 돌아가는지, 색인이 가리키는 행이 틀리면 엉뚱한 행 대신 오류를 내는지, DROP이 색인 페이지를 수집하는지.
 * 다루지 않는 것: SQLite 동일 SQL 차등(test_database_sqlite), 실패 주입(test_database_fault), 거래 안
 * 동작(test_session_txn).
 *
 * 합성 데이터만 쓴다. 기대 결과는 검사 안의 모델(배열)에서 얻는다.
 */

#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql/database.h"
#include "sql/result.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/page_file.h"
#include "storage/pk_tree.h"

#define DIR_NAME "build/test_primary_index_dir"
#define DB_PATH DIR_NAME "/db.lrdb"

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

static database_t *create_db(void)
{
    clean_dir();
    assert(mkdir(DIR_NAME, 0700) == 0);
    database_t *db;
    assert(database_create(DB_PATH, &db) == DATABASE_OK);
    return db;
}

static database_t *reopen(database_t *db)
{
    assert(database_close(db) == DATABASE_OK);
    assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
    return db;
}

/* ── 실행·결과 ── */

typedef struct {
    int kind;       /* result_kind_t */
    int error_code; /* 오류 결과일 때 code */
    char text[512]; /* 행은 "id|text/ ..." 순서 그대로 */
    size_t rows;
} outcome_t;

static outcome_t run(database_t *db, const char *sql)
{
    outcome_t out = {0, 0, "", 0};
    result_t *result = NULL;
    database_exec_status_t status = database_exec(db, (const uint8_t *)sql, strlen(sql), NULL, &result);
    assert(status == DATABASE_EXEC_OK && result != NULL);
    out.kind = (int)result_kind(result);
    if (result_kind(result) == RESULT_KIND_ERROR) {
        out.error_code = result_outcome(result).error->code;
    } else if (result_kind(result) == RESULT_KIND_ROWS) {
        result_row_t row;
        size_t used = 0;
        while (result_next(result, &row) == RESULT_NEXT_ROW) {
            for (size_t i = 0; i < row.cell_count; i++) {
                const value_view_t *cell = &row.cells[i];
                int n = 0;
                if (cell->type == VALUE_INTEGER) {
                    n = snprintf(out.text + used, sizeof(out.text) - used, "%s%lld", i > 0 ? "|" : "",
                                 (long long)cell->integer);
                } else if (cell->type == VALUE_TEXT) {
                    n = snprintf(out.text + used, sizeof(out.text) - used, "%s%.*s", i > 0 ? "|" : "",
                                 (int)(cell->size_bytes < 20 ? cell->size_bytes : 20), cell->bytes);
                } else {
                    n = snprintf(out.text + used, sizeof(out.text) - used, "%s?", i > 0 ? "|" : "");
                }
                assert(n >= 0 && (size_t)n < sizeof(out.text) - used);
                used += (size_t)n;
            }
            out.rows++;
            if (used + 3 < sizeof(out.text)) used += (size_t)snprintf(out.text + used, sizeof(out.text) - used, "/");
        }
    }
    result_release(&result);
    return out;
}

static void ok(database_t *db, const char *sql)
{
    outcome_t out = run(db, sql);
    CHECK(out.kind == RESULT_KIND_COMMAND, "%s -> kind %d code %d", sql, out.kind, out.error_code);
}

static database_scan_stats_t stats_of(database_t *db)
{
    database_scan_stats_t stats;
    database_scan_stats(db, &stats);
    return stats;
}

#define DUPLICATE_KEY_CODE 11

/* ── 1. 스캔 없이 같은 결과 ── */

static void test_key_statements_do_not_scan(void)
{
    printf("\n[test_key_statements_do_not_scan]\n");
    database_t *db = create_db();
    ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, score INTEGER)");
    for (int id = 10; id <= 2000; id += 10) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'n%d', %d)", id, id, id * 2);
        ok(db, sql);
    }
    database_scan_stats_t before = stats_of(db);
    CHECK(before.rows_scanned == 0, "명시한 기본 키 삽입 200건이 한 행도 스캔하지 않음 (%llu)",
          (unsigned long long)before.rows_scanned);

    outcome_t hit = run(db, "SELECT id, name, score FROM t WHERE id = 1230");
    CHECK(hit.rows == 1 && strcmp(hit.text, "1230|n1230|2460/") == 0, "있는 키 조회: %s", hit.text);
    outcome_t miss = run(db, "SELECT * FROM t WHERE id = 1235");
    CHECK(miss.kind == RESULT_KIND_ROWS && miss.rows == 0, "없는 키 조회는 빈 결과");
    outcome_t dup = run(db, "INSERT INTO t VALUES (1230, 'again', 0)");
    CHECK(dup.kind == RESULT_KIND_ERROR && dup.error_code == DUPLICATE_KEY_CODE, "중복 삽입은 제약 오류 (%d)", dup.error_code);
    ok(db, "UPDATE t SET score = 7 WHERE id = 1230");
    outcome_t moved_key = run(db, "UPDATE t SET id = 1240 WHERE id = 1230");
    CHECK(moved_key.kind == RESULT_KIND_ERROR && moved_key.error_code == DUPLICATE_KEY_CODE, "이미 있는 키로 바꾸면 거절");
    ok(db, "UPDATE t SET id = 1235 WHERE id = 1230");
    ok(db, "DELETE FROM t WHERE id = 1240");
    ok(db, "DELETE FROM t WHERE id = 999999");
    ok(db, "INSERT INTO t (name) VALUES ('auto')");
    database_scan_stats_t after = stats_of(db);
    CHECK(after.rows_scanned == before.rows_scanned, "기본 키 문장들이 한 행도 스캔하지 않음 (%llu)",
          (unsigned long long)(after.rows_scanned - before.rows_scanned));
    CHECK(after.index_lookups > before.index_lookups && after.index_pages_read > before.index_pages_read,
          "색인 조회가 계수됨");

    CHECK(run(db, "SELECT id FROM t WHERE id = 1230").rows == 0, "바꾼 옛 키는 없음");
    CHECK(strcmp(run(db, "SELECT id, score FROM t WHERE id = 1235").text, "1235|7/") == 0, "바꾼 새 키가 같은 행");
    CHECK(run(db, "SELECT id FROM t WHERE id = 1240").rows == 0, "지운 키는 없음");
    CHECK(strcmp(run(db, "SELECT id, name FROM t WHERE id = 2001").text, "2001|auto/") == 0, "자동 키는 기존 최대 키 + 1");

    database_scan_stats_t control = stats_of(db);
    outcome_t scan = run(db, "SELECT id FROM t WHERE name = 'n500'");
    CHECK(scan.rows == 1 && stats_of(db).rows_scanned - control.rows_scanned == 200,
          "기본 키가 아닌 조건은 전체를 스캔함: 계수기 양성 대조 (%llu)",
          (unsigned long long)(stats_of(db).rows_scanned - control.rows_scanned));

    db = reopen(db);
    database_scan_stats_t reopened = stats_of(db);
    CHECK(strcmp(run(db, "SELECT id, score FROM t WHERE id = 1235").text, "1235|7/") == 0, "재시작 뒤 조회");
    CHECK(run(db, "SELECT id FROM t WHERE id = 1230").rows == 0 && run(db, "SELECT id FROM t WHERE id = 1240").rows == 0,
          "재시작 뒤에도 없는 키는 없음");
    CHECK(run(db, "INSERT INTO t VALUES (1235, 'x', 0)").error_code == DUPLICATE_KEY_CODE, "재시작 뒤 중복 검사");
    CHECK(stats_of(db).rows_scanned == reopened.rows_scanned, "재시작 뒤에도 스캔하지 않음");
    CHECK(strcmp(run(db, "SELECT id, name FROM t WHERE id = 2002").text, "") == 0, "아직 없는 자동 키");
    ok(db, "INSERT INTO t (name) VALUES ('after restart')");
    CHECK(strcmp(run(db, "SELECT id, name FROM t WHERE id = 2002").text, "2002|after restart/") == 0, "재시작 뒤 자동 키가 이어짐");
    assert(database_close(db) == DATABASE_OK);
}

/* ── 2. 행이 옮겨지거나 지워진 뒤 낡은 locator가 없다 ── */

#define MODEL_ROWS 40
typedef struct {
    bool is_live;
    size_t bio_size;
} model_row_t;

static void check_model(database_t *db, const model_row_t *model, const char *label)
{
    size_t live = 0;
    for (int id = 0; id < MODEL_ROWS + 5; id++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "SELECT id, bio FROM t WHERE id = %d", id);
        outcome_t out = run(db, sql);
        bool expected = id < MODEL_ROWS && model[id].is_live;
        live += expected;
        CHECK(out.rows == (expected ? 1u : 0u), "%s: id %d 조회 행 수 %zu", label, id, out.rows);
        if (expected) {
            char want[32];
            snprintf(want, sizeof(want), "%d|", id);
            CHECK(strncmp(out.text, want, strlen(want)) == 0 && out.text[strlen(want)] == (char)('a' + id % 26),
                  "%s: id %d의 본문이 자기 것 (%s)", label, id, out.text);
        }
    }
    outcome_t all = run(db, "SELECT id FROM t");
    CHECK(all.rows == live, "%s: 스캔한 행 수 %zu와 색인으로 찾은 수 %zu가 같음", label, all.rows, live);
}

static void exec_row(database_t *db, int id, size_t bio_size, bool update)
{
    char *sql = malloc(bio_size + 128);
    char *bio = malloc(bio_size + 1);
    assert(sql != NULL && bio != NULL);
    memset(bio, 'a' + id % 26, bio_size);
    bio[bio_size] = '\0';
    if (update) snprintf(sql, bio_size + 128, "UPDATE t SET bio = '%s' WHERE id = %d", bio, id);
    else snprintf(sql, bio_size + 128, "INSERT INTO t VALUES (%d, '%s')", id, bio);
    ok(db, sql);
    free(bio);
    free(sql);
}

static void test_moved_and_deleted_rows_have_no_stale_locator(void)
{
    printf("\n[test_moved_and_deleted_rows_have_no_stale_locator]\n");
    database_t *db = create_db();
    ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, bio TEXT)");
    model_row_t model[MODEL_ROWS] = {{false, 0}};
    for (int id = 0; id < MODEL_ROWS; id++) {
        exec_row(db, id, 150, false);
        model[id] = (model_row_t){true, 150};
    }
    check_model(db, model, "초기");

    /* 한 페이지에 가득 찬 행을 키우면 다음 페이지로 옮겨진다 */
    for (int id = 3; id < MODEL_ROWS; id += 7) {
        exec_row(db, id, 3000, true);
        model[id].bio_size = 3000;
    }
    check_model(db, model, "옮긴 뒤");
    for (int id = 1; id < MODEL_ROWS; id += 5) {
        char sql[64];
        snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id = %d", id);
        ok(db, sql);
        model[id].is_live = false;
    }
    check_model(db, model, "지운 뒤");
    for (int id = 1; id < MODEL_ROWS; id += 10) {
        exec_row(db, id, 150, false);
        model[id] = (model_row_t){true, 150};
    }
    check_model(db, model, "지운 키를 다시 넣은 뒤");
    outcome_t again = run(db, "INSERT INTO t VALUES (3, 'dup')");
    CHECK(again.error_code == DUPLICATE_KEY_CODE, "옮겨진 행의 키도 중복 검사에 걸림");
    db = reopen(db);
    check_model(db, model, "재시작 뒤");
    assert(database_close(db) == DATABASE_OK);
}

/* ── 3. 색인이 없거나 어긋난 표는 스캔으로 ── */

static void rewrite_catalog(const char *table, uint32_t (*pick)(const catalog_table_view_t *view),
                            catalog_t **loaded_out, page_file_t **file_out, catalog_table_id_t *id_out)
{
    page_file_t *file;
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_store_load(file, &catalog) == CATALOG_STORE_OK);
    catalog_table_id_t id;
    assert(catalog_find(catalog, table, strlen(table), &id) == CATALOG_OK);
    catalog_table_view_t view;
    assert(catalog_describe(catalog, id, &view) == CATALOG_OK);
    assert(catalog_set_locators(catalog, id, view.heap_root, pick(&view)) == CATALOG_OK);
    *loaded_out = catalog;
    *file_out = file;
    *id_out = id;
}

static uint32_t no_index(const catalog_table_view_t *view)
{
    (void)view;
    return CATALOG_NO_PAGE;
}

static void test_table_without_index_scans_and_stays_correct(void)
{
    printf("\n[test_table_without_index_scans_and_stays_correct]\n");
    database_t *db = create_db();
    ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    for (int id = 1; id <= 50; id++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'v%d')", id, id);
        ok(db, sql);
    }
    assert(database_close(db) == DATABASE_OK);

    /* 색인을 모르는 옛 파일: 행은 있는데 primary_root가 없다 */
    catalog_t *catalog;
    page_file_t *file;
    catalog_table_id_t id;
    rewrite_catalog("t", no_index, &catalog, &file, &id);
    assert(catalog_store_save(file, catalog) == CATALOG_STORE_OK);
    catalog_close(catalog);
    assert(page_file_close(file) == PAGE_FILE_OK);

    assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
    CHECK(strcmp(run(db, "SELECT id, v FROM t WHERE id = 33").text, "33|v33/") == 0, "색인이 없어도 조회가 맞음");
    CHECK(stats_of(db).rows_scanned >= 33, "색인이 없으면 스캔함: 계수기 양성 대조 (%llu)", (unsigned long long)stats_of(db).rows_scanned);
    CHECK(run(db, "INSERT INTO t VALUES (33, 'dup')").error_code == DUPLICATE_KEY_CODE, "색인이 없어도 중복 검사가 맞음");
    ok(db, "INSERT INTO t VALUES (51, 'new')");
    ok(db, "UPDATE t SET id = 60 WHERE id = 51");
    ok(db, "DELETE FROM t WHERE id = 33");
    CHECK(run(db, "SELECT id FROM t WHERE id = 60").rows == 1 && run(db, "SELECT id FROM t WHERE id = 33").rows == 0,
          "색인 없는 표의 갱신·삭제 결과");
    db = reopen(db);
    CHECK(strcmp(run(db, "SELECT v FROM t WHERE id = 60").text, "new/") == 0, "재시작 뒤에도 같음");
    assert(database_close(db) == DATABASE_OK);
}

static void insert_row_without_index(const char *table, int64_t key)
{
    page_file_t *file;
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_store_load(file, &catalog) == CATALOG_STORE_OK);
    catalog_table_id_t id;
    assert(catalog_find(catalog, table, strlen(table), &id) == CATALOG_OK);
    catalog_table_view_t view;
    assert(catalog_describe(catalog, id, &view) == CATALOG_OK);
    value_type_t types[2] = {VALUE_INTEGER, VALUE_TEXT};
    heap_schema_t schema = {types, 2};
    heap_t *heap;
    assert(heap_open_ex(file, id, view.heap_root, &schema, NULL, &heap) == HEAP_OK);
    heap_batch_t *batch;
    assert(heap_batch_begin(heap, &batch) == HEAP_OK);
    value_view_t values[2] = {{VALUE_INTEGER, key, 0, NULL, 0}, {VALUE_TEXT, 0, 0, (const uint8_t *)"outside", 7}};
    heap_locator_t at;
    assert(heap_batch_insert(batch, values, 2, &at) == HEAP_OK);
    heap_batch_result_t finished;
    assert(heap_batch_finish(batch, &finished) == HEAP_OK);
    /* 색인을 모르는 쓰기: heap root만 바꾸고 primary_root는 그대로 둔다 */
    assert(catalog_set_locators(catalog, id, finished.new_root, view.primary_root) == CATALOG_OK);
    assert(catalog_set_row_count(catalog, id, view.row_count + 1) == CATALOG_OK);
    assert(catalog_store_save(file, catalog) == CATALOG_STORE_OK);
    heap_batch_free(batch);
    heap_close(heap);
    catalog_close(catalog);
    assert(page_file_close(file) == PAGE_FILE_OK);
}

static void test_index_out_of_step_with_heap_is_not_used(void)
{
    printf("\n[test_index_out_of_step_with_heap_is_not_used]\n");
    database_t *db = create_db();
    ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    for (int id = 1; id <= 20; id++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'v%d')", id, id);
        ok(db, sql);
    }
    assert(database_close(db) == DATABASE_OK);
    insert_row_without_index("t", 777);

    assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
    CHECK(strcmp(run(db, "SELECT id, v FROM t WHERE id = 777").text, "777|outside/") == 0,
          "색인을 모르는 쓰기로 생긴 행도 조회됨(낡은 색인을 쓰지 않음)");
    CHECK(run(db, "INSERT INTO t VALUES (777, 'dup')").error_code == DUPLICATE_KEY_CODE, "그 행의 키도 중복으로 거절");
    CHECK(stats_of(db).rows_scanned > 0, "어긋난 색인은 쓰지 않고 스캔함");
    ok(db, "DELETE FROM t WHERE id = 5");
    CHECK(run(db, "SELECT id FROM t WHERE id = 5").rows == 0 && run(db, "SELECT id FROM t WHERE id = 777").rows == 1,
          "그 뒤의 쓰기도 맞음");
    assert(database_close(db) == DATABASE_OK);
}

/* ── 4. 색인이 가리키는 행이 틀리면 엉뚱한 행을 돌려주지 않는다 ── */

static void test_wrong_locator_is_reported_not_returned(void)
{
    printf("\n[test_wrong_locator_is_reported_not_returned]\n");
    database_t *db = create_db();
    ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    for (int id = 1; id <= 5; id++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'v%d')", id, id);
        ok(db, sql);
    }
    assert(database_close(db) == DATABASE_OK);

    page_file_t *file;
    assert(page_file_open(DB_PATH, true, &file, NULL) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_store_load(file, &catalog) == CATALOG_STORE_OK);
    catalog_table_id_t id;
    assert(catalog_find(catalog, "t", 1, &id) == CATALOG_OK);
    catalog_table_view_t view;
    assert(catalog_describe(catalog, id, &view) == CATALOG_OK);
    uint8_t body[PAGE_BODY_BYTES];
    assert(page_file_read(file, view.primary_root, PAGE_KIND_INDEX, id, body) == PAGE_FILE_OK);
    /* leaf의 첫 항목(key 1)이 둘째 항목(key 2)의 행을 가리키게 한다 */
    memcpy(body + PK_TREE_HEADER_BYTES + 8, body + PK_TREE_HEADER_BYTES + PK_TREE_LEAF_ENTRY_BYTES + 8, 6);
    assert(page_file_write(file, view.primary_root, PAGE_KIND_INDEX, id, body) == PAGE_FILE_OK);
    catalog_close(catalog);
    assert(page_file_close(file) == PAGE_FILE_OK);

    assert(database_open(DB_PATH, true, &db) == DATABASE_OK);
    outcome_t wrong = run(db, "SELECT id, v FROM t WHERE id = 1");
    CHECK(wrong.kind == RESULT_KIND_ERROR, "다른 행을 가리키는 색인은 그 행을 돌려주지 않고 오류 (%d: %s)", wrong.kind, wrong.text);
    CHECK(run(db, "INSERT INTO t VALUES (1, 'dup')").kind == RESULT_KIND_ERROR, "중복 검사도 틀린 행을 믿지 않음");
    CHECK(strcmp(run(db, "SELECT id, v FROM t WHERE id = 3").text, "3|v3/") == 0, "손상되지 않은 키는 계속 조회됨");
    assert(database_close(db) == DATABASE_OK);
}

/* ── 5. DROP과 색인 페이지 ── */

static void test_collect_pages_includes_index_nodes(void)
{
    printf("\n[test_collect_pages_includes_index_nodes]\n");
    database_t *db = create_db();
    ok(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    for (int id = 1; id <= 30; id++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'v')", id);
        ok(db, sql);
    }
    assert(database_close(db) == DATABASE_OK);

    page_file_t *file;
    assert(page_file_open(DB_PATH, false, &file, NULL) == PAGE_FILE_OK);
    catalog_t *catalog;
    assert(catalog_store_load(file, &catalog) == CATALOG_STORE_OK);
    catalog_table_id_t id;
    assert(catalog_find(catalog, "t", 1, &id) == CATALOG_OK);
    catalog_table_view_t view;
    assert(catalog_describe(catalog, id, &view) == CATALOG_OK);
    CHECK(view.primary_root != CATALOG_NO_PAGE, "게시된 카탈로그에 색인 root가 있음");
    value_type_t types[2] = {VALUE_INTEGER, VALUE_TEXT};
    heap_schema_t schema = {types, 2};
    heap_index_spec_t spec = {view.primary_key_column, view.primary_root, NULL};
    heap_t *heap;
    assert(heap_open_indexed(file, id, view.heap_root, &schema, &spec, NULL, &heap) == HEAP_OK);
    CHECK(heap_is_indexed(heap), "게시된 상태에서 색인이 heap과 짝");
    uint32_t *pages = NULL;
    size_t count = 0;
    assert(heap_collect_pages(heap, &pages, &count) == HEAP_OK);
    bool has_index_root = false;
    for (size_t i = 0; i < count; i++) has_index_root |= pages[i] == view.primary_root;
    CHECK(has_index_root, "DROP이 놓을 페이지에 색인 root가 들어 있음");
    heap_free_pages(heap, pages, count);
    heap_close(heap);
    catalog_close(catalog);
    assert(page_file_close(file) == PAGE_FILE_OK);
}

int main(void)
{
    test_key_statements_do_not_scan();
    test_moved_and_deleted_rows_have_no_stale_locator();
    test_table_without_index_scans_and_stays_correct();
    test_index_out_of_step_with_heap_is_not_used();
    test_wrong_locator_is_reported_not_returned();
    test_collect_pages_includes_index_nodes();
    clean_dir();
    printf("\n%s: %d/%d\n", g_fail == 0 ? "ALL PASSED" : "FAILED", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
