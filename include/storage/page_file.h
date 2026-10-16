#ifndef PAGE_FILE_H
#define PAGE_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "storage/overflow.h"

/*
 * 새 저장 형식의 파일 I/O 드라이버. OS 페이지 크기와 독립적으로 물리 페이지 8192바이트를 쓴다.
 * 기존 pager·MINIDB v1/v2 파일과 무관한 독립 API이며, 옛 파일을 열거나 해석하거나 바꾸지 않는다.
 *
 * 페이지 배치(모든 정수는 little-endian):
 *   0..31   envelope: "LRPG", 버전 u16, 종류 u16, page ID u32, table ID u32,
 *           본문 길이 u32, 예약 8바이트(0), 체크섬 u32(FNV-1a, 체크섬 필드 제외 전체)
 *   32..8191 본문 8160바이트. 본문 길이 뒤는 0이어야 한다.
 * page 0, 1은 제어 페이지 두 장이다. 세대 번호가 큰 쪽이 현재 값이고 commit은 작은 쪽에 쓴다.
 *
 * 보장하지 않는 것: 이 모듈은 WAL·거래·DB 전체 재시작 복구를 제공하지 않는다.
 * - 부분 쓰기(torn)는 체크섬으로 탐지하지만, 성공을 돌려준 뒤 저장 장치가 버린 쓰기(lost)는
 *   옛 페이지가 자기 일관적이라 탐지하지 못한다. 가장 최근 제어 페이지 쓰기가 사라지면
 *   한 세대 전 상태로 정상 열리며 "게시되지 않은 것"과 구분되지 않는다.
 * - 제어 페이지 한쪽이 손상됐을 때 다른 쪽으로 열리는 fallback은 open_info.damaged_slots로
 *   알리지만, 손상된 쪽이 최신이었는지는 알 수 없다.
 * - 여러 페이지 갱신의 원자성은 없다. commit은 "데이터 페이지 fsync → 제어 페이지 쓰기 → fsync"
 *   순서만 지킨다.
 * - 해제한 페이지의 재사용 목록은 메모리에만 있다. 다시 열면 회수되지 않고 새는 공간이 된다.
 * 단일 스레드 계약이다.
 */

#define PAGE_FILE_PAGE_BYTES 8192u
#define PAGE_ENVELOPE_BYTES 32u
#define PAGE_BODY_BYTES (PAGE_FILE_PAGE_BYTES - PAGE_ENVELOPE_BYTES)
#define PAGE_FILE_CONTROL_PAGES 2u
#define PAGE_FILE_NO_PAGE 0u
#define PAGE_FILE_FORMAT_VERSION 1u
/* durable extension. 제어 본문이 52바이트에서 76바이트로 늘고 applied_lsn·wal_file_id·durable_epoch가 붙는다 */
#define PAGE_FILE_DURABLE_FORMAT_VERSION 2u
#define PAGE_FILE_MAX_PAGES 0xFFFFFFF0u

typedef enum {
    PAGE_KIND_CONTROL = 1,
    PAGE_KIND_RECORD = 2,
    PAGE_KIND_OVERFLOW = 3,
    /* 테이블 heap의 페이지 디렉터리. 본문은 자체 magic·version을 갖는다(heap_store.h). */
    PAGE_KIND_DIRECTORY = 4,
    /* 테이블 기본 키 색인 노드. 본문은 자체 magic·version을 갖는다(pk_tree.h). */
    PAGE_KIND_INDEX = 5
} page_kind_t;

