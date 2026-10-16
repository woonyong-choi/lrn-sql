/*
 * database_subquery.c — 서브쿼리(스칼라·EXISTS·IN), WITH, FROM 서브쿼리의 결과 구체화와 상관 서브쿼리 재실행.
 */
#include "database_exec.h"

static void correlated_free_all(database_t *db);

/* ── 서브쿼리·WITH·FROM 서브쿼리 결과 ── */

/* 결과 내용을 비운다. key·next·refresh는 둔다 */
static void relation_clear(database_t *db, relation_t *rel)
{
    if (rel->cells != NULL) owned_clear(db, rel->cells, rel->row_count * rel->column_count);
    q_free(db, rel->cells, rel->row_cap * rel->column_count * sizeof(value_t));
    if (rel->columns != NULL) q_free(db, rel->columns, (rel->column_count > 0 ? rel->column_count : 1) * sizeof(catalog_column_t));
    q_free(db, rel->names, rel->names_bytes);
    q_free(db, rel->set, rel->set_bytes);
    rel->cells = NULL;
    rel->columns = NULL;
    rel->names = NULL;
    rel->set = NULL;
    rel->column_count = rel->row_count = rel->row_cap = rel->names_bytes = rel->set_bytes = 0;
    rel->info.first = rel->info.set = NULL;
    rel->info.row_count = rel->info.set_count = 0;
    rel->info.set_has_null = false;
}

static void relation_free(database_t *db, relation_t *rel)
{
    relation_clear(db, rel);
    q_free(db, rel, sizeof(*rel));
}

void relations_free(database_t *db)
{
    while (db->relations != NULL) {
        relation_t *next = db->relations->next;
        relation_free(db, db->relations);
        db->relations = next;
    }
    correlated_free_all(db);
}

static relation_t *relation_find(const database_t *db, const void *key)
{
    for (relation_t *rel = db->relations; rel != NULL; rel = rel->next) {
        if (rel->key == key) return rel;
    }
    return NULL;
}

static database_exec_status_t relation_add_row(database_t *db, relation_t *rel, const result_row_t *row)
{
    size_t n = rel->column_count;
    if (rel->row_count == rel->row_cap) {
        size_t cap = rel->row_cap == 0 ? 16 : rel->row_cap * 2;
        if (n > 0 && cap > SIZE_MAX / n / sizeof(value_t)) return DATABASE_EXEC_NOMEM;
        size_t bytes = cap * n * sizeof(value_t), old = rel->row_cap * n * sizeof(value_t);
        if (!q_reserve(db, bytes)) return DATABASE_EXEC_NOMEM;
        value_t *grown = realloc(rel->cells, bytes > 0 ? bytes : 1);
        if (grown == NULL) {
            q_release(db, bytes);
            return DATABASE_EXEC_NOMEM;
        }
        q_release(db, old);
        memset(grown + rel->row_cap * n, 0, (cap - rel->row_cap) * n * sizeof(value_t));
        rel->cells = grown;
        rel->row_cap = cap;
    }
    value_t *cells = rel->cells + rel->row_count * n;
    for (size_t c = 0; c < n; c++) {
        if (q_clone(db, &row->cells[c], &cells[c]) != VALUE_OK) {
            owned_clear(db, cells, c);
            return DATABASE_EXEC_NOMEM;
        }
    }
    rel->row_count++;
    return DATABASE_EXEC_OK;
}

