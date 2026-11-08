/*
 * RTPS transport provider.
 *
 * Backed by the in-tree RTPS implementation through its public rtps_* API
 * (include/rtps/). This file must not include any engine header: the provider
 * is an ordinary user of the interface layer, exactly like an external program,
 * and a contract test enforces that.
 *
 * The provider carries the framework's encoded v2 frame as an opaque CDR body
 * whose type name is "nb::Frame". rtps_writer_write() adds the four-octet CDR
 * encapsulation header and the receive path hands back the body, which may
 * carry up to three trailing alignment octets that the envelope decoder trims.
 *
 * Topics follow the same scheme as the Cyclone provider so both providers can
 * coexist in one process:
 *   request  -> <Service>_<method>_req
 *   response -> <Service>_<method>_res
 *   event    -> <Service>_<method>_evt
 *
 * Binding URIs:
 *   rtps://<domain>                      multicast discovery, ephemeral ports
 *   rtps://<domain>:<spdp-port>          unicast only, fixed discovery port
 *   rtps://<domain>:<port>/<host>:<port>,... unicast peers, discovery disabled
 *   rtps://<domain>[...]?tcp[=<port>]    also listen for RTPS over TCP
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "transport_internal.h"

#include <nb/discovery_view.h>

#include "envelope.h"
#include "util.h"

#include <nb/log.h>

#include <rtps/rtps.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_RTPS_TRANSPORT_MAX_ENDPOINTS 32
#define NB_RTPS_TRANSPORT_TOPIC_MAX 160
#define NB_RTPS_TRANSPORT_MAX_PEERS 16
/* Non-"dds::Message" so the engine hands back the raw CDR body. */
#define NB_RTPS_TRANSPORT_TYPE_NAME "nb::Frame"
/* Heartbeat cadence for reliable application writers. Cyclone DDS defaults to
 * 100 ms; matching it keeps loss recovery equally prompt now that a write no
 * longer sends an immediate, per-message heartbeat. */
#define NB_RTPS_TRANSPORT_DEFAULT_HEARTBEAT_MS 100u
/* Frames up to this size are encoded on the stack. 1 KB payloads with the
 * envelope fit comfortably, so the common case does not allocate. */
#define NB_RTPS_TRANSPORT_INLINE_FRAME 1536u

typedef struct nb_rtps_transport nb_rtps_transport_t;

/* A unicast SPDP peer parsed from the binding URI. */
typedef struct nb_rtps_transport_peer {
    char host[NB_HOST_MAX];
    uint16_t port;
} nb_rtps_transport_peer_t;

typedef struct nb_rtps_transport_endpoint {
    int used;
    char method[NB_GLOBAL_MAX_ROUTE_BYTES];
    char topic_req[NB_RTPS_TRANSPORT_TOPIC_MAX];
    char topic_res[NB_RTPS_TRANSPORT_TOPIC_MAX];
    char topic_evt[NB_RTPS_TRANSPORT_TOPIC_MAX];
    rtps_topic_t *t_req;
    rtps_topic_t *t_res;
    rtps_topic_t *t_evt;
    rtps_writer_t *wr_req;
    rtps_reader_t *rd_req;
    rtps_writer_t *wr_res;
    rtps_reader_t *rd_res;
    rtps_writer_t *wr_evt;
    rtps_reader_t *rd_evt;
    int have_responder;
    int have_requester;
    int have_publisher;
    /* Native pub/sub topic endpoint: the event topic is named after the topic
     * itself (no "_evt" suffix) and the reader role is independent of the RPC
     * responder role. */
    int topic_mode;
    int have_topic_reader;
} nb_rtps_transport_endpoint_t;

struct nb_rtps_transport {
    nb_transport_provider_options_t options;
    rtps_participant_t *participant;
    char authority[NB_HOST_MAX];
    char advertise[NB_HOST_MAX];
    ntk_mutex *lock;
    nb_rtps_transport_endpoint_t endpoints[NB_RTPS_TRANSPORT_MAX_ENDPOINTS];
    size_t endpoint_count;
    /* Topic/endpoint QoS from the binding. Reader endpoints deliberately do
     * not carry a history: the provider consumes samples through a callback,
     * and a history would only copy every frame a second time. */
    rtps_qos_t qos;
    rtps_qos_t reader_qos;
    int started;
};

static int nb_rtps_transport_status(int rtps_status)
{
    switch (rtps_status) {
    case RTPS_OK:
        return NB_OK;
    case RTPS_ERR_INVALID:
        return NB_ERR_INVALID;
    case RTPS_ERR_NOMEM:
        return NB_ERR_NOMEM;
    case RTPS_ERR_NOTFOUND:
        return NB_ERR_NOTFOUND;
    case RTPS_ERR_TIMEOUT:
        return NB_ERR_TIMEOUT;
    case RTPS_ERR_IO:
        return NB_ERR_IO;
    case RTPS_ERR_STATE:
        return NB_ERR_STATE;
    case RTPS_ERR_AGAIN:
        return NB_ERR_AGAIN;
    case RTPS_ERR_TOOBIG:
        return NB_ERR_TOOBIG;
    case RTPS_ERR_CLOSED:
        return NB_ERR_CLOSED;
    case RTPS_ERR_ALREADY:
        return NB_ERR_ALREADY;
    case RTPS_ERR_UNSUPPORTED:
        return NB_ERR_NOT_SUPPORTED;
    case RTPS_ERR_CANCELED:
        return NB_ERR_CANCELED;
    case RTPS_ERR_OVERLOAD:
        return NB_ERR_OVERLOAD;
    case RTPS_ERR_BAD_MESSAGE:
        return NB_ERR_BAD_MESSAGE;
    case RTPS_ERR_BUSY:
        return NB_ERR_BUSY;
    case RTPS_ERR_PRECONDITION:
    case RTPS_ERR_INTERNAL:
    default:
        return NB_ERR_STATE;
    }
}

