/*
 * rpc 压测客户端（对标 fastcall-c tests/perf/framework/rpc_loadgen.cpp 的
 * CLI 与结果 JSON 契约，便于复用 rpc_bench.py 的聚合/报告）。
 *
 * 形状：单进程单"热连接"，同一时刻 1 个 outstanding RPC（对齐 gRPC 单 channel）。
 */
#include <nb/proxy.h>
#include <nb/node.h>
#include <nb/dds.h>
#include <nb/rtps.h>
#include <nb/shm.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "loadgen"
#include <nb/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uint64_t now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)((double)counter.QuadPart * 1000000000.0 / (double)freq.QuadPart);
}
#else
#include <time.h>
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/*
 * 延迟直方图：小延迟用 1µs 线性桶，大延迟用 log2 分桶，避免高延迟全部被
 * 合并进单一哨兵值。数组索引与代表值（µs）的映射见 lat_hist_index/key()。
 */
#define LAT_LINEAR_MAX 65536 /* [0,65535] 为 1µs 线性桶 */
#define LAT_LOG_BUCKETS 24   /* 覆盖约 65ms .. 数小时 */
#define LAT_HIST_SIZE (LAT_LINEAR_MAX + LAT_LOG_BUCKETS)

/* 延迟(µs) -> 直方图桶索引 */
static int lat_hist_index(uint64_t us)
{
    int b = 0;
    if (us < LAT_LINEAR_MAX) {
        return (int)us;
    }
    while ((us >> (b + 1)) != 0) {
        b++;
    }
    b -= 16; /* floor(log2(us)) - floor(log2(65536)) */
    if (b >= LAT_LOG_BUCKETS) {
        b = LAT_LOG_BUCKETS - 1;
    }
    return LAT_LINEAR_MAX + b;
}

/* 直方图桶索引 -> 代表值(µs)：线性桶取精确值，对数桶取区间中点 */
static uint64_t lat_hist_key(int idx)
{
    int k;
    uint64_t lower;
    uint64_t upper;
    if (idx < LAT_LINEAR_MAX) {
        return (uint64_t)idx;
    }
    k = idx - LAT_LINEAR_MAX;
    lower = (uint64_t)1 << (16 + k);
    upper = (lower << 1) - 1;
    return lower + (upper - lower) / 2;
}

typedef struct options {
    const char *service;
    const char *method;
    const char *worker_id;
    const char *output;
    uint64_t duration_seconds;
    uint64_t max_requests;
    size_t payload_bytes;
    size_t expect_response_bytes;
    uint64_t timeout_ms;
    double rate;
    int verify_response;
    uint32_t connect_timeout_seconds;
    uint16_t discovery_port;
    uint16_t data_port;
    const char *bind;
    const char *peer_ip;
    uint16_t peer_port;
    const char *target_ip;
    uint16_t target_port;
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t backpressure_bytes;
    unsigned io_threads;
    int use_sync;
    const char *spike_log;        /* 尖峰样本落盘路径（诊断用，NULL 表示关闭） */
    uint64_t spike_threshold_us;  /* 记录阈值，默认 1000us */
    int use_dds;
    int dds_domain;
    int use_shm;
    const char *shm_channel;
    int use_rtps;
    const char *rtps_endpoint;
    const char *rtps_target;
    nb_transport_kind_t transport;
    uint64_t window; /* 0 = 收一发一；>0 = 滑动窗口在途上限 */
} options_t;

typedef struct callout {
    ntk_sem *sem;
    int status;
    int app_status;
    size_t resp_len;
    int verify_ok;
    uint64_t send_ns;
    uint64_t latency_ns;
} callout_t;

typedef struct error_entry {
    int code;
    uint64_t count;
} error_entry_t;

static int g_status;
static const uint8_t *g_expect_data;
static size_t g_expect_len;

/* ---- 尖峰日志（诊断用）----
 * 把延迟超过阈值的样本连同样本发出的时刻一起落盘，用于判断尖峰是周期性的
 * 还是孤立的，以及与外部事件对齐。记录点只会在一个线程里执行（异步模式在
 * poller 线程、同步模式在主线程），因此使用普通计数即可。 */
#define SPIKE_MAX_SAMPLES 8192
typedef struct {
    uint64_t send_ns;
    uint64_t latency_ns;
} spike_sample_t;

static spike_sample_t g_spikes[SPIKE_MAX_SAMPLES];
static unsigned g_spike_used;
static unsigned g_spike_dropped;
static unsigned g_slow_total;
static int g_spike_enabled;
static uint64_t g_spike_threshold_ns = 1000000ull;
static const char *g_spike_path;
static uint64_t g_spike_t0_ns;

static void spike_record(uint64_t send_ns, uint64_t latency_ns)
{
    unsigned slot;
    if (latency_ns < g_spike_threshold_ns) {
        return;
    }
    g_slow_total++;
    if (!g_spike_enabled) {
        return;
    }
    slot = g_spike_used++;
    if (slot >= SPIKE_MAX_SAMPLES) {
        g_spike_dropped++;
        return;
    }
    g_spikes[slot].send_ns = send_ns;
    g_spikes[slot].latency_ns = latency_ns;
}

