#ifndef NB_RTPS_PARAM_H
#define NB_RTPS_PARAM_H

/*
 * RTPS ParameterList encoding.
 *
 * A ParameterList is a sequence of Parameter entries followed by PID_SENTINEL.
 * Each entry is: ParameterId (u16 LE), length (u16 LE), value, zero padding to
 * a 4-byte boundary. The stored length includes the padding and is always a
 * multiple of 4, matching Cyclone DDS. The value itself is a little-endian CDR
 * stream whose alignment starts at the first byte of the value.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nb/base/error.h>

#include "rtps_types.h"

/* Parameter ids used by SPDP and SEDP. */
#define NB_RTPS_PID_PAD 0x0000u
#define NB_RTPS_PID_SENTINEL 0x0001u
#define NB_RTPS_PID_PARTICIPANT_LEASE_DURATION 0x0002u
#define NB_RTPS_PID_TOPIC_NAME 0x0005u
#define NB_RTPS_PID_TYPE_NAME 0x0007u
#define NB_RTPS_PID_UNICAST_LOCATOR 0x002fu
#define NB_RTPS_PID_MULTICAST_LOCATOR 0x0030u
#define NB_RTPS_PID_PROTOCOL_VERSION 0x0015u
#define NB_RTPS_PID_VENDORID 0x0016u
#define NB_RTPS_PID_RELIABILITY 0x001au
#define NB_RTPS_PID_DURABILITY 0x001du
#define NB_RTPS_PID_PARTITION 0x0029u
#define NB_RTPS_PID_USER_DATA 0x002cu
#define NB_RTPS_PID_DEFAULT_UNICAST_LOCATOR 0x0031u
#define NB_RTPS_PID_METATRAFFIC_UNICAST_LOCATOR 0x0032u
#define NB_RTPS_PID_METATRAFFIC_MULTICAST_LOCATOR 0x0033u
#define NB_RTPS_PID_HISTORY 0x0040u
#define NB_RTPS_PID_DEFAULT_MULTICAST_LOCATOR 0x0048u
#define NB_RTPS_PID_PARTICIPANT_GUID 0x0050u
#define NB_RTPS_PID_BUILTIN_ENDPOINT_SET 0x0058u
#define NB_RTPS_PID_PROPERTY_LIST 0x0059u
#define NB_RTPS_PID_ENDPOINT_GUID 0x005au
#define NB_RTPS_PID_ENTITY_NAME 0x0062u
#define NB_RTPS_PID_KEY_HASH 0x0070u
#define NB_RTPS_PID_STATUS_INFO 0x0071u

/* QoS policies other than reliability, durability, history and partition.
 * The numeric values match DDSI-RTPS 2.5 / Cyclone DDS `ddsi__protocol.h`. */
#define NB_RTPS_PID_OWNERSHIP_STRENGTH 0x0006u
#define NB_RTPS_PID_LIVELINESS 0x001bu
#define NB_RTPS_PID_OWNERSHIP 0x001fu
#define NB_RTPS_PID_PRESENTATION 0x0021u
#define NB_RTPS_PID_DEADLINE 0x0023u
#define NB_RTPS_PID_DESTINATION_ORDER 0x0025u
#define NB_RTPS_PID_LATENCY_BUDGET 0x0027u
#define NB_RTPS_PID_LIFESPAN 0x002bu
#define NB_RTPS_PID_RESOURCE_LIMITS 0x0041u
/* TimeBasedFilter. Cyclone DDS also uses 0x0004, and its readers announce it
 * unconditionally, so parsing it is required for interop. */
#define NB_RTPS_PID_TIME_BASED_FILTER 0x0004u

/* Vendor-specific range, used for the compact nb service record. */
#define NB_RTPS_PID_VENDOR_FIRST 0x8000u
#define NB_RTPS_PID_NB_RECORD 0x8001u
/* Cyclone DDS vendor parameter: a reader that sets it wants PID_KEY_HASH in
 * the inline QoS of every keyed sample (0x8000 | 0x1c). */
#define NB_RTPS_PID_CYCLONE_REQUESTS_KEYHASH 0x801cu

/* Built-in endpoint set bits (DDSI-RTPS 2.x, section 9.3.2). */
#define NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_ANNOUNCER 0x00000001u
#define NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR 0x00000002u
#define NB_RTPS_BUILTIN_ENDPOINT_PUBLICATIONS_ANNOUNCER 0x00000004u
#define NB_RTPS_BUILTIN_ENDPOINT_PUBLICATIONS_DETECTOR 0x00000008u
#define NB_RTPS_BUILTIN_ENDPOINT_SUBSCRIPTIONS_ANNOUNCER 0x00000010u
#define NB_RTPS_BUILTIN_ENDPOINT_SUBSCRIPTIONS_DETECTOR 0x00000020u
#define NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER 0x00000400u
#define NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER 0x00000800u

/* CDR encapsulation identifier used by RTPS discovery payloads. */
#define NB_RTPS_ENCAPSULATION_PL_CDR_LE 0x0003u

typedef struct nb_rtps_param_writer {
    uint8_t *data;
    size_t cap;
    size_t len;
    int failed;
} nb_rtps_param_writer_t;

typedef struct nb_rtps_param_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int failed;
} nb_rtps_param_reader_t;

