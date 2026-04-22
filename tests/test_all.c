#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <inttypes.h>

#include "storage/pager.h"
#include "storage/schema.h"
#include "storage/table.h"
#include "storage/bptree.h"
#include "sql/parser.h"
#include "sql/planner.h"
#include "sql/executor.h"

#define TEST_DB "__test__.db"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

/* ── helper: setup a fresh DB with users table ── */
static void setup_db(pager_t *pager) {
    unlink(TEST_DB);
    assert(pager_open(pager, TEST_DB, true) == 0);

    db_header_t *hdr = &pager->header;

    /* id BIGINT */
    hdr->column_count = 0;
    column_meta_t *c = &hdr->columns[hdr->column_count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, "id", 31);
    c->type = COL_TYPE_BIGINT; c->size = 8; c->is_system = 1;

    /* name VARCHAR(32) */
    c = &hdr->columns[hdr->column_count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, "name", 31);
    c->type = COL_TYPE_VARCHAR; c->size = 32;

    /* age INT */
    c = &hdr->columns[hdr->column_count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, "age", 31);
    c->type = COL_TYPE_INT; c->size = 4;

    schema_compute_layout(hdr);
    pager->header_dirty = true;
}

/* ════════════════════════════════════════════════════════════ */
/*  1. Schema tests                                            */
/* ════════════════════════════════════════════════════════════ */
static void test_schema(void) {
    printf("[test_schema]\n");
    pager_t pager;
    setup_db(&pager);
    db_header_t *hdr = &pager.header;

    ASSERT(hdr->row_size == 44, "row_size should be 44");
    ASSERT(hdr->columns[0].offset == 0, "id offset 0");
    ASSERT(hdr->columns[1].offset == 8, "name offset 8");
    ASSERT(hdr->columns[2].offset == 40, "age offset 40");

    /* serialize / deserialize roundtrip */
    row_value_t vals[MAX_COLUMNS];
    memset(vals, 0, sizeof(vals));
    vals[0].bigint_val = 42;
    strncpy(vals[1].str_val, "Alice", 255);
    vals[2].int_val = 30;

    uint8_t buf[64];
    row_serialize(hdr, vals, buf);

    row_value_t out[MAX_COLUMNS];
    row_deserialize(hdr, buf, out);

    ASSERT(out[0].bigint_val == 42, "id roundtrip");
    ASSERT(strcmp(out[1].str_val, "Alice") == 0, "name roundtrip");
    ASSERT(out[2].int_val == 30, "age roundtrip");

    pager_close(&pager);
    unlink(TEST_DB);
}

/* ════════════════════════════════════════════════════════════ */
/*  2. Pager tests                                             */
/* ════════════════════════════════════════════════════════════ */
static void test_pager(void) {
    printf("[test_pager]\n");

    /* create and reopen */
    {
        pager_t p;
        setup_db(&p);
        ASSERT(p.header.page_size > 0, "page_size > 0");
        ASSERT(p.header.next_page_id == 3, "initial next_page_id == 3");
        pager_close(&p);

        /* reopen */
        pager_t p2;
        assert(pager_open(&p2, TEST_DB, false) == 0);
        ASSERT(p2.header.next_page_id == 3, "reopen next_page_id == 3");
        ASSERT(memcmp(p2.header.magic, DB_MAGIC, 7) == 0, "magic matches");
        pager_close(&p2);
        unlink(TEST_DB);
    }

    /* alloc and free */
    {
        pager_t p;
        setup_db(&p);
        uint32_t a = pager_alloc_page(&p);
        ASSERT(a == 3, "first alloc page 3");
        uint32_t b = pager_alloc_page(&p);
        ASSERT(b == 4, "second alloc page 4");

        pager_free_page(&p, a);
        ASSERT(p.header.free_page_head == a, "free_page_head == a");

        uint32_t c = pager_alloc_page(&p);
        ASSERT(c == a, "re-alloc returns freed page");

        pager_close(&p);
        unlink(TEST_DB);
    }
}

static bool heap_count_cb(const uint8_t *d, row_ref_t r, void *c) {
    (void)d; (void)r;
    (*(uint32_t *)c)++;
    return true;
}

