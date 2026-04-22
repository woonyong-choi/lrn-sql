/*
 * pager.c — 디스크 I/O 및 페이지 캐시 관리자
 *
 * 역할:
 *   데이터베이스 파일을 고정 크기 페이지 단위로 읽고 쓰는 저수준 계층이다.
 *   메모리에 MAX_FRAMES(256)개의 프레임 버퍼를 유지하며, LRU 정책으로
 *   페이지를 교체한다. 모든 상위 모듈(heap, B+ tree)은 pager를 통해서만
 *   디스크에 접근한다.
 *
 * 페이지 크기:
 *   sysconf(_SC_PAGESIZE)로 OS 페이지 크기를 가져온다.
 *   - x86_64 리눅스: 4096바이트 (4KB)
 *   - Apple Silicon macOS: 16384바이트 (16KB)
 *   기존 DB 파일을 열 때는 헤더에 기록된 page_size를 사용한다.
 *
 * 파일 구조:
 *   [page 0: DB 헤더] [page 1: 첫 힙 페이지] [page 2: B+ tree 루트] [page 3...]
 *   |__ page_size ___|__ page_size __________|__ page_size _________|
 *
 * 캐시(프레임) 동작:
 *   pager_get_page(pid) → 캐시에 있으면 반환(pin++), 없으면 LRU 교체 후
 * 디스크에서 읽기 pager_mark_dirty(pid) → 수정됨 표시, flush 시 디스크에 기록
 *   pager_unpin(pid) → pin-- (0이 되면 LRU 교체 대상)
 *
 * 빈 페이지 재활용:
 *   삭제로 비워진 페이지는 free page 연결 리스트에 추가된다.
 *   free_page_head → page_A → page_B → 0 (끝)
 *   pager_alloc_page()에서 새 페이지 할당 전에 먼저 재활용한다.
 */

#include "storage/pager.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 페이지 타입 → 문자열 변환 (로깅용) ── */
static const char* page_type_str(uint32_t ptype) {
  switch (ptype) {
    case PAGE_TYPE_HEADER:
      return "HEADER";
    case PAGE_TYPE_HEAP:
      return "HEAP";
    case PAGE_TYPE_LEAF:
      return "LEAF";
    case PAGE_TYPE_INTERNAL:
      return "INTERNAL";
    case PAGE_TYPE_FREE:
      return "FREE";
    default:
      return "UNKNOWN";
  }
}

/* ── 디스크 직접 I/O 헬퍼 ──
 *
 * pread/pwrite로 파일의 특정 오프셋에 직접 읽기/쓰기한다.
 * 오프셋 = page_id × page_size
 *
 * 예시: page_id=3, page_size=4096
 *   → 파일 오프셋 = 3 × 4096 = 12288바이트 위치에서 4096바이트를 읽음
 */

/* 디스크에서 page_id 위치의 페이지를 buf로 직접 읽는다 (캐시 우회) */
static ssize_t pager_raw_read(pager_t* p, uint32_t page_id, uint8_t* buf) {
  off_t off = (off_t)page_id * p->page_size;
  return pread(p->fd, buf, p->page_size, off);
}

/* buf의 내용을 디스크의 page_id 위치에 직접 쓴다 (캐시 우회) */
static ssize_t pager_raw_write(pager_t* p, uint32_t page_id,
                               const uint8_t* buf) {
  off_t off = (off_t)page_id * p->page_size;
  return pwrite(p->fd, buf, p->page_size, off);
}

/* ── 프레임 탐색 / 교체 ──
 *
 * 프레임 배열(frames[0..255])에서 원하는 페이지를 찾거나,
 * 교체할 프레임을 LRU 정책으로 선택한다.
 *
 * 프레임 상태:
 *   is_valid=false → 아직 사용 안 된 빈 프레임
 *   is_valid=true, pin_count>0 → 사용 중 (교체 불가)
 *   is_valid=true, pin_count=0 → 교체 가능 (used_tick이 작을수록 우선)
 */

/*
 * find_frame - 프레임 배열에서 page_id에 해당하는 프레임 인덱스를 찾는다.
 *
 * 캐시 히트 시 인덱스를 반환하고, 캐시 미스 시 -1을 반환한다.
 * 선형 탐색 O(256) — 프레임 수가 고정이므로 충분히 빠르다.
 */
static int find_frame(pager_t* p, uint32_t page_id) {
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (p->frames[i].is_valid && p->frames[i].page_id == page_id) {
      return i;
    }
  }
  return -1;
}

