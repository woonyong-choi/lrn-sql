/*
 * main.c — minidb REPL (Read-Eval-Print Loop)
 *
 * 역할:
 *   사용자로부터 SQL 또는 메타 명령어를 입력받아 실행하는 인터랙티브 셸이다.
 *   fgets를 사용하여 표준 입력에서 한 줄씩 읽는다.
 *
 * 명령어 처리 흐름:
 *
 *   사용자 입력: "INSERT INTO users VALUES ('Alice', 25)"
 *     1. fgets() → 문자열 입력
 *     2. 앞뒤 공백 제거
 *     3. line[0] == '.' → 메타 명령어 체크 (아니므로 패스)
 *     4. parse(line, &stmt) → statement_t 생성
 *     5. execute(&pager, &stmt) → 실행 및 결과 출력
 *     6. 다음 입력 대기
 *
 * 메타 명령어 (디버그/관리용):
 *   .exit / .quit  — 프로그램 종료
 *   .btree         — B+ tree 구조 출력 (노드별 키 목록)
 *   .pages         — 페이지 유형별 개수 통계
 *   .stats         — DB 전체 통계 (행 수, 페이지 크기, 트리 높이 등)
 *   .log           — pager 플러시 로그 ON/OFF 토글
 *   .flush         — 모든 dirty 페이지를 수동으로 디스크에 기록
 *   .debug         — 쿼리 디버그 모드 ON/OFF 토글 (페이지 로드/히트/미스/소요시간 출력)
 *
 * 사용법:
 *   ./minidb [DB_PATH]             — REPL 모드
 *   ./minidb --server [PORT] DB_PATH  — TCP 서버 모드 (기본 포트 8080)
 *
 *   파일이 없으면 새로 생성, 있으면 기존 DB를 연다.
 */

#include "sql/parser.h"
#include "sql/executor.h"
#include "sql/planner.h"
#include "storage/pager.h"
#include "storage/bptree.h"
#include "server/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
/* readline 제거 — fgets 기반 입력 */
#define MAX_INPUT_LEN 4096

/*
 * cmd_btree - B+ tree 구조를 출력한다.
 *
 * 예시 출력:
 *   [internal] page 4: keys=[30]
 *     [leaf] page 2: keys=[10, 20]
 *     [leaf] page 3: keys=[30, 40]
 */
static void cmd_btree(pager_t *pager)
{
    bptree_print(pager);
}

/*
 * cmd_pages - 페이지 유형별 개수를 집계하여 출력한다.
 *
 * page 1부터 next_page_id-1까지 순회하며 첫 4바이트(page_type)를 읽어 분류한다.
 *
 * 예시 출력 (next_page_id=8):
 *   전체 페이지: 8
 *     HEADER:   1
 *     HEAP:     2
 *     LEAF:     3
 *     INTERNAL: 1
 *     FREE:     1
 *   빈 페이지 목록: 7
 */
static void cmd_pages(pager_t *pager)
{
    db_header_t *hdr = &pager->header;
    uint32_t heap_count = 0, leaf_count = 0, internal_count = 0, free_count = 0;

    /* 모든 페이지를 순회하며 유형별로 분류 */
    for (uint32_t i = 1; i < hdr->next_page_id; i++) {
        uint8_t *page = pager_get_page(pager, i);
        uint32_t ptype;
        memcpy(&ptype, page, sizeof(uint32_t));
        pager_unpin(pager, i);
        switch (ptype) {
            case PAGE_TYPE_HEAP:     heap_count++; break;
            case PAGE_TYPE_LEAF:     leaf_count++; break;
            case PAGE_TYPE_INTERNAL: internal_count++; break;
            case PAGE_TYPE_FREE:     free_count++; break;
        }
    }

    printf("전체 페이지: %u\n", hdr->next_page_id);
    printf("  HEADER:   1\n");
    printf("  HEAP:     %u\n", heap_count);
    printf("  LEAF:     %u\n", leaf_count);
    printf("  INTERNAL: %u\n", internal_count);
    printf("  FREE:     %u\n", free_count);

    /* 빈 페이지 연결 리스트 출력 (최대 20개까지) */
    if (hdr->free_page_head != 0) {
        printf("빈 페이지 목록:");
        uint32_t fp = hdr->free_page_head;
        int c = 0;
        while (fp != 0 && c < 20) {
            printf(" %u", fp);
            uint8_t *p = pager_get_page(pager, fp);
            free_page_header_t fph;
            memcpy(&fph, p, sizeof(fph));
            pager_unpin(pager, fp);
            fp = fph.next_free_page;
            c++;
            if (fp != 0) {
                printf(" ->");
            }
        }
        if (fp != 0) {
            printf(" ...");
        }
        printf("\n");
    }
}

/*
 * cmd_stats - DB 통계 정보를 출력한다.
 *
 * 예시 출력:
 *   행 수: 100 (live)
 *   다음 ID: 101
 *   페이지 크기: 4096
 *   행 크기: 44
 *   페이지당 행 수: ~80
 *   전체 페이지: 8
 *   B+ Tree 높이: 2
 *   빈 페이지 헤드: 0
 *
 * 페이지당 행 수 계산:
 *   (page_size - heap_header) / (row_size + slot_size)
 *   = (4096 - 16) / (44 + 8) = 4080 / 52 ≈ 78
 */
