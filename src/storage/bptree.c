/*
 * bptree.c - 디스크 기반 B+ 트리 인덱스 (래치 커플링 동시성 제어)
 *
 * B+ 트리는 id 컬럼의 인덱스로 사용된다.
 * 모든 노드는 디스크의 한 페이지(page_size 바이트)에 대응하며,
 * 자식 참조는 메모리 포인터가 아닌 page_id로 표현한다.
 *
 * 래치 커플링 (Crab Protocol):
 *   트리 탐색 시 부모→자식 순서로 페이지 래치를 잡되,
 *   자식 래치 획득 후 부모가 "안전"하면 부모 래치를 해제한다.
 *
 *   검색: rlatch로 내려가며, 자식 rlatch 획득 후 부모 rlatch 해제.
 *   삽입: wlatch로 내려가며, safe node를 만나면 조상 wlatch 전부 해제.
 *   삭제: wlatch로 내려가며, safe node를 만나면 조상 wlatch 전부 해제.
 *
 *   safe node 판별:
 *     삽입: key_count < max  (키 하나 추가해도 분할 불필요)
 *     삭제: key_count > min  (키 하나 제거해도 병합 불필요)
 *
 *   이로써 대부분의 연산에서 루트~리프 전체가 아닌,
 *   실제 변경이 일어나는 소수의 노드만 wlatch를 유지한다.
 */

#include "storage/bptree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 용량 계산 헬퍼 ──
 *
 * 페이지에 들어갈 수 있는 최대/최소 키 개수를 계산한다.
 * 페이지 크기에서 헤더를 빼고 엔트리 크기로 나눈다.
 * 최소 키 개수 = 최대 / 2 (B+ 트리 불변량: 절반 이상 채워져야 함)
 */

static uint32_t max_leaf_keys(pager_t* p) {
  return (p->page_size - sizeof(leaf_page_header_t)) / sizeof(leaf_entry_t);
}

static uint32_t max_internal_keys(pager_t* p) {
  return (p->page_size - sizeof(internal_page_header_t)) /
         sizeof(internal_entry_t);
}

static uint32_t min_leaf_keys(pager_t* p) { return max_leaf_keys(p) / 2; }

static uint32_t min_internal_keys(pager_t* p) {
  return max_internal_keys(p) / 2;
}

/* ── 페이지 내부 접근 헬퍼 ── */

static leaf_entry_t* leaf_entries(uint8_t* page) {
  return (leaf_entry_t*)(page + sizeof(leaf_page_header_t));
}

static internal_entry_t* internal_entries(uint8_t* page) {
  return (internal_entry_t*)(page + sizeof(internal_page_header_t));
}

static uint32_t root_id(pager_t* p) { return p->header.root_index_page_id; }

/* ── 이진 탐색 ── */

/*
 * leaf_find - leaf 엔트리 배열에서 key 이상인 첫 번째 위치를 찾는다.
 * 반환값이 count 미만이고 해당 위치의 key가 일치하면 검색 성공.
 */
