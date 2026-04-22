/*
 * server.c — Thread-per-connection TCP 서버
 *
 * 이전 구조: 고정 크기 스레드 풀 + keep-alive → HoL blocking 발생
 * 현재 구조: accept() 시 연결마다 스레드 생성 + pthread_detach
 *           MAX_CONNECTIONS로 동시 연결 수 제한 (MySQL 모델)
 *
 * 변경 이유:
 *   스레드 풀(4 workers)에서 keep-alive 연결 16개가 오면
 *   worker 4개가 fd 4개에 묶여 나머지 12개는 큐에서 대기.
 *   → head-of-line blocking. 자세한 디버깅 과정은
 *   docs/temp/DB/SQL 엔진/db-sql-engine-concurrency-debugging-debrief.md 참조.
 */

#include "server/server.h"
#include "server/http.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── 설정 ── */
#define MAX_CONNECTIONS 128  /* 동시 연결 상한 (MySQL max_connections 역할) */

/* ── 전역 상태 ── */
static volatile sig_atomic_t g_running = 1;

/* 서버 통계 (atomic) */
static _Atomic int     g_active_connections = 0;
static _Atomic uint64_t g_total_connections  = 0;
static _Atomic uint64_t g_total_processed    = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── /stats JSON 응답 생성 ── */
static void handle_stats(pager_t *pager, int client_fd)
{
    int active  = atomic_load(&g_active_connections);
    uint64_t total_conn = atomic_load(&g_total_connections);
    uint64_t total_proc = atomic_load(&g_total_processed);

    /* Row Lock 통계 */
    lock_stats_t ls = db_lock_stats();

    /* Pager 캐시 통계 */
    uint32_t frames_valid = 0, frames_dirty = 0, frames_pinned = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (pager->frames[i].is_valid) {
            frames_valid++;
            if (pager->frames[i].is_dirty) frames_dirty++;
            if (pager->frames[i].pin_count > 0) frames_pinned++;
        }
    }

    char body[2048];
    int len = snprintf(body, sizeof(body),
        "{\n"
        "  \"server\": {\n"
        "    \"model\": \"thread-per-connection\",\n"
        "    \"max_connections\": %d,\n"
        "    \"active_connections\": %d,\n"
        "    \"total_connections\": %lu,\n"
        "    \"total_processed\": %lu\n"
        "  },\n"
        "  \"locks\": {\n"
        "    \"total\": %d,\n"
        "    \"shared\": %d,\n"
        "    \"exclusive\": %d\n"
        "  },\n"
        "  \"pager\": {\n"
        "    \"page_size\": %u,\n"
        "    \"max_frames\": %d,\n"
        "    \"frames_used\": %u,\n"
        "    \"frames_dirty\": %u,\n"
        "    \"frames_pinned\": %u,\n"
        "    \"total_pages\": %u,\n"
        "    \"free_pages\": %u\n"
        "  },\n"
        "  \"db\": {\n"
        "    \"row_count\": %lu,\n"
        "    \"next_id\": %lu\n"
        "  }\n"
        "}\n",
        MAX_CONNECTIONS, active,
        (unsigned long)total_conn,
        (unsigned long)total_proc,
        ls.total, ls.shared, ls.exclusive,
        pager->page_size, MAX_FRAMES,
        frames_valid, frames_dirty, frames_pinned,
        pager->header.next_page_id,
        pager->header.free_page_head,
        (unsigned long)pager->header.row_count,
        (unsigned long)pager->header.next_id);

    http_send_ok(client_fd, body, (size_t)len);
}

/*
 * handle_one_request — 소켓에서 요청 하나를 읽어 처리한다.
 *
 * 반환값:
 *   1  = keep-alive → 같은 소켓에서 다음 요청 대기
 *   0  = close → 이 소켓 종료
 *   -1 = 읽기 실패 (클라이언트가 끊음)
 */
