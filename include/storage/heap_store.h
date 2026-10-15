#ifndef HEAP_STORE_H
#define HEAP_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/mem_ledger.h"
#include "storage/page_file.h"
#include "storage/value.h"

/*
 * 테이블 하나의 행 저장소. page_file 위에서 행을 record page(LRP2)에 담고, 큰 TEXT·BLOB은
 * overflow 체인에 둔다. 어떤 페이지가 이 heap의 record page인지는 "페이지 디렉터리"가 알려 준다.
 * catalog의 heap_root는 이 디렉터리의 첫 페이지를 가리킨다(0이면 빈 heap).
 *
 * 위치 locator 구별: heap_root가 가리키는 페이지의 종류를 page_file_page_info로 검증한다.
 *   PAGE_KIND_DIRECTORY + table ID 일치 + magic "LHD1"/"LHD2"/"LHD3"와 대응 version
 *                                                                    -> 이 모듈의 디렉터리
 *   PAGE_KIND_RECORD (#77 legacy_migrate가 만든 단일 record root 포함) 그 밖의 종류
 *                                                                    -> HEAP_UNSUPPORTED_LOCATOR
 * 어떤 경우에도 옛 record root를 디렉터리로 재해석하거나 변환하지 않으며, 변환 함수도 없다.
 *
 * 디렉터리 페이지 본문(little-endian, 8160바이트, 뒤는 0):
 *   0..3   magic "LHD1"
 *   4..5   version u16 = 1
 *   6..7   flags u16 = 0
 *   8..11  table ID u32 (envelope의 table ID와 같아야 한다)
 *   12..15 다음 디렉터리 페이지 u32 (0이면 마지막)
 *   16..19 항목 수 u32 (한 페이지 최대 HEAP_DIRECTORY_ENTRIES)
 *   20..   record page ID u32 배열
 * 항목의 순서가 heap의 행 순서(scan 순서)이며 같은 page ID가 두 번 나오거나 디렉터리 페이지와
 * 겹치면 CORRUPT다. 체인이 순환하거나 page_count를 넘어 길면 CORRUPT다.
 *
 * 옛 LHD1 체인과 LHD2/LHD1 트리는 읽을 수 있다. 첫 변경에서 각 record page의 실제 연속 빈 바이트를
 * 한 번 읽어 새 형식으로 바꾼다. 이후 재시작에서는 저장된 힌트를 읽고 record page 전수를 다시 읽지 않는다.
 * 새 디렉터리 잎은 LHD3(version 3)이며, 20바이트 헤더 뒤에 (record page ID u32, 연속 빈 바이트 u16)
 * 쌍을 HEAP_HINT_DIRECTORY_ENTRIES개까지 저장한다. 힌트는 삽입 전 실제 페이지에서 재검증한다.
 *   LHD3 단일 잎  record page가 HEAP_HINT_DIRECTORY_ENTRIES개 이하일 때 루트 자체가 잎이다.
 *   LHD2 트리       그보다 많으면 루트 한 장 아래 LHD3 잎 여러 장. 한 행을 바꾸면 바뀐 record page가
 *              속한 잎과 루트만 새로 쓰고 나머지 잎은 그대로 가리킨다.
 *     루트 본문: 0..3 magic "LHD2", 4..5 version u16 = 2, 6..7 flags u16 = 0, 8..11 table ID u32,
 *                12..15 예약 u32 = 0, 16..19 잎 수 u32(1..HEAP_DIRECTORY_ENTRIES), 20.. 잎 페이지 ID u32 배열, 뒤는 0.
 *     잎 본문: LHD3 본문에서 다음 페이지가 0이다. 마지막 잎을 뺀 모든 잎은 항목이 가득 차 있고
 *              i번째 잎이 i * HEAP_HINT_DIRECTORY_ENTRIES번째 항목부터 담는다.
 *   LHD2를 모르는 옛 코드는 트리 heap의 루트를 UNSUPPORTED_LOCATOR로 거절한다(변환하지 않는 계약과 같다).
 *
 * 행 형식(record 하나, version 1): 필드 1 + 2n개.
 *   필드 0: INTEGER 1 (행 형식 version)
 *   컬럼마다 (kind, payload): kind는 INTEGER
 *     0  NULL      payload는 NULL
 *     1  INLINE    INTEGER 컬럼은 INTEGER, REAL은 8바이트 little-endian IEEE754 비트 패턴의
 *                  BLOB(-0.0 보존), TEXT는 TEXT, BLOB은 BLOB
 *     2  OVERFLOW  TEXT·BLOB 컬럼만. payload는 BLOB 8바이트(head u32, size u32 little-endian)
 * 인코딩이 HEAP_INLINE_LIMIT_BYTES를 넘으면 16바이트를 넘는 TEXT·BLOB 중 큰 것부터 overflow로
 * 보낸다. 더 보낼 것이 없어도 HEAP_MAX_ROW_BYTES 이하이면 그대로 저장하고 넘으면 HEAP_LIMIT이다.
 * 컬럼은 HEAP_MAX_COLUMNS개까지.
 * 알 수 없는 version·kind, 컬럼 타입과 맞지 않는 payload, 길이가 다른 overflow는 CORRUPT다.
 * 컬럼 타입은 catalog의 컬럼 타입이며 NOT NULL·VARCHAR 같은 제약은 이 모듈이 다루지 않는다.
 *
 * COW와 게시: 이미 commit된 상태에서 도달할 수 있는 페이지는 제자리에서 덮어쓰지 않는다.
 * 변경은 heap_batch_t에 모으고 바뀐 record page와 바뀐 디렉터리 페이지(LHD3은 한 장, LHD2는 루트와 바뀐 잎)를
 * 새로 할당한 페이지에 쓴다. 바뀌지 않은 잎은 옛 페이지를 그대로 쓰므로 해제 목록에 넣지 않는다.
 * 호출자(database)의 순서:
 *   1. heap_batch_begin -> insert/update/delete -> heap_batch_finish (new_root와 해제 목록)
 *   2. catalog_set_locators(new_root) 후 catalog_store_save로 게시
 *   3. 성공이 확실하면 heap_batch_release_old (옛 페이지를 page_file_release)
 *      NOT_APPLIED로 확인된 실패면 heap_batch_abort (이번에 새로 할당한 페이지를 release)
 *      CATALOG_STORE_OUTCOME_UNKNOWN이면 둘 다 하지 않고 heap_batch_free만 한다. 디스크가
 *      어느 쪽을 가리키는지 알 수 없어 옛 페이지도 새 페이지도 재사용하면 안 된다.
 * heap_store는 page_file_commit·catalog 저장을 호출하지 않는다. 옛 페이지를 release하는 시점은
 * 반드시 게시 성공 뒤다. 그 전에 release하면 실패한 게시 뒤의 다른 commit이 아직 도달 가능한
 * 페이지를 재사용 가능으로 만들 수 있다.
 *
 * locator(heap_locator_t)는 한 heap_t 또는 한 batch 안에서만 유효하다. 디렉터리 항목 순번과
 * slot이며 COW로 page 번호가 바뀌어도 같은 batch 안에서는 유지된다. 삭제한 slot 번호는
 * 재사용되지 않는다. update로 행이 옮겨지면 새 locator를 돌려준다. commit 뒤에는 순번이
 * 같아도 다시 open한 heap_t의 locator로만 쓴다.
 *
 * 보장하지 않는 것: WAL·거래 복구, 여러 페이지 갱신의 원자성(게시는 catalog commit 하나에 기대며
 * 그 한계는 page_file.h를 따른다), 재시작 뒤 해제 목록 회수(page_file이 메모리에만 둔다),
 * 삭제된 slot 번호의 재사용, 동시 접근(단일 스레드). 삭제된 행의 데이터 바이트는 같은 record page에
 * 새 행을 넣을 때 재사용한다. 후보 탐색은 메모리 힌트 배열을 훑는 best-fit이며 record page 읽기를 줄이지만
 * 후보 수에 비례하는 CPU 비용은 남는다. 디렉터리 최대 record page 수는
 * HEAP_DIRECTORY_ENTRIES * HEAP_HINT_DIRECTORY_ENTRIES다.
 */

