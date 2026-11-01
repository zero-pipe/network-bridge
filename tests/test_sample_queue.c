/*
 * DDS read/take (P2.4): reader history, KEEP_LAST, state masks, instance
 * filtering, borrowed read vs owned take, and the service-level wiring that
 * turns an EVENT delivery into queued samples.
 */
#include <nb/nb.h>

#include "sample_queue.h"

#include <platform.h>
#include <thread/thread.h>

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            fails++;                                                                         \
        }                                                                                    \
    } while (0)

static void init_msg(nb_message_t *msg, const char *text)
{
    memset(msg, 0, sizeof(*msg));
    msg->kind = NB_MSG_KIND_EVENT;
    msg->payload.data = text;
    msg->payload.len = strlen(text);
}

static int payload_is(const nb_sample_t *sample, const char *text)
{
    size_t len = strlen(text);
    return sample->data.len == len && sample->data.data &&
           memcmp(sample->data.data, text, len) == 0;
}

static void test_keep_last_and_read_take(void)
{
    nb_sample_queue_t *queue = nb_sample_queue_create(3);
    nb_sample_filter_t read_filter = {NB_STATE_SAMPLE_READ, NULL};
    nb_sample_filter_t not_read = {NB_STATE_SAMPLE_NOT_READ, NULL};
    nb_message_t msg;
    nb_sample_t out[8];
    size_t n;

    CHECK(queue != NULL);
    if (!queue) {
        return;
    }
    init_msg(&msg, "0");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "1");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "2");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    CHECK(nb_sample_queue_size(queue) == 3);

    /* KEEP_LAST: the two oldest samples are dropped. */
    init_msg(&msg, "3");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "4");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    CHECK(nb_sample_queue_size(queue) == 3);
    CHECK(nb_sample_queue_dropped(queue) == 2);

    n = nb_sample_queue_read(queue, NULL, out, 8);
    CHECK(n == 3);
    CHECK(payload_is(&out[0], "2"));
    CHECK(payload_is(&out[1], "3"));
    CHECK(payload_is(&out[2], "4"));
    CHECK(out[0].info.state == NB_SAMPLE_STATE_VALID);
    CHECK(nb_sample_info_valid_data(&out[0].info));

    /* read() marks samples as READ but keeps them. */
    CHECK(nb_sample_queue_read(queue, &not_read, out, 8) == 0);
    CHECK(nb_sample_queue_read(queue, &read_filter, out, 8) == 3);

    /* take() removes them and hands back owned copies. */
    n = nb_sample_queue_take(queue, &read_filter, out, 8);
    CHECK(n == 3);
    CHECK(payload_is(&out[0], "2"));
    CHECK(nb_sample_queue_size(queue) == 0);
    for (n = 0; n < 3; n++) {
        nb_sample_release(&out[n]);
    }

    /* Depth changes are honoured on the next push. */
    nb_sample_queue_set_depth(queue, 2);
    init_msg(&msg, "a");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "b");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "c");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    CHECK(nb_sample_queue_size(queue) == 2);
    CHECK(nb_sample_queue_take(queue, NULL, out, 8) == 2);
    CHECK(payload_is(&out[0], "b"));
    CHECK(payload_is(&out[1], "c"));
    nb_sample_release(&out[0]);
    nb_sample_release(&out[1]);

    nb_sample_queue_destroy(queue);
}

