/*
 * bptree.c - 디스크 기반 B+ 트리 인덱스
 *
 * B+ 트리는 id 컬럼의 인덱스로 사용된다.
 * 모든 노드는 디스크의 한 페이지(page_size 바이트)에 대응하며,
 * 자식 참조는 메모리 포인터가 아닌 page_id로 표현한다.
 *
 * 구조:
 *   - leaf 노드: (key, row_ref) 쌍을 정렬 저장. 양방향 연결 리스트.
 *   - internal 노드: 라우팅 전용. key와 right_child_page_id를 저장하며,
 *     leftmost_child_page_id는 헤더에 별도 보관.
 *
 * 주요 연산:
 *   - 검색: root에서 leaf까지 internal_find()로 자식을 선택하며 내려감. O(log
 * n)
 *   - 삽입: leaf에 공간 있으면 삽입, 가득 차면 split → 부모에 키 전파. O(log n)
 *   - 삭제: leaf에서 제거 후 underflow 시 borrow/merge로 균형 복구. O(log n)
 *
 * 페이지 레이아웃 (leaf):
 *   [leaf_page_header_t][leaf_entry_t * N][빈 공간]
 *   - 4096바이트 기준 최대 약 284개 엔트리
 *
 * 페이지 레이아웃 (internal):
 *   [internal_page_header_t][internal_entry_t * N][빈 공간]
 *   - 4096바이트 기준 최대 약 339개 엔트리
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
 * [leaf_page_header_t][entry_0][entry_1][entry_2]...[entry_N]
 * |_____ 20바이트 _____||____________ 나머지 전부 ______________|
 */

/*
 * leaf 페이지에 들어갈 수 있는 최대 엔트리 수
 * 4096 기준 사용 가능한 공간 = 4096 - 20(헤더) = 4076바이트
 * leaf_entry_t 하나 = 14바이트 (key 8바이트 + row_ref_t 6바이트)
 * 최대 키 수 = 4076 / 14 = 291개
 */
static uint32_t max_leaf_keys(pager_t* p) {
  return (p->page_size - sizeof(leaf_page_header_t)) / sizeof(leaf_entry_t);
}

/* internal 페이지에 들어갈 수 있는 최대 엔트리 수 */
static uint32_t max_internal_keys(pager_t* p) {
  return (p->page_size - sizeof(internal_page_header_t)) /
         sizeof(internal_entry_t);
}

/* leaf 노드의 최소 키 개수 (이보다 적으면 underflow → borrow 또는 merge 필요)
 */
static uint32_t min_leaf_keys(pager_t* p) { return max_leaf_keys(p) / 2; }

/* internal 노드의 최소 키 개수 */
static uint32_t min_internal_keys(pager_t* p) {
  return max_internal_keys(p) / 2;
}

/* ── 페이지 내부 접근 헬퍼 ──
 *
 * 페이지 바이트 배열에서 헤더 뒤의 엔트리 배열 시작 주소를 반환한다.
 * 디스크에 저장된 바이트를 구조체 배열처럼 접근할 수 있게 해준다.
 */

/* leaf 페이지의 엔트리 배열 시작 포인터 */
static leaf_entry_t* leaf_entries(uint8_t* page) {
  return (leaf_entry_t*)(page + sizeof(leaf_page_header_t));
}

/* internal 페이지의 엔트리 배열 시작 포인터 */
static internal_entry_t* internal_entries(uint8_t* page) {
  return (internal_entry_t*)(page + sizeof(internal_page_header_t));
}

/* DB 헤더에 저장된 현재 루트 페이지 ID */
static uint32_t root_id(pager_t* p) { return p->header.root_index_page_id; }

/* ── 이진 탐색 ──
 *
 * leaf와 internal 노드 모두 엔트리가 key 기준으로 오름차순 정렬되어 있다.
 * 이진 탐색으로 삽입 위치 또는 검색 위치를 O(log k) 시간에 찾는다.
 * (k = 해당 노드의 키 개수)
 */

/*
 * leaf_find - leaf 엔트리 배열에서 key 이상인 첫 번째 위치를 찾는다.
 *
 * 반환값이 count 미만이고 해당 위치의 key가 일치하면 → 검색 성공.
 * 반환값 위치에 새 엔트리를 삽입하면 정렬이 유지된다.
 *
 * 예시: entries = [10, 20, 30], key = 25
 *   → lo=0, hi=3 → mid=1(20<25, lo=2) → mid=2(30>=25, hi=2) → 반환 2
 *   → entries[2]=30 ≠ 25이므로 검색 실패, 삽입 시 위치 2에 넣으면 [10,20,25,30]
 */