/*
 * evict_frame - 교체할 프레임을 선택한다.
 *
 * 탐색 순서:
 *   1단계: 빈 프레임(is_valid == false)이 있으면 바로 반환
 *   2단계: pin_count == 0인 프레임 중 used_tick이 가장 작은 것을 선택 (LRU)
 *   3단계: 선택된 프레임이 dirty이면 디스크에 플러시한 뒤 반환
 *
 * 예시: frames[0..4]의 상태가 아래와 같을 때
 *   [0] valid, pin=1, tick=10  → 사용 중, 교체 불가
 *   [1] valid, pin=0, tick=5   → 교체 가능 (가장 오래됨 ←선택)
 *   [2] valid, pin=0, tick=8   → 교체 가능
 *   [3] invalid                → 빈 프레임 (이게 먼저 선택됨)
 *
 * 모든 프레임이 pin 상태이면 -1을 반환한다.
 */
static int evict_frame(pager_t* p) {
  /* 빈 프레임 탐색 */
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (!p->frames[i].is_valid) {
      return i;
    }
  }
  /* LRU: 고정되지 않은 프레임 중 가장 오래된 것 선택 */
  int best = -1;
  uint64_t min_tick = UINT64_MAX;
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (p->frames[i].pin_count == 0 && p->frames[i].used_tick < min_tick) {
      min_tick = p->frames[i].used_tick;
      best = i;
    }
  }
  if (best < 0) {
    fprintf(stderr, "pager: 모든 프레임이 고정되어 교체할 수 없습니다\n");
    return -1;
  }
  /* dirty 프레임은 디스크에 기록 후 교체 */
  if (p->frames[best].is_dirty) {
    pager_raw_write(p, p->frames[best].page_id, p->frames[best].data);
    p->stats.pages_flushed++;
    if (p->log_flushes) {
      uint32_t ptype;
      memcpy(&ptype, p->frames[best].data, sizeof(uint32_t));
      fprintf(stderr, "[pager] evict  page %u (%s, dirty→disk)\n",
              p->frames[best].page_id, page_type_str(ptype));
    }
    p->frames[best].is_dirty = false;
  }
  p->frames[best].is_valid = false;
  return best;
}

/* ══════════════════════════════════════════════════════════════════════
 *  생명주기 (open / close)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * pager_open - 데이터베이스 파일을 열거나 새로 생성한다.
 *
 * create == true (새 DB 생성):
 *   1. sysconf(_SC_PAGESIZE)로 OS 페이지 크기를 결정한다.
 *      - x86_64: 4096, Apple Silicon: 16384
 *   2. 프레임 버퍼 256개를 해당 크기로 calloc한다.
 *   3. DB 헤더(page 0)를 초기화한다:
 *      - magic = "MINIDB\0", version = 1
 *      - first_heap_page_id = 1
 *      - root_index_page_id = 2
 *      - next_page_id = 3 (다음에 할당할 페이지)
 *   4. page 0(헤더), page 1(빈 힙), page 2(빈 B+ tree 리프)를 디스크에
 * 기록한다.
 *
 * create == false (기존 DB 열기):
 *   1. 파일의 첫 4096바이트를 읽어 헤더를 확인한다.
 *   2. 헤더의 page_size가 OS 페이지 크기와 다르면 프레임을 재할당한다.
 *      예: 리눅스(4096)에서 만든 DB를 macOS(16384)에서 열면 프레임 크기 조정
 *   3. 매직 넘버("MINIDB\0")를 검증한다.
 *
 * 반환값: 0 = 성공, -1 = 실패
 */
