#ifndef NB_DDS_QOS_H
#define NB_DDS_QOS_H

/*
 * DDS QoS policy set.
 *
 * Policies describe topics and endpoints, not calls. `present` records which
 * fields the caller set explicitly; a clear bit means "inherit from the parent"
 * (participant default -> topic -> writer/reader). This is what lets a bool be
 * explicitly false instead of "unset".
 *
 * Both backends start from the same DDS defaults (best_effort / volatile /
 * keep_last depth 1) so RTPS and Cyclone behave identically when no policy is
 * configured.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DDS_QOS_PARTITION_MAX 128

typedef enum nb_dds_reliability {
    NB_DDS_RELIABILITY_BEST_EFFORT = 0,
    NB_DDS_RELIABILITY_RELIABLE = 1
} nb_dds_reliability_t;

typedef enum nb_dds_durability {
    NB_DDS_DURABILITY_VOLATILE = 0,
    NB_DDS_DURABILITY_TRANSIENT_LOCAL = 1
} nb_dds_durability_t;

typedef enum nb_dds_history {
    NB_DDS_HISTORY_KEEP_LAST = 0,
    NB_DDS_HISTORY_KEEP_ALL = 1
} nb_dds_history_t;

typedef enum nb_dds_liveliness {
    NB_DDS_LIVELINESS_AUTOMATIC = 0,
    NB_DDS_LIVELINESS_MANUAL_BY_PARTICIPANT = 1,
    NB_DDS_LIVELINESS_MANUAL_BY_TOPIC = 2
} nb_dds_liveliness_t;

typedef enum nb_dds_ownership {
    NB_DDS_OWNERSHIP_SHARED = 0,
    NB_DDS_OWNERSHIP_EXCLUSIVE = 1
} nb_dds_ownership_t;

typedef enum nb_dds_presentation_scope {
    NB_DDS_PRESENTATION_INSTANCE = 0,
    NB_DDS_PRESENTATION_TOPIC = 1,
    NB_DDS_PRESENTATION_GROUP = 2
} nb_dds_presentation_scope_t;

#define NB_DDS_QOS_PRESENT_RELIABILITY (1ull << 0)
#define NB_DDS_QOS_PRESENT_DURABILITY (1ull << 1)
#define NB_DDS_QOS_PRESENT_HISTORY (1ull << 2)
#define NB_DDS_QOS_PRESENT_DEPTH (1ull << 3)
#define NB_DDS_QOS_PRESENT_DEADLINE (1ull << 4)
#define NB_DDS_QOS_PRESENT_LIFESPAN (1ull << 5)
#define NB_DDS_QOS_PRESENT_LATENCY_BUDGET (1ull << 6)
#define NB_DDS_QOS_PRESENT_MIN_SEPARATION (1ull << 7)
#define NB_DDS_QOS_PRESENT_LIVELINESS (1ull << 8)
#define NB_DDS_QOS_PRESENT_LEASE_DURATION (1ull << 9)
#define NB_DDS_QOS_PRESENT_OWNERSHIP (1ull << 10)
#define NB_DDS_QOS_PRESENT_PARTITION (1ull << 11)
#define NB_DDS_QOS_PRESENT_MAX_SAMPLES (1ull << 12)
#define NB_DDS_QOS_PRESENT_MAX_INSTANCES (1ull << 13)
#define NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE (1ull << 14)
#define NB_DDS_QOS_PRESENT_AUTODISPOSE (1ull << 15)
#define NB_DDS_QOS_PRESENT_PRESENTATION (1ull << 16)
#define NB_DDS_QOS_PRESENT_COHERENT_ACCESS (1ull << 17)
#define NB_DDS_QOS_PRESENT_ORDERED_ACCESS (1ull << 18)

typedef struct nb_dds_qos {
    uint64_t present;
    nb_dds_reliability_t reliability;
    nb_dds_durability_t durability;
    nb_dds_history_t history;
    nb_dds_liveliness_t liveliness;
    nb_dds_ownership_t ownership;
    uint32_t depth; /* keep_last depth */
    uint32_t deadline_ms;
    uint32_t lifespan_ms;
    uint32_t lease_duration_ms; /* liveliness lease */
    uint32_t latency_budget_ms;
    /* TimeBasedFilter minimum separation (reader side); 0 means no filter. */
    uint32_t min_separation_ms;
    char partition[NB_DDS_QOS_PARTITION_MAX];
    /* Resource limits; 0 means unlimited / backend default. */
    uint32_t max_samples;
    uint32_t max_instances;
    uint32_t max_samples_per_instance;
    bool autodispose_unregistered_instances;
    nb_dds_presentation_scope_t presentation_scope;
    bool coherent_access;
    bool ordered_access;
} nb_dds_qos_t;

void nb_dds_qos_init(nb_dds_qos_t *qos);
/* Copy every field qos does not set explicitly from parent, including bools. */
void nb_dds_qos_inherit(nb_dds_qos_t *qos, const nb_dds_qos_t *parent);
bool nb_dds_qos_is_default(const nb_dds_qos_t *qos);
int nb_dds_qos_validate(const nb_dds_qos_t *qos);
const char *nb_dds_qos_validate_reason(const nb_dds_qos_t *qos);

void nb_dds_qos_set_reliability(nb_dds_qos_t *qos, nb_dds_reliability_t value);
void nb_dds_qos_set_durability(nb_dds_qos_t *qos, nb_dds_durability_t value);
void nb_dds_qos_set_history(nb_dds_qos_t *qos, nb_dds_history_t kind, uint32_t depth);
void nb_dds_qos_set_deadline_ms(nb_dds_qos_t *qos, uint32_t ms);
void nb_dds_qos_set_lifespan_ms(nb_dds_qos_t *qos, uint32_t ms);
void nb_dds_qos_set_latency_budget_ms(nb_dds_qos_t *qos, uint32_t ms);
void nb_dds_qos_set_min_separation_ms(nb_dds_qos_t *qos, uint32_t ms);
void nb_dds_qos_set_liveliness(nb_dds_qos_t *qos, nb_dds_liveliness_t value, uint32_t lease_ms);
void nb_dds_qos_set_ownership(nb_dds_qos_t *qos, nb_dds_ownership_t value);
void nb_dds_qos_set_partition(nb_dds_qos_t *qos, const char *partition);
void nb_dds_qos_set_resource_limits(nb_dds_qos_t *qos,
                                    uint32_t max_samples,
                                    uint32_t max_instances,
                                    uint32_t max_samples_per_instance);
void nb_dds_qos_set_autodispose(nb_dds_qos_t *qos, bool enabled);
void nb_dds_qos_set_presentation(nb_dds_qos_t *qos,
                                 nb_dds_presentation_scope_t scope,
                                 bool coherent_access,
                                 bool ordered_access);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_QOS_H */