typedef enum {
    PAGE_FILE_OK,
    PAGE_FILE_INVALID,      /* 인자·범위·이중 해제 */
    PAGE_FILE_IO,           /* 읽기·쓰기·fsync·열기 실패. errno를 남긴다 */
    PAGE_FILE_CORRUPT,      /* 체크섬·magic·page ID·길이·세대 불일치, 잘린 파일 */
    PAGE_FILE_UNSUPPORTED,  /* 알 수 없는 version·페이지 크기·byte order, 옛 MINIDB 파일 */
    PAGE_FILE_NO_CONTROL,   /* 유효한 제어 페이지가 없음. 게시되지 않은 파일 */
    PAGE_FILE_EXISTS,       /* 목적지 또는 partial 파일이 이미 있음 */
    PAGE_FILE_LOCKED,       /* 다른 열린 핸들이 잠금을 쥐고 있음 */
    PAGE_FILE_NOMEM,
    PAGE_FILE_LIMIT,        /* 페이지 번호 한계 */
    PAGE_FILE_PUBLISHED_DIRTY, /* 게시는 됐으나 partial 삭제 또는 디렉터리 fsync가 실패함 */
    PAGE_FILE_MISMATCH         /* 제어 페이지의 WAL 식별자와 다른 로그, 또는 durable이 아닌 파일 */
} page_file_status_t;

/* 테스트가 실패를 주입하는 자리. 기본값은 libc다. */
typedef struct {
    ssize_t (*pread)(int fd, void *buf, size_t size, off_t offset);
    ssize_t (*pwrite)(int fd, const void *buf, size_t size, off_t offset);
    int (*fsync)(int fd);
    int (*link)(const char *from, const char *to);
    int (*unlink)(const char *path);
} page_file_io_t;

/* 이후 열리는 핸들이 쓰는 I/O 진입점을 바꾼다. NULL이면 libc로 복원한다. 테스트 전용. */
void page_file_set_default_io(const page_file_io_t *io);

/*
 * 읽기 캐시. 핸들마다 체크섬을 검증한 데이터 페이지 본문을 최대 PAGE_FILE_CACHE_PAGES장 담는다. 적중한 읽기는
 * pread와 체크섬 계산 없이 복사만 한다. 쓰기는 pwrite가 성공한 뒤에만 캐시를 갱신하고 실패하면 그 페이지를 뺀다.
 * 그래서 캐시는 그 핸들이 검증했거나 디스크에 쓴 바이트와 언제나 같다. 손상 페이지는 담지 않는다.
 * 쓰기 핸들은 파일을 배타 잠금하므로 다른 핸들이 같은 파일을 바꾸는 일은 계약 밖이다.
 * 프레임 메모리는 처음 쓰일 때 할당하며 할당에 실패하면 그 핸들은 캐시 없이 동작한다.
 */
#define PAGE_FILE_CACHE_PAGES 256u

/* 이후 열리는 핸들의 캐시 프레임 수. 0이면 캐시를 쓰지 않는다. 측정·테스트용. */
void page_file_set_default_cache_pages(size_t pages);

typedef struct {
    uint64_t hits;       /* 캐시에서 돌려준 데이터 페이지 읽기 */
    uint64_t misses;     /* 파일에서 읽은 데이터 페이지 읽기 */
    uint64_t evictions;  /* 용량 때문에 내보낸 프레임 */
} page_file_cache_stats_t;

/* 현재 기본 I/O 진입점. 옛 파일 읽기(legacy_migrate)가 같은 실패 주입 자리를 쓴다. */
page_file_io_t page_file_default_io(void);

typedef struct page_file page_file_t;

/* 이 핸들의 캐시 지표. 캐시가 아직 없거나 file이 NULL이면 0이다 */
page_file_cache_stats_t page_file_cache_stats(const page_file_t *file);

