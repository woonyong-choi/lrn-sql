/*
 * stress.c — MiniDB 혼합 부하 스트레스 테스트 + 실시간 대시보드
 *
 * 기능:
 *   - INSERT / SELECT / UPDATE / DELETE를 설정 비율로 혼합
 *   - 1초 간격 실시간 대시보드 (TPS, 지연시간, 서버 상태 in-place 갱신)
 *   - 최종 요약: 유형별 breakdown, 히스토그램, 전체 통계
 *
 * 사용법:
 *   ./build/stress [HOST] [PORT] [THREADS] [TOTAL_REQUESTS] [DURATION_SEC]
 *
 *   TOTAL_REQUESTS=0 -> DURATION_SEC 동안 무한 반복 (시간 기반)
 *   DURATION_SEC=0   -> TOTAL_REQUESTS만큼 실행 (횟수 기반)
 *   둘 다 0 아님     -> 둘 중 먼저 도달하는 조건에서 종료
 *
 * 기본값: localhost 8080 8스레드 10000건 0초(횟수 기반)
 *
 * 혼합 비율 (환경변수):
 *   MIX_INSERT=60  MIX_SELECT=25  MIX_UPDATE=10  MIX_DELETE=5
 *
 * 소켓 고갈 방지:
 *   SO_LINGER(0)으로 TIME_WAIT 회피 + keep-alive 커넥션 재사용
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdatomic.h>
#include <math.h>
#include <signal.h>
#include <errno.h>

/* ── 설정 ── */

static const char *g_host = "127.0.0.1";
static int g_port = 8080;

typedef enum {
    OP_INSERT = 0,
    OP_SELECT,
    OP_UPDATE,
    OP_DELETE,
    OP_COUNT
} op_type_t;

static const char *OP_NAMES[] = {"INSERT", "SELECT", "UPDATE", "DELETE"};
static int g_mix[OP_COUNT] = {60, 25, 10, 5};

/* ── 지연시간 기록 (마이크로초) ── */

#define MAX_LATENCIES (20 * 1000 * 1000)

typedef struct {
    uint64_t *data;
    _Atomic uint64_t count;
    uint64_t capacity;
} latency_store_t;

static latency_store_t g_latencies;

static void latency_init(uint64_t cap)
{
    if (cap > MAX_LATENCIES) {
        cap = MAX_LATENCIES;
    }
    g_latencies.capacity = cap;
    atomic_store(&g_latencies.count, 0);
    g_latencies.data = (uint64_t *)calloc(cap, sizeof(uint64_t));
}

static void latency_record(uint64_t us)
{
    uint64_t idx = atomic_fetch_add(&g_latencies.count, 1);
    if (idx < g_latencies.capacity) {
        g_latencies.data[idx] = us;
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* ── 원자적 통계 ── */

typedef struct {
    _Atomic uint64_t ok;
    _Atomic uint64_t fail;
    _Atomic uint64_t latency_sum_us;
} op_stats_t;

static op_stats_t g_stats[OP_COUNT];
static _Atomic uint64_t g_total_done;
static _Atomic uint64_t g_total_target;
static _Atomic int g_running;

typedef struct {
    uint64_t ok[OP_COUNT];
    uint64_t fail[OP_COUNT];
    uint64_t total;
} snapshot_t;

/* ── 시간 유틸 ── */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ── 버퍼드 소켓 리더 (keep-alive용) ── */

typedef struct {
    int   fd;
    char  buf[16384];
    size_t len;   /* buf 안의 유효 바이트 수 */
    size_t pos;   /* 소비된 위치 */
} conn_buf_t;

static void conn_buf_init(conn_buf_t *cb, int fd)
{
    cb->fd  = fd;
    cb->len = 0;
    cb->pos = 0;
}

/* 버퍼에 데이터를 채운다. 이미 있는 미소비 데이터는 앞으로 이동 */
static ssize_t conn_buf_fill(conn_buf_t *cb)
{
    /* 미소비 데이터를 앞으로 이동 */
    if (cb->pos > 0) {
        size_t remaining = cb->len - cb->pos;
        if (remaining > 0) {
            memmove(cb->buf, cb->buf + cb->pos, remaining);
        }
        cb->len = remaining;
        cb->pos = 0;
    }
    /* 남은 공간에 recv */
    size_t space = sizeof(cb->buf) - cb->len;
    if (space == 0) {
        return 0;
    }
    ssize_t n = recv(cb->fd, cb->buf + cb->len, space, 0);
    if (n > 0) {
        cb->len += (size_t)n;
    }
    return n;
}

/* 미소비 버퍼에서 "\r\n\r\n"을 찾는다. 찾으면 buf+pos 기준 오프셋 반환, 없으면 -1 */
static ssize_t conn_buf_find_header_end(conn_buf_t *cb)
{
    size_t avail = cb->len - cb->pos;
    if (avail < 4) {
        return -1;
    }
    for (size_t i = 0; i <= avail - 4; i++) {
        if (memcmp(cb->buf + cb->pos + i, "\r\n\r\n", 4) == 0)
            return (ssize_t)(i + 4);
    }
    return -1;
}

/* ── HTTP 요청 전송 (SO_LINGER(0) + 재시도) ── */

static int open_connection(void)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    for (int attempt = 0; attempt < 3; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            usleep(1000);
            continue;
        }

        /* TIME_WAIT 방지: 닫을 때 RST 전송 */
        struct linger lg = {.l_onoff = 1, .l_linger = 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }

        close(fd);
        usleep(1000 * (uint32_t)(1 << attempt));
    }
    return -1;
}

