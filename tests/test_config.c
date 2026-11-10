/* RPC and DDS JSON configuration parsing, defaults and error paths. */
#include <nb/config.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_rpc_full(void)
{
    const char *json = "{"
                       "\"name\":\"nested\","
                       "\"threads\":{\"io\":2,\"handlers\":3,\"send_lanes\":1},"
                       "\"limits\":{\"max_inflight\":100,\"max_streams\":8,"
                       "\"handler_queue\":64,\"peer_rate_per_second\":10},"
                       "\"rpc\":{"
                       "\"mode\":\"static\","
                       "\"listen\":[\"tcp://127.0.0.1:9000\"],"
                       "\"max_message_bytes\":4194304,"
                       "\"backpressure_bytes\":8192,"
                       "\"udp\":{\"fragment_bytes\":1024,\"rtcp_interval\":\"2s\",\"rtcp\":false},"
                       "\"discovery\":{\"broadcast\":\"127.0.0.1:41953\","
                       "\"hello_interval\":\"500ms\",\"node_ttl\":\"9s\","
                       "\"peers\":[\"10.0.0.1:41953\"]},"
                       "\"routes\":[{\"service\":\"echo\",\"endpoint\":\"tcp://10.0.0.2:9001\"}]"
                       "},"
                       "\"metrics\":{\"enabled\":false,\"latency_histogram\":false,"
                       "\"sample_every\":5},"
                       "\"log\":{\"enabled\":false,\"level\":\"debug\"},"
                       "\"dds\":{\"backend\":\"cyclone\",\"domain\":3}"
                       "}";
    nb_rpc_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_rpc_config_parse(json, &cfg, NULL) == NB_OK);
    n = nb_rpc_config_node(cfg);
    CHECK(n != NULL);
    if (!n) {
        nb_rpc_config_doc_destroy(cfg);
        return;
    }
    CHECK(strcmp(n->name, "nested") == 0);
    CHECK(n->runtime.io_threads == 2);
    CHECK(n->runtime.handler_threads == 3);
    CHECK(n->runtime.send_lanes == 1);
    CHECK(n->runtime.max_inflight == 100);
    CHECK(n->runtime.max_streams == 8);
    CHECK(n->runtime.handler_max_concurrency == 64);
    CHECK(n->runtime.peer_rate_limit_pps == 10);
    CHECK(n->rpc.mode == NB_NODE_MODE_STATIC);
    CHECK(n->rpc.discovery.disable == 1);
    CHECK(strcmp(n->rpc.discovery.broadcast_host, "127.0.0.1") == 0);
    CHECK(n->rpc.discovery.port == 41953);
    CHECK(n->rpc.discovery.hello_interval_ms == 500);
    CHECK(n->rpc.discovery.node_ttl_ms == 9000);
    CHECK(n->rpc.peer_count == 1);
    CHECK(strcmp(n->rpc.peers[0].host, "10.0.0.1") == 0);
    CHECK(n->rpc.peers[0].port == 41953);
    CHECK(n->rpc.max_msg_bytes == 4194304);
    CHECK(n->rpc.backpressure_bytes == 8192);
    CHECK(n->rpc.frag_bytes == 1024);
    CHECK(n->rpc.rtcp_interval_ms == 2000);
    CHECK(n->rpc.disable_rtcp == 1);
    CHECK(n->rpc.route_count == 1);
    CHECK(strcmp(n->rpc.routes[0].service, "echo") == 0);
    CHECK(strcmp(n->rpc.routes[0].endpoint, "tcp://10.0.0.2:9001") == 0);
    CHECK(n->binding_count == 1);
    if (n->binding_count == 1) {
        CHECK(strcmp(n->bindings[0].endpoint, "tcp://127.0.0.1:9000") == 0);
        CHECK(strcmp(n->bindings[0].name, "tcp") == 0);
    }
    CHECK(n->metrics.disable == 1);
    CHECK(n->metrics.disable_latency_histogram == 1);
    CHECK(n->metrics.sample_every == 5);
    CHECK(n->log.disable == 1);
    CHECK(n->log.level == NB_LOG_LEVEL_DEBUG);
    nb_rpc_config_doc_destroy(cfg);
}

