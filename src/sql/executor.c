/*
 * executor.c — SQL 실행기
 *
 * 역할:
 *   플래너가 결정한 접근 경로(access path)에 따라 실제 데이터 조작을 수행한다.
 *
 * 전체 실행 흐름 예시 (INSERT INTO users VALUES ('Alice', 25)):
 *
 *   1. parse() → stmt = {type=INSERT, table_name="users", values=["Alice","25"]}
 *   2. execute() → planner_create_plan() → ACCESS_PATH_INSERT
 *   3. exec_insert() 호출:
 *      a. id = next_id = 1 (자동 할당)
 *      b. values[0].bigint_val = 1     (id)
 *         values[1].str_val = "Alice"  (name)
 *         values[2].int_val = 25       (age)
 *      c. row_serialize() → 44바이트 버퍼 생성
 *      d. heap_insert() → row_ref_t {page_id=1, slot_id=0} 획득
 *      e. bptree_insert(key=1, ref={1,0}) → B+ tree에 등록
 *      f. next_id=2, row_count=1 갱신
 *
 * 접근 경로별 실행 함수:
 *   CREATE_TABLE  → exec_create_table()  : 스키마 정의
 *   INSERT        → exec_insert()        : 힙 삽입 + B+ tree 삽입
 *   INDEX_LOOKUP  → exec_index_lookup()  : B+ tree O(log n) 조회
 *   TABLE_SCAN    → exec_table_scan()    : 힙 O(n) 전체 스캔
 *   INDEX_DELETE  → exec_index_delete()  : B+ tree로 찾아 삭제
 *   TABLE_SCAN(삭제) → exec_delete_scan(): 스캔 후 2-pass 일괄 삭제
 */

#include "sql/executor.h"
#include "storage/schema.h"
#include "storage/table.h"
#include "storage/bptree.h"
#include "db.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <pthread.h>

/* ══════════════════════════════════════════════════════════════════════
 *  동적 출력 버퍼 (printf 대체)
 *
 *  SELECT/EXPLAIN 결과를 메모리 버퍼에 축적한다.
 *  서버 모드에서는 이 버퍼를 클라이언트에 send()하고,
 *  REPL 모드에서는 printf("%s", buf)로 출력한다.
 * ====================================================================== */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} out_buf_t;

static void buf_init(out_buf_t *b)
{
    b->cap  = 4096;
    b->data = (char *)malloc(b->cap);
    b->len  = 0;
    b->data[0] = '\0';
}

