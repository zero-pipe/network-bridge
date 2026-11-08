#ifndef NB_QOS_H
#define NB_QOS_H

/*
 * Pub/sub QoS: the DDS policy set, attached to the pub/sub spine.
 *
 * These policies describe topics, not calls: reliability, durability,
 * history, liveliness, ownership, partition, deadline, lifespan, resource
 * limits and presentation. They are honoured by topic-based backends (`dds`
 * and `rtps`); point-to-point RPC links (udp/tcp/shm) never read them.
 *
 * The type is named nb_ps_qos_t to make that ownership explicit: it is the
 * pub/sub spine's QoS, not a transport-neutral policy set. Do not add RPC-only
 * settings here; they belong to the RPC spine.
 *
 * The `dds` and `rtps` providers map these fields onto their native
 * equivalents: the `rtps` provider announces every field here and applies the
 * requested/offered matching rules. Lifespan is enforced: a retained sample
 * past its lifespan is announced with GAP instead of being retransmitted.
 * Deadline is monitored: a publication that does not write, and a subscription
 * that receives nothing from a matched writer, report a miss through the
 * participant status callback. Automatic liveliness is enforced: a publication
 * with a finite lease is asserted with ParticipantMessageData, and a
 * subscription reports `LIVELINESS_CHANGED` when a matched writer stops
 * asserting. Manual liveliness still needs an application-level "assert"
 * operation, so those endpoints are announced as automatic. Providers without a
 * native equivalent (udp/tcp/shm) treat these fields as advisory.
 *
 * TimeBasedFilter is honoured the way Cyclone DDS honours it: a subscription
 * with a non-zero minimum separation drops samples that arrive less than that
 * apart, measured on the writer's source timestamps per instance, while still
 * acknowledging them on the reliable channel. Samples without a source
 * timestamp are never filtered. Publications announce the value but do not
 * filter on it, so the two implementations agree on what an application sees.
 *
 * NB_QOS_*_INHERIT (0) means "do not override the provider default". This
 * keeps existing behaviour when no QoS is configured and lets a binding
 * override only the policies it cares about.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_QOS_PARTITION_MAX 128

typedef enum nb_qos_reliability {
    NB_QOS_RELIABILITY_INHERIT = 0,
    NB_QOS_RELIABILITY_BEST_EFFORT = 1,
    NB_QOS_RELIABILITY_RELIABLE = 2
} nb_qos_reliability_t;

typedef enum nb_qos_durability {
    NB_QOS_DURABILITY_INHERIT = 0,
    NB_QOS_DURABILITY_VOLATILE = 1,
    NB_QOS_DURABILITY_TRANSIENT_LOCAL = 2
} nb_qos_durability_t;

typedef enum nb_qos_history {
    NB_QOS_HISTORY_INHERIT = 0,
    NB_QOS_HISTORY_KEEP_LAST = 1,
    NB_QOS_HISTORY_KEEP_ALL = 2
} nb_qos_history_t;

typedef enum nb_qos_liveliness {
    NB_QOS_LIVELINESS_INHERIT = 0,
    NB_QOS_LIVELINESS_AUTOMATIC = 1,
    NB_QOS_LIVELINESS_MANUAL_BY_PARTICIPANT = 2,
    NB_QOS_LIVELINESS_MANUAL_BY_TOPIC = 3
} nb_qos_liveliness_t;

typedef enum nb_qos_ownership {
    NB_QOS_OWNERSHIP_INHERIT = 0,
    NB_QOS_OWNERSHIP_SHARED = 1,
    NB_QOS_OWNERSHIP_EXCLUSIVE = 2
} nb_qos_ownership_t;

/* DDS PresentationQosPolicy::access_scope. INSTANCE < TOPIC < GROUP: a reader
 * that asks for a wider scope than the writer offers does not match. */
typedef enum nb_qos_presentation_scope {
    NB_QOS_PRESENTATION_INSTANCE = 0,
    NB_QOS_PRESENTATION_TOPIC = 1,
    NB_QOS_PRESENTATION_GROUP = 2
} nb_qos_presentation_scope_t;

typedef struct nb_ps_qos {
    nb_qos_reliability_t reliability;
    nb_qos_durability_t durability;
    nb_qos_history_t history;
    nb_qos_liveliness_t liveliness;
    nb_qos_ownership_t ownership;
    uint32_t depth;      /* keep_last depth; 0 = provider default */
    uint32_t deadline_ms;
    uint32_t lifespan_ms;
    uint32_t lease_duration_ms; /* liveliness lease */
    uint32_t latency_budget_ms;
    /* TimeBasedFilter minimum separation. A subscription drops samples that
     * arrive closer together than this, measured on source timestamps; 0 means
     * no filter. A publication only announces it. */
    uint32_t min_separation_ms;
    char partition[NB_QOS_PARTITION_MAX];
    /* ResourceLimits. 0 means unlimited; a writer applies max_samples and
     * max_samples_per_instance to its history, a reader applies max_instances
     * and max_samples_per_instance to what it accepts. */
    uint32_t max_samples;
    uint32_t max_instances;
    uint32_t max_samples_per_instance;
    /* WriterDataLifecycle: dispose the instances of a writer that unregisters
     * instead of leaving them alive for the reader. */
    bool autodispose_unregistered_instances;
    /* Presentation. The scope is checked for RxO compatibility and announced on
     * the wire; grouping samples into coherent/ordered sets is a read/take
     * feature (P2.4), so a provider that delivers samples one at a time only
     * honours the compatibility rule. */
    nb_qos_presentation_scope_t presentation_scope;
    bool coherent_access;
    bool ordered_access;
} nb_ps_qos_t;

void nb_ps_qos_init(nb_ps_qos_t *qos);
/* Fill fields that are INHERIT/empty in qos from parent (field-wise). */
void nb_ps_qos_inherit(nb_ps_qos_t *qos, const nb_ps_qos_t *parent);
/* True when no policy is set (all fields INHERIT/zero/empty). */
bool nb_ps_qos_is_default(const nb_ps_qos_t *qos);
/* Check enum ranges and the partition NUL terminator. */
int nb_ps_qos_validate(const nb_ps_qos_t *qos);
const char *nb_ps_qos_validate_reason(const nb_ps_qos_t *qos);

#ifdef __cplusplus
}
#endif

#endif /* NB_QOS_H */