static void test_rpc_defaults(void)
{
    nb_rpc_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_rpc_config_parse("{}", &cfg, NULL) == NB_OK);
    n = nb_rpc_config_node(cfg);
    CHECK(n != NULL);
    if (!n) {
        nb_rpc_config_doc_destroy(cfg);
        return;
    }
    CHECK(strcmp(n->name, "nb") == 0);
    CHECK(n->rpc.kind == NB_TRANSPORT_KIND_UDP);
    CHECK(n->rpc.mode == NB_NODE_MODE_MESH);
    CHECK(n->rpc.discovery.disable == 0);
    CHECK(strcmp(n->rpc.bind_host, "0.0.0.0") == 0);
    CHECK(strcmp(n->rpc.discovery.broadcast_host, "255.255.255.255") == 0);
    CHECK(n->rpc.discovery.port != 0);
    CHECK(n->rpc.frag_bytes == 1440);
    CHECK(n->binding_count == 0);
    CHECK(n->bindings == NULL);
    CHECK(n->rpc.peer_count == 0);
    CHECK(n->rpc.route_count == 0);
    CHECK(n->metrics.disable == 0);
    CHECK(n->metrics.sample_every == 1);
    CHECK(n->log.disable == 0);
    CHECK(n->log.level == NB_LOG_LEVEL_INFO);
    nb_rpc_config_doc_destroy(cfg);
}

static void test_rpc_strict(void)
{
    static const char *bad[] = {
        "{\"name\":42}",
        "{\"threads\":[]}",
        "{\"threads\":{\"io\":\"2\"}}",
        "{\"threads\":{\"io\":2000}}",
        "{\"threads\":{\"io\":9}}",
        "{\"threads\":{\"handlers\":1025}}",
        "{\"threads\":{\"send_lanes\":65}}",
        "{\"limits\":{\"max_streams\":65}}",
        "{\"rpc\":[]}",
        "{\"rpc\":{\"mode\":\"foo\"}}",
        "{\"rpc\":{\"mode\":\"STATIC\"}}",
        "{\"rpc\":{\"listen\":\"tcp://127.0.0.1:1\"}}",
        "{\"rpc\":{\"listen\":[\"not-a-uri\"]}}",
        "{\"rpc\":{\"max_message_bytes\":67108865}}",
        "{\"rpc\":{\"udp\":{\"fragment_bytes\":255}}}",
        "{\"rpc\":{\"udp\":{\"fragment_bytes\":8193}}}",
        "{\"rpc\":{\"udp\":{\"rtcp_interval\":\"nope\"}}}",
        "{\"rpc\":{\"udp\":{\"rtcp\":\"no\"}}}",
        "{\"rpc\":{\"discovery\":[]}}",
        "{\"rpc\":{\"discovery\":{\"broadcast\":\"bad\"}}}",
        "{\"rpc\":{\"discovery\":{\"peers\":[\"bad\"]}}}",
        "{\"rpc\":{\"routes\":[{\"service\":\"x\"}]}}",
        "{\"rpc\":{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}}",
        "{\"metrics\":\"off\"}",
        "{\"log\":{\"level\":\"nope\"}}",
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        nb_rpc_config_doc_t *cfg = (nb_rpc_config_doc_t *)1;

        if (nb_rpc_config_parse(bad[i], &cfg, NULL) != NB_ERR_INVALID) {
            printf("strict config accepted invalid case %lu: %s\n", (unsigned long)i, bad[i]);
            g_fail = 1;
        }
        CHECK(cfg == NULL);
    }
}

static void test_rpc_unknown_and_mode(void)
{
    nb_rpc_config_doc_t *cfg = NULL;
    const nb_node_config_t *n;

    CHECK(nb_rpc_config_parse("{\"threads\":{\"handler_thread\":3}}", &cfg, NULL) == NB_OK);
    n = nb_rpc_config_node(cfg);
    CHECK(n != NULL && n->runtime.handler_threads == 0);
    nb_rpc_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_rpc_config_parse("{\"rpc\":{\"mode\":\"static\"}}", &cfg, NULL) == NB_OK);
    n = nb_rpc_config_node(cfg);
    CHECK(n != NULL && n->rpc.mode == NB_NODE_MODE_STATIC && n->rpc.discovery.disable == 1);
    nb_rpc_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_rpc_config_parse("{\"rpc\":{\"mode\":\"mesh\"}}", &cfg, NULL) == NB_OK);
    n = nb_rpc_config_node(cfg);
    CHECK(n != NULL && n->rpc.mode == NB_NODE_MODE_MESH && n->rpc.discovery.disable == 0);
    nb_rpc_config_doc_destroy(cfg);
}

static void test_rpc_errors(void)
{
    nb_rpc_config_doc_t *cfg = NULL;
    nb_config_error_t err;

    memset(&err, 0, sizeof(err));
    CHECK(nb_rpc_config_parse("{\"threads\":{\"io\":\"2\"}}", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "threads.io") == 0);
    CHECK(err.reason[0] != '\0');

    memset(&err, 0, sizeof(err));
    CHECK(nb_rpc_config_parse("{\"rpc\":{\"routes\":[{\"service\":\"x\",\"endpoint\":\"bad\"}]}}",
                              &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "rpc.routes[0].endpoint") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_rpc_config_parse("{\"rpc\":{\"discovery\":{\"peers\":[\"bad\"]}}}", &cfg, &err) ==
          NB_ERR_INVALID);
    CHECK(strcmp(err.path, "rpc.discovery.peers[0]") == 0);

    memset(&err, 0, sizeof(err));
    CHECK(nb_rpc_config_parse("{ not json", &cfg, &err) == NB_ERR_INVALID);
    CHECK(strcmp(err.path, "") == 0);
}