static void test_state_masks_and_instances(void)
{
    nb_sample_queue_t *queue = nb_sample_queue_create(8);
    nb_sample_filter_t disposed = {NB_STATE_INSTANCE_DISPOSED, NULL};
    nb_sample_filter_t no_writers = {NB_STATE_INSTANCE_NO_WRITERS, NULL};
    nb_sample_filter_t new_not_read = {NB_STATE_VIEW_NEW | NB_STATE_SAMPLE_NOT_READ, NULL};
    nb_sample_filter_t instance_a = {NB_STATE_ANY, NULL};
    uint8_t handle_a[NB_INSTANCE_HANDLE_SIZE];
    uint8_t handle_b[NB_INSTANCE_HANDLE_SIZE];
    nb_message_t msg;
    nb_sample_t out[8];
    size_t n;

    CHECK(queue != NULL);
    if (!queue) {
        return;
    }
    memset(handle_a, 0x11, sizeof(handle_a));
    memset(handle_b, 0x22, sizeof(handle_b));
    instance_a.instance = handle_a;

    init_msg(&msg, "A");
    msg.sample.has_instance_handle = true;
    memcpy(msg.sample.instance_handle, handle_a, sizeof(handle_a));
    msg.sample.view_state = NB_VIEW_STATE_NEW;
    msg.sample.instance_state = NB_INSTANCE_STATE_ALIVE;
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);

    init_msg(&msg, "B");
    msg.sample.state = NB_SAMPLE_STATE_DISPOSED;
    msg.sample.has_instance_handle = true;
    memcpy(msg.sample.instance_handle, handle_b, sizeof(handle_b));
    msg.sample.view_state = NB_VIEW_STATE_NEW;
    msg.sample.instance_state = NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED;
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);

    init_msg(&msg, "C");
    msg.sample.state = NB_SAMPLE_STATE_NO_WRITERS;
    msg.sample.has_instance_handle = true;
    memcpy(msg.sample.instance_handle, handle_b, sizeof(handle_b));
    msg.sample.view_state = NB_VIEW_STATE_NOT_NEW;
    msg.sample.instance_state = NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS;
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);

    n = nb_sample_queue_read(queue, &new_not_read, out, 8);
    CHECK(n == 2);

    n = nb_sample_queue_take(queue, &disposed, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "B"));
    CHECK(!nb_sample_info_valid_data(&out[0].info));
    nb_sample_release(&out[0]);

    n = nb_sample_queue_take(queue, &no_writers, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "C"));
    CHECK(out[0].info.instance_state == NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS);
    nb_sample_release(&out[0]);

    n = nb_sample_queue_take(queue, &instance_a, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "A"));
    nb_sample_release(&out[0]);

    CHECK(nb_sample_queue_take(queue, NULL, out, 8) == 0);
    nb_sample_queue_destroy(queue);
}

static int wait_for_samples(nb_node_t *node, nb_subscriber_t *sub, const char *topic, size_t want)
{
    int i;
    for (i = 0; i < 400; i++) {
        if (nb_subscriber_queue_size(sub, topic) >= want) {
            return 0;
        }
        (void)nb_node_run_once(node, 5);
    }
    return -1;
}

typedef struct producer {
    nb_proxy_t *proxy;
    int rc;
} producer_t;

static void producer_fn(void *user)
{
    producer_t *producer = (producer_t *)user;
    nb_payload_t pl;

    pl.data = "three";
    pl.len = 5;
    pl.encoding = 0;
    ntk_sleep_ms(150);
    producer->rc = nb_proxy_call_event(producer->proxy, "srv", "data", &pl, NULL);
}

