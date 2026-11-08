#include <nb/qos.h>
#include <nb/error.h>

#include <string.h>

void nb_ps_qos_init(nb_ps_qos_t *qos)
{
    if (!qos) {
        return;
    }
    memset(qos, 0, sizeof(*qos));
}

void nb_ps_qos_inherit(nb_ps_qos_t *qos, const nb_ps_qos_t *parent)
{
    if (!qos || !parent) {
        return;
    }
    if (qos->reliability == 0) {
        qos->reliability = parent->reliability;
    }
    if (qos->durability == 0) {
        qos->durability = parent->durability;
    }
    if (qos->history == 0) {
        qos->history = parent->history;
    }
    if (qos->liveliness == 0) {
        qos->liveliness = parent->liveliness;
    }
    if (qos->ownership == 0) {
        qos->ownership = parent->ownership;
    }
    if (qos->depth == 0) {
        qos->depth = parent->depth;
    }
    if (qos->deadline_ms == 0) {
        qos->deadline_ms = parent->deadline_ms;
    }
    if (qos->lifespan_ms == 0) {
        qos->lifespan_ms = parent->lifespan_ms;
    }
    if (qos->lease_duration_ms == 0) {
        qos->lease_duration_ms = parent->lease_duration_ms;
    }
    if (qos->latency_budget_ms == 0) {
        qos->latency_budget_ms = parent->latency_budget_ms;
    }
    if (qos->min_separation_ms == 0) {
        qos->min_separation_ms = parent->min_separation_ms;
    }
    if (!qos->partition[0]) {
        memcpy(qos->partition, parent->partition, sizeof(qos->partition));
    }
}

bool nb_ps_qos_is_default(const nb_ps_qos_t *qos)
{
    static const nb_ps_qos_t zero;

    if (!qos) {
        return true;
    }
    return memcmp(qos, &zero, sizeof(zero)) == 0;
}

int nb_ps_qos_validate(const nb_ps_qos_t *qos)
{
    if (!qos) {
        return NB_ERR_INVALID;
    }
    if (qos->reliability > NB_QOS_RELIABLE || qos->durability > NB_QOS_TRANSIENT_LOCAL ||
        qos->history > NB_QOS_KEEP_ALL ||
        qos->liveliness > NB_QOS_LIVELINESS_MANUAL_BY_TOPIC ||
        qos->ownership > NB_QOS_OWNERSHIP_EXCLUSIVE ||
        qos->presentation_scope > NB_QOS_PRESENTATION_GROUP) {
        return NB_ERR_INVALID;
    }
    if (qos->partition[NB_QOS_PARTITION_MAX - 1u] != '\0') {
        return NB_ERR_INVALID;
    }
    return NB_OK;
}

const char *nb_ps_qos_validate_reason(const nb_ps_qos_t *qos)
{
    if (!qos) {
        return "qos is null";
    }
    if (qos->reliability > NB_QOS_RELIABLE || qos->durability > NB_QOS_TRANSIENT_LOCAL ||
        qos->history > NB_QOS_KEEP_ALL ||
        qos->liveliness > NB_QOS_LIVELINESS_MANUAL_BY_TOPIC ||
        qos->ownership > NB_QOS_OWNERSHIP_EXCLUSIVE) {
        return "qos policy value out of range";
    }
    if (qos->partition[NB_QOS_PARTITION_MAX - 1u] != '\0') {
        return "partition is too long";
    }
    return "invalid qos";
}
