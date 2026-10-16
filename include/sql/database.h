#ifndef DATABASE_H
#define DATABASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sql/lexer.h"
#include "sql/mem_ledger.h"
#include "sql/result.h"

/*
 * 새 저장 형식(page_file) 위에서 CREATE·INSERT·SELECT·UPDATE·DELETE·DROP을 실행하는 데이터베이스.
 * 파일 하나에 테이블 여러 개가 있고 카탈로그(catalog_store)와 테이블별 heap(heap_store)을 연결한다.
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
 * 기본 키: INTEGER와 길이 1024바이트 이하 TEXT 기본 키는 지속형 색인으로 중복 검사와
 * `기본 키 = 값` 조회·갱신·삭제를 전체 스캔 없이 한다. 색인은 힙 변경과 같은 commit으로 게시된다.
 * 색인이 없는 표(색인 도입 전 이미 행이 있던 옛 파일 등)는 전체 스캔으로 검사한다.
 * identity(AUTOINCREMENT)는 카탈로그 카운터가 주며, 자동 값은 늘 기존 최대 키보다 크다(명시한 큰 키가 있으면
 * 카운터를 그 뒤로 한 번에 옮긴다). 최대 키가 INT64_MAX면 더 줄 값이 없어 오류다.
 *
 * 보장하지 않는 것: 여러 문장 거래(database_txn.h가 붙인다), 전원 손실 복구, lost write 탐지, 동시 접근(단일
 * 스레드, 쓰기 핸들은 파일 잠금이 배타), 큰 테이블의 성능(기본 키 등가 밖의 조회는 전체 스캔이고 문장마다 카탈로그를 다시 쓴다), SQLite와 같은
 * 타입 친화성(여기서는 정확히 변환되는 값만 받고 아니면 오류).
 */


