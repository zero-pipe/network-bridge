/*
 * Cross-process DDS event delivery.
 *
 * The parent publishes through the framework's Cyclone-backed "dds" provider
 * with the intra-process fast path disabled. The child is a plain Cyclone DDS
 * application: it creates a reader for the same topic and type, and must observe
 * the encoded framework frame. This proves that the provider uses the DDS data
 * plane rather than the local event shortcut.
 */
#include <nb/proxy.h>
#include <nb/node.h>
#include <nb/dds.h>

#include "cy_dds_message.h"

#include <dds/dds.h>
#include <platform.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define TEST_DOMAIN 61
#define TEST_TOPIC "RemoteService_chat_evt"
#define TEST_SERVICE "RemoteService"
#define TEST_METHOD "chat"
#define TEST_PAYLOAD "remote-hello"

static int contains_bytes(const uint8_t *buf, size_t len, const char *text)
{
    size_t text_len = strlen(text);
    size_t i;

    if (!buf || text_len == 0 || len < text_len) {
        return 0;
    }
    for (i = 0; i + text_len <= len; ++i) {
        if (memcmp(buf + i, text, text_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int run_subscriber(void)
{
    dds_entity_t participant;
    dds_entity_t topic;
    dds_entity_t reader;
    int received = 0;
    int i;

    participant = dds_create_participant(TEST_DOMAIN, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "test_dds_remote: participant failed: %s\n",
                dds_strretcode(participant));
        return 1;
    }
    topic = dds_create_topic(participant, &dds_Message_desc, TEST_TOPIC, NULL, NULL);
    if (topic < 0) {
        fprintf(stderr, "test_dds_remote: topic failed: %s\n", dds_strretcode(topic));
        (void)dds_delete(participant);
        return 1;
    }
    reader = dds_create_reader(participant, topic, NULL, NULL);
    if (reader < 0) {
        fprintf(stderr, "test_dds_remote: reader failed: %s\n", dds_strretcode(reader));
        (void)dds_delete(participant);
        return 1;
    }

    for (i = 0; i < 300 && !received; ++i) {
        void *sample = NULL;
        dds_sample_info_t info;
        dds_return_t n;

        memset(&info, 0, sizeof(info));
        n = dds_take(reader, &sample, &info, 1, 1);
        if (n < 0) {
            fprintf(stderr, "test_dds_remote: dds_take failed: %s\n",
                    dds_strretcode(n));
            break;
        }
        if (n > 0 && info.valid_data && sample) {
            dds_Message *msg = (dds_Message *)sample;

            if (msg->payload._buffer && msg->payload._length >= 28 &&
                msg->payload._buffer[0] == 0x6e &&
                msg->payload._buffer[1] == 0x62 &&
                contains_bytes(msg->payload._buffer,
                               msg->payload._length,
                               TEST_PAYLOAD)) {
                printf("test_dds_remote: subscriber received %u-byte DDS sample\n",
                       (unsigned)msg->payload._length);
                received = 1;
            }
        }
        if (n > 0) {
            (void)dds_return_loan(reader, &sample, (size_t)n);
        }
        if (!received) {
            ntk_sleep_ms(50);
        }
    }

    (void)dds_delete(reader);
    (void)dds_delete(topic);
    (void)dds_delete(participant);
    if (!received) {
        fprintf(stderr, "test_dds_remote: subscriber timed out\n");
        return 1;
    }
    return 0;
}

#ifdef _WIN32
typedef intptr_t nb_child_t;

static int start_subscriber(const char *exe, nb_child_t *child)
{
    *child = _spawnl(_P_NOWAIT, exe, exe, "--sub", (char *)NULL);
    if (*child == -1) {
        fprintf(stderr, "test_dds_remote: _spawnl failed\n");
        return -1;
    }
    return 0;
}

static int wait_subscriber(nb_child_t child)
{
    int status = 0;

    if (_cwait(&status, child, 0) == -1) {
        return -1;
    }
    return status;
}
#else
typedef pid_t nb_child_t;

static int start_subscriber(const char *exe, nb_child_t *child)
{
    *child = fork();
    if (*child < 0) {
        return -1;
    }
    if (*child == 0) {
        execl(exe, exe, "--sub", (char *)NULL);
        _exit(127);
    }
    return 0;
}

static int wait_subscriber(nb_child_t child)
{
    int status = 0;

    if (waitpid(child, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

static int run_publisher(const char *exe)
{
    nb_binding_config_t binding;
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t payload;
    nb_child_t child = 0;
    int have_child = 0;
    int subscriber_rc = -1;
    int rc;
    int sent = 0;
    int i;

    if (start_subscriber(exe, &child) != 0) {
        nb_log_error("test_dds_remote: subscriber process failed to start");
        return 1;
    }
    have_child = 1;

    rc = nb_cyclone_dds_register();
    if (rc != NB_OK) {
        nb_log_error("test_dds_remote: dds register failed: %d", rc);
        if (have_child) {
            (void)wait_subscriber(child);
        }
        return 1;
    }

    memset(&binding, 0, sizeof(binding));
    binding.name = "dds";
    binding.endpoint = "dds://61";
    binding.intra_process = NB_INTRA_PROCESS_OFF;

    nb_node_config_default(&cfg);
    cfg.name = "DdsRemotePublisher";
    cfg.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.transport.bindings = &binding;
    cfg.transport.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_proxy_create(node, NULL, &proxy);
    }
    if (rc == NB_OK) {
        rc = nb_proxy_set_target_uri(proxy, "dds://61");
    }
    if (rc != NB_OK) {
        nb_log_error("test_dds_remote: publisher setup failed: %d", rc);
        goto done;
    }

    payload.data = TEST_PAYLOAD;
    payload.len = strlen(TEST_PAYLOAD);
    payload.encoding = 0;
    for (i = 0; i < 200; ++i) {
        rc = nb_proxy_call_event(proxy, TEST_SERVICE, TEST_METHOD, &payload, NULL);
        if (rc != NB_OK) {
            nb_log_error("test_dds_remote: publish failed: %d", rc);
            break;
        }
        sent++;
        ntk_sleep_ms(50);
    }

done:
    if (have_child) {
        subscriber_rc = wait_subscriber(child);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (node) {
        nb_node_destroy(node);
    }
    if (rc != NB_OK) {
        return 1;
    }
    if (subscriber_rc != 0) {
        nb_log_error("test_dds_remote: independent subscriber did not receive data");
        return 1;
    }
    nb_log_info("test_dds_remote: ok (%d frames published)", sent);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--sub") == 0) {
        return run_subscriber();
    }
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        fprintf(stderr, "test_dds_remote: missing executable path\n");
        return 1;
    }
    return run_publisher(argv[0]);
}
