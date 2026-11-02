/*
 * Cyclone DDS transport provider.
 *
 * The provider carries the framework's encoded v2 frame as an opaque octet
 * sample. Topics are derived from the full "Service.method" route, so services
 * sharing a method name do not collide:
 *   request  -> <Service>_<method>_req
 *   response -> <Service>_<method>_res
 *   event    -> <Service>_<method>_evt
 *
 * Service registration calls the provider subscribe hook, which creates the
 * request/event readers and the response writer. Client requests lazily create
 * the request writer and response reader. Events are one-way publishes.
 */
#include "transport_internal.h"

#include "envelope.h"
#include <nb/poller_alloc.h>
#include "util.h"

#include <nb/log.h>

#include <dds/dds.h>

#include "cy_dds_message.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_DDS_MAX_ENDPOINTS 32
#define NB_DDS_MAX_SAMPLES 32
#define NB_DDS_TOPIC_MAX 160

typedef struct nb_dds_instance nb_dds_instance_t;

typedef struct nb_dds_endpoint {
    int used;
    char method[NB_GLOBAL_MAX_ROUTE_BYTES];
    char topic_req[NB_DDS_TOPIC_MAX];
    char topic_res[NB_DDS_TOPIC_MAX];
    char topic_evt[NB_DDS_TOPIC_MAX];
    dds_entity_t topic_req_ent;
    dds_entity_t topic_res_ent;
    dds_entity_t topic_evt_ent;
    dds_entity_t wr_req;
    dds_entity_t rd_req;
    dds_entity_t wr_res;
    dds_entity_t rd_res;
    dds_entity_t wr_evt;
    dds_entity_t rd_evt;
    dds_listener_t *listener;
    int have_responder;
    int have_requester;
    int have_publisher;
    int local_event;
    nb_dds_instance_t *owner;
} nb_dds_endpoint_t;

struct nb_dds_instance {
    nb_transport_provider_options_t options;
    dds_entity_t participant;
    char authority[NB_HOST_MAX];
    ntk_mutex *lock;
    nb_dds_endpoint_t endpoints[NB_DDS_MAX_ENDPOINTS];
    size_t endpoint_count;
};

static int nb_dds_status(int status, const char *what)
{
    if (status >= 0) {
        return NB_OK;
    }
    nb_log_error("cyclone dds %s failed: %s (%d)",
                 what ? what : "call",
                 dds_strretcode(status),
                 status);
    return NB_ERR_IO;
}

