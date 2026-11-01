/*
 * 配置加载：模型驱动的 JSON 解析。
 *
 * 配置按模块拆成子对象（runtime / transport / discovery / metrics / log），
 * 由 codec_json 的 model 描述字段布局后统一填充；缺省值由 defaults.h 与
 * nb_node_config_normalize 收敛，未知字段忽略，已知字段非法时返回带路径的错误。
 */
#include <nb/config.h>
#include <nb/node.h>

#define NB_LOG_TAG "config"
#include <nb/log.h>

#include "cJSON.h"
#include "codec_json_internal.h"
#include "defaults.h"

#include <ntk.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nb_cfg_copy(char *dst, size_t cap, const char *src, const char *def);
static void nb_config_doc_rebind(nb_config_doc_t *doc);

/* Opaque in the public header; fully defined here so the parser can build the
 * self-referential view internally without exposing it to callers. */
struct nb_config_doc {
    nb_node_config_t node; /* pointers refer to buffers in this document */
    char name[NB_NAME_MAX];
    char bind_host[NB_HOST_MAX];
    char discovery_host[NB_HOST_MAX];
    nb_static_route_t routes[NB_CONFIG_MAX_ROUTES];
    size_t route_count;
    nb_peer_endpoint_t peers[NB_CONFIG_MAX_PEERS];
    size_t peer_count;
    nb_binding_endpoint_t bindings[NB_CONFIG_MAX_BINDINGS];
    nb_binding_config_t binding_configs[NB_CONFIG_MAX_BINDINGS];
    size_t binding_count;
};

static int nb_cfg_fail_code(nb_config_error_t *err, int code, const char *path, const char *reason)
{
    if (err) {
        err->code = code;
        snprintf(err->path, sizeof(err->path), "%s", path ? path : "");
        snprintf(err->reason, sizeof(err->reason), "%s", reason ? reason : "");
    }
    return code;
}

static int nb_cfg_fail(nb_config_error_t *err, const char *path, const char *reason)
{
    return nb_cfg_fail_code(err, NB_ERR_INVALID, path, reason);
}

static const char *nb_cfg_codec_reason(int status)
{
    switch (status) {
    case NB_CODEC_TYPE_MISMATCH:
        return "type mismatch";
    case NB_CODEC_LIMIT_EXCEEDED:
        return "value out of range";
    case NB_CODEC_MALFORMED:
        return "malformed value";
    case NB_CODEC_OUT_OF_MEMORY:
        return "out of memory";
    case NB_CODEC_INVALID_ARGUMENT:
        return "invalid argument";
    default:
        return "invalid value";
    }
}

/* ---- QoS 子对象 ---- */

/* 拼接诊断路径：base 为空时只用 key，否则 "base.key"。 */
static void nb_cfg_join(char *dst, size_t cap, const char *base, const char *key)
{
    if (base && base[0]) {
        snprintf(dst, cap, "%s.%s", base, key);
    } else {
        snprintf(dst, cap, "%s", key ? key : "");
    }
}

static int nb_cfg_qos_enum(const cJSON *qos,
                           const char *key,
                           int *out,
                           const char *const *names,
                           const int *values,
                           size_t count,
                           nb_config_error_t *err,
                           const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(qos, key);
    char full[96];
    size_t i;

    nb_cfg_join(full, sizeof(full), path, key);
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return nb_cfg_fail(err, full, "must be a string");
    }
    for (i = 0; i < count; i++) {
        if (strcmp(item->valuestring, names[i]) == 0) {
            *out = values[i];
            return NB_OK;
        }
    }
    return nb_cfg_fail(err, full, "invalid value");
}

static int nb_cfg_qos_u32(const cJSON *qos,
                          const char *key,
                          uint32_t *out,
                          nb_config_error_t *err,
                          const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(qos, key);
    char full[96];
    double d;

    nb_cfg_join(full, sizeof(full), path, key);
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsNumber(item)) {
        return nb_cfg_fail(err, full, "must be a non-negative integer");
    }
    d = item->valuedouble;
    /* 与 codec 的 U32 语义一致：拒绝小数，而不是静默截断。 */
    if (d != d || d < 0.0 || d > 4294967295.0 || (double)(uint64_t)d != d) {
        return nb_cfg_fail(err, full, "must be a non-negative integer");
    }
    *out = (uint32_t)d;
    return NB_OK;
}

static int nb_cfg_qos_bool(const cJSON *qos,
                           const char *key,
                           bool *out,
                           nb_config_error_t *err,
                           const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(qos, key);
    char full[96];

    nb_cfg_join(full, sizeof(full), path, key);
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsBool(item)) {
        return nb_cfg_fail(err, full, "must be a boolean");
    }
    *out = cJSON_IsTrue(item) ? true : false;
    return NB_OK;
}

