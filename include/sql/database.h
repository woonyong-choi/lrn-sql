#ifndef DATABASE_H
#define DATABASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/lexer.h"
#include "sql/result.h"

/*
 * 새 저장 형식(page_file) 위에서 CREATE·INSERT·SELECT·UPDATE·DELETE·DROP을 실행하는 데이터베이스.
 * 파일 하나에 테이블 여러 개가 있고 카탈로그(catalog_store)와 테이블별 heap(heap_store)을 연결한다.
 * 기존 pager·table·bptree·executor·main과 legacy parser는 쓰지도 바꾸지도 않는다.
 * 옛 MINIDB 파일이나 #77 단일 record root는 열지 않으며 자동으로 옮기지도 않는다.
 *
 * 실행 단위: database_exec 한 번은 SQL 문장 하나이며 문장마다 자동 commit이다. 문장이 둘 이상이면
 * 실행하지 않고 SYNTAX 오류 결과를 돌려준다. 매개변수는 binding으로 받고 SQL 문자열에 이어 붙이지 않는다.
 *
 * 결과(result.h): 문장 수준의 실패는 호출이 성공하고 RESULT_KIND_ERROR 결과로 돌아온다.
 *   SELECT -> RESULT_KIND_ROWS, 그 밖의 성공 -> RESULT_KIND_COMMAND(tag·영향 행 수)
 *   문법·어휘 오류 -> RESULT_E_SYNTAX, 이름·타입·미지원 문법 -> RESULT_E_SEMANTIC,
 *   NOT NULL·기본 키 중복 -> RESULT_E_CONSTRAINT, 한도 -> RESULT_E_LIMIT,
 *   저장소 I/O -> RESULT_E_IO, 손상·알 수 없는 저장 형식 -> RESULT_E_INTERNAL
 * 할당 실패는 결과를 만들 수 없으므로 DATABASE_EXEC_NOMEM이고 *out은 NULL이다. 이때 문장이 이미
 * 반영됐는지는 database_is_poisoned로 보는 불확정 경우를 제외하면 "반영되지 않음"이다.
 * 성공 결과는 commit보다 먼저 만들어 두므로, 결과 생성 실패 때문에 반영된 문장이 NOMEM으로
 * 보고되는 일은 없다.
 *
 * 실패와 보존: 문장이 실패하면 이미 게시된 카탈로그와 모든 테이블의 행은 그대로다. 변경은 새 페이지에만
 * 쓰고 마지막에 카탈로그 한 번을 commit하는 COW이다. 게시 결과가 불확정(제어 쓰기를 시도한 뒤
 * pwrite·fsync 실패)이면 이 핸들은 poison되어 이후 문장을 모두 RESULT_E_IO로 거절하고, 호출한
 * 문장도 성공으로 응답하지 않는다. 다시 열면 이전 상태일 수도 새 상태일 수도 있다.
 * commit 전에 실패했다고 확인된 경우에도 fsync가 실패했다면 핸들이 poison될 수 있다.
 *
 * 기본 키: 기본 키 중복은 첫 범위 스캔으로 검사한다. 실제 index는 구현하지 않았다(primary_root는
 * 항상 비어 있다). identity(AUTOINCREMENT)는 카탈로그 카운터가 주며, 명시한 값이 카운터보다 크면
 * 이후 자동 값은 그 값을 넘을 때까지 카운터를 올려 건너뛴다(한도 DATABASE_IDENTITY_SKIP_LIMIT).
 *
 * 보장하지 않는 것: WAL, 여러 문장 거래, 전원 손실 복구, lost write 탐지, 동시 접근(단일 스레드,
 * 쓰기 핸들은 파일 잠금이 배타), 재시작 뒤 해제 페이지 회수(page_file이 메모리에만 둔다),
 * 큰 테이블의 성능(모든 조회가 전체 스캔이고 문장마다 카탈로그를 다시 쓴다), SQLite와 같은
 * 타입 친화성(여기서는 정확히 변환되는 값만 받고 아니면 오류).
 */

#define DATABASE_IDENTITY_SKIP_LIMIT 1048576u

typedef enum {
    DATABASE_OK,
    DATABASE_INVALID,
    DATABASE_IO,           /* errno를 남긴다 */
    DATABASE_CORRUPT,
    DATABASE_UNSUPPORTED,  /* 옛 MINIDB 파일, 알 수 없는 version */
    DATABASE_NOMEM,
    DATABASE_EXISTS,       /* create: 파일이 이미 있음 */
    DATABASE_LOCKED        /* 다른 핸들이 잠금을 쥐고 있음 */
} database_status_t;

typedef enum {
    DATABASE_EXEC_OK,       /* *out에 결과가 있다(오류 결과 포함) */
    DATABASE_EXEC_INVALID,  /* 인자 오류. *out은 바뀌지 않는다 */
    DATABASE_EXEC_NOMEM     /* 결과를 만들지 못했다. *out은 NULL */
} database_exec_status_t;

typedef struct database database_t;

/* 새 파일을 만들어 빈 카탈로그를 commit하고 게시한다. 목적지가 있으면 DATABASE_EXISTS다. */
database_status_t database_create(const char *path, database_t **out);
/* 게시된 파일을 연다. writable이 아니면 SELECT만 실행된다. 열기는 파일을 바꾸지 않는다. */
database_status_t database_open(const char *path, bool writable, database_t **out);
/* 핸들을 닫는다. poison된 핸들도 닫을 수 있다. close(2) 실패는 DATABASE_IO다. */
database_status_t database_close(database_t *db);

/* 불확정 commit 결과나 fsync 실패로 더는 쓸 수 없는 핸들인지. */
bool database_is_poisoned(const database_t *db);
size_t database_table_count(const database_t *db);

/*
 * 문장 하나를 실행한다. binding은 NULL이면 매개변수가 없는 SQL이어야 한다. 성공한 *out은 호출자가
 * result_release한다. sql은 (bytes, size)이며 NUL 종료를 가정하지 않는다.
 */
database_exec_status_t database_exec(database_t *db, const uint8_t *sql, size_t size,
                                     const lexer_binding_t *binding, result_t **out);

#endif