static void nb_dds_sanitize(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    size_t j = 0;

    if (!out || cap == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    if (in[0] >= '0' && in[0] <= '9') {
        out[j++] = '_';
    }
    for (i = 0; in[i] != '\0' && j + 1 < cap; ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

/* Topic key: the full "Service.method" route, sanitized. Including the service
 * keeps two services that share a method name on distinct DDS topics. */
static void nb_dds_method_from_route(const char *route, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!route) {
        return;
    }
    nb_dds_sanitize(route, out, cap);
}

static void nb_dds_endpoint_topics(nb_dds_endpoint_t *ep)
{
    char suffix[8];

    nb_dds_sanitize(ep->method, ep->topic_req, sizeof(ep->topic_req));
    snprintf(suffix, sizeof(suffix), "_req");
    strncat(ep->topic_req, suffix, sizeof(ep->topic_req) - strlen(ep->topic_req) - 1);

    nb_dds_sanitize(ep->method, ep->topic_res, sizeof(ep->topic_res));
    snprintf(suffix, sizeof(suffix), "_res");
    strncat(ep->topic_res, suffix, sizeof(ep->topic_res) - strlen(ep->topic_res) - 1);

    nb_dds_sanitize(ep->method, ep->topic_evt, sizeof(ep->topic_evt));
    snprintf(suffix, sizeof(suffix), "_evt");
    strncat(ep->topic_evt, suffix, sizeof(ep->topic_evt) - strlen(ep->topic_evt) - 1);
}

static void nb_dds_endpoint_init(nb_dds_endpoint_t *ep, nb_dds_instance_t *owner, const char *method)
{
    memset(ep, 0, sizeof(*ep));
    ep->used = 1;
    ep->owner = owner;
    nb_copy_str(ep->method, sizeof(ep->method), method, "topic");
    ep->topic_req_ent = DDS_ENTITY_NIL;
    ep->topic_res_ent = DDS_ENTITY_NIL;
    ep->topic_evt_ent = DDS_ENTITY_NIL;
    ep->wr_req = DDS_ENTITY_NIL;
    ep->rd_req = DDS_ENTITY_NIL;
    ep->wr_res = DDS_ENTITY_NIL;
    ep->rd_res = DDS_ENTITY_NIL;
    ep->wr_evt = DDS_ENTITY_NIL;
    ep->rd_evt = DDS_ENTITY_NIL;
    nb_dds_endpoint_topics(ep);
}

static dds_entity_t nb_dds_make_topic(nb_dds_instance_t *inst, const char *name)
{
    dds_entity_t topic = dds_create_topic(inst->participant, &dds_Message_desc, name, NULL, NULL);
    if (topic < 0) {
        nb_log_error("cyclone dds topic create failed: %s (%d)", name, topic);
    }
    return topic;
}

/*
 * Map framework QoS to Cyclone DDS.  Inherit (0) leaves the DDS default in
 * place; only explicitly configured policies are written.
 */
static dds_qos_t *nb_dds_make_qos(nb_dds_instance_t *inst, int ignore_local)
{
    const nb_qos_t *q = &inst->options.qos;
    dds_qos_t *qos;

    if (!ignore_local && nb_qos_is_default(q)) {
        return NULL;
    }
    qos = dds_create_qos();
    if (!qos) {
        return NULL;
    }
    if (ignore_local) {
        dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
    }
    if (q->reliability == NB_QOS_RELIABLE) {
        dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
    } else if (q->reliability == NB_QOS_BEST_EFFORT) {
        dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
    }
    if (q->durability == NB_QOS_TRANSIENT_LOCAL) {
        dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
    } else if (q->durability == NB_QOS_VOLATILE) {
        dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
    }
    if (q->history == NB_QOS_KEEP_ALL) {
        dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, 0);
    } else if (q->history == NB_QOS_KEEP_LAST) {
        dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, (int32_t)(q->depth ? q->depth : 1u));
    }
    if (q->deadline_ms > 0) {
        dds_qset_deadline(qos, DDS_MSECS(q->deadline_ms));
    }
    if (q->lifespan_ms > 0) {
        dds_qset_lifespan(qos, DDS_MSECS(q->lifespan_ms));
    }
    if (q->latency_budget_ms > 0) {
        dds_qset_latency_budget(qos, DDS_MSECS(q->latency_budget_ms));
    }
    if (q->min_separation_ms > 0) {
        dds_qset_time_based_filter(qos, DDS_MSECS(q->min_separation_ms));
    }
    if (q->lease_duration_ms > 0 ||
        q->liveliness == NB_QOS_LIVELINESS_MANUAL_BY_PARTICIPANT ||
        q->liveliness == NB_QOS_LIVELINESS_MANUAL_BY_TOPIC) {
        dds_liveliness_kind_t kind = DDS_LIVELINESS_AUTOMATIC;
        if (q->liveliness == NB_QOS_LIVELINESS_MANUAL_BY_PARTICIPANT) {
            kind = DDS_LIVELINESS_MANUAL_BY_PARTICIPANT;
        } else if (q->liveliness == NB_QOS_LIVELINESS_MANUAL_BY_TOPIC) {
            kind = DDS_LIVELINESS_MANUAL_BY_TOPIC;
        }
        dds_qset_liveliness(qos, kind, DDS_MSECS(q->lease_duration_ms));
    }
    if (q->ownership == NB_QOS_OWNERSHIP_EXCLUSIVE) {
        dds_qset_ownership(qos, DDS_OWNERSHIP_EXCLUSIVE);
    } else if (q->ownership == NB_QOS_OWNERSHIP_SHARED) {
        dds_qset_ownership(qos, DDS_OWNERSHIP_SHARED);
    }
    if (q->presentation_scope != NB_QOS_PRESENTATION_INSTANCE ||
        q->coherent_access || q->ordered_access) {
        dds_presentation_access_scope_kind_t scope = DDS_PRESENTATION_INSTANCE;

        if (q->presentation_scope == NB_QOS_PRESENTATION_TOPIC) {
            scope = DDS_PRESENTATION_TOPIC;
        } else if (q->presentation_scope == NB_QOS_PRESENTATION_GROUP) {
            scope = DDS_PRESENTATION_GROUP;
        }
        dds_qset_presentation(qos, scope, q->coherent_access, q->ordered_access);
    }
    if (q->partition[0]) {
        dds_qset_partition1(qos, q->partition);
    }
    if (q->max_samples > 0 || q->max_instances > 0 || q->max_samples_per_instance > 0) {
        dds_qset_resource_limits(qos,
                                 q->max_samples > 0 ? (int32_t)q->max_samples
                                                    : DDS_LENGTH_UNLIMITED,
                                 q->max_instances > 0 ? (int32_t)q->max_instances
                                                      : DDS_LENGTH_UNLIMITED,
                                 q->max_samples_per_instance > 0
                                     ? (int32_t)q->max_samples_per_instance
                                     : DDS_LENGTH_UNLIMITED);
    }
    dds_qset_writer_data_lifecycle(qos, q->autodispose_unregistered_instances);
    return qos;
}