static void test_service_wiring(void)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_subscriber_t *sub = NULL;
    nb_proxy_t *proxy = NULL;
    nb_sample_filter_t not_read = {NB_STATE_SAMPLE_NOT_READ, NULL};
    nb_sample_filter_t read_filter = {NB_STATE_SAMPLE_READ, NULL};
    nb_sample_t samples[8];
    nb_payload_t pl;
    int i;
    int rc;
    size_t n;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "qsrv";
    scfg.transport.kind = NB_TRANSPORT_KIND_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.transport.data_port = 0;
    scfg.discovery.broadcast_host = "127.0.0.1";
    scfg.discovery.port = 42511;
    scfg.runtime.io_threads = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "qcli";
    ccfg.transport.kind = NB_TRANSPORT_KIND_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.transport.data_port = 0;
    ccfg.discovery.broadcast_host = "127.0.0.1";
    ccfg.discovery.port = 42512;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    rc = nb_node_create(&scfg, &server);
    CHECK(rc == NB_OK);
    if (rc != NB_OK) {
        return;
    }
    rc = nb_node_create(&ccfg, &client);
    CHECK(rc == NB_OK);
    if (rc != NB_OK) {
        nb_node_destroy(server);
        return;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", 42512);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", 42511);

    rc = nb_subscriber_create(server, "srv", &sub);
    CHECK(rc == NB_OK);
    if (rc == NB_OK) {
        rc = nb_subscriber_subscribe_queue(sub, "data", 8);
        CHECK(rc == NB_OK);
    }
    rc = nb_proxy_create(client, NULL, &proxy);
    CHECK(rc == NB_OK);
    if (!sub || !proxy) {
        goto cleanup;
    }

    /* Wait until discovery has routed at least one event to the reader. */
    pl.data = "hello";
    pl.len = 6;
    pl.encoding = 0;
    for (i = 0; i < 200 && nb_subscriber_queue_size(sub, "data") == 0; i++) {
        (void)nb_proxy_call_event(proxy, "srv", "data", &pl, NULL);
        (void)nb_node_run_once(server, 10);
    }
    CHECK(nb_subscriber_queue_size(sub, "data") > 0);
    if (nb_subscriber_queue_size(sub, "data") == 0) {
        goto cleanup;
    }

    /* Start from an empty history. */
    while ((n = nb_subscriber_take(sub, "data", NULL, samples, 8)) > 0) {
        size_t k;
        for (k = 0; k < n; k++) {
            nb_sample_release(&samples[k]);
        }
    }

    pl.data = "one";
    pl.len = 3;
    rc = nb_proxy_call_event(proxy, "srv", "data", &pl, NULL);
    CHECK(rc == NB_OK);
    CHECK(wait_for_samples(server, sub, "data", 1) == 0);

    n = nb_subscriber_read(sub, "data", &not_read, samples, 8);
    CHECK(n == 1);
    CHECK(n == 1 && payload_is(&samples[0], "one"));
    CHECK(n == 1 && nb_sample_info_valid_data(&samples[0].info));
    CHECK(nb_subscriber_read(sub, "data", &not_read, samples, 8) == 0);
    CHECK(nb_subscriber_read(sub, "data", &read_filter, samples, 8) == 1);

    n = nb_subscriber_take(sub, "data", &read_filter, samples, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(payload_is(&samples[0], "one"));
        nb_sample_release(&samples[0]);
    }
    CHECK(nb_subscriber_queue_size(sub, "data") == 0);

    /* ---- P2.5 wait set: timeout, then an event-driven wake-up ---- */
    {
        nb_waitset_t *ws = NULL;
        ntk_thread *thread;
        producer_t producer;
        uint64_t started;
        uint64_t elapsed;
        int wrc;

        CHECK(nb_waitset_create(server, &ws) == NB_OK);
        CHECK(nb_waitset_attach(ws, sub, "data") == NB_OK);
        CHECK(nb_waitset_ready(ws, NULL) == 0);
        CHECK(nb_waitset_wait(ws, NULL, 0) == 0);
        CHECK(nb_waitset_wait(ws, NULL, 80) == 0);

        producer.proxy = proxy;
        producer.rc = -1;
        thread = ntk_thread_create(producer_fn, &producer);
        CHECK(thread != NULL);
        started = ntk_monotonic_ms();
        wrc = nb_waitset_wait(ws, NULL, 3000);
        elapsed = ntk_monotonic_ms() - started;
        CHECK(wrc == 1);
        CHECK(elapsed >= 50);
        CHECK(elapsed < 2500);
        if (thread) {
            (void)ntk_thread_join(thread);
            ntk_thread_destroy(thread);
        }
        CHECK(producer.rc == NB_OK);
        CHECK(nb_waitset_ready(ws, NULL) == 1);
        CHECK(nb_subscriber_take(sub, "data", NULL, samples, 8) == 1);
        CHECK(payload_is(&samples[0], "three"));
        nb_sample_release(&samples[0]);
        CHECK(nb_waitset_ready(ws, NULL) == 0);

        CHECK(nb_waitset_detach(ws, sub, "data") == NB_OK);
        CHECK(nb_waitset_detach(ws, sub, "data") == NB_ERR_NOTFOUND);
        nb_waitset_destroy(ws);
    }

    /* A queued subscription has no handler: a REQUEST must be rejected, not
     * dispatched to a NULL function pointer. */
    {
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;
        nb_response_t resp;
        int st;
        memset(&resp, 0, sizeof(resp));
        opt.timeout_ms = 1000;
        pl.data = "req";
        pl.len = 3;
        st = nb_proxy_call_sync(proxy, "srv", "data", &pl, &opt, &resp);
        CHECK(st == NB_ERR_NOTFOUND);
        nb_response_release(&resp);
    }

cleanup:
    nb_proxy_destroy(proxy);
    nb_service_destroy(sub);
    nb_node_destroy(client);
    nb_node_destroy(server);
}

int main(void)
{
    test_keep_last_and_read_take();
    test_state_masks_and_instances();
    test_service_wiring();

    if (fails) {
        printf("test_sample_queue: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_sample_queue: ok\n");
    return 0;
}