#define HEAP_MAX_COLUMNS 511u
#define HEAP_INLINE_LIMIT_BYTES 4000u
#define HEAP_MAX_ROW_BYTES 8000u
#define HEAP_DIRECTORY_MAGIC "LHD1"
#define HEAP_DIRECTORY_VERSION 1u
#define HEAP_TREE_ROOT_MAGIC "LHD2"
#define HEAP_TREE_ROOT_VERSION 2u
#define HEAP_DIRECTORY_HEADER_BYTES 20u
#define HEAP_DIRECTORY_ENTRIES ((PAGE_BODY_BYTES - HEAP_DIRECTORY_HEADER_BYTES) / 4u)
#define HEAP_HINT_DIRECTORY_MAGIC "LHD3"
#define HEAP_HINT_DIRECTORY_VERSION 3u
#define HEAP_HINT_DIRECTORY_ENTRIES ((PAGE_BODY_BYTES - HEAP_DIRECTORY_HEADER_BYTES) / 6u)

typedef enum {
    HEAP_OK,
    HEAP_INVALID,              /* 인자·컬럼 수·값 타입 오류 */
    HEAP_NOT_FOUND,            /* 없거나 삭제된 locator */
    HEAP_LIMIT,                /* 행이 record page에 담기지 않음, 페이지 번호 한계 */
    HEAP_NOMEM,
    HEAP_IO,                   /* errno를 남긴다. 이미 commit된 파일 내용은 바뀌지 않았다 */
    HEAP_CORRUPT,
    HEAP_UNSUPPORTED_LOCATOR,  /* heap_root가 이 모듈의 디렉터리가 아님 */
    HEAP_BATCH_FAILED,         /* 앞선 연산이 실패한 batch는 abort·free만 가능하다 */
    HEAP_KEY_EXISTS,           /* 키 색인에 같은 키가 이미 있음 */
    HEAP_DONE                  /* cursor가 끝에 도달 */
} heap_status_t;