/* 제어 페이지가 담는 현재 상태. */
typedef struct {
    uint64_t generation;          /* commit마다 1씩 오른다. 첫 commit이 1이다 */
    uint32_t page_count;          /* 제어 페이지 포함 높은 수위 */
    uint32_t catalog_head;        /* 카탈로그 overflow 체인 head. 없으면 PAGE_FILE_NO_PAGE */
    uint32_t catalog_size;
    uint64_t catalog_generation;
    /*
     * 제어 형식. 1은 52바이트 본문(기존 파일)이고 아래 세 값은 항상 0이다. 2는 durable extension이다.
     * applied_lsn: 이 제어 페이지가 가리키는 상태에 이미 반영된 WAL 위치(COMMIT LSN). 상태 참조와 한 번의
     *   제어 쓰기로 함께 게시되므로 둘이 어긋난 채 보이는 일이 없다.
     * wal_file_id: 이 파일과 짝인 WAL 파일의 식별자(wal_file_id). LSN은 WAL 파일 안의 오프셋이라 다른 로그에는
     *   의미가 없으므로 한 번 정하면 바꿀 수 없다.
     * durable_epoch: 호출자가 정하는 세대 번호. 이 모듈은 저장만 한다.
     * durable_ready: 이 제어 페이지를 쓸 때 직전 현재 페이지도 v2였다(두 번째 이상의 v2 commit). 슬롯 자신에
     *   기록되므로 준비가 끝난 뒤 반대편 슬롯이 손상돼도 살아남은 슬롯이 ready로 읽힌다.
     */
    uint32_t format_version;
    uint64_t applied_lsn;
    uint64_t wal_file_id;
    uint32_t durable_epoch;
    bool durable_ready;
} page_file_control_t;

typedef struct {
    uint32_t damaged_slots;       /* 손상으로 쓰지 못한 제어 슬롯 수. 0이 아니면 fallback이다 */
    uint32_t blank_slots;         /* 한 번도 쓰이지 않은(전부 0) 제어 슬롯 수 */
} page_file_open_info_t;

/*
 * 새 파일을 만든다. 목적지는 path이고 실제 쓰기는 "path.partial"에 한다. partial은 O_EXCL로 만든
 * 뒤 배타 잠금을 잡고, 제어 페이지 두 장을 0으로 채운다. 첫 commit 전에는 어떤 열기도 성공하지
 * 않는다. 목적지나 partial이 이미 있으면 PAGE_FILE_EXISTS이며 어느 쪽도 바꾸지 않는다.
 * publish 전에 close하면 partial을 지운다.
 */
page_file_status_t page_file_create(const char *path, page_file_t **out);

/*
 * 첫 commit이 끝난 새 파일을 목적지 이름으로 게시한다. link()는 목적지가 있으면 실패하므로
 * rename과 달리 다른 프로세스가 먼저 만든 목적지를 덮어쓰지 않는다. 그 뒤 partial을 지우고
 * 디렉터리를 fsync한다. 실패하면 목적지를 만들지 않았거나(EXISTS, IO, UNSUPPORTED) 핸들은
 * 여전히 partial을 가리킨다. PAGE_FILE_PUBLISHED_DIRTY는 목적지가 보이지만 정리나 이름 fsync가
 * 실패했다는 뜻이며 이름의 내구성을 주장할 수 없다.
 */
page_file_status_t page_file_publish(page_file_t *file);

/*
 * 게시된 파일을 연다. 쓰기 전에는 아무것도 쓰지 않으며 version·길이·header가 맞지 않으면
 * 파일을 바꾸지 않고 거절한다. writable이면 배타, 아니면 공유 잠금을 non-blocking으로 잡는다.
 * 옛 MINIDB 파일은 PAGE_FILE_UNSUPPORTED다. 어떤 경우에도 자동으로 변환하지 않는다.
 */
page_file_status_t page_file_open(const char *path, bool writable, page_file_t **out,
                                  page_file_open_info_t *info);
/* 핸들을 닫고 잠금을 푼다. 게시 전이면 partial을 지운다. close(2) 실패는 IO다. */
page_file_status_t page_file_close(page_file_t *file);

const page_file_control_t *page_file_control(const page_file_t *file);
/*
 * 지금 할당된 페이지의 높은 수위(제어 페이지 포함). 아직 commit하지 않은 페이지를 포함하므로 게시된 제어 페이지의
 * page_count 이상이다. 한 번의 commit으로 묶는 여러 변경(트랜잭션)이 이전 변경이 새로 할당한 페이지를 읽을 때의 범위 검사에
 * 쓴다. file이 NULL이면 0이다.
 */
