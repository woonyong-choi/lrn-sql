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

    pthread_kill(server, SIGINT);
    pthread_join(server, NULL);
    unlink(TEST_DB_PATH);

    printf("http input: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
