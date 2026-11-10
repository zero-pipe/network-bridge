/*
 * RPC spine JSON configuration.
 *
 * Only the `rpc` block (plus name/threads/limits/metrics/log) is parsed here.
 * The `dds` block, if present, is ignored silently: the two spines parse
 * independently.
 */
#include <nb/rpc/config.h>

#include <nb/base/log.h>
#include <nb/rpc/node.h>

#include "defaults.h"
#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_KEYS(array) array, sizeof(array) / sizeof((array)[0])

struct nb_rpc_config_doc {
    nb_node_config_t node;
    char name[NB_NAME_MAX];
    char bind_host[NB_HOST_MAX];
    char discovery_host[NB_HOST_MAX];
    nb_static_route_t routes[NB_CONFIG_MAX_ROUTES];
    size_t route_count;
    nb_peer_endpoint_t peers[NB_CONFIG_MAX_PEERS];
    size_t peer_count;
    nb_binding_endpoint_t endpoints[NB_CONFIG_MAX_BINDINGS];
    nb_binding_config_t bindings[NB_CONFIG_MAX_BINDINGS];
    size_t binding_count;
};

typedef nb_rpc_config_doc_t rpc_doc_t;

/* ---- bindings ----------------------------------------------------------- */

static nb_binding_config_t *cfg_add_binding(rpc_doc_t *doc,
                                            const char *endpoint,
                                            nb_config_error_t *err,
                                            const char *path)
{
    nb_endpoint_t parsed;
    size_t i;

    if (!endpoint || nb_endpoint_parse(endpoint, &parsed) != NB_OK) {
        nb_json_fail(err, path, "invalid endpoint URI");
        return NULL;
    }
    for (i = 0; i < doc->binding_count; i++) {
        if (strcmp(doc->bindings[i].endpoint, endpoint) == 0) {
            return &doc->bindings[i];
        }
    }
    if (doc->binding_count >= NB_CONFIG_MAX_BINDINGS) {
        nb_json_fail(err, path, "too many bindings");
        return NULL;
    }
    if (strlen(endpoint) >= NB_ENDPOINT_URI_MAX || strlen(parsed.scheme) >= NB_SCHEME_MAX) {
        nb_json_fail(err, path, "endpoint too long");
        return NULL;
    }
    {
        nb_binding_endpoint_t *ep = &doc->endpoints[doc->binding_count];
        nb_binding_config_t *binding = &doc->bindings[doc->binding_count];

        snprintf(ep->name, sizeof(ep->name), "%s", parsed.scheme);
        snprintf(ep->endpoint, sizeof(ep->endpoint), "%s", endpoint);
        memset(binding, 0, sizeof(*binding));
        binding->name = ep->name;
        binding->endpoint = ep->endpoint;
        doc->binding_count++;
        return binding;
    }
}

/* ---- section parsers ---------------------------------------------------- */

static int cfg_parse_name(rpc_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    const char *name = NULL;
    int rc = nb_json_string_field(root, "name", &name, err, "");

    if (rc != NB_OK) {
        return rc;
    }
    if (name) {
        if (strlen(name) >= sizeof(doc->name)) {
            return nb_json_fail(err, "name", "too long");
        }
        snprintf(doc->name, sizeof(doc->name), "%s", name);
    }
    return NB_OK;
}

static int cfg_parse_threads(rpc_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"io", "handlers", "send_lanes"};
    const cJSON *obj = NULL;
    uint32_t io = 0;
    uint32_t handlers = 0;
    uint32_t send_lanes = 0;
    int rc = nb_json_object(root, "threads", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "threads");
    rc = nb_json_u32_field(obj, "io", &io, err, "threads");
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "handlers", &handlers, err, "threads");
    }
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "send_lanes", &send_lanes, err, "threads");
    }
    if (rc != NB_OK) {
        return rc;
    }
    if (io > NB_TRANSPORT_MAX_IO) {
        return nb_json_fail(err, "threads.io", "too large");
    }
    if (handlers > NB_MAX_HANDLER_THREADS) {
        return nb_json_fail(err, "threads.handlers", "too large");
    }
    if (send_lanes > NB_MAX_SEND_LANES) {
        return nb_json_fail(err, "threads.send_lanes", "too large");
    }
    doc->node.runtime.io_threads = (unsigned)io;
    doc->node.runtime.handler_threads = (unsigned)handlers;
    doc->node.runtime.send_lanes = (unsigned)send_lanes;
    return NB_OK;
}