uint32_t page_file_allocated_pages(const page_file_t *file);
bool page_file_is_published(const page_file_t *file);
/* 이 파일을 자기 호출자가 알 수 있게 partial 경로를 돌려준다. 게시 뒤에는 NULL. */
const char *page_file_partial_path(const page_file_t *file);

/*
 * 본문 8160바이트를 읽는다. 종류와 table ID가 기대와 다르면 CORRUPT다. page_id는 제어 페이지를
 * 제외한 할당된 범위여야 한다. 실패하면 body를 바꾸지 않는다.
 */
page_file_status_t page_file_read(page_file_t *file, uint32_t page_id, page_kind_t kind,
                                  uint32_t table_id, uint8_t body[PAGE_BODY_BYTES]);
page_file_status_t page_file_write(page_file_t *file, uint32_t page_id, page_kind_t kind,
                                   uint32_t table_id, const uint8_t body[PAGE_BODY_BYTES]);

/*
 * 페이지 하나의 envelope만 검증해 종류와 table ID를 알려 준다. 읽기 전용이며 checksum·page ID·
 * 길이 검사는 page_file_read와 같다. 위치 locator가 어떤 종류의 페이지를 가리키는지 해석하기 전에
 * 호출하며, 알 수 없는 종류 값은 PAGE_FILE_UNSUPPORTED다. 실패하면 info를 바꾸지 않는다.
 */
typedef struct {
    page_kind_t kind;
    uint32_t table_id;
} page_file_page_info_t;

page_file_status_t page_file_page_info(page_file_t *file, uint32_t page_id,
                                       page_file_page_info_t *info);

/*
 * 새 페이지 번호를 받는다. 재사용 가능한 페이지를 먼저 돌려주고 없으면 높은 수위를 올린다.
 * 이 시점에는 파일에 아무것도 쓰지 않으며 현재 제어 페이지와 fallback 제어 페이지 어느 쪽이
 * 가리켰을 수 있는 페이지도 돌려주지 않는다. 실패하면 상태를 바꾸지 않는다.
 */
page_file_status_t page_file_alloc(page_file_t *file, uint32_t *page_id);
/*
 * 페이지를 놓는다. 곧바로 재사용하지 않는다. 가리키던 참조를 바꾼 제어 페이지가 commit되어도
 * 그 직전 세대(fallback)가 아직 이 페이지를 가리킬 수 있으므로, 그 commit 다음의 commit까지
 * 두 번 성공한 뒤에야 alloc이 돌려줄 수 있다. 그래서 최신 제어 쓰기가 사라져 한 세대 전으로
 * 열려도 그 세대가 가리키는 페이지는 덮어쓰이지 않았다. 호출자는 가리키던 참조를 바꾼 다음에
 * release한다. commit이 실패하면 놓은 페이지는 계속 보류 상태이다.
 */
page_file_status_t page_file_release(page_file_t *file, uint32_t page_id);
size_t page_file_pending_release_count(const page_file_t *file);
size_t page_file_aging_release_count(const page_file_t *file);
size_t page_file_reusable_count(const page_file_t *file);

/*
 * durable extension. 형식은 파일마다 명시적으로 한 번 올리며 되돌리지 않는다. 자동 변환은 없다.
 *   - 52바이트 v1 제어 페이지는 그대로 읽고(format_version 1, 세 값 0) enable하지 않으면 v1로 계속 쓴다.
 *   - enable은 다음 commit부터 v2 본문(76바이트)을 쓴다. v1만 아는 구버전은 v2 슬롯을 손상으로 보므로,
 *     두 슬롯이 모두 v2가 되면 구버전은 열 수 없다. 둘 중 하나만 v2인 동안에는 구버전이 한 세대 전 상태로
 *     열릴 수 있고 이때 applied_lsn은 0으로 보인다. 그래서 첫 v2 commit은 ready가 아니고 두 번째 v2 commit부터
 *     ready이며, ready가 되기 전에는 applied_lsn을 올리지 못한다. WAL로 거래를 승인하기 전에 enable 뒤 commit을
 *     두 번 성공시킨다.
 *   - v2 슬롯이 손상돼 v1 슬롯으로 fallback되면 이 파일만으로는 durable이었는지 알 수 없다. 그 판정은 WAL과
 *     함께 보는 호출자 몫이다(durable_pair.h). 이 모듈은 fallback 사실만 open_info.damaged_slots로 알린다.
 *   - 알 수 없는 확장 flags는 UNSUPPORTED이고 예약 영역은 해석하지 않는다.
 * wal_file_id는 0이 아니어야 한다. 같은 값으로 다시 부르면 무동작이고 다른 값은 INVALID다.
 * 쓰기 핸들만 쓸 수 있고 commit 전까지 디스크에 닿지 않는다.
 */