/* Route -> identifier-safe method, matching the Cyclone provider's rule so the
 * same service maps to the same topic name on both transports. */
static void nb_rtps_transport_method(const char *route, char *out, size_t cap)
{
    size_t i = 0;
    size_t j = 0;

    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!route) {
        return;
    }
    if (route[0] >= '0' && route[0] <= '9') {
        out[j++] = '_';
    }
    for (i = 0; route[i] != '\0' && j + 1 < cap; ++i) {
        char c = route[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

/* Map the framework QoS onto the public rtps QoS. The transport has always
 * defaulted to reliable/volatile and BY_RECEPTION ordering; keep that. */
static void nb_rtps_transport_build_qos(const nb_ps_qos_t *q,
                                        int with_history,
                                        rtps_qos_t *out)
{
    rtps_qos_init(out);
    rtps_qos_set_reliability(
        out,
        (q && q->reliability == NB_QOS_RELIABILITY_BEST_EFFORT)
            ? RTPS_RELIABILITY_BEST_EFFORT
            : RTPS_RELIABILITY_RELIABLE);
    rtps_qos_set_durability(
        out,
        (q && q->durability == NB_QOS_DURABILITY_TRANSIENT_LOCAL)
            ? RTPS_DURABILITY_TRANSIENT_LOCAL
            : RTPS_DURABILITY_VOLATILE);
    if (with_history) {
        rtps_qos_set_history(
            out,
            (q && q->history == NB_QOS_HISTORY_KEEP_ALL) ? RTPS_HISTORY_KEEP_ALL
                                                         : RTPS_HISTORY_KEEP_LAST,
            (q && q->depth) ? q->depth : 8u);
    }
    rtps_qos_set_ownership(
        out,
        (q && q->ownership == NB_QOS_OWNERSHIP_EXCLUSIVE) ? RTPS_OWNERSHIP_EXCLUSIVE
                                                          : RTPS_OWNERSHIP_SHARED,
        0);
    rtps_qos_set_destination_order(out, RTPS_DESTINATION_ORDER_BY_RECEPTION);
    rtps_qos_set_liveliness(out, RTPS_LIVELINESS_AUTOMATIC,
                            q ? q->lease_duration_ms : 0u);
    if (q) {
        if (q->deadline_ms) {
            rtps_qos_set_deadline_ms(out, q->deadline_ms);
        }
        if (q->lifespan_ms) {
            rtps_qos_set_lifespan_ms(out, q->lifespan_ms);
        }
        if (q->latency_budget_ms) {
            rtps_qos_set_latency_budget_ms(out, q->latency_budget_ms);
        }
        if (q->min_separation_ms) {
            rtps_qos_set_time_based_filter_ms(out, q->min_separation_ms);
        }
        if (q->max_samples || q->max_instances || q->max_samples_per_instance) {
            rtps_qos_set_resource_limits(out,
                                         q->max_samples,
                                         q->max_instances,
                                         q->max_samples_per_instance);
        }
        if (q->autodispose_unregistered_instances) {
            rtps_qos_set_autodispose(out, 1);
        }
        rtps_qos_set_presentation(out,
                                  (rtps_presentation_scope_t)q->presentation_scope,
                                  q->coherent_access ? 1 : 0,
                                  q->ordered_access ? 1 : 0);
        if (q->partition[0]) {
            rtps_qos_set_partition(out, q->partition);
        }
        if (q->liveliness == NB_QOS_LIVELINESS_MANUAL_BY_PARTICIPANT ||
            q->liveliness == NB_QOS_LIVELINESS_MANUAL_BY_TOPIC) {
            /* The public API has no explicit assert operation yet; announcing
             * manual liveliness without assertions would make peers declare
             * us dead after the lease. */
            nb_log_warn("rtps transport: manual liveliness needs an explicit "
                        "assert operation, which is not exposed; announcing "
                        "automatic liveliness");
        }
    }
}

/* ---- endpoint bookkeeping ----------------------------------------------- */

static nb_rtps_transport_endpoint_t *nb_rtps_transport_endpoint_init(
    nb_rtps_transport_t *inst,
    const char *method)
{
    nb_rtps_transport_endpoint_t *ep;

    if (inst->endpoint_count >= NB_RTPS_TRANSPORT_MAX_ENDPOINTS) {
        return NULL;
    }
    ep = &inst->endpoints[inst->endpoint_count++];
    memset(ep, 0, sizeof(*ep));
    ep->used = 1;
    nb_copy_str(ep->method, sizeof(ep->method), method, "");
    (void)snprintf(ep->topic_req, sizeof(ep->topic_req), "%s_req", method);
    (void)snprintf(ep->topic_res, sizeof(ep->topic_res), "%s_res", method);
    (void)snprintf(ep->topic_evt, sizeof(ep->topic_evt), "%s_evt", method);
    return ep;
}

static rtps_topic_t *nb_rtps_transport_topic(nb_rtps_transport_t *inst,
                                             const char *name)
{
    rtps_topic_desc_t desc;
    rtps_topic_t *topic = NULL;

    rtps_topic_desc_init(&desc);
    desc.name = name;
    desc.type_name = NB_RTPS_TRANSPORT_TYPE_NAME;
    desc.qos = inst->qos;
    if (rtps_topic_create(inst->participant, &desc, &topic) != RTPS_OK) {
        nb_log_error("rtps transport: topic %s create failed", name);
        return NULL;
    }
    return topic;
}

static rtps_writer_t *nb_rtps_transport_writer(nb_rtps_transport_t *inst,
                                               rtps_topic_t *topic)
{
    rtps_writer_t *writer = NULL;

    if (rtps_writer_create(topic, NULL, &writer) != RTPS_OK) {
        nb_log_error("rtps transport: writer on %s create failed",
                     rtps_topic_name(topic));
        return NULL;
    }
    (void)inst;
    return writer;
}

static void nb_rtps_transport_on_data(void *user,
                                      rtps_reader_t *reader,
                                      const rtps_sample_t *samples,
                                      size_t count);

static rtps_reader_t *nb_rtps_transport_reader(nb_rtps_transport_t *inst,
                                               rtps_topic_t *topic)
{
    rtps_reader_t *reader = NULL;

    if (rtps_reader_create(topic, &inst->reader_qos, &reader) != RTPS_OK) {
        nb_log_error("rtps transport: reader on %s create failed",
                     rtps_topic_name(topic));
        return NULL;
    }
    (void)rtps_reader_set_data_callback(reader, nb_rtps_transport_on_data, inst);
    return reader;
}

static nb_rtps_transport_endpoint_t *nb_rtps_transport_ensure(
    nb_rtps_transport_t *inst,
    const char *method,
    int roles)
{
    nb_rtps_transport_endpoint_t *ep = NULL;
    size_t i;

    if (!inst || !method || !method[0]) {
        return NULL;
    }
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        if (inst->endpoints[i].used &&
            strcmp(inst->endpoints[i].method, method) == 0) {
            ep = &inst->endpoints[i];
            break;
        }
    }
    if (!ep) {
        ep = nb_rtps_transport_endpoint_init(inst, method);
        if (!ep) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
    }
    if ((roles & 1) && !ep->have_responder) { /* responder */
        ep->t_req = ep->t_req ? ep->t_req : nb_rtps_transport_topic(inst, ep->topic_req);
        ep->t_evt = ep->t_evt ? ep->t_evt : nb_rtps_transport_topic(inst, ep->topic_evt);
        ep->t_res = ep->t_res ? ep->t_res : nb_rtps_transport_topic(inst, ep->topic_res);
        if (!ep->t_req || !ep->t_evt || !ep->t_res) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->rd_req = nb_rtps_transport_reader(inst, ep->t_req);
        ep->rd_evt = nb_rtps_transport_reader(inst, ep->t_evt);
        ep->wr_res = nb_rtps_transport_writer(inst, ep->t_res);
        if (!ep->rd_req || !ep->rd_evt || !ep->wr_res) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_responder = 1;
    }
    if ((roles & 2) && !ep->have_requester) { /* requester */
        ep->t_req = ep->t_req ? ep->t_req : nb_rtps_transport_topic(inst, ep->topic_req);
        ep->t_res = ep->t_res ? ep->t_res : nb_rtps_transport_topic(inst, ep->topic_res);
        if (!ep->t_req || !ep->t_res) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->wr_req = nb_rtps_transport_writer(inst, ep->t_req);
        ep->rd_res = nb_rtps_transport_reader(inst, ep->t_res);
        if (!ep->wr_req || !ep->rd_res) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_requester = 1;
    }
    if ((roles & 4) && !ep->have_publisher) { /* event publisher */
        ep->t_evt = ep->t_evt ? ep->t_evt : nb_rtps_transport_topic(inst, ep->topic_evt);
        if (!ep->t_evt) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->wr_evt = nb_rtps_transport_writer(inst, ep->t_evt);
        if (!ep->wr_evt) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_publisher = 1;
    }
    ntk_mutex_unlock(inst->lock);
    return ep;
}

/* Native pub/sub topic: the same endpoint table, but the event topic carries
 * the topic name verbatim and a reader role that is separate from the RPC
 * responder role. */
static nb_rtps_transport_endpoint_t *nb_rtps_transport_ensure_topic(
    nb_rtps_transport_t *inst,
    const char *topic,
    uint32_t roles)
{
    nb_rtps_transport_endpoint_t *ep = NULL;
    char name[NB_GLOBAL_MAX_ROUTE_BYTES];
    size_t i;

    if (!inst || !topic || !topic[0]) {
        return NULL;
    }
    nb_rtps_transport_method(topic, name, sizeof(name));
    if (!name[0]) {
        return NULL;
    }
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        if (inst->endpoints[i].used && strcmp(inst->endpoints[i].method, name) == 0) {
            ep = &inst->endpoints[i];
            break;
        }
    }
    if (ep && !ep->topic_mode) {
        /* The name is already used by an RPC endpoint; topic names and route
         * names must stay disjoint. */
        ntk_mutex_unlock(inst->lock);
        return NULL;
    }
    if (!ep) {
        ep = nb_rtps_transport_endpoint_init(inst, name);
        if (!ep) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        nb_copy_str(ep->topic_evt, sizeof(ep->topic_evt), name, "");
        ep->topic_mode = 1;
    }
    if (!ep->t_evt) {
        ep->t_evt = nb_rtps_transport_topic(inst, ep->topic_evt);
        if (!ep->t_evt) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
    }
    if ((roles & NB_PS_TOPIC_WRITER) && !ep->have_publisher) {
        ep->wr_evt = nb_rtps_transport_writer(inst, ep->t_evt);
        if (!ep->wr_evt) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_publisher = 1;
    }
    if ((roles & NB_PS_TOPIC_READER) && !ep->have_topic_reader) {
        ep->rd_evt = ep->rd_evt ? ep->rd_evt : nb_rtps_transport_reader(inst, ep->t_evt);
        if (!ep->rd_evt) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_topic_reader = 1;
    }
    ntk_mutex_unlock(inst->lock);
    return ep;
}

