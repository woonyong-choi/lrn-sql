#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc_audit.h"
#include "sql/mem_ledger.h"
#include "sql/result.h"

/*
 * result의 메모리 회계(ledger) 독립 검사. 세션·database 없이 result.c만 -Dmalloc=audit_malloc -Dcalloc=audit_calloc
 * -Drealloc=audit_realloc -Dfree=audit_free로 빌드해 정확한 추적 ledger와 맞춘다(치환하지 않으면 시작할 때 실패).
 * 이 ledger는 result.c의 할당만 계상하므로 "살아 있는 요청 바이트(audit_live) == 장부 사용량"이 어느 순간에도 맞아야
 * 하고, 어긋나면 순서가 틀린 것이다.
 *   - reserve는 할당보다 먼저(할당 직후 live <= used),
 *   - release는 해제보다 나중에(release 직후 live <= used이고 정리 끝에서 둘 다 0).
 * 확인하는 것:
 *   - builder 생성, 행 추가(슬롯 배열 증가 포함), finish 뒤 release, 도중 destroy, 행 추가 실패 뒤 destroy에서
 *     정확히 한 번씩 반환하고 반환 순서가 맞다.
 *   - command·error 결과의 계상과 반환.
 *   - result_accounted_bytes가 장부의 현재 사용량과 같고 rebind 뒤 새 ledger가 반환하며 retain·drop이 맞는다.
 *   - n번째 reserve 실패는 호출 전 상태를 유지하고(행 추가 실패 뒤에도 builder를 계속 쓸 수 있다) 누수가 없다.
 * 이 검사는 치환해 빌드한 result.c가 실행한 경로에 대한 것이다.
 */

static int g_run, g_fail;
#define CHECK(cond, ...)                              \
    do {                                              \
        g_run++;                                      \
        if (!(cond)) {                                \
            g_fail++;                                 \
            printf("  FAIL (line %d): ", __LINE__);   \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
        }                                             \
    } while (0)

typedef struct {
    size_t used;
    size_t peak;
    long reserves;
    long fail_at;            /* n번째 reserve를 거절. 0이면 끈다 */
    long retains, drops;
    unsigned long violations;
    mem_scope_t last_scope;
    size_t scope_used[3];
} ledger_state_t;

static ledger_state_t g;

static bool l_reserve(void *ctx, mem_scope_t scope, size_t bytes)
{
    ledger_state_t *s = ctx;
    if (s->fail_at != 0 && ++s->reserves == s->fail_at) return false;
    if (s->fail_at == 0) s->reserves++;
    s->used += bytes;
    s->scope_used[scope] += bytes;
    if (s->used > s->peak) s->peak = s->used;
    return true;
}

static void l_release(void *ctx, mem_scope_t scope, size_t bytes)
{
    ledger_state_t *s = ctx;
    assert(bytes <= s->used && bytes <= s->scope_used[scope]);
    s->used -= bytes;
    s->scope_used[scope] -= bytes;
    s->last_scope = scope;
    /* 반환 직후: 아직 살아 있는 요청 바이트가 장부보다 많으면 해제 전에 돌려준 것이다 */
    if (audit_live_now() > s->used) s->violations++;
}

static void l_retain(void *ctx) { ((ledger_state_t *)ctx)->retains++; }
static void l_drop(void *ctx) { ((ledger_state_t *)ctx)->drops++; }

static const mem_ledger_ops_t OPS = {l_reserve, l_release, l_retain, l_drop};

static void on_alloc(void *ctx, size_t request, size_t live)
{
    (void)request;
    ledger_state_t *s = ctx;
    if (live > s->used) s->violations++;  /* 할당이 reserve보다 앞섰다 */
}

static mem_ledger_t make_ledger(ledger_state_t *s) { return (mem_ledger_t){&OPS, s}; }

static void begin(void)
{
    memset(&g, 0, sizeof(g));
    audit_reset();
    audit_on_alloc = on_alloc;
    audit_ctx = &g;
}

static void end(const char *what)
{
    CHECK(g.used == 0, "%s: 장부에 남은 몫 %zu", what, g.used);
    CHECK(audit_live_now() == 0, "%s: 살아 있는 할당 %zu", what, audit_live_now());
    CHECK(g.violations == 0, "%s: 반환·할당 순서 위반 %lu", what, g.violations);
    CHECK(g.retains == g.drops, "%s: retain %ld drop %ld", what, g.retains, g.drops);
    audit_on_alloc = NULL;
}

static const result_column_t COLUMNS[] = {
    {(const uint8_t *)"id", 2, VALUE_INTEGER, false},
    {(const uint8_t *)"label", 5, VALUE_TEXT, true},
};

