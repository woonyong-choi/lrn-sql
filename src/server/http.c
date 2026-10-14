/*
 * http.c — HTTP/1.1 최소 서브셋 파서/포매터
 *
 * POST /query 와 GET /stats 만 인식. 요청 줄→헤더→본문이 모두 도착한 뒤에만 요청을 돌려준다.
 */

#define _GNU_SOURCE
#include "server/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <ctype.h>

typedef struct {
    int    has_length;
    size_t content_length;
    int    has_transfer_encoding;
    int    wants_keep_alive;
    int    wants_close;
} headers_t;

static int is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/* 본문 한도를 넘는 값은 숫자 전체를 검증한 뒤 413. 숫자가 아니면 400 */
static int parse_content_length(const char *value, size_t len, size_t *out)
{
    int too_large = 0;
    size_t parsed = 0;
    if (len == 0) return 400;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)value[i])) return 400;
        parsed = parsed * 10 + (size_t)(value[i] - '0');
        if (parsed > HTTP_MAX_BODY_BYTES) {
            too_large = 1;
            parsed = HTTP_MAX_BODY_BYTES + 1;
        }
    }
    *out = parsed;
    return too_large ? 413 : 0;
}

static int token_equals(const char *token, size_t len, const char *word)
{
    return strlen(word) == len && strncasecmp(token, word, len) == 0;
}

static void parse_connection_tokens(const char *value, size_t len, headers_t *h)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && value[i] != ',') i++;
        size_t end = i;
        while (start < end && is_ows(value[start])) start++;
        while (end > start && is_ows(value[end - 1])) end--;
        if (token_equals(value + start, end - start, "keep-alive")) h->wants_keep_alive = 1;
        if (token_equals(value + start, end - start, "close")) h->wants_close = 1;
        i++;
    }
}

static int apply_header(const char *name, size_t name_len,
                        const char *value, size_t value_len, headers_t *h)
{
    if (token_equals(name, name_len, "Content-Length")) {
        size_t length;
        int status = parse_content_length(value, value_len, &length);
        if (status != 0) return status;
        if (h->has_length && h->content_length != length) return 400;
        h->has_length = 1;
        h->content_length = length;
    } else if (token_equals(name, name_len, "Transfer-Encoding")) {
        h->has_transfer_encoding = 1;
    } else if (token_equals(name, name_len, "Connection")) {
        parse_connection_tokens(value, value_len, h);
    }
    return 0;
}

/* 헤더 줄 하나("이름: 값")를 검사하고 반영한다. 형식 오류는 400 */
static int parse_header_line(const char *line, size_t len, headers_t *h)
{
    const char *colon = memchr(line, ':', len);
    if (colon == NULL || colon == line || is_ows(line[0])) return 400;
    for (const char *c = line; c < colon; c++) {
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7f) return 400;
    }
    const char *value = colon + 1;
    const char *end = line + len;
    while (value < end && is_ows(*value)) value++;
    while (end > value && is_ows(end[-1])) end--;
    return apply_header(line, (size_t)(colon - line), value, (size_t)(end - value), h);
}

/* 요청 줄 뒤부터 빈 줄 앞까지의 헤더들. 줄바꿈은 CRLF만 허용한다 */
static int parse_headers(const char *p, const char *end, headers_t *h)
{
    while (p < end) {
        const char *eol = memmem(p, (size_t)(end - p), "\r\n", 2);
        if (eol == NULL) eol = end;
        for (const char *c = p; c < eol; c++) {
            if (*c == '\r' || *c == '\n' || *c == '\0') return 400;
        }
        int status = parse_header_line(p, (size_t)(eol - p), h);
        if (status != 0) return status;
        p = eol + 2;
    }
    return 0;
}

/* 요청 줄로 메서드·경로를 정한다. 쿼리 문자열은 경로 비교에서 제외 */
static int parse_request_line(const char *line, size_t len, http_request_t *req)
{
    const char *sp1 = memchr(line, ' ', len);
    if (sp1 == NULL || sp1 == line) return 400;
    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(line + len - target));
    if (sp2 == NULL || sp2 == target) return 400;
    const char *version = sp2 + 1;
    size_t version_len = (size_t)(line + len - version);
    if (!token_equals(version, version_len, "HTTP/1.1")
        && !token_equals(version, version_len, "HTTP/1.0")) return 400;

    size_t method_len = (size_t)(sp1 - line);
    size_t path_len = (size_t)(sp2 - target);
    const char *query = memchr(target, '?', path_len);
    if (query != NULL) path_len = (size_t)(query - target);

    int is_post = token_equals(line, method_len, "POST");
    int is_get = token_equals(line, method_len, "GET");
    if (path_len == 6 && memcmp(target, "/query", 6) == 0) {
        req->route = ROUTE_QUERY;
        req->allow = "POST";
        return is_post ? 0 : 405;
    }
    if (path_len == 6 && memcmp(target, "/stats", 6) == 0) {
        req->route = ROUTE_STATS;
        req->allow = "GET";
        return is_get ? 0 : 405;
    }
    return 404;
}

/* 본문이 올바른 UTF-8인지(과대 인코딩·서로게이트·범위 초과 제외) 확인한다 */
static int is_valid_utf8(const unsigned char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        size_t extra = c < 0x80 ? 0 : (c >= 0xC2 && c <= 0xDF) ? 1
                     : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 4;
        if (extra == 4 || i + extra >= len + (extra == 0)) return 0;
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        }
        if (c == 0xE0 && s[i + 1] < 0xA0) return 0;
        if (c == 0xED && s[i + 1] >= 0xA0) return 0;
        if (c == 0xF0 && s[i + 1] < 0x90) return 0;
        if (c == 0xF4 && s[i + 1] >= 0x90) return 0;
        i += extra + 1;
    }
    return 1;
}

