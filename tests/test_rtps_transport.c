/*
 * RTPS transport provider: request/response and event delivery between two
 * provider instances, using explicit unicast discovery so the test does not
 * depend on multicast.
 */
#include <nb/log.h>
#include <nb/message.h>
#include <nb/rtps.h>
#include <nb/transport_provider.h>
#include <nb/nb.h>

#include <ntk.h>

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

typedef struct peer_ctx {
    const char *name;
    const nb_transport_provider_t *provider;
    nb_transport_instance_t *instance;
    ntk_mutex *lock;
    int requests;
    int responses;
    int events;
    int status_events;
    int last_status_kind;
    char last_status_endpoint[NB_STATUS_ENDPOINT_MAX];
    int last_status_has_peer;
    char last_payload[64];
    size_t last_payload_len;
    uint32_t last_request_id;
} peer_ctx_t;

static void peer_record(peer_ctx_t *ctx,
                        const nb_transport_message_t *message,
                        int *counter)
{
    size_t len = message->payload.len;

    ntk_mutex_lock(ctx->lock);
    (*counter)++;
    if (len > sizeof(ctx->last_payload)) {
        len = sizeof(ctx->last_payload);
    }
    if (message->payload.data && len > 0) {
        memcpy(ctx->last_payload, message->payload.data, len);
    }
    ctx->last_payload_len = len;
    ctx->last_request_id = message->request_id;
    ntk_mutex_unlock(ctx->lock);
}

static void on_receive(void *user, void *reply_token, const nb_transport_message_t *message)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;
    static const char pong[] = "pong";
    nb_transport_message_t response;

    if (!ctx || !message || !message->payload.data) {
        return;
    }
    if (message->kind == NB_MSG_KIND_REQUEST) {
        peer_record(ctx, message, &ctx->requests);
        memset(&response, 0, sizeof(response));
        response.kind = NB_MSG_KIND_RESPONSE;
        response.route = message->route;
        response.request_id = message->request_id;
        response.status = 7;
        response.payload.data = pong;
        response.payload.len = sizeof(pong) - 1u;
        response.payload.encoding = message->payload.encoding;
        (void)ctx->provider->ops->rpc->reply(
            ctx->instance, reply_token, message->peer, &response);
    } else if (message->kind == NB_MSG_KIND_RESPONSE) {
        peer_record(ctx, message, &ctx->responses);
    } else if (message->kind == NB_MSG_KIND_EVENT) {
        peer_record(ctx, message, &ctx->events);
    }
}

static int peer_count(peer_ctx_t *ctx, int *counter)
{
    int value;

    ntk_mutex_lock(ctx->lock);
    value = *counter;
    ntk_mutex_unlock(ctx->lock);
    return value;
}

static void on_status(void *user, const nb_status_event_t *event)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;

    if (!ctx || !event) {
        return;
    }
    ntk_mutex_lock(ctx->lock);
    ctx->status_events++;
    ctx->last_status_kind = (int)event->kind;
    (void)snprintf(ctx->last_status_endpoint,
                   sizeof(ctx->last_status_endpoint),
                   "%s",
                   event->endpoint);
    ctx->last_status_has_peer = event->has_peer ? 1 : 0;
    ntk_mutex_unlock(ctx->lock);
}

static int status_events(peer_ctx_t *ctx)
{
    int value;

    ntk_mutex_lock(ctx->lock);
    value = ctx->status_events;
    ntk_mutex_unlock(ctx->lock);
    return value;
}

static nb_transport_instance_t *make_instance(const nb_transport_provider_t *provider,
                                              const char *endpoint,
                                              peer_ctx_t *ctx)
{
    nb_transport_provider_options_t options;
    nb_ps_backend_options_t ps;
    nb_transport_instance_t *instance = NULL;

    memset(&options, 0, sizeof(options));
    memset(&ps, 0, sizeof(ps));
    ps.struct_size = sizeof(ps);
    ps.on_status = on_status;
    options.scheme = "rtps";
    options.endpoint = endpoint;
    options.user = ctx;
    options.on_receive = on_receive;
    options.ps = &ps;
    options.intra_process = NB_INTRA_PROCESS_OFF;
    options.struct_size = sizeof(options);
    ctx->provider = provider;
    if (provider->ops->create(&options, &instance) != NB_OK) {
        return NULL;
    }
    ctx->instance = instance;
    return instance;
}

