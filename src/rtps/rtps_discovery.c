/*
 * RTPS 发现：SPDP（参与者公告）与 SEDP（端点公告）的报文收发、远端参与者
 * 与远端端点的登记/淘汰。
 *
 * 只负责“发现面”的样本与状态：报文怎么拼交给 rtps_wire.c，用户数据面在
 * rtps_datapath.c，生命周期与线程节拍留在 rtps_participant.c。
 */
#include "rtps_participant_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_LOG_TAG "rtps"
#include <nb/base/log.h>

#include "rtps_message.h"
#include "rtps_param.h"
#include "rtps_sedp.h"
#include "rtps_spdp.h"
#include "rtps_wire.h"

int parse_ipv4(const char *host, uint8_t out[4])
{
    unsigned long parts[4];
    const char *cursor = host;
    int i;

    if (!host || host[0] == '\0') {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < 4; i++) {
        char *end = NULL;
        parts[i] = strtoul(cursor, &end, 10);
        if (end == cursor || parts[i] > 255u) {
            return NB_ERR_INVALID;
        }
        if (i < 3) {
            if (*end != '.') {
                return NB_ERR_INVALID;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return NB_ERR_INVALID;
        }
    }
    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)parts[i];
    }
    return NB_OK;
}
uint32_t lease_from_info(const nb_rtps_participant_info_t *info, uint32_t fallback)
{
    uint64_t ms;

    if (info->lease_duration.seconds <= 0) {
        return fallback;
    }
    ms = (uint64_t)(uint32_t)info->lease_duration.seconds * 1000u;
    ms += ((uint64_t)info->lease_duration.fraction * 1000u) >> 32;
    if (ms < NB_RTPS_MIN_LEASE_MS) {
        ms = NB_RTPS_MIN_LEASE_MS;
    }
    if (ms > NB_RTPS_MAX_LEASE_MS) {
        ms = NB_RTPS_MAX_LEASE_MS;
    }
    return (uint32_t)ms;
}
void fill_local_info(const nb_rtps_participant_t *participant,
                            nb_rtps_participant_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    info->vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    info->guid = participant->local_guid;
    info->guid.entity = nb_rtps_eid_participant;
    info->lease_duration.seconds = (int32_t)(participant->lease_ms / 1000u);
    info->lease_duration.fraction =
        (uint32_t)(((uint64_t)(participant->lease_ms % 1000u) << 32) / 1000u);
    info->builtin_endpoints = NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_ANNOUNCER |
                              NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR |
                              NB_RTPS_BUILTIN_ENDPOINT_PUBLICATIONS_ANNOUNCER |
                              NB_RTPS_BUILTIN_ENDPOINT_PUBLICATIONS_DETECTOR |
                              NB_RTPS_BUILTIN_ENDPOINT_SUBSCRIPTIONS_ANNOUNCER |
                              NB_RTPS_BUILTIN_ENDPOINT_SUBSCRIPTIONS_DETECTOR |
                              NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER |
                              NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER;
    info->metatraffic_unicast = participant->metatraffic_unicast;
    if (participant->metatraffic_tcp.kind != NB_RTPS_LOCATOR_KIND_INVALID) {
        info->metatraffic_tcp = participant->metatraffic_tcp;
    }
    if (participant->multicast_enabled) {
        info->metatraffic_multicast = participant->metatraffic_multicast;
    }
    info->default_unicast = participant->default_unicast;
    copy_str(info->name, sizeof(info->name), participant->name);
}
static int send_sedp_data(nb_rtps_participant_t *participant,
                          nb_rtps_endpoint_kind_t kind,
                          uint64_t sn,
                          const uint8_t *payload,
                          size_t payload_len,
                          int dispose,
                          const nb_rtps_guid_prefix_t *dst,
                          const char *host,
                          uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    uint8_t inline_qos[16];
    nb_rtps_param_writer_t params;
    size_t inline_qos_len = 0;
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_data_t data;
    uint64_t now;
    size_t message_len;

    if (!participant || !payload || !host || host[0] == '\0' || port == 0 ||
        payload_len > NB_RTPS_SEDP_PAYLOAD_MAX) {
        return NB_ERR_INVALID;
    }
    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    memset(&data, 0, sizeof(data));
    data.reader_id = nb_rtps_eid_unknown;
    data.writer_id = kind == NB_RTPS_ENDPOINT_PUBLICATION
                         ? nb_rtps_eid_sedp_publications_writer
                         : nb_rtps_eid_sedp_subscriptions_writer;
    nb_rtps_sn_from_u64(&data.writer_sn, sn);
    data.payload = payload;
    data.payload_len = payload_len;
    data.dispose = dispose ? 1 : 0;
    if (dispose) {
        /* A dispose carries the state in PID_STATUS_INFO. Without it peers
         * ignore the K flag in the sense that they do not learn the kind of
         * state change: Cyclone routes discovery samples by the status bits
         * parsed out of the inline QoS, so a key-only sample with no status
         * would be treated as a live (re-)announcement instead of a removal. */
        uint8_t status_info[4] = {0x00, 0x00, 0x00, 0x01};

        nb_rtps_param_writer_init(&params, inline_qos, sizeof(inline_qos));
        if (nb_rtps_param_put_bytes(&params,
                                    NB_RTPS_PID_STATUS_INFO,
                                    status_info,
                                    sizeof(status_info)) != NB_OK ||
            nb_rtps_param_writer_finish(&params) != NB_OK) {
            return NB_ERR_BAD_MESSAGE;
        }
        inline_qos_len = nb_rtps_param_writer_len(&params);
        data.inline_qos = inline_qos;
        data.inline_qos_len = inline_qos_len;
    }

    message_len = nb_rtps_wire_data(message, sizeof(message), &header, &timestamp, dst, &data);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant->net.unicast_sock, message, message_len, host, port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.sedp_sent++;
    return NB_OK;
}
static void send_local_endpoint_to_remote(nb_rtps_participant_t *participant,
                                          const nb_rtps_local_endpoint_t *local,
                                          const nb_rtps_guid_prefix_t *dst,
                                          const char *host,
                                          uint16_t port)
{
    uint8_t payload[NB_RTPS_SEDP_PAYLOAD_MAX];
    size_t payload_len = 0;

    if (nb_rtps_sedp_build(&local->info, payload, sizeof(payload), &payload_len) == NB_OK) {
        (void)send_sedp_data(participant,
                             local->kind,
                             local->sn,
                             payload,
                             payload_len,
                             0,
                             dst,
                             host,
                             port);
    }
}
void broadcast_sedp_sample(nb_rtps_participant_t *participant,
                                  nb_rtps_endpoint_kind_t kind,
                                  uint64_t sn,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  int dispose)
{
    char host[NB_RTPS_PEER_HOST_MAX];
    uint16_t port;
    uint32_t i;

    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->max_remotes; i++) {
        nb_rtps_remote_participant_t *remote = &participant->tables.remotes[i];
        if (!remote->used || remote_target(remote, host, sizeof(host), &port) != NB_OK) {
            continue;
        }
        (void)send_sedp_data(participant,
                             kind,
                             sn,
                             payload,
                             payload_len,
                             dispose,
                             &remote->info.guid.prefix,
                             host,
                             port);
    }
    zb_mutex_unlock(participant->net.lock);
}
void send_local_endpoint_to_remotes(nb_rtps_participant_t *participant,
                                           const nb_rtps_local_endpoint_t *local)
{
    uint8_t payload[NB_RTPS_SEDP_PAYLOAD_MAX];
    size_t payload_len = 0;

    if (nb_rtps_sedp_build(&local->info, payload, sizeof(payload), &payload_len) == NB_OK) {
        broadcast_sedp_sample(
            participant, local->kind, local->sn, payload, payload_len, 0);
    }
}
static void send_all_local_endpoints_to_remote(nb_rtps_participant_t *participant,
                                               const nb_rtps_guid_prefix_t *dst,
                                               const char *host,
                                               uint16_t port)
{
    size_t i;

    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->tables.max_local_endpoints; i++) {
        if (participant->tables.local_endpoints[i].used) {
            send_local_endpoint_to_remote(
                participant, &participant->tables.local_endpoints[i], dst, host, port);
        }
    }
    zb_mutex_unlock(participant->net.lock);
}
static void send_sedp_heartbeat(nb_rtps_participant_t *participant,
                                nb_rtps_endpoint_kind_t kind,
                                const nb_rtps_remote_participant_t *remote)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_heartbeat_t heartbeat;
    nb_rtps_reliable_writer_t *history = kind == NB_RTPS_ENDPOINT_PUBLICATION
                                             ? participant->tables.publications_writer
                                             : participant->tables.subscriptions_writer;
    uint64_t first_sn = 0;
    uint64_t last_sn = 0;
    size_t sample_count = 0;
    char host[NB_RTPS_PEER_HOST_MAX];
    uint16_t port;
    uint64_t now;
    size_t message_len;

    nb_rtps_reliable_state(history, &first_sn, &last_sn, &sample_count);
    if (sample_count == 0 || remote_target(remote, host, sizeof(host), &port) != NB_OK) {
        return;
    }
    if (kind == NB_RTPS_ENDPOINT_PUBLICATION) {
        heartbeat.count = ++participant->tables.publications_hb_count;
    } else {
        heartbeat.count = ++participant->tables.subscriptions_hb_count;
    }

    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    heartbeat.reader_id = kind == NB_RTPS_ENDPOINT_PUBLICATION
                              ? nb_rtps_eid_sedp_publications_reader
                              : nb_rtps_eid_sedp_subscriptions_reader;
    heartbeat.writer_id = kind == NB_RTPS_ENDPOINT_PUBLICATION
                              ? nb_rtps_eid_sedp_publications_writer
                              : nb_rtps_eid_sedp_subscriptions_writer;
    nb_rtps_sn_from_u64(&heartbeat.first_sn, first_sn);
    nb_rtps_sn_from_u64(&heartbeat.last_sn, last_sn);

    message_len = nb_rtps_wire_heartbeat(message,
                                         sizeof(message),
                                         &header,
                                         &timestamp,
                                         &remote->info.guid.prefix,
                                         &heartbeat);
    if (message_len == 0) {
        return;
    }
    (void)nb_rtps_sendto(participant, participant->net.unicast_sock, message, message_len, host, port);
    participant->stats.heartbeats_sent++;
}
void send_sedp_heartbeats(nb_rtps_participant_t *participant)
{
    uint32_t i;

    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->max_remotes; i++) {
        if (participant->tables.remotes[i].used) {
            send_sedp_heartbeat(
                participant, NB_RTPS_ENDPOINT_PUBLICATION, &participant->tables.remotes[i]);
            send_sedp_heartbeat(
                participant, NB_RTPS_ENDPOINT_SUBSCRIPTION, &participant->tables.remotes[i]);
        }
    }
    zb_mutex_unlock(participant->net.lock);
}
static void send_sedp_acknack(nb_rtps_participant_t *participant,
                              nb_rtps_endpoint_kind_t kind,
                              const nb_rtps_guid_prefix_t *dst,
                              const char *host,
                              uint16_t port,
                              uint32_t base,
                              uint32_t num_bits,
                              const uint32_t *bitmap,
                              uint32_t count)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_acknack_t acknack;
    uint64_t now;
    size_t message_len;

    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    memset(&acknack, 0, sizeof(acknack));
    acknack.reader_id = kind == NB_RTPS_ENDPOINT_PUBLICATION
                            ? nb_rtps_eid_sedp_publications_reader
                            : nb_rtps_eid_sedp_subscriptions_reader;
    acknack.writer_id = kind == NB_RTPS_ENDPOINT_PUBLICATION
                            ? nb_rtps_eid_sedp_publications_writer
                            : nb_rtps_eid_sedp_subscriptions_writer;
    nb_rtps_sn_from_u64(&acknack.base, base);
    acknack.num_bits = num_bits;
    acknack.count = count;
    if (bitmap) {
        memcpy(acknack.bitmap, bitmap, sizeof(acknack.bitmap));
    }
    message_len = nb_rtps_wire_acknack(message,
                                       sizeof(message),
                                       &header,
                                       &timestamp,
                                       dst,
                                       &acknack);
    if (message_len == 0) {
        return;
    }
    (void)nb_rtps_sendto(participant, participant->net.unicast_sock, message, message_len, host, port);
    participant->stats.acknacks_sent++;
}
int send_spdp(nb_rtps_participant_t *participant)
{
    nb_rtps_participant_info_t info;
    uint8_t payload[NB_RTPS_SPDP_PAYLOAD_MAX];
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_data_t data;
    size_t payload_len = 0;
    size_t message_len;
    uint64_t sn;
    size_t i;
    int sent = 0;
    int targets = 0;
    uint64_t now;

    fill_local_info(participant, &info);
    if (nb_rtps_spdp_build(&info, payload, sizeof(payload), &payload_len) != NB_OK) {
        return NB_ERR_BAD_MESSAGE;
    }
    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header.guid_prefix = participant->local_guid.prefix;

    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);

    zb_mutex_lock(participant->net.lock);
    sn = ++participant->timers.spdp_sn;
    zb_mutex_unlock(participant->net.lock);

    memset(&data, 0, sizeof(data));
    data.reader_id = nb_rtps_eid_unknown;
    data.writer_id = nb_rtps_eid_spdp_writer;
    nb_rtps_sn_from_u64(&data.writer_sn, sn);
    data.payload = payload;
    data.payload_len = payload_len;

    /* SPDP 只按源地址投递，不带 INFO_DST。 */
    message_len = nb_rtps_wire_data(message, sizeof(message), &header, &timestamp, NULL, &data);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }

    if (participant->multicast_enabled) {
        targets++;
        if (nb_rtps_sendto(participant, participant->net.rx_sock,
                              message,
                              message_len,
                              participant->multicast_group,
                              participant->spdp_port) >= 0) {
            sent++;
        participant->stats.spdp_sent++;
        }
    }
    for (i = 0; i < participant->peer_count; i++) {
        targets++;
        if (nb_rtps_sendto(participant, participant->net.rx_sock,
                              message,
                              message_len,
                              participant->peers[i].host,
                              participant->peers[i].port) >= 0) {
            sent++;
        participant->stats.spdp_sent++;
        }
    }
    if (targets == 0) {
        return NB_OK;
    }
    return sent > 0 ? NB_OK : NB_ERR_IO;
}
static void update_remote_endpoint(nb_rtps_participant_t *participant,
                                   nb_rtps_endpoint_kind_t kind,
                                   const nb_rtps_endpoint_info_t *info)
{
    nb_rtps_remote_endpoint_t *slot = NULL;
    nb_rtps_remote_endpoint_t copy;
    nb_rtps_endpoint_event_t event = NB_RTPS_ENDPOINT_UPDATED;
    uint64_t now = zn_wall_ms();
    size_t slot_index = 0;
    int was_used = 0;
    size_t i;

    zb_mutex_lock(participant->net.lock);
    slot = find_remote_endpoint(participant, &info->endpoint_guid);
    if (slot) {
        if (memcmp(&slot->info, info, sizeof(*info)) == 0 && slot->kind == kind) {
            slot->last_seen_ms = now;
            zb_mutex_unlock(participant->net.lock);
            return;
        }
    } else {
        for (i = 0; i < participant->tables.max_remote_endpoints; i++) {
            if (!participant->tables.remote_endpoints[i].used) {
                slot = &participant->tables.remote_endpoints[i];
                break;
            }
        }
        if (!slot) {
            slot = &participant->tables.remote_endpoints[0];
            for (i = 1; i < participant->tables.max_remote_endpoints; i++) {
                if (participant->tables.remote_endpoints[i].last_seen_ms < slot->last_seen_ms) {
                    slot = &participant->tables.remote_endpoints[i];
                }
            }
        }
        event = NB_RTPS_ENDPOINT_ADDED;
    }
    was_used = slot->used;
    slot->used = 1;
    slot->kind = kind;
    slot->info = *info;
    slot->last_seen_ms = now;
    slot_index = (size_t)(slot - participant->tables.remote_endpoints);
    if (!was_used) {
        nb_rtps_used_add(participant->tables.remote_ep_used,
                         &participant->tables.remote_ep_used_count,
                         participant->tables.max_remote_endpoints,
                         slot_index);
    }
    if (kind == NB_RTPS_ENDPOINT_PUBLICATION) {
        arm_reader_deadlines(participant, slot);
    }
    copy = *slot;
    zb_mutex_unlock(participant->net.lock);

    notify_endpoint(participant, event, copy.kind, &copy.info);
    if (event == NB_RTPS_ENDPOINT_ADDED && kind == NB_RTPS_ENDPOINT_SUBSCRIPTION) {
        replay_retained_history(participant, &copy.info);
    }
    if (event == NB_RTPS_ENDPOINT_ADDED) {
        notify_matches(participant, &copy.info, kind, 1);
    }
}
static int remove_remote_endpoint(nb_rtps_participant_t *participant,
                                  const nb_rtps_guid_t *guid,
                                  nb_rtps_endpoint_kind_t *kind,
                                  nb_rtps_endpoint_info_t *info)
{
    nb_rtps_remote_endpoint_t *slot;
    nb_rtps_endpoint_kind_t removed_kind = NB_RTPS_ENDPOINT_PUBLICATION;
    nb_rtps_endpoint_info_t removed_info;
    int found = 0;

    memset(&removed_info, 0, sizeof(removed_info));
    zb_mutex_lock(participant->net.lock);
    slot = find_remote_endpoint(participant, guid);
    if (slot) {
        removed_kind = slot->kind;
        removed_info = slot->info;
        if (kind) {
            *kind = slot->kind;
        }
        if (info) {
            *info = slot->info;
        }
        slot->used = 0;
        nb_rtps_used_remove(participant->tables.remote_ep_used,
                            &participant->tables.remote_ep_used_count,
                            (size_t)(slot - participant->tables.remote_endpoints));
        if (slot->kind == NB_RTPS_ENDPOINT_PUBLICATION) {
            drop_data_readers_for_writer(participant, &slot->info.endpoint_guid);
        }
        found = 1;
    }
    zb_mutex_unlock(participant->net.lock);
    if (found) {
        notify_matches(participant, &removed_info, removed_kind, 0);
    }
    return found;
}
void remove_remote_endpoints_for_prefix(nb_rtps_participant_t *participant,
                                               const nb_rtps_guid_prefix_t *prefix)
{
    for (;;) {
        nb_rtps_remote_endpoint_t copy;
        size_t i;
        int found = 0;

        zb_mutex_lock(participant->net.lock);
        for (i = 0; i < participant->tables.max_remote_endpoints; i++) {
            nb_rtps_remote_endpoint_t *endpoint = &participant->tables.remote_endpoints[i];
            if (endpoint->used &&
                nb_rtps_guid_prefix_equal(&endpoint->info.endpoint_guid.prefix, prefix)) {
                copy = *endpoint;
                endpoint->used = 0;
                nb_rtps_used_remove(participant->tables.remote_ep_used,
                                    &participant->tables.remote_ep_used_count,
                                    i);
                if (copy.kind == NB_RTPS_ENDPOINT_PUBLICATION) {
                    size_t j;

                    drop_data_readers_for_writer(participant, &copy.info.endpoint_guid);
                    for (j = 0; j < participant->tables.max_local_endpoints; j++) {
                        if (participant->tables.local_endpoints[j].used) {
                            mark_instances_no_writers(
                                &participant->tables.local_endpoints[j],
                                &copy.info.endpoint_guid);
                        }
                    }
                }
                found = 1;
                break;
            }
        }
        zb_mutex_unlock(participant->net.lock);
        if (!found) {
            break;
        }
        notify_endpoint(participant, NB_RTPS_ENDPOINT_REMOVED, copy.kind, &copy.info);
        notify_matches(participant, &copy.info, copy.kind, 0);
    }
}
void update_remote(nb_rtps_participant_t *participant,
                          const nb_rtps_participant_info_t *info,
                          const char *source_host,
                          uint16_t source_port)
{
    nb_rtps_remote_participant_t *slot = NULL;
    nb_rtps_participant_info_t copy;
    nb_rtps_participant_event_t event = NB_RTPS_PARTICIPANT_UPDATED;
    uint64_t now = zn_wall_ms();
    uint32_t i;
    int notify_event = 0;
    int send_endpoints = 0;
    int was_used = 0;
    nb_rtps_guid_prefix_t evicted_prefix;
    int evicted = 0;

    zb_mutex_lock(participant->net.lock);
    slot = find_remote(participant, &info->guid.prefix);
    if (slot) {
        notify_event = memcmp(&slot->info, info, sizeof(*info)) != 0;
        event = NB_RTPS_PARTICIPANT_UPDATED;
        if (memcmp(&slot->info.metatraffic_unicast,
                   &info->metatraffic_unicast,
                   sizeof(info->metatraffic_unicast)) != 0) {
            send_endpoints = 1;
        }
    } else {
        for (i = 0; i < participant->max_remotes; i++) {
            if (!participant->tables.remotes[i].used) {
                slot = &participant->tables.remotes[i];
                break;
            }
        }
        if (!slot) {
            /* Deterministic eviction: the oldest lease wins. */
            slot = &participant->tables.remotes[0];
            for (i = 1; i < participant->max_remotes; i++) {
                if (participant->tables.remotes[i].last_seen_ms < slot->last_seen_ms) {
                    slot = &participant->tables.remotes[i];
                }
            }
            evicted_prefix = slot->info.guid.prefix;
            evicted = slot->used;
        }
        was_used = slot->used;
        memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        if (!was_used) {
            nb_rtps_used_add(participant->tables.remotes_used,
                             &participant->tables.remotes_used_count,
                             participant->max_remotes,
                             (size_t)(slot - participant->tables.remotes));
        }
        nb_rtps_reader_state_reset(&slot->publications_reader, 1);
        nb_rtps_reader_state_reset(&slot->subscriptions_reader, 1);
        event = NB_RTPS_PARTICIPANT_ADDED;
        notify_event = 1;
        send_endpoints = 1;
    }
    slot->info = *info;
    slot->last_seen_ms = now;
    slot->lease_ms = lease_from_info(info, participant->lease_ms);
    copy_str(slot->source_host, sizeof(slot->source_host), source_host);
    slot->source_port = source_port;
    copy = slot->info;
    zb_mutex_unlock(participant->net.lock);

    if (notify_event) {
        notify(participant, event, &copy);
    }
    if (evicted) {
        remove_remote_endpoints_for_prefix(participant, &evicted_prefix);
    }
    if (send_endpoints) {
        char host[NB_RTPS_PEER_HOST_MAX];
        uint16_t port = 0;
        if (locator_to_host(&copy.metatraffic_unicast, host, sizeof(host), &port) != NB_OK) {
            copy_str(host, sizeof(host), source_host);
            port = source_port;
        }
        if (port != 0 && host[0] != '\0') {
            send_all_local_endpoints_to_remote(
                participant, &copy.guid.prefix, host, port);
        }
        /* A peer that just appeared has no assertion history, so its lease
         * would otherwise start counting from discovery. Assert right away. */
        if (participant->timers.pmd_interval_ms != 0) {
            send_pmd(participant, NB_RTPS_PMD_KIND_AUTOMATIC);
            participant->timers.next_pmd_ms = zn_wall_ms() + participant->timers.pmd_interval_ms;
        }
    }
    /* A fresh participant is the moment to bring the stream up, not the next
     * announce interval. */
    if (participant->tcp) {
        maybe_tcp_connect(participant, &copy.guid.prefix);
    }
}
static nb_rtps_sedp_reader_state_t *sedp_reader_state(nb_rtps_remote_participant_t *remote,
                                                      nb_rtps_endpoint_kind_t kind)
{
    return kind == NB_RTPS_ENDPOINT_PUBLICATION ? &remote->publications_reader
                                                : &remote->subscriptions_reader;
}
void handle_sedp_gap(nb_rtps_participant_t *participant,
                            nb_rtps_endpoint_kind_t kind,
                            const nb_rtps_gap_t *gap,
                            const nb_rtps_guid_prefix_t *source_prefix)
{
    nb_rtps_remote_participant_t *remote;

    zb_mutex_lock(participant->net.lock);
    remote = find_remote(participant, source_prefix);
    if (remote) {
        nb_rtps_reader_on_gap(sedp_reader_state(remote, kind), gap);
    }
    zb_mutex_unlock(participant->net.lock);
}
void handle_sedp_data(nb_rtps_participant_t *participant,
                             nb_rtps_endpoint_kind_t kind,
                             const nb_rtps_data_view_t *data,
                             const nb_rtps_guid_prefix_t *source_prefix)
{
    nb_rtps_remote_participant_t *remote;
    nb_rtps_sedp_reader_state_t *state;
    nb_rtps_endpoint_info_t info;
    nb_rtps_guid_t key;

    uint64_t sample_sn = nb_rtps_sn_to_u64(&data->writer_sn);

    if (sample_sn == 0) {
        return;
    }
    /* Acknowledge the sequence number before interpreting the sample. A SEDP
     * sample this implementation cannot parse (an oversized optional field, a
     * vendor extension) must still leave the reader window, otherwise the peer
     * keeps retransmitting it forever in answer to our NACK. */
    zb_mutex_lock(participant->net.lock);
    remote = find_remote(participant, source_prefix);
    if (remote) {
        state = sedp_reader_state(remote, kind);
        (void)nb_rtps_reader_on_sample(state, sample_sn);
    }
    zb_mutex_unlock(participant->net.lock);
    if (!remote) {
        return;
    }
    if (data->dispose) {
        if (nb_rtps_sedp_parse_key(data->payload, data->payload_len, &key) != NB_OK ||
            !nb_rtps_guid_prefix_equal(&key.prefix, source_prefix)) {
            return;
        }
        if (remote) {
            nb_rtps_endpoint_kind_t removed_kind;
            if (remove_remote_endpoint(participant, &key, &removed_kind, &info)) {
                if (removed_kind == NB_RTPS_ENDPOINT_PUBLICATION) {
                    /* The remote writer unregistered: the instances it fed are
                     * still known to the subscriptions but have no writer, so
                     * the next SampleInfo for them reports NO_WRITERS. */
                    size_t j;

                    zb_mutex_lock(participant->net.lock);
                    for (j = 0; j < participant->tables.max_local_endpoints; j++) {
                        if (participant->tables.local_endpoints[j].used) {
                            mark_instances_no_writers(
                                &participant->tables.local_endpoints[j],
                                &info.endpoint_guid);
                        }
                    }
                    zb_mutex_unlock(participant->net.lock);
                }
                notify_endpoint(
                    participant, NB_RTPS_ENDPOINT_REMOVED, removed_kind, &info);
            }
        }
        return;
    }
    if (nb_rtps_sedp_parse(data->payload, data->payload_len, &info) != NB_OK ||
        !nb_rtps_guid_prefix_equal(&info.endpoint_guid.prefix, source_prefix)) {
        return;
    }
    update_remote_endpoint(participant, kind, &info);
}
void handle_sedp_heartbeat(nb_rtps_participant_t *participant,
                                  nb_rtps_endpoint_kind_t kind,
                                  const nb_rtps_heartbeat_t *heartbeat,
                                  const nb_rtps_guid_prefix_t *source_prefix,
                                  const char *source_host,
                                  uint16_t source_port)
{
    nb_rtps_remote_participant_t *remote;
    nb_rtps_sedp_reader_state_t *state;
    char target_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t target_port = 0;
    uint32_t base;
    uint32_t num_bits = 0;
    uint32_t bitmap[NB_RTPS_ACKNACK_MAX_WORDS];
    uint32_t count;
    uint64_t first_sn;
    uint64_t last_sn;
    uint32_t i;

    first_sn = nb_rtps_sn_to_u64(&heartbeat->first_sn);
    if (first_sn == 0) {
        first_sn = 1;
    }
    last_sn = nb_rtps_sn_to_u64(&heartbeat->last_sn);
    if (last_sn == 0) {
        return;
    }
    memset(bitmap, 0, sizeof(bitmap));

    zb_mutex_lock(participant->net.lock);
    remote = find_remote(participant, source_prefix);
    if (!remote) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    if (remote_target(remote, target_host, sizeof(target_host), &target_port) != NB_OK) {
        copy_str(target_host, sizeof(target_host), source_host);
        target_port = source_port;
    }
    state = sedp_reader_state(remote, kind);
    if (first_sn > state->next_expected) {
        nb_rtps_reader_state_reset(state, first_sn);
    } else if (last_sn + 1u < state->next_expected) {
        /* The writer restarted and reused sequence numbers. */
        nb_rtps_reader_state_reset(state, first_sn);
    }
    base = state->next_expected;
    if (last_sn >= base) {
        uint64_t span = (uint64_t)last_sn - (uint64_t)base + 1u;
        num_bits = span > NB_RTPS_ACKNACK_MAX_BITS
                       ? NB_RTPS_ACKNACK_MAX_BITS
                       : (uint32_t)span;
        for (i = 0; i < num_bits; i++) {
            if ((state->received[i / 32u] & (1u << (i % 32u))) == 0) {
                bitmap[i / 32u] |= 1u << (31u - (i % 32u));
            }
        }
    }
    count = ++state->ack_count;
    zb_mutex_unlock(participant->net.lock);

    send_sedp_acknack(participant,
                      kind,
                      source_prefix,
                      target_host,
                      target_port,
                      base,
                      num_bits,
                      bitmap,
                      count);
}
typedef struct sedp_resend_context {
    nb_rtps_participant_t *participant;
    nb_rtps_endpoint_kind_t kind;
    const nb_rtps_guid_prefix_t *dst;
    const char *host;
    uint16_t port;
} sedp_resend_context_t;