/* 컬럼 타입 배열은 호출자 소유이며 heap_t가 복사한다. VALUE_NULL은 컬럼 타입이 아니다. */
typedef struct {
    const value_type_t *types;
    size_t column_count;
} heap_schema_t;

typedef struct {
    uint32_t directory_index;
    uint16_t slot;
} heap_locator_t;

typedef struct heap heap_t;
typedef struct heap_batch heap_batch_t;
typedef struct heap_cursor heap_cursor_t;

/* 호출자가 주는 누적 계수기. heap과 여기서 연 cursor·batch가 올린다(단일 스레드). */
typedef struct {
    uint64_t rows_scanned;      /* cursor가 돌려준 행 수 */
    uint64_t index_lookups;     /* 키 색인 조회(find·max) 횟수 */
    uint64_t index_pages_read;  /* 색인 노드 페이지 읽기 횟수 */
} heap_stats_t;

/*
 * 정수 또는 TEXT 키 색인(pk_tree.h, text_tree.h). key_column의 NULL이 아닌 값마다 locator를 색인에 두며, heap batch의 insert·update·
 * delete가 행과 같은 batch 안에서 색인을 함께 바꾼다. 색인 페이지는 행 페이지와 같은 fresh·old 목록에 실려
 * 같은 catalog 게시로 올라가므로 게시된 색인은 항상 게시된 heap의 행 위치를 가리킨다.
 *
 * 색인을 쓰는 조건(heap_is_indexed): key_column이 INTEGER 또는 TEXT 컬럼이고 다음 중 하나다.
 *   - index_root가 있고 root의 stamp가 heap root와 같다(이 heap의 색인이다).
 *   - index_root가 없고 heap_root도 없다(빈 heap. 첫 batch가 root를 만든다).
 * 그 밖에는 색인이 없는 것으로 보고 호출자는 cursor로 스캔한다. 색인 없이 만들어진 heap에 행이 있으면(옛 파일)
 * 색인을 만들지 않으며, 다른 쪽(색인을 모르는 구버전)이 heap만 바꿔 stamp가 어긋난 색인도 쓰지 않는다.
 * key_column이 SIZE_MAX면 색인을 쓰지 않는다.
 */
typedef struct {
    size_t key_column;
    uint32_t index_root;
    heap_stats_t *stats;  /* NULL이면 세지 않는다 */
} heap_index_spec_t;

typedef struct {
    uint16_t column;
    bool unique;
    uint32_t root;
} heap_secondary_spec_t;

/*
 * 커밋된 상태를 연다. 디렉터리 페이지를 모두 읽어 검증한다(record page 자체는 읽을 때 종류와
 * table ID를 검증한다). 실패하면 *out은 NULL이고 파일은 바뀌지 않는다. 호출자는 file을
 * heap_close보다 오래 살려 둔다.
 */
heap_status_t heap_open(page_file_t *file, uint32_t table_id, uint32_t heap_root,
                        const heap_schema_t *schema, heap_t **out);
