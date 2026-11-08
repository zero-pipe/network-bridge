/*
 * 03 DDS 发布/订阅：topic、writer、reader。
 *
 * 和普通 DDS 应用的心智模型一致：participant 绑定一个 domain，topic 是
 * 名字 + 类型名 + QoS，writer 发样本，reader 收样本。topic 名原样上线，
 * 不做任何 "Service.method" 派生。
 *
 * 本进程同时创建 writer 和两个 reader：
 *   - 回调 reader：每个样本到达时回调；
 *   - history reader：KEEP_LAST(depth) 队列，用 nb_reader_take 批量取走。
 *
 * 换后端只改 binding：dds://0（Cyclone DDS）、rtps://0（内建 RTPS）、
 * shm://channel:1（同机共享内存，Linux）。
 */

#define NB_LOG_TAG "example"
#include <nb/dds.h>
#include <nb/node.h>
#include <nb/pubsub.h>

#include <ntk.h>

#include <nb/log.h>

#include <stdio.h>
#include <string.h>

typedef struct chat_state {
    int received;
    char last[128];
} chat_state_t;

static void on_chat(void *user, const nb_sample_t *sample)
{
    chat_state_t *st = (chat_state_t *)user;
    size_t n = sample->data.len;

    if (n >= sizeof(st->last)) {
        n = sizeof(st->last) - 1;
    }
    if (sample->data.data && n > 0) {
        memcpy(st->last, sample->data.data, n);
    }
    st->last[n] = '\0';
    st->received++;
}

int main(int argc, char **argv)
{
    const char *scheme = argc > 1 ? argv[1] : "dds";
    nb_node_config_t cfg;
    nb_binding_config_t binding;
    nb_node_t *node = NULL;
    nb_participant_t *participant = NULL;
    nb_participant_options_t popt;
    nb_topic_t *topic = NULL;
    nb_writer_t *writer = NULL;
    nb_reader_t *callback_reader = NULL;
    nb_reader_t *history_reader = NULL;
    nb_sample_t samples[4];
    nb_payload_t payload;
    chat_state_t chat;
    int rc;
    int i;
    size_t taken;

    memset(&chat, 0, sizeof(chat));

    if (strcmp(scheme, "dds") == 0) {
        if (nb_cyclone_dds_register() != NB_OK) {
            nb_log_error("Cyclone DDS provider unavailable");
            return 1;
        }
    } else if (strcmp(scheme, "rtps") == 0) {
        if (nb_rtps_transport_register() != NB_OK) {
            nb_log_error("RTPS provider unavailable");
            return 1;
        }
    } else if (strcmp(scheme, "shm") == 0) {
        if (nb_shm_register() != NB_OK) {
            nb_log_error("SHM provider unavailable");
            return 1;
        }
    } else {
        nb_log_error("unknown backend \"%s\" (use dds, rtps or shm)", scheme);
        return 1;
    }

    memset(&binding, 0, sizeof(binding));
    binding.name = scheme;
    binding.endpoint = strcmp(scheme, "shm") == 0 ? "shm://demo:1" : "dds://0";
    if (strcmp(scheme, "rtps") == 0) {
        binding.endpoint = "rtps://0";
    }
    binding.roles = NB_PROVIDER_ROLE_PS;
    /* 可靠的发布/订阅：也可以只设 pubsub.default_qos，让所有 binding 继承。 */
    binding.ps_qos.reliability = NB_QOS_RELIABILITY_RELIABLE;
    binding.ps_qos.history = NB_QOS_HISTORY_KEEP_LAST;
    binding.ps_qos.depth = 4;

    nb_node_config_default(&cfg);
    cfg.name = "dds-pubsub";
    cfg.runtime.io_threads = 1;
    cfg.bindings = &binding;
    cfg.binding_count = 1;

    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        memset(&popt, 0, sizeof(popt));
        popt.node = node;
        popt.scheme = scheme;
        popt.name = "demo";
        rc = nb_participant_create(&popt, &participant);
    }
    if (rc == NB_OK) {
        rc = nb_topic_create(participant, "chat", "text", NULL, &topic);
    }
    if (rc == NB_OK) {
        rc = nb_reader_create(topic, NULL, on_chat, &chat, 0, &callback_reader);
    }
    if (rc == NB_OK) {
        rc = nb_reader_create(topic, NULL, NULL, NULL, 4, &history_reader);
    }
    if (rc == NB_OK) {
        rc = nb_writer_create(topic, NULL, &writer);
    }
    if (rc != NB_OK) {
        nb_log_error("pub/sub setup failed: %d (%s)", rc, nb_error_name(rc));
        goto done;
    }

    payload.data = "hello pub/sub";
    payload.len = strlen((const char *)payload.data);
    payload.encoding = 0;
    /* reliable/volatile 的 writer 在发现完成前写的样本会丢，重发直到收到。 */
    for (i = 0; i < 50 && chat.received == 0; i++) {
        rc = nb_writer_write(writer, &payload);
        if (rc != NB_OK) {
            break;
        }
        ntk_sleep_ms(100);
    }
    if (rc != NB_OK || chat.received == 0) {
        nb_log_error("pub/sub not delivered: rc=%d received=%d", rc, chat.received);
        goto done;
    }
    nb_log_info("callback reader: %s", chat.last);

    /* history reader 保留了最近 depth 个样本，用 take 取走所有权副本。 */
    memset(samples, 0, sizeof(samples));
    taken = nb_reader_take(history_reader, NULL, samples, 4);
    nb_log_info("history reader: %zu sample(s)", taken);
    for (i = 0; i < (int)taken; i++) {
        nb_log_info("  [%d] %.*s",
                    i,
                    (int)samples[i].data.len,
                    (const char *)samples[i].data.data);
        nb_sample_release(&samples[i]);
    }

done:
    nb_reader_destroy(history_reader);
    nb_reader_destroy(callback_reader);
    nb_writer_destroy(writer);
    nb_topic_destroy(topic);
    nb_participant_destroy(participant);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}