page_file_status_t page_file_enable_durable(page_file_t *file, uint64_t wal_file_id, uint32_t durable_epoch);
/* 현재 제어 페이지의 durable_ready. applied_lsn을 올릴 수 있는 조건이다. */
bool page_file_durable_ready(const page_file_t *file);
/*
 * 다음 commit과 함께 게시할 applied_lsn을 정한다. durable이 enable된 핸들만 쓸 수 있고 값은 현재 commit된
 * 값보다 작을 수 없다. 현재 값보다 올리려면 durable_ready여야 한다. commit이 성공하기 전까지 디스크에 닿지
 * 않으며, 상태 참조(데이터 페이지, 카탈로그)와 같은 제어 쓰기 한 번으로 게시된다.
 */
page_file_status_t page_file_stage_applied_lsn(page_file_t *file, uint64_t applied_lsn);
/*
 * durable_epoch를 올린다. 세션에 새 epoch를 보여 주기 전에 먼저 이 값을 commit해 영속화하기 위한 것이다
 * (재시작 뒤 epoch는 영속된 값보다 커야 한다). enable된 핸들만 쓸 수 있고, 같은 wal_file_id를 유지하며,
 * 현재 commit된 값보다 커야 한다(같거나 작으면 INVALID). 이미 staged한 같은 값은 무동작이다.
 * applied_lsn과 마찬가지로 durable_ready가 아니면 INVALID이고 commit 전까지 디스크에 닿지 않는다.
 */
page_file_status_t page_file_stage_durable_epoch(page_file_t *file, uint32_t durable_epoch);
/* 현재 commit된 durable_epoch의 다음 값. UINT32_MAX를 넘으면 PAGE_FILE_LIMIT이고 *next는 바뀌지 않는다. */
page_file_status_t page_file_next_durable_epoch(const page_file_t *file, uint32_t *next);
/*
 * 파일과 WAL의 짝을 확인한다. 현재 제어 페이지가 v2이고 wal_file_id가 같을 때만 OK이며 그 밖에는
 * PAGE_FILE_MISMATCH다. 열기 순서에서 WAL을 열어 replay하기 전에 부른다.
 */
page_file_status_t page_file_check_wal_identity(const page_file_t *file, uint64_t wal_file_id);

/* 다음 commit이 제어 페이지에 쓸 카탈로그 참조를 정한다. commit 전까지 디스크에 닿지 않는다. */
page_file_status_t page_file_stage_catalog(page_file_t *file, uint32_t head, uint32_t size,
                                           uint64_t catalog_generation);
/*
 * 데이터 fsync → 오래된 제어 슬롯에 새 세대 쓰기 → fsync. 성공해야 보류 중이던 해제 페이지가
 * 재사용 가능해진다. 이 핸들이 아는 현재 세대와 보류 목록은 실패하면 그대로이지만, 실패한
 * commit이 디스크에 새 세대를 남기지 않았다고 항상 말할 수는 없다. 결과는 page_file_last_commit_outcome이
 * 알린다. 제어 쓰기를 시도한 뒤의 실패는 다시 열면 새 상태가 보일 수 있고 아닐 수도 있다.
 * fsync나 제어 쓰기가 한 번 실패한 핸들은 커널이 오류를 버렸을 수 있어 poison되며 이후 commit을
 * 거절한다.
 */
