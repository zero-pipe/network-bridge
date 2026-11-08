/*
 * JSON 配置解析。
 *
 * 配置按使用方式分块，字段名直接描述含义（见 docs/api/config.md）：
 *
 *   name / threads / limits / rpc / pubsub / metrics / log
 *
 * 这里只做解析与装配：默认值、归一化和严格校验在 config_defaults.c。
 * 字符串一律复制进 nb_config_doc_t 自己的缓冲区，cJSON 树解析完即可释放。
 */
#include <nb/config.h>
#include <nb/node.h>
#include <nb/transport_provider.h>

#define NB_LOG_TAG "config"
#include <nb/log.h>

#include "caps.h"
#include "defaults.h"

#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    nb_binding_endpoint_t endpoints[NB_CONFIG_MAX_BINDINGS];
    nb_binding_config_t bindings[NB_CONFIG_MAX_BINDINGS];
    unsigned binding_ps[NB_CONFIG_MAX_BINDINGS]; /* binding declared as a ps backend */
    size_t binding_count;
};

typedef nb_config_doc_t cfg_doc_t;

#define CFG_KEYS(array) array, sizeof(array) / sizeof((array)[0])

/* ---- error helpers ------------------------------------------------------ */

static int cfg_fail_code(nb_config_error_t *err, int code, const char *path, const char *reason)
{
    if (err) {
        err->code = code;
        snprintf(err->path, sizeof(err->path), "%s", path ? path : "");
        snprintf(err->reason, sizeof(err->reason), "%s", reason ? reason : "");
    }
    return code;
}

static int cfg_fail(nb_config_error_t *err, const char *path, const char *reason)
{
    return cfg_fail_code(err, NB_ERR_INVALID, path, reason);
}

static void *cfg_fail_ptr(nb_config_error_t *err, const char *path, const char *reason)
{
    cfg_fail(err, path, reason);
    return NULL;
}

static void cfg_join(char *out, size_t cap, const char *parent, const char *key)
{
    if (parent && parent[0]) {
        snprintf(out, cap, "%s.%s", parent, key);
    } else {
        snprintf(out, cap, "%s", key);
    }
}

/* ---- field readers ------------------------------------------------------ */
/*
 * Every reader takes an optional field: absent leaves *out untouched and
 * returns NB_OK. Type errors report parent.key (or parent[index]).
 */

static int cfg_child(const cJSON *parent, const char *key, const cJSON **out,
                     nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    char full[96];

    *out = NULL;
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsObject(item)) {
        cfg_join(full, sizeof(full), path, key);
        return cfg_fail(err, full, "must be an object");
    }
    *out = item;
    return NB_OK;
}

static int cfg_string(const cJSON *obj, const char *key, const char **out,
                      nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        cfg_join(full, sizeof(full), path, key);
        return cfg_fail(err, full, "must be a string");
    }
    *out = item->valuestring;
    return NB_OK;
}

static int cfg_u32(const cJSON *obj, const char *key, uint32_t *out,
                   nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > 4294967295.0 ||
        (double)(uint32_t)item->valuedouble != item->valuedouble) {
        cfg_join(full, sizeof(full), path, key);
        return cfg_fail(err, full, "must be a non-negative integer");
    }
    *out = (uint32_t)item->valuedouble;
    return NB_OK;
}

static int cfg_bool(const cJSON *obj, const char *key, bool *out,
                    nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsBool(item)) {
        cfg_join(full, sizeof(full), path, key);
        return cfg_fail(err, full, "must be a boolean");
    }
    *out = cJSON_IsTrue(item) ? true : false;
    return NB_OK;
}

/* A duration is a number of milliseconds or a string such as "250ms" / "2s". */
static int cfg_parse_duration(const char *text, uint32_t *out_ms)
{
    char *end = NULL;
    unsigned long value;

    if (!text || !text[0]) {
        return NB_ERR_INVALID;
    }
    value = strtoul(text, &end, 10);
    if (end == text) {
        return NB_ERR_INVALID;
    }
    if (*end == '\0') {
        /* bare number = milliseconds */
    } else if (strcmp(end, "ms") == 0) {
        /* as written */
    } else if (strcmp(end, "s") == 0) {
        if (value > 4294967295ul / 1000ul) {
            return NB_ERR_INVALID;
        }
        value *= 1000ul;
    } else {
        return NB_ERR_INVALID;
    }
    if (value > 4294967295ul) {
        return NB_ERR_INVALID;
    }
    *out_ms = (uint32_t)value;
    return NB_OK;
}