static int nb_cfg_load_qos(const cJSON *parent,
                           nb_qos_t *out,
                           nb_config_error_t *err,
                           const char *path)
{
    static const char *const reliability_names[] = {"best_effort", "reliable"};
    static const int reliability_values[] = {NB_QOS_BEST_EFFORT, NB_QOS_RELIABLE};
    static const char *const durability_names[] = {"volatile", "transient_local"};
    static const int durability_values[] = {NB_QOS_VOLATILE, NB_QOS_TRANSIENT_LOCAL};
    static const char *const history_names[] = {"keep_last", "keep_all"};
    static const int history_values[] = {NB_QOS_KEEP_LAST, NB_QOS_KEEP_ALL};
    static const char *const liveliness_names[] = {
        "automatic", "manual_by_participant", "manual_by_topic"};
    static const int liveliness_values[] = {
        NB_QOS_LIVELINESS_AUTOMATIC,
        NB_QOS_LIVELINESS_MANUAL_BY_PARTICIPANT,
        NB_QOS_LIVELINESS_MANUAL_BY_TOPIC};
    static const char *const ownership_names[] = {"shared", "exclusive"};
    static const int ownership_values[] = {NB_QOS_OWNERSHIP_SHARED, NB_QOS_OWNERSHIP_EXCLUSIVE};
    static const char *const presentation_names[] = {"instance", "topic", "group"};
    static const int presentation_values[] = {NB_QOS_PRESENTATION_INSTANCE,
                                              NB_QOS_PRESENTATION_TOPIC,
                                              NB_QOS_PRESENTATION_GROUP};
    const cJSON *qos;
    const cJSON *partition;
    int rc;

    if (!parent || !out) {
        return NB_ERR_INVALID;
    }
    qos = cJSON_GetObjectItemCaseSensitive(parent, "qos");
    if (!qos) {
        return NB_OK;
    }
    if (!cJSON_IsObject(qos)) {
        return nb_cfg_fail(err, path, "must be an object");
    }
    nb_qos_init(out);
    rc = nb_cfg_qos_enum(qos,
                         "reliability",
                         (int *)&out->reliability,
                         reliability_names,
                         reliability_values,
                         2,
                         err,
                         path);
    if (rc == NB_OK) {
        rc = nb_cfg_qos_enum(qos,
                             "durability",
                             (int *)&out->durability,
                             durability_names,
                             durability_values,
                             2,
                             err,
                             path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_enum(
            qos, "history", (int *)&out->history, history_names, history_values, 2, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_enum(qos,
                             "liveliness",
                             (int *)&out->liveliness,
                             liveliness_names,
                             liveliness_values,
                             3,
                             err,
                             path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_enum(
            qos, "ownership", (int *)&out->ownership, ownership_names, ownership_values, 2, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "depth", &out->depth, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "deadline_ms", &out->deadline_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "lifespan_ms", &out->lifespan_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "lease_duration_ms", &out->lease_duration_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "latency_budget_ms", &out->latency_budget_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "min_separation_ms", &out->min_separation_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "max_samples", &out->max_samples, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(qos, "max_instances", &out->max_instances, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_u32(
            qos, "max_samples_per_instance", &out->max_samples_per_instance, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_bool(qos,
                             "autodispose_unregistered_instances",
                             &out->autodispose_unregistered_instances,
                             err,
                             path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_enum(qos,
                             "presentation_scope",
                             (int *)&out->presentation_scope,
                             presentation_names,
                             presentation_values,
                             3,
                             err,
                             path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_bool(qos, "coherent_access", &out->coherent_access, err, path);
    }
    if (rc == NB_OK) {
        rc = nb_cfg_qos_bool(qos, "ordered_access", &out->ordered_access, err, path);
    }
    if (rc != NB_OK) {
        return rc;
    }
    partition = cJSON_GetObjectItemCaseSensitive(qos, "partition");
    if (partition) {
        char full[96];
        nb_cfg_join(full, sizeof(full), path, "partition");
        if (!cJSON_IsString(partition) || !partition->valuestring) {
            return nb_cfg_fail(err, full, "must be a string");
        }
        if (nb_cfg_copy(out->partition, sizeof(out->partition), partition->valuestring, NULL) !=
            NB_OK) {
            return nb_cfg_fail(err, full, "value too long");
        }
    }
    if (nb_qos_validate(out) != NB_OK) {
        return nb_cfg_fail(err, path, nb_qos_validate_reason(out));
    }
    return NB_OK;
}

/* ---- 默认值 ---- */

void nb_node_runtime_config_default(nb_node_runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void nb_node_transport_config_default(nb_node_transport_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind = NB_TRANSPORT_KIND_UDP;
    cfg->bind_host = NB_DEFAULT_BIND_HOST;
    nb_qos_init(&cfg->qos);
}

void nb_node_discovery_config_default(nb_node_discovery_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->broadcast_host = NB_DEFAULT_DISCOVERY_HOST;
}

void nb_node_metrics_config_default(nb_node_metrics_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_every = NB_DEFAULT_METRICS_SAMPLE_EVERY;
}

void nb_node_log_config_default(nb_node_log_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->level = NB_LOG_LEVEL_INFO;
}

void nb_node_config_default(nb_node_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->name = NB_DEFAULT_NODE_NAME;
    cfg->mode = NB_NODE_MODE_MESH;
    nb_node_runtime_config_default(&cfg->runtime);
    nb_node_transport_config_default(&cfg->transport);
    nb_node_discovery_config_default(&cfg->discovery);
    nb_node_metrics_config_default(&cfg->metrics);
    nb_node_log_config_default(&cfg->log);
}

void nb_binding_config_default(nb_binding_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->intra_process = NB_INTRA_PROCESS_OFF;
    nb_qos_init(&cfg->qos);
}

void nb_node_config_normalize(nb_node_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    if (!cfg->name) {
        cfg->name = NB_DEFAULT_NODE_NAME;
    }
    if (!cfg->transport.bind_host) {
        cfg->transport.bind_host = NB_DEFAULT_BIND_HOST;
    }
    if (!cfg->discovery.broadcast_host) {
        cfg->discovery.broadcast_host = NB_DEFAULT_DISCOVERY_HOST;
    }
    /*
     * 顶层 mode 与 discovery.disable 是同一件事的两种写法，这里收敛为一个
     * 一致状态，避免出现“mode=mesh 但 discovery.disable=true”这种自相矛盾。
     */
    if (cfg->mode == NB_NODE_MODE_STATIC) {
        cfg->discovery.disable = true;
    } else if (cfg->discovery.disable) {
        cfg->mode = NB_NODE_MODE_STATIC;
    }
    if (!cfg->runtime.io_threads) {
        unsigned hw = ntk_thread_hardware_concurrency();
        cfg->runtime.io_threads = hw ? hw : 1u;
    }
    if (cfg->runtime.io_threads > NB_TRANSPORT_MAX_IO) {
        cfg->runtime.io_threads = NB_TRANSPORT_MAX_IO;
    }
    if (cfg->runtime.handler_threads > NB_MAX_HANDLER_THREADS) {
        cfg->runtime.handler_threads = NB_MAX_HANDLER_THREADS;
    }
    if (cfg->runtime.send_lanes > NB_MAX_SEND_LANES) {
        cfg->runtime.send_lanes = NB_MAX_SEND_LANES;
    }
    if (!cfg->transport.frag_bytes) {
        cfg->transport.frag_bytes = NB_DEFAULT_FRAG_BYTES;
    } else if (cfg->transport.frag_bytes < NB_DEFAULT_FRAG_MIN_BYTES) {
        cfg->transport.frag_bytes = NB_DEFAULT_FRAG_MIN_BYTES;
    } else if (cfg->transport.frag_bytes > NB_DEFAULT_FRAG_MAX_BYTES) {
        cfg->transport.frag_bytes = NB_DEFAULT_FRAG_MAX_BYTES;
    }
    if (!cfg->transport.max_msg_bytes) {
        cfg->transport.max_msg_bytes = NB_DEFAULT_MAX_MSG_BYTES;
    } else if (cfg->transport.max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
        cfg->transport.max_msg_bytes = NB_GLOBAL_MAX_MSG_BYTES_LIMIT;
    }
    if (cfg->transport.max_msg_bytes < cfg->transport.frag_bytes) {
        cfg->transport.max_msg_bytes = cfg->transport.frag_bytes;
    }
    if (!cfg->transport.backpressure_bytes) {
        cfg->transport.backpressure_bytes = NB_DEFAULT_BACKPRESSURE_BYTES;
    }
    if (!cfg->transport.rtcp_interval_ms) {
        cfg->transport.rtcp_interval_ms = NB_DEFAULT_RTCP_INTERVAL_MS;
    }
    if (!cfg->discovery.port) {
        cfg->discovery.port = NB_DEFAULT_DISCOVERY_PORT;
    }
    if (!cfg->discovery.hello_interval_ms) {
        cfg->discovery.hello_interval_ms = NB_DEFAULT_HELLO_INTERVAL_MS;
    }
    if (!cfg->discovery.node_ttl_ms) {
        cfg->discovery.node_ttl_ms = NB_DEFAULT_NODE_TTL_MS;
    }
    if (!cfg->runtime.max_inflight) {
        cfg->runtime.max_inflight = NB_DEFAULT_MAX_INFLIGHT;
    }
    if (!cfg->runtime.max_streams) {
        cfg->runtime.max_streams = NB_DEFAULT_MAX_STREAMS;
    } else if (cfg->runtime.max_streams > NB_STREAM_MAX_RECV_STREAMS) {
        cfg->runtime.max_streams = NB_STREAM_MAX_RECV_STREAMS;
    }
    if (!cfg->metrics.sample_every) {
        cfg->metrics.sample_every = NB_DEFAULT_METRICS_SAMPLE_EVERY;
    }
    /*
     * log.level 0 is a valid NB_LOG_LEVEL_ASSERT and is never treated as
     * "unset". Use NB_LOG_LEVEL_DEFAULT to request INFO from an otherwise
     * zero-initialized configuration.
     */
    if (cfg->log.level == NB_LOG_LEVEL_DEFAULT ||
        cfg->log.level < NB_LOG_LEVEL_ASSERT || cfg->log.level > NB_LOG_LEVEL_VERBOSE) {
        cfg->log.level = NB_LOG_LEVEL_INFO;
    }
    /* log.level: 0 是合法的 NB_LOG_LEVEL_ASSERT，不能当作“未设置”。 */
}

/* ---- 模型驱动的解析中间结构 ---- */

int nb_node_config_validate(const nb_node_config_t *cfg)
{
    size_t i;

    if (!cfg) {
        return NB_ERR_INVALID;
    }
    if (!cfg->transport.bind_host || !cfg->transport.bind_host[0]) {
        return NB_ERR_INVALID;
    }
    if (strlen(cfg->transport.bind_host) >= NB_HOST_MAX ||
        (cfg->name && strlen(cfg->name) >= NB_NAME_MAX) ||
        (cfg->discovery.broadcast_host && strlen(cfg->discovery.broadcast_host) >= NB_HOST_MAX)) {
        return NB_ERR_TOOBIG;
    }
    if (cfg->runtime.io_threads > NB_TRANSPORT_MAX_IO ||
        cfg->runtime.handler_threads > NB_MAX_HANDLER_THREADS ||
        cfg->runtime.send_lanes > NB_MAX_SEND_LANES ||
        cfg->runtime.max_streams > NB_STREAM_MAX_RECV_STREAMS) {
        return NB_ERR_INVALID;
    }
    if (cfg->transport.frag_bytes != 0 &&
        (cfg->transport.frag_bytes < NB_DEFAULT_FRAG_MIN_BYTES ||
         cfg->transport.frag_bytes > NB_DEFAULT_FRAG_MAX_BYTES)) {
        return NB_ERR_INVALID;
    }
    if (cfg->transport.max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
        return NB_ERR_INVALID;
    }
    if (cfg->transport.max_msg_bytes != 0 && cfg->transport.frag_bytes != 0 &&
        cfg->transport.max_msg_bytes < cfg->transport.frag_bytes) {
        return NB_ERR_INVALID;
    }
    if (cfg->log.level != NB_LOG_LEVEL_DEFAULT &&
        (cfg->log.level < NB_LOG_LEVEL_ASSERT || cfg->log.level > NB_LOG_LEVEL_VERBOSE)) {
        return NB_ERR_INVALID;
    }
    if (cfg->transport.binding_count > 0 && !cfg->transport.bindings) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < cfg->transport.binding_count; i++) {
        if (nb_qos_validate(&cfg->transport.bindings[i].qos) != NB_OK) {
            return NB_ERR_INVALID;
        }
    }
    return NB_OK;
}

typedef struct nb_cfg_runtime_in {
    uint32_t io_threads;
    uint32_t handler_threads;
    uint32_t handler_max_concurrency;
    uint32_t max_inflight;
    uint32_t max_streams;
    uint32_t peer_rate_limit_pps;
    uint32_t send_lanes;
} nb_cfg_runtime_in_t;

typedef struct nb_cfg_transport_in {
    char *kind;
    char *bind_host;
    uint32_t data_port;
    uint32_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t backpressure_bytes;
    uint32_t rtcp_interval_ms;
    uint8_t disable_rtcp;
} nb_cfg_transport_in_t;

typedef struct nb_cfg_discovery_in {
    char *mode;
    char *broadcast;
    uint32_t hello_interval_ms;
    uint32_t node_ttl_ms;
} nb_cfg_discovery_in_t;

typedef struct nb_cfg_metrics_in {
    uint8_t disable;
    uint8_t disable_latency_histogram;
    uint32_t sample_every;
} nb_cfg_metrics_in_t;

typedef struct nb_cfg_log_in {
    uint8_t disable;
    char *level;
} nb_cfg_log_in_t;

typedef struct nb_cfg_route_in {
    char *service;
    char *endpoint;
} nb_cfg_route_in_t;

typedef struct nb_cfg_binding_in {
    char *name;
    char *endpoint;
    char *intra_process;
    uint32_t poll_us;
} nb_cfg_binding_in_t;

/* 用显式结构体字段偏移描述每个子对象，交给 codec_json 填充。 */
static const nb_codec_json_model_t nb_g_runtime_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_runtime_in_t) } },
    { NB_CODEC_JSON_U32, "io_threads", offsetof(nb_cfg_runtime_in_t, io_threads), 0, { 0 } },
    { NB_CODEC_JSON_U32, "handler_threads", offsetof(nb_cfg_runtime_in_t, handler_threads), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "handler_max_concurrency",
      offsetof(nb_cfg_runtime_in_t, handler_max_concurrency),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "max_inflight", offsetof(nb_cfg_runtime_in_t, max_inflight), 0, { 0 } },
    { NB_CODEC_JSON_U32, "max_streams", offsetof(nb_cfg_runtime_in_t, max_streams), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "peer_rate_limit_pps",
      offsetof(nb_cfg_runtime_in_t, peer_rate_limit_pps),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "send_lanes", offsetof(nb_cfg_runtime_in_t, send_lanes), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_transport_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_transport_in_t) } },
    { NB_CODEC_JSON_STRING, "kind", offsetof(nb_cfg_transport_in_t, kind), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "bind_host", offsetof(nb_cfg_transport_in_t, bind_host), 0, { 0 } },
    { NB_CODEC_JSON_U32, "data_port", offsetof(nb_cfg_transport_in_t, data_port), 0, { 0 } },
    { NB_CODEC_JSON_U32, "frag_bytes", offsetof(nb_cfg_transport_in_t, frag_bytes), 0, { 0 } },
    { NB_CODEC_JSON_U32, "max_msg_bytes", offsetof(nb_cfg_transport_in_t, max_msg_bytes), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "backpressure_bytes",
      offsetof(nb_cfg_transport_in_t, backpressure_bytes),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32,
      "rtcp_interval_ms",
      offsetof(nb_cfg_transport_in_t, rtcp_interval_ms),
      0,
      { 0 } },
    { NB_CODEC_JSON_BOOL, "disable_rtcp", offsetof(nb_cfg_transport_in_t, disable_rtcp), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_discovery_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_discovery_in_t) } },
    { NB_CODEC_JSON_STRING, "mode", offsetof(nb_cfg_discovery_in_t, mode), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "broadcast", offsetof(nb_cfg_discovery_in_t, broadcast), 0, { 0 } },
    { NB_CODEC_JSON_U32,
      "hello_interval_ms",
      offsetof(nb_cfg_discovery_in_t, hello_interval_ms),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "node_ttl_ms", offsetof(nb_cfg_discovery_in_t, node_ttl_ms), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_metrics_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_metrics_in_t) } },
    { NB_CODEC_JSON_BOOL, "disable", offsetof(nb_cfg_metrics_in_t, disable), 0, { 0 } },
    { NB_CODEC_JSON_BOOL,
      "disable_latency_histogram",
      offsetof(nb_cfg_metrics_in_t, disable_latency_histogram),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "sample_every", offsetof(nb_cfg_metrics_in_t, sample_every), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_log_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_log_in_t) } },
    { NB_CODEC_JSON_BOOL, "disable", offsetof(nb_cfg_log_in_t, disable), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "level", offsetof(nb_cfg_log_in_t, level), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_route_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_route_in_t) } },
    { NB_CODEC_JSON_STRING, "service", offsetof(nb_cfg_route_in_t, service), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "endpoint", offsetof(nb_cfg_route_in_t, endpoint), 0, { 0 } },
};