/* query를 실행해 빈 rel을 채운다. cte가 열 이름을 주면 그 이름을 쓴다. 실패하면 rel은 relation_clear로 비운다 */
static database_exec_status_t relation_fill(database_t *db, const sqlstmt_select_t *query, const sqlstmt_cte_t *cte,
                                            relation_t *rel)
{
    if (cancel_requested(db)) return cancelled_result(db);
    result_t *result = NULL;
    database_exec_status_t status = exec_select(db, query, &result);
    if (status != DATABASE_EXEC_OK) return status;
    size_t n = result_column_count(result);
    rel->column_count = n;
    if (cte != NULL && cte->column_count > 0 && cte->column_count != n) {
        result_release(&result);
        return fail_result(db, RESULT_E_SEMANTIC, DATABASE_CODE_SUBQUERY, "WITH %.*s의 열 이름 수(%zu)가 결과 열 수(%zu)와 다릅니다",
                           name_len(cte->name.size), (const char *)cte->name.bytes, cte->column_count, n);
    }
    for (size_t c = 0; c < n; c++) {
        bool renamed = cte != NULL && cte->column_count > 0;
        rel->names_bytes += renamed ? cte->columns[c].size : result_column(result, c)->name_len;
    }
    rel->columns = q_calloc(db, n > 0 ? n : 1, sizeof(catalog_column_t));
    rel->names = rel->names_bytes > 0 ? q_malloc(db, rel->names_bytes) : NULL;
    if (rel->columns == NULL || (rel->names_bytes > 0 && rel->names == NULL)) {
        result_release(&result);
        return DATABASE_EXEC_NOMEM;
    }
    size_t used = 0;
    for (size_t c = 0; c < n; c++) {
        const result_column_t *meta = result_column(result, c);
        const uint8_t *name = meta->name;
        size_t length = meta->name_len;
        if (cte != NULL && cte->column_count > 0) {
            name = cte->columns[c].bytes;
            length = cte->columns[c].size;
        }
        if (length > 0) memcpy(rel->names + used, name, length);
        rel->columns[c] = (catalog_column_t){rel->names + used, length, meta->type, !meta->is_nullable, false, {0}};
        used += length;
    }
    result_row_t row;
    result_next_t next;
    while ((next = result_next(result, &row)) == RESULT_NEXT_ROW && status == DATABASE_EXEC_OK) {
        status = relation_add_row(db, rel, &row);
    }
    if (status == DATABASE_EXEC_OK && next != RESULT_NEXT_DONE) {
        status = next == RESULT_NEXT_CANCELLED ? cancelled_result(db) : DATABASE_EXEC_NOMEM;
    }
    result_release(&result);
    if (status != DATABASE_EXEC_OK) return status;
    rel->info.type = n > 0 ? rel->columns[0].type : VALUE_NULL;
    rel->info.column_count = n;
    rel->info.row_count = rel->row_count;
    rel->info.first = rel->row_count > 0 && n > 0 ? &rel->cells[0].view : NULL;
    return DATABASE_EXEC_OK;
}

/* query를 실행해 key의 결과로 문장 끝까지 쥔다(같은 key는 한 번만). 바깥 열은 보지 않는다 */
database_exec_status_t relation_build(database_t *db, const void *key, const sqlstmt_select_t *query,
                                             const sqlstmt_cte_t *cte, relation_t **out)
{
    *out = relation_find(db, key);
    if (*out != NULL) return DATABASE_EXEC_OK;
    relation_t *rel = q_calloc(db, 1, sizeof(*rel));
    if (rel == NULL) return DATABASE_EXEC_NOMEM;
    rel->key = key;
    rel->next = db->relations;
    db->relations = rel;
    struct outer_scope *outer = db->outer;
    db->outer = NULL;
    database_exec_status_t status = relation_fill(db, query, cte, rel);
    db->outer = outer;
    if (status == DATABASE_EXEC_OK) *out = rel;
    return status;
}

/* IN ( query )용 정렬·중복 제거한 첫 열 값 */
static database_exec_status_t relation_set(database_t *db, relation_t *rel)
{
    if (rel->set != NULL || rel->row_count == 0) return DATABASE_EXEC_OK;
    size_t n = rel->column_count;
    rel->set_bytes = rel->row_count * sizeof(value_view_t);
    rel->set = q_malloc(db, rel->set_bytes);
    size_t scratch_bytes = rel->row_count * sizeof(value_t *);
    value_t **order = q_malloc(db, scratch_bytes);
    value_t **scratch = q_malloc(db, scratch_bytes);
    database_exec_status_t status = DATABASE_EXEC_OK;
    if (rel->set == NULL || order == NULL || scratch == NULL) {
        status = DATABASE_EXEC_NOMEM;
    } else {
        size_t count = 0;
        for (size_t r = 0; r < rel->row_count; r++) {
            value_t *cell = &rel->cells[r * n];
            if (cell->view.type == VALUE_NULL) rel->info.set_has_null = true;
            else order[count++] = cell;
        }
        sort_rows(order, scratch, count, compare_first_value, NULL);
        for (size_t i = 0; i < count; i++) {
            if (rel->info.set_count == 0 ||
                sqleval_compare(&rel->set[rel->info.set_count - 1], &order[i]->view) != 0) {
                rel->set[rel->info.set_count++] = order[i]->view;
            }
        }
        rel->info.set = rel->set;
    }
    q_free(db, order, scratch_bytes);
    q_free(db, scratch, scratch_bytes);
    return status;
}

