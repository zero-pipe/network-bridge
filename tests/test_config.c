#include <nb/config.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_nested(void)
{
    const char *json = "{"
                       "\"name\":\"nested\",\"mode\":\"static\","
                       "\"runtime\":{\"io_threads\":2,\"handler_threads\":3,"
                       "\"handler_max_concurrency\":64,\"max_inflight\":100,"
                       "\"max_streams\":8,\"peer_rate_limit_pps\":10,\"send_lanes\":1},"
                       "\"transport\":{\"kind\":\"tcp\",\"bind_host\":\"127.0.0.1\","
                       "\"data_port\":9000,\"frag_bytes\":1024,\"max_msg_bytes\":4194304,"
                       "\"backpressure_bytes\":8192,\"rtcp_interval_ms\":2000,\"disable_rtcp\":true,"
                       "\"bindings\":[\"tcp://127.0.0.1:9000\"]},"
                       "\"discovery\":{\"mode\":\"static\",\"broadcast\":\"127.0.0.1:41953\","
                       "\"hello_interval_ms\":500,\"node_ttl_ms\":9000,"
                       "\"peers\":[\"10.0.0.1:41953\"]},"
                       "\"metrics\":{\"disable\":true,\"disable_latency_histogram\":true,"
                       "\"sample_every\":5},"
                       "\"log\":{\"disable\":true,\"level\":\"debug\"},"
                       "\"routes\":[{\"service\":\"echo\",\"endpoint\":\"tcp://10.0.0.2:9001\"}]"
                       "}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (!n) {
        nb_config_doc_destroy(cfg);
        return;
    }
    CHECK(strcmp(n->name, "nested") == 0);
    CHECK(n->transport.kind == NB_TRANSPORT_KIND_TCP);
    CHECK(n->mode == NB_NODE_MODE_STATIC);
    CHECK(strcmp(n->transport.bind_host, "127.0.0.1") == 0);
    CHECK(n->transport.data_port == 9000);
    CHECK(n->transport.frag_bytes == 1024);
    CHECK(n->transport.max_msg_bytes == 4194304);
    CHECK(n->transport.backpressure_bytes == 8192);
    CHECK(n->transport.rtcp_interval_ms == 2000);
    CHECK(n->transport.disable_rtcp == 1);
    CHECK(n->runtime.io_threads == 2);
    CHECK(n->runtime.handler_threads == 3);
    CHECK(n->runtime.handler_max_concurrency == 64);
    CHECK(n->runtime.max_inflight == 100);
    CHECK(n->runtime.max_streams == 8);
    CHECK(n->runtime.peer_rate_limit_pps == 10);
    CHECK(n->runtime.send_lanes == 1);
    CHECK(n->discovery.disable == 1);
    CHECK(strcmp(n->discovery.broadcast_host, "127.0.0.1") == 0);
    CHECK(n->discovery.port == 41953);
    CHECK(n->discovery.hello_interval_ms == 500);
    CHECK(n->discovery.node_ttl_ms == 9000);
    CHECK(n->peer_count == 1);
    CHECK(strcmp(n->peers[0].host, "10.0.0.1") == 0);
    CHECK(n->peers[0].port == 41953);
    CHECK(n->metrics.disable == 1);
    CHECK(n->metrics.disable_latency_histogram == 1);
    CHECK(n->metrics.sample_every == 5);
    CHECK(n->log.disable == 1);
    CHECK(n->log.level == NB_LOG_LEVEL_DEBUG);
    CHECK(n->route_count == 1);
    CHECK(strcmp(n->routes[0].service, "echo") == 0);
    CHECK(strcmp(n->routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
    nb_config_doc_destroy(cfg);
}

static void test_defaults(void)
{
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse("{}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (!n) {
        nb_config_doc_destroy(cfg);
        return;
    }
    CHECK(n->transport.kind == NB_TRANSPORT_KIND_UDP);
    CHECK(n->transport.frag_bytes == 1440);
    CHECK(n->mode == NB_NODE_MODE_MESH);
    CHECK(n->discovery.disable == 0);
    CHECK(strcmp(n->name, "nb") == 0);
    CHECK(strcmp(n->transport.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(n->discovery.broadcast_host, "255.255.255.255") == 0);
    CHECK(n->discovery.port == NB_DISCOVERY_PORT);
    CHECK(n->metrics.sample_every == 1);
    CHECK(n->log.level == NB_LOG_LEVEL_INFO);
    CHECK(n->transport.binding_count == 0);
    CHECK(n->transport.bindings == NULL);
    CHECK(n->peer_count == 0);
    CHECK(n->route_count == 0);
    CHECK(n->routes == NULL);
    CHECK(n->peers == NULL);
    nb_config_doc_destroy(cfg);
}

static void test_bad_json(void)
{
    nb_config_doc_t *cfg = (nb_config_doc_t *)1;

    CHECK(nb_config_parse("{ not json", &cfg, NULL) == NB_ERR_INVALID);
    CHECK(cfg == NULL);
    cfg = (nb_config_doc_t *)1;
    CHECK(nb_config_parse(NULL, &cfg, NULL) == NB_ERR_INVALID);
    CHECK(cfg == NULL);
}

static void test_strict_validation(void)
{
    static const char *bad[] = {
        "{\"mode\":\"foo\"}",
        "{\"mode\":\"STATIC\"}",
        "{\"name\":42}",
        "{\"routes\":[{\"service\":\"x\"}]}",
        "{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}",
        "{\"discovery\":[]}",
        "{\"discovery\":{\"broadcast\":\"bad\"}}",
        "{\"discovery\":{\"peers\":[\"bad\"]}}",
        "{\"discovery\":{\"mode\":\"foo\"}}",
        "{\"discovery\":{\"mode\":\"on\"}}",
        "{\"runtime\":[]}",
        "{\"runtime\":{\"io_threads\":\"2\"}}",
        "{\"runtime\":{\"io_threads\":2000}}",
        "{\"runtime\":{\"io_threads\":9}}",
        "{\"runtime\":{\"handler_threads\":1025}}",
        "{\"runtime\":{\"send_lanes\":65}}",
        "{\"runtime\":{\"max_streams\":65}}",
        "{\"transport\":[]}",
        "{\"transport\":{\"kind\":\"sctp\"}}",
        "{\"transport\":{\"kind\":\"TCP\"}}",
        "{\"transport\":{\"data_port\":70000}}",
        "{\"transport\":{\"frag_bytes\":70000}}",
        "{\"transport\":{\"frag_bytes\":255}}",
        "{\"transport\":{\"frag_bytes\":8193}}",
        "{\"transport\":{\"max_msg_bytes\":67108865}}",
        "{\"transport\":{\"max_msg_bytes\":1000}}",
        "{\"transport\":{\"max_msg_bytes\":512,\"frag_bytes\":1024}}",
        "{\"transport\":{\"disable_rtcp\":\"no\"}}",
        "{\"transport\":{\"bindings\":\"tcp://127.0.0.1:1\"}}",
        "{\"transport\":{\"bindings\":[\"not-a-uri\"]}}",
        "{\"transport\":{\"bindings\":[{\"uri\":\"tcp://127.0.0.1:1\"}]}}",
        "{\"transport\":{\"bindings\":[{\"name\":\"0123456789abcdef\","
        "\"endpoint\":\"tcp://127.0.0.1:1\"}]}}",
        "{\"transport\":{\"qos\":{\"depth\":1.5}}}",
        "{\"metrics\":\"off\"}",
        "{\"log\":{\"level\":\"nope\"}}",
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        nb_config_doc_t *cfg = (nb_config_doc_t *)1;
        if (nb_config_parse(bad[i], &cfg, NULL) != NB_ERR_INVALID) {
            nb_log_error("strict config accepted invalid case %lu: %s",
                         (unsigned long)i,
                         bad[i]);
            g_fail = 1;
        }
        CHECK(cfg == NULL);
    }
}

static void test_unknown_keys_ignored(void)
{
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse("{\"runtime\":{\"handler_thread\":3}}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->runtime.handler_threads == 0);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"metrics\":{\"disable_per_peer_stats\":true}}", &cfg, NULL) == NB_OK);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"log\":{\"file\":\"nb.log\"}}", &cfg, NULL) == NB_OK);
    nb_config_doc_destroy(cfg);
}