static const nb_codec_json_model_t nb_g_binding_model[] = {
    { NB_CODEC_JSON_OBJ, NULL, 0, 0, { .obj_size = sizeof(nb_cfg_binding_in_t) } },
    { NB_CODEC_JSON_STRING, "name", offsetof(nb_cfg_binding_in_t, name), 0, { 0 } },
    { NB_CODEC_JSON_STRING, "endpoint", offsetof(nb_cfg_binding_in_t, endpoint), 0, { 0 } },
    { NB_CODEC_JSON_STRING,
      "intra_process",
      offsetof(nb_cfg_binding_in_t, intra_process),
      0,
      { 0 } },
    { NB_CODEC_JSON_U32, "poll_us", offsetof(nb_cfg_binding_in_t, poll_us), 0, { 0 } },
};

#define CFG_MODEL_N(model_) (sizeof(model_) / sizeof((model_)[0]))

/* ---- 小工具 ---- */

/* 复制字符串到定长缓冲；src 为空时使用 def；超容量返回 NB_ERR_INVALID。 */
static int nb_cfg_copy(char *dst, size_t cap, const char *src, const char *def)
{
    int n;
    if (!src) {
        src = def;
    }
    if (!src) {
        if (cap) {
            dst[0] = '\0';
        }
        return NB_OK;
    }
    n = snprintf(dst, cap, "%s", src);
    if (n < 0 || (size_t)n >= cap) {
        return NB_ERR_INVALID;
    }
    return NB_OK;
}

