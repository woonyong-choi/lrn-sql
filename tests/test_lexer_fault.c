#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql/lexer.h"

static int calls;
static int fail_at;
static long live;

void *lexer_test_malloc(size_t size)
{
    if (++calls == fail_at) return NULL;
    void *memory = malloc(size);
    if (memory != NULL) live++;
    return memory;
}

void lexer_test_free(void *memory)
{
    if (memory != NULL) live--;
    free(memory);
}

static const lexer_limits_t LIMITS = {1u << 16, 1u << 16, 1u << 12, 16};

/*
 * #37: 어느 할당이 실패해도 NOMEM이고 *out과 error를 바꾸지 않으며 새어 나간 할당이 없다.
 * 어휘 오류로 끝난 호출도 임시 할당을 남기지 않는다.
 */
int main(void)
{
    const char sql[] = "select 'it''s', \"q\"\"x\", ?1 from t; select ?2";
    static char marker;
    int failures = 0;
    for (int step = 1; step <= 12; step++) {
        lexed_t *out = (lexed_t *)&marker;
        lexer_error_t error = {LEXER_REASON_UNEXPECTED_BYTE, 77};
        calls = 0;
        fail_at = step;
        lexer_status_t status = lexer_scan((const uint8_t *)sql, sizeof(sql) - 1, &LIMITS, &out, &error);
        fail_at = 0;
        if (status == LEXER_OK) {
            assert(calls < step && out != (lexed_t *)&marker);
            assert(live > 0);
            lexer_free(out);
            break;
        }
        assert(status == LEXER_NOMEM && out == (lexed_t *)&marker);
        assert(error.reason == LEXER_REASON_NONE && error.offset == 0);
        assert(live == 0);
        failures++;
    }
    assert(failures == 5 && live == 0);

    const char *bad[] = {"select 'unterminated", "select ?1, ?3", "a;;", "select 1 /*", "select @"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        lexed_t *out = (lexed_t *)&marker;
        lexer_error_t error;
        assert(lexer_scan((const uint8_t *)bad[i], strlen(bad[i]), &LIMITS, &out, &error) == LEXER_SYNTAX);
        assert(out == (lexed_t *)&marker && live == 0);
    }
    puts("lexer: allocation failures keep outputs; syntax failures leave no allocations");
    return 0;
}
