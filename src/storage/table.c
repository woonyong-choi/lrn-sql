/*
 * table.c — 슬롯 기반 힙 테이블 구현
 *
 * 역할:
 *   행(row) 데이터를 힙 페이지에 저장, 조회, 삭제, 스캔하는 테이블 계층이다.
 *   B+ tree가 목차(인덱스)라면, 힙 페이지는 실제 데이터가 적힌 본문이다.
 *
 * 힙 페이지 레이아웃 (4096바이트 기준, row_size=44 예시):
 *
 *   ┌─ 앞쪽 (offset 0) ──────────────────────────────────────┐
 *   │ heap_page_header_t (16바이트)                           │
 *   │ slot_0 (8바이트): offset=4008, ALIVE, next_free=NONE    │
 *   │ slot_1 (8바이트): offset=3964, ALIVE, next_free=NONE    │
 *   │ slot_2 (8바이트): offset=3920, FREE,  next_free=NONE    │ ← 삭제됨
 *   │ ... (슬롯이 아래로 자람)                                 │
 *   │                                                          │
 *   │              [ 빈 공간 ]                                  │
 *   │                                                          │
 *   │ row_2 (44바이트) at offset 3920  ← 삭제됐지만 데이터 남음 │
 *   │ row_1 (44바이트) at offset 3964                          │
 *   │ row_0 (44바이트) at offset 4008                          │
 *   └─ 뒤쪽 (offset 4095) ───────────────────────────────────┘
 *
 *   사용 가능 공간 = (page_size - free_space_offset) - slots_end
 *   4096바이트 페이지에 row_size=44인 행을 최대 ~88개 저장 가능
 *   (16 + 88×8 = 720바이트 슬롯 + 88×44 = 3872바이트 행 = 4592 → 초과, 실제는 ~80개)
 *
 * 삭제 처리 (톰스톤 방식):
 *   행 삭제 시 slot.status를 FREE로 변경하고 free_slot_head 체인에 연결한다.
 *   실제 행 데이터는 지우지 않는다.
 *   다음 INSERT 시 빈 슬롯을 먼저 재활용하여 같은 위치에 새 데이터를 덮어쓴다.
 *
 *   예: slot_2를 삭제 → slot_2.status=FREE, slot_2.next_free=NONE
 *       free_slot_head = 2
 *       다음 INSERT → slot_2를 재활용, free_slot_head = NONE
 *
 * 힙 체인:
 *   힙 페이지들은 next_heap_page_id로 연결 리스트를 형성한다.
 *   header.first_heap_page_id(=1) → page 1 → page 5 → page 8 → 0 (끝)
 */

#include "storage/table.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * slots_end - 슬롯 디렉터리의 끝 오프셋을 계산한다.
 *
 * 예시: slot_count=3
 *   sizeof(heap_page_header_t) = 16바이트
 *   sizeof(slot_t) = 8바이트
 *   slots_end = 16 + 3 × 8 = 40바이트
 */
static uint16_t slots_end(uint16_t slot_count)
{
    return (uint16_t)(sizeof(heap_page_header_t) + slot_count * sizeof(slot_t));
}

/*
 * available_space - 페이지의 사용 가능한 공간을 계산한다.
 *
 *   front: 슬롯 디렉터리가 차지하는 영역의 끝 (앞에서 뒤로 자람)
 *   back:  행 데이터가 차지하는 영역의 시작 (뒤에서 앞으로 자람)
 *
 * 예시: page_size=4096, slot_count=3, free_space_offset=132 (row 3개 × 44바이트)
 *   front = 16 + 3×8 = 40
 *   back  = 4096 - 132 = 3964
 *   사용 가능 = 3964 - 40 = 3924바이트
 */
static uint16_t available_space(pager_t *pager, heap_page_header_t *hph)
{
    uint16_t front = slots_end(hph->slot_count);
    uint16_t back  = (uint16_t)(pager->page_size - hph->free_space_offset);
    if (back <= front)
    {
        return 0;
    }
    return back - front;
}

