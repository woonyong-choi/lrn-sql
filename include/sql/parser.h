/*
 * parser.h — SQL 파서 인터페이스
 *
 * SQL 문자열을 분석하여 statement_t 구조체로 변환한다.
 */

#ifndef PARSER_H
#define PARSER_H

#include "statement.h"

/* SQL 문자열을 파싱한다. 성공 시 0, 실패 시 -1 */
int parse(const char *input, statement_t *stmt);

#endif /* PARSER_H */