static int cfg_duration(const cJSON *obj, const char *key, uint32_t *out,
                        nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > 4294967295.0 ||
            (double)(uint32_t)item->valuedouble != item->valuedouble) {
            cfg_join(full, sizeof(full), path, key);
            return cfg_fail(err, full, "must be a duration");
        }
        *out = (uint32_t)item->valuedouble;
        return NB_OK;
    }
    if (cJSON_IsString(item) && item->valuestring &&
        cfg_parse_duration(item->valuestring, out) == NB_OK) {
        return NB_OK;
    }
    cfg_join(full, sizeof(full), path, key);
    return cfg_fail(err, full, "must be a duration like \"250ms\" or \"2s\"");
}

static int cfg_enum_name(const char *text, const char *const *names,
                         const int *values, size_t count, int *out)
{
    size_t i;

    if (!text) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(text, names[i]) == 0) {
            *out = values[i];
            return NB_OK;
        }
    }
    return NB_ERR_INVALID;
}

static int cfg_enum(const cJSON *obj, const char *key,
                    const char *const *names, size_t count, const int *values,
                    int *out, nb_config_error_t *err, const char *path)
{
    const char *text = NULL;
    char full[96];
    int rc = cfg_string(obj, key, &text, err, path);

    if (rc != NB_OK || !text) {
        return rc;
    }
    if (cfg_enum_name(text, names, values, count, out) != NB_OK) {
        cfg_join(full, sizeof(full), path, key);
        return cfg_fail(err, full, "invalid value");
    }
    return NB_OK;
}

static void cfg_warn_unknown(const cJSON *obj, const char *const *known, size_t count,
                             const char *section)
{
    const cJSON *child;

    if (!obj || !cJSON_IsObject(obj)) {
        return;
    }
    cJSON_ArrayForEach(child, obj)
    {
        size_t i;
        int found = 0;
        char full[96];

        if (!child->string) {
            continue;
        }
        for (i = 0; i < count; i++) {
            if (known[i] && strcmp(known[i], child->string) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            cfg_join(full, sizeof(full), section, child->string);
            nb_log_warn("unknown config key ignored: %s", full);
        }
    }
}

/* "host:port" -> host buffer + port. */
static int cfg_host_port(const char *text, char *host, size_t cap, uint16_t *port)
{
    const char *colon = text ? strrchr(text, ':') : NULL;
    size_t len;
    unsigned long value;
    char *end = NULL;

    if (!colon || colon == text) {
        return NB_ERR_INVALID;
    }
    len = (size_t)(colon - text);
    if (len == 0 || len >= cap) {
        return NB_ERR_INVALID;
    }
    value = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || value > 65535ul) {
        return NB_ERR_INVALID;
    }
    memcpy(host, text, len);
    host[len] = '\0';
    *port = (uint16_t)value;
    return NB_OK;
}

/* ---- QoS ---------------------------------------------------------------- */

