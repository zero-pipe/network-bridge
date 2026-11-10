/*
 * 04 配置：DDS 面。JSON 里的 dds 块决定 backend/domain/partition/prefix/qos。
 *
 * 用法：example_04_config_dds [dds.json]
 */
#define NB_LOG_TAG "example"
#include <nb/config.h>
#include <nb/dds.h>

#include <ntk.h>

#include <nb/base/log.h>

#include <stdio.h>
#include <string.h>

static int received;

static void on_sample(void *user, const nb_dds_sample_t *sample)
{
    (void)user;
    nb_log_info("received: %.*s", (int)sample->data.len, (const char *)sample->data.data);
    received++;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "dds.json";
    nb_dds_config_doc_t *cfg = NULL;
    nb_config_error_t err;
    nb_dds_participant_t *participant = NULL;
    nb_dds_topic_t *topic = NULL;
    nb_dds_writer_t *writer = NULL;
    nb_dds_reader_t *reader = NULL;
    nb_dds_qos_t qos;
    int rc;
    int i;

    rc = nb_dds_config_parse_file(path, &cfg, &err);
    if (rc != NB_OK) {
        nb_log_error("load config failed: %s (%d) path=%s reason=%s", path, rc, err.path,
                     err.reason);
        return 1;
    }
    rc = nb_dds_participant_create_from_config(cfg, &participant);
    nb_dds_config_doc_destroy(cfg);
    if (rc != NB_OK) {
        nb_log_error("participant setup failed: %d (%s)", rc, nb_error_name(rc));
        return 1;
    }

    nb_dds_qos_init(&qos);
    nb_dds_qos_set_reliability(&qos, NB_DDS_RELIABILITY_RELIABLE);
    nb_dds_qos_set_history(&qos, NB_DDS_HISTORY_KEEP_LAST, 4);

    rc = nb_dds_topic_create(participant, "chat", "text", &qos, &topic);
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(topic, &qos, on_sample, NULL, &reader);
    }
    if (rc == NB_OK) {
        rc = nb_dds_writer_create(topic, &qos, &writer);
    }
    if (rc != NB_OK) {
        nb_log_error("dds setup failed: %d (%s)", rc, nb_error_name(rc));
        goto out;
    }

    for (i = 0; i < 100 && received == 0; i++) {
        (void)nb_dds_writer_write(writer, "hello config", 12);
        ntk_sleep_ms(20);
    }
    if (received == 0) {
        nb_log_error("no sample delivered");
        rc = 1;
    } else {
        nb_log_info("dds config example ok");
        rc = 0;
    }

out:
    nb_dds_reader_destroy(reader);
    nb_dds_writer_destroy(writer);
    nb_dds_topic_destroy(topic);
    nb_dds_participant_destroy(participant);
    return rc;
}