static dds_entity_t nb_dds_make_reader(nb_dds_instance_t *inst,
                                       dds_entity_t topic,
                                       dds_listener_t *listener,
                                       int ignore_local)
{
    dds_qos_t *qos = nb_dds_make_qos(inst, ignore_local);
    dds_entity_t reader = dds_create_reader(inst->participant, topic, qos, listener);

    if (qos) {
        dds_delete_qos(qos);
    }
    return reader;
}

static dds_entity_t nb_dds_make_writer(nb_dds_instance_t *inst, dds_entity_t topic)
{
    dds_qos_t *qos = nb_dds_make_qos(inst, 0);
    dds_entity_t writer = dds_create_writer(inst->participant, topic, qos, NULL);
    if (qos) {
        dds_delete_qos(qos);
    }
    return writer;
}

/*
 * 接收走 dds_take loan：samples/payload 指向 reader 借出的内存，整批样本在
 * 本回调返回后必须交给 dds_return_loan 归还。因此这些指针只在同步 on_receive
 * 期间有效，传输层不把它们包装成跨线程 frame_owner。
 *
 * 跨线程规则：INLINE handler 直接在回调内借用；WORKER/EXECUTOR、跨 poller
 * 响应等需要延后消费的路径由 core 在入队时深拷贝（frame_owner 为 NULL 的
 * 默认分支）。如果将来要为 DDS 做零拷贝所有权转移，必须让 loan 的生命周期
 * 覆盖所有消费者，不能在这里提前 return_loan。e2e.dds_loan 覆盖该契约。
 */
