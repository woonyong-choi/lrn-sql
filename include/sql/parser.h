/*
 * parser.h — SQL 파서 인터페이스
 *
 * SQL 문자열을 분석하여 statement_t 구조체로 변환한다.
 */

#ifndef PARSER_H
#define PARSER_H

#include "statement.h"

/* 파서가 받는 SQL 한 문장의 최대 바이트 수(NUL 제외). 넘으면 실행 없이 실패 */
#define PARSER_MAX_INPUT_LEN 4095

/* SQL 문자열을 전체 검증하며 파싱한다. 성공 시 0, 실패 시 -1 (길이 초과·필드 초과 포함) */
int parse(const char *input, statement_t *stmt);

#endif /* PARSER_H */