static void buf_append(out_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void buf_append(out_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;

    while (b->len + (size_t)need + 1 > b->cap) {
        b->cap *= 2;
        b->data = (char *)realloc(b->data, b->cap);
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}

/* 버퍼 소유권을 exec_result_t에 이전. 호출 후 b는 사용 불가 */
static void buf_transfer(out_buf_t *b, exec_result_t *res)
{
    res->out_buf = b->data;
    res->out_len = b->len;
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* 버퍼를 해제 (transfer하지 않는 경로용) */
static void buf_free(out_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/*
 * append_row - 행 데이터를 컬럼별로 포맷하여 버퍼에 기록한다.
 *
 * 예시: columns = [id BIGINT, name VARCHAR(32), age INT]
 *       values = [1, "Alice", 25]
 *       출력: "1 | Alice | 25\n"
 */
static void append_row(out_buf_t *b, const db_header_t *hdr,
                        const row_value_t *values)
{
    for (uint16_t i = 0; i < hdr->column_count; i++) {
        if (i > 0) {
            buf_append(b, " | ");
        }
        const column_meta_t *col = &hdr->columns[i];
        switch (col->type) {
            case COL_TYPE_INT:
                buf_append(b, "%d", values[i].int_val);
                break;
            case COL_TYPE_BIGINT:
                buf_append(b, "%" PRId64, values[i].bigint_val);
                break;
            case COL_TYPE_VARCHAR:
                buf_append(b, "%s", values[i].str_val);
                break;
        }
    }
    buf_append(b, "\n");
}

/*
 * append_header - 컬럼 이름과 구분선을 버퍼에 기록한다.
 */
static void append_header(out_buf_t *b, const db_header_t *hdr)
{
    for (uint16_t i = 0; i < hdr->column_count; i++) {
        if (i > 0) {
            buf_append(b, " | ");
        }
        buf_append(b, "%s", hdr->columns[i].name);
    }
    buf_append(b, "\n");
    for (uint16_t i = 0; i < hdr->column_count; i++) {
        if (i > 0) {
            buf_append(b, "-+-");
        }
        for (uint16_t j = 0; j < 10; j++) {
            buf_append(b, "-");
        }
    }
    buf_append(b, "\n");
}

/* ══════════════════════════════════════════════════════════════════════
 *  CREATE TABLE
 *
 *  예시: CREATE TABLE users (name VARCHAR(32), age INT)
 *
 *  결과 (DB 헤더에 저장):
 *    columns[0] = {name="id",   type=BIGINT,     size=8,  offset=0,  is_system=1}
 *    columns[1] = {name="name", type=VARCHAR(32), size=32, offset=8,  is_system=0}
 *    columns[2] = {name="age",  type=INT,         size=4,  offset=40, is_system=0}
 *    column_count = 3, row_size = 44
 *
 *  id는 시스템 컬럼으로 자동 추가된다 (사용자가 명시해도 건너뜀).
 *  현재 단일 테이블만 지원하며, 이미 테이블이 있으면 오류를 반환한다.
 * ══════════════════════════════════════════════════════════════════════ */
static exec_result_t exec_create_table(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;

    if (hdr->column_count > 0) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message),
                 "오류: '%s' 테이블이 이미 존재합니다", stmt->table_name);
        return res;
    }

    /* id를 첫 번째 시스템 컬럼으로 추가 (BIGINT 8바이트) */
    hdr->column_count = 0;
    column_meta_t *id_col = &hdr->columns[hdr->column_count++];
    memset(id_col, 0, sizeof(*id_col));
    strncpy(id_col->name, "id", 31);
    id_col->type = COL_TYPE_BIGINT;
    id_col->size = 8;
    id_col->is_system = 1;

    /* 사용자 정의 컬럼 등록 (id가 명시적으로 정의되었으면 건너뜀) */
    for (uint16_t i = 0; i < stmt->col_count; i++) {
        column_def_t *cd = &stmt->col_defs[i];
        if (strncmp(cd->name, "id", 32) == 0) {
            continue;
        }

        column_meta_t *col = &hdr->columns[hdr->column_count++];
        memset(col, 0, sizeof(*col));
        strncpy(col->name, cd->name, 31);
        col->type = cd->type;
        col->size = cd->size;
        col->is_system = 0;
    }

    /*
     * 컬럼 오프셋 계산
     * schema_compute_layout()이 각 컬럼의 offset을 누적 계산하고 row_size를 설정한다.
     * 예: [8, 32, 4] → offsets=[0, 8, 40], row_size=44
     */
    schema_compute_layout(hdr);
    pager->header_dirty = true;

    snprintf(res.message, sizeof(res.message),
             "'%s' 테이블 생성 완료 (row_size=%u, columns=%u)",
             stmt->table_name, hdr->row_size, hdr->column_count);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  INSERT
 *
 *  예시: INSERT INTO users VALUES ('Alice', 25)
 *
 *  실행 과정:
 *    1. id = next_id = 1 (자동 할당)
 *    2. values[0] = id(1), values[1] = "Alice", values[2] = 25
 *    3. row_serialize() → 44바이트 버퍼: [01 00...][Alice\0...][19 00 00 00]
 *    4. heap_insert(row_buf) → ref = {page_id=1, slot_id=0}
 *    5. bptree_insert(key=1, ref={1,0})
 *    6. next_id = 2, row_count = 1
 * ══════════════════════════════════════════════════════════════════════ */
static exec_result_t exec_insert(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;

    if (hdr->column_count == 0) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message), "오류: 생성된 테이블이 없습니다");
        return res;
    }

    row_value_t values[MAX_COLUMNS];
    memset(values, 0, sizeof(values));

    /*
     * id 할당: header_lock으로 next_id를 원자적으로 읽고 증가시킨다.
     * DML이 rdlock으로 동시 실행되므로 여러 INSERT가 같은 next_id를 쓰지 않도록 보호.
     */
    pthread_mutex_lock(&pager->header_lock);
    uint64_t my_id = hdr->next_id++;
    pager->header_dirty = true;
    pthread_mutex_unlock(&pager->header_lock);

    /*
     * Gap Check (Next-Key Lock): 새 id가 다른 스레드의 range lock 범위 안이면 대기.
     * 예: 스레드 A가 UPDATE WHERE id>5 로 range X lock [6, MAX]을 잡고 있으면,
     *     이 INSERT의 id=7은 그 범위 안이므로 A가 끝날 때까지 대기한다.
     * → Phantom Insert 방지.
     *
     * range lock이 존재할 때만 gap check 수행한다.
     * range lock이 없으면 point lock도 불필요하므로 건너뛴다.
     */
    lock_table_t *lt = db_get_lock_table();
    if (lt->range_locks != NULL) {
        if (lock_acquire(lt, my_id, LOCK_X) != 0) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message),
                     "오류: INSERT gap check timeout (id=%" PRIu64 ", range lock 충돌)", my_id);
            return res;
        }
    }

    values[0].bigint_val = (int64_t)my_id;

    /*
     * 사용자 입력 값을 비시스템 컬럼에 매핑한다.
     * 컬럼 0(id)은 건너뛰고, 컬럼 1부터 사용자 값을 순서대로 넣는다.
     */
    uint16_t val_idx = 0;
    for (uint16_t i = 1; i < hdr->column_count && val_idx < stmt->insert_value_count; i++) {
        const column_meta_t *col = &hdr->columns[i];
        const char *sv = stmt->insert_values[val_idx++];
        switch (col->type) {
            case COL_TYPE_INT:
                values[i].int_val = atoi(sv);
                break;
            case COL_TYPE_BIGINT:
                values[i].bigint_val = atoll(sv);
                break;
            case COL_TYPE_VARCHAR:
                strncpy(values[i].str_val, sv, 255);
                break;
        }
    }

    /* 행 직렬화 → 힙 삽입 → B+ tree 등록 */
    uint8_t *row_buf = (uint8_t *)calloc(1, hdr->row_size);
    row_serialize(hdr, values, row_buf);

    row_ref_t ref = heap_insert(pager, row_buf, hdr->row_size);
    int rc = bptree_insert(pager, my_id, ref);
    free(row_buf);

    if (rc != 0) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message), "오류: 중복된 키입니다");
        return res;
    }

    pthread_mutex_lock(&pager->header_lock);
    hdr->row_count++;
    pager->header_dirty = true;
    pthread_mutex_unlock(&pager->header_lock);

    snprintf(res.message, sizeof(res.message),
             "1행 삽입 완료 (id=%" PRIu64 ")", my_id);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SELECT — INDEX_LOOKUP (B+ tree O(log n) 단건 조회)
 *
 *  예시: SELECT * FROM users WHERE id = 3
 *
 *  실행 과정:
 *    1. bptree_search(key=3) → ref = {page_id=1, slot_id=2}
 *    2. heap_fetch(ref) → 44바이트 행 데이터 포인터
 *    3. row_deserialize() → values = [3, "Charlie", 30]
 *    4. print_header() + print_row()
 *
 *  시간 복잡도: O(log n) — 100만 건에서도 3~4번의 페이지 접근
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * match_predicate - WHERE 절 조건과 행 값을 비교한다.
 *
 * PREDICATE_FIELD_EQ일 때 pred_field 컬럼의 값이 pred_value와 일치하면 true.
 * PREDICATE_NONE이면 항상 true (무조건 일치).
 *
 * 예시: WHERE name = 'Alice'
 *   pred_field="name", pred_value="Alice"
 *   → columns[1].name == "name" → strcmp(values[1].str_val, "Alice") → match
 */
