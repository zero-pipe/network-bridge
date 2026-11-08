#ifndef NB_TRANSPORT_PROVIDER_H
#define NB_TRANSPORT_PROVIDER_H

#include <nb/message.h>
#include <nb/status.h>
#include <nb/qos.h>
#include <nb/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_transport_provider nb_transport_provider_t;
typedef struct nb_transport_instance nb_transport_instance_t;

/* Forward declarations for optional ops; the definitions live in
 * <nb/discovery_view.h> and <nb/metrics.h>. Keeping the tags visible here
 * avoids "declared inside parameter list" warnings in every includer. */
struct nb_discovery_sample;
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

/* Roles for nb_transport_provider_ops.register_topic. */
#define NB_PS_TOPIC_WRITER 0x1u
#define NB_PS_TOPIC_READER 0x2u

/*
 * Pub/sub role options. They are only filled for a binding that serves the
 * pub/sub spine; an RPC-only binding leaves nb_transport_provider_options_t.ps
 * NULL, so an RPC transport never sees QoS policies or DDS status callbacks.
 *
 * The core copies this struct into the provider's create() call; a provider
 * that keeps the values must copy them too (the pointer is stack-scoped).
 */
typedef struct nb_ps_backend_options {
    uint32_t struct_size;              /* sizeof(nb_ps_backend_options_t) */
    nb_ps_qos_t qos;                   /* pub/sub QoS; zero = provider default */
    /* Optional DDS status events (deadline, liveliness, matching), called with
     * the same `user` as on_receive. */
    nb_status_callback_t on_status;
} nb_ps_backend_options_t;

typedef struct nb_transport_provider_options {
    const char *scheme;              /* provider scheme, e.g. "dds" */
    const char *endpoint;            /* binding endpoint URI */
    void *user;                      /* passed back to on_receive */
    transport_receive_fn on_receive; /* inbound message callback */
    nb_intra_process_t intra_process; /* provider-local event fast path */
    /* Busy-poll window in microseconds after draining; 0 = blocking wait. Only
     * providers with an inspectable local queue (shm) honor this; others ignore. */
    uint32_t poll_us;
    /* ABI guard: sizeof(nb_transport_provider_options_t) as compiled into the
     * caller. The core always fills this. */
    uint32_t struct_size;
    /* Role-specific options; NULL when the binding does not serve that role. */
    const nb_ps_backend_options_t *ps;
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
/* ---- Role interfaces ---- */
/*
 * A provider advertises the roles it implements:
 *
 *   rpc  a point-to-point link for the RPC spine (udp/tcp/shm, plus a DDS
 *        backend used as an RPC link).
 *   ps   a topic backend for the pub/sub spine (dds/rtps/shm).
 *
 * A provider may implement both (shm does). The core asks for the role it
 * needs and returns NB_ERR_NOT_SUPPORTED when it is absent, instead of probing
 * a bag of optional slots.
 */
#define NB_PROVIDER_ROLE_RPC 0x1u
#define NB_PROVIDER_ROLE_PS 0x2u

/*
 * RPC role. send() returning NB_OK means the provider accepted the message for
 * delivery. Lifetime rules crossing this interface:
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

/*
 * Pub/sub role. register_topic is required: the pub/sub spine
 * (participant / topic / writer / reader) registers a topic by its own name,
 * and a provider with native topics names the native topic after it. roles is
 * a bitmask of NB_PS_TOPIC_WRITER / NB_PS_TOPIC_READER and the call is
 * idempotent per (topic, role).
 *
 * register_method / unregister_method are the RPC-over-PS bridge hooks: the RPC
 * spine maps a "service.method" route onto request/response/event topics
 * through them. A backend that can carry RPC implements them; a pub/sub-only
 * backend leaves them NULL.
 */
typedef struct nb_ps_backend_ops {
    int (*register_topic)(nb_transport_instance_t *instance,
                          const char *topic,
                          uint32_t roles);
    int (*register_method)(nb_transport_instance_t *instance,
                           const char *service,
                           const char *method);
    void (*unregister_method)(nb_transport_instance_t *instance,
                              const char *service,
                              const char *method);
    /*
     * Optional discovery view (the DCPS built-in topics). Fill up to max
     * entries of `topic` (an nb_builtin_topic_t value) and return how many
     * were written. Backends without discovery leave this NULL.
     */
    size_t (*discovery_read)(nb_transport_instance_t *instance,
                             int topic,
                             struct nb_discovery_sample *out,
                             size_t max);
} nb_ps_backend_ops_t;

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
    /* Role interfaces; at least one must be non-NULL. */
    const nb_rpc_transport_ops_t *rpc;
    const nb_ps_backend_ops_t *ps;
} nb_transport_provider_ops_t;

#define NB_TRANSPORT_PROVIDER_ABI_VERSION 4u

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
