#ifndef NB_CONFIG_H
#define NB_CONFIG_H

/*
 * Single configuration entry point for network bridge.
 *
 * The JSON is grouped by how it is used, not by internal module:
 *
 * {
 *   "name": "echo-server",
 *   "threads": { "io": 2, "handlers": 4, "send_lanes": 0 },
 *   "limits": {
 *     "max_inflight": 65536,
 *     "max_streams": 64,
 *     "handler_queue": 64,
 *     "peer_rate_per_second": 0
 *   },
 *   "rpc": {
 *     "mode": "static",
 *     "listen": ["tcp://0.0.0.0:42420"],
 *     "max_message_bytes": 1048576,
 *     "backpressure_bytes": 4194304,
 *     "udp": { "fragment_bytes": 1440, "rtcp_interval": "1s", "rtcp": true },
 *     "discovery": {
 *       "broadcast": "255.255.255.255:41953",
 *       "peers": ["10.0.0.2:41953"],
 *       "hello_interval": "3s",
 *       "node_ttl": "9s"
 *     },
 *     "routes": [ { "service": "EchoService", "endpoint": "tcp://10.0.0.2:42420" } ]
 *   },
 *   "pubsub": {
 *     "backends": [
 *       "dds://0",
 *       { "endpoint": "shm://orders:1", "intra_process": "off",
 *         "qos": { "reliability": "reliable" } }
 *     ],
 *     "default_qos": { "durability": "transient_local" }
 *   },
 *   "metrics": { "enabled": true, "latency_histogram": true, "sample_every": 1 },
 *   "log": { "enabled": true, "level": "info" }
 * }
 *
 * Durations are numbers of milliseconds or strings with a unit ("250ms", "2s").
 * The same endpoint listed under rpc.listen and pubsub.backends is one
 * transport serving both spines.
 *
 * String enums (mode, intra_process, log.level, QoS values) are lowercase and
 * case-sensitive.
 *
 * A field of the wrong type, out of range, or an invalid URI returns
 * NB_ERR_INVALID; unknown fields are ignored (and logged as warnings). Pass a
 * non-NULL error output to locate the failing field and its reason (for
 * example "rpc.listen[0]" or "pubsub.default_qos.depth").
 *
 * Ownership: a parsed document is opaque and heap allocated. Release it with
 * nb_config_doc_destroy(). nb_node_create / nb_node_create_from_config copy the
 * strings and endpoints they need into the node, so a document only has to
 * outlive the create call.
 */

#include <nb/endpoint.h>
#include <nb/qos.h>
#include <nb/service_table.h>
#include <nb/types.h>

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

/* ---- Bindings ---- */
/*
 * One transport instance the node starts once. This is the normalized form:
 * the JSON parser produces it from rpc.listen (roles = RPC) and
 * pubsub.backends (roles = PS), merging entries that name the same endpoint.
 * Code-built configs can fill it directly.
 *
 * `roles` says which spines may use the binding (NB_PROVIDER_ROLE_RPC /
 * NB_PROVIDER_ROLE_PS); 0 means the provider's default, i.e. every role it
 * implements. `ps_qos` applies to the pub/sub role only; INHERIT takes
 * nb_pubsub_config_t.default_qos.
 */
typedef struct nb_binding_config {
    const char *name;                 /* provider/binding name, e.g. "tcp" */
    const char *endpoint;             /* URI, e.g. tcp://127.0.0.1:8080 */
    unsigned roles;                   /* NB_PROVIDER_ROLE_*; 0 = provider default */
    nb_intra_process_t intra_process; /* provider-local event fast path */
    /* Provider busy-poll window in microseconds; 0 blocks on wakeup. Honored by
     * the shm provider: after draining, keep polling this long before falling
     * back to a blocking wait. Trades one core's idle for lower tail latency. */
    uint32_t poll_us;
    nb_ps_qos_t ps_qos;               /* pub/sub role QoS; INHERIT = default */
} nb_binding_config_t;

/* ---- Per-module configuration ---- */