static void test_discovery_mode(void)
{
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse("{\"mode\":\"static\"}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->mode == NB_NODE_MODE_STATIC && n->discovery.disable == 1);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"discovery\":{\"mode\":\"off\"}}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->mode == NB_NODE_MODE_STATIC && n->discovery.disable == 1);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"discovery\":{\"mode\":\"none\"}}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->mode == NB_NODE_MODE_STATIC);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"discovery\":{\"mode\":\"mesh\"}}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->mode == NB_NODE_MODE_MESH && n->discovery.disable == 0);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"mode\":\"static\",\"discovery\":{\"mode\":\"mesh\"}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->mode == NB_NODE_MODE_STATIC && n->discovery.disable == 1);
    nb_config_doc_destroy(cfg);
}

static void test_endpoint_model(void)
{
    nb_endpoint_t endpoint;
    char text[128];

    CHECK(nb_endpoint_parse("tcp://127.0.0.1:8080", &endpoint) == NB_OK);
    CHECK(endpoint.transport == NB_TRANSPORT_KIND_TCP);
    CHECK(endpoint.port == 8080);
    CHECK(nb_endpoint_format(&endpoint, text, sizeof(text)) == NB_OK);
    CHECK(strcmp(text, "tcp://127.0.0.1:8080") == 0);
    CHECK(nb_endpoint_parse("dds://127.0.0.1:7400", &endpoint) == NB_OK);
    CHECK(endpoint.transport == NB_TRANSPORT_KIND_CUSTOM);
    CHECK(nb_endpoint_parse("shm://orders:3/control", &endpoint) == NB_OK);
    CHECK(strcmp(endpoint.scheme, "shm") == 0);
    CHECK(strcmp(endpoint.host, "orders") == 0);
    CHECK(strcmp(endpoint.path, "/control") == 0);
    CHECK(strcmp(endpoint.uri, "shm://orders:3/control") == 0);
    CHECK(nb_endpoint_is_network(&endpoint) == 0);
    CHECK(nb_endpoint_parse("tcp://127.0.0.1:8080", &endpoint) == NB_OK);
    CHECK(nb_endpoint_is_network(&endpoint) == 1);
    CHECK(nb_endpoint_from_parts(&endpoint, "dds", "0", 1, NB_TRANSPORT_KIND_UDP) == NB_ERR_INVALID);
}

static void test_qos_config(void)
{
    const char *json = "{"
                       "\"transport\":{"
                       "\"qos\":{\"reliability\":\"reliable\",\"durability\":\"transient_local\","
                       "\"history\":\"keep_last\",\"depth\":8,\"deadline_ms\":250,"
                       "\"lifespan_ms\":1000,\"liveliness\":\"manual_by_topic\","
                       "\"lease_duration_ms\":2000,\"ownership\":\"exclusive\","
                       "\"partition\":\"orders\"},"
                       "\"bindings\":[{\"name\":\"dds\",\"endpoint\":\"dds://0:1\","
                       "\"qos\":{\"reliability\":\"best_effort\",\"history\":\"keep_all\"}}]"
                       "}"
                       "}";
    nb_config_doc_t *cfg = NULL;
    nb_config_error_t err;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->transport.qos.reliability == NB_QOS_RELIABILITY_RELIABLE);
        CHECK(n->transport.qos.durability == NB_QOS_DURABILITY_TRANSIENT_LOCAL);
        CHECK(n->transport.qos.history == NB_QOS_HISTORY_KEEP_LAST);
        CHECK(n->transport.qos.depth == 8);
        CHECK(n->transport.qos.deadline_ms == 250);
        CHECK(n->transport.qos.lifespan_ms == 1000);
        CHECK(n->transport.qos.liveliness == NB_QOS_LIVELINESS_MANUAL_BY_TOPIC);
        CHECK(n->transport.qos.lease_duration_ms == 2000);
        CHECK(n->transport.qos.ownership == NB_QOS_OWNERSHIP_EXCLUSIVE);
        CHECK(strcmp(n->transport.qos.partition, "orders") == 0);
        CHECK(n->transport.binding_count == 1);
        CHECK(n->transport.bindings[0].qos.reliability == NB_QOS_RELIABILITY_BEST_EFFORT);
        CHECK(n->transport.bindings[0].qos.history == NB_QOS_HISTORY_KEEP_ALL);
    }
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"transport\":{\"qos\":{\"reliability\":\"sometimes\"}}}",
                          &cfg,
                          &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "transport.qos.reliability") == 0);

    cfg = NULL;
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"transport\":{\"qos\":{\"depth\":1.5}}}", &cfg, &err) ==
          NB_ERR_INVALID);
    CHECK(strcmp(err.path, "transport.qos.depth") == 0);

    cfg = NULL;
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"transport\":{\"bindings\":[{\"endpoint\":\"tcp://127.0.0.1:1\","
                          "\"qos\":{\"history\":\"nope\"}}]}}",
                          &cfg,
                          &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "transport.bindings[0].qos.history") == 0);
}