static int cfg_parse_limits(rpc_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"max_inflight", "max_streams", "handler_queue",
                                        "peer_rate_per_second"};
    const cJSON *obj = NULL;
    uint32_t value = 0;
    int rc = nb_json_object(root, "limits", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "limits");
    rc = nb_json_u32_field(obj, "max_inflight", &doc->node.runtime.max_inflight, err, "limits");
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "max_streams", &doc->node.runtime.max_streams, err, "limits");
    }
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "handler_queue", &doc->node.runtime.handler_max_concurrency,
                               err, "limits");
    }
    if (rc == NB_OK) {
        value = doc->node.runtime.peer_rate_limit_pps;
        rc = nb_json_u32_field(obj, "peer_rate_per_second", &value, err, "limits");
        doc->node.runtime.peer_rate_limit_pps = value;
    }
    if (rc == NB_OK && doc->node.runtime.max_streams > NB_STREAM_MAX_RECV_STREAMS) {
        rc = nb_json_fail(err, "limits.max_streams", "too large");
    }
    return rc;
}

static int cfg_parse_routes(rpc_doc_t *doc, const cJSON *rpc, nb_config_error_t *err)
{
    static const char *const known[] = {"service", "endpoint"};
    const cJSON *routes = cJSON_GetObjectItemCaseSensitive(rpc, "routes");
    const cJSON *item;
    size_t index = 0;

    if (!routes) {
        return NB_OK;
    }
    if (!cJSON_IsArray(routes)) {
        return nb_json_fail(err, "rpc.routes", "must be an array");
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
            return nb_json_fail(err, path, "each route must be an object");
        }
        nb_json_warn_unknown(item, CFG_KEYS(known), path);
        if (doc->route_count >= NB_CONFIG_MAX_ROUTES) {
            return nb_json_fail(err, "rpc.routes", "too many routes");
        }
        rc = nb_json_string_field(item, "service", &service, err, path);
        if (rc == NB_OK) {
            rc = nb_json_string_field(item, "endpoint", &endpoint, err, path);
        }
        if (rc != NB_OK) {
            return rc;
        }
        if (!service || !service[0]) {
            char full[96];

            nb_json_join(full, sizeof(full), path, "service");
            return nb_json_fail(err, full, "required");
        }
        if (!endpoint || nb_endpoint_parse(endpoint, &parsed) != NB_OK) {
            char full[96];

            nb_json_join(full, sizeof(full), path, "endpoint");
            return nb_json_fail(err, full, "invalid endpoint URI");
        }
        if (strlen(service) >= NB_SERVICE_NAME_MAX ||
            strlen(endpoint) >= NB_ENDPOINT_URI_MAX) {
            return nb_json_fail(err, path, "value too long");
        }
        route = &doc->routes[doc->route_count];
        snprintf(route->service, sizeof(route->service), "%s", service);
        snprintf(route->endpoint, sizeof(route->endpoint), "%s", endpoint);
        doc->route_count++;
    }
    return NB_OK;
}

static int cfg_parse_peers(rpc_doc_t *doc, const cJSON *discovery, nb_config_error_t *err)
{
    const cJSON *peers = cJSON_GetObjectItemCaseSensitive(discovery, "peers");
    const cJSON *item;
    size_t index = 0;

    if (!peers) {
        return NB_OK;
    }
    if (!cJSON_IsArray(peers)) {
        return nb_json_fail(err, "rpc.discovery.peers", "must be an array");
    }
    cJSON_ArrayForEach(item, peers)
    {
        char path[96];
        nb_peer_endpoint_t *peer;

        snprintf(path, sizeof(path), "rpc.discovery.peers[%lu]", (unsigned long)index++);
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_json_fail(err, path, "must be a string");
        }
        if (doc->peer_count >= NB_CONFIG_MAX_PEERS) {
            return nb_json_fail(err, "rpc.discovery.peers", "too many peers");
        }
        peer = &doc->peers[doc->peer_count];
        if (nb_json_host_port(item->valuestring, peer->host, sizeof(peer->host), &peer->port) !=
            NB_OK) {
            return nb_json_fail(err, path, "must be host:port");
        }
        doc->peer_count++;
    }
    return NB_OK;
}

