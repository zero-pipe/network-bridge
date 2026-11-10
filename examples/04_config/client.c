/*
 * 04 配置：客户端。同一套 JSON 格式，通过 rpc.routes 静态指向服务端。
 *
 * 用法：example_04_config_client [client.json]
 */

#define NB_LOG_TAG "example"
#include <nb/config.h>
#include <nb/rpc/node.h>
#include <nb/rpc/proxy.h>

#include <nb/base/log.h>

#include <string.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "client.json";
    nb_rpc_config_doc_t *cfg = NULL;
    nb_config_error_t err;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    nb_call_options_t opt = NB_CALL_OPTIONS_INIT;
    int rc;

    rc = nb_rpc_config_parse_file(path, &cfg, &err);
    if (rc != NB_OK) {
        nb_log_error(
            "load config failed: %s (%d) path=%s reason=%s", path, rc, err.path, err.reason);
        return 1;
    }
    rc = nb_node_create_from_config(cfg, &node);
    nb_rpc_config_doc_destroy(cfg);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc != NB_OK) {
        nb_log_error("client setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    request.data = "hello config";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    opt.timeout_ms = 3000;
    memset(&response, 0, sizeof(response));
    rc = nb_proxy_call_sync(proxy, "EchoService", "echo", &request, &opt, &response);
    if (rc == NB_OK) {
        nb_log_info(
            "response: %.*s", (int)response.payload.len, (const char *)response.payload.data);
    } else {
        nb_log_error("call failed: %d (%s) - is the server running?", rc, nb_error_name(rc));
    }

    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