/* 비교 연산자 적용 헬퍼 */
static bool apply_cmp_int(int32_t a, int32_t b, compare_op_t op)
{
    switch (op) {
        case OP_EQ: return a == b;
        case OP_NE: return a != b;
        case OP_LT: return a <  b;
        case OP_GT: return a >  b;
        case OP_LE: return a <= b;
        case OP_GE: return a >= b;
    }
    return false;
}

static bool apply_cmp_bigint(int64_t a, int64_t b, compare_op_t op)
{
    switch (op) {
        case OP_EQ: return a == b;
        case OP_NE: return a != b;
        case OP_LT: return a <  b;
        case OP_GT: return a >  b;
        case OP_LE: return a <= b;
        case OP_GE: return a >= b;
    }
    return false;
}

static bool apply_cmp_str(const char *a, const char *b, compare_op_t op)
{
    int cmp = strcmp(a, b);
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp <  0;
        case OP_GT: return cmp >  0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

static bool match_predicate(const db_header_t *hdr, const row_value_t *values,
                            const statement_t *stmt)
{
    if (stmt->predicate_kind == PREDICATE_NONE) {
        return true;
    }
    /* PREDICATE_ID_EQ 는 인덱스 경로에서 처리되지만 table scan fallback 시 여기로 올 수 있다 */
    if (stmt->predicate_kind == PREDICATE_ID_EQ) {
        return values[0].bigint_val == (int64_t)stmt->pred_id;
    }

    compare_op_t op = (stmt->predicate_kind == PREDICATE_FIELD_EQ) ? OP_EQ : stmt->pred_op;

    for (uint16_t i = 0; i < hdr->column_count; i++) {
        if (strncmp(hdr->columns[i].name, stmt->pred_field, 32) != 0) {
            continue;
        }
        switch (hdr->columns[i].type) {
            case COL_TYPE_INT:
                return apply_cmp_int(values[i].int_val, atoi(stmt->pred_value), op);
            case COL_TYPE_BIGINT:
                return apply_cmp_bigint(values[i].bigint_val, atoll(stmt->pred_value), op);
            case COL_TYPE_VARCHAR:
                return apply_cmp_str(values[i].str_val, stmt->pred_value, op);
        }
    }
    return false;
}

/* SELECT의 테이블 스캔 콜백에서 사용하는 컨텍스트 */
typedef struct {
    pager_t *pager;
    const statement_t *stmt;
    uint32_t count;
    out_buf_t *buf;
} scan_ctx_t;

/*
 * select_scan_cb - SELECT의 테이블 스캔 콜백
 *
 * 각 행에 대해 match_predicate()로 조건 검사 후 일치하면 출력한다.
 */
static bool select_scan_cb(const uint8_t *row_data, row_ref_t ref, void *ctx)
{
    (void)ref;
    scan_ctx_t *sc = (scan_ctx_t *)ctx;
    const db_header_t *hdr = &sc->pager->header;

    row_value_t values[MAX_COLUMNS];
    row_deserialize(hdr, row_data, values);

    if (!match_predicate(hdr, values, sc->stmt)) {
        return true; /* 조건 불일치, 다음 행으로 계속 */
    }

    /* 첫 번째 결과 행 출력 전에 헤더를 버퍼에 기록 */
    if (sc->count == 0) {
        append_header(sc->buf, hdr);
    }
    append_row(sc->buf, hdr, values);
    sc->count++;
    return true;
}

static exec_result_t exec_index_lookup(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;

    /* B+ tree에서 id로 검색 → 힙 위치(row_ref_t) 획득 */
    row_ref_t ref;
    if (bptree_search(pager, stmt->pred_id, &ref) == false) {
        snprintf(res.message, sizeof(res.message), "오류: id=%" PRIu64 "인 행을 찾을 수 없습니다", stmt->pred_id);
        return res;
    }

    /* 힙에서 행 데이터 읽기 */
    const uint8_t *row_data = heap_fetch(pager, ref, hdr->row_size);
    if (row_data == NULL) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message), "오류: 행 데이터를 읽지 못했습니다");
        return res;
    }

    row_value_t values[MAX_COLUMNS];
    row_deserialize(hdr, row_data, values);
    pager_unlatch_r(pager, ref.page_id);

    out_buf_t buf;
    buf_init(&buf);
    append_header(&buf, hdr);
    append_row(&buf, hdr, values);
    buf_transfer(&buf, &res);

    snprintf(res.message, sizeof(res.message), "1행 조회 (INDEX_LOOKUP)");
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SELECT — TABLE_SCAN (힙 O(n) 전체 스캔)
 *
 *  예시: SELECT * FROM users WHERE name = 'Alice'
 *
 *  실행 과정:
 *    1. heap_scan() 호출 → 모든 힙 페이지를 순회
 *    2. 각 행에 대해 select_scan_cb() 호출
 *    3. name 컬럼에서 'Alice'와 일치하는 행만 출력
 *
 *  시간 복잡도: O(n) — 전체 행을 읽어야 하므로 느림
 * ══════════════════════════════════════════════════════════════════════ */