/* 堆复制字符串；失败返回 NULL。 */
static char *nb_cfg_dup(const char *src)
{
    size_t n;
    char *copy;
    if (!src) {
        return NULL;
    }
    n = strlen(src);
    copy = (char *)malloc(n + 1u);
    if (copy) {
        memcpy(copy, src, n + 1u);
    }
    return copy;
}

/* 把子对象填充进已清零的中间结构；obj 为 NULL 时保持默认。
 * 失败时 bad_key 指向模型中的字段名，便于拼出 "runtime.io_threads" 这样的路径。 */
static int nb_cfg_fill(const cJSON *obj,
                       const nb_codec_json_model_t *model,
                       size_t n,
                       void *out,
                       const char **bad_key)
{
    if (bad_key) {
        *bad_key = NULL;
    }
    if (!obj) {
        return NB_CODEC_OK;
    }
    return nb_codec_json_fill_object_ex(obj, model, n, out, bad_key);
}

/* 取可选子对象：不存在返回 NB_OK+NULL；存在但类型不对返回 NB_ERR_INVALID。 */
static int nb_cfg_child(const cJSON *root, const char *key, const cJSON **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    *out = NULL;
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsObject(item)) {
        return NB_ERR_INVALID;
    }
    *out = item;
    return NB_OK;
}