/*
 * find_heap_page - 행을 삽입할 수 있는 힙 페이지를 찾는다.
 *
 * 탐색 순서 (힙 체인을 따라가며):
 *   1. free_slot_head != SLOT_NONE인 페이지 → 삭제된 슬롯 재활용 가능
 *   2. 새 슬롯(8바이트) + 행 데이터(row_size바이트)를 위한 공간이 있는 페이지
 *   3. 없으면 0을 반환 → 호출자가 새 페이지를 할당해야 함
 *
 * 예시: row_size=44, 필요 공간 = sizeof(slot_t) + 44 = 8 + 44 = 52바이트
 */
static uint32_t find_heap_page(pager_t *pager, uint16_t row_size)
{
    /*
     * 순차 INSERT 최적화:
     * 대부분의 append workload에서는 마지막 힙 페이지만 보면 충분하다.
     * 마지막 페이지가 가득 찼을 때만 free slot 탐색을 위해 전체 체인을 본다.
     */
    if (pager->last_heap_page_id != 0) {
        uint8_t *tail_page = pager_get_page(pager, pager->last_heap_page_id);
        heap_page_header_t tail_hph;
        memcpy(&tail_hph, tail_page, sizeof(tail_hph));
        pager_unpin(pager, pager->last_heap_page_id);

        if (tail_hph.free_slot_head != SLOT_NONE) {
            return pager->last_heap_page_id;
        }

        uint16_t need = (uint16_t)(sizeof(slot_t) + row_size);
        if (available_space(pager, &tail_hph) >= need) {
            return pager->last_heap_page_id;
        }
    }

    uint32_t pid = pager->header.first_heap_page_id;
    while (pid != 0) {
        uint8_t *page = pager_get_page(pager, pid);
        heap_page_header_t hph;
        memcpy(&hph, page, sizeof(hph));
        pager_unpin(pager, pid);

        /* 재활용 가능한 빈 슬롯이 있는지 확인 */
        if (hph.free_slot_head != SLOT_NONE)
        {
            return pid;
        }

        pid = hph.next_heap_page_id;
    }
    return 0; /* 적합한 페이지 없음 */
}

/* ══════════════════════════════════════════════════════════════════════
 *  INSERT
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * heap_insert - 힙에 행을 삽입하고, 삽입된 위치(page_id, slot_id)를 반환한다.
 *
 * 전체 흐름:
 *   1. find_heap_page()로 삽입 가능한 페이지 탐색
 *   2. 없으면 pager_alloc_page()로 새 힙 페이지를 할당하고 체인 끝에 연결
 *   3. 빈 슬롯이 있으면 재활용, 없으면 새 슬롯 생성
 *   4. 행 데이터를 해당 위치에 복사
 *
 * 예시: row_data = [id=3의 직렬화된 44바이트], page 1에 삽입
 *
 *   빈 슬롯 재활용 시:
 *     free_slot_head = 2 → slot_2를 꺼냄
 *     slot_2.offset 위치에 row_data를 덮어씀
 *     free_slot_head = slot_2.next_free
 *     반환: {page_id=1, slot_id=2}
 *
 *   새 슬롯 생성 시:
 *     slot_id = slot_count (= 3이라면 slot_3 생성)
 *     row_offset = page_size - free_space_offset - row_size
 *                = 4096 - 132 - 44 = 3920
 *     slot_3 = {offset=3920, ALIVE}
 *     free_space_offset += 44 → 176
 *     반환: {page_id=1, slot_id=3}
 */
