#ifndef NB_NODE_H
#define NB_NODE_H

/*
 * Node: lifecycle, transport selection, threads, and discovery configuration.
 *
 * A node owns:
 *   - one ntk_poller_pool (IO thread pool; it creates no threads of its own);
 *   - one or more bindings; when bindings are not configured explicitly, the
 *     transport config creates a single default binding;
 *   - optional service discovery (MESH) and a route table.
 */

#include <nb/types.h>
#include <nb/metrics.h>
#include <nb/service_table.h>
#include <nb/config.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle ---- */
int nb_node_create(const nb_node_config_t *config, nb_node_t **out);
void nb_node_destroy(nb_node_t *node);

/* ---- Node state and endpoints ---- */
const char *nb_node_name(const nb_node_t *node);
nb_transport_kind_t nb_node_transport(const nb_node_t *node);
uint16_t nb_node_discovery_port(const nb_node_t *node);
uint16_t nb_node_data_port(const nb_node_t *node);
size_t nb_node_binding_count(const nb_node_t *node);
int nb_node_binding_endpoint(const nb_node_t *node, size_t index, char *out, size_t cap);

/* ---- Metrics and peer quality ---- */
void nb_node_metrics(const nb_node_t *node, nb_metrics_t *out);
/* Query link statistics for one peer (host:port); returns NB_ERR_NOTFOUND when
 * the peer is unknown. */
int nb_node_peer_stats(const nb_node_t *node, const char *host, uint16_t port, nb_peer_stats_t *out);

/* ---- Discovery and routing ---- */
/* Add an explicit discovery peer (unicast HELLO target); useful for targeted
 * discovery and tests. */
int nb_node_add_discovery_peer(nb_node_t *node, const char *host, uint16_t port);

/* Static route: register service -> endpoint directly, skipping discovery. */
int nb_node_add_static_route(nb_node_t *node,
                             const char *service,
                             const char *host,
                             uint16_t port,
                             nb_transport_kind_t transport);
int nb_node_add_static_route_uri(nb_node_t *node, const char *service, const char *endpoint);

/* ---- Waiting and scheduling ---- */
/* Wait until the service appears in the route table (used during MESH startup).
 * STATIC mode returns NB_OK immediately. */
int nb_node_wait_for_service(nb_node_t *node, const char *service, uint64_t timeout_ms);

/*
 * Post a task to the node poller thread (asynchronous; does not block the
 * caller). High-throughput clients may issue the next call directly from a
 * callback or task: nb_proxy_call_async detects "already on the poller
 * thread" and runs inline, avoiding cross-thread overhead.
 */
int nb_node_post(nb_node_t *node, void (*handler)(void *user), void *user);
int nb_node_post_delay(nb_node_t *node, uint64_t delay_ms, void (*handler)(void *user), void *user);

/*
 * Post a task to the node's dedicated send lane (requires
 * runtime.send_lanes > 0). Use it to run a publish loop: nb_proxy_call_event
 * called inside the task takes the inline send fast path (no cross-thread
 * post), and the send lane is isolated from the io pollers that receive and
 * dispatch requests. Returns NB_ERR_INVALID when send_lanes == 0.
 */
int nb_node_post_send(nb_node_t *node, void (*handler)(void *user), void *user);

/* ---- Blocking run ---- */
/* Run the poller loop for at most timeout_ms; returns NB_ERR_TIMEOUT. Does not
 * install signal handlers, so it is safe inside a host application that owns
 * its own SIGINT/SIGTERM policy. */
int nb_node_run_once(nb_node_t *node, uint64_t timeout_ms);
/* Install SIGINT/SIGTERM handlers and run until one arrives (server main). */
int nb_node_run_until_signal(nb_node_t *node);
/* Compatibility alias for nb_node_run_until_signal. */
int nb_node_run(nb_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* NB_NODE_H */
