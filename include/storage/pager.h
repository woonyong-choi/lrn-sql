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
#include <pthread.h>

#define MAX_FRAMES 256  /* 메모리에 유지할 최대 페이지 프레임 수 */

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
    uint8_t *data;       /* 페이지 데이터 버퍼 (page_size 바이트) */
} frame_t;

/*
 * 페이저: DB 파일 핸들과 페이지 캐시를 관리하는 최상위 구조체.
 * 모든 상위 모듈(heap, B+ tree)은 pager_t를 통해 디스크에 접근한다.
 */
typedef struct {
    int         fd;                  /* DB 파일 디스크립터 */
    uint32_t    page_size;           /* 페이지 크기 (바이트) */
    db_header_t header;              /* DB 헤더 (page 0의 인메모리 사본) */
    uint32_t    last_heap_page_id;   /* 마지막 힙 페이지 ID (순차 INSERT 최적화용 캐시) */
    frame_t     frames[MAX_FRAMES];  /* 페이지 프레임 배열 */
    uint64_t    tick;                /* 전역 틱 카운터 (LRU 추적용) */
    bool        header_dirty;        /* 헤더가 수정되었는지 여부 */
    bool        log_flushes;         /* CLI에 pager flush 로그를 출력할지 여부 */
    bool        debug_mode;          /* 쿼리 통계 출력 모드 (.debug로 토글) */
    uint32_t    dirty_low_watermark; /* write-back 후 유지할 dirty frame 목표치 */
    uint32_t    dirty_high_watermark;/* 이 개수 이상 dirty면 선제 flush 수행 */
    query_stats_t stats;             /* 현재 쿼리의 실행 통계 */
    pthread_mutex_t pager_mutex;     /* Level 2: 프레임 메타데이터 보호 */
} pager_t;

/* 생명주기 */
int  pager_open(pager_t *pager, const char *path, bool create);  /* DB 열기/생성 */
void pager_close(pager_t *pager);  /* DB 닫기 (flush 후 리소스 해제) */

/* 캐시 기반 페이지 접근 */
uint8_t *pager_get_page(pager_t *pager, uint32_t page_id);     /* 페이지 로드 (pin++) */
void     pager_mark_dirty(pager_t *pager, uint32_t page_id);   /* dirty 표시 */
void     pager_unpin(pager_t *pager, uint32_t page_id);        /* pin 해제 (pin--) */

/* 페이지 할당/해제 */
uint32_t pager_alloc_page(pager_t *pager);                     /* 새 페이지 할당 */
void     pager_free_page(pager_t *pager, uint32_t page_id);    /* 페이지 해제 (free 리스트에 추가) */

/* 플러시 */
void pager_flush_all(pager_t *pager);  /* 모든 dirty 프레임을 디스크에 기록 */

/* 통계 */
void pager_reset_stats(pager_t *pager);  /* 쿼리 통계 초기화 (쿼리 실행 전 호출) */

#endif /* PAGER_H */
