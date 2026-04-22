/*
 * bench.c — 동시 요청 벤치마크 (TPS 측정)
 *
 * 사용법:
 *   ./build/bench [HOST] [PORT] [THREADS] [REQUESTS_PER_THREAD]
 *
 * 기본값: localhost 8080 4 1000
 *
 * 동작:
 *   1. CREATE TABLE + seed INSERT (서버에 테이블 준비)
 *   2. N 스레드가 각각 M건의 INSERT를 수행
 *   3. 총 소요 시간 + TPS(Transactions Per Second) 출력
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static const char *g_host = "127.0.0.1";
static int g_port = 8080;

/* HTTP POST /query 전송 후 응답 수신 */
static int send_query(const char *sql, char *resp, size_t resp_sz)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    size_t sql_len = strlen(sql);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST /query HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_host, g_port, sql_len);

    send(fd, header, (size_t)hlen, 0);
    send(fd, sql, sql_len, 0);

    ssize_t n = recv(fd, resp, resp_sz - 1, 0);
    if (n > 0) resp[n] = '\0';
    else resp[0] = '\0';

    close(fd);
    return (n > 0 && strstr(resp, "200 OK")) ? 0 : -1;
}

typedef struct {
    int thread_id;
    int requests;
    int success;
    int fail;
} bench_arg_t;

static void *bench_thread(void *arg)
{
    bench_arg_t *a = (bench_arg_t *)arg;
    char sql[256], resp[4096];

    for (int i = 0; i < a->requests; i++) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO bench VALUES ('t%d_r%d', %d)",
                 a->thread_id, i, a->thread_id * 100000 + i);
        if (send_query(sql, resp, sizeof(resp)) == 0)
            a->success++;
        else
            a->fail++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int num_threads = 4;
    int requests_per_thread = 1000;

    if (argc >= 2) g_host = argv[1];
    if (argc >= 3) g_port = atoi(argv[2]);
    if (argc >= 4) num_threads = atoi(argv[3]);
    if (argc >= 5) requests_per_thread = atoi(argv[4]);

    printf("=== MiniDB Benchmark ===\n");
    printf("Target: %s:%d\n", g_host, g_port);
    printf("Threads: %d, Requests/thread: %d, Total: %d\n",
           num_threads, requests_per_thread,
           num_threads * requests_per_thread);

    /* 테이블 준비 */
    char resp[4096];
    if (send_query("CREATE TABLE bench (name VARCHAR(32), val INT)",
                   resp, sizeof(resp)) != 0) {
        /* 이미 존재할 수 있음 → 무시 */
    }

    /* 벤치마크 시작 */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    bench_arg_t *args = malloc(sizeof(bench_arg_t) * (size_t)num_threads);

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].requests = requests_per_thread;
        args[i].success = 0;
        args[i].fail = 0;
        pthread_create(&threads[i], NULL, bench_thread, &args[i]);
    }

    int total_ok = 0, total_fail = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_ok += args[i].success;
        total_fail += args[i].fail;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec)
                   + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    printf("\n=== Results ===\n");
    printf("Elapsed: %.2f sec\n", elapsed);
    printf("Success: %d, Fail: %d\n", total_ok, total_fail);
    printf("TPS: %.1f transactions/sec\n",
           elapsed > 0 ? total_ok / elapsed : 0);

    /* COUNT 확인: "COUNT(*)\n----------\n123\n" 형태 */
    if (send_query("SELECT COUNT(*) FROM bench", resp, sizeof(resp)) == 0) {
        char *body = strstr(resp, "\r\n\r\n");
        if (body) {
            body += 4;
            /* "----------\n" 다음 줄이 숫자 */
            char *sep = strstr(body, "----------\n");
            if (sep) {
                char *num_start = sep + 11; /* strlen("----------\n") */
                printf("Rows in DB: %d\n", atoi(num_start));
            }
        }
    }

    free(threads);
    free(args);
    return 0;
}
