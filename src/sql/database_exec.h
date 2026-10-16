#ifndef DATABASE_EXEC_H
#define DATABASE_EXEC_H

/*
 * database 실행기 파일들(database*.c)이 함께 쓰는 내부 타입과 함수(#198). 공개 계약은 include/sql/database.h이고,
 * 이 헤더는 src/sql 밖에서 쓰지 않는다. 한 파일 안에서만 쓰는 함수는 그 파일에 static으로 둔다.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sql/database.h"
#include "database_internal.h"
#include "sql/sql_eval.h"
#include "sql/sql_statement.h"
#include "storage/catalog.h"
#include "storage/catalog_store.h"
#include "storage/heap_store.h"
#include "storage/overflow.h"
#include "storage/page_file.h"
#include "storage/text_tree.h"

typedef struct {
    result_error_class_t error_class;
    int code;
    char message[256];
} pending_error_t;

typedef struct {
    uint32_t *items;
    size_t len;
    size_t cap;
} page_list_t;

/* 쓰기 작업의 상태. database_internal.h의 설명을 따른다 */
typedef struct {
    bool active;
    bool failed;
    bool inside;                 /* 지금 실행 중인 문장이 이 작업의 문장이다(작업 카탈로그 기준으로 읽는다) */
    catalog_t *cat;              /* 작업 카탈로그. NULL이면 바꾼 것이 없다 */
    page_list_t old_committed;   /* 게시된 상태가 가리키고 작업이 대체한 페이지. 게시 성공 뒤에만 놓는다 */
    size_t res_cat;              /* resident로 계상한 작업 카탈로그 몫 */
    size_t res_lists;            /* resident로 계상한 위 목록들의 상한 */
} work_state_t;

struct database {
    page_file_t *file;
    catalog_t *catalog;   /* 게시된 카탈로그. 문장이 성공했을 때만 교체한다 */
    bool writable;
    bool poisoned;
    /* 상주 메모리 회계(attach된 경우) */
    mem_ledger_t resident;
    size_t res_fixed;
    size_t res_catalog;
    size_t res_lists;
    /* 실행 중 문맥. 핸들은 단일 스레드라 한 번에 한 문장만 쓴다 */
    mem_ledger_t ledger;
    const database_cancel_t *cancel;
    pending_error_t pending;
    /* 쓰기 작업과 transaction 계층 */
    work_state_t work;
    bool read_only_call;
    database_control_t *control_sink;
    const database_layer_t *layer;
    void *layer_ctx;
    heap_stats_t stats;   /* 문장이 연 heap들이 올리는 누적 계수기 */
    /* 문장 동안 쥐는 서브쿼리·WITH·FROM 서브쿼리 결과와 보이는 WITH 이름 */
    struct relation *relations;
    struct correlated *correlated;
    struct cte_scope *ctes;
    struct outer_scope *outer;        /* 서브쿼리를 컴파일할 때 보이는 바깥 질의 범위(안쪽이 위) */
    const struct resolver_s *compile_resolver;  /* 지금 컴파일하는 식의 열 해석기. 바깥 범위가 된다 */
    database_exec_status_t subquery_status;  /* 서브쿼리 실행이 실패한 결과(평가기 오류에서 돌려준다) */
};

#define NO_COLUMN CATALOG_NO_COLUMN

typedef struct {
    catalog_t *cat;
    size_t charged;  /* 이 카탈로그에 대해 reserve해 둔 바이트 */
} work_t;

/* FROM 원본 하나. 결합 행에서 이 원본의 열은 offset부터 view->column_count칸이다 */
typedef struct {
    const catalog_table_view_t *view;
    const sqlstmt_name_t *table;
    const sqlstmt_name_t *alias;       /* 없으면 NULL이나 size 0 */
    size_t offset;
    bool nullable;                     /* LEFT JOIN 쪽이라 NOT NULL 열도 NULL이 될 수 있다 */
} source_ref_t;

/* 열 이름 해석. 앞의 count개 원본만 본다(ON은 자기 원본까지만 본다). qualifier는 원본의 별칭이나 이름이다 */
typedef struct resolver_s {
    const source_ref_t *sources;
    size_t count;
} resolver_t;

/*
 * 질의 결과를 문장이 끝날 때까지 쥔다. key는 결과를 만든 문법 노드(sqlexpr_t·sqlstmt_source_t·sqlstmt_cte_t)이며
 * 같은 노드는 한 번만 실행한다. 바깥 행에 기대지 않으므로 한 번 계산하면 된다.
 */
