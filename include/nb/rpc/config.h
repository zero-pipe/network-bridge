#ifndef NB_RPC_CONFIG_H
#define NB_RPC_CONFIG_H

/*
 * RPC spine configuration and JSON loading.
 *
 * The JSON document may also contain a `dds` block; this parser ignores it.
 * The two spines parse independently, so an RPC-only process never depends on
 * DDS headers or symbols.
 *
 * Durations are numbers of milliseconds or strings with a unit ("250ms", "2s").
 * String enums are lowercase and case-sensitive.
 */

#include <nb/base/config_error.h>
#include <nb/rpc/endpoint.h>
#include <nb/rpc/types.h>
#include <nb/rpc/service_table.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Capacity ---- */
#define NB_CONFIG_MAX_ROUTES 32
#define NB_CONFIG_MAX_PEERS 32
#define NB_CONFIG_MAX_BINDINGS NB_MAX_BINDINGS

/* One transport instance the node starts once (RPC listen URI). */
typedef struct nb_binding_config {
    const char *name;     /* provider/binding name, e.g. "tcp" */
    const char *endpoint; /* URI, e.g. tcp://127.0.0.1:8080 */
} nb_binding_config_t;

/* Threads, concurrency and flow-control policy. 0 selects the module default. */
typedef struct nb_node_runtime_config {
    unsigned io_threads;
    unsigned handler_threads;
    unsigned handler_max_concurrency;
    uint32_t max_inflight;
    uint32_t max_streams;
    uint32_t peer_rate_limit_pps;
    unsigned send_lanes;
} nb_node_runtime_config_t;

/* Service discovery. */
typedef struct nb_node_discovery_config {
    bool disable;
    const char *broadcast_host;
    uint16_t port;
    uint32_t hello_interval_ms;
    uint32_t node_ttl_ms;
} nb_node_discovery_config_t;

/* Metrics switches (process-global). */
typedef struct nb_node_metrics_config {
    bool disable;
    bool disable_latency_histogram;
    uint32_t sample_every;
} nb_node_metrics_config_t;

/* Logging switches (process-global). */
typedef struct nb_node_log_config {
    bool disable;
    nb_log_level_t level;
} nb_node_log_config_t;

typedef struct nb_static_route {
    char service[NB_SERVICE_NAME_MAX];
    char endpoint[NB_ENDPOINT_URI_MAX];
} nb_static_route_t;

typedef struct nb_peer_endpoint {
    char host[NB_HOST_MAX];
    uint16_t port;
} nb_peer_endpoint_t;

/* RPC link tuning, discovery and routes. */
typedef struct nb_rpc_config {
    nb_node_mode_t mode;
    const nb_service_table_t *service_table; /* optional shared declaration table */
    /* Fallback binding for code-built configs that leave bindings empty. */
    nb_transport_kind_t kind;
    const char *bind_host;
    uint16_t data_port;
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t backpressure_bytes;
    uint32_t rtcp_interval_ms;
    bool disable_rtcp;
    nb_node_discovery_config_t discovery;
    const nb_static_route_t *routes;
    size_t route_count;
    const nb_peer_endpoint_t *peers;
    size_t peer_count;
} nb_rpc_config_t;

/* Composite RPC node configuration. */
typedef struct nb_node_config {
    const char *name;
    nb_node_runtime_config_t runtime;
    nb_rpc_config_t rpc;
    const nb_binding_config_t *bindings;
    size_t binding_count;
    nb_node_metrics_config_t metrics;
    nb_node_log_config_t log;
} nb_node_config_t;

/* One configured binding: a naming label plus its endpoint URI. */
typedef struct nb_binding_endpoint {
    char name[NB_SCHEME_MAX];
    char endpoint[NB_ENDPOINT_URI_MAX];
} nb_binding_endpoint_t;

/* Opaque parsed RPC document. */
typedef struct nb_rpc_config_doc nb_rpc_config_doc_t;

/* ---- Defaults ---- */
void nb_node_runtime_config_default(nb_node_runtime_config_t *config);
void nb_rpc_config_default(nb_rpc_config_t *config);
void nb_node_discovery_config_default(nb_node_discovery_config_t *config);
void nb_node_metrics_config_default(nb_node_metrics_config_t *config);
void nb_node_log_config_default(nb_node_log_config_t *config);
void nb_node_config_default(nb_node_config_t *config);
void nb_binding_config_default(nb_binding_config_t *config);
int nb_node_config_validate(const nb_node_config_t *config);
void nb_node_config_normalize(nb_node_config_t *config);

/* ---- Load and create ---- */
int nb_rpc_config_parse(const char *json, nb_rpc_config_doc_t **out, nb_config_error_t *error);
int nb_rpc_config_parse_file(const char *path,
                             nb_rpc_config_doc_t **out,
                             nb_config_error_t *error);
void nb_rpc_config_doc_destroy(nb_rpc_config_doc_t *doc);
/* Read-only view of the parsed node configuration; NULL for a NULL document. */
const nb_node_config_t *nb_rpc_config_node(const nb_rpc_config_doc_t *doc);
/* Create a node from a parsed document and apply static routes and peers. */
int nb_node_create_from_config(const nb_rpc_config_doc_t *config, nb_node_t **out);

#ifdef __cplusplus
}
#endif

#endif /* NB_RPC_CONFIG_H */
