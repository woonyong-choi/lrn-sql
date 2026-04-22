/*
 * server.c — TCP 소켓 서버
 *
 * Main thread: socket → bind → listen → accept loop
 * 수신된 client_fd를 스레드 풀에 제출한다.
 * SIGINT(Ctrl+C)로 graceful shutdown.
 */

#include "server/server.h"
#include "server/thread_pool.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

int server_run(pager_t *pager, int port)
{
    /* SIGINT 핸들러 등록 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* 소켓 생성 */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    /* Row Lock 테이블 초기화 */
    db_init();

    /* 스레드 풀 초기화 (코어 수 기반) */
    thread_pool_t pool;
    if (thread_pool_init(&pool, pager, 0, 0) != 0) {
        close(server_fd);
        return -1;
    }

    fprintf(stderr, "[server] listening on port %d\n", port);

    /* accept 루프 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  /* SIGINT */
            perror("accept");
            continue;
        }

        if (thread_pool_submit(&pool, client_fd) != 0) {
            close(client_fd);
        }
    }

    fprintf(stderr, "\n[server] shutting down...\n");

    /* graceful shutdown */
    thread_pool_destroy(&pool);
    close(server_fd);

    /* Row Lock 테이블 정리 */
    db_destroy();

    /* DB flush */
    pager_flush_all(pager);
    pager_close(pager);

    fprintf(stderr, "[server] done\n");
    return 0;
}