static value_view_t int_cell(int64_t v) { return (value_view_t){VALUE_INTEGER, v, 0, NULL, 0}; }
static value_view_t text_cell(const char *t) { return (value_view_t){VALUE_TEXT, 0, 0, (const uint8_t *)t, strlen(t)}; }

static result_status_t add(result_builder_t *b, int64_t id, const char *label)
{
    value_view_t cells[2] = {int_cell(id), text_cell(label)};
    return result_builder_add_row(b, cells, 2);
}

/* ── 검사 ── */

static void test_builder_finish_release(void)
{
    printf("builder → finish → release\n");
    begin();
    mem_ledger_t ledger = make_ledger(&g);
    result_builder_spec_t spec = {COLUMNS, 2, NULL};
    result_builder_t *builder = NULL;
    CHECK(result_builder_create_ex(&spec, &ledger, &builder) == RESULT_OK, "create");
    size_t after_create = g.used;
    CHECK(after_create > 0, "생성하면 계상된다");
    /* 슬롯 배열이 여러 번 늘어나도록 40행 */
    for (int i = 0; i < 40; i++) CHECK(add(builder, i, "row label for growth") == RESULT_OK, "add %d", i);
    result_t *result = NULL;
    CHECK(result_builder_finish(&builder, &result) == RESULT_OK && builder == NULL, "finish");
    CHECK(result_accounted_bytes(result) == g.used, "accounted %zu 장부 %zu", result_accounted_bytes(result), g.used);
    CHECK(g.violations == 0, "finish까지 순서 위반 %lu", g.violations);
    result_release(&result);
    end("finish·release");
}

static void test_destroy_paths(void)
{
    printf("builder destroy: 생성 직후·행 추가 뒤·finish 전\n");
    for (int rows = 0; rows <= 40; rows += (rows < 3 ? 1 : 13)) {
        begin();
        mem_ledger_t ledger = make_ledger(&g);
        result_builder_spec_t spec = {COLUMNS, 2, NULL};
        result_builder_t *builder = NULL;
        CHECK(result_builder_create_ex(&spec, &ledger, &builder) == RESULT_OK, "create");
        for (int i = 0; i < rows; i++) CHECK(add(builder, i, "label") == RESULT_OK, "add");
        result_builder_destroy(&builder);
        CHECK(builder == NULL, "destroy는 NULL로 만든다");
        char label[48];
        snprintf(label, sizeof(label), "destroy rows=%d", rows);
        end(label);
        result_builder_destroy(&builder);  /* NULL이면 아무 일도 없다 */
    }
}

/* n번째 reserve 실패: create, add_row, 그 뒤 destroy 또는 계속 사용 */
static void test_reserve_failure_sweep(void)
{
    printf("n번째 reserve 실패: 생성·행 추가·슬롯 증가, 실패 뒤 destroy와 계속 사용\n");
    long create_fail = 0, add_fail = 0;
    for (long n = 1; n < 200; n++) {
        for (int mode = 0; mode < 2; mode++) {
            begin();
            mem_ledger_t ledger = make_ledger(&g);
            result_builder_spec_t spec = {COLUMNS, 2, NULL};
            result_builder_t *builder = NULL;
            g.fail_at = n;
            result_status_t created = result_builder_create_ex(&spec, &ledger, &builder);
            char label[64];
            snprintf(label, sizeof(label), "reserve %ld mode %d", n, mode);
            if (created != RESULT_OK) {
                CHECK(created == RESULT_NOMEM && builder == NULL, "%s: create 거절", label);
                create_fail++;
                CHECK(g.used == 0, "%s: 거절된 create는 몫을 남기지 않는다", label);
                g.fail_at = 0;
                end(label);
                continue;
            }
            bool failed_add = false;
            size_t rows = 0;
            for (int i = 0; i < 40; i++) {
                size_t before = g.used;
                result_status_t st = add(builder, i, "row label for growth");
                if (st == RESULT_NOMEM) {
                    failed_add = true;
                    CHECK(g.used == before, "%s: 실패한 행은 몫을 바꾸지 않는다 (%zu→%zu)", label, before, g.used);
                    break;
                }
                CHECK(st == RESULT_OK, "%s: add %d", label, i);
                rows++;
            }
            g.fail_at = 0;
            if (!failed_add) {
                result_builder_destroy(&builder);
                end(label);
                break;  /* 이 단계의 모든 reserve 지점을 지났다 */
            }
            add_fail++;
            if (mode == 0) {
                /* 실패 뒤 정리: builder를 destroy */
                result_builder_destroy(&builder);
            } else {
                /* 실패 뒤 계속 사용: 다시 추가하고 finish한 뒤 release */
                CHECK(add(builder, 1000, "retry") == RESULT_OK, "%s: 실패 뒤 다시 추가", label);
                result_t *result = NULL;
                CHECK(result_builder_finish(&builder, &result) == RESULT_OK, "%s: finish", label);
                result_row_t row;
                size_t seen = 0;
                while (result_next(result, &row) == RESULT_NEXT_ROW) seen++;
                CHECK(seen == rows + 1, "%s: 행 %zu 기대 %zu", label, seen, rows + 1);
                CHECK(result_accounted_bytes(result) == g.used, "%s: accounted", label);
                result_release(&result);
            }
            end(label);
        }
    }
    CHECK(create_fail >= 2 && add_fail >= 3, "주입 지점 create %ld add %ld", create_fail, add_fail);
    printf("  create 실패 %ld곳, add 실패 %ld곳\n", create_fail, add_fail);
}

