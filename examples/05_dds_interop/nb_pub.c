/*
 * network-bridge side: publish raw-CDR Trade samples with XTypes
 * TypeInformation.
 *
 * This is the RTPS publisher. It interoperates with:
 *   - another network-bridge subscriber (example_05_nb_sub),
 *   - a Cyclone DDS subscriber, including one using strict type consistency
 *     (iso_cyl_sub / cyl_sub.c), because SEDP advertises TRADE_TI.
 *
 * Usage: example_05_nb_pub [domain]      (default domain 42)
 */
#define NB_LOG_TAG "nb-pub"
#include <nb/dds.h>

#include <zn/zn.h>

#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trade_cdr.h"
#include "trade_ti.h"

int main(int argc, char **argv)
{
    nb_dds_participant_options_t popt;
    nb_dds_participant_t *participant = NULL;
    nb_dds_topic_t *topic = NULL;
    nb_dds_writer_t *writer = NULL;
    trade_value_t value;
    unsigned char body[64];
    uint8_t domain = (uint8_t)(argc > 1 ? atoi(argv[1]) : 42);
    int rc;
    int i;

    memset(&popt, 0, sizeof(popt));
    popt.domain = domain;
    popt.name = "nb-pub";
    /* Leave popt.prefix empty so the wire topic name is exactly "Trade" and
     * matches the Cyclone peer. */

    rc = nb_dds_participant_create(&popt, &participant);
    if (rc != NB_OK) {
        nb_log_error("participant create failed: %d", rc);
        return 1;
    }
    /* Real IDL type name ("Trade") => samples are sent as raw CDR bodies.
     * The TRADE_TI blob is advertised so strict peers can match. */
    rc = nb_dds_topic_create_with_type_info(participant, "Trade", "Trade",
                                            TRADE_TI, sizeof(TRADE_TI), NULL, &topic);
    if (rc == NB_OK) {
        rc = nb_dds_writer_create(topic, NULL, &writer);
    }
    if (rc != NB_OK) {
        nb_log_error("setup failed: %d", rc);
        goto out;
    }
    nb_log_info("publishing Trade on domain %u", (unsigned)domain);

    /* reliable/volatile: samples written before a reader matches are lost, so
     * keep publishing for a while. Cyclone/strict sub needs a few seconds of
     * discovery first. */
    value.price = 123.5;
    value.volume = 7;
    for (i = 0; i < 300; i++) {
        snprintf(value.symbol, sizeof(value.symbol), "AAPL");
        value.price = 123.5 + (i % 10);
        value.volume = 7 + i;
        {
            size_t n = trade_encode(body, &value);
            (void)nb_dds_writer_write(writer, body, n);
        }
        zn_sleep_ms(100);
    }

out:
    nb_dds_writer_destroy(writer);
    nb_dds_topic_destroy(topic);
    nb_dds_participant_destroy(participant);
    return rc == NB_OK ? 0 : 1;
}
