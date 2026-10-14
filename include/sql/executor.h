/*
 * executor.h — SQL 실행기 인터페이스
 *
 * 플래너가 결정한 접근 경로에 따라 실제 데이터 조작을 수행한다.
 */

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "statement.h"
#include "planner.h"
#include "../storage/pager.h"
#include <stddef.h>

/* 실행 결과 */
typedef struct {
    int   status;        /* 실행 상태 (0 = 성공, -1 = 오류) */
    char  message[512];  /* 결과 메시지 (사용자에게 출력) */
    char *out_buf;       /* SELECT/EXPLAIN 결과 본문 (동적 할당, 호출자가 free) */
    size_t out_len;      /* out_buf에 기록된 바이트 수 */
} exec_result_t;

/* SQL 문을 실행하고 결과를 반환한다 */
exec_result_t execute(pager_t *pager, statement_t *stmt);

#ifdef MINIDB_TEST_HOOKS
/*
 * 출력 버퍼 할당 실패 주입 (시험 빌드 전용). 함수를 부르지 않은 프로세스는 환경 변수
 * MINIDB_TEST_FAIL_OUT_MALLOC·MINIDB_TEST_FAIL_OUT_REALLOC(프로세스 시작부터 센 n번째)을 따른다.
 * 함수로 지정·해제할 때는 한 스레드에서 문장 사이에만 부른다.
 * malloc_at/realloc_at번째(1부터) 출력 버퍼 malloc/realloc을 실패시킨다. 0이면 끈다.
 * 호출 시 malloc·realloc 호출 횟수를 0으로 되돌린다.
 */
void executor_test_fail_out_alloc(long malloc_at, long realloc_at);
/* 출력 버퍼 malloc·realloc 호출 수, 아직 해제도 이전도 안 된 버퍼 수, 호출자에게 넘긴 버퍼 수 */
void executor_test_out_stats(long *mallocs, long *reallocs, long *live, long *transferred);
#endif

#endif /* EXECUTOR_H */
