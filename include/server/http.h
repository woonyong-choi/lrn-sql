/*
 * http.h — HTTP/1.1 최소 서브셋 파서/포매터
 *
 * 지원: POST /query 만 인식, Content-Length 기반 본문 읽기
 */

#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* 요청 파싱 결과 */
typedef struct {
    int   valid;          /* 1 = 유효한 POST /query */
    char  body[4096];     /* SQL 본문 */
    size_t body_len;
} http_request_t;

/* client_fd에서 HTTP 요청을 읽어 파싱한다 */
int http_read_request(int client_fd, http_request_t *req);

/* HTTP 200 응답을 client_fd에 전송한다 */
void http_send_ok(int client_fd, const char *body, size_t body_len);

/* HTTP 400 응답을 client_fd에 전송한다 */
void http_send_error(int client_fd, const char *body, size_t body_len);

#endif /* HTTP_H */