int pager_open(pager_t* pager, const char* path, bool create) {
  memset(pager, 0, sizeof(*pager));

  /* OS 페이지 크기를 기본값으로 사용 */
  uint32_t ps = (uint32_t)sysconf(_SC_PAGESIZE);
  if (ps == 0 || ps == (uint32_t)-1) {
    ps = 4096;
  }

  /* 워터마크 기본값: dirty 프레임 64개 넘으면 16개까지 선제 플러시 */
  pager->dirty_high_watermark = 64;
  pager->dirty_low_watermark = 16;

  int flags = O_RDWR;
  if (create) {
    flags |= O_CREAT | O_TRUNC;
  }
  int fd = open(path, flags, 0644);
  if (fd < 0) {
    return -1;
  }

  pager->fd = fd;

  /*
   * 프레임 버퍼 256개 할당 (초기에는 OS 페이지 크기로)
   * 4096바이트 × 256 = 약 1MB 메모리 사용
   * 16384바이트 × 256 = 약 4MB 메모리 사용
   */
  for (int i = 0; i < MAX_FRAMES; i++) {
    pager->frames[i].data = (uint8_t*)calloc(1, ps);
    if (pager->frames[i].data == NULL) {
      close(fd);
      return -1;
    }
  }

  /* DB 파일 최초 생성 */
  if (create) {
    pager->page_size = ps;
    pager->last_heap_page_id = 1;

    /* DB 헤더 초기화 */
    db_header_t* h = &pager->header;
    memcpy(h->magic, DB_MAGIC, 8);
    h->version = DB_VERSION;
    h->page_size = ps;
    h->first_heap_page_id = 1; /* page 1 = 첫 번째 힙 페이지 */
    h->root_index_page_id = 2; /* page 2 = B+ tree 루트 (빈 리프) */
    h->next_page_id = 3;       /* 다음 할당 = page 3부터 */
    h->free_page_head = 0;     /* 빈 페이지 없음 */
    h->next_id = 1;            /* 자동 증가 ID 시작값 */
    h->row_count = 0;
    h->column_count = 0;
    h->row_size = 0;

    /*
     * 초기 3개 페이지를 디스크에 기록한다.
     * 파일 레이아웃:
     *   [page 0: DB 헤더][page 1: 빈 힙][page 2: 빈 리프]
     *   |_____ ps ______||_____ ps ____||_____ ps ______|
     */
    uint8_t* buf = (uint8_t*)calloc(1, ps);

    /* page 0: DB 헤더 */
    memcpy(buf, h, sizeof(*h));
    pager_raw_write(pager, 0, buf);

    /* page 1: 빈 힙 페이지 (slot_count=0, 행 없음) */
    memset(buf, 0, ps);
    heap_page_header_t hph = {.page_type = PAGE_TYPE_HEAP,
                              .next_heap_page_id = 0,
                              .slot_count = 0,
                              .free_slot_head = SLOT_NONE,
                              .free_space_offset = 0,
                              .reserved = 0};
    memcpy(buf, &hph, sizeof(hph));
    pager_raw_write(pager, 1, buf);

    /* page 2: 빈 B+ tree 리프 루트 (key_count=0) */
    memset(buf, 0, ps);
    leaf_page_header_t lph = {.page_type = PAGE_TYPE_LEAF,
                              .parent_page_id = 0,
                              .key_count = 0,
                              .next_leaf_page_id = 0,
                              .prev_leaf_page_id = 0};
    memcpy(buf, &lph, sizeof(lph));
    pager_raw_write(pager, 2, buf);

    free(buf);
    fsync(fd);
  } else {
    /*
     * 기존 DB 열기
     *
     * 헤더의 page_size가 OS 페이지 크기와 다를 수 있다.
     * 예: 리눅스(4096)에서 만든 DB를 Apple Silicon(16384)에서 열면
     *     initial_ps=16384인데 헤더의 page_size=4096
     *     → 프레임을 4096 크기로 재할당해야 함
     */
    uint32_t initial_ps = ps;
    uint8_t tmp[4096];
    pread(fd, tmp, sizeof(tmp), 0);
    db_header_t* th = (db_header_t*)tmp;
    pager->page_size = th->page_size;
    ps = pager->page_size;

    /* DB의 page_size가 초기 할당 크기와 다르면 프레임 재할당 */
    if (ps != initial_ps) {
      for (int i = 0; i < MAX_FRAMES; i++) {
        free(pager->frames[i].data);
        pager->frames[i].data = (uint8_t*)calloc(1, ps);
      }
    }

    /* 전체 헤더 페이지 읽기 */
    uint8_t* hbuf = (uint8_t*)calloc(1, ps);
    pread(fd, hbuf, ps, 0);
    memcpy(&pager->header, hbuf, sizeof(db_header_t));
    free(hbuf);

    /* 매직 넘버 검증 ("MINIDB\0" 7바이트 비교) */
    if (memcmp(pager->header.magic, DB_MAGIC, 7) != 0) {
      fprintf(stderr, "pager: 유효하지 않은 매직 넘버입니다\n");
      close(fd);
      return -1;
    }

    /*
     * 마지막 힙 페이지를 한 번만 복원해 두면,
     * 이후 순차 INSERT에서 매번 힙 체인 전체를 스캔하지 않아도 된다.
     */
    pager->last_heap_page_id = pager->header.first_heap_page_id;
    if (pager->last_heap_page_id != 0) {
      uint32_t pid = pager->last_heap_page_id;
      while (pid != 0) {
        uint8_t* page = pager_get_page(pager, pid);
        heap_page_header_t hph;
        memcpy(&hph, page, sizeof(hph));
        pager_unpin(pager, pid);
        pager->last_heap_page_id = pid;
        pid = hph.next_heap_page_id;
      }
    }
  }
  /* Recursive mutex: pager_alloc_page 등 내부에서 pager_get_page를 재호출하므로 */
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&pager->pager_mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  return 0;
}

