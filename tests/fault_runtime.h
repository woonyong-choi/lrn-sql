#ifndef FAULT_RUNTIME_H
#define FAULT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* #133: 단일 실행 순서의 시험용 지점이다. 제품 거래·WAL 구현을 뜻하지 않는다. */
#define FAULT_MAX_SITES 32u
#define FAULT_NAME_BYTES 48u

typedef struct {
    char name[FAULT_NAME_BYTES];
    uint64_t calls;
} fault_site_t;

typedef struct {
    FILE *trace; /* 호출자가 소유하며 runtime보다 오래 살아 있어야 한다. */
    uint64_t seed;
    uint64_t random_state;
    uint64_t sequence;
    uint64_t target_call;
    size_t site_count;
    int target_site;
    int trace_failed;
    fault_site_t sites[FAULT_MAX_SITES];
} fault_runtime_t;

/* 명시된 문맥은 한 스레드만 사용한다. 병렬 일정의 재현은 별도 barrier가 책임진다. */
void fault_runtime_init(fault_runtime_t *runtime, FILE *trace);
/* 이름은 1..47바이트 ASCII [a-z0-9_.-], 중복·한도 초과는 -1이다. */
int fault_register(fault_runtime_t *runtime, const char *name);
int fault_find(const fault_runtime_t *runtime, const char *name);
/* FAULT_SEED/FAULT_SITE/FAULT_CALL을 읽으며 미등록 지점·잘못된 숫자를 거절한다. */
int fault_configure_env(fault_runtime_t *runtime);
/* 반환 0=진행, 1=지정 실패 주입, -1=검사 장치 오류. trace 오류는 sticky다. */
int fault_probe(fault_runtime_t *runtime, int site, uint64_t object_id);
uint64_t fault_random(fault_runtime_t *runtime);

#endif