static int handle_one_request(pager_t *pager, int client_fd)
{
    http_request_t req;
    if (http_read_request(client_fd, &req) != 0)
        return -1;  /* 연결 끊김 */

    if (!req.valid) {
        const char *msg = "오류: 잘못된 요청입니다";
        http_send_error(client_fd, msg, strlen(msg));
        return 0;
    }

    int ka = req.keep_alive;

    /* GET /stats 라우트 */
    if (req.route == ROUTE_STATS) {
        handle_stats(pager, client_fd);
        return ka ? 1 : 0;
    }

    exec_result_t res = db_execute(pager, req.body);
    atomic_fetch_add(&g_total_processed, 1);

    /* 응답 본문 조립: out_buf(SELECT 결과) + message */
    char resp[8192];
    size_t off = 0;
    if (res.out_buf && res.out_len > 0) {
        size_t copy = res.out_len < sizeof(resp) - 1 ? res.out_len : sizeof(resp) - 1;
        memcpy(resp, res.out_buf, copy);
        off = copy;
    }
    if (res.message[0] != '\0' && off < sizeof(resp) - 2) {
        int n = snprintf(resp + off, sizeof(resp) - off, "%s%s\n",
                         off > 0 ? "" : "", res.message);
        if (n > 0) {
            size_t avail = sizeof(resp) - off - 1;
            off += ((size_t)n < avail) ? (size_t)n : avail;
        }
    }

    if (res.status == 0) {
        if (ka)
            http_send_ok_keepalive(client_fd, resp, off);
        else
            http_send_ok(client_fd, resp, off);
    } else {
        http_send_error(client_fd, resp, off);
    }

    if (res.out_buf) free(res.out_buf);
    return ka ? 1 : 0;
}

/*
 * handle_client — 연결 하나를 처리한다.
 *
 * keep-alive이면 같은 소켓에서 반복, 아니면 1건 후 종료.
 * 이제 전용 스레드에서 실행되므로 keep-alive가 다른 연결을 차단하지 않는다.
 */
static void handle_client(pager_t *pager, int client_fd)
{
    while (1) {
        int rc = handle_one_request(pager, client_fd);
        if (rc <= 0) break;  /* close 또는 연결 끊김 */
        /* rc == 1: keep-alive -> 루프 계속 */
    }
}

/* ── 연결 핸들러 스레드 ── */
typedef struct {
    pager_t *pager;
    int      client_fd;
} conn_arg_t;

static void *connection_handler(void *arg)
{
    conn_arg_t *ca = (conn_arg_t *)arg;

    handle_client(ca->pager, ca->client_fd);

    close(ca->client_fd);
    atomic_fetch_sub(&g_active_connections, 1);
    free(ca);
    return NULL;
}

/* ── 공개 API ── */
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

    fprintf(stderr, "[server] listening on port %d (thread-per-connection, max=%d)\n",
            port, MAX_CONNECTIONS);

    /* accept 루프 — 연결마다 스레드 생성 */
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

        atomic_fetch_add(&g_total_connections, 1);

        /* 동시 연결 수 제한 */
        if (atomic_load(&g_active_connections) >= MAX_CONNECTIONS) {
            const char *msg = "오류: 최대 연결 수 초과";
            http_send_error(client_fd, msg, strlen(msg));
            close(client_fd);
            continue;
        }

        conn_arg_t *ca = (conn_arg_t *)malloc(sizeof(conn_arg_t));
        ca->pager     = pager;
        ca->client_fd = client_fd;

        atomic_fetch_add(&g_active_connections, 1);

        pthread_t tid;
        if (pthread_create(&tid, NULL, connection_handler, ca) != 0) {
            perror("pthread_create");
            close(client_fd);
            atomic_fetch_sub(&g_active_connections, 1);
            free(ca);
            continue;
        }
        pthread_detach(tid);
    }

    fprintf(stderr, "\n[server] shutting down...\n");

    /* graceful shutdown: 열린 연결이 자연히 끊기기를 잠시 대기 */
    close(server_fd);

    /* Row Lock 테이블 정리 */
    db_destroy();

    /* DB flush */
    pager_flush_all(pager);
    pager_close(pager);

    fprintf(stderr, "[server] done\n");
    return 0;
}
