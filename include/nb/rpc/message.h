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

#include <nb/base/buf.h>
#include <nb/rpc/endpoint.h>
#include <nb/rpc/types.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque token a provider uses to route a reply back to its peer. */
typedef void *nb_reply_token_t;

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
} nb_message_t;

#ifdef __cplusplus
}
#endif

#endif /* NB_MESSAGE_H */
