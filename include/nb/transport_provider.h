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
typedef struct nb_transport_provider_options {
    const char *scheme;              /* provider scheme, e.g. "dds" */
    const char *endpoint;            /* binding endpoint URI */
    void *user;                      /* passed back to on_receive */
    transport_receive_fn on_receive; /* inbound message callback */
    nb_intra_process_t intra_process; /* provider-local event fast path */
    /* Busy-poll window in microseconds after draining; 0 = blocking wait. Only
     * providers with an inspectable local queue (shm) honor this; others ignore. */
    uint32_t poll_us;
    nb_qos_t qos;                    /* provider QoS; zero = provider default */
    /* Optional: DDS status events (deadline, liveliness, matching), called with
     * the same `user` as on_receive. Appended last so the offsets of the fields
     * above stay stable: a provider built against an older header sees a
     * smaller struct_size and must not read past it. */
    nb_status_callback_t on_status;
    /* ABI guard: sizeof(nb_transport_provider_options_t) as compiled into the
     * caller. The core always fills this. Providers should ignore trailing
     * fields when struct_size is smaller than they expect. */
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
typedef struct nb_transport_provider_ops {
    /* Must be NB_TRANSPORT_PROVIDER_ABI_VERSION; 0 is accepted as legacy. */
    uint32_t abi_version;
    /* sizeof(nb_transport_provider_ops_t); 0 is accepted as legacy. */
    uint32_t struct_size;
    int (*create)(const nb_transport_provider_options_t *options, nb_transport_instance_t **out);
    int (*start)(nb_transport_instance_t *instance);
    void (*stop)(nb_transport_instance_t *instance);
    void (*destroy)(nb_transport_instance_t *instance);
    int (*send)(nb_transport_instance_t *instance,
                const nb_endpoint_t *target,
                const nb_transport_message_t *message);
    int (*reply)(nb_transport_instance_t *instance,
                 void *reply_token,
                 const nb_endpoint_t *peer,
                 const nb_transport_message_t *message);
    /*
     * Optional topic/route registration hooks for transports that need explicit
     * registration (for example DDS topics). The core calls them when a service
     * method is registered/unregistered on a node. Providers that do not need
     * them may leave both NULL.
     */
    int (*register_method)(nb_transport_instance_t *instance,
                           const char *service,
                           const char *method);
    void (*unregister_method)(nb_transport_instance_t *instance,
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
    /*
     * Optional discovery view (the DCPS built-in topics). Fill up to max
     * entries of `topic` (an nb_builtin_topic_t value) and return how many
     * were written. Providers without discovery leave this NULL.
     */
    size_t (*discovery_read)(nb_transport_instance_t *instance,
                             int topic,
                             struct nb_discovery_sample *out,
                             size_t max);
    /*
     * Optional: merge the provider's own counters into `out` (only the fields
     * of its metric scope). nb_node_metrics calls it after taking the node
     * snapshot, so providers never race with each other.
     */
    void (*metrics_read)(nb_transport_instance_t *instance, struct nb_metrics *out);
} nb_transport_provider_ops_t;

#define NB_TRANSPORT_PROVIDER_ABI_VERSION 1u

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
