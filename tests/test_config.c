#include <nb/config.h>
#include <nb/transport_provider.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_nested(void)
{
    const char *json = "{"
                       "\"name\":\"nested\","
                       "\"runtime\":{\"io_threads\":2,\"handler_threads\":3,"
                       "\"handler_max_concurrency\":64,\"max_inflight\":100,"
                       "\"max_streams\":8,\"peer_rate_limit_pps\":10,\"send_lanes\":1},"
                       "\"rpc\":{\"mode\":\"static\",\"kind\":\"tcp\",\"bind_host\":\"127.0.0.1\","
                       "\"data_port\":9000,\"frag_bytes\":1024,\"max_msg_bytes\":4194304,"
                       "\"backpressure_bytes\":8192,\"rtcp_interval_ms\":2000,\"disable_rtcp\":true,"
                       "\"discovery\":{\"broadcast\":\"127.0.0.1:41953\","
                       "\"hello_interval_ms\":500,\"node_ttl_ms\":9000,"
                       "\"peers\":[\"10.0.0.1:41953\"]},"
                       "\"routes\":[{\"service\":\"echo\",\"endpoint\":\"tcp://10.0.0.2:9001\"}]},"
                       "\"bindings\":[\"tcp://127.0.0.1:9000\"],"
                       "\"metrics\":{\"disable\":true,\"disable_latency_histogram\":true,"
                       "\"sample_every\":5},"
                       "\"log\":{\"disable\":true,\"level\":\"debug\"}"
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
    CHECK(n->rpc.kind == NB_TRANSPORT_KIND_TCP);
    CHECK(n->rpc.mode == NB_NODE_MODE_STATIC);
    CHECK(strcmp(n->rpc.bind_host, "127.0.0.1") == 0);
    CHECK(n->rpc.data_port == 9000);
    CHECK(n->rpc.frag_bytes == 1024);
    CHECK(n->rpc.max_msg_bytes == 4194304);
    CHECK(n->rpc.backpressure_bytes == 8192);
    CHECK(n->rpc.rtcp_interval_ms == 2000);
    CHECK(n->rpc.disable_rtcp == 1);
    CHECK(n->runtime.io_threads == 2);
    CHECK(n->runtime.handler_threads == 3);
    CHECK(n->runtime.handler_max_concurrency == 64);
    CHECK(n->runtime.max_inflight == 100);
    CHECK(n->runtime.max_streams == 8);
    CHECK(n->runtime.peer_rate_limit_pps == 10);
    CHECK(n->runtime.send_lanes == 1);
    CHECK(n->rpc.discovery.disable == 1);
    CHECK(strcmp(n->rpc.discovery.broadcast_host, "127.0.0.1") == 0);
    CHECK(n->rpc.discovery.port == 41953);
    CHECK(n->rpc.discovery.hello_interval_ms == 500);
    CHECK(n->rpc.discovery.node_ttl_ms == 9000);
    CHECK(n->rpc.peer_count == 1);
    CHECK(strcmp(n->rpc.peers[0].host, "10.0.0.1") == 0);
    CHECK(n->rpc.peers[0].port == 41953);
    CHECK(n->metrics.disable == 1);
    CHECK(n->metrics.disable_latency_histogram == 1);
    CHECK(n->metrics.sample_every == 5);
    CHECK(n->log.disable == 1);
    CHECK(n->log.level == NB_LOG_LEVEL_DEBUG);
    CHECK(n->rpc.route_count == 1);
    CHECK(strcmp(n->rpc.routes[0].service, "echo") == 0);
    CHECK(strcmp(n->rpc.routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
    CHECK(n->binding_count == 1);
    CHECK(strcmp(n->bindings[0].endpoint, "tcp://127.0.0.1:9000") == 0);
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
    CHECK(n->rpc.kind == NB_TRANSPORT_KIND_UDP);
    CHECK(n->rpc.frag_bytes == 1440);
    CHECK(n->rpc.mode == NB_NODE_MODE_MESH);
    CHECK(n->rpc.discovery.disable == 0);
    CHECK(strcmp(n->name, "nb") == 0);
    CHECK(strcmp(n->rpc.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(n->rpc.discovery.broadcast_host, "255.255.255.255") == 0);
    CHECK(n->rpc.discovery.port == NB_DISCOVERY_PORT);
    CHECK(n->metrics.sample_every == 1);
    CHECK(n->log.level == NB_LOG_LEVEL_INFO);
    CHECK(n->binding_count == 0);
    CHECK(n->bindings == NULL);
    CHECK(n->rpc.peer_count == 0);
    CHECK(n->rpc.route_count == 0);
    CHECK(n->rpc.routes == NULL);
    CHECK(n->rpc.peers == NULL);
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
        "{\"rpc\":{\"mode\":\"foo\"}}",
        "{\"rpc\":{\"mode\":\"STATIC\"}}",
        "{\"rpc\":{\"discovery\":[]}}",
        "{\"rpc\":{\"discovery\":{\"broadcast\":\"bad\"}}}",
        "{\"rpc\":{\"discovery\":{\"peers\":[\"bad\"]}}}",
        "{\"rpc\":{\"routes\":[{\"service\":\"x\"}]}}",
        "{\"rpc\":{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}}",
        "{\"name\":42}",
        "{\"runtime\":[]}",
        "{\"runtime\":{\"io_threads\":\"2\"}}",
        "{\"runtime\":{\"io_threads\":2000}}",
        "{\"runtime\":{\"io_threads\":9}}",
        "{\"runtime\":{\"handler_threads\":1025}}",
        "{\"runtime\":{\"send_lanes\":65}}",
        "{\"runtime\":{\"max_streams\":65}}",
        "{\"rpc\":[]}",
        "{\"rpc\":{\"kind\":\"sctp\"}}",
        "{\"rpc\":{\"kind\":\"TCP\"}}",
        "{\"rpc\":{\"data_port\":70000}}",
        "{\"rpc\":{\"frag_bytes\":70000}}",
        "{\"rpc\":{\"frag_bytes\":255}}",
        "{\"rpc\":{\"frag_bytes\":8193}}",
        "{\"rpc\":{\"max_msg_bytes\":67108865}}",
        "{\"rpc\":{\"max_msg_bytes\":1000}}",
        "{\"rpc\":{\"max_msg_bytes\":512,\"frag_bytes\":1024}}",
        "{\"rpc\":{\"disable_rtcp\":\"no\"}}",
        "{\"bindings\":\"tcp://127.0.0.1:1\"}",
        "{\"bindings\":[\"not-a-uri\"]}",
        "{\"bindings\":[{\"endpoint\":\"tcp://127.0.0.1:1\",\"roles\":[\"nope\"]}]}",
        "{\"bindings\":[{\"endpoint\":\"tcp://127.0.0.1:1\",\"roles\":\"ps\"}]}",
        "{\"bindings\":[{\"uri\":\"tcp://127.0.0.1:1\"}]}",
        "{\"bindings\":[{\"name\":\"0123456789abcdef\",\"endpoint\":\"tcp://127.0.0.1:1\"}]}",
        "{\"bindings\":[{\"endpoint\":\"tcp://127.0.0.1:1\",\"ps_qos\":{\"depth\":1.5}}]}",
        "{\"pubsub\":{\"default_qos\":{\"depth\":1.5}}}",
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

    CHECK(nb_config_parse("{\"rpc\":{\"mode\":\"static\"}}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->rpc.mode == NB_NODE_MODE_STATIC && n->rpc.discovery.disable == 1);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"rpc\":{\"mode\":\"mesh\"}}", &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->rpc.mode == NB_NODE_MODE_MESH && n->rpc.discovery.disable == 0);
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
                       "\"pubsub\":{\"default_qos\":{"
                       "\"reliability\":\"reliable\",\"durability\":\"transient_local\","
                       "\"history\":\"keep_last\",\"depth\":8,\"deadline_ms\":250,"
                       "\"lifespan_ms\":1000,\"liveliness\":\"manual_by_topic\","
                       "\"lease_duration_ms\":2000,\"ownership\":\"exclusive\","
                       "\"partition\":\"orders\"}},"
                       "\"bindings\":[{\"name\":\"dds\",\"endpoint\":\"dds://0:1\","
                       "\"ps_qos\":{\"reliability\":\"best_effort\",\"history\":\"keep_all\"}}]"
                       "}";
    nb_config_doc_t *cfg = NULL;
    nb_config_error_t err;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->pubsub.default_qos.reliability == NB_QOS_RELIABILITY_RELIABLE);
        CHECK(n->pubsub.default_qos.durability == NB_QOS_DURABILITY_TRANSIENT_LOCAL);
        CHECK(n->pubsub.default_qos.history == NB_QOS_HISTORY_KEEP_LAST);
        CHECK(n->pubsub.default_qos.depth == 8);
        CHECK(n->pubsub.default_qos.deadline_ms == 250);
        CHECK(n->pubsub.default_qos.lifespan_ms == 1000);
        CHECK(n->pubsub.default_qos.liveliness == NB_QOS_LIVELINESS_MANUAL_BY_TOPIC);
        CHECK(n->pubsub.default_qos.lease_duration_ms == 2000);
        CHECK(n->pubsub.default_qos.ownership == NB_QOS_OWNERSHIP_EXCLUSIVE);
        CHECK(strcmp(n->pubsub.default_qos.partition, "orders") == 0);
        CHECK(n->binding_count == 1);
        CHECK(n->bindings[0].ps_qos.reliability == NB_QOS_RELIABILITY_BEST_EFFORT);
        CHECK(n->bindings[0].ps_qos.history == NB_QOS_HISTORY_KEEP_ALL);
    }
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"pubsub\":{\"default_qos\":{\"reliability\":\"sometimes\"}}}",
                          &cfg,
                          &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "pubsub.default_qos.reliability") == 0);

    cfg = NULL;
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"pubsub\":{\"default_qos\":{\"depth\":1.5}}}", &cfg, &err) ==
          NB_ERR_INVALID);
    CHECK(strcmp(err.path, "pubsub.default_qos.depth") == 0);

    cfg = NULL;
    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"bindings\":[{\"endpoint\":\"tcp://127.0.0.1:1\","
                          "\"ps_qos\":{\"history\":\"nope\"}}]}",
                          &cfg,
                          &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "bindings[0].ps_qos.history") == 0);
}