/*
 * pager_close - 데이터베이스를 닫는다.
 *
 * 동작:
 *   1. pager_flush_all()로 모든 dirty 프레임을 디스크에 기록
 *   2. 프레임 메모리 256개 해제
 *   3. 파일 디스크립터 닫기
 */
void pager_close(pager_t* pager) {
  pager_flush_all(pager);
  for (int i = 0; i < MAX_FRAMES; i++) {
    free(pager->frames[i].data);
    pager->frames[i].data = NULL;
  }
  if (pager->fd >= 0) {
    close(pager->fd);
    pager->fd = -1;
  }
  pthread_mutex_destroy(&pager->pager_mutex);
}

/* ══════════════════════════════════════════════════════════════════════
 *  캐시 기반 페이지 접근
 *
 *  호출 흐름 (캐시 미스 시):
 *    pager_get_page(pid=5)
 *      → find_frame(): -1 (캐시에 없음)
 *      → evict_frame(): LRU 프레임 선택 (dirty면 먼저 디스크에 기록)
 *      → pread(fd, buf, page_size, 5 * page_size): 디스크에서 읽기
 *      → pin_count = 1, used_tick 갱신
 *      → 포인터 반환
 *
 *  호출 흐름 (캐시 히트 시):
 *    pager_get_page(pid=5)
 *      → find_frame(): idx 반환 (이미 캐시에 있음)
 *      → pin_count++, used_tick 갱신
 *      → 포인터 반환
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * pager_get_page - page_id에 해당하는 페이지를 반환한다.
 *
 * 반환된 포인터는 프레임 내부 버퍼(page_size 바이트)를 가리킨다.
 * pin_count가 증가되어 있으므로 사용 후 반드시 pager_unpin()을 호출해야 한다.
 * pin 해제를 잊으면 해당 프레임이 영원히 교체 불가 → 메모리 부족 발생.
 */
uint8_t* pager_get_page(pager_t* pager, uint32_t page_id) {
  pthread_mutex_lock(&pager->pager_mutex);
  pager->stats.page_loads++;

  int idx = find_frame(pager, page_id);
  if (idx >= 0) {
    /* 캐시 히트: pin++하고 LRU tick 갱신 */
    pager->stats.cache_hits++;
    pager->frames[idx].pin_count++;
    pager->frames[idx].used_tick = ++pager->tick;
    pthread_mutex_unlock(&pager->pager_mutex);
    return pager->frames[idx].data;
  }
  /* 캐시 미스: 프레임 교체 후 디스크에서 읽기 */
  pager->stats.cache_misses++;
  idx = evict_frame(pager);
  if (idx < 0) {
    pthread_mutex_unlock(&pager->pager_mutex);
    return NULL;
  }

  frame_t* f = &pager->frames[idx];
  f->page_id = page_id;
  f->is_valid = true;
  f->is_dirty = false;
  f->pin_count = 1;
  f->used_tick = ++pager->tick;
  memset(f->data, 0, pager->page_size);
  pager_raw_read(pager, page_id, f->data);
  pthread_mutex_unlock(&pager->pager_mutex);
  return f->data;
}

/*
 * count_dirty - 현재 dirty 프레임 수를 센다.
 */
static uint32_t count_dirty(pager_t* pager) {
  uint32_t n = 0;
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (pager->frames[i].is_valid && pager->frames[i].is_dirty) {
      n++;
    }
  }
  return n;
}

/*
 * pager_flush_dirty - dirty + unpinned 프레임을 target_count개까지 줄인다.
 *
 * LRU 순서(used_tick이 작은 것)로 dirty 프레임을 디스크에 기록한다.
 * pin_count > 0인 프레임은 건너뛴다.
 *
 * 예시: dirty=70, high=64, low=16
 *   → 70 - 16 = 54개를 플러시해야 함
 *   → used_tick이 작은 순서대로 54개를 기록
 */