static void test_request_response_and_event(void)
{
    const nb_transport_provider_t *provider = nb_rtps_transport_provider();
    nb_transport_instance_t *a = NULL;
    nb_transport_instance_t *b = NULL;
    peer_ctx_t ctx_a;
    peer_ctx_t ctx_b;
    nb_transport_message_t message;
    static const char ping[] = "ping";
    static const char tick[] = "tick-1";
    int i;

    memset(&ctx_a, 0, sizeof(ctx_a));
    memset(&ctx_b, 0, sizeof(ctx_b));
    ctx_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    ctx_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    ctx_a.name = "a";
    ctx_b.name = "b";
    CHECK(ctx_a.lock != NULL && ctx_b.lock != NULL);
    if (!ctx_a.lock || !ctx_b.lock) {
        return;
    }

    CHECK(provider != NULL);
    CHECK(nb_rtps_transport_register() == NB_OK);
    CHECK(nb_rtps_transport_register() == NB_OK); /* idempotent */
    if (!provider) {
        goto cleanup;
    }

    a = make_instance(provider, "rtps://7:47130/127.0.0.1:47131", &ctx_a);
    b = make_instance(provider, "rtps://7:47131/127.0.0.1:47130", &ctx_b);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        goto cleanup;
    }
    CHECK(provider->ops->start(a) == NB_OK);
    CHECK(provider->ops->start(b) == NB_OK);
    CHECK(provider->ops->ps->register_method(a, "svc", "echo") == NB_OK);
    CHECK(provider->ops->ps->register_method(b, "svc", "tick") == NB_OK);

    /* Request/response: b asks, a answers through the reply token. */
    memset(&message, 0, sizeof(message));
    message.kind = NB_MSG_KIND_REQUEST;
    message.route = "svc.echo";
    message.request_id = 42;
    message.payload.data = ping;
    message.payload.len = sizeof(ping) - 1u;
    for (i = 0; i < 120 && peer_count(&ctx_b, &ctx_b.responses) == 0; i++) {
        (void)provider->ops->rpc->send(b, NULL, &message);
        ntk_sleep_ms(100);
    }
    CHECK(peer_count(&ctx_a, &ctx_a.requests) > 0);
    CHECK(peer_count(&ctx_b, &ctx_b.responses) > 0);
    ntk_mutex_lock(ctx_b.lock);
    CHECK(ctx_b.last_payload_len == 4 && memcmp(ctx_b.last_payload, "pong", 4) == 0);
    CHECK(ctx_b.last_request_id == 42);
    ntk_mutex_unlock(ctx_b.lock);

    /* Event: a publishes one-way, b's event reader receives it. */
    memset(&message, 0, sizeof(message));
    message.kind = NB_MSG_KIND_EVENT;
    message.route = "svc.tick";
    message.payload.data = tick;
    message.payload.len = sizeof(tick) - 1u;
    for (i = 0; i < 120 && peer_count(&ctx_b, &ctx_b.events) == 0; i++) {
        (void)provider->ops->rpc->send(a, NULL, &message);
        ntk_sleep_ms(100);
    }
    CHECK(peer_count(&ctx_b, &ctx_b.events) > 0);
    ntk_mutex_lock(ctx_b.lock);
    CHECK(ctx_b.last_payload_len == 6 && memcmp(ctx_b.last_payload, "tick-1", 6) == 0);
    ntk_mutex_unlock(ctx_b.lock);

cleanup:
    if (a) {
        provider->ops->stop(a);
        provider->ops->destroy(a);
    }
    if (b) {
        provider->ops->stop(b);
        provider->ops->destroy(b);
    }
    if (ctx_a.lock) {
        ntk_mutex_destroy(ctx_a.lock);
    }
    if (ctx_b.lock) {
        ntk_mutex_destroy(ctx_b.lock);
    }
}

/*
 * The tcp query parameter keeps the UDP discovery layout - port, peers - and
 * adds an RTPS-over-TCP listener. The binding must still accept traffic in both
 * directions, so the same request/response exchange has to work through it.
 */