row_ref_t heap_insert(pager_t *pager, const uint8_t *row_data, uint16_t row_size)
{
    row_ref_t ref = {0, 0};
    uint32_t pid = find_heap_page(pager, row_size);

    if (pid == 0) {
        /* 새 힙 페이지 할당 */
        pid = pager_alloc_page(pager);
        uint8_t *page = pager_get_page(pager, pid);

        heap_page_header_t hph = {
            .page_type = PAGE_TYPE_HEAP,
            .next_heap_page_id = 0,
            .slot_count = 0,
            .free_slot_head = SLOT_NONE,
            .free_space_offset = 0,
            .reserved = 0
        };
        memcpy(page, &hph, sizeof(hph));
        pager_mark_dirty(pager, pid);
        pager_unpin(pager, pid);

        /*
         * pager가 마지막 힙 페이지를 메모리에 캐시하므로
         * 매번 체인 전체를 순회하지 않고 tail에 바로 연결한다.
         */
        uint32_t prev_pid = pager->last_heap_page_id;
        if (prev_pid != 0) {
            uint8_t *pp = pager_get_page(pager, prev_pid);
            heap_page_header_t ph;
            memcpy(&ph, pp, sizeof(ph));
            ph.next_heap_page_id = pid;
            memcpy(pp, &ph, sizeof(ph));
            pager_mark_dirty(pager, prev_pid);
            pager_unpin(pager, prev_pid);
        }
        pager->last_heap_page_id = pid;
    }

    /* 선택된 페이지에 행 삽입 */
    uint8_t *page = pager_get_page(pager, pid);
    heap_page_header_t hph;
    memcpy(&hph, page, sizeof(hph));

    uint16_t slot_id;
    slot_t   slot;

    if (hph.free_slot_head != SLOT_NONE) {
        /*
         * 빈 슬롯 재활용
         *
         * free_slot_head에서 슬롯을 꺼내고, 해당 위치에 행 데이터를 덮어쓴다.
         * slot.offset은 이전 삭제된 행의 위치이므로 그대로 사용한다.
         */
        slot_id = hph.free_slot_head;
        size_t slot_off = sizeof(heap_page_header_t) + slot_id * sizeof(slot_t);
        memcpy(&slot, page + slot_off, sizeof(slot));
        hph.free_slot_head = slot.next_free;

        memcpy(page + slot.offset, row_data, row_size);
        slot.status = SLOT_ALIVE;
        slot.next_free = SLOT_NONE;
        memcpy(page + slot_off, &slot, sizeof(slot));
    } else {
        /*
         * 새 슬롯 생성
         *
         * 슬롯 디렉터리 끝에 새 슬롯을 추가하고,
         * 행 데이터는 페이지 뒤쪽에서 앞으로 자라도록 기록한다.
         *
         * row_offset = page_size - free_space_offset - row_size
         * 예: 4096 - 132 - 44 = 3920
         */
        slot_id = hph.slot_count;
        uint16_t row_offset = (uint16_t)(pager->page_size - hph.free_space_offset - row_size);

        slot.offset = row_offset;
        slot.status = SLOT_ALIVE;
        slot.next_free = SLOT_NONE;
        slot.reserved = 0;

        size_t slot_off = sizeof(heap_page_header_t) + slot_id * sizeof(slot_t);
        memcpy(page + slot_off, &slot, sizeof(slot));
        memcpy(page + row_offset, row_data, row_size);

        hph.slot_count++;
        hph.free_space_offset += row_size;
    }

    memcpy(page, &hph, sizeof(hph));
    pager_mark_dirty(pager, pid);
    pager_unpin(pager, pid);

    ref.page_id = pid;
    ref.slot_id = slot_id;
    return ref;
}

/* ══════════════════════════════════════════════════════════════════════
 *  FETCH (단건 조회)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * heap_fetch - row_ref_t로 지정된 행을 조회한다.
 *
 * 동작:
 *   1. ref.page_id 페이지를 로드한다.
 *   2. ref.slot_id 위치의 슬롯을 읽는다.
 *   3. slot.status가 ALIVE가 아니면 NULL 반환 (삭제된 행).
 *   4. ALIVE이면 page + slot.offset 위치의 포인터를 반환한다.
 *
 * 주의: 반환된 포인터는 캐시 페이지 내부를 가리킨다.
 *       사용 후 반드시 pager_unpin(pager, ref.page_id)을 호출해야 한다.
 *       unpin 전에 페이지가 교체되면 포인터가 무효화된다.
 *
 * 예시: ref = {page_id=1, slot_id=2}
 *   slot_off = 16 + 2×8 = 32
 *   slot = page[32..39]  → {offset=3920, ALIVE}
 *   반환: page + 3920 (44바이트 행 데이터의 시작 주소)
 */
