#include "dds_bridge.h"

#include "caps.h"
#include "envelope.h"

#include <stdio.h>
#include <string.h>

void nb_bridge_topic_key(const char *route, char *out, size_t cap)
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

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static void nb_bridge_topic_one(const char *key, const char *suffix, char *out, size_t cap)
{
    int n;

    if (!out || cap == 0) {
        return;
    }
    n = snprintf(out, cap, "%s%s", key, suffix);
    if (n < 0 || (size_t)n >= cap) {
        out[cap - 1] = '\0';
    }
}

void nb_bridge_route_topics(const char *route,
                            char *req,
                            size_t req_cap,
                            char *res,
                            size_t res_cap,
                            char *evt,
                            size_t evt_cap)
{
    char key[NB_GLOBAL_MAX_ROUTE_BYTES];

    nb_bridge_topic_key(route, key, sizeof(key));
    nb_bridge_topic_one(key, "_req", req, req_cap);
    nb_bridge_topic_one(key, "_res", res, res_cap);
    nb_bridge_topic_one(key, "_evt", evt, evt_cap);
}

static void nb_bridge_env_from_message(const nb_message_t *message, nb_envelope_t *env)
{
    memset(env, 0, sizeof(*env));
    env->kind = message->kind;
    env->encoding = message->payload.encoding;
    env->flags = message->flags;
    env->status = message->status;
    env->request_id = message->request_id;
    env->timeout_ms = message->timeout_ms;
    env->route = message->route;
    env->metadata = message->metadata;
    env->metadata_len = message->metadata_len;
    env->status_msg = message->status_msg;
    env->status_msg_len = message->status_msg_len;
    env->payload = message->payload.data;
    env->payload_len = message->payload.len;
    if (message->has_stream) {
        env->stream_flags = (uint8_t)message->stream.flags;
        env->stream_id = message->stream.stream_id;
        env->stream_offset = message->stream.offset;
        env->stream_total_size = message->stream.total_size;
    }
}

size_t nb_bridge_frame_size(const nb_message_t *message)
{
    nb_envelope_t env;

    if (!message) {
        return 0;
    }
    nb_bridge_env_from_message(message, &env);
    return nb_envelope_size(&env);
}

size_t nb_bridge_frame_encode(const nb_message_t *message, uint8_t *buf, size_t cap)
{
    nb_envelope_t env;
    size_t len;

    if (!message || !buf) {
        return 0;
    }
    nb_bridge_env_from_message(message, &env);
    len = nb_envelope_size(&env);
    if (len == 0 || len > cap) {
        return 0;
    }
    return nb_envelope_encode(buf, len, &env) == len ? len : 0;
}