static int cfg_parse_discovery(rpc_doc_t *doc, const cJSON *rpc, nb_config_error_t *err)
{
    static const char *const known[] = {"broadcast", "peers", "hello_interval", "node_ttl"};
    const cJSON *obj = NULL;
    const char *broadcast = NULL;
    int rc = nb_json_object(rpc, "discovery", &obj, err, "rpc");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "rpc.discovery");
    rc = nb_json_string_field(obj, "broadcast", &broadcast, err, "rpc.discovery");
    if (rc == NB_OK && broadcast) {
        if (nb_json_host_port(broadcast, doc->discovery_host, sizeof(doc->discovery_host),
                              &doc->node.rpc.discovery.port) != NB_OK) {
            return nb_json_fail(err, "rpc.discovery.broadcast", "must be host:port");
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "hello_interval",
                                    &doc->node.rpc.discovery.hello_interval_ms, err,
                                    "rpc.discovery");
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "node_ttl", &doc->node.rpc.discovery.node_ttl_ms, err,
                                    "rpc.discovery");
    }
    if (rc == NB_OK) {
        rc = cfg_parse_peers(doc, obj, err);
    }
    return rc;
}

static int cfg_parse_listen(rpc_doc_t *doc, const cJSON *rpc, nb_config_error_t *err)
{
    const cJSON *listen = cJSON_GetObjectItemCaseSensitive(rpc, "listen");
    const cJSON *item;
    size_t index = 0;

    if (!listen) {
        return NB_OK;
    }
    if (!cJSON_IsArray(listen)) {
        return nb_json_fail(err, "rpc.listen", "must be an array of endpoint URIs");
    }
    cJSON_ArrayForEach(item, listen)
    {
        char path[96];

        snprintf(path, sizeof(path), "rpc.listen[%lu]", (unsigned long)index++);
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_json_fail(err, path, "must be an endpoint URI string");
        }
        if (!cfg_add_binding(doc, item->valuestring, err, path)) {
            return NB_ERR_INVALID;
        }
    }
    return NB_OK;
}

static int cfg_parse_rpc(rpc_doc_t *doc, const cJSON *root, nb_config_error_t *err)
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
    int rc = nb_json_object(root, "rpc", &rpc, err, "");

    if (rc != NB_OK || !rpc) {
        return rc;
    }
    nb_json_warn_unknown(rpc, CFG_KEYS(known), "rpc");

    rc = nb_json_enum_field(rpc, "mode", CFG_KEYS(mode_names), mode_values,
                            (int *)&doc->node.rpc.mode, err, "rpc");
    if (rc == NB_OK) {
        rc = nb_json_u32_field(rpc, "max_message_bytes", &doc->node.rpc.max_msg_bytes, err,
                               "rpc");
    }
    if (rc == NB_OK) {
        if (doc->node.rpc.max_msg_bytes > NB_GLOBAL_MAX_MSG_BYTES_LIMIT) {
            return nb_json_fail(err, "rpc.max_message_bytes", "too large");
        }
        rc = nb_json_u32_field(rpc, "backpressure_bytes", &doc->node.rpc.backpressure_bytes, err,
                               "rpc");
    }
    if (rc == NB_OK) {
        rc = nb_json_object(rpc, "udp", &udp, err, "rpc");
    }
    if (rc == NB_OK && udp) {
        nb_json_warn_unknown(udp, CFG_KEYS(udp_known), "rpc.udp");
        value = doc->node.rpc.frag_bytes;
        rc = nb_json_u32_field(udp, "fragment_bytes", &value, err, "rpc.udp");
        if (rc == NB_OK) {
            if (value != 0 && (value < NB_DEFAULT_FRAG_MIN_BYTES ||
                               value > NB_DEFAULT_FRAG_MAX_BYTES)) {
                return nb_json_fail(err, "rpc.udp.fragment_bytes",
                                    "must be 0 or within 256..8192");
            }
            doc->node.rpc.frag_bytes = (uint16_t)value;
            rc = nb_json_duration_field(udp, "rtcp_interval",
                                        &doc->node.rpc.rtcp_interval_ms, err, "rpc.udp");
        }
        if (rc == NB_OK) {
            rc = nb_json_bool_field(udp, "rtcp", &rtcp, err, "rpc.udp");
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

static int cfg_parse_metrics(rpc_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"enabled", "latency_histogram", "sample_every"};
    const cJSON *obj = NULL;
    bool enabled = true;
    bool latency = true;
    int rc = nb_json_object(root, "metrics", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "metrics");
    rc = nb_json_bool_field(obj, "enabled", &enabled, err, "metrics");
    if (rc == NB_OK) {
        rc = nb_json_bool_field(obj, "latency_histogram", &latency, err, "metrics");
    }
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "sample_every", &doc->node.metrics.sample_every, err,
                               "metrics");
    }
    if (rc != NB_OK) {
        return rc;
    }
    doc->node.metrics.disable = enabled ? false : true;
    doc->node.metrics.disable_latency_histogram = latency ? false : true;
    return NB_OK;
}