/* Connection: close 방식 — 시드 데이터 삽입 및 stats 조회용 */
static int send_query(const char *sql, char *resp, size_t resp_sz)
{
    int fd = open_connection();
    if (fd < 0) {
        return -1;
    }

    size_t sql_len = strlen(sql);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST /query HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_host, g_port, sql_len);

    send(fd, header, (size_t)hlen, 0);
    send(fd, sql, sql_len, 0);

    ssize_t total = 0;
    while (total < (ssize_t)(resp_sz - 1)) {
        ssize_t n = recv(fd, resp + total, resp_sz - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    close(fd);

    return (total > 0 && strstr(resp, "200 OK")) ? 0 : -1;
}

static int send_get_stats(char *resp, size_t resp_sz)
{
    int fd = open_connection();
    if (fd < 0) {
        return -1;
    }

    const char *req =
        "GET /stats HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    send(fd, req, strlen(req), 0);

    ssize_t total = 0;
    while (total < (ssize_t)(resp_sz - 1)) {
        ssize_t n = recv(fd, resp + total, resp_sz - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    close(fd);

    return (total > 0 && strstr(resp, "200 OK")) ? 0 : -1;
}

/* ── 요청 유형 선택 ── */

static op_type_t pick_op(unsigned int *seed)
{
    int total = 0;
    for (int i = 0; i < OP_COUNT; i++) total += g_mix[i];
    int r = rand_r(seed) % total;
    int acc = 0;
    for (int i = 0; i < OP_COUNT; i++) {
        acc += g_mix[i];
        if (r < acc) return (op_type_t)i;
    }
    return OP_INSERT;
}

/* ── SQL 생성 ── */

static const char *NAMES[] = {
    "alice", "bob", "charlie", "david", "emma",
    "frank", "grace", "henry", "iris", "jack",
    "kate", "liam", "mia", "noah", "olivia"
};
#define NAME_COUNT 15

static void make_sql(op_type_t op, int thread_id, int seq,
                     unsigned int *seed, char *buf, size_t sz)
{
    int id;
    switch (op) {
    case OP_INSERT:
        snprintf(buf, sz, "INSERT INTO stress VALUES ('%s_%d_%d', %d)",
                 NAMES[rand_r(seed) % NAME_COUNT], thread_id, seq,
                 rand_r(seed) % 100);
        break;
    case OP_SELECT:
        if (rand_r(seed) % 10 < 7) {
            id = (rand_r(seed) % 10000) + 1;
            snprintf(buf, sz, "SELECT * FROM stress WHERE id = %d", id);
        } else {
            int age = rand_r(seed) % 80 + 10;
            snprintf(buf, sz, "SELECT * FROM stress WHERE val > %d", age);
        }
        break;
    case OP_UPDATE:
        id = (rand_r(seed) % 10000) + 1;
        snprintf(buf, sz, "UPDATE stress SET val = %d WHERE id = %d",
                 rand_r(seed) % 100, id);
        break;
    case OP_DELETE:
        id = (rand_r(seed) % 50000) + 1;
        snprintf(buf, sz, "DELETE FROM stress WHERE id = %d", id);
        break;
    default:
        snprintf(buf, sz, "SELECT COUNT(*) FROM stress");
        break;
    }
}

/* ── keep-alive 요청/응답 (벌크 버퍼 읽기) ── */

/*
 * send_query_keepalive — 버퍼드 리더로 keep-alive 응답을 정확히 읽는다.
 *
 * 핵심: recv를 한 번에 큰 청크로 읽어 syscall 횟수를 줄이고,
 *       Content-Length 기반으로 정확한 바운더리를 지킨다.
 *       남은 데이터는 conn_buf에 보존되어 다음 응답에 재사용.
 *
 * 반환: 0=성공, -1=실패 (연결 끊김)
 */
static int send_query_keepalive(conn_buf_t *cb, const char *sql)
{
    size_t sql_len = strlen(sql);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST /query HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        g_host, g_port, sql_len);

    if (send(cb->fd, header, (size_t)hlen, MSG_NOSIGNAL) <= 0) {
        return -1;
    }
    if (send(cb->fd, sql, sql_len, MSG_NOSIGNAL) <= 0) {
        return -1;
    }

    /* 1단계: 헤더 끝(\r\n\r\n)을 찾을 때까지 벌크 읽기 */
    ssize_t hdr_end;
    while ((hdr_end = conn_buf_find_header_end(cb)) < 0) {
        ssize_t n = conn_buf_fill(cb);
        if (n <= 0) return -1;
    }

    /* 헤더에서 Content-Length 파싱 */
    /* 임시로 null-terminate해서 strstr 사용 */
    char saved = cb->buf[cb->pos + (size_t)hdr_end];
    cb->buf[cb->pos + (size_t)hdr_end] = '\0';

    size_t content_len = 0;
    const char *cl = strstr(cb->buf + cb->pos, "Content-Length:");
    if (!cl) cl = strstr(cb->buf + cb->pos, "content-length:");
    if (cl) content_len = (size_t)atoi(cl + 15);

    /* 200 OK 확인 */
    int is_ok = (strstr(cb->buf + cb->pos, "200 OK") != NULL) ? 1 : 0;

    cb->buf[cb->pos + (size_t)hdr_end] = saved;

    /* 2단계: body를 정확히 content_len 바이트만큼 확보 */
    size_t after_header = cb->len - cb->pos - (size_t)hdr_end;  /* 헤더 이후 이미 읽은 양 */

    while (after_header < content_len) {
        ssize_t n = conn_buf_fill(cb);
        if (n <= 0) return -1;
        after_header = cb->len - cb->pos - (size_t)hdr_end;
    }

    /* pos를 본문 끝으로 전진 (헤더 + 본문 소비 완료) */
    cb->pos += (size_t)hdr_end + content_len;

    return is_ok ? 0 : -1;
}

/* ── Persistent Connection 워커 ── */

typedef struct {
    int thread_id;
    int requests;
} worker_arg_t;

static void *worker_thread(void *arg)
{
    worker_arg_t *a = (worker_arg_t *)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ (a->thread_id * 7919));
    char sql[512];
    int count = 0;
    conn_buf_t cb;
    cb.fd = -1;

    while (atomic_load(&g_running)) {
        if (a->requests > 0 && count >= a->requests) break;

        /* 연결이 없으면 새로 열기 */
        if (cb.fd < 0) {
            int fd = open_connection();
            if (fd < 0) {
                op_type_t op = pick_op(&seed);
                atomic_fetch_add(&g_stats[op].fail, 1);
                atomic_fetch_add(&g_total_done, 1);
                count++;
                usleep(1000);
                continue;
            }
            conn_buf_init(&cb, fd);
        }

        op_type_t op = pick_op(&seed);
        make_sql(op, a->thread_id, count, &seed, sql, sizeof(sql));

        uint64_t t0 = now_us();
        int rc = send_query_keepalive(&cb, sql);
        uint64_t elapsed = now_us() - t0;

        if (rc == 0) {
            atomic_fetch_add(&g_stats[op].ok, 1);
        } else {
            atomic_fetch_add(&g_stats[op].fail, 1);
            close(cb.fd);
            cb.fd = -1;
        }

        atomic_fetch_add(&g_stats[op].latency_sum_us, elapsed);
        atomic_fetch_add(&g_total_done, 1);
        latency_record(elapsed);
        count++;
    }

    if (cb.fd >= 0) {
        close(cb.fd);
    }
    return NULL;
}

/* ── 서버 상태 구조체 ── */

typedef struct {
    int workers_total, workers_active, workers_idle;
    int queue_pending, queue_capacity;
    long total_processed, total_connections;
    int locks_total, locks_shared, locks_exclusive;
    int frames_used, frames_dirty, frames_pinned;
    long row_count;
    int valid;
} server_stats_t;

static int json_int(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    return atoi(p);
}

static long json_long(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    return atol(p);
}

static int fetch_stats(server_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    char buf[4096];
    if (send_get_stats(buf, sizeof(buf)) != 0) return -1;

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        return -1;
    }
    body += 4;

    st->workers_total    = json_int(body, "workers_total");
    st->workers_active   = json_int(body, "workers_active");
    st->workers_idle     = json_int(body, "workers_idle");
    st->queue_pending    = json_int(body, "queue_pending");
    st->queue_capacity   = json_int(body, "queue_capacity");
    st->total_processed  = json_long(body, "total_processed");
    st->total_connections = json_long(body, "total_connections");
    st->locks_total      = json_int(body, "total");
    st->locks_shared     = json_int(body, "shared");
    st->locks_exclusive  = json_int(body, "exclusive");
    st->frames_used      = json_int(body, "frames_used");
    st->frames_dirty     = json_int(body, "frames_dirty");
    st->frames_pinned    = json_int(body, "frames_pinned");
    st->row_count        = json_long(body, "row_count");
    st->valid = 1;
    return 0;
}