static size_t resolver_width(const resolver_t *resolver)
{
    if (resolver == NULL || resolver->count == 0) return 0;
    const source_ref_t *last = &resolver->sources[resolver->count - 1];
    return last->offset + last->view->column_count;
}

static void correlated_free(database_t *db, correlated_t *item)
{
    relation_clear(db, &item->rel);
    size_t width = item->scope.width > 0 ? item->scope.width : 1;
    q_free(db, item->scope.params, width * sizeof(value_t));
    q_free(db, item->scope.used, width);
    if (item->last != NULL) owned_clear(db, item->last, item->column_count);
    q_free(db, item->last, (item->column_count > 0 ? item->column_count : 1) * sizeof(value_t));
    q_free(db, item->columns, (item->column_count > 0 ? item->column_count : 1) * sizeof(size_t));
    q_free(db, item, sizeof(*item));
}

static void correlated_free_all(database_t *db)
{
    while (db->correlated != NULL) {
        correlated_t *next = db->correlated->next;
        correlated_free(db, db->correlated);
        db->correlated = next;
    }
}

bool outer_hook(void *ctx, const sqlstmt_name_t *qualifier, const sqlstmt_name_t *name, value_type_t *type,
                       const value_t **cell)
{
    database_t *db = ctx;
    for (outer_scope_t *scope = db->outer; scope != NULL && scope->resolver != NULL; scope = scope->parent) {
        size_t column = resolve_column((void *)scope->resolver, qualifier, name, type);
        if (column == SQLEVAL_AMBIGUOUS) return false;
        if (column == SQLEVAL_NO_COLUMN) continue;
        /* 이 범위까지의 안쪽 서브쿼리는 모두 바깥 행에 기댄다 */
        for (outer_scope_t *inner = db->outer; inner != scope->parent; inner = inner->parent) inner->correlated = true;
        scope->used[column] = true;
        *cell = &scope->params[column];
        return true;
    }
    return false;
}

static database_exec_status_t correlated_run(database_t *db, correlated_t *item)
{
    outer_scope_t *saved = db->outer;
    db->outer = &item->scope;
    relation_clear(db, &item->rel);
    database_exec_status_t status = relation_fill(db, item->expr->subquery, NULL, &item->rel);
    if (status == DATABASE_EXEC_OK && item->expr->kind == SQLEXPR_IN) status = relation_set(db, &item->rel);
    db->outer = saved;
    return status;
}

static sqleval_status_t correlated_refresh(void *ctx, const value_t *row)
{
    correlated_t *item = ctx;
    database_t *db = item->db;
    bool same = item->ran;
    for (size_t i = 0; i < item->column_count; i++) {
        const value_view_t *now = &row[item->columns[i]].view;
        if (same && (item->last[i].view.type != now->type || sqleval_compare(&item->last[i].view, now) != 0)) same = false;
        item->scope.params[item->columns[i]] = (value_t){*now, false};
    }
    if (same) return SQLEVAL_OK;
    database_exec_status_t status = correlated_run(db, item);
    for (size_t i = 0; i < item->column_count && status == DATABASE_EXEC_OK; i++) {
        owned_clear(db, &item->last[i], 1);
        if (q_clone(db, &item->scope.params[item->columns[i]].view, &item->last[i]) != VALUE_OK) status = DATABASE_EXEC_NOMEM;
    }
    item->ran = status == DATABASE_EXEC_OK;
    if (status == DATABASE_EXEC_OK) return SQLEVAL_OK;
    db->subquery_status = status;
    return status == DATABASE_EXEC_NOMEM ? SQLEVAL_NOMEM : SQLEVAL_SUBQUERY;
}

static sqleval_status_t subquery_failed(database_t *db, database_exec_status_t status)
{
    db->subquery_status = status;
    return status == DATABASE_EXEC_NOMEM ? SQLEVAL_NOMEM : SQLEVAL_SUBQUERY;
}

/*
 * 식 자리 서브쿼리. 먼저 바깥 값을 NULL로 두고 한 번 실행해 본다. 바깥 열을 쓰지 않으면 그 결과를 문장 끝까지
 * 쥐고, 쓰면 바깥 행마다 다시 실행하는 객체로 남긴다(평가기가 refresh를 부른다).
 */
