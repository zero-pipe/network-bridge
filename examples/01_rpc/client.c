/*
 * 01 RPC：客户端。
 *
 * 先同步调用 EchoService.echo，再用同一方法演示异步调用。
 * 服务名通过 node 上的静态路由解析；去掉路由并启用 MESH 发现即可做到
 * 客户端不配置地址（见 README）。
 */

#define NB_LOG_TAG "example"
#include <nb/rpc/node.h>
#include <nb/rpc/proxy.h>

#include <nb/base/log.h>

#include <stdlib.h>
#include <string.h>

static volatile int g_done;
static int g_status;
static char g_reply[128];

static void on_response(int status, const nb_response_t *resp, void *user)
{
    size_t n = 0;

    (void)user;
    g_status = status;
    if (resp && resp->payload.data) {
        n = resp->payload.len;
        if (n >= sizeof(g_reply)) {
            n = sizeof(g_reply) - 1;
        }
        memcpy(g_reply, resp->payload.data, n);
    }
    g_reply[n] = '\0';
    g_done = 1;
}

int main(int argc, char **argv)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 42420);
    nb_payload_t request;
    nb_call_options_t opt = NB_CALL_OPTIONS_INIT;
    nb_response_t response;
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "echo-client";
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    cfg.rpc.kind = NB_TRANSPORT_KIND_TCP;
    cfg.rpc.bind_host = "127.0.0.1";
    cfg.rpc.data_port = 0;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        /* 跳过发现：把 EchoService 指向服务端端点。 */
        rc = nb_node_add_static_route(node, "EchoService", host, port, NB_TRANSPORT_KIND_TCP);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc != NB_OK) {
        nb_log_error("client setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    request.data = "hello rpc";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;

    /* 同步调用：阻塞到收到响应或超时。 */
    opt.timeout_ms = 3000;
    memset(&response, 0, sizeof(response));
    rc = nb_proxy_call_sync(proxy, "EchoService", "echo", &request, &opt, &response);
    if (rc == NB_OK) {
        nb_log_info("sync reply: %.*s",
                    (int)response.payload.len,
                    (const char *)response.payload.data);
    } else {
        nb_log_error(
            "sync call failed: %d (%s) - is the server running?", rc, nb_error_name(rc));
    }
    nb_response_release(&response);

    /* 异步调用：响应在 node 的 poller 线程上回调。 */
    if (rc == NB_OK) {
        rc = nb_proxy_call_async(
            proxy, "EchoService", "echo", &request, &opt, on_response, NULL);
    }
    if (rc == NB_OK) {
        int i;

        for (i = 0; i < 50 && !g_done; i++) {
            (void)nb_node_run_once(node, 100);
        }
        if (!g_done) {
            rc = NB_ERR_TIMEOUT;
            nb_log_error("async call timed out");
        }
    }
    if (rc == NB_OK) {
        nb_log_info("async reply: %s (status=%d)", g_reply, g_status);
    }

    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
