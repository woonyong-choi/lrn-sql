#ifndef ALLOC_AUDIT_H
#define ALLOC_AUDIT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 시험 전용 할당 추적. 라이브러리 소스를 -Dmalloc=audit_malloc -Dcalloc=audit_calloc
 * -Drealloc=audit_realloc -Dfree=audit_free로 빌드하면 그 모듈들의 모든 힙 할당이 여기를 지난다.
 * 요청한 바이트의 합(live)과 최대치(peak), 가장 큰 단일 요청을 센다. 이 값은 "치환해 빌드한 모듈이 실행한
 * 경로"의 요청 바이트일 뿐이며 프로그램 전체의 누수나 한도 준수를 증명하지 않는다. 시험 코드와 libc 내부의
 * 할당은 세지 않는다.
 */
extern size_t audit_live;
extern size_t audit_peak;
extern size_t audit_calls;
extern size_t audit_max_single;
/* 할당이 성공으로 반영된 직후(live가 갱신된 뒤) 불린다 */
extern void (*audit_on_alloc)(void *ctx, size_t request, size_t live);
extern void *audit_ctx;
/* 블록을 실제로 해제한 직후(live가 줄어든 뒤) 불린다. 시험이 스레드를 이 지점에서 멈춰 반납과 다음 reserve 사이를
 * 고정할 수 있다 */
extern void (*audit_on_free)(void *ctx, size_t size, size_t live);
extern void *audit_free_ctx;
/* 여러 스레드가 부르는 할당을 보호한다. live의 일관된 읽기는 이 함수로 한다 */
size_t audit_live_now(void);
/* block이 아직 해제되지 않은 audit 블록인가. 해제한 객체의 주소를 비교용으로만 넘긴다(역참조 없음) */
bool audit_is_live(const void *block);

void audit_reset(void);
void *audit_malloc(size_t size);
void *audit_calloc(size_t count, size_t size);
void *audit_realloc(void *block, size_t size);
void audit_free(void *block);

#endif