static void test_command_and_error(void)
{
    printf("command·error 결과와 반환\n");
    for (int kind = 0; kind < 2; kind++) {
        for (long n = 0; n < 6; n++) {
            begin();
            mem_ledger_t ledger = make_ledger(&g);
            g.fail_at = n;  /* 0이면 주입 없음 */
            result_t *result = NULL;
            result_status_t st;
            if (kind == 0) {
                result_command_t command = {0, "OK", 2};
                st = result_open_command_ex(&command, &ledger, &result);
            } else {
                result_error_t error = {RESULT_E_SEMANTIC, 7, "something is wrong", 18, false};
                st = result_open_error_ex(&error, &ledger, &result);
            }
            g.fail_at = 0;
            char label[48];
            snprintf(label, sizeof(label), "kind %d n %ld", kind, n);
            if (st != RESULT_OK) {
                CHECK(st == RESULT_NOMEM && result == NULL && g.used == 0, "%s: 거절은 몫을 남기지 않는다", label);
            } else {
                CHECK(st == RESULT_OK, "%s", label);
                CHECK(result_accounted_bytes(result) == g.used && g.used > 0, "%s: accounted", label);
                result_release(&result);
            }
            end(label);
        }
    }
}

static void test_rebind(void)
{
    printf("rebind: 새 ledger가 반환하고 retain·drop이 맞는다\n");
    begin();
    static ledger_state_t owner;
    memset(&owner, 0, sizeof(owner));
    mem_ledger_t query = make_ledger(&g);
    mem_ledger_t holder = make_ledger(&owner);
    result_builder_spec_t spec = {COLUMNS, 2, NULL};
    result_builder_t *builder = NULL;
    assert(result_builder_create_ex(&spec, &query, &builder) == RESULT_OK);
    for (int i = 0; i < 5; i++) assert(add(builder, i, "x") == RESULT_OK);
    result_t *result = NULL;
    assert(result_builder_finish(&builder, &result) == RESULT_OK);
    size_t bytes = result_accounted_bytes(result);
    CHECK(g.used == bytes && g.scope_used[MEM_SCOPE_QUERY] == bytes, "query 몫이 accounted와 같다");
    /* 호출자가 하는 일: query 몫을 옮기고 결과가 반환할 곳을 바꾼다 */
    g.used -= bytes;
    g.scope_used[MEM_SCOPE_QUERY] -= bytes;
    owner.used += bytes;
    owner.scope_used[MEM_SCOPE_RESULT] += bytes;
    audit_ctx = &owner;  /* 이후 할당 검사는 새 장부 기준 */
    result_rebind_ledger(result, &holder, MEM_SCOPE_RESULT);
    CHECK(owner.retains == 1 && g.drops == 1, "새 ledger retain %ld, 옛 ledger drop %ld", owner.retains, g.drops);
    result_release(&result);
    CHECK(owner.used == 0 && owner.scope_used[MEM_SCOPE_RESULT] == 0, "새 ledger가 RESULT scope로 반환 %zu", owner.used);
    CHECK(owner.drops == 1 && owner.violations == 0, "새 ledger drop %ld", owner.drops);
    CHECK(g.used == 0, "옛 ledger에는 남은 몫이 없다");
    CHECK(audit_live_now() == 0, "살아 있는 할당 %zu", audit_live_now());
    audit_on_alloc = NULL;
    /* 계상하지 않는 결과는 rebind가 아무 일도 하지 않는다 */
    begin();
    result_builder_t *plain = NULL;
    assert(result_builder_create(&spec, &plain) == RESULT_OK);
    result_t *plain_result = NULL;
    assert(result_builder_finish(&plain, &plain_result) == RESULT_OK);
    result_rebind_ledger(plain_result, &holder, MEM_SCOPE_RESULT);
    CHECK(result_accounted_bytes(plain_result) == 0, "계상하지 않은 결과");
    result_release(&plain_result);
    audit_on_alloc = NULL;
}

int main(void)
{
    test_builder_finish_release();
    test_destroy_paths();
    test_reserve_failure_sweep();
    test_command_and_error();
    test_rebind();
    printf("\n%d checks, %d failures\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