sqleval_status_t subquery_hook(void *ctx, const sqlexpr_t *expr, const sqleval_subquery_t **out)
{
    database_t *db = ctx;
    relation_t *cached = relation_find(db, expr);
    if (cached != NULL) {
        *out = &cached->info;
        return SQLEVAL_OK;
    }
    /* 바깥 질의가 다시 실행되며 같은 식을 다시 컴파일하면 객체를 재사용하고 새 문맥에 맞춘다 */
    for (correlated_t *item = db->correlated; item != NULL; item = item->next) {
        if (item->expr != expr) continue;
        if (db->compile_resolver != NULL) item->scope.own = *db->compile_resolver;
        item->scope.parent = db->outer;
        item->ran = false;
        *out = &item->rel.info;
        return SQLEVAL_OK;
    }
    correlated_t *item = q_calloc(db, 1, sizeof(*item));
    if (item == NULL) return subquery_failed(db, DATABASE_EXEC_NOMEM);
    item->db = db;
    item->expr = expr;
    if (db->compile_resolver != NULL) {
        item->scope.own = *db->compile_resolver;
        item->scope.resolver = &item->scope.own;
    }
    item->scope.width = resolver_width(db->compile_resolver);
    item->scope.parent = db->outer;
    size_t width = item->scope.width > 0 ? item->scope.width : 1;
    item->scope.params = q_calloc(db, width, sizeof(value_t));
    item->scope.used = q_calloc(db, width, 1);
    if (item->scope.params == NULL || item->scope.used == NULL) {
        correlated_free(db, item);
        return subquery_failed(db, DATABASE_EXEC_NOMEM);
    }
    database_exec_status_t status = correlated_run(db, item);
    if (status != DATABASE_EXEC_OK) {
        correlated_free(db, item);
        return subquery_failed(db, status);
    }
    if (!item->scope.correlated) {
        /* 바깥 열을 쓰지 않는다: 결과를 쥐고 다시 실행하지 않는다 */
        relation_t *rel = q_calloc(db, 1, sizeof(*rel));
        if (rel == NULL) {
            correlated_free(db, item);
            return subquery_failed(db, DATABASE_EXEC_NOMEM);
        }
        *rel = item->rel;
        memset(&item->rel, 0, sizeof(item->rel));
        rel->key = expr;
        rel->info.first = rel->row_count > 0 && rel->column_count > 0 ? &rel->cells[0].view : NULL;
        rel->info.set = rel->set;
        rel->next = db->relations;
        db->relations = rel;
        correlated_free(db, item);
        *out = &rel->info;
        return SQLEVAL_OK;
    }
    for (size_t c = 0; c < item->scope.width; c++) item->column_count += item->scope.used[c];
    size_t slots = item->column_count > 0 ? item->column_count : 1;
    item->columns = q_calloc(db, slots, sizeof(size_t));
    item->last = q_calloc(db, slots, sizeof(value_t));
    if (item->columns == NULL || item->last == NULL) {
        correlated_free(db, item);
        return subquery_failed(db, DATABASE_EXEC_NOMEM);
    }
    for (size_t c = 0, k = 0; c < item->scope.width; c++) {
        if (item->scope.used[c]) item->columns[k++] = c;
    }
    item->rel.info.refresh = correlated_refresh;
    item->rel.info.refresh_ctx = item;
    item->rel.info.outer_columns = item->columns;
    item->rel.info.outer_count = item->column_count;
    item->next = db->correlated;
    db->correlated = item;
    *out = &item->rel.info;
    return SQLEVAL_OK;
}

/* FROM의 이름이 보이는 WITH 이름이면 그 결과를 만든다. 아니면 *out은 NULL */
database_exec_status_t cte_relation(database_t *db, const sqlstmt_name_t *name, relation_t **out)
{
    *out = NULL;
    for (cte_scope_t *scope = db->ctes; scope != NULL; scope = scope->parent) {
        for (size_t i = 0; i < scope->visible; i++) {
            const sqlstmt_cte_t *cte = &scope->owner->ctes[i];
            if (cte->name.size != name->size || memcmp(cte->name.bytes, name->bytes, name->size) != 0) continue;
            /* WITH 질의는 자기보다 앞의 이름만 본다 */
            cte_scope_t own = {scope->owner, i, scope->parent};
            cte_scope_t *saved = db->ctes;
            db->ctes = &own;
            database_exec_status_t status = relation_build(db, cte, cte->query, cte, out);
            db->ctes = saved;
            return status;
        }
    }
    return DATABASE_EXEC_OK;
}