static void test_dds(void)
{
    const char *json = "{"
                       "\"name\":\"dds-app\","
                       "\"rpc\":{\"mode\":\"static\"},"
                       "\"dds\":{"
                       "\"backend\":\"rtps\",\"domain\":7,"
                       "\"partition\":\"orders\",\"prefix\":\"app\","
                       "\"qos\":{\"reliability\":\"reliable\",\"durability\":\"transient_local\","
                       "\"history\":\"keep_last\",\"depth\":8,\"deadline\":\"250ms\","
                       "\"lifespan\":\"1s\",\"liveliness\":\"manual_by_topic\","
                       "\"lease_duration\":2000,\"ownership\":\"exclusive\","
                       "\"autodispose_unregistered\":true,\"presentation_scope\":\"topic\"},"
                       "\"rtps\":{\"bind_host\":\"127.0.0.1\",\"multicast_group\":\"\","
                       "\"spdp_port\":48200,\"peers\":[\"127.0.0.1:48202\"]}"
                       "}}";
    nb_dds_config_doc_t *cfg = NULL;
    const nb_dds_config_t *d;

    CHECK(nb_dds_config_parse(json, &cfg, NULL) == NB_OK);
    d = nb_dds_config_get(cfg);
    CHECK(d != NULL);
    if (!d) {
        nb_dds_config_doc_destroy(cfg);
        return;
    }
    CHECK(d->backend == NB_DDS_BACKEND_RTPS);
    CHECK(d->domain == 7);
    CHECK(strcmp(d->partition, "orders") == 0);
    CHECK(strcmp(d->prefix, "app") == 0);
    CHECK(strcmp(d->name, "dds-app") == 0);
    CHECK(d->qos.reliability == NB_DDS_RELIABILITY_RELIABLE);
    CHECK(d->qos.durability == NB_DDS_DURABILITY_TRANSIENT_LOCAL);
    CHECK(d->qos.history == NB_DDS_HISTORY_KEEP_LAST);
    CHECK(d->qos.depth == 8);
    CHECK(d->qos.deadline_ms == 250);
    CHECK(d->qos.lifespan_ms == 1000);
    CHECK(d->qos.liveliness == NB_DDS_LIVELINESS_MANUAL_BY_TOPIC);
    CHECK(d->qos.lease_duration_ms == 2000);
    CHECK(d->qos.ownership == NB_DDS_OWNERSHIP_EXCLUSIVE);
    CHECK(d->qos.autodispose_unregistered_instances);
    CHECK(d->qos.presentation_scope == NB_DDS_PRESENTATION_TOPIC);
    CHECK(d->has_rtps);
    CHECK(strcmp(d->rtps.bind_host, "127.0.0.1") == 0);
    CHECK(d->rtps.spdp_port == 48200);
    CHECK(d->rtps.peer_count == 1);
    if (d->rtps.peer_count == 1) {
        CHECK(strcmp(d->rtps.peers[0].host, "127.0.0.1") == 0);
        CHECK(d->rtps.peers[0].port == 48202);
    }
    nb_dds_config_doc_destroy(cfg);

    cfg = NULL;
    CHECK(nb_dds_config_parse("{}", &cfg, NULL) == NB_OK);
    d = nb_dds_config_get(cfg);
    CHECK(d != NULL && d->backend == NB_DDS_BACKEND_RTPS && d->domain == 0);
    nb_dds_config_doc_destroy(cfg);

    cfg = (nb_dds_config_doc_t *)1;
    CHECK(nb_dds_config_parse("{\"dds\":{\"backend\":\"nope\"}}", &cfg, NULL) ==
          NB_ERR_INVALID);
    CHECK(cfg == NULL);

    cfg = (nb_dds_config_doc_t *)1;
    CHECK(nb_dds_config_parse("{\"dds\":{\"qos\":{\"reliability\":\"sometimes\"}}}", &cfg,
                              NULL) == NB_ERR_INVALID);
    CHECK(cfg == NULL);
}

int main(void)
{
    test_rpc_full();
    test_rpc_defaults();
    test_rpc_strict();
    test_rpc_unknown_and_mode();
    test_rpc_errors();
    test_dds();

    if (g_fail) {
        printf("test_config: failed\n");
        return 1;
    }
    printf("test_config: ok\n");
    return 0;
}