/* ---- publishing --------------------------------------------------------- */

static int nb_rtps_transport_publish(nb_rtps_transport_t *inst,
                                     rtps_writer_t *writer,
                                     const nb_transport_message_t *message)
{
    nb_envelope_t env;
    uint8_t inline_frame[NB_RTPS_TRANSPORT_INLINE_FRAME];
    uint8_t *buf = inline_frame;
    size_t frame_len;
    int buf_heap = 0;
    int rc;

    (void)inst;
    if (!writer || !message) {
        return NB_ERR_STATE;
    }
    if (message->sample.has_instance_handle &&
        message->sample.state != NB_SAMPLE_STATE_VALID) {
        /* A state change is key-only on the wire: the instance handle travels
         * in the inline QoS and the caller's payload is the serialized key. */
        rtps_instance_handle_t handle;
        rtps_instance_state_t state =
            message->sample.state == NB_SAMPLE_STATE_NO_WRITERS
                ? RTPS_INSTANCE_NOT_ALIVE_NO_WRITERS
                : RTPS_INSTANCE_NOT_ALIVE_DISPOSED;

        memcpy(handle.bytes, message->sample.instance_handle, sizeof(handle.bytes));
        return nb_rtps_transport_status(
            rtps_writer_write_instance(writer,
                                       &handle,
                                       state,
                                       message->payload.data,
                                       message->payload.len));
    }
    memset(&env, 0, sizeof(env));
    env.kind = message->kind;
    env.encoding = message->payload.encoding;
    env.flags = message->flags;
    env.status = message->status;
    env.request_id = message->request_id;
    env.timeout_ms = message->timeout_ms;
    env.route = message->route;
    env.metadata = message->metadata;
    env.metadata_len = message->metadata_len;
    env.status_msg = message->status_msg;
    env.status_msg_len = message->status_msg_len;
    env.payload = message->payload.data;
    env.payload_len = message->payload.len;
    if (message->has_stream) {
        env.stream_flags = (uint8_t)message->stream.flags;
        env.stream_id = message->stream.stream_id;
        env.stream_offset = message->stream.offset;
        env.stream_total_size = message->stream.total_size;
    }
    frame_len = nb_envelope_size(&env);
    if (frame_len == 0) {
        return NB_ERR_INVALID;
    }
    if (frame_len > sizeof(inline_frame)) {
        buf = (uint8_t *)malloc(frame_len);
        if (!buf) {
            return NB_ERR_NOMEM;
        }
        buf_heap = 1;
    }
    if (nb_envelope_encode(buf, frame_len, &env) != frame_len) {
        if (buf_heap) {
            free(buf);
        }
        return NB_ERR_INVALID;
    }
    rc = nb_rtps_transport_status(rtps_writer_write(writer, buf, frame_len));
    if (buf_heap) {
        free(buf);
    }
    return rc;
}