/* 未知字段保持“忽略”语义，但打一条带完整路径的 warning，避免拼写错误静默生效。 */
static int nb_cfg_model_has_key(const nb_codec_json_model_t *model, size_t n, const char *key)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (model[i].type != NB_CODEC_JSON_OBJ && model[i].key &&
            strcmp(model[i].key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

static void nb_cfg_warn_unknown(const cJSON *obj,
                                const nb_codec_json_model_t *model,
                                size_t model_n,
                                const char *const *extra,
                                size_t extra_n,
                                const char *section)
{
    const cJSON *child;

    if (!obj || !cJSON_IsObject(obj)) {
        return;
    }
    cJSON_ArrayForEach(child, obj)
    {
        size_t i;
        int known = 0;
        char full[96];

        if (!child->string) {
            continue;
        }
        known = nb_cfg_model_has_key(model, model_n, child->string);
        for (i = 0; !known && i < extra_n; i++) {
            if (extra[i] && strcmp(extra[i], child->string) == 0) {
                known = 1;
            }
        }
        if (known) {
            continue;
        }
        nb_cfg_join(full, sizeof(full), section, child->string);
        nb_log_warn("unknown config key ignored: %s", full);
    }
}

static int nb_cfg_transport_kind(const char *text, nb_transport_kind_t *out)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    if (strcmp(text, "tcp") == 0) {
        *out = NB_TRANSPORT_KIND_TCP;
        return NB_OK;
    }
    if (strcmp(text, "udp") == 0) {
        *out = NB_TRANSPORT_KIND_UDP;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

static int nb_cfg_mode(const char *text, nb_node_mode_t *out)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    if (strcmp(text, "static") == 0) {
        *out = NB_NODE_MODE_STATIC;
        return NB_OK;
    }
    if (strcmp(text, "mesh") == 0) {
        *out = NB_NODE_MODE_MESH;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

/* discovery.mode: mesh enables discovery; static/off/none disables it. */
static int nb_cfg_discovery_mode(const char *text, bool *disable)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    if (strcmp(text, "mesh") == 0) {
        *disable = false;
        return NB_OK;
    }
    if (strcmp(text, "static") == 0 || strcmp(text, "off") == 0 || strcmp(text, "none") == 0) {
        *disable = true;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

static int nb_cfg_log_level(const char *text, nb_log_level_t *out)
{
    static const struct {
        const char *name;
        nb_log_level_t level;
    } levels[] = {
        { "assert", NB_LOG_LEVEL_ASSERT },
        { "error", NB_LOG_LEVEL_ERROR },
        { "warn", NB_LOG_LEVEL_WARN },
        { "info", NB_LOG_LEVEL_INFO },
        { "debug", NB_LOG_LEVEL_DEBUG },
        { "verbose", NB_LOG_LEVEL_VERBOSE },
    };
    size_t i;
    if (!text) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(text, levels[i].name) == 0) {
            *out = levels[i].level;
            return NB_OK;
        }
    }
    return NB_ERR_INVALID;
}

/* 解析 "host:port"（含 [ipv6]:port）；成功返回 NB_OK。 */
static int nb_cfg_host_port(const char *text, char *host, size_t host_cap, uint16_t *port)
{
    nb_endpoint_t endpoint;
    char uri[NB_HOST_MAX * 2];
    int n;
    if (!text || !host || !port) {
        return NB_ERR_INVALID;
    }
    n = snprintf(uri, sizeof(uri), "udp://%s", text);
    if (n < 0 || (size_t)n >= sizeof(uri) || nb_endpoint_parse(uri, &endpoint) != NB_OK) {
        return NB_ERR_INVALID;
    }
    if (nb_cfg_copy(host, host_cap, endpoint.host, NULL) != NB_OK) {
        return NB_ERR_INVALID;
    }
    *port = endpoint.port;
    return NB_OK;
}

/* ---- 子结构装载 ---- */

static int nb_cfg_fill_error(nb_config_error_t *err,
                             const char *section,
                             const char *bad_key,
                             int codec_status)
{
    char path[96];

    if (bad_key && bad_key[0]) {
        nb_cfg_join(path, sizeof(path), section, bad_key);
    } else {
        snprintf(path, sizeof(path), "%s", section);
    }
    return nb_cfg_fail(err, path, nb_cfg_codec_reason(codec_status));
}

static int nb_load_runtime(const cJSON *root, nb_cfg_runtime_in_t *in, nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    const char *bad = NULL;
    int rc;

    if (nb_cfg_child(root, "runtime", &obj) != NB_OK) {
        return nb_cfg_fail(err, "runtime", "must be an object");
    }
    nb_cfg_warn_unknown(
        obj, nb_g_runtime_model, CFG_MODEL_N(nb_g_runtime_model), NULL, 0, "runtime");
    rc = nb_cfg_fill(obj, nb_g_runtime_model, CFG_MODEL_N(nb_g_runtime_model), in, &bad);
    return rc == NB_CODEC_OK ? NB_OK : nb_cfg_fill_error(err, "runtime", bad, rc);
}

static int nb_load_transport(const cJSON *root,
                          nb_cfg_transport_in_t *in,
                          const cJSON **bindings,
                          nb_config_error_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "transport");
    static const char *const extra[] = {"bindings", "qos"};
    const char *bad = NULL;
    int rc;

    *bindings = NULL;
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsObject(item)) {
        return nb_cfg_fail(err, "transport", "must be an object");
    }
    nb_cfg_warn_unknown(item,
                        nb_g_transport_model,
                        CFG_MODEL_N(nb_g_transport_model),
                        extra,
                        sizeof(extra) / sizeof(extra[0]),
                        "transport");
    rc = nb_cfg_fill(item, nb_g_transport_model, CFG_MODEL_N(nb_g_transport_model), in, &bad);
    if (rc != NB_CODEC_OK) {
        return nb_cfg_fill_error(err, "transport", bad, rc);
    }
    *bindings = cJSON_GetObjectItemCaseSensitive(item, "bindings");
    if (*bindings && !cJSON_IsArray(*bindings)) {
        return nb_cfg_fail(err, "transport.bindings", "must be an array");
    }
    return NB_OK;
}

static int nb_load_discovery(const cJSON *root,
                          nb_cfg_discovery_in_t *in,
                          const cJSON **peers,
                          nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    static const char *const extra[] = {"peers"};
    const char *bad = NULL;
    int rc;

    *peers = NULL;
    if (nb_cfg_child(root, "discovery", &obj) != NB_OK) {
        return nb_cfg_fail(err, "discovery", "must be an object");
    }
    nb_cfg_warn_unknown(obj,
                        nb_g_discovery_model,
                        CFG_MODEL_N(nb_g_discovery_model),
                        extra,
                        sizeof(extra) / sizeof(extra[0]),
                        "discovery");
    rc = nb_cfg_fill(obj, nb_g_discovery_model, CFG_MODEL_N(nb_g_discovery_model), in, &bad);
    if (rc != NB_CODEC_OK) {
        return nb_cfg_fill_error(err, "discovery", bad, rc);
    }
    if (obj) {
        *peers = cJSON_GetObjectItemCaseSensitive(obj, "peers");
    }
    if (*peers && !cJSON_IsArray(*peers)) {
        return nb_cfg_fail(err, "discovery.peers", "must be an array");
    }
    return NB_OK;
}

static int nb_load_metrics(const cJSON *root, nb_cfg_metrics_in_t *in, nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    const char *bad = NULL;
    int rc;

    if (nb_cfg_child(root, "metrics", &obj) != NB_OK) {
        return nb_cfg_fail(err, "metrics", "must be an object");
    }
    nb_cfg_warn_unknown(
        obj, nb_g_metrics_model, CFG_MODEL_N(nb_g_metrics_model), NULL, 0, "metrics");
    rc = nb_cfg_fill(obj, nb_g_metrics_model, CFG_MODEL_N(nb_g_metrics_model), in, &bad);
    return rc == NB_CODEC_OK ? NB_OK : nb_cfg_fill_error(err, "metrics", bad, rc);
}

static int nb_load_log(const cJSON *root, nb_cfg_log_in_t *in, nb_config_error_t *err)
{
    const cJSON *obj = NULL;
    const char *bad = NULL;
    int rc;

    if (nb_cfg_child(root, "log", &obj) != NB_OK) {
        return nb_cfg_fail(err, "log", "must be an object");
    }
    nb_cfg_warn_unknown(obj, nb_g_log_model, CFG_MODEL_N(nb_g_log_model), NULL, 0, "log");
    rc = nb_cfg_fill(obj, nb_g_log_model, CFG_MODEL_N(nb_g_log_model), in, &bad);
    return rc == NB_CODEC_OK ? NB_OK : nb_cfg_fill_error(err, "log", bad, rc);
}

/* ---- 转换到 nb_config_doc_t ---- */

static int nb_fill_bindings(nb_config_doc_t *out, const cJSON *bindings, nb_config_error_t *err)
{
    const cJSON *item;
    size_t index = 0;

    if (!bindings) {
        return NB_OK;
    }
    cJSON_ArrayForEach(item, bindings)
    {
        nb_binding_endpoint_t *binding;
        nb_binding_config_t *view;
        nb_cfg_binding_in_t parsed;
        const char *endpoint;
        const char *name;
        nb_endpoint_t endpoint_parsed;
        nb_intra_process_t intra = NB_INTRA_PROCESS_OFF;
        char path[96];
        char sub[96];
        int rc = NB_OK;
        int frc;

        snprintf(path, sizeof(path), "transport.bindings[%lu]", (unsigned long)index);
        index++;
        if (out->binding_count >= NB_CONFIG_MAX_BINDINGS) {
            return nb_cfg_fail(err, "transport.bindings", "too many bindings");
        }
        memset(&parsed, 0, sizeof(parsed));
        if (cJSON_IsString(item) && item->valuestring) {
            parsed.endpoint = nb_cfg_dup(item->valuestring);
            if (!parsed.endpoint) {
                rc = nb_cfg_fail(err, path, "out of memory");
                goto done;
            }
        } else if (cJSON_IsObject(item)) {
            const char *bad = NULL;
            static const char *const extra[] = {"qos"};

            nb_cfg_warn_unknown(item,
                                nb_g_binding_model,
                                CFG_MODEL_N(nb_g_binding_model),
                                extra,
                                sizeof(extra) / sizeof(extra[0]),
                                path);
            frc = nb_cfg_fill(
                item, nb_g_binding_model, CFG_MODEL_N(nb_g_binding_model), &parsed, &bad);
            if (frc != NB_CODEC_OK) {
                rc = nb_cfg_fill_error(err, path, bad, frc);
                goto done;
            }
        } else {
            rc = nb_cfg_fail(err, path, "must be string or object");
            goto done;
        }
        endpoint = parsed.endpoint;
        name = parsed.name;
        if (parsed.intra_process) {
            if (strcmp(parsed.intra_process, "off") == 0) {
                intra = NB_INTRA_PROCESS_OFF;
            } else if (strcmp(parsed.intra_process, "auto") == 0) {
                intra = NB_INTRA_PROCESS_AUTO;
            } else if (strcmp(parsed.intra_process, "only") == 0) {
                intra = NB_INTRA_PROCESS_ONLY;
            } else {
                nb_cfg_join(sub, sizeof(sub), path, "intra_process");
                rc = nb_cfg_fail(err, sub, "must be off/auto/only");
                goto done;
            }
        }
        if (!endpoint || nb_endpoint_parse(endpoint, &endpoint_parsed) != NB_OK) {
            nb_cfg_join(sub, sizeof(sub), path, "endpoint");
            rc = nb_cfg_fail(err, sub, "invalid endpoint URI");
            goto done;
        }
        binding = &out->bindings[out->binding_count];
        view = &out->binding_configs[out->binding_count];
        if (nb_cfg_copy(binding->endpoint, sizeof(binding->endpoint), endpoint, NULL) != NB_OK) {
            nb_cfg_join(sub, sizeof(sub), path, "endpoint");
            rc = nb_cfg_fail(err, sub, "endpoint too long");
            goto done;
        }
        if (name && name[0]) {
            if (nb_cfg_copy(binding->name, sizeof(binding->name), name, NULL) != NB_OK) {
                nb_cfg_join(sub, sizeof(sub), path, "name");
                rc = nb_cfg_fail(err, sub, "binding name too long");
                goto done;
            }
        } else if (nb_cfg_copy(binding->name, sizeof(binding->name), endpoint_parsed.scheme, NULL) !=
                   NB_OK) {
            nb_cfg_join(sub, sizeof(sub), path, "name");
            rc = nb_cfg_fail(err, sub, "binding name too long");
            goto done;
        }
        if (!binding->name[0]) {
            rc = nb_cfg_fail(err, path, "missing binding name");
            goto done;
        }
        view->name = binding->name;
        view->endpoint = binding->endpoint;
        view->intra_process = intra;
        view->poll_us = parsed.poll_us;
        if (cJSON_IsObject(item)) {
            nb_cfg_join(sub, sizeof(sub), path, "qos");
            if (nb_cfg_load_qos(item, &view->qos, err, sub) != NB_OK) {
                rc = NB_ERR_INVALID;
                goto done;
            }
        }
        out->binding_count++;
    done:
        nb_codec_json_clear(&parsed, nb_g_binding_model, CFG_MODEL_N(nb_g_binding_model));
        if (rc != NB_OK) {
            return rc;
        }
    }
    return NB_OK;
}

static int nb_fill_peers(nb_config_doc_t *out, const cJSON *peers, nb_config_error_t *err)
{
    const cJSON *item;
    size_t index = 0;

    if (!peers) {
        return NB_OK;
    }
    cJSON_ArrayForEach(item, peers)
    {
        nb_peer_endpoint_t *peer;
        char path[96];

        snprintf(path, sizeof(path), "discovery.peers[%lu]", (unsigned long)index);
        index++;
        if (out->peer_count >= NB_CONFIG_MAX_PEERS) {
            return nb_cfg_fail(err, "discovery.peers", "too many peers");
        }
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_cfg_fail(err, path, "must be a string");
        }
        peer = &out->peers[out->peer_count];
        if (nb_cfg_host_port(item->valuestring, peer->host, sizeof(peer->host), &peer->port) !=
            NB_OK) {
            return nb_cfg_fail(err, path, "invalid host:port");
        }
        if (!peer->host[0] || !peer->port) {
            return nb_cfg_fail(err, path, "invalid host:port");
        }
        out->peer_count++;
    }
    return NB_OK;
}