/*
 * heap_open과 같고 ledger가 있으면 이 heap과 여기서 연 cursor·batch의 모든 할당을 MEM_SCOPE_QUERY로
 * 먼저 reserve한다. ledger는 heap·cursor·batch가 모두 닫힐 때까지 호출자가 살려 둔다. reserve가
 * 거절되면 HEAP_NOMEM이며 그 호출은 아무 상태도 바꾸지 않는다(batch는 failed로 표시된다). NULL이거나
 * ops가 NULL이면 계상하지 않는다. 재할당은 새 크기를 먼저 reserve하므로 순간 최대치는 old+new다.
 *
 * 호출자가 쥐는 몫의 반환:
 *   heap_get·heap_batch_get의 row, cursor가 돌려준 row: heap_release_row가 clear하고 돌려준다.
 *     cursor의 row는 다음 heap_cursor_next·heap_cursor_close에서 이 모듈이 돌려주므로 호출자는
 *     value_clear만 하면 된다(heap_release_row를 부르면 이중 반환이다).
 *   heap_collect_pages의 pages: heap_free_pages가 free하고 돌려준다.
 * 계상하는 것은 이 모듈이 직접 만드는 할당과 overflow·record_page·value가 이 모듈 호출 중에 쓰는
 * 임시 메모리의 상한이다. page_file의 해제 목록은 page_file 소유이며 여기서 계상하지 않는다.
 */
heap_status_t heap_open_ex(page_file_t *file, uint32_t table_id, uint32_t heap_root,
                           const heap_schema_t *schema, const mem_ledger_t *ledger, heap_t **out);
/* spec이 NULL이면 heap_open_ex와 같다. 색인 root 페이지가 형식에 맞지 않으면 HEAP_CORRUPT다. */
heap_status_t heap_open_indexed(page_file_t *file, uint32_t table_id, uint32_t heap_root,
                                const heap_schema_t *schema, const heap_index_spec_t *spec,
                                const mem_ledger_t *ledger, heap_t **out);
/* 이 heap이 키 색인을 쓰는지. false면 호출자가 스캔한다. */
bool heap_is_indexed(const heap_t *heap);
/*
 * 키 색인으로 한 행의 locator를 찾는다. 없으면 HEAP_NOT_FOUND다. 찾은 행을 읽어 키가 같은지 확인하며
 * 어긋나면 HEAP_CORRUPT다(낡은 locator를 돌려주지 않는다). 색인을 쓰지 않으면 HEAP_INVALID다.
 */
heap_status_t heap_find_key(heap_t *heap, int64_t key, heap_locator_t *locator);
heap_status_t heap_find_text_key(heap_t *heap, const uint8_t *key, size_t key_bytes,
                                 heap_locator_t *locator);
bool heap_secondary_key_fits(const value_view_t *value);
heap_status_t heap_secondary_visit_equal(heap_t *heap, heap_secondary_spec_t spec,
                                         const value_view_t *value,
                                         bool (*visit)(void *context, heap_locator_t locator), void *context);
/* 색인에 있는 가장 큰 키. 비어 있으면 *present가 false다. */
heap_status_t heap_max_key(heap_t *heap, bool *present, int64_t *key);
void heap_release_row(const heap_t *heap, value_t *row);
void heap_free_pages(const heap_t *heap, uint32_t *pages, size_t count);
void heap_close(heap_t *heap);
uint32_t heap_root(const heap_t *heap);
size_t heap_record_page_count(const heap_t *heap);

/*
 * 행을 value_t 배열(column_count개, 호출자가 0으로 초기화)에 채운다. 호출자가 value_clear한다.
 * 실패하면 out은 변경하지 않는다.
 */
heap_status_t heap_get(heap_t *heap, heap_locator_t locator, value_t *row);
/* selected는 column_count개의 bool이다. NULL이면 모든 컬럼을 복원한다. */
heap_status_t heap_get_projected(heap_t *heap, heap_locator_t locator, const bool *selected,
                                 value_t *row);

/* 디렉터리 순서·slot 순서로 행을 하나씩 돌려준다. cursor는 열 때의 heap_t 상태를 본다. */
heap_status_t heap_cursor_open(heap_t *heap, heap_cursor_t **out);
/* HEAP_OK면 row(column_count개)와 locator를 채우고, 끝이면 HEAP_DONE이다. row는 호출자가 clear한다. */
heap_status_t heap_cursor_next(heap_cursor_t *cursor, value_t *row, heap_locator_t *locator);
/* 선택하지 않은 overflow 체인은 읽지 않으며, 레코드의 기본 형식은 검사한다. */
heap_status_t heap_cursor_next_projected(heap_cursor_t *cursor, const bool *selected, value_t *row,
                                         heap_locator_t *locator);
