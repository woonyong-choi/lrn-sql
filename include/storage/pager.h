/*
 * pager.h — 디스크 I/O 및 페이지 캐시 관리자 인터페이스
 *
 * pager는 DB 파일을 고정 크기 페이지 단위로 관리하며,
 * MAX_FRAMES(256)개의 프레임 버퍼에 LRU 캐시를 유지한다.
 */

#ifndef PAGER_H
#define PAGER_H

#include "page_format.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>

#define MAX_FRAMES 256  /* 메모리에 유지할 최대 페이지 프레임 수 */
#define FRAME_HASH_BUCKETS 512  /* page_id -> frame_idx 해시 버킷 수 */

/*
 * 쿼리 실행 통계: 각 쿼리 실행 전 초기화되어, 쿼리 동안의 I/O 패턴을 추적한다.
 * .debug 명령어로 디버그 모드를 켜면 매 쿼리 후 이 통계가 출력된다.
 */
typedef struct {
    uint32_t page_loads;     /* pager_get_page() 호출 총 수 */
    uint32_t cache_hits;     /* 캐시 히트 수 (프레임에 이미 있던 횟수) */
    uint32_t cache_misses;   /* 캐시 미스 수 (디스크에서 읽어야 했던 횟수) */
    uint32_t pages_flushed;  /* 디스크에 기록한 페이지 수 (evict + watermark) */
} query_stats_t;

/*
 * 디스크 I/O 진입점. 기본값은 libc이고, 테스트가 실패·짧은 I/O를 주입할 때만 바꾼다.
 */
typedef struct {
    ssize_t (*pread)(int fd, void *buf, size_t n, off_t off);
    ssize_t (*pwrite)(int fd, const void *buf, size_t n, off_t off);
    int     (*fsync)(int fd);
} pager_io_t;

/*
 * 프레임: 메모리에 캐시된 하나의 페이지를 나타낸다.
 * pin_count > 0이면 교체 대상에서 제외되며,
 * used_tick이 작을수록 LRU 교체 우선 대상이 된다.
 */
typedef struct {
    uint32_t page_id;    /* 이 프레임에 적재된 페이지 ID */
    bool     is_valid;   /* 프레임에 유효한 데이터가 있는지 여부 */
    bool     is_dirty;   /* 수정되어 디스크에 기록이 필요한지 여부 */
    uint32_t pin_count;  /* 현재 사용 중인 참조 수 (0이면 교체 가능) */
    uint64_t used_tick;  /* 마지막 접근 시점의 틱 (LRU 판별용) */
    int      hash_next;  /* 같은 해시 버킷의 다음 frame 인덱스 */
    uint8_t *data;       /* 페이지 데이터 버퍼 (page_size 바이트) */
    pthread_rwlock_t latch;  /* 페이지 래치: B+ tree 래치 커플링용 */
} frame_t;

/*
 * 페이저: DB 파일 핸들과 페이지 캐시를 관리하는 최상위 구조체.
 * 모든 상위 모듈(heap, B+ tree)은 pager_t를 통해 디스크에 접근한다.
 */
