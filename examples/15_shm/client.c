/*
 * 15 SHM：客户端。
 *
 * 通过 shm://nb-example:1 直接调用服务端，不启动服务发现。
 */
#include <nb/proxy.h>
#include <nb/node.h>
#include <nb/shm.h>

#define NB_LOG_TAG "example"
#include <nb/log.h>

#include <string.h>

int main(void)
{
    nb_binding_config_t binding;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_response_t response;
    nb_payload_t request;
    int rc;

    rc = nb_shm_register();
    if (rc != NB_OK) {
        nb_log_error("shm provider register failed: %d (%s)", rc, nb_error_name(rc));
        return 1;
    }

    memset(&binding, 0, sizeof(binding));
    binding.name = "shm";
    binding.endpoint = "shm://nb-example:2";
    binding.intra_process = NB_INTRA_PROCESS_OFF;

    nb_node_config_default(&cfg);
    cfg.name = "ShmClient";
    cfg.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_target_uri(proxy, "shm://nb-example:1");
    }
    if (rc != NB_OK) {
        nb_log_error("shm client setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    request.data = "hello over shm";
    request.len = strlen((const char *)request.data);
    request.encoding = 0;
    memset(&response, 0, sizeof(response));
    {
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;

        opt.timeout_ms = 3000;
        rc = nb_proxy_call_sync(proxy, "OrderService", "get", &request, &opt, &response);
    }
    if (rc == NB_OK) {
        nb_log_info("shm response: %.*s",
                    (int)response.payload.len,
                    (const char *)response.payload.data);
    } else {
        nb_log_error("shm call failed: %d (%s)", rc, nb_error_name(rc));
    }

    nb_response_release(&response);
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
