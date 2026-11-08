#ifndef NB_SERVICE_H
#define NB_SERVICE_H

/*
 * Server side: register services/methods and handle requests.
 *
 * A handler receives a request view and must eventually call nb_reply(), either
 * synchronously or asynchronously. The request view (including its payload) is
 * valid only inside the handler; copy anything needed for a later reply.
 *
 * Threading model: the caller selects an execution mode per method through
 * nb_service_method_options_t:
 *   - NB_SERVICE_EXEC_MODE_INLINE (default): runs inline on the ntk poller thread
 *     that received the request. Must not block; blocking stalls every
 *     connection on that poller.
 *   - NB_SERVICE_EXEC_MODE_WORKER: runs on the node's handler_threads pool. Suited
 *     to slow or blocking work (IO, locks, sleep); replies are posted back to
 *     the original poller for sending.
 *   - NB_SERVICE_EXEC_MODE_EXECUTOR: runs on a caller-provided
 *     nb_service_executor_t, reusing an application thread pool or event loop.
 *     The caller owns its lifetime.
 *
 * With no handler_threads configured, WORKER degrades to INLINE with a warning;
 * if enqueueing fails the request is failed rather than falling back to inline
 * execution. Streaming methods support the same three modes; under
 * worker/executor, chunks of one stream are dispatched serially to preserve
 * order.
 */

#include <nb/types.h>
#include <nb/message_view.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asynchronous request handler: must eventually call one of the nb_reply*
 * functions exactly once. */
typedef void (*nb_handler_fn)(nb_call_t *call, const nb_request_t *req, void *user);

/*
 * Synchronous-style handler: the return value becomes the application status;
 * the handler writes its response into resp and the framework replies after it
 * returns (the reply is still sent asynchronously). resp owns its buffer and
 * needs no caller cleanup.
 */
typedef int (*nb_sync_handler_fn)(const nb_request_t *req, nb_message_view_t *resp, void *user);

/*
 * Streaming request callback. The framework invokes it once per chunk in
 * arrival order; a chunk is valid only during its callback. flags contains
 * NB_STREAM_FLAG_FIRST / NB_STREAM_FLAG_LAST.
 */
typedef void (*nb_chunk_handler_fn)(nb_call_t *call,
                                 const nb_request_t *req,
                                 const nb_chunk_t *chunk,
                                 void *user);

/* Deprecated spellings kept for source compatibility. */
#ifndef NB_NO_DEPRECATED_API
typedef nb_handler_fn handler_fn;
typedef nb_sync_handler_fn sync_handler_fn;
typedef nb_chunk_handler_fn chunk_handler_fn;
#endif

/* ---- Method execution modes ---- */

typedef enum nb_service_exec_mode {
    NB_SERVICE_EXEC_MODE_INLINE = 0,  /* inline on the receiving poller thread (default) */
    NB_SERVICE_EXEC_MODE_WORKER = 1,  /* node handler pool; replies post back to the poller */
    NB_SERVICE_EXEC_MODE_EXECUTOR = 2 /* caller-provided nb_service_executor_t */
} nb_service_exec_mode_t;

/*
 * Custom executor, semantically equivalent to a gRPC executor: the caller
 * creates and owns it, and the framework only calls submit. The caller must
 * keep it alive for the node's lifetime and, before nb_node_destroy, stop
 * submitting and wait for all submitted tasks to finish.
 */
typedef struct nb_service_executor {
    void *user;
    /*
     * Run fn(arg) on an executor thread. Returning NB_OK means the task was
     * accepted; returning an error means it was not (the framework frees the
     * task and replies with an error status).
     */
    int (*submit)(void *user, void (*fn)(void *arg), void *arg);
} nb_service_executor_t;

typedef struct nb_service_method_options {
    nb_service_exec_mode_t exec; /* default NB_SERVICE_EXEC_MODE_INLINE */
    /* Required when exec == NB_SERVICE_EXEC_MODE_EXECUTOR; submit must be non-NULL. */
    const nb_service_executor_t *executor;
} nb_service_method_options_t;

#define NB_SERVICE_METHOD_OPTIONS_INIT { NB_SERVICE_EXEC_MODE_INLINE, NULL }

/* ---- Declarative method table ---- */
typedef enum nb_method_kind {
    NB_METHOD_KIND_UNARY = 0,  /* nb_handler_fn */
    NB_METHOD_KIND_SYNC = 1,   /* nb_sync_handler_fn */
    NB_METHOD_KIND_STREAM = 2  /* nb_chunk_handler_fn */
} nb_method_kind_t;

typedef struct nb_method_desc {
    const char *name;
    nb_method_kind_t kind;
    union {
        nb_handler_fn unary;
        nb_sync_handler_fn sync;
        nb_chunk_handler_fn stream;
    } handler;
    void *user;
    const nb_service_method_options_t *options; /* NULL = INLINE */
} nb_method_desc_t;

/*
 * Register several methods in one call. Registration stops at the first
 * failure; methods registered before it remain active (the call is not atomic).
 */
int nb_service_add_methods(nb_service_t *service,
                           const nb_method_desc_t *methods,
                           size_t count);

/* ---- Service lifecycle ---- */
int nb_service_create(nb_node_t *node, const char *service_name, nb_service_t **out);
/*
 * Unregister the service: after this returns new requests are no longer
 * dispatched to it. The handle and its method table stay allocated until
 * nb_node_destroy() because an in-flight handler may still reference them; do
 * not use the handle afterwards.
 */
void nb_service_destroy(nb_service_t *service);