typedef struct {
    int         fd;                  /* DB 파일 디스크립터 */
    uint32_t    page_size;           /* 페이지 크기 (바이트) */
    db_header_t header;              /* DB 헤더 (page 0의 인메모리 사본) */
    _Atomic uint32_t last_heap_page_id; /* 마지막 힙 페이지 ID (순차 INSERT 최적화용 캐시).
                                         * tail_lock 아래에서 release로 게시하고 잠금 없이 acquire로 읽는다 */
    _Atomic uint32_t root_page_id;       /* B+ tree 루트의 잠금 없는 읽기용 사본. header.root_index_page_id와
                                         * 함께 header_lock 아래에서만 갱신한다 (pager_publish_root) */
    _Atomic bool heap_may_have_free_slots; /* 힙 어딘가에 재활용 가능한 FREE 슬롯이
                                           * 있을 수 있는지 (보수적 힌트).
                                           * false면 INSERT가 힙 체인 재탐색을 생략한다.
                                           * 스레드 경합 시 최악의 경우 불필요한 탐색
                                           * 1회 또는 재활용 1회 지연만 발생 (정합성 무관) */
    _Atomic uint64_t heap_chain_rescan_count; /* find_heap_page()가 힙 체인 전체를
                                           * 재탐색한 횟수 (계측 전용).
                                           * 순차 INSERT 회귀 테스트가 이 값이
                                           * 0에 머무는지 확인한다. */
    frame_t     frames[MAX_FRAMES];  /* 페이지 프레임 배열 */
    int         frame_buckets[FRAME_HASH_BUCKETS]; /* page_id -> frame_idx 해시 인덱스 */
    uint64_t    tick;                /* 전역 틱 카운터 (LRU 추적용) */
    _Atomic bool header_dirty;       /* 헤더가 수정되었는지 여부. 모든 설정은 pager_note_header_change()로,
                                      * 해제는 pager_flush_all()의 exchange로만 한다 */
    bool        log_flushes;         /* CLI에 pager flush 로그를 출력할지 여부 */
    bool        debug_mode;          /* 쿼리 통계 출력 모드 (.debug로 토글) */
    uint32_t    dirty_low_watermark; /* write-back 후 유지할 dirty frame 목표치 */
    uint32_t    dirty_high_watermark;/* 이 개수 이상 dirty면 선제 flush 수행 */
    query_stats_t stats;             /* 현재 쿼리의 실행 통계 */
    pager_io_t  io;                  /* 디스크 I/O 진입점 (기본 libc) */
    uint64_t    io_error_seq;        /* I/O 실패 누적 횟수. 문장 전후 비교로 실패를 귀속한다 */
    int         io_errno;            /* 마지막 I/O 실패의 errno (0 = 없음) */
    bool        poisoned;            /* 구조 변경 도중 I/O가 실패해 메모리 상태가 일관되지 않음.
                                      * 이후 페이지 접근은 모두 실패하고 flush는 디스크를 건드리지 않는다 */
    bool        fsync_failed;        /* fsync가 한 번이라도 실패함. 커널이 오류를 버렸을 수 있어
                                      * 다시 열기 전에는 flush 성공을 주장하지 않는다 */
    pthread_mutex_t pager_mutex;     /* Level 1: 프레임 메타데이터 보호 */
    pthread_mutex_t header_lock;     /* header 이미지 필드(next_id, row_count, root) 보호. 다른 lock을 쥐지 않는
                                      * leaf lock이며 pager_header_lock()으로만 잡는다 */
    pthread_mutex_t tail_lock;       /* 힙 체인 확장(새 tail 할당·연결·게시) 직렬화. pager_tail_lock()으로만 잡는다 */
} pager_t;

/* 생명주기 */
int  pager_open(pager_t *pager, const char *path, bool create);  /* DB 열기/생성. 0 = 성공, -1 = 실패 */
int  pager_close(pager_t *pager);  /* DB 닫기 (flush 후 리소스 해제). flush 실패 시 -1, 자원은 해제한다 */

/* 이후 pager_open이 쓰는 I/O 진입점을 바꾼다. NULL이면 libc로 복원. 테스트 전용 */
void pager_set_default_io(const pager_io_t *io);

/* 캐시 기반 페이지 접근 */
uint8_t *pager_get_page(pager_t *pager, uint32_t page_id);     /* 페이지 로드 (pin++). 읽기·교체 쓰기 실패 시 NULL */
void     pager_mark_dirty(pager_t *pager, uint32_t page_id);   /* dirty 표시 */
void     pager_unpin(pager_t *pager, uint32_t page_id);        /* pin 해제 (pin--) */

/* 래치 기반 페이지 접근 (Latch Coupling / Crab Protocol 용) */
uint8_t *pager_get_page_rlatch(pager_t *pager, uint32_t page_id); /* pin + 읽기 래치 */
uint8_t *pager_get_page_wlatch(pager_t *pager, uint32_t page_id); /* pin + 쓰기 래치 */
void     pager_unlatch_r(pager_t *pager, uint32_t page_id);       /* 읽기 래치 해제 + unpin */
void     pager_unlatch_w(pager_t *pager, uint32_t page_id);       /* 쓰기 래치 해제 + unpin */

/* 페이지 할당/해제 */
uint32_t pager_alloc_page(pager_t *pager);                     /* 새 페이지 할당. 실패 시 0 (page 0은 헤더) */
void     pager_free_page(pager_t *pager, uint32_t page_id);    /* 페이지 해제 (free 리스트에 추가) */

/* 플러시 */
int pager_flush_all(pager_t *pager);  /* 모든 dirty 프레임을 디스크에 기록. 실패 시 -1, 실패한 프레임은 dirty 유지 */

/*
 * 여러 페이지에 걸친 변경 도중 I/O가 실패해 되돌릴 수 없을 때 호출한다.
 * WAL이 없어 메모리 상태를 복구할 수 없으므로, 이후 페이지 접근을 모두 막고
 * flush가 디스크의 마지막 상태를 덮어쓰지 않게 한다. 다시 열어야 풀린다.
 */
void pager_poison(pager_t *pager, int err);
bool pager_is_poisoned(pager_t *pager);

