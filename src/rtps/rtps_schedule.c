/* RTPS spine: participant thread, discovery/keepalive timers and liveness/deadline sweeps. */
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

static void check_deadlines(nb_rtps_participant_t *participant, uint64_t now)
{
    nb_rtps_status_event_t events[NB_RTPS_MAX_STATUS_EVENTS];
    size_t count = 0;
    size_t i;
    nb_rtps_status_callback_t callback;
    void *user;

    zb_mutex_lock(participant->lock);
    callback = participant->status_callback;
    user = participant->status_user;
    if (callback) {
        for (i = 0; i < participant->max_local_endpoints; i++) {
            nb_rtps_local_endpoint_t *local = &participant->local_endpoints[i];

            if (!local->used || local->kind != NB_RTPS_ENDPOINT_PUBLICATION ||
                local->info.deadline_ms == 0 || count == NB_RTPS_MAX_STATUS_EVENTS) {
                continue;
            }
            if (now < local->last_write_ms ||
                now - local->last_write_ms <= local->info.deadline_ms) {
                continue;
            }
            if (now < local->next_deadline_ms) {
                continue;
            }
            local->deadline_miss_count++;
            events[count].kind = NB_RTPS_STATUS_OFFERED_DEADLINE_MISSED;
            events[count].endpoint = local->info.endpoint_guid;
            events[count].count = local->deadline_miss_count;
            count++;
            local->next_deadline_ms = now + local->info.deadline_ms;
        }
        for (i = 0; participant->data_readers && i < participant->max_data_readers; i++) {
            nb_rtps_data_reader_state_t *state = &participant->data_readers[i];
            nb_rtps_local_endpoint_t *local;
            nb_rtps_guid_t reader_guid;

            if (!state->used || count == NB_RTPS_MAX_STATUS_EVENTS) {
                continue;
            }
            reader_guid.prefix = participant->local_guid.prefix;
            reader_guid.entity = state->reader;
            local = find_local_endpoint(participant, &reader_guid);
            if (!local || local->kind != NB_RTPS_ENDPOINT_SUBSCRIPTION ||
                local->info.deadline_ms == 0) {
                continue;
            }
            if (now < state->last_sample_ms ||
                now - state->last_sample_ms <= local->info.deadline_ms) {
                continue;
            }
            if (now < state->next_deadline_ms) {
                continue;
            }
            state->deadline_miss_count++;
            events[count].kind = NB_RTPS_STATUS_REQUESTED_DEADLINE_MISSED;
            events[count].endpoint = local->info.endpoint_guid;
            events[count].count = state->deadline_miss_count;
            count++;
            state->next_deadline_ms = now + local->info.deadline_ms;
        }
    }
    zb_mutex_unlock(participant->lock);

    if (callback) {
        for (i = 0; i < count; i++) {
            callback(user, &events[i]);
        }
    }
}

void send_pmd(nb_rtps_participant_t *participant, uint32_t kind)
{
    uint8_t payload[64];
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_pmd_t pmd;
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_data_t data;
    size_t payload_len = 0;
    size_t message_len;
    uint64_t sn;
    uint64_t now;
    size_t i;

    memset(&pmd, 0, sizeof(pmd));
    pmd.participant = participant->local_guid.prefix;
    pmd.kind = kind;
    if (nb_rtps_pmd_encode(&pmd, payload, sizeof(payload), &payload_len) != NB_OK) {
        return;
    }
    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    zb_mutex_lock(participant->lock);
    sn = ++participant->pmd_sn;
    zb_mutex_unlock(participant->lock);

    memset(&data, 0, sizeof(data));
    data.reader_id = nb_rtps_eid_participant_message_reader;
    data.writer_id = nb_rtps_eid_participant_message_writer;
    nb_rtps_sn_from_u64(&data.writer_sn, sn);
    data.payload = payload;
    data.payload_len = payload_len;
    /* PMD 是内建 writer，按源地址投递，不带 INFO_DST。 */
    message_len = nb_rtps_wire_data(message, sizeof(message), &header, &timestamp, NULL, &data);
    if (message_len == 0) {
        return;
    }

    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remotes; i++) {
        nb_rtps_remote_participant_t *remote = &participant->remotes[i];
        char host[NB_RTPS_PEER_HOST_MAX];
        uint16_t port = 0;

        if (!remote->used ||
            remote_target(remote, host, sizeof(host), &port) != NB_OK) {
            continue;
        }
        (void)nb_rtps_sendto(participant, participant->unicast_sock, message, message_len, host, port);
    }
    zb_mutex_unlock(participant->lock);
}

