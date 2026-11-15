#ifndef NB_TRANSPORT_DELIVER_H
#define NB_TRANSPORT_DELIVER_H

/*
 * Inbound delivery shared by every built-in transport.
 *
 * A transport only decides *how* bytes arrive (TCP length prefix, UDP datagram
 * reassembly, SHM ring slot). Turning those bytes into an nb_message_t and
 * handing it to the node is the same step everywhere, so it lives here once:
 * one place to keep the envelope→message mapping and the frame ownership rules
 * in sync across tcp/udp/shm.
 */

#include <stddef.h>
#include <stdint.h>

#include "transport_internal.h"

/*
 * Decode one complete wire frame (envelope bytes, no transport header) and pass
 * it to nb_node_on_message(). Takes ownership of frame_owner: it is released on
 * a decode failure, and otherwise handed to the node through msg.frame_owner
 * (the node releases it after the inbound path is done with the message).
 *
 * peer_host, peer_port and reply_token describe the source and are only read
 * during the call, but peer_host must stay valid while the node handles the
 * message (the message borrows it).
 *
 * Returns NB_OK when the message was delivered, NB_ERR_INVALID when the frame
 * did not decode.
 */
int nb_transport_deliver_frame(nb_transport_t *t,
                               const uint8_t *body,
                               size_t body_len,
                               const char *peer_host,
                               uint16_t peer_port,
                               void *reply_token,
                               nb_buf_t *frame_owner);

#endif /* NB_TRANSPORT_DELIVER_H */