static void trim_sql_tail(http_request_t *req)
{
    size_t len = req->body_len;
    while (len > 0 && (isspace((unsigned char)req->body[len - 1]) || req->body[len - 1] == ';')) {
        req->body[--len] = '\0';
    }
    req->body_len = len;
}

static int copy_body(const char *body, size_t len, http_request_t *req)
{
    if (memchr(body, '\0', len) != NULL || !is_valid_utf8((const unsigned char *)body, len))
        return 400;
    memcpy(req->body, body, len);
    req->body[len] = '\0';
    req->body_len = len;
    trim_sql_tail(req);
    return 0;
}

static int reject(http_request_t *req, int status)
{
    req->valid = 0;
    req->error_status = status;
    return 0;
}

/* 헤더 블록 [buf, hdr_len)을 해석해 라우트와 본문 길이를 정한다. 오류 상태 또는 0 */
static int parse_head(const char *buf, size_t hdr_len, http_request_t *req, headers_t *h)
{
    const char *line_end = memmem(buf, hdr_len, "\r\n", 2);
    const char *block_end = buf + hdr_len - 4;
    if (line_end == NULL || memchr(buf, '\0', hdr_len) != NULL) return 400;
    if (line_end > block_end) line_end = block_end;
    for (const char *c = buf; c < line_end; c++) {
        if (*c == '\r' || *c == '\n') return 400;
    }
    int status = parse_request_line(buf, (size_t)(line_end - buf), req);
    if (status != 0) return status;
    if (line_end < block_end) status = parse_headers(line_end + 2, block_end, h);
    if (status != 0) return status;
    if (h->has_transfer_encoding) return h->has_length ? 400 : 501;
    if (!h->has_length && req->route == ROUTE_QUERY) return 411;
    return 0;
}

void http_conn_init(http_conn_t *conn, int client_fd)
{
    conn->fd = client_fd;
    conn->len = 0;
    conn->buf[0] = '\0';
}

static int fill_buffer(http_conn_t *conn)
{
    for (;;) {
        ssize_t n = recv(conn->fd, conn->buf + conn->len, sizeof(conn->buf) - 1 - conn->len, 0);
        if (n > 0) {
            conn->len += (size_t)n;
            conn->buf[conn->len] = '\0';
            return 0;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

/* 헤더 끝(CRLF CRLF) 뒤까지의 길이. 아직 없으면 0, 한도 초과면 -1 */
static long header_block_len(const http_conn_t *conn)
{
    const char *end = memmem(conn->buf, conn->len, "\r\n\r\n", 4);
    if (end == NULL) return conn->len >= HTTP_MAX_HEADER_BYTES ? -1 : 0;
    size_t hdr_len = (size_t)(end - conn->buf) + 4;
    return hdr_len > HTTP_MAX_HEADER_BYTES ? -1 : (long)hdr_len;
}

int http_conn_read_request(http_conn_t *conn, http_request_t *req)
{
    memset(req, 0, sizeof(*req));
    long hdr_len = 0;
    while ((hdr_len = header_block_len(conn)) == 0) {
        if (fill_buffer(conn) != 0) return -1;
    }
    if (hdr_len < 0) return reject(req, 431);

    headers_t h = {0};
    int status = parse_head(conn->buf, (size_t)hdr_len, req, &h);
    if (status != 0) return reject(req, status);

    size_t total = (size_t)hdr_len + h.content_length;
    while (conn->len < total) {
        if (fill_buffer(conn) != 0) return -1;
    }
    if (req->route == ROUTE_QUERY && copy_body(conn->buf + hdr_len, h.content_length, req) != 0)
        return reject(req, 400);

    req->keep_alive = h.wants_keep_alive && !h.wants_close;
    req->valid = 1;
    memmove(conn->buf, conn->buf + total, conn->len - total);
    conn->len -= total;
    conn->buf[conn->len] = '\0';
    return 0;
}

int http_read_request(int client_fd, http_request_t *req)
{
    http_conn_t conn;
    http_conn_init(&conn, client_fd);
    return http_conn_read_request(&conn, req);
}

/* ── HTTP 응답 전송 헬퍼 ── */
static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    default:  return "Bad Request";
    }
}

/* 부분 전송과 EINTR를 이어서 보낸다. 실패하면 -1 */
static int send_all(int client_fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(client_fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_response(int client_fd, int status, const char *allow,
                          const char *body, size_t body_len, int keep_alive)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, status_text(status),
        allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "",
        body_len, keep_alive ? "keep-alive" : "close");

    if (send_all(client_fd, header, (size_t)hlen) != 0) return;
    if (body_len > 0) send_all(client_fd, body, body_len);
}

void http_send_status(int client_fd, int status, const char *allow,
                      const char *body, size_t body_len)
{
    send_response(client_fd, status, allow, body, body_len, 0);
}

void http_send_ok(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 200, NULL, body, body_len, 0);
}

void http_send_ok_keepalive(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 200, NULL, body, body_len, 1);
}

void http_send_error(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 400, NULL, body, body_len, 0);
}

void http_send_error_keepalive(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 400, NULL, body, body_len, 1);
}