typedef enum {
    DATABASE_OK,
    DATABASE_INVALID,
    DATABASE_IO,           /* errno를 남긴다 */
    DATABASE_CORRUPT,
    DATABASE_UNSUPPORTED,  /* 옛 MINIDB 파일, 알 수 없는 version */
    DATABASE_NOMEM,
    DATABASE_EXISTS,       /* create: 파일이 이미 있음 */
    DATABASE_LOCKED,       /* 다른 핸들이 잠금을 쥐고 있음 */
    DATABASE_BUSY          /* close: 해제하지 않은 트랜잭션이 남아 있다. 핸들은 그대로다 */
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

/* 이 핸들이 연 뒤 문장들이 누적한 읽기 비용. 전체 스캔이 얼마나 일어났는지 대조하는 데 쓴다. */
typedef struct {
    uint64_t rows_scanned;      /* 전체 스캔이 돌려준 행 수 */
    uint64_t index_lookups;     /* 기본 키 색인 조회 횟수 */
    uint64_t index_pages_read;  /* 색인 노드 페이지 읽기 횟수 */
} database_scan_stats_t;
void database_scan_stats(const database_t *db, database_scan_stats_t *out);

/*
 * 게시된 상태의 페이지 회계(무결성 검사). 데이터 페이지 [2, page_count)를 하나씩 셋 가운데 하나에 넣는다.
 *   reachable  카탈로그 체인, 테이블의 heap 디렉터리·record·overflow 페이지, 기본 키·보조 색인 노드
 *   free       빈 목록(재사용·대기)과 빈 목록을 담는 사슬 페이지
 *   orphaned   어디서도 닿지 않고 빈 목록에도 없는 페이지(새는 공간)
 * conflicts는 두 번 닿거나 닿으면서 빈 목록에도 있는 페이지 수다(조기 회수나 이중 소유). 정상이면 orphaned와
 * conflicts가 0이다. 쓰기 핸들에서 열린 작업이 없을 때만 부른다(아니면 DATABASE_INVALID). 검사 메모리는 페이지당
 * 1바이트이며 문장 예산 밖이다. 호출자가 다른 호출과 직렬화한다.
 */
typedef struct {
    uint64_t data_pages;
    uint64_t reachable;
    uint64_t free;
    uint64_t orphaned;
    uint64_t conflicts;
} database_page_check_t;
database_status_t database_check_pages(database_t *db, database_page_check_t *out);

/*
 * 문장 하나를 실행한다. binding은 NULL이면 매개변수가 없는 SQL이어야 한다. 성공한 *out은 호출자가
 * result_release한다. sql은 (bytes, size)이며 NUL 종료를 가정하지 않는다.
 */
database_exec_status_t database_exec(database_t *db, const uint8_t *sql, size_t size,
                                     const lexer_binding_t *binding, result_t **out);

/*
 * 메모리 예산과 취소. database_exec_ex는 database_exec와 같고 선택 옵션을 받는다. opts가 NULL이거나 모든
 * 필드가 0이면 database_exec와 같은 동작이다(계상도 제한도 없다).
 *
 * ledger가 있으면 이 문장이 쓰는 모든 메모리를 할당하기 전에 MEM_SCOPE_QUERY로 reserve하고 끝날 때 모두
 * 돌려준다. 이전된 결과의 몫(result_accounted_bytes)만 호출자에게 남으며 result_rebind_ledger로 옮긴다.
 * 요청 바이트 기준이며 allocator 오버헤드·정렬·단편화·프로세스 RSS와 스택은 포함하지 않는다. 한도는
 * RSS를 보장하지 않는다. 계상은 다음과 같다.
 *   - database·heap_store·result가 직접 만드는 할당: 정확한 요청 크기. 재할당은 새 크기를 먼저 reserve해
 *     순간 최대치가 old+new다.
 *   - lexer·parser·카탈로그 복제와 저장·값 변환처럼 다른 모듈 안에서 일어나는 할당: 그 모듈이 입력과 객체
 *     한도로 계산해 주는 요청 바이트의 상한(lexer_scan_peak_bytes, sqlstmt_parse_peak_bytes,
 *     catalog_clone_peak_bytes 등)을 호출 전에 reserve한다. 상한이므로 실제 사용량보다 클 수 있다.
 *   - 입력 SQL은 max_sql_bytes(0이면 DATABASE_BOUNDED_SQL_BYTES)를 넘으면 lexer를 부르기 전에
 *     RESULT_E_LIMIT으로 거절한다. ledger가 없으면 이 제한을 적용하지 않는다(기존 lexer 한도만).
 * reserve가 거절돼 문장을 끝낼 수 없으면 아무것도 반영하지 않고 RESULT_E_NOMEM 오류 결과로 돌아온다.
 * 그 오류 결과조차 reserve할 수 없으면 DATABASE_EXEC_NOMEM이다. ledger가 있는 호출에서는 실제 할당 실패도
 * 같은 방식으로 보고한다.
 *
 * cancel은 협력형이다. 행을 하나 읽기 전과 게시(commit)하기 직전에 확인하고 요청이 보이면 아무것도 반영하지
 * 않고 RESULT_E_CANCELLED 오류 결과로 끝낸다. 게시를 시작한 뒤에는 확인하지 않으며 그 결과(성공, 미반영,
 * 불확정)를 그대로 보고한다. 취소 요청이 게시 뒤에 와도 성공을 취소로 바꾸지 않는다.
 */
#define DATABASE_BOUNDED_SQL_BYTES ((size_t)1 << 20)
/* 취소 오류 결과의 code */
#define DATABASE_CODE_CANCELLED 23
/* 메모리 예산 거절 오류 결과의 code */
#define DATABASE_CODE_BUDGET 24
/* 식 평가 오류 결과의 code(RESULT_E_SEMANTIC). 메시지는 원인 식의 입력 위치를 함께 적는다 */
#define DATABASE_CODE_TYPE 40               /* 형식이 맞지 않는 연산·비교·조건 */
#define DATABASE_CODE_DIVIDE_BY_ZERO 41
#define DATABASE_CODE_ARITHMETIC_OVERFLOW 42
#define DATABASE_CODE_UNKNOWN_FUNCTION 43
#define DATABASE_CODE_FUNCTION_ARGUMENT 44
#define DATABASE_CODE_NOT_CONSTANT 45       /* 상수 자리(INSERT 값·LIMIT·OFFSET)에 열이 나옴 */
#define DATABASE_CODE_AMBIGUOUS_COLUMN 46
#define DATABASE_CODE_BAD_LIMIT 47          /* LIMIT·OFFSET이 음수이거나 정수가 아님 */
#define DATABASE_CODE_BAD_ORDER_POSITION 48 /* ORDER BY 번호가 선택 목록 밖 */
#define DATABASE_CODE_AGGREGATE_MISUSE 49   /* WHERE·GROUP BY·집계 안 등 집계를 쓸 수 없는 자리 */
#define DATABASE_CODE_NOT_GROUPED 50        /* 그룹 질의에서 GROUP BY에 없는 컬럼을 집계 밖에서 씀 */
#define DATABASE_CODE_SET_OPERATION 51      /* 집합 연산 양쪽 열 수가 다르거나 ORDER BY가 결과 열이 아님 */
#define DATABASE_CODE_SUBQUERY 52           /* 서브쿼리 결과 모양이 자리에 맞지 않음, WITH 열 이름 수가 다름 */
#define DATABASE_CODE_CONFLICT 53           /* ON CONFLICT 대상·갱신 열이 맞지 않거나 같은 행을 두 번 바꿈 */

typedef struct {
    bool (*requested)(void *ctx);
    void *ctx;
} database_cancel_t;

typedef struct {
    const mem_ledger_t *ledger;
    const database_cancel_t *cancel;
    size_t max_sql_bytes;
} database_exec_options_t;

database_exec_status_t database_exec_ex(database_t *db, const database_exec_options_t *opts,
                                        const uint8_t *sql, size_t size,
                                        const lexer_binding_t *binding, result_t **out);

/*
 * 상주 메모리. 핸들이 열려 있는 동안 남는 몫(핸들 구조체, page_file 핸들, 게시된 카탈로그, page_file 해제
 * 목록의 상한)을 MEM_SCOPE_RESIDENT로 계상한다. attach가 현재 몫을 reserve하고 거절되면 DATABASE_NOMEM이며
 * 붙지 않는다. 붙은 뒤에는 카탈로그가 커지거나 페이지 수가 늘어 목록 상한이 커질 때 게시 전에 늘어난 만큼을
 * reserve하고(거절되면 그 문장은 반영되지 않는다), 줄어든 카탈로그 몫은 게시 뒤에 돌려준다. 해제 목록 몫은
 * 줄이지 않는다. database_close가 남은 몫을 돌려주고 ledger를 drop한다. 열기(database_open·create)가 쓰는
 * 임시 메모리는 계상하지 않는다. 한 핸들에 한 번만 붙일 수 있으며 ledger가 없으면 아무 일도 하지 않는다.
 * 같은 핸들을 쓰는 exec와 동시에 부르면 안 된다.
 */
database_status_t database_attach_ledger(database_t *db, const mem_ledger_t *resident);
size_t database_resident_bytes(const database_t *db);

/*
 * 상한을 지키는 열기·만들기. 열기가 쓰는 임시 메모리까지 보장하려고 할당보다 먼저 admission을 reserve한다
 * (모두 MEM_SCOPE_RESIDENT). 열기(open)는 경로 이름을 따로 조회하지 않는다. 파일을 열고 잠근 뒤 같은 fd의
 * fstat 크기로 핸들·경로 사본·해제 목록 상한을 reserve한 다음에야 핸들을 할당하고, 제어 페이지가 약속한
 * 카탈로그 크기(CATALOG_MAX_ENCODED_BYTES를 넘으면 할당 없이 DATABASE_CORRUPT)를 안 뒤 읽기 buffer를 할당하기
 * 전에 읽기·복호화 임시와
 * 상주 카탈로그 상한을 reserve한다. 따라서 조회와 열기 사이에 경로의 파일이 교체되거나 자라도 상한은 열린
 * 그 fd의 파일에 대해 계산된다. 사후 보정으로 상한을 보장하지 않는다. 거절되면 DATABASE_NOMEM이고 핸들을
 * 만들지 않았다. 열기나 만들기가 실패하면 reserve한 몫을 모두 돌려준다. 성공하면 임시 몫을 돌려주고 상주
 * 몫만 남기며(database_attach_ledger와 같은 상태) 핸들이 ledger를 쥔다. 실제 상주 몫이 admission을 넘으면
 * 상한 계산의 오류이므로 닫고 DATABASE_INVALID로 거절한다. resident가 NULL이거나 ops가 NULL이면
 * database_open·database_create와 같다. 한계: 파일 잠금은 권고(advisory)라 잠금을 지키지 않는 외부 쓰기가
 * 열기 도중 파일 내용을 바꾸는 경우(예: 카탈로그 체인의 내용)는 막지 못한다. 크기는 제어 페이지의 값을 쓰므로
 * 내용이 바뀌어도 할당 크기는 커지지 않는다. 만들기는 크기에 의존하지 않는 상수 admission을 쓴다.
 */
database_status_t database_open_bounded(const char *path, bool writable, const mem_ledger_t *resident,
                                        database_t **out);
database_status_t database_create_bounded(const char *path, const mem_ledger_t *resident, database_t **out);

#endif