/* Deliver an event to local subscriber endpoints without a round trip. */
static void nb_rtps_transport_local_deliver(nb_rtps_transport_t *inst,
                                            const nb_transport_message_t *message)
{
    nb_rtps_transport_endpoint_t *targets[NB_RTPS_TRANSPORT_MAX_ENDPOINTS];
    size_t count = 0;
    size_t i;

    if (!inst->options.on_receive) {
        return;
    }
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        nb_rtps_transport_endpoint_t *ep = &inst->endpoints[i];

        if (ep->used && ep->have_responder &&
            count < NB_RTPS_TRANSPORT_MAX_ENDPOINTS) {
            targets[count++] = ep;
        }
    }
    ntk_mutex_unlock(inst->lock);
    for (i = 0; i < count; ++i) {
        inst->options.on_receive(inst->options.user, targets[i], message);
    }
}

/* RTPS pads each submessage to a four-byte boundary, so the body handed to the
 * reader may carry up to three trailing filler bytes. Trim them and retry when
 * the exact length does not decode. */
static int nb_rtps_transport_decode_frame(const uint8_t *buf,
                                          size_t len,
                                          nb_envelope_t *env)
{
    size_t trim;

    if (nb_envelope_decode(buf, len, env) == NB_OK) {
        return NB_OK;
    }
    for (trim = 1; trim <= 3 && trim < len; trim++) {
        if (nb_envelope_decode(buf, len - trim, env) == NB_OK) {
            return NB_OK;
        }
    }
    return NB_ERR_INVALID;
}

static void nb_rtps_transport_fill_sample(nb_sample_info_t *dst,
                                          const rtps_sample_info_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src->valid_data) {
        dst->state = NB_SAMPLE_STATE_VALID;
    } else if (src->instance_state == RTPS_INSTANCE_NOT_ALIVE_NO_WRITERS) {
        dst->state = NB_SAMPLE_STATE_NO_WRITERS;
    } else {
        dst->state = NB_SAMPLE_STATE_DISPOSED;
    }
    dst->has_instance_handle = src->has_instance_handle ? true : false;
    if (src->has_instance_handle) {
        memcpy(dst->instance_handle, src->instance_handle.bytes, NB_INSTANCE_HANDLE_SIZE);
    }
    dst->source_timestamp_ns = src->source_timestamp_ns;
    dst->reception_timestamp_ns = src->reception_timestamp_ns;
    dst->instance_state = (nb_instance_state_t)src->instance_state;
    dst->view_state = src->view_state == RTPS_VIEW_STATE_NEW ? NB_VIEW_STATE_NEW
                                                             : NB_VIEW_STATE_NOT_NEW;
    dst->generation_rank = (int32_t)src->generation_rank;
}

static nb_rtps_transport_endpoint_t *nb_rtps_transport_endpoint_for_reader(
    nb_rtps_transport_t *inst,
    rtps_reader_t *reader)
{
    nb_rtps_transport_endpoint_t *found = NULL;
    size_t i;

    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        nb_rtps_transport_endpoint_t *ep = &inst->endpoints[i];

        if (ep->used && (ep->rd_req == reader || ep->rd_res == reader ||
                         ep->rd_evt == reader)) {
            found = ep;
            break;
        }
    }
    ntk_mutex_unlock(inst->lock);
    return found;
}

