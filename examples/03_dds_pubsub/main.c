/*
 * 03 DDS 发布/订阅：participant、topic、writer、reader。
 *
 * DDS 是独立的通信面：不依赖 RPC node、binding、路由或 ntk_discovery。
 * participant 绑定 domain，topic 是 name + type + QoS，后端可选：
 *   - rtps   （内建 RTPS 引擎，总是可用）
 *   - cyclone （编译期 NB_ENABLE_DDS=ON）
 *
 * 本进程同时创建 writer、回调 reader 和 history reader。
 *
 * 用法：example_03_dds_pubsub [rtps|cyclone]
 */
#define NB_LOG_TAG "example"
#include <nb/dds.h>

#include <ntk.h>

#include <nb/base/log.h>

#include <stdio.h>
#include <string.h>

typedef struct chat_state {
    int received;
    char last[128];
} chat_state_t;

static void on_chat(void *user, const nb_dds_sample_t *sample)
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

static void make_qos(nb_dds_qos_t *qos)
{
    nb_dds_qos_init(qos);
    nb_dds_qos_set_reliability(qos, NB_DDS_RELIABILITY_RELIABLE);
    nb_dds_qos_set_history(qos, NB_DDS_HISTORY_KEEP_LAST, 4);
}

int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "rtps";
    nb_dds_participant_options_t popt;
    nb_dds_participant_t *participant = NULL;
    nb_dds_topic_t *topic = NULL;
    nb_dds_writer_t *writer = NULL;
    nb_dds_reader_t *callback_reader = NULL;
    nb_dds_reader_t *history_reader = NULL;
    nb_dds_qos_t qos;
    nb_dds_sample_t samples[4];
    chat_state_t chat;
    size_t taken;
    int rc;
    int i;

    memset(&chat, 0, sizeof(chat));
    make_qos(&qos);

    memset(&popt, 0, sizeof(popt));
    popt.backend = strcmp(backend, "cyclone") == 0 ? NB_DDS_BACKEND_CYCLONE
                                                   : NB_DDS_BACKEND_RTPS;
    popt.domain = 70;
    popt.prefix = "example";
    popt.name = "dds-pubsub-example";
    popt.default_qos = &qos;

    rc = nb_dds_participant_create(&popt, &participant);
    if (rc != NB_OK) {
        nb_log_error("participant create failed: %d (%s) backend=%s", rc, nb_error_name(rc),
                     backend);
        return 1;
    }
    rc = nb_dds_topic_create(participant, "chat", "text", &qos, &topic);
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(topic, &qos, on_chat, &chat, &callback_reader);
    }
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(topic, &qos, NULL, NULL, &history_reader);
    }
    if (rc == NB_OK) {
        rc = nb_dds_writer_create(topic, &qos, &writer);
    }
    if (rc != NB_OK) {
        nb_log_error("dds setup failed: %d (%s)", rc, nb_error_name(rc));
        goto out;
    }

    /* reliable/volatile 的 writer 在匹配前发送的样本会丢，重试直到有样本。 */
    for (i = 0; i < 100 && chat.received == 0; i++) {
        (void)nb_dds_writer_write(writer, "hello dds", 9);
        ntk_sleep_ms(20);
    }
    if (chat.received == 0) {
        nb_log_error("no sample delivered (backend=%s)", backend);
        rc = 1;
        goto out;
    }
    nb_log_info("callback reader got: %s", chat.last);

    taken = nb_dds_reader_take(history_reader, NULL, samples, 4);
    if (taken > 0) {
        nb_log_info("history reader took %lu sample(s), first=%.*s", (unsigned long)taken,
                    (int)samples[0].data.len, (const char *)samples[0].data.data);
        nb_dds_sample_release(samples, taken);
    }
    while ((taken = nb_dds_reader_take(history_reader, NULL, samples, 4)) > 0) {
        nb_dds_sample_release(samples, taken);
    }
    rc = 0;

out:
    nb_dds_reader_destroy(history_reader);
    nb_dds_reader_destroy(callback_reader);
    nb_dds_writer_destroy(writer);
    nb_dds_topic_destroy(topic);
    nb_dds_participant_destroy(participant);
    return rc;
}
