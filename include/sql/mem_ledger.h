#ifndef MEM_LEDGER_H
#define MEM_LEDGER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 메모리 예산 회계의 순수 타입. 이 헤더는 구현도 다른 모듈 의존도 없다. 예산을 쥔 쪽(session 등)이
 * 함수 표를 채우고 database·heap_store·result가 할당하기 전에 reserve, 해제한 뒤 release를 부른다.
 *
 * 단위는 "요청한 바이트"다. malloc 오버헤드·정렬·단편화·프로세스 RSS는 포함하지 않으며 한도가
 * RSS를 보장하지도 않는다. reserve가 true면 그 바이트를 쓸 수 있고, false면 아무 상태도 바뀌지 않았다.
 * reserve는 기다리지 않는다. 재할당은 새 크기를 먼저 reserve한 뒤 할당·복사하고 옛 크기를 release하므로
 * 순간 최대치는 old+new다.
 *
 * scope: QUERY는 문장 하나가 실행되는 동안의 작업 메모리, RESULT는 호출자에게 이전된 결과가
 * release될 때까지 유지하는 메모리, RESIDENT는 핸들이 열려 있는 동안 남는 상주 메모리(카탈로그,
 * page_file 목록 같은 공용 캐시)다. 어느 scope든 같은 바이트를 두 번 세지 않는다.
 *
 * retain·drop은 선택이다. ctx가 이 ledger를 쥔 객체(결과 등)보다 오래 살아야 하면 쥐는 쪽이 retain하고
 * 놓을 때 drop한다. NULL이면 ctx의 수명은 호출자가 보장한다.
 */
typedef enum {
    MEM_SCOPE_QUERY,
    MEM_SCOPE_RESULT,
    MEM_SCOPE_RESIDENT
} mem_scope_t;

typedef struct {
    bool (*reserve)(void *ctx, mem_scope_t scope, size_t bytes);
    void (*release)(void *ctx, mem_scope_t scope, size_t bytes);
    void (*retain)(void *ctx);
    void (*drop)(void *ctx);
} mem_ledger_ops_t;

/* ops가 NULL이면(또는 ledger 자체가 NULL이면) 계상하지 않는 기본이다. */
typedef struct {
    const mem_ledger_ops_t *ops;
    void *ctx;
} mem_ledger_t;

static inline bool mem_ledger_enabled(const mem_ledger_t *ledger)
{
    return ledger != NULL && ledger->ops != NULL;
}

static inline bool mem_reserve(const mem_ledger_t *ledger, mem_scope_t scope, size_t bytes)
{
    if (!mem_ledger_enabled(ledger) || bytes == 0) return true;
    return ledger->ops->reserve(ledger->ctx, scope, bytes);
}

static inline void mem_release(const mem_ledger_t *ledger, mem_scope_t scope, size_t bytes)
{
    if (!mem_ledger_enabled(ledger) || bytes == 0) return;
    ledger->ops->release(ledger->ctx, scope, bytes);
}

static inline void mem_ledger_retain(const mem_ledger_t *ledger)
{
    if (mem_ledger_enabled(ledger) && ledger->ops->retain != NULL) ledger->ops->retain(ledger->ctx);
}

static inline void mem_ledger_drop(const mem_ledger_t *ledger)
{
    if (mem_ledger_enabled(ledger) && ledger->ops->drop != NULL) ledger->ops->drop(ledger->ctx);
}

#endif