/* ════════════════════════════════════════════════════════════ */
/*  3. Heap table tests                                        */
/* ════════════════════════════════════════════════════════════ */
static void test_heap(void) {
    printf("[test_heap]\n");
    pager_t pager;
    setup_db(&pager);
    db_header_t *hdr = &pager.header;

    row_value_t vals[MAX_COLUMNS];
    memset(vals, 0, sizeof(vals));
    vals[0].bigint_val = 1;
    strncpy(vals[1].str_val, "Bob", 255);
    vals[2].int_val = 25;

    uint8_t buf[64];
    row_serialize(hdr, vals, buf);

    /* insert */
    row_ref_t ref = heap_insert(&pager, buf, hdr->row_size);
    ASSERT(ref.page_id > 0, "insert returns valid page_id");

    /* fetch */
    const uint8_t *data = heap_fetch(&pager, ref, hdr->row_size);
    ASSERT(data != NULL, "fetch returns data");
    row_value_t out[MAX_COLUMNS];
    row_deserialize(hdr, data, out);
    pager_unpin(&pager, ref.page_id);
    ASSERT(out[0].bigint_val == 1, "fetched id");
    ASSERT(strcmp(out[1].str_val, "Bob") == 0, "fetched name");

    /* delete */
    int rc = heap_delete(&pager, ref);
    ASSERT(rc == 0, "delete success");

    /* fetch after delete should fail */
    const uint8_t *data2 = heap_fetch(&pager, ref, hdr->row_size);
    ASSERT(data2 == NULL, "fetch after delete returns NULL");

    /* re-insert should reuse slot */
    vals[0].bigint_val = 2;
    strncpy(vals[1].str_val, "Charlie", 255);
    row_serialize(hdr, vals, buf);
    row_ref_t ref2 = heap_insert(&pager, buf, hdr->row_size);
    ASSERT(ref2.page_id == ref.page_id && ref2.slot_id == ref.slot_id,
           "reinsert reuses freed slot");

    /* scan */
    uint32_t scan_count = 0;
    heap_scan(&pager, hdr->row_size, heap_count_cb, &scan_count);
    ASSERT(scan_count == 1, "scan finds 1 alive row");

    pager_close(&pager);
    unlink(TEST_DB);
}

/* ════════════════════════════════════════════════════════════ */
/*  4. B+ tree tests                                           */
/* ════════════════════════════════════════════════════════════ */
static void test_bptree(void) {
    printf("[test_bptree]\n");
    pager_t pager;
    setup_db(&pager);

    row_ref_t dummy = { .page_id = 100, .slot_id = 0 };

    /* single insert/search */
    int rc = bptree_insert(&pager, 42, dummy);
    ASSERT(rc == 0, "insert key=42");

    row_ref_t found;
    bool ok = bptree_search(&pager, 42, &found);
    ASSERT(ok, "search key=42 found");
    ASSERT(found.page_id == 100, "search returns correct ref");

    /* duplicate */
    rc = bptree_insert(&pager, 42, dummy);
    ASSERT(rc == -1, "duplicate insert fails");

    /* not found */
    ok = bptree_search(&pager, 999, &found);
    ASSERT(!ok, "search non-existent key");

    /* many inserts to trigger splits */
    uint32_t N = 2000;
    for (uint32_t i = 1; i <= N; i++) {
        if (i == 42) continue;
        row_ref_t r = { .page_id = i, .slot_id = 0 };
        rc = bptree_insert(&pager, i, r);
        if (rc != 0) {
            printf("  insert %u failed\n", i);
            break;
        }
    }

    /* verify all */
    bool all_found = true;
    for (uint32_t i = 1; i <= N; i++) {
        if (!bptree_search(&pager, i, &found)) {
            printf("  key %u not found after bulk insert\n", i);
            all_found = false;
            break;
        }
    }
    ASSERT(all_found, "all 2000 keys found after insert");

    int h = bptree_height(&pager);
    ASSERT(h >= 2, "tree height >= 2 after 2000 inserts");

    /* delete some keys */
    for (uint32_t i = 1; i <= 500; i++) {
        rc = bptree_delete(&pager, i);
        if (rc != 0) {
            printf("  delete key %u failed\n", i);
            break;
        }
    }

    /* deleted keys should not be found */
    bool none_found = true;
    for (uint32_t i = 1; i <= 500; i++) {
        if (bptree_search(&pager, i, &found)) {
            printf("  key %u still found after delete\n", i);
            none_found = false;
            break;
        }
    }
    ASSERT(none_found, "deleted keys not found");

    /* remaining keys should still be found */
    bool rest_found = true;
    for (uint32_t i = 501; i <= N; i++) {
        if (!bptree_search(&pager, i, &found)) {
            printf("  key %u not found after partial delete\n", i);
            rest_found = false;
            break;
        }
    }
    ASSERT(rest_found, "remaining keys still found after partial delete");

    pager_close(&pager);
    unlink(TEST_DB);
}