static uint32_t leaf_find(leaf_entry_t* entries, uint32_t count, uint64_t key) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (entries[mid].key < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/*
 * internal_find - internal 엔트리 배열에서 key가 속할 자식 인덱스를 찾는다.
 * 반환값 0이면 leftmost_child, i(>0)이면 entries[i-1].right_child.
 */
static uint32_t internal_find(internal_entry_t* entries, uint32_t count,
                              uint64_t key) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (entries[mid].key <= key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/*
 * internal_child_for_key - internal 노드에서 key가 속할 자식 페이지 ID를 반환.
 * (디버그 출력용으로 유지)
 */
static uint32_t internal_child_for_key(uint8_t* page, uint64_t key) {
  internal_page_header_t iph;
  memcpy(&iph, page, sizeof(iph));
  internal_entry_t* entries = internal_entries(page);
  uint32_t idx = internal_find(entries, iph.key_count, key);
  if (idx == 0)
    return iph.leftmost_child_page_id;
  return entries[idx - 1].right_child_page_id;
}

/* ══════════════════════════════════════════════════════════════════════
 *  래치 스택 (Crab Protocol 인프라)
 *
 *  wlatch를 잡고 있는 조상 노드의 page_id를 추적한다.
 *  safe node를 만나면 스택의 모든 조상 래치를 해제하고,
 *  분할/병합 전파 시에는 스택에서 부모를 꺼내 사용한다.
 *
 *  스택 레이아웃:
 *    pids[0]        = 루트 (또는 safe node 이후 첫 노드)
 *    pids[size-2]   = 리프의 부모
 *    pids[size-1]   = 리프
 * ══════════════════════════════════════════════════════════════════════ */

#define MAX_TREE_DEPTH 32

typedef struct {
  uint32_t pids[MAX_TREE_DEPTH];
  int size;
} latch_stack_t;

static void ls_init(latch_stack_t* s) { s->size = 0; }

static void ls_push(latch_stack_t* s, uint32_t pid) {
  if (s->size < MAX_TREE_DEPTH) s->pids[s->size++] = pid;
}

/* 스택의 모든 wlatch를 해제한다 (bottom → top) */
static void ls_release_all(pager_t* p, latch_stack_t* s) {
  for (int i = 0; i < s->size; i++)
    pager_unlatch_w(p, s->pids[i]);
  s->size = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  래치 커플링 탐색
 *
 *  find_leaf_rlatch: 검색용. rlatch coupling으로 leaf까지 내려간다.
 *    - 반환: leaf 페이지의 데이터 포인터 (rlatch 유지 상태)
 *    - 호출자가 pager_unlatch_r()로 해제해야 한다.
 *
 *  find_leaf_wlatch: 삽입/삭제용. wlatch coupling + safe-node 최적화.
 *    - 반환: leaf 페이지의 데이터 포인터 (wlatch 유지 상태)
 *    - stack에 wlatch 중인 조상 page_id가 담겨 있다.
 *    - 호출자가 ls_release_all()로 해제해야 한다.
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * find_leaf_rlatch - rlatch coupling으로 leaf를 찾는다.
 *
 * 동작:
 *   1. root를 rlatch로 잡는다.
 *   2. internal이면 자식 페이지를 rlatch로 잡고, 부모 rlatch를 해제한다.
 *   3. leaf에 도달하면 rlatch 유지 상태로 반환한다.
 *
 * root 변경 대응:
 *   래치 대기 중에 다른 스레드가 root split/shrink를 하면
 *   root_id가 바뀔 수 있다. 래치 획득 후 root_id를 재검증하고,
 *   변경되었으면 재시도한다.
 */
static uint8_t* find_leaf_rlatch(pager_t* pager, uint64_t key,
                                 uint32_t* out_pid) {
retry:;
  uint32_t pid = root_id(pager);
  uint8_t* page = pager_get_page_rlatch(pager, pid);
  if (!page) {
    *out_pid = pid;
    return NULL;
  }

  /* 래치 대기 중 root가 변경되었으면 재시도 */
  if (pid != root_id(pager)) {
    pager_unlatch_r(pager, pid);
    goto retry;
  }

  while (1) {
    uint32_t ptype;
    memcpy(&ptype, page, sizeof(uint32_t));
    if (ptype == PAGE_TYPE_LEAF) {
      *out_pid = pid;
      return page; /* rlatch 유지 상태로 반환 */
    }

    /* internal: 자식 선택 → 자식 rlatch → 부모 rlatch 해제 */
    uint32_t child_pid = internal_child_for_key(page, key);
    uint8_t* child_page = pager_get_page_rlatch(pager, child_pid);
    pager_unlatch_r(pager, pid); /* 부모 해제 (Crab Protocol) */
    pid = child_pid;
    page = child_page;
  }
}

/*
 * find_leaf_wlatch - wlatch coupling + safe-node 최적화로 leaf를 찾는다.
 *
 * 동작:
 *   1. root를 wlatch로 잡고 스택에 push.
 *   2. 자식을 wlatch로 잡는다.
 *   3. 자식이 "safe"이면 스택의 모든 조상 wlatch를 해제한다.
 *      (safe = 삽입/삭제 전파가 이 노드에서 멈출 수 있음)
 *   4. 자식을 스택에 push하고 반복.
 *   5. leaf에 도달하면 반환. 스택에 wlatch 중인 노드들이 남아 있다.
 *
 * safe 판별:
 *   삽입: key_count < max  → 키 하나 추가해도 분할 불필요
 *   삭제: key_count > min  → 키 하나 제거해도 병합 불필요
 */
static uint8_t* find_leaf_wlatch(pager_t* pager, uint64_t key,
                                 uint32_t* out_pid, latch_stack_t* stack,
                                 bool is_insert) {
retry:
  ls_init(stack);
  {
    uint32_t pid = root_id(pager);
    uint8_t* page = pager_get_page_wlatch(pager, pid);

    /* 래치 대기 중 root가 변경되었으면 재시도 */
    if (pid != root_id(pager)) {
      pager_unlatch_w(pager, pid);
      goto retry;
    }

    ls_push(stack, pid);

    while (1) {
      uint32_t ptype;
      memcpy(&ptype, page, sizeof(uint32_t));
      if (ptype == PAGE_TYPE_LEAF) {
        *out_pid = pid;
        return page; /* wlatch 유지 상태로 반환 */
      }

      /* internal: 자식 선택 */
      internal_page_header_t iph;
      memcpy(&iph, page, sizeof(iph));
      internal_entry_t* ents = internal_entries(page);
      uint32_t idx = internal_find(ents, iph.key_count, key);
      uint32_t child_pid =
          (idx == 0) ? iph.leftmost_child_page_id
                     : ents[idx - 1].right_child_page_id;

      /* 자식 wlatch 획득 */
      uint8_t* child_page = pager_get_page_wlatch(pager, child_pid);

      /* ── safe-node 판별 ── */
      uint32_t child_ptype;
      memcpy(&child_ptype, child_page, sizeof(uint32_t));
      bool safe = false;

      if (is_insert) {
        if (child_ptype == PAGE_TYPE_LEAF) {
          leaf_page_header_t lph;
          memcpy(&lph, child_page, sizeof(lph));
          safe = (lph.key_count < max_leaf_keys(pager));
        } else {
          internal_page_header_t ciph;
          memcpy(&ciph, child_page, sizeof(ciph));
          safe = (ciph.key_count < max_internal_keys(pager));
        }
      } else {
        /* 삭제: safe = 병합 불필요 */
        if (child_ptype == PAGE_TYPE_LEAF) {
          leaf_page_header_t lph;
          memcpy(&lph, child_page, sizeof(lph));
          safe = (lph.key_count > min_leaf_keys(pager));
        } else {
          internal_page_header_t ciph;
          memcpy(&ciph, child_page, sizeof(ciph));
          safe = (ciph.key_count > min_internal_keys(pager));
        }
      }

      /* safe이면 조상 wlatch 전부 해제 → 전파가 여기서 멈출 수 있으므로 */
      if (safe) ls_release_all(pager, stack);

      ls_push(stack, child_pid);
      pid = child_pid;
      page = child_page;
    }
  }
}

/* ══════════════════════════════════════════════════════════════════════
 *  검색 (Search) — rlatch coupling
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * bptree_search - B+ 트리에서 key를 검색하여 row_ref를 반환한다.
 *
 * rlatch coupling으로 leaf까지 내려가서 이진 탐색한다.
 * 동시에 다른 스레드의 삽입/삭제와 안전하게 공존한다.
 */
bool bptree_search(pager_t* pager, uint64_t key, row_ref_t* out_ref) {
  uint32_t leaf_pid;
  uint8_t* page = find_leaf_rlatch(pager, key, &leaf_pid);
  if (!page) return false;

  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  uint32_t idx = leaf_find(entries, lph.key_count, key);
  bool found = (idx < lph.key_count && entries[idx].key == key);
  if (found && out_ref) *out_ref = entries[idx].row_ref;

  pager_unlatch_r(pager, leaf_pid);
  return found;
}

/* ══════════════════════════════════════════════════════════════════════
 *  삽입 (Insert) — wlatch coupling + safe-node 최적화
 *
 *  삽입 흐름:
 *    find_leaf_wlatch()로 leaf를 찾는다 (조상 wlatch는 스택에).
 *      → leaf에 공간 있음 → 삽입 → 스택의 모든 래치 해제 → 끝
 *      → leaf가 가득 참  → split_leaf
 *          → propagate_insert()로 부모에 separator 전파
 *              → 부모에 공간 있음 → 삽입 → 래치 해제 → 끝
 *              → 부모도 가득 참  → 부모 split → 반복
 *                  → 루트까지 올라가면 → 새 루트 생성 (높이 +1)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * leaf_insert_entry - leaf 페이지에 (key, row_ref) 엔트리를 정렬 유지하며 삽입.
 * 호출 전 key_count < max_leaf_keys 보장 필요.
 */
static void leaf_insert_entry(uint8_t* page, leaf_page_header_t* lph,
                              uint64_t key, row_ref_t ref) {
  leaf_entry_t* entries = leaf_entries(page);
  uint32_t idx = leaf_find(entries, lph->key_count, key);
  for (uint32_t i = lph->key_count; i > idx; i--)
    entries[i] = entries[i - 1];
  entries[idx].key = key;
  entries[idx].row_ref = ref;
  lph->key_count++;
  memcpy(page, lph, sizeof(*lph));
}

/*
 * propagate_insert - 분할에서 발생한 separator를 부모에 전파한다.
 *
 * 반복적으로(iterative) 상위 노드에 separator를 삽입하고,
 * 부모도 가득 차면 부모를 분할하여 계속 위로 전파한다.
 * 루트까지 올라가면 새 루트를 생성한다.
 *
 * 매개변수:
 *   key       - 부모에 삽입할 separator 키
 *   right_pid - 분할로 생성된 새 오른쪽 노드의 page_id
 *   stack     - wlatch 중인 노드 스택 (top = 방금 분할한 노드)
 */
static void propagate_insert(pager_t* pager, uint64_t key, uint32_t right_pid,
                             latch_stack_t* stack) {
  int level = stack->size - 1; /* 방금 분할한 노드의 스택 인덱스 */
  uint64_t cur_key = key;
  uint32_t cur_right = right_pid;

  while (level > 0) {
    /* 부모는 level - 1 위치 */
    uint32_t parent_pid = stack->pids[level - 1];
    uint8_t* pp = pager_get_page(pager, parent_pid); /* pin++ (wlatch 유지) */
    internal_page_header_t iph;
    memcpy(&iph, pp, sizeof(iph));

    if (iph.key_count < max_internal_keys(pager)) {
      /* ── 부모에 공간 있음 → separator 삽입 ── */
      internal_entry_t* entries = internal_entries(pp);
      uint32_t idx = internal_find(entries, iph.key_count, cur_key);
      for (uint32_t i = iph.key_count; i > idx; i--)
        entries[i] = entries[i - 1];
      entries[idx].key = cur_key;
      entries[idx].right_child_page_id = cur_right;
      iph.key_count++;
      memcpy(pp, &iph, sizeof(iph));
      pager_mark_dirty(pager, parent_pid);
      pager_unpin(pager, parent_pid); /* pin-- */

      /* 오른쪽 자식의 parent_page_id 갱신 */
      uint8_t* rcp = pager_get_page(pager, cur_right);
      memcpy(rcp + 4, &parent_pid, sizeof(uint32_t));
      pager_mark_dirty(pager, cur_right);
      pager_unpin(pager, cur_right);

      ls_release_all(pager, stack);
      return;
    }

    /* ── 부모도 가득 참 → 부모를 분할 ── */

    /* 오른쪽 자식의 parent를 현재 부모로 설정 (분할 후 재조정됨) */
    {
      uint8_t* rcp = pager_get_page(pager, cur_right);
      memcpy(rcp + 4, &parent_pid, sizeof(uint32_t));
      pager_mark_dirty(pager, cur_right);
      pager_unpin(pager, cur_right);
    }

    /* 기존 엔트리 + 새 separator를 임시 배열에 정렬 합침 */
    internal_entry_t* entries = internal_entries(pp);
    uint32_t total = iph.key_count;
    uint32_t mk = max_internal_keys(pager);

    internal_entry_t* tmp =
        (internal_entry_t*)malloc((total + 1) * sizeof(internal_entry_t));
    uint32_t ins = internal_find(entries, total, cur_key);
    memcpy(tmp, entries, ins * sizeof(internal_entry_t));
    tmp[ins].key = cur_key;
    tmp[ins].right_child_page_id = cur_right;
    memcpy(tmp + ins + 1, entries + ins,
           (total - ins) * sizeof(internal_entry_t));
    total++;

    uint32_t split = total / 2;
    uint64_t promote_key = tmp[split].key;

    /* 오른쪽 절반을 새 internal 페이지에 저장 */
    uint32_t new_pid = pager_alloc_page(pager);
    uint8_t* new_page = pager_get_page(pager, new_pid);
    memset(new_page, 0, pager->page_size);

    internal_page_header_t new_iph = {
        .page_type = PAGE_TYPE_INTERNAL,
        .parent_page_id = iph.parent_page_id,
        .key_count = total - split - 1,
        .leftmost_child_page_id = tmp[split].right_child_page_id};
    memcpy(new_page, &new_iph, sizeof(new_iph));
    memcpy(internal_entries(new_page), tmp + split + 1,
           (total - split - 1) * sizeof(internal_entry_t));
    pager_mark_dirty(pager, new_pid);

    /* 새 오른쪽 노드 자식들의 parent_page_id를 갱신 */
    {
      uint8_t* cp = pager_get_page(pager, new_iph.leftmost_child_page_id);
      uint32_t pp_val = new_pid;
      memcpy(cp + 4, &pp_val, sizeof(uint32_t));
      pager_mark_dirty(pager, new_iph.leftmost_child_page_id);
      pager_unpin(pager, new_iph.leftmost_child_page_id);

      internal_entry_t* ne = internal_entries(new_page);
      for (uint32_t i = 0; i < new_iph.key_count; i++) {
        cp = pager_get_page(pager, ne[i].right_child_page_id);
        memcpy(cp + 4, &pp_val, sizeof(uint32_t));
        pager_mark_dirty(pager, ne[i].right_child_page_id);
        pager_unpin(pager, ne[i].right_child_page_id);
      }
    }

    /* 원래 노드를 왼쪽 절반으로 갱신 */
    iph.key_count = split;
    memcpy(pp, &iph, sizeof(iph));
    memcpy(internal_entries(pp), tmp, split * sizeof(internal_entry_t));
    memset(internal_entries(pp) + split, 0,
           (mk - split) * sizeof(internal_entry_t));
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid); /* pin-- */

    free(tmp);
    pager_unpin(pager, new_pid);

    /* 상위로 계속 전파 */
    cur_key = promote_key;
    cur_right = new_pid;
    level--;
  }

  /* ── level == 0: 루트가 분할됨 → 새 루트 생성 ── */
  uint32_t old_root = stack->pids[0];
  uint32_t new_root = pager_alloc_page(pager);
  uint8_t* rp = pager_get_page(pager, new_root);
  memset(rp, 0, pager->page_size);

  internal_page_header_t root_iph = {.page_type = PAGE_TYPE_INTERNAL,
                                     .parent_page_id = 0,
                                     .key_count = 1,
                                     .leftmost_child_page_id = old_root};
  memcpy(rp, &root_iph, sizeof(root_iph));
  internal_entry_t e = {.key = cur_key, .right_child_page_id = cur_right};
  memcpy(internal_entries(rp), &e, sizeof(e));
  pager_mark_dirty(pager, new_root);
  pager_unpin(pager, new_root);

  /* 양쪽 자식의 parent를 새 루트로 갱신 */
  uint8_t* lp = pager_get_page(pager, old_root);
  memcpy(lp + 4, &new_root, sizeof(uint32_t));
  pager_mark_dirty(pager, old_root);
  pager_unpin(pager, old_root);

  uint8_t* rrp = pager_get_page(pager, cur_right);
  memcpy(rrp + 4, &new_root, sizeof(uint32_t));
  pager_mark_dirty(pager, cur_right);
  pager_unpin(pager, cur_right);

  /* root_id를 갱신 — old_root의 wlatch가 아직 유지 중이므로
   * 다른 스레드는 old_root에 접근할 수 없다.
   * ls_release_all 후에야 다른 스레드가 새 루트를 통해 접근 가능. */
  pager->header.root_index_page_id = new_root;
  pager->header_dirty = true;
  ls_release_all(pager, stack);
}

/*
 * bptree_insert - B+ 트리에 (key, row_ref) 쌍을 삽입한다.
 *
 * 반환값: 0 = 성공, -1 = 중복 키
 */
int bptree_insert(pager_t* pager, uint64_t key, row_ref_t ref) {
  latch_stack_t stack;
  uint32_t leaf_pid;
  uint8_t* page = find_leaf_wlatch(pager, key, &leaf_pid, &stack, true);

  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  /* 중복 키 체크 */
  uint32_t idx = leaf_find(entries, lph.key_count, key);
  if (idx < lph.key_count && entries[idx].key == key) {
    ls_release_all(pager, &stack);
    return -1;
  }

  if (lph.key_count < max_leaf_keys(pager)) {
    /* ── leaf에 공간 있음 → 바로 삽입 ── */
    leaf_insert_entry(page, &lph, key, ref);
    pager_mark_dirty(pager, leaf_pid);
    ls_release_all(pager, &stack);
    return 0;
  }

  /* ── leaf가 가득 참 → 분할 필요 ──
   *
   * 이 시점에서 leaf는 safe가 아니었으므로 (key_count == max),
   * find_leaf_wlatch가 조상 wlatch를 해제하지 않았다.
   * 스택에 root~leaf까지의 경로가 모두 wlatch 상태로 남아 있다.
   */
  uint32_t total = lph.key_count;
  uint32_t mk = max_leaf_keys(pager);

  /* 기존 + 새 엔트리를 임시 배열에 정렬 합침 */
  leaf_entry_t* tmp =
      (leaf_entry_t*)malloc((total + 1) * sizeof(leaf_entry_t));
  uint32_t ins = leaf_find(entries, total, key);
  memcpy(tmp, entries, ins * sizeof(leaf_entry_t));
  tmp[ins].key = key;
  tmp[ins].row_ref = ref;
  memcpy(tmp + ins + 1, entries + ins, (total - ins) * sizeof(leaf_entry_t));
  total++;

  uint32_t split = total / 2;

  /* 오른쪽 절반을 새 leaf 페이지에 저장 */
  uint32_t new_pid = pager_alloc_page(pager);
  uint8_t* new_page = pager_get_page(pager, new_pid);
  memset(new_page, 0, pager->page_size);

  leaf_page_header_t new_lph = {
      .page_type = PAGE_TYPE_LEAF,
      .parent_page_id = lph.parent_page_id,
      .key_count = total - split,
      .next_leaf_page_id = lph.next_leaf_page_id,
      .prev_leaf_page_id = leaf_pid};
  memcpy(new_page, &new_lph, sizeof(new_lph));
  memcpy(leaf_entries(new_page), tmp + split,
         (total - split) * sizeof(leaf_entry_t));
  pager_mark_dirty(pager, new_pid);

  /* 기존 다음 이웃의 prev 포인터를 새 노드로 갱신 */
  if (lph.next_leaf_page_id != 0) {
    uint8_t* rp = pager_get_page(pager, lph.next_leaf_page_id);
    leaf_page_header_t rlph;
    memcpy(&rlph, rp, sizeof(rlph));
    rlph.prev_leaf_page_id = new_pid;
    memcpy(rp, &rlph, sizeof(rlph));
    pager_mark_dirty(pager, lph.next_leaf_page_id);
    pager_unpin(pager, lph.next_leaf_page_id);
  }

  /* 원래 leaf를 왼쪽 절반으로 갱신 */
  lph.key_count = split;
  lph.next_leaf_page_id = new_pid;
  memcpy(page, &lph, sizeof(lph));
  memcpy(leaf_entries(page), tmp, split * sizeof(leaf_entry_t));
  memset(leaf_entries(page) + split, 0, (mk - split) * sizeof(leaf_entry_t));
  pager_mark_dirty(pager, leaf_pid);

  uint64_t promote_key = tmp[split].key;
  free(tmp);

  pager_unpin(pager, new_pid);

  /* 부모에 separator 전파 (스택을 사용하여 반복적으로 올라감) */
  propagate_insert(pager, promote_key, new_pid, &stack);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  삭제 (Delete) — wlatch coupling + safe-node 최적화
 *
 *  삭제 흐름:
 *    find_leaf_wlatch()로 leaf를 찾는다 (조상 wlatch는 스택에).
 *      → leaf에서 엔트리 제거
 *      → underflow 없으면 → 래치 해제 → 끝
 *      → underflow이면:
 *          1순위: 오른쪽 형제에서 borrow (wlatch로 잠금)
 *          2순위: 왼쪽 형제에서 borrow (wlatch로 잠금)
 *          3순위: 형제와 merge → 부모 separator 제거
 *              → propagate_delete()로 부모 underflow 처리
 *                  → 루트 key=0이면 root shrink (높이 -1)
 * ══════════════════════════════════════════════════════════════════════ */

/* leaf에서 idx 위치의 엔트리를 제거한다 */
static void delete_entry_from_leaf(uint8_t* page, leaf_page_header_t* lph,
                                   uint32_t idx) {
  leaf_entry_t* entries = leaf_entries(page);
  for (uint32_t i = idx; i < lph->key_count - 1; i++)
    entries[i] = entries[i + 1];
  lph->key_count--;
  memcpy(page, lph, sizeof(*lph));
}

/* internal에서 idx 위치의 엔트리를 제거한다 */
static void delete_entry_from_internal(uint8_t* page,
                                       internal_page_header_t* iph,
                                       uint32_t idx) {
  internal_entry_t* entries = internal_entries(page);
  for (uint32_t i = idx; i < iph->key_count - 1; i++)
    entries[i] = entries[i + 1];
  iph->key_count--;
  memcpy(page, iph, sizeof(*iph));
}

/*
 * find_child_idx - parent 내에서 child_pid가 몇 번째 자식인지 찾는다.
 *
 * 반환값:
 *   0          = leftmost_child
 *   i (i > 0)  = entries[i-1].right_child
 *   -1         = 찾지 못함
 */
static int find_child_idx(uint8_t* parent_page, uint32_t child_pid) {
  internal_page_header_t iph;
  memcpy(&iph, parent_page, sizeof(iph));
  if (iph.leftmost_child_page_id == child_pid) return 0;
  internal_entry_t* entries = internal_entries(parent_page);
  for (uint32_t i = 0; i < iph.key_count; i++) {
    if (entries[i].right_child_page_id == child_pid) return (int)i + 1;
  }
  return -1;
}

/* 전방 선언 */
static void fix_leaf_underflow_latched(pager_t* pager, uint32_t leaf_pid,
                                       uint8_t* leaf_page,
                                       latch_stack_t* stack);
static void fix_internal_underflow_latched(pager_t* pager, uint32_t node_pid,
                                           uint8_t* node_page,
                                           latch_stack_t* stack,
                                           int node_level);

static void update_child_parent_page(pager_t* pager, uint32_t child_pid,
                                     uint32_t parent_pid) {
  uint8_t* cp = pager_get_page(pager, child_pid);
  memcpy(cp + 4, &parent_pid, sizeof(uint32_t));
  pager_mark_dirty(pager, child_pid);
  pager_unpin(pager, child_pid);
}

/*
 * propagate_delete - merge 후 internal 노드의 underflow를 처리한다.
 *
 * parent_level: 스택에서 separator가 제거된 부모의 인덱스.
 *
 * 처리:
 *   - 루트이고 key=0 → root shrink (유일한 자식을 새 루트로)
 *   - 루트 아니고 key >= min → 문제없음
 *   - 루트 아니고 key < min → 최소 구현에서는 허용
 *     (internal borrow/merge는 미구현. 정합성에 영향 없음)
 */
static void propagate_delete(pager_t* pager, latch_stack_t* stack,
                             int parent_level) {
  uint32_t node_pid = stack->pids[parent_level];

  if (node_pid == root_id(pager)) {
    /* 루트 축소: key가 0이면 유일한 자식을 새 루트로 승격 */
    uint8_t* page = pager_get_page(pager, node_pid); /* pin++ */
    internal_page_header_t iph;
    memcpy(&iph, page, sizeof(iph));
    if (iph.key_count == 0) {
      uint32_t child = iph.leftmost_child_page_id;
      pager_unpin(pager, node_pid); /* pin-- */
      pager_free_page(pager, node_pid);

      pager->header.root_index_page_id = child;
      pager->header_dirty = true;

      /* 새 루트의 parent를 0으로 설정 */
      uint8_t* cp = pager_get_page(pager, child);
      uint32_t zero = 0;
      memcpy(cp + 4, &zero, sizeof(uint32_t));
      pager_mark_dirty(pager, child);
      pager_unpin(pager, child);
    } else {
      pager_unpin(pager, node_pid);
    }
    ls_release_all(pager, stack);
    return;
  }

  /* 루트가 아닌 internal 노드의 underflow 확인 */
  uint8_t* page = pager_get_page(pager, node_pid);
  internal_page_header_t iph;
  memcpy(&iph, page, sizeof(iph));
  pager_unpin(pager, node_pid);

  if (iph.key_count >= min_internal_keys(pager)) {
    ls_release_all(pager, stack);
    return;
  }

  fix_internal_underflow_latched(pager, node_pid, page, stack, parent_level);
}

static void fix_internal_underflow_latched(pager_t* pager, uint32_t node_pid,
                                           uint8_t* node_page,
                                           latch_stack_t* stack,
                                           int node_level) {
  if (node_level == 0) {
    pager_unpin(pager, node_pid);
    ls_release_all(pager, stack);
    return;
  }

  uint32_t parent_pid = stack->pids[node_level - 1];
  uint8_t* pp = pager_get_page(pager, parent_pid);
  internal_page_header_t iph;
  memcpy(&iph, pp, sizeof(iph));
  internal_entry_t* pentries = internal_entries(pp);

  internal_page_header_t niph;
  memcpy(&niph, node_page, sizeof(niph));
  internal_entry_t* nentries = internal_entries(node_page);

  int child_idx = find_child_idx(pp, node_pid);
  if (child_idx < 0) {
    pager_unpin(pager, node_pid);
    pager_unpin(pager, parent_pid);
    ls_release_all(pager, stack);
    return;
  }

  /* ── 1순위: 오른쪽 형제에서 borrow ── */
  if (child_idx < (int)iph.key_count) {
    uint32_t sep_idx = (uint32_t)child_idx;
    uint32_t rsib_pid = pentries[sep_idx].right_child_page_id;
    uint8_t* rpage = pager_get_page_wlatch(pager, rsib_pid);
    internal_page_header_t riph;
    memcpy(&riph, rpage, sizeof(riph));

    if (riph.key_count > min_internal_keys(pager)) {
      internal_entry_t* rentries = internal_entries(rpage);
      uint32_t moved_child = riph.leftmost_child_page_id;

      nentries[niph.key_count].key = pentries[sep_idx].key;
      nentries[niph.key_count].right_child_page_id = moved_child;
      niph.key_count++;

      pentries[sep_idx].key = rentries[0].key;
      riph.leftmost_child_page_id = rentries[0].right_child_page_id;
      for (uint32_t i = 0; i < riph.key_count - 1; i++) {
        rentries[i] = rentries[i + 1];
      }
      riph.key_count--;

      memcpy(node_page, &niph, sizeof(niph));
      memcpy(rpage, &riph, sizeof(riph));
      memcpy(pp, &iph, sizeof(iph));
      pager_mark_dirty(pager, node_pid);
      pager_mark_dirty(pager, rsib_pid);
      pager_mark_dirty(pager, parent_pid);
      update_child_parent_page(pager, moved_child, node_pid);

      pager_unpin(pager, node_pid);
      pager_unlatch_w(pager, rsib_pid);
      pager_unpin(pager, parent_pid);
      ls_release_all(pager, stack);
      return;
    }
    pager_unlatch_w(pager, rsib_pid);
  }

  /* ── 2순위: 왼쪽 형제에서 borrow ── */
  if (child_idx > 0) {
    uint32_t sep_idx = (uint32_t)(child_idx - 1);
    uint32_t lsib_pid = (child_idx == 1)
                            ? iph.leftmost_child_page_id
                            : pentries[child_idx - 2].right_child_page_id;
    uint8_t* lpage = pager_get_page_wlatch(pager, lsib_pid);
    internal_page_header_t liph;
    memcpy(&liph, lpage, sizeof(liph));

    if (liph.key_count > min_internal_keys(pager)) {
      internal_entry_t* lentries = internal_entries(lpage);
      internal_entry_t borrowed = lentries[liph.key_count - 1];

      for (uint32_t i = niph.key_count; i > 0; i--) {
        nentries[i] = nentries[i - 1];
      }
      nentries[0].key = pentries[sep_idx].key;
      nentries[0].right_child_page_id = niph.leftmost_child_page_id;
      niph.leftmost_child_page_id = borrowed.right_child_page_id;
      niph.key_count++;

      pentries[sep_idx].key = borrowed.key;
      liph.key_count--;

      memcpy(node_page, &niph, sizeof(niph));
      memcpy(lpage, &liph, sizeof(liph));
      memcpy(pp, &iph, sizeof(iph));
      pager_mark_dirty(pager, node_pid);
      pager_mark_dirty(pager, lsib_pid);
      pager_mark_dirty(pager, parent_pid);
      update_child_parent_page(pager, niph.leftmost_child_page_id, node_pid);

      pager_unpin(pager, node_pid);
      pager_unlatch_w(pager, lsib_pid);
      pager_unpin(pager, parent_pid);
      ls_release_all(pager, stack);
      return;
    }
    pager_unlatch_w(pager, lsib_pid);
  }

  /* ── 3순위: merge (합병) ── */
  if (child_idx > 0) {
    uint32_t sep_idx = (uint32_t)(child_idx - 1);
    uint32_t lsib_pid = (child_idx == 1)
                            ? iph.leftmost_child_page_id
                            : pentries[child_idx - 2].right_child_page_id;
    uint8_t* lpage = pager_get_page_wlatch(pager, lsib_pid);
    internal_page_header_t liph;
    memcpy(&liph, lpage, sizeof(liph));
    internal_entry_t* lentries = internal_entries(lpage);

    uint32_t base = liph.key_count;
    lentries[base].key = pentries[sep_idx].key;
    lentries[base].right_child_page_id = niph.leftmost_child_page_id;
    memcpy(lentries + base + 1, nentries, niph.key_count * sizeof(internal_entry_t));
    liph.key_count = base + 1 + niph.key_count;
    memcpy(lpage, &liph, sizeof(liph));
    pager_mark_dirty(pager, lsib_pid);

    update_child_parent_page(pager, niph.leftmost_child_page_id, lsib_pid);
    for (uint32_t i = 0; i < niph.key_count; i++) {
      update_child_parent_page(pager, nentries[i].right_child_page_id, lsib_pid);
    }

    delete_entry_from_internal(pp, &iph, sep_idx);
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    pager_unpin(pager, node_pid);
    pager_unlatch_w(pager, lsib_pid);
    pager_free_page(pager, node_pid);
    propagate_delete(pager, stack, node_level - 1);
    return;
  } else {
    uint32_t rsib_pid = pentries[0].right_child_page_id;
    uint8_t* rpage = pager_get_page_wlatch(pager, rsib_pid);
    internal_page_header_t riph;
    memcpy(&riph, rpage, sizeof(riph));
    internal_entry_t* rentries = internal_entries(rpage);

    uint32_t base = niph.key_count;
    nentries[base].key = pentries[0].key;
    nentries[base].right_child_page_id = riph.leftmost_child_page_id;
    memcpy(nentries + base + 1, rentries, riph.key_count * sizeof(internal_entry_t));
    niph.key_count = base + 1 + riph.key_count;
    memcpy(node_page, &niph, sizeof(niph));
    pager_mark_dirty(pager, node_pid);

    update_child_parent_page(pager, riph.leftmost_child_page_id, node_pid);
    for (uint32_t i = 0; i < riph.key_count; i++) {
      update_child_parent_page(pager, rentries[i].right_child_page_id, node_pid);
    }

    delete_entry_from_internal(pp, &iph, 0);
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    pager_unpin(pager, node_pid);
    pager_unlatch_w(pager, rsib_pid);
    pager_free_page(pager, rsib_pid);
    propagate_delete(pager, stack, node_level - 1);
    return;
  }
}

/*
 * fix_leaf_underflow_latched - 삭제 후 leaf의 underflow를 복구한다.
 *
 * 스택에 leaf와 그 조상이 wlatch 상태로 있다.
 * 부모의 wlatch 덕분에 형제 노드에 안전하게 접근할 수 있다
 * (다른 스레드가 부모를 통과해 형제에 접근할 수 없으므로).
 *
 * 복구 전략 (우선순위 순서):
 *   1. 오른쪽 형제에서 borrow
 *   2. 왼쪽 형제에서 borrow
 *   3. 형제와 merge
 */
static void fix_leaf_underflow_latched(pager_t* pager, uint32_t leaf_pid,
                                       uint8_t* leaf_page,
                                       latch_stack_t* stack) {
  int leaf_level = stack->size - 1;

  /* 스택에 부모가 없으면 (leaf가 root) → 복구 불필요 */
  if (leaf_level == 0) {
    ls_release_all(pager, stack);
    return;
  }

  /* 부모 접근 (wlatch 유지 중, pin++ 추가) */
  uint32_t parent_pid = stack->pids[leaf_level - 1];
  uint8_t* pp = pager_get_page(pager, parent_pid);
  internal_page_header_t iph;
  memcpy(&iph, pp, sizeof(iph));
  internal_entry_t* pentries = internal_entries(pp);

  int child_idx = find_child_idx(pp, leaf_pid);
  if (child_idx < 0) {
    pager_unpin(pager, parent_pid);
    ls_release_all(pager, stack);
    return;
  }

  /* ── 1순위: 오른쪽 형제에서 borrow ── */
  if (child_idx < (int)iph.key_count) {
    uint32_t sep_idx = (uint32_t)child_idx;
    uint32_t rsib_pid = pentries[sep_idx].right_child_page_id;

    /* 형제를 wlatch (부모 wlatch가 있으므로 데드락 없음) */
    uint8_t* rpage = pager_get_page_wlatch(pager, rsib_pid);
    leaf_page_header_t rlph;
    memcpy(&rlph, rpage, sizeof(rlph));

    if (rlph.key_count > min_leaf_keys(pager)) {
      /* 오른쪽 형제의 첫 엔트리를 빌려온다 */
      leaf_entry_t* rentries = leaf_entries(rpage);
      leaf_entry_t borrowed = rentries[0];
      delete_entry_from_leaf(rpage, &rlph, 0);

      /* 새 separator 키를 래치 해제 전에 읽는다 */
      uint64_t new_sep_key = leaf_entries(rpage)[0].key;
      pager_mark_dirty(pager, rsib_pid);
      pager_unlatch_w(pager, rsib_pid);

      /* 현재 leaf에 삽입 (leaf_page는 wlatch 유지 중) */
      leaf_page_header_t lph;
      memcpy(&lph, leaf_page, sizeof(lph));
      leaf_insert_entry(leaf_page, &lph, borrowed.key, borrowed.row_ref);
      pager_mark_dirty(pager, leaf_pid);

      /* 부모 separator 갱신 */
      pentries[sep_idx].key = new_sep_key;
      memcpy(pp, &iph, sizeof(iph));
      pager_mark_dirty(pager, parent_pid);
      pager_unpin(pager, parent_pid);

      ls_release_all(pager, stack);
      return;
    }
    pager_unlatch_w(pager, rsib_pid);
  }

  /* ── 2순위: 왼쪽 형제에서 borrow ── */
  if (child_idx > 0) {
    uint32_t sep_idx = (uint32_t)(child_idx - 1);
    uint32_t lsib_pid = (child_idx == 1)
                            ? iph.leftmost_child_page_id
                            : pentries[child_idx - 2].right_child_page_id;

    uint8_t* lpage = pager_get_page_wlatch(pager, lsib_pid);
    leaf_page_header_t llph;
    memcpy(&llph, lpage, sizeof(llph));

    if (llph.key_count > min_leaf_keys(pager)) {
      /* 왼쪽 형제의 마지막 엔트리를 빌려온다 */
      leaf_entry_t* lentries = leaf_entries(lpage);
      leaf_entry_t borrowed = lentries[llph.key_count - 1];
      llph.key_count--;
      memcpy(lpage, &llph, sizeof(llph));
      pager_mark_dirty(pager, lsib_pid);
      pager_unlatch_w(pager, lsib_pid);

      /* 현재 leaf에 삽입 */
      leaf_page_header_t lph;
      memcpy(&lph, leaf_page, sizeof(lph));
      leaf_insert_entry(leaf_page, &lph, borrowed.key, borrowed.row_ref);
      pager_mark_dirty(pager, leaf_pid);

      /* 부모 separator를 현재 leaf의 새 첫 번째 키로 갱신 */
      pentries[sep_idx].key = leaf_entries(leaf_page)[0].key;
      memcpy(pp, &iph, sizeof(iph));
      pager_mark_dirty(pager, parent_pid);
      pager_unpin(pager, parent_pid);

      ls_release_all(pager, stack);
      return;
    }
    pager_unlatch_w(pager, lsib_pid);
  }

  /* ── 3순위: merge (합병) ── */

  if (child_idx > 0) {
    /*
     * 왼쪽 형제와 merge:
     *   현재 leaf의 모든 엔트리를 왼쪽 형제에 복사한다.
     *   현재 leaf 페이지는 free list에 반환한다.
     *   부모에서 separator를 제거한다.
     */
    uint32_t sep_idx = (uint32_t)(child_idx - 1);
    uint32_t lsib_pid = (child_idx == 1)
                            ? iph.leftmost_child_page_id
                            : pentries[child_idx - 2].right_child_page_id;

    uint8_t* lpage = pager_get_page_wlatch(pager, lsib_pid);
    leaf_page_header_t llph;
    memcpy(&llph, lpage, sizeof(llph));

    leaf_page_header_t lph;
    memcpy(&lph, leaf_page, sizeof(lph));
    leaf_entry_t* cur_entries = leaf_entries(leaf_page);
    leaf_entry_t* left_entries = leaf_entries(lpage);

    /* 현재 leaf의 엔트리를 왼쪽 형제에 복사 */
    for (uint32_t i = 0; i < lph.key_count; i++)
      left_entries[llph.key_count + i] = cur_entries[i];
    llph.key_count += lph.key_count;
    llph.next_leaf_page_id = lph.next_leaf_page_id;
    memcpy(lpage, &llph, sizeof(llph));
    pager_mark_dirty(pager, lsib_pid);
    pager_unlatch_w(pager, lsib_pid);

    /* 다음 이웃의 prev 포인터를 왼쪽 형제로 갱신 */
    if (lph.next_leaf_page_id != 0) {
      uint8_t* np = pager_get_page(pager, lph.next_leaf_page_id);
      leaf_page_header_t nlph;
      memcpy(&nlph, np, sizeof(nlph));
      nlph.prev_leaf_page_id = lsib_pid;
      memcpy(np, &nlph, sizeof(nlph));
      pager_mark_dirty(pager, lph.next_leaf_page_id);
      pager_unpin(pager, lph.next_leaf_page_id);
    }

    /* 부모에서 separator 제거 */
    delete_entry_from_internal(pp, &iph, sep_idx);
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    /* leaf를 free list에 반환 (wlatch가 아직 유지 중이므로 안전) */
    pager_free_page(pager, leaf_pid);

    /* 부모 underflow 확인 */
    propagate_delete(pager, stack, leaf_level - 1);
  } else {
    /*
     * 오른쪽 형제와 merge (child_idx == 0):
     *   오른쪽 형제의 엔트리를 현재 leaf에 복사한다.
     *   오른쪽 형제 페이지는 free list에 반환한다.
     */
    uint32_t rsib_pid = pentries[0].right_child_page_id;

    leaf_page_header_t lph;
    memcpy(&lph, leaf_page, sizeof(lph));
    leaf_entry_t* cur_entries = leaf_entries(leaf_page);

    uint8_t* rpage = pager_get_page_wlatch(pager, rsib_pid);
    leaf_page_header_t rlph;
    memcpy(&rlph, rpage, sizeof(rlph));
    leaf_entry_t* rentries = leaf_entries(rpage);

    /* 오른쪽 형제의 엔트리를 현재 leaf에 복사 */
    for (uint32_t i = 0; i < rlph.key_count; i++)
      cur_entries[lph.key_count + i] = rentries[i];
    lph.key_count += rlph.key_count;
    lph.next_leaf_page_id = rlph.next_leaf_page_id;
    memcpy(leaf_page, &lph, sizeof(lph));
    pager_mark_dirty(pager, leaf_pid);
    pager_unlatch_w(pager, rsib_pid);

    /* 오른쪽 형제의 다음 이웃의 prev를 현재 leaf로 갱신 */
    if (rlph.next_leaf_page_id != 0) {
      uint8_t* np = pager_get_page(pager, rlph.next_leaf_page_id);
      leaf_page_header_t nlph;
      memcpy(&nlph, np, sizeof(nlph));
      nlph.prev_leaf_page_id = leaf_pid;
      memcpy(np, &nlph, sizeof(nlph));
      pager_mark_dirty(pager, rlph.next_leaf_page_id);
      pager_unpin(pager, rlph.next_leaf_page_id);
    }

    /* 부모에서 첫 번째 separator 제거 */
    delete_entry_from_internal(pp, &iph, 0);
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    /* 오른쪽 형제를 free list에 반환 */
    pager_free_page(pager, rsib_pid);

    /* 부모 underflow 확인 */
    propagate_delete(pager, stack, leaf_level - 1);
  }
}

/*
 * bptree_delete - B+ 트리에서 key를 삭제한다.
 *
 * 반환값: 0 = 성공, -1 = 키 없음
 */
int bptree_delete(pager_t* pager, uint64_t key) {
  latch_stack_t stack;
  uint32_t leaf_pid;
  uint8_t* page = find_leaf_wlatch(pager, key, &leaf_pid, &stack, false);

  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  /* 삭제할 키의 위치를 이진 탐색으로 찾는다 */
  uint32_t idx = leaf_find(entries, lph.key_count, key);
  if (idx >= lph.key_count || entries[idx].key != key) {
    ls_release_all(pager, &stack);
    return -1;
  }

  /* 엔트리 제거 */
  delete_entry_from_leaf(page, &lph, idx);
  pager_mark_dirty(pager, leaf_pid);

  /* underflow 확인: 루트이거나 키가 충분하면 복구 불필요 */
  if (leaf_pid == root_id(pager) || lph.key_count >= min_leaf_keys(pager)) {
    ls_release_all(pager, &stack);
    return 0;
  }

  /* underflow → borrow 또는 merge 수행 */
  fix_leaf_underflow_latched(pager, leaf_pid, page, &stack);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  디버그 (Debug)
 *
 *  단일 스레드 디버그 도구이므로 래치 없이 pin/unpin만 사용한다.
 * ══════════════════════════════════════════════════════════════════════ */

static void print_node(pager_t* pager, uint32_t pid, int depth) {
  uint8_t* page = pager_get_page(pager, pid);
  uint32_t ptype;
  memcpy(&ptype, page, sizeof(uint32_t));

  for (int i = 0; i < depth; i++)
    printf("  ");

  if (ptype == PAGE_TYPE_LEAF) {
    leaf_page_header_t lph;
    memcpy(&lph, page, sizeof(lph));
    leaf_entry_t* entries = leaf_entries(page);
    printf("[LEAF page=%u] keys=%u:", pid, lph.key_count);
    for (uint32_t i = 0; i < lph.key_count && i < 5; i++)
      printf(" %lu", (unsigned long)entries[i].key);
    if (lph.key_count > 5) printf(" ...");
    printf("\n");
  } else if (ptype == PAGE_TYPE_INTERNAL) {
    internal_page_header_t iph;
    memcpy(&iph, page, sizeof(iph));
    internal_entry_t* entries = internal_entries(page);
    printf("[INTERNAL page=%u] keys=%u:", pid, iph.key_count);
    for (uint32_t i = 0; i < iph.key_count && i < 5; i++)
      printf(" %lu", (unsigned long)entries[i].key);
    if (iph.key_count > 5) printf(" ...");
    printf("\n");
    pager_unpin(pager, pid);

    page = pager_get_page(pager, pid);
    memcpy(&iph, page, sizeof(iph));
    entries = internal_entries(page);

    print_node(pager, iph.leftmost_child_page_id, depth + 1);
    for (uint32_t i = 0; i < iph.key_count; i++)
      print_node(pager, entries[i].right_child_page_id, depth + 1);
    pager_unpin(pager, pid);
    return;
  }
  pager_unpin(pager, pid);
}

void bptree_print(pager_t* pager) {
  printf("B+ Tree (root: page %u)\n", root_id(pager));
  print_node(pager, root_id(pager), 1);
}

int bptree_height(pager_t* pager) {
  int h = 0;
  uint32_t pid = root_id(pager);
  while (1) {
    h++;
    uint8_t* page = pager_get_page(pager, pid);
    uint32_t ptype;
    memcpy(&ptype, page, sizeof(uint32_t));
    if (ptype == PAGE_TYPE_LEAF) {
      pager_unpin(pager, pid);
      return h;
    }
    internal_page_header_t iph;
    memcpy(&iph, page, sizeof(iph));
    pager_unpin(pager, pid);
    pid = iph.leftmost_child_page_id;
  }
}