static void nb_dds_on_data(dds_entity_t reader, void *arg)
{
    nb_dds_endpoint_t *ep = (nb_dds_endpoint_t *)arg;
    void *samples[NB_DDS_MAX_SAMPLES];
    dds_sample_info_t infos[NB_DDS_MAX_SAMPLES];
    dds_return_t count;
    int32_t i;

    if (!ep || !ep->owner) {
        return;
    }
    memset(samples, 0, sizeof(samples));
    memset(infos, 0, sizeof(infos));
    count = dds_take(reader, samples, infos, NB_DDS_MAX_SAMPLES, NB_DDS_MAX_SAMPLES);
    if (count <= 0) {
        return;
    }
    for (i = 0; i < count; ++i) {
        dds_Message *sample = (dds_Message *)samples[i];
        nb_envelope_t env;
        nb_transport_message_t msg;
        nb_endpoint_t peer;
        char route_buf[NB_GLOBAL_MAX_ROUTE_BYTES];

        if (!infos[i].valid_data || !sample || !sample->payload._buffer || sample->payload._length == 0) {
            continue;
        }
        if (nb_envelope_decode(sample->payload._buffer, sample->payload._length, &env) != NB_OK) {
            continue;
        }
        memset(&msg, 0, sizeof(msg));
        msg.kind = env.kind;
        msg.flags = env.flags;
        msg.status = env.status;
        msg.request_id = env.request_id;
        msg.timeout_ms = env.timeout_ms;
        msg.metadata = env.metadata;
        msg.metadata_len = env.metadata_len;
        msg.status_msg = env.status_msg;
        msg.status_msg_len = env.status_msg_len;
        if (env.route && env.route_len < sizeof(route_buf)) {
            memcpy(route_buf, env.route, env.route_len);
            route_buf[env.route_len] = '\0';
            msg.route = route_buf;
        }
        msg.payload.data = env.payload;
        msg.payload.len = env.payload_len;
        msg.payload.encoding = env.encoding;
        if (env.stream_flags != 0 || env.stream_id != 0) {
            msg.has_stream = 1;
            msg.stream.flags = env.stream_flags;
            msg.stream.stream_id = env.stream_id;
            msg.stream.offset = env.stream_offset;
            msg.stream.total_size = env.stream_total_size;
        }
        if (nb_endpoint_from_parts(&peer,
                                   "dds",
                                   ep->owner->authority,
                                   1,
                                   nb_transport_kind_from_scheme("dds")) == NB_OK) {
            msg.peer = &peer;
            msg.peer_host = peer.host;
            msg.peer_port = peer.port;
        }
        ep->owner->options.on_receive(ep->owner->options.user, ep, &msg);
    }
    dds_return_loan(reader, samples, count);
}

static nb_dds_endpoint_t *nb_dds_endpoint_ensure(nb_dds_instance_t *inst, const char *method, int roles)
{
    nb_dds_endpoint_t *ep = NULL;
    size_t i;

    if (!inst || !method || !method[0]) {
        return NULL;
    }
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        if (inst->endpoints[i].used && strcmp(inst->endpoints[i].method, method) == 0) {
            ep = &inst->endpoints[i];
            break;
        }
    }
    if (!ep) {
        if (inst->endpoint_count >= NB_DDS_MAX_ENDPOINTS) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep = &inst->endpoints[inst->endpoint_count++];
        nb_dds_endpoint_init(ep, inst, method);
        ep->topic_req_ent = nb_dds_make_topic(inst, ep->topic_req);
        ep->topic_res_ent = nb_dds_make_topic(inst, ep->topic_res);
        ep->topic_evt_ent = nb_dds_make_topic(inst, ep->topic_evt);
        if (ep->topic_req_ent < 0 || ep->topic_res_ent < 0 || ep->topic_evt_ent < 0) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
    }
    if ((roles & 1) && !ep->have_responder) { /* responder */
        ep->listener = dds_create_listener(ep);
        if (!ep->listener) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        dds_lset_data_available(ep->listener, nb_dds_on_data);
        ep->rd_req = nb_dds_make_reader(inst, ep->topic_req_ent, ep->listener, 0);
        if (inst->options.intra_process != NB_INTRA_PROCESS_ONLY) {
            ep->rd_evt = nb_dds_make_reader(inst,
                                            ep->topic_evt_ent,
                                            ep->listener,
                                            inst->options.intra_process == NB_INTRA_PROCESS_AUTO);
        }
        ep->wr_res = nb_dds_make_writer(inst, ep->topic_res_ent);
        if (ep->rd_req < 0 || ep->wr_res < 0 ||
            (inst->options.intra_process != NB_INTRA_PROCESS_ONLY && ep->rd_evt < 0)) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        if (inst->options.intra_process != NB_INTRA_PROCESS_OFF) {
            ep->local_event = 1;
        }
        ep->have_responder = 1;
    }
    if ((roles & 2) && !ep->have_requester) { /* requester */
        ep->wr_req = nb_dds_make_writer(inst, ep->topic_req_ent);
        if (!ep->listener) {
            ep->listener = dds_create_listener(ep);
            if (!ep->listener) {
                ntk_mutex_unlock(inst->lock);
                return NULL;
            }
            dds_lset_data_available(ep->listener, nb_dds_on_data);
        }
            ep->rd_res = nb_dds_make_reader(inst, ep->topic_res_ent, ep->listener, 0);
        if (ep->wr_req < 0 || ep->rd_res < 0) {
            ntk_mutex_unlock(inst->lock);
            return NULL;
        }
        ep->have_requester = 1;
    }
    if ((roles & 4) && !ep->have_publisher) { /* publisher */
        if (inst->options.intra_process != NB_INTRA_PROCESS_ONLY) {
            ep->wr_evt = nb_dds_make_writer(inst, ep->topic_evt_ent);
            if (ep->wr_evt < 0) {
                ntk_mutex_unlock(inst->lock);
                return NULL;
            }
        }
        ep->have_publisher = 1;
    }
    ntk_mutex_unlock(inst->lock);
    return ep;
}