/* ════════════════════════════════════════════════════════════ */
/*  5. Parser tests                                            */
/* ════════════════════════════════════════════════════════════ */
static void test_parser(void) {
    printf("[test_parser]\n");

    statement_t stmt;

    /* CREATE TABLE */
    int rc = parse("CREATE TABLE users (name VARCHAR(32), age INT);", &stmt);
    ASSERT(rc == 0, "parse CREATE TABLE");
    ASSERT(stmt.type == STMT_CREATE_TABLE, "stmt type CREATE_TABLE");
    ASSERT(strcmp(stmt.table_name, "users") == 0, "table name");

    /* INSERT */
    rc = parse("INSERT INTO users VALUES ('Alice', 30);", &stmt);
    ASSERT(rc == 0, "parse INSERT");
    ASSERT(stmt.type == STMT_INSERT, "stmt type INSERT");
    ASSERT(strcmp(stmt.insert_values[0], "Alice") == 0, "insert value 0");
    ASSERT(strcmp(stmt.insert_values[1], "30") == 0, "insert value 1");

    /* SELECT with id */
    rc = parse("SELECT * FROM users WHERE id = 5;", &stmt);
    ASSERT(rc == 0, "parse SELECT WHERE id");
    ASSERT(stmt.predicate_kind == PREDICATE_ID_EQ, "predicate ID_EQ");
    ASSERT(stmt.pred_id == 5, "pred_id == 5");

    /* SELECT with field */
    rc = parse("SELECT * FROM users WHERE name = 'Bob';", &stmt);
    ASSERT(rc == 0, "parse SELECT WHERE name");
    ASSERT(stmt.predicate_kind == PREDICATE_FIELD_EQ, "predicate FIELD_EQ");

    /* DELETE */
    rc = parse("DELETE FROM users WHERE id = 10;", &stmt);
    ASSERT(rc == 0, "parse DELETE WHERE id");
    ASSERT(stmt.type == STMT_DELETE, "stmt type DELETE");
    ASSERT(stmt.pred_id == 10, "delete pred_id == 10");

    /* EXPLAIN */
    rc = parse("EXPLAIN SELECT * FROM users WHERE id = 1;", &stmt);
    ASSERT(rc == 0, "parse EXPLAIN");
    ASSERT(stmt.type == STMT_EXPLAIN, "stmt type EXPLAIN");
}

/* ════════════════════════════════════════════════════════════ */
/*  6. Planner tests                                           */
/* ════════════════════════════════════════════════════════════ */
static void test_planner(void) {
    printf("[test_planner]\n");

    statement_t stmt;
    memset(&stmt, 0, sizeof(stmt));

    stmt.type = STMT_SELECT;
    stmt.predicate_kind = PREDICATE_ID_EQ;
    plan_t p = planner_create_plan(&stmt);
    ASSERT(p.access_path == ACCESS_PATH_INDEX_LOOKUP, "SELECT id=? -> INDEX_LOOKUP");

    stmt.predicate_kind = PREDICATE_FIELD_EQ;
    p = planner_create_plan(&stmt);
    ASSERT(p.access_path == ACCESS_PATH_TABLE_SCAN, "SELECT name=? -> TABLE_SCAN");

    stmt.type = STMT_DELETE;
    stmt.predicate_kind = PREDICATE_ID_EQ;
    p = planner_create_plan(&stmt);
    ASSERT(p.access_path == ACCESS_PATH_INDEX_DELETE, "DELETE id=? -> INDEX_DELETE");

    stmt.predicate_kind = PREDICATE_NONE;
    p = planner_create_plan(&stmt);
    ASSERT(p.access_path == ACCESS_PATH_TABLE_SCAN, "DELETE no pred -> TABLE_SCAN");
}

