#ifndef NB_MESSAGE_H
#define NB_MESSAGE_H

/*
 * Unified message context shared by the core and transport providers.
 *
 * A message crosses the transport/core boundary exactly once in this shape:
 * inbound transports fill peer + payload/route, outbound core fills target and
 * payload. This is also the public provider-SPI message type, so adding a
 * field no longer requires a parallel adapter struct that can silently drift.
 */

#include <nb/buf.h>
#include <nb/endpoint.h>
#include <nb/types.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque token a provider uses to route a reply back to its peer. */
typedef void *nb_reply_token_t;

/* Width of a DDS instance handle (a key hash), in octets. */
#define NB_INSTANCE_HANDLE_SIZE 16

/*
 * State of an inbound DDS sample. VALID is zero on purpose: a provider that
 * does not model samples leaves nb_sample_info_t zeroed, and a zeroed sample
 * must read as ordinary data rather than as a dispose.
 */
typedef enum nb_sample_state {
    NB_SAMPLE_STATE_VALID = 0,      /* normal sample, DDS valid_data = true */
    NB_SAMPLE_STATE_DISPOSED = 1,   /* instance disposed, valid_data = false */
    NB_SAMPLE_STATE_NO_WRITERS = 2  /* instance lost its writer, valid_data = false */
} nb_sample_state_t;

/* DDS ViewStateKind / InstanceStateKind. Both keep the DDS numeric values and
 * default to zero, so a zeroed nb_sample_info_t still reads as "alive, not
 * new". */
typedef enum nb_view_state {
    NB_VIEW_STATE_NOT_NEW = 0,
    NB_VIEW_STATE_NEW = 1
} nb_view_state_t;

typedef enum nb_instance_state {
    NB_INSTANCE_STATE_ALIVE = 0,
    NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED = 1,
    NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS = 2
} nb_instance_state_t;

/*
 * DDS sample metadata attached to an inbound message. Only the RTPS provider
 * fills it today; the other providers leave it zeroed, which reads as
 * "valid data, no instance handle, no source timestamp".
 */
typedef struct nb_sample_info {
    nb_sample_state_t state;
    bool has_instance_handle;
    uint8_t instance_handle[NB_INSTANCE_HANDLE_SIZE];
    /* Writer source timestamp (INFO_TS) in nanoseconds; 0 when the peer sent
     * none. */
    int64_t source_timestamp_ns;
    /* Reader reception timestamp in nanoseconds, filled by the provider that
     * received the sample. 0 when the provider does not stamp it. */
    int64_t reception_timestamp_ns;
    /* Per-instance state as the reader knows it: NEW until the first sample of
     * an instance has been delivered, ALIVE while the writer keeps the instance
     * registered, and a NOT_ALIVE_* state after a dispose or unregister. */
    nb_instance_state_t instance_state;
    nb_view_state_t view_state;
    /* DDS generationRank: 0 for the first sample of an instance the reader
     * sees, incremented whenever the instance is re-registered. */
    int32_t generation_rank;
} nb_sample_info_t;

/* DDS `SampleInfo::valid_data`: false only for a state-change notification. */
static inline bool nb_sample_info_valid_data(const nb_sample_info_t *sample)
{
    return !sample || sample->state == NB_SAMPLE_STATE_VALID;
}

/* Envelope-level message kind, stored in nb_message_t.kind. Internal code may
 * use the short KIND_* aliases from envelope.h. */
typedef enum nb_message_kind {
    NB_MSG_KIND_REQUEST = 0,
    NB_MSG_KIND_RESPONSE = 1,
    NB_MSG_KIND_EVENT = 2,
    NB_MSG_KIND_CANCEL = 3, /* client cancel: locate the call by request_id + peer */
    NB_MSG_KIND_GOAWAY = 4, /* peer stops accepting new requests */
    NB_MSG_KIND_PING = 5    /* control-plane heartbeat; echoed back unchanged */
} nb_message_kind_t;

typedef struct nb_message {
    uint8_t kind;   /* nb_message_kind_t value */
    uint8_t flags;  /* NB_MSG_FLAG_*; forwarded end-to-end in the envelope */
    int status;     /* RESPONSE: application status, or a framework code when flagged */
    nb_request_id_t request_id; /* request/response correlation id */
    uint32_t timeout_ms; /* remaining request timeout in ms (32-bit on the wire); 0 = no deadline */
    const void *metadata;
    uint16_t metadata_len;
    const char *status_msg;
    uint16_t status_msg_len;
    const char *route; /* "service.method"; may be NULL */
    nb_payload_t payload;
    nb_stream_meta_t stream;
    bool has_stream;

    /* Inbound source, set by the provider; valid only during the callback. */
    const nb_endpoint_t *peer;
    /* Outbound target, set by the core; valid only for this send. */
    const nb_endpoint_t *target;

    /* Address snapshot derived from peer. Kept for hot paths (rate limiting,
     * cancel registration, built-in UDP/TCP transports). New code should read
     * peer instead. */
    const char *peer_host;
    uint16_t peer_port;
    nb_reply_token_t reply_token;
    nb_buf_t *frame_owner;
    /*
     * Pub/sub sample metadata. Providers that model samples (DDS/RTPS) point
     * this at storage valid for the callback; the pointer is NULL for RPC-only
     * messages and for providers that do not model samples, which reads as
     * ordinary data (NB_SAMPLE_STATE_VALID is zero).
     *
     * Kept as a pointer on purpose: the DDS instance/view state must not make
     * every RPC message carry it.
     */
    const nb_sample_info_t *ps_sample;
} nb_message_t;

#ifdef __cplusplus
}
#endif

#endif /* NB_MESSAGE_H */
