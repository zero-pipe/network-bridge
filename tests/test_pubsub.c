/*
 * Pub/sub spine: participant / topic / writer / reader over the RTPS backend.
 *
 * Two nodes in one process use explicit unicast discovery, so the test does
 * not depend on multicast. It covers:
 *   - callback delivery, and that the wire topic is the topic name itself
 *     (no "service.method" route, no "_evt" suffix);
 *   - a KEEP_LAST history reader drained with read/take;
 *   - argument validation and the rejection of non-topic bindings.
 */
#include <nb/log.h>
#include <nb/nb.h>
#include <nb/pubsub.h>

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

typedef struct sink {
    ntk_mutex *lock;
    int count;
    size_t last_len;
    char last[64];
} sink_t;

static void sink_init(sink_t *sink)
{
    memset(sink, 0, sizeof(*sink));
    sink->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
}

static void sink_destroy(sink_t *sink)
{
    if (sink->lock) {
        ntk_mutex_destroy(sink->lock);
    }
}

static int sink_count(sink_t *sink)
{
    int count;

    ntk_mutex_lock(sink->lock);
    count = sink->count;
    ntk_mutex_unlock(sink->lock);
    return count;
}

static void on_sample(void *user, const nb_sample_t *sample)
{
    sink_t *sink = (sink_t *)user;
    size_t len;

    if (!sink || !sample) {
        return;
    }
    len = sample->data.len;
    if (len > sizeof(sink->last)) {
        len = sizeof(sink->last);
    }
    ntk_mutex_lock(sink->lock);
    sink->count++;
    sink->last_len = len;
    if (sample->data.data && len > 0) {
        memcpy(sink->last, sample->data.data, len);
    }
    ntk_mutex_unlock(sink->lock);
}

static nb_node_t *make_node(const char *endpoint, const char *name)
{
    nb_node_config_t cfg;
    nb_binding_config_t binding;
    nb_node_t *node = NULL;

    memset(&binding, 0, sizeof(binding));
    binding.name = "rtps";
    binding.endpoint = endpoint;
    binding.intra_process = NB_INTRA_PROCESS_OFF;
    nb_node_config_default(&cfg);
    cfg.name = name;
    cfg.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;
    if (nb_node_create(&cfg, &node) != NB_OK) {
        return NULL;
    }
    return node;
}

static const char *wire_topic_name(nb_node_t *node, const char *want)
{
    static char found[NB_DISCOVERY_TOPIC_MAX];
    nb_discovery_sample_t views[16];
    size_t n;
    size_t i;

    n = nb_node_discovery_read(node, NB_BUILTIN_TOPIC_TOPIC, views, 16);
    for (i = 0; i < n; i++) {
        if (strcmp(views[i].name, want) == 0) {
            snprintf(found, sizeof(found), "%s", views[i].name);
            return found;
        }
    }
    return NULL;
}

static void test_callback_and_wire_name(void)
{
    nb_node_t *node_a = make_node("rtps://20:47410/127.0.0.1:47411", "ps-a");
    nb_node_t *node_b = make_node("rtps://20:47411/127.0.0.1:47410", "ps-b");
    nb_participant_t *part_a = NULL;
    nb_participant_t *part_b = NULL;
    nb_topic_t *topic_a = NULL;
    nb_topic_t *topic_b = NULL;
    nb_writer_t *writer = NULL;
    nb_reader_t *reader = NULL;
    nb_participant_options_t popt;
    sink_t sink;
    static const char text[] = "hello-pubsub";
    nb_payload_t payload;
    int i;

    sink_init(&sink);
    CHECK(node_a != NULL && node_b != NULL);
    if (!node_a || !node_b) {
        goto cleanup;
    }
    memset(&popt, 0, sizeof(popt));
    popt.scheme = "rtps";
    popt.node = node_a;
    popt.name = "a";
    CHECK(nb_participant_create(&popt, &part_a) == NB_OK);
    popt.node = node_b;
    popt.name = "b";
    CHECK(nb_participant_create(&popt, &part_b) == NB_OK);
    if (!part_a || !part_b) {
        goto cleanup;
    }
    CHECK(nb_topic_create(part_a, "chat", "text", NULL, &topic_a) == NB_OK);
    CHECK(nb_topic_create(part_b, "chat", "text", NULL, &topic_b) == NB_OK);
    if (!topic_a || !topic_b) {
        goto cleanup;
    }
    CHECK(nb_reader_create(topic_b, NULL, on_sample, &sink, 0, &reader) == NB_OK);
    CHECK(nb_writer_create(topic_a, NULL, &writer) == NB_OK);
    if (!reader || !writer) {
        goto cleanup;
    }

    memset(&payload, 0, sizeof(payload));
    payload.data = text;
    payload.len = sizeof(text) - 1u;
    for (i = 0; i < 200 && sink_count(&sink) == 0; i++) {
        CHECK(nb_writer_write(writer, &payload) == NB_OK);
        ntk_sleep_ms(50);
    }
    CHECK(sink_count(&sink) > 0);
    ntk_mutex_lock(sink.lock);
    CHECK(sink.last_len == sizeof(text) - 1u);
    CHECK(memcmp(sink.last, text, sizeof(text) - 1u) == 0);
    ntk_mutex_unlock(sink.lock);

    /* The provider names the native topic after the topic itself: no
     * "service.method" route, no "_evt" suffix. */
    for (i = 0; i < 40 && wire_topic_name(node_a, "chat") == NULL; i++) {
        ntk_sleep_ms(50);
    }
    CHECK(wire_topic_name(node_a, "chat") != NULL);
    CHECK(wire_topic_name(node_a, "chat_evt") == NULL);

cleanup:
    nb_reader_destroy(reader);
    nb_writer_destroy(writer);
    nb_topic_destroy(topic_a);
    nb_topic_destroy(topic_b);
    nb_participant_destroy(part_a);
    nb_participant_destroy(part_b);
    nb_node_destroy(node_b);
    nb_node_destroy(node_a);
    sink_destroy(&sink);
}

