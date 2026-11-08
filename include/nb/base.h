#ifndef NB_BASE_H
#define NB_BASE_H

/*
 * Model-neutral base of the SDK.
 *
 * Everything reachable from this header is shared by both spines:
 *
 *   <nb/rpc.h>     request/response calls (node, service, proxy, call)
 *   <nb/pubsub.h>  topic publish/subscribe (participant, topic, writer, reader)
 *
 * This header deliberately contains no call-model or topic-model declaration:
 * no node, service, proxy, QoS, sample, status or discovery-view type. If a
 * type is meaningful to only one spine it belongs in that spine's header, not
 * here. Keeping the base neutral is what stops <nb/nb.h> from growing as a
 * union of two unrelated APIs; see docs/architecture-split.md.
 *
 * Contents: error codes, base types, endpoints, buffers and allocators,
 * logging, metrics, payload views, the provider-SPI message shape, and the
 * shared service/method declaration table.
 */

#include <nb/alloc.h>
#include <nb/buf.h>
#include <nb/codec.h>
#include <nb/endpoint.h>
#include <nb/error.h>
#include <nb/log.h>
#include <nb/message.h>
#include <nb/message_view.h>
#include <nb/metrics.h>
#include <nb/service_table.h>
#include <nb/types.h>

#endif /* NB_BASE_H */