static int cfg_parse_qos(const cJSON *obj, nb_ps_qos_t *qos, nb_config_error_t *err,
                         const char *path)
{
    static const char *const known[] = {
        "reliability", "durability", "history", "depth", "deadline", "lifespan",
        "latency_budget", "min_separation", "liveliness", "lease_duration",
        "ownership", "partition", "max_samples", "max_instances",
        "max_samples_per_instance", "autodispose_unregistered",
        "presentation_scope", "coherent_access", "ordered_access"};
    static const char *const reliability_names[] = {"best_effort", "reliable"};
    static const int reliability_values[] = {NB_QOS_RELIABILITY_BEST_EFFORT,
                                             NB_QOS_RELIABILITY_RELIABLE};
    static const char *const durability_names[] = {"volatile", "transient_local"};
    static const int durability_values[] = {NB_QOS_DURABILITY_VOLATILE,
                                            NB_QOS_DURABILITY_TRANSIENT_LOCAL};
    static const char *const history_names[] = {"keep_last", "keep_all"};
    static const int history_values[] = {NB_QOS_HISTORY_KEEP_LAST, NB_QOS_HISTORY_KEEP_ALL};
    static const char *const liveliness_names[] = {"automatic", "manual_by_participant",
                                                   "manual_by_topic"};
    static const int liveliness_values[] = {NB_QOS_LIVELINESS_AUTOMATIC,
                                            NB_QOS_LIVELINESS_MANUAL_BY_PARTICIPANT,
                                            NB_QOS_LIVELINESS_MANUAL_BY_TOPIC};
    static const char *const ownership_names[] = {"shared", "exclusive"};
    static const int ownership_values[] = {NB_QOS_OWNERSHIP_SHARED, NB_QOS_OWNERSHIP_EXCLUSIVE};
    static const char *const presentation_names[] = {"instance", "topic", "group"};
    static const int presentation_values[] = {NB_QOS_PRESENTATION_INSTANCE,
                                              NB_QOS_PRESENTATION_TOPIC,
                                              NB_QOS_PRESENTATION_GROUP};
    const char *partition = NULL;
    int rc;

    nb_ps_qos_init(qos);
    cfg_warn_unknown(obj, CFG_KEYS(known), path);

    rc = cfg_enum(obj, "reliability", CFG_KEYS(reliability_names), reliability_values,
                  (int *)&qos->reliability, err, path);
    if (rc == NB_OK) {
        rc = cfg_enum(obj, "durability", CFG_KEYS(durability_names), durability_values,
                      (int *)&qos->durability, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_enum(obj, "history", CFG_KEYS(history_names), history_values,
                      (int *)&qos->history, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_enum(obj, "liveliness", CFG_KEYS(liveliness_names), liveliness_values,
                      (int *)&qos->liveliness, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_enum(obj, "ownership", CFG_KEYS(ownership_names), ownership_values,
                      (int *)&qos->ownership, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_enum(obj, "presentation_scope", CFG_KEYS(presentation_names),
                      presentation_values, (int *)&qos->presentation_scope, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "depth", &qos->depth, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "deadline", &qos->deadline_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "lifespan", &qos->lifespan_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "latency_budget", &qos->latency_budget_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "min_separation", &qos->min_separation_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "lease_duration", &qos->lease_duration_ms, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "max_samples", &qos->max_samples, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "max_instances", &qos->max_instances, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "max_samples_per_instance", &qos->max_samples_per_instance, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_bool(obj, "autodispose_unregistered",
                      &qos->autodispose_unregistered_instances, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_bool(obj, "coherent_access", &qos->coherent_access, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_bool(obj, "ordered_access", &qos->ordered_access, err, path);
    }
    if (rc == NB_OK) {
        rc = cfg_string(obj, "partition", &partition, err, path);
    }
    if (rc != NB_OK) {
        return rc;
    }
    if (partition && partition[0]) {
        if (strlen(partition) >= NB_QOS_PARTITION_MAX) {
            char full[96];

            cfg_join(full, sizeof(full), path, "partition");
            return cfg_fail(err, full, "too long");
        }
        snprintf(qos->partition, sizeof(qos->partition), "%s", partition);
    }
    if (nb_ps_qos_validate(qos) != NB_OK) {
        return cfg_fail(err, path, nb_ps_qos_validate_reason(qos));
    }
    return NB_OK;
}

/* ---- bindings ----------------------------------------------------------- */

static nb_binding_config_t *cfg_add_binding(cfg_doc_t *doc, const char *endpoint,
                                            unsigned roles, int ps_backend,
                                            nb_config_error_t *err, const char *path)
{
    nb_endpoint_t parsed;
    size_t i;

    if (!endpoint || nb_endpoint_parse(endpoint, &parsed) != NB_OK) {
        return cfg_fail_ptr(err, path, "invalid endpoint URI");
    }
    for (i = 0; i < doc->binding_count; i++) {
        if (strcmp(doc->bindings[i].endpoint, endpoint) == 0) {
            if (ps_backend && doc->binding_ps[i]) {
                return cfg_fail_ptr(err, path, "duplicate pub/sub backend");
            }
            doc->bindings[i].roles |= roles;
            if (ps_backend) {
                doc->binding_ps[i] = 1;
            }
            return &doc->bindings[i];
        }
    }
    if (doc->binding_count >= NB_CONFIG_MAX_BINDINGS) {
        return cfg_fail_ptr(err, path, "too many bindings");
    }
    if (strlen(endpoint) >= NB_ENDPOINT_URI_MAX || strlen(parsed.scheme) >= NB_SCHEME_MAX) {
        return cfg_fail_ptr(err, path, "endpoint too long");
    }
    {
        nb_binding_endpoint_t *ep = &doc->endpoints[doc->binding_count];
        nb_binding_config_t *binding = &doc->bindings[doc->binding_count];

        snprintf(ep->name, sizeof(ep->name), "%s", parsed.scheme);
        snprintf(ep->endpoint, sizeof(ep->endpoint), "%s", endpoint);
        memset(binding, 0, sizeof(*binding));
        binding->name = ep->name;
        binding->endpoint = ep->endpoint;
        binding->roles = roles;
        binding->intra_process = NB_INTRA_PROCESS_OFF;
        nb_ps_qos_init(&binding->ps_qos);
        doc->binding_ps[doc->binding_count] = (unsigned)(ps_backend ? 1 : 0);
        doc->binding_count++;
        return binding;
    }
}

/* ---- section parsers ---------------------------------------------------- */

static int cfg_parse_name(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    const char *name = NULL;
    int rc = cfg_string(root, "name", &name, err, "");

    if (rc != NB_OK) {
        return rc;
    }
    if (name) {
        if (strlen(name) >= sizeof(doc->name)) {
            return cfg_fail(err, "name", "too long");
        }
        snprintf(doc->name, sizeof(doc->name), "%s", name);
    }
    return NB_OK;
}

static int cfg_parse_threads(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"io", "handlers", "send_lanes"};
    const cJSON *obj = NULL;
    uint32_t io = 0;
    uint32_t handlers = 0;
    uint32_t send_lanes = 0;
    int rc = cfg_child(root, "threads", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    cfg_warn_unknown(obj, CFG_KEYS(known), "threads");
    rc = cfg_u32(obj, "io", &io, err, "threads");
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "handlers", &handlers, err, "threads");
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "send_lanes", &send_lanes, err, "threads");
    }
    if (rc != NB_OK) {
        return rc;
    }
    if (io > NB_TRANSPORT_MAX_IO) {
        return cfg_fail(err, "threads.io", "too large");
    }
    if (handlers > NB_MAX_HANDLER_THREADS) {
        return cfg_fail(err, "threads.handlers", "too large");
    }
    if (send_lanes > NB_MAX_SEND_LANES) {
        return cfg_fail(err, "threads.send_lanes", "too large");
    }
    doc->node.runtime.io_threads = (unsigned)io;
    doc->node.runtime.handler_threads = (unsigned)handlers;
    doc->node.runtime.send_lanes = (unsigned)send_lanes;
    return NB_OK;
}

static int cfg_parse_limits(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"max_inflight", "max_streams", "handler_queue",
                                        "peer_rate_per_second"};
    const cJSON *obj = NULL;
    uint32_t value = 0;
    int rc = cfg_child(root, "limits", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    cfg_warn_unknown(obj, CFG_KEYS(known), "limits");
    rc = cfg_u32(obj, "max_inflight", &doc->node.runtime.max_inflight, err, "limits");
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "max_streams", &doc->node.runtime.max_streams, err, "limits");
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "handler_queue", &doc->node.runtime.handler_max_concurrency, err,
                     "limits");
    }
    if (rc == NB_OK) {
        value = doc->node.runtime.peer_rate_limit_pps;
        rc = cfg_u32(obj, "peer_rate_per_second", &value, err, "limits");
        doc->node.runtime.peer_rate_limit_pps = value;
    }
    if (rc == NB_OK && doc->node.runtime.max_streams > NB_STREAM_MAX_RECV_STREAMS) {
        rc = cfg_fail(err, "limits.max_streams", "too large");
    }
    return rc;
}

static int cfg_parse_routes(cfg_doc_t *doc, const cJSON *rpc, nb_config_error_t *err)
{
    static const char *const known[] = {"service", "endpoint"};
    const cJSON *routes = cJSON_GetObjectItemCaseSensitive(rpc, "routes");
    const cJSON *item;
    size_t index = 0;

    if (!routes) {
        return NB_OK;
    }
    if (!cJSON_IsArray(routes)) {
        return cfg_fail(err, "rpc.routes", "must be an array");
    }
    cJSON_ArrayForEach(item, routes)
    {
        char path[96];
        const char *service = NULL;
        const char *endpoint = NULL;
        nb_endpoint_t parsed;
        nb_static_route_t *route;
        int rc;

        snprintf(path, sizeof(path), "rpc.routes[%lu]", (unsigned long)index++);
        if (!cJSON_IsObject(item)) {
            return cfg_fail(err, path, "each route must be an object");
        }
        cfg_warn_unknown(item, CFG_KEYS(known), path);
        if (doc->route_count >= NB_CONFIG_MAX_ROUTES) {
            return cfg_fail(err, "rpc.routes", "too many routes");
        }
        rc = cfg_string(item, "service", &service, err, path);
        if (rc != NB_OK) {
            return rc;
        }
        rc = cfg_string(item, "endpoint", &endpoint, err, path);
        if (rc != NB_OK) {
            return rc;
        }
        if (!service || !service[0]) {
            char full[96];

            cfg_join(full, sizeof(full), path, "service");
            return cfg_fail(err, full, "required");
        }
        if (!endpoint || nb_endpoint_parse(endpoint, &parsed) != NB_OK) {
            char full[96];

            cfg_join(full, sizeof(full), path, "endpoint");
            return cfg_fail(err, full, "invalid endpoint URI");
        }
        if (strlen(service) >= NB_SERVICE_NAME_MAX ||
            strlen(endpoint) >= NB_ENDPOINT_URI_MAX) {
            return cfg_fail(err, path, "value too long");
        }
        route = &doc->routes[doc->route_count];
        snprintf(route->service, sizeof(route->service), "%s", service);
        snprintf(route->endpoint, sizeof(route->endpoint), "%s", endpoint);
        doc->route_count++;
    }
    return NB_OK;
}

static int cfg_parse_peers(cfg_doc_t *doc, const cJSON *discovery, nb_config_error_t *err)
{
    const cJSON *peers = cJSON_GetObjectItemCaseSensitive(discovery, "peers");
    const cJSON *item;
    size_t index = 0;

    if (!peers) {
        return NB_OK;
    }
    if (!cJSON_IsArray(peers)) {
        return cfg_fail(err, "rpc.discovery.peers", "must be an array");
    }
    cJSON_ArrayForEach(item, peers)
    {
        char path[96];
        nb_peer_endpoint_t *peer;

        snprintf(path, sizeof(path), "rpc.discovery.peers[%lu]", (unsigned long)index++);
        if (!cJSON_IsString(item) || !item->valuestring) {
            return cfg_fail(err, path, "must be a string");
        }
        if (doc->peer_count >= NB_CONFIG_MAX_PEERS) {
            return cfg_fail(err, "rpc.discovery.peers", "too many peers");
        }
        peer = &doc->peers[doc->peer_count];
        if (cfg_host_port(item->valuestring, peer->host, sizeof(peer->host), &peer->port) !=
            NB_OK) {
            return cfg_fail(err, path, "must be host:port");
        }
        doc->peer_count++;
    }
    return NB_OK;
}

static int cfg_parse_discovery(cfg_doc_t *doc, const cJSON *rpc, nb_config_error_t *err)
{
    static const char *const known[] = {"broadcast", "peers", "hello_interval", "node_ttl"};
    const cJSON *obj = NULL;
    const char *broadcast = NULL;
    int rc = cfg_child(rpc, "discovery", &obj, err, "rpc");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    cfg_warn_unknown(obj, CFG_KEYS(known), "rpc.discovery");
    rc = cfg_string(obj, "broadcast", &broadcast, err, "rpc.discovery");
    if (rc == NB_OK && broadcast) {
        if (cfg_host_port(broadcast,
                          doc->discovery_host,
                          sizeof(doc->discovery_host),
                          &doc->node.rpc.discovery.port) != NB_OK) {
            return cfg_fail(err, "rpc.discovery.broadcast", "must be host:port");
        }
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "hello_interval", &doc->node.rpc.discovery.hello_interval_ms, err,
                          "rpc.discovery");
    }
    if (rc == NB_OK) {
        rc = cfg_duration(obj, "node_ttl", &doc->node.rpc.discovery.node_ttl_ms, err,
                          "rpc.discovery");
    }
    if (rc == NB_OK) {
        rc = cfg_parse_peers(doc, obj, err);
    }
    return rc;
}

