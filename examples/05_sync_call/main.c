/*
 * 05 Sync Call：同步调用便捷层。对应 fastcall-c 的 fc_proxy_call_sync。
 *
 * 先启动 02_service（默认端口 42410），再运行本示例。
 * 注意：rpc 核心仍是异步的；nb_proxy_call_sync 只是阻塞等待，禁止在
 * poller 线程回调里使用。
 */
#include <nb/proxy.h>
#include <nb/node.h>

#define NB_LOG_TAG "example"
#include <nb/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    char endpoint[64];
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42410);
    const char *text = "hello";
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "SyncClient";
    cfg.rpc.kind = NB_TRANSPORT_KIND_TCP;
    cfg.rpc.bind_host = "127.0.0.1";
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc != NB_OK) {
        nb_log_error("client setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }

    snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%u", (unsigned)port);
    if (nb_proxy_set_target_uri(proxy, endpoint) != NB_OK) {
        nb_log_error("bad endpoint: %s", endpoint);
        nb_proxy_destroy(proxy);
        nb_node_destroy(node);
        return 1;
    }

    request.data = text;
    request.len = strlen(text);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    {
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;

        opt.timeout_ms = 3000;
        rc = nb_proxy_call_sync(proxy, "HealthService", "check", &request, &opt, &response);
    }
    if (rc == NB_OK) {
        nb_log_info(
            "response: %.*s", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        nb_log_error("call failed: %d (server running?)", rc);
    }
    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
