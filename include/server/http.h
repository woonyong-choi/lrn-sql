/*
 * http.h — HTTP/1.1 최소 서브셋 파서/포매터
 *
 * 지원: POST /query 만 인식, Content-Length 기반 본문 읽기
 */

#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* 요청 라우트 */
typedef enum {
    ROUTE_UNKNOWN = 0,
    ROUTE_QUERY,     /* POST /query — SQL 실행 */
    ROUTE_STATS      /* GET  /stats — 서버 내부 상태 조회 */
} http_route_t;

/* 요청 파싱 결과 */
typedef struct {
    int          valid;          /* 1 = 유효한 요청 */
    http_route_t route;          /* 요청 라우트 */
    int          keep_alive;     /* 1 = Connection: keep-alive */
    char         body[4096];     /* SQL 본문 (POST /query 전용) */
    size_t       body_len;
} http_request_t;

/* client_fd에서 HTTP 요청을 읽어 파싱한다 */
int http_read_request(int client_fd, http_request_t *req);

/* HTTP 200 응답을 client_fd에 전송한다 */
void http_send_ok(int client_fd, const char *body, size_t body_len);

/* HTTP 200 응답 (keep-alive 헤더 포함) */
void http_send_ok_keepalive(int client_fd, const char *body, size_t body_len);

/* HTTP 400 응답을 client_fd에 전송한다 */
void http_send_error(int client_fd, const char *body, size_t body_len);

#endif /* HTTP_H */
