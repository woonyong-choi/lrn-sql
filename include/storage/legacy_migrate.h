#ifndef LEGACY_MIGRATE_H
#define LEGACY_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

#include "storage/page_file.h"

/*
 * 옛 MINIDB v1/v2 단일 테이블 파일을 새 page_file 형식으로 옮기는 명시적 전환 API.
 * 열기 경로에서 옛 파일을 자동으로 읽거나 in-place로 바꾸지 않는다. 전환은 호출자가 이 함수를
 * 직접 부를 때만 일어나며 원본은 읽기 전용으로만 연다(O_RDONLY, 공유 잠금, 쓰기·삭제 없음).
 *
 * 두 단계로 나뉜다.
 *   legacy_export: 원본을 읽어 검증하고 메모리의 export 값으로 만든다. 새 파일을 만들지 않는다.
 *   legacy_import: export 값으로 새 파일을 만든다. 목적지가 이미 있으면 덮어쓰지 않는다.
 *
 * 변환 규칙: id 시스템 컬럼 → INTEGER 기본 키(identity), INT·BIGINT → INTEGER, VARCHAR(N) →
 * TEXT(폭은 제한으로 옮기지 않는다). 첫 NUL까지를 값으로 보며 유효한 UTF-8이 아니면 변환하지
 * 않고 거절한다. 인덱스는 옮기지 않는다(primary_root 없음).
 *
 * 현재 약속 범위: 행은 레코드 페이지 하나에 모두 들어가야 한다(카탈로그 heap_root가 첫 레코드
 * 페이지 하나만 가리킨다). 넘으면 일부를 버리지 않고 LEGACY_LIMIT로 거절한다. 연결 체인과 다중
 * 페이지 heap CRUD는 #50 범위다. 이 API는 DB 전체 재시작·거래 복구를 뜻하지 않는다.
 */

typedef enum {
    LEGACY_OK,
    LEGACY_INVALID,
    LEGACY_IO,               /* errno를 남긴다 */
    LEGACY_CORRUPT,          /* 헤더·힙 체인·슬롯·행 수의 일관성 오류, 잘린 파일 */
    LEGACY_UNSUPPORTED,      /* MINIDB v1/v2가 아니거나 지원하지 않는 페이지 크기·컬럼 */
    LEGACY_UNCONVERTIBLE,    /* 새 형식으로 정확히 옮길 수 없는 값 (잘못된 UTF-8, 중복 id 등) */
    LEGACY_LIMIT,            /* 행이 레코드 페이지 하나에 들어가지 않음 */
    LEGACY_EXISTS,           /* 목적지가 이미 있음 */
    LEGACY_LOCKED,           /* 원본을 다른 쓰기 핸들이 쥐고 있음 */
    LEGACY_NOMEM,
    LEGACY_PUBLISHED_DIRTY   /* 새 파일은 게시됐으나 정리나 이름 fsync가 실패함 */
} legacy_status_t;

typedef struct legacy_export legacy_export_t;

/*
 * v1 파일에는 테이블 이름이 없으므로 v1_table_name을 호출자가 명시해야 한다(NULL이면 v1은
 * LEGACY_UNSUPPORTED). v2에서는 헤더의 이름을 쓰며 인자가 있으면 같아야 한다.
 */
legacy_status_t legacy_export(const char *path, const char *v1_table_name, legacy_export_t **out);
void legacy_export_free(legacy_export_t *data);
const char *legacy_export_table_name(const legacy_export_t *data);
uint64_t legacy_export_row_count(const legacy_export_t *data);
uint64_t legacy_export_next_id(const legacy_export_t *data);

/*
 * 새 파일을 만든다. 실패하면 목적지는 만들어지지 않고 partial도 남지 않는다. 열기에 성공하는 파일은
 * 제어 페이지 commit과 게시까지 끝난 것뿐이다. 반환 전에 쓴 내용을 다시 읽어 export와 대조한다.
 */
legacy_status_t legacy_import(const legacy_export_t *data, const char *destination);

/* export + import. 원본은 어떤 경우에도 바꾸지 않는다. */
legacy_status_t legacy_migrate(const char *source, const char *destination,
                               const char *v1_table_name);

#endif
