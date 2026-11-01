#ifndef NB_PROXY_H
#define NB_PROXY_H

/*
 * Client-side proxy: sync, async, event, and streaming call forms.
 *
 * One node may host both a service (server side) and a proxy (client side), or
 * only one of them. Discovery and routing happen inside the framework; the
 * proxy only sends payloads and routes responses back to callbacks.
 *
 * Threading contract: call_async / call_event may be called from any thread.
 * When the caller is already on one of the node's poller threads (or on its
 * dedicated send lane, see nb_node_post_send) the send runs inline on that
 * thread; otherwise it is posted to a poller. call_sync blocks until a
 * response and must not be used inside a poller callback.
 *
 * Name suffix convention:
 *   _owned          takes an nb_owned_payload_t and takes ownership of it
 *
 * Every call form takes an optional nb_call_options_t carrying the timeout,
 * request metadata, and correlation-id output; a NULL options pointer means
 * all defaults.
 */

#include <nb/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_proxy_options {
    uint8_t payload_encoding;         /* default encoding tag for requests; 0 = raw bytes */
    uint32_t prefer_stream_threshold; /* 0 = disabled; larger payloads prefer TCP */
} nb_proxy_options_t;

/* ---- Per-call options ---- */
/*
 * Options shared by every proxy call form. NULL options means all defaults.
 *
 *   timeout_ms      0 = NB_DEFAULT_TIMEOUT_MS for request forms; ignored by
 *                   event forms (no response to wait for). Stream opens use it
 *                   as the stream deadline.
 *   metadata        request metadata; NULL = none. Read before the call
 *                   returns and copied, so it may be released afterwards.
 *   out_request_id  optional; receives the correlation id for nb_proxy_cancel.
 *                   Not filled by the open_* stream forms, which allocate
 *                   their own stream ids.
 */
typedef struct nb_call_options {
    uint64_t timeout_ms;
    const nb_metadata_t *metadata;
    nb_request_id_t *out_request_id;
} nb_call_options_t;

#define NB_CALL_OPTIONS_INIT { 0, NULL, NULL }

/*
 * Client interceptor (the minimal gRPC unary client interceptor shape).
 * before runs on the calling thread and may modify metadata / timeout; a
 * non-NB_OK return short-circuits the call. after runs on the poller thread,
 * before the user callback, and observes the final status and response
 * metadata.
 */
typedef struct nb_proxy_interceptor {
    void *user;
    int (*before)(void *user,
                  const char *service,
                  const char *method,
                  nb_metadata_t *metadata,
                  uint64_t *timeout_ms);
    void (*after)(void *user, int status, const nb_response_t *resp);
} nb_proxy_interceptor_t;

/* Install/clear the client interceptor; NULL clears it. The struct is copied;
 * user and the callbacks must stay valid for the proxy's lifetime. */
int nb_proxy_set_interceptor(nb_proxy_t *proxy, const nb_proxy_interceptor_t *interceptor);

/* ---- Lifecycle ---- */
/* options may be NULL for defaults. */
int nb_proxy_create(nb_node_t *node, const nb_proxy_options_t *options, nb_proxy_t **out);
void nb_proxy_destroy(nb_proxy_t *proxy);

/* ---- Target endpoint ---- */
/*
 * Point the proxy directly at a data endpoint (host:port + transport kind) and
 * skip service discovery. Suited to high-performance callers that already know
 * the peer address.
 */
int nb_proxy_set_target(nb_proxy_t *proxy,
                        const char *host,
                        uint16_t port,
                        nb_transport_kind_t transport);
/* Point the proxy directly at a target URI (for example tcp://127.0.0.1:8080,
 * dds://..., shm://...) and skip service discovery. */
int nb_proxy_set_target_uri(nb_proxy_t *proxy, const char *endpoint);

