#ifndef NB_RTPS_TCP_H
#define NB_RTPS_TCP_H

/*
 * RTPS over TCP.
 *
 * The RTPS specification treats a connection-oriented transport as a byte
 * stream that carries ordinary RTPS messages: the 20-octet header followed by
 * submessages. There is no extra length prefix, so the receiver has to
 * reassemble the stream itself: it looks for the next "RTPS" header, and then
 * consumes submessages by their own 16-bit length until the next header
 * arrives. That is what this module does.
 *
 * One instance owns a listening socket plus a small table of connections. It
 * knows nothing about participants: every decoded submessage is handed to the
 * callback, which is the same per-submessage entry point the UDP path uses.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_message.h"
#include "rtps_types.h"

/* Connections a participant keeps open at once (inbound plus outbound). */
#define NB_RTPS_TCP_MAX_CONNECTIONS 16u
/* Addressing key used in place of a host:port pair for a TCP peer. The
 * participant threads it through the send paths exactly like an address. */
#define NB_RTPS_TCP_KEY_PREFIX "tcp+"
#define NB_RTPS_TCP_KEY_MAX 16u
#define NB_RTPS_TCP_HOST_MAX 64u
/* Reassembly buffer per connection. One RTPS message is at most a fragment
 * burst; a peer that runs ahead of us is dropped rather than swallowing
 * memory. */
#define NB_RTPS_TCP_RX_MAX (128u * 1024u)
/* Outbound queue per connection, for uploads that outpace the socket. */
#define NB_RTPS_TCP_TX_MAX (512u * 1024u)

typedef struct nb_rtps_tcp nb_rtps_tcp_t;
struct ntk_poller;

/* One decoded submessage. `index` is the connection it arrived on. */
typedef void (*nb_rtps_tcp_message_fn)(void *user,
                                       int index,
                                       const nb_rtps_header_t *header,
                                       const nb_rtps_submessage_view_t *view,
                                       nb_rtps_receiver_state_t *state);

/* A connection became usable / went away. */
typedef void (*nb_rtps_tcp_link_fn)(void *user, int index);

typedef struct nb_rtps_tcp_stats {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint32_t links;
    uint32_t rejected;
} nb_rtps_tcp_stats_t;

/* Listen on `bind_host` (NULL or "" = every interface). `port` 0 asks the
 * system for one; nb_rtps_tcp_port() reports the result. `poller` may be NULL
 * and set later with nb_rtps_tcp_set_poller(). */
nb_rtps_tcp_t *nb_rtps_tcp_create(const char *bind_host,
                                  uint16_t port,
                                  struct ntk_poller *poller,
                                  nb_rtps_tcp_message_fn on_message,
                                  nb_rtps_tcp_link_fn on_link,
                                  nb_rtps_tcp_link_fn on_unlink,
                                  void *user);

void nb_rtps_tcp_destroy(nb_rtps_tcp_t *tcp);

/* Register the listening socket (and any open connection) with the poller.
 * Called once the participant's poller exists. */
int nb_rtps_tcp_set_poller(nb_rtps_tcp_t *tcp, struct ntk_poller *poller);

uint16_t nb_rtps_tcp_port(const nb_rtps_tcp_t *tcp);

/* Start a connection. Returns the slot index, or -1. The connection is
 * non-blocking; it completes inside nb_rtps_tcp_service(). */
int nb_rtps_tcp_connect(nb_rtps_tcp_t *tcp, const char *host, uint16_t port);

/* Is there a slot for this address already (connecting or established)? */
int nb_rtps_tcp_has_link(const nb_rtps_tcp_t *tcp, const char *host, uint16_t port);

/* Index of an established connection to this address, or -1. */
int nb_rtps_tcp_find_link(const nb_rtps_tcp_t *tcp, const char *host, uint16_t port);

/* The addressing key for a slot ("tcp+3"), or NULL. */
const char *nb_rtps_tcp_key(const nb_rtps_tcp_t *tcp, int index);

/* Slot index for a key, or -1. */
int nb_rtps_tcp_key_index(const char *key);

/* Establish state of a slot. */
int nb_rtps_tcp_established(const nb_rtps_tcp_t *tcp, int index);

/* Address a slot connects to. Empty for inbound connections until the peer
 * identifies itself. */
void nb_rtps_tcp_link_info(const nb_rtps_tcp_t *tcp,
                           int index,
                           char *host,
                           size_t cap,
                           uint16_t *port);

/* Queue bytes for a slot. Returns 0 when queued (or written), -1 on error. */
int nb_rtps_tcp_send(nb_rtps_tcp_t *tcp, int index, const void *data, size_t len);

/* Accept pending connections, finish in-progress connects, read, parse and
 * dispatch, flush queued output, and reap dead links. Call from the
 * participant thread. */
void nb_rtps_tcp_service(nb_rtps_tcp_t *tcp);

size_t nb_rtps_tcp_links(const nb_rtps_tcp_t *tcp);

void nb_rtps_tcp_stats(const nb_rtps_tcp_t *tcp, nb_rtps_tcp_stats_t *out);

#endif /* NB_RTPS_TCP_H */