/* ── 대시보드 모니터 스레드 ── */

/*
 * ANSI 대시보드: 고정 영역을 매초 덮어씀 (ASCII 전용)
 *
 * 레이아웃 (18줄 고정):
 *   [1]  헤더
 *   [2]  TPS 표 헤더
 *   [3]  구분선
 *   [4-7]  유형별 통계
 *   [8]  합계
 *   [9]  빈줄
 *   [10] Server Internals 헤더
 *   [11-13] 서버 상태 3줄
 *   [14] Server Internals 닫기
 *   [15] 빈줄
 *   [16] 진행 바
 *   [17] TPS 표시
 *   [18] 빈줄 (여유)
 */

#define DASHBOARD_LINES 18

static _Atomic int g_monitor_stop;

static void take_snapshot(snapshot_t *s)
{
    s->total = atomic_load(&g_total_done);
    for (int i = 0; i < OP_COUNT; i++) {
        s->ok[i] = atomic_load(&g_stats[i].ok);
        s->fail[i] = atomic_load(&g_stats[i].fail);
    }
}

/* 숫자를 3자리 콤마 형식으로 (예: 1,234,567) */
static void fmt_num(uint64_t n, char *buf, size_t sz)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%lu", (unsigned long)n);
    int len = (int)strlen(raw);
    int commas = (len - 1) / 3;
    int total = len + commas;
    if ((size_t)total >= sz) {
        snprintf(buf, sz, "%s", raw);
        return;
    }

    buf[total] = '\0';
    int src = len - 1, dst = total - 1, cnt = 0;
    while (src >= 0) {
        buf[dst--] = raw[src--];
        cnt++;
        if (cnt == 3 && src >= 0) { buf[dst--] = ','; cnt = 0; }
    }
}

