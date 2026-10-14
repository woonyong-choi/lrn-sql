#ifndef CATALOG_STORE_H
#define CATALOG_STORE_H

#include "storage/catalog.h"
#include "storage/page_file.h"

/*
 * catalog(#6)를 page_file(#77)에 저장하고 다시 읽는다. catalog_encode 결과를 table ID 0의
 * overflow 체인에 쓰고, 제어 페이지의 카탈로그 참조(head, size, generation)로 가리킨다.
 *
 * 참조 순서 계약:
 *   1. 새 체인을 쓴다 (옛 체인은 건드리지 않는다)
 *   2. 제어 페이지를 commit해 새 체인을 가리키게 한다
 *   3. 그 commit이 성공한 뒤에야 옛 체인을 release한다
 * release한 페이지는 page_file이 이후 commit이 두 번 성공하기 전에는 재사용하지 않는다. 따라서
 * 가장 최근 제어 페이지를 잃어 한 세대 전으로 열려도 그 세대의 체인은 덮어쓰이지 않았다.
 * 이 계약은 WAL·복구를 뜻하지 않는다. 쓰기가 사라진 경우의 한계는 page_file.h를 따른다.
 */

typedef enum {
    CATALOG_STORE_OK,
    CATALOG_STORE_EMPTY,        /* 저장된 카탈로그가 없다 */
    CATALOG_STORE_INVALID,
    CATALOG_STORE_STALE,        /* 저장된 것보다 낮은 generation의 카탈로그 */
    CATALOG_STORE_IO,           /* errno를 남긴다. 제어 쓰기를 시도하기 전의 실패라 저장은 반영되지 않았다 */
    CATALOG_STORE_CORRUPT,
    CATALOG_STORE_UNSUPPORTED,
    CATALOG_STORE_NOMEM,
    CATALOG_STORE_LIMIT,
    /* 저장은 성공했으나 옛 체인을 회수하지 못해 페이지가 샌다. */
    CATALOG_STORE_UNRECLAIMED,
    /*
     * 제어 쓰기를 시도한 뒤 pwrite나 fsync가 실패했다. 다시 열면 이전 카탈로그가 보일 수도, 새
     * 카탈로그가 보일 수도 있으며 호출자는 어느 쪽인지 알 수 없다. 핸들은 poison되어 더는 쓸 수 없고,
     * 새 체인과 옛 체인 모두 release하지 않았다. 이 결과를 성공으로 응답하거나 "반영되지 않았다"고
     * 말하면 안 된다. errno를 남긴다.
     */
    CATALOG_STORE_OUTCOME_UNKNOWN
} catalog_store_status_t;

/*
 * 카탈로그를 저장하고 commit한다. 제어 쓰기를 시도하기 전에 실패하면(IO·NOMEM·LIMIT 등) 제어
 * 페이지는 이전 세대를 가리키고 이전 카탈로그는 그대로 읽히며, 새 체인은 해제한다. 제어 쓰기를
 * 시도한 뒤의 실패는 CATALOG_STORE_OUTCOME_UNKNOWN이며 어느 세대가 남았는지 알 수 없다.
 * poison된 핸들은 아무것도 쓰지 않고 IO로 거절한다.
 * catalog_generation(catalog)가 저장된 값보다 작으면 STALE이다.
 */
catalog_store_status_t catalog_store_save(page_file_t *file, const catalog_t *catalog);
/* 현재 제어 페이지가 가리키는 카탈로그를 읽는다. 성공한 *out은 호출자가 catalog_close한다. */
catalog_store_status_t catalog_store_load(page_file_t *file, catalog_t **out);

#endif