/* Threads, concurrency, and flow-control policy. 0 selects the module default.
 * Ranges are enforced by nb_config_parse and clamped by nb_node_config_normalize. */
typedef struct nb_node_runtime_config {
    unsigned io_threads;              /* poller threads; 0 = hw, max NB_MAX_IO_THREADS */
    unsigned handler_threads;         /* worker pool size; 0 = disabled, max NB_MAX_HANDLER_THREADS */
    unsigned handler_max_concurrency; /* max in-flight worker jobs; 0 = unlimited */
    uint32_t max_inflight;            /* max pending client calls; 0 = 65536 */
    uint32_t max_streams;             /* max inbound streams; 0 = 64, max 64 */
    uint32_t peer_rate_limit_pps;     /* per-peer requests/sec cap; 0 = unlimited */
    unsigned send_lanes;              /* dedicated send-lane pollers; 0 = none, max NB_MAX_SEND_LANES */
} nb_node_runtime_config_t;

/* Service discovery. */
typedef struct nb_node_discovery_config {
    bool disable;               /* true disables discovery (equivalent to mode=static) */
    const char *broadcast_host; /* broadcast/discovery address; default 255.255.255.255 */
    uint16_t port;              /* discovery port; 0 = NB_DISCOVERY_PORT */
    uint32_t hello_interval_ms; /* HELLO interval; 0 = 3000 */
    uint32_t node_ttl_ms;       /* peer expiry; 0 = 9000 */
} nb_node_discovery_config_t;

/* Metrics collection. Zero-initialized means enabled.
 * These switches are process-global: nb_node_create applies them to the shared
 * metric runtime, so the most recently created node wins. */
typedef struct nb_node_metrics_config {
    bool disable;                    /* true disables all metric collection */
    bool disable_latency_histogram;  /* true disables the latency histogram */
    uint32_t sample_every;           /* 0/1 = every sample; N = every Nth sample */
} nb_node_metrics_config_t;

/* Logging. Zero-initialized means enabled. Process-global, like metrics. */
typedef struct nb_node_log_config {
    bool disable;             /* true disables logging */
    nb_log_level_t level;     /* default NB_LOG_LEVEL_INFO */
} nb_node_log_config_t;

/* One static route entry (service -> endpoint URI). */
typedef struct nb_static_route {
    char service[NB_SERVICE_NAME_MAX];
    char endpoint[NB_ENDPOINT_URI_MAX];
} nb_static_route_t;

/* One explicit discovery peer (unicast HELLO target). */
typedef struct nb_peer_endpoint {
    char host[NB_HOST_MAX];
    uint16_t port;
} nb_peer_endpoint_t;

/* ---- RPC spine ---- */
/*
 * RPC link tuning, service discovery and routes. Everything here is used by the
 * RPC spine only; a process that only publishes/subscribes leaves it at the
 * defaults.
 */
typedef struct nb_rpc_config {
    nb_node_mode_t mode;                     /* MESH (default) / STATIC */
    const nb_service_table_t *service_table; /* optional shared declaration table; static lifetime */
    /* Fallback binding for code-built configs that leave nb_node_config_t.bindings
     * empty. JSON configs declare explicit rpc.listen endpoints instead, so these
     * three fields are not part of the JSON schema. */
    nb_transport_kind_t kind;                /* default binding kind; default UDP */
    const char *bind_host;                   /* bind address; default "0.0.0.0" */
    uint16_t data_port;                      /* data-plane port; 0 = auto-assign */
    uint16_t frag_bytes;                     /* UDP fragment size; 0 = 1440, else 256..8192 */
    uint32_t max_msg_bytes;                  /* max encoded envelope; 0 = 1 MiB, max 64 MiB */
    uint32_t backpressure_bytes;             /* outbound high-water mark; 0 = 4 MiB */
    uint32_t rtcp_interval_ms;               /* UDP RTCP interval; 0 = 1000 ms */
    bool disable_rtcp;                       /* true disables UDP RTCP */
    nb_node_discovery_config_t discovery;    /* HELLO-based service discovery */
    /* Optional static routes applied during nb_node_create. */
    const nb_static_route_t *routes;
    size_t route_count;
    /* Optional unicast discovery peers applied during nb_node_create. */
    const nb_peer_endpoint_t *peers;
    size_t peer_count;
} nb_rpc_config_t;

