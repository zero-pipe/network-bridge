#ifndef NB_NB_H
#define NB_NB_H

/*
 * Canonical SDK entry point: the complete public API.
 *
 * Applications that want the full surface can include <nb/nb.h>. Projects that
 * prefer a minimal include set include the header that declares what they use:
 * there is no role-header layer in between, so the list below is exactly the
 * public API.
 *
 * Compatibility: headers stay source-compatible across releases, but struct
 * layouts are not guaranteed to be binary-compatible; link consumers against
 * the same library version and rebuild when upgrading. NB_VERSION_* in
 * types.h identifies the header API level.
 */

#include <nb/alloc.h>
#include <nb/buf.h>
#include <nb/codec.h>
#include <nb/config.h>
#include <nb/dds.h>
#include <nb/discovery_view.h>
#include <nb/endpoint.h>
#include <nb/error.h>
#include <nb/log.h>
#include <nb/message.h>
#include <nb/message_view.h>
#include <nb/metrics.h>
#include <nb/node.h>
#include <nb/poolset.h>
#include <nb/proxy.h>
#include <nb/publish.h>
#include <nb/qos.h>
#include <nb/rtps.h>
#include <nb/sample.h>
#include <nb/service.h>
#include <nb/service_table.h>
#include <nb/shm.h>
#include <nb/slotpool.h>
#include <nb/status.h>
#include <nb/stub.h>
#include <nb/subscribe.h>
#include <nb/transport_provider.h>
#include <nb/types.h>
#include <nb/waitset.h>

#endif /* NB_NB_H */