/* ── ORDER BY / LIMIT 지원을 위한 수집형 스캔 ── */

typedef struct {
    row_value_t *rows;   /* 수집된 행 배열 */
    uint32_t count;
    uint32_t cap;
} row_collect_t;

static void row_collect_init(row_collect_t *rc)
{
    rc->cap   = 256;
    rc->count = 0;
    rc->rows  = (row_value_t *)malloc(rc->cap * MAX_COLUMNS * sizeof(row_value_t));
}

static void row_collect_push(row_collect_t *rc, const row_value_t *vals)
{
    if (rc->count >= rc->cap) {
        rc->cap *= 2;
        rc->rows = (row_value_t *)realloc(rc->rows,
                    rc->cap * MAX_COLUMNS * sizeof(row_value_t));
    }
    memcpy(&rc->rows[rc->count * MAX_COLUMNS], vals, MAX_COLUMNS * sizeof(row_value_t));
    rc->count++;
}

static void row_collect_free(row_collect_t *rc)
{
    free(rc->rows);
    rc->rows = NULL;
}

static row_value_t *row_collect_get(row_collect_t *rc, uint32_t idx)
{
    return &rc->rows[idx * MAX_COLUMNS];
}

/* collect_scan_cb: 조건 일치하는 행을 배열에 수집 */
typedef struct {
    pager_t *pager;
    const statement_t *stmt;
    row_collect_t *collector;
} collect_ctx_t;

static bool collect_scan_cb(const uint8_t *row_data, row_ref_t ref, void *ctx)
{
    (void)ref;
    collect_ctx_t *cc = (collect_ctx_t *)ctx;
    const db_header_t *hdr = &cc->pager->header;

    row_value_t values[MAX_COLUMNS];
    row_deserialize(hdr, row_data, values);

    if (!match_predicate(hdr, values, cc->stmt)) {
        return true;
    }
    row_collect_push(cc->collector, values);
    return true;
}

/* qsort 비교용 전역 (ORDER BY) */
static const db_header_t *g_sort_hdr;
static int g_sort_col_idx;
static bool g_sort_desc;

static int sort_compare(const void *a, const void *b)
{
    const row_value_t *ra = (const row_value_t *)a + g_sort_col_idx;
    const row_value_t *rb = (const row_value_t *)b + g_sort_col_idx;
    int cmp = 0;
    switch (g_sort_hdr->columns[g_sort_col_idx].type) {
        case COL_TYPE_INT:
            cmp = (ra->int_val > rb->int_val) - (ra->int_val < rb->int_val);
            break;
        case COL_TYPE_BIGINT:
            cmp = (ra->bigint_val > rb->bigint_val) - (ra->bigint_val < rb->bigint_val);
            break;
        case COL_TYPE_VARCHAR:
            cmp = strcmp(ra->str_val, rb->str_val);
            break;
    }
    return g_sort_desc ? -cmp : cmp;
}

static int find_column_index(const db_header_t *hdr, const char *name)
{
    for (uint16_t i = 0; i < hdr->column_count; i++) {
        if (strncmp(hdr->columns[i].name, name, 32) == 0)
            return (int)i;
    }
    return -1;
}