static void test_binding_config(void)
{
    const char *json = "{\"name\":\"dual\",\"mode\":\"static\","
                       "\"transport\":{\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "{\"name\":\"udp\",\"endpoint\":\"udp://127.0.0.1:8081\","
                       "\"poll_us\":250}]},"
                       "\"routes\":[{\"service\":\"order\","
                       "\"endpoint\":\"tcp://127.0.0.1:9000\"}]}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->transport.binding_count == 2);
        CHECK(strcmp(n->transport.bindings[0].name, "tcp") == 0);
        CHECK(strcmp(n->transport.bindings[1].endpoint, "udp://127.0.0.1:8081") == 0);
        CHECK(n->transport.bindings[0].poll_us == 0);
        CHECK(n->transport.bindings[1].poll_us == 250);
        CHECK(n->route_count == 1);
        CHECK(strcmp(n->routes[0].endpoint, "tcp://127.0.0.1:9000") == 0);
    }
    nb_config_doc_destroy(cfg);
}

static void test_nested_bindings(void)
{
    const char *json = "{\"transport\":{\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "\"udp://127.0.0.1:8081\"]}}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->transport.binding_count == 2);
        CHECK(strcmp(n->transport.bindings[0].name, "tcp") == 0);
        CHECK(strcmp(n->transport.bindings[1].name, "udp") == 0);
        CHECK(strcmp(n->transport.bindings[1].endpoint, "udp://127.0.0.1:8081") == 0);
    }
    nb_config_doc_destroy(cfg);
}

