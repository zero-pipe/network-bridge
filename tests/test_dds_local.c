/* DDS intra-process event delivery.
 *
 * Regression: the local fast path compared the sanitized topic key against the
 * raw "Service.method" endpoint key, so same-process events were never
 * delivered once topics became service-scoped. */
#include <nb/nb.h>

#include <stdio.h>
#include <string.h>

static int g_hits;
static char g_text[64];

static void on_event(nb_call_t *call, const nb_request_t *req, void *user)
{
    size_t n = req->payload.len;

    (void)call;
    (void)user;
    if (n >= sizeof g_text) {
        n = sizeof g_text - 1;
    }
    if (req->payload.data) {
        memcpy(g_text, req->payload.data, n);
    }
    g_text[n] = '\0';
    g_hits++;
}

int main(void)
{
    nb_binding_config_t binding;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t payload;
    int rc;
    int i;

    if (nb_cyclone_dds_register() != NB_OK) {
        printf("test_dds_local: SKIP (provider unavailable)\n");
        return 0;
    }

    memset(&binding, 0, sizeof binding);
    binding.name = "dds";
    binding.endpoint = "dds://0";
    binding.intra_process = NB_INTRA_PROCESS_ONLY;

    nb_node_config_default(&cfg);
    cfg.name = "DdsLocal";
    cfg.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "LocalService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(svc, "ping", on_event, NULL);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_target_uri(proxy, "dds://0");
    }
    if (rc != NB_OK) {
        printf("test_dds_local: setup failed rc=%d\n", rc);
        nb_proxy_destroy(proxy);
        nb_service_destroy(svc);
        nb_node_destroy(node);
        return 1;
    }

    payload.data = "payload";
    payload.len = 7;
    payload.encoding = 0;
    rc = nb_proxy_call_event(proxy, "LocalService", "ping", &payload, NULL);
    for (i = 0; rc == NB_OK && i < 50 && g_hits == 0; i++) {
        (void)nb_node_run_once(node, 20);
    }
    if (rc != NB_OK || g_hits == 0 || strcmp(g_text, "payload") != 0) {
        printf("test_dds_local: FAIL rc=%d hits=%d text=%s\n", rc, g_hits, g_text);
        rc = 1;
    } else {
        printf("test_dds_local: ok\n");
        rc = 0;
    }

    nb_proxy_destroy(proxy);
    nb_service_destroy(svc);
    nb_node_destroy(node);
    return rc;
}