static exec_result_t exec_table_scan(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;

    /* COUNT(*) 전용 경로 */
    if (stmt->select_count) {
        out_buf_t buf;
        buf_init(&buf);
        /* 간단 카운트: 조건 일치 행만 센다 */
        row_collect_t rc;
        row_collect_init(&rc);
        collect_ctx_t cc = { .pager = pager, .stmt = stmt, .collector = &rc };
        heap_scan(pager, hdr->row_size, collect_scan_cb, &cc);
        buf_append(&buf, "COUNT(*)\n----------\n%u\n", rc.count);
        buf_transfer(&buf, &res);
        snprintf(res.message, sizeof(res.message), "count = %u", rc.count);
        row_collect_free(&rc);
        return res;
    }

    /* ORDER BY 또는 LIMIT가 있으면 수집형 스캔 */
    if (stmt->has_order_by || stmt->has_limit) {
        row_collect_t rc;
        row_collect_init(&rc);
        collect_ctx_t cc = { .pager = pager, .stmt = stmt, .collector = &rc };
        heap_scan(pager, hdr->row_size, collect_scan_cb, &cc);

        /* ORDER BY */
        if (stmt->has_order_by && rc.count > 1) {
            int col_idx = find_column_index(hdr, stmt->order_by_field);
            if (col_idx >= 0) {
                g_sort_hdr = hdr;
                g_sort_col_idx = col_idx;
                g_sort_desc = stmt->order_desc;
                qsort(rc.rows, rc.count, MAX_COLUMNS * sizeof(row_value_t), sort_compare);
            }
        }

        /* 출력 (LIMIT 적용) */
        uint32_t output_count = rc.count;
        if (stmt->has_limit && stmt->limit_count < output_count)
            output_count = stmt->limit_count;

        out_buf_t buf;
        buf_init(&buf);
        if (output_count > 0) {
            append_header(&buf, hdr);
            for (uint32_t i = 0; i < output_count; i++) {
                append_row(&buf, hdr, row_collect_get(&rc, i));
            }
        }
        if (output_count > 0) {
            buf_transfer(&buf, &res);
        } else {
            buf_free(&buf);
        }
        snprintf(res.message, sizeof(res.message), "%u행 조회 (TABLE_SCAN)", output_count);
        row_collect_free(&rc);
        return res;
    }

    /* 기본 스트리밍 스캔 (기존 로직) */
    out_buf_t buf;
    buf_init(&buf);
    scan_ctx_t sc = { .pager = pager, .stmt = stmt, .count = 0, .buf = &buf };
    heap_scan(pager, hdr->row_size, select_scan_cb, &sc);
    if (sc.count > 0) {
        buf_transfer(&buf, &res);
    } else {
        buf_free(&buf);
    }
    snprintf(res.message, sizeof(res.message), "%u행 조회 (TABLE_SCAN)", sc.count);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DELETE — INDEX_DELETE (B+ tree O(log n) 단건 삭제)
 *
 *  예시: DELETE FROM users WHERE id = 3
 *
 *  실행 과정:
 *    1. bptree_search(key=3) → ref = {page_id=1, slot_id=2}
 *    2. heap_delete(ref) → slot_2.status = FREE (톰스톤)
 *    3. bptree_delete(key=3) → B+ tree에서 엔트리 제거
 *    4. row_count-- 갱신
 * ══════════════════════════════════════════════════════════════════════ */

/* DELETE 테이블 스캔 콜백에서 사용하는 컨텍스트 */
typedef struct {
    pager_t *pager;
    const statement_t *stmt;
    uint32_t count;
    uint64_t *ids_to_delete;  /* 삭제할 id 배열 (동적 할당) */
    uint32_t ids_cap;          /* 배열 용량 */
    uint32_t ids_len;          /* 현재 수집된 id 수 */
} delete_scan_ctx_t;

/*
 * delete_scan_cb - DELETE의 테이블 스캔 콜백
 *
 * match_predicate()로 조건 검사 후 일치하는 행의 id를 수집한다.
 * 스캔 중 직접 삭제하면 이터레이터가 깨질 수 있으므로 2-pass 방식이다.
 *
 * 1차 (이 콜백): id 수집
 * 2차 (exec_delete_scan): 수집된 id로 일괄 삭제
 */
static bool delete_scan_cb(const uint8_t *row_data, row_ref_t ref, void *ctx)
{
    (void)ref;
    delete_scan_ctx_t *dc = (delete_scan_ctx_t *)ctx;
    const db_header_t *hdr = &dc->pager->header;

    row_value_t values[MAX_COLUMNS];
    row_deserialize(hdr, row_data, values);

    if (!match_predicate(hdr, values, dc->stmt)) {
        return true; /* 조건 불일치, 다음 행으로 계속 */
    }

    /* 배열 용량 부족 시 2배로 확장 (초기 64개) */
    if (dc->ids_len >= dc->ids_cap) {
        dc->ids_cap = dc->ids_cap ? dc->ids_cap * 2 : 64;
        dc->ids_to_delete = realloc(dc->ids_to_delete,
                                    dc->ids_cap * sizeof(uint64_t));
    }
    /* id는 항상 columns[0] (BIGINT) */
    dc->ids_to_delete[dc->ids_len++] = values[0].bigint_val;
    return true;
}

static exec_result_t exec_index_delete(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};

    row_ref_t ref;
    if (bptree_search(pager, stmt->pred_id, &ref) == false) {
        snprintf(res.message, sizeof(res.message), "오류: id=%" PRIu64 "인 행을 찾을 수 없습니다", stmt->pred_id);
        return res;
    }

    /* 힙 삭제 (톰스톤) + B+ tree 삭제 */
    heap_delete(pager, ref);
    bptree_delete(pager, stmt->pred_id);

    pthread_mutex_lock(&pager->header_lock);
    pager->header.row_count--;
    pager->header_dirty = true;
    pthread_mutex_unlock(&pager->header_lock);

    snprintf(res.message, sizeof(res.message), "1행 삭제 완료 (id=%" PRIu64 ")", stmt->pred_id);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DELETE — TABLE_SCAN (2-pass 일괄 삭제)
 *
 *  예시: DELETE FROM users WHERE name = 'Alice'
 *
 *  실행 과정 (2-pass):
 *    1차: heap_scan → 조건 일치하는 행의 id를 배열에 수집
 *         → ids_to_delete = [1, 5, 12]
 *    2차: 수집된 id를 순회하며:
 *         bptree_search(id) → ref 획득
 *         heap_delete(ref)  → 톰스톤 삭제
 *         bptree_delete(id) → 인덱스 삭제
 *
 *  스캔 중 직접 삭제하면 힙 체인 순회가 깨질 수 있으므로 분리한다.
 * ══════════════════════════════════════════════════════════════════════ */
static exec_result_t exec_delete_scan(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    delete_scan_ctx_t dc = {
        .pager = pager, .stmt = stmt, .count = 0,
        .ids_to_delete = NULL, .ids_cap = 0, .ids_len = 0
    };

    /* 1차: 조건에 맞는 id 수집 */
    heap_scan(pager, pager->header.row_size, delete_scan_cb, &dc);

    /* 2차: 수집된 id에 대해 일괄 삭제 */
    for (uint32_t i = 0; i < dc.ids_len; i++) {
        row_ref_t ref;
        if (bptree_search(pager, dc.ids_to_delete[i], &ref)) {
            heap_delete(pager, ref);
            bptree_delete(pager, dc.ids_to_delete[i]);
            pthread_mutex_lock(&pager->header_lock);
            pager->header.row_count--;
            pager->header_dirty = true;
            pthread_mutex_unlock(&pager->header_lock);
            dc.count++;
        }
    }
    free(dc.ids_to_delete);

    snprintf(res.message, sizeof(res.message), "%u행 삭제 완료 (TABLE_SCAN)", dc.count);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  UPDATE — INDEX_UPDATE (단건) / TABLE_SCAN (다건)
 * ══════════════════════════════════════════════════════════════════════ */
static void apply_update_to_row(const db_header_t *hdr, row_value_t *values,
                                 const statement_t *stmt)
{
    int col_idx = find_column_index(hdr, stmt->update_field);
    if (col_idx < 0) return;
    switch (hdr->columns[col_idx].type) {
        case COL_TYPE_INT:
            values[col_idx].int_val = atoi(stmt->update_value);
            break;
        case COL_TYPE_BIGINT:
            values[col_idx].bigint_val = atoll(stmt->update_value);
            break;
        case COL_TYPE_VARCHAR:
            memset(values[col_idx].str_val, 0, 256);
            strncpy(values[col_idx].str_val, stmt->update_value, 255);
            break;
    }
}

static exec_result_t exec_index_update(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;

    row_ref_t ref;
    if (!bptree_search(pager, stmt->pred_id, &ref)) {
        snprintf(res.message, sizeof(res.message),
                 "0행 수정 (id=%" PRIu64 " 없음)", stmt->pred_id);
        return res;
    }

    const uint8_t *row_data = heap_fetch(pager, ref, hdr->row_size);
    if (!row_data) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message), "오류: 행 데이터를 읽지 못했습니다");
        return res;
    }

    row_value_t values[MAX_COLUMNS];
    row_deserialize(hdr, row_data, values);
    pager_unlatch_r(pager, ref.page_id);

    apply_update_to_row(hdr, values, stmt);

    /* 수정된 행을 다시 직렬화하여 힙에 덮어쓰기 (쓰기 래치) */
    uint8_t *new_buf = (uint8_t *)calloc(1, hdr->row_size);
    row_serialize(hdr, values, new_buf);

    uint8_t *page = pager_get_page_wlatch(pager, ref.page_id);
    slot_t slot;
    size_t slot_off = sizeof(heap_page_header_t) + ref.slot_id * sizeof(slot_t);
    memcpy(&slot, page + slot_off, sizeof(slot));
    memcpy(page + slot.offset, new_buf, hdr->row_size);
    pager_mark_dirty(pager, ref.page_id);
    pager_unlatch_w(pager, ref.page_id);
    free(new_buf);

    snprintf(res.message, sizeof(res.message),
             "1행 수정 완료 (id=%" PRIu64 ")", stmt->pred_id);
    return res;
}

