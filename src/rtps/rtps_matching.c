/* RTPS spine: endpoint matching, partition rules and reader-state bookkeeping. */
#include "rtps_participant.h"

/* 内部共享形态（表、计数、定时器）与数据面入口，见 rtps_participant_priv.h。 */
#include "rtps_participant_priv.h"

#define NB_LOG_TAG "rtps"
#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zn/zn.h>

#include "rtps_message.h"
#include "rtps_param.h"
#include "rtps_reliable.h"
#include "rtps_sedp.h"
#include "rtps_spdp.h"
#include "rtps_wire.h"

static int partition_name_match(const char *pattern,
                                size_t pattern_len,
                                const char *name,
                                size_t name_len)
{
    size_t pi = 0;
    size_t ni = 0;

    while (pi < pattern_len) {
        if (pattern[pi] == '*') {
            size_t k;

            while (pi < pattern_len && pattern[pi] == '*') {
                pi++;
            }
            if (pi == pattern_len) {
                return 1;
            }
            for (k = ni; k <= name_len; k++) {
                if (partition_name_match(
                        pattern + pi, pattern_len - pi, name + k, name_len - k)) {
                    return 1;
                }
            }
            return 0;
        }
        if (ni >= name_len || name[ni] != pattern[pi]) {
            return 0;
        }
        pi++;
        ni++;
    }
    return ni == name_len;
}

static int partition_lists_match(const char *left, const char *right)
{
    const char *lp = left;

    for (;;) {
        size_t left_len = strcspn(lp, ",");
        const char *rp = right;

        for (;;) {
            size_t right_len = strcspn(rp, ",");

            if (partition_name_match(lp, left_len, rp, right_len) ||
                partition_name_match(rp, right_len, lp, left_len)) {
                return 1;
            }
            rp += right_len;
            if (*rp != ',') {
                break;
            }
            rp++;
        }
        lp += left_len;
        if (*lp != ',') {
            break;
        }
        lp++;
    }
    return 0;
}

const char *endpoint_qos_mismatch(const nb_rtps_endpoint_info_t *publication,
                                         const nb_rtps_endpoint_info_t *subscription)
{
    uint32_t pub_reliability;
    uint32_t sub_reliability;

    if (strcmp(publication->topic_name, subscription->topic_name) != 0) {
        return "topic";
    }
    if (strcmp(publication->type_name, subscription->type_name) != 0) {
        return "type";
    }
    if (!partition_lists_match(publication->partition, subscription->partition)) {
        return "partition";
    }
    pub_reliability = publication->reliability ? publication->reliability
                                               : NB_RTPS_RELIABILITY_BEST_EFFORT;
    sub_reliability = subscription->reliability ? subscription->reliability
                                                : NB_RTPS_RELIABILITY_BEST_EFFORT;
    if (sub_reliability == NB_RTPS_RELIABILITY_RELIABLE &&
        pub_reliability != NB_RTPS_RELIABILITY_RELIABLE) {
        return "reliability";
    }
    if (publication->durability < subscription->durability) {
        return "durability";
    }
    /*
     * Requested/offered policies, in the sense of DDS 1.4 section 2.2.3:
     * the writer has to offer at least what the reader requests. A zero
     * duration means "not set", which is the default and never a constraint.
     */
    if (subscription->deadline_ms > 0 &&
        (publication->deadline_ms == 0 ||
         publication->deadline_ms > subscription->deadline_ms)) {
        return "deadline";
    }
    if (subscription->latency_budget_ms > 0 &&
        publication->latency_budget_ms > subscription->latency_budget_ms) {
        return "latency budget";
    }
    if (publication->ownership != subscription->ownership) {
        return "ownership";
    }
    /* AUTOMATIC < MANUAL_BY_PARTICIPANT < MANUAL_BY_TOPIC. */
    if (publication->liveliness_kind < subscription->liveliness_kind) {
        return "liveliness kind";
    }
    if (subscription->liveliness_lease_ms > 0 &&
        (publication->liveliness_lease_ms == 0 ||
         publication->liveliness_lease_ms > subscription->liveliness_lease_ms)) {
        return "liveliness lease";
    }
    if (publication->destination_order < subscription->destination_order) {
        return "destination order";
    }
    /* Presentation: INSTANCE < TOPIC < GROUP, and the writer has to offer the
     * coherent/ordered access the reader asked for. */
    if (publication->presentation_scope < subscription->presentation_scope) {
        return "presentation scope";
    }
    if (subscription->coherent_access && !publication->coherent_access) {
        return "coherent access";
    }
    if (subscription->ordered_access && !publication->ordered_access) {
        return "ordered access";
    }
    return NULL;
}

