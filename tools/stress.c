/*
 * stress.c — MiniDB 혼합 부하 스트레스 테스트 + 실시간 모니터링
 *
 * 기능:
 *   - INSERT / SELECT / UPDATE / DELETE를 설정 비율로 혼합
 *   - 1초 간격 실시간 TPS, 지연시간(avg/p50/p95/p99), 에러율 출력
 *   - 최종 요약: 유형별 breakdown, 히스토그램, 전체 통계
 *
 * 사용법:
 *   ./build/stress [HOST] [PORT] [THREADS] [TOTAL_REQUESTS] [DURATION_SEC]
 *
 *   TOTAL_REQUESTS=0 이면 DURATION_SEC 동안 무한 반복 (시간 기반 모드)
 *   DURATION_SEC=0 이면 TOTAL_REQUESTS만큼만 실행 (횟수 기반 모드)
 *   둘 다 0이 아니면 둘 중 먼저 도달하는 조건에서 종료
 *
 * 기본값: localhost 8080 8스레드 10000건 0초(횟수 기반)
 *
 * 요청 혼합 비율 (환경변수로 조정 가능):
 *   MIX_INSERT=60  MIX_SELECT=25  MIX_UPDATE=10  MIX_DELETE=5
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

/* 요청 유형 */
typedef enum {
    OP_INSERT = 0,
    OP_SELECT,
    OP_UPDATE,
    OP_DELETE,
    OP_COUNT  /* 센티넬: 유형 수 */
} op_type_t;

static const char *OP_NAMES[] = {"INSERT", "SELECT", "UPDATE", "DELETE"};

/* 혼합 비율 (기본값, 환경변수로 오버라이드 가능) */
static int g_mix[OP_COUNT] = {60, 25, 10, 5};

/* ── 지연시간 기록 (마이크로초 단위) ── */

#define MAX_LATENCIES (20 * 1000 * 1000)  /* 최대 2000만 건 기록 */

typedef struct {
    uint64_t *data;
    _Atomic uint64_t count;
    uint64_t capacity;
} latency_store_t;

static latency_store_t g_latencies;

static void latency_init(uint64_t cap) {
    if (cap > MAX_LATENCIES) cap = MAX_LATENCIES;
    g_latencies.capacity = cap;
    atomic_store(&g_latencies.count, 0);
    g_latencies.data = (uint64_t *)calloc(cap, sizeof(uint64_t));
}

