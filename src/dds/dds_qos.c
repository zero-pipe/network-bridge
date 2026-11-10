/* DDS spine QoS helpers: presence-aware inheritance on top of nb_ps_qos_t. */
#include <nb/dds/qos.h>

#include <nb/base/error.h>

#include <string.h>
#include <stdio.h>

void nb_dds_qos_init(nb_dds_qos_t *qos)
{
    if (!qos) {
        return;
    }
    memset(qos, 0, sizeof(*qos));
}

/* Old-style callers fill fields without the present mask. Treat a non-zero
 * field as explicitly set so inheritance keeps their behaviour. */
static int qos_parent_has(const nb_dds_qos_t *parent, uint64_t bit, int non_zero)
{
    if (parent->present & bit) {
        return 1;
    }
    return non_zero;
}

void nb_dds_qos_inherit(nb_dds_qos_t *qos, const nb_dds_qos_t *parent)
{
    if (!qos || !parent) {
        return;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_RELIABILITY) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_RELIABILITY, parent->reliability != 0)) {
        qos->reliability = parent->reliability;
        qos->present |= NB_DDS_QOS_PRESENT_RELIABILITY;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_DURABILITY) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_DURABILITY, parent->durability != 0)) {
        qos->durability = parent->durability;
        qos->present |= NB_DDS_QOS_PRESENT_DURABILITY;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_HISTORY) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_HISTORY, parent->history != 0)) {
        qos->history = parent->history;
        qos->present |= NB_DDS_QOS_PRESENT_HISTORY;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_DEPTH) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_DEPTH, parent->depth != 0)) {
        qos->depth = parent->depth;
        qos->present |= NB_DDS_QOS_PRESENT_DEPTH;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_DEADLINE) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_DEADLINE, parent->deadline_ms != 0)) {
        qos->deadline_ms = parent->deadline_ms;
        qos->present |= NB_DDS_QOS_PRESENT_DEADLINE;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_LIFESPAN) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_LIFESPAN, parent->lifespan_ms != 0)) {
        qos->lifespan_ms = parent->lifespan_ms;
        qos->present |= NB_DDS_QOS_PRESENT_LIFESPAN;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_LATENCY_BUDGET) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_LATENCY_BUDGET, parent->latency_budget_ms != 0)) {
        qos->latency_budget_ms = parent->latency_budget_ms;
        qos->present |= NB_DDS_QOS_PRESENT_LATENCY_BUDGET;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_MIN_SEPARATION) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_MIN_SEPARATION, parent->min_separation_ms != 0)) {
        qos->min_separation_ms = parent->min_separation_ms;
        qos->present |= NB_DDS_QOS_PRESENT_MIN_SEPARATION;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_LIVELINESS) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_LIVELINESS, parent->liveliness != 0)) {
        qos->liveliness = parent->liveliness;
        qos->present |= NB_DDS_QOS_PRESENT_LIVELINESS;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_LEASE_DURATION) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_LEASE_DURATION, parent->lease_duration_ms != 0)) {
        qos->lease_duration_ms = parent->lease_duration_ms;
        qos->present |= NB_DDS_QOS_PRESENT_LEASE_DURATION;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_OWNERSHIP) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_OWNERSHIP, parent->ownership != 0)) {
        qos->ownership = parent->ownership;
        qos->present |= NB_DDS_QOS_PRESENT_OWNERSHIP;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_PARTITION) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_PARTITION, parent->partition[0] != '\0')) {
        memcpy(qos->partition, parent->partition, sizeof(qos->partition));
        qos->present |= NB_DDS_QOS_PRESENT_PARTITION;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_MAX_SAMPLES) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_MAX_SAMPLES, parent->max_samples != 0)) {
        qos->max_samples = parent->max_samples;
        qos->present |= NB_DDS_QOS_PRESENT_MAX_SAMPLES;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_MAX_INSTANCES) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_MAX_INSTANCES, parent->max_instances != 0)) {
        qos->max_instances = parent->max_instances;
        qos->present |= NB_DDS_QOS_PRESENT_MAX_INSTANCES;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE) &&
        qos_parent_has(parent,
                       NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE,
                       parent->max_samples_per_instance != 0)) {
        qos->max_samples_per_instance = parent->max_samples_per_instance;
        qos->present |= NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_AUTODISPOSE) &&
        qos_parent_has(parent,
                       NB_DDS_QOS_PRESENT_AUTODISPOSE,
                       parent->autodispose_unregistered_instances)) {
        qos->autodispose_unregistered_instances = parent->autodispose_unregistered_instances;
        qos->present |= NB_DDS_QOS_PRESENT_AUTODISPOSE;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_PRESENTATION) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_PRESENTATION, parent->presentation_scope != 0)) {
        qos->presentation_scope = parent->presentation_scope;
        qos->present |= NB_DDS_QOS_PRESENT_PRESENTATION;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_COHERENT_ACCESS) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_COHERENT_ACCESS, parent->coherent_access)) {
        qos->coherent_access = parent->coherent_access;
        qos->present |= NB_DDS_QOS_PRESENT_COHERENT_ACCESS;
    }
    if (!(qos->present & NB_DDS_QOS_PRESENT_ORDERED_ACCESS) &&
        qos_parent_has(parent, NB_DDS_QOS_PRESENT_ORDERED_ACCESS, parent->ordered_access)) {
        qos->ordered_access = parent->ordered_access;
        qos->present |= NB_DDS_QOS_PRESENT_ORDERED_ACCESS;
    }
}

