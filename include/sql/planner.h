/*
 * planner.h — 규칙 기반 쿼리 플래너 인터페이스
 *
 * SQL 문의 유형과 조건을 분석하여 접근 경로(access path)를 결정한다.
 */

#ifndef PLANNER_H
#define PLANNER_H

#include "statement.h"

/* 접근 경로: 실행기가 어떤 방식으로 데이터에 접근할지 결정한다 */
typedef enum {
    ACCESS_PATH_TABLE_SCAN,    /* 힙 전체 스캔 */
    ACCESS_PATH_INDEX_LOOKUP,  /* B+ tree 인덱스로 단건 조회 */
    ACCESS_PATH_INDEX_DELETE,  /* B+ tree 인덱스로 단건 삭제 */
    ACCESS_PATH_INSERT,        /* 힙 삽입 + 인덱스 등록 */
    ACCESS_PATH_INDEX_UPDATE,  /* B+ tree 인덱스로 단건 UPDATE */
    ACCESS_PATH_CREATE_TABLE,  /* 테이블 생성 (스키마 등록) */
    ACCESS_PATH_DROP_TABLE     /* 테이블 삭제 */
} access_path_t;

/* 실행 계획: 접근 경로를 담는 구조체 */
typedef struct {
    access_path_t access_path;  /* 선택된 접근 경로 */
} plan_t;

/* SQL 문을 분석하여 실행 계획을 생성한다 */
plan_t planner_create_plan(const statement_t *stmt);

/* 접근 경로의 이름 문자열을 반환한다 (EXPLAIN 출력용) */
const char *access_path_name(access_path_t ap);

#endif /* PLANNER_H */
