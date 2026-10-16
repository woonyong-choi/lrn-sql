#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/value.h"

/*
 * 다중 테이블 카탈로그: 이름과 안정적인 table ID로 테이블별 schema·행 수·identity 카운터·
 * 저장 locator를 보관하는 순수 메타데이터 모듈이다. 페이지를 할당·해제하거나 파일 header를
 * 읽고 쓰지 않는다. locator는 DB 공통 page allocator를 가진 호출자가 넘기는 값이다.
 * 단일 스레드 계약이며 동시 접근은 호출하는 DB 계층이 직렬화한다.
 *
 * 이 모듈은 메타데이터와 직렬화만 다룬다. 행 CRUD는 heap_store, 파일 저장과 원자적 게시는
 * catalog_store·page_file, SQL 연결은 database가 맡는다.
 */

typedef uint32_t catalog_table_id_t;

#define CATALOG_NO_TABLE 0u
#define CATALOG_NO_PAGE 0u
#define CATALOG_NO_COLUMN UINT16_MAX
#define CATALOG_NAME_MAX_BYTES 255u
/* record codec의 필드 수 한계와 같은 값이다. */
#define CATALOG_MAX_COLUMNS 1024u
#define CATALOG_MAX_INDEXES 64u
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
/* 컬럼 기본값(TEXT·BLOB) 크기 한계 */
#define CATALOG_DEFAULT_MAX_BYTES 65536u

/*
 * default_value가 NULL이 아니면 컬럼 기본값이다. 형식은 컬럼 형식과 같아야 하고 NULL 값은 쓰지 않는다
 * (기본값이 없으면 NULL이 기본이다). 기본값이 있는 카탈로그는 형식 버전 3으로 쓴다.
 */
typedef struct {
    const char *name;
    size_t name_bytes;
    value_type_t type;
    bool not_null;
    const value_view_t *default_value;
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
    bool has_default;
    value_view_t default_value;   /* has_default일 때. 바이트는 카탈로그가 소유한다 */
} catalog_column_t;

/* 이름은 catalog가 소유하며 다음 변경 전까지만 유효하다. */
typedef struct {
    const char *name;
    size_t name_bytes;
    catalog_table_id_t table_id;
    uint16_t column;
    bool unique;
    bool ordered;
    uint32_t root;
} catalog_index_view_t;

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

catalog_status_t catalog_open_empty(catalog_t **out);
void catalog_close(catalog_t *catalog);

/*
 * 모든 변경 함수는 실패하면 카탈로그·카운터·출력 인자를 바꾸지 않는다.
 * table ID는 1부터 단조 증가하며 drop 뒤에도, 실패한 create 때문에도 재사용하거나 소비하지 않는다.
 */
catalog_status_t catalog_create_table(catalog_t *catalog, const catalog_table_spec_t *spec,
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
/*
 * 보조 색인을 등록한다. ordered는 비고유 색인 키의 꼬리가 정수 기본 키라는 표시다. 같은 값의 항목이 기본 키 순서로
 * 놓여 WHERE 열 = 값 ORDER BY 기본 키를 색인 순서로 읽을 수 있다. unique와 함께 줄 수 없다(CATALOG_INVALID).
 */
catalog_status_t catalog_add_index(catalog_t *catalog, catalog_table_id_t table_id,
                                   const char *name, size_t name_bytes, uint16_t column,
                                   bool unique, bool ordered);
catalog_status_t catalog_set_index_root(catalog_t *catalog, const char *name, size_t name_bytes,
                                        uint32_t root);
size_t catalog_index_count(const catalog_t *catalog, catalog_table_id_t table_id);
catalog_status_t catalog_index_at(const catalog_t *catalog, catalog_table_id_t table_id,
                                  size_t index, catalog_index_view_t *view);
/* identity가 있는 테이블의 다음 값을 돌려주고 카운터를 올린다. INT64_MAX를 넘으면 CATALOG_LIMIT. */
catalog_status_t catalog_reserve_identity(catalog_t *catalog, catalog_table_id_t id,
                                          uint64_t *value);
/*
 * 다음 identity를 at_least 이상으로 한 번에 올린다(명시한 키 뒤로 옮길 때). 이미 그 이상이면 바꾸지 않는다.
 * at_least는 INT64_MAX + 1까지 받으며 그 값은 더 줄 identity가 없다는 뜻이다. identity 없는 표는 CATALOG_INVALID.
 */
catalog_status_t catalog_raise_identity(catalog_t *catalog, catalog_table_id_t id, uint64_t at_least);
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

/*
 * 메모리 회계용 읽기 전용 접근자. 요청한 바이트 기준이며 allocator 오버헤드는 포함하지 않는다.
 * catalog_memory_bytes: 이 카탈로그가 지금 쥔 할당(구조체, tables 배열 용량, 이름과 컬럼 배열)의 합.
 * catalog_clone_peak_bytes: catalog_encode한 뒤 catalog_decode로 복제하는 동안 요청 바이트의 순간 최대치
 *   상한(부호화 buffer, 복제본, 복호화 임시 배열). 복제 성공 뒤 남는 몫은 복제본의 catalog_memory_bytes다.
 * catalog_create_peak_bytes: 컬럼 column_count개, 이름 바이트 합 names_bytes(테이블 이름 포함)인 테이블을
 *   catalog_create_table로 추가하는 동안의 순간 최대치 상한. tables 배열을 늘릴 때의 old+new를 포함한다.
 */
size_t catalog_memory_bytes(const catalog_t *catalog);
/* 지금 catalog_encode하면 나오는 바이트 수(catalog_store_save의 부호화 buffer 크기) */
size_t catalog_encoded_bytes(const catalog_t *catalog);
/*
 * encoded_bytes 길이의 부호화 buffer를 catalog_decode하는 동안 decode가 요청하는 바이트의 순간 최대치 상한
 * (입력 buffer 제외). 부호화 형식의 최소 테이블·컬럼 크기로 개수를 제한한 값이라 실제보다 크다. 성공한
 * 카탈로그의 catalog_memory_bytes도 이 값 이하다.
 */
size_t catalog_decode_peak_bytes(size_t encoded_bytes);
size_t catalog_clone_peak_bytes(const catalog_t *catalog);
size_t catalog_create_peak_bytes(const catalog_t *catalog, size_t column_count, size_t names_bytes);
size_t catalog_add_index_peak_bytes(const catalog_t *catalog, size_t name_bytes);

#endif