/* ════════════════════════════════════════════════════════════ */
/*  7. Integration: insert → persist → reopen → select         */
/* ════════════════════════════════════════════════════════════ */
static void test_persistence(void) {
    printf("[test_persistence]\n");
    unlink(TEST_DB);

    /* create and insert */
    {
        pager_t pager;
        assert(pager_open(&pager, TEST_DB, true) == 0);

        statement_t stmt;
        parse("CREATE TABLE users (name VARCHAR(32), age INT);", &stmt);
        execute(&pager, &stmt);

        for (int i = 0; i < 100; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('user%d', %d);", i, 20 + i);
            parse(sql, &stmt);
            execute(&pager, &stmt);
        }

        ASSERT(pager.header.row_count == 100, "100 rows inserted");
        ASSERT(pager.header.next_id == 101, "next_id == 101");

        pager_close(&pager);
    }

    /* reopen and verify */
    {
        pager_t pager;
        assert(pager_open(&pager, TEST_DB, false) == 0);

        ASSERT(pager.header.row_count == 100, "after reopen: 100 rows");
        ASSERT(pager.header.next_id == 101, "after reopen: next_id == 101");

        /* index lookup */
        row_ref_t ref;
        bool found = bptree_search(&pager, 50, &ref);
        ASSERT(found, "key 50 found after reopen");

        if (found) {
            const uint8_t *data = heap_fetch(&pager, ref, pager.header.row_size);
            ASSERT(data != NULL, "fetch row 50");
            if (data) {
                row_value_t vals[MAX_COLUMNS];
                row_deserialize(&pager.header, data, vals);
                pager_unpin(&pager, ref.page_id);
                ASSERT(vals[0].bigint_val == 50, "row 50 id correct");
                ASSERT(strcmp(vals[1].str_val, "user49") == 0, "row 50 name correct");
            }
        }

        /* delete and re-insert */
        statement_t stmt;
        parse("DELETE FROM users WHERE id = 50;", &stmt);
        exec_result_t res = execute(&pager, &stmt);
        ASSERT(res.status == 0, "delete id=50 success");
        ASSERT(pager.header.row_count == 99, "99 rows after delete");

        found = bptree_search(&pager, 50, &ref);
        ASSERT(!found, "key 50 not found after delete");

        /* insert new row, should get id=101 */
        parse("INSERT INTO users VALUES ('newuser', 99);", &stmt);
        res = execute(&pager, &stmt);
        ASSERT(res.status == 0, "insert after delete success");
        ASSERT(pager.header.next_id == 102, "next_id == 102");
        ASSERT(pager.header.row_count == 100, "100 rows again");

        pager_close(&pager);
    }

    unlink(TEST_DB);
}

/* ════════════════════════════════════════════════════════════ */
/*  8. Delete and free slot reuse integration                  */
/* ════════════════════════════════════════════════════════════ */
static void test_delete_reuse(void) {
    printf("[test_delete_reuse]\n");
    unlink(TEST_DB);

    pager_t pager;
    assert(pager_open(&pager, TEST_DB, true) == 0);

    statement_t stmt;
    parse("CREATE TABLE users (name VARCHAR(32), age INT);", &stmt);
    execute(&pager, &stmt);

    /* insert 10 rows */
    for (int i = 0; i < 10; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('user%d', %d);", i, i);
        parse(sql, &stmt);
        execute(&pager, &stmt);
    }

    /* delete rows 3,5,7 */
    parse("DELETE FROM users WHERE id = 3;", &stmt);
    execute(&pager, &stmt);
    parse("DELETE FROM users WHERE id = 5;", &stmt);
    execute(&pager, &stmt);
    parse("DELETE FROM users WHERE id = 7;", &stmt);
    execute(&pager, &stmt);

    ASSERT(pager.header.row_count == 7, "7 rows after deleting 3");

    /* insert 3 more — should reuse freed slots */
    for (int i = 0; i < 3; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('new%d', %d);", i, 100+i);
        parse(sql, &stmt);
        execute(&pager, &stmt);
    }

    ASSERT(pager.header.row_count == 10, "10 rows after re-insert");

    /* verify all existing ids are searchable */
    row_ref_t ref;
    ASSERT(!bptree_search(&pager, 3, &ref), "id=3 still deleted");
    ASSERT(!bptree_search(&pager, 5, &ref), "id=5 still deleted");
    ASSERT(bptree_search(&pager, 1, &ref), "id=1 still present");
    ASSERT(bptree_search(&pager, 11, &ref), "new id=11 present");

    pager_close(&pager);
    unlink(TEST_DB);
}

/* ════════════════════════════════════════════════════════════ */
/*  main                                                       */
/* ════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== MiniDB Test Suite ===\n\n");

    test_schema();
    test_pager();
    test_heap();
    test_bptree();
    test_parser();
    test_planner();
    test_persistence();
    test_delete_reuse();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
