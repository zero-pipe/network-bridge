/* Contract tests for the reviewed API surface: ownership, length checks and
 * validation that used to fail silently. Pure unit level except for one
 * loopback UDP binding used to exercise node accessors. */
#include <nb/nb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

static int dummy_sync(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)req;
    (void)resp;
    (void)user;
    return 0;
}

static void test_metadata(void)
{
    nb_metadata_t md;
    const unsigned char binary[] = { 'a', 0, 'b' };
    size_t len = 0;
    const char *text;
    nb_metadata_t big;
    unsigned char value[NB_METADATA_VALUE_MAX];
    unsigned char wire[8192];
    char key[NB_METADATA_KEY_MAX];
    size_t wire_len = 0;
    int i;
    int all_ok = 1;

    nb_metadata_init(&md);
    CHECK(nb_metadata_set(&md, "bin", binary, sizeof binary) == NB_OK);
    /* Binary values without a trailing NUL must never look like strings. */
    CHECK(nb_metadata_get_string(&md, "bin") == NULL);
    CHECK(nb_metadata_get(&md, "bin", &len) != NULL && len == sizeof binary);
    CHECK(nb_metadata_set_string(&md, "txt", "hello") == NB_OK);
    text = nb_metadata_get_string(&md, "txt");
    CHECK(text != NULL && strcmp(text, "hello") == 0);
    CHECK(nb_metadata_get_string(&md, "missing") == NULL);

    /* 16 entries of (4 + 63 + 256) bytes exceed NB_METADATA_WIRE_MAX (4096)
     * even when the caller's buffer is large enough. */
    nb_metadata_init(&big);
    memset(value, 'x', sizeof value);
    memset(key, 'k', sizeof key - 1);
    key[sizeof key - 1] = '\0';
    for (i = 0; i < NB_METADATA_MAX_ENTRIES; i++) {
        key[0] = (char)('a' + i);
        if (nb_metadata_set(&big, key, value, sizeof value) != NB_OK) {
            all_ok = 0;
        }
    }
    CHECK(all_ok);
    CHECK(nb_metadata_encode(&big, wire, sizeof wire, &wire_len) == NB_ERR_TOOBIG);
    CHECK(wire_len == 0);
}

static void test_endpoint(void)
{
    nb_endpoint_t ep;
    nb_endpoint_t with_port;
    char small[4];

    CHECK(nb_endpoint_parse("dds://0", &ep) == NB_OK);
    CHECK(strcmp(ep.scheme, "dds") == 0 && strcmp(ep.host, "0") == 0);
    CHECK(!ep.has_port && ep.port == 0);
    CHECK(strcmp(ep.uri, "dds://0") == 0);
    CHECK(!nb_endpoint_is_network(&ep));

    /* The legacy explicit-port form still parses and stays distinct. */
    CHECK(nb_endpoint_parse("dds://0:1", &with_port) == NB_OK);
    CHECK(with_port.has_port && with_port.port == 1);
    CHECK(!nb_endpoint_equal(&ep, &with_port));

    CHECK(nb_endpoint_parse("tcp://127.0.0.1", &ep) == NB_ERR_INVALID);
    CHECK(nb_endpoint_parse("udp://[::1]", &ep) == NB_ERR_INVALID);
    CHECK(nb_endpoint_parse("tcp://127.0.0.1:0", &ep) == NB_OK);
    CHECK(ep.has_port && nb_endpoint_is_network(&ep));
    CHECK(nb_endpoint_parse("dds://", &ep) == NB_ERR_INVALID);
    CHECK(nb_endpoint_parse("no-scheme", &ep) == NB_ERR_INVALID);

    /* scheme is authoritative: dds must not be labelled UDP. */
    CHECK(nb_endpoint_from_parts(&ep, "dds", "0", 0, NB_TRANSPORT_KIND_CUSTOM) == NB_OK);
    CHECK(!ep.has_port && strcmp(ep.uri, "dds://0") == 0);
    CHECK(nb_endpoint_format(&ep, small, sizeof small) == NB_ERR_TOOBIG);
    CHECK(nb_endpoint_from_parts(&ep, "dds", "0", 0, NB_TRANSPORT_KIND_UDP) == NB_ERR_INVALID);
    CHECK(nb_endpoint_from_parts(&ep, "tcp", "127.0.0.1", 0, NB_TRANSPORT_KIND_TCP) == NB_OK);
    CHECK(ep.has_port && ep.port == 0);
}