static void pager_flush_dirty(pager_t* pager, uint32_t target_count) {
  uint32_t dirty = count_dirty(pager);
  uint32_t flushed = 0;

  while (dirty > target_count) {
    /* dirty + unpinned 중 가장 오래된 프레임 선택 */
    int best = -1;
    uint64_t min_tick = UINT64_MAX;
    for (int i = 0; i < MAX_FRAMES; i++) {
      frame_t* f = &pager->frames[i];
      if (f->is_valid && f->is_dirty && f->pin_count == 0 &&
          f->used_tick < min_tick) {
        min_tick = f->used_tick;
        best = i;
      }
    }
    if (best < 0) break; /* 모든 dirty가 pinned → 포기 */

    frame_t* f = &pager->frames[best];
    pager_raw_write(pager, f->page_id, f->data);
    pager->stats.pages_flushed++;
    if (pager->log_flushes) {
      uint32_t ptype;
      memcpy(&ptype, f->data, sizeof(uint32_t));
      fprintf(stderr, "[pager] flush  page %u (%s, dirty→clean)\n", f->page_id,
              page_type_str(ptype));
    }
    f->is_dirty = false;
    dirty--;
    flushed++;
  }

  if (flushed > 0 && pager->log_flushes) {
    fprintf(stderr, "[pager] watermark flush: %u pages written\n", flushed);
  }
}

/*
 * pager_mark_dirty - 해당 페이지를 dirty로 표시한다.
 *
 * dirty 프레임은 flush 시 디스크에 기록된다.
 * 페이지 내용을 수정한 후에는 반드시 이 함수를 호출해야 한다.
 * 호출하지 않으면 수정 내용이 유실된다.
 *
 * dirty 프레임 수가 high_watermark 이상이면 자동으로 선제 플러시를 수행한다.
 * 예: high=64, low=16 → dirty가 64개 넘으면 16개까지 줄인다.
 */
void pager_mark_dirty(pager_t* pager, uint32_t page_id) {
  pthread_mutex_lock(&pager->pager_mutex);
  int idx = find_frame(pager, page_id);
  if (idx >= 0) {
    pager->frames[idx].is_dirty = true;
  }
  pager->header_dirty = true;

  /* 워터마크 체크: dirty가 너무 많으면 선제 플러시 */
  if (pager->dirty_high_watermark > 0 &&
      count_dirty(pager) >= pager->dirty_high_watermark) {
    pager_flush_dirty(pager, pager->dirty_low_watermark);
  }
  pthread_mutex_unlock(&pager->pager_mutex);
}

/*
 * pager_unpin - 페이지의 pin_count를 감소시킨다.
 *
 * pin_count가 0이 되면 LRU 교체 대상이 된다.
 * pager_get_page() 호출 횟수만큼 pager_unpin()을 호출해야 한다.
 */
void pager_unpin(pager_t* pager, uint32_t page_id) {
  pthread_mutex_lock(&pager->pager_mutex);
  int idx = find_frame(pager, page_id);
  if (idx >= 0 && pager->frames[idx].pin_count > 0) {
    pager->frames[idx].pin_count--;
  }
  pthread_mutex_unlock(&pager->pager_mutex);
}

/* ══════════════════════════════════════════════════════════════════════
 *  페이지 할당 / 해제
 *
 *  새 페이지가 필요한 경우:
 *    - B+ tree 노드 분할 시 새 리프/내부 노드
 *    - 힙 페이지가 모두 가득 찼을 때 새 힙 페이지
 *
 *  빈 페이지 재활용 우선:
 *    free_page_head → page_7 → page_12 → 0
 *    pager_alloc_page() 호출 시 page_7을 꺼내고
 *    free_page_head = page_12로 갱신
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * pager_alloc_page - 새 페이지를 할당한다.
 *
 * 동작:
 *   1. free_page_head가 0이 아니면 → 빈 페이지 재활용
 *      - free_page_head를 꺼내고 다음 페이지로 갱신
 *      - 꺼낸 페이지를 0으로 초기화
 *   2. 없으면 → next_page_id를 사용하고 1 증가
 *      - 파일 끝에 새 페이지가 추가됨
 *
 * 예시: free_page_head=7, page 7의 next_free_page=12
 *   → page 7을 꺼내서 반환
 *   → free_page_head = 12
 *
 * 예시: free_page_head=0, next_page_id=5
 *   → page 5를 반환
 *   → next_page_id = 6
 */