/* 진행 바 생성 */
static void make_progress_bar(double pct, char *bar, size_t sz)
{
    int width = 40;
    int filled = (int)(pct / 100.0 * width);
    if (filled > width) {
        filled = width;
    }
    int i = 0;
    bar[i++] = '[';
    for (int j = 0; j < width; j++) {
        if (j < filled) bar[i++] = '#';
        else bar[i++] = '.';
    }
    bar[i++] = ']';
    bar[i] = '\0';
    (void)sz;
}

static void *monitor_thread(void *arg)
{
    (void)arg;
    snapshot_t prev;
    memset(&prev, 0, sizeof(prev));
    int sec = 0;
    uint64_t start_us = now_us();

    /* 대시보드 영역 확보 (빈 줄 출력) */
    for (int i = 0; i < DASHBOARD_LINES; i++) {
        fprintf(stderr, "\n");
    }

    while (!atomic_load(&g_monitor_stop)) {
        usleep(1000000);
        sec++;

        snapshot_t cur;
        take_snapshot(&cur);

        /* 델타 계산 */
        uint64_t delta_total = cur.total - prev.total;
        uint64_t delta_ok[OP_COUNT], delta_fail[OP_COUNT];
        uint64_t sum_delta_fail = 0;
        for (int i = 0; i < OP_COUNT; i++) {
            delta_ok[i] = cur.ok[i] - prev.ok[i];
            delta_fail[i] = cur.fail[i] - prev.fail[i];
            sum_delta_fail += delta_fail[i];
        }
        (void)sum_delta_fail;
        (void)delta_ok;
        (void)delta_fail;

        uint64_t cum_ok = 0, cum_fail = 0;
        for (int i = 0; i < OP_COUNT; i++) {
            cum_ok += cur.ok[i];
            cum_fail += cur.fail[i];
        }

        double elapsed = (double)(now_us() - start_us) / 1000000.0;
        double avg_tps = elapsed > 0 ? (double)cur.total / elapsed : 0;
        double fail_pct = cur.total > 0 ? (double)cum_fail / (double)cur.total * 100.0 : 0;

        /* 서버 상태 */
        server_stats_t ss;
        int has_ss = (fetch_stats(&ss) == 0);

        /* 진행률 */
        uint64_t target = atomic_load(&g_total_target);
        double progress = (target > 0) ? (double)cur.total / (double)target * 100.0 : 0;
        if (progress > 100.0) {
            progress = 100.0;
        }

        /* ── 대시보드 그리기 ── */
        /* 커서를 DASHBOARD_LINES만큼 위로 이동 */
        fprintf(stderr, "\033[%dA", DASHBOARD_LINES);

        char total_str[32], ok_str[32], fail_str[32];
        fmt_num(cur.total, total_str, sizeof(total_str));
        fmt_num(cum_ok, ok_str, sizeof(ok_str));
        fmt_num(cum_fail, fail_str, sizeof(fail_str));

        /* [1] 헤더 */
        fprintf(stderr, "\033[2K\033[1;32m=== MiniDB Stress Dashboard ===\033[0m  "
                "[%ds] total: %s  ok: %s  fail: %s (%.1f%%)\r\n",
                sec, total_str, ok_str, fail_str, fail_pct);

        /* [2] TPS 표 헤더 */
        fprintf(stderr, "\033[2K\033[1;36m  %-8s | %9s | %9s | %9s | %9s | %9s\033[0m\r\n",
                "TYPE", "THIS_SEC", "OK", "FAIL", "AVG(ms)", "CUM_TPS");
        /* [3] 구분선 */
        fprintf(stderr, "\033[2K  ---------+-----------+-----------+-----------+-----------+-----------\r\n");

        /* [4-7] 유형별 */
        for (int i = 0; i < OP_COUNT; i++) {
            uint64_t c_ok = cur.ok[i];
            uint64_t c_fail = cur.fail[i];
            uint64_t lat_sum = atomic_load(&g_stats[i].latency_sum_us);
            uint64_t cnt = c_ok + c_fail;
            double avg_ms = cnt > 0 ? (double)lat_sum / (double)cnt / 1000.0 : 0;
            double ctps = elapsed > 0 ? (double)cnt / elapsed : 0;

            fprintf(stderr, "\033[2K  %-8s | %9lu | %9lu | %9lu | %9.2f | %9.0f\r\n",
                    OP_NAMES[i],
                    (unsigned long)(delta_ok[i] + delta_fail[i]),
                    (unsigned long)c_ok,
                    (unsigned long)c_fail,
                    avg_ms, ctps);
        }

        /* [8] 합계 */
        fprintf(stderr, "\033[2K  \033[1m%-8s | %9lu | %9lu | %9lu | %9s | %9.0f\033[0m\r\n",
                "TOTAL",
                (unsigned long)delta_total,
                (unsigned long)cum_ok,
                (unsigned long)cum_fail,
                "",
                avg_tps);

        /* [9] 빈줄 */
        fprintf(stderr, "\033[2K\r\n");

        /* [10] Server Internals 헤더 */
        fprintf(stderr, "\033[2K\033[1;33m  +-- Server Internals ----------------------------------------+\033[0m\r\n");
        if (has_ss) {
            /* [11] */
            fprintf(stderr, "\033[2K\033[0;33m  |\033[0m Workers: \033[1m%d\033[0m active / \033[1m%d\033[0m idle / %d total"
                    "    Queue: \033[1m%d\033[0m / %d\r\n",
                    ss.workers_active, ss.workers_idle, ss.workers_total,
                    ss.queue_pending, ss.queue_capacity);
            /* [12] */
            fprintf(stderr, "\033[2K\033[0;33m  |\033[0m Row Lock: \033[1m%d\033[0m (S:%d X:%d)"
                    "    Page Cache: \033[1m%d\033[0m/256 (dirty:%d pin:%d)\r\n",
                    ss.locks_total, ss.locks_shared, ss.locks_exclusive,
                    ss.frames_used, ss.frames_dirty, ss.frames_pinned);
            /* [13] */
            fprintf(stderr, "\033[2K\033[0;33m  |\033[0m DB rows: \033[1m%ld\033[0m"
                    "    Server processed: %ld    Connections: %ld\r\n",
                    ss.row_count, ss.total_processed, ss.total_connections);
        } else {
            fprintf(stderr, "\033[2K\033[0;33m  |\033[0m (server not responding)\r\n");
            fprintf(stderr, "\033[2K\033[0;33m  |\033[0m\r\n");
            fprintf(stderr, "\033[2K\033[0;33m  |\033[0m\r\n");
        }
        /* [14] */
        fprintf(stderr, "\033[2K\033[1;33m  +------------------------------------------------------------+\033[0m\r\n");

        /* [15] 빈줄 */
        fprintf(stderr, "\033[2K\r\n");

        /* [16] 진행 바 */
        if (target > 0) {
            char bar[64];
            make_progress_bar(progress, bar, sizeof(bar));
            fprintf(stderr, "\033[2K  %s %.1f%%\r\n", bar, progress);
        } else {
            fprintf(stderr, "\033[2K  (duration mode -- Ctrl+C to stop)\r\n");
        }
        /* [17] TPS */
        fprintf(stderr, "\033[2K  TPS(1s): \033[1m%lu\033[0m  avg TPS: \033[1m%.0f\033[0m\r\n",
                (unsigned long)delta_total, avg_tps);
        /* [18] 여유 */
        fprintf(stderr, "\033[2K\r\n");

        prev = cur;
    }
    return NULL;
}