static void test_config_validate(void)
{
    nb_node_config_t cfg;
    char long_name[NB_NAME_MAX + 8];

    nb_node_config_default(&cfg);
    CHECK(nb_node_config_validate(&cfg) == NB_OK);

    cfg.runtime.io_threads = NB_MAX_IO_THREADS + 1;
    CHECK(nb_node_config_validate(&cfg) == NB_ERR_INVALID);
    cfg.runtime.io_threads = 1;

    memset(long_name, 'n', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    cfg.name = long_name;
    CHECK(nb_node_config_validate(&cfg) == NB_ERR_TOOBIG);
}

static void test_node_and_service(void)
{
    nb_node_config_t cfg;
    nb_binding_config_t binding;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    nb_method_desc_t methods[2];
    char uri[96];
    char short_uri[4];
    char long_name[NB_SERVICE_NAME_MAX + 8];
    char long_method[NB_METHOD_NAME_MAX + 8];
    int rc;

    nb_node_config_default(&cfg);
    cfg.name = "contracts";
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    memset(&binding, 0, sizeof binding);
    binding.name = "udp";
    binding.endpoint = "udp://127.0.0.1:0";
    cfg.bindings = &binding;
    cfg.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    CHECK(rc == NB_OK && node != NULL);
    if (!node) {
        return;
    }

    CHECK(nb_node_binding_endpoint(node, 0, short_uri, sizeof short_uri) == NB_ERR_TOOBIG);
    CHECK(nb_node_binding_endpoint(node, 0, uri, sizeof uri) == NB_OK);
    CHECK(strncmp(uri, "udp://", 6) == 0);
    CHECK(nb_node_binding_endpoint(node, 3, uri, sizeof uri) == NB_ERR_NOTFOUND);

    memset(long_name, 's', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    CHECK(nb_service_create(node, long_name, &svc) == NB_ERR_TOOBIG);
    CHECK(svc == NULL);
    /* A dot would make "service.method" ambiguous on the wire. */
    CHECK(nb_service_create(node, "bad.name", &svc) == NB_ERR_INVALID);
    CHECK(nb_service_create(node, "ContractService", &svc) == NB_OK);

    memset(long_method, 'm', sizeof long_method - 1);
    long_method[sizeof long_method - 1] = '\0';
    CHECK(nb_service_add_sync_method(svc, long_method, dummy_sync, NULL) == NB_ERR_TOOBIG);
    CHECK(nb_service_add_sync_method(svc, "get", dummy_sync, NULL) == NB_OK);

    memset(methods, 0, sizeof methods);
    methods[0].name = "a";
    methods[0].kind = NB_METHOD_KIND_SYNC;
    methods[0].handler.sync = dummy_sync;
    methods[1].name = "b";
    methods[1].kind = NB_METHOD_KIND_SYNC;
    methods[1].handler.sync = dummy_sync;
    CHECK(nb_service_add_methods(svc, methods, 2) == NB_OK);

    nb_service_destroy(svc);
    nb_node_destroy(node);
}

static void test_route_failure(void)
{
    nb_node_config_t cfg;
    nb_static_route_t route;
    nb_node_t *node = NULL;

    nb_node_config_default(&cfg);
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    memset(&route, 0, sizeof route);
    snprintf(route.service, sizeof route.service, "%s", "Svc");
    snprintf(route.endpoint, sizeof route.endpoint, "%s", "not-a-uri");
    cfg.rpc.routes = &route;
    cfg.rpc.route_count = 1;

    CHECK(nb_node_create(&cfg, &node) == NB_ERR_INVALID);
    CHECK(node == NULL);

    snprintf(route.endpoint, sizeof route.endpoint, "%s", "tcp://127.0.0.1:1");
    route.service[0] = '\0'; /* empty service must be rejected too */
    CHECK(nb_node_create(&cfg, &node) == NB_ERR_INVALID);
    CHECK(node == NULL);
}

static void test_response_and_view(void)
{
    nb_response_t resp;
    static unsigned char local[4] = { 1, 2, 3, 4 };
    unsigned char *heap;
    nb_message_view_t view;

    memset(&resp, 0, sizeof resp);
    resp.payload.data = local;
    resp.payload.len = sizeof local;
    resp.owns_payload = false; /* callback-scoped: release must not free it */
    nb_response_release(&resp);
    CHECK(local[0] == 1 && resp.payload.data == NULL);

    heap = (unsigned char *)malloc(8);
    CHECK(heap != NULL);
    if (heap) {
        resp.payload.data = heap;
        resp.payload.len = 8;
        resp.owns_payload = true;
        nb_response_release(&resp);
        CHECK(resp.payload.data == NULL);
    }

    nb_message_view_init(&view);
    CHECK(nb_message_view_set_string(&view, "abc") == NB_OK);
    CHECK(view.owner != NULL && view.len == 3);
    nb_message_view_to_payload(&view, &resp.payload);
    CHECK(resp.payload.len == 3);
    nb_message_view_release(&view);
    CHECK(view.owner == NULL && view.data == NULL);
    CHECK(nb_message_view_set_borrowed(&view, "xyz", 3) == NB_OK);
    nb_message_view_release(&view);
    CHECK(view.data == NULL);
}

int main(void)
{
    int (*reply_fn)(nb_call_t *, int, const nb_payload_t *) = nb_reply;

    (void)reply_fn;

    test_metadata();
    test_endpoint();
    test_config_validate();
    test_node_and_service();
    test_route_failure();
    test_response_and_view();

    if (g_failures) {
        printf("test_contracts: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_contracts: ok\n");
    return 0;
}