static int sedp_resend_callback(void *user,
                                uint64_t sn,
                                const uint8_t *payload,
                                size_t len,
                                int dispose)
{
    sedp_resend_context_t *context = (sedp_resend_context_t *)user;

    return send_sedp_data(context->participant,
                          context->kind,
                          sn,
                          payload,
                          len,
                          dispose,
                          context->dst,
                          context->host,
                          context->port);
}
void handle_sedp_acknack(nb_rtps_participant_t *participant,
                                nb_rtps_endpoint_kind_t kind,
                                const nb_rtps_acknack_t *acknack,
                                const nb_rtps_guid_prefix_t *source_prefix,
                                const char *source_host,
                                uint16_t source_port)
{
    nb_rtps_reliable_writer_t *history = kind == NB_RTPS_ENDPOINT_PUBLICATION
                                             ? participant->tables.publications_writer
                                             : participant->tables.subscriptions_writer;
    nb_rtps_remote_participant_t *remote;
    char target_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t target_port = source_port;
    sedp_resend_context_t context;

    copy_str(target_host, sizeof(target_host), source_host);
    zb_mutex_lock(participant->net.lock);
    remote = find_remote(participant, source_prefix);
    if (remote &&
        remote_target(remote, target_host, sizeof(target_host), &target_port) != NB_OK) {
        copy_str(target_host, sizeof(target_host), source_host);
        target_port = source_port;
    }
    zb_mutex_unlock(participant->net.lock);

    context.participant = participant;
    context.kind = kind;
    context.dst = source_prefix;
    context.host = target_host;
    context.port = target_port;
    (void)nb_rtps_reliable_process_acknack(
        history, acknack, sedp_resend_callback, &context, NULL);
}
