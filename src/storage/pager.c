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
 * 실패 모델과 보장 범위 (WAL 없음):
 *   - 프로세스 종료(SIGKILL): 이미 pwrite한 페이지는 OS 캐시에 남아 보존된다.
 *     flush_all 성공 이후의 변경만 파일에 있다고 말할 수 있고, 그 뒤 변경은 사라진다.
 *   - 부분 쓰기(torn): 한 페이지가 절반만 새 내용일 수 있다. 페이지 체크섬이
 *     없는 현재 형식은 이를 탐지하지 못한다.
 *   - 성공 반환 후 미반영(lost): pwrite/fsync가 성공을 돌려줘도 저장 장치가
 *     버렸다면 알 방법이 없다.
 *   - 여러 페이지에 걸친 변경은 원자적이지 않다. 기록 순서만으로 일관성을 보장하지 않는다.
 *   따라서 flush_all 성공은 "이 시점의 페이지 이미지를 쓰고 fsync가 오류 없이
 *   끝났다"까지만 뜻하며, 트랜잭션 단위 내구성이나 크래시 복구를 주장하지 않는다.
 *
 * 빈 페이지 재활용:
 *   삭제로 비워진 페이지는 free page 연결 리스트에 추가된다.
 *   free_page_head → page_A → page_B → 0 (끝)
 *   pager_alloc_page()에서 새 페이지 할당 전에 먼저 재활용한다.
 */

#include "storage/pager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/*
 * 잠금 순서 추적. pager.h의 순서(tail_lock → latch → pager_mutex → header_lock)를 거슬러
 * 높은 번호를 쥔 채 낮은 번호를 잡으면 센다. 시험 빌드(MINIDB_TEST_HOOKS)에서만 켜지며
 * 제품 빌드에서는 아무것도 하지 않는다.
 */
enum { RANK_TAIL = 1, RANK_LATCH = 2, RANK_PAGER = 3, RANK_HEADER = 4, RANK_COUNT = 5 };

#ifdef MINIDB_TEST_HOOKS
void (*minidb_test_hook)(minidb_hook_point_t point, pager_t* pager, uint32_t page_id);
static _Thread_local int held_ranks[RANK_COUNT];
static _Atomic uint64_t order_violations;

static void order_acquire(int rank) {
  for (int higher = rank + 1; higher < RANK_COUNT; higher++) {
    if (held_ranks[higher] > 0) {
      atomic_fetch_add(&order_violations, 1);
      break;
    }
  }
  held_ranks[rank]++;
}

static void order_release(int rank) {
  if (held_ranks[rank] > 0) held_ranks[rank]--;
}

uint64_t pager_test_lock_order_violations(void) {
  return atomic_load(&order_violations);
}
#define ORDER_ACQUIRE(rank) order_acquire(rank)
#define ORDER_RELEASE(rank) order_release(rank)
#else
#define ORDER_ACQUIRE(rank) ((void)0)
#define ORDER_RELEASE(rank) ((void)0)
#endif

static void pm_lock(pager_t* pager) {
  ORDER_ACQUIRE(RANK_PAGER);
  pthread_mutex_lock(&pager->pager_mutex);
}

static void pm_unlock(pager_t* pager) {
  pthread_mutex_unlock(&pager->pager_mutex);
  ORDER_RELEASE(RANK_PAGER);
}

void pager_header_lock(pager_t* pager) {
  ORDER_ACQUIRE(RANK_HEADER);
  pthread_mutex_lock(&pager->header_lock);
}

void pager_header_unlock(pager_t* pager) {
  pthread_mutex_unlock(&pager->header_lock);
  ORDER_RELEASE(RANK_HEADER);
}

void pager_tail_lock(pager_t* pager) {
  ORDER_ACQUIRE(RANK_TAIL);
  pthread_mutex_lock(&pager->tail_lock);
}

void pager_tail_unlock(pager_t* pager) {
  pthread_mutex_unlock(&pager->tail_lock);
  ORDER_RELEASE(RANK_TAIL);
}

/*
 * 새 루트를 게시한다. header 이미지와 잠금 없는 사본을 같은 header_lock 구간에서 바꿔
 * flush의 스냅샷이 둘 중 하나만 반영하는 일이 없게 한다. 옛 루트의 wlatch를 쥔 채 호출하므로
 * 루트를 읽은 뒤 latch를 기다리던 스레드는 latch를 얻은 다음 pager_root_id()를 다시 읽어
 * 바뀐 것을 알아본다.
 */
