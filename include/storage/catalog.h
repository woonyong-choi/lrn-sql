#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/value.h"

/*
 * 다중 테이블 카탈로그: 이름과 안정적인 table ID로 테이블별 schema·행 수·identity 카운터·
 * 저장 locator를 보관하는 순수 메타데이터 모듈이다. 페이지를 할당·해제하거나 pager·db 전역
 * header를 읽고 쓰지 않는다. locator는 DB 공통 page allocator를 가진 호출자가 넘기는 값이다.
 * 단일 스레드 계약이며 동시 접근은 호출하는 DB 계층이 직렬화한다.
 *
 * 이 단계는 메타데이터와 직렬화만 다룬다. 실제 행 CRUD, 파일 close/reopen, 원자적 게시,
 * SQL 연결은 table·pager·저장 형식(#77)·#8/#9·#50이 따로 연결해야 한다.
 */

typedef uint32_t catalog_table_id_t;

#define CATALOG_NO_TABLE 0u
#define CATALOG_NO_PAGE 0u
#define CATALOG_NO_COLUMN UINT16_MAX
#define CATALOG_NAME_MAX_BYTES 255u
/* record codec의 필드 수 한계와 같은 값이다. */
#define CATALOG_MAX_COLUMNS 1024u
/* encode 결과와 decode 입력의 최대 바이트. 이를 넘는 카탈로그는 만들 수 없다. */
#define CATALOG_MAX_ENCODED_BYTES (16u * 1024u * 1024u)

typedef enum {
    CATALOG_OK,
    CATALOG_INVALID,      /* 인자·이름·schema 오류 */
    CATALOG_EXISTS,       /* 같은 이름의 테이블이 이미 있음 */
    CATALOG_NOT_FOUND,    /* 없는 table ID */
    CATALOG_LIMIT,        /* ID·카운터·generation·encode 바이트 한계 */
    CATALOG_NOMEM,
    CATALOG_CORRUPT,      /* 잘림·손상·checksum·의미 오류 */
    CATALOG_UNSUPPORTED   /* 알 수 없는 version */
} catalog_status_t;

typedef struct catalog catalog_t;

/*
 * 선언형 컬럼. type은 VALUE_INTEGER·VALUE_REAL·VALUE_TEXT·VALUE_BLOB만 허용하고
 * VALUE_NULL은 거절한다(NULL은 값의 상태이지 컬럼 타입이 아니다). VARCHAR(n)의 n은 여기서
 * 제한이나 절단 기준으로 해석하지 않는다. 길이 제한이 필요하면 별도 명시적 제약으로 추가한다.
 */
typedef struct {
    const char *name;
    size_t name_bytes;
    value_type_t type;
    bool not_null;
} catalog_column_spec_t;

typedef struct {
    const char *name;
    size_t name_bytes;
    const catalog_column_spec_t *columns;
    size_t column_count;
    /* 기본 키 컬럼 번호 또는 CATALOG_NO_COLUMN. 인덱스 root의 컬럼 locator다. */
    uint16_t primary_key_column;
    /* true면 기본 키 컬럼이 INTEGER여야 하며 identity는 1부터 시작한다. */
    bool has_identity;
} catalog_table_spec_t;

/* 이름·컬럼 이름은 NUL 없는 1~CATALOG_NAME_MAX_BYTES 바이트이며 바이트 단위로 비교한다. */
typedef struct {
    const char *name;
    size_t name_bytes;
    value_type_t type;
    bool not_null;
} catalog_column_t;

/* describe 결과. 다음 변경 전까지만 유효한 빌림 값이다. */
typedef struct {
    catalog_table_id_t id;
    const char *name;
    size_t name_bytes;
    const catalog_column_t *columns;
    size_t column_count;
    uint16_t primary_key_column;
    bool has_identity;
    uint64_t row_count;
    /* 다음에 예약할 identity 값. identity가 없으면 0이다. */
    uint64_t next_identity;
    uint32_t heap_root;     /* 설정 전에는 CATALOG_NO_PAGE */
    uint32_t primary_root;  /* 기본 키 인덱스 root, 설정 전에는 CATALOG_NO_PAGE */
} catalog_table_view_t;

/*
 * 명시적 legacy import 입력. 호출자가 옛 단일 테이블 header를 직접 해석해 채운다. 이 모듈은
 * 옛 파일을 자동으로 읽거나 재해석하지 않는다. 옛 VARCHAR 폭은 제한으로 옮기지 않는다.
 */
typedef struct {
    catalog_table_spec_t table;
    uint64_t row_count;
    uint64_t next_identity;  /* has_identity일 때만 의미가 있고 1 이상이어야 한다. */
    uint32_t heap_root;
    uint32_t primary_root;
} catalog_legacy_table_t;

catalog_status_t catalog_open_empty(catalog_t **out);
void catalog_close(catalog_t *catalog);

/*
 * 모든 변경 함수는 실패하면 카탈로그·카운터·출력 인자를 바꾸지 않는다.
 * table ID는 1부터 단조 증가하며 drop 뒤에도, 실패한 create 때문에도 재사용하거나 소비하지 않는다.
 */
catalog_status_t catalog_create_table(catalog_t *catalog, const catalog_table_spec_t *spec,
                                      catalog_table_id_t *id);
catalog_status_t catalog_import_legacy(catalog_t *catalog, const catalog_legacy_table_t *legacy,
                                       catalog_table_id_t *id);
catalog_status_t catalog_drop_table(catalog_t *catalog, catalog_table_id_t id);

catalog_status_t catalog_find(const catalog_t *catalog, const char *name, size_t name_bytes,
                              catalog_table_id_t *id);
catalog_status_t catalog_describe(const catalog_t *catalog, catalog_table_id_t id,
                                  catalog_table_view_t *view);
size_t catalog_table_count(const catalog_t *catalog);
/* index는 ID 오름차순 위치이며 범위를 벗어나면 CATALOG_NOT_FOUND다. */
catalog_status_t catalog_table_at(const catalog_t *catalog, size_t index,
                                  catalog_table_view_t *view);

catalog_status_t catalog_set_row_count(catalog_t *catalog, catalog_table_id_t id,
                                       uint64_t row_count);
catalog_status_t catalog_set_locators(catalog_t *catalog, catalog_table_id_t id,
                                      uint32_t heap_root, uint32_t primary_root);
/* identity가 있는 테이블의 다음 값을 돌려주고 카운터를 올린다. INT64_MAX를 넘으면 CATALOG_LIMIT. */
catalog_status_t catalog_reserve_identity(catalog_t *catalog, catalog_table_id_t id,
                                          uint64_t *value);
/* 성공한 변경마다 1씩 오른다. 게시·충돌 감지에 쓰는 값이다. */
uint64_t catalog_generation(const catalog_t *catalog);

/*
 * 직렬화: little-endian 고정 폭, magic·version·길이·FNV-1a checksum을 포함한다.
 * encode한 bytes는 호출자가 free한다. decode는 잘림·손상·중복·범위 밖 값·과도한 길이·
 * CATALOG_MAX_ENCODED_BYTES 초과를 거절한다. 길이와 개수는 남은 바이트와 대조한 뒤에만 할당에
 * 쓰고, 실패하면 *out을 바꾸지 않는다.
 * 알 수 없는 version은 CATALOG_UNSUPPORTED다.
 */
catalog_status_t catalog_encode(const catalog_t *catalog, uint8_t **bytes, size_t *size);
catalog_status_t catalog_decode(const uint8_t *bytes, size_t size, catalog_t **out);

#endif