static int nb_dds_publish(nb_dds_instance_t *inst, dds_entity_t writer, const nb_transport_message_t *message)
{
    nb_envelope_t env;
    dds_Message sample;
    uint8_t *wire;
    uint8_t inline_wire[512];
    int wire_heap = 0;
    size_t len;
    int rc;

    if (writer == DDS_ENTITY_NIL || !message) {
        return NB_ERR_INVALID;
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
    len = nb_envelope_size(&env);
    if (len == 0) {
        return NB_ERR_INVALID;
    }
    if (len <= sizeof(inline_wire)) {
        wire = inline_wire;
    } else {
        wire = (uint8_t *)nb_poller_alloc(len);
        if (!wire) {
            return NB_ERR_NOMEM;
        }
        wire_heap = 1;
    }
    if (nb_envelope_encode(wire, len, &env) != len) {
        if (wire_heap) {
            nb_poller_free(wire);
        }
        return NB_ERR_IO;
    }
    memset(&sample, 0, sizeof(sample));
    sample.payload._buffer = wire;
    sample.payload._length = (uint32_t)len;
    sample.payload._maximum = (uint32_t)len;
    sample.payload._release = false;
    rc = dds_write(writer, &sample);
    if (wire_heap) {
        nb_poller_free(wire);
    }
    (void)inst;
    return nb_dds_status(rc, "dds_write");
}

/*
 * Deliver one event to every in-process subscriber of the same topic without
 * encoding a frame or touching the DDS reader path. The message pointers stay
 * valid for the synchronous callback; no payload copy is made.
 */
static void nb_dds_local_deliver(nb_dds_instance_t *inst,
                                 const nb_transport_message_t *message,
                                 const char *route)
{
    nb_dds_endpoint_t *targets[NB_DDS_MAX_ENDPOINTS];
    char route_key[NB_DDS_TOPIC_MAX];
    char ep_key[NB_DDS_TOPIC_MAX];
    size_t count = 0;
    size_t i;

    if (!inst || !message || !route || !inst->options.on_receive) {
        return;
    }
    /* Endpoints are keyed by the raw "Service.method" route; topics use the
     * sanitized form. Accept either spelling so a registration that was
     * already sanitized still matches. */
    nb_dds_sanitize(route, route_key, sizeof(route_key));
    ntk_mutex_lock(inst->lock);
    for (i = 0; i < inst->endpoint_count; ++i) {
        nb_dds_endpoint_t *ep = &inst->endpoints[i];
        if (!ep->used || !ep->local_event) {
            continue;
        }
        if (strcmp(ep->method, route) == 0 || strcmp(ep->method, route_key) == 0) {
            targets[count++] = ep;
            continue;
        }
        nb_dds_sanitize(ep->method, ep_key, sizeof(ep_key));
        if (strcmp(ep_key, route_key) == 0) {
            targets[count++] = ep;
        }
    }
    ntk_mutex_unlock(inst->lock);

    for (i = 0; i < count; ++i) {
        inst->options.on_receive(inst->options.user, targets[i], message);
    }
}

/* ---- provider ops ---- */

static int nb_dds_op_create(const nb_transport_provider_options_t *options, nb_transport_instance_t **out)
{
    nb_dds_instance_t *inst;
    const char *p;
    char domain_text[16];
    size_t n = 0;
    int domain = 0;

    if (!options || !out) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    inst = (nb_dds_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) {
        return NB_ERR_NOMEM;
    }
    inst->options = *options;
    inst->participant = DDS_ENTITY_NIL;

    p = options->endpoint ? strstr(options->endpoint, "://") : NULL;
    if (p) {
        p += 3;
        while (p[n] && p[n] != ':' && n + 1 < sizeof(domain_text)) {
            domain_text[n] = p[n];
            n++;
        }
    }
    domain_text[n] = '\0';
    if (domain_text[0]) {
        domain = (int)strtol(domain_text, NULL, 10);
    }
    nb_copy_str(inst->authority, sizeof(inst->authority), domain_text[0] ? domain_text : "0", "0");

    inst->participant = dds_create_participant(domain, NULL, NULL);
    if (inst->participant < 0) {
        nb_log_error("cyclone dds participant create failed: %s (%d)",
                     dds_strretcode(inst->participant),
                     inst->participant);
        free(inst);
        return NB_ERR_IO;
    }
    inst->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!inst->lock) {
        dds_delete(inst->participant);
        free(inst);
        return NB_ERR_NOMEM;
    }
    *out = (nb_transport_instance_t *)inst;
    return NB_OK;
}

