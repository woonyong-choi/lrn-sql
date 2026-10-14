/*
 * writer 대기 timeout의 실제 소요 시간을 로컬에서 재는 계측 도구. 통과·실패를 판정하지 않고 CI에 넣지 않는다.
 * 빌드: gcc -Wall -Wextra -Werror -O2 -std=c11 -Iinclude -pthread tests/measure_transaction_timeout.c
 *       src/storage/transaction.c -o build/measure_transaction_timeout
 * 실행: build/measure_transaction_timeout [timeout_ms] [반복 횟수]
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "storage/transaction.h"

static txn_status_t be_begin(void *ctx, uint64_t base, void **write)
{
    (void)ctx;
    (void)base;
    *write = NULL;
    return TXN_OK;
}

static txn_publish_result_t be_publish(void *ctx, void *write, uint64_t seq, txn_guarantee_t requested)
{
    (void)ctx;
    (void)write;
    (void)seq;
    (void)requested;
    return (txn_publish_result_t){TXN_OUTCOME_APPLIED, TXN_GUARANTEE_SYNCED};
}

static void be_finish(void *ctx, void *write, txn_outcome_t outcome, uint64_t seq)
{
    (void)ctx;
    (void)write;
    (void)outcome;
    (void)seq;
}

static const txn_backend_ops_t OPS = {be_begin, be_publish, be_finish, NULL, NULL, NULL};

int main(int argc, char **argv)
{
    uint32_t timeout_ms = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 60;
    int rounds = argc > 2 ? atoi(argv[2]) : 20;
    txn_manager_spec_t spec = {.ops = &OPS, .max_guarantee = TXN_GUARANTEE_SYNCED};
    txn_manager_t *manager = NULL;
    txn_t *holder = NULL;

    if (txn_manager_open(&spec, &manager) != TXN_OK || txn_begin(manager, NULL, &holder) != TXN_OK) {
        fputs("setup failed\n", stderr);
        return 1;
    }
    for (int i = 0; i < rounds; i++) {
        txn_begin_options_t options = {.timeout_ms = timeout_ms};
        txn_t *other = NULL;
        struct timespec t0;
        struct timespec t1;
        txn_status_t status;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        status = txn_begin(manager, &options, &other);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("round %d status=%d elapsed_ms=%.3f (requested %u)\n", i, (int)status,
               (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6,
               timeout_ms);
    }
    txn_release(&holder);
    txn_manager_close(&manager);
    return 0;
}