static void spike_log_write(void)
{
    FILE *f;
    unsigned i;
    unsigned n;
    if (!g_spike_enabled || !g_spike_path) {
        return;
    }
    f = fopen(g_spike_path, "w");
    if (!f) {
        return;
    }
    n = g_spike_used > SPIKE_MAX_SAMPLES ? SPIKE_MAX_SAMPLES : g_spike_used;
    fprintf(f, "t_us,latency_us\n");
    for (i = 0; i < n; i++) {
        fprintf(f, "%.3f,%.3f\n",
                (double)(g_spikes[i].send_ns - g_spike_t0_ns) / 1000.0,
                (double)g_spikes[i].latency_ns / 1000.0);
    }
    fprintf(f, "# dropped=%u threshold_us=%.0f\n", g_spike_dropped,
            (double)g_spike_threshold_ns / 1000.0);
    fclose(f);
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    callout_t *co = (callout_t *)user;
    co->status = status;
    co->app_status = resp ? resp->status : status;
    co->resp_len = (status == NB_OK && resp) ? resp->payload.len : 0;
    co->latency_ns = now_ns() - co->send_ns;
    spike_record(co->send_ns, co->latency_ns);
    co->verify_ok = 1;
    if (g_expect_data && status == NB_OK && resp) {
        co->verify_ok = (resp->payload.len == g_expect_len &&
                         memcmp(resp->payload.data, g_expect_data, g_expect_len) == 0);
    }
    ntk_sem_post(co->sem, 1);
}