static int nb_dds_op_start(nb_transport_instance_t *instance)
{
    return instance ? NB_OK : NB_ERR_INVALID;
}

static void nb_dds_op_stop(nb_transport_instance_t *instance)
{
    (void)instance;
}

static void nb_dds_op_destroy(nb_transport_instance_t *instance)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    size_t i;

    if (!inst) {
        return;
    }
    for (i = 0; i < inst->endpoint_count; ++i) {
        nb_dds_endpoint_t *ep = &inst->endpoints[i];
        if (!ep->used) {
            continue;
        }
        if (ep->rd_req != DDS_ENTITY_NIL) dds_delete(ep->rd_req);
        if (ep->rd_res != DDS_ENTITY_NIL) dds_delete(ep->rd_res);
        if (ep->rd_evt != DDS_ENTITY_NIL) dds_delete(ep->rd_evt);
        if (ep->wr_req != DDS_ENTITY_NIL) dds_delete(ep->wr_req);
        if (ep->wr_res != DDS_ENTITY_NIL) dds_delete(ep->wr_res);
        if (ep->wr_evt != DDS_ENTITY_NIL) dds_delete(ep->wr_evt);
        if (ep->listener) dds_delete_listener(ep->listener);
        if (ep->topic_req_ent != DDS_ENTITY_NIL) dds_delete(ep->topic_req_ent);
        if (ep->topic_res_ent != DDS_ENTITY_NIL) dds_delete(ep->topic_res_ent);
        if (ep->topic_evt_ent != DDS_ENTITY_NIL) dds_delete(ep->topic_evt_ent);
    }
    if (inst->participant != DDS_ENTITY_NIL) {
        dds_delete(inst->participant);
    }
    if (inst->lock) {
        ntk_mutex_destroy(inst->lock);
    }
    free(inst);
}