static void test_history_reader(void)
{
    nb_node_t *node_a = make_node("rtps://20:47420/127.0.0.1:47421", "ps-a");
    nb_node_t *node_b = make_node("rtps://20:47421/127.0.0.1:47420", "ps-b");
    nb_participant_t *part_a = NULL;
    nb_participant_t *part_b = NULL;
    nb_topic_t *topic_a = NULL;
    nb_topic_t *topic_b = NULL;
    nb_writer_t *writer = NULL;
    nb_reader_t *reader = NULL;
    nb_participant_options_t popt;
    nb_sample_t samples[4];
    nb_payload_t payload;
    static const char *const words[] = { "one", "two", "three" };
    size_t confirmed = 0;
    int waited = 0;
    size_t taken;
    size_t i;

    CHECK(node_a != NULL && node_b != NULL);
    if (!node_a || !node_b) {
        goto cleanup;
    }
    memset(&popt, 0, sizeof(popt));
    popt.scheme = "rtps";
    popt.node = node_a;
    CHECK(nb_participant_create(&popt, &part_a) == NB_OK);
    popt.node = node_b;
    CHECK(nb_participant_create(&popt, &part_b) == NB_OK);
    if (!part_a || !part_b) {
        goto cleanup;
    }
    CHECK(nb_topic_create(part_a, "news", "text", NULL, &topic_a) == NB_OK);
    CHECK(nb_topic_create(part_b, "news", "text", NULL, &topic_b) == NB_OK);
    if (!topic_a || !topic_b) {
        goto cleanup;
    }
    CHECK(nb_reader_create(topic_b, NULL, NULL, NULL, 4, &reader) == NB_OK);
    CHECK(nb_writer_create(topic_a, NULL, &writer) == NB_OK);
    if (!reader || !writer) {
        goto cleanup;
    }
    CHECK(nb_reader_size(reader) == 0);

    /* Re-send the next missing word until it shows up: samples published before
     * the reliable/volatile reader has matched are simply dropped, so a single
     * write per sample would be lost on a cold start. The word index follows the
     * history size, so at most one sample is in flight at a time. */
    while (waited < 15000 && confirmed < 3) {
        confirmed = nb_reader_size(reader);
        if (confirmed >= 3) {
            break;
        }
        payload.data = words[confirmed];
        payload.len = strlen(words[confirmed]);
        payload.encoding = 0;
        CHECK(nb_writer_write(writer, &payload) == NB_OK);
        ntk_sleep_ms(200);
        waited += 200;
    }
    confirmed = nb_reader_size(reader);
    CHECK(confirmed == 3);

    memset(samples, 0, sizeof(samples));
    taken = nb_reader_take(reader, NULL, samples, 4);
    CHECK(taken == 3);
    for (i = 0; i < taken && i < 3; i++) {
        CHECK(samples[i].data.len == strlen(words[i]));
        CHECK(samples[i].data.data &&
              memcmp(samples[i].data.data, words[i], strlen(words[i])) == 0);
        nb_sample_release(&samples[i]);
    }
    CHECK(nb_reader_size(reader) == 0);

cleanup:
    nb_reader_destroy(reader);
    nb_writer_destroy(writer);
    nb_topic_destroy(topic_a);
    nb_topic_destroy(topic_b);
    nb_participant_destroy(part_a);
    nb_participant_destroy(part_b);
    nb_node_destroy(node_b);
    nb_node_destroy(node_a);
}

#ifdef NB_ENABLE_DDS
/* Same object model over the Cyclone DDS backend: two nodes on one domain, real
 * DDS discovery, topic named after the application topic. */