static int cfg_parse_log(rpc_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"enabled", "level"};
    static const char *const level_names[] = {"assert", "error", "warn", "info", "debug",
                                              "verbose"};
    static const int level_values[] = {NB_LOG_LEVEL_ASSERT, NB_LOG_LEVEL_ERROR,
                                       NB_LOG_LEVEL_WARN,   NB_LOG_LEVEL_INFO,
                                       NB_LOG_LEVEL_DEBUG,  NB_LOG_LEVEL_VERBOSE};
    const cJSON *obj = NULL;
    bool enabled = true;
    int rc = nb_json_object(root, "log", &obj, err, "");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "log");
    rc = nb_json_bool_field(obj, "enabled", &enabled, err, "log");
    if (rc == NB_OK) {
        rc = nb_json_enum_field(obj, "level", CFG_KEYS(level_names), level_values,
                                (int *)&doc->node.log.level, err, "log");
    }
    if (rc != NB_OK) {
        return rc;
    }
    doc->node.log.disable = enabled ? false : true;
    return NB_OK;
}

/* ---- doc assembly ------------------------------------------------------- */

static void cfg_doc_rebind(rpc_doc_t *doc)
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

int nb_rpc_config_parse(const char *json, nb_rpc_config_doc_t **out, nb_config_error_t *err)
{
    static const char *const top_keys[] = {"name", "threads", "limits", "rpc", "metrics", "log",
                                           "dds", "$schema"};
    cJSON *root = NULL;
    rpc_doc_t *doc;
    int rc;

    if (!out) {
        return nb_json_fail(err, "", "null argument");
    }
    *out = NULL;
    rc = nb_json_parse_string(json, &root, err);
    if (rc != NB_OK) {
        return rc;
    }
    doc = (rpc_doc_t *)calloc(1, sizeof(*doc));
    if (!doc) {
        cJSON_Delete(root);
        return nb_json_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    snprintf(doc->name, sizeof(doc->name), "%s", NB_DEFAULT_NODE_NAME);
    snprintf(doc->bind_host, sizeof(doc->bind_host), "%s", NB_DEFAULT_BIND_HOST);
    snprintf(doc->discovery_host, sizeof(doc->discovery_host), "%s", NB_DEFAULT_DISCOVERY_HOST);
    nb_node_config_default(&doc->node);

    nb_json_warn_unknown(root, CFG_KEYS(top_keys), "");

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
        rc = nb_json_fail(err, "", "configuration is invalid");
        goto done;
    }
    *out = doc;
    doc = NULL;
    rc = NB_OK;

done:
    cJSON_Delete(root);
    free(doc);
    return rc;
}

int nb_rpc_config_parse_file(const char *path,
                             nb_rpc_config_doc_t **out,
                             nb_config_error_t *err)
{
    char *text = NULL;
    int rc;

    if (!out) {
        return nb_json_fail(err, "", "null argument");
    }
    *out = NULL;
    rc = nb_json_load_file(path, &text, err);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rpc_config_parse(text, out, err);
    free(text);
    return rc;
}

void nb_rpc_config_doc_destroy(nb_rpc_config_doc_t *doc)
{
    free(doc);
}

const nb_node_config_t *nb_rpc_config_node(const nb_rpc_config_doc_t *doc)
{
    return doc ? &doc->node : NULL;
}

int nb_node_create_from_config(const nb_rpc_config_doc_t *cfg, nb_node_t **out)
{
    if (!cfg || !out) {
        return NB_ERR_INVALID;
    }
    return nb_node_create(&cfg->node, out);
}