static int cfg_parse_listen(cfg_doc_t *doc, const cJSON *rpc, nb_config_error_t *err)
{
    const cJSON *listen = cJSON_GetObjectItemCaseSensitive(rpc, "listen");
    const cJSON *item;
    size_t index = 0;

    if (!listen) {
        return NB_OK;
    }
    if (!cJSON_IsArray(listen)) {
        return cfg_fail(err, "rpc.listen", "must be an array of endpoint URIs");
    }
    cJSON_ArrayForEach(item, listen)
    {
        char path[96];

        snprintf(path, sizeof(path), "rpc.listen[%lu]", (unsigned long)index++);
        if (!cJSON_IsString(item) || !item->valuestring) {
            return cfg_fail(err, path, "must be an endpoint URI string");
        }
        if (!cfg_add_binding(doc, item->valuestring, NB_PROVIDER_ROLE_RPC, 0, err, path)) {
            return NB_ERR_INVALID;
        }
    }
    return NB_OK;
}

static int cfg_parse_rpc(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"mode", "listen", "discovery", "routes", "udp",
                                        "max_message_bytes", "backpressure_bytes"};
    static const char *const mode_names[] = {"mesh", "static"};
    static const int mode_values[] = {NB_NODE_MODE_MESH, NB_NODE_MODE_STATIC};
    static const char *const udp_known[] = {"fragment_bytes", "rtcp_interval", "rtcp"};
    const cJSON *rpc = NULL;
    const cJSON *udp = NULL;
    uint32_t value = 0;
    bool rtcp = true;
    int rc = cfg_child(root, "rpc", &rpc, err, "");

    if (rc != NB_OK || !rpc) {
        return rc;
    }
    cfg_warn_unknown(rpc, CFG_KEYS(known), "rpc");

    rc = cfg_enum(rpc, "mode", CFG_KEYS(mode_names), mode_values,
                  (int *)&doc->node.rpc.mode, err, "rpc");
    if (rc == NB_OK) {
        rc = cfg_u32(rpc, "max_message_bytes", &doc->node.rpc.max_msg_bytes, err, "rpc");
    }
    if (rc == NB_OK) {
        if (doc->node.rpc.max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
            return cfg_fail(err, "rpc.max_message_bytes", "too large");
        }
        rc = cfg_u32(rpc, "backpressure_bytes", &doc->node.rpc.backpressure_bytes, err, "rpc");
    }
    if (rc == NB_OK) {
        rc = cfg_child(rpc, "udp", &udp, err, "rpc");
    }
    if (rc == NB_OK && udp) {
        cfg_warn_unknown(udp, CFG_KEYS(udp_known), "rpc.udp");
        value = doc->node.rpc.frag_bytes;
        rc = cfg_u32(udp, "fragment_bytes", &value, err, "rpc.udp");
        if (rc == NB_OK) {
            if (value != 0 && (value < NB_DEFAULT_FRAG_MIN_BYTES ||
                               value > NB_DEFAULT_FRAG_MAX_BYTES)) {
                return cfg_fail(err, "rpc.udp.fragment_bytes",
                                "must be 0 or within 256..8192");
            }
            doc->node.rpc.frag_bytes = (uint16_t)value;
            rc = cfg_duration(
                udp, "rtcp_interval", &doc->node.rpc.rtcp_interval_ms, err, "rpc.udp");
        }
        if (rc == NB_OK) {
            rc = cfg_bool(udp, "rtcp", &rtcp, err, "rpc.udp");
        }
        if (rc == NB_OK) {
            doc->node.rpc.disable_rtcp = rtcp ? false : true;
        }
    }
    if (rc == NB_OK) {
        rc = cfg_parse_discovery(doc, rpc, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_routes(doc, rpc, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_listen(doc, rpc, err);
    }
    return rc;
}

static int cfg_parse_backends(cfg_doc_t *doc, const cJSON *pubsub, nb_config_error_t *err)
{
    static const char *const known[] = {"endpoint", "qos", "intra_process", "poll_us"};
    static const char *const intra_names[] = {"off", "auto", "only"};
    static const int intra_values[] = {NB_INTRA_PROCESS_OFF, NB_INTRA_PROCESS_AUTO,
                                       NB_INTRA_PROCESS_ONLY};
    const cJSON *backends = cJSON_GetObjectItemCaseSensitive(pubsub, "backends");
    const cJSON *item;
    size_t index = 0;

    if (!backends) {
        return NB_OK;
    }
    if (!cJSON_IsArray(backends)) {
        return cfg_fail(err, "pubsub.backends", "must be an array");
    }
    cJSON_ArrayForEach(item, backends)
    {
        char path[96];
        char qos_path[96];
        const char *endpoint = NULL;
        const cJSON *qos = NULL;
        nb_binding_config_t *binding;
        int intra = NB_INTRA_PROCESS_OFF;
        uint32_t poll_us = 0;
        int rc;

        snprintf(path, sizeof(path), "pubsub.backends[%lu]", (unsigned long)index);
        snprintf(qos_path, sizeof(qos_path), "pubsub.backends[%lu].qos", (unsigned long)index);
        index++;
        if (cJSON_IsString(item)) {
            endpoint = item->valuestring;
        } else if (cJSON_IsObject(item)) {
            cfg_warn_unknown(item, CFG_KEYS(known), path);
            rc = cfg_string(item, "endpoint", &endpoint, err, path);
            if (rc != NB_OK) {
                return rc;
            }
            rc = cfg_enum(item, "intra_process", CFG_KEYS(intra_names), intra_values, &intra,
                          err, path);
            if (rc != NB_OK) {
                return rc;
            }
            rc = cfg_u32(item, "poll_us", &poll_us, err, path);
            if (rc != NB_OK) {
                return rc;
            }
            rc = cfg_child(item, "qos", &qos, err, path);
            if (rc != NB_OK) {
                return rc;
            }
        } else {
            return cfg_fail(err, path, "must be an endpoint string or an object");
        }
        if (!endpoint) {
            char full[96];

            cfg_join(full, sizeof(full), path, "endpoint");
            return cfg_fail(err, full, "required");
        }
        binding = cfg_add_binding(doc, endpoint, NB_PROVIDER_ROLE_PS, 1, err, path);
        if (!binding) {
            return NB_ERR_INVALID;
        }
        binding->intra_process = (nb_intra_process_t)intra;
        binding->poll_us = poll_us;
        if (qos) {
            rc = cfg_parse_qos(qos, &binding->ps_qos, err, qos_path);
            if (rc != NB_OK) {
                return rc;
            }
        }
    }
    return NB_OK;
}

static int cfg_parse_pubsub(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"backends", "default_qos"};
    const cJSON *pubsub = NULL;
    const cJSON *qos = NULL;
    int rc = cfg_child(root, "pubsub", &pubsub, err, "");

    if (rc != NB_OK || !pubsub) {
        return rc;
    }
    cfg_warn_unknown(pubsub, CFG_KEYS(known), "pubsub");
    rc = cfg_parse_backends(doc, pubsub, err);
    if (rc == NB_OK) {
        rc = cfg_child(pubsub, "default_qos", &qos, err, "pubsub");
    }
    if (rc == NB_OK && qos) {
        rc = cfg_parse_qos(qos, &doc->node.pubsub.default_qos, err, "pubsub.default_qos");
    }
    return rc;
}