/* 오류 조회 */
uint64_t pager_io_error_seq(pager_t *pager);   /* 누적 I/O 실패 횟수 */
int      pager_last_io_errno(pager_t *pager);  /* 마지막 실패의 errno */

/* 통계 */
void pager_reset_stats(pager_t *pager);  /* 쿼리 통계 초기화 (쿼리 실행 전 호출) */

/*
 * 잠금 순서 (낮은 번호를 먼저 잡고 높은 번호를 쥔 채 낮은 번호를 잡지 않는다):
 *   1. tail_lock      힙 체인 확장. 쥔 채 2·3·4를 잡을 수 있다
 *   2. 페이지 latch   부모 → 자식 순. 쥔 채 3·4를 잡을 수 있다
 *   3. pager_mutex    프레임 메타데이터. 재귀 lock이며 쥔 채 4를 잡을 수 있다
 *   4. header_lock    leaf. 쥔 채 다른 lock·latch·I/O를 하지 않는다
 * 행 lock·range lock과 엔진 rwlock은 db.c가 이보다 먼저 잡는다. latch를 기다리는 동안
 * pager_mutex·header_lock·tail_lock 중 3·4번을 쥐고 있지 않아야 한다.
 */
void pager_header_lock(pager_t *pager);
void pager_header_unlock(pager_t *pager);
void pager_tail_lock(pager_t *pager);
void pager_tail_unlock(pager_t *pager);

/* 헤더 이미지가 바뀌었음을 알린다. flush와 경합해도 갱신을 잃지 않는다 (atomic) */
static inline void pager_note_header_change(pager_t *pager)
{
    atomic_store_explicit(&pager->header_dirty, true, memory_order_release);
}

/* B+ tree 루트 ID를 잠금 없이 읽는다. 읽은 뒤 latch를 잡고 다시 확인해야 한다 */
static inline uint32_t pager_root_id(pager_t *pager)
{
    return atomic_load_explicit(&pager->root_page_id, memory_order_acquire);
}

/* 새 루트를 게시한다. 호출자는 옛 루트의 wlatch를 쥐고 있어야 하며 header_lock을 쥐면 안 된다 */
void pager_publish_root(pager_t *pager, uint32_t new_root);

#ifdef MINIDB_TEST_HOOKS
/*
 * 시험 전용 훅. 결정적 스케줄과 정지 위치 기록에만 쓰며 제품 빌드에는 들어가지 않는다.
 * 훅은 해당 지점에서 lock을 쥔 채 호출되므로 오래 막는 훅은 의도한 스케줄에서만 쓴다.
 */
typedef enum {
    MINIDB_HOOK_TAIL_READ,        /* find_heap_page가 tail을 읽은 직후 (page_id = 읽은 tail) */
    MINIDB_HOOK_TAIL_LOCK_WAIT,   /* alloc_heap_page가 tail_lock을 잡기 직전 (page_id = 찬 페이지) */
    MINIDB_HOOK_TAIL_LINK,        /* 새 페이지를 할당·초기화한 뒤 옛 tail에 잇기 직전 (page_id = 새 페이지) */
    MINIDB_HOOK_TAIL_PUBLISH,     /* 옛 tail에 연결하고 latch를 푼 뒤 새 tail을 게시하기 직전 (page_id = 새 페이지) */
    MINIDB_HOOK_ROOT_READ,        /* B+ tree가 루트 ID를 읽은 직후 latch 전 (page_id = 읽은 루트) */
    MINIDB_HOOK_FLUSH_SNAPSHOT,   /* flush가 header 스냅샷을 뜬 직후 (page_id = 0) */
    MINIDB_HOOK_LATCH_WAIT,       /* 페이지 latch를 기다리기 직전 */
    MINIDB_HOOK_LATCH_ACQUIRED    /* 페이지 latch를 얻은 직후 */
} minidb_hook_point_t;

extern void (*minidb_test_hook)(minidb_hook_point_t point, pager_t *pager, uint32_t page_id);
#define MINIDB_HOOK(point, pager, page_id) \
    do { if (minidb_test_hook) minidb_test_hook((point), (pager), (page_id)); } while (0)

/* 지금까지 관측한 잠금 순서 위반 수 (스레드별 추적, 위 순서를 거스른 획득) */
uint64_t pager_test_lock_order_violations(void);
/* 모든 worker가 멈춘 뒤 호출: pin이 남은 프레임 수와 latch가 잡힌 프레임 수를 센다 */
void pager_test_quiescence(pager_t *pager, uint32_t *pinned, uint32_t *latched);
#else
#define MINIDB_HOOK(point, pager, page_id) ((void)0)
#endif

#endif /* PAGER_H */