typedef struct relation {
    const void *key;
    catalog_column_t *columns;
    size_t column_count;
    char *names;
    size_t names_bytes;
    value_t *cells;             /* row_count × column_count */
    size_t row_count;
    size_t row_cap;
    sqleval_subquery_t info;    /* 식 자리 서브쿼리로 쓸 때 */
    value_view_t *set;          /* IN ( query )의 정렬된 값. 처음 필요할 때 만든다 */
    size_t set_bytes;
    struct relation *next;
} relation_t;

/* 보이는 WITH 이름: owner의 ctes[0..visible) */
typedef struct cte_scope {
    const sqlstmt_select_t *owner;
    size_t visible;
    struct cte_scope *parent;
} cte_scope_t;

typedef int (*row_compare_fn)(const void *ctx, const value_t *a, const value_t *b);

/*
 * 바깥 질의 범위. 서브쿼리 안의 열 이름이 자기 FROM에 없으면 여기서 찾고, 값은 params[바깥 칸]에서 읽는다.
 * resolver가 NULL이면 바깥 열을 볼 수 없다(그룹 질의의 출력·FROM 서브쿼리·WITH 등).
 */
typedef struct outer_scope {
    const resolver_t *resolver; /* NULL이거나 own을 가리킨다 */
    resolver_t own;             /* 컴파일한 식의 해석기 복사본(원본 배열은 질의가 끝날 때까지 산다) */
    value_t *params;            /* 바깥 행의 칸마다 빌린 값 */
    bool *used;
    size_t width;
    bool correlated;            /* 안쪽이 이 범위나 더 바깥 범위의 열을 썼다 */
    struct outer_scope *parent;
} outer_scope_t;

/* 바깥 열을 보는 서브쿼리. 바깥 행마다 params를 채우고 다시 실행한다 */
typedef struct correlated {
    database_t *db;
    const sqlexpr_t *expr;
    outer_scope_t scope;
    relation_t rel;
    size_t *columns;            /* 바깥 행에서 읽는 칸 */
    size_t column_count;
    value_t *last;              /* 마지막으로 실행한 바깥 값(같으면 다시 실행하지 않는다) */
    bool ran;
    struct correlated *next;
} correlated_t;

/* 컬럼 = 상수 꼴의 조건(색인 탐침 후보). literal은 컬럼 형식으로 정확히 바꾼 값이다 */
#define WHERE_MAX_PROBES 8

typedef struct {
    size_t column;
    value_t literal;
} probe_t;

/* 정수 컬럼 비교(<, <=, >, >=)를 모은 닫힌 구간 [low, high]. 같은 컬럼의 조건은 교집합으로 좁힌다 */
typedef struct {
    size_t column;
    int64_t low, high;
} range_t;

typedef struct {
    bool present;
    sqleval_prog_t *prog;
    bool never;                        /* 최상위 AND 안의 등가 조건이 어떤 행과도 맞을 수 없다 */
    probe_t probes[WHERE_MAX_PROBES];
    size_t probe_count;
    range_t ranges[WHERE_MAX_PROBES];
    size_t range_count;
} where_t;

/* RETURNING: 바뀐 행마다 목록을 계산해 결과 행으로 모은다. 목록이 없으면 명령 결과를 만든다 */
typedef struct returning returning_t;

/* 선택 목록의 출력 열 하나. prog가 NULL이면 테이블 열 column을 그대로 낸다 */
typedef struct {
    sqleval_prog_t *prog;
    size_t column;
    result_column_t meta;
} select_out_t;

/* 정렬 키. slot은 행 안의 자리: prog가 NULL이면 출력 열(ORDER BY 번호·별칭), 아니면 출력 열 뒤의 식 자리 */
typedef struct {
    sqleval_prog_t *prog;
    size_t slot;
    bool desc;
    bool nulls_first;
} select_key_t;

typedef struct {
    database_t *db;
    select_out_t *outs;
    size_t out_count;
    size_t outs_bytes;
    select_key_t *keys;
    size_t key_count;
    size_t extra_count;         /* 식 정렬 키 수(행마다 출력 열 뒤에 둔다) */
    size_t keys_bytes;
    result_column_t *columns;   /* out_count개 결과 열 */
    bool distinct;
    bool buffered;              /* ORDER BY·DISTINCT는 모아서 정렬한다 */
    bool has_limit;
    uint64_t limit;
    uint64_t offset;
    uint64_t skipped;           /* 스트리밍에서 OFFSET으로 건너뛴 행 */
    uint64_t emitted;
    bool done;                  /* LIMIT을 채웠다. 더 읽지 않는다 */
    result_builder_t *builder;
    value_t *cells;             /* out_count개. 스트리밍 행의 계산값 */
    value_view_t *views;
    value_t **rows;             /* 모은 행. 행마다 out_count + key_count개 값 */
    size_t row_count;
    size_t row_cap;
} select_ctx_t;

