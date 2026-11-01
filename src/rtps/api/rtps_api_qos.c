#include "rtps_api_internal.h"

#include <string.h>

/* ---- public QoS helpers ------------------------------------------------- */

void rtps_qos_init(rtps_qos_t *qos)
{
    if (!qos) {
        return;
    }
    memset(qos, 0, sizeof(*qos));
}

int rtps_qos_is_default(const rtps_qos_t *qos)
{
    return qos && qos->present == 0;
}

void rtps_qos_set_reliability(rtps_qos_t *qos, rtps_reliability_t value)
{
    if (!qos) {
        return;
    }
    qos->reliability = (uint32_t)value;
    qos->present |= RTPS_QOS_PRESENT_RELIABILITY;
}

void rtps_qos_set_durability(rtps_qos_t *qos, rtps_durability_t value)
{
    if (!qos) {
        return;
    }
    qos->durability = (uint32_t)value;
    qos->present |= RTPS_QOS_PRESENT_DURABILITY;
}

void rtps_qos_set_history(rtps_qos_t *qos, rtps_history_t kind, uint32_t depth)
{
    if (!qos) {
        return;
    }
    qos->history = (uint32_t)kind;
    qos->depth = depth;
    qos->present |= RTPS_QOS_PRESENT_HISTORY;
}

void rtps_qos_set_deadline_ms(rtps_qos_t *qos, uint32_t ms)
{
    if (!qos) {
        return;
    }
    qos->deadline_ms = ms;
    qos->present |= RTPS_QOS_PRESENT_DEADLINE;
}

void rtps_qos_set_lifespan_ms(rtps_qos_t *qos, uint32_t ms)
{
    if (!qos) {
        return;
    }
    qos->lifespan_ms = ms;
    qos->present |= RTPS_QOS_PRESENT_LIFESPAN;
}

void rtps_qos_set_liveliness(rtps_qos_t *qos,
                             rtps_liveliness_t kind,
                             uint32_t lease_ms)
{
    if (!qos) {
        return;
    }
    qos->liveliness_kind = (uint32_t)kind;
    qos->liveliness_lease_ms = lease_ms;
    qos->present |= RTPS_QOS_PRESENT_LIVELINESS;
}

void rtps_qos_set_ownership(rtps_qos_t *qos,
                            rtps_ownership_t kind,
                            int32_t strength)
{
    if (!qos) {
        return;
    }
    qos->ownership = (uint32_t)kind;
    qos->ownership_strength = strength;
    qos->present |= RTPS_QOS_PRESENT_OWNERSHIP;
}

void rtps_qos_set_destination_order(rtps_qos_t *qos,
                                    rtps_destination_order_t value)
{
    if (!qos) {
        return;
    }
    qos->destination_order = (uint32_t)value;
    qos->present |= RTPS_QOS_PRESENT_DESTINATION_ORDER;
}

void rtps_qos_set_presentation(rtps_qos_t *qos,
                               rtps_presentation_scope_t scope,
                               int coherent,
                               int ordered)
{
    if (!qos) {
        return;
    }
    qos->presentation_scope = (uint32_t)scope;
    qos->coherent_access = coherent != 0;
    qos->ordered_access = ordered != 0;
    qos->present |= RTPS_QOS_PRESENT_PRESENTATION;
}

void rtps_qos_set_partition(rtps_qos_t *qos, const char *names)
{
    if (!qos) {
        return;
    }
    rtps_api_copy(qos->partition, sizeof(qos->partition), names);
    qos->present |= RTPS_QOS_PRESENT_PARTITION;
}

void rtps_qos_set_latency_budget_ms(rtps_qos_t *qos, uint32_t ms)
{
    if (!qos) {
        return;
    }
    qos->latency_budget_ms = ms;
    qos->present |= RTPS_QOS_PRESENT_LATENCY_BUDGET;
}

void rtps_qos_set_time_based_filter_ms(rtps_qos_t *qos, uint32_t ms)
{
    if (!qos) {
        return;
    }
    qos->time_based_filter_ms = ms;
    qos->present |= RTPS_QOS_PRESENT_TIME_BASED_FILTER;
}