static void test_rtcp_config(void)
{
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse("{\"name\":\"r\",\"transport\":{\"rtcp_interval_ms\":2000}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->transport.rtcp_interval_ms == 2000 && n->transport.disable_rtcp == 0);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"name\":\"r\",\"transport\":{\"disable_rtcp\":false}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->transport.disable_rtcp == 0);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"name\":\"r\",\"transport\":{\"disable_rtcp\":true}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->transport.disable_rtcp == 1);
    nb_config_doc_destroy(cfg);
}

static void test_structured_errors(void)
{
    nb_config_doc_t *cfg = NULL;
    nb_config_error_t err;

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"runtime\":{\"io_threads\":\"2\"}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "runtime.io_threads") == 0);
    CHECK(err.reason[0] != '\0');

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"transport\":{\"frag_bytes\":70000}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "transport.frag_bytes") == 0);
    CHECK(err.reason[0] != '\0');

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"mode\":\"nope\"}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "mode") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}", &cfg, &err) ==
          NB_ERR_INVALID);
    CHECK(strcmp(err.path, "routes[0].endpoint") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"discovery\":{\"peers\":[\"bad\"]}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "discovery.peers[0]") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{ not json", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "") == 0);
}

static void test_defaults_api(void)
{
    nb_node_config_t cfg;
    nb_node_config_default(&cfg);
    CHECK(strcmp(cfg.name, "nb") == 0);
    CHECK(cfg.mode == NB_NODE_MODE_MESH);
    CHECK(cfg.transport.kind == NB_TRANSPORT_KIND_UDP);
    CHECK(strcmp(cfg.transport.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.discovery.broadcast_host, "255.255.255.255") == 0);
    CHECK(cfg.metrics.sample_every == 1);
    CHECK(cfg.log.level == NB_LOG_LEVEL_INFO);
    CHECK(cfg.routes == NULL && cfg.route_count == 0);
    CHECK(cfg.peers == NULL && cfg.peer_count == 0);
}