static void nb_rtps_transport_on_data(void *user,
                                      rtps_reader_t *reader,
                                      const rtps_sample_t *samples,
                                      size_t count)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)user;
    nb_rtps_transport_endpoint_t *ep;
    nb_endpoint_t peer;
    size_t i;

    if (!inst || !reader || !samples || !inst->options.on_receive) {
        return;
    }
    ep = nb_rtps_transport_endpoint_for_reader(inst, reader);
    if (!ep) {
        return;
    }
    if (nb_endpoint_from_parts(&peer,
                               "rtps",
                               inst->authority,
                               0,
                               NB_TRANSPORT_KIND_CUSTOM) != NB_OK) {
        memset(&peer, 0, sizeof(peer));
    }
    for (i = 0; i < count; i++) {
        nb_envelope_t env;
        nb_transport_message_t msg;
        char route_buf[NB_GLOBAL_MAX_ROUTE_BYTES];

        /* A state change carries no envelope to route; the callback contract
         * only delivers data samples. */
        if (!samples[i].info.valid_data || !samples[i].data ||
            samples[i].size == 0) {
            continue;
        }
        if (nb_rtps_transport_decode_frame(
                samples[i].data, samples[i].size, &env) != NB_OK) {
            continue;
        }
        memset(&msg, 0, sizeof(msg));
        memset(route_buf, 0, sizeof(route_buf));
        nb_envelope_to_message(&env, route_buf, sizeof(route_buf), &msg);
        nb_rtps_transport_fill_sample(&msg.sample, &samples[i].info);
        msg.peer = &peer;
        msg.peer_host = peer.host;
        msg.peer_port = peer.port;
        inst->options.on_receive(inst->options.user, ep, &msg);
    }
}

/* Provider name of a local endpoint, by GUID. The endpoint table is guarded by
 * the transport lock, which is recursive because a status callback can be
 * raised from inside endpoint creation, where that lock is already held. */
static void nb_rtps_transport_method_of(nb_rtps_transport_t *inst,
                                        const rtps_guid_t *guid,
                                        char *out,
                                        size_t cap)
{
    size_t i;

    out[0] = '\0';
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        const nb_rtps_transport_endpoint_t *ep = &inst->endpoints[i];
        rtps_guid_t g;

        if (!ep->used) {
            continue;
        }
        if ((ep->wr_req && rtps_writer_get_guid(ep->wr_req, &g) == RTPS_OK &&
             memcmp(&g, guid, sizeof(g)) == 0) ||
            (ep->rd_req && rtps_reader_get_guid(ep->rd_req, &g) == RTPS_OK &&
             memcmp(&g, guid, sizeof(g)) == 0) ||
            (ep->wr_res && rtps_writer_get_guid(ep->wr_res, &g) == RTPS_OK &&
             memcmp(&g, guid, sizeof(g)) == 0) ||
            (ep->rd_res && rtps_reader_get_guid(ep->rd_res, &g) == RTPS_OK &&
             memcmp(&g, guid, sizeof(g)) == 0) ||
            (ep->wr_evt && rtps_writer_get_guid(ep->wr_evt, &g) == RTPS_OK &&
             memcmp(&g, guid, sizeof(g)) == 0) ||
            (ep->rd_evt && rtps_reader_get_guid(ep->rd_evt, &g) == RTPS_OK &&
             memcmp(&g, guid, sizeof(g)) == 0)) {
            nb_copy_str(out, cap, ep->method, "");
            break;
        }
    }
    ntk_mutex_unlock(inst->lock);
}

static void nb_rtps_transport_on_status(void *user, const rtps_status_event_t *event)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)user;
    nb_status_event_t out;

    if (!inst || !event || !inst->options.on_status) {
        return;
    }
    memset(&out, 0, sizeof(out));
    switch (event->kind) {
    case RTPS_STATUS_OFFERED_DEADLINE_MISSED:
        out.kind = NB_STATUS_OFFERED_DEADLINE_MISSED;
        break;
    case RTPS_STATUS_REQUESTED_DEADLINE_MISSED:
        out.kind = NB_STATUS_REQUESTED_DEADLINE_MISSED;
        break;
    case RTPS_STATUS_LIVELINESS_CHANGED:
        out.kind = NB_STATUS_LIVELINESS_CHANGED;
        break;
    case RTPS_STATUS_PUBLICATION_MATCHED:
        out.kind = NB_STATUS_PUBLICATION_MATCHED;
        break;
    case RTPS_STATUS_SUBSCRIPTION_MATCHED:
        out.kind = NB_STATUS_SUBSCRIPTION_MATCHED;
        break;
    case RTPS_STATUS_QOS_INCOMPATIBLE:
        out.kind = NB_STATUS_QOS_INCOMPATIBLE;
        break;
    default:
        return;
    }
    nb_rtps_transport_method_of(inst, &event->endpoint, out.endpoint, sizeof(out.endpoint));
    out.count = event->count;
    out.total_count = event->total_count;
    out.alive = event->alive;
    out.alive_count = event->alive_count;
    out.not_alive_count = event->not_alive_count;
    if (event->has_peer) {
        out.has_peer = true;
        memcpy(out.peer_guid, &event->peer, sizeof(out.peer_guid));
    }
    nb_copy_str(out.reason, sizeof(out.reason), event->reason, "");
    inst->options.on_status(inst->options.user, &out);
}

/* ---- binding parsing and creation --------------------------------------- */

static void nb_rtps_transport_parse_peers(const char *path,
                                          nb_rtps_transport_peer_t *peers,
                                          size_t *peer_count)
{
    const char *p = path;
    size_t count = 0;

    *peer_count = 0;
    while (p && *p == '/') {
        p++;
    }
    while (p && *p && count < NB_RTPS_TRANSPORT_MAX_PEERS) {
        const char *end = strchr(p, ',');
        const char *colon;
        size_t host_len;
        unsigned long port;
        char port_text[8];
        size_t port_len;

        if (!end) {
            end = p + strlen(p);
        }
        {
            const char *query = (const char *)memchr(p, '?', (size_t)(end - p));

            if (query) {
                end = query;
            }
        }
        colon = (const char *)memchr(p, ':', (size_t)(end - p));
        if (!colon) {
            break;
        }
        host_len = (size_t)(colon - p);
        port_len = (size_t)(end - colon - 1);
        if (host_len == 0 || host_len >= sizeof(peers[count].host) ||
            port_len == 0 || port_len >= sizeof(port_text)) {
            break;
        }
        memcpy(peers[count].host, p, host_len);
        peers[count].host[host_len] = '\0';
        memcpy(port_text, colon + 1, port_len);
        port_text[port_len] = '\0';
        port = strtoul(port_text, NULL, 10);
        if (port == 0 || port > 65535) {
            break;
        }
        peers[count].port = (uint16_t)port;
        count++;
        p = *end ? end + 1 : end;
    }
    *peer_count = count;
}

