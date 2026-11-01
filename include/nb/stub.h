#ifndef NB_STUB_H
#define NB_STUB_H

/*
 * Stub: transport-independent low-level send primitive.
 *
 * Mirrors the fc_stub layering of fastcall-c: the node owns lifecycle and
 * transport, the stub assembles a service.method + payload call and tracks its
 * correlation (request_id), and a proxy layers service discovery/routing on top
 * (see proxy.h).
 *
 * When no send hook is injected, the stub uses the node's default routing
 * (static route table + service discovery).
 */

#include <nb/types.h>
#include <nb/endpoint.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Target and send hook ---- */
typedef struct nb_stub nb_stub_t;

/* Target of one call; the send hook fills it in (used for retries). */
typedef nb_endpoint_t nb_stub_target_t;

typedef enum nb_stub_kind {
    NB_STUB_KIND_REQUEST = 0, /* expects a response */
    NB_STUB_KIND_EVENT = 1,   /* one-way; does not wait for a response */
    NB_STUB_KIND_CANCEL = 2   /* control-plane cancel; does not wait for a response */
} nb_stub_kind_t;

/* Immutable description of one call plus a fill-in target endpoint.
 * metadata/metadata_len carry the encoded nb_metadata wire form (see
 * nb_metadata_encode); the proxy layer fills them from nb_metadata_t. */
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

/*
 * Low-level single-message send hook. Implementations must resolve the target,
 * fill request->target, and perform the real send. The payload is valid only
 * for the duration of this call. Returns NB_OK or an error code.
 */
typedef int (*nb_stub_send_fn)(nb_stub_t *stub,
                            const nb_stub_request_t *request,
                            void *user);
typedef nb_stub_send_fn stub_send_fn;

typedef struct nb_stub_options {
    uint8_t payload_encoding; /* default encoding tag for requests; 0 = raw bytes */
    stub_send_fn send;        /* NULL uses the node's default routing */
    void *user;               /* passed to send; the stub does not free it */
} nb_stub_options_t;

/*
 * Options for the owned-payload call form. cb is used for unary responses;
 * when streaming is true, chunk_cb/done_cb receive the response stream instead.
 * NULL metadata means none. timeout_ms 0 selects NB_DEFAULT_TIMEOUT_MS.
 */
typedef struct nb_stub_call_options {
    uint64_t timeout_ms;
    const nb_metadata_t *metadata;
    nb_request_id_t *out_request_id;
    nb_response_fn cb;
    nb_response_chunk_fn chunk_cb;
    nb_stream_done_fn done_cb;
    bool streaming;
    void *user;
} nb_stub_call_options_t;

#define NB_STUB_CALL_OPTIONS_INIT { 0, NULL, NULL, NULL, NULL, NULL, false, NULL }

/* ---- Lifecycle ---- */
int nb_stub_create(nb_node_t *node, const nb_stub_options_t *options, nb_stub_t **out);
void nb_stub_destroy(nb_stub_t *stub);

/* Borrow the underlying node. */
nb_node_t *nb_stub_node(const nb_stub_t *stub);

/* ---- Calls ---- */
/* One-way send (EVENT). Does not wait for a response. Runs inline on the
 * caller when it is already on a poller/send-lane thread, otherwise on the
 * node poller thread. */
int nb_stub_call_event(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       nb_request_id_t *out_request_id);
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

/*
 * Asynchronous request. Returns immediately after discovery and send; the
 * response or timeout arrives through cb (on the poller thread).
 * timeout_ms: 0 selects NB_DEFAULT_TIMEOUT_MS.
 * The payload is copied before returning (also across threads), so the caller
 * may release its own buffer immediately.
 */
int nb_stub_call_async(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       nb_response_fn cb,
                       void *user,
                       uint64_t timeout_ms,
                       nb_request_id_t *out_request_id);
/* Asynchronous request with metadata; NULL metadata is equivalent to
 * nb_stub_call_async. */
int nb_stub_call_async_with_metadata(nb_stub_t *stub,
                                     const char *service,
                                     const char *method,
                                     const nb_payload_t *payload,
                                     const nb_metadata_t *metadata,
                                     nb_response_fn cb,
                                     void *user,
                                     uint64_t timeout_ms,
                                     nb_request_id_t *out_request_id);

/*
 * Owned-payload call with optional streaming callbacks. metadata may be NULL.
 * When streaming is false, cb receives the single response; when true,
 * chunk_cb/done_cb receive response chunks instead. The stub takes ownership of
 * payload at entry even when the call fails.
 */
int nb_stub_call_owned(nb_stub_t *stub,
                       const char *service,
                       const char *method,
                       nb_owned_payload_t *payload,
                       const nb_metadata_t *metadata,
                       nb_response_fn cb,
                       nb_response_chunk_fn chunk_cb,
                       nb_stream_done_fn done_cb,
                       bool streaming,
                       void *user,
                       uint64_t timeout_ms,
                       nb_request_id_t *out_request_id);
/* Preferred form of nb_stub_call_owned; the options struct keeps the call site
 * stable as more per-call settings are added. */
int nb_stub_call_owned_ex(nb_stub_t *stub,
                          const char *service,
                          const char *method,
                          nb_owned_payload_t *payload,
                          const nb_stub_call_options_t *options);

#ifdef __cplusplus
}
#endif

/* ---- Deprecated enum spellings ---------------------------------------- */
#ifndef NB_NO_DEPRECATED_API
#define NB_STUB_REQUEST  NB_STUB_KIND_REQUEST
#define NB_STUB_EVENT    NB_STUB_KIND_EVENT
#define NB_STUB_CANCEL   NB_STUB_KIND_CANCEL
#endif /* NB_NO_DEPRECATED_API */

#endif /* NB_STUB_H */