typedef struct {
    database_exec_status_t (*fn)(void *ctx, const value_t *row);
    void *ctx;
    const bool *stop;           /* 참이 되면 더 넘기지 않는다(LIMIT을 채움) */
} row_sink_t;

typedef struct {
    sqleval_resolve_fn resolve;
    sqleval_match_fn match;
    void *ctx;
    const resolver_t *table;    /* FROM 원본들. 열 그대로 내보내기·*에 쓴다 */
} compile_ctx_t;

typedef enum { AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX } agg_kind_t;

typedef struct {
    const sqlexpr_t *call;
    agg_kind_t kind;
    sqleval_prog_t *arg;        /* COUNT(*)는 NULL */
    value_type_t type;          /* 결과 형식 */
} aggregate_t;

/* 그룹 행 배치: [GROUP BY 값 key_count개][집계 값 agg_count개] */
typedef struct {
    const resolver_t *table;
    const sqlexpr_t **keys;
    sqleval_prog_t **key_progs;
    value_type_t *key_types;
    size_t key_count;
    aggregate_t *aggs;
    size_t agg_count;
    size_t agg_cap;
    size_t keys_bytes;
} group_plan_t;

/*
 * 해시 집계. 그룹마다 [GROUP BY 값][집계 결과] 칸과 집계 누적 상태만 두므로 메모리는 입력 행 수가 아니라 그룹 수에
 * 비례한다. DISTINCT 집계만 그룹 안의 NULL 아닌 값을 모아 끝에서 중복을 없앤다. 그룹 키의 같음은 sqleval_compare와
 * 같다(1과 1.0은 같은 그룹, NULL끼리 한 그룹).
 */
typedef struct {
    int64_t present;            /* NULL 아닌 인자 수(COUNT(*)는 행 수) */
    __int128 whole;             /* 정수 합. 행마다 int64라 2^63행까지 넘치지 않는다 */
    double real;
    value_t best;               /* MIN·MAX 후보 */
    value_t *seen;              /* DISTINCT 집계의 값 */
    size_t seen_count, seen_cap;
} agg_state_t;

typedef struct {
    uint64_t hash;
    value_t *cells;             /* group_width칸. 키는 모을 때, 집계 결과는 내보낼 때 채운다 */
    agg_state_t *states;
} group_t;

typedef struct {
    database_t *db;
    const group_plan_t *plan;
    group_t **slots;            /* 열린 주소법 표. 크기는 2의 거듭제곱 */
    size_t slot_cap;
    group_t **groups;           /* 만든 순서 */
    size_t count;
    size_t cap;
    value_t *keys;              /* 이번 행의 키 */
} group_rows_t;

/* 최상위 AND로 나눈 조건 하나. level 단계에서 행을 받을 때 본다. after면 LEFT JOIN의 NULL 채움 뒤에 본다 */
typedef struct {
    sqleval_prog_t *prog;
    size_t level;
    bool after;
} join_cond_t;

/* 이 단계 원본의 열 = 앞 단계만 쓰는 식 */
typedef struct {
    size_t column;              /* 원본 안 열 번호 */
    value_type_t type;
    sqleval_prog_t *value;
    sqlexpr_op_t op;            /* 열 쪽에서 본 비교. EQ면 등가 탐침, 대소 비교면 정수 구간 */
} join_probe_t;

typedef struct join join_t;

typedef struct {
    join_t *join;
    size_t level;               /* FROM 순서의 원본 번호 */
    size_t pos;                 /* 실행 순서. RIGHT JOIN이면 첫 두 원본이 바뀐다 */
    catalog_table_id_t id;
    catalog_table_view_t view;
    heap_t *heap;
    relation_t *relation;       /* WITH·FROM 서브쿼리 결과면 heap 대신 이것을 훑는다 */
    bool left;
    bool matched;               /* 이번 바깥 행에서 ON을 통과한 행이 있었는지 */
    join_probe_t probes[WHERE_MAX_PROBES];
    size_t probe_count;
    row_sink_t sink;
} join_level_t;