/* ── 최종 리포트 ── */

static void print_report(double elapsed_sec)
{
    uint64_t n = atomic_load(&g_latencies.count);
    if (n > g_latencies.capacity) n = g_latencies.capacity;

    if (n > 0) {
        qsort(g_latencies.data, (size_t)n, sizeof(uint64_t), cmp_u64);
    }

    uint64_t total_ok = 0, total_fail = 0;
    for (int i = 0; i < OP_COUNT; i++) {
        total_ok += atomic_load(&g_stats[i].ok);
        total_fail += atomic_load(&g_stats[i].fail);
    }
    uint64_t grand = total_ok + total_fail;

    /* 대시보드 영역 지우기 */
    fprintf(stderr, "\033[%dA", DASHBOARD_LINES);
    for (int i = 0; i < DASHBOARD_LINES; i++) {
        fprintf(stderr, "\033[2K\n");
    }
    fprintf(stderr, "\033[%dA", DASHBOARD_LINES);

    fprintf(stderr, "\033[1;33m===========================================================\033[0m\n");
    fprintf(stderr, "\033[1;33m                    STRESS TEST REPORT                     \033[0m\n");
    fprintf(stderr, "\033[1;33m===========================================================\033[0m\n\n");

    fprintf(stderr, "\033[1m[Total Stats]\033[0m\n");
    char grand_s[32], ok_s[32], fail_s[32];
    fmt_num(grand, grand_s, sizeof(grand_s));
    fmt_num(total_ok, ok_s, sizeof(ok_s));
    fmt_num(total_fail, fail_s, sizeof(fail_s));
    fprintf(stderr, "  Elapsed    : %.2f sec\n", elapsed_sec);
    fprintf(stderr, "  Requests   : %s\n", grand_s);
    fprintf(stderr, "  Success    : %s\n", ok_s);
    fprintf(stderr, "  Fail       : %s (%.2f%%)\n", fail_s,
            grand > 0 ? (double)total_fail / (double)grand * 100.0 : 0.0);
    fprintf(stderr, "  Total TPS  : %.1f req/sec\n",
            elapsed_sec > 0 ? (double)grand / elapsed_sec : 0.0);
    fprintf(stderr, "  OK TPS     : %.1f req/sec\n",
            elapsed_sec > 0 ? (double)total_ok / elapsed_sec : 0.0);
    fprintf(stderr, "\n");

    fprintf(stderr, "\033[1m[Per-Type Stats]\033[0m\n");
    fprintf(stderr, "  %-8s | %10s | %10s | %10s | %10s\n",
            "TYPE", "OK", "FAIL", "AVG(ms)", "TPS");
    fprintf(stderr, "  ---------+------------+------------+------------+------------\n");
    for (int i = 0; i < OP_COUNT; i++) {
        uint64_t ok = atomic_load(&g_stats[i].ok);
        uint64_t fail = atomic_load(&g_stats[i].fail);
        uint64_t lat_sum = atomic_load(&g_stats[i].latency_sum_us);
        uint64_t cnt = ok + fail;
        double avg_ms = cnt > 0 ? (double)lat_sum / (double)cnt / 1000.0 : 0;
        double tps = elapsed_sec > 0 ? (double)cnt / elapsed_sec : 0;
        fprintf(stderr, "  %-8s | %10lu | %10lu | %10.2f | %10.1f\n",
                OP_NAMES[i], (unsigned long)ok, (unsigned long)fail, avg_ms, tps);
    }
    fprintf(stderr, "\n");

    if (n > 0) {
        fprintf(stderr, "\033[1m[Latency Distribution (ms)]\033[0m\n");
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; i++) sum += g_latencies.data[i];

        double avg = (double)sum / (double)n / 1000.0;
        double p50 = (double)g_latencies.data[n * 50 / 100] / 1000.0;
        double p75 = (double)g_latencies.data[n * 75 / 100] / 1000.0;
        double p90 = (double)g_latencies.data[n * 90 / 100] / 1000.0;
        double p95 = (double)g_latencies.data[n * 95 / 100] / 1000.0;
        double p99 = (double)g_latencies.data[n * 99 / 100] / 1000.0;
        double p999 = (double)g_latencies.data[(uint64_t)((double)n * 0.999)] / 1000.0;
        double mn = (double)g_latencies.data[0] / 1000.0;
        double mx = (double)g_latencies.data[n - 1] / 1000.0;

        fprintf(stderr, "  min    : %10.2f ms\n", mn);
        fprintf(stderr, "  avg    : %10.2f ms\n", avg);
        fprintf(stderr, "  p50    : %10.2f ms\n", p50);
        fprintf(stderr, "  p75    : %10.2f ms\n", p75);
        fprintf(stderr, "  p90    : %10.2f ms\n", p90);
        fprintf(stderr, "  p95    : %10.2f ms\n", p95);
        fprintf(stderr, "  p99    : %10.2f ms\n", p99);
        fprintf(stderr, "  p99.9  : %10.2f ms\n", p999);
        fprintf(stderr, "  max    : %10.2f ms\n", mx);
        fprintf(stderr, "\n");

        fprintf(stderr, "\033[1m[Latency Histogram]\033[0m\n");
        static const double BUCKET_UPPER[] = {
            500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 500000, 1e18
        };
        static const char *BUCKET_LABEL[] = {
            "< 0.5ms", "<   1ms", "<   2ms", "<   5ms", "<  10ms",
            "<  20ms", "<  50ms", "< 100ms", "< 500ms", ">= 500ms"
        };
        #define NBUCKETS 10

        uint64_t bucket[NBUCKETS];
        memset(bucket, 0, sizeof(bucket));
        for (uint64_t i = 0; i < n; i++) {
            for (int b = 0; b < NBUCKETS; b++) {
                if ((double)g_latencies.data[i] < BUCKET_UPPER[b]) {
                    bucket[b]++;
                    break;
                }
            }
        }

        uint64_t max_bucket = 0;
        for (int b = 0; b < NBUCKETS; b++) {
            if (bucket[b] > max_bucket) {
                max_bucket = bucket[b];
            }
        }

        for (int b = 0; b < NBUCKETS; b++) {
            if (bucket[b] == 0 && b > 0 && bucket[b-1] == 0) continue;
            int bar_len = max_bucket > 0
                ? (int)((double)bucket[b] / (double)max_bucket * 40.0) : 0;
            if (bucket[b] > 0 && bar_len == 0) {
                bar_len = 1;
            }
            char bar[42];
            memset(bar, '#', (size_t)bar_len);
            bar[bar_len] = '\0';
            double pct = (double)bucket[b] / (double)n * 100.0;
            char cnt_str[32];
            fmt_num(bucket[b], cnt_str, sizeof(cnt_str));
            fprintf(stderr, "  %9s | %6.1f%% | %s (%s)\n",
                    BUCKET_LABEL[b], pct, bar, cnt_str);
        }
    }

    /* 서버 최종 상태 */
    fprintf(stderr, "\n");
    server_stats_t final_ss;
    if (fetch_stats(&final_ss) == 0) {
        fprintf(stderr, "\033[1m[Server Final State]\033[0m\n");
        fprintf(stderr, "  Workers       : %d (active: %d, idle: %d)\n",
                final_ss.workers_total, final_ss.workers_active, final_ss.workers_idle);
        fprintf(stderr, "  Queue         : %d / %d\n",
                final_ss.queue_pending, final_ss.queue_capacity);
        fprintf(stderr, "  Processed     : %ld\n", final_ss.total_processed);
        fprintf(stderr, "  Connections   : %ld\n", final_ss.total_connections);
        fprintf(stderr, "  Row Lock      : %d (S:%d, X:%d)\n",
                final_ss.locks_total, final_ss.locks_shared, final_ss.locks_exclusive);
        fprintf(stderr, "  Page Cache    : %d/256 (dirty: %d, pinned: %d)\n",
                final_ss.frames_used, final_ss.frames_dirty, final_ss.frames_pinned);
        fprintf(stderr, "  DB rows       : %ld\n", final_ss.row_count);
    }

    fprintf(stderr, "\n\033[1;33m===========================================================\033[0m\n");
}