static void test_dds_topic(void)
{
    nb_node_config_t cfg;
    nb_binding_config_t binding;
    nb_node_t *node_a = NULL;
    nb_node_t *node_b = NULL;
    nb_participant_t *part_a = NULL;
    nb_participant_t *part_b = NULL;
    nb_topic_t *topic_a = NULL;
    nb_topic_t *topic_b = NULL;
    nb_writer_t *writer = NULL;
    nb_reader_t *reader = NULL;
    nb_participant_options_t popt;
    sink_t sink;
    static const char text[] = "dds-pubsub";
    nb_payload_t payload;
    int i;

    sink_init(&sink);
    if (nb_cyclone_dds_register() != NB_OK) {
        sink_destroy(&sink);
        return;
    }
    memset(&binding, 0, sizeof(binding));
    binding.name = "dds";
    binding.endpoint = "dds://62";
    binding.intra_process = NB_INTRA_PROCESS_OFF;
    nb_node_config_default(&cfg);
    cfg.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;
    cfg.name = "ps-dds-a";
    CHECK(nb_node_create(&cfg, &node_a) == NB_OK);
    cfg.name = "ps-dds-b";
    CHECK(nb_node_create(&cfg, &node_b) == NB_OK);
    if (!node_a || !node_b) {
        goto cleanup;
    }
    memset(&popt, 0, sizeof(popt));
    popt.scheme = "dds";
    popt.node = node_a;
    CHECK(nb_participant_create(&popt, &part_a) == NB_OK);
    popt.node = node_b;
    CHECK(nb_participant_create(&popt, &part_b) == NB_OK);
    if (!part_a || !part_b) {
        goto cleanup;
    }
    CHECK(nb_topic_create(part_a, "chatdds", "text", NULL, &topic_a) == NB_OK);
    CHECK(nb_topic_create(part_b, "chatdds", "text", NULL, &topic_b) == NB_OK);
    if (!topic_a || !topic_b) {
        goto cleanup;
    }
    CHECK(nb_reader_create(topic_b, NULL, on_sample, &sink, 0, &reader) == NB_OK);
    CHECK(nb_writer_create(topic_a, NULL, &writer) == NB_OK);
    if (!reader || !writer) {
        goto cleanup;
    }

    memset(&payload, 0, sizeof(payload));
    payload.data = text;
    payload.len = sizeof(text) - 1u;
    for (i = 0; i < 200 && sink_count(&sink) == 0; i++) {
        CHECK(nb_writer_write(writer, &payload) == NB_OK);
        ntk_sleep_ms(50);
    }
    CHECK(sink_count(&sink) > 0);
    ntk_mutex_lock(sink.lock);
    CHECK(sink.last_len == sizeof(text) - 1u);
    CHECK(memcmp(sink.last, text, sizeof(text) - 1u) == 0);
    ntk_mutex_unlock(sink.lock);

cleanup:
    nb_reader_destroy(reader);
    nb_writer_destroy(writer);
    nb_topic_destroy(topic_a);
    nb_topic_destroy(topic_b);
    nb_participant_destroy(part_a);
    nb_participant_destroy(part_b);
    nb_node_destroy(node_b);
    nb_node_destroy(node_a);
    sink_destroy(&sink);
}
#endif /* NB_ENABLE_DDS */

static void test_argument_validation(void)
{
    nb_node_t *node = make_node("rtps://20:47412", "ps-v");
    nb_participant_t *part = NULL;
    nb_participant_t *none = NULL;
    nb_topic_t *topic = NULL;
    nb_reader_t *reader = NULL;
    nb_participant_options_t popt;

    CHECK(node != NULL);
    if (!node) {
        return;
    }
    memset(&popt, 0, sizeof(popt));
    popt.scheme = "rtps";
    popt.node = node;
    CHECK(nb_participant_create(&popt, &part) == NB_OK);

    /* A scheme whose backend has no native topics cannot carry pub/sub. */
    popt.scheme = "tcp";
    CHECK(nb_participant_create(&popt, &none) == NB_ERR_NOT_SUPPORTED);
    CHECK(none == NULL);

    CHECK(nb_topic_create(part, "", "text", NULL, &topic) == NB_ERR_INVALID);
    CHECK(nb_topic_create(part, "a.b", "text", NULL, &topic) == NB_ERR_INVALID);
    CHECK(nb_topic_create(part, "ok", "text", NULL, &topic) == NB_OK);
    /* Neither a callback nor a history depth. */
    CHECK(nb_reader_create(topic, NULL, NULL, NULL, 0, &reader) == NB_ERR_INVALID);
    CHECK(reader == NULL);
    CHECK(nb_writer_write(NULL, NULL) == NB_ERR_INVALID);

    nb_topic_destroy(topic);
    nb_participant_destroy(part);
    nb_node_destroy(node);
}

int main(void)
{
    (void)nb_rtps_transport_register();
    test_callback_and_wire_name();
    test_history_reader();
#ifdef NB_ENABLE_DDS
    test_dds_topic();
#endif
    test_argument_validation();
    if (g_failures == 0) {
        nb_log_info("pubsub tests passed (%d checks)", g_checks);
        return 0;
    }
    nb_log_error("pubsub tests failed: %d of %d checks", g_failures, g_checks);
    return 1;
}
