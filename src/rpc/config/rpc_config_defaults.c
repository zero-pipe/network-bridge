/* RPC config defaults, normalization and strict validation. */
#include <nb/rpc/config.h>

#include <ntk.h>

#include "caps.h"
#include "defaults.h"

#include <string.h>

void nb_node_runtime_config_default(nb_node_runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void nb_rpc_config_default(nb_rpc_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = NB_NODE_MODE_MESH;
    cfg->kind = NB_TRANSPORT_KIND_UDP;
    cfg->bind_host = NB_DEFAULT_BIND_HOST;
    nb_node_discovery_config_default(&cfg->discovery);
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
    nb_node_runtime_config_default(&cfg->runtime);
    nb_rpc_config_default(&cfg->rpc);
    nb_node_metrics_config_default(&cfg->metrics);
    nb_node_log_config_default(&cfg->log);
}

void nb_binding_config_default(nb_binding_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void nb_node_config_normalize(nb_node_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    if (!cfg->name) {
        cfg->name = NB_DEFAULT_NODE_NAME;
    }
    if (!cfg->rpc.bind_host) {
        cfg->rpc.bind_host = NB_DEFAULT_BIND_HOST;
    }
    if (!cfg->rpc.discovery.broadcast_host) {
        cfg->rpc.discovery.broadcast_host = NB_DEFAULT_DISCOVERY_HOST;
    }
    if (cfg->rpc.mode == NB_NODE_MODE_STATIC) {
        cfg->rpc.discovery.disable = true;
    } else if (cfg->rpc.discovery.disable) {
        cfg->rpc.mode = NB_NODE_MODE_STATIC;
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
    if (!cfg->rpc.frag_bytes) {
        cfg->rpc.frag_bytes = NB_DEFAULT_FRAG_BYTES;
    } else if (cfg->rpc.frag_bytes < NB_DEFAULT_FRAG_MIN_BYTES) {
        cfg->rpc.frag_bytes = NB_DEFAULT_FRAG_MIN_BYTES;
    } else if (cfg->rpc.frag_bytes > NB_DEFAULT_FRAG_MAX_BYTES) {
        cfg->rpc.frag_bytes = NB_DEFAULT_FRAG_MAX_BYTES;
    }
    if (!cfg->rpc.max_msg_bytes) {
        cfg->rpc.max_msg_bytes = NB_DEFAULT_MAX_MSG_BYTES;
    } else if (cfg->rpc.max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
        cfg->rpc.max_msg_bytes = NB_GLOBAL_MAX_MSG_BYTES_LIMIT;
    }
    if (cfg->rpc.max_msg_bytes < cfg->rpc.frag_bytes) {
        cfg->rpc.max_msg_bytes = cfg->rpc.frag_bytes;
    }
    if (!cfg->rpc.backpressure_bytes) {
        cfg->rpc.backpressure_bytes = NB_DEFAULT_BACKPRESSURE_BYTES;
    }
    if (!cfg->rpc.rtcp_interval_ms) {
        cfg->rpc.rtcp_interval_ms = NB_DEFAULT_RTCP_INTERVAL_MS;
    }
    if (!cfg->rpc.discovery.port) {
        cfg->rpc.discovery.port = NB_DEFAULT_DISCOVERY_PORT;
    }
    if (!cfg->rpc.discovery.hello_interval_ms) {
        cfg->rpc.discovery.hello_interval_ms = NB_DEFAULT_HELLO_INTERVAL_MS;
    }
    if (!cfg->rpc.discovery.node_ttl_ms) {
        cfg->rpc.discovery.node_ttl_ms = NB_DEFAULT_NODE_TTL_MS;
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
    if (cfg->log.level == NB_LOG_LEVEL_DEFAULT ||
        cfg->log.level < NB_LOG_LEVEL_ASSERT || cfg->log.level > NB_LOG_LEVEL_VERBOSE) {
        cfg->log.level = NB_LOG_LEVEL_INFO;
    }
}

int nb_node_config_validate(const nb_node_config_t *cfg)
{
    if (!cfg) {
        return NB_ERR_INVALID;
    }
    if (!cfg->rpc.bind_host || !cfg->rpc.bind_host[0]) {
        return NB_ERR_INVALID;
    }
    if (strlen(cfg->rpc.bind_host) >= NB_HOST_MAX ||
        (cfg->name && strlen(cfg->name) >= NB_NAME_MAX) ||
        (cfg->rpc.discovery.broadcast_host &&
         strlen(cfg->rpc.discovery.broadcast_host) >= NB_HOST_MAX)) {
        return NB_ERR_TOOBIG;
    }
    if (cfg->runtime.io_threads > NB_TRANSPORT_MAX_IO ||
        cfg->runtime.handler_threads > NB_MAX_HANDLER_THREADS ||
        cfg->runtime.send_lanes > NB_MAX_SEND_LANES ||
        cfg->runtime.max_streams > NB_STREAM_MAX_RECV_STREAMS) {
        return NB_ERR_INVALID;
    }
    if (cfg->rpc.frag_bytes != 0 &&
        (cfg->rpc.frag_bytes < NB_DEFAULT_FRAG_MIN_BYTES ||
         cfg->rpc.frag_bytes > NB_DEFAULT_FRAG_MAX_BYTES)) {
        return NB_ERR_INVALID;
    }
    if (cfg->rpc.max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
        return NB_ERR_INVALID;
    }
    if (cfg->rpc.max_msg_bytes != 0 && cfg->rpc.frag_bytes != 0 &&
        cfg->rpc.max_msg_bytes < cfg->rpc.frag_bytes) {
        return NB_ERR_INVALID;
    }
    if (cfg->log.level != NB_LOG_LEVEL_DEFAULT &&
        (cfg->log.level < NB_LOG_LEVEL_ASSERT || cfg->log.level > NB_LOG_LEVEL_VERBOSE)) {
        return NB_ERR_INVALID;
    }
    if (cfg->binding_count > 0 && !cfg->bindings) {
        return NB_ERR_INVALID;
    }
    return NB_OK;
}
