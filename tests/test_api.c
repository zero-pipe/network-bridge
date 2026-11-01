/* 覆盖新增抽象：service_table、message_view、proxy 与 sync 便捷层。 */
#include <nb/proxy.h>
#include <nb/node.h>
#include <nb/codec.h>
#include <nb/service.h>
#include <platform.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <thread/sync.h>

#include <stdio.h>
#include <string.h>

/* ---- 两端共享的声明表 ---- */
NB_SERVICE_DEFINE(
    g_order_table, 7u, "OrderService", NB_SERVICE_METHOD(1u, "get"), NB_SERVICE_METHOD(2u, "put"));

static int test_service_table(void)
{
    uint16_t id = 0;
    int failed = 0;
    nb_service_table_reset();
    if (nb_service_table_is_active()) {
        nb_log_error("table should be inactive after reset");
        failed = 1;
    }
    if (nb_service_table_install(&g_order_table) != NB_OK) {
        nb_log_error("install failed");
        return 1;
    }
    /* 幂等。 */
    if (nb_service_table_install(&g_order_table) != NB_OK) {
        nb_log_error("re-install should be idempotent");
        failed = 1;
    }
    if (!nb_service_table_is_active()) {
        nb_log_error("table should be active");
        failed = 1;
    }
    if (nb_service_table_find_id("OrderService", "get", &id) != NB_OK || id != 1u) {
        nb_log_error("find get failed: id=%u", (unsigned)id);
        failed = 1;
    }
    if (nb_service_table_find_id("OrderService", "missing", &id) != NB_ERR_NOTFOUND) {
        nb_log_error("missing method should not be found");
        failed = 1;
    }
    nb_service_table_reset();
    return failed;
}

static int test_message_view(void)
{
    nb_message_view_t view;
    nb_payload_t pl;
    int failed = 0;
    const char borrowed[] = "borrowed";
    nb_message_view_init(&view);
    if (nb_message_view_set_string(&view, "hello") != NB_OK || view.len != 5 ||
        memcmp(view.data, "hello", 5) != 0) {
        nb_log_error("set_string failed");
        failed = 1;
    }
    nb_message_view_release(&view);
    if (view.data != NULL || view.len != 0 || view.owner != NULL) {
        nb_log_error("release did not clear view");
        failed = 1;
    }
    if (nb_message_view_set_data(&view, "abc", 3) != NB_OK) {
        nb_log_error("set_data failed");
        failed = 1;
    }
    nb_message_view_set_borrowed(&view, borrowed, sizeof(borrowed) - 1);
    if (view.owner != NULL) {
        nb_log_error("borrowed view should not own memory");
        failed = 1;
    }
    pl.data = view.data;
    pl.len = view.len;
    pl.encoding = 2;
    if (nb_message_view_set_payload(&view, &pl) != NB_OK || view.encoding != 2 ||
        memcmp(view.data, borrowed, sizeof(borrowed) - 1) != 0) {
        nb_log_error("set_payload failed");
        failed = 1;
    }
    nb_message_view_release(&view);
    return failed;
}

typedef struct test_counter {
    ntk_mutex *lock;
    int value;
} test_counter_t;

static int echo_sync(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    test_counter_t *counter = (test_counter_t *)user;
    if (counter && counter->lock) {
        ntk_mutex_lock(counter->lock);
        counter->value++;
        ntk_mutex_unlock(counter->lock);
    }
    (void)nb_message_view_set_data(resp, req->payload.data, req->payload.len);
    return 0;
}

static int counter_read(test_counter_t *counter)
{
    int value;
    ntk_mutex_lock(counter->lock);
    value = counter->value;
    ntk_mutex_unlock(counter->lock);
    return value;
}

static int test_proxy_and_sync(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t pl;
    uint16_t sport;
    uint16_t cport;
    test_counter_t counter;
    int failed = 0;
    int rc;

    sport = 42311;
    cport = 42312;
    memset(&counter, 0, sizeof(counter));
    counter.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!counter.lock) {
        return 1;
    }

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "api_srv";
    scfg.transport.kind = NB_TRANSPORT_KIND_UDP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.broadcast_host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;
    scfg.service_table = &g_order_table;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "api_cli";
    ccfg.transport.kind = NB_TRANSPORT_KIND_UDP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.broadcast_host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    rc = nb_node_create(&scfg, &server);
    if (rc != NB_OK) {
        nb_log_error("server create failed: %d", rc);
        ntk_mutex_destroy(counter.lock);
        return 1;
    }
    rc = nb_service_create(server, "OrderService", &svc);
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "get", echo_sync, &counter);
    }
    if (rc != NB_OK) {
        nb_log_error("service setup failed: %d", rc);
        nb_node_destroy(server);
        ntk_mutex_destroy(counter.lock);
        return 1;
    }
    rc = nb_node_create(&ccfg, &client);
    if (rc != NB_OK) {
        nb_log_error("client create failed: %d", rc);
        nb_node_destroy(server);
        ntk_mutex_destroy(counter.lock);
        return 1;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_node_wait_for_service(client, "OrderService", 5000) != NB_OK) {
        nb_log_error("service not discovered");
        failed = 1;
        goto cleanup;
    }

    rc = nb_proxy_create(client, NULL, &proxy);
    if (rc != NB_OK) {
        nb_log_error("proxy create failed: %d", rc);
        failed = 1;
        goto cleanup;
    }
    memset(&response, 0, sizeof(response));
    pl.data = "ping";
    pl.len = 4;
    pl.encoding = 0;
    {
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;

        opt.timeout_ms = 3000;
        rc = nb_proxy_call_sync(proxy, "OrderService", "get", &pl, &opt, &response);
    }
    if (rc != NB_OK) {
        nb_log_error("call_sync failed: %d", rc);
        failed = 1;
    } else if (response.status != 0 || response.payload.len != 4 ||
               memcmp(response.payload.data, "ping", 4) != 0) {
        nb_log_error(
            "call_sync bad response: status=%d len=%zu", response.status, response.payload.len);
        failed = 1;
    }
    nb_response_release(&response);

    /* proxy 的单向事件入口：同一 node 既可作为 service 也可作为 client。 */
    {
        int before = counter_read(&counter);
        nb_request_id_t event_id = 0;
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;

        opt.out_request_id = &event_id;
        rc = nb_proxy_call_event(proxy, "OrderService", "get", &pl, &opt);
        if (rc != NB_OK || event_id == 0) {
            nb_log_error("call_event failed: %d", rc);
            failed = 1;
        } else {
            ntk_sleep_ms(100);
            if (counter_read(&counter) <= before) {
                nb_log_error("call_event was not delivered");
                failed = 1;
            }
        }
    }

cleanup:
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    ntk_mutex_destroy(counter.lock);
    return failed;
}

int main(void)
{
    int failed = 0;
    failed += test_service_table();
    failed += test_message_view();
    failed += test_proxy_and_sync();
    if (failed) {
        nb_log_error("nb_api: FAIL");
        return 1;
    }
    nb_log_info("nb_api: NB_OK");
    return 0;
}