static void cmd_stats(pager_t *pager)
{
    db_header_t *hdr = &pager->header;
    printf("행 수: %" PRIu64 " (live)\n", hdr->row_count);
    printf("다음 ID: %" PRIu64 "\n", hdr->next_id);
    printf("페이지 크기: %u\n", hdr->page_size);
    printf("행 크기: %u\n", hdr->row_size);
    if (hdr->row_size > 0) {
        uint32_t rows_per_page = (hdr->page_size - sizeof(heap_page_header_t)) /
                                 (hdr->row_size + sizeof(slot_t));
        printf("페이지당 행 수: ~%u\n", rows_per_page);
    }
    printf("전체 페이지: %u\n", hdr->next_page_id);
    printf("B+ Tree 높이: %d\n", bptree_height(pager));
    printf("빈 페이지 헤드: %u\n", hdr->free_page_head);
}

/*
 * main - 프로그램 진입점
 *
 * 흐름:
 *   1. DB 파일 존재 여부 확인 (fopen으로 체크)
 *   2. pager_open()으로 DB 열기 (없으면 create=true)
 *   3. REPL 루프: fgets → 파싱 → 실행 → 반복
 *   4. .exit 또는 EOF(Ctrl+D) → pager_close()로 flush 후 종료
 */
int main(int argc, char **argv)
{
    const char *db_path = "test.db";
    int server_mode = 0;
    int port = 8080;

    /* 인수 파싱 */
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "--server") == 0) {
        server_mode = 1;
        argi++;
        /* 선택적 포트 번호 */
        if (argi < argc && argv[argi][0] >= '0' && argv[argi][0] <= '9') {
            port = atoi(argv[argi]);
            argi++;
        }
    }
    if (argi < argc) {
        db_path = argv[argi];
    }

    pager_t pager;
    bool create = true;

    /* 파일 존재 여부 확인 */
    FILE *f = fopen(db_path, "r");
    if (f != NULL) {
        create = false;
        fclose(f);
    }

    /* 데이터베이스 열기 */
    if (pager_open(&pager, db_path, create) != 0) {
        fprintf(stderr, "오류: '%s' 데이터베이스를 열 수 없습니다\n", db_path);
        return 1;
    }

    /* 서버 모드 */
    if (server_mode) {
        fprintf(stderr, "minidb server: '%s' (page_size=%u)\n",
                db_path, pager.page_size);
        return server_run(&pager, port);
    }

    printf("minidb> '%s' 연결됨 (page_size=%u)\n", db_path, pager.page_size);

    /* REPL: fgets로 입력을 받아 반복 실행 */
    char line[MAX_INPUT_LEN];
    while (1) {
        printf("minidb> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break; /* EOF (Ctrl+D) */
        }

        /* 앞뒤 공백/줄바꿈 제거 */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
                        || line[len - 1] == ' '  || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        char *start = line;
        while (*start == ' ' || *start == '\t') {
            start++;
            len--;
        }
        if (start != line) {
            memmove(line, start, len + 1);
        }

        /* 빈 입력은 무시 */
        if (len == 0) {
            continue;
        }

        /* 메타 명령어 처리 ('.'으로 시작) */
        if (line[0] == '.') {
            if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
                goto exit_repl;
            }
            if (strcmp(line, ".btree") == 0) {
                cmd_btree(&pager);
                continue;
            }
            if (strcmp(line, ".pages") == 0) {
                cmd_pages(&pager);
                continue;
            }
            if (strcmp(line, ".stats") == 0) {
                cmd_stats(&pager);
                continue;
            }
            if (strcmp(line, ".log") == 0) {
                pager.log_flushes = !pager.log_flushes;
                printf("pager 로그: %s\n", pager.log_flushes ? "ON" : "OFF");
                continue;
            }
            if (strcmp(line, ".flush") == 0) {
                pager_flush_all(&pager);
                printf("모든 dirty 페이지를 디스크에 기록했습니다.\n");
                continue;
            }
            if (strcmp(line, ".debug") == 0) {
                pager.debug_mode = !pager.debug_mode;
                printf("디버그 모드: %s\n", pager.debug_mode ? "ON" : "OFF");
                continue;
            }
            printf("알 수 없는 명령어: %s\n", line);
            continue;
        }

        /* SQL 파싱 및 실행 */
        statement_t stmt;
        if (parse(line, &stmt) != 0) {
            printf("오류: SQL 구문을 해석할 수 없습니다\n");
            continue;
        }

        /* 디버그 모드: 통계 초기화 + 실행 시간 측정 시작 */
        struct timespec ts_start, ts_end;
        if (pager.debug_mode) {
            pager_reset_stats(&pager);
            clock_gettime(CLOCK_MONOTONIC, &ts_start);
        }

        exec_result_t res = execute(&pager, &stmt);
        if (res.out_buf != NULL) {
            printf("%s", res.out_buf);
            free(res.out_buf);
        }
        if (res.message[0] != '\0') {
            printf("%s\n", res.message);
        }

        /* 디버그 모드: 통계 출력 */
        if (pager.debug_mode) {
            clock_gettime(CLOCK_MONOTONIC, &ts_end);
            double elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0
                              + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
            query_stats_t *s = &pager.stats;
            printf("[debug] 소요: %.2fms | 페이지 로드: %u (히트: %u, 미스: %u) | 디스크 기록: %u\n",
                   elapsed_ms, s->page_loads, s->cache_hits, s->cache_misses, s->pages_flushed);
        }

    }

exit_repl:
    /* 종료: 모든 변경사항을 디스크에 플러시하고 파일을 닫는다 */
    pager_close(&pager);
    printf("종료합니다.\n");
    return 0;
}
