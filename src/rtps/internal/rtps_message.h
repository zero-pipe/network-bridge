#ifndef NB_RTPS_MESSAGE_H
#define NB_RTPS_MESSAGE_H

/*
 * RTPS message framing for the discovery subset.
 *
 * Messages are written through an nb_cdr_writer_t that starts at the message
 * header, so submessage lengths and 4-byte alignment are handled in one place.
 * Only little-endian messages are produced and accepted.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nb/base/error.h>

#include "rtps_cdr.h"
#include "rtps_types.h"

#define NB_RTPS_HEADER_SIZE 20u

/* Submessage ids. */
#define NB_RTPS_SUBMSG_ACKNACK 0x06u
#define NB_RTPS_SUBMSG_HEARTBEAT 0x07u
#define NB_RTPS_SUBMSG_GAP 0x08u
#define NB_RTPS_SUBMSG_INFO_TS 0x09u
#define NB_RTPS_SUBMSG_INFO_SRC 0x0cu
#define NB_RTPS_SUBMSG_INFO_DST 0x0eu
#define NB_RTPS_SUBMSG_INFO_REPLY 0x0fu
#define NB_RTPS_SUBMSG_NACK_FRAG 0x12u
#define NB_RTPS_SUBMSG_HEARTBEAT_FRAG 0x13u
#define NB_RTPS_SUBMSG_DATA 0x15u
#define NB_RTPS_SUBMSG_DATA_FRAG 0x16u

/* Submessage flags. */
#define NB_RTPS_FLAG_ENDIAN_LITTLE 0x01u
#define NB_RTPS_FLAG_INLINE_QOS 0x02u
#define NB_RTPS_FLAG_DATA 0x04u
#define NB_RTPS_FLAG_KEY 0x08u

/* ACKNACK submessage flag: no heartbeat response is required. */
#define NB_RTPS_ACKNACK_FLAG_FINAL 0x02u

/* HEARTBEAT submessage flags: 0x02 is FINAL and 0x04 is the liveliness
 * assertion (RTPS 2.5 8.3.7.5.1); they are not interchangeable. A
 * manual-by-topic writer asserts itself with 0x04. */
#define NB_RTPS_FLAG_HEARTBEAT_FINAL 0x02u
#define NB_RTPS_FLAG_HEARTBEAT_LIVELINESS 0x04u

/* DATA_FRAG reuses the low flags for E/Q/K, where K is 0x04 (not the DATA
 * submessage's 0x08). */
#define NB_RTPS_FLAG_FRAG_KEY 0x04u

typedef struct nb_rtps_header {
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t vendor_id[2];
    nb_rtps_guid_prefix_t guid_prefix;
} nb_rtps_header_t;

typedef struct nb_rtps_submessage_view {
    uint8_t id;
    uint8_t flags;
    const uint8_t *body;
    size_t body_len;
} nb_rtps_submessage_view_t;

typedef struct nb_rtps_data {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t writer_sn;
    const uint8_t *inline_qos;
    size_t inline_qos_len;
    const uint8_t *payload;
    size_t payload_len;
    int dispose;
    int key;
} nb_rtps_data_t;

typedef struct nb_rtps_data_view {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t writer_sn;
    const uint8_t *inline_qos;
    size_t inline_qos_len;
    const uint8_t *payload;
    size_t payload_len;
    int dispose;
    int key;
    /* Source timestamp from the INFO_TS submessage that preceded this DATA, when
     * one was present. Transport-level, not part of the decoded DATA body. */
    int has_source_ts;
    nb_rtps_time_t source_ts;
} nb_rtps_data_view_t;

/*
 * DATA_FRAG carries one or more fragments of a sample that is too large for a
 * single DATA submessage. fragmentStartingNum is 1-based, fragmentSize is the
 * size of every fragment except possibly the last one, and sampleSize is the
 * size of the whole serialized payload.
 */
typedef struct nb_rtps_data_frag {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t writer_sn;
    uint32_t fragment_starting_num;
    uint16_t fragments_in_submessage;
    uint16_t fragment_size;
    uint32_t sample_size;
    const uint8_t *inline_qos;
    size_t inline_qos_len;
    const uint8_t *payload;
    size_t payload_len;
    int key;
} nb_rtps_data_frag_t;

#define NB_RTPS_ACKNACK_MAX_BITS 256u
#define NB_RTPS_ACKNACK_MAX_WORDS (NB_RTPS_ACKNACK_MAX_BITS / 32u)

