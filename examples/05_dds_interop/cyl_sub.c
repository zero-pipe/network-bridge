/*
 * Cyclone DDS side: subscribe to Trade samples.
 *
 * Interoperates with example_05_nb_pub (network-bridge publisher). With the
 * optional "strict" argument it enables strict type consistency
 * (DISALLOW_TYPE_COERCION + force_type_validation), which only matches because
 * the network-bridge publisher advertises PID_TYPE_INFORMATION.
 *
 * Usage: example_05_cyl_sub [domain] [strict]   (default domain 42)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dds/dds.h"
#include "Trade.h"

int main(int argc, char **argv)
{
    dds_domainid_t domain = (dds_domainid_t)(argc > 1 ? atoi(argv[1]) : 42);
    int strict = argc > 2 && strcmp(argv[2], "strict") == 0;
    dds_entity_t pp;
    dds_qos_t *qos = NULL;
    dds_entity_t tp;
    dds_entity_t reader;
    void *samples[4] = {0};
    dds_sample_info_t info[4];
    int matched = 0;
    int n = 0;
    int i;

    pp = dds_create_participant(domain, NULL, NULL);
    if (pp < 0) {
        printf("CYCLONE_PART_FAIL %d\n", (int)pp);
        return 1;
    }
    if (strict) {
        qos = dds_create_qos();
        dds_qset_type_consistency(qos, DDS_TYPE_CONSISTENCY_DISALLOW_TYPE_COERCION,
                                  false, false, false, false, true);
    }
    tp = dds_create_topic(pp, &Trade_desc, "Trade", qos, NULL);
    reader = tp < 0 ? tp : dds_create_reader(pp, tp, qos, NULL);
    if (qos) {
        dds_delete_qos(qos);
    }
    if (reader < 0) {
        printf("CYCLONE_SETUP_FAIL tp=%d rd=%d\n", (int)tp, (int)reader);
        dds_delete(pp);
        return 1;
    }
    printf("CYCLONE_SUB_READY%s\n", strict ? " (strict)" : "");
    fflush(stdout);

    for (i = 0; i < 300 && matched == 0; i++) {
        dds_subscription_matched_status_t st;
        if (dds_get_subscription_matched_status(reader, &st) == DDS_RETCODE_OK &&
            st.current_count > 0) {
            matched = st.current_count;
        } else {
            dds_sleepfor(DDS_MSECS(100));
        }
    }
    printf("CYCLONE_MATCHED=%d\n", matched);
    fflush(stdout);

    for (i = 0; i < 100 && n <= 0; i++) {
        n = dds_take(reader, samples, info, 4, 4);
        if (n <= 0) {
            dds_sleepfor(DDS_MSECS(100));
        }
    }
    if (n > 0) {
        Trade *g = (Trade *)samples[0];
        printf("CYCLONE_GOT %s %.1f %d\n", g->symbol, g->price, g->volume);
        dds_return_loan(reader, samples, n);
    }

    dds_delete(pp);
    return matched > 0 ? 0 : 1;
}