static int cfg_parse_metrics(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"enabled", "latency_histogram", "sample_every"};
    const cJSON *obj = NULL;
    bool enabled = true;
    bool latency = true;
    int rc = cfg_child(root, "metrics", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    cfg_warn_unknown(obj, CFG_KEYS(known), "metrics");
    rc = cfg_bool(obj, "enabled", &enabled, err, "metrics");
    if (rc == NB_OK) {
        rc = cfg_bool(obj, "latency_histogram", &latency, err, "metrics");
    }
    if (rc == NB_OK) {
        rc = cfg_u32(obj, "sample_every", &doc->node.metrics.sample_every, err, "metrics");
    }
    if (rc != NB_OK) {
        return rc;
    }
    doc->node.metrics.disable = enabled ? false : true;
    doc->node.metrics.disable_latency_histogram = latency ? false : true;
    return NB_OK;
}

static int cfg_parse_log(cfg_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"enabled", "level"};
    static const char *const level_names[] = {"assert", "error", "warn", "info", "debug",
                                              "verbose"};
    static const int level_values[] = {NB_LOG_LEVEL_ASSERT, NB_LOG_LEVEL_ERROR,
                                       NB_LOG_LEVEL_WARN,   NB_LOG_LEVEL_INFO,
                                       NB_LOG_LEVEL_DEBUG,  NB_LOG_LEVEL_VERBOSE};
    const cJSON *obj = NULL;
    bool enabled = true;
    int rc = cfg_child(root, "log", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    cfg_warn_unknown(obj, CFG_KEYS(known), "log");
    rc = cfg_bool(obj, "enabled", &enabled, err, "log");
    if (rc == NB_OK) {
        rc = cfg_enum(obj, "level", CFG_KEYS(level_names), level_values,
                      (int *)&doc->node.log.level, err, "log");
    }
    if (rc != NB_OK) {
        return rc;
    }
    doc->node.log.disable = enabled ? false : true;
    return NB_OK;
}

