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
    send_bytes(&c, keep, strlen(keep));
    CHECK(read_response(&c, NULL, 0) == 200, "same connection reused");
    CHECK(server_closed(&c), "Connection: close closes after response");
    close(c.fd);

    make_insert(raw, sizeof(raw), "");
    assert(connect_client(&c) == 0);
    send_bytes(&c, raw, strlen(raw));
    CHECK(read_response(&c, NULL, 0) == 200, "HTTP/1.1 request answered");
    send_bytes(&c, raw, strlen(raw));
    CHECK(read_response(&c, NULL, 0) == 200, "HTTP/1.1 connection persistent by default");
    close(c.fd);

    static const char http10[] = "GET /stats HTTP/1.0\r\n\r\n";
    static const char http10_keep[] = "GET /stats HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    assert(connect_client(&c) == 0);
    send_bytes(&c, http10, sizeof(http10) - 1);
    CHECK(read_response(&c, NULL, 0) == 200, "HTTP/1.0 request answered");
    CHECK(server_closed(&c), "HTTP/1.0 closes without explicit keep-alive");
    close(c.fd);
    assert(connect_client(&c) == 0);
    send_bytes(&c, http10_keep, sizeof(http10_keep) - 1);
    CHECK(read_response(&c, NULL, 0) == 200, "HTTP/1.0 keep-alive answered");
    send_bytes(&c, http10_keep, sizeof(http10_keep) - 1);
    CHECK(read_response(&c, NULL, 0) == 200, "HTTP/1.0 explicit keep-alive persists");
    close(c.fd);

    assert(connect_client(&c) == 0);
    send_bytes(&c, "POST /nope HTTP/1.1\r\nConnection: keep-alive\r\n\r\n", 47);
    CHECK(read_response(&c, NULL, 0) == 404, "unknown path answered");
    CHECK(server_closed(&c), "error closes the connection");
    close(c.fd);
}

#define CHUNKED_HEAD "POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
#define CHUNKED_SQL  "11\r\nINSERT INTO users\r\n" "A;ext=1\r\n VALUES ('\r\n" "9\r\nch', 1)  \r\n"
#define CHUNKED_END  "0\r\nX-Trailer: v\r\n\r\n"

/* 합성 입력 자체가 올바른지 확인할 수 있게 같은 SQL의 CL 요청과 길이를 맞춘다 */
static long chunk_rows(void)
{
    char body[1024];
    static const char q[] = "POST /query HTTP/1.1\r\nContent-Length: 44\r\n\r\n"
                            "SELECT COUNT(*) FROM users WHERE name = 'ch'";
    if (exchange(q, sizeof(q) - 1, body, sizeof(body)) != 200) return -1;
    char *p = strstr(body, "count = ");
    return p ? atol(p + 8) : -1;
}

typedef struct {
    const char *label;
    const char *raw;
    int         status;
} chunk_case_t;

/* #66: 청크 본문이 2·3조각으로 나뉘어 와도 SQL은 한 번, 같은 결과로 실행된다 */
static void http_chunked_body_split_across_segments_runs_once_each(void)
{
    static const char raw[] = CHUNKED_HEAD CHUNKED_SQL CHUNKED_END;
    size_t len = sizeof(raw) - 1;
    long rows = row_count();
    long named = chunk_rows();

    for (size_t cut = 1; cut < len; cut++) {
        client_t c;
        assert(connect_client(&c) == 0);
        send_bytes(&c, raw, cut);
        usleep(1000);
        size_t second = cut + (len - cut) / 2;
        send_bytes(&c, raw + cut, second - cut);
        usleep(1000);
        send_bytes(&c, raw + second, len - second);
        CHECK(read_response(&c, NULL, 0) == 200, "split chunked request answered 200");
        close(c.fd);
    }
    CHECK(row_count() == rows + (long)(len - 1), "each split chunked request ran once");
    CHECK(chunk_rows() == named + (long)(len - 1), "chunked SQL stored the same row");
}