static int nb_fill_routes(nb_config_doc_t *out, const cJSON *routes, nb_config_error_t *err)
{
    const cJSON *item;
    size_t index = 0;

    if (!routes) {
        return NB_OK;
    }
    if (!cJSON_IsArray(routes)) {
        return nb_cfg_fail(err, "routes", "must be an array");
    }
    cJSON_ArrayForEach(item, routes)
    {
        nb_static_route_t *route;
        nb_cfg_route_in_t parsed;
        nb_endpoint_t endpoint;
        char path[96];
        char sub[96];
        int rc = NB_OK;
        int frc;

        snprintf(path, sizeof(path), "routes[%lu]", (unsigned long)index);
        index++;
        if (out->route_count >= NB_CONFIG_MAX_ROUTES) {
            return nb_cfg_fail(err, "routes", "too many routes");
        }
        if (!cJSON_IsObject(item)) {
            return nb_cfg_fail(err, path, "each route must be an object");
        }
        nb_cfg_warn_unknown(item, nb_g_route_model, CFG_MODEL_N(nb_g_route_model), NULL, 0, path);
        memset(&parsed, 0, sizeof(parsed));
        {
            const char *bad = NULL;
            frc = nb_cfg_fill(item, nb_g_route_model, CFG_MODEL_N(nb_g_route_model), &parsed, &bad);
            if (frc != NB_CODEC_OK) {
                rc = nb_cfg_fill_error(err, path, bad, frc);
                nb_codec_json_clear(&parsed, nb_g_route_model, CFG_MODEL_N(nb_g_route_model));
                return rc;
            }
        }
        route = &out->routes[out->route_count];
        if (nb_cfg_copy(route->service, sizeof(route->service), parsed.service, NULL) != NB_OK) {
            nb_cfg_join(sub, sizeof(sub), path, "service");
            rc = nb_cfg_fail(err, sub, "service name too long");
        } else if (nb_cfg_copy(route->endpoint, sizeof(route->endpoint), parsed.endpoint, NULL) !=
                   NB_OK) {
            nb_cfg_join(sub, sizeof(sub), path, "endpoint");
            rc = nb_cfg_fail(err, sub, "endpoint too long");
        } else if (!route->service[0]) {
            nb_cfg_join(sub, sizeof(sub), path, "service");
            rc = nb_cfg_fail(err, sub, "missing service name");
        } else if (!route->endpoint[0]) {
            nb_cfg_join(sub, sizeof(sub), path, "endpoint");
            rc = nb_cfg_fail(err, sub, "missing endpoint");
        } else if (nb_endpoint_parse(route->endpoint, &endpoint) != NB_OK) {
            nb_cfg_join(sub, sizeof(sub), path, "endpoint");
            rc = nb_cfg_fail(err, sub, "invalid endpoint URI");
        }
        if (rc == NB_OK) {
            out->route_count++;
        }
        nb_codec_json_clear(&parsed, nb_g_route_model, CFG_MODEL_N(nb_g_route_model));
        if (rc != NB_OK) {
            return rc;
        }
    }
    return NB_OK;
}