static void test_binding_config(void)
{
    const char *json = "{\"name\":\"dual\","
                       "\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "{\"name\":\"udp\",\"endpoint\":\"udp://127.0.0.1:8081\","
                       "\"roles\":[\"rpc\"],\"poll_us\":250}],"
                       "\"rpc\":{\"mode\":\"static\","
                       "\"routes\":[{\"service\":\"order\","
                       "\"endpoint\":\"tcp://127.0.0.1:9000\"}]}}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->binding_count == 2);
        CHECK(strcmp(n->bindings[0].name, "tcp") == 0);
        CHECK(strcmp(n->bindings[1].endpoint, "udp://127.0.0.1:8081") == 0);
        CHECK(n->bindings[0].poll_us == 0);
        CHECK(n->bindings[1].poll_us == 250);
        CHECK(n->bindings[1].roles == NB_PROVIDER_ROLE_RPC);
        CHECK(n->rpc.route_count == 1);
        CHECK(strcmp(n->rpc.routes[0].endpoint, "tcp://127.0.0.1:9000") == 0);
    }
    nb_config_doc_destroy(cfg);
}

static void test_nested_bindings(void)
{
    const char *json = "{\"bindings\":["
                       "{\"name\":\"tcp\",\"endpoint\":\"tcp://127.0.0.1:8080\"},"
                       "\"udp://127.0.0.1:8081\"]}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(n->binding_count == 2);
        CHECK(strcmp(n->bindings[0].name, "tcp") == 0);
        CHECK(strcmp(n->bindings[1].name, "udp") == 0);
        CHECK(strcmp(n->bindings[1].endpoint, "udp://127.0.0.1:8081") == 0);
        CHECK(n->bindings[1].roles == 0);
    }
    nb_config_doc_destroy(cfg);
}

