#include "rtps_types.h"

#include <string.h>

const nb_rtps_entity_id_t nb_rtps_eid_participant = { { 0x00, 0x00, 0x01, 0xc1 } };
const nb_rtps_entity_id_t nb_rtps_eid_spdp_writer = { { 0x00, 0x01, 0x00, 0xc2 } };
const nb_rtps_entity_id_t nb_rtps_eid_spdp_reader = { { 0x00, 0x01, 0x00, 0xc7 } };
const nb_rtps_entity_id_t nb_rtps_eid_sedp_publications_writer = { { 0x00, 0x00, 0x03, 0xc2 } };
const nb_rtps_entity_id_t nb_rtps_eid_sedp_publications_reader = { { 0x00, 0x00, 0x03, 0xc7 } };
const nb_rtps_entity_id_t nb_rtps_eid_sedp_subscriptions_writer = { { 0x00, 0x00, 0x04, 0xc2 } };
const nb_rtps_entity_id_t nb_rtps_eid_sedp_subscriptions_reader = { { 0x00, 0x00, 0x04, 0xc7 } };
const nb_rtps_entity_id_t nb_rtps_eid_participant_message_writer = { { 0x00, 0x02, 0x00, 0xc2 } };
const nb_rtps_entity_id_t nb_rtps_eid_participant_message_reader = { { 0x00, 0x02, 0x00, 0xc7 } };
const nb_rtps_entity_id_t nb_rtps_eid_unknown = { { 0x00, 0x00, 0x00, 0x00 } };

bool nb_rtps_entity_id_equal(const nb_rtps_entity_id_t *a, const nb_rtps_entity_id_t *b)
{
    return memcmp(a->bytes, b->bytes, NB_RTPS_ENTITY_ID_SIZE) == 0;
}

bool nb_rtps_guid_prefix_equal(const nb_rtps_guid_prefix_t *a, const nb_rtps_guid_prefix_t *b)
{
    return memcmp(a->bytes, b->bytes, NB_RTPS_GUID_PREFIX_SIZE) == 0;
}

bool nb_rtps_guid_equal(const nb_rtps_guid_t *a, const nb_rtps_guid_t *b)
{
    return nb_rtps_guid_prefix_equal(&a->prefix, &b->prefix) &&
           nb_rtps_entity_id_equal(&a->entity, &b->entity);
}

bool nb_rtps_sequence_number_equal(const nb_rtps_sequence_number_t *a,
                                   const nb_rtps_sequence_number_t *b)
{
    return a->high == b->high && a->low == b->low;
}

int nb_rtps_sequence_number_compare(const nb_rtps_sequence_number_t *a,
                                    const nb_rtps_sequence_number_t *b)
{
    if (a->high != b->high) {
        return a->high < b->high ? -1 : 1;
    }
    if (a->low != b->low) {
        return a->low < b->low ? -1 : 1;
    }
    return 0;
}

void nb_rtps_locator_ipv4(nb_rtps_locator_t *locator,
                          uint8_t a,
                          uint8_t b,
                          uint8_t c,
                          uint8_t d,
                          uint16_t port)
{
    if (!locator) {
        return;
    }
    memset(locator, 0, sizeof(*locator));
    locator->kind = NB_RTPS_LOCATOR_KIND_UDPV4;
    locator->port = port;
    /* RTPS Locator_t stores an IPv4 address in the last four bytes of the
     * 16-byte address field; the first twelve bytes are zero. */
    locator->address[12] = a;
    locator->address[13] = b;
    locator->address[14] = c;
    locator->address[15] = d;
}

void nb_rtps_locator_spdp_multicast(nb_rtps_locator_t *locator, uint16_t port)
{
    nb_rtps_locator_ipv4(locator, 239, 255, 0, 1, port);
}
