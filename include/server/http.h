/*
 * http.h — HTTP/1.1 최소 서브셋 파서/포매터
 *
 * 지원: POST /query, GET /stats. Content-Length 본문만 받고 Transfer-Encoding은 501
 */

#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* 요청 헤더 블록(요청 줄 포함)과 본문의 최대 바이트 수. 넘으면 실행 없이 거절 */
#define HTTP_MAX_HEADER_BYTES 8192
#define HTTP_MAX_BODY_BYTES   4095

/* chunked 본문의 한도: 청크 크기 줄, 트레일러, 청크 틀까지 포함한 원본 바이트 합 */
#define HTTP_MAX_CHUNK_LINE_BYTES 256
#define HTTP_MAX_TRAILER_BYTES    1024
#define HTTP_MAX_CHUNKED_BYTES    65536

/* 요청 라우트 */
typedef enum {
    ROUTE_UNKNOWN = 0,
    ROUTE_QUERY,     /* POST /query — SQL 실행 */
    ROUTE_STATS      /* GET  /stats — 서버 내부 상태 조회 */
} http_route_t;

/* 요청 파싱 결과. valid=0이면 error_status가 응답할 HTTP 상태(400·404·405·411·413·431·501) */
typedef struct {
    int          valid;          /* 1 = 유효한 요청 */
    int          error_status;
    const char  *allow;          /* 405 응답의 Allow 값 */
    http_route_t route;          /* 요청 라우트 */
    int          keep_alive;     /* 1 = Connection: keep-alive */
    char         body[HTTP_MAX_BODY_BYTES + 1]; /* SQL 본문 (POST /query 전용) */
    size_t       body_len;
} http_request_t;

/* 연결 하나의 수신 상태. 다음 요청의 앞부분이 함께 도착해도 버리지 않고 보존한다 */
typedef struct {
    int    fd;
    size_t len;
    char   buf[HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES + 1];
} http_conn_t;

void http_conn_init(http_conn_t *conn, int client_fd);

/*
 * 완전한 요청 하나를 읽는다. 헤더와 선언한 본문이 모두 도착하기 전에는 반환하지 않는다.
 * 0 = req 채움(valid=0이면 거절 사유가 error_status), -1 = 연결 종료·시간 초과·미완성 요청
 */
int http_conn_read_request(http_conn_t *conn, http_request_t *req);

/* conn 없이 요청 하나만 읽는다. 뒤따르는 바이트는 버린다 */
int http_read_request(int client_fd, http_request_t *req);

/* 임의 상태의 오류 응답을 전송한다. 응답 뒤 연결은 닫는다 */
void http_send_status(int client_fd, int status, const char *allow,
                      const char *body, size_t body_len);

/* HTTP 200 응답을 client_fd에 전송한다 */
void http_send_ok(int client_fd, const char *body, size_t body_len);

/* HTTP 200 응답 (keep-alive 헤더 포함) */
void http_send_ok_keepalive(int client_fd, const char *body, size_t body_len);

/* HTTP 400 응답을 client_fd에 전송한다 */
void http_send_error(int client_fd, const char *body, size_t body_len);

/* HTTP 400 응답 (keep-alive 헤더 포함) */
void http_send_error_keepalive(int client_fd, const char *body, size_t body_len);

#endif /* HTTP_H */