static void latency_record(uint64_t us) {
    uint64_t idx = atomic_fetch_add(&g_latencies.count, 1);
    if (idx < g_latencies.capacity) {
        g_latencies.data[idx] = us;
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* ── 원자적 통계 카운터 ── */

typedef struct {
    _Atomic uint64_t ok;
    _Atomic uint64_t fail;
    _Atomic uint64_t latency_sum_us;  /* 합산 (평균 계산용) */
} op_stats_t;

static op_stats_t g_stats[OP_COUNT];
static _Atomic uint64_t g_total_done;
static _Atomic int g_running;   /* 0이면 모든 워커 정지 */

/* 이전 스냅샷 (1초 간격 델타 계산용) */
typedef struct {
    uint64_t ok[OP_COUNT];
    uint64_t fail[OP_COUNT];
    uint64_t total;
} snapshot_t;

/* ── 시간 유틸 ── */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ── HTTP 요청 전송 ── */

static int send_query(const char *sql, char *resp, size_t resp_sz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
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

    /* 응답 전체 수신 (짧은 응답이므로 한 번이면 충분) */
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

/* ── 요청 유형 선택 (가중치 기반 랜덤) ── */

static op_type_t pick_op(unsigned int *seed) {
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
                     unsigned int *seed, char *buf, size_t sz) {
    int id;
    switch (op) {
    case OP_INSERT:
        snprintf(buf, sz, "INSERT INTO stress VALUES ('%s_%d_%d', %d)",
                 NAMES[rand_r(seed) % NAME_COUNT],
                 thread_id, seq,
                 rand_r(seed) % 100);
        break;
    case OP_SELECT:
        /* 70% id 단건 조회, 30% 범위 스캔 */
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
        /* 큰 범위의 랜덤 id → 없을 수도 있지만 요청 자체는 유효 */
        id = (rand_r(seed) % 50000) + 1;
        snprintf(buf, sz, "DELETE FROM stress WHERE id = %d", id);
        break;
    default:
        snprintf(buf, sz, "SELECT COUNT(*) FROM stress");
        break;
    }
}

/* ── 워커 스레드 ── */

typedef struct {
    int thread_id;
    int requests;       /* 0이면 g_running 체크로 무한 반복 */
} worker_arg_t;

static void *worker_thread(void *arg) {
    worker_arg_t *a = (worker_arg_t *)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ (a->thread_id * 7919));
    char sql[512], resp[4096];
    int count = 0;

    while (atomic_load(&g_running)) {
        if (a->requests > 0 && count >= a->requests) break;

        op_type_t op = pick_op(&seed);
        make_sql(op, a->thread_id, count, &seed, sql, sizeof(sql));

        uint64_t t0 = now_us();
        int rc = send_query(sql, resp, sizeof(resp));
        uint64_t elapsed = now_us() - t0;

        if (rc == 0) {
            atomic_fetch_add(&g_stats[op].ok, 1);
        } else {
            atomic_fetch_add(&g_stats[op].fail, 1);
        }
        atomic_fetch_add(&g_stats[op].latency_sum_us, elapsed);
        atomic_fetch_add(&g_total_done, 1);
        latency_record(elapsed);

        count++;
    }
    return NULL;
}

/* ── GET /stats 폴링 ── */

typedef struct {
    int workers_total;
    int workers_active;
    int workers_idle;
    int queue_pending;
    int queue_capacity;
    long total_processed;
    long total_connections;
    int locks_total;
    int locks_shared;
    int locks_exclusive;
    int frames_used;
    int frames_dirty;
    int frames_pinned;
    long row_count;
    int valid;  /* 파싱 성공 여부 */
} server_stats_t;

static int json_int(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atoi(p);
}

static long json_long(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atol(p);
}

static int fetch_stats(server_stats_t *st) {
    memset(st, 0, sizeof(*st));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    const char *req =
        "GET /stats HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    send(fd, req, strlen(req), 0);

    char buf[4096];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(fd);

    if (total <= 0 || !strstr(buf, "200 OK")) return -1;

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    st->workers_total    = json_int(body, "workers_total");
    st->workers_active   = json_int(body, "workers_active");
    st->workers_idle     = json_int(body, "workers_idle");
    st->queue_pending    = json_int(body, "queue_pending");
    st->queue_capacity   = json_int(body, "queue_capacity");
    st->total_processed  = json_long(body, "total_processed");
    st->total_connections = json_long(body, "total_connections");
    st->locks_total      = json_int(body, "total");    /* locks section */
    st->locks_shared     = json_int(body, "shared");
    st->locks_exclusive  = json_int(body, "exclusive");
    st->frames_used      = json_int(body, "frames_used");
    st->frames_dirty     = json_int(body, "frames_dirty");
    st->frames_pinned    = json_int(body, "frames_pinned");
    st->row_count        = json_long(body, "row_count");
    st->valid = 1;
    return 0;
}

/* ── 실시간 모니터 스레드 ── */

static _Atomic int g_monitor_stop;

static void take_snapshot(snapshot_t *s) {
    s->total = atomic_load(&g_total_done);
    for (int i = 0; i < OP_COUNT; i++) {
        s->ok[i] = atomic_load(&g_stats[i].ok);
        s->fail[i] = atomic_load(&g_stats[i].fail);
    }
}

static void *monitor_thread(void *arg) {
    (void)arg;
    snapshot_t prev;
    memset(&prev, 0, sizeof(prev));
    int sec = 0;

    /* 헤더 — 클라이언트 지표 */
    fprintf(stderr,
        "\033[1;36m"
        "%5s │ %8s │ %8s │ %8s │ %8s │ %8s │ %8s │ %10s │ %6s"
        "\033[0m\n",
        "SEC", "TPS", "INSERT", "SELECT", "UPDATE", "DELETE",
        "FAIL", "TOTAL", "FAIL%");
    fprintf(stderr,
        "──────┼──────────┼──────────┼──────────┼──────────┼──────────"
        "┼──────────┼────────────┼───────\n");

    while (!atomic_load(&g_monitor_stop)) {
        usleep(1000000);  /* 1초 */
        sec++;

        snapshot_t now;
        take_snapshot(&now);

        uint64_t delta_total = now.total - prev.total;
        uint64_t delta_ok[OP_COUNT], delta_fail = 0;
        for (int i = 0; i < OP_COUNT; i++) {
            delta_ok[i] = now.ok[i] - prev.ok[i];
            delta_fail += (now.fail[i] - prev.fail[i]);
        }

        uint64_t total_ok_now = 0, total_fail_now = 0;
        for (int i = 0; i < OP_COUNT; i++) {
            total_ok_now += now.ok[i];
            total_fail_now += now.fail[i];
        }

        double fail_pct = (now.total > 0)
            ? (double)total_fail_now / (double)now.total * 100.0 : 0.0;

        /* 클라이언트 지표 */
        fprintf(stderr,
            "%5d │ %8lu │ %8lu │ %8lu │ %8lu │ %8lu │ %8lu │ %10lu │ %5.1f%%",
            sec,
            (unsigned long)delta_total,
            (unsigned long)delta_ok[OP_INSERT],
            (unsigned long)delta_ok[OP_SELECT],
            (unsigned long)delta_ok[OP_UPDATE],
            (unsigned long)delta_ok[OP_DELETE],
            (unsigned long)delta_fail,
            (unsigned long)now.total,
            fail_pct);

        /* 서버 내부 상태 (GET /stats 폴링) */
        server_stats_t ss;
        if (fetch_stats(&ss) == 0) {
            fprintf(stderr,
                " │ \033[0;33mW:%d/%d Q:%d/%d Lk:%d(S%d/X%d) "
                "Cache:%d/%d dirty:%d pin:%d rows:%ld\033[0m",
                ss.workers_active, ss.workers_total,
                ss.queue_pending, ss.queue_capacity,
                ss.locks_total, ss.locks_shared, ss.locks_exclusive,
                ss.frames_used, 256,
                ss.frames_dirty, ss.frames_pinned,
                ss.row_count);
        }

        fprintf(stderr, "\n");
        prev = now;
    }
    return NULL;
}

/* ── 최종 리포트 ── */

static void print_report(double elapsed_sec) {
    uint64_t n = atomic_load(&g_latencies.count);
    if (n > g_latencies.capacity) n = g_latencies.capacity;

    /* 지연시간 정렬 (백분위수 계산용) */
    if (n > 0) {
        qsort(g_latencies.data, (size_t)n, sizeof(uint64_t), cmp_u64);
    }

    uint64_t total_ok = 0, total_fail = 0;
    for (int i = 0; i < OP_COUNT; i++) {
        total_ok += atomic_load(&g_stats[i].ok);
        total_fail += atomic_load(&g_stats[i].fail);
    }
    uint64_t grand = total_ok + total_fail;

    fprintf(stderr, "\n");
    fprintf(stderr, "\033[1;33m═══════════════════════════════════════════════════════════\033[0m\n");
    fprintf(stderr, "\033[1;33m                    STRESS TEST REPORT                     \033[0m\n");
    fprintf(stderr, "\033[1;33m═══════════════════════════════════════════════════════════\033[0m\n\n");

    /* 전체 통계 */
    fprintf(stderr, "\033[1m[전체 통계]\033[0m\n");
    fprintf(stderr, "  소요 시간  : %.2f sec\n", elapsed_sec);
    fprintf(stderr, "  총 요청    : %lu\n", (unsigned long)grand);
    fprintf(stderr, "  성공       : %lu\n", (unsigned long)total_ok);
    fprintf(stderr, "  실패       : %lu (%.2f%%)\n",
            (unsigned long)total_fail,
            grand > 0 ? (double)total_fail / (double)grand * 100.0 : 0.0);
    fprintf(stderr, "  전체 TPS   : %.1f req/sec\n",
            elapsed_sec > 0 ? (double)grand / elapsed_sec : 0.0);
    fprintf(stderr, "  성공 TPS   : %.1f req/sec\n",
            elapsed_sec > 0 ? (double)total_ok / elapsed_sec : 0.0);
    fprintf(stderr, "\n");

    /* 유형별 통계 */
    fprintf(stderr, "\033[1m[유형별 통계]\033[0m\n");
    fprintf(stderr, "  %-8s │ %10s │ %10s │ %10s │ %10s\n",
            "TYPE", "OK", "FAIL", "AVG(ms)", "TPS");
    fprintf(stderr, "  ─────────┼────────────┼────────────┼────────────┼────────────\n");

    for (int i = 0; i < OP_COUNT; i++) {
        uint64_t ok = atomic_load(&g_stats[i].ok);
        uint64_t fail = atomic_load(&g_stats[i].fail);
        uint64_t lat_sum = atomic_load(&g_stats[i].latency_sum_us);
        uint64_t cnt = ok + fail;
        double avg_ms = cnt > 0 ? (double)lat_sum / (double)cnt / 1000.0 : 0.0;
        double tps = elapsed_sec > 0 ? (double)cnt / elapsed_sec : 0.0;

        fprintf(stderr, "  %-8s │ %10lu │ %10lu │ %10.2f │ %10.1f\n",
                OP_NAMES[i],
                (unsigned long)ok, (unsigned long)fail,
                avg_ms, tps);
    }
    fprintf(stderr, "\n");

    /* 백분위수 */
    if (n > 0) {
        fprintf(stderr, "\033[1m[지연시간 분포 (ms)]\033[0m\n");

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

        /* 히스토그램 (로그 스케일 버킷) */
        fprintf(stderr, "\033[1m[지연시간 히스토그램]\033[0m\n");

        /* 버킷: <0.5ms, <1ms, <2ms, <5ms, <10ms, <20ms, <50ms, <100ms, <500ms, >=500ms */
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

        /* 가장 큰 버킷 찾기 (바 스케일링) */
        uint64_t max_bucket = 0;
        for (int b = 0; b < NBUCKETS; b++) {
            if (bucket[b] > max_bucket) max_bucket = bucket[b];
        }

        for (int b = 0; b < NBUCKETS; b++) {
            if (bucket[b] == 0 && b > 0 && bucket[b-1] == 0) continue;

            int bar_len = (max_bucket > 0)
                ? (int)((double)bucket[b] / (double)max_bucket * 40.0) : 0;
            if (bucket[b] > 0 && bar_len == 0) bar_len = 1;

            char bar[42];
            memset(bar, '#', (size_t)bar_len);
            bar[bar_len] = '\0';

            double pct = (double)bucket[b] / (double)n * 100.0;

            fprintf(stderr, "  %9s │ %6.1f%% │ %s (%lu)\n",
                    BUCKET_LABEL[b], pct, bar, (unsigned long)bucket[b]);
        }
    }

    /* 서버 최종 상태 */
    fprintf(stderr, "\n");
    server_stats_t final_ss;
    if (fetch_stats(&final_ss) == 0) {
        fprintf(stderr, "\033[1m[서버 최종 상태]\033[0m\n");
        fprintf(stderr, "  Worker 스레드  : %d (active: %d, idle: %d)\n",
                final_ss.workers_total, final_ss.workers_active, final_ss.workers_idle);
        fprintf(stderr, "  작업 큐       : %d / %d\n",
                final_ss.queue_pending, final_ss.queue_capacity);
        fprintf(stderr, "  서버 누적처리 : %ld\n", final_ss.total_processed);
        fprintf(stderr, "  서버 누적연결 : %ld\n", final_ss.total_connections);
        fprintf(stderr, "  Row Lock      : %d (S:%d, X:%d)\n",
                final_ss.locks_total, final_ss.locks_shared, final_ss.locks_exclusive);
        fprintf(stderr, "  Page Cache    : %d/256 (dirty: %d, pinned: %d)\n",
                final_ss.frames_used, final_ss.frames_dirty, final_ss.frames_pinned);
        fprintf(stderr, "  DB rows       : %ld\n", final_ss.row_count);
    }

    fprintf(stderr, "\n\033[1;33m═══════════════════════════════════════════════════════════\033[0m\n");
}

/* ── SIGINT 핸들러 ── */

static void sigint_handler(int sig) {
    (void)sig;
    atomic_store(&g_running, 0);
}

/* ── 메인 ── */

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    int num_threads = 8;
    uint64_t total_requests = 10000;
    int duration_sec = 0;

    if (argc >= 2) g_host = argv[1];
    if (argc >= 3) g_port = atoi(argv[2]);
    if (argc >= 4) num_threads = atoi(argv[3]);
    if (argc >= 5) total_requests = (uint64_t)atoll(argv[4]);
    if (argc >= 6) duration_sec = atoi(argv[5]);

    /* 환경변수로 혼합 비율 오버라이드 */
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
    if (total_requests > 0)
        fprintf(stderr, "Requests   : %lu\n", (unsigned long)total_requests);
    if (duration_sec > 0)
        fprintf(stderr, "Duration   : %d sec\n", duration_sec);
    fprintf(stderr, "Mix        : INSERT %d%% | SELECT %d%% | UPDATE %d%% | DELETE %d%%\n",
            g_mix[OP_INSERT] * 100 / mix_total,
            g_mix[OP_SELECT] * 100 / mix_total,
            g_mix[OP_UPDATE] * 100 / mix_total,
            g_mix[OP_DELETE] * 100 / mix_total);
    fprintf(stderr, "\n");

    /* SIGINT 핸들러 등록 (Ctrl+C로 조기 종료) */
    signal(SIGINT, sigint_handler);

    /* 초기화 */
    atomic_store(&g_running, 1);
    atomic_store(&g_total_done, 0);
    atomic_store(&g_monitor_stop, 0);
    memset(g_stats, 0, sizeof(g_stats));

    uint64_t lat_cap = (total_requests > 0) ? total_requests : 10000000ULL;
    latency_init(lat_cap);

    /* stress 테이블 준비 */
    char resp[4096];
    send_query("CREATE TABLE stress (name VARCHAR(32), val INT)", resp, sizeof(resp));

    /* 시드 데이터 (UPDATE/SELECT가 초반에 빈 행을 안 건드리도록) */
    fprintf(stderr, "Seeding 100 rows...\n");
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO stress VALUES ('seed_%d', %d)", i, i);
        send_query(sql, resp, sizeof(resp));
    }
    fprintf(stderr, "Seed complete.\n\n");

    /* 워커 스레드 시작 */
    int per_thread = (total_requests > 0)
        ? (int)(total_requests / (uint64_t)num_threads) : 0;
    int remainder = (total_requests > 0)
        ? (int)(total_requests % (uint64_t)num_threads) : 0;

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
    worker_arg_t *args = malloc(sizeof(worker_arg_t) * (size_t)num_threads);

    /* 모니터 스레드 시작 */
    pthread_t mon_tid;
    pthread_create(&mon_tid, NULL, monitor_thread, NULL);

    uint64_t t_start = now_us();

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].requests = per_thread + (i < remainder ? 1 : 0);
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    /* 시간 기반 모드: duration_sec 후 정지 신호 */
    if (duration_sec > 0) {
        for (int s = 0; s < duration_sec && atomic_load(&g_running); s++) {
            usleep(1000000);
        }
        atomic_store(&g_running, 0);
    }

    /* 워커 종료 대기 */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t t_end = now_us();
    double elapsed = (double)(t_end - t_start) / 1000000.0;

    /* 모니터 정지 */
    atomic_store(&g_monitor_stop, 1);
    pthread_join(mon_tid, NULL);

    /* COUNT 확인 */
    fprintf(stderr, "\n");
    if (send_query("SELECT COUNT(*) FROM stress", resp, sizeof(resp)) == 0) {
        char *body = strstr(resp, "\r\n\r\n");
        if (body) {
            body += 4;
            char *sep = strstr(body, "----------\n");
            if (sep) {
                char *num_start = sep + 11;
                fprintf(stderr, "DB rows    : %d\n", atoi(num_start));
            }
        }
    }

    /* 최종 리포트 */
    print_report(elapsed);

    /* 정리 */
    free(g_latencies.data);
    free(threads);
    free(args);
    return 0;
}