/* 把中间结构合入 nb_config_doc_t.node，应用默认值并校验范围。 */
static int nb_apply_config(nb_config_doc_t *out,
                        const nb_cfg_runtime_in_t *rt,
                        const nb_cfg_transport_in_t *tp,
                        const nb_cfg_discovery_in_t *disc,
                        const nb_cfg_metrics_in_t *mt,
                        const nb_cfg_log_in_t *lg,
                        const cJSON *root,
                        const cJSON *bindings,
                        const cJSON *peers,
                        const cJSON *routes,
                        nb_config_error_t *err)
{
    nb_node_config_t *node = &out->node;
    nb_node_mode_t mode = NB_NODE_MODE_MESH;
    nb_transport_kind_t kind = NB_TRANSPORT_KIND_UDP;
    const char *name = NULL;
    const cJSON *item;
    char reason[96];
    int rc;

    nb_node_config_default(node);

    /* name / mode（顶层） */
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (item) {
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_cfg_fail(err, "name", "must be a string");
        }
        name = item->valuestring;
    }
    if (nb_cfg_copy(out->name, sizeof(out->name), name, NB_DEFAULT_NODE_NAME) != NB_OK) {
        return nb_cfg_fail(err, "name", "too long");
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (item) {
        if (!cJSON_IsString(item) || nb_cfg_mode(item->valuestring, &mode) != NB_OK) {
            return nb_cfg_fail(err, "mode", "must be \"mesh\" or \"static\"");
        }
    }
    node->mode = mode;

    /* runtime */
    if (rt->io_threads > NB_TRANSPORT_MAX_IO) {
        snprintf(reason, sizeof(reason), "must be 0..%u", (unsigned)NB_TRANSPORT_MAX_IO);
        return nb_cfg_fail(err, "runtime.io_threads", reason);
    }
    if (rt->handler_threads > NB_MAX_HANDLER_THREADS) {
        snprintf(reason, sizeof(reason), "must be 0..%u", (unsigned)NB_MAX_HANDLER_THREADS);
        return nb_cfg_fail(err, "runtime.handler_threads", reason);
    }
    if (rt->send_lanes > NB_MAX_SEND_LANES) {
        snprintf(reason, sizeof(reason), "must be 0..%u", (unsigned)NB_MAX_SEND_LANES);
        return nb_cfg_fail(err, "runtime.send_lanes", reason);
    }
    if (rt->max_streams > NB_STREAM_MAX_RECV_STREAMS) {
        snprintf(reason, sizeof(reason), "must be 0..%u", (unsigned)NB_STREAM_MAX_RECV_STREAMS);
        return nb_cfg_fail(err, "runtime.max_streams", reason);
    }
    node->runtime.io_threads = (unsigned)rt->io_threads;
    node->runtime.handler_threads = (unsigned)rt->handler_threads;
    node->runtime.handler_max_concurrency = (unsigned)rt->handler_max_concurrency;
    node->runtime.max_inflight = rt->max_inflight;
    node->runtime.max_streams = rt->max_streams;
    node->runtime.peer_rate_limit_pps = rt->peer_rate_limit_pps;
    node->runtime.send_lanes = (unsigned)rt->send_lanes;

    /* transport */
    if (tp->kind && nb_cfg_transport_kind(tp->kind, &kind) != NB_OK) {
        return nb_cfg_fail(err, "transport.kind", "must be \"udp\" or \"tcp\"");
    }
    if (tp->data_port > UINT16_MAX) {
        return nb_cfg_fail(err, "transport.data_port", "out of range");
    }
    if (tp->frag_bytes != 0 &&
        (tp->frag_bytes < NB_DEFAULT_FRAG_MIN_BYTES || tp->frag_bytes > NB_DEFAULT_FRAG_MAX_BYTES)) {
        snprintf(reason,
                 sizeof(reason),
                 "must be 0 or %u..%u",
                 (unsigned)NB_DEFAULT_FRAG_MIN_BYTES,
                 (unsigned)NB_DEFAULT_FRAG_MAX_BYTES);
        return nb_cfg_fail(err, "transport.frag_bytes", reason);
    }
    if (tp->max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
        snprintf(reason,
                 sizeof(reason),
                 "must be 0..%u",
                 (unsigned)NB_GLOBAL_MAX_MSG_BYTES_LIMIT);
        return nb_cfg_fail(err, "transport.max_msg_bytes", reason);
    }
    {
        uint32_t effective_frag =
            tp->frag_bytes ? tp->frag_bytes : (uint32_t)NB_DEFAULT_FRAG_BYTES;
        if (tp->max_msg_bytes && tp->max_msg_bytes < effective_frag) {
            return nb_cfg_fail(err, "transport.max_msg_bytes", "must be >= frag_bytes");
        }
    }
    node->transport.kind = kind;
    if (nb_cfg_copy(out->bind_host, sizeof(out->bind_host), tp->bind_host, NB_DEFAULT_BIND_HOST) !=
        NB_OK) {
        return nb_cfg_fail(err, "transport.bind_host", "too long");
    }
    node->transport.data_port = (uint16_t)tp->data_port;
    node->transport.frag_bytes = (uint16_t)tp->frag_bytes;
    node->transport.max_msg_bytes = tp->max_msg_bytes;
    node->transport.backpressure_bytes = tp->backpressure_bytes;
    node->transport.rtcp_interval_ms = tp->rtcp_interval_ms;
    node->transport.disable_rtcp = tp->disable_rtcp ? true : false;
    {
        const cJSON *transport_obj = cJSON_GetObjectItemCaseSensitive(root, "transport");
        if (transport_obj) {
            rc = nb_cfg_load_qos(transport_obj, &node->transport.qos, err, "transport.qos");
            if (rc != NB_OK) {
                return rc;
            }
        }
    }

    /* discovery */
    if (disc->mode && nb_cfg_discovery_mode(disc->mode, &node->discovery.disable) != NB_OK) {
        return nb_cfg_fail(err, "discovery.mode", "must be mesh/static/off/none");
    }
    if (mode == NB_NODE_MODE_STATIC) {
        node->discovery.disable = true;
    }
    if (disc->broadcast) {
        if (nb_cfg_host_port(
                disc->broadcast, out->discovery_host, sizeof(out->discovery_host), &node->discovery.port) !=
            NB_OK) {
            return nb_cfg_fail(err, "discovery.broadcast", "invalid host:port");
        }
    } else if (nb_cfg_copy(out->discovery_host,
                        sizeof(out->discovery_host),
                        NULL,
                        NB_DEFAULT_DISCOVERY_HOST) != NB_OK) {
        return nb_cfg_fail(err, "discovery.broadcast", "default host too long");
    }
    node->discovery.hello_interval_ms = disc->hello_interval_ms;
    node->discovery.node_ttl_ms = disc->node_ttl_ms;

    /* metrics */
    node->metrics.disable = mt->disable ? true : false;
    node->metrics.disable_latency_histogram = mt->disable_latency_histogram ? true : false;
    node->metrics.sample_every =
        mt->sample_every ? mt->sample_every : NB_DEFAULT_METRICS_SAMPLE_EVERY;

    /* log */
    node->log.disable = lg->disable ? true : false;
    if (lg->level && nb_cfg_log_level(lg->level, &node->log.level) != NB_OK) {
        return nb_cfg_fail(err, "log.level", "invalid level");
    }

    rc = nb_fill_bindings(out, bindings, err);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_fill_peers(out, peers, err);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_fill_routes(out, routes, err);
    if (rc != NB_OK) {
        return rc;
    }
    nb_config_doc_rebind(out);
    nb_node_config_normalize(node);
    return NB_OK;
}