/* #66: 청크 문법 위반·모호한 TE·한도 초과는 실행 없이 거절 */
static void http_invalid_chunked_request_is_rejected_without_execution(void)
{
    static const chunk_case_t cases[] = {
        {"duplicate TE headers",
         "POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400},
        {"TE list with chunked",
         "POST /query HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", 400},
        {"TE unknown coding",
         "POST /query HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n", 501},
        {"TE prefix of chunked",
         "POST /query HTTP/1.1\r\nTransfer-Encoding: chunkedx\r\n\r\n0\r\n\r\n", 501},
        {"TE in HTTP/1.0",
         "POST /query HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400},
        {"TE with CL", "POST /query HTTP/1.1\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400},
        {"non-hex chunk size", CHUNKED_HEAD "g\r\nINSERT\r\n0\r\n\r\n", 400},
        {"empty chunk size", CHUNKED_HEAD "\r\nINSERT\r\n0\r\n\r\n", 400},
        {"signed chunk size", CHUNKED_HEAD "+5\r\nINSERT\r\n0\r\n\r\n", 400},
        {"chunk size with space", CHUNKED_HEAD "6 \r\nINSERT\r\n0\r\n\r\n", 400},
        {"hex size far over limit", CHUNKED_HEAD "FFFFFFFFFFFFFFFFFFFFFFFF\r\nINSERT\r\n0\r\n\r\n", 413},
        {"chunk over body limit", CHUNKED_HEAD "1000\r\nINSERT\r\n0\r\n\r\n", 413},
        {"chunk data longer than size", CHUNKED_HEAD "5\r\nINSERT\r\n0\r\n\r\n", 400},
        {"bare LF framing", CHUNKED_HEAD "6\nINSERT\n0\n\n", 400},
        {"control byte in extension", CHUNKED_HEAD "6;a\x01\r\nINSERT\r\n0\r\n\r\n", 400},
        {"trailer without colon", CHUNKED_HEAD "0\r\nbad trailer\r\n\r\n", 400},
        {"CL in trailer", CHUNKED_HEAD "0\r\nContent-Length: 5\r\n\r\n", 400},
        {"invalid UTF-8 in chunk", CHUNKED_HEAD "2\r\n\xC3(\r\n0\r\n\r\n", 400},
    };
    long rows = row_count();

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(exchange_str(cases[i].raw) == cases[i].status, cases[i].label);

    static const char nul[] = CHUNKED_HEAD "27\r\nINSERT INTO users VALUES ('ch', 1)\0hid\r\n0\r\n\r\n";
    CHECK(exchange(nul, sizeof(nul) - 1, NULL, 0) == 400, "NUL byte in chunk");

    static char big[HTTP_MAX_CHUNKED_BYTES];
    int n = snprintf(big, sizeof(big), CHUNKED_HEAD);
    for (int i = 0; i < 5; i++) {
        n += snprintf(big + n, sizeof(big) - (size_t)n, "3E8\r\n");
        memset(big + n, ' ', 1000);
        n += 1000;
        n += snprintf(big + n, sizeof(big) - (size_t)n, "\r\n");
    }
    n += snprintf(big + n, sizeof(big) - (size_t)n, "0\r\n\r\n");
    CHECK(exchange(big, (size_t)n, NULL, 0) == 413, "decoded body over limit rejected");

    static char trailer[HTTP_MAX_TRAILER_BYTES + 256];
    n = snprintf(trailer, sizeof(trailer), CHUNKED_HEAD "0\r\nX-Pad: ");
    memset(trailer + n, 'a', HTTP_MAX_TRAILER_BYTES);
    strcpy(trailer + n + HTTP_MAX_TRAILER_BYTES, "\r\n\r\n");
    CHECK(exchange_str(trailer) == 431, "oversized trailer rejected");

    CHECK(row_count() == rows, "invalid chunked requests ran no statement");
    CHECK(exchange_str("GET /stats HTTP/1.1\r\nConnection: close\r\n\r\n") == 200,
          "next connection works after rejections");
}

/* #66: 마지막 청크·트레일러가 오기 전에 끊기면 어느 지점에서도 실행하지 않는다 */
static void http_chunked_request_cut_at_any_point_is_not_executed(void)
{
    static const char raw[] = CHUNKED_HEAD CHUNKED_SQL CHUNKED_END;
    long rows = row_count();

    for (size_t cut = 1; cut < sizeof(raw) - 1; cut++) {
        client_t c;
        assert(connect_client(&c) == 0);
        send_bytes(&c, raw, cut);
        shutdown(c.fd, SHUT_WR);
        CHECK(read_response(&c, NULL, 0) != 200, "cut chunked request not answered 200");
        close(c.fd);
    }
    CHECK(row_count() == rows, "cut chunked requests ran no statement");
    CHECK(exchange_str("GET /stats HTTP/1.1\r\nConnection: close\r\n\r\n") == 200,
          "next connection works after cut requests");
}

/* #66: 청크 요청 뒤에 이어 붙은 요청은 경계에서 정확히 나뉘어 각각 실행된다 */
static void http_chunked_request_followed_by_pipelined_requests_runs_each(void)
{
    static const char chunked[] = CHUNKED_HEAD CHUNKED_SQL CHUNKED_END;
    char plain[512], stream[2048];
    make_insert(plain, sizeof(plain), "");
    int n = snprintf(stream, sizeof(stream), "%s%s%s", chunked, plain, chunked);
    long rows = row_count();
    client_t c;
    assert(connect_client(&c) == 0);

    send_bytes(&c, stream, (size_t)n);
    for (int i = 0; i < 3; i++)
        CHECK(read_response(&c, NULL, 0) == 200, "pipelined response after chunked request");

    size_t cut = sizeof(chunked) - 1 + strlen(plain) + 20;
    send_bytes(&c, stream, cut);
    usleep(5000);
    send_bytes(&c, stream + cut, (size_t)n - cut);
    for (int i = 0; i < 3; i++)
        CHECK(read_response(&c, NULL, 0) == 200, "split pipelined response after chunked request");
    close(c.fd);

    CHECK(row_count() == rows + 6, "six pipelined requests ran six times");
}

/* #66: 청크 경계에 걸친 다중 바이트 문자는 이어 붙여 검증한다 */
static void http_chunked_multibyte_split_across_chunks_runs(void)
{
    static const char raw[] = CHUNKED_HEAD "1C\r\nINSERT INTO users VALUES ('\xEA\r\n"
                              "1\r\n\xB0\r\n" "1\r\n\x80\r\n" "5\r\n', 1)\r\n" "0\r\n\r\n";
    long rows = row_count();

    CHECK(exchange(raw, sizeof(raw) - 1, NULL, 0) == 200, "multibyte split across chunks runs");
    CHECK(row_count() == rows + 1, "multibyte chunked request ran once");
}

/* ── #126: HTTP 본문 길이 경계와 거절 뒤 연결 정책 ──
 *
 * 본문 한도는 HTTP_MAX_BODY_BYTES(4095)이며 PARSER_MAX_INPUT_LEN과 같다. 한도 안의 유효한 문장은
 * 실행하고, 한도를 넘거나 나쁜 꼬리가 붙은 문장은 앞부분이 유효한 INSERT·UPDATE·DELETE여도 한 번도
 * 실행하지 않는다. 실행 여부는 users 전체 SELECT 응답 본문의 동일성으로 판정한다.
 * 프레이밍 오류(413)는 연결을 닫고, 한도 안의 SQL 오류(400)는 연결을 유지한다. 두 경우 모두 앞 요청의
 * 남은 바이트가 다음 요청에 섞여 실행되지 않는다.
 */
#define B126_RAW_CAP (2 * HTTP_MAX_BODY_BYTES + 1024)
#define B126_FOLLOW_SQL "UPDATE users SET age = 77 WHERE id = 2"

typedef struct {
    const char *label;
    const char *head;    /* 유효한 문장 전체 */
    const char *before;  /* head를 공백 하나 자리에서 둘로 나눈 앞부분 */
    const char *after;   /* 뒷부분. 공백으로 늘려도 같은 문장이다 */
    const char *effect;  /* 실행되면 SELECT 응답에 나타나거나(appears) 사라지는 문자열 */
    bool        appears;
} b126_kind_t;

static const b126_kind_t B126_KINDS[] = {
    {"INSERT", "INSERT INTO users VALUES ('mallory', 99)",
     "INSERT INTO users VALUES", "('mallory', 99)", "mallory", true},
    {"UPDATE", "UPDATE users SET name = 'hijacked' WHERE id = 1",
     "UPDATE users SET name = 'hijacked'", "WHERE id = 1", "hijacked", true},
    {"DELETE", "DELETE FROM users WHERE id = 1",
     "DELETE FROM users", "WHERE id = 1", "alice", false},
};
#define B126_KIND_COUNT (sizeof(B126_KINDS) / sizeof(B126_KINDS[0]))

static char g_b126_raw[B126_RAW_CAP];
static char g_b126_sql[B126_RAW_CAP];

/* Content-Length를 본문 길이로 선언한 POST를 만들고 전체 바이트 수를 돌려준다 */
static size_t build_post(char *out, size_t cap, const char *body, size_t body_len)
{
    int head = snprintf(out, cap, "POST /query HTTP/1.1\r\nHost: t\r\nContent-Length: %zu\r\n\r\n",
                        body_len);
    assert(head > 0 && (size_t)head + body_len < cap);
    memcpy(out + head, body, body_len);
    return (size_t)head + body_len;
}

/* head 뒤를 공백으로 채워 tail을 포함한 전체를 total 바이트로 맞춘 SQL을 g_b126_sql에 만든다 */
static size_t pad_statement(const char *head, size_t total, const char *tail)
{
    size_t head_len = strlen(head), tail_len = strlen(tail);
    assert(head_len + tail_len <= total && total < sizeof(g_b126_sql));
    memcpy(g_b126_sql, head, head_len);
    memset(g_b126_sql + head_len, ' ', total - head_len - tail_len);
    memcpy(g_b126_sql + total - tail_len, tail, tail_len);
    g_b126_sql[total] = '\0';
    return total;
}

/*
 * before와 after 사이를 공백으로 채워 전체를 total 바이트로 맞춘 SQL을 g_b126_sql에 만든다.
 * HTTP 경로는 본문 끝의 공백·세미콜론을 실행 전에 잘라내므로, 끝 공백으로 채운 문장은 parser에
 * 한도보다 짧게 보인다. 문장 가운데를 채워야 parser가 4094·4095바이트를 그대로 본다.
 */
static size_t pad_inner(const char *before, const char *after, size_t total)
{
    size_t before_len = strlen(before), after_len = strlen(after);
    assert(before_len + after_len + 1 <= total && total < sizeof(g_b126_sql));
    memcpy(g_b126_sql, before, before_len);
    memset(g_b126_sql + before_len, ' ', total - before_len - after_len);
    memcpy(g_b126_sql + total - after_len, after, after_len);
    g_b126_sql[total] = '\0';
    return total;
}

/* sql 바이트를 요청 하나로 보내고 상태를 돌려준다. body가 있으면 응답 본문을 담는다 */
static int post_sql(const char *sql, size_t len, char *body, size_t body_cap)
{
    size_t total = build_post(g_b126_raw, sizeof(g_b126_raw), sql, len);
    return exchange(g_b126_raw, total, body, body_cap);
}

static int post_text(const char *sql)
{
    return post_sql(sql, strlen(sql), NULL, 0);
}

static void snapshot(char *out, size_t cap)
{
    static const char select[] = "SELECT * FROM users";
    int status = post_sql(select, sizeof(select) - 1, out, cap);
    CHECK(status == 200, "snapshot SELECT answered 200");
    if (status != 200) out[0] = '\0';
}

/* users를 alice(1)·bob(2) 두 행으로 되돌린다 */
static void b126_reseed(void)
{
    static const char *const stmts[] = {
        "DROP TABLE users",
        "CREATE TABLE users (name VARCHAR(32), age INT)",
        "INSERT INTO users VALUES ('alice', 20)",
        "INSERT INTO users VALUES ('bob', 30)",
    };
    for (size_t i = 0; i < sizeof(stmts) / sizeof(stmts[0]); i++)
        CHECK(post_text(stmts[i]) == 200, stmts[i]);
}

/* 4094·4095바이트의 유효한 INSERT·UPDATE·DELETE는 parser가 그 길이로 보고도 실행하며 효과가 나타난다 */
static void http_126_statements_inside_limit_run(void)
{
    static char before[RESPONSE_CAP], after[RESPONSE_CAP];
    static const size_t totals[] = {HTTP_MAX_BODY_BYTES - 1, HTTP_MAX_BODY_BYTES};

    for (size_t k = 0; k < B126_KIND_COUNT; k++) {
        for (size_t t = 0; t < sizeof(totals) / sizeof(totals[0]); t++) {
            char label[96];
            snprintf(label, sizeof(label), "%s at %zu bytes", B126_KINDS[k].label, totals[t]);
            b126_reseed();
            snapshot(before, sizeof(before));
            size_t n = pad_inner(B126_KINDS[k].before, B126_KINDS[k].after, totals[t]);
            CHECK(post_sql(g_b126_sql, n, NULL, 0) == 200, label);
            snapshot(after, sizeof(after));
            CHECK(strcmp(before, after) != 0, label);
            CHECK((strstr(after, B126_KINDS[k].effect) != NULL) == B126_KINDS[k].appears, label);
        }
    }
}

/* 4096바이트 이상은 앞부분이 유효해도 413이며 한 번도 실행하지 않는다 */
static void http_126_over_limit_runs_no_statement(void)
{
    static char before[RESPONSE_CAP], after[RESPONSE_CAP];
    static const size_t totals[] = {HTTP_MAX_BODY_BYTES + 1, HTTP_MAX_BODY_BYTES + 2,
                                    HTTP_MAX_BODY_BYTES + 17, 2 * HTTP_MAX_BODY_BYTES + 50};
    static const char *const tails[] = {"", "ZZZ", "; DELETE FROM users"};

    b126_reseed();
    snapshot(before, sizeof(before));
    for (size_t k = 0; k < B126_KIND_COUNT; k++) {
        for (size_t t = 0; t < sizeof(totals) / sizeof(totals[0]); t++) {
            for (size_t x = 0; x < sizeof(tails) / sizeof(tails[0]); x++) {
                char label[128];
                snprintf(label, sizeof(label), "%s over limit at %zu bytes, tail '%s'",
                         B126_KINDS[k].label, totals[t], tails[x]);
                size_t n = pad_statement(B126_KINDS[k].head, totals[t], tails[x]);
                CHECK(post_sql(g_b126_sql, n, NULL, 0) == 413, label);
                snapshot(after, sizeof(after));
                CHECK(strcmp(before, after) == 0, label);
            }
        }
    }
}

/* 한도 안이라도 나쁜 꼬리가 붙은 문장은 400이며 앞부분을 실행하지 않는다 */
static void http_126_bad_tail_inside_limit_runs_no_statement(void)
{
    static char before[RESPONSE_CAP], after[RESPONSE_CAP];
    static const size_t totals[] = {HTTP_MAX_BODY_BYTES - 1, HTTP_MAX_BODY_BYTES};
    static const char *const tails[] = {"ZZZ", "; DELETE FROM users"};

    b126_reseed();
    snapshot(before, sizeof(before));
    for (size_t k = 0; k < B126_KIND_COUNT; k++) {
        for (size_t t = 0; t < sizeof(totals) / sizeof(totals[0]); t++) {
            for (size_t x = 0; x < sizeof(tails) / sizeof(tails[0]); x++) {
                char label[128];
                snprintf(label, sizeof(label), "%s bad tail ending at %zu bytes, tail '%s'",
                         B126_KINDS[k].label, totals[t], tails[x]);
                size_t n = pad_statement(B126_KINDS[k].head, totals[t], tails[x]);
                CHECK(post_sql(g_b126_sql, n, NULL, 0) == 400, label);
                snapshot(after, sizeof(after));
                CHECK(strcmp(before, after) == 0, label);
            }
        }
    }
}

/* 다중 바이트 문자가 본문 한도에 걸쳐도 조각난 값이 실행되거나 저장되지 않는다 */
static void http_126_multibyte_at_limit_is_never_split(void)
{
    static const char hangul[] = "\xEA\xB0\x80"; /* U+AC00, 3바이트 */
    static char before[RESPONSE_CAP], after[RESPONSE_CAP];
    static const char *const head = "UPDATE users SET age = 55 WHERE id = 1";

    b126_reseed();
    snapshot(before, sizeof(before));

    /* 한글이 본문 끝(4094~4098바이트)에 걸린다. 한도 안은 구문 오류 400, 넘으면 413이다 */
    for (size_t end = HTTP_MAX_BODY_BYTES - 1; end <= HTTP_MAX_BODY_BYTES + 3; end++) {
        char label[96];
        snprintf(label, sizeof(label), "hangul tail ending at byte %zu", end);
        size_t n = pad_statement(head, end, hangul);
        CHECK(post_sql(g_b126_sql, n, NULL, 0) == (end <= HTTP_MAX_BODY_BYTES ? 400 : 413), label);
        snapshot(after, sizeof(after));
        CHECK(strcmp(before, after) == 0, label);
    }

    /* 본문이 정확히 한도에서 한글의 첫 1·2바이트로 끝나면 잘못된 UTF-8이라 400이다 */
    static const char *const cut[] = {"\xEA", "\xEA\xB0"};
    for (size_t i = 0; i < sizeof(cut) / sizeof(cut[0]); i++) {
        char label[96];
        snprintf(label, sizeof(label), "multibyte cut after %zu bytes at the limit", i + 1);
        size_t n = pad_statement(head, HTTP_MAX_BODY_BYTES, cut[i]);
        CHECK(post_sql(g_b126_sql, n, NULL, 0) == 400, label);
        snapshot(after, sizeof(after));
        CHECK(strcmp(before, after) == 0, label);
    }

    /* 한도 안의 유효한 다중 바이트 리터럴은 4094·4095바이트에서도 통째로 저장된다 */
    for (size_t total = HTTP_MAX_BODY_BYTES - 1; total <= HTTP_MAX_BODY_BYTES; total++) {
        char label[96];
        snprintf(label, sizeof(label), "valid hangul literal at %zu bytes", total);
        b126_reseed();
        size_t n = pad_inner("UPDATE users SET name = '\xEA\xB0\x80\xEB\x82\x98'", "WHERE id = 1", total);
        CHECK(post_sql(g_b126_sql, n, NULL, 0) == 200, label);
        snapshot(after, sizeof(after));
        CHECK(strstr(after, "\xEA\xB0\x80\xEB\x82\x98") != NULL, label);
    }
}

/* 한도를 넘어 413으로 거절된 연결은 닫히고, 같은 연결의 뒤따르는 정상 문장은 실행되지 않는다 */
static void http_126_over_limit_closes_connection_and_follow_up_is_not_run(void)
{
    static char before[RESPONSE_CAP], after[RESPONSE_CAP];
    char follow[256];

    build_post(follow, sizeof(follow), B126_FOLLOW_SQL, strlen(B126_FOLLOW_SQL));
    for (size_t k = 0; k < B126_KIND_COUNT; k++) {
        char label[96];
        client_t c;

        b126_reseed();
        snapshot(before, sizeof(before));

        /* 응답을 받은 뒤 같은 연결로 정상 문장을 보낸다 */
        snprintf(label, sizeof(label), "%s over limit, follow-up after the response", B126_KINDS[k].label);
        size_t n = pad_statement(B126_KINDS[k].head, HTTP_MAX_BODY_BYTES + 1, "");
        size_t total = build_post(g_b126_raw, sizeof(g_b126_raw), g_b126_sql, n);
        assert(connect_client(&c) == 0);
        send_bytes(&c, g_b126_raw, total);
        CHECK(read_response(&c, NULL, 0) == 413, label);
        CHECK(server_closed(&c), label);
        send_bytes(&c, follow, strlen(follow));
        CHECK(read_response(&c, NULL, 0) != 200, label);
        close(c.fd);
        snapshot(after, sizeof(after));
        CHECK(strcmp(before, after) == 0, label);

        /* 거절된 요청 바로 뒤에 정상 문장을 이어 보내도 섞이거나 실행되지 않는다 */
        snprintf(label, sizeof(label), "%s over limit, follow-up pipelined behind it", B126_KINDS[k].label);
        total = build_post(g_b126_raw, sizeof(g_b126_raw), g_b126_sql, n);
        size_t both = total;
        memcpy(g_b126_raw + both, follow, strlen(follow));
        both += strlen(follow);
        assert(connect_client(&c) == 0);
        send_bytes(&c, g_b126_raw, both);
        CHECK(read_response(&c, NULL, 0) == 413, label);
        CHECK(read_response(&c, NULL, 0) != 200, label);
        close(c.fd);
        snapshot(after, sizeof(after));
        CHECK(strcmp(before, after) == 0, label);

        /* 새 연결의 같은 정상 문장은 정확히 한 번 실행된다 */
        CHECK(post_text(B126_FOLLOW_SQL) == 200, label);
        snapshot(after, sizeof(after));
        CHECK(strstr(after, "77") != NULL && strcmp(before, after) != 0, label);
    }
}

/* 한도 안의 SQL 오류(400)는 연결을 유지하며, 다음 정상 문장은 한 번만 온전히 실행된다 */
static void http_126_sql_error_keeps_connection_and_next_statement_runs_once(void)
{
    static char before[RESPONSE_CAP], after[RESPONSE_CAP];
    char follow[256];

    build_post(follow, sizeof(follow), B126_FOLLOW_SQL, strlen(B126_FOLLOW_SQL));
    for (size_t k = 0; k < B126_KIND_COUNT; k++) {
        char label[96];
        client_t c;

        b126_reseed();
        snapshot(before, sizeof(before));
        size_t n = pad_statement(B126_KINDS[k].head, HTTP_MAX_BODY_BYTES, "ZZZ");
        size_t total = build_post(g_b126_raw, sizeof(g_b126_raw), g_b126_sql, n);

        snprintf(label, sizeof(label), "%s bad tail, follow-up after the response", B126_KINDS[k].label);
        assert(connect_client(&c) == 0);
        send_bytes(&c, g_b126_raw, total);
        CHECK(read_response(&c, NULL, 0) == 400, label);
        send_bytes(&c, follow, strlen(follow));
        CHECK(read_response(&c, NULL, 0) == 200, label);
        close(c.fd);
        snapshot(after, sizeof(after));
        CHECK(strstr(after, "77") != NULL, label);
        CHECK((strstr(after, B126_KINDS[k].effect) != NULL) != B126_KINDS[k].appears, label);

        b126_reseed();
        snprintf(label, sizeof(label), "%s bad tail, follow-up pipelined behind it", B126_KINDS[k].label);
        total = build_post(g_b126_raw, sizeof(g_b126_raw), g_b126_sql, n);
        memcpy(g_b126_raw + total, follow, strlen(follow));
        assert(connect_client(&c) == 0);
        send_bytes(&c, g_b126_raw, total + strlen(follow));
        CHECK(read_response(&c, NULL, 0) == 400, label);
        CHECK(read_response(&c, NULL, 0) == 200, label);
        close(c.fd);
        snapshot(after, sizeof(after));
        CHECK(strstr(after, "77") != NULL, label);
        CHECK((strstr(after, B126_KINDS[k].effect) != NULL) != B126_KINDS[k].appears, label);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    alarm(120); /* 멈추면 시간 초과로 실패 */
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
    http_chunked_body_split_across_segments_runs_once_each();
    http_invalid_chunked_request_is_rejected_without_execution();
    http_chunked_request_cut_at_any_point_is_not_executed();
    http_chunked_request_followed_by_pipelined_requests_runs_each();
    http_chunked_multibyte_split_across_chunks_runs();
    http_126_statements_inside_limit_run();
    http_126_over_limit_runs_no_statement();
    http_126_bad_tail_inside_limit_runs_no_statement();
    http_126_multibyte_at_limit_is_never_split();
    http_126_over_limit_closes_connection_and_follow_up_is_not_run();
    http_126_sql_error_keeps_connection_and_next_statement_runs_once();

    pthread_kill(server, SIGINT);
    pthread_join(server, NULL);
    unlink(TEST_DB_PATH);

    printf("http input: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