/* ---- Method registration ---- */
/*
 * Register a plain method. NULL options is equivalent to NB_SERVICE_EXEC_MODE_INLINE.
 * WORKER mode requires nb_node_config_t.handler_threads > 0.
 */
int nb_service_add_method(nb_service_t *service, const char *method, nb_handler_fn handler, void *user);
int nb_service_add_method_with_options(nb_service_t *service,
                                       const char *method,
                                       nb_handler_fn handler,
                                       void *user,
                                       const nb_service_method_options_t *options);
/* Register a synchronous-style method (the framework replies after it returns). */
int nb_service_add_sync_method(nb_service_t *service, const char *method, nb_sync_handler_fn handler, void *user);
int nb_service_add_sync_method_with_options(nb_service_t *service,
                                            const char *method,
                                            nb_sync_handler_fn handler,
                                            void *user,
                                            const nb_service_method_options_t *options);
/* Register a streaming method; INLINE / WORKER / EXECUTOR all supported, with
 * per-stream serial dispatch under worker modes. */
int nb_service_add_stream_method(nb_service_t *service, const char *method, nb_chunk_handler_fn handler, void *user);
int nb_service_add_stream_method_with_options(nb_service_t *service,
                                              const char *method,
                                              nb_chunk_handler_fn handler,
                                              void *user,
                                              const nb_service_method_options_t *options);

/*
 * Convenience wrappers for options.exec = NB_SERVICE_EXEC_MODE_WORKER.
 * nb_reply() is thread-safe inside a handler and sends on the original poller;
 * the framework already copied the request payload, so a worker may read it.
 */
int nb_service_add_method_worker(nb_service_t *service, const char *method, nb_handler_fn handler, void *user);
int nb_service_add_sync_method_worker(nb_service_t *service,
                                      const char *method,
                                      nb_sync_handler_fn handler,
                                      void *user);

/* ---- Replies ---- */
/* One-shot asynchronous reply. status is application-defined; replies to EVENT
 * requests are silently ignored. Returns NB_OK when the reply was accepted for
 * sending, NB_ERR_ALREADY on a duplicate reply, NB_ERR_TOOBIG when the status
 * message or metadata exceeds the wire limit, or another framework code. */
int nb_reply(nb_call_t *call, int status, const nb_payload_t *payload);
/*
 * Reply with a status message and response metadata (gRPC-style status message
 * / response metadata). message/metadata may be NULL; the framework copies them
 * before sending, so they may be released on return.
 */
int nb_reply_with_metadata(nb_call_t *call,
                            int status,
                            const nb_payload_t *payload,
                            const char *message,
                            const nb_metadata_t *metadata);
int nb_reply_owned(nb_call_t *call, int status, nb_owned_payload_t *payload);
int nb_reply_owned_with_metadata(nb_call_t *call,
                                  int status,
                                  nb_owned_payload_t *payload,
                                  const char *message,
                                  const nb_metadata_t *metadata);
/* Convenience: reply with raw bytes (encoding = 0). */
int nb_reply_bytes(nb_call_t *call, int status, const void *data, size_t len);
/* Explicit status variant: message_len is authoritative, so embedded NUL bytes
 * are preserved. Values at or above NB_STATUS_MSG_MAX return NB_ERR_TOOBIG. */
int nb_reply_status(nb_call_t *call,
                    const nb_status_t *status,
                    const nb_payload_t *payload,
                    const nb_metadata_t *metadata);
/* Send response chunks in several calls; the last chunk must carry
 * NB_STREAM_FLAG_LAST. */
int nb_reply_stream(nb_call_t *call, int status, const nb_chunk_t *chunk);

/* ---- Server interceptor ---- */
/*
 * Runs before the handler (on the handler thread in worker/executor modes).
 * Return NB_OK to continue; any other status short-circuits the call, replies
 * with that status, and skips the handler.
 */
typedef int (*nb_service_interceptor_fn)(nb_call_t *call,
                                         const nb_request_t *req,
                                         void *user);
#ifndef NB_NO_DEPRECATED_API
typedef nb_service_interceptor_fn service_interceptor_fn;
#endif
int nb_service_set_interceptor(nb_service_t *service, nb_service_interceptor_fn interceptor, void *user);

/* ---- Call context ---- */
/*
 * Request deadline in absolute monotonic milliseconds (0 = none), readable from
 * worker and inline handlers; same source as req->deadline_ms. Returns 0 when
 * the client set no timeout.
 */
uint64_t nb_call_deadline_ms(const nb_call_t *call);
/* Correlation id of the request (matches the client's request_id), for logging
 * and tracing. */
nb_request_id_t nb_call_request_id(const nb_call_t *call);
/*
 * Best-effort client cancellation check. Cancellation cannot preempt a running
 * handler, but a handler may poll this to exit early; queued worker jobs are
 * skipped before they start.
 */
bool nb_call_canceled(const nb_call_t *call);

#ifdef __cplusplus
}
#endif

/* ---- Deprecated enum spellings ---------------------------------------- */
#ifndef NB_NO_DEPRECATED_API
#define NB_SERVICE_EXEC_INLINE    NB_SERVICE_EXEC_MODE_INLINE
#define NB_SERVICE_EXEC_WORKER    NB_SERVICE_EXEC_MODE_WORKER
#define NB_SERVICE_EXEC_EXECUTOR  NB_SERVICE_EXEC_MODE_EXECUTOR
#define NB_METHOD_UNARY           NB_METHOD_KIND_UNARY
#define NB_METHOD_SYNC            NB_METHOD_KIND_SYNC
#define NB_METHOD_STREAM          NB_METHOD_KIND_STREAM
#endif /* NB_NO_DEPRECATED_API */

#endif /* NB_SERVICE_H */