static void emit_match_event(nb_rtps_participant_t *participant,
                             const nb_rtps_guid_t *local_guid,
                             const nb_rtps_endpoint_info_t *remote_info,
                             nb_rtps_endpoint_kind_t remote_kind,
                             int added)
{
    nb_rtps_status_event_t event;
    nb_rtps_status_callback_t callback;
    void *user;
    nb_rtps_local_endpoint_t *local;
    const char *reason;

    memset(&event, 0, sizeof(event));
    zb_mutex_lock(participant->lock);
    callback = participant->status_callback;
    user = participant->status_user;
    local = find_local_endpoint(participant, local_guid);
    if (!callback || !local || !local->used) {
        zb_mutex_unlock(participant->lock);
        return;
    }
    if (strcmp(local->info.topic_name, remote_info->topic_name) != 0 ||
        strcmp(local->info.type_name, remote_info->type_name) != 0) {
        zb_mutex_unlock(participant->lock);
        return;
    }
    if (local->kind == NB_RTPS_ENDPOINT_PUBLICATION &&
        remote_kind == NB_RTPS_ENDPOINT_SUBSCRIPTION) {
        event.kind = NB_RTPS_STATUS_PUBLICATION_MATCHED;
        reason = endpoint_qos_mismatch(&local->info, remote_info);
    } else if (local->kind == NB_RTPS_ENDPOINT_SUBSCRIPTION &&
               remote_kind == NB_RTPS_ENDPOINT_PUBLICATION) {
        event.kind = NB_RTPS_STATUS_SUBSCRIPTION_MATCHED;
        reason = endpoint_qos_mismatch(remote_info, &local->info);
    } else {
        zb_mutex_unlock(participant->lock);
        return;
    }
    if (reason) {
        /* A pair that never matched has nothing to un-match, and repeating the
         * same incompatibility on every SEDP refresh would be noise. */
        if (!added) {
            zb_mutex_unlock(participant->lock);
            return;
        }
        event.kind = NB_RTPS_STATUS_QOS_INCOMPATIBLE;
        copy_str(event.reason, sizeof(event.reason), reason);
    } else {
        if (added) {
            local->match_total++;
        }
        event.count = matched_remote_count(participant, local);
        event.total_count = local->match_total;
    }
    event.endpoint = local->info.endpoint_guid;
    event.writer = remote_info->endpoint_guid;
    event.has_peer = 1;
    zb_mutex_unlock(participant->lock);

    callback(user, &event);
}

void notify_matches(nb_rtps_participant_t *participant,
                           const nb_rtps_endpoint_info_t *remote_info,
                           nb_rtps_endpoint_kind_t remote_kind,
                           int added)
{
    size_t i;

    if (!participant || !remote_info) {
        return;
    }
    for (i = 0; i < participant->max_local_endpoints; i++) {
        nb_rtps_guid_t local_guid;
        int used = 0;

        zb_mutex_lock(participant->lock);
        if (participant->local_endpoints[i].used) {
            local_guid = participant->local_endpoints[i].info.endpoint_guid;
            used = 1;
        }
        zb_mutex_unlock(participant->lock);
        if (used) {
            emit_match_event(participant, &local_guid, remote_info, remote_kind, added);
        }
    }
}

void notify_matches_for_local(nb_rtps_participant_t *participant,
                              const nb_rtps_guid_t *local_guid)
{
    size_t i;

    if (!participant || !local_guid) {
        return;
    }
    for (i = 0; i < participant->max_remote_endpoints; i++) {
        nb_rtps_endpoint_info_t remote_info;
        nb_rtps_endpoint_kind_t remote_kind;
        int used = 0;

        zb_mutex_lock(participant->lock);
        if (participant->remote_endpoints[i].used) {
            remote_info = participant->remote_endpoints[i].info;
            remote_kind = participant->remote_endpoints[i].kind;
            used = 1;
        }
        zb_mutex_unlock(participant->lock);
        if (used) {
            emit_match_event(participant, local_guid, &remote_info, remote_kind, 1);
        }
    }
}

int endpoint_qos_compatible(const nb_rtps_endpoint_info_t *publication,
                                   const nb_rtps_endpoint_info_t *subscription)
{
    const char *reason = endpoint_qos_mismatch(publication, subscription);

    if (reason) {
        nb_log_debug("rtps: %s endpoints on topic '%s' do not match: %s",
                     publication->topic_name[0] ? "publisher/subscriber" : "endpoint",
                     publication->topic_name,
                     reason);
        return 0;
    }
    return 1;
}

int data_endpoint_matches(const nb_rtps_local_endpoint_t *local,
                                 const nb_rtps_remote_endpoint_t *remote)
{
    if (!local || !remote || local->kind != NB_RTPS_ENDPOINT_SUBSCRIPTION ||
        remote->kind != NB_RTPS_ENDPOINT_PUBLICATION) {
        return 0;
    }
    return endpoint_qos_compatible(&remote->info, &local->info);
}

