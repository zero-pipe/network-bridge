/*
 * 01 RPC：服务端。
 *
 * 最小的 RPC 服务：创建 node，注册一个同步方法，然后阻塞运行。
 * 客户端见 client.c，通过 TCP 调用这个方法。
 */

#define NB_LOG_TAG "example"
#include <nb/rpc/node.h>
#include <nb/rpc/service.h>

#include <nb/base/log.h>

#include <stdlib.h>

static int echo(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    /* 载荷对框架透明：原样回显。 */
    return nb_message_view_set_data(resp, req->payload.data, req->payload.len);
}

int main(int argc, char **argv)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 42420);
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "echo-server";
    cfg.rpc.mode = NB_NODE_MODE_STATIC; /* 本例使用固定端点，不启用发现 */
    cfg.rpc.kind = NB_TRANSPORT_KIND_TCP;
    cfg.rpc.bind_host = "0.0.0.0";
    cfg.rpc.data_port = port;
    cfg.runtime.io_threads = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "EchoService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "echo", echo, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("server setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    nb_log_info("EchoService.echo listening on tcp://0.0.0.0:%u (Ctrl+C to stop)",
                (unsigned)port);
    (void)nb_node_run(node);
    nb_node_destroy(node);
    return 0;
}
