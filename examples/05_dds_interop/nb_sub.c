/*
 * network-bridge side: subscribe to raw-CDR Trade samples.
 *
 * The RTPS subscriber. Interoperates with example_05_nb_pub and with a Cyclone
 * DDS publisher (cyl_pub.c). It advertises TRADE_TI so a strict Cyclone writer
 * matches too.
 *
 * Usage: example_05_nb_sub [domain] [seconds]   (defaults 42, 20)
 */
#define NB_LOG_TAG "nb-sub"
#include <nb/dds.h>

#include <zn/zn.h>

#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trade_cdr.h"
#include "trade_ti.h"

typedef struct sub_state {
    int received;
} sub_state_t;

static void on_trade(void *user, const nb_dds_sample_t *sample)
{
    sub_state_t *st = (sub_state_t *)user;
    trade_value_t v;

    if (trade_decode(sample->data.data, sample->data.len, &v) != 0) {
        nb_log_warn("dropped undecodable sample (%lu bytes)",
                    (unsigned long)sample->data.len);
        return;
    }
    st->received++;
    printf("GOT %s %.1f %d\n", v.symbol, v.price, v.volume);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    nb_dds_participant_options_t popt;
    nb_dds_participant_t *participant = NULL;
    nb_dds_topic_t *topic = NULL;
    nb_dds_reader_t *reader = NULL;
    sub_state_t st = {0};
    uint8_t domain = (uint8_t)(argc > 1 ? atoi(argv[1]) : 42);
    int seconds = argc > 2 ? atoi(argv[2]) : 20;
    int rc;
    int i;

    memset(&popt, 0, sizeof(popt));
    popt.domain = domain;
    popt.name = "nb-sub";

    rc = nb_dds_participant_create(&popt, &participant);
    if (rc != NB_OK) {
        nb_log_error("participant create failed: %d", rc);
        return 1;
    }
    rc = nb_dds_topic_create_with_type_info(participant, "Trade", "Trade",
                                            TRADE_TI, sizeof(TRADE_TI), NULL, &topic);
    if (rc == NB_OK) {
        rc = nb_dds_reader_create(topic, NULL, on_trade, &st, &reader);
    }
    if (rc != NB_OK) {
        nb_log_error("setup failed: %d", rc);
        goto out;
    }
    nb_log_info("subscribed to Trade on domain %u, waiting %d s",
                (unsigned)domain, seconds);

    for (i = 0; i < seconds * 10; i++) {
        if (st.received > 0) {
            break;
        }
        zn_sleep_ms(100);
    }
    nb_log_info("received %d sample(s)", st.received);

out:
    nb_dds_reader_destroy(reader);
    nb_dds_topic_destroy(topic);
    nb_dds_participant_destroy(participant);
    return rc == NB_OK ? 0 : 1;
}