void rtps_qos_set_resource_limits(rtps_qos_t *qos,
                                  uint32_t max_samples,
                                  uint32_t max_instances,
                                  uint32_t max_samples_per_instance)
{
    if (!qos) {
        return;
    }
    qos->max_samples = max_samples;
    qos->max_instances = max_instances;
    qos->max_samples_per_instance = max_samples_per_instance;
    qos->present |= RTPS_QOS_PRESENT_RESOURCE_LIMITS;
}

void rtps_qos_set_autodispose(rtps_qos_t *qos, int enabled)
{
    if (!qos) {
        return;
    }
    qos->autodispose_unregistered_instances = enabled != 0;
    qos->present |= RTPS_QOS_PRESENT_AUTODISPOSE;
}

/* ---- internal helpers --------------------------------------------------- */

void rtps_api_qos_merge(const rtps_qos_t *base,
                        const rtps_qos_t *override,
                        rtps_qos_t *out)
{
    if (!out) {
        return;
    }
    if (base) {
        *out = *base;
    } else {
        memset(out, 0, sizeof(*out));
    }
    if (!override) {
        return;
    }
    if (override->present & RTPS_QOS_PRESENT_RELIABILITY) {
        out->reliability = override->reliability;
        out->present |= RTPS_QOS_PRESENT_RELIABILITY;
    }
    if (override->present & RTPS_QOS_PRESENT_DURABILITY) {
        out->durability = override->durability;
        out->present |= RTPS_QOS_PRESENT_DURABILITY;
    }
    if (override->present & RTPS_QOS_PRESENT_HISTORY) {
        out->history = override->history;
        out->depth = override->depth;
        out->present |= RTPS_QOS_PRESENT_HISTORY;
    }
    if (override->present & RTPS_QOS_PRESENT_DEADLINE) {
        out->deadline_ms = override->deadline_ms;
        out->present |= RTPS_QOS_PRESENT_DEADLINE;
    }
    if (override->present & RTPS_QOS_PRESENT_LIFESPAN) {
        out->lifespan_ms = override->lifespan_ms;
        out->present |= RTPS_QOS_PRESENT_LIFESPAN;
    }
    if (override->present & RTPS_QOS_PRESENT_LIVELINESS) {
        out->liveliness_kind = override->liveliness_kind;
        out->liveliness_lease_ms = override->liveliness_lease_ms;
        out->present |= RTPS_QOS_PRESENT_LIVELINESS;
    }
    if (override->present & RTPS_QOS_PRESENT_OWNERSHIP) {
        out->ownership = override->ownership;
        out->ownership_strength = override->ownership_strength;
        out->present |= RTPS_QOS_PRESENT_OWNERSHIP;
    }
    if (override->present & RTPS_QOS_PRESENT_DESTINATION_ORDER) {
        out->destination_order = override->destination_order;
        out->present |= RTPS_QOS_PRESENT_DESTINATION_ORDER;
    }
    if (override->present & RTPS_QOS_PRESENT_PRESENTATION) {
        out->presentation_scope = override->presentation_scope;
        out->coherent_access = override->coherent_access;
        out->ordered_access = override->ordered_access;
        out->present |= RTPS_QOS_PRESENT_PRESENTATION;
    }
    if (override->present & RTPS_QOS_PRESENT_PARTITION) {
        memcpy(out->partition, override->partition, sizeof(out->partition));
        out->present |= RTPS_QOS_PRESENT_PARTITION;
    }
    if (override->present & RTPS_QOS_PRESENT_LATENCY_BUDGET) {
        out->latency_budget_ms = override->latency_budget_ms;
        out->present |= RTPS_QOS_PRESENT_LATENCY_BUDGET;
    }
    if (override->present & RTPS_QOS_PRESENT_TIME_BASED_FILTER) {
        out->time_based_filter_ms = override->time_based_filter_ms;
        out->present |= RTPS_QOS_PRESENT_TIME_BASED_FILTER;
    }
    if (override->present & RTPS_QOS_PRESENT_RESOURCE_LIMITS) {
        out->max_samples = override->max_samples;
        out->max_instances = override->max_instances;
        out->max_samples_per_instance = override->max_samples_per_instance;
        out->present |= RTPS_QOS_PRESENT_RESOURCE_LIMITS;
    }
    if (override->present & RTPS_QOS_PRESENT_AUTODISPOSE) {
        out->autodispose_unregistered_instances =
            override->autodispose_unregistered_instances;
        out->present |= RTPS_QOS_PRESENT_AUTODISPOSE;
    }
}