void pager_publish_root(pager_t* pager, uint32_t new_root) {
  pager_header_lock(pager);
  pager->header.root_index_page_id = new_root;
  atomic_store_explicit(&pager->root_page_id, new_root, memory_order_release);
  pager_header_unlock(pager);
  pager_note_header_change(pager);
}

#ifdef MINIDB_TEST_HOOKS
void pager_test_quiescence(pager_t* pager, uint32_t* pinned, uint32_t* latched) {
  *pinned = 0;
  *latched = 0;
  pm_lock(pager);
  for (int i = 0; i < MAX_FRAMES; i++) {
    frame_t* f = &pager->frames[i];
    if (!f->is_valid) continue;
    if (f->pin_count > 0) (*pinned)++;
    if (pthread_rwlock_trywrlock(&f->latch) == 0) {
      pthread_rwlock_unlock(&f->latch);
    } else {
      (*latched)++;
    }
  }
  pm_unlock(pager);
}
#endif

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

/* 연속 EINTR 허용 횟수. 넘으면 실패로 돌려 무한 재시도를 막는다 */
#define IO_EINTR_RETRY_MAX 100

static ssize_t libc_pread(int fd, void* buf, size_t n, off_t off) {
  return pread(fd, buf, n, off);
}

static ssize_t libc_pwrite(int fd, const void* buf, size_t n, off_t off) {
  return pwrite(fd, buf, n, off);
}

static const pager_io_t LIBC_IO = {libc_pread, libc_pwrite, fsync};
static pager_io_t g_default_io = {libc_pread, libc_pwrite, fsync};

void pager_set_default_io(const pager_io_t* io) {
  g_default_io = io ? *io : LIBC_IO;
}

/* 실패를 기록한다. 호출자는 pager_mutex를 쥐고 있거나 아직 pager를 공유하기 전이다 */
static void note_io_error(pager_t* p, int err) {
  p->io_error_seq++;
  p->io_errno = err;
}

/*
 * 디스크에서 page_id 위치의 페이지를 buf로 직접 읽는다 (캐시 우회).
 * 짧은 읽기는 이어 읽고, EINTR은 재시도한다. EOF로 페이지를 채우지 못하면 실패다.
 * 반환값: 0 = 성공, -1 = 실패 (errno 설정)
 */
static int pager_raw_read(pager_t* p, uint32_t page_id, uint8_t* buf) {
  off_t base = (off_t)page_id * p->page_size;
  size_t done = 0;
  int eintr = 0;
  while (done < p->page_size) {
    ssize_t r = p->io.pread(p->fd, buf + done, p->page_size - done,
                            base + (off_t)done);
    if (r < 0) {
      if (errno == EINTR && ++eintr <= IO_EINTR_RETRY_MAX) continue;
      return -1;
    }
    if (r == 0) {
      errno = EIO;
      return -1;
    }
    eintr = 0;
    done += (size_t)r;
  }
  return 0;
}

/*
 * buf의 내용을 디스크의 page_id 위치에 직접 쓴다 (캐시 우회).
 * 짧은 쓰기는 이어 쓰고, 진전이 없으면 ENOSPC로 실패한다.
 * 반환값: 0 = 성공, -1 = 실패 (errno 설정)
 */
static int pager_raw_write(pager_t* p, uint32_t page_id, const uint8_t* buf) {
  off_t base = (off_t)page_id * p->page_size;
  size_t done = 0;
  int eintr = 0;
  while (done < p->page_size) {
    ssize_t w = p->io.pwrite(p->fd, buf + done, p->page_size - done,
                             base + (off_t)done);
    if (w < 0) {
      if (errno == EINTR && ++eintr <= IO_EINTR_RETRY_MAX) continue;
      return -1;
    }
    if (w == 0) {
      errno = ENOSPC;
      return -1;
    }
    eintr = 0;
    done += (size_t)w;
  }
  return 0;
}