/* UPDATE TABLE_SCAN: 조건 일치 행 수집 → 일괄 수정 */
typedef struct {
    pager_t *pager;
    const statement_t *stmt;
    uint32_t count;
    uint64_t *ids;
    uint32_t ids_cap;
    uint32_t ids_len;
} update_scan_ctx_t;

static bool update_scan_cb(const uint8_t *row_data, row_ref_t ref, void *ctx)
{
    (void)ref;
    update_scan_ctx_t *uc = (update_scan_ctx_t *)ctx;
    const db_header_t *hdr = &uc->pager->header;
    row_value_t values[MAX_COLUMNS];
    row_deserialize(hdr, row_data, values);
    if (!match_predicate(hdr, values, uc->stmt)) return true;
    if (uc->ids_len >= uc->ids_cap) {
        uc->ids_cap = uc->ids_cap ? uc->ids_cap * 2 : 64;
        uc->ids = realloc(uc->ids, uc->ids_cap * sizeof(uint64_t));
    }
    uc->ids[uc->ids_len++] = values[0].bigint_val;
    return true;
}

static exec_result_t exec_update_scan(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;
    update_scan_ctx_t uc = {
        .pager = pager, .stmt = stmt, .count = 0,
        .ids = NULL, .ids_cap = 0, .ids_len = 0
    };
    heap_scan(pager, hdr->row_size, update_scan_cb, &uc);

    for (uint32_t i = 0; i < uc.ids_len; i++) {
        row_ref_t ref;
        if (!bptree_search(pager, uc.ids[i], &ref)) continue;

        const uint8_t *row_data = heap_fetch(pager, ref, hdr->row_size);
        if (!row_data) continue;

        row_value_t values[MAX_COLUMNS];
        row_deserialize(hdr, row_data, values);
        pager_unlatch_r(pager, ref.page_id);

        apply_update_to_row(hdr, values, stmt);

        uint8_t *new_buf = (uint8_t *)calloc(1, hdr->row_size);
        row_serialize(hdr, values, new_buf);

        uint8_t *page = pager_get_page_wlatch(pager, ref.page_id);
        size_t slot_off = sizeof(heap_page_header_t) + ref.slot_id * sizeof(slot_t);
        slot_t slot;
        memcpy(&slot, page + slot_off, sizeof(slot));
        memcpy(page + slot.offset, new_buf, hdr->row_size);
        pager_mark_dirty(pager, ref.page_id);
        pager_unlatch_w(pager, ref.page_id);
        free(new_buf);
        uc.count++;
    }
    free(uc.ids);
    pager->header_dirty = true;

    snprintf(res.message, sizeof(res.message), "%u행 수정 완료 (TABLE_SCAN)", uc.count);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DROP TABLE
 * ══════════════════════════════════════════════════════════════════════ */
static exec_result_t exec_drop_table(pager_t *pager, statement_t *stmt)
{
    exec_result_t res = {0, "", NULL, 0};
    db_header_t *hdr = &pager->header;

    if (hdr->column_count == 0) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message),
                 "오류: '%s' 테이블이 존재하지 않습니다", stmt->table_name);
        return res;
    }

    /* 모든 heap/leaf/internal 페이지를 free list로 반환
     * DDL(wrlock)이므로 다른 DML이 없어 래치 불필요 */
    for (uint32_t i = 1; i < hdr->next_page_id; i++) {
        uint8_t *page = pager_get_page(pager, i);
        uint32_t ptype;
        memcpy(&ptype, page, sizeof(uint32_t));
        pager_unpin(pager, i);
        if (ptype != PAGE_TYPE_FREE) {
            pager_free_page(pager, i);
        }
    }

    /* 새 빈 힙 페이지 + B+ tree 루트 리프 할당 (free list에서 재활용)
     * DDL wrlock 아래이므로 래치 불필요 */
    uint32_t new_heap = pager_alloc_page(pager);
    {
        uint8_t *page = pager_get_page(pager, new_heap);
        heap_page_header_t hph = {
            .page_type = PAGE_TYPE_HEAP, .next_heap_page_id = 0,
            .slot_count = 0, .free_slot_head = SLOT_NONE,
            .free_space_offset = 0, .reserved = 0
        };
        memcpy(page, &hph, sizeof(hph));
        pager_mark_dirty(pager, new_heap);
        pager_unpin(pager, new_heap);
    }
    uint32_t new_root = pager_alloc_page(pager);
    {
        uint8_t *page = pager_get_page(pager, new_root);
        leaf_page_header_t lph = {
            .page_type = PAGE_TYPE_LEAF, .parent_page_id = 0,
            .key_count = 0, .next_leaf_page_id = 0,
            .prev_leaf_page_id = 0
        };
        memcpy(page, &lph, sizeof(lph));
        pager_mark_dirty(pager, new_root);
        pager_unpin(pager, new_root);
    }

    /* 스키마 초기화 */
    hdr->column_count = 0;
    hdr->row_size = 0;
    hdr->row_count = 0;
    hdr->next_id = 1;
    hdr->first_heap_page_id = new_heap;
    hdr->root_index_page_id = new_root;
    memset(hdr->columns, 0, sizeof(hdr->columns));
    pager->last_heap_page_id = new_heap;
    pager->header_dirty = true;

    snprintf(res.message, sizeof(res.message),
             "'%s' 테이블 삭제 완료", stmt->table_name);
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  EXPLAIN
 *
 *  예시: EXPLAIN SELECT * FROM users WHERE id = 3
 *
 *  출력:
 *    Access Path: INDEX_LOOKUP
 *      Index: B+ Tree (id)
 *      Target: id = 3
 *
 *  실제 데이터 조작은 수행하지 않고 실행 계획만 출력한다.
 * ══════════════════════════════════════════════════════════════════════ */
