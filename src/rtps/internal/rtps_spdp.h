#ifndef NB_RTPS_SPDP_H
#define NB_RTPS_SPDP_H

/*
 * SPDP participant announcement payload.
 *
 * This module only builds and parses the ParameterList carried by the SPDP
 * DATA sample. Socket handling, timers and lease tracking live in the
 * participant runtime.
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/error.h>

#include "rtps_types.h"

#define NB_RTPS_SPDP_NAME_MAX 64
#define NB_RTPS_SPDP_USER_DATA_MAX 256

typedef struct nb_rtps_participant_info {
    uint8_t vendor_id[2];
    nb_rtps_guid_t guid;
    nb_rtps_duration_t lease_duration;
    uint32_t builtin_endpoints;
    nb_rtps_locator_t metatraffic_unicast;
    nb_rtps_locator_t metatraffic_multicast;
    /* Connection-oriented metatraffic locator (kind 4/5), advertised as a
     * second PID_METATRAFFIC_UNICAST_LOCATOR. Invalid when TCP is off. */
    nb_rtps_locator_t metatraffic_tcp;
    nb_rtps_locator_t default_unicast;
    nb_rtps_locator_t default_multicast;
    char name[NB_RTPS_SPDP_NAME_MAX];
    uint8_t user_data[NB_RTPS_SPDP_USER_DATA_MAX];
    size_t user_data_len;
} nb_rtps_participant_info_t;

/* Build the serialized ParameterList for an SPDP DATA payload. */
int nb_rtps_spdp_build(const nb_rtps_participant_info_t *info,
                       void *buf,
                       size_t cap,
                       size_t *out_len);

/* Parse an SPDP payload. Fails when a required PID is missing or malformed.
 * Unknown parameters (including vendor-specific ones) are ignored. */
int nb_rtps_spdp_parse(const void *payload, size_t len, nb_rtps_participant_info_t *out);

#endif /* NB_RTPS_SPDP_H */