static void test_tcp_query_binding(void)
{
    const nb_transport_provider_t *provider = nb_rtps_transport_provider();
    nb_transport_instance_t *a = NULL;
    nb_transport_instance_t *b = NULL;
    peer_ctx_t ctx_a;
    peer_ctx_t ctx_b;
    nb_transport_message_t message;
    static const char ping[] = "ping";
    int i;

    memset(&ctx_a, 0, sizeof(ctx_a));
    memset(&ctx_b, 0, sizeof(ctx_b));
    ctx_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    ctx_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    ctx_a.name = "tcp-a";
    ctx_b.name = "tcp-b";
    CHECK(ctx_a.lock != NULL && ctx_b.lock != NULL);
    if (!ctx_a.lock || !ctx_b.lock) {
        return;
    }
    CHECK(provider != NULL);
    if (!provider) {
        goto cleanup;
    }

    a = make_instance(provider,
                      "rtps://11:47200/127.0.0.1:47201?tcp",
                      &ctx_a);
    b = make_instance(provider,
                      "rtps://11:47201/127.0.0.1:47200?tcp=47301",
                      &ctx_b);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        goto cleanup;
    }
    CHECK(provider->ops->start(a) == NB_OK);
    CHECK(provider->ops->start(b) == NB_OK);
    CHECK(provider->ops->ps->register_method(a, "svc", "echo") == NB_OK);
    CHECK(provider->ops->ps->register_method(b, "svc", "echo") == NB_OK);

    memset(&message, 0, sizeof(message));
    message.kind = NB_MSG_KIND_REQUEST;
    message.route = "svc.echo";
    message.request_id = 7;
    message.payload.data = ping;
    message.payload.len = sizeof(ping) - 1u;
    for (i = 0; i < 120 && peer_count(&ctx_b, &ctx_b.responses) == 0; i++) {
        (void)provider->ops->rpc->send(b, NULL, &message);
        ntk_sleep_ms(100);
    }
    CHECK(peer_count(&ctx_a, &ctx_a.requests) > 0);
    CHECK(peer_count(&ctx_b, &ctx_b.responses) > 0);

cleanup:
    if (a) {
        provider->ops->stop(a);
        provider->ops->destroy(a);
    }
    if (b) {
        provider->ops->stop(b);
        provider->ops->destroy(b);
    }
    if (ctx_a.lock) {
        ntk_mutex_destroy(ctx_a.lock);
    }
    if (ctx_b.lock) {
        ntk_mutex_destroy(ctx_b.lock);
    }
}

/*
 * Status callbacks: two instances registering the same method publish and
 * subscribe to the same response topic, so each side matches a peer. The
 * provider reports that through options.ps->on_status, naming the endpoint by the
 * method it belongs to.
 */
static void test_status_events(void)
{
    const nb_transport_provider_t *provider = nb_rtps_transport_provider();
    nb_transport_instance_t *a = NULL;
    nb_transport_instance_t *b = NULL;
    peer_ctx_t ctx_a;
    peer_ctx_t ctx_b;
    nb_transport_message_t message;
    static const char ping[] = "ping";
    int i;

    memset(&ctx_a, 0, sizeof(ctx_a));
    memset(&ctx_b, 0, sizeof(ctx_b));
    ctx_a.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    ctx_b.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    ctx_a.name = "status-a";
    ctx_b.name = "status-b";
    CHECK(ctx_a.lock != NULL && ctx_b.lock != NULL);
    if (!ctx_a.lock || !ctx_b.lock) {
        goto cleanup;
    }
    CHECK(provider != NULL);
    if (!provider) {
        goto cleanup;
    }

    a = make_instance(provider, "rtps://8:47140/127.0.0.1:47141", &ctx_a);
    b = make_instance(provider, "rtps://8:47141/127.0.0.1:47140", &ctx_b);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        goto cleanup;
    }
    CHECK(provider->ops->start(a) == NB_OK);
    CHECK(provider->ops->start(b) == NB_OK);
    CHECK(provider->ops->ps->register_method(a, "svc", "echo") == NB_OK);

    /* Sending a request turns b into the requester for this method, which gives
     * it the request writer and response reader that pair with a's endpoints. */
    memset(&message, 0, sizeof(message));
    message.kind = NB_MSG_KIND_REQUEST;
    message.route = "svc.echo";
    message.request_id = 1;
    message.payload.data = ping;
    message.payload.len = sizeof(ping) - 1u;
    for (i = 0; i < 150 && (status_events(&ctx_a) == 0 || status_events(&ctx_b) == 0); i++) {
        (void)provider->ops->rpc->send(b, NULL, &message);
        ntk_sleep_ms(100);
    }
    CHECK(status_events(&ctx_a) > 0);
    CHECK(status_events(&ctx_b) > 0);
    ntk_mutex_lock(ctx_a.lock);
    CHECK(ctx_a.last_status_kind == NB_STATUS_PUBLICATION_MATCHED ||
          ctx_a.last_status_kind == NB_STATUS_SUBSCRIPTION_MATCHED);
    CHECK(strcmp(ctx_a.last_status_endpoint, "svc_echo") == 0);
    CHECK(ctx_a.last_status_has_peer);
    ntk_mutex_unlock(ctx_a.lock);
    ntk_mutex_lock(ctx_b.lock);
    CHECK(ctx_b.last_status_kind == NB_STATUS_PUBLICATION_MATCHED ||
          ctx_b.last_status_kind == NB_STATUS_SUBSCRIPTION_MATCHED);
    CHECK(strcmp(ctx_b.last_status_endpoint, "svc_echo") == 0);
    CHECK(ctx_b.last_status_has_peer);
    ntk_mutex_unlock(ctx_b.lock);

