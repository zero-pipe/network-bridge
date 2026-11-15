/* 入站投递：把一条完整 envelope 帧解码成 nb_message_t 并交给 node。 */
#include "deliver.h"

#include "envelope.h"

#include <nb/base/buf.h>

int nb_transport_deliver_frame(nb_transport_t *t,
                               const uint8_t *body,
                               size_t body_len,
                               const char *peer_host,
                               uint16_t peer_port,
                               void *reply_token,
                               nb_buf_t *frame_owner)
{
    nb_envelope_t env;
    nb_message_t msg;
    char route[NB_GLOBAL_MAX_ROUTE_BYTES];

    if (!t || !t->node) {
        if (frame_owner) {
            nb_buf_release(frame_owner);
        }
        return NB_ERR_INVALID;
    }
    if (!body || nb_envelope_decode(body, body_len, &env) != NB_OK) {
        if (frame_owner) {
            nb_buf_release(frame_owner);
        }
        return NB_ERR_INVALID;
    }
    nb_envelope_to_message(&env, route, sizeof(route), &msg);
    msg.peer_host = peer_host;
    msg.peer_port = peer_port;
    msg.reply_token = reply_token;
    msg.frame_owner = frame_owner;
    nb_node_on_message(t->node, t, &msg);
    return NB_OK;
}
