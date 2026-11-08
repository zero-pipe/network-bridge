/*
 * 04 配置：服务端。生产环境通常由 JSON 决定绑定、线程和路由，代码只注册业务。
 *
 * 用法：example_04_config_server [server.json]
 */

#define NB_LOG_TAG "example"
#include <nb/config.h>
#include <nb/node.h>
#include <nb/service.h>

#include <nb/log.h>

static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    /* payload 对框架透明：原样回显。 */
    nb_reply(call, 0, &req->payload);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "server.json";
    nb_config_doc_t *cfg = NULL;
    nb_config_error_t err;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    int rc;

    rc = nb_config_parse_file(path, &cfg, &err);
    if (rc != NB_OK) {
        nb_log_error(
            "load config failed: %s (%d) path=%s reason=%s", path, rc, err.path, err.reason);
        return 1;
    }
    rc = nb_node_create_from_config(cfg, &node);
    nb_config_doc_destroy(cfg);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "EchoService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(svc, "echo", echo_handler, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("server setup failed: %d (%s)", rc, nb_error_name(rc));
        nb_node_destroy(node);
        return 1;
    }

    nb_log_info("EchoService.echo ready (config=%s, Ctrl+C to stop)", path);
    (void)nb_node_run(node);
    nb_node_destroy(node);
    return 0;
}
