#ifndef NB_TRANSPORT_PROVIDER_H
#define NB_TRANSPORT_PROVIDER_H

#include <nb/rpc/message.h>
#include <nb/rpc/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_transport_provider nb_transport_provider_t;
typedef struct nb_transport_instance nb_transport_instance_t;

/* Forward declaration for the optional metrics hook; the definition lives in
 * <nb/base/metrics.h>. Keeping the tag visible avoids "declared inside
 * parameter list" warnings in every includer. */
struct nb_metrics;

/*
 * Security boundary: the built-in UDP/TCP transports are plaintext and
 * unauthenticated. The envelope only checks magic/version/CRC integrity
 * (against bit errors and cross-packet mixing), provides no confidentiality or
 * identity guarantees, and must not be exposed directly to an untrusted
 * network. For TLS/DTLS or application-level authentication:
 *   - carry an encrypted channel through a custom transport provider (this
 *     interface), or
 *   - validate credentials in metadata inside nb_service_set_interceptor / a
 *     handler, and use peer_rate_limit_pps / max_streams for basic flow
 *     control and resource caps.
 */

/* ---- Messages and callbacks ---- */
/* Provider SPI uses the same message type as the core. */
typedef nb_message_t nb_transport_message_t;

/* user is the receiver context from nb_transport_provider_options_t;
 * reply_token is the opaque token to pass back to ops->reply. */
typedef void (*nb_transport_receive_fn)(void *user,
                                     void *reply_token,
                                     const nb_transport_message_t *message);
typedef nb_transport_receive_fn transport_receive_fn;

/* ---- Provider lifecycle and operations ---- */

/*
 * Transport options. RPC transports are point-to-point links: the core gives
 * them the local binding URI, an inbound callback and the node's transport
 * tuning snapshot. There is no QoS or topic surface here; topic semantics live
 * in the DDS spine.
 */
typedef struct nb_transport_provider_options {
    const char *scheme;              /* provider scheme, e.g. "shm" */
    const char *endpoint;            /* binding endpoint URI */
    void *user;                      /* passed back to on_receive */
    transport_receive_fn on_receive; /* inbound message callback */
    /* ABI guard: sizeof(nb_transport_provider_options_t) as compiled into the
     * caller. The core always fills this. */
    uint32_t struct_size;
} nb_transport_provider_options_t;

/*
 * send() returning NB_OK means the provider accepted the message for delivery.
 *
 * Lifetime rules crossing this SPI:
 *   - options.endpoint is the binding URI and is valid for the instance's
 *     lifetime (it points at storage owned by the core).
 *   - message (including payload/metadata/status/route pointers) is valid only
 *     for the duration of send/reply; copy anything retained.
 *   - message->frame_owner is a referenced nb_buf_t owned by the core. A
 *     provider that retains the frame must take its own reference with
 *     nb_buf_ref() and release it later; otherwise the core releases it as soon
 *     as the callback returns.
 *   - reply_token is opaque, belongs to the provider, and is valid only during
 *     on_receive and later ops->reply calls for that same inbound message.
 *   - message->peer is the provider-filled source endpoint and is callback
 *     scoped; message->target is the core-filled destination for send().
 */
/* ---- RPC transport interface ---- */
/*
 * send() returning NB_OK means the provider accepted the message for delivery.
 * Lifetime rules crossing this interface:
 *   - options.endpoint is the binding URI and is valid for the instance's
 *     lifetime (it points at storage owned by the core).
 *   - message (including payload/metadata/status/route pointers) is valid only
 *     for the duration of send/reply; copy anything retained.
 *   - message->frame_owner is a referenced nb_buf_t owned by the core. A
 *     provider that retains the frame must take its own reference with
 *     nb_buf_ref() and release it later; otherwise the core releases it as soon
 *     as the callback returns.
 *   - reply_token is opaque, belongs to the provider, and is valid only during
 *     on_receive and later reply calls for that same inbound message.
 *   - message->peer is the provider-filled source endpoint and is callback
 *     scoped; message->target is the core-filled destination for send().
 */
typedef struct nb_rpc_transport_ops {
    int (*send)(nb_transport_instance_t *instance,
                const nb_endpoint_t *target,
                const nb_transport_message_t *message);
    int (*reply)(nb_transport_instance_t *instance,
                 void *reply_token,
                 const nb_endpoint_t *peer,
                 const nb_transport_message_t *message);
    /*
     * Optional RPC event subscription. A transport that fans one-way calls out
     * by route (shm) subscribes "service.method" here; point-to-point links
     * leave both NULL. This is RPC-only: it is not a topic registration hook.
     */
    int (*subscribe)(nb_transport_instance_t *instance,
                     const char *service,
                     const char *method);
    void (*unsubscribe)(nb_transport_instance_t *instance,
                        const char *service,
                        const char *method);
    /*
     * Optional zero-copy publish path (iceoryx-style loan/borrow). Providers
     * that keep payloads in inspectable memory (shm) implement all three; the
     * core then lets publishers write the payload in place instead of copying.
     *
     *   loan: reserve a chunk big enough for a message with this route and
     *         payload_capacity. *payload_data receives the writable payload
     *         area (inside the chunk), *payload_capacity_out the actual
     *         capacity, and *loan an opaque handle owned by the caller.
     *   send_loan: encode the envelope header around the already-written
     *         payload and publish. Takes ownership of the loan regardless of
     *         the return value. message->route must match the loan route and
     *         message->payload.data must be the loaned pointer.
     *   release_loan: return an unpublished loan to the pool.
     */
    int (*loan)(nb_transport_instance_t *instance,
                const char *route,
                uint32_t payload_capacity,
                void **payload_data,
                uint32_t *payload_capacity_out,
                void **loan);
    int (*send_loan)(nb_transport_instance_t *instance,
                     const char *route,
                     void *loan,
                     const nb_transport_message_t *message);
    void (*release_loan)(nb_transport_instance_t *instance, void *loan);
} nb_rpc_transport_ops_t;

typedef struct nb_transport_provider_ops {
    /* Must be NB_TRANSPORT_PROVIDER_ABI_VERSION. */
    uint32_t abi_version;
    /* sizeof(nb_transport_provider_ops_t). */
    uint32_t struct_size;
    int (*create)(const nb_transport_provider_options_t *options, nb_transport_instance_t **out);
    int (*start)(nb_transport_instance_t *instance);
    void (*stop)(nb_transport_instance_t *instance);
    void (*destroy)(nb_transport_instance_t *instance);
    /*
     * Optional: merge the provider's own counters into `out` (only the fields
     * of its metric scope). nb_node_metrics calls it after taking the node
     * snapshot, so providers never race with each other.
     */
    void (*metrics_read)(nb_transport_instance_t *instance, struct nb_metrics *out);
    /* RPC interface; required. */
    const nb_rpc_transport_ops_t *rpc;
} nb_transport_provider_ops_t;

#define NB_TRANSPORT_PROVIDER_ABI_VERSION 5u

/* Static descriptor registered with nb_transport_provider_register. */
struct nb_transport_provider {
    const char *scheme;               /* URI scheme this provider handles */
    const nb_transport_provider_ops_t *ops;
};

/* ---- Registration ---- */
/* Register before creating nodes. Provider descriptors and ops must have static
 * lifetime. */
int nb_transport_provider_register(const nb_transport_provider_t *provider);
/* Remove a provider registration. Call only while no node uses the scheme;
 * returns NB_ERR_NOTFOUND for an unknown scheme. */
int nb_transport_provider_unregister(const char *scheme);

#ifdef __cplusplus
}
#endif

#endif /* NB_TRANSPORT_PROVIDER_H */
