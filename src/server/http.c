/*
 * http.c — HTTP/1.1 최소 서브셋 파서/포매터
 *
 * POST /query 만 인식. Content-Length 기반 본문 읽기.
 */

#define _GNU_SOURCE
#include "server/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <ctype.h>

/* ── 헤더+본문 한 번에 읽기 ── */
int http_read_request(int client_fd, http_request_t *req)
{
    memset(req, 0, sizeof(*req));

    /* 충분히 큰 버퍼에 recv (최대 8K) */
    char buf[8192];
    ssize_t total = 0;

    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = recv(client_fd, buf + total, (size_t)(sizeof(buf) - 1 - (size_t)total), 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        /* 헤더 종료(\r\n\r\n)를 찾고 body까지 충분히 읽었으면 탈출 */
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            size_t hdr_len = (size_t)(hdr_end - buf) + 4;
            /* Content-Length 파싱 */
            int content_length = 0;
            char *cl = strcasestr(buf, "Content-Length:");
            if (cl) content_length = atoi(cl + 15);
            if ((size_t)total >= hdr_len + (size_t)content_length)
                break;
        }
    }
    if (total <= 0) return -1;

    /* Connection: keep-alive 감지 */
    req->keep_alive = 0;
    char *conn_hdr = strcasestr(buf, "Connection:");
    if (conn_hdr) {
        if (strcasestr(conn_hdr, "keep-alive"))
            req->keep_alive = 1;
    }

    /* 메서드 + 경로 확인 */
    if (strncmp(buf, "GET /stats", 10) == 0) {
        req->valid = 1;
        req->route = ROUTE_STATS;
        return 0;
    }
    if (strncmp(buf, "POST /query", 11) != 0) {
        req->valid = 0;
        return 0;
    }
    req->route = ROUTE_QUERY;

    /* 헤더 끝 찾기 */
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) { req->valid = 0; return 0; }

    char *body_start = hdr_end + 4;
    size_t body_avail = (size_t)(total - (body_start - buf));

    /* Content-Length 파싱 */
    size_t content_length = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl) content_length = (size_t)atoi(cl + 15);

    size_t copy = content_length;
    if (copy > body_avail) copy = body_avail;
    if (copy > sizeof(req->body) - 1) copy = sizeof(req->body) - 1;

    memcpy(req->body, body_start, copy);
    req->body[copy] = '\0';

    /* 후미 공백/세미콜론 제거 */
    while (copy > 0 && (isspace((unsigned char)req->body[copy-1])
                        || req->body[copy-1] == ';')) {
        req->body[--copy] = '\0';
    }

    req->body_len = copy;
    req->valid = 1;
    return 0;
}

/* ── HTTP 응답 전송 헬퍼 ── */
static void send_response(int client_fd, int status, const char *status_text,
                          const char *body, size_t body_len, int keep_alive)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, status_text, body_len,
        keep_alive ? "keep-alive" : "close");

    send(client_fd, header, (size_t)hlen, MSG_NOSIGNAL);
    if (body_len > 0)
        send(client_fd, body, body_len, MSG_NOSIGNAL);
}

void http_send_ok(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 200, "OK", body, body_len, 0);
}

void http_send_ok_keepalive(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 200, "OK", body, body_len, 1);
}

void http_send_error(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 400, "Bad Request", body, body_len, 0);
}

void http_send_error_keepalive(int client_fd, const char *body, size_t body_len)
{
    send_response(client_fd, 400, "Bad Request", body, body_len, 1);
}