typedef struct nb_rtps_heartbeat {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t first_sn;
    nb_rtps_sequence_number_t last_sn;
    uint32_t count;
    /* Receive side: set from the submessage flags by the decoder. Send
     * side: asserts liveliness when set. */
    int liveliness;
    /* Receive side: the writer wants an acknowledgement (FINAL clear). Send
     * side: set to ask for one. A periodic heartbeat leaves this at zero and
     * is encoded with FINAL, so a reader that has nothing missing does not
     * answer it; Cyclone DDS does the same. */
    int ack_requested;
} nb_rtps_heartbeat_t;

typedef struct nb_rtps_acknack {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t base;
    uint32_t num_bits;
    uint32_t bitmap[NB_RTPS_ACKNACK_MAX_WORDS];
    uint32_t count;
} nb_rtps_acknack_t;

/*
 * NACK_FRAG asks a writer to resend individual fragments of one sample;
 * HEARTBEAT_FRAG tells a reader how many fragments that sample has. Fragment
 * numbers are 1-based and independent of sequence numbers. The bitmap uses the
 * same MSB-first convention as ACKNACK: bit 0 is 0x80000000.
 */
#define NB_RTPS_FRAG_NUMBER_SET_MAX_BITS 256u
#define NB_RTPS_FRAG_NUMBER_SET_MAX_WORDS (NB_RTPS_FRAG_NUMBER_SET_MAX_BITS / 32u)

typedef struct nb_rtps_nack_frag {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t writer_sn;
    uint32_t bitmap_base;
    uint32_t num_bits;
    uint32_t bitmap[NB_RTPS_FRAG_NUMBER_SET_MAX_WORDS];
    uint32_t count;
} nb_rtps_nack_frag_t;

typedef struct nb_rtps_heartbeat_frag {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t writer_sn;
    uint32_t last_fragment_num;
    uint32_t count;
} nb_rtps_heartbeat_frag_t;

/*
 * GAP announces samples that the writer will never send to a reader. gap_start
 * is always part of the gap; the sequence-number set (base + bitmap) lists
 * additional gap samples. As in ACKNACK, bit 0 of a word is the most
 * significant bit.
 */
typedef struct nb_rtps_gap {
    nb_rtps_entity_id_t reader_id;
    nb_rtps_entity_id_t writer_id;
    nb_rtps_sequence_number_t gap_start;
    nb_rtps_sequence_number_t base;
    uint32_t num_bits;
    uint32_t bitmap[NB_RTPS_ACKNACK_MAX_WORDS];
} nb_rtps_gap_t;

int nb_rtps_header_encode(uint8_t *buf,
                          size_t cap,
                          size_t *out_len,
                          const nb_rtps_header_t *header);
int nb_rtps_header_decode(const uint8_t *buf, size_t len, nb_rtps_header_t *header);

/* Write the 20-byte message header into writer. */
int nb_rtps_message_init(nb_cdr_writer_t *writer,
                         void *buf,
                         size_t cap,
                         const nb_rtps_header_t *header);

/* Reserve a submessage header and return its offset, or SIZE_MAX on error. */
size_t nb_rtps_submessage_begin(nb_cdr_writer_t *writer, uint8_t id, uint8_t flags);
/* Pad the submessage to 4 bytes and fill octetsToNextHeader. */
int nb_rtps_submessage_end(nb_cdr_writer_t *writer, size_t start_offset);

int nb_rtps_info_ts(nb_cdr_writer_t *writer, const nb_rtps_time_t *ts, bool invalidate);

/* Decode an INFO_TS body. The invalidation flag carries no body at all, so a
 * short body is reported as a bad message; destination ordering needs the
 * timestamp, and a sample without one keeps the order it arrived in. */
int nb_rtps_info_ts_decode(uint8_t flags, const uint8_t *body, size_t body_len,
                           nb_rtps_time_t *out);
int nb_rtps_info_dst(nb_cdr_writer_t *writer, const nb_rtps_guid_prefix_t *prefix);
/* Write INFO_SRC (four unused octets followed by the GuidPrefix). */
int nb_rtps_info_src(nb_cdr_writer_t *writer, const nb_rtps_guid_prefix_t *prefix);
/* Decode INFO_DST / INFO_SRC. INFO_DST carries a 12-octet GuidPrefix;
 * INFO_SRC carries four unused octets followed by the prefix. */
int nb_rtps_info_dst_decode(uint8_t flags,
                            const uint8_t *body,
                            size_t body_len,
                            nb_rtps_guid_prefix_t *out);
int nb_rtps_info_src_decode(uint8_t flags,
                            const uint8_t *body,
                            size_t body_len,
                            nb_rtps_guid_prefix_t *out);