void handle_pmd_data(nb_rtps_participant_t *participant,
                            const nb_rtps_data_view_t *data,
                            const nb_rtps_guid_prefix_t *prefix)
{
    nb_rtps_pmd_t pmd;
    nb_rtps_remote_participant_t *remote;
    uint64_t now;
    size_t i;

    if (nb_rtps_pmd_decode(data->payload, data->payload_len, &pmd) != NB_OK) {
        return;
    }
    /* The sample carries the prefix it claims to be for; a forged or stale
     * prefix must not renew a different participant's lease. */
    if (!nb_rtps_guid_prefix_equal(&pmd.participant, prefix)) {
        return;
    }
    if (pmd.kind != NB_RTPS_PMD_KIND_AUTOMATIC && pmd.kind != NB_RTPS_PMD_KIND_MANUAL) {
        return;
    }
    now = zn_wall_ms();
    zb_mutex_lock(participant->lock);
    remote = find_remote(participant, prefix);
    if (remote) {
        remote->last_pmd_ms = now;
    }
    for (i = 0; participant->data_readers && i < participant->max_data_readers; i++) {
        nb_rtps_data_reader_state_t *state = &participant->data_readers[i];

        if (state->used &&
            nb_rtps_guid_prefix_equal(&state->writer.prefix, prefix) &&
            /* A manual-by-topic writer is asserted on its own messages, so a
             * participant-level sample must not keep it alive. */
            state->liveliness_kind != NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC &&
            (!state->liveliness_kind ||
             state->liveliness_kind ==
                 (pmd.kind == NB_RTPS_PMD_KIND_MANUAL
                      ? NB_RTPS_LIVELINESS_MANUAL_BY_PARTICIPANT
                      : NB_RTPS_LIVELINESS_AUTOMATIC))) {
            state->last_liveliness_ms = now;
        }
    }
    zb_mutex_unlock(participant->lock);
}

static void liveliness_totals(nb_rtps_participant_t *participant,
                              const nb_rtps_entity_id_t *reader_id,
                              uint32_t *alive_count,
                              uint32_t *not_alive_count)
{
    size_t i;

    *alive_count = 0;
    *not_alive_count = 0;
    for (i = 0; participant->data_readers && i < participant->max_data_readers; i++) {
        const nb_rtps_data_reader_state_t *state = &participant->data_readers[i];

        if (!state->used || !nb_rtps_entity_id_equal(&state->reader, reader_id)) {
            continue;
        }
        if (state->alive) {
            (*alive_count)++;
        } else {
            (*not_alive_count)++;
        }
    }
}

static void check_liveliness(nb_rtps_participant_t *participant, uint64_t now)
{
    nb_rtps_status_event_t events[NB_RTPS_MAX_STATUS_EVENTS];
    size_t count = 0;
    size_t i;
    nb_rtps_status_callback_t callback;
    void *user;

    memset(events, 0, sizeof(events));
    zb_mutex_lock(participant->lock);
    callback = participant->status_callback;
    user = participant->status_user;
    if (callback && participant->data_readers) {
        for (i = 0; i < participant->max_data_readers; i++) {
            nb_rtps_data_reader_state_t *state = &participant->data_readers[i];
            nb_rtps_local_endpoint_t *local;
            nb_rtps_guid_t reader_guid;
            uint32_t lease;
            int alive;

            if (!state->used || count == NB_RTPS_MAX_STATUS_EVENTS) {
                continue;
            }
            /* The lease is the one the matched writer announced; every kind is
             * monitored, the renewal rules differ per kind. */
            lease = state->liveliness_lease_ms;
            if (lease == 0) {
                continue;
            }
            reader_guid.prefix = participant->local_guid.prefix;
            reader_guid.entity = state->reader;
            local = find_local_endpoint(participant, &reader_guid);
            if (!local || local->kind != NB_RTPS_ENDPOINT_SUBSCRIPTION) {
                continue;
            }
            alive = now >= state->last_liveliness_ms &&
                    now - state->last_liveliness_ms <= lease;
            if (alive == state->alive) {
                continue;
            }
            state->alive = alive;
            state->liveliness_events++;
            events[count].kind = NB_RTPS_STATUS_LIVELINESS_CHANGED;
            events[count].endpoint = local->info.endpoint_guid;
            events[count].writer = state->writer;
            events[count].has_peer = 1;
            events[count].count = state->liveliness_events;
            events[count].alive = alive;
            liveliness_totals(participant,
                              &local->info.endpoint_guid.entity,
                              &events[count].alive_count,
                              &events[count].not_alive_count);
            count++;
        }
    }
    zb_mutex_unlock(participant->lock);

    if (callback) {
        for (i = 0; i < count; i++) {
            callback(user, &events[i]);
        }
    }
}

