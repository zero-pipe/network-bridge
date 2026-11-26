/*
 * Cyclone DDS side: publish Trade samples.
 *
 * Interoperates with example_05_nb_sub (network-bridge subscriber). Builds
 * only when a Cyclone DDS toolchain is present (see CMakeLists.txt); the
 * compiler generates Trade.c/Trade.h from Trade.idl.
 *
 * Usage: example_05_cyl_pub [domain]     (default domain 42)
 */
#include <stdio.h>
#include <stdlib.h>

#include "dds/dds.h"
#include "Trade.h"

int main(int argc, char **argv)
{
    dds_domainid_t domain = (dds_domainid_t)(argc > 1 ? atoi(argv[1]) : 42);
    dds_entity_t pp;
    dds_entity_t tp;
    dds_entity_t writer;
    int i;

    pp = dds_create_participant(domain, NULL, NULL);
    if (pp < 0) {
        printf("CYCLONE_PART_FAIL %d\n", (int)pp);
        return 1;
    }
    tp = dds_create_topic(pp, &Trade_desc, "Trade", NULL, NULL);
    writer = tp < 0 ? tp : dds_create_writer(pp, tp, NULL, NULL);
    if (writer < 0) {
        printf("CYCLONE_SETUP_FAIL tp=%d wr=%d\n", (int)tp, (int)writer);
        dds_delete(pp);
        return 1;
    }
    printf("CYCLONE_PUB_READY\n");
    fflush(stdout);

    for (i = 0; i < 300; i++) {
        char sym[] = "AAPL";
        Trade t;
        t.symbol = sym;
        t.price = 123.5 + (i % 10);
        t.volume = 7 + i;
        dds_write(writer, &t);
        dds_sleepfor(DDS_MSECS(100));
    }

    dds_delete(pp);
    return 0;
}
