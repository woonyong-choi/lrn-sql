/*
 * test_bptree_property.c — B+ tree 구조 불변식 property test
 *
 * 기존 테스트(tests/test_all.c)는 키를 1..N 오름차순으로 넣고 앞에서부터
 * 지운다. 그 경로는 "리프가 오른쪽으로만 분할되는" 한 가지 모양만 만들기
 * 때문에, 형제 재분배(borrow)와 병합(merge)이 섞여 일어나는 실제 삭제
 * 경로를 거의 건드리지 못한다.
 *
 * 이 파일은 그 자리를 메운다.
 *   1. 고정 시드 난수로 삽입·삭제를 섞어 돌리고,
 *   2. 매 단계마다 트리 전체를 걸어 8가지 구조 불변식을 검사하고,
 *   3. 참조 모델(정렬된 키 집합)과 키 집합·범위 스캔 결과를 대조한다.
 *
 * 검사하는 불변식:
 *   I1  모든 리프가 같은 깊이에 있다 (균형)
 *   I2  한 노드 안의 키가 순증가한다
 *   I3  자식 서브트리의 키가 부모 separator 가 정한 [lo, hi) 안에 있다
 *   I4  루트가 아닌 노드는 최소 점유율(max/2) 이상을 유지한다
 *   I5  리프 체인이 모든 리프를 좌→우 한 번씩 방문하고 prev/next 가 대칭이다
 *   I6  도달 가능한 페이지가 두 번 나타나지 않는다 (자식 포인터 중복 없음)
 *   I7  트리가 담은 키 집합이 참조 모델과 같고 전역 오름차순이다
 *   I8  parent_page_id 가 실제 부모를 가리킨다
 *
 * 범위 스캔은 별도로, 경계 포함/제외 4조합 + 한쪽 열린 구간을 무작위로
 * 뽑아 참조 모델의 필터 결과와 순서까지 대조한다. bptree_range_scan 은
 * 이 저장소의 INDEX_RANGE 경로가 딛고 선 유일한 자료구조 연산인데,
 * 지금까지 어떤 테스트도 호출하지 않았다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdarg.h>

#include "storage/pager.h"
#include "storage/schema.h"
#include "storage/bptree.h"

#define TEST_DB "__test__prop.db"

static int g_run = 0, g_fail = 0;

#define CHECK(cond, ...) do {                       \
    g_run++;                                        \
    if (!(cond)) {                                  \
        g_fail++;                                   \
        printf("  FAIL (line %d): ", __LINE__);     \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
    }                                               \
} while (0)

/* 실패가 누적되면 뒤 출력이 의미 없어지므로 한 시나리오당 상한을 둔다 */
#define BAIL_IF_FAILING(tag) do {                   \
    if (g_fail > 20) {                              \
        printf("  (%s: 실패 누적으로 중단)\n", tag); \
        return;                                     \
    }                                               \
} while (0)

/* ── 참조 모델: 존재하는 키를 그대로 들고 있는 비트맵 ── */
typedef struct {
    uint8_t *present;   /* present[k] != 0 이면 키 k 가 트리에 있어야 한다 */
    uint64_t max_key;
    uint64_t count;
} model_t;

static void model_init(model_t *m, uint64_t max_key) {
    m->present = (uint8_t *)calloc(max_key + 1, 1);
    assert(m->present != NULL);
    m->max_key = max_key;
    m->count = 0;
}

static void model_free(model_t *m) { free(m->present); m->present = NULL; }

/* ── 결정적 난수 (xorshift64*) — 플랫폼 rand() 구현에 의존하지 않는다 ── */
static uint64_t g_rng;
static void rng_seed(uint64_t s) { g_rng = s ? s : 0x9E3779B97F4A7C15ull; }
static uint64_t rng_next(void) {
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static uint64_t rng_below(uint64_t n) { return rng_next() % n; }

/* ── 용량 헬퍼 (bptree.c 의 계산을 테스트 쪽에서 재현) ── */
static uint32_t cap_leaf(const pager_t *p) {
    return (p->page_size - (uint32_t)sizeof(leaf_page_header_t))
           / (uint32_t)sizeof(leaf_entry_t);
}
static uint32_t cap_internal(const pager_t *p) {
    return (p->page_size - (uint32_t)sizeof(internal_page_header_t))
           / (uint32_t)sizeof(internal_entry_t);
}

/* ══════════════════════════════════════════════════════════════════
 *  트리 순회 검증
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    pager_t  *pager;
    uint8_t  *seen;        /* 페이지 방문 표시 (I6) */
    uint32_t  seen_cap;
    int       leaf_depth;  /* 최초로 만난 리프의 깊이 (I1 기준값) */
    uint32_t  leaf_count;
    uint32_t  first_leaf;  /* 가장 왼쪽 리프 (I5 시작점) */
    uint64_t  key_total;
    int       errors;
} walk_t;