static void test_normalize(void)
{
    nb_node_config_t cfg;

    nb_node_config_default(&cfg);
    cfg.runtime.handler_threads = 5000;
    cfg.runtime.send_lanes = 5000;
    cfg.transport.frag_bytes = 100;
    cfg.transport.max_msg_bytes = 64;
    cfg.runtime.max_streams = 5000;
    CHECK(nb_node_config_validate(&cfg) == NB_ERR_INVALID);
    nb_node_config_normalize(&cfg);
    CHECK(cfg.runtime.handler_threads <= NB_MAX_HANDLER_THREADS);
    CHECK(cfg.runtime.send_lanes <= NB_MAX_SEND_LANES);
    CHECK(cfg.transport.frag_bytes == 256);
    CHECK(cfg.transport.max_msg_bytes == 256);
    CHECK(cfg.runtime.max_streams == NB_MAX_STREAM_RECV_STREAMS);

    nb_node_config_default(&cfg);
    cfg.discovery.disable = true;
    nb_node_config_normalize(&cfg);
    CHECK(cfg.mode == NB_NODE_MODE_STATIC);

    nb_node_config_default(&cfg);
    cfg.mode = NB_NODE_MODE_STATIC;
    nb_node_config_normalize(&cfg);
    CHECK(cfg.discovery.disable == 1);

    nb_node_config_default(&cfg);
    cfg.log.level = NB_LOG_LEVEL_DEFAULT;
    nb_node_config_normalize(&cfg);
    CHECK(cfg.log.level == NB_LOG_LEVEL_INFO);
}

static void test_doc_lifetime(void)
{
    const char *json = "{\"name\":\"orig\",\"transport\":{\"bindings\":[\"tcp://127.0.0.1:1\"]}}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(strcmp(n->name, "orig") == 0);
        CHECK(n->transport.binding_count == 1);
        CHECK(strcmp(n->transport.bindings[0].endpoint, "tcp://127.0.0.1:1") == 0);
    }
    nb_config_doc_destroy(cfg);
    CHECK(nb_config_node(NULL) == NULL);
}

static void test_log_switch(void)
{
    CHECK(nb_log_enabled() == 1);
    nb_log_configure(0, NB_LOG_LEVEL_ERROR);
    CHECK(nb_log_enabled() == 0);
    nb_log_configure(1, NB_LOG_LEVEL_INFO);
    CHECK(nb_log_enabled() == 1);
}

int main(void)
{
    test_nested();
    test_defaults();
    test_bad_json();
    test_strict_validation();
    test_unknown_keys_ignored();
    test_discovery_mode();
    test_structured_errors();
    test_endpoint_model();
    test_qos_config();
    test_binding_config();
    test_nested_bindings();
    test_rtcp_config();
    test_defaults_api();
    test_normalize();
    test_doc_lifetime();
    test_log_switch();
    if (g_fail) {
        nb_log_error("config: FAIL");
        return 1;
    }
    nb_log_info("config: NB_OK");
    return 0;
}