/* ── SIGINT 핸들러 ── */

static void sigint_handler(int sig)
{
    (void)sig;
    atomic_store(&g_running, 0);
}

/* ── 메인 ── */

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    int num_threads = 8;
    uint64_t total_requests = 10000;
    int duration_sec = 0;

    if (argc >= 2) { g_host = argv[1]; }
    if (argc >= 3) { g_port = atoi(argv[2]); }
    if (argc >= 4) { num_threads = atoi(argv[3]); }
    if (argc >= 5) { total_requests = (uint64_t)atoll(argv[4]); }
    if (argc >= 6) { duration_sec = atoi(argv[5]); }

    const char *env;
    if ((env = getenv("MIX_INSERT")) != NULL) g_mix[OP_INSERT] = atoi(env);
    if ((env = getenv("MIX_SELECT")) != NULL) g_mix[OP_SELECT] = atoi(env);
    if ((env = getenv("MIX_UPDATE")) != NULL) g_mix[OP_UPDATE] = atoi(env);
    if ((env = getenv("MIX_DELETE")) != NULL) g_mix[OP_DELETE] = atoi(env);

    int mix_total = 0;
    for (int i = 0; i < OP_COUNT; i++) mix_total += g_mix[i];

    fprintf(stderr, "\033[1;32m=== MiniDB Stress Test ===\033[0m\n");
    fprintf(stderr, "Target     : %s:%d\n", g_host, g_port);
    fprintf(stderr, "Threads    : %d\n", num_threads);
    if (total_requests > 0) {
        fprintf(stderr, "Requests   : %lu\n",
                (unsigned long)total_requests);
    }
    if (duration_sec > 0) {
        fprintf(stderr, "Duration   : %d sec\n", duration_sec);
    }
    fprintf(stderr, "Mix        : INSERT %d%% | SELECT %d%% | UPDATE %d%% | DELETE %d%%\n",
            g_mix[OP_INSERT] * 100 / mix_total,
            g_mix[OP_SELECT] * 100 / mix_total,
            g_mix[OP_UPDATE] * 100 / mix_total,
            g_mix[OP_DELETE] * 100 / mix_total);
    fprintf(stderr, "\n");

    signal(SIGINT, sigint_handler);

    atomic_store(&g_running, 1);
    atomic_store(&g_total_done, 0);
    atomic_store(&g_total_target, total_requests);
    atomic_store(&g_monitor_stop, 0);
    memset(g_stats, 0, sizeof(g_stats));

    uint64_t lat_cap = (total_requests > 0) ? total_requests : 10000000ULL;
    latency_init(lat_cap);

    /* 테이블 준비 */
    char resp[4096];
    send_query("CREATE TABLE stress (name VARCHAR(32), val INT)", resp, sizeof(resp));

    fprintf(stderr, "Seeding 100 rows...\n");
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO stress VALUES ('seed_%d', %d)", i, i);
        send_query(sql, resp, sizeof(resp));
    }
    fprintf(stderr, "Seed complete.\n\n");

    /* 워커 */
    int per_thread = (total_requests > 0)
        ? (int)(total_requests / (uint64_t)num_threads) : 0;
    int remainder = (total_requests > 0)
        ? (int)(total_requests % (uint64_t)num_threads) : 0;

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    worker_arg_t *args = malloc(sizeof(worker_arg_t) * (size_t)num_threads);

    pthread_t mon_tid;
    pthread_create(&mon_tid, NULL, monitor_thread, NULL);

    uint64_t t_start = now_us();

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].requests = per_thread + (i < remainder ? 1 : 0);
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    if (duration_sec > 0) {
        for (int s = 0; s < duration_sec && atomic_load(&g_running); s++) {
            usleep(1000000);
        }
        atomic_store(&g_running, 0);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t t_end = now_us();
    double elapsed = (double)(t_end - t_start) / 1000000.0;

    /* 마지막 대시보드 갱신을 위해 잠시 대기 */
    usleep(200000);
    atomic_store(&g_monitor_stop, 1);
    pthread_join(mon_tid, NULL);

    print_report(elapsed);

    free(g_latencies.data);
    free(threads);
    free(args);
    return 0;
}