static exec_result_t exec_explain(pager_t *pager, statement_t *stmt)
{
    (void)pager;
    exec_result_t res = {0, "", NULL, 0};
    plan_t plan = planner_create_plan(stmt);

    out_buf_t buf;
    buf_init(&buf);
    buf_append(&buf, "Access Path: %s\n", access_path_name(plan.access_path));

    if (plan.access_path == ACCESS_PATH_INDEX_LOOKUP) {
        buf_append(&buf, "  Index: B+ Tree (id)\n");
        buf_append(&buf, "  Target: id = %" PRIu64 "\n", stmt->pred_id);
    } else if (plan.access_path == ACCESS_PATH_INDEX_DELETE) {
        buf_append(&buf, "  Index: B+ Tree (id)\n");
        buf_append(&buf, "  Delete: id = %" PRIu64 "\n", stmt->pred_id);
    } else if (plan.access_path == ACCESS_PATH_INDEX_UPDATE) {
        buf_append(&buf, "  Index: B+ Tree (id)\n");
        buf_append(&buf, "  Update: id = %" PRIu64 "\n", stmt->pred_id);
    } else if (plan.access_path == ACCESS_PATH_DROP_TABLE) {
        buf_append(&buf, "  Drop: table '%s'\n", stmt->table_name);
    } else if (plan.access_path == ACCESS_PATH_TABLE_SCAN) {
        if (stmt->predicate_kind == PREDICATE_FIELD_EQ) {
            buf_append(&buf, "  Filter: %s = '%s'\n", stmt->pred_field, stmt->pred_value);
        } else if (stmt->predicate_kind == PREDICATE_FIELD_CMP) {
            buf_append(&buf, "  Filter: %s (comparison)\n", stmt->pred_field);
        }
        buf_append(&buf, "  Scan: all heap pages\n");
    }
    buf_transfer(&buf, &res);

    snprintf(res.message, sizeof(res.message), "실행 계획 출력 완료");
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 *  메인 디스패치
 *
 *  execute()는 플래너에서 접근 경로를 결정하고, 해당 실행 함수로 분기한다.
 *
 *  흐름:
 *    execute(stmt)
 *      → EXPLAIN이면 exec_explain()
 *      → 아니면 planner_create_plan(stmt)
 *      → switch(access_path):
 *          CREATE_TABLE → exec_create_table()
 *          INSERT       → exec_insert()
 *          INDEX_LOOKUP → exec_index_lookup()
 *          TABLE_SCAN   → exec_table_scan() 또는 exec_delete_scan()
 *          INDEX_DELETE → exec_index_delete()
 * ══════════════════════════════════════════════════════════════════════ */
exec_result_t execute(pager_t *pager, statement_t *stmt)
{
    /* EXPLAIN은 별도 처리 */
    if (stmt->type == STMT_EXPLAIN) {
        return exec_explain(pager, stmt);
    }

    plan_t plan = planner_create_plan(stmt);

    switch (plan.access_path) {
        case ACCESS_PATH_CREATE_TABLE:
            return exec_create_table(pager, stmt);
        case ACCESS_PATH_INSERT:
            return exec_insert(pager, stmt);
        case ACCESS_PATH_INDEX_LOOKUP:
            return exec_index_lookup(pager, stmt);
        case ACCESS_PATH_TABLE_SCAN:
            if (stmt->type == STMT_DELETE)
                return exec_delete_scan(pager, stmt);
            if (stmt->type == STMT_UPDATE)
                return exec_update_scan(pager, stmt);
            return exec_table_scan(pager, stmt);
        case ACCESS_PATH_INDEX_DELETE:
            return exec_index_delete(pager, stmt);
        case ACCESS_PATH_INDEX_UPDATE:
            return exec_index_update(pager, stmt);
        case ACCESS_PATH_DROP_TABLE:
            return exec_drop_table(pager, stmt);
    }

    exec_result_t res = { -1, "오류: 알 수 없는 접근 경로입니다", NULL, 0 };
    return res;
}