/* A participant that announces 127.0.0.1 is reachable only from its own host,
 * so pick a routable interface address by default. */
static const char *nb_rtps_transport_advertise(char *buf, size_t cap)
{
    const char *override = getenv("NB_RTPS_ADVERTISE_HOST");
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    char hostname[256];

    nb_copy_str(buf, cap, "127.0.0.1", "127.0.0.1");
    if (override && override[0]) {
        nb_copy_str(buf, cap, override, "127.0.0.1");
        return buf;
    }
#if defined(_WIN32)
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return buf;
        }
    }
#endif
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0';
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostname, NULL, &hints, &res) == 0) {
            for (it = res; it; it = it->ai_next) {
                const struct sockaddr_in *sin = (const struct sockaddr_in *)it->ai_addr;
                char text[INET_ADDRSTRLEN];
                uint32_t addr;

                if (!sin || it->ai_family != AF_INET) {
                    continue;
                }
                addr = ntohl(sin->sin_addr.s_addr);
                if (addr == 0 || (addr >> 24) == 127 ||
                    (addr & 0xFFFF0000u) == 0xA9FE0000u) {
                    continue;
                }
                if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) {
                    continue;
                }
                nb_copy_str(buf, cap, text, "127.0.0.1");
                break;
            }
            freeaddrinfo(res);
        }
    }
#if defined(_WIN32)
    WSACleanup();
#endif
    return buf;
}

static int nb_rtps_op_create(const nb_transport_provider_options_t *options,
                             nb_transport_instance_t **out)
{
    nb_rtps_transport_t *inst;
    rtps_participant_config_t config;
    nb_rtps_transport_peer_t *peers = NULL;
    const char *p;
    const char *authority;
    const char *auth_end;
    char domain_text[16];
    char port_text[8];
    size_t n = 0;
    size_t path_peers = 0;
    uint16_t spdp_port = 0;
    uint16_t tcp_port = 0;
    int tcp_enabled = 0;
    int standard_ports = 0;
    uint32_t participant_id = 0;
    int domain = 0;
    size_t i;

    if (!options || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    inst = (nb_rtps_transport_t *)calloc(1, sizeof(*inst));
    if (!inst) {
        return NB_ERR_NOMEM;
    }
    inst->options = *options;
    peers = (nb_rtps_transport_peer_t *)calloc(NB_RTPS_TRANSPORT_MAX_PEERS, sizeof(*peers));
    if (!peers) {
        free(inst);
        return NB_ERR_NOMEM;
    }

    authority = options->endpoint ? strstr(options->endpoint, "://") : NULL;
    authority = authority ? authority + 3 : "";
    auth_end = strchr(authority, '/');
    if (!auth_end) {
        auth_end = authority + strlen(authority);
    }
    while (authority + n < auth_end && authority[n] && authority[n] != ':' &&
           n + 1 < sizeof(domain_text)) {
        domain_text[n] = authority[n];
        n++;
    }
    domain_text[n] = '\0';
    if (domain_text[0]) {
        domain = (int)strtol(domain_text, NULL, 10);
    }
    nb_copy_str(inst->authority, sizeof(inst->authority),
                domain_text[0] ? domain_text : "0", "0");

    p = (const char *)memchr(authority, ':', (size_t)(auth_end - authority));
    if (p && p + 1 < auth_end) {
        const char *q = p + 1;
        size_t digits = 0;

        while (q + digits < auth_end && digits + 1 < sizeof(port_text)) {
            port_text[digits] = q[digits];
            digits++;
        }
        port_text[digits] = '\0';
        spdp_port = (uint16_t)strtoul(port_text, NULL, 10);
    }
    if (auth_end && *auth_end == '/') {
        nb_rtps_transport_parse_peers(auth_end, peers, &path_peers);
    }
    {
        const char *query = strchr(auth_end, '?');
        const char *token = query ? query + 1 : NULL;

        while (token && *token) {
            const char *separator = strchr(token, '&');
            size_t token_len = separator ? (size_t)(separator - token) : strlen(token);

            if (token_len == 3 && strncmp(token, "std", 3) == 0) {
                standard_ports = 1;
            } else if (token_len >= 4 && strncmp(token, "pid=", 4) == 0) {
                char pid_text[12];
                size_t digits = token_len - 4;

                if (digits >= sizeof(pid_text)) {
                    digits = sizeof(pid_text) - 1;
                }
                memcpy(pid_text, token + 4, digits);
                pid_text[digits] = '\0';
                participant_id = (uint32_t)strtoul(pid_text, NULL, 10);
                standard_ports = 1;
            } else if (token_len >= 3 && strncmp(token, "tcp", 3) == 0 &&
                (token_len == 3 || token[3] == '=')) {
                tcp_enabled = 1;
                if (token_len > 4) {
                    char tcp_text[8];
                    size_t digits = token_len - 4;

                    if (digits >= sizeof(tcp_text)) {
                        digits = sizeof(tcp_text) - 1;
                    }
                    memcpy(tcp_text, token + 4, digits);
                    tcp_text[digits] = '\0';
                    tcp_port = (uint16_t)strtoul(tcp_text, NULL, 10);
                }
            }
            token = separator ? separator + 1 : NULL;
        }
    }

    nb_rtps_transport_build_qos(&options->qos, 1, &inst->qos);
    nb_rtps_transport_build_qos(&options->qos, 0, &inst->reader_qos);

    rtps_participant_config_init(&config);
    config.domain_id = (uint32_t)domain;
    config.bind_host = "0.0.0.0";
    config.advertise_host =
        nb_rtps_transport_advertise(inst->advertise, sizeof(inst->advertise));
    /* Multicast is the default; an explicit port or peer list pins discovery
     * to unicast peers instead. */
    config.multicast_group = (spdp_port != 0 || path_peers > 0) ? "" : NULL;
    config.spdp_port = spdp_port;
    config.unicast_port = 0;
    config.announce_interval_ms = 1000;
    config.lease_ms = 0;
    config.data_heartbeat_ms = NB_RTPS_TRANSPORT_DEFAULT_HEARTBEAT_MS;
    config.data_history_depth = 0;
    config.name = "nb-rtps";
    config.enable_tcp = tcp_enabled;
    config.tcp_port = tcp_port;
    config.standard_ports = standard_ports;
    config.participant_id = participant_id;

    if (rtps_participant_create(&config, &inst->participant) != RTPS_OK) {
        nb_log_error("rtps participant create failed for endpoint %s",
                     options->endpoint ? options->endpoint : "(null)");
        free(peers);
        free(inst);
        return NB_ERR_IO;
    }
    for (i = 0; i < path_peers; i++) {
        if (rtps_participant_add_peer(
                inst->participant, peers[i].host, peers[i].port) != RTPS_OK) {
            nb_log_warn("rtps transport: peer %s:%u rejected",
                        peers[i].host,
                        (unsigned)peers[i].port);
        }
    }
    free(peers);
    nb_log_info("rtps transport: domain %d, advertise %s, %s discovery%s%u",
                domain,
                inst->advertise,
                (config.multicast_group && config.multicast_group[0]) ? "multicast"
                                                                     : "unicast",
                tcp_enabled ? ", tcp " : "",
                tcp_enabled ? (unsigned)rtps_participant_tcp_port(inst->participant)
                            : 0u);
    (void)rtps_participant_set_status_callback(
        inst->participant, nb_rtps_transport_on_status, inst);
    /* Recursive: endpoint creation can raise a match status, and the status
     * callback reads the endpoint table. */
    inst->lock = ntk_mutex_create(NTK_MUTEX_RECURSIVE);
    if (!inst->lock) {
        rtps_participant_destroy(inst->participant);
        free(inst);
        return NB_ERR_NOMEM;
    }
    *out = (nb_transport_instance_t *)inst;
    return NB_OK;
}

static int nb_rtps_op_start(nb_transport_instance_t *instance)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;

    if (!inst || !inst->participant) {
        return NB_ERR_INVALID;
    }
    if (inst->started) {
        return NB_OK;
    }
    if (rtps_participant_start(inst->participant) != RTPS_OK) {
        return NB_ERR_IO;
    }
    inst->started = 1;
    return NB_OK;
}

