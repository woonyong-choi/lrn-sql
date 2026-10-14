/*
 * test_http_input.c -- 이슈 #66: 불완전하거나 모호한 HTTP 요청은 SQL을 실행하지 않는다
 *
 * 실제 TCP 연결로 server_run()에 합성 요청을 보내고, 행 수로 DML 실행 횟수를 센다.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "db.h"
#include "server/http.h"
#include "server/server.h"
#include "storage/pager.h"

#define TEST_DB_PATH "__test_http_input.db"
#define RESPONSE_CAP 16384

static int g_checks = 0;
static int g_failed = 0;
static int g_port;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failed++; \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

typedef struct {
    int    fd;
    char   buf[RESPONSE_CAP];
    size_t len;
} client_t;

static pager_t g_pager;

static void *server_thread(void *arg)
{
    (void)arg;
    server_run(&g_pager, g_port);
    return NULL;
}

static int connect_client(client_t *c)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int attempt = 0; attempt < 100; attempt++) {
        c->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            int on = 1;
            struct timeval tv = {.tv_sec = 2};
            setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            c->len = 0;
            return 0;
        }
        close(c->fd);
        usleep(20000);
    }
    return -1;
}

static void send_bytes(client_t *c, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c->fd, data + sent, len - sent, 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

/* 응답 하나를 읽어 상태 코드를 반환한다. 연결이 끊겼거나 시간 초과면 0 */
static int read_response(client_t *c, char *body, size_t body_cap)
{
    for (;;) {
        char *end = memmem(c->buf, c->len, "\r\n\r\n", 4);
        if (end != NULL) {
            size_t head_len = (size_t)(end - c->buf) + 4;
            char *cl = strcasestr(c->buf, "Content-Length:");
            size_t body_len = cl ? (size_t)atol(cl + 15) : 0;
            if (c->len >= head_len + body_len) {
                int status = atoi(c->buf + 9);
                size_t copy = body_len < body_cap - 1 ? body_len : body_cap - 1;
                if (body) { memcpy(body, c->buf + head_len, copy); body[copy] = '\0'; }
                memmove(c->buf, c->buf + head_len + body_len, c->len - head_len - body_len);
                c->len -= head_len + body_len;
                return status;
            }
        }
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len - 1, 0);
        if (n <= 0) return 0;
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
    }
}

/* 응답을 모두 읽은 뒤 서버가 연결을 닫았는지 확인한다 */
static int server_closed(client_t *c)
{
    char byte;
    return recv(c->fd, &byte, 1, 0) == 0;
}

static void format_post(char *out, size_t cap, const char *body, size_t body_len, const char *extra)
{
    int head = snprintf(out, cap, "POST /query HTTP/1.1\r\nHost: t\r\n%sContent-Length: %zu\r\n\r\n",
                        extra, body_len);
    memcpy(out + head, body, body_len);
    out[head + (int)body_len] = '\0';
}

/* 요청 바이트를 보내고 응답 상태를 돌려준다. 보낸 뒤 연결은 닫는다 */
static int exchange(const char *raw, size_t len, char *body, size_t body_cap)
{
    client_t c;
    if (connect_client(&c) != 0) return -1;
    send_bytes(&c, raw, len);
    int status = read_response(&c, body, body_cap);
    close(c.fd);
    return status;
}

static int exchange_str(const char *raw)
{
    return exchange(raw, strlen(raw), NULL, 0);
}

static long row_count(void)
{
    char sql[256], body[1024];
    snprintf(sql, sizeof(sql), "POST /query HTTP/1.1\r\nContent-Length: 26\r\n\r\nSELECT COUNT(*) FROM users");
    if (exchange(sql, strlen(sql), body, sizeof(body)) != 200) return -1;
    char *p = strstr(body, "count = ");
    return p ? atol(p + 8) : -1;
}

#define INSERT_SQL "INSERT INTO users VALUES ('u', 1)"

static void make_insert(char *out, size_t cap, const char *extra)
{
    format_post(out, cap, INSERT_SQL, strlen(INSERT_SQL), extra);
}

/* #66: 응답 전 요청이 모든 바이트 경계에서 나뉘어 와도 실행은 정확히 1회 */
static void http_request_split_at_every_byte_runs_once_each(void)
{
    char raw[512];
    make_insert(raw, sizeof(raw), "");
    size_t len = strlen(raw);
    long before = row_count();

    for (size_t cut = 1; cut < len; cut++) {
        client_t c;
        assert(connect_client(&c) == 0);
        send_bytes(&c, raw, cut);
        usleep(1500);
        send_bytes(&c, raw + cut, len - cut);
        CHECK(read_response(&c, NULL, 0) == 200, "split request answered 200");
        close(c.fd);
    }
    CHECK(row_count() == before + (long)(len - 1), "each split request ran exactly once");
}