/* fsync. EINTR은 재시도하고, 실패하면 이후 flush 성공 주장을 막는다 */
static int pager_sync(pager_t* p) {
  int eintr = 0;
  while (p->io.fsync(p->fd) != 0) {
    if (errno == EINTR && ++eintr <= IO_EINTR_RETRY_MAX) continue;
    p->fsync_failed = true;
    return -1;
  }
  return 0;
}

static uint32_t frame_bucket(uint32_t page_id) {
  return page_id % FRAME_HASH_BUCKETS;
}

static void frame_index_init(pager_t* p) {
  for (int i = 0; i < FRAME_HASH_BUCKETS; i++) {
    p->frame_buckets[i] = -1;
  }
  for (int i = 0; i < MAX_FRAMES; i++) {
    p->frames[i].hash_next = -1;
  }
}

static void frame_index_insert(pager_t* p, int idx) {
  uint32_t bucket = frame_bucket(p->frames[idx].page_id);
  p->frames[idx].hash_next = p->frame_buckets[bucket];
  p->frame_buckets[bucket] = idx;
}

static void frame_index_remove(pager_t* p, int idx) {
  uint32_t bucket = frame_bucket(p->frames[idx].page_id);
  int *link = &p->frame_buckets[bucket];

  while (*link >= 0) {
    if (*link == idx) {
      *link = p->frames[idx].hash_next;
      p->frames[idx].hash_next = -1;
      return;
    }
    link = &p->frames[*link].hash_next;
  }

  p->frames[idx].hash_next = -1;
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
 * page_id -> frame_idx 해시 인덱스를 사용하므로 평균 O(1)이다.
 */
static int find_frame(pager_t* p, uint32_t page_id) {
  int idx = p->frame_buckets[frame_bucket(page_id)];
  while (idx >= 0) {
    if (p->frames[idx].is_valid && p->frames[idx].page_id == page_id) {
      return idx;
    }
    idx = p->frames[idx].hash_next;
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
 * 모든 프레임이 pin 상태이거나 dirty 프레임 기록에 실패하면 -1을 반환한다.
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
  /* dirty 프레임은 디스크에 기록 후 교체. 기록 실패 시 dirty 그대로 두고 교체하지 않는다 */
  if (p->frames[best].is_dirty) {
    if (pager_raw_write(p, p->frames[best].page_id, p->frames[best].data) != 0) {
      note_io_error(p, errno);
      return -1;
    }
    p->stats.pages_flushed++;
    if (p->log_flushes) {
      uint32_t ptype;
      memcpy(&ptype, p->frames[best].data, sizeof(uint32_t));
      fprintf(stderr, "[pager] evict  page %u (%s, dirty→disk)\n",
              p->frames[best].page_id, page_type_str(ptype));
    }
    p->frames[best].is_dirty = false;
  }
  frame_index_remove(p, best);
  p->frames[best].is_valid = false;
  return best;
}

/* ══════════════════════════════════════════════════════════════════════
 *  생명주기 (open / close)
 * ══════════════════════════════════════════════════════════════════════ */

/* pager_open 실패 정리: 지금까지 만든 자원을 모두 풀고 -1을 반환한다 */
static int open_fail(pager_t* pager, int fd) {
  int saved = errno;
  pthread_mutex_destroy(&pager->tail_lock);
  pthread_mutex_destroy(&pager->header_lock);
  pthread_mutex_destroy(&pager->pager_mutex);
  for (int i = 0; i < MAX_FRAMES; i++) {
    free(pager->frames[i].data);
    pager->frames[i].data = NULL;
  }
  close(fd);
  pager->fd = -1;
  errno = saved;
  return -1;
}

/* 헤더 page_size가 프레임 할당에 쓸 수 있는 값인지 확인한다 */
static bool page_size_valid(uint32_t ps) {
  return ps >= sizeof(db_header_t) && ps <= (1u << 20) && (ps & (ps - 1)) == 0;
}

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

  /*
   * 같은 파일을 쓰는 pager는 하나만 둔다. advisory lock은 열린 fd(open file
   * description)에 묶이므로 프로세스가 SIGKILL로 죽어도 커널이 풀어 준다.
   * pid 파일 같은 숫자 기록으로 생존을 판정하지 않는다.
   * 새 DB 생성(O_TRUNC)은 잠금을 얻은 뒤에 비워야 다른 pager의 파일을 지우지 않는다.
   */
  int flags = O_RDWR;
  if (create) {
    flags |= O_CREAT;
  }
  pager->io = g_default_io;
  int fd = open(path, flags, 0644);
  if (fd < 0) {
    pager->fd = -1;
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int err = errno;
    close(fd);
    pager->fd = -1;
    errno = err; /* 이미 열려 있으면 EWOULDBLOCK */
    return -1;
  }
  if (create && ftruncate(fd, 0) != 0) {
    int err = errno;
    close(fd);
    pager->fd = -1;
    errno = err;
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
      for (int j = 0; j < i; j++) free(pager->frames[j].data);
      close(fd);
      pager->fd = -1;
      errno = ENOMEM;
      return -1;
    }
  }

  frame_index_init(pager);

  /* pager_get_page()가 reopen 경로에서 바로 쓰이므로 먼저 초기화한다 */
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&pager->pager_mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  pthread_mutex_init(&pager->header_lock, NULL);
  pthread_mutex_init(&pager->tail_lock, NULL);

  /* DB 파일 최초 생성 */
  if (create) {
    pager->page_size = ps;
    pager->last_heap_page_id = 1;
    pager->heap_may_have_free_slots = false; /* 새 DB에는 삭제된 슬롯이 없다 */

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
    if (buf == NULL) {
      errno = ENOMEM;
      return open_fail(pager, fd);
    }

    /* page 0: DB 헤더 */
    memcpy(buf, h, sizeof(*h));
    int rc = pager_raw_write(pager, 0, buf);

    /* page 1: 빈 힙 페이지 (slot_count=0, 행 없음) */
    memset(buf, 0, ps);
    heap_page_header_t hph = {.page_type = PAGE_TYPE_HEAP,
                              .next_heap_page_id = 0,
                              .slot_count = 0,
                              .free_slot_head = SLOT_NONE,
                              .free_space_offset = 0,
                              .reserved = 0};
    memcpy(buf, &hph, sizeof(hph));
    if (rc == 0) rc = pager_raw_write(pager, 1, buf);

    /* page 2: 빈 B+ tree 리프 루트 (key_count=0) */
    memset(buf, 0, ps);
    leaf_page_header_t lph = {.page_type = PAGE_TYPE_LEAF,
                              .parent_page_id = 0,
                              .key_count = 0,
                              .next_leaf_page_id = 0,
                              .prev_leaf_page_id = 0};
    memcpy(buf, &lph, sizeof(lph));
    if (rc == 0) rc = pager_raw_write(pager, 2, buf);

    free(buf);
    if (rc == 0) rc = pager_sync(pager);
    if (rc != 0) {
      perror("pager: 새 DB 초기 기록 실패");
      return open_fail(pager, fd);
    }
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
    size_t probed = 0;
    int probe_err = 0;
    while (probed < sizeof(db_header_t) && probe_err == 0) {
      ssize_t r = pager->io.pread(fd, tmp + probed, sizeof(tmp) - probed,
                                  (off_t)probed);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) probe_err = errno;
      else if (r == 0) probe_err = EIO;
      else probed += (size_t)r;
    }
    if (probe_err != 0) {
      errno = probe_err;
      perror("pager: 헤더를 읽을 수 없습니다");
      return open_fail(pager, fd);
    }
    db_header_t th;
    memcpy(&th, tmp, sizeof(th));
    if (memcmp(th.magic, DB_MAGIC, 7) != 0 || !page_size_valid(th.page_size)) {
      fprintf(stderr, "pager: 지원하지 않는 데이터베이스 파일입니다\n");
      errno = EINVAL;
      return open_fail(pager, fd);
    }
    pager->page_size = th.page_size;
    ps = pager->page_size;

    /* DB의 page_size가 초기 할당 크기와 다르면 프레임 재할당 */
    if (ps != initial_ps) {
      for (int i = 0; i < MAX_FRAMES; i++) {
        free(pager->frames[i].data);
        pager->frames[i].data = (uint8_t*)calloc(1, ps);
        if (pager->frames[i].data == NULL) {
          errno = ENOMEM;
          return open_fail(pager, fd);
        }
      }
    }

    /* 전체 헤더 페이지 읽기 */
    uint8_t* hbuf = (uint8_t*)calloc(1, ps);
    if (hbuf == NULL) {
      errno = ENOMEM;
      return open_fail(pager, fd);
    }
    if (pager_raw_read(pager, 0, hbuf) != 0) {
      perror("pager: 헤더 페이지를 읽을 수 없습니다");
      free(hbuf);
      return open_fail(pager, fd);
    }
    memcpy(&pager->header, hbuf, sizeof(db_header_t));
    free(hbuf);

    /* 매직과 지원 파일 버전을 확인한다. v1에는 테이블 이름이 없다. */
    if (memcmp(pager->header.magic, DB_MAGIC, 7) != 0
        || (pager->header.version != 1 && pager->header.version != DB_VERSION)) {
      fprintf(stderr, "pager: 지원하지 않는 데이터베이스 파일입니다\n");
      errno = EINVAL;
      return open_fail(pager, fd);
    }

    if (pager->header.version == 1) {
      memset(pager->header.table_name, 0, sizeof(pager->header.table_name));
    }

    /*
     * 마지막 힙 페이지를 한 번만 복원해 두면,
     * 이후 순차 INSERT에서 매번 힙 체인 전체를 스캔하지 않아도 된다.
     * 같은 순회에서 FREE 슬롯 보유 여부도 함께 확인해
     * heap_may_have_free_slots 힌트를 정확한 값으로 복원한다.
     */
    pager->last_heap_page_id = pager->header.first_heap_page_id;
    pager->heap_may_have_free_slots = false;
    if (pager->last_heap_page_id != 0) {
      uint32_t pid = pager->last_heap_page_id;
      while (pid != 0) {
        uint8_t* page = pager_get_page(pager, pid);
        if (page == NULL) {
          perror("pager: 힙 페이지를 읽을 수 없습니다");
          return open_fail(pager, fd);
        }
        heap_page_header_t hph;
        memcpy(&hph, page, sizeof(hph));
        pager_unpin(pager, pid);
        if (hph.free_slot_head != SLOT_NONE) {
          pager->heap_may_have_free_slots = true;
        }
        pager->last_heap_page_id = pid;
        pid = hph.next_heap_page_id;
      }
    }
  }

  /* 잠금 없는 루트 읽기용 사본. 아직 pager를 공유하기 전이라 plain 초기화로 충분하다 */
  atomic_store_explicit(&pager->root_page_id, pager->header.root_index_page_id, memory_order_release);

  /* 프레임별 래치 초기화 (래치 커플링용) */
  for (int i = 0; i < MAX_FRAMES; i++)
    pthread_rwlock_init(&pager->frames[i].latch, NULL);

  return 0;
}

