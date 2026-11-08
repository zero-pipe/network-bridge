#ifndef NB_NB_H
#define NB_NB_H

/*
 * Canonical SDK entry point: the complete public API.
 *
 * The SDK has two spines over one model-neutral base:
 *
 *   <nb/rpc.h>     request/response calls, streams, node runtime
 *   <nb/pubsub.h>  topics, writers/readers, reader history, DDS QoS + status
 *
 * Both build on <nb/base.h>. Include the spine header that declares what you
 * use; include this umbrella header only when an application genuinely needs
 * both. New declarations must go into a spine header, never here: this file
 * only re-exports them (see docs/architecture-split.md).
 *
 * Internal facilities (include/nb/internal/*) are deliberately not reachable
 * from here and are not installed.
 *
 * Compatibility: headers stay source-compatible across releases, but struct
 * layouts are not guaranteed to be binary-compatible; link consumers against
 * the same library version and rebuild when upgrading. NB_VERSION_* in
 * types.h identifies the header API level.
 */

#include <nb/base.h>
#include <nb/rpc.h>
#include <nb/pubsub.h>

#endif /* NB_NB_H */