static void nb_rtps_op_stop(nb_transport_instance_t *instance)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;

    if (!inst || !inst->started) {
        return;
    }
    (void)rtps_participant_stop(inst->participant);
    inst->started = 0;
}

static void nb_rtps_op_destroy(nb_transport_instance_t *instance)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;

    if (!inst) {
        return;
    }
    nb_rtps_op_stop(instance);
    if (inst->participant) {
        rtps_participant_destroy(inst->participant);
    }
    if (inst->lock) {
        ntk_mutex_destroy(inst->lock);
    }
    free(inst);
}

static int nb_rtps_op_send(nb_transport_instance_t *instance,
                           const nb_endpoint_t *target,
                           const nb_transport_message_t *message)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;
    nb_rtps_transport_endpoint_t *ep;
    char method[NB_GLOBAL_MAX_ROUTE_BYTES];

    (void)target;
    if (!inst || !message) {
        return NB_ERR_INVALID;
    }
    nb_rtps_transport_method(message->route, method, sizeof(method));
    if (!method[0]) {
        return NB_ERR_INVALID;
    }
    if (message->kind == KIND_EVENT) {
        if (inst->options.intra_process != NB_INTRA_PROCESS_OFF) {
            nb_rtps_transport_local_deliver(inst, message);
            if (inst->options.intra_process == NB_INTRA_PROCESS_ONLY) {
                return NB_OK;
            }
        }
        ep = nb_rtps_transport_ensure(inst, method, 4);
        if (!ep) {
            return NB_ERR_NOMEM;
        }
        return nb_rtps_transport_publish(inst, ep->wr_evt, message);
    }
    ep = nb_rtps_transport_ensure(inst, method, 2);
    if (!ep) {
        return NB_ERR_NOMEM;
    }
    return nb_rtps_transport_publish(inst, ep->wr_req, message);
}

static int nb_rtps_op_reply(nb_transport_instance_t *instance,
                            void *reply_token,
                            const nb_endpoint_t *peer,
                            const nb_transport_message_t *message)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;
    nb_rtps_transport_endpoint_t *ep = (nb_rtps_transport_endpoint_t *)reply_token;

    (void)peer;
    if (!inst || !ep || !message) {
        return NB_ERR_INVALID;
    }
    if (!ep->have_responder) {
        return NB_ERR_STATE;
    }
    return nb_rtps_transport_publish(inst, ep->wr_res, message);
}

static int nb_rtps_op_register_method(nb_transport_instance_t *instance,
                                      const char *service,
                                      const char *method)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;
    char route[NB_GLOBAL_MAX_ROUTE_BYTES];
    char name[NB_GLOBAL_MAX_ROUTE_BYTES];
    int n;

    if (!inst || !service || !service[0] || !method || !method[0]) {
        return NB_ERR_INVALID;
    }
    n = snprintf(route, sizeof(route), "%s.%s", service, method);
    if (n < 0 || (size_t)n >= sizeof(route)) {
        return NB_ERR_TOOBIG;
    }
    nb_rtps_transport_method(route, name, sizeof(name));
    return nb_rtps_transport_ensure(inst, name, 1) ? NB_OK : NB_ERR_NOMEM;
}

static void nb_rtps_op_unregister_method(nb_transport_instance_t *instance,
                                         const char *service,
                                         const char *method)
{
    (void)instance;
    (void)service;
    (void)method;
}

