/*
 * consumer.c — C worker. 큐 shipping에서 작업을 claim하고, 출고 기록과 ack를 한 트랜잭션으로 확정한다.
 *
 * 사용: consumer <db 경로> <worker 이름> <lease 밀리초> [--hang-after-claim]
 *   --hang-after-claim: 작업 하나를 claim한 뒤 처리하지 않고 멈춘다. 강제 종료 시연용이다.
 * 큐가 비고 남은 lease도 없으면 끝난다. 처리한 작업마다 "SHIPPED <주문 id> <worker>"를 출력한다.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "totedb.h"

#define RESULT_KIND_ERROR 2
#define RESULT_E_BUSY 7

static void pause_ms(long ms)
{
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

/* 문장 하나를 실행한다. 오류 결과이면 분류를 *error_class에 주고 false다 */
static bool exec_ok(tote_db_t *db, tote_txn_t *txn, const char *sql, const tote_param_t *params, size_t count,
                    uint32_t *error_class)
{
    uint8_t *out = NULL;
    size_t size = 0;
    int status = tote_exec(db, txn, sql, params, count, &out, &size);
    bool ok = status == TOTE_OK && size > 0 && out[0] != RESULT_KIND_ERROR;
    *error_class = 0;
    if (status == TOTE_OK && size >= 5 && out[0] == RESULT_KIND_ERROR) memcpy(error_class, out + 1, 4);
    tote_free(out);
    return ok;
}

/* 남은 READY·LEASED 작업 수. 다른 worker가 쥔 lease가 남아 있으면 끝내지 않고 만료를 기다린다 */
static uint64_t unfinished(tote_queue_t *queue)
{
    uint64_t counts[4] = {0};
    for (;;) {
        int status = tote_queue_stats(queue, NULL, counts);
        if (status == TOTE_OK) return counts[0] + counts[1];
        pause_ms(5);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "사용: %s <db> <worker> <lease_ms> [--hang-after-claim]\n", argv[0]);
        return 2;
    }
    const char *worker = argv[2];
    uint64_t lease_ms = strtoull(argv[3], NULL, 10);
    bool hang = argc > 4 && strcmp(argv[4], "--hang-after-claim") == 0;
    tote_db_t *db = NULL;
    tote_queue_t *queue = NULL;
    if (tote_open(argv[1], 0, &db) != TOTE_OK) return 3;
    int opened;
    while ((opened = tote_queue_open(db, "shipping", 0, NULL, &queue)) == TOTE_BUSY) pause_ms(2);
    if (opened != TOTE_OK) return 4;
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (;;) {
        tote_queue_item_t item;
        int claimed = tote_queue_get(queue, NULL, 0, lease_ms, &item);
        if (claimed == TOTE_BUSY) {
            pause_ms(1);
            continue;
        }
        if (claimed == TOTE_EMPTY) {
            if (unfinished(queue) == 0) break;
            pause_ms(50);  /* 다른 worker의 lease가 끝나기를 기다린다 */
            continue;
        }
        if (claimed != TOTE_OK) return 5;
        long long order_id = strtoll((const char *)item.payload + strlen("order:"), NULL, 10);
        printf("CLAIMED %lld %s fence=%llu\n", order_id, worker, (unsigned long long)item.fence);
        if (hang) {
            for (;;) pause();
        }
        pause_ms(20);  /* 포장·택배 접수 같은 처리 시간을 흉내 낸다 */
        /* 출고 기록과 ack를 한 트랜잭션으로 확정한다. 둘 중 하나만 남는 일이 없다 */
        tote_param_t params[2] = {{TOTE_PARAM_INTEGER, order_id, 0, NULL, 0},
                                  {TOTE_PARAM_TEXT, 0, 0, (const uint8_t *)worker, strlen(worker)}};
        for (;;) {
            tote_txn_t *txn = NULL;
            int begun = tote_begin(db, &txn);
            if (begun == TOTE_BUSY) {
                pause_ms(1);
                continue;
            }
            if (begun != TOTE_OK) return 6;
            uint32_t error_class = 0;
            bool ok = exec_ok(db, txn, "INSERT INTO shipments (order_id, worker) VALUES (?, ?)", params, 2, &error_class);
            int acked = ok ? tote_queue_ack(queue, txn, item.id, item.fence) : TOTE_INVALID;
            if (ok && acked == TOTE_OK && tote_commit(&txn) == TOTE_OK) {
                printf("SHIPPED %lld %s\n", order_id, worker);
                break;
            }
            if (txn != NULL) tote_rollback(&txn);
            if (acked == TOTE_STALE) {
                /* lease가 끝나 다른 worker가 가져갔다. 출고 기록도 함께 버렸다 */
                printf("STALE %lld %s\n", order_id, worker);
                break;
            }
            fprintf(stderr, "처리 실패: 주문 %lld, 오류 분류 %u, ack %s\n", order_id, error_class, tote_status_name(acked));
            return 7;
        }
        tote_queue_item_free(&item);
    }
    tote_queue_close(&queue);
    return tote_close(&db) == TOTE_OK ? 0 : 8;
}
