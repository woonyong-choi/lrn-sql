/*
 * gen_data.c — 대량 테스트 데이터 생성기
 *
 * 역할:
 *   minidb의 pager/table/bptree API를 직접 호출하여 대량의 랜덤 행을 삽입한다.
 *   SQL 파싱 없이 직접 삽입하므로, 100만 건도 빠르게 생성할 수 있다.
 *
 * 스키마:
 *   CREATE TABLE users (name VARCHAR(32), email VARCHAR(32), age INT)
 *   → id(BIGINT 8B) + name(VARCHAR 32B) + email(VARCHAR 32B) + age(INT 4B) = 76바이트/행
 *
 * 사용법:
 *   ./build/gen_data <db파일> <행수>
 *   예: ./build/gen_data sql.db 1000000
 *
 * 진행 상황:
 *   10만 건마다 진행률과 속도를 stderr에 출력한다.
 */

#include "storage/pager.h"
#include "storage/schema.h"
#include "storage/table.h"
#include "storage/bptree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

/* ── 랜덤 이름 생성용 테이블 ── */

static const char *FIRST_NAMES[] = {
    "Alice", "Bob", "Charlie", "David", "Emma",
    "Frank", "Grace", "Henry", "Iris", "Jack",
    "Kate", "Liam", "Mia", "Noah", "Olivia",
    "Paul", "Quinn", "Ruby", "Sam", "Tina",
    "Uma", "Vince", "Wendy", "Xander", "Yuna",
    "Zoe", "Amber", "Brian", "Chloe", "Derek"
};
#define FIRST_COUNT (sizeof(FIRST_NAMES) / sizeof(FIRST_NAMES[0]))

static const char *LAST_NAMES[] = {
    "Kim", "Lee", "Park", "Choi", "Jung",
    "Kang", "Yoon", "Jang", "Lim", "Han",
    "Oh", "Seo", "Shin", "Kwon", "Hwang",
    "Ahn", "Song", "Ryu", "Jeon", "Hong"
};
#define LAST_COUNT (sizeof(LAST_NAMES) / sizeof(LAST_NAMES[0]))

/* ── 스키마 초기화 ── */

/*
 * init_schema - CREATE TABLE users (name VARCHAR(32), email VARCHAR(32), age INT)에
 *               해당하는 스키마를 헤더에 직접 설정한다.
 */
static void init_schema(db_header_t *hdr)
{
    hdr->column_count = 0;

    /* columns[0]: id BIGINT (시스템 컬럼) */
    column_meta_t *id_col = &hdr->columns[hdr->column_count++];
    memset(id_col, 0, sizeof(*id_col));
    strncpy(id_col->name, "id", 31);
    id_col->type = COL_TYPE_BIGINT;
    id_col->size = 8;
    id_col->is_system = 1;

    /* columns[1]: name VARCHAR(32) */
    column_meta_t *name_col = &hdr->columns[hdr->column_count++];
    memset(name_col, 0, sizeof(*name_col));
    strncpy(name_col->name, "name", 31);
    name_col->type = COL_TYPE_VARCHAR;
    name_col->size = 32;
    name_col->is_system = 0;

    /* columns[2]: email VARCHAR(32) — 유니크 값 (user{id}@test.com) */
    column_meta_t *email_col = &hdr->columns[hdr->column_count++];
    memset(email_col, 0, sizeof(*email_col));
    strncpy(email_col->name, "email", 31);
    email_col->type = COL_TYPE_VARCHAR;
    email_col->size = 32;
    email_col->is_system = 0;

    /* columns[3]: age INT */
    column_meta_t *age_col = &hdr->columns[hdr->column_count++];
    memset(age_col, 0, sizeof(*age_col));
    strncpy(age_col->name, "age", 31);
    age_col->type = COL_TYPE_INT;
    age_col->size = 4;
    age_col->is_system = 0;

    schema_compute_layout(hdr);
}