typedef struct {
    const char *label;
    const char *raw;
    int         status;
} reject_case_t;

/* #66: 모호하거나 잘못된 framing·경로·메서드는 실행 없이 거절 */
static void http_malformed_request_is_rejected_without_execution(void)
{
    static const reject_case_t cases[] = {
        {"CL and TE conflict",
         "POST /query HTTP/1.1\r\nContent-Length: 33\r\nTransfer-Encoding: chunked\r\n\r\n" INSERT_SQL, 400},
        {"TE chunked unsupported",
         "POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 501},
        {"differing duplicate CL",
         "POST /query HTTP/1.1\r\nContent-Length: 33\r\nContent-Length: 5\r\n\r\n" INSERT_SQL, 400},
        {"negative CL",
         "POST /query HTTP/1.1\r\nContent-Length: -33\r\n\r\n" INSERT_SQL, 400},
        {"signed CL",
         "POST /query HTTP/1.1\r\nContent-Length: +33\r\n\r\n" INSERT_SQL, 400},
        {"CL with junk",
         "POST /query HTTP/1.1\r\nContent-Length: 33x\r\n\r\n" INSERT_SQL, 400},
        {"CL overflows integer",
         "POST /query HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n" INSERT_SQL, 413},
        {"missing CL, CL text in body",
         "POST /query HTTP/1.1\r\n\r\nContent-Length: 33 " INSERT_SQL, 411},
        {"query prefix path",
         "POST /queryfoo HTTP/1.1\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 404},
        {"query suffix path",
         "POST /query/x HTTP/1.1\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 404},
        {"case-different path",
         "POST /Query HTTP/1.1\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 404},
        {"wrong method on query",
         "GET /query HTTP/1.1\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 405},
        {"wrong method on stats",
         "POST /stats HTTP/1.1\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 405},
        {"stats prefix path", "GET /statsx HTTP/1.1\r\n\r\n", 404},
        {"space before header colon",
         "POST /query HTTP/1.1\r\nContent-Length : 33\r\n\r\n" INSERT_SQL, 400},
        {"bad request line", "POST/query HTTP/1.1\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 400},
        {"unknown version",
         "POST /query HTTP/9.9\r\nContent-Length: 33\r\n\r\n" INSERT_SQL, 400},
        {"overlong UTF-8 body",
         "POST /query HTTP/1.1\r\nContent-Length: 35\r\n\r\n" INSERT_SQL "\xC0\x80", 400},
        {"surrogate UTF-8 body",
         "POST /query HTTP/1.1\r\nContent-Length: 36\r\n\r\n" INSERT_SQL "\xED\xA0\x80", 400},
        {"invalid UTF-8 body",
         "POST /query HTTP/1.1\r\nContent-Length: 34\r\n\r\n" INSERT_SQL "\xC3", 400},
    };
    long before = row_count();

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *raw = cases[i].raw;
        CHECK(exchange_str(raw) == cases[i].status, cases[i].label);
    }
    static const char nul_body[] = "POST /query HTTP/1.1\r\nContent-Length: 37\r\n\r\n" INSERT_SQL "\0hid";
    CHECK(exchange(nul_body, sizeof(nul_body) - 1, NULL, 0) == 400, "NUL byte in body");
    CHECK(row_count() == before, "rejected requests ran no statement");
}

/* #66: 선언한 길이보다 짧게 끊긴 요청은 실행하지 않고, 서버는 다음 연결을 받는다 */
static void http_truncated_body_at_eof_is_not_executed(void)
{
    char raw[512];
    make_insert(raw, sizeof(raw), "");
    long before = row_count();

    for (size_t drop = 1; drop < strlen(INSERT_SQL); drop += 7) {
        client_t c;
        assert(connect_client(&c) == 0);
        send_bytes(&c, raw, strlen(raw) - drop);
        shutdown(c.fd, SHUT_WR);
        CHECK(read_response(&c, NULL, 0) != 200, "truncated body not answered 200");
        close(c.fd);
    }
    CHECK(row_count() == before, "truncated bodies ran no statement");
}

/* #66: 한 번에 온 두 요청은 각각 한 번씩 실행하고 응답을 순서대로 보낸다 */
static void http_two_requests_in_one_segment_run_once_each(void)
{
    char one[512], two[1100];
    make_insert(one, sizeof(one), "Connection: keep-alive\r\n");
    snprintf(two, sizeof(two), "%s%s", one, one);
    long before = row_count();
    client_t c;
    assert(connect_client(&c) == 0);

    send_bytes(&c, two, strlen(two));
    CHECK(read_response(&c, NULL, 0) == 200, "first pipelined response");
    CHECK(read_response(&c, NULL, 0) == 200, "second pipelined response");

    size_t cut = strlen(one) + strlen(one) / 2;
    send_bytes(&c, two, cut);
    usleep(5000);
    send_bytes(&c, two + cut, strlen(two) - cut);
    CHECK(read_response(&c, NULL, 0) == 200, "merged-then-split first response");
    CHECK(read_response(&c, NULL, 0) == 200, "merged-then-split second response");
    close(c.fd);

    CHECK(row_count() == before + 4, "four pipelined requests ran four times");
}

/* #66: 본문 크기·헤더 크기 한도 */
static void http_oversized_input_is_rejected_without_execution(void)
{
    static char big[HTTP_MAX_BODY_BYTES + 200];
    static char raw[HTTP_MAX_BODY_BYTES + 400];
    long before = row_count();

    memset(big, ' ', sizeof(big) - 1);
    memcpy(big, INSERT_SQL, strlen(INSERT_SQL));
    format_post(raw, sizeof(raw), big, HTTP_MAX_BODY_BYTES, "");
    CHECK(exchange_str(raw) == 200, "body at the limit runs");

    format_post(raw, sizeof(raw), big, HTTP_MAX_BODY_BYTES + 1, "");
    CHECK(exchange_str(raw) == 413, "body over the limit rejected");

    static char huge[HTTP_MAX_HEADER_BYTES + 512];
    int n = snprintf(huge, sizeof(huge), "POST /query HTTP/1.1\r\nX-Pad: ");
    memset(huge + n, 'a', HTTP_MAX_HEADER_BYTES);
    strcpy(huge + n + HTTP_MAX_HEADER_BYTES, "\r\nContent-Length: 33\r\n\r\n" INSERT_SQL);
    CHECK(exchange_str(huge) == 431, "oversized header rejected");

    const char hangul[] = "POST /query HTTP/1.1\r\nContent-Length: 35\r\n\r\n"
                          "INSERT INTO users VALUES ('\xEA\xB0\x80', 1)";
    CHECK(exchange_str(hangul) == 200, "valid multibyte body runs");

    CHECK(row_count() == before + 2, "only the in-limit and valid bodies ran");
}

/* #66: 본문 안의 헤더 모양 문자열은 헤더가 아니며, 연결 재사용 정책이 응답과 일치한다 */
static void http_connection_reuse_follows_headers_and_errors(void)
{
    char body_sql[] = "INSERT INTO users VALUES ('Content-Length: 5', 1)";
    char raw[512], keep[512];
    format_post(raw, sizeof(raw), body_sql, strlen(body_sql), "Connection: keep-alive\r\n");
    format_post(keep, sizeof(keep), "SELECT COUNT(*) FROM users", 26, "Connection: close\r\n");
    client_t c;
    assert(connect_client(&c) == 0);

    send_bytes(&c, raw, strlen(raw));
    CHECK(read_response(&c, NULL, 0) == 200, "keep-alive request answered");
    CHECK(strcasestr(raw, "keep-alive") != NULL, "request asked for keep-alive");
    send_bytes(&c, keep, strlen(keep));
    CHECK(read_response(&c, NULL, 0) == 200, "same connection reused");
    CHECK(server_closed(&c), "Connection: close closes after response");
    close(c.fd);

    make_insert(raw, sizeof(raw), "");
    assert(connect_client(&c) == 0);
    send_bytes(&c, raw, strlen(raw));
    CHECK(read_response(&c, NULL, 0) == 200, "plain request answered");
    CHECK(server_closed(&c), "no keep-alive header closes after response");
    close(c.fd);

    assert(connect_client(&c) == 0);
    send_bytes(&c, "POST /nope HTTP/1.1\r\nConnection: keep-alive\r\n\r\n", 47);
    CHECK(read_response(&c, NULL, 0) == 404, "unknown path answered");
    CHECK(server_closed(&c), "error closes the connection");
    close(c.fd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    g_port = 20000 + (int)(getpid() % 20000);
    unlink(TEST_DB_PATH);
    assert(pager_open(&g_pager, TEST_DB_PATH, true) == 0);

    pthread_t server;
    pthread_create(&server, NULL, server_thread, NULL);
    char create[] = "POST /query HTTP/1.1\r\nContent-Length: 46\r\n\r\n"
                    "CREATE TABLE users (name VARCHAR(32), age INT)";
    CHECK(exchange_str(create) == 200, "create table over http");

    http_request_split_at_every_byte_runs_once_each();
    http_malformed_request_is_rejected_without_execution();
    http_truncated_body_at_eof_is_not_executed();
    http_two_requests_in_one_segment_run_once_each();
    http_oversized_input_is_rejected_without_execution();
    http_connection_reuse_follows_headers_and_errors();

    pthread_kill(server, SIGINT);
    pthread_join(server, NULL);
    unlink(TEST_DB_PATH);

    printf("http input: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
