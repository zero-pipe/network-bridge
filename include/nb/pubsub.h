#ifndef NB_PUBSUB_H
#define NB_PUBSUB_H

/*
 * Publish/subscribe spine: typed topics, writers, readers, history, waitsets
 * and status events.
 *
 * Topic-based backends: dds, rtps, shm. The policies in <nb/qos.h> are DDS
 * policies and are honoured by topic-based backends; point-to-point RPC links
 * ignore them. Reader-side history (read/take, waitsets, sample filters) and
 * the DDS status events also belong to this spine, not to the RPC one.
 *
 * The publisher/subscriber aliases in publish.h and subscribe.h are the
 * legacy spelling, where a topic was expressed as a service method. They are
 * kept for source compatibility during 0.x; new code should use the topic
 * objects once they land (see docs/architecture-split.md, milestone M3).
 */

#include <nb/base.h>
#include <nb/qos.h>
#include <nb/sample.h>
#include <nb/status.h>
#include <nb/waitset.h>
#include <nb/discovery_view.h>
#include <nb/publish.h>
#include <nb/subscribe.h>

/* Topic-based backends; including these also makes their registration calls
 * visible. They are opt-in providers, so including them here is what a
 * pub/sub application needs in practice. */
#include <nb/dds.h>
#include <nb/rtps.h>
#include <nb/shm.h>

#endif /* NB_PUBSUB_H */