static void walk_err(walk_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void walk_err(walk_t *w, const char *fmt, ...) {
    if (w->errors < 10) {
        va_list ap;
        va_start(ap, fmt);
        printf("  FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
    w->errors++;
}

/*
 * 한 노드를 검사하고 자식으로 내려간다.
 * lo/hi 는 부모 separator 가 이 서브트리에 허용한 반열린 구간 [lo, hi).
 * has_lo/has_hi 가 false 면 그쪽은 무한대.
 */
static void walk_node(walk_t *w, uint32_t pid, int depth,
                      bool has_lo, uint64_t lo, bool has_hi, uint64_t hi,
                      uint32_t expect_parent, bool is_root)
{
    if (pid == 0 || pid >= w->seen_cap) {
        walk_err(w, "자식 page_id 가 범위 밖: %" PRIu64 " (cap %" PRIu64 ")",
                 (uint64_t)pid, (uint64_t)w->seen_cap);
        return;
    }
    /* I6: 같은 페이지가 두 부모에 달려 있으면 트리가 아니라 DAG 다 */
    if (w->seen[pid]) {
        walk_err(w, "page %" PRIu64 " 가 두 번 등장한다 (depth %" PRIu64 ")",
                 (uint64_t)pid, (uint64_t)depth);
        return;
    }
    w->seen[pid] = 1;

    uint8_t *page = pager_get_page_rlatch(w->pager, pid);
    if (page == NULL) {
        walk_err(w, "page %" PRIu64 " 로드 실패", (uint64_t)pid);
        return;
    }
    uint8_t *copy = (uint8_t *)malloc(w->pager->page_size);
    assert(copy != NULL);
    memcpy(copy, page, w->pager->page_size);
    pager_unlatch_r(w->pager, pid);

    uint32_t ptype;
    memcpy(&ptype, copy, sizeof(ptype));

    if (ptype == PAGE_TYPE_LEAF) {
        leaf_page_header_t lph;
        memcpy(&lph, copy, sizeof(lph));
        leaf_entry_t *e = (leaf_entry_t *)(copy + sizeof(lph));

        /* I1: 모든 리프가 같은 깊이 */
        if (w->leaf_depth < 0) {
            w->leaf_depth = depth;
            w->first_leaf = pid;
        } else if (depth != w->leaf_depth) {
            walk_err(w, "리프 깊이 불일치: page %" PRIu64 " depth %" PRIu64,
                     (uint64_t)pid, (uint64_t)depth);
        }
        w->leaf_count++;
        w->key_total += lph.key_count;

        /* I8: parent 역포인터 */
        if (lph.parent_page_id != expect_parent) {
            walk_err(w, "리프 page %" PRIu64 " 의 parent_page_id 가 %" PRIu64
                     " (실제 부모와 다름)", (uint64_t)pid, (uint64_t)lph.parent_page_id);
        }
        /* I4: 루트가 아닌 리프는 최소 점유율 이상 */
        if (!is_root && lph.key_count < cap_leaf(w->pager) / 2) {
            walk_err(w, "리프 page %" PRIu64 " 언더플로우: key_count=%" PRIu64,
                     (uint64_t)pid, (uint64_t)lph.key_count);
        }
        for (uint32_t i = 0; i < lph.key_count; i++) {
            /* I2: 노드 내부 키 순증가 */
            if (i > 0 && e[i - 1].key >= e[i].key) {
                walk_err(w, "리프 page %" PRIu64 " 키 정렬 깨짐: %" PRIu64,
                         (uint64_t)pid, e[i].key);
            }
            /* I3: 부모 separator 구간 준수 */
            if (has_lo && e[i].key < lo) {
                walk_err(w, "리프 page %" PRIu64 " 키 %" PRIu64 " 가 하한 미만",
                         (uint64_t)pid, e[i].key);
            }
            if (has_hi && e[i].key >= hi) {
                walk_err(w, "리프 page %" PRIu64 " 키 %" PRIu64 " 가 상한 이상",
                         (uint64_t)pid, e[i].key);
            }
        }
        free(copy);
        return;
    }

    if (ptype != PAGE_TYPE_INTERNAL) {
        walk_err(w, "page %" PRIu64 " 의 page_type 이 %" PRIu64 " (리프/내부 아님)",
                 (uint64_t)pid, (uint64_t)ptype);
        free(copy);
        return;
    }

    internal_page_header_t iph;
    memcpy(&iph, copy, sizeof(iph));
    internal_entry_t *ie = (internal_entry_t *)(copy + sizeof(iph));

    if (iph.parent_page_id != expect_parent) {
        walk_err(w, "내부 page %" PRIu64 " 의 parent_page_id 가 %" PRIu64,
                 (uint64_t)pid, (uint64_t)iph.parent_page_id);
    }
    if (!is_root && iph.key_count < cap_internal(w->pager) / 2) {
        walk_err(w, "내부 page %" PRIu64 " 언더플로우: key_count=%" PRIu64,
                 (uint64_t)pid, (uint64_t)iph.key_count);
    }
    if (is_root && iph.key_count == 0) {
        walk_err(w, "루트 내부 page %" PRIu64 " 의 키가 0개", (uint64_t)pid);
    }
    for (uint32_t i = 1; i < iph.key_count; i++) {
        if (ie[i - 1].key >= ie[i].key) {
            walk_err(w, "내부 page %" PRIu64 " separator 정렬 깨짐: %" PRIu64,
                     (uint64_t)pid, ie[i].key);
        }
    }

    /* 자식마다 구간을 좁혀 내려간다 */
    uint32_t nkeys = iph.key_count;
    walk_node(w, iph.leftmost_child_page_id, depth + 1,
              has_lo, lo, true, nkeys ? ie[0].key : hi, pid, false);
    for (uint32_t i = 0; i < nkeys; i++) {
        bool child_has_hi = (i + 1 < nkeys) || has_hi;
        uint64_t child_hi = (i + 1 < nkeys) ? ie[i + 1].key : hi;
        walk_node(w, ie[i].right_child_page_id, depth + 1,
                  true, ie[i].key, child_has_hi, child_hi, pid, false);
    }
    free(copy);
}

/*
 * verify_tree — 구조 불변식 전체 + 참조 모델 대조.
 * tag 는 실패 메시지에 붙는 시나리오 이름이다.
 */
static void verify_tree(pager_t *pager, model_t *m, const char *tag)
{
    walk_t w;
    memset(&w, 0, sizeof(w));
    w.pager = pager;
    w.seen_cap = pager->header.next_page_id + 1;
    w.seen = (uint8_t *)calloc(w.seen_cap, 1);
    assert(w.seen != NULL);
    w.leaf_depth = -1;

    walk_node(&w, pager->header.root_index_page_id, 0,
              false, 0, false, 0, 0, true);

    CHECK(w.errors == 0, "[%s] 구조 불변식 위반 %d건 (I1~I4, I6, I8)",
          tag, w.errors);
    CHECK(w.key_total == m->count,
          "[%s] 트리 키 수 %" PRIu64 " != 모델 %" PRIu64,
          tag, w.key_total, m->count);

    /* ── I5 + I7: 리프 체인을 왼쪽 끝부터 따라가며 전역 순서와 집합을 본다 ── */
    uint32_t pid = w.first_leaf;
    uint32_t chain_leaves = 0;
    uint32_t prev_pid = 0;
    uint64_t last_key = 0;
    bool have_last = false;
    uint64_t chain_keys = 0;
    int order_err = 0, set_err = 0, link_err = 0;

    while (pid != 0) {
        if (pid >= w.seen_cap || !w.seen[pid]) {
            link_err++;   /* 체인이 트리 밖 페이지로 샌다 */
            break;
        }
        uint8_t *page = pager_get_page_rlatch(pager, pid);
        if (page == NULL) { link_err++; break; }
        leaf_page_header_t lph;
        memcpy(&lph, page, sizeof(lph));
        leaf_entry_t *e = (leaf_entry_t *)(page + sizeof(lph));

        if (lph.prev_leaf_page_id != prev_pid) link_err++;  /* prev/next 대칭 */

        for (uint32_t i = 0; i < lph.key_count; i++) {
            uint64_t k = e[i].key;
            if (have_last && k <= last_key) order_err++;    /* 전역 오름차순 */
            last_key = k; have_last = true;
            if (k > m->max_key || !m->present[k]) set_err++; /* 모델에 없는 키 */
            /* 리프 엔트리의 row_ref 는 삽입할 때 쓴 값(page_id=key)과 같아야 한다 */
            if (e[i].row_ref.page_id != (uint32_t)k) set_err++;
        }
        chain_keys += lph.key_count;
        chain_leaves++;
        uint32_t next_pid = lph.next_leaf_page_id;
        pager_unlatch_r(pager, pid);
        prev_pid = pid;
        pid = next_pid;
    }

    CHECK(link_err == 0, "[%s] 리프 체인 링크 오류 %d건 (prev/next 비대칭 또는 미도달 페이지)",
          tag, link_err);
    CHECK(order_err == 0, "[%s] 리프 체인 전역 키 순서 위반 %d건", tag, order_err);
    CHECK(set_err == 0, "[%s] 모델에 없는 키/잘못된 row_ref %d건", tag, set_err);
    CHECK(chain_leaves == w.leaf_count,
          "[%s] 리프 체인이 방문한 리프 %" PRIu64 " != 트리 리프 %" PRIu64,
          tag, (uint64_t)chain_leaves, (uint64_t)w.leaf_count);
    CHECK(chain_keys == m->count,
          "[%s] 리프 체인 키 수 %" PRIu64 " != 모델 %" PRIu64,
          tag, chain_keys, m->count);

    free(w.seen);
}

/* ══════════════════════════════════════════════════════════════════
 *  범위 스캔 대조
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t *keys;
    uint32_t  count;
    uint32_t  cap;
    bool      order_broken;
    bool      ref_broken;
} collect_t;

static bool collect_cb(uint64_t key, row_ref_t ref, void *ctx) {
    collect_t *c = (collect_t *)ctx;
    if (c->count > 0 && key <= c->keys[c->count - 1]) c->order_broken = true;
    if (ref.page_id != (uint32_t)key) c->ref_broken = true;
    if (c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->keys = (uint64_t *)realloc(c->keys, c->cap * sizeof(uint64_t));
        assert(c->keys != NULL);
    }
    c->keys[c->count++] = key;
    return true;
}

/*
 * 무작위 구간을 뽑아 스캔 결과를 모델 필터와 비교한다.
 * 경계 포함/제외 4조합과 한쪽 열린 구간을 모두 돌린다.
 */
static void verify_ranges(pager_t *pager, model_t *m, int trials, const char *tag)
{
    int mismatch = 0, order_bad = 0, ref_bad = 0;

    for (int t = 0; t < trials; t++) {
        /* t 의 하위 2비트로 경계 포함 여부를, 그 위 2비트로 개방 여부를 정한다 */
        bool lo_inc = (t & 1) != 0;
        bool hi_inc = (t & 2) != 0;
        bool has_lo = (t % 7) != 0;    /* 가끔 하한 없음 */
        bool has_hi = (t % 5) != 0;    /* 가끔 상한 없음 */

        uint64_t a = rng_below(m->max_key + 1);
        uint64_t b = rng_below(m->max_key + 1);
        uint64_t lo = a < b ? a : b;
        uint64_t hi = a < b ? b : a;

        collect_t c;
        memset(&c, 0, sizeof(c));
        bptree_range_scan(pager, has_lo, lo, lo_inc, has_hi, hi, hi_inc,
                          collect_cb, &c);

        /* 참조 모델로 같은 구간을 직접 센다 */
        uint32_t expect = 0;
        uint32_t idx = 0;
        bool content_bad = false;
        for (uint64_t k = 0; k <= m->max_key; k++) {
            if (!m->present[k]) continue;
            if (has_lo && (lo_inc ? k < lo : k <= lo)) continue;
            if (has_hi && (hi_inc ? k > hi : k >= hi)) continue;
            expect++;
            if (idx < c.count && c.keys[idx] != k) content_bad = true;
            idx++;
        }
        if (c.count != expect || content_bad) mismatch++;
        if (c.order_broken) order_bad++;
        if (c.ref_broken) ref_bad++;
        free(c.keys);
    }

    CHECK(mismatch == 0, "[%s] 범위 스캔 결과가 모델과 다른 구간 %d개 / %d",
          tag, mismatch, trials);
    CHECK(order_bad == 0, "[%s] 범위 스캔이 오름차순이 아닌 구간 %d개", tag, order_bad);
    CHECK(ref_bad == 0, "[%s] 범위 스캔이 잘못된 row_ref 를 준 구간 %d개", tag, ref_bad);
}

/* ══════════════════════════════════════════════════════════════════
 *  시나리오
 * ══════════════════════════════════════════════════════════════════ */

static void setup(pager_t *pager) {
    unlink(TEST_DB);
    assert(pager_open(pager, TEST_DB, true) == 0);
    db_header_t *hdr = &pager->header;
    hdr->column_count = 0;
    column_meta_t *c = &hdr->columns[hdr->column_count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, "id", 31);
    c->type = COL_TYPE_BIGINT; c->size = 8; c->is_system = 1;
    schema_compute_layout(hdr);
    pager->header_dirty = true;
}

static void teardown(pager_t *pager) {
    pager_close(pager);
    unlink(TEST_DB);
}

/* row_ref 를 키에서 결정적으로 만들어 두면, 검증 쪽에서 매핑이 섞였는지 본다 */
static row_ref_t ref_for(uint64_t key) {
    row_ref_t r;
    r.page_id = (uint32_t)key;
    r.slot_id = (uint16_t)(key & 0xFFFF);
    return r;
}

/*
 * 시나리오 1 — 무작위 순서 삽입.
 * 오름차순 삽입과 달리 리프가 가운데에서 쪼개지므로 split 경로의 분기가
 * 전부 밟힌다.
 */
static void test_random_insert(void) {
    printf("[test_random_insert]\n");
    pager_t pager;
    setup(&pager);

    /* 리프 약 30개 분량 — 페이지 크기(4K/16K)에 맞춰 자동으로 조절된다 */
    uint64_t n = (uint64_t)cap_leaf(&pager) * 30;
    model_t m;
    model_init(&m, n);
    rng_seed(20260922);

    /* 1..n 을 Fisher-Yates 로 섞어 중복 없이 무작위 순서로 넣는다 */
    uint64_t *order = (uint64_t *)malloc((n + 1) * sizeof(uint64_t));
    assert(order != NULL);
    for (uint64_t i = 1; i <= n; i++) order[i] = i;
    for (uint64_t i = n; i > 1; i--) {
        uint64_t j = 1 + rng_below(i);
        uint64_t tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }

    int ins_fail = 0;
    for (uint64_t i = 1; i <= n; i++) {
        uint64_t k = order[i];
        if (bptree_insert(&pager, k, ref_for(k)) != 0) ins_fail++;
        m.present[k] = 1;
        m.count++;
    }
    free(order);
    CHECK(ins_fail == 0, "무작위 삽입 실패 %d건 / %" PRIu64, ins_fail, n);

    printf("  %" PRIu64 "개 무작위 삽입, 높이=%d\n", n, bptree_height(&pager));
    verify_tree(&pager, &m, "random_insert");
    verify_ranges(&pager, &m, 40, "random_insert");

    /* 중복 삽입은 거부되고 트리를 건드리지 않아야 한다 */
    CHECK(bptree_insert(&pager, n / 2, ref_for(999)) == -1,
          "중복 키 삽입이 거부되지 않았다");
    verify_tree(&pager, &m, "random_insert/dup");

    model_free(&m);
    teardown(&pager);
}

/*
 * 시나리오 2 — 무작위 삭제.
 * 삭제 순서가 무작위라 borrow(형제 재분배)와 merge(병합)가 섞여 일어난다.
 * 기존 테스트는 앞에서부터 순차 삭제라 이 조합을 만들지 못한다.
 */
static void test_random_delete(void) {
    printf("[test_random_delete]\n");
    pager_t pager;
    setup(&pager);

    uint64_t n = (uint64_t)cap_leaf(&pager) * 20;
    model_t m;
    model_init(&m, n);
    rng_seed(0xC0FFEE);

    for (uint64_t k = 1; k <= n; k++) {
        bptree_insert(&pager, k, ref_for(k));
        m.present[k] = 1; m.count++;
    }
    verify_tree(&pager, &m, "delete/seed");

    /* 무작위 순서로 90% 를 지우고, 주기적으로 전체 불변식을 다시 본다 */
    uint64_t *order = (uint64_t *)malloc((n + 1) * sizeof(uint64_t));
    assert(order != NULL);
    for (uint64_t i = 1; i <= n; i++) order[i] = i;
    for (uint64_t i = n; i > 1; i--) {
        uint64_t j = 1 + rng_below(i);
        uint64_t tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }

    uint64_t target = n - n / 10;
    uint64_t checkpoint = target / 8 ? target / 8 : 1;
    int del_fail = 0;
    for (uint64_t i = 1; i <= target; i++) {
        uint64_t k = order[i];
        if (bptree_delete(&pager, k) != 0) del_fail++;
        m.present[k] = 0; m.count--;
        if (i % checkpoint == 0) {
            verify_tree(&pager, &m, "random_delete/mid");
            BAIL_IF_FAILING("random_delete");
        }
    }
    free(order);
    CHECK(del_fail == 0, "무작위 삭제 실패 %d건 / %" PRIu64, del_fail, target);

    printf("  %" PRIu64 "개 중 %" PRIu64 "개 무작위 삭제, 높이=%d\n",
           n, target, bptree_height(&pager));
    verify_tree(&pager, &m, "random_delete");
    verify_ranges(&pager, &m, 40, "random_delete");

    /* 없는 키 삭제는 -1 이고 트리를 바꾸지 않는다 */
    uint64_t gone = 0;
    for (uint64_t k = 1; k <= n && !gone; k++) if (!m.present[k]) gone = k;
    if (gone) {
        CHECK(bptree_delete(&pager, gone) == -1,
              "없는 키 삭제가 성공을 반환했다: key=%" PRIu64, gone);
        verify_tree(&pager, &m, "random_delete/absent");
    }

    model_free(&m);
    teardown(&pager);
}

/*
 * 시나리오 3 — 삽입과 삭제를 섞는다.
 * 지웠던 자리에 다시 넣는 흐름이 섞이면 페이지 재활용(pager_free_page →
 * pager_alloc_page)이 트리 구조와 엮인다. 여기서 페이지가 두 부모에 달리는
 * 종류의 오류(I6)가 드러난다.
 */
static void test_mixed_churn(void) {
    printf("[test_mixed_churn]\n");
    pager_t pager;
    setup(&pager);

    uint64_t n = (uint64_t)cap_leaf(&pager) * 8;
    model_t m;
    model_init(&m, n);
    rng_seed(0x5EED1234);

    uint64_t ops = n * 6;
    uint64_t checkpoint = ops / 10 ? ops / 10 : 1;
    int churn_fail = 0;
    for (uint64_t i = 0; i < ops; i++) {
        uint64_t k = 1 + rng_below(n);
        if (m.present[k]) {
            if (bptree_delete(&pager, k) != 0) churn_fail++;
            m.present[k] = 0; m.count--;
        } else {
            if (bptree_insert(&pager, k, ref_for(k)) != 0) churn_fail++;
            m.present[k] = 1; m.count++;
        }
        if (i % checkpoint == 0) {
            verify_tree(&pager, &m, "mixed_churn/mid");
            BAIL_IF_FAILING("mixed_churn");
        }
    }
    CHECK(churn_fail == 0, "혼합 연산 실패 %d건 / %" PRIu64, churn_fail, ops);

    printf("  %" PRIu64 "회 삽입/삭제 혼합, 남은 키=%" PRIu64 ", 높이=%d\n",
           ops, m.count, bptree_height(&pager));
    verify_tree(&pager, &m, "mixed_churn");
    verify_ranges(&pager, &m, 60, "mixed_churn");

    /* 점 조회가 모델과 일치하는지 (범위 스캔과 독립적인 경로) */
    int point_bad = 0;
    for (uint64_t k = 1; k <= n; k++) {
        row_ref_t out;
        bool hit = bptree_search(&pager, k, &out);
        if (hit != (m.present[k] != 0)) point_bad++;
        else if (hit && out.page_id != (uint32_t)k) point_bad++;
    }
    CHECK(point_bad == 0, "점 조회가 모델과 다른 키 %d개", point_bad);

    model_free(&m);
    teardown(&pager);
}

/*
 * 시나리오 4 — 범위 스캔 경계 조건.
 * 무작위 구간으로는 잘 안 걸리는 자리들을 콕 집어 본다:
 *   빈 트리 / 구간이 데이터보다 완전히 왼쪽·오른쪽 / lo == hi /
 *   lo > hi (모순 구간) / 양끝 개방.
 */
static void test_range_edges(void) {
    printf("[test_range_edges]\n");
    pager_t pager;
    setup(&pager);

    collect_t c;

    /* 빈 트리: 어떤 구간이든 0행 */
    memset(&c, 0, sizeof(c));
    bptree_range_scan(&pager, false, 0, true, false, 0, true, collect_cb, &c);
    CHECK(c.count == 0, "빈 트리 전체 스캔이 %" PRIu64 "행을 냈다", (uint64_t)c.count);
    free(c.keys);

    /* 키 10, 20, ... 100 — 사이가 비어 있어 경계 판정이 드러난다 */
    model_t m;
    model_init(&m, 200);
    for (uint64_t k = 10; k <= 100; k += 10) {
        bptree_insert(&pager, k, ref_for(k));
        m.present[k] = 1; m.count++;
    }

    struct { bool hl; uint64_t lo; bool li; bool hh; uint64_t hi; bool hi_i;
             uint32_t expect; const char *desc; } cases[] = {
        { true, 10, true,  true, 100, true,  10, "id BETWEEN 10 AND 100 (전체)" },
        { true, 10, false, true, 100, false,  8, "10 < id < 100 (양끝 제외)" },
        { true, 15, true,  true,  25, true,   1, "구간 안에 키 1개(20)" },
        { true, 11, true,  true,  19, true,   0, "키 사이 빈 구간" },
        { true, 50, true,  true,  50, true,   1, "lo == hi, 양끝 포함" },
        { true, 50, false, true,  50, true,   0, "lo == hi, 하한 제외" },
        { true, 80, true,  true,  20, true,   0, "lo > hi (모순 구간)" },
        { true,  0, true,  true,   5, true,   0, "데이터보다 완전히 왼쪽" },
        { true,150, true,  true, 200, true,   0, "데이터보다 완전히 오른쪽" },
        { false, 0, true,  true,  35, true,   3, "하한 없음, id <= 35" },
        { true, 75, true, false,   0, true,   3, "상한 없음, id >= 75" },
        { false, 0, true, false,   0, true,  10, "양끝 개방 = 전체 순회" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&c, 0, sizeof(c));
        bptree_range_scan(&pager, cases[i].hl, cases[i].lo, cases[i].li,
                          cases[i].hh, cases[i].hi, cases[i].hi_i,
                          collect_cb, &c);
        CHECK(c.count == cases[i].expect, "%s: %" PRIu64 "행 (기대 %" PRIu64 ")",
              cases[i].desc, (uint64_t)c.count, (uint64_t)cases[i].expect);
        CHECK(!c.order_broken, "%s: 결과가 오름차순이 아니다", cases[i].desc);
        free(c.keys);
    }

    model_free(&m);
    teardown(&pager);
}

int main(void) {
    printf("=== B+Tree Property Test Suite ===\n\n");

    test_random_insert();
    test_random_delete();
    test_mixed_churn();
    test_range_edges();

    printf("\n=== Results: %d/%d passed ===\n", g_run - g_fail, g_run);
    return g_fail == 0 ? 0 : 1;
}