/*
 * pager_close - 데이터베이스를 닫는다.
 *
 * 동작:
 *   1. pager_flush_all()로 모든 dirty 프레임을 디스크에 기록
 *   2. 프레임 메모리 256개 해제
 *   3. 파일 디스크립터 닫기
 *
 * flush가 실패해도 자원은 해제한다. 이때 기록되지 못한 변경은 사라지므로
 * 반환값 -1을 호출자가 사용자에게 알려야 한다.
 */
int pager_close(pager_t* pager) {
  int rc = pager_flush_all(pager);
  for (int i = 0; i < MAX_FRAMES; i++) {
    pthread_rwlock_destroy(&pager->frames[i].latch);
    free(pager->frames[i].data);
    pager->frames[i].data = NULL;
  }
  if (pager->fd >= 0) {
    if (close(pager->fd) != 0 && rc == 0) rc = -1;
    pager->fd = -1;
  }
  pthread_mutex_destroy(&pager->tail_lock);
  pthread_mutex_destroy(&pager->header_lock);
  pthread_mutex_destroy(&pager->pager_mutex);
  return rc;
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
 *
 * 디스크 읽기(짧은 읽기·EOF 포함)나 교체 대상 dirty 프레임 기록이 실패하면
 * pin 없이 NULL을 반환하고 errno를 남긴다. 호출자는 NULL을 반드시 확인한다.
 */
static uint8_t* acquire_page(pager_t* pager, uint32_t page_id, bool fresh) {
  pm_lock(pager);
  if (pager->poisoned) {
    note_io_error(pager, EIO);
    pm_unlock(pager);
    errno = EIO;
    return NULL;
  }
  pager->stats.page_loads++;

  int idx = find_frame(pager, page_id);
  if (idx >= 0) {
    /* 캐시 히트: pin++하고 LRU tick 갱신 */
    pager->stats.cache_hits++;
    pager->frames[idx].pin_count++;
    pager->frames[idx].used_tick = ++pager->tick;
    pm_unlock(pager);
    return pager->frames[idx].data;
  }
  /* 캐시 미스: 프레임 교체 후 디스크에서 읽기 */
  pager->stats.cache_misses++;
  idx = evict_frame(pager);
  if (idx < 0) {
    pm_unlock(pager);
    return NULL;
  }

  /* 읽기가 실패하면 프레임을 등록하지 않아 반쯤 채워진 페이지가 캐시에 남지 않는다 */
  frame_t* f = &pager->frames[idx];
  memset(f->data, 0, pager->page_size);
  if (!fresh && pager_raw_read(pager, page_id, f->data) != 0) {
    int err = errno;
    note_io_error(pager, err);
    memset(f->data, 0, pager->page_size);
    pm_unlock(pager);
    errno = err;
    return NULL;
  }
  f->page_id = page_id;
  f->is_valid = true;
  f->is_dirty = false;
  f->pin_count = 1;
  f->used_tick = ++pager->tick;
  frame_index_insert(pager, idx);
  pm_unlock(pager);
  return f->data;
}

uint8_t* pager_get_page(pager_t* pager, uint32_t page_id) {
  return acquire_page(pager, page_id, false);
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
 * 기록에 실패하면 거기서 멈추고 오류를 기록한다. 실패한 프레임은 dirty로 남는다.
 * pager_mark_dirty가 반환값을 갖지 않으므로 호출자는 io_error_seq로 실패를 알아챈다.
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
    if (pager_raw_write(pager, f->page_id, f->data) != 0) {
      note_io_error(pager, errno);
      break; /* 실패한 프레임은 dirty로 남겨 다음 flush에서 다시 시도한다 */
    }
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
  pm_lock(pager);
  int idx = find_frame(pager, page_id);
  if (idx >= 0) {
    pager->frames[idx].is_dirty = true;
  }
  pager_note_header_change(pager);

  /* 워터마크 체크: dirty가 너무 많으면 선제 플러시 */
  if (!pager->poisoned && pager->dirty_high_watermark > 0 &&
      count_dirty(pager) >= pager->dirty_high_watermark) {
    pager_flush_dirty(pager, pager->dirty_low_watermark);
  }
  pm_unlock(pager);
}

/*
 * pager_unpin - 페이지의 pin_count를 감소시킨다.
 *
 * pin_count가 0이 되면 LRU 교체 대상이 된다.
 * pager_get_page() 호출 횟수만큼 pager_unpin()을 호출해야 한다.
 */
void pager_unpin(pager_t* pager, uint32_t page_id) {
  pm_lock(pager);
  int idx = find_frame(pager, page_id);
  if (idx >= 0 && pager->frames[idx].pin_count > 0) {
    pager->frames[idx].pin_count--;
  }
  pm_unlock(pager);
}

/* ======================================================================
 *  래치 기반 페이지 접근 (Latch Coupling / Crab Protocol)
 *
 *  pin + latch를 원자적으로 획득/해제한다.
 *  페이지가 pinned 상태이므로 프레임은 안정적이며,
 *  래치는 페이지 내용에 대한 동시 접근을 제어한다.
 *
 *  래치 순서: pager_mutex -> 해제 -> latch 획득 (데드락 방지)
 * ====================================================================== */

uint8_t *pager_get_page_rlatch(pager_t *pager, uint32_t page_id) {
    uint8_t *data = pager_get_page(pager, page_id);
    if (!data) return NULL;
    /* 페이지가 pinned 상태이므로 프레임이 evict되지 않는다 */
    pm_lock(pager);
    int idx = find_frame(pager, page_id);
    pm_unlock(pager);
    if (idx >= 0) {
        /* pager_mutex·header_lock을 쥔 채 latch를 기다리면 안 된다(pager.h의 잠금 순서) */
        MINIDB_HOOK(MINIDB_HOOK_LATCH_WAIT, pager, page_id);
        ORDER_ACQUIRE(RANK_LATCH);
        pthread_rwlock_rdlock(&pager->frames[idx].latch);
        MINIDB_HOOK(MINIDB_HOOK_LATCH_ACQUIRED, pager, page_id);
    }
    return data;
}

uint8_t *pager_get_page_wlatch(pager_t *pager, uint32_t page_id) {
    uint8_t *data = pager_get_page(pager, page_id);
    if (!data) return NULL;
    pm_lock(pager);
    int idx = find_frame(pager, page_id);
    pm_unlock(pager);
    if (idx >= 0) {
        MINIDB_HOOK(MINIDB_HOOK_LATCH_WAIT, pager, page_id);
        ORDER_ACQUIRE(RANK_LATCH);
        pthread_rwlock_wrlock(&pager->frames[idx].latch);
        MINIDB_HOOK(MINIDB_HOOK_LATCH_ACQUIRED, pager, page_id);
    }
    return data;
}

void pager_unlatch_r(pager_t *pager, uint32_t page_id) {
    pm_lock(pager);
    int idx = find_frame(pager, page_id);
    if (idx >= 0) {
        pthread_rwlock_unlock(&pager->frames[idx].latch);
        ORDER_RELEASE(RANK_LATCH);
        if (pager->frames[idx].pin_count > 0)
            pager->frames[idx].pin_count--;
    }
    pm_unlock(pager);
}

void pager_unlatch_w(pager_t *pager, uint32_t page_id) {
    pm_lock(pager);
    int idx = find_frame(pager, page_id);
    if (idx >= 0) {
        pthread_rwlock_unlock(&pager->frames[idx].latch);
        ORDER_RELEASE(RANK_LATCH);
        if (pager->frames[idx].pin_count > 0)
            pager->frames[idx].pin_count--;
    }
    pm_unlock(pager);
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
 * 프레임을 얻지 못하면 헤더를 바꾸지 않고 0을 반환한다.
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
  pm_lock(pager);
  /* 빈 페이지 재활용 */
  if (pager->header.free_page_head != 0) {
    uint32_t pid = pager->header.free_page_head;
    uint8_t* page = pager_get_page(pager, pid);
    if (page == NULL) {
      pm_unlock(pager);
      return 0;
    }
    free_page_header_t fph;
    memcpy(&fph, page, sizeof(fph));
    pager->header.free_page_head = fph.next_free_page;
    /* 재활용할 페이지를 0으로 초기화 */
    memset(page, 0, pager->page_size);
    pager_mark_dirty(pager, pid);
    pager_unpin(pager, pid);
    pm_unlock(pager);
    return pid;
  }
  /* 새 페이지 할당: 파일 끝에 추가 */
  uint32_t pid = pager->header.next_page_id;
  uint8_t* page = acquire_page(pager, pid, true); /* 디스크에 아직 없는 페이지라 읽지 않는다 */
  if (page == NULL) {
    pm_unlock(pager);
    return 0;
  }
  pager->header.next_page_id = pid + 1;
  memset(page, 0, pager->page_size);
  pager_mark_dirty(pager, pid);
  pager_unpin(pager, pid);
  pm_unlock(pager);
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
  pm_lock(pager);
  uint8_t* page = pager_get_page(pager, page_id);
  if (page == NULL) {
    /* 해제하지 못한 페이지는 새지만 데이터는 그대로다. 실패는 io_error_seq에 남았다 */
    pm_unlock(pager);
    return;
  }
  free_page_header_t fph = {.page_type = PAGE_TYPE_FREE,
                            .next_free_page = pager->header.free_page_head};
  memset(page, 0, pager->page_size);
  memcpy(page, &fph, sizeof(fph));
  pager_mark_dirty(pager, page_id);
  pager_unpin(pager, page_id);
  pager->header.free_page_head = page_id;
  pm_unlock(pager);
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
 *   4. 1~3이 모두 성공했을 때만 프레임의 dirty를 내린다
 *
 * 반환값: 0 = 성공, -1 = 실패 (errno 설정).
 * 실패하면 프레임은 dirty로 남아 다시 시도할 수 있다. 쓰기 실패 시 헤더는
 * 쓰지 않아 디스크 헤더가 기록되지 않은 페이지를 가리키지 않게 한다.
 * fsync가 한 번이라도 실패했다면 커널이 오류를 버렸을 수 있어, 다시 열기 전까지
 * 이후 flush도 성공으로 보고하지 않는다. WAL이 없으므로 성공은 "이 시점의
 * 페이지 이미지가 파일에 반영됨"만 뜻하고 트랜잭션 단위 내구성이 아니다.
 *
 * pager_close() 내부에서 호출되며, 프로그램 종료 전 데이터 유실을 방지한다.
 */
int pager_flush_all(pager_t* pager) {
  pm_lock(pager);
  if (pager->poisoned) {
    /* 일관되지 않은 메모리 상태를 쓰면 디스크의 마지막 정상 상태가 사라진다 */
    note_io_error(pager, EIO);
    pm_unlock(pager);
    errno = EIO;
    return -1;
  }
  /*
   * header_dirty를 먼저 내리고 header를 스냅샷한다. 스냅샷 뒤에 다른 스레드가 올린 표시는
   * 지워지지 않고, 아래에서 실패하면 내리기 전 값으로 되돌려 갱신이 사라지지 않게 한다.
   */
  bool was_header_dirty =
      atomic_exchange_explicit(&pager->header_dirty, false, memory_order_acq_rel);
  int rc = 0;
  int err = 0;
  uint32_t flush_count = 0;
  for (int i = 0; i < MAX_FRAMES && rc == 0; i++) {
    frame_t* f = &pager->frames[i];
    if (!f->is_valid || !f->is_dirty) continue;
    if (pager_raw_write(pager, f->page_id, f->data) != 0) {
      rc = -1;
      err = errno;
      break;
    }
    if (pager->log_flushes) {
      uint32_t ptype;
      memcpy(&ptype, f->data, sizeof(uint32_t));
      fprintf(stderr, "[pager] flush  page %u (%s)\n", f->page_id,
              page_type_str(ptype));
    }
    flush_count++;
  }
  if (rc == 0) {
    /* DB 헤더(page 0) 기록 */
    uint8_t* hbuf = (uint8_t*)calloc(1, pager->page_size);
    if (hbuf == NULL) {
      rc = -1;
      err = ENOMEM;
    } else {
      /* next_id·row_count·root는 header_lock 아래에서만 바뀐다. leaf lock이라 잠깐 잡는다 */
      pager_header_lock(pager);
      memcpy(hbuf, &pager->header, sizeof(db_header_t));
      pager_header_unlock(pager);
      MINIDB_HOOK(MINIDB_HOOK_FLUSH_SNAPSHOT, pager, 0);
      if (pager_raw_write(pager, 0, hbuf) != 0) {
        rc = -1;
        err = errno;
      }
      free(hbuf);
    }
  }
  if (rc == 0 && pager_sync(pager) != 0) {
    rc = -1;
    err = errno;
  }
  if (rc == 0 && pager->fsync_failed) {
    rc = -1;
    err = EIO;
  }
  if (rc != 0) {
    if (was_header_dirty) pager_note_header_change(pager);
    note_io_error(pager, err);
    pm_unlock(pager);
    errno = err;
    return -1;
  }
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (pager->frames[i].is_valid) pager->frames[i].is_dirty = false;
  }
  pager->stats.pages_flushed += flush_count;
  if (pager->log_flushes && flush_count > 0) {
    fprintf(stderr, "[pager] flush_all: %u pages written + header\n",
            flush_count);
  }
  pm_unlock(pager);
  return 0;
}

void pager_poison(pager_t* pager, int err) {
  pm_lock(pager);
  pager->poisoned = true;
  note_io_error(pager, err);
  pm_unlock(pager);
}

bool pager_is_poisoned(pager_t* pager) {
  pm_lock(pager);
  bool poisoned = pager->poisoned;
  pm_unlock(pager);
  return poisoned;
}

uint64_t pager_io_error_seq(pager_t* pager) {
  pm_lock(pager);
  uint64_t seq = pager->io_error_seq;
  pm_unlock(pager);
  return seq;
}

int pager_last_io_errno(pager_t* pager) {
  pm_lock(pager);
  int err = pager->io_errno;
  pm_unlock(pager);
  return err;
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
  /* 다른 스레드의 page_loads++ 같은 갱신(pager_mutex 아래)과 겹치지 않게 같은 lock으로 지운다 */
  pm_lock(pager);
  memset(&pager->stats, 0, sizeof(query_stats_t));
  pm_unlock(pager);
}
