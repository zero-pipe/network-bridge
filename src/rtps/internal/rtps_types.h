#ifndef NB_RTPS_TYPES_H
#define NB_RTPS_TYPES_H

/*
 * RTPS wire types shared by the discovery codec and the participant runtime.
 *
 * Scope of the discovery RTPS subset: RTPS 2.x over IPv4 UDP, little-endian
 * CDR, no fragmentation, no security, no TCP transport. These are internal
 * types; they are not part of the public discovery API.
 *
 * All fields are stored in host form. Encoding to the little-endian wire form
 * happens in rtps_message.c and rtps_param.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NB_RTPS_GUID_PREFIX_SIZE 12
#define NB_RTPS_ENTITY_ID_SIZE 4
#define NB_RTPS_GUID_SIZE 16
#define NB_RTPS_LOCATOR_ADDRESS_SIZE 16

#define NB_RTPS_PROTOCOL_VERSION_MAJOR 2u
#define NB_RTPS_PROTOCOL_VERSION_MINOR 4u

/*
 * Placeholder vendor id. Vendor ids are assigned by the OMG; replace both
 * bytes with the assigned value before claiming conformance.
 */
#define NB_RTPS_VENDOR_ID_0 0x01u
#define NB_RTPS_VENDOR_ID_1 0x9au

typedef struct nb_rtps_guid_prefix {
    uint8_t bytes[NB_RTPS_GUID_PREFIX_SIZE];
} nb_rtps_guid_prefix_t;

typedef struct nb_rtps_entity_id {
    uint8_t bytes[NB_RTPS_ENTITY_ID_SIZE];
} nb_rtps_entity_id_t;

typedef struct nb_rtps_guid {
    nb_rtps_guid_prefix_t prefix;
    nb_rtps_entity_id_t entity;
} nb_rtps_guid_t;

typedef struct nb_rtps_sequence_number {
    int32_t high;
    uint32_t low;
} nb_rtps_sequence_number_t;

typedef struct nb_rtps_locator {
    int32_t kind;
    uint32_t port;
    uint8_t address[NB_RTPS_LOCATOR_ADDRESS_SIZE];
} nb_rtps_locator_t;

typedef struct nb_rtps_time {
    int32_t seconds;
    uint32_t fraction;
} nb_rtps_time_t;

typedef struct nb_rtps_duration {
    int32_t seconds;
    uint32_t fraction;
} nb_rtps_duration_t;

/* Locator kinds from the RTPS spec. Only UDPv4 is implemented. */
#define NB_RTPS_LOCATOR_KIND_INVALID 0
#define NB_RTPS_LOCATOR_KIND_UDPV4 1
#define NB_RTPS_LOCATOR_KIND_UDPV6 2
/* RTPS 2.5 locator kinds for connection-oriented transports. A TCP locator
 * names the listening port; the stream itself carries plain RTPS messages
 * (see rtps_tcp.c). */
#define NB_RTPS_LOCATOR_KIND_TCPV4 4
#define NB_RTPS_LOCATOR_KIND_TCPV6 8

/* Built-in entity ids used by SPDP and SEDP. */
extern const nb_rtps_entity_id_t nb_rtps_eid_participant;
extern const nb_rtps_entity_id_t nb_rtps_eid_spdp_writer;
extern const nb_rtps_entity_id_t nb_rtps_eid_spdp_reader;
extern const nb_rtps_entity_id_t nb_rtps_eid_sedp_publications_writer;
extern const nb_rtps_entity_id_t nb_rtps_eid_sedp_publications_reader;
extern const nb_rtps_entity_id_t nb_rtps_eid_sedp_subscriptions_writer;
extern const nb_rtps_entity_id_t nb_rtps_eid_sedp_subscriptions_reader;
extern const nb_rtps_entity_id_t nb_rtps_eid_participant_message_writer;
extern const nb_rtps_entity_id_t nb_rtps_eid_participant_message_reader;
extern const nb_rtps_entity_id_t nb_rtps_eid_unknown;

bool nb_rtps_entity_id_equal(const nb_rtps_entity_id_t *a, const nb_rtps_entity_id_t *b);
bool nb_rtps_guid_prefix_equal(const nb_rtps_guid_prefix_t *a, const nb_rtps_guid_prefix_t *b);
bool nb_rtps_guid_equal(const nb_rtps_guid_t *a, const nb_rtps_guid_t *b);
bool nb_rtps_sequence_number_equal(const nb_rtps_sequence_number_t *a,
                                   const nb_rtps_sequence_number_t *b);
int nb_rtps_sequence_number_compare(const nb_rtps_sequence_number_t *a,
                                    const nb_rtps_sequence_number_t *b);

/* Fill an IPv4 locator. The address is stored in the last four bytes of the
 * 16-byte RTPS address field; the first twelve bytes are left zero. */
void nb_rtps_locator_ipv4(nb_rtps_locator_t *locator,
                          uint8_t a,
                          uint8_t b,
                          uint8_t c,
                          uint8_t d,
                          uint16_t port);

/* Well-known SPDP multicast group, 239.255.0.1. */
void nb_rtps_locator_spdp_multicast(nb_rtps_locator_t *locator, uint16_t port);

#endif /* NB_RTPS_TYPES_H */
