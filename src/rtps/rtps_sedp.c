#include "rtps_sedp.h"

#include <stdio.h>
#include <string.h>

#include "rtps_param.h"

/*
 * Durations on the wire are seconds plus a fraction in units of 2^-32 s, the
 * same convention the rest of the stack uses for timestamps. A zero
 * millisecond value means "policy not set": the PIDs are then omitted and the
 * peer applies the DDS default (infinite for deadline, lifespan and lease).
 */
#define NB_RTPS_DURATION_INFINITE_SECONDS 0x7fffffffu
#define NB_RTPS_MS_PER_SECOND 1000u

static void sedp_put_u32_le(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t sedp_get_u32_le(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static nb_rtps_duration_t sedp_duration_from_ms(uint32_t ms)
{
    nb_rtps_duration_t duration;

    if (ms == 0) {
        duration.seconds = (int32_t)NB_RTPS_DURATION_INFINITE_SECONDS;
        duration.fraction = 0xffffffffu;
    } else {
        duration.seconds = (int32_t)(ms / NB_RTPS_MS_PER_SECOND);
        duration.fraction =
            (uint32_t)(((uint64_t)(ms % NB_RTPS_MS_PER_SECOND) << 32) / NB_RTPS_MS_PER_SECOND);
    }
    return duration;
}

static uint32_t sedp_duration_to_ms(const nb_rtps_duration_t *duration)
{
    if (!duration || duration->seconds < 0 ||
        (uint32_t)duration->seconds == NB_RTPS_DURATION_INFINITE_SECONDS) {
        return 0;
    }
    /* Round to the nearest millisecond: 5 ms encodes as 21474836 / 2^32 s,
     * which truncates back to 4 without the rounding term. */
    return (uint32_t)duration->seconds * NB_RTPS_MS_PER_SECOND +
           (uint32_t)((((uint64_t)duration->fraction * NB_RTPS_MS_PER_SECOND) + 0x80000000ull) >>
                      32);
}

/* PID_LIVELINESS packs the kind and the lease into one parameter. */
static int sedp_put_liveliness(nb_rtps_param_writer_t *writer,
                               uint32_t kind,
                               uint32_t lease_ms)
{
    nb_rtps_duration_t lease = sedp_duration_from_ms(lease_ms);
    uint8_t value[12];

    sedp_put_u32_le(value, kind);
    value[4] = (uint8_t)((uint32_t)lease.seconds & 0xffu);
    value[5] = (uint8_t)(((uint32_t)lease.seconds >> 8) & 0xffu);
    value[6] = (uint8_t)(((uint32_t)lease.seconds >> 16) & 0xffu);
    value[7] = (uint8_t)(((uint32_t)lease.seconds >> 24) & 0xffu);
    sedp_put_u32_le(value + 8, lease.fraction);
    return nb_rtps_param_put_bytes(writer, NB_RTPS_PID_LIVELINESS, value, sizeof(value));
}

static int sedp_get_liveliness(const uint8_t *value,
                               size_t value_len,
                               uint32_t *kind,
                               uint32_t *lease_ms)
{
    nb_rtps_duration_t lease;
    int rc;

    if (!value || !kind || !lease_ms || value_len < 12) {
        return NB_ERR_INVALID;
    }
    rc = nb_rtps_param_get_duration(value + 4, value_len - 4, &lease);
    if (rc != NB_OK) {
        return rc;
    }
    *kind = sedp_get_u32_le(value);
    *lease_ms = sedp_duration_to_ms(&lease);
    return NB_OK;
}

static const uint8_t *sedp_params_begin(const uint8_t *payload, size_t len, size_t *params_len)
{
    const uint8_t *params = payload;
    size_t remaining = len;

    if (remaining >= 4 && params[0] == 0x00 && params[1] == 0x03 &&
        params[2] == 0x00 && params[3] == 0x00) {
        params += 4;
        remaining -= 4;
    }
    if (params_len) {
        *params_len = remaining;
    }
    return params;
}

int nb_rtps_sedp_build(const nb_rtps_endpoint_info_t *info,
                       void *buf,
                       size_t cap,
                       size_t *out_len)
{
    nb_rtps_param_writer_t writer;
    nb_rtps_duration_t max_blocking_time;
    uint32_t reliability;
    uint32_t history_depth;
    int rc;

    if (!info || !buf || info->topic_name[0] == '\0' || info->type_name[0] == '\0') {
        return NB_ERR_INVALID;
    }
    if (info->unicast_count > NB_RTPS_SEDP_LOCATORS_MAX ||
        info->multicast_count > NB_RTPS_SEDP_LOCATORS_MAX ||
        info->user_data_len > NB_RTPS_SEDP_USER_DATA_MAX) {
        return NB_ERR_TOOBIG;
    }
    reliability = info->reliability ? info->reliability : NB_RTPS_RELIABILITY_BEST_EFFORT;
    max_blocking_time.seconds = 0;
    max_blocking_time.fraction = 0;
    history_depth = info->history_depth ? info->history_depth : 1;

    nb_rtps_param_writer_init(&writer, buf, cap);
    rc = nb_rtps_param_put_encapsulation(&writer);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rtps_param_put_guid(&writer, NB_RTPS_PID_ENDPOINT_GUID, &info->endpoint_guid);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rtps_param_put_string(&writer, NB_RTPS_PID_TOPIC_NAME, info->topic_name);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rtps_param_put_string(&writer, NB_RTPS_PID_TYPE_NAME, info->type_name);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rtps_param_put_reliability(
        &writer, NB_RTPS_PID_RELIABILITY, reliability, &max_blocking_time);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rtps_param_put_u32(&writer, NB_RTPS_PID_DURABILITY, info->durability);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_rtps_param_put_history(
        &writer, NB_RTPS_PID_HISTORY, info->history_kind, history_depth);
    if (rc != NB_OK) {
        return rc;
    }
    if (info->deadline_ms > 0) {
        nb_rtps_duration_t deadline = sedp_duration_from_ms(info->deadline_ms);

        rc = nb_rtps_param_put_duration(&writer, NB_RTPS_PID_DEADLINE, &deadline);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->latency_budget_ms > 0) {
        nb_rtps_duration_t budget = sedp_duration_from_ms(info->latency_budget_ms);

        rc = nb_rtps_param_put_duration(&writer, NB_RTPS_PID_LATENCY_BUDGET, &budget);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->lifespan_ms > 0) {
        nb_rtps_duration_t lifespan = sedp_duration_from_ms(info->lifespan_ms);

        rc = nb_rtps_param_put_duration(&writer, NB_RTPS_PID_LIFESPAN, &lifespan);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->min_separation_ms > 0) {
        nb_rtps_duration_t separation = sedp_duration_from_ms(info->min_separation_ms);

        rc = nb_rtps_param_put_duration(
            &writer, NB_RTPS_PID_TIME_BASED_FILTER, &separation);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->max_samples != 0 || info->max_instances != 0 ||
        info->max_samples_per_instance != 0) {
        rc = nb_rtps_param_put_resource_limits(&writer,
                                               NB_RTPS_PID_RESOURCE_LIMITS,
                                               info->max_samples,
                                               info->max_instances,
                                               info->max_samples_per_instance);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->liveliness_kind != NB_RTPS_LIVELINESS_AUTOMATIC ||
        info->liveliness_lease_ms > 0) {
        rc = sedp_put_liveliness(
            &writer, info->liveliness_kind, info->liveliness_lease_ms);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->ownership != NB_RTPS_OWNERSHIP_SHARED) {
        rc = nb_rtps_param_put_u32(&writer, NB_RTPS_PID_OWNERSHIP, info->ownership);
        if (rc != NB_OK) {
            return rc;
        }
        rc = nb_rtps_param_put_u32(
            &writer, NB_RTPS_PID_OWNERSHIP_STRENGTH, info->ownership_strength);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->destination_order != NB_RTPS_DESTINATION_ORDER_BY_RECEPTION) {
        rc = nb_rtps_param_put_u32(
            &writer, NB_RTPS_PID_DESTINATION_ORDER, info->destination_order);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->presentation_scope != NB_RTPS_PRESENTATION_INSTANCE ||
        info->coherent_access || info->ordered_access) {
        /* accessScope is a 4-byte enum followed by the two boolean flags, the
         * layout the DDS spec fixes for PresentationQosPolicy. */
        uint8_t presentation[6];

        presentation[0] = (uint8_t)(info->presentation_scope & 0xffu);
        presentation[1] = (uint8_t)((info->presentation_scope >> 8) & 0xffu);
        presentation[2] = (uint8_t)((info->presentation_scope >> 16) & 0xffu);
        presentation[3] = (uint8_t)((info->presentation_scope >> 24) & 0xffu);
        presentation[4] = info->coherent_access ? 1u : 0u;
        presentation[5] = info->ordered_access ? 1u : 0u;
        rc = nb_rtps_param_put_bytes(
            &writer, NB_RTPS_PID_PRESENTATION, presentation, sizeof(presentation));
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->partition[0] != '\0') {
        char copy[NB_RTPS_SEDP_PARTITION_MAX];
        const char *names[8];
        size_t name_count = 0;
        char *p;

        (void)snprintf(copy, sizeof(copy), "%s", info->partition);
        p = copy;
        while (p && *p != '\0' && name_count < sizeof(names) / sizeof(names[0])) {
            char *comma;

            while (*p == ' ') {
                p++;
            }
            if (*p == '\0') {
                break;
            }
            names[name_count++] = p;
            comma = strchr(p, ',');
            if (comma) {
                *comma = '\0';
                p = comma + 1;
            } else {
                p = NULL;
            }
        }
        if (name_count > 0) {
            rc = nb_rtps_param_put_string_sequence(
                &writer, NB_RTPS_PID_PARTITION, names, name_count);
            if (rc != NB_OK) {
                return rc;
            }
        }
    }
    if (info->unicast_count > 0) {
        size_t i;
        for (i = 0; i < info->unicast_count; i++) {
            rc = nb_rtps_param_put_locator(
                &writer, NB_RTPS_PID_UNICAST_LOCATOR, &info->unicast[i]);
            if (rc != NB_OK) {
                return rc;
            }
        }
    }
    if (info->multicast_count > 0) {
        size_t i;
        for (i = 0; i < info->multicast_count; i++) {
            rc = nb_rtps_param_put_locator(
                &writer, NB_RTPS_PID_MULTICAST_LOCATOR, &info->multicast[i]);
            if (rc != NB_OK) {
                return rc;
            }
        }
    }
    if (info->user_data_len > 0) {
        rc = nb_rtps_param_put_octet_sequence(
            &writer, NB_RTPS_PID_USER_DATA, info->user_data, info->user_data_len);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (info->requests_keyhash) {
        /* Cyclone only attaches PID_KEY_HASH to keyed samples when a matched
         * reader asked for it. The value is a CDR boolean (one octet). */
        uint8_t flag = 1;

        rc = nb_rtps_param_put_bytes(
            &writer, NB_RTPS_PID_CYCLONE_REQUESTS_KEYHASH, &flag, sizeof(flag));
        if (rc != NB_OK) {
            return rc;
        }
    }
    rc = nb_rtps_param_writer_finish(&writer);
    if (rc != NB_OK) {
        return rc;
    }
    if (out_len) {
        *out_len = nb_rtps_param_writer_len(&writer);
    }
    return NB_OK;
}

int nb_rtps_sedp_parse(const void *payload, size_t len, nb_rtps_endpoint_info_t *out)
{
    nb_rtps_param_reader_t reader;
    const uint8_t *params;
    size_t params_len = 0;
    uint16_t pid = 0;
    const uint8_t *value = NULL;
    size_t value_len = 0;
    int seen_guid = 0;
    int seen_topic = 0;
    int seen_type = 0;
    int rc;

    if (!payload || !out) {
        return NB_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    out->reliability = NB_RTPS_RELIABILITY_BEST_EFFORT;
    out->durability = NB_RTPS_DURABILITY_VOLATILE;
    out->history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    out->history_depth = 1;

    params = sedp_params_begin((const uint8_t *)payload, len, &params_len);
    nb_rtps_param_reader_init(&reader, params, params_len);
    for (;;) {
        rc = nb_rtps_param_reader_next(&reader, &pid, &value, &value_len);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            break;
        }
        switch (pid) {
        case NB_RTPS_PID_ENDPOINT_GUID:
            rc = nb_rtps_param_get_guid(value, value_len, &out->endpoint_guid);
            if (rc != NB_OK) {
                return rc;
            }
            seen_guid = 1;
            break;
        case NB_RTPS_PID_TOPIC_NAME:
            rc = nb_rtps_param_get_string(
                value, value_len, out->topic_name, sizeof(out->topic_name));
            if (rc != NB_OK) {
                return rc;
            }
            seen_topic = 1;
            break;
        case NB_RTPS_PID_TYPE_NAME:
            rc = nb_rtps_param_get_string(
                value, value_len, out->type_name, sizeof(out->type_name));
            if (rc != NB_OK) {
                return rc;
            }
            seen_type = 1;
            break;
        case NB_RTPS_PID_RELIABILITY:
            rc = nb_rtps_param_get_u32(value, value_len, &out->reliability);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_DURABILITY:
            rc = nb_rtps_param_get_u32(value, value_len, &out->durability);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_HISTORY:
            rc = nb_rtps_param_get_history(
                value, value_len, &out->history_kind, &out->history_depth);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_DEADLINE: {
            nb_rtps_duration_t deadline;

            rc = nb_rtps_param_get_duration(value, value_len, &deadline);
            if (rc != NB_OK) {
                return rc;
            }
            out->deadline_ms = sedp_duration_to_ms(&deadline);
            break;
        }
        case NB_RTPS_PID_LATENCY_BUDGET: {
            nb_rtps_duration_t budget;

            rc = nb_rtps_param_get_duration(value, value_len, &budget);
            if (rc != NB_OK) {
                return rc;
            }
            out->latency_budget_ms = sedp_duration_to_ms(&budget);
            break;
        }
        case NB_RTPS_PID_LIFESPAN: {
            nb_rtps_duration_t lifespan;

            rc = nb_rtps_param_get_duration(value, value_len, &lifespan);
            if (rc != NB_OK) {
                return rc;
            }
            out->lifespan_ms = sedp_duration_to_ms(&lifespan);
            break;
        }
        case NB_RTPS_PID_TIME_BASED_FILTER: {
            nb_rtps_duration_t separation;

            rc = nb_rtps_param_get_duration(value, value_len, &separation);
            if (rc != NB_OK) {
                return rc;
            }
            out->min_separation_ms = sedp_duration_to_ms(&separation);
            break;
        }
        case NB_RTPS_PID_RESOURCE_LIMITS:
            rc = nb_rtps_param_get_resource_limits(value,
                                                   value_len,
                                                   &out->max_samples,
                                                   &out->max_instances,
                                                   &out->max_samples_per_instance);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_LIVELINESS:
            rc = sedp_get_liveliness(
                value, value_len, &out->liveliness_kind, &out->liveliness_lease_ms);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_OWNERSHIP:
            rc = nb_rtps_param_get_u32(value, value_len, &out->ownership);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_OWNERSHIP_STRENGTH:
            rc = nb_rtps_param_get_u32(value, value_len, &out->ownership_strength);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_DESTINATION_ORDER:
            rc = nb_rtps_param_get_u32(value, value_len, &out->destination_order);
            if (rc != NB_OK) {
                return rc;
            }
            break;
        case NB_RTPS_PID_PRESENTATION:
            if (value_len < 6) {
                return NB_ERR_BAD_MESSAGE;
            }
            out->presentation_scope = (uint32_t)value[0] |
                                      ((uint32_t)value[1] << 8) |
                                      ((uint32_t)value[2] << 16) |
                                      ((uint32_t)value[3] << 24);
            out->coherent_access = value[4] != 0;
            out->ordered_access = value[5] != 0;
            if (out->presentation_scope > NB_RTPS_PRESENTATION_GROUP) {
                return NB_ERR_BAD_MESSAGE;
            }
            break;
        case NB_RTPS_PID_PARTITION:
            /* The wire form is a sequence of names; the stack keeps the list as
             * one comma-separated string, the same spelling nb_qos_t uses. */
            (void)nb_rtps_param_get_string_sequence_joined(
                value, value_len, out->partition, sizeof(out->partition));
            break;
        case NB_RTPS_PID_UNICAST_LOCATOR:
            if (out->unicast_count < NB_RTPS_SEDP_LOCATORS_MAX) {
                rc = nb_rtps_param_get_locator(
                    value, value_len, &out->unicast[out->unicast_count]);
                if (rc != NB_OK) {
                    return rc;
                }
                out->unicast_count++;
            }
            break;
        case NB_RTPS_PID_MULTICAST_LOCATOR:
            if (out->multicast_count < NB_RTPS_SEDP_LOCATORS_MAX) {
                rc = nb_rtps_param_get_locator(
                    value, value_len, &out->multicast[out->multicast_count]);
                if (rc != NB_OK) {
                    return rc;
                }
                out->multicast_count++;
            }
            break;
        case NB_RTPS_PID_USER_DATA: {
            const uint8_t *data = NULL;
            size_t data_len = 0;
            rc = nb_rtps_param_get_octet_sequence(value, value_len, &data, &data_len);
            if (rc != NB_OK) {
                return rc;
            }
            /* Optional and unbounded on the wire: keep what fits. */
            if (data_len > sizeof(out->user_data)) {
                data_len = sizeof(out->user_data);
            }
            if (data_len > 0) {
                memcpy(out->user_data, data, data_len);
            }
            out->user_data_len = data_len;
            break;
        }
        default:
            /* Unknown and vendor-specific parameters are ignored. */
            break;
        }
    }
    if (!nb_rtps_param_reader_ok(&reader)) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (!seen_guid || !seen_topic || !seen_type ||
        out->topic_name[0] == '\0' || out->type_name[0] == '\0') {
        return NB_ERR_BAD_MESSAGE;
    }
    return NB_OK;
}

int nb_rtps_sedp_build_key(const nb_rtps_guid_t *endpoint_guid,
                           void *buf,
                           size_t cap,
                           size_t *out_len)
{
    nb_rtps_param_writer_t writer;
    int rc;

    if (!endpoint_guid || !buf) {
        return NB_ERR_INVALID;
    }
    nb_rtps_param_writer_init(&writer, buf, cap);
    rc = nb_rtps_param_put_encapsulation(&writer);
    if (rc == NB_OK) {
        rc = nb_rtps_param_put_guid(
            &writer, NB_RTPS_PID_ENDPOINT_GUID, endpoint_guid);
    }
    if (rc == NB_OK) {
        rc = nb_rtps_param_writer_finish(&writer);
    }
    if (rc != NB_OK) {
        return rc;
    }
    if (out_len) {
        *out_len = nb_rtps_param_writer_len(&writer);
    }
    return NB_OK;
}

int nb_rtps_sedp_parse_key(const void *payload, size_t len, nb_rtps_guid_t *out)
{
    nb_rtps_param_reader_t reader;
    const uint8_t *params;
    size_t params_len = 0;
    uint16_t pid = 0;
    const uint8_t *value = NULL;
    size_t value_len = 0;
    int found = 0;
    int rc;

    if (!payload || !out) {
        return NB_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    params = sedp_params_begin((const uint8_t *)payload, len, &params_len);
    nb_rtps_param_reader_init(&reader, params, params_len);
    for (;;) {
        rc = nb_rtps_param_reader_next(&reader, &pid, &value, &value_len);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            break;
        }
        if (pid == NB_RTPS_PID_ENDPOINT_GUID) {
            rc = nb_rtps_param_get_guid(value, value_len, out);
            if (rc != NB_OK) {
                return rc;
            }
            found = 1;
            break;
        }
    }
    if (!nb_rtps_param_reader_ok(&reader) || !found) {
        return NB_ERR_BAD_MESSAGE;
    }
    return NB_OK;
}
