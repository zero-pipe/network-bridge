#ifndef NB_SHM_H
#define NB_SHM_H

/*
 * POSIX shared-memory transport provider (same host).
 *
 * Register the provider before creating any node, then use a SHM binding:
 *
 *   node A: shm://<channel>:1
 *   node B: shm://<channel>:2
 *
 * One channel is one shared-memory segment; the port is the node's unique id
 * in that segment (1..NB_SHM_MAX_NODES). There are no fixed server/client
 * roles: any node may receive requests, reply, and publish events. In MESH
 * mode discovery broadcasts the full shm://channel:id URI, so services can also
 * be found by name. For direct calls, point the proxy endpoint at the target
 * node, for example shm://<channel>:2.
 *
 * The transport uses an iceoryx-style size-classed chunk pool, lock-free
 * queues, and offset addressing: the sender encodes the envelope directly in
 * the segment, and the receiver hands the chunk to the core as a frame_owner
 * without copying, so large payloads cross threads copy-free. The maximum
 * message size is the largest size class (see NB_SHM_MAX_MESSAGE_BYTES).
 *
 * Segment size is controlled by the NB_SHM_BYTES environment variable
 * (default 32 MB, minimum 2 MB); every process on a channel must use the same
 * size. Large-message concurrency is bounded by the chunk count in that size
 * class: when the segment temporarily has no free chunk, send returns
 * NB_ERR_AGAIN (retryable); raise NB_SHM_BYTES to increase capacity.
 *
 * Publish/subscribe: as with DDS, a subscriber is an ordinary service whose
 * method names a topic, and a publisher sends KIND_EVENT through
 * nb_proxy_call_event. Events are fanned out copy-free inside the segment to
 * every subscribed node (excluding the publisher) by route ("service.method").
 * Each node may subscribe to at most NB_SHM_MAX_TOPICS_PER_NODE topics, and one
 * channel supports at most NB_SHM_MAX_NODES nodes.
 *
 * Zero-copy publish: the shm provider implements the loan/borrow SPI, so
 * nb_publisher_loan / nb_publisher_publish_loan (see publish.h) write the
 * payload straight into a segment chunk and skip the send-side copy that the
 * ordinary event path performs.
 */

#include <nb/transport_provider.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum encoded envelope size = capacity of the largest size class. */
#define NB_SHM_MAX_MESSAGE_BYTES (1024u * 1024u)

/* Maximum nodes (server id 1 plus client ids 2..N). */
#define NB_SHM_MAX_NODES 9u
/* Maximum topics a single node may subscribe to. */
#define NB_SHM_MAX_TOPICS_PER_NODE 16u

/* Idempotently register the built-in "shm" provider. */
int nb_shm_register(void);

/* Static provider descriptor, for callers that prefer explicit registration. */
const nb_transport_provider_t *nb_shm_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_SHM_H */