static int nb_dds_op_send(nb_transport_instance_t *instance,
                          const nb_endpoint_t *target,
                          const nb_transport_message_t *message)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    nb_dds_endpoint_t *ep;
    char method[NB_GLOBAL_MAX_ROUTE_BYTES];
    int roles;
    (void)target;

    if (!inst || !message) {
        return NB_ERR_INVALID;
    }
    nb_dds_method_from_route(message->route, method, sizeof(method));
    if (!method[0]) {
        return NB_ERR_INVALID;
    }
    if (message->kind == KIND_EVENT) {
        if (inst->options.intra_process != NB_INTRA_PROCESS_OFF) {
            nb_dds_local_deliver(inst, message, message->route);
            if (inst->options.intra_process == NB_INTRA_PROCESS_ONLY) {
                return NB_OK;
            }
        }
        roles = 4;
        ep = nb_dds_endpoint_ensure(inst, method, roles);
        if (!ep) {
            return NB_ERR_NOMEM;
        }
        return ep->wr_evt == DDS_ENTITY_NIL ? NB_OK : nb_dds_publish(inst, ep->wr_evt, message);
    }
    roles = 2;
    ep = nb_dds_endpoint_ensure(inst, method, roles);
    return ep ? nb_dds_publish(inst, ep->wr_req, message) : NB_ERR_NOMEM;
}

static int nb_dds_op_reply(nb_transport_instance_t *instance,
                           void *reply_token,
                           const nb_endpoint_t *peer,
                           const nb_transport_message_t *message)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    nb_dds_endpoint_t *ep = (nb_dds_endpoint_t *)reply_token;
    (void)peer;

    if (!inst || !ep || !message || ep->wr_res == DDS_ENTITY_NIL) {
        return NB_ERR_INVALID;
    }
    return nb_dds_publish(inst, ep->wr_res, message);
}

static int nb_dds_op_register_method(nb_transport_instance_t *instance,
                               const char *service,
                               const char *method)
{
    nb_dds_instance_t *inst = (nb_dds_instance_t *)instance;
    char route[NB_GLOBAL_MAX_ROUTE_BYTES];
    int n;

    if (!inst || !service || !service[0] || !method || !method[0]) {
        return NB_ERR_INVALID;
    }
    n = snprintf(route, sizeof(route), "%s.%s", service, method);
    if (n < 0 || (size_t)n >= sizeof(route)) {
        return NB_ERR_TOOBIG;
    }
    return nb_dds_endpoint_ensure(inst, route, 1) ? NB_OK : NB_ERR_NOMEM;
}

static void nb_dds_op_unregister_method(nb_transport_instance_t *instance,
                                  const char *service,
                                  const char *method)
{
    (void)instance;
    (void)service;
    (void)method;
}

static const nb_transport_provider_ops_t nb_g_dds_ops = {
    .create = nb_dds_op_create,
    .start = nb_dds_op_start,
    .stop = nb_dds_op_stop,
    .destroy = nb_dds_op_destroy,
    .send = nb_dds_op_send,
    .reply = nb_dds_op_reply,
    .register_method = nb_dds_op_register_method,
    .unregister_method = nb_dds_op_unregister_method,
};

static const nb_transport_provider_t nb_g_dds_provider = {
    .scheme = "dds",
    .ops = &nb_g_dds_ops,
};

const nb_transport_provider_t *nb_cyclone_dds_provider(void)
{
    return &nb_g_dds_provider;
}

int nb_cyclone_dds_register(void)
{
    if (nb_transport_provider_find("dds")) {
        return NB_OK;
    }
    return nb_transport_provider_register(&nb_g_dds_provider);
}