/* ---- doc assembly ------------------------------------------------------- */

static void cfg_doc_rebind(cfg_doc_t *doc)
{
    doc->node.name = doc->name;
    doc->node.rpc.bind_host = doc->bind_host;
    doc->node.rpc.discovery.broadcast_host = doc->discovery_host;
    doc->node.bindings = doc->binding_count ? doc->bindings : NULL;
    doc->node.binding_count = doc->binding_count;
    doc->node.rpc.routes = doc->route_count ? doc->routes : NULL;
    doc->node.rpc.route_count = doc->route_count;
    doc->node.rpc.peers = doc->peer_count ? doc->peers : NULL;
    doc->node.rpc.peer_count = doc->peer_count;
}

/* ---- public API --------------------------------------------------------- */

int nb_config_parse(const char *json, nb_config_doc_t **out, nb_config_error_t *err)
{
    static const char *const top_keys[] = {"name", "threads", "limits", "rpc", "pubsub",
                                           "metrics", "log"};
    cJSON *root = NULL;
    cfg_doc_t *doc;
    int rc;

    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (out) {
        *out = NULL;
    }
    if (!json || !out) {
        return cfg_fail(err, "", "null argument");
    }
    doc = (cfg_doc_t *)calloc(1, sizeof(*doc));
    if (!doc) {
        return cfg_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    snprintf(doc->name, sizeof(doc->name), "%s", NB_DEFAULT_NODE_NAME);
    snprintf(doc->bind_host, sizeof(doc->bind_host), "%s", NB_DEFAULT_BIND_HOST);
    snprintf(doc->discovery_host, sizeof(doc->discovery_host), "%s", NB_DEFAULT_DISCOVERY_HOST);
    nb_node_config_default(&doc->node);

    root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        rc = cfg_fail(err, "", "malformed JSON object");
        goto done;
    }
    cfg_warn_unknown(root, CFG_KEYS(top_keys), "");

    rc = cfg_parse_name(doc, root, err);
    if (rc == NB_OK) {
        rc = cfg_parse_threads(doc, root, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_limits(doc, root, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_rpc(doc, root, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_pubsub(doc, root, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_metrics(doc, root, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_log(doc, root, err);
    }
    if (rc != NB_OK) {
        goto done;
    }
    cfg_doc_rebind(doc);
    nb_node_config_normalize(&doc->node);
    if (nb_node_config_validate(&doc->node) != NB_OK) {
        rc = cfg_fail(err, "", "configuration is invalid");
        goto done;
    }
    *out = doc;
    doc = NULL;
    rc = NB_OK;

done:
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
        return cfg_fail(err, "", "null argument");
    }
    f = fopen(path, "rb");
    if (!f) {
        return cfg_fail_code(err, NB_ERR_IO, "", "cannot open config file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return cfg_fail_code(err, NB_ERR_IO, "", "cannot seek config file");
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return cfg_fail_code(err, NB_ERR_IO, "", "cannot size config file");
    }
    if ((unsigned long)size > (unsigned long)NB_CONFIG_MAX_FILE_BYTES) {
        fclose(f);
        return cfg_fail_code(err, NB_ERR_INVALID, "", "config file too large");
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cfg_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return cfg_fail_code(err, NB_ERR_IO, "", "cannot read config file");
    }
    buf[size] = '\0';
    fclose(f);
    rc = nb_config_parse(buf, out, err);
    free(buf);
    return rc;
}

void nb_config_doc_destroy(nb_config_doc_t *doc)
{
    free(doc);
}

const nb_node_config_t *nb_config_node(const nb_config_doc_t *doc)
{
    return doc ? &doc->node : NULL;
}

int nb_node_create_from_config(const nb_config_doc_t *cfg, nb_node_t **out)
{
    if (!cfg || !out) {
        return NB_ERR_INVALID;
    }
    return nb_node_create(&cfg->node, out);
}