static uint32_t leaf_find(leaf_entry_t* entries, uint32_t count, uint64_t key) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (entries[mid].key < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/*
 * internal_find - internal 엔트리 배열에서 key가 속할 자식 인덱스를 찾는다.
 *
 * internal 노드 구조:
 *   leftmost_child | key[0] | child[0] | key[1] | child[1] | ...
 *
 * key <= entries[mid].key이면 mid의 왼쪽 자식으로, 아니면 오른쪽으로 간다.
 * 반환값 0이면 leftmost_child, 반환값 i(>0)이면 entries[i-1].right_child.
 *
 * 예시: entries = [{key=100}, {key=200}], key=150
 *   → mid=0(100<=150, lo=1) → mid=1(200>150, hi=1) → 반환 1
 *   → entries[1-1=0].right_child로 이동
 */
static uint32_t internal_find(internal_entry_t* entries, uint32_t count,
                              uint64_t key) {
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (entries[mid].key <= key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/*
 * internal_child_for_key - internal 노드에서 key가 속할 자식 페이지 ID를
 * 반환한다.
 *
 * internal_find()의 결과를 사용하여:
 *   idx == 0 → leftmost_child (key가 모든 separator보다 작음)
 *   idx > 0  → entries[idx-1].right_child
 */
static uint32_t internal_child_for_key(uint8_t* page, uint64_t key) {
  internal_page_header_t iph;
  memcpy(&iph, page, sizeof(iph));
  internal_entry_t* entries = internal_entries(page);
  uint32_t idx = internal_find(entries, iph.key_count, key);
  if (idx == 0) {
    return iph.leftmost_child_page_id;
  }
  return entries[idx - 1].right_child_page_id;
}

/*
 * find_leaf - 루트에서 시작하여 key가 속할 leaf 페이지를 찾는다.
 *
 * 탐색 과정:
 *   1. 루트 페이지를 읽는다.
 *   2. 페이지 타입이 LEAF이면 해당 페이지가 결과.
 *   3. INTERNAL이면 internal_child_for_key()로 자식을 선택하고 반복.
 *
 * 트리 높이만큼 페이지를 읽으므로 O(log n).
 * 높이 3인 트리에서 100만 건도 3번의 페이지 접근으로 leaf에 도달한다.
 */
static uint32_t find_leaf(pager_t* pager, uint64_t key) {
  uint32_t pid = root_id(pager);
  while (1) {
    uint8_t* page = pager_get_page(pager, pid);
    uint32_t ptype;
    memcpy(&ptype, page, sizeof(uint32_t));
    if (ptype == PAGE_TYPE_LEAF) {
      pager_unpin(pager, pid);
      return pid;
    }
    uint32_t child = internal_child_for_key(page, key);
    pager_unpin(pager, pid);
    pid = child;
  }
}

/* ══════════════════════════════════════════════════════════════════════
 *  검색 (Search)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * bptree_search - B+ 트리에서 key를 검색하여 row_ref를 반환한다.
 *
 * 동작:
 *   1. find_leaf()로 key가 속할 leaf 페이지를 찾는다.
 *   2. leaf_find()로 해당 leaf 안에서 이진 탐색한다.
 *   3. 정확히 일치하는 key가 있으면 true + row_ref 반환.
 *
 * row_ref는 heap 테이블에서의 위치(page_id, slot_id)이다.
 * 이를 사용하여 heap_fetch()로 실제 행 데이터를 읽을 수 있다.
 *
 * 시간 복잡도: O(log n) — 트리 높이 + leaf 내 이진 탐색
 */
bool bptree_search(pager_t* pager, uint64_t key, row_ref_t* out_ref) {
  uint32_t leaf_pid = find_leaf(pager, key);
  uint8_t* page = pager_get_page(pager, leaf_pid);
  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  uint32_t idx = leaf_find(entries, lph.key_count, key);
  bool found = (idx < lph.key_count && entries[idx].key == key);
  if (found && out_ref) {
    *out_ref = entries[idx].row_ref;
  }
  pager_unpin(pager, leaf_pid);
  return found;
}

/* ══════════════════════════════════════════════════════════════════════
 *  삽입 (Insert)
 *
 *  삽입 흐름:
 *    bptree_insert()
 *      → leaf에 공간 있음 → leaf_insert_entry()로 정렬 삽입 → 끝
 *      → leaf가 가득 참  → split_leaf()
 *          → 새 페이지 할당, 엔트리 절반 분배
 *          → promote_key를 부모에 전파 → insert_into_parent()
 *              → 부모에 공간 있음 → 삽입 → 끝
 *              → 부모도 가득 참  → split_internal() → 재귀
 *                  → 루트까지 올라가면 → 새 루트 생성 (높이 +1)
 * ══════════════════════════════════════════════════════════════════════ */

/* split 시 부모에 키를 전파하는 함수 (전방 선언, 아래에서 구현) */
static void insert_into_parent(pager_t* pager, uint32_t left_pid, uint64_t key,
                               uint32_t right_pid);

/*
 * leaf_insert_entry - leaf 페이지에 (key, row_ref) 엔트리를 정렬 유지하며
 * 삽입한다.
 *
 * 동작:
 *   1. leaf_find()로 삽입 위치(idx)를 찾는다.
 *   2. idx 이후의 엔트리를 한 칸씩 오른쪽으로 밀어낸다.
 *   3. idx 위치에 새 엔트리를 기록한다.
 *   4. key_count를 증가시키고 헤더를 페이지에 다시 쓴다.
 *
 * 호출 전 key_count < max_leaf_keys임이 보장되어야 한다.
 */
static void leaf_insert_entry(uint8_t* page, leaf_page_header_t* lph,
                              uint64_t key, row_ref_t ref) {
  leaf_entry_t* entries = leaf_entries(page);
  uint32_t idx = leaf_find(entries, lph->key_count, key);
  /* idx 이후 엔트리를 한 칸씩 뒤로 밀기 */
  for (uint32_t i = lph->key_count; i > idx; i--) {
    entries[i] = entries[i - 1];
  }
  entries[idx].key = key;
  entries[idx].row_ref = ref;
  lph->key_count++;
  memcpy(page, lph, sizeof(*lph));
}

/*
 * split_leaf - 가득 찬 leaf 노드를 두 개로 분할한다.
 *
 * 동작:
 *   1. 기존 엔트리 + 새 엔트리를 임시 배열(tmp)에 정렬 순서로 합친다.
 *   2. 절반(split) 기준으로 왼쪽/오른쪽으로 나눈다.
 *   3. 새 페이지를 할당하여 오른쪽 절반을 저장한다.
 *   4. 양방향 연결 리스트 포인터를 갱신한다:
 *      - 새 노드의 prev = 원래 노드
 *      - 새 노드의 next = 원래 노드의 다음 이웃
 *      - 원래 노드의 next = 새 노드
 *      - 기존 다음 이웃의 prev = 새 노드
 *   5. promote_key(오른쪽 첫 키)를 부모에 전파한다.
 *
 * 분할 전: [1, 2, 3, 4, 5] + 새 키 6 삽입 (max=5)
 *   tmp = [1, 2, 3, 4, 5, 6], split = 3
 *   왼쪽 leaf: [1, 2, 3]
 *   오른쪽 leaf: [4, 5, 6]
 *   promote_key = 4 → 부모에 전파
 */
static void split_leaf(pager_t* pager, uint32_t leaf_pid, uint64_t key,
                       row_ref_t ref) {
  uint8_t* page = pager_get_page(pager, leaf_pid);
  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  uint32_t total = lph.key_count;
  uint32_t mk = max_leaf_keys(pager);

  /* 기존 엔트리 + 새 엔트리를 임시 배열에 정렬 순서로 합침 */
  leaf_entry_t* tmp = (leaf_entry_t*)malloc((total + 1) * sizeof(leaf_entry_t));
  uint32_t ins = leaf_find(entries, total, key);
  memcpy(tmp, entries, ins * sizeof(leaf_entry_t));
  tmp[ins].key = key;
  tmp[ins].row_ref = ref;
  memcpy(tmp + ins + 1, entries + ins, (total - ins) * sizeof(leaf_entry_t));
  total++;

  /* 절반 기준점 */
  uint32_t split = total / 2;

  /* 오른쪽 절반을 새 페이지에 저장 */
  uint32_t new_pid = pager_alloc_page(pager);
  uint8_t* new_page = pager_get_page(pager, new_pid);
  memset(new_page, 0, pager->page_size);

  leaf_page_header_t new_lph = {
      .page_type = PAGE_TYPE_LEAF,
      .parent_page_id = lph.parent_page_id,
      .key_count = total - split,
      .next_leaf_page_id =
          lph.next_leaf_page_id,    /* 기존 다음 이웃을 물려받음 */
      .prev_leaf_page_id = leaf_pid /* 이전 = 원래 노드 */
  };
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

  /* 원래 노드를 왼쪽 절반으로 갱신 */
  lph.key_count = split;
  lph.next_leaf_page_id = new_pid;
  memcpy(page, &lph, sizeof(lph));
  memcpy(leaf_entries(page), tmp, split * sizeof(leaf_entry_t));
  /* 남은 영역을 0으로 정리 */
  memset(leaf_entries(page) + split, 0, (mk - split) * sizeof(leaf_entry_t));
  pager_mark_dirty(pager, leaf_pid);

  /* 오른쪽 첫 키를 부모에 전파할 promote_key로 선정 */
  uint64_t promote_key = tmp[split].key;
  free(tmp);

  pager_unpin(pager, new_pid);
  pager_unpin(pager, leaf_pid);

  /* 부모에 separator 키 삽입 (재귀적으로 위로 전파될 수 있음) */
  insert_into_parent(pager, leaf_pid, promote_key, new_pid);
}

/*
 * split_internal - 가득 찬 internal 노드를 두 개로 분할한다.
 *
 * leaf split과 유사하지만 중요한 차이가 있다:
 *   - leaf: promote_key가 오른쪽 노드에도 남아있음 (데이터 보존)
 *   - internal: promote_key는 부모로 올라가고, 양쪽에서 제거됨 (라우팅 전용)
 *
 * 분할 전: [100, 200, 300, 400, 500] + 새 키 350 (max=5)
 *   tmp = [100, 200, 300, 350, 400, 500], split = 3
 *   promote_key = 350 (tmp[3].key)
 *   왼쪽:  [100, 200, 300]
 *   오른쪽: [400, 500] (350은 부모로 올라감)
 *   오른쪽의 leftmost_child = tmp[3].right_child (350의 오른쪽 자식)
 *
 * 자식 노드들의 parent_page_id도 새 부모로 갱신해야 한다.
 */
static void split_internal(pager_t* pager, uint32_t node_pid, uint64_t key,
                           uint32_t right_child) {
  uint8_t* page = pager_get_page(pager, node_pid);
  internal_page_header_t iph;
  memcpy(&iph, page, sizeof(iph));
  internal_entry_t* entries = internal_entries(page);

  uint32_t total = iph.key_count;
  uint32_t mk = max_internal_keys(pager);

  /* 기존 엔트리 + 새 엔트리를 임시 배열에 정렬 순서로 합침 */
  internal_entry_t* tmp =
      (internal_entry_t*)malloc((total + 1) * sizeof(internal_entry_t));
  uint32_t ins = internal_find(entries, total, key);
  memcpy(tmp, entries, ins * sizeof(internal_entry_t));
  tmp[ins].key = key;
  tmp[ins].right_child_page_id = right_child;
  memcpy(tmp + ins + 1, entries + ins,
         (total - ins) * sizeof(internal_entry_t));
  total++;

  uint32_t split = total / 2;
  /* 중간 키는 부모로 올라가고 양쪽 노드에서 제거된다 */
  uint64_t promote_key = tmp[split].key;

  /* 오른쪽 절반을 새 internal 페이지에 저장 */
  uint32_t new_pid = pager_alloc_page(pager);
  uint8_t* new_page = pager_get_page(pager, new_pid);
  memset(new_page, 0, pager->page_size);

  internal_page_header_t new_iph = {
      .page_type = PAGE_TYPE_INTERNAL,
      .parent_page_id = iph.parent_page_id,
      .key_count = total - split - 1, /* promote_key 하나가 빠지므로 -1 */
      .leftmost_child_page_id = tmp[split].right_child_page_id};
  memcpy(new_page, &new_iph, sizeof(new_iph));
  memcpy(internal_entries(new_page), tmp + split + 1,
         (total - split - 1) * sizeof(internal_entry_t));
  pager_mark_dirty(pager, new_pid);

  /*
   * 새 오른쪽 internal 노드로 옮겨간 자식들의 parent_page_id를 갱신한다.
   * leaf/internal 헤더 모두 offset 4에 parent_page_id가 있다.
   *   [page_type(4바이트)][parent_page_id(4바이트)]...
   */
  {
    /* 새 노드의 leftmost_child */
    uint8_t* cp = pager_get_page(pager, new_iph.leftmost_child_page_id);
    uint32_t pp = new_pid;
    memcpy(cp + 4, &pp, sizeof(uint32_t));
    pager_mark_dirty(pager, new_iph.leftmost_child_page_id);
    pager_unpin(pager, new_iph.leftmost_child_page_id);

    /* 새 노드의 각 엔트리의 right_child */
    internal_entry_t* ne = internal_entries(new_page);
    for (uint32_t i = 0; i < new_iph.key_count; i++) {
      cp = pager_get_page(pager, ne[i].right_child_page_id);
      memcpy(cp + 4, &pp, sizeof(uint32_t));
      pager_mark_dirty(pager, ne[i].right_child_page_id);
      pager_unpin(pager, ne[i].right_child_page_id);
    }
  }

  /* 원래 노드를 왼쪽 절반으로 갱신 */
  iph.key_count = split;
  memcpy(page, &iph, sizeof(iph));
  memcpy(internal_entries(page), tmp, split * sizeof(internal_entry_t));
  memset(internal_entries(page) + split, 0,
         (mk - split) * sizeof(internal_entry_t));
  pager_mark_dirty(pager, node_pid);

  free(tmp);
  pager_unpin(pager, new_pid);
  pager_unpin(pager, node_pid);

  /* promote_key를 부모에 전파 (재귀) */
  insert_into_parent(pager, node_pid, promote_key, new_pid);
}

/*
 * insert_into_parent - split 후 부모 노드에 separator 키를 삽입한다.
 *
 * 두 가지 경우를 처리한다:
 *
 * 1. 부모가 없음 (현재 노드가 루트) → 새 루트 생성
 *    - 새 internal 페이지를 할당한다.
 *    - leftmost_child = 왼쪽 노드, entries[0] = (key, 오른쪽 노드)
 *    - 양쪽 자식의 parent_page_id를 새 루트로 갱신한다.
 *    - 트리 높이가 1 증가한다.
 *
 * 2. 부모가 있음
 *    a. 부모에 공간 있음 → 정렬 유지하며 삽입
 *    b. 부모도 가득 참 → split_internal()로 부모를 분할 (재귀)
 */
static void insert_into_parent(pager_t* pager, uint32_t left_pid, uint64_t key,
                               uint32_t right_pid) {
  /* 왼쪽 자식의 parent_page_id를 읽어서 부모를 찾는다 */
  uint8_t* left_page = pager_get_page(pager, left_pid);
  uint32_t parent_pid;
  memcpy(&parent_pid, left_page + 4, sizeof(uint32_t));
  pager_unpin(pager, left_pid);

  /* 부모가 없고 현재가 루트이면 → 새 루트 생성 */
  if (parent_pid == 0 && left_pid == root_id(pager)) {
    uint32_t new_root = pager_alloc_page(pager);
    uint8_t* rp = pager_get_page(pager, new_root);
    memset(rp, 0, pager->page_size);

    internal_page_header_t iph = {.page_type = PAGE_TYPE_INTERNAL,
                                  .parent_page_id = 0, /* 루트는 부모 없음 */
                                  .key_count = 1,
                                  .leftmost_child_page_id = left_pid};
    memcpy(rp, &iph, sizeof(iph));
    internal_entry_t e = {.key = key, .right_child_page_id = right_pid};
    memcpy(internal_entries(rp), &e, sizeof(e));
    pager_mark_dirty(pager, new_root);
    pager_unpin(pager, new_root);

    /* 양쪽 자식의 parent를 새 루트로 갱신 */
    uint8_t* lp = pager_get_page(pager, left_pid);
    memcpy(lp + 4, &new_root, sizeof(uint32_t));
    pager_mark_dirty(pager, left_pid);
    pager_unpin(pager, left_pid);

    uint8_t* rrp = pager_get_page(pager, right_pid);
    memcpy(rrp + 4, &new_root, sizeof(uint32_t));
    pager_mark_dirty(pager, right_pid);
    pager_unpin(pager, right_pid);

    pager->header.root_index_page_id = new_root;
    return;
  }

  /* 기존 부모에 삽입 */
  uint8_t* pp = pager_get_page(pager, parent_pid);
  internal_page_header_t iph;
  memcpy(&iph, pp, sizeof(iph));

  if (iph.key_count < max_internal_keys(pager)) {
    /* 부모에 공간이 있으면 정렬 유지하며 삽입 */
    internal_entry_t* entries = internal_entries(pp);
    uint32_t idx = internal_find(entries, iph.key_count, key);
    for (uint32_t i = iph.key_count; i > idx; i--) {
      entries[i] = entries[i - 1];
    }
    entries[idx].key = key;
    entries[idx].right_child_page_id = right_pid;
    iph.key_count++;
    memcpy(pp, &iph, sizeof(iph));
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    /* 오른쪽 자식의 parent를 부모로 설정 */
    uint8_t* rcp = pager_get_page(pager, right_pid);
    memcpy(rcp + 4, &parent_pid, sizeof(uint32_t));
    pager_mark_dirty(pager, right_pid);
    pager_unpin(pager, right_pid);
  } else {
    /* 부모도 가득 참 → 부모를 분할 */
    pager_unpin(pager, parent_pid);

    /* 오른쪽 자식의 parent를 임시로 설정 (split_internal에서 재조정됨) */
    uint8_t* rcp = pager_get_page(pager, right_pid);
    memcpy(rcp + 4, &parent_pid, sizeof(uint32_t));
    pager_mark_dirty(pager, right_pid);
    pager_unpin(pager, right_pid);

    split_internal(pager, parent_pid, key, right_pid);
  }
}

/*
 * bptree_insert - B+ 트리에 (key, row_ref) 쌍을 삽입한다.
 *
 * 반환값: 0 = 성공, -1 = 중복 키
 *
 * 동작:
 *   1. find_leaf()로 key가 들어갈 leaf를 찾는다.
 *   2. 중복 체크: 이미 같은 key가 있으면 -1 반환.
 *   3. leaf에 공간 있으면 → leaf_insert_entry()로 삽입.
 *   4. leaf가 가득 차면 → split_leaf()로 분할 + 부모에 전파.
 */
int bptree_insert(pager_t* pager, uint64_t key, row_ref_t ref) {
  uint32_t leaf_pid = find_leaf(pager, key);
  uint8_t* page = pager_get_page(pager, leaf_pid);
  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  /* 중복 키 체크 */
  uint32_t idx = leaf_find(entries, lph.key_count, key);
  if (idx < lph.key_count && entries[idx].key == key) {
    pager_unpin(pager, leaf_pid);
    return -1;
  }

  if (lph.key_count < max_leaf_keys(pager)) {
    /* 공간이 있으면 바로 삽입 */
    leaf_insert_entry(page, &lph, key, ref);
    pager_mark_dirty(pager, leaf_pid);
    pager_unpin(pager, leaf_pid);
    return 0;
  }

  /* leaf가 가득 참 → 분할 */
  pager_unpin(pager, leaf_pid);
  split_leaf(pager, leaf_pid, key, ref);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  삭제 (Delete)
 *
 *  삭제 흐름:
 *    bptree_delete()
 *      → leaf에서 엔트리 제거 → delete_entry_from_leaf()
 *      → fix_leaf_after_delete()로 underflow 확인
 *          → 충분하면 끝
 *          → underflow이면:
 *              1순위: 오른쪽 형제에서 borrow (키 빌려옴)
 *              2순위: 왼쪽 형제에서 borrow
 *              3순위: 형제와 merge (합병)
 *                  → 빈 페이지를 free list에 반환
 *                  → 부모에서 separator 제거
 *                  → fix_internal_after_delete()
 *                      → 루트가 키 0개면 root shrink (높이 -1)
 * ══════════════════════════════════════════════════════════════════════ */

/* leaf에서 idx 위치의 엔트리를 제거한다 (왼쪽으로 밀기) */
static void delete_entry_from_leaf(uint8_t* page, leaf_page_header_t* lph,
                                   uint32_t idx) {
  leaf_entry_t* entries = leaf_entries(page);
  for (uint32_t i = idx; i < lph->key_count - 1; i++) {
    entries[i] = entries[i + 1];
  }
  lph->key_count--;
  memcpy(page, lph, sizeof(*lph));
}

/* internal에서 idx 위치의 엔트리를 제거한다 (왼쪽으로 밀기) */
static void delete_entry_from_internal(uint8_t* page,
                                       internal_page_header_t* iph,
                                       uint32_t idx) {
  internal_entry_t* entries = internal_entries(page);
  for (uint32_t i = idx; i < iph->key_count - 1; i++) {
    entries[i] = entries[i + 1];
  }
  iph->key_count--;
  memcpy(page, iph, sizeof(*iph));
}

/* 전방 선언: merge 후 부모의 underflow를 처리 */
static void fix_internal_after_delete(pager_t* pager, uint32_t node_pid);

/*
 * fix_leaf_after_delete - 삭제 후 leaf의 underflow를 복구한다.
 *
 * B+ 트리 불변량: leaf는 최소 min_leaf_keys개의 키를 가져야 한다.
 * (루트는 예외: 0개도 허용)
 *
 * 복구 전략 (우선순위 순서):
 *
 * 1. 오른쪽 형제에서 borrow:
 *    - 오른쪽 형제의 첫 번째 키를 가져온다.
 *    - 부모의 separator를 오른쪽 형제의 새 첫 번째 키로 갱신한다.
 *    - 오른쪽 형제가 min_leaf_keys 초과일 때만 가능.
 *
 * 2. 왼쪽 형제에서 borrow:
 *    - 왼쪽 형제의 마지막 키를 가져온다.
 *    - 부모의 separator를 현재 노드의 새 첫 번째 키로 갱신한다.
 *
 * 3. merge (합병):
 *    - borrow 불가능하면 형제와 합친다.
 *    - 현재 노드의 모든 키를 왼쪽 형제에 복사한다.
 *    - 연결 리스트 포인터를 갱신한다.
 *    - 현재 페이지를 free page list에 반환한다.
 *    - 부모에서 separator를 제거한다 → fix_internal_after_delete() 호출.
 */
static void fix_leaf_after_delete(pager_t* pager, uint32_t leaf_pid) {
  uint8_t* page = pager_get_page(pager, leaf_pid);
  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  pager_unpin(pager, leaf_pid);

  /* 루트는 underflow 허용 */
  if (leaf_pid == root_id(pager)) {
    return;
  }
  /* 키가 충분하면 복구 불필요 */
  if (lph.key_count >= min_leaf_keys(pager)) {
    return;
  }

  /*
   * 부모에서 현재 노드의 위치(child_idx)를 찾는다.
   *
   * internal 노드의 자식 구조:
   *   leftmost_child(child_idx=0) | key[0] | child[0](child_idx=1) | key[1] |
   * child[1](child_idx=2) | ...
   *
   * child_idx가 0이면 leftmost_child, i+1이면 entries[i].right_child.
   */
  uint32_t parent_pid = lph.parent_page_id;
  uint8_t* pp = pager_get_page(pager, parent_pid);
  internal_page_header_t iph;
  memcpy(&iph, pp, sizeof(iph));
  internal_entry_t* pentries = internal_entries(pp);

  int child_idx = -1;
  if (iph.leftmost_child_page_id == leaf_pid) {
    child_idx = 0;
  } else {
    for (uint32_t i = 0; i < iph.key_count; i++) {
      if (pentries[i].right_child_page_id == leaf_pid) {
        child_idx = (int)i + 1;
        break;
      }
    }
  }
  pager_unpin(pager, parent_pid);
  if (child_idx < 0) {
    return;
  }

  /* ── 1순위: 오른쪽 형제에서 borrow 시도 ── */
  if (child_idx <= (int)iph.key_count - 1 ||
      (child_idx == 0 && iph.key_count > 0)) {
    uint32_t sep_idx = (child_idx == 0) ? 0 : (uint32_t)child_idx;
    if (sep_idx < iph.key_count) {
      uint32_t rsib_pid = pentries[sep_idx].right_child_page_id;
      if (child_idx > 0) {
        if ((uint32_t)child_idx < iph.key_count) {
          rsib_pid = pentries[child_idx].right_child_page_id;
          sep_idx = (uint32_t)child_idx;
        } else {
          goto try_left;
        }
      }

      uint8_t* rpage = pager_get_page(pager, rsib_pid);
      leaf_page_header_t rlph;
      memcpy(&rlph, rpage, sizeof(rlph));

      if (rlph.key_count > min_leaf_keys(pager)) {
        /* 오른쪽 형제의 첫 번째 엔트리를 빌려온다 */
        leaf_entry_t* rentries = leaf_entries(rpage);
        leaf_entry_t borrowed = rentries[0];
        delete_entry_from_leaf(rpage, &rlph, 0);
        pager_mark_dirty(pager, rsib_pid);
        pager_unpin(pager, rsib_pid);

        /* 빌려온 엔트리를 현재 노드에 삽입 */
        page = pager_get_page(pager, leaf_pid);
        memcpy(&lph, page, sizeof(lph));
        leaf_insert_entry(page, &lph, borrowed.key, borrowed.row_ref);
        pager_mark_dirty(pager, leaf_pid);
        pager_unpin(pager, leaf_pid);

        /* 부모의 separator를 오른쪽 형제의 새 첫 번째 키로 갱신 */
        pp = pager_get_page(pager, parent_pid);
        memcpy(&iph, pp, sizeof(iph));
        pentries = internal_entries(pp);
        rpage = pager_get_page(pager, rsib_pid);
        memcpy(&rlph, rpage, sizeof(rlph));
        pentries[sep_idx].key = leaf_entries(rpage)[0].key;
        pager_unpin(pager, rsib_pid);
        memcpy(pp, &iph, sizeof(iph));
        pager_mark_dirty(pager, parent_pid);
        pager_unpin(pager, parent_pid);
        return;
      }
      pager_unpin(pager, rsib_pid);
    }
  }

try_left:
  /* ── 2순위: 왼쪽 형제에서 borrow 시도 ── */
  if (child_idx > 0) {
    uint32_t lsib_pid;
    uint32_t sep_idx = (uint32_t)(child_idx - 1);
    if (child_idx == 1) {
      lsib_pid = iph.leftmost_child_page_id;
    } else {
      lsib_pid = pentries[child_idx - 2].right_child_page_id;
    }

    uint8_t* lpage = pager_get_page(pager, lsib_pid);
    leaf_page_header_t llph;
    memcpy(&llph, lpage, sizeof(llph));

    if (llph.key_count > min_leaf_keys(pager)) {
      /* 왼쪽 형제의 마지막 엔트리를 빌려온다 */
      leaf_entry_t* lentries = leaf_entries(lpage);
      leaf_entry_t borrowed = lentries[llph.key_count - 1];
      llph.key_count--;
      memcpy(lpage, &llph, sizeof(llph));
      pager_mark_dirty(pager, lsib_pid);
      pager_unpin(pager, lsib_pid);

      /* 빌려온 엔트리를 현재 노드에 삽입 */
      page = pager_get_page(pager, leaf_pid);
      memcpy(&lph, page, sizeof(lph));
      leaf_insert_entry(page, &lph, borrowed.key, borrowed.row_ref);
      pager_mark_dirty(pager, leaf_pid);
      pager_unpin(pager, leaf_pid);

      /* 부모의 separator를 현재 노드의 새 첫 번째 키로 갱신 */
      pp = pager_get_page(pager, parent_pid);
      memcpy(&iph, pp, sizeof(iph));
      pentries = internal_entries(pp);
      page = pager_get_page(pager, leaf_pid);
      memcpy(&lph, page, sizeof(lph));
      pentries[sep_idx].key = leaf_entries(page)[0].key;
      pager_unpin(pager, leaf_pid);
      memcpy(pp, &iph, sizeof(iph));
      pager_mark_dirty(pager, parent_pid);
      pager_unpin(pager, parent_pid);
      return;
    }
    pager_unpin(pager, lsib_pid);
  }

  /* ── 3순위: merge (합병) ── */

  if (child_idx > 0) {
    /*
     * 왼쪽 형제와 merge (왼쪽 형제 쪽으로 합침):
     *   왼쪽 형제: [10, 20]  현재: [40]  → 합병 후: [10, 20, 40]
     *   부모에서 separator(30) 제거
     *   현재 페이지는 free list에 반환
     */
    uint32_t lsib_pid;
    uint32_t sep_idx = (uint32_t)(child_idx - 1);
    if (child_idx == 1) {
      lsib_pid = iph.leftmost_child_page_id;
    } else {
      lsib_pid = pentries[child_idx - 2].right_child_page_id;
    }

    /* 현재 노드의 모든 엔트리를 왼쪽 형제 뒤에 복사 */
    uint8_t* lpage = pager_get_page(pager, lsib_pid);
    leaf_page_header_t llph;
    memcpy(&llph, lpage, sizeof(llph));

    page = pager_get_page(pager, leaf_pid);
    memcpy(&lph, page, sizeof(lph));
    leaf_entry_t* cur_entries = leaf_entries(page);
    leaf_entry_t* left_entries = leaf_entries(lpage);

    for (uint32_t i = 0; i < lph.key_count; i++) {
      left_entries[llph.key_count + i] = cur_entries[i];
    }
    llph.key_count += lph.key_count;
    /* 왼쪽 형제의 next를 현재 노드의 next로 갱신 (연결 리스트 유지) */
    llph.next_leaf_page_id = lph.next_leaf_page_id;
    memcpy(lpage, &llph, sizeof(llph));
    pager_mark_dirty(pager, lsib_pid);
    pager_unpin(pager, lsib_pid);
    pager_unpin(pager, leaf_pid);

    /* 현재 노드의 다음 이웃의 prev 포인터를 왼쪽 형제로 갱신 */
    if (lph.next_leaf_page_id != 0) {
      uint8_t* np = pager_get_page(pager, lph.next_leaf_page_id);
      leaf_page_header_t nlph;
      memcpy(&nlph, np, sizeof(nlph));
      nlph.prev_leaf_page_id = lsib_pid;
      memcpy(np, &nlph, sizeof(nlph));
      pager_mark_dirty(pager, lph.next_leaf_page_id);
      pager_unpin(pager, lph.next_leaf_page_id);
    }

    /* 현재 페이지를 free page list에 반환 */
    pager_free_page(pager, leaf_pid);

    /* 부모에서 separator 제거 */
    pp = pager_get_page(pager, parent_pid);
    memcpy(&iph, pp, sizeof(iph));
    pentries = internal_entries(pp);
    delete_entry_from_internal(pp, &iph, sep_idx);
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    /* 부모의 underflow 확인 (재귀) */
    fix_internal_after_delete(pager, parent_pid);
  } else {
    /*
     * 오른쪽 형제를 현재 노드로 merge (child_idx==0일 때):
     *   현재: [5]  오른쪽 형제: [20, 30]  → 합병 후: [5, 20, 30]
     *   부모에서 separator(10) 제거
     *   오른쪽 형제 페이지는 free list에 반환
     */
    uint32_t rsib_pid = pentries[0].right_child_page_id;

    page = pager_get_page(pager, leaf_pid);
    memcpy(&lph, page, sizeof(lph));
    leaf_entry_t* cur_entries = leaf_entries(page);

    uint8_t* rpage = pager_get_page(pager, rsib_pid);
    leaf_page_header_t rlph;
    memcpy(&rlph, rpage, sizeof(rlph));
    leaf_entry_t* rentries = leaf_entries(rpage);

    /* 오른쪽 형제의 모든 엔트리를 현재 노드 뒤에 복사 */
    for (uint32_t i = 0; i < rlph.key_count; i++) {
      cur_entries[lph.key_count + i] = rentries[i];
    }
    lph.key_count += rlph.key_count;
    lph.next_leaf_page_id = rlph.next_leaf_page_id;
    memcpy(page, &lph, sizeof(lph));
    pager_mark_dirty(pager, leaf_pid);
    pager_unpin(pager, leaf_pid);
    pager_unpin(pager, rsib_pid);

    /* 오른쪽 형제의 다음 이웃의 prev를 현재 노드로 갱신 */
    if (rlph.next_leaf_page_id != 0) {
      uint8_t* np = pager_get_page(pager, rlph.next_leaf_page_id);
      leaf_page_header_t nlph;
      memcpy(&nlph, np, sizeof(nlph));
      nlph.prev_leaf_page_id = leaf_pid;
      memcpy(np, &nlph, sizeof(nlph));
      pager_mark_dirty(pager, rlph.next_leaf_page_id);
      pager_unpin(pager, rlph.next_leaf_page_id);
    }

    /* 오른쪽 형제 페이지를 free list에 반환 */
    pager_free_page(pager, rsib_pid);

    /* 부모에서 첫 번째 separator 제거 */
    pp = pager_get_page(pager, parent_pid);
    memcpy(&iph, pp, sizeof(iph));
    pentries = internal_entries(pp);
    delete_entry_from_internal(pp, &iph, 0);
    pager_mark_dirty(pager, parent_pid);
    pager_unpin(pager, parent_pid);

    /* 부모의 underflow 확인 (재귀) */
    fix_internal_after_delete(pager, parent_pid);
  }
}

/*
 * fix_internal_after_delete - merge 후 internal 노드의 underflow를 처리한다.
 *
 * 루트인 경우:
 *   key_count가 0이면 루트의 유일한 자식(leftmost_child)을 새 루트로 승격한다.
 *   빈 루트 페이지는 free list에 반환한다.
 *   이로써 트리 높이가 1 감소한다 (root shrink).
 *
 * 루트가 아닌 경우:
 *   최소 구현에서는 internal 노드의 borrow/merge를 생략한다.
 *   internal 노드가 underfull 상태로 남을 수 있지만,
 *   검색/삽입/삭제의 정확성에는 영향이 없다.
 */
static void fix_internal_after_delete(pager_t* pager, uint32_t node_pid) {
  if (node_pid == root_id(pager)) {
    /* 루트 축소: 키가 0개이면 유일한 자식을 새 루트로 승격 */
    uint8_t* page = pager_get_page(pager, node_pid);
    internal_page_header_t iph;
    memcpy(&iph, page, sizeof(iph));
    if (iph.key_count == 0) {
      uint32_t child = iph.leftmost_child_page_id;
      pager_unpin(pager, node_pid);
      pager_free_page(pager, node_pid);

      pager->header.root_index_page_id = child;
      /* 새 루트의 parent를 0으로 설정 */
      uint8_t* cp = pager_get_page(pager, child);
      uint32_t zero = 0;
      memcpy(cp + 4, &zero, sizeof(uint32_t));
      pager_mark_dirty(pager, child);
      pager_unpin(pager, child);
    } else {
      pager_unpin(pager, node_pid);
    }
    return;
  }

  /* 루트가 아닌 internal 노드의 underflow 확인 */
  uint8_t* page = pager_get_page(pager, node_pid);
  internal_page_header_t iph;
  memcpy(&iph, page, sizeof(iph));
  pager_unpin(pager, node_pid);

  if (iph.key_count >= min_internal_keys(pager)) {
    return;
  }

  /*
   * 최소 구현: internal 노드의 borrow/merge는 생략한다.
   * 대량 삭제 시 internal 노드가 underfull 상태로 남을 수 있으나
   * 트리의 정합성(검색/삽입/삭제)에는 영향이 없다.
   */
}

/*
 * bptree_delete - B+ 트리에서 key를 삭제한다.
 *
 * 반환값: 0 = 성공, -1 = 키 없음
 *
 * 동작:
 *   1. find_leaf()로 key가 있는 leaf를 찾는다.
 *   2. leaf_find()로 정확한 위치를 찾고, 없으면 -1 반환.
 *   3. delete_entry_from_leaf()로 엔트리를 제거한다.
 *   4. fix_leaf_after_delete()로 underflow를 확인하고 필요시 복구한다.
 *
 * 주의: 이 함수는 B+ 트리 인덱스에서만 키를 제거한다.
 *       heap 테이블의 행 데이터는 별도로 heap_delete()를 호출해야 한다.
 */
int bptree_delete(pager_t* pager, uint64_t key) {
  uint32_t leaf_pid = find_leaf(pager, key);
  uint8_t* page = pager_get_page(pager, leaf_pid);
  leaf_page_header_t lph;
  memcpy(&lph, page, sizeof(lph));
  leaf_entry_t* entries = leaf_entries(page);

  /* 삭제할 키의 위치를 이진 탐색으로 찾는다 */
  uint32_t idx = leaf_find(entries, lph.key_count, key);
  if (idx >= lph.key_count || entries[idx].key != key) {
    pager_unpin(pager, leaf_pid);
    return -1;
  }

  /* 엔트리 제거 */
  delete_entry_from_leaf(page, &lph, idx);
  pager_mark_dirty(pager, leaf_pid);
  pager_unpin(pager, leaf_pid);

  /* underflow 확인 및 복구 */
  fix_leaf_after_delete(pager, leaf_pid);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  디버그 (Debug)
 * ══════════════════════════════════════════════════════════════════════ */

/* 트리 노드를 재귀적으로 출력한다 (들여쓰기로 깊이 표현) */
static void print_node(pager_t* pager, uint32_t pid, int depth) {
  uint8_t* page = pager_get_page(pager, pid);
  uint32_t ptype;
  memcpy(&ptype, page, sizeof(uint32_t));

  /* 깊이만큼 들여쓰기 */
  for (int i = 0; i < depth; i++) {
    printf("  ");
  }

  if (ptype == PAGE_TYPE_LEAF) {
    leaf_page_header_t lph;
    memcpy(&lph, page, sizeof(lph));
    leaf_entry_t* entries = leaf_entries(page);
    printf("[LEAF page=%u] keys=%u:", pid, lph.key_count);
    /* 최대 5개까지만 출력 */
    for (uint32_t i = 0; i < lph.key_count && i < 5; i++) {
      printf(" %lu", (unsigned long)entries[i].key);
    }
    if (lph.key_count > 5) {
      printf(" ...");
    }
    printf("\n");
  } else if (ptype == PAGE_TYPE_INTERNAL) {
    internal_page_header_t iph;
    memcpy(&iph, page, sizeof(iph));
    internal_entry_t* entries = internal_entries(page);
    printf("[INTERNAL page=%u] keys=%u:", pid, iph.key_count);
    for (uint32_t i = 0; i < iph.key_count && i < 5; i++) {
      printf(" %lu", (unsigned long)entries[i].key);
    }
    if (iph.key_count > 5) {
      printf(" ...");
    }
    printf("\n");
    pager_unpin(pager, pid);

    /* 자식 노드를 재귀적으로 출력 */
    page = pager_get_page(pager, pid);
    memcpy(&iph, page, sizeof(iph));
    entries = internal_entries(page);

    /* leftmost_child 먼저, 그 다음 각 엔트리의 right_child 순서 */
    print_node(pager, iph.leftmost_child_page_id, depth + 1);
    for (uint32_t i = 0; i < iph.key_count; i++) {
      print_node(pager, entries[i].right_child_page_id, depth + 1);
    }
    pager_unpin(pager, pid);
    return;
  }
  pager_unpin(pager, pid);
}

/* B+ 트리 전체 구조를 출력한다 (.btree 메타 명령어에서 호출) */
void bptree_print(pager_t* pager) {
  printf("B+ Tree (root: page %u)\n", root_id(pager));
  print_node(pager, root_id(pager), 1);
}

/*
 * bptree_height - B+ 트리의 높이를 반환한다.
 *
 * 루트에서 시작하여 leftmost_child를 따라 leaf까지 내려가며 카운트한다.
 * leaf만 있으면 높이 1, internal 1개 + leaf이면 높이 2.
 */
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
