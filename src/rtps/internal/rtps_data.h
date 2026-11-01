#ifndef NB_RTPS_DATA_H
#define NB_RTPS_DATA_H

/*
 * Application DATA payload for the RTPS data plane.
 *
 * The wire type is the standard DDS IDL type:
 *
 *   module dds {
 *     struct Message {
 *       sequence<octet> payload;
 *     };
 *   };
 *
 * A payload is a CDR_LE encapsulated XCDR1 value:
 *
 *   00 01 00 00 | uint32 payload_length | payload bytes
 *
 * The four-byte encapsulation header is followed by the CDR sequence length
 * and the raw bytes. No framework-specific framing is added here.
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/error.h>
#include <nb/message.h>

#define NB_RTPS_DDS_MESSAGE_TOPIC "dds/Message"
#define NB_RTPS_DDS_MESSAGE_TYPE "dds::Message"

/* Size of the CDR encapsulation header that prefixes every RTPS sample. */
#define NB_RTPS_CDR_HEADER_SIZE 4u

/*
 * A non-owning view of dds::Message.payload plus the sample metadata that came
 * with the DATA submessage. When produced by nb_rtps_dds_message_deserialize(),
 * payload points into the caller's input buffer and remains valid for as long
 * as that buffer is valid.
 *
 * `sample` is transport metadata and is never serialized: DDS carries it in
 * INFO_TS, the DATA flags and the inline QoS parameter list.
 */
typedef struct nb_rtps_dds_message {
    const uint8_t *payload;
    size_t payload_len;
    nb_sample_info_t sample;
    /* Low 32 bits of the writer sequence number the sample travelled with. */
    uint32_t writer_sn;
} nb_rtps_dds_message_t;

/* Serialize dds::Message with the CDR_LE encapsulation header. The output
 * length is 8 + payload_len. */
int nb_rtps_dds_message_serialize(const nb_rtps_dds_message_t *message,
                                  void *buf,
                                  size_t cap,
                                  size_t *out_len);

/* Deserialize dds::Message without copying payload bytes. */
int nb_rtps_dds_message_deserialize(const void *buf,
                                    size_t len,
                                    nb_rtps_dds_message_t *out);

#endif /* NB_RTPS_DATA_H */