const uint8_t *heap_fetch(pager_t *pager, row_ref_t ref, uint16_t row_size)
{
    (void)row_size;
    uint8_t *page = pager_get_page(pager, ref.page_id);
    if (page == NULL)
    {
        return NULL;
    }

    slot_t slot;
    size_t slot_off = sizeof(heap_page_header_t) + ref.slot_id * sizeof(slot_t);
    memcpy(&slot, page + slot_off, sizeof(slot));

    if (slot.status != SLOT_ALIVE) {
        pager_unpin(pager, ref.page_id);
        return NULL;
    }

    /* 주의: 호출자가 사용 후 pager_unpin(pager, ref.page_id)을 호출해야 한다 */
    return page + slot.offset;
}

/* ══════════════════════════════════════════════════════════════════════
 *  DELETE
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * heap_delete - row_ref_t로 지정된 행을 삭제한다.
 *
 * 톰스톤 삭제 방식:
 *   실제 데이터는 지우지 않고 slot.status를 FREE로 변경한 뒤,
 *   free_slot_head 체인에 연결한다.
 *
 * 예시: ref = {page_id=1, slot_id=2}, 기존 free_slot_head = NONE
 *   slot_2.status = FREE
 *   slot_2.next_free = NONE (기존 free_slot_head)
 *   free_slot_head = 2
 *
 *   → 다음 INSERT 시 slot_2를 재활용
 *
 * 반환값: 0 = 성공, -1 = 실패 (이미 삭제됨 또는 페이지 없음)
 */
int heap_delete(pager_t *pager, row_ref_t ref)
{
    uint8_t *page = pager_get_page(pager, ref.page_id);
    if (page == NULL)
    {
        return -1;
    }

    heap_page_header_t hph;
    memcpy(&hph, page, sizeof(hph));

    slot_t slot;
    size_t slot_off = sizeof(heap_page_header_t) + ref.slot_id * sizeof(slot_t);
    memcpy(&slot, page + slot_off, sizeof(slot));

    if (slot.status != SLOT_ALIVE) {
        pager_unpin(pager, ref.page_id);
        return -1;
    }

    /* 톰스톤 처리: 슬롯을 FREE로 표시하고 빈 슬롯 체인에 연결 */
    slot.status = SLOT_FREE;
    slot.next_free = hph.free_slot_head;
    hph.free_slot_head = ref.slot_id;

    memcpy(page + slot_off, &slot, sizeof(slot));
    memcpy(page, &hph, sizeof(hph));
    pager_mark_dirty(pager, ref.page_id);
    pager_unpin(pager, ref.page_id);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SCAN (전체 스캔)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * heap_scan - 모든 힙 페이지를 순회하며, 살아 있는 행마다 콜백을 호출한다.
 *
 * 동작:
 *   1. first_heap_page_id부터 힙 체인을 따라간다.
 *   2. 각 페이지의 슬롯을 0번부터 순회한다.
 *   3. slot.status == ALIVE인 행에 대해 cb(행데이터, row_ref, ctx) 호출
 *   4. cb가 false를 반환하면 스캔 즉시 중단
 *
 * SELECT * FROM users → 모든 행 출력
 * SELECT * FROM users WHERE name='Alice' → cb 안에서 조건 검사
 *
 * 시간 복잡도: O(전체 행 수) — 인덱스를 사용하지 않으므로 느림
 *              WHERE id=N 일 때는 B+ tree를 사용하는 INDEX_LOOKUP이 훨씬 빠름
 */
void heap_scan(pager_t *pager, uint16_t row_size, scan_cb cb, void *ctx)
{
    (void)row_size;
    uint32_t pid = pager->header.first_heap_page_id;
    while (pid != 0) {
        uint8_t *page = pager_get_page(pager, pid);
        heap_page_header_t hph;
        memcpy(&hph, page, sizeof(hph));

        for (uint16_t i = 0; i < hph.slot_count; i++) {
            slot_t slot;
            size_t slot_off = sizeof(heap_page_header_t) + i * sizeof(slot_t);
            memcpy(&slot, page + slot_off, sizeof(slot));
            if (slot.status == SLOT_ALIVE) {
                row_ref_t ref = { .page_id = pid, .slot_id = i };
                if (!cb(page + slot.offset, ref, ctx)) {
                    pager_unpin(pager, pid);
                    return;
                }
            }
        }

        uint32_t next = hph.next_heap_page_id;
        pager_unpin(pager, pid);
        pid = next;
    }
}