static int nb_rtps_op_register_topic(nb_transport_instance_t *instance,
                                     const char *topic,
                                     uint32_t roles)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;

    if (!inst || !topic || !topic[0]) {
        return NB_ERR_INVALID;
    }
    roles &= (NB_PS_TOPIC_WRITER | NB_PS_TOPIC_READER);
    if (roles == 0) {
        return NB_ERR_INVALID;
    }
    return nb_rtps_transport_ensure_topic(inst, topic, roles) ? NB_OK : NB_ERR_NOMEM;
}

static size_t nb_rtps_op_discovery_read(nb_transport_instance_t *instance,
                                        int topic,
                                        nb_discovery_sample_t *out,
                                        size_t max)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;
    rtps_discovery_sample_t *tmp;
    size_t count;
    size_t i;

    if (!inst || !inst->participant || !out || max == 0) {
        return 0;
    }
    tmp = (rtps_discovery_sample_t *)calloc(max, sizeof(*tmp));
    if (!tmp) {
        return 0;
    }
    count = rtps_participant_discovery_read(
        inst->participant, (rtps_builtin_topic_t)topic, tmp, max);
    for (i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].topic = (nb_builtin_topic_t)tmp[i].topic;
        memcpy(out[i].handle, tmp[i].handle, sizeof(out[i].handle));
        nb_copy_str(out[i].name, sizeof(out[i].name), tmp[i].name, "");
        nb_copy_str(out[i].type_name, sizeof(out[i].type_name), tmp[i].type_name, "");
        nb_copy_str(out[i].host, sizeof(out[i].host), tmp[i].host, "");
        out[i].port = tmp[i].port;
        out[i].is_local = tmp[i].is_local;
        out[i].is_writer = tmp[i].is_writer;
        nb_copy_str(out[i].partition, sizeof(out[i].partition), tmp[i].partition, "");
        out[i].reliability = tmp[i].reliability;
        out[i].durability = tmp[i].durability;
        out[i].history_kind = tmp[i].history_kind;
        out[i].history_depth = tmp[i].history_depth;
        out[i].deadline_ms = tmp[i].deadline_ms;
        out[i].lifespan_ms = tmp[i].lifespan_ms;
        out[i].liveliness_kind = tmp[i].liveliness_kind;
        out[i].liveliness_lease_ms = tmp[i].liveliness_lease_ms;
        out[i].ownership_strength = tmp[i].ownership_strength;
        out[i].lease_ms = tmp[i].lease_ms;
        out[i].writer_count = tmp[i].writer_count;
        out[i].reader_count = tmp[i].reader_count;
    }
    free(tmp);
    return count;
}

static void nb_rtps_op_metrics_read(nb_transport_instance_t *instance, nb_metrics_t *out)
{
    nb_rtps_transport_t *inst = (nb_rtps_transport_t *)instance;
    rtps_metrics_t metrics;

    if (!inst || !inst->participant || !out) {
        return;
    }
    memset(&metrics, 0, sizeof(metrics));
    if (rtps_participant_metrics(inst->participant, &metrics) != RTPS_OK) {
        return;
    }
    out->rtps_packets_sent = metrics.packets_sent;
    out->rtps_packets_received = metrics.packets_received;
    out->rtps_bytes_sent = metrics.bytes_sent;
    out->rtps_bytes_received = metrics.bytes_received;
    out->rtps_spdp_sent = metrics.spdp_sent;
    out->rtps_spdp_received = metrics.spdp_received;
    out->rtps_sedp_sent = metrics.sedp_sent;
    out->rtps_sedp_received = metrics.sedp_received;
    out->rtps_data_sent = metrics.data_sent;
    out->rtps_data_received = metrics.data_received;
    out->rtps_heartbeats_sent = metrics.heartbeats_sent;
    out->rtps_heartbeats_received = metrics.heartbeats_received;
    out->rtps_acknacks_sent = metrics.acknacks_sent;
    out->rtps_acknacks_received = metrics.acknacks_received;
    out->rtps_gaps_sent = metrics.gaps_sent;
    out->rtps_gaps_received = metrics.gaps_received;
    out->rtps_fragments_sent = metrics.fragments_sent;
    out->rtps_fragments_received = metrics.fragments_received;
    out->rtps_nack_frags_sent = metrics.nack_frags_sent;
    out->rtps_nack_frags_received = metrics.nack_frags_received;
    out->rtps_parse_errors = metrics.parse_errors;
    out->rtps_samples_dropped = metrics.samples_dropped;
    out->rtps_lease_expired = metrics.lease_expired;
    out->rtps_participants = metrics.participants;
    out->rtps_publications = metrics.publications;
    out->rtps_subscriptions = metrics.subscriptions;
}

static const nb_transport_provider_ops_t nb_g_rtps_ops = {
    .abi_version = NB_TRANSPORT_PROVIDER_ABI_VERSION,
    .struct_size = sizeof(nb_transport_provider_ops_t),
    .create = nb_rtps_op_create,
    .start = nb_rtps_op_start,
    .stop = nb_rtps_op_stop,
    .destroy = nb_rtps_op_destroy,
    .send = nb_rtps_op_send,
    .reply = nb_rtps_op_reply,
    .register_method = nb_rtps_op_register_method,
    .unregister_method = nb_rtps_op_unregister_method,
    .register_topic = nb_rtps_op_register_topic,
    .discovery_read = nb_rtps_op_discovery_read,
    .metrics_read = nb_rtps_op_metrics_read,
};

static const nb_transport_provider_t nb_g_rtps_provider = {
    .scheme = "rtps",
    .ops = &nb_g_rtps_ops,
};

const nb_transport_provider_t *nb_rtps_transport_provider(void)
{
    return &nb_g_rtps_provider;
}

int nb_rtps_transport_register(void)
{
    if (nb_transport_provider_find("rtps")) {
        return NB_OK;
    }
    return nb_transport_provider_register(&nb_g_rtps_provider);
}