static void sweep(nb_rtps_participant_t *participant, uint64_t now)
{
    for (;;) {
        nb_rtps_participant_info_t info;
        int found = 0;
        uint32_t i;

        zb_mutex_lock(participant->lock);
        for (i = 0; i < participant->max_remotes; i++) {
            nb_rtps_remote_participant_t *remote = &participant->remotes[i];
            if (remote->used && now >= remote->last_seen_ms &&
                now - remote->last_seen_ms > remote->lease_ms) {
                info = remote->info;
                remote->used = 0;
                nb_rtps_used_remove(participant->remotes_used,
                                    &participant->remotes_used_count,
                                    i);
                participant->stats.lease_expired++;
                found = 1;
                break;
            }
        }
        zb_mutex_unlock(participant->lock);
        if (!found) {
            break;
        }
        notify(participant, NB_RTPS_PARTICIPANT_REMOVED, &info);
        remove_remote_endpoints_for_prefix(participant, &info.guid.prefix);
    }
    check_deadlines(participant, now);
    check_liveliness(participant, now);
    retry_tcp_links(participant);
}

static uint64_t next_announce_delay(nb_rtps_participant_t *participant)
{
    uint32_t window = participant->announce_interval_ms / 5u;
    int64_t delay;
    uint32_t random;

    if (window == 0) {
        return participant->announce_interval_ms;
    }
    participant->rng = participant->rng * 1103515245u + 12345u;
    random = participant->rng % (window * 2u + 1u);
    delay = (int64_t)participant->announce_interval_ms + (int64_t)random - (int64_t)window;
    if (delay < (int64_t)(participant->announce_interval_ms / 2u)) {
        delay = participant->announce_interval_ms / 2u;
    }
    if (delay < (int64_t)NB_RTPS_MIN_ANNOUNCE_MS) {
        delay = NB_RTPS_MIN_ANNOUNCE_MS;
    }
    return (uint64_t)delay;
}

static void service_frag_assemblies(nb_rtps_participant_t *participant, uint64_t now)
{
    size_t i;

    if (!participant || !participant->frag_assemblies) {
        return;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_frag_assemblies; i++) {
        nb_rtps_frag_assembly_t *assembly = &participant->frag_assemblies[i];

        if (!assembly->used || assembly->complete) {
            continue;
        }
        if (assembly->nack_count >= NB_RTPS_FRAG_MAX_NACKS) {
            continue;
        }
        if (now < assembly->last_update_ms ||
            now - assembly->last_update_ms < NB_RTPS_FRAG_NACK_DELAY_MS) {
            continue;
        }
        if (assembly->nack_count > 0 && now >= assembly->last_nack_ms &&
            now - assembly->last_nack_ms < NB_RTPS_FRAG_NACK_RETRY_MS) {
            continue;
        }
        (void)send_frag_nack(participant, assembly, now);
    }
    zb_mutex_unlock(participant->lock);
}

void thread_main(void *user)
{
    nb_rtps_participant_t *participant = (nb_rtps_participant_t *)user;

    while (!participant->stop) {
        int rc = zn_poller_poll(participant->poller, 100);
        uint64_t now;

        if (rc > 0) {
            drain_socket(participant, participant->rx_sock);
            drain_socket(participant, participant->unicast_sock);
            if (participant->data_sock) {
                drain_socket(participant, participant->data_sock);
            }
        }
        nb_rtps_tcp_service(participant->tcp);
        now = zn_wall_ms();
        if (now >= participant->next_announce_ms) {
            (void)send_spdp(participant);
            participant->next_announce_ms = now + next_announce_delay(participant);
        }
        if (now >= participant->next_sweep_ms) {
            sweep(participant, now);
            participant->next_sweep_ms = now + NB_RTPS_SWEEP_INTERVAL_MS;
        }
        if (now >= participant->next_sedp_heartbeat_ms) {
            send_sedp_heartbeats(participant);
            participant->next_sedp_heartbeat_ms = now + participant->sedp_heartbeat_ms;
        }
        if (now >= participant->next_data_heartbeat_ms) {
            send_data_heartbeats(participant);
            participant->next_data_heartbeat_ms = now + participant->data_heartbeat_ms;
        }
        if (participant->pmd_interval_ms != 0 && now >= participant->next_pmd_ms) {
            send_pmd(participant, NB_RTPS_PMD_KIND_AUTOMATIC);
            participant->next_pmd_ms = now + participant->pmd_interval_ms;
        }
        service_frag_assemblies(participant, now);
    }
}