bool nb_dds_qos_is_default(const nb_dds_qos_t *qos)
{
    if (!qos) {
        return true;
    }
    return qos->present == 0 && qos->partition[0] == '\0' && qos->depth == 0 &&
           qos->deadline_ms == 0 && qos->lifespan_ms == 0 && qos->lease_duration_ms == 0 &&
           qos->latency_budget_ms == 0 && qos->min_separation_ms == 0 &&
           qos->max_samples == 0 && qos->max_instances == 0 &&
           qos->max_samples_per_instance == 0;
}

int nb_dds_qos_validate(const nb_dds_qos_t *qos)
{
    if (!qos) {
        return NB_ERR_INVALID;
    }
    if ((unsigned)qos->reliability > NB_DDS_RELIABILITY_RELIABLE ||
        (unsigned)qos->durability > NB_DDS_DURABILITY_TRANSIENT_LOCAL ||
        (unsigned)qos->history > NB_DDS_HISTORY_KEEP_ALL ||
        (unsigned)qos->liveliness > NB_DDS_LIVELINESS_MANUAL_BY_TOPIC ||
        (unsigned)qos->ownership > NB_DDS_OWNERSHIP_EXCLUSIVE ||
        (unsigned)qos->presentation_scope > NB_DDS_PRESENTATION_GROUP) {
        return NB_ERR_INVALID;
    }
    return NB_OK;
}

const char *nb_dds_qos_validate_reason(const nb_dds_qos_t *qos)
{
    if (!qos) {
        return "null qos";
    }
    if ((unsigned)qos->reliability > NB_DDS_RELIABILITY_RELIABLE) {
        return "invalid reliability";
    }
    if ((unsigned)qos->durability > NB_DDS_DURABILITY_TRANSIENT_LOCAL) {
        return "invalid durability";
    }
    if ((unsigned)qos->history > NB_DDS_HISTORY_KEEP_ALL) {
        return "invalid history";
    }
    if ((unsigned)qos->liveliness > NB_DDS_LIVELINESS_MANUAL_BY_TOPIC) {
        return "invalid liveliness";
    }
    if ((unsigned)qos->ownership > NB_DDS_OWNERSHIP_EXCLUSIVE) {
        return "invalid ownership";
    }
    if ((unsigned)qos->presentation_scope > NB_DDS_PRESENTATION_GROUP) {
        return "invalid presentation scope";
    }
    return "ok";
}

void nb_dds_qos_set_reliability(nb_dds_qos_t *qos, nb_dds_reliability_t value)
{
    if (qos) {
        qos->reliability = value;
        qos->present |= NB_DDS_QOS_PRESENT_RELIABILITY;
    }
}

void nb_dds_qos_set_durability(nb_dds_qos_t *qos, nb_dds_durability_t value)
{
    if (qos) {
        qos->durability = value;
        qos->present |= NB_DDS_QOS_PRESENT_DURABILITY;
    }
}