static void test_rtcp_config(void)
{
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse("{\"name\":\"r\",\"rpc\":{\"rtcp_interval_ms\":2000}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->rpc.rtcp_interval_ms == 2000 && n->rpc.disable_rtcp == 0);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"name\":\"r\",\"rpc\":{\"disable_rtcp\":false}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->rpc.disable_rtcp == 0);
    nb_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_config_parse("{\"name\":\"r\",\"rpc\":{\"disable_rtcp\":true}}", &cfg, NULL) ==
          NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL && n->rpc.disable_rtcp == 1);
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
    CHECK(nb_config_parse("{\"rpc\":{\"frag_bytes\":70000}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "rpc.frag_bytes") == 0);
    CHECK(err.reason[0] != '\0');

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"rpc\":{\"mode\":\"nope\"}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "rpc.mode") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"rpc\":{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}}",
                          &cfg,
                          &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "rpc.routes[0].endpoint") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{\"rpc\":{\"discovery\":{\"peers\":[\"bad\"]}}}", &cfg, &err) ==
          NB_ERR_INVALID);
    CHECK(strcmp(err.path, "rpc.discovery.peers[0]") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_config_parse("{ not json", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "") == 0);
}

static void test_defaults_api(void)
{
    nb_node_config_t cfg;
    nb_node_config_default(&cfg);
    CHECK(strcmp(cfg.name, "nb") == 0);
    CHECK(cfg.rpc.mode == NB_NODE_MODE_MESH);
    CHECK(cfg.rpc.kind == NB_TRANSPORT_KIND_UDP);
    CHECK(strcmp(cfg.rpc.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(cfg.rpc.discovery.broadcast_host, "255.255.255.255") == 0);
    CHECK(cfg.metrics.sample_every == 1);
    CHECK(cfg.log.level == NB_LOG_LEVEL_INFO);
    CHECK(cfg.rpc.routes == NULL && cfg.rpc.route_count == 0);
    CHECK(cfg.rpc.peers == NULL && cfg.rpc.peer_count == 0);
    CHECK(cfg.bindings == NULL && cfg.binding_count == 0);
    CHECK(nb_ps_qos_is_default(&cfg.pubsub.default_qos));
}

static void test_normalize(void)
{
    nb_node_config_t cfg;

    nb_node_config_default(&cfg);
    cfg.runtime.handler_threads = 5000;
    cfg.runtime.send_lanes = 5000;
    cfg.rpc.frag_bytes = 100;
    cfg.rpc.max_msg_bytes = 64;
    cfg.runtime.max_streams = 5000;
    CHECK(nb_node_config_validate(&cfg) == NB_ERR_INVALID);
    nb_node_config_normalize(&cfg);
    CHECK(cfg.runtime.handler_threads <= NB_MAX_HANDLER_THREADS);
    CHECK(cfg.runtime.send_lanes <= NB_MAX_SEND_LANES);
    CHECK(cfg.rpc.frag_bytes == 256);
    CHECK(cfg.rpc.max_msg_bytes == 256);
    CHECK(cfg.runtime.max_streams == NB_MAX_STREAM_RECV_STREAMS);

    nb_node_config_default(&cfg);
    cfg.rpc.discovery.disable = true;
    nb_node_config_normalize(&cfg);
    CHECK(cfg.rpc.mode == NB_NODE_MODE_STATIC);

    nb_node_config_default(&cfg);
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    nb_node_config_normalize(&cfg);
    CHECK(cfg.rpc.discovery.disable == 1);

    nb_node_config_default(&cfg);
    cfg.log.level = NB_LOG_LEVEL_DEFAULT;
    nb_node_config_normalize(&cfg);
    CHECK(cfg.log.level == NB_LOG_LEVEL_INFO);
}

static void test_doc_lifetime(void)
{
    const char *json = "{\"name\":\"orig\",\"bindings\":[\"tcp://127.0.0.1:1\"]}";
    nb_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_config_node(cfg);
    CHECK(n != NULL);
    if (n) {
        CHECK(strcmp(n->name, "orig") == 0);
        CHECK(n->binding_count == 1);
        CHECK(strcmp(n->bindings[0].endpoint, "tcp://127.0.0.1:1") == 0);
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
    return 0;
}