/*
 * Per-message interpreter state. INFO_SRC / INFO_DST / INFO_TS apply to the
 * Entity Submessages that follow them until the next one of the same kind (or
 * the end of the RTPS message). The UDP and TCP receive paths both use this
 * struct so their interpretation is identical.
 */
typedef struct nb_rtps_receiver_state {
    int has_source_ts;
    nb_rtps_time_t source_ts;
    int has_source_prefix;
    nb_rtps_guid_prefix_t source_prefix;
    int has_dst_prefix;
    nb_rtps_guid_prefix_t dst_prefix;
} nb_rtps_receiver_state_t;


int nb_rtps_data(nb_cdr_writer_t *writer, const nb_rtps_data_t *data);
int nb_rtps_data_decode(uint8_t flags,
                        const uint8_t *body,
                        size_t body_len,
                        nb_rtps_data_view_t *out);

/* Size of the instance handle carried by PID_KEY_HASH. */
#define NB_RTPS_KEY_HASH_SIZE 16u

/*
 * Decode the status parameters a writer may put in a DATA inline QoS:
 * PID_STATUS_INFO (disposed / unregistered bits) and PID_KEY_HASH (the
 * instance handle of the sample). Absent parameters leave the outputs cleared;
 * a malformed list is reported as NB_ERR_BAD_MESSAGE and never fails the
 * sample, because the payload already arrived.
 */
int nb_rtps_inline_qos_status(const uint8_t *qos,
                              size_t len,
                              int *disposed,
                              int *unregistered,
                              uint8_t key_hash[NB_RTPS_KEY_HASH_SIZE],
                              int *has_key_hash);

int nb_rtps_data_frag(nb_cdr_writer_t *writer, const nb_rtps_data_frag_t *fragment);
int nb_rtps_data_frag_decode(uint8_t flags,
                             const uint8_t *body,
                             size_t body_len,
                             nb_rtps_data_frag_t *out);

int nb_rtps_heartbeat(nb_cdr_writer_t *writer, const nb_rtps_heartbeat_t *heartbeat);
int nb_rtps_heartbeat_decode(uint32_t flags,
                             const uint8_t *body,
                             size_t body_len,
                             nb_rtps_heartbeat_t *out);

int nb_rtps_acknack(nb_cdr_writer_t *writer, const nb_rtps_acknack_t *acknack);
int nb_rtps_acknack_decode(const uint8_t *body, size_t body_len, nb_rtps_acknack_t *out);

int nb_rtps_nack_frag(nb_cdr_writer_t *writer, const nb_rtps_nack_frag_t *nack);
int nb_rtps_nack_frag_decode(const uint8_t *body, size_t body_len, nb_rtps_nack_frag_t *out);

int nb_rtps_heartbeat_frag(nb_cdr_writer_t *writer,
                           const nb_rtps_heartbeat_frag_t *heartbeat);
int nb_rtps_heartbeat_frag_decode(const uint8_t *body,
                                  size_t body_len,
                                  nb_rtps_heartbeat_frag_t *out);

int nb_rtps_gap(nb_cdr_writer_t *writer, const nb_rtps_gap_t *gap);
int nb_rtps_gap_decode(const uint8_t *body, size_t body_len, nb_rtps_gap_t *out);

/*
 * ParticipantMessageData is the payload of the built-in liveliness writer
 * (entity id 0x000200c2). It is a plain CDR sample, not a dedicated submessage,
 * so it travels inside DATA exactly like an application sample. The sample
 * carries the sender's participant prefix, the assertion kind, and a payload
 * octet sequence the spec leaves to the kind. Peers use it to renew a
 * participant's automatic liveliness lease.
 */
#define NB_RTPS_PMD_KIND_AUTOMATIC 1u
#define NB_RTPS_PMD_KIND_MANUAL 2u

typedef struct nb_rtps_pmd {
    nb_rtps_guid_prefix_t participant;
    uint32_t kind;
} nb_rtps_pmd_t;

int nb_rtps_pmd_encode(const nb_rtps_pmd_t *pmd, void *buf, size_t cap, size_t *out_len);
int nb_rtps_pmd_decode(const void *buf, size_t len, nb_rtps_pmd_t *out);

int nb_rtps_message_begin(const uint8_t *buf, size_t len, nb_rtps_header_t *header,
                          size_t *offset);
/* Returns 1 for a submessage, 0 at the end of the message, or NB_ERR_* on
 * malformed input. */
int nb_rtps_message_next(const uint8_t *buf,
                         size_t len,
                         size_t *offset,
                         nb_rtps_submessage_view_t *view);

#endif /* NB_RTPS_MESSAGE_H */
