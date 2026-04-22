/*
 * server.h — TCP 소켓 서버 인터페이스
 *
 * listen → accept 루프, 수신된 client_fd를 스레드 풀에 제출
 */

#ifndef SERVER_H
#define SERVER_H

#include "storage/pager.h"

/* 서버 시작 (블로킹, SIGINT로 종료) */
int server_run(pager_t *pager, int port);

#endif /* SERVER_H */
