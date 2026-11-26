#ifndef NB_RTPS_WIRE_H
#define NB_RTPS_WIRE_H

/*
 * RTPS message builders.
 *
 * Every outbound RTPS message has the same shape:
 *
 *   MessageHeader + INFO_TS + (optional INFO_DST) + one entity submessage
 *
 * The participant used to spell that chain out at each call site, which made
 * the protocol sequencing hard to read and easy to get inconsistent (a missing
 * INFO_DST, a forgotten writer_ok check). These builders own the chain; the
 * caller only decides which submessage and where to send it.
 *
 * Each returns the message length in bytes, or 0 on error (buffer too small,
 * malformed input). The destination prefix may be NULL for messages that are
 * source-addressed only (SPDP); the timestamp may be NULL to omit INFO_TS.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_message.h"

size_t nb_rtps_wire_data(uint8_t *buf,
                         size_t cap,
                         const nb_rtps_header_t *header,
                         const nb_rtps_time_t *ts,
                         const nb_rtps_guid_prefix_t *dst,
                         const nb_rtps_data_t *data);

/*
 * One Message carrying `count` DATA submessages for the same reader (header and
 * INFO_TS/INFO_DST written once). Used by the bulk-write path to put several
 * small samples in a single datagram. Returns the message length or 0.
 */
size_t nb_rtps_wire_data_multi(uint8_t *buf,
                               size_t cap,
                               const nb_rtps_header_t *header,
                               const nb_rtps_time_t *ts,
                               const nb_rtps_guid_prefix_t *dst,
                               const nb_rtps_data_t *items,
                               size_t count);

size_t nb_rtps_wire_data_frag(uint8_t *buf,
                              size_t cap,
                              const nb_rtps_header_t *header,
                              const nb_rtps_time_t *ts,
                              const nb_rtps_guid_prefix_t *dst,
                              const nb_rtps_data_frag_t *fragment);

size_t nb_rtps_wire_heartbeat(uint8_t *buf,
                              size_t cap,
                              const nb_rtps_header_t *header,
                              const nb_rtps_time_t *ts,
                              const nb_rtps_guid_prefix_t *dst,
                              const nb_rtps_heartbeat_t *heartbeat);

size_t nb_rtps_wire_heartbeat_frag(uint8_t *buf,
                                   size_t cap,
                                   const nb_rtps_header_t *header,
                                   const nb_rtps_time_t *ts,
                                   const nb_rtps_guid_prefix_t *dst,
                                   const nb_rtps_heartbeat_frag_t *heartbeat);

size_t nb_rtps_wire_acknack(uint8_t *buf,
                            size_t cap,
                            const nb_rtps_header_t *header,
                            const nb_rtps_time_t *ts,
                            const nb_rtps_guid_prefix_t *dst,
                            const nb_rtps_acknack_t *acknack);

size_t nb_rtps_wire_gap(uint8_t *buf,
                        size_t cap,
                        const nb_rtps_header_t *header,
                        const nb_rtps_time_t *ts,
                        const nb_rtps_guid_prefix_t *dst,
                        const nb_rtps_gap_t *gap);

size_t nb_rtps_wire_nack_frag(uint8_t *buf,
                              size_t cap,
                              const nb_rtps_header_t *header,
                              const nb_rtps_time_t *ts,
                              const nb_rtps_guid_prefix_t *dst,
                              const nb_rtps_nack_frag_t *nack);

#endif /* NB_RTPS_WIRE_H */