uint32_t pager_alloc_page(pager_t* pager) {
  pthread_mutex_lock(&pager->pager_mutex);
  /* 빈 페이지 재활용 */
  if (pager->header.free_page_head != 0) {
    uint32_t pid = pager->header.free_page_head;
    uint8_t* page = pager_get_page(pager, pid);
    free_page_header_t fph;
    memcpy(&fph, page, sizeof(fph));
    pager->header.free_page_head = fph.next_free_page;
    /* 재활용할 페이지를 0으로 초기화 */
    memset(page, 0, pager->page_size);
    pager_mark_dirty(pager, pid);
    pager_unpin(pager, pid);
    pthread_mutex_unlock(&pager->pager_mutex);
    return pid;
  }
  /* 새 페이지 할당: 파일 끝에 추가 */
  uint32_t pid = pager->header.next_page_id++;
  uint8_t* page = pager_get_page(pager, pid);
  memset(page, 0, pager->page_size);
  pager_mark_dirty(pager, pid);
  pager_unpin(pager, pid);
  pthread_mutex_unlock(&pager->pager_mutex);
  return pid;
}

/*
 * pager_free_page - 페이지를 해제하여 free page 리스트에 추가한다.
 *
 * 동작:
 *   1. 페이지를 0으로 초기화한다.
 *   2. free_page_header_t를 기록한다:
 *      - page_type = PAGE_TYPE_FREE
 *      - next_free_page = 기존 free_page_head
 *   3. free_page_head를 이 페이지로 갱신한다.
 *
 * 예시: 기존 free_page_head=12, page 7을 해제
 *   → page 7: { PAGE_TYPE_FREE, next=12 }
 *   → free_page_head = 7
 *   → 리스트: 7 → 12 → 0
 */
void pager_free_page(pager_t* pager, uint32_t page_id) {
  pthread_mutex_lock(&pager->pager_mutex);
  uint8_t* page = pager_get_page(pager, page_id);
  free_page_header_t fph = {.page_type = PAGE_TYPE_FREE,
                            .next_free_page = pager->header.free_page_head};
  memset(page, 0, pager->page_size);
  memcpy(page, &fph, sizeof(fph));
  pager_mark_dirty(pager, page_id);
  pager_unpin(pager, page_id);
  pager->header.free_page_head = page_id;
  pthread_mutex_unlock(&pager->pager_mutex);
}

/* ══════════════════════════════════════════════════════════════════════
 *  플러시
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * pager_flush_all - 모든 dirty 프레임을 디스크에 기록하고, 헤더도 갱신한다.
 *
 * 동작:
 *   1. frames[0..255]를 순회하며 dirty 프레임을 pwrite로 기록
 *   2. DB 헤더(page 0)를 기록 (next_id, row_count 등이 갱신되었을 수 있음)
 *   3. fsync()로 디스크 동기화 보장
 *
 * pager_close() 내부에서 호출되며, 프로그램 종료 전 데이터 유실을 방지한다.
 */
void pager_flush_all(pager_t* pager) {
  pthread_mutex_lock(&pager->pager_mutex);
  /* dirty 프레임을 디스크에 기록 */
  uint32_t flush_count = 0;
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (pager->frames[i].is_valid && pager->frames[i].is_dirty) {
      pager_raw_write(pager, pager->frames[i].page_id, pager->frames[i].data);
      if (pager->log_flushes) {
        uint32_t ptype;
        memcpy(&ptype, pager->frames[i].data, sizeof(uint32_t));
        fprintf(stderr, "[pager] flush  page %u (%s)\n",
                pager->frames[i].page_id, page_type_str(ptype));
      }
      pager->frames[i].is_dirty = false;
      flush_count++;
    }
  }
  if (pager->log_flushes && flush_count > 0) {
    fprintf(stderr, "[pager] flush_all: %u pages written + header\n",
            flush_count);
  }
  /* DB 헤더(page 0) 기록 */
  uint8_t* hbuf = (uint8_t*)calloc(1, pager->page_size);
  memcpy(hbuf, &pager->header, sizeof(db_header_t));
  pager_raw_write(pager, 0, hbuf);
  free(hbuf);
  fsync(pager->fd);
  pager->header_dirty = false;
  pthread_mutex_unlock(&pager->pager_mutex);
}

/* ══════════════════════════════════════════════════════════════════════
 *  통계
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * pager_reset_stats - 쿼리 통계를 0으로 초기화한다.
 *
 * 매 쿼리 실행 직전에 호출하여, 해당 쿼리의 I/O 패턴만 정확히 측정한다.
 */
void pager_reset_stats(pager_t* pager) {
  memset(&pager->stats, 0, sizeof(query_stats_t));
}