/* ---- Unary calls ---- */
/*
 * Asynchronous request: discover, send, and return immediately; the response
 * or timeout arrives through cb (on the poller thread). options may be NULL;
 * see nb_call_options_t.
 *
 * Payload ownership: the framework copies the payload before returning
 * (including cross-thread calls); the caller may release payload->data as soon
 * as this function returns. The framework never takes or frees it.
 */
int nb_proxy_call_async(nb_proxy_t *proxy,
                        const char *service,
                        const char *method,
                        const nb_payload_t *payload,
                        const nb_call_options_t *options,
                        nb_response_fn cb,
                        void *user);
int nb_proxy_call_async_owned(nb_proxy_t *proxy,
                              const char *service,
                              const char *method,
                              nb_owned_payload_t *payload,
                              const nb_call_options_t *options,
                              nb_response_fn cb,
                              void *user);

/*
 * One-way event: send only, never wait for a response (EVENT). Delivery runs
 * on the node poller thread, or inline when the caller is already on a poller
 * or send-lane thread. As with nb_proxy_call_async, the payload is copied
 * before returning and the caller may release it immediately.
 */
int nb_proxy_call_event(nb_proxy_t *proxy,
                        const char *service,
                        const char *method,
                        const nb_payload_t *payload,
                        const nb_call_options_t *options);
int nb_proxy_call_event_owned(nb_proxy_t *proxy,
                              const char *service,
                              const char *method,
                              nb_owned_payload_t *payload,
                              const nb_call_options_t *options);

/*
 * Synchronous request: block until a response, error, or timeout. On success
 * response->payload is owned by an internal buffer; release it with
 * nb_response_release(). Must not be called inside a poller callback.
 */
int nb_proxy_call_sync(nb_proxy_t *proxy,
                       const char *service,
                       const char *method,
                       const nb_payload_t *payload,
                       const nb_call_options_t *options,
                       nb_response_t *response);

/* Release the response buffer filled by nb_proxy_call_sync and zero it. */
void nb_response_release(nb_response_t *response);

/* ---- Call control ---- */
/* Cancel an unfinished call; returns NB_ERR_NOTFOUND if it already completed. */
int nb_proxy_cancel(nb_proxy_t *proxy, nb_request_id_t request_id);

/* ---- Streaming calls ---- */
/* Server streaming: a unary request with a chunked response, until NB_STREAM_FLAG_LAST.
 * options supplies the timeout and, optionally, out_request_id. */
int nb_proxy_call_server_stream(nb_proxy_t *proxy,
                                const char *service,
                                const char *method,
                                const nb_payload_t *payload,
                                const nb_call_options_t *options,
                                nb_response_chunk_fn on_chunk,
                                nb_stream_done_fn on_done,
                                void *user);

/* Client streaming: request chunks are sent via nb_proxy_stream_send and a
 * single response arrives through cb. options is used for the timeout only. */
int nb_proxy_open_client_stream(nb_proxy_t *proxy,
                                const char *service,
                                const char *method,
                                const nb_call_options_t *options,
                                nb_response_fn cb,
                                void *user,
                                nb_stream_t **out_stream);
/* Bidirectional streaming: request chunks are sent while response chunks arrive.
 * options is used for the timeout only. */
int nb_proxy_open_bidi_stream(nb_proxy_t *proxy,
                              const char *service,
                              const char *method,
                              const nb_call_options_t *options,
                              nb_response_chunk_fn on_chunk,
                              nb_stream_done_fn on_done,
                              void *user,
                              nb_stream_t **out_stream);

/*
 * Send one request chunk; the final chunk must carry NB_STREAM_FLAG_LAST.
 * The chunk may be released as soon as send returns; the framework copies
 * whatever it needs.
 */
int nb_proxy_stream_send(nb_stream_t *stream, const nb_chunk_t *chunk);
int nb_proxy_stream_cancel(nb_stream_t *stream);
int nb_proxy_stream_destroy(nb_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* NB_PROXY_H */
