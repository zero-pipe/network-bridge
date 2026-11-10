#ifndef NB_BASE_H
#define NB_BASE_H

/*
 * Model-neutral base of the SDK: buffers/allocators, errors, logging, metrics,
 * opaque payloads and the API version.
 *
 * Everything here is shared by both spines:
 *
 *   <nb/rpc.h>  request/response calls (node, service, proxy, call)
 *   <nb/dds.h>  topics, writers/readers, DDS QoS and status
 *
 * Call-model and topic-model declarations deliberately do not appear here.
 */

#include <nb/base/alloc.h>
#include <nb/base/buf.h>
#include <nb/base/error.h>
#include <nb/base/log.h>
#include <nb/base/metrics.h>
#include <nb/base/sample_info.h>
#include <nb/base/types.h>

#endif /* NB_BASE_H */