/* ---- Pub/sub spine ---- */
typedef struct nb_pubsub_config {
    nb_ps_qos_t default_qos; /* inherited by bindings whose ps_qos is INHERIT */
} nb_pubsub_config_t;

/* Composite node configuration: identity, shared runtime, and the two spines. */
typedef struct nb_node_config {
    const char *name;          /* node name (logs/discovery); may be NULL */
    nb_node_runtime_config_t runtime;
    nb_rpc_config_t rpc;
    /* Transports, started once each and shared by both spines. */
    const nb_binding_config_t *bindings;
    size_t binding_count;
    nb_pubsub_config_t pubsub;
    nb_node_metrics_config_t metrics;
    nb_node_log_config_t log;
} nb_node_config_t;

/* ---- Parse results ---- */

/* One configured binding: a naming label plus its endpoint URI. */
typedef struct nb_binding_endpoint {
    char name[NB_SCHEME_MAX];
    char endpoint[NB_ENDPOINT_URI_MAX];
} nb_binding_endpoint_t;

/* Opaque parsed document. Use nb_config_node() to inspect the result. */
typedef struct nb_config_doc nb_config_doc_t;

/* Structured parse error: path looks like "rpc.frag_bytes",
 * "rpc.routes[0].endpoint" or "bindings[1].ps_qos.depth". */
typedef struct nb_config_error {
    int code; /* NB_ERR_* */
    char path[96];
    char reason[128];
} nb_config_error_t;

/* ---- Defaults ---- */
void nb_node_runtime_config_default(nb_node_runtime_config_t *config);
void nb_rpc_config_default(nb_rpc_config_t *config);
void nb_pubsub_config_default(nb_pubsub_config_t *config);
void nb_node_discovery_config_default(nb_node_discovery_config_t *config);
void nb_node_metrics_config_default(nb_node_metrics_config_t *config);
void nb_node_log_config_default(nb_node_log_config_t *config);
/* Fill composite defaults; for callers that build nb_node_config_t directly. */
void nb_node_config_default(nb_node_config_t *config);
void nb_binding_config_default(nb_binding_config_t *config);
/*
 * Strict validation for callers building nb_node_config_t by hand.
 * nb_node_create normalizes (fills defaults and clamps) instead; validate when
 * an out-of-range value must be reported rather than silently adjusted.
 * Returns NB_OK, NB_ERR_INVALID, or NB_ERR_TOOBIG (name/host too long).
 */
int nb_node_config_validate(const nb_node_config_t *config);
/*
 * Normalize: replace 0/NULL fields with defaults and clamp values to their
 * valid range. nb_node_create applies this to a copy of the incoming config,
 * so callers that build nb_node_config_t directly need no fallback of their
 * own. log.level == 0 is a valid NB_LOG_LEVEL_ASSERT and is not rewritten.
 */
void nb_node_config_normalize(nb_node_config_t *config);

/* ---- Load and create ---- */
/* Parse a JSON string. On success *out receives a new document; on failure
 * *out is NULL. error may be NULL; when non-NULL it receives the failing field
 * path and reason. */
int nb_config_parse(const char *json, nb_config_doc_t **out, nb_config_error_t *error);
/* Read a file and parse it. error may be NULL. */
int nb_config_parse_file(const char *path, nb_config_doc_t **out, nb_config_error_t *error);

/* Release a document returned by nb_config_parse/nb_config_parse_file. */
void nb_config_doc_destroy(nb_config_doc_t *doc);
/* Read-only view of the parsed node configuration; NULL for a NULL document. */
const nb_node_config_t *nb_config_node(const nb_config_doc_t *doc);

/* Create a node from a parsed document and apply static routes and explicit
 * discovery peers. */
int nb_node_create_from_config(const nb_config_doc_t *config, nb_node_t **out);

#ifdef __cplusplus
}
#endif

#endif /* NB_CONFIG_H */
