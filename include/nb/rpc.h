#ifndef NB_RPC_H
#define NB_RPC_H

/*
 * RPC spine: one request in, one response out (or a stream), over a
 * point-to-point link.
 *
 * A node owns the runtime (pollers, bindings, discovery, routes). A service
 * declares methods; a proxy calls them synchronously, asynchronously, one-way
 * (event) or as a stream. Calls carry metadata, a deadline and a correlation
 * id, and can be cancelled.
 *
 * Point-to-point links underneath: tcp, udp, shm. A topic-based backend can
 * also carry RPC through the RPC-over-PS bridge, but that bridge belongs to
 * this spine, not to the backend.
 *
 * What does NOT belong here: topics, samples, reader histories, waitsets,
 * DDS status events and the DDS QoS policy set. Those live in <nb/pubsub.h>.
 * The aliases in publish.h / subscribe.h are the legacy spelling of the
 * pub/sub spine and are kept only for source compatibility; new code should
 * use <nb/pubsub.h>.
 */

#include <nb/base.h>
#include <nb/config.h>
#include <nb/node.h>
#include <nb/proxy.h>
#include <nb/service.h>

#endif /* NB_RPC_H */