struct join {
    database_t *db;
    join_level_t *levels;       /* FROM 순서 */
    size_t *order;              /* 실행 순서 → 원본 번호 */
    size_t level_count;
    source_ref_t *sources;
    join_cond_t *conds;
    size_t cond_count;
    size_t cond_cap;
    value_t *row;               /* 결합 행 */
    size_t width;
    bool *needed;               /* 결합 행에서 읽을 칸 */
    bool *scratch;              /* 조건이 쓰는 칸 표시용 */
    const row_sink_t *out;
};

struct returning {
    select_ctx_t ctx;
    source_ref_t source;
    resolver_t resolver;
};

/* database.c */
bool q_reserve(database_t *db, size_t bytes);
void q_release(database_t *db, size_t bytes);
void *q_malloc(database_t *db, size_t bytes);
void *q_calloc(database_t *db, size_t count, size_t size);
void q_free(database_t *db, void *block, size_t bytes);
bool cancel_requested(const database_t *db);
size_t value_bytes(const value_t *value);
void owned_clear(database_t *db, value_t *cells, size_t count);
value_status_t q_cast(database_t *db, const value_view_t *source, value_type_t type, value_t *out);
value_status_t q_clone(database_t *db, const value_view_t *source, value_t *out);
const catalog_t *base_catalog(const database_t *db);
database_exec_status_t fail_result(database_t *db, result_error_class_t error_class, int code,
                                          const char *format, ...);
int name_len(size_t size);
database_exec_status_t heap_failure(database_t *db, heap_status_t status, const char *where);
database_exec_status_t cancelled_result(database_t *db);
void clear_cells(value_t *cells, size_t count);
bool lookup_table(database_t *db, const sqlstmt_name_t *name, catalog_table_id_t *id,
                         catalog_table_view_t *view, database_exec_status_t *status);
size_t find_column(const catalog_table_view_t *view, const sqlstmt_name_t *name);
database_exec_status_t unknown_column(database_t *db, const sqlstmt_name_t *name);
heap_status_t open_heap(database_t *db, const catalog_table_view_t *view,
                               catalog_table_id_t id, heap_t **heap);
heap_status_t configure_secondary(heap_batch_t *batch, const catalog_t *catalog,
                                         catalog_table_id_t table_id);
void work_close(database_t *db, work_t *work);
bool work_clone(database_t *db, work_t *work);
void work_settle(database_t *db, work_t *work, size_t peak);
database_exec_status_t publish(database_t *db, work_t *work, catalog_table_id_t id,
                                      heap_batch_t *batch,
                                      const uint32_t *drop_pages, size_t drop_count,
                                      result_t *success, result_t **out);
result_status_t open_command(database_t *db, const char *tag, uint64_t affected, result_t **result);

/* database_expr.c */
bool source_named(const source_ref_t *source, const sqlstmt_name_t *name);
size_t resolve_column(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name,
                             value_type_t *type);
size_t source_of(const resolver_t *r, size_t column);
database_exec_status_t eval_failure(database_t *db, const sqleval_error_t *error);
bool compile_expr(database_t *db, const sqlexpr_t *expr, const resolver_t *resolver, sqleval_prog_t **prog,
                         database_exec_status_t *status);
void where_clear(database_t *db, where_t *where);
bool cast_for_column(database_t *db, const value_view_t *source, const catalog_column_t *column,
                            value_t *target, database_exec_status_t *status);
bool values_equal(const value_view_t *a, const value_view_t *b);
sqlexpr_op_t flip_comparison(sqlexpr_op_t op);
void narrow_range(where_t *where, size_t index, sqlexpr_op_t op, int64_t bound);
bool compile_where(database_t *db, const resolver_t *resolver, const sqlexpr_t *source, where_t *where,
                          database_exec_status_t *status);
database_exec_status_t where_matches(database_t *db, const where_t *where, const value_t *row, bool *matched);
bool probe_holds(const probe_t *probe, const value_t *row);
bool compile_in(database_t *db, const sqlexpr_t *expr, const compile_ctx_t *cc, sqleval_prog_t **prog,
                       database_exec_status_t *status);
database_exec_status_t plan_outputs(database_t *db, const sqlstmt_select_item_t *items, size_t item_count,
                                           const compile_ctx_t *cc, select_ctx_t *ctx);
database_exec_status_t plan_keys(database_t *db, const sqlstmt_select_t *select, const compile_ctx_t *cc,
                                        select_ctx_t *ctx);

/* database_subquery.c */
void relations_free(database_t *db);
database_exec_status_t relation_build(database_t *db, const void *key, const sqlstmt_select_t *query,
                                             const sqlstmt_cte_t *cte, relation_t **out);