void heap_cursor_close(heap_cursor_t *cursor);

/*
 * 테이블을 지울 때 해제해야 할 모든 페이지(디렉터리·record page·overflow 체인)를 모은다.
 * 성공한 *pages는 호출자가 free하며, 게시 성공 뒤에 page_file_release한다.
 */
heap_status_t heap_collect_pages(heap_t *heap, uint32_t **pages, size_t *count);
heap_status_t heap_collect_pages_with_indexes(heap_t *heap, const uint32_t *roots, size_t root_count,
                                              uint32_t **pages, size_t *count);

/* batch. heap_t가 batch보다 오래 살아야 한다. */
heap_status_t heap_batch_begin(heap_t *heap, heap_batch_t **out);
heap_status_t heap_batch_configure_secondary(heap_batch_t *batch, const heap_secondary_spec_t *specs,
                                             size_t count);
heap_status_t heap_batch_secondary_insert(heap_batch_t *batch, const value_view_t *values,
                                          size_t count, heap_locator_t locator);
heap_status_t heap_batch_secondary_remove(heap_batch_t *batch, const value_view_t *values,
                                          size_t count, heap_locator_t locator);
heap_status_t heap_batch_secondary_build(heap_batch_t *batch, size_t index, heap_t *source);
/* 실패한 연산 뒤 batch는 HEAP_BATCH_FAILED를 돌려주고 abort·free만 한다(INVALID·NOT_FOUND 제외). */
heap_status_t heap_batch_insert(heap_batch_t *batch, const value_view_t *values, size_t count,
                                heap_locator_t *locator);
heap_status_t heap_batch_get(heap_batch_t *batch, heap_locator_t locator, value_t *row);
/* 갱신한 행이 옮겨질 수 있어 새 locator를 돌려준다. */
heap_status_t heap_batch_update(heap_batch_t *batch, heap_locator_t locator,
                                const value_view_t *values, size_t count,
                                heap_locator_t *new_locator);
heap_status_t heap_batch_delete(heap_batch_t *batch, heap_locator_t locator);
/*
 * insert·update·delete는 색인을 쓰면 색인도 바꾼다. 새 키가 이미 다른 행의 것이면 HEAP_KEY_EXISTS이며 batch는
 * FAILED다. 호출자는 먼저 heap_find_key로 중복을 거른다. 한 batch 안에서 같은 키를 여러 행에 임시로 겹쳐 쓰는
 * 갱신은 지원하지 않는다(호출자가 최종 결과가 유일한 갱신만 보낸다).
 */

typedef struct {
    uint32_t new_root;           /* 새 디렉터리 루트. record page가 하나도 없으면 0 */
    const uint32_t *old_pages;   /* 게시 성공 뒤 release할 페이지 */
    size_t old_count;
    const uint32_t *new_pages;   /* 이번 batch가 할당해 살아 있는 페이지(색인 페이지 포함) */
    size_t new_count;
    uint32_t new_primary_root;   /* 새 색인 root. 색인을 쓰지 않으면 열 때의 index_root 그대로 */
    const uint32_t *new_secondary_roots; /* batch가 소유하는 array. free 전까지 유효 */
    size_t secondary_count;
} heap_batch_result_t;

/*
 * 바뀐 record page와 바뀐 디렉터리 페이지를 쓴다. 성공하면 result를 채운다(batch가 소유, free 전까지
 * 유효). 바뀐 것이 없어도 새 디렉터리를 쓰지 않고 현재 root를 돌려준다. 실패하면 batch는 FAILED다.
 */
heap_status_t heap_batch_finish(heap_batch_t *batch, heap_batch_result_t *result);
/* 게시 성공 뒤 옛 페이지를 release한다. 모든 페이지를 시도하고 처음 실패 상태를 돌려준다. */
heap_status_t heap_batch_release_old(heap_batch_t *batch);
/* 게시하지 않기로 했을 때 새로 할당한 페이지를 release한다. */
heap_status_t heap_batch_abort(heap_batch_t *batch);
/* 이 batch가 할당했고 아직 놓지 않은 페이지 수. abort가 놓을 개수이며 진단과 검사에 쓴다. */
size_t heap_batch_fresh_count(const heap_batch_t *batch);
void heap_batch_free(heap_batch_t *batch);

/* 마지막으로 실패한 page_file 호출의 상태. HEAP_IO·CORRUPT의 원인을 가를 때 쓴다. */
page_file_status_t heap_last_page_file_status(const heap_t *heap);

#endif