static uint64_t sleep_until_ns(uint64_t deadline)
{
    for (;;) {
        uint64_t now = now_ns();
        uint64_t remain;
        if (now >= deadline) {
            return now;
        }
        remain = deadline - now;
        if (remain > 2000000ull) {
            ntk_sleep_ms((unsigned)((remain - 1000000ull) / 1000000ull));
        } else {
            /* 自旋等待亚毫秒精度 */
        }
    }
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !s[0]) {
        return 0;
    }
    v = strtoull(s, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

static int parse_size(const char *s, size_t *out)
{
    uint64_t v;
    if (!parse_u64(s, &v)) {
        return 0;
    }
    *out = (size_t)v;
    return 1;
}

static void usage(const char *argv0)
{
    nb_log_error("usage: %s [--service s] [--method m] [--duration-seconds n]\n"
                  "          [--transport udp|tcp] [--dds] [--dds-domain n] "
                  "[--shm] [--shm-channel name] [--rtps <binding-uri>] "
                  "[--rtps-target <uri>] [--io-threads n]\n"
                  "          [--max-requests n] [--payload-bytes n] [--expect-response-bytes n]\n"
                  "          [--timeout-ms n] [--rate qps] [--window n] [--sync]\n"
                  "          [--spike-log path] [--spike-threshold-us n] [--verify-response]\n"
                  "          [--connect-timeout-seconds n] [--worker-id id] [--output path|-]\n"
                  "          [--mode static|mesh]\n"
                  "          [--discovery-port p] [--data-port p] [--max-msg-bytes n] [--bind ip] "
                  "[--peer host:port]",
                  argv0);
}

static void error_add(error_entry_t *errors, int *error_count, int code)
{
    int i;
    for (i = 0; i < *error_count; i++) {
        if (errors[i].code == code) {
            errors[i].count++;
            return;
        }
    }
    if (*error_count < 16) {
        errors[*error_count].code = code;
        errors[*error_count].count = 1;
        (*error_count)++;
    }
}

/* ---- 事件驱动（closed-loop）：全程在 poller 线程内联发送，无跨线程/sem 开销 ---- */

static nb_node_t *g_node;
static nb_proxy_t *g_proxy;
static ntk_sem *g_done;
static options_t g_opt;
static const uint8_t *g_payload;
static uint64_t g_deadline_ns;
static uint64_t g_attempted, g_succeeded, g_failed, g_response_bytes;
static uint64_t g_min_ns, g_max_ns, g_sum_ns, g_lat_count;
static uint64_t g_lat_hist[LAT_HIST_SIZE];
static error_entry_t g_errors[16];
static int g_error_count;
static uint64_t g_window;        /* 0 = 收一发一；>0 = 滑动窗口在途上限 */
static uint64_t g_inflight;      /* 当前在途请求数 */
static uint64_t g_inflight_peak; /* 在途峰值 */
static int g_finished;
static int g_in_issue;
static int g_pause; /* 内联回调要求外层填充循环暂停 */

static void ed_issue(void *user);

static void ed_finish(void)
{
    if (g_finished) {
        return;
    }
    g_finished = 1;
    ntk_sem_post(g_done, 1);
}

static void ed_resp(int status, const nb_response_t *resp, void *user)
{
    int inline_cb = g_in_issue;
    uint64_t send_ns = (uint64_t)(uintptr_t)user;
    uint64_t lat = now_ns() - send_ns;
    spike_record(send_ns, lat);
    uint64_t us = (lat + 999ull) / 1000ull;
    uint64_t hidx = (uint64_t)lat_hist_index(us);
    int ok;

    if (g_inflight > 0) {
        g_inflight--;
    }

    g_lat_hist[hidx]++;

    ok = (status == NB_OK) && resp && resp->status == NB_OK &&
        (g_opt.expect_response_bytes == 0 ||
         (resp && resp->payload.len == g_opt.expect_response_bytes));
    if (ok && g_opt.verify_response && resp) {
        ok = (resp->payload.len == g_opt.payload_bytes &&
              memcmp(resp->payload.data, g_payload, g_opt.payload_bytes) == 0);
    }
    if (ok) {
        g_succeeded++;
        g_response_bytes += resp ? resp->payload.len : 0;
        if (g_lat_count == 0 || lat < g_min_ns) {
            g_min_ns = lat;
        }
        if (lat > g_max_ns) {
            g_max_ns = lat;
        }
        g_sum_ns += lat;
        g_lat_count++;
    } else {
        g_failed++;
        error_add(
            g_errors, &g_error_count, status != NB_OK ? status : (resp ? resp->status : NB_ERR_STATE));
    }

    if (now_ns() >= g_deadline_ns ||
        (g_opt.max_requests != 0 && g_attempted >= g_opt.max_requests)) {
        if (g_inflight == 0) {
            ed_finish();
        } else if (inline_cb) {
            g_pause = 1;
        }
        return;
    }
    if (status == NB_ERR_NOTFOUND) {
        /* 尚未发现：稍后重试，避免忙转 */
        if (inline_cb) {
            g_pause = 1;
        }
        (void)nb_node_post_delay(g_node, 1, ed_issue, NULL);
        return;
    }
    if (!inline_cb) {
        ed_issue(NULL);
    }
}

static void ed_issue(void *user)
{
    (void)user;
    if (g_in_issue) {
        return;
    }
    g_in_issue = 1;
    for (;;) {
        uint64_t limit = g_window ? g_window : 1;
        nb_request_id_t id = 0;
        int rc;
        g_pause = 0;
        if (now_ns() >= g_deadline_ns ||
            (g_opt.max_requests != 0 && g_attempted >= g_opt.max_requests)) {
            if (g_inflight == 0) {
                ed_finish();
            }
            break;
        }
        if (g_inflight >= limit) {
            break; /* 在途额度已满，等响应回收后再补发 */
        }
        g_attempted++;
        g_inflight++;
        if (g_inflight > g_inflight_peak) {
            g_inflight_peak = g_inflight;
        }
        {
            nb_payload_t pl;
            nb_call_options_t call_opt = NB_CALL_OPTIONS_INIT;

            pl.data = g_payload;
            pl.len = g_opt.payload_bytes;
            pl.encoding = 0;
            call_opt.timeout_ms = g_opt.timeout_ms;
            call_opt.out_request_id = &id;
            rc = nb_proxy_call_async(g_proxy,
                                  g_opt.service,
                                  g_opt.method,
                                  &pl,
                                  &call_opt,
                                  ed_resp,
                                  (void *)(uintptr_t)now_ns());
        }
        if (rc != NB_OK) {
            g_failed++;
            error_add(g_errors, &g_error_count, rc);
            g_inflight--;
            (void)nb_node_post_delay(g_node, 1, ed_issue, NULL);
            break;
        }
        if (g_pause) {
            break; /* 回调内联执行并要求暂停（NOTFOUND / 已到期） */
        }
    }
    g_in_issue = 0;
}

int main(int argc, char **argv)
{
    options_t opt;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_binding_config_t dds_binding;
    nb_binding_config_t shm_binding;
    ntk_sem *sem = NULL;
    callout_t co;
    uint8_t *payload = NULL;
    error_entry_t errors[16];
    int error_count = 0;
    uint64_t attempted = 0, succeeded = 0, failed = 0, response_bytes = 0;
    uint64_t peak_inflight = 0;
    uint64_t min_ns = 0, max_ns = 0, sum_ns = 0, lat_count = 0;
    uint64_t start_ns, deadline_ns, end_ns;
    char *colon;
    char peer_host[64];
    char target_host[64];
    char rtps_target_buf[64];
    char dds_endpoint[32];
    char shm_endpoint[128];
    char shm_target[128];
    nb_binding_config_t rtps_binding;
    int i;
    int rc;
    nb_node_mode_t mode = NB_NODE_MODE_MESH;
    FILE *out = stdout;

    memset(&opt, 0, sizeof(opt));
    opt.service = "OrderService";
    opt.method = "get";
    opt.output = "-";
    opt.duration_seconds = 15;
    opt.payload_bytes = 16;
    opt.timeout_ms = 5000;
    opt.connect_timeout_seconds = 20;
    opt.io_threads = 1;
    opt.shm_channel = "nb-shm-bench";
    opt.bind = "0.0.0.0";

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--transport") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            opt.transport =
                (strcmp(t, "tcp") == 0 || strcmp(t, "TCP") == 0) ? NB_TRANSPORT_KIND_TCP : NB_TRANSPORT_KIND_UDP;
        } else if (strcmp(a, "--dds") == 0) {
            opt.use_dds = 1;
        } else if (strcmp(a, "--dds-domain") == 0 && i + 1 < argc) {
            opt.dds_domain = atoi(argv[++i]);
        } else if (strcmp(a, "--shm") == 0) {
            opt.use_shm = 1;
        } else if (strcmp(a, "--shm-channel") == 0 && i + 1 < argc) {
            opt.shm_channel = argv[++i];
        } else if (strcmp(a, "--rtps") == 0 && i + 1 < argc) {
            opt.use_rtps = 1;
            opt.rtps_endpoint = argv[++i];
        } else if (strcmp(a, "--rtps-target") == 0 && i + 1 < argc) {
            opt.rtps_target = argv[++i];
        } else if (strcmp(a, "--io-threads") == 0 && i + 1 < argc) {
            opt.io_threads = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--service") == 0 && i + 1 < argc) {
            opt.service = argv[++i];
        } else if (strcmp(a, "--mode") == 0 && i + 1 < argc) {
            mode = (strcmp(argv[++i], "static") == 0) ? NB_NODE_MODE_STATIC : NB_NODE_MODE_MESH;
        } else if (strcmp(a, "--method") == 0 && i + 1 < argc) {
            opt.method = argv[++i];
        } else if (strcmp(a, "--worker-id") == 0 && i + 1 < argc) {
            opt.worker_id = argv[++i];
        } else if (strcmp(a, "--output") == 0 && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (strcmp(a, "--duration-seconds") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.duration_seconds);
        } else if (strcmp(a, "--max-requests") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.max_requests);
        } else if (strcmp(a, "--payload-bytes") == 0 && i + 1 < argc) {
            parse_size(argv[++i], &opt.payload_bytes);
        } else if (strcmp(a, "--expect-response-bytes") == 0 && i + 1 < argc) {
            parse_size(argv[++i], &opt.expect_response_bytes);
        } else if (strcmp(a, "--timeout-ms") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.timeout_ms);
        } else if (strcmp(a, "--connect-timeout-seconds") == 0 && i + 1 < argc) {
            uint64_t v = 0;
            parse_u64(argv[++i], &v);
            opt.connect_timeout_seconds = (uint32_t)v;
        } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
            opt.rate = atof(argv[++i]);
        } else if (strcmp(a, "--window") == 0 && i + 1 < argc) {
            parse_u64(argv[++i], &opt.window);
        } else if (strcmp(a, "--sync") == 0) {
            opt.use_sync = 1;
        } else if (strcmp(a, "--verify-response") == 0) {
            opt.verify_response = 1;
        } else if (strcmp(a, "--discovery-port") == 0 && i + 1 < argc) {
            opt.discovery_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(a, "--data-port") == 0 && i + 1 < argc) {
            opt.data_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(a, "--max-msg-bytes") == 0 && i + 1 < argc) {
            opt.max_msg_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--backpressure-bytes") == 0 && i + 1 < argc) {
            opt.backpressure_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--frag-bytes") == 0 && i + 1 < argc) {
            opt.frag_bytes = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(a, "--bind") == 0 && i + 1 < argc) {
            opt.bind = argv[++i];
        } else if (strcmp(a, "--spike-log") == 0 && i + 1 < argc) {
            opt.spike_log = argv[++i];
        } else if (strcmp(a, "--spike-threshold-us") == 0 && i + 1 < argc) {
            uint64_t v = 0;
            parse_u64(argv[++i], &v);
            opt.spike_threshold_us = v;
        } else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
            char *pair = argv[++i];
            colon = strrchr(pair, ':');
            if (colon) {
                size_t hl = (size_t)(colon - pair);
                if (hl >= sizeof(peer_host)) {
                    hl = sizeof(peer_host) - 1;
                }
                memcpy(peer_host, pair, hl);
                peer_host[hl] = '\0';
                opt.peer_ip = peer_host;
                opt.peer_port = (uint16_t)atoi(colon + 1);
            }
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            char *pair = argv[++i];
            colon = strrchr(pair, ':');
            if (colon) {
                size_t hl = (size_t)(colon - pair);
                if (hl >= sizeof(target_host)) {
                    hl = sizeof(target_host) - 1;
                }
                memcpy(target_host, pair, hl);
                target_host[hl] = '\0';
                opt.target_ip = target_host;
                opt.target_port = (uint16_t)atoi(colon + 1);
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    /* Custom transports are matched by their binding host, so the proxy target
     * must reuse the RTPS authority (the domain id). */
    if (opt.use_rtps && !opt.rtps_target) {
        const char *sep = opt.rtps_endpoint ? strstr(opt.rtps_endpoint, "://") : NULL;
        const char *host = sep ? sep + 3 : opt.rtps_endpoint;
        size_t n = 0;

        while (host && host[n] && host[n] != ':' && host[n] != '/' &&
               n + 8 < sizeof(rtps_target_buf)) {
            n++;
        }
        if (n == 0) {
            nb_log_error("--rtps requires a binding URI");
            return 1;
        }
        snprintf(rtps_target_buf, sizeof(rtps_target_buf), "rtps://%.*s:0", (int)n, host);
        opt.rtps_target = rtps_target_buf;
    }

    g_spike_path = opt.spike_log;
    g_spike_enabled = opt.spike_log != NULL;
    if (opt.spike_threshold_us > 0) {
        g_spike_threshold_ns = opt.spike_threshold_us * 1000ull;
    }

    nb_node_config_default(&cfg);
    cfg.name = opt.worker_id ? opt.worker_id : "loadgen";
    cfg.transport.kind = opt.transport;
    cfg.transport.bind_host = opt.bind;
    cfg.transport.data_port = opt.data_port;
    cfg.discovery.broadcast_host = "255.255.255.255";
    cfg.discovery.port = opt.discovery_port;
    cfg.mode = mode;
    cfg.runtime.io_threads = opt.io_threads ? opt.io_threads : 1;
    cfg.discovery.hello_interval_ms = 100;
    cfg.discovery.node_ttl_ms = 3000;
    cfg.transport.frag_bytes = opt.frag_bytes;
    cfg.transport.max_msg_bytes = opt.max_msg_bytes;
    cfg.transport.backpressure_bytes = opt.backpressure_bytes;

    if ((opt.use_shm ? 1 : 0) + (opt.use_dds ? 1 : 0) + (opt.use_rtps ? 1 : 0) > 1) {
        nb_log_error("--shm, --dds and --rtps are mutually exclusive");
        return 1;
    }
    if (opt.use_shm) {
        if (nb_shm_register() != NB_OK) {
            nb_log_error("shm provider register failed");
            return 1;
        }
        snprintf(shm_endpoint, sizeof(shm_endpoint), "shm://%s:2", opt.shm_channel);
        snprintf(shm_target, sizeof(shm_target), "shm://%s:1", opt.shm_channel);
        memset(&shm_binding, 0, sizeof(shm_binding));
        shm_binding.name = "shm";
        shm_binding.endpoint = shm_endpoint;
        shm_binding.intra_process = NB_INTRA_PROCESS_OFF;
        cfg.transport.bindings = &shm_binding;
        cfg.transport.binding_count = 1;
        cfg.mode = NB_NODE_MODE_STATIC;
    } else if (opt.use_dds) {
        if (nb_cyclone_dds_register() != NB_OK) {
            nb_log_error("dds provider register failed");
            return 1;
        }
        snprintf(dds_endpoint, sizeof(dds_endpoint), "dds://%d", opt.dds_domain);
        memset(&dds_binding, 0, sizeof(dds_binding));
        dds_binding.name = "dds";
        dds_binding.endpoint = dds_endpoint;
        dds_binding.intra_process = NB_INTRA_PROCESS_OFF;
        cfg.transport.bindings = &dds_binding;
        cfg.transport.binding_count = 1;
        cfg.mode = NB_NODE_MODE_STATIC;
    } else if (opt.use_rtps) {
        if (nb_rtps_transport_register() != NB_OK) {
            nb_log_error("rtps provider register failed");
            return 1;
        }
        memset(&rtps_binding, 0, sizeof(rtps_binding));
        rtps_binding.name = "rtps";
        rtps_binding.endpoint = opt.rtps_endpoint;
        rtps_binding.intra_process = NB_INTRA_PROCESS_OFF;
        cfg.transport.bindings = &rtps_binding;
        cfg.transport.binding_count = 1;
        cfg.mode = NB_NODE_MODE_STATIC;
    }

    sem = ntk_sem_create(0);
    if (!sem) {
        nb_log_error("sem create failed");
        return 1;
    }
    if (opt.payload_bytes > 0) {
        payload = (uint8_t *)malloc(opt.payload_bytes);
        if (!payload) {
            return 1;
        }
        for (i = 0; i < (int)opt.payload_bytes; i++) {
            payload[i] = (uint8_t)('a' + (i % 26));
        }
    }

    rc = nb_node_create(&cfg, &node);
    if (rc != NB_OK) {
        nb_log_error("node create failed: %d", rc);
        return 1;
    }
    if (opt.peer_ip) {
        (void)nb_node_add_discovery_peer(node, opt.peer_ip, opt.peer_port);
    }
    rc = nb_proxy_create(node, NULL, &proxy);
    if (rc != NB_OK) {
        nb_log_error("proxy create failed: %d", rc);
        return 1;
    }
    if (opt.use_shm) {
        rc = nb_proxy_set_target_uri(proxy, shm_target);
        if (rc != NB_OK) {
            nb_log_error("proxy set shm endpoint failed: %d", rc);
            nb_node_destroy(node);
            return 1;
        }
    } else if (opt.use_dds) {
        rc = nb_proxy_set_target_uri(proxy, dds_endpoint);
        if (rc != NB_OK) {
            nb_log_error("proxy set dds endpoint failed: %d", rc);
            nb_node_destroy(node);
            return 1;
        }
    } else if (opt.use_rtps) {
        rc = nb_proxy_set_target_uri(proxy, opt.rtps_target);
        if (rc != NB_OK) {
            nb_log_error("proxy set rtps endpoint failed: %d", rc);
            nb_node_destroy(node);
            return 1;
        }
    } else if (opt.target_ip && opt.target_port) {
        (void)nb_proxy_set_target(proxy, opt.target_ip, opt.target_port, nb_node_transport(node));
    }

    memset(&co, 0, sizeof(co));
    co.sem = sem;

    /* MESH：先等目标服务进入路由表（启动等待），再做预热。 */
    if (!opt.use_shm && !opt.use_dds && !opt.use_rtps &&
        !(opt.target_ip && opt.target_port)) {
        (void)nb_node_wait_for_service(
            node, opt.service, (uint64_t)opt.connect_timeout_seconds * 1000ull);
    }

    /* 预热：等发现收敛（NOTFOUND 视为尚未发现，继续重试）。 */
    {
        uint64_t wdeadline = now_ns() + (uint64_t)opt.connect_timeout_seconds * 1000000000ull;
        nb_request_id_t id = 0;
        int warmed = 0;
        while (now_ns() < wdeadline) {
            nb_payload_t pl;
            nb_call_options_t call_opt = NB_CALL_OPTIONS_INIT;

            pl.data = payload;
            pl.len = opt.payload_bytes;
            pl.encoding = 0;
            co.status = -999;
            co.send_ns = now_ns();
            call_opt.timeout_ms = opt.timeout_ms;
            call_opt.out_request_id = &id;
            rc = nb_proxy_call_async(
                proxy, opt.service, opt.method, &pl, &call_opt, on_response, &co);
            if (rc != NB_OK) {
                nb_log_error("nb_proxy_call_async failed: %d", rc);
                nb_node_destroy(node);
                return 1;
            }
            (void)ntk_sem_timedwait(sem, (unsigned)(opt.timeout_ms + 100));
            /* DDS discovery may still be converging: require a real reply. */
            if (co.status != NB_ERR_NOTFOUND &&
                (!(opt.use_dds || opt.use_shm || opt.use_rtps) || co.status == NB_OK)) {
                warmed = 1;
                break;
            }
            ntk_sleep_ms(20);
        }
        if (!warmed) {
            g_status = 1; /* discovery_error */
        }
    }

    memset(g_lat_hist, 0, sizeof(g_lat_hist));
    start_ns = now_ns();
    g_spike_t0_ns = start_ns;
    deadline_ns = start_ns + opt.duration_seconds * 1000000000ull;
    if (opt.verify_response) {
        g_expect_data = payload;
        g_expect_len = opt.payload_bytes;
    }

    if (g_status == 0) {
        if (opt.use_sync) {
            uint64_t index = 0;
            while (now_ns() < deadline_ns &&
                   (opt.max_requests == 0 || attempted < opt.max_requests)) {
                nb_payload_t pl;
                nb_response_t response;
                uint64_t send_ns;
                uint64_t latency_ns;
                uint64_t us;
                uint64_t hidx;
                int verify_ok = 1;

                if (opt.rate > 0.0) {
                    uint64_t target =
                        start_ns + (uint64_t)((double)index * 1000000000.0 / opt.rate);
                    sleep_until_ns(target);
                }

                memset(&response, 0, sizeof(response));
                pl.data = payload;
                pl.len = opt.payload_bytes;
                pl.encoding = 0;
                send_ns = now_ns();
                {
                    nb_call_options_t call_opt = NB_CALL_OPTIONS_INIT;

                    call_opt.timeout_ms = opt.timeout_ms;
                    rc = nb_proxy_call_sync(
                        proxy, opt.service, opt.method, &pl, &call_opt, &response);
                }
                latency_ns = now_ns() - send_ns;
                spike_record(send_ns, latency_ns);
                attempted++;
                peak_inflight = 1;

                if (rc == NB_OK && opt.verify_response) {
                    verify_ok =
                        response.payload.len == opt.payload_bytes &&
                        (opt.payload_bytes == 0 ||
                         memcmp(response.payload.data, payload, opt.payload_bytes) == 0);
                }
                us = (latency_ns + 999ull) / 1000ull;
                hidx = (uint64_t)lat_hist_index(us);
                g_lat_hist[hidx]++;

                if (rc == NB_OK && response.status == NB_OK && verify_ok &&
                    (opt.expect_response_bytes == 0 ||
                     response.payload.len == opt.expect_response_bytes)) {
                    succeeded++;
                    response_bytes += response.payload.len;
                    if (lat_count == 0 || latency_ns < min_ns) {
                        min_ns = latency_ns;
                    }
                    if (latency_ns > max_ns) {
                        max_ns = latency_ns;
                    }
                    sum_ns += latency_ns;
                    lat_count++;
                } else {
                    failed++;
                    error_add(errors,
                              &error_count,
                              rc != NB_OK ? rc
                                          : (response.status != NB_OK ? response.status
                                                                      : NB_ERR_INVALID));
                }
                nb_response_release(&response);
                index++;
            }
        } else if (opt.rate > 0.0) {
            uint64_t index = 0;
            while (now_ns() < deadline_ns &&
                   (opt.max_requests == 0 || attempted < opt.max_requests)) {
                nb_request_id_t id = 0;
                uint64_t us;
                uint64_t target = start_ns + (uint64_t)((double)index * 1000000000.0 / opt.rate);
                nb_payload_t pl;
                sleep_until_ns(target);
                pl.data = payload;
                pl.len = opt.payload_bytes;
                pl.encoding = 0;
                co.status = -999;
                co.resp_len = 0;
                co.verify_ok = 1;
                co.send_ns = now_ns();
                {
                    nb_call_options_t call_opt = NB_CALL_OPTIONS_INIT;

                    call_opt.timeout_ms = opt.timeout_ms;
                    call_opt.out_request_id = &id;
                    rc = nb_proxy_call_async(
                        proxy, opt.service, opt.method, &pl, &call_opt, on_response, &co);
                }
                attempted++;
                if (rc != NB_OK) {
                    failed++;
                    error_add(errors, &error_count, rc);
                    index++;
                    continue;
                }
                if (ntk_sem_timedwait(sem, (unsigned)(opt.timeout_ms + 100)) != NTK_OK) {
                    failed++;
                    error_add(errors, &error_count, NB_ERR_TIMEOUT);
                    index++;
                    continue;
                }
                us = (co.latency_ns + 999ull) / 1000ull;
                {
                    uint64_t hidx = (uint64_t)lat_hist_index(us);
                    g_lat_hist[hidx]++;
                }
                if (co.status == NB_OK && co.app_status == NB_OK && co.verify_ok &&
                    (opt.expect_response_bytes == 0 || co.resp_len == opt.expect_response_bytes)) {
                    succeeded++;
                    response_bytes += co.resp_len;
                    if (lat_count == 0 || co.latency_ns < min_ns) {
                        min_ns = co.latency_ns;
                    }
                    if (co.latency_ns > max_ns) {
                        max_ns = co.latency_ns;
                    }
                    sum_ns += co.latency_ns;
                    lat_count++;
                } else {
                    failed++;
                    error_add(errors, &error_count, co.status != NB_OK ? co.status : co.app_status);
                }
                index++;
            }
        } else {
            /* 事件驱动：全程在 poller 线程内联 ping-pong */
            g_node = node;
            g_proxy = proxy;
            g_opt = opt;
            g_payload = payload;
            g_deadline_ns = deadline_ns;
            g_attempted = g_succeeded = g_failed = g_response_bytes = 0;
            g_min_ns = g_max_ns = g_sum_ns = g_lat_count = 0;
            g_error_count = 0;
            g_window = opt.window;
            g_inflight = 0;
            g_inflight_peak = 0;
            g_finished = 0;
            g_done = ntk_sem_create(0);
            if (g_done) {
                (void)nb_node_post(node, ed_issue, NULL);
                (void)ntk_sem_timedwait(
                    g_done, (unsigned)(opt.duration_seconds * 1000 + opt.timeout_ms + 2000));
                ntk_sem_destroy(g_done);
                g_done = NULL;
            }
            attempted = g_attempted;
            succeeded = g_succeeded;
            failed = g_failed;
            response_bytes = g_response_bytes;
            min_ns = g_min_ns;
            max_ns = g_max_ns;
            sum_ns = g_sum_ns;
            lat_count = g_lat_count;
            memcpy(errors, g_errors, sizeof(errors));
            error_count = g_error_count;
            peak_inflight = g_inflight_peak;
        }
    }
    end_ns = now_ns();

    if (!opt.output || strcmp(opt.output, "-") == 0) {
        out = stdout;
    } else {
        out = fopen(opt.output, "w");
        if (!out) {
            out = stdout;
        }
    }

    {
        double duration = (double)(end_ns - start_ns) / 1000000000.0;
        const char *status = (g_status == 1) ? "discovery_error" : (failed == 0 ? "ok" : "error");
        fprintf(out, "{");
        fprintf(out, "\"status\":\"%s\",", status);
        fprintf(out, "\"worker_id\":\"%s\",", opt.worker_id ? opt.worker_id : "");
        fprintf(out, "\"service\":\"%s\",\"method\":\"%s\",", opt.service, opt.method);
        fprintf(out, "\"sync\":%s,", opt.use_sync ? "true" : "false");
        fprintf(out,
                "\"attempted\":%llu,\"succeeded\":%llu,\"failed\":%llu,",
                (unsigned long long)attempted,
                (unsigned long long)succeeded,
                (unsigned long long)failed);
        fprintf(out, "\"peak_inflight\":%llu,", (unsigned long long)peak_inflight);
        fprintf(out, "\"slow_samples\":%u,\"slow_threshold_us\":%.0f,", g_slow_total,
                (double)g_spike_threshold_ns / 1000.0);
        fprintf(out, "\"response_bytes\":%llu,", (unsigned long long)response_bytes);
        fprintf(out,
                "\"expected_response_bytes\":%llu,",
                (unsigned long long)opt.expect_response_bytes);
        fprintf(out, "\"duration_seconds\":%.6f,", duration);
        fprintf(out, "\"errors\":{");
        for (i = 0; i < error_count; i++) {
            fprintf(out,
                    "%s\"%d\":%llu",
                    i ? "," : "",
                    errors[i].code,
                    (unsigned long long)errors[i].count);
        }
        fprintf(out, "},");
        fprintf(out,
                "\"latency_us\":{\"min\":%.1f,\"mean\":%.1f,\"max\":%.1f},",
                lat_count ? (double)min_ns / 1000.0 : 0.0,
                lat_count ? (double)sum_ns / (double)lat_count / 1000.0 : 0.0,
                lat_count ? (double)max_ns / 1000.0 : 0.0);
        fprintf(out, "\"latency_histogram_us\":{");
        {
            int first = 1;
            for (i = 0; i < LAT_HIST_SIZE; i++) {
                if (g_lat_hist[i]) {
                    fprintf(out,
                            "%s\"%llu\":%llu",
                            first ? "" : ",",
                            (unsigned long long)lat_hist_key(i),
                            (unsigned long long)g_lat_hist[i]);
                    first = 0;
                }
            }
        }
        {
            nb_metrics_t metrics;
            memset(&metrics, 0, sizeof(metrics));
            if (node) {
                nb_node_metrics(node, &metrics);
            }
            fprintf(out,
                    "},\"metrics\":{\"requests_sent\":%llu,\"responses_received\":%llu,"
                    "\"requests_failed\":%llu,\"timeouts\":%llu,\"retries\":%llu,"
                    "\"bytes_sent\":%llu,\"bytes_received\":%llu,"
                    "\"udp_packets_sent\":%llu,\"udp_packets_received\":%llu,"
                    "\"udp_retransmissions\":%llu,\"udp_nacks_sent\":%llu,"
                    "\"udp_reassembly_expired\":%llu,\"tcp_frames_sent\":%llu,"
                    "\"tcp_frames_received\":%llu,"
                    "\"rtcp_sr_sent\":%llu,\"rtcp_rr_sent\":%llu,"
                    "\"rtcp_sr_received\":%llu,\"rtcp_rr_received\":%llu,"
                    "\"rtcp_rtt_ms\":%u,\"rtcp_jitter_ms\":%u,\"rtcp_remote_lost\":%d,"
                    "\"rtps_packets_sent\":%llu,\"rtps_packets_received\":%llu,"
                    "\"rtps_data_sent\":%llu,\"rtps_data_received\":%llu,"
                    "\"rtps_heartbeats_sent\":%llu,\"rtps_heartbeats_received\":%llu,"
                    "\"rtps_acknacks_sent\":%llu,\"rtps_acknacks_received\":%llu}}\n",
                    (unsigned long long)metrics.requests_sent,
                    (unsigned long long)metrics.responses_received,
                    (unsigned long long)metrics.requests_failed,
                    (unsigned long long)metrics.timeouts,
                    (unsigned long long)metrics.retries,
                    (unsigned long long)metrics.bytes_sent,
                    (unsigned long long)metrics.bytes_received,
                    (unsigned long long)metrics.udp_packets_sent,
                    (unsigned long long)metrics.udp_packets_received,
                    (unsigned long long)metrics.udp_retransmissions,
                    (unsigned long long)metrics.udp_nacks_sent,
                    (unsigned long long)metrics.udp_reassembly_expired,
                    (unsigned long long)metrics.tcp_frames_sent,
                    (unsigned long long)metrics.tcp_frames_received,
                    (unsigned long long)metrics.rtcp_sr_sent,
                    (unsigned long long)metrics.rtcp_rr_sent,
                    (unsigned long long)metrics.rtcp_sr_received,
                    (unsigned long long)metrics.rtcp_rr_received,
                    (unsigned)metrics.rtcp_rtt_ms,
                    (unsigned)metrics.rtcp_jitter_ms,
                    (int)metrics.rtcp_remote_lost,
                    (unsigned long long)metrics.rtps_packets_sent,
                    (unsigned long long)metrics.rtps_packets_received,
                    (unsigned long long)metrics.rtps_data_sent,
                    (unsigned long long)metrics.rtps_data_received,
                    (unsigned long long)metrics.rtps_heartbeats_sent,
                    (unsigned long long)metrics.rtps_heartbeats_received,
                    (unsigned long long)metrics.rtps_acknacks_sent,
                    (unsigned long long)metrics.rtps_acknacks_received);
        }
    }

    if (out != stdout) {
        fclose(out);
    }
    spike_log_write();
    free(payload);
    ntk_sem_destroy(sem);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return 0;
}