void nb_rtps_param_writer_init(nb_rtps_param_writer_t *writer, void *buf, size_t cap);
bool nb_rtps_param_writer_ok(const nb_rtps_param_writer_t *writer);
size_t nb_rtps_param_writer_len(const nb_rtps_param_writer_t *writer);

/* Write the 4-byte PL_CDR_LE encapsulation header required by DDSI
 * discovery serialization. */
int nb_rtps_param_put_encapsulation(nb_rtps_param_writer_t *writer);

int nb_rtps_param_put_octets2(nb_rtps_param_writer_t *writer,
                              uint16_t pid,
                              uint8_t first,
                              uint8_t second);
int nb_rtps_param_put_u16(nb_rtps_param_writer_t *writer, uint16_t pid, uint16_t value);
int nb_rtps_param_put_u32(nb_rtps_param_writer_t *writer, uint16_t pid, uint32_t value);
/* ResourceLimits: three uint32 values (max_samples, max_instances,
 * max_samples_per_instance), 0 meaning unlimited. */
int nb_rtps_param_put_resource_limits(nb_rtps_param_writer_t *writer,
                                      uint16_t pid,
                                      uint32_t max_samples,
                                      uint32_t max_instances,
                                      uint32_t max_samples_per_instance);
int nb_rtps_param_put_bytes(nb_rtps_param_writer_t *writer,
                            uint16_t pid,
                            const void *data,
                            size_t len);
int nb_rtps_param_put_octet_sequence(nb_rtps_param_writer_t *writer,
                                     uint16_t pid,
                                     const void *data,
                                     size_t len);
int nb_rtps_param_put_string(nb_rtps_param_writer_t *writer, uint16_t pid, const char *value);
int nb_rtps_param_put_guid(nb_rtps_param_writer_t *writer, uint16_t pid,
                           const nb_rtps_guid_t *guid);
int nb_rtps_param_put_locator(nb_rtps_param_writer_t *writer,
                              uint16_t pid,
                              const nb_rtps_locator_t *locator);
int nb_rtps_param_put_locator_sequence(nb_rtps_param_writer_t *writer,
                                       uint16_t pid,
                                       const nb_rtps_locator_t *locators,
                                       size_t count);
int nb_rtps_param_put_string_sequence(nb_rtps_param_writer_t *writer,
                                      uint16_t pid,
                                      const char *const *values,
                                      size_t count);
int nb_rtps_param_put_history(nb_rtps_param_writer_t *writer,
                              uint16_t pid,
                              uint32_t kind,
                              uint32_t depth);
int nb_rtps_param_put_reliability(nb_rtps_param_writer_t *writer,
                                  uint16_t pid,
                                  uint32_t kind,
                                  const nb_rtps_duration_t *max_blocking_time);
int nb_rtps_param_put_duration(nb_rtps_param_writer_t *writer,
                               uint16_t pid,
                               const nb_rtps_duration_t *duration);
/* Write PID_SENTINEL. Safe to call once; later calls are rejected. */
int nb_rtps_param_writer_finish(nb_rtps_param_writer_t *writer);

void nb_rtps_param_reader_init(nb_rtps_param_reader_t *reader, const void *buf, size_t len);
bool nb_rtps_param_reader_ok(const nb_rtps_param_reader_t *reader);

/* Returns 1 and fills the value view for an entry, 0 on PID_SENTINEL, or a
 * negative NB_ERR_* value on malformed input. */
int nb_rtps_param_reader_next(nb_rtps_param_reader_t *reader,
                              uint16_t *pid,
                              const uint8_t **value,
                              size_t *value_len);

int nb_rtps_param_get_octets2(const uint8_t *value,
                              size_t len,
                              uint8_t *first,
                              uint8_t *second);
int nb_rtps_param_get_u16(const uint8_t *value, size_t len, uint16_t *out);
int nb_rtps_param_get_u32(const uint8_t *value, size_t len, uint32_t *out);
int nb_rtps_param_get_resource_limits(const uint8_t *value,
                                      size_t len,
                                      uint32_t *max_samples,
                                      uint32_t *max_instances,
                                      uint32_t *max_samples_per_instance);
int nb_rtps_param_get_string(const uint8_t *value, size_t len, char *out, size_t cap);
int nb_rtps_param_get_guid(const uint8_t *value, size_t len, nb_rtps_guid_t *out);
int nb_rtps_param_get_locator(const uint8_t *value, size_t len, nb_rtps_locator_t *out);
int nb_rtps_param_get_locator_sequence(const uint8_t *value,
                                       size_t len,
                                       nb_rtps_locator_t *out,
                                       size_t cap,
                                       size_t *count);
int nb_rtps_param_get_first_string(const uint8_t *value,
                                   size_t len,
                                   char *out,
                                   size_t cap);
/* Read a string sequence and join its elements with commas, which is how the
 * rest of the stack represents a partition list. Elements that do not fit are
 * skipped; the output is always NUL-terminated. */
int nb_rtps_param_get_string_sequence_joined(const uint8_t *value,
                                             size_t len,
                                             char *out,
                                             size_t cap);
int nb_rtps_param_get_history(const uint8_t *value,
                              size_t len,
                              uint32_t *kind,
                              uint32_t *depth);
int nb_rtps_param_get_duration(const uint8_t *value, size_t len, nb_rtps_duration_t *out);
int nb_rtps_param_get_octet_sequence(const uint8_t *value,
                                     size_t len,
                                     const uint8_t **data,
                                     size_t *data_len);

#endif /* NB_RTPS_PARAM_H */