int ensure_data_readers(nb_rtps_participant_t *participant)
{
    if (participant->data_readers) {
        return NB_OK;
    }
    participant->max_data_readers = participant->max_remote_endpoints;
    if (participant->max_data_readers > NB_RTPS_MAX_DATA_READERS) {
        participant->max_data_readers = NB_RTPS_MAX_DATA_READERS;
    }
    if (participant->max_data_readers == 0) {
        return NB_ERR_NOMEM;
    }
    participant->data_readers = (nb_rtps_data_reader_state_t *)calloc(
        participant->max_data_readers, sizeof(*participant->data_readers));
    participant->data_reader_used =
        (uint32_t *)calloc(participant->max_data_readers, sizeof(uint32_t));
    if (!participant->data_readers || !participant->data_reader_used) {
        free(participant->data_readers);
        free(participant->data_reader_used);
        participant->data_readers = NULL;
        participant->data_reader_used = NULL;
        participant->max_data_readers = 0;
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

nb_rtps_data_reader_state_t *find_data_reader(nb_rtps_participant_t *participant,
                                                     const nb_rtps_guid_t *writer,
                                                     const nb_rtps_entity_id_t *reader)
{
    size_t i;

    if (!participant->data_readers) {
        return NULL;
    }
    for (i = 0; i < participant->data_reader_used_count; i++) {
        nb_rtps_data_reader_state_t *state =
            &participant->data_readers[participant->data_reader_used[i]];

        if (nb_rtps_guid_equal(&state->writer, writer) &&
            nb_rtps_entity_id_equal(&state->reader, reader)) {
            return state;
        }
    }
    return NULL;
}

nb_rtps_data_reader_state_t *get_data_reader(nb_rtps_participant_t *participant,
                                                    const nb_rtps_guid_t *writer,
                                                    const nb_rtps_entity_id_t *reader,
                                                    uint64_t first_expected)
{
    nb_rtps_data_reader_state_t *state = find_data_reader(participant, writer, reader);
    nb_rtps_remote_endpoint_t *remote;
    size_t i;

    if (state) {
        return state;
    }
    if (ensure_data_readers(participant) != NB_OK) {
        return NULL;
    }
    for (i = 0; i < participant->max_data_readers; i++) {
        if (!participant->data_readers[i].used) {
            state = &participant->data_readers[i];
            break;
        }
    }
    if (!state) {
        if (!participant->data_reader_overflow_logged) {
            participant->data_reader_overflow_logged = 1;
            nb_log_warn("rtps: reader window table is full (%zu entries); "
                        "extra reliable pairs run without deduplication",
                        participant->max_data_readers);
        }
        return NULL;
    }
    memset(state, 0, sizeof(*state));
    state->used = 1;
    nb_rtps_used_add(participant->data_reader_used,
                     &participant->data_reader_used_count,
                     participant->max_data_readers,
                     (size_t)(state - participant->data_readers));
    state->writer = *writer;
    state->reader = *reader;
    /* The liveliness policy is the writer's, announced in SEDP. */
    remote = find_remote_endpoint(participant, writer);
    if (remote && remote->kind == NB_RTPS_ENDPOINT_PUBLICATION) {
        state->liveliness_kind = remote->info.liveliness_kind;
        state->liveliness_lease_ms = remote->info.liveliness_lease_ms;
    }
    participant->data_reader_count++;
    nb_rtps_reader_state_reset(&state->state, first_expected ? first_expected : 1u);
    state->last_sample_ms = zn_wall_ms();
    /* DDS considers a writer alive as soon as it is matched; the first missing
     * assertion is what changes that. */
    state->alive = 1;
    state->last_liveliness_ms = state->last_sample_ms;
    return state;
}

void arm_reader_deadlines(nb_rtps_participant_t *participant,
                                 const nb_rtps_remote_endpoint_t *remote)
{
    int monitors_liveliness;
    size_t i;

    if (!remote || remote->kind != NB_RTPS_ENDPOINT_PUBLICATION) {
        return;
    }
    /* Every liveliness kind with a finite lease is monitored: automatic and
     * manual-by-participant renew on ParticipantMessageData, manual-by-topic on
     * a message from the writer itself. */
    monitors_liveliness = remote->info.liveliness_lease_ms > 0;
    for (i = 0; i < participant->max_local_endpoints; i++) {
        nb_rtps_local_endpoint_t *local = &participant->local_endpoints[i];

        if (!local->used || local->kind != NB_RTPS_ENDPOINT_SUBSCRIPTION) {
            continue;
        }
        if (local->info.deadline_ms == 0 && !monitors_liveliness) {
            continue;
        }
        if (!endpoint_qos_compatible(&remote->info, &local->info)) {
            continue;
        }
        (void)get_data_reader(participant,
                              &remote->info.endpoint_guid,
                              &local->info.endpoint_guid.entity,
                              1u);
    }
}

void drop_data_readers_for_writer(nb_rtps_participant_t *participant,
                                         const nb_rtps_guid_t *writer)
{
    size_t i;

    if (!participant->data_readers) {
        return;
    }
    for (i = 0; i < participant->max_data_readers; i++) {
        nb_rtps_data_reader_state_t *state = &participant->data_readers[i];

        if (state->used && nb_rtps_guid_equal(&state->writer, writer)) {
            state->used = 0;
            nb_rtps_used_remove(participant->data_reader_used,
                                &participant->data_reader_used_count,
                                i);
            if (participant->data_reader_count > 0) {
                participant->data_reader_count--;
            }
        }
    }
}