bool outer_hook(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name, value_type_t *type,
                       const value_t **cell);
sqleval_status_t subquery_hook(void *ctx, const sqlexpr_t *expr, const sqleval_subquery_t **out);
database_exec_status_t cte_relation(database_t *db, const sqlstmt_name_t *name, relation_t **out);

/* database_ddl.c */
database_exec_status_t exec_create(database_t *db, const sqlstmt_create_t *create, result_t **out);
database_exec_status_t exec_create_index(database_t *db, const sqlstmt_index_t *index, result_t **out);
database_exec_status_t exec_drop(database_t *db, const sqlstmt_drop_t *drop, result_t **out);
database_exec_status_t probe_key_index(database_t *db, const catalog_table_view_t *view, heap_t *heap,
                                              const value_t *key, bool check_pk, bool need_max,
                                              bool *any, int64_t *max_key);

/* database_insert.c */
database_exec_status_t duplicate_key(database_t *db, const catalog_table_view_t *view);
database_exec_status_t not_null_violation(database_t *db, const catalog_column_t *column);
bool eval_constant(database_t *db, const sqlexpr_t *expr, value_t *out, database_exec_status_t *status);
database_exec_status_t exec_insert(database_t *db, const sqlstmt_insert_t *insert, result_t **out);

/* database_select.c */
database_exec_status_t select_prepare(select_ctx_t *ctx);
void select_ctx_free(select_ctx_t *ctx);
database_exec_status_t select_value(database_t *db, const sqleval_prog_t *prog, size_t column,
                                           const value_t *row, value_t *out);
database_exec_status_t select_emit(select_ctx_t *ctx, const value_t *row);
database_exec_status_t select_flush(select_ctx_t *ctx);
bool constant_count(database_t *db, const sqlexpr_t *expr, bool *present, uint64_t *value,
                           database_exec_status_t *status);
size_t key_probe(const catalog_table_view_t *view, const heap_t *heap, const where_t *where);
size_t key_range(const catalog_table_view_t *view, const heap_t *heap, const where_t *where);
size_t secondary_probe(database_t *db, catalog_table_id_t table_id, const where_t *where,
                              heap_secondary_spec_t *spec);
database_exec_status_t locate_key(database_t *db, heap_t *heap, const value_view_t *key, bool *is_hit,
                                         heap_locator_t *at);
database_exec_status_t scan_rows(database_t *db, const catalog_table_view_t *table, catalog_table_id_t id,
                                        heap_t *heap, const where_t *where, const bool *needed, value_t *row,
                                        const row_sink_t *sink);
database_exec_status_t exec_select(database_t *db, const sqlstmt_select_t *select, result_t **out);

/* database_group.c */
size_t group_match(void *ctx, const sqlexpr_t *expr, value_type_t *type);
size_t group_resolve(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name, value_type_t *type);
void group_plan_free(database_t *db, group_plan_t *plan);
database_exec_status_t plan_groups(database_t *db, const sqlstmt_select_t *select, const resolver_t *table,
                                          group_plan_t *plan);
database_exec_status_t group_collect(void *context, const value_t *row);
void group_rows_free(group_rows_t *rows);
void sort_rows(value_t **rows, value_t **scratch, size_t count, row_compare_fn cmp, const void *ctx);
int compare_first_value(const void *ctx, const value_t *a, const value_t *b);
database_exec_status_t group_flush(database_t *db, const group_plan_t *plan, group_rows_t *rows,
                                          const sqleval_prog_t *having, select_ctx_t *ctx);
database_exec_status_t emit_sink(void *context, const value_t *row);
bool select_is_grouped(const sqlstmt_select_t *select);

/* database_join.c */
database_exec_status_t select_run(database_t *db, const sqlstmt_select_t *select, bool core_only,
                                         select_ctx_t *ctx);
database_exec_status_t select_finish(select_ctx_t *ctx, result_t **out);

/* database_update.c */
database_exec_status_t returning_open(database_t *db, const sqlstmt_returning_t *spec,
                                             const catalog_table_view_t *view, const sqlstmt_name_t *table,
                                             returning_t **out);
database_exec_status_t returning_row(returning_t *ret, const value_t *row);
database_exec_status_t returning_finish(database_t *db, returning_t *ret, const char *tag, uint64_t affected,
                                               result_t **success);
void returning_close(database_t *db, returning_t *ret);
database_exec_status_t exec_update(database_t *db, const sqlstmt_update_t *update, result_t **out);
database_exec_status_t exec_delete(database_t *db, const sqlstmt_delete_t *del, result_t **out);

#endif