/* ---- 配置加载 ---- */

static void nb_config_doc_rebind(nb_config_doc_t *doc)
{
    size_t i;

    if (!doc) {
        return;
    }
    doc->node.name = doc->name;
    doc->node.transport.bind_host = doc->bind_host;
    doc->node.discovery.broadcast_host = doc->discovery_host;
    if (doc->binding_count > NB_CONFIG_MAX_BINDINGS) {
        doc->binding_count = NB_CONFIG_MAX_BINDINGS;
    }
    for (i = 0; i < doc->binding_count; i++) {
        doc->binding_configs[i].name = doc->bindings[i].name;
        doc->binding_configs[i].endpoint = doc->bindings[i].endpoint;
    }
    doc->node.transport.bindings = doc->binding_count ? doc->binding_configs : NULL;
    doc->node.transport.binding_count = doc->binding_count;
    doc->node.routes = doc->route_count ? doc->routes : NULL;
    doc->node.route_count = doc->route_count;
    doc->node.peers = doc->peer_count ? doc->peers : NULL;
    doc->node.peer_count = doc->peer_count;
}

int nb_config_parse(const char *json, nb_config_doc_t **out, nb_config_error_t *err)
{
    cJSON *root = NULL;
    nb_config_doc_t *doc = NULL;
    nb_cfg_runtime_in_t rt;
    nb_cfg_transport_in_t tp;
    nb_cfg_discovery_in_t disc;
    nb_cfg_metrics_in_t mt;
    nb_cfg_log_in_t lg;
    const cJSON *bindings = NULL;
    const cJSON *peers = NULL;
    const cJSON *routes = NULL;
    int rc = NB_ERR_INVALID;

    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (out) {
        *out = NULL;
    }
    if (!json || !out) {
        return nb_cfg_fail(err, "", "null argument");
    }
    doc = (nb_config_doc_t *)calloc(1, sizeof(*doc));
    if (!doc) {
        return nb_cfg_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    memset(&rt, 0, sizeof(rt));
    memset(&tp, 0, sizeof(tp));
    memset(&disc, 0, sizeof(disc));
    memset(&mt, 0, sizeof(mt));
    memset(&lg, 0, sizeof(lg));

    root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        nb_cfg_fail(err, "", "malformed JSON object");
        goto done;
    }
    {
        static const char *const top_keys[] = {
            "name", "mode", "runtime", "transport", "discovery", "metrics", "log", "routes"};
        nb_cfg_warn_unknown(root,
                            NULL,
                            0,
                            top_keys,
                            sizeof(top_keys) / sizeof(top_keys[0]),
                            "");
    }
    routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
    if (routes && !cJSON_IsArray(routes)) {
        nb_cfg_fail(err, "routes", "must be an array");
        goto done;
    }
    rc = nb_load_runtime(root, &rt, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_transport(root, &tp, &bindings, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_discovery(root, &disc, &peers, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_metrics(root, &mt, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_load_log(root, &lg, err);
    if (rc != NB_OK) {
        goto done;
    }
    rc = nb_apply_config(doc, &rt, &tp, &disc, &mt, &lg, root, bindings, peers, routes, err);
    if (rc != NB_OK) {
        goto done;
    }
    *out = doc;
    doc = NULL;
    rc = NB_OK;

done:
    nb_codec_json_clear(&rt, nb_g_runtime_model, CFG_MODEL_N(nb_g_runtime_model));
    nb_codec_json_clear(&tp, nb_g_transport_model, CFG_MODEL_N(nb_g_transport_model));
    nb_codec_json_clear(&disc, nb_g_discovery_model, CFG_MODEL_N(nb_g_discovery_model));
    nb_codec_json_clear(&mt, nb_g_metrics_model, CFG_MODEL_N(nb_g_metrics_model));
    nb_codec_json_clear(&lg, nb_g_log_model, CFG_MODEL_N(nb_g_log_model));
    if (root) {
        cJSON_Delete(root);
    }
    free(doc);
    return rc;
}

int nb_config_parse_file(const char *path, nb_config_doc_t **out, nb_config_error_t *err)
{
    FILE *f;
    long size;
    char *buf;
    int rc;

    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (!path || !out) {
        return nb_cfg_fail(err, "", "null argument");
    }
    f = fopen(path, "rb");
    if (!f) {
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot open config file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot seek config file");
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot size config file");
    }
    if ((unsigned long)size > (unsigned long)NB_CONFIG_MAX_FILE_BYTES) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_INVALID, "", "config file too large");
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return nb_cfg_fail_code(err, NB_ERR_IO, "", "cannot read config file");
    }
    buf[size] = '\0';
    fclose(f);
    rc = nb_config_parse(buf, out, err);
    free(buf);
    return rc;
}

/* ---- Node creation ---- */

int nb_node_create_from_config(const nb_config_doc_t *cfg, nb_node_t **out)
{
    int rc;

    if (!cfg || !out) {
        return NB_ERR_INVALID;
    }
    rc = nb_node_create(&cfg->node, out);
    if (rc != NB_OK) {
        return rc;
    }
    return NB_OK;
}

void nb_config_doc_destroy(nb_config_doc_t *doc)
{
    free(doc);
}

const nb_node_config_t *nb_config_node(const nb_config_doc_t *doc)
{
    return doc ? &doc->node : NULL;
}