cleanup:
    if (a) {
        provider->ops->stop(a);
        provider->ops->destroy(a);
    }
    if (b) {
        provider->ops->stop(b);
        provider->ops->destroy(b);
    }
    if (ctx_a.lock) {
        ntk_mutex_destroy(ctx_a.lock);
    }
    if (ctx_b.lock) {
        ntk_mutex_destroy(ctx_b.lock);
    }
}

/* The discovery view is reached from a node: the core asks every transport
 * that implements the optional provider op, and the RTPS provider answers
 * from its participant tables. */
static void test_node_discovery_view(void)
{
    nb_node_config_t cfg;
    nb_binding_config_t binding;
    nb_node_t *node = NULL;
    nb_discovery_sample_t views[8];
    size_t n;

    (void)nb_rtps_transport_register();
    memset(&binding, 0, sizeof(binding));
    binding.name = "rtps";
    binding.endpoint = "rtps://5:47190";
    binding.intra_process = NB_INTRA_PROCESS_OFF;
    nb_node_config_default(&cfg);
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.bindings = &binding;
    cfg.binding_count = 1;
    CHECK(nb_node_create(&cfg, &node) == NB_OK);
    if (!node) {
        return;
    }

    /* The local participant is visible; nothing has been published yet. */
    n = nb_node_discovery_read(node, NB_BUILTIN_TOPIC_PARTICIPANT, views, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(views[0].is_local);
        CHECK(views[0].topic == NB_BUILTIN_TOPIC_PARTICIPANT);
    }
    CHECK(nb_node_discovery_read(node, NB_BUILTIN_TOPIC_PUBLICATION, views, 8) == 0);
    CHECK(nb_node_discovery_read(node, NB_BUILTIN_TOPIC_TOPIC, views, 8) == 0);
    CHECK(nb_node_discovery_read(node, (nb_builtin_topic_t)7, views, 8) == 0);
    CHECK(nb_node_discovery_read(node, NB_BUILTIN_TOPIC_PARTICIPANT, views, 0) == 0);

    nb_node_destroy(node);
}

/* nb_node_metrics must pick up the provider counters through the optional op. */
static void test_node_metrics_include_rtps(void)
{
    nb_node_config_t cfg;
    nb_binding_config_t binding;
    nb_node_t *node = NULL;
    nb_metrics_t metrics;

    (void)nb_rtps_transport_register();
    memset(&binding, 0, sizeof(binding));
    binding.name = "rtps";
    binding.endpoint = "rtps://6:47192";
    binding.intra_process = NB_INTRA_PROCESS_OFF;
    nb_node_config_default(&cfg);
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.bindings = &binding;
    cfg.binding_count = 1;
    CHECK(nb_node_create(&cfg, &node) == NB_OK);
    if (!node) {
        return;
    }
    ntk_sleep_ms(100);
    memset(&metrics, 0, sizeof(metrics));
    nb_node_metrics(node, &metrics);
    /* The local participant is announced on its own schedule; the gauge is
     * immediate even when no datagram has been sent yet. */
    CHECK(metrics.rtps_participants == 1);
    nb_node_destroy(node);
}

int main(void)
{
    test_request_response_and_event();
    test_tcp_query_binding();
    test_status_events();
    test_node_discovery_view();
    test_node_metrics_include_rtps();
    if (g_failures == 0) {
        nb_log_info("rtps transport tests passed (%d checks)", g_checks);
        return 0;
    }
    nb_log_error("rtps transport tests failed: %d of %d checks", g_failures, g_checks);
    return 1;
}