void rtps_api_fill_endpoint(const struct rtps_topic *topic,
                            const rtps_qos_t *qos,
                            nb_rtps_endpoint_info_t *info)
{
    if (!topic || !qos || !info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    rtps_api_copy(info->topic_name, sizeof(info->topic_name), topic->name);
    rtps_api_copy(info->type_name, sizeof(info->type_name), topic->type_name);
    if (qos->present & RTPS_QOS_PRESENT_RELIABILITY) {
        info->reliability = qos->reliability;
    }
    if (qos->present & RTPS_QOS_PRESENT_DURABILITY) {
        info->durability = qos->durability;
    }
    if (qos->present & RTPS_QOS_PRESENT_HISTORY) {
        info->history_kind = qos->history;
        info->history_depth = qos->depth;
    }
    if (qos->present & RTPS_QOS_PRESENT_DEADLINE) {
        info->deadline_ms = qos->deadline_ms;
    }
    if (qos->present & RTPS_QOS_PRESENT_LIFESPAN) {
        info->lifespan_ms = qos->lifespan_ms;
    }
    if (qos->present & RTPS_QOS_PRESENT_LIVELINESS) {
        info->liveliness_kind = qos->liveliness_kind;
        info->liveliness_lease_ms = qos->liveliness_lease_ms;
    }
    if (qos->present & RTPS_QOS_PRESENT_OWNERSHIP) {
        info->ownership = qos->ownership;
        info->ownership_strength = qos->ownership_strength;
    }
    if (qos->present & RTPS_QOS_PRESENT_DESTINATION_ORDER) {
        info->destination_order = qos->destination_order;
    }
    if (qos->present & RTPS_QOS_PRESENT_PRESENTATION) {
        info->presentation_scope = qos->presentation_scope;
        info->coherent_access = qos->coherent_access;
        info->ordered_access = qos->ordered_access;
    }
    if (qos->present & RTPS_QOS_PRESENT_PARTITION) {
        rtps_api_copy(info->partition, sizeof(info->partition), qos->partition);
    }
    if (qos->present & RTPS_QOS_PRESENT_LATENCY_BUDGET) {
        info->latency_budget_ms = qos->latency_budget_ms;
    }
    if (qos->present & RTPS_QOS_PRESENT_TIME_BASED_FILTER) {
        info->min_separation_ms = qos->time_based_filter_ms;
    }
    if (qos->present & RTPS_QOS_PRESENT_RESOURCE_LIMITS) {
        info->max_samples = qos->max_samples;
        info->max_instances = qos->max_instances;
        info->max_samples_per_instance = qos->max_samples_per_instance;
    }
    if (qos->present & RTPS_QOS_PRESENT_AUTODISPOSE) {
        info->autodispose = qos->autodispose_unregistered_instances;
    }
}

void rtps_api_fill_sample(const nb_rtps_dds_message_t *message,
                          rtps_sample_t *out)
{
    if (!message || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->data = message->payload;
    out->size = message->payload_len;
    out->info.valid_data = nb_sample_info_valid_data(&message->sample) ? 1 : 0;
    out->info.instance_state = (rtps_instance_state_t)message->sample.instance_state;
    out->info.view_state = message->sample.view_state == NB_VIEW_STATE_NEW
                               ? RTPS_VIEW_STATE_NEW
                               : RTPS_VIEW_STATE_NOT_NEW;
    out->info.sample_state = RTPS_SAMPLE_STATE_NOT_READ;
    out->info.has_instance_handle = message->sample.has_instance_handle ? 1 : 0;
    if (message->sample.has_instance_handle) {
        memcpy(out->info.instance_handle.bytes,
               message->sample.instance_handle,
               sizeof(out->info.instance_handle.bytes));
    }
    out->info.source_timestamp_ns = message->sample.source_timestamp_ns;
    out->info.reception_timestamp_ns = message->sample.reception_timestamp_ns;
    out->info.generation_rank =
        message->sample.generation_rank > 0
            ? (uint32_t)message->sample.generation_rank
            : 0u;
    out->info.writer_sn = message->writer_sn;
}
