#ifndef NB_STUB_INTERNAL_H
#define NB_STUB_INTERNAL_H

/*
 * Stub: the internal call engine under proxy.
 *
 * This is not part of the public API. The public client surface is
 * <nb/proxy.h>; proxy.c installs its routing send hook here, and nothing
 * outside src/core may include this header. Providers that need a custom
 * transport extend <nb/transport_provider.h> instead of this layer.
 */

#include <nb/types.h>
#include <nb/endpoint.h>

#include <stddef.h>
#include <stdint.h>

typedef struct nb_stub nb_stub_t;

/* Target of one call; the send hook fills it in (used for retries). */
typedef nb_endpoint_t nb_stub_target_t;

typedef enum nb_stub_kind {
    NB_STUB_KIND_REQUEST = 0, /* expects a response */
    NB_STUB_KIND_EVENT = 1,   /* one-way; does not wait for a response */
    NB_STUB_KIND_CANCEL = 2   /* control-plane cancel; does not wait for a response */
} nb_stub_kind_t;

/* Immutable description of one call plus a fill-in target endpoint.
 * metadata/metadata_len carry the encoded nb_metadata wire form; the proxy
 * layer fills them from nb_metadata_t. */
typedef struct nb_stub_request {
    const char *service;
    const char *method;
    const nb_payload_t *payload;
    nb_stub_kind_t kind;
    nb_request_id_t request_id;
    uint64_t timeout_ms;
    const void *metadata;
    uint16_t metadata_len;
    nb_endpoint_t *target; /* unresolvable when NULL on input; filled on send success */
} nb_stub_request_t;

/* Single-message send hook. Implementations must resolve the target, fill
 * request->target, and perform the real send. The payload is valid only for
 * the duration of this call. Returns NB_OK or an error code. */
typedef int (*nb_stub_send_fn)(nb_stub_t *stub,
                               const nb_stub_request_t *request,
                               void *user);

typedef struct nb_stub_options {
    uint8_t payload_encoding; /* default encoding tag for requests; 0 = raw bytes */
    nb_stub_send_fn send;     /* NULL uses the node's default routing */
    void *user;               /* passed to send; the stub does not free it */
} nb_stub_options_t;

struct nb_stub {
    nb_node_t *node;
    uint8_t payload_encoding;
    nb_stub_send_fn send;
    void *user;
};

/* ---- Lifecycle ---- */
int nb_stub_create(nb_node_t *node, const nb_stub_options_t *options, nb_stub_t **out);
void nb_stub_destroy(nb_stub_t *stub);

/* Borrow the underlying node. */
nb_node_t *nb_stub_node(const nb_stub_t *stub);

/* ---- Calls (proxy-facing subset) ---- */

/* Request with optional streaming callbacks. */
int nb_stub_call_ex(nb_stub_t *stub,
                    const char *service,
                    const char *method,
                    const nb_payload_t *payload,
                    response_fn cb,
                    response_chunk_fn chunk_cb,
                    stream_done_fn done_cb,
                    int streaming,
                    void *user,
                    uint64_t timeout_ms,
                    nb_request_id_t *out_request_id,
                    const nb_metadata_t *metadata);

/* Request that takes ownership of the payload at entry. */
int nb_stub_call_owned(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       nb_owned_payload_t *payload,
                       const nb_metadata_t *metadata,
                       response_fn cb,
                       response_chunk_fn chunk_cb,
                       stream_done_fn done_cb,
                       bool streaming,
                       void *user,
                       uint64_t timeout_ms,
                       nb_request_id_t *out_request_id);

/* One-way event. */
int nb_stub_call_event_with_metadata(nb_stub_t *stub,
                                     const char *service,
                                     const char *method,
                                     const nb_payload_t *payload,
                                     const nb_metadata_t *metadata,
                                     nb_request_id_t *out_request_id);
int nb_stub_call_event_owned_with_metadata(nb_stub_t *stub,
                                           const char *service,
                                           const char *method,
                                           nb_owned_payload_t *payload,
                                           const nb_metadata_t *metadata,
                                           nb_request_id_t *out_request_id);

/* Cancel every unfinished call owned by this stub (called before destroy so a
 * late callback cannot touch freed proxy/stream state). */
int nb_stub_cancel_all(nb_stub_t *stub, int status);

#endif /* NB_STUB_INTERNAL_H */