/* ── 메인 ── */

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 3) {
        fprintf(stderr, "사용법: %s <db파일> <행수>\n", argv[0]);
        fprintf(stderr, "  예: %s sql.db 1000000\n", argv[0]);
        return 1;
    }

    const char *db_path = argv[1];
    uint64_t total = (uint64_t)atoll(argv[2]);
    if (total == 0) {
        fprintf(stderr, "오류: 행 수는 1 이상이어야 합니다\n");
        return 1;
    }

    /* DB 새로 생성 */
    pager_t pager;
    if (pager_open(&pager, db_path, true) != 0) {
        fprintf(stderr, "오류: '%s' DB를 생성할 수 없습니다\n", db_path);
        return 1;
    }

    /* 스키마 설정 */
    init_schema(&pager.header);
    pager.header_dirty = true;

    db_header_t *hdr = &pager.header;
    uint8_t *row_buf = (uint8_t *)calloc(1, hdr->row_size);
    if (!row_buf) {
        fprintf(stderr, "오류: 메모리 할당 실패\n");
        pager_close(&pager);
        return 1;
    }

    srand((unsigned)time(NULL));

    fprintf(stderr, "=== minidb 데이터 생성기 ===\n");
    fprintf(stderr, "DB: %s | 행 수: %" PRIu64 " | row_size: %u\n",
            db_path, total, hdr->row_size);
    fprintf(stderr, "스키마: id(BIGINT) + name(VARCHAR 32) + email(VARCHAR 32) + age(INT)\n\n");

    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* 100만 건에서도 초반부터 진행 상황이 보이도록 1% 단위로 보고 */
    uint64_t report_interval = total / 100;
    if (report_interval < 1000) {
        report_interval = total / 5;
        if (report_interval == 0) report_interval = 1;
    }

    for (uint64_t i = 0; i < total; i++) {
        row_value_t values[MAX_COLUMNS];
        memset(values, 0, sizeof(values));

        /* id: 자동 증가 */
        uint64_t id = hdr->next_id;
        values[0].bigint_val = (int64_t)id;

        /* name: "FirstName LastName" 조합 */
        const char *first = FIRST_NAMES[rand() % FIRST_COUNT];
        const char *last  = LAST_NAMES[rand() % LAST_COUNT];
        snprintf(values[1].str_val, 32, "%s %s", first, last);

        /* email: user{id}@test.com (유니크) */
        snprintf(values[2].str_val, 32, "user%" PRIu64 "@test.com", id);

        /* age: 18~80 랜덤 */
        values[3].int_val = 18 + (rand() % 63);

        /* 직렬화 → 힙 삽입 → B+ tree 등록 */
        row_serialize(hdr, values, row_buf);
        row_ref_t ref = heap_insert(&pager, row_buf, hdr->row_size);
        bptree_insert(&pager, id, ref);

        hdr->next_id++;
        hdr->row_count++;
        pager.header_dirty = true;

        /* 진행 보고 */
        if ((i + 1) % report_interval == 0 || i + 1 == total) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double elapsed = (ts_now.tv_sec - ts_start.tv_sec)
                           + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            double rate = (i + 1) / elapsed;
            fprintf(stderr, "\r  [%" PRIu64 "/%" PRIu64 "] %.1f%% | %.0f행/초 | %.1f초 경과",
                    i + 1, total,
                    (double)(i + 1) / total * 100.0,
                    rate, elapsed);
        }
    }

    fprintf(stderr, "\n\n");

    /* 마무리: flush + close */
    free(row_buf);
    pager_close(&pager);

    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    double total_sec = (ts_now.tv_sec - ts_start.tv_sec)
                     + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;

    fprintf(stderr, "=== 완료 ===\n");
    fprintf(stderr, "총 %" PRIu64 "행 삽입 | %.2f초 | %.0f행/초\n",
            total, total_sec, total / total_sec);
    fprintf(stderr, "DB 파일: %s\n", db_path);

    return 0;
}
