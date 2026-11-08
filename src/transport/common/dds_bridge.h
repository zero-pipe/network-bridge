#ifndef NB_DDS_BRIDGE_H
#define NB_DDS_BRIDGE_H

/*
 * Shared support for the two DDS-shaped providers (cyclone and rtps).
 *
 * Both providers carry the framework's v2 envelope as an opaque sample and map
 * an RPC route ("service.method") onto three topics ("_req"/"_res"/"_evt").
 * That mapping and the envelope wrapping used to be duplicated in both
 * providers; it lives here so the two can only drift in the parts that are
 * genuinely backend-specific (participant, matching, status).
 */

#include <nb/message.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Route or topic name -> identifier-safe topic key (the rule both providers
 * used: keep [A-Za-z0-9_], replace the rest with '_', and prefix a leading
 * digit with '_'). */
void nb_bridge_topic_key(const char *route, char *out, size_t cap);

/* Fill the three RPC topic names for a route. Any output may be NULL. */
void nb_bridge_route_topics(const char *route,
                            char *req,
                            size_t req_cap,
                            char *res,
                            size_t res_cap,
                            char *evt,
                            size_t evt_cap);

/* Encoded envelope length for a message; 0 when it cannot be encoded. */
size_t nb_bridge_frame_size(const nb_message_t *message);

/* Encode message into buf[cap]; returns the encoded length or 0. */
size_t nb_bridge_frame_encode(const nb_message_t *message, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_BRIDGE_H */
