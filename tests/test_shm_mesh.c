/*
 * SHM + mesh end-to-end test (Linux only).
 *
 * Two peer nodes join the same SHM channel with distinct node ids and discover
 * each other over UDP. The service lives on node 2; node 1 resolves the full
 * shm://channel:2 URI from discovery and calls it without a direct endpoint.
 */
#include <nb/proxy.h>
#include <nb/node.h>
#include <nb/service.h>
#include <nb/shm.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct call_state {
    ntk_sem *sem;
    int status;
    char buf[256];
    size_t len;
} call_state_t;

static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    call_state_t *st = (call_state_t *)user;
    st->status = status;
    st->len = 0;
    if (status == NB_OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
    }
    ntk_sem_post(st->sem, 1);
}

int main(void)
{
    char channel[64];
    char endpoint1[96];
    char endpoint2[96];
    nb_binding_config_t binding1;
    nb_binding_config_t binding2;
    nb_node_config_t cfg1;
    nb_node_config_t cfg2;
    nb_node_t *node1 = NULL;
    nb_node_t *node2 = NULL;
    nb_service_t *service = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t payload;
    call_state_t st;
    int failed = 0;
    int rc;

    snprintf(channel, sizeof(channel), "test-shm-mesh-%ld", (long)getpid());
    snprintf(endpoint1, sizeof(endpoint1), "shm://%s:1", channel);
    snprintf(endpoint2, sizeof(endpoint2), "shm://%s:2", channel);

    if (nb_shm_register() != NB_OK) {
        nb_log_error("shm provider register failed");
        return 1;
    }

    memset(&binding1, 0, sizeof(binding1));
    binding1.name = "shm";
    binding1.endpoint = endpoint1;
    memset(&binding2, 0, sizeof(binding2));
    binding2.name = "shm";
    binding2.endpoint = endpoint2;

    nb_node_config_default(&cfg1);
    cfg1.name = "shm-mesh-a";
    cfg1.rpc.mode = NB_NODE_MODE_MESH;
    cfg1.runtime.io_threads = 1;
    cfg1.rpc.discovery.broadcast_host = "127.0.0.1";
    cfg1.rpc.discovery.port = 42621;
    cfg1.rpc.discovery.hello_interval_ms = 100;
    cfg1.rpc.discovery.node_ttl_ms = 3000;
    cfg1.bindings = &binding1;
    cfg1.binding_count = 1;

    nb_node_config_default(&cfg2);
    cfg2.name = "shm-mesh-b";
    cfg2.rpc.mode = NB_NODE_MODE_MESH;
    cfg2.runtime.io_threads = 1;
    cfg2.rpc.discovery.broadcast_host = "127.0.0.1";
    cfg2.rpc.discovery.port = 42622;
    cfg2.rpc.discovery.hello_interval_ms = 100;
    cfg2.rpc.discovery.node_ttl_ms = 3000;
    cfg2.bindings = &binding2;
    cfg2.binding_count = 1;

    rc = nb_node_create(&cfg1, &node1);
    if (rc == NB_OK) {
        rc = nb_node_create(&cfg2, &node2);
    }
    if (rc == NB_OK) {
        rc = nb_service_create(node2, "MeshEcho", &service);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(service, "echo", echo_handler, NULL);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(node1, NULL, &proxy);
    }
    if (rc != NB_OK) {
        nb_log_error("shm mesh setup failed: %d", rc);
        failed = 1;
        goto done;
    }

    (void)nb_node_add_discovery_peer(node1, "127.0.0.1", cfg2.rpc.discovery.port);
    (void)nb_node_add_discovery_peer(node2, "127.0.0.1", cfg1.rpc.discovery.port);
    if (nb_node_wait_for_service(node1, "MeshEcho", 5000) != NB_OK) {
        nb_log_error("shm mesh service was not discovered");
        failed = 1;
        goto done;
    }

    memset(&st, 0, sizeof(st));
    st.sem = ntk_sem_create(0);
    if (!st.sem) {
        failed = 1;
        goto done;
    }
    payload.data = "mesh-over-shm";
    payload.len = strlen((const char *)payload.data) + 1;
    payload.encoding = 0;
    {
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;

        opt.timeout_ms = 3000;
        rc = nb_proxy_call_async(proxy, "MeshEcho", "echo", &payload, &opt, on_response, &st);
    }
    if (rc != NB_OK || ntk_sem_timedwait(st.sem, 4000) != NTK_OK) {
        nb_log_error("shm mesh call failed: %d", rc);
        failed = 1;
    } else if (st.status != NB_OK || st.len != payload.len ||
               memcmp(st.buf, payload.data, payload.len) != 0) {
        nb_log_error("shm mesh response mismatch");
        failed = 1;
    }
    ntk_sem_destroy(st.sem);

done:
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (service) {
        nb_service_destroy(service);
    }
    if (node2) {
        nb_node_destroy(node2);
    }
    if (node1) {
        nb_node_destroy(node1);
    }
    if (failed) {
        nb_log_error("nb_shm_mesh: FAIL");
        return 1;
    }
    nb_log_info("nb_shm_mesh: NB_OK");
    return 0;
}