page_file_status_t page_file_commit(page_file_t *file);

typedef enum {
    PAGE_FILE_COMMIT_NONE,         /* 이 핸들에서 commit을 시도한 적 없음 */
    PAGE_FILE_COMMIT_NOT_APPLIED,  /* 제어 쓰기를 시작하기 전에 실패했다. 디스크의 세대는 그대로다 */
    PAGE_FILE_COMMIT_UNKNOWN,      /* 제어 쓰기를 시도한 뒤 실패했다. 다시 열면 새 상태가 보일 수 있다 */
    PAGE_FILE_COMMIT_APPLIED       /* 제어 쓰기와 fsync까지 성공했다 */
} page_file_commit_outcome_t;

/* 마지막 commit 호출의 결과. poison된 핸들이 거절한 commit은 poison을 일으킨 호출의 값을 유지한다. */
page_file_commit_outcome_t page_file_last_commit_outcome(const page_file_t *file);
/* fsync·제어 쓰기 실패로 이 핸들이 더는 commit하지 않는지. */
bool page_file_is_poisoned(const page_file_t *file);

/*
 * 메모리 회계용 읽기 전용 접근자(요청 바이트 기준, allocator 오버헤드 제외).
 * page_file_handle_bytes: 핸들이 쥔 고정 할당(구조체와 경로 사본). 해제 목록은 포함하지 않는다.
 * page_file_list_peak_bytes: 해제 목록 셋(pending·aging·reusable)이 page_count쪽 이하의 페이지를 담을 때
 *   요청 바이트의 순간 최대치 상한. 각 목록은 8에서 시작해 두 배씩 늘며 길이가 page_count를 넘지 않고,
 *   늘 때 옛 배열과 새 배열이 함께 있다.
 */
size_t page_file_handle_bytes(const page_file_t *file);
size_t page_file_list_peak_bytes(uint32_t page_count);
/* page_file_open·page_file_create가 path에 대해 요청하는 바이트의 순간 최대치 상한(핸들과 경로 사본들) */
size_t page_file_open_peak_bytes(const char *path);

/*
 * 메모리 상한을 지키며 여는 경로용. admit는 파일을 열고 잠근 뒤 같은 fd의 fstat 결과(file_bytes)를 얻은 직후,
 * 어떤 힙 할당보다 먼저 한 번 불린다. 경로 이름으로 따로 조회한 값이 아니라 잠근 그 파일의 크기이므로 조회와
 * 열기 사이의 교체·성장이 끼어들지 않는다. false를 돌려주면 fd를 닫고 PAGE_FILE_NOMEM이며 아무것도 할당하지
 * 않았다. NULL이면 page_file_open과 같다. 열린 파일이 제어 페이지가 약속한 길이(page_count 쪽)보다 짧으면
 * 기존처럼 CORRUPT이므로 열린 핸들의 page_count는 file_bytes / PAGE_FILE_PAGE_BYTES 이하다.
 */
typedef bool (*page_file_admit_fn)(void *ctx, uint64_t file_bytes);
page_file_status_t page_file_open_admitted(const char *path, bool writable, page_file_t **out,
                                           page_file_open_info_t *info, page_file_admit_fn admit,
                                           void *ctx);

/*
 * overflow 어댑터. 본문 8160바이트를 한 overflow 페이지로 쓴다. ctx 저장소는 호출자가 소유하며
 * 반환된 io가 쓰이는 동안 살아 있어야 한다. 해제는 page_file_release의 보류 규칙을 따른다.
 */
typedef struct {
    page_file_t *file;
    uint32_t table_id;
    /* 마지막으로 실패한 page_file 호출의 상태. overflow는 실패를 IO 하나로만 알리므로 원인을 남긴다. */
    page_file_status_t last_status;
} page_file_overflow_ctx_t;

overflow_io_t page_file_overflow_io(page_file_overflow_ctx_t *ctx, page_file_t *file,
                                    uint32_t table_id);

#endif
