#include <nb/proxy.h>
#include <nb/service.h>
#include <nb/node.h>
#include <platform.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <nb/buf.h>

#include <thread/sem.h>

#include <stdio.h>
#include <string.h>

static ntk_sem *g_resp_sem;
static ntk_sem *g_event_sem;
static int g_response_ok;

static void on_worker_request(nb_call_t *call, const nb_request_t *req, void *user)
{
    nb_buf_t *buf;
    nb_owned_payload_t payload;

    (void)user;
    if (!req || req->payload.len != 12 ||
        memcmp(req->payload.data, "move-request", 12) != 0) {
        nb_reply(call, -1, NULL);
        return;
    }
    buf = nb_buf_alloc(32);
    if (!buf) {
        nb_reply(call, -2, NULL);
        return;
    }
    memcpy(nb_buf_data(buf), "move-reply", 10);
    nb_owned_payload_init(&payload, buf, 0, 10, 0);
    nb_reply_owned(call, 0, &payload);
}

static void on_event(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)call;
    (void)req;
    (void)user;
    if (g_event_sem) {
        ntk_sem_post(g_event_sem, 1);
    }
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    (void)user;
    g_response_ok = status == NB_OK && resp && resp->payload.len == 10 &&
        memcmp(resp->payload.data, "move-reply", 10) == 0;
    if (g_resp_sem) {
        ntk_sem_post(g_resp_sem, 1);
    }
}

int main(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_service_method_options_t worker_opts;
    nb_buf_t *buf;
    nb_owned_payload_t request;
    int rc;
    int failed = 0;

    nb_node_config_default(&scfg);
    scfg.name = "move_srv";
    scfg.rpc.mode = NB_NODE_MODE_STATIC;
    scfg.rpc.kind = NB_TRANSPORT_KIND_UDP;
    scfg.rpc.bind_host = "127.0.0.1";
    scfg.rpc.data_port = 42511;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 1;

    nb_node_config_default(&ccfg);
    ccfg.name = "move_cli";
    ccfg.rpc.mode = NB_NODE_MODE_STATIC;
    ccfg.rpc.kind = NB_TRANSPORT_KIND_UDP;
    ccfg.rpc.bind_host = "127.0.0.1";
    ccfg.runtime.io_threads = 1;

    rc = nb_node_create(&scfg, &server);
    if (rc == NB_OK) {
        rc = nb_service_create(server, "MoveService", &svc);
    }
    if (rc == NB_OK) {
        worker_opts.exec = NB_SERVICE_EXEC_MODE_WORKER;
        worker_opts.executor = NULL;
        rc = nb_service_add_method_with_options(svc, "work", on_worker_request, NULL, &worker_opts);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(svc, "event", on_event, NULL);
    }
    if (rc == NB_OK) {
        rc = nb_node_create(&ccfg, &client);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(client, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_target_uri(proxy, "udp://127.0.0.1:42511");
    }
    if (rc != NB_OK) {
        nb_log_error("move test setup failed: %d", rc);
        failed = 1;
        goto done;
    }

    g_resp_sem = ntk_sem_create(0);
    g_event_sem = ntk_sem_create(0);
    if (!g_resp_sem || !g_event_sem) {
        failed = 1;
        goto done;
    }

    buf = nb_buf_alloc(32);
    if (!buf) {
        failed = 1;
        goto done;
    }
    memcpy(nb_buf_data(buf), "move-request", 12);
    nb_owned_payload_init(&request, buf, 0, 12, 0);
    {
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;

        opt.timeout_ms = 2000;
        rc = nb_proxy_call_async_owned(
            proxy, "MoveService", "work", &request, &opt, on_response, NULL);
    }
    if (rc != NB_OK || request.buf != NULL) {
        nb_log_error("call_async_move failed: rc=%d buf=%p", rc, (void *)request.buf);
        failed = 1;
        goto done;
    }
    if (ntk_sem_timedwait(g_resp_sem, 2000) != NTK_OK) {
        nb_log_error("move unary timed out");
        failed = 1;
    } else if (!g_response_ok) {
        nb_log_error("move unary response bad");
        failed = 1;
    }

    buf = nb_buf_alloc(32);
    if (!buf) {
        failed = 1;
        goto done;
    }
    memcpy(nb_buf_data(buf), "move-event", 10);
    nb_owned_payload_init(&request, buf, 0, 10, 0);
    rc = nb_proxy_call_event_owned(proxy, "MoveService", "event", &request, NULL);
    if (rc != NB_OK || request.buf != NULL) {
        nb_log_error("call_event_move failed: rc=%d buf=%p", rc, (void *)request.buf);
        failed = 1;
        goto done;
    }
    if (ntk_sem_timedwait(g_event_sem, 2000) != NTK_OK) {
        nb_log_error("move event not delivered");
        failed = 1;
    }

done:
    nb_proxy_destroy(proxy);
    nb_service_destroy(svc);
    nb_node_destroy(client);
    nb_node_destroy(server);
    if (g_resp_sem) {
        ntk_sem_destroy(g_resp_sem);
        g_resp_sem = NULL;
    }
    if (g_event_sem) {
        ntk_sem_destroy(g_event_sem);
        g_event_sem = NULL;
    }
    if (failed) {
        nb_log_error("MOVE FAIL");
    } else {
        nb_log_info("MOVE NB_OK");
    }
    return failed ? 1 : 0;
}