void nb_dds_qos_set_history(nb_dds_qos_t *qos, nb_dds_history_t kind, uint32_t depth)
{
    if (qos) {
        qos->history = kind;
        qos->depth = depth;
        qos->present |= NB_DDS_QOS_PRESENT_HISTORY | NB_DDS_QOS_PRESENT_DEPTH;
    }
}

void nb_dds_qos_set_deadline_ms(nb_dds_qos_t *qos, uint32_t ms)
{
    if (qos) {
        qos->deadline_ms = ms;
        qos->present |= NB_DDS_QOS_PRESENT_DEADLINE;
    }
}

void nb_dds_qos_set_lifespan_ms(nb_dds_qos_t *qos, uint32_t ms)
{
    if (qos) {
        qos->lifespan_ms = ms;
        qos->present |= NB_DDS_QOS_PRESENT_LIFESPAN;
    }
}

void nb_dds_qos_set_latency_budget_ms(nb_dds_qos_t *qos, uint32_t ms)
{
    if (qos) {
        qos->latency_budget_ms = ms;
        qos->present |= NB_DDS_QOS_PRESENT_LATENCY_BUDGET;
    }
}

void nb_dds_qos_set_min_separation_ms(nb_dds_qos_t *qos, uint32_t ms)
{
    if (qos) {
        qos->min_separation_ms = ms;
        qos->present |= NB_DDS_QOS_PRESENT_MIN_SEPARATION;
    }
}

void nb_dds_qos_set_liveliness(nb_dds_qos_t *qos, nb_dds_liveliness_t value, uint32_t lease_ms)
{
    if (qos) {
        qos->liveliness = value;
        qos->lease_duration_ms = lease_ms;
        qos->present |= NB_DDS_QOS_PRESENT_LIVELINESS | NB_DDS_QOS_PRESENT_LEASE_DURATION;
    }
}

void nb_dds_qos_set_ownership(nb_dds_qos_t *qos, nb_dds_ownership_t value)
{
    if (qos) {
        qos->ownership = value;
        qos->present |= NB_DDS_QOS_PRESENT_OWNERSHIP;
    }
}

void nb_dds_qos_set_partition(nb_dds_qos_t *qos, const char *partition)
{
    if (!qos) {
        return;
    }
    if (partition && partition[0]) {
        snprintf(qos->partition, sizeof(qos->partition), "%s", partition);
        qos->present |= NB_DDS_QOS_PRESENT_PARTITION;
    } else {
        qos->partition[0] = '\0';
        qos->present &= ~NB_DDS_QOS_PRESENT_PARTITION;
    }
}

void nb_dds_qos_set_resource_limits(nb_dds_qos_t *qos,
                                    uint32_t max_samples,
                                    uint32_t max_instances,
                                    uint32_t max_samples_per_instance)
{
    if (qos) {
        qos->max_samples = max_samples;
        qos->max_instances = max_instances;
        qos->max_samples_per_instance = max_samples_per_instance;
        qos->present |= NB_DDS_QOS_PRESENT_MAX_SAMPLES | NB_DDS_QOS_PRESENT_MAX_INSTANCES |
                        NB_DDS_QOS_PRESENT_MAX_SAMPLES_PER_INSTANCE;
    }
}

void nb_dds_qos_set_autodispose(nb_dds_qos_t *qos, bool enabled)
{
    if (qos) {
        qos->autodispose_unregistered_instances = enabled;
        qos->present |= NB_DDS_QOS_PRESENT_AUTODISPOSE;
    }
}

void nb_dds_qos_set_presentation(nb_dds_qos_t *qos,
                                 nb_dds_presentation_scope_t scope,
                                 bool coherent_access,
                                 bool ordered_access)
{
    if (qos) {
        qos->presentation_scope = scope;
        qos->coherent_access = coherent_access;
        qos->ordered_access = ordered_access;
        qos->present |= NB_DDS_QOS_PRESENT_PRESENTATION | NB_DDS_QOS_PRESENT_COHERENT_ACCESS |
                        NB_DDS_QOS_PRESENT_ORDERED_ACCESS;
    }
}
