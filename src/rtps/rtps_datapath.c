/*
 * RTPS 用户数据面：DATA / DATA_FRAG 的收发、GAP / ACKNACK / HEARTBEAT 的收发、
 * 分片重组，以及把样本投递给本地订阅。
 *
 * 报文怎么拼由 rtps_wire.c 负责；这里只决定发什么、发给谁、按什么时序。
 * 参与者的表、锁与目标解析来自 rtps_participant_priv.h。
 */
#include "rtps_participant_priv.h"

#include <stdlib.h>
#include <string.h>

#define NB_LOG_TAG "rtps"
#include <nb/base/log.h>

#include "rtps_message.h"
#include "rtps_param.h"
#include "rtps_wire.h"

/* Defined further down; the fragmented and single-DATA senders both use it. */
static int build_instance_inline_qos(
    const uint8_t *handle,
    uint32_t status,
    uint8_t *buf,
    size_t cap,
    size_t *out_len);

static uint32_t nb_rtps_fragment_count(size_t payload_len)
{
    return (uint32_t)((payload_len + NB_RTPS_FRAGMENT_SIZE - 1u) / NB_RTPS_FRAGMENT_SIZE);
}
/*
 * Send one DATA_FRAG submessage for fragment `fragment_num` (1-based). One
 * fragment travels per datagram: a fragment plus the RTPS and submessage
 * headers stays well inside the 1400-byte packet budget, and it keeps both the
 * send path and a partial retransmit simple.
 */
static int send_user_fragment(nb_rtps_participant_t *participant,
                              const nb_rtps_guid_t *writer_guid,
                              const nb_rtps_guid_t *reader_guid,
                              uint64_t sn,
                              uint64_t source_ns,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t fragment_num,
                              const uint8_t *handle,
                              uint32_t status,
                              const char *host,
                              uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    uint8_t inline_qos[64];
    size_t inline_qos_len = 0;
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_data_frag_t fragment;
    size_t message_len;
    size_t offset;
    size_t chunk;

    if (fragment_num == 0) {
        return NB_ERR_INVALID;
    }
    offset = (size_t)(fragment_num - 1u) * NB_RTPS_FRAGMENT_SIZE;
    if (offset >= payload_len) {
        return NB_ERR_INVALID;
    }
    chunk = payload_len - offset;
    if (chunk > NB_RTPS_FRAGMENT_SIZE) {
        chunk = NB_RTPS_FRAGMENT_SIZE;
    }
    init_header(participant, &header);
    timestamp = rtps_time_from_ns(source_ns ? source_ns : zn_wall_ms() * 1000000ull);
    memset(&fragment, 0, sizeof(fragment));
    fragment.reader_id = reader_guid->entity;
    fragment.writer_id = writer_guid->entity;
    nb_rtps_sn_from_u64(&fragment.writer_sn, sn);
    fragment.fragment_starting_num = fragment_num;
    fragment.fragments_in_submessage = 1;
    fragment.fragment_size = NB_RTPS_FRAGMENT_SIZE;
    fragment.sample_size = (uint32_t)payload_len;
    fragment.payload = payload + offset;
    fragment.payload_len = chunk;
    /* Key hash and status travel once, in the first fragment, the way Cyclone
     * puts them on fragnum 0 only. */
    if (handle && fragment_num == 1) {
        if (build_instance_inline_qos(
                handle, status, inline_qos, sizeof(inline_qos), &inline_qos_len) != NB_OK) {
            return NB_ERR_BAD_MESSAGE;
        }
        fragment.inline_qos = inline_qos;
        fragment.inline_qos_len = inline_qos_len;
    }
    fragment.key = status != 0;

    message_len = nb_rtps_wire_data_frag(message,
                                         sizeof(message),
                                         &header,
                                         &timestamp,
                                         &reader_guid->prefix,
                                         &fragment);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant),
                          message,
                          message_len,
                          host,
                          port) < 0) {
        return NB_ERR_IO;    }
    participant->stats.data_sent++;
    participant->stats.fragments_sent++;

    return NB_OK;
}
/*
 * HEARTBEAT_FRAG tells a reader how many fragments a sample has, so it can ask
 * for the ones it missed with NACK_FRAG instead of triggering a whole-sample
 * retransmit.
 */
static int send_user_heartbeat_frag(nb_rtps_participant_t *participant,
                                    const nb_rtps_guid_t *writer_guid,
                                    const nb_rtps_guid_t *reader_guid,
                                    uint64_t sn,
                                    uint32_t last_fragment_num,
                                    const char *host,
                                    uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_heartbeat_frag_t heartbeat;
    uint64_t now;
    size_t message_len;

    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.reader_id = reader_guid->entity;
    heartbeat.writer_id = writer_guid->entity;
    nb_rtps_sn_from_u64(&heartbeat.writer_sn, sn);
    heartbeat.last_fragment_num = last_fragment_num;
    heartbeat.count = ++participant->tables.frag_heartbeat_count;

    message_len = nb_rtps_wire_heartbeat_frag(message,
                                              sizeof(message),
                                              &header,
                                              &timestamp,
                                              &reader_guid->prefix,
                                              &heartbeat);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant),
                          message,
                          message_len,
                          host,
                          port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.heartbeats_sent++;

    return NB_OK;
}
static int send_user_data_fragmented(nb_rtps_participant_t *participant,
                                     const nb_rtps_guid_t *writer_guid,
                                     const nb_rtps_guid_t *reader_guid,
                                     uint64_t sn,
                                     uint64_t source_ns,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     const uint8_t *handle,
                                     uint32_t status,
                                     const char *host,
                                     uint16_t port)
{
    uint32_t fragment_count = nb_rtps_fragment_count(payload_len);
    uint32_t fragment_num;
    int rc;

    for (fragment_num = 1; fragment_num <= fragment_count; fragment_num++) {
        rc = send_user_fragment(participant,
                                writer_guid,
                                reader_guid,
                                sn,
                                source_ns,
                                payload,
                                payload_len,
                                fragment_num,
                                handle,
                                status,
                                host,
                                port);
        if (rc != NB_OK) {
            return rc;
        }
    }
    (void)send_user_heartbeat_frag(
        participant, writer_guid, reader_guid, sn, fragment_count, host, port);
    return NB_OK;
}
/*
 * Build the inline QoS of a keyed sample. The instance handle always goes on
 * the wire and a state change adds the DDS status bits, so a reader that
 * cannot deserialize the key still knows which instance changed. Shared by the
 * single-DATA and the DATA_FRAG paths; only the first fragment carries it.
 */
static int build_instance_inline_qos(const uint8_t *handle,
                                     uint32_t status,
                                     uint8_t *buf,
                                     size_t cap,
                                     size_t *out_len)
{
    nb_rtps_param_writer_t params;
    uint8_t status_info[4] = {0x00, 0x00, 0x00, (uint8_t)status};

    if (!handle || !buf || !out_len) {
        return NB_ERR_INVALID;
    }
    nb_rtps_param_writer_init(&params, buf, cap);
    if (nb_rtps_param_put_bytes(&params,
                                NB_RTPS_PID_KEY_HASH,
                                handle,
                                NB_INSTANCE_HANDLE_SIZE) != NB_OK ||
        (status != 0 &&
         nb_rtps_param_put_bytes(&params,
                                 NB_RTPS_PID_STATUS_INFO,
                                 status_info,
                                 sizeof(status_info)) != NB_OK) ||
        nb_rtps_param_writer_finish(&params) != NB_OK) {
        return NB_ERR_BAD_MESSAGE;
    }
    *out_len = nb_rtps_param_writer_len(&params);
    return NB_OK;
}
static int send_user_data(nb_rtps_participant_t *participant,
                          const nb_rtps_guid_t *writer_guid,
                          const nb_rtps_guid_t *reader_guid,
                          uint64_t sn,
                          uint64_t source_ns,
                          const void *payload,
                          size_t payload_len,
                          const uint8_t *handle,
                          uint32_t status,
                          const char *host,
                          uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    uint8_t inline_qos[64];
    size_t inline_qos_len = 0;
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_data_t data;
    size_t message_len;

    if (!participant || !writer_guid || !reader_guid || !payload ||
        payload_len == 0 || payload_len > NB_RTPS_SAMPLE_MAX ||
        !host || host[0] == '\0' || port == 0) {
        return NB_ERR_INVALID;
    }
    if (payload_len > NB_RTPS_DATA_PAYLOAD_MAX) {
        return send_user_data_fragmented(participant,
                                         writer_guid,
                                         reader_guid,
                                         sn,
                                         source_ns,
                                         (const uint8_t *)payload,
                                         payload_len,
                                         handle,
                                         status,
                                         host,
                                         port);
    }
    init_header(participant, &header);
    timestamp = rtps_time_from_ns(source_ns ? source_ns : zn_wall_ms() * 1000000ull);
    memset(&data, 0, sizeof(data));
    data.reader_id = reader_guid->entity;
    data.writer_id = writer_guid->entity;
    nb_rtps_sn_from_u64(&data.writer_sn, sn);
    data.payload = (const uint8_t *)payload;
    data.payload_len = payload_len;
    if (handle) {
        if (build_instance_inline_qos(
                handle, status, inline_qos, sizeof(inline_qos), &inline_qos_len) != NB_OK) {
            return NB_ERR_BAD_MESSAGE;
        }
        data.inline_qos = inline_qos;
        data.inline_qos_len = inline_qos_len;
        data.key = status != 0;
        data.dispose = status == 1;
    }

    message_len = nb_rtps_wire_data(message,
                                    sizeof(message),
                                    &header,
                                    &timestamp,
                                    &reader_guid->prefix,
                                    &data);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant), message, message_len, host, port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.data_sent++;

    return NB_OK;
}
/* Announce that the writer will not send [first_sn, last_sn]. */
static int send_user_gap(nb_rtps_participant_t *participant,
                         const nb_rtps_guid_t *writer_guid,
                         const nb_rtps_guid_t *reader_guid,
                         uint64_t first_sn,
                         uint64_t last_sn,
                         const char *host,
                         uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_gap_t gap;
    uint64_t now;
    size_t message_len;

    if (!participant || !writer_guid || !reader_guid || first_sn == 0 ||
        last_sn < first_sn || !host || host[0] == '\0' || port == 0) {
        return NB_ERR_INVALID;
    }
    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    memset(&gap, 0, sizeof(gap));
    gap.reader_id = reader_guid->entity;
    gap.writer_id = writer_guid->entity;
    nb_rtps_sn_from_u64(&gap.gap_start, first_sn);
    /* GAP semantics (RTPS 2.5 8.3.7.4): the reader must consider unavailable
     * every sequence number in [gapStart, bitmapBase) plus the set bits. Every
     * caller here announces one contiguous range, so an empty bitmap with
     * bitmapBase just past the range is both the shortest encoding and the one
     * Cyclone accepts: a zero bitmapBase is rejected as malformed. */
    nb_rtps_sn_from_u64(&gap.base, last_sn + 1u);
    gap.num_bits = 0;
    message_len = nb_rtps_wire_gap(message,
                                   sizeof(message),
                                   &header,
                                   &timestamp,
                                   &reader_guid->prefix,
                                   &gap);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant), message, message_len, host, port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.gaps_sent++;

    return NB_OK;
}
static int send_user_acknack(nb_rtps_participant_t *participant,
                             const nb_rtps_guid_t *writer_guid,
                             const nb_rtps_guid_t *reader_guid,
                             const nb_rtps_acknack_t *acknack,
                             const char *host,
                             uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    uint64_t now;
    size_t message_len;

    if (!participant || !writer_guid || !reader_guid || !acknack ||
        !host || host[0] == '\0' || port == 0) {
        return NB_ERR_INVALID;
    }
    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    message_len = nb_rtps_wire_acknack(message,
                                       sizeof(message),
                                       &header,
                                       &timestamp,
                                       &writer_guid->prefix,
                                       acknack);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant), message, message_len, host, port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.acknacks_sent++;

    return NB_OK;
}
int send_user_heartbeat(nb_rtps_participant_t *participant,
                               nb_rtps_local_endpoint_t *local,
                               const nb_rtps_endpoint_info_t *remote,
                               uint64_t first_sn,
                               uint64_t last_sn,
                               const char *host,
                               uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_heartbeat_t heartbeat;
    uint64_t now;
    size_t message_len;

    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.reader_id = remote->endpoint_guid.entity;
    heartbeat.writer_id = local->info.endpoint_guid.entity;
    nb_rtps_sn_from_u64(&heartbeat.first_sn, first_sn);
    nb_rtps_sn_from_u64(&heartbeat.last_sn, last_sn);
    heartbeat.count = ++local->heartbeat_count;
    message_len = nb_rtps_wire_heartbeat(message,
                                         sizeof(message),
                                         &header,
                                         &timestamp,
                                         &remote->endpoint_guid.prefix,
                                         &heartbeat);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant), message, message_len, host, port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.heartbeats_sent++;

    return NB_OK;
}
/*
 * MANUAL_BY_TOPIC liveliness assertion. The DDS rule is that writing asserts
 * the writer, and RTPS carries that as a HEARTBEAT with the L flag (Cyclone DDS
 * does the same in ddsi_write_hb_liveliness). It is per writer, unlike
 * ParticipantMessageData, which renews every manual-by-participant writer of a
 * participant at once.
 */
void send_user_liveliness_heartbeat(nb_rtps_participant_t *participant,
                                           nb_rtps_local_endpoint_t *local,
                                           const nb_rtps_endpoint_info_t *remote,
                                           const char *host,
                                           uint16_t port)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_heartbeat_t heartbeat;
    uint64_t now;
    size_t message_len;

    init_header(participant, &header);
    now = zn_wall_ms();
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.reader_id = remote->endpoint_guid.entity;
    heartbeat.writer_id = local->info.endpoint_guid.entity;
    {
        uint64_t first_sn = 1;
        uint64_t last_sn = 1;

        if (local->history) {
            uint64_t history_first = 0;
            uint64_t history_last = 0;

            nb_rtps_reliable_state(local->history, &history_first, &history_last, NULL);
            /* Advertise the range the writer actually has. A best-effort
             * writer has no history and uses its own send counter; a reliable
             * one has always advanced next_sn, so last_sn is the newest
             * sequence number. */
            if (history_last != 0) {
                first_sn = history_first;
                last_sn = history_last;
            }
        } else if (local->data_sn != 0) {
            first_sn = local->data_sn;
            last_sn = local->data_sn;
        }
        nb_rtps_sn_from_u64(&heartbeat.first_sn, first_sn);
        nb_rtps_sn_from_u64(&heartbeat.last_sn, last_sn);
    }
    heartbeat.count = ++local->heartbeat_count;
    heartbeat.liveliness = 1;
    message_len = nb_rtps_wire_heartbeat(message,
                                         sizeof(message),
                                         &header,
                                         &timestamp,
                                         &remote->endpoint_guid.prefix,
                                         &heartbeat);
    if (message_len == 0) {
        return;
    }
    (void)nb_rtps_sendto(participant, participant_data_socket(participant), message, message_len, host, port);
    participant->stats.heartbeats_sent++;
}
/* HEARTBEAT timer for reliable application writers. */
/* Proactive sends go to every advertised address of the peer. A multi-homed
 * reader is then reachable whichever interface of it the sender can route to;
 * the copies that arrive twice are dropped by the reader's sequence window. */
int send_user_data_all(nb_rtps_participant_t *participant,
                              const nb_rtps_guid_t *writer_guid,
                              const nb_rtps_guid_t *reader_guid,
                              uint64_t sn,
                              uint64_t source_ns,
                              const void *payload,
                              size_t payload_len,
                              const uint8_t *handle,
                              uint32_t status,
                              const nb_rtps_targets_t *targets)
{
    size_t i;
    int sent = 0;
    int last = NB_ERR_IO;

    if (!targets) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < targets->count; i++) {
        int rc = send_user_data(participant,
                                writer_guid,
                                reader_guid,
                                sn,
                                source_ns,
                                payload,
                                payload_len,
                                handle,
                                status,
                                targets->host[i],
                                targets->port[i]);

        if (rc == NB_OK) {
            sent++;
        } else {
            last = rc;
        }
    }
    return sent > 0 ? NB_OK : last;
}
void send_user_gap_all(nb_rtps_participant_t *participant,
                              const nb_rtps_guid_t *writer_guid,
                              const nb_rtps_guid_t *reader_guid,
                              uint64_t gap_start,
                              uint64_t gap_last,
                              const nb_rtps_targets_t *targets)
{
    size_t i;

    if (!targets) {
        return;
    }
    for (i = 0; i < targets->count; i++) {
        (void)send_user_gap(participant,
                            writer_guid,
                            reader_guid,
                            gap_start,
                            gap_last,
                            targets->host[i],
                            targets->port[i]);
    }
}
void send_user_heartbeat_all(nb_rtps_participant_t *participant,
                                    nb_rtps_local_endpoint_t *local,
                                    const nb_rtps_endpoint_info_t *reader_info,
                                    uint64_t first_sn,
                                    uint64_t last_sn,
                                    const nb_rtps_targets_t *targets)
{
    size_t i;

    if (!targets) {
        return;
    }
    for (i = 0; i < targets->count; i++) {
        (void)send_user_heartbeat(participant,
                                  local,
                                  reader_info,
                                  first_sn,
                                  last_sn,
                                  targets->host[i],
                                  targets->port[i]);
    }
}
void send_data_heartbeats(nb_rtps_participant_t *participant)
{
    size_t i;
    size_t j;

    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->tables.max_local_endpoints; i++) {
        nb_rtps_local_endpoint_t *local = &participant->tables.local_endpoints[i];
        uint64_t first_sn = 0;
        uint64_t last_sn = 0;

        if (!local->used || local->kind != NB_RTPS_ENDPOINT_PUBLICATION ||
            !local->history) {
            continue;
        }
        nb_rtps_reliable_state(local->history, &first_sn, &last_sn, NULL);
        /* Advertise the history even when it is empty (firstSN = lastSN + 1)
         * or when everything in it has expired. The heartbeat is what makes a
         * reader ask; the writer answers a request for an evicted or expired
         * sample with GAP, which is how the reader converges instead of
         * retrying forever. */
        for (j = 0; j < participant->tables.max_remote_endpoints; j++) {
            nb_rtps_remote_endpoint_t *remote = &participant->tables.remote_endpoints[j];
            nb_rtps_targets_t targets;

            if (!remote->used || remote->kind != NB_RTPS_ENDPOINT_SUBSCRIPTION ||
                remote->info.reliability != NB_RTPS_RELIABILITY_RELIABLE ||
                !endpoint_qos_compatible(&local->info, &remote->info)) {
                continue;
            }
            if (endpoint_data_targets(participant, remote, &targets) == 0) {
                continue;
            }
            send_user_heartbeat_all(
                participant, local, &remote->info, first_sn, last_sn, &targets);
        }
    }
    zb_mutex_unlock(participant->net.lock);
}
/*
 * Ownership: an EXCLUSIVE subscription takes data from one writer only. Among
 * the matched publications the highest ownership strength wins, and equal
 * strengths are broken by GUID so that every participant in the domain picks
 * the same writer instead of flapping between equals (DDS leaves the tie
 * unspecified, but a deterministic choice is required for the reader to be
 * stable). A writer whose automatic lease has lapsed drops out of the running,
 * which is exactly what hands the instance to the next strongest writer when
 * the owner dies.
 */
static int guid_less(const nb_rtps_guid_t *a, const nb_rtps_guid_t *b)
{
    int cmp = memcmp(a->prefix.bytes, b->prefix.bytes, NB_RTPS_GUID_PREFIX_SIZE);

    if (cmp != 0) {
        return cmp < 0;
    }
    return memcmp(a->entity.bytes, b->entity.bytes, NB_RTPS_ENTITY_ID_SIZE) < 0;
}
/* A pair with no receive window yet has never been declared dead. */
static int writer_alive_for(nb_rtps_participant_t *participant,
                            const nb_rtps_local_endpoint_t *local,
                            const nb_rtps_remote_endpoint_t *remote)
{
    nb_rtps_data_reader_state_t *state = find_data_reader(
        participant, &remote->info.endpoint_guid, &local->info.endpoint_guid.entity);

    return state ? state->alive : 1;
}
static int writer_owns(nb_rtps_participant_t *participant,
                       const nb_rtps_local_endpoint_t *local,
                       const nb_rtps_remote_endpoint_t *candidate)
{
    size_t i;

    if (!writer_alive_for(participant, local, candidate)) {
        return 0;
    }
    for (i = 0; i < participant->tables.max_remote_endpoints; i++) {
        const nb_rtps_remote_endpoint_t *other = &participant->tables.remote_endpoints[i];

        if (!other->used || other == candidate ||
            other->kind != NB_RTPS_ENDPOINT_PUBLICATION ||
            !data_endpoint_matches(local, other) ||
            !writer_alive_for(participant, local, other)) {
            continue;
        }
        if (other->info.ownership_strength > candidate->info.ownership_strength ||
            (other->info.ownership_strength == candidate->info.ownership_strength &&
             guid_less(&other->info.endpoint_guid, &candidate->info.endpoint_guid))) {
            return 0;
        }
    }
    return 1;
}
/* Reader-side instance table: entries appear with the first sample that carries
 * an instance handle, so a subscription to a non-keyed type allocates nothing. */
static nb_rtps_instance_t *find_instance(nb_rtps_local_endpoint_t *local,
                                         const uint8_t *handle)
{
    uint32_t i;

    if (!local || !local->instances) {
        return NULL;
    }
    for (i = 0; i < local->instance_count; i++) {
        nb_rtps_instance_t *instance = &local->instances[i];

        if (instance->used &&
            memcmp(instance->handle, handle, NB_INSTANCE_HANDLE_SIZE) == 0) {
            return instance;
        }
    }
    return NULL;
}
/*
 * Look an instance up, creating the table lazily. NULL means the subscription
 * cannot hold another instance; the caller counts the sample as rejected
 * instead of dropping it silently.
 */
nb_rtps_instance_t *get_instance(nb_rtps_local_endpoint_t *local,
                                        const uint8_t *handle,
                                        const nb_rtps_guid_t *writer)
{
    nb_rtps_instance_t *instance = find_instance(local, handle);
    uint32_t capacity;

    if (instance) {
        instance->writer = *writer;
        return instance;
    }
    capacity = local->max_instances != 0 ? local->max_instances
                                         : NB_RTPS_DEFAULT_MAX_INSTANCES;
    if (capacity > NB_RTPS_MAX_INSTANCES) {
        capacity = NB_RTPS_MAX_INSTANCES;
    }
    if (!local->instances) {
        local->instances =
            (nb_rtps_instance_t *)calloc(capacity, sizeof(*local->instances));
        if (!local->instances) {
            return NULL;
        }
        local->max_instances = capacity;
        local->instance_count = 0;
    }
    if (local->instance_count >= local->max_instances) {
        return NULL;
    }
    instance = &local->instances[local->instance_count++];
    instance->used = 1;
    memcpy(instance->handle, handle, NB_INSTANCE_HANDLE_SIZE);
    instance->state = NB_INSTANCE_STATE_ALIVE;
    instance->writer = *writer;
    return instance;
}
/* A writer that goes away takes its instances to NO_WRITERS, which is the state
 * the next SampleInfo for them reports. */
void mark_instances_no_writers(nb_rtps_local_endpoint_t *local,
                                      const nb_rtps_guid_t *writer)
{
    uint32_t i;

    if (!local || !local->instances) {
        return;
    }
    for (i = 0; i < local->instance_count; i++) {
        nb_rtps_instance_t *instance = &local->instances[i];

        if (instance->used && nb_rtps_guid_equal(&instance->writer, writer)) {
            instance->state = NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS;
        }
    }
}
static void queue_status_event(nb_rtps_queued_status_t *events,
                               size_t *event_count,
                               size_t capacity,
                               nb_rtps_status_kind_t kind,
                               const nb_rtps_guid_t *endpoint,
                               uint32_t count,
                               const char *reason)
{
    if (*event_count >= capacity) {
        return;
    }
    events[*event_count].kind = kind;
    events[*event_count].endpoint = *endpoint;
    events[*event_count].count = count;
    events[*event_count].reason = reason;
    *event_count += 1;
}
static void emit_queued_status(nb_rtps_participant_t *participant,
                               const nb_rtps_queued_status_t *queued)
{
    nb_rtps_status_event_t event;
    nb_rtps_status_callback_t callback;
    void *user;

    zb_mutex_lock(participant->net.lock);
    callback = participant->tables.status_callback;
    user = participant->tables.status_user;
    zb_mutex_unlock(participant->net.lock);
    if (!callback) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.kind = queued->kind;
    event.endpoint = queued->endpoint;
    event.count = queued->count;
    if (queued->reason) {
        copy_str(event.reason, sizeof(event.reason), queued->reason);
    }
    callback(user, &event);
}
/* Build the SampleInfo for an incoming DATA sample: disposition from the DATA
 * submessage flags and the inline PID_STATUS_INFO, plus the key hash (which
 * decides instance tracking) and the source timestamp. */
static void derive_sample_info(const nb_rtps_data_view_t *data,
                               nb_sample_info_t *sample,
                               uint8_t instance_handle[NB_INSTANCE_HANDLE_SIZE],
                               int *has_instance_handle)
{
    uint8_t key_hash[NB_RTPS_KEY_HASH_SIZE];
    int disposed = 0;
    int unregistered = 0;
    int has_key_hash = 0;

    memset(sample, 0, sizeof(*sample));
    sample->reception_timestamp_ns = (int64_t)(zn_wall_ms() * 1000000ull);
    /* Dispose and unregister reach the application as a sample that is not
     * alive, the way DDS SampleInfo reports them. The writers that can express
     * it do so either with the DATA submessage flags (key-only / dispose) or
     * with PID_STATUS_INFO in the inline QoS; both are accepted. */
    (void)nb_rtps_inline_qos_status(data->inline_qos,
                                    data->inline_qos_len,
                                    &disposed,
                                    &unregistered,
                                    key_hash,
                                    &has_key_hash);
    /* PID_STATUS_INFO is authoritative when the peer sent it: a key-only
     * sample alone cannot say whether the writer disposed the instance or just
     * unregistered, and the DATA decoder has to report the former for peers
     * that omit the status bits. */
    if (unregistered) {
        sample->state = NB_SAMPLE_STATE_NO_WRITERS;
    } else if (data->dispose || disposed) {
        sample->state = NB_SAMPLE_STATE_DISPOSED;
    } else if (data->key) {
        /* KEY set without PID_STATUS_INFO: the payload is a key, so the sample
         * reports a state change rather than a new value. */
        sample->state = NB_SAMPLE_STATE_DISPOSED;
    } else {
        sample->state = NB_SAMPLE_STATE_VALID;
    }
    if (has_key_hash) {
        sample->has_instance_handle = true;
        memcpy(sample->instance_handle, key_hash, sizeof(key_hash));
        memcpy(instance_handle, key_hash, sizeof(key_hash));
        *has_instance_handle = 1;
    }
    if (data->has_source_ts) {
        sample->source_timestamp_ns = (int64_t)rtps_time_to_ns(&data->source_ts);
    }
}

/* Record a sample one local endpoint refused: count it for status and queue a
 * SAMPLE_REJECTED event attributed to that endpoint. */
static void reject_sample(nb_rtps_participant_t *participant,
                          nb_rtps_local_endpoint_t *local,
                          nb_rtps_queued_status_t *queued,
                          size_t *queued_count,
                          const char *reason)
{
    local->sample_rejected_count++;
    participant->stats.samples_dropped++;
    queue_status_event(queued,
                       queued_count,
                       NB_RTPS_MAX_DATA_TARGETS,
                       NB_RTPS_STATUS_SAMPLE_REJECTED,
                       &local->info.endpoint_guid,
                       local->sample_rejected_count,
                       reason);
}

/* Apply DESTINATION_ORDER (BY_SOURCE) and TIME_BASED_FILTER to one candidate
 * sample for one local endpoint. Returns a rejection reason, or NULL when the
 * sample may be delivered.
 *
 * BY_SOURCE_TIMESTAMP: a sample stamped at or before the last delivered one
 * arrives out of order and is discarded, which is what Cyclone does for its
 * reader history. Without a timestamp the sample keeps reception order;
 * matching still requires both ends to announce the same policy, so this only
 * affects peers that asked for source ordering.
 *
 * TIME_BASED_FILTER: a sample stamped less than the requested minimum
 * separation after the last delivered one is dropped, the same test Cyclone
 * applies on insertion into its reader history.
 *
 * Both policies only advance their timestamp when a sample is actually
 * delivered, so a sample rejected by either does not make the next one look
 * too early. */
static const char *time_filter_reject(nb_rtps_local_endpoint_t *local,
                                      const nb_rtps_data_view_t *data,
                                      nb_sample_state_t state)
{
    uint64_t source_ns;

    if (!data->has_source_ts) {
        return NULL;
    }
    source_ns = (uint64_t)rtps_time_to_ns(&data->source_ts);

    if (local->info.destination_order == NB_RTPS_DESTINATION_ORDER_BY_SOURCE &&
        local->has_last_source && source_ns <= local->last_source_ns) {
        return "destination order";
    }
    if (local->info.min_separation_ms > 0 &&
        state == NB_SAMPLE_STATE_VALID &&
        local->has_last_filtered &&
        source_ns <
            local->last_filtered_ns +
                (uint64_t)local->info.min_separation_ms * 1000000ull) {
        return "time based filter";
    }
    if (local->info.destination_order == NB_RTPS_DESTINATION_ORDER_BY_SOURCE) {
        local->has_last_source = 1;
        local->last_source_ns = source_ns;
    }
    if (local->info.min_separation_ms > 0) {
        local->has_last_filtered = 1;
        local->last_filtered_ns = source_ns;
    }
    return NULL;
}

/* Fill the per-instance parts of SampleInfo for one local endpoint: the
 * instance keeps its generation rank while it stays alive and gets a new one
 * when a writer registers it again, and the view is NEW only for the sample
 * that brings a fresh instance to the reader. Returns 0 when the endpoint has
 * no room for another instance. */
static int fill_instance_sample_info(nb_rtps_local_endpoint_t *local,
                                     const nb_rtps_guid_t *writer_guid,
                                     const uint8_t *instance_handle,
                                     int has_instance_handle,
                                     nb_sample_info_t *sample)
{
    nb_rtps_instance_t *instance;

    if (!has_instance_handle) {
        return 1;
    }
    instance = get_instance(local, instance_handle, writer_guid);
    if (!instance) {
        return 0;
    }
    if (sample->state == NB_SAMPLE_STATE_VALID) {
        /* NEW means "the reader has not seen this instance, or the instance was
         * re-registered since it last saw it". */
        int fresh = !instance->seen ||
                    instance->state != NB_INSTANCE_STATE_ALIVE;

        if (instance->state != NB_INSTANCE_STATE_ALIVE) {
            instance->generation_rank++;
        }
        sample->view_state = fresh ? NB_VIEW_STATE_NEW : NB_VIEW_STATE_NOT_NEW;
        instance->state = NB_INSTANCE_STATE_ALIVE;
        instance->seen = 1;
    } else if (sample->state == NB_SAMPLE_STATE_DISPOSED) {
        sample->view_state = NB_VIEW_STATE_NOT_NEW;
        instance->state = NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED;
    } else {
        sample->view_state = NB_VIEW_STATE_NOT_NEW;
        instance->state = NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS;
    }
    sample->instance_state = instance->state;
    sample->generation_rank = (int32_t)instance->generation_rank;
    return 1;
}

/* Hand the accepted samples to the data callback. A sample whose state is not
 * VALID carries only metadata (dispose/unregister); a dds::Message payload is
 * deserialized once and reused, any other type is handed over as its XCDR1
 * body so the application can run its own codec. */
static void deliver_samples(nb_rtps_data_callback_t callback,
                            void *user,
                            const nb_rtps_guid_t *writer_guid,
                            uint64_t sample_sn,
                            const nb_rtps_data_view_t *data,
                            const nb_rtps_guid_t *readers,
                            const uint8_t *dds_reader,
                            const nb_sample_info_t *samples,
                            size_t reader_count)
{
    nb_rtps_dds_message_t message;
    int dds_parsed = 0;
    int dds_ok = 0;
    size_t i;

    if (!callback) {
        return;
    }
    for (i = 0; i < reader_count; i++) {
        nb_rtps_dds_message_t view;
        nb_sample_info_t delivered = samples[i];

        /* A state-change notification has no payload: hand the application the
         * metadata only, so it can react to the instance rather than to data. */
        if (delivered.state != NB_SAMPLE_STATE_VALID) {
            memset(&view, 0, sizeof(view));
            view.sample = delivered;
            view.writer_sn = sample_sn;
            callback(user, &readers[i], writer_guid, &view);
            continue;
        }
        if (dds_reader[i]) {
            if (!dds_parsed) {
                dds_parsed = 1;
                dds_ok = nb_rtps_dds_message_deserialize(
                             data->payload, data->payload_len, &message) == NB_OK;
            }
            if (!dds_ok) {
                continue;
            }
            view = message;
            view.sample = delivered;
        } else {
            /* Any other type travels as an XCDR1 encapsulated sample. Hand the
             * application the CDR body; it owns the codec for its own type. */
            if (data->payload_len <= NB_RTPS_CDR_HEADER_SIZE ||
                data->payload[0] != 0x00 ||
                (data->payload[1] != 0x01 && data->payload[1] != 0x03)) {
                continue;
            }
            view.payload = data->payload + NB_RTPS_CDR_HEADER_SIZE;
            view.payload_len = data->payload_len - NB_RTPS_CDR_HEADER_SIZE;
            view.sample = delivered;
        }
        view.writer_sn = sample_sn;
        callback(user, &readers[i], writer_guid, &view);
    }
}

static void handle_user_data(nb_rtps_participant_t *participant,
                             const nb_rtps_data_view_t *data,
                             const nb_rtps_guid_prefix_t *source_prefix)
{
    nb_rtps_guid_t writer_guid;
    nb_rtps_guid_t readers[NB_RTPS_MAX_DATA_TARGETS];
    nb_sample_info_t samples[NB_RTPS_MAX_DATA_TARGETS];
    nb_rtps_queued_status_t queued[NB_RTPS_MAX_DATA_TARGETS];
    uint8_t dds_reader[NB_RTPS_MAX_DATA_TARGETS];
    nb_rtps_data_callback_t callback;
    void *user;
    nb_rtps_remote_endpoint_t *remote;
    nb_sample_info_t sample;
    uint8_t instance_handle[NB_INSTANCE_HANDLE_SIZE];
    size_t reader_count = 0;
    size_t queued_count = 0;
    int has_instance_handle = 0;
    uint64_t sample_sn;
    size_t i;

    if (!participant || !data || !source_prefix) {
        return;
    }
    sample_sn = nb_rtps_sn_to_u64(&data->writer_sn);
    if (sample_sn == 0) {
        return;
    }
    writer_guid.prefix = *source_prefix;
    writer_guid.entity = data->writer_id;

    derive_sample_info(data, &sample, instance_handle, &has_instance_handle);

    zb_mutex_lock(participant->net.lock);
    callback = participant->tables.data_callback;
    user = participant->tables.data_user;
    remote = find_remote_endpoint(participant, &writer_guid);
    if (remote) {
        for (i = 0; i < participant->tables.local_used_count; i++) {
            nb_rtps_local_endpoint_t *local =
                &participant->tables.local_endpoints[participant->tables.local_used[i]];
            nb_rtps_data_reader_state_t *reader_state;
            const char *reject;

            if (!local->used ||
                !data_endpoint_matches(local, remote) ||
                (!nb_rtps_entity_id_equal(&data->reader_id, &nb_rtps_eid_unknown) &&
                 !nb_rtps_entity_id_equal(&data->reader_id,
                                          &local->info.endpoint_guid.entity))) {
                continue;
            }
            if (reader_count == NB_RTPS_MAX_DATA_TARGETS) {
                break;
            }
            /* Every pair is deduplicated by writer sequence number. A duplicate
             * DATA submessage, a retransmit that was already handled, or the
             * same sample sent to several of our locators must not reach the
             * application twice. The window advances even without a callback so
             * a reliable reader does not request the same samples forever.
             *
             * A window is normally opened by the first sample that arrives, but
             * starting it at that sample's sequence number loses both a
             * reordered older sample and the retained history a durable
             * subscription is entitled to. Durable readers therefore start at
             * the beginning of the sequence space and let the writer GAP what
             * it no longer has; VOLATILE readers start at the sample. */
            reader_state = get_data_reader(
                participant,
                &writer_guid,
                &local->info.endpoint_guid.entity,
                local->info.durability >= NB_RTPS_DURABILITY_TRANSIENT_LOCAL
                    ? 1u
                    : sample_sn);
            if (reader_state &&
                !nb_rtps_reader_on_sample(&reader_state->state, sample_sn)) {
                continue;
            }
            if (reader_state) {
                uint64_t sample_now = zn_wall_ms();

                reader_state->last_sample_ms = sample_now;
                /* A write asserts the writer's liveliness (DDS 2.2.2.4.2.10),
                 * which is what keeps a manual-by-topic writer alive. */
                reader_state->last_liveliness_ms = sample_now;
            }
            /* An EXCLUSIVE subscription ignores every writer but the owner. The
             * sequence number is still consumed above, so a reliable reader does
             * not keep asking for a sample it will never be given. */
            if (local->info.ownership == NB_RTPS_OWNERSHIP_EXCLUSIVE &&
                !writer_owns(participant, local, remote)) {
                reject_sample(participant, local, queued, &queued_count,
                              "ownership");
                continue;
            }
            reject = time_filter_reject(local, data, sample.state);
            if (reject) {
                reject_sample(participant, local, queued, &queued_count, reject);
                continue;
            }
            samples[reader_count] = sample;
            if (!fill_instance_sample_info(local, &writer_guid, instance_handle,
                                           has_instance_handle,
                                           &samples[reader_count])) {
                reject_sample(participant, local, queued, &queued_count,
                              "resource limits");
                continue;
            }
            readers[reader_count] = local->info.endpoint_guid;
            dds_reader[reader_count] =
                strncmp(local->info.type_name,
                        NB_RTPS_DDS_MESSAGE_TYPE,
                        sizeof(local->info.type_name)) == 0
                    ? 1u
                    : 0u;
            reader_count++;
        }
    }
    zb_mutex_unlock(participant->net.lock);

    for (i = 0; i < queued_count; i++) {
        emit_queued_status(participant, &queued[i]);
    }
    deliver_samples(callback, user, &writer_guid, sample_sn, data,
                    readers, dds_reader, samples, reader_count);
}
/*
 * A GAP tells the reader that some writer sequence numbers will never arrive.
 * Advancing the receive window prevents an endless ACKNACK loop for samples
 * that were evicted from the writer history.
 */
void handle_user_gap(nb_rtps_participant_t *participant,
                            const nb_rtps_gap_t *gap,
                            const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_guid_prefix_t *source_prefix = rx->prefix;
    nb_rtps_guid_t writer_guid;
    nb_rtps_remote_endpoint_t *remote;
    nb_rtps_queued_status_t queued[NB_RTPS_MAX_DATA_TARGETS];
    size_t queued_count = 0;
    uint32_t lost = 0;
    uint64_t gap_start;
    size_t i;

    if (!participant || !gap || !source_prefix) {
        return;
    }
    gap_start = nb_rtps_sn_to_u64(&gap->gap_start);
    if (gap_start != 0) {
        /* gap_start plus every bit the gap lists is a sample the writer
         * withdrew: the reader knows it will never arrive. */
        lost = 1;
        for (i = 0; i < gap->num_bits; i++) {
            if ((gap->bitmap[i / 32u] & (1u << (31u - (i % 32u)))) != 0) {
                lost++;
            }
        }
    }
    writer_guid.prefix = *source_prefix;
    writer_guid.entity = gap->writer_id;

    zb_mutex_lock(participant->net.lock);
    remote = find_remote_endpoint(participant, &writer_guid);
    if (remote) {
        for (i = 0; i < participant->tables.max_local_endpoints; i++) {
            nb_rtps_local_endpoint_t *local = &participant->tables.local_endpoints[i];
            nb_rtps_data_reader_state_t *state;

            if (!local->used || !data_endpoint_matches(local, remote)) {
                continue;
            }
            if (local->info.reliability != NB_RTPS_RELIABILITY_RELIABLE) {
                continue;
            }
            if (!nb_rtps_entity_id_equal(&gap->reader_id, &nb_rtps_eid_unknown) &&
                !nb_rtps_entity_id_equal(&gap->reader_id,
                                         &local->info.endpoint_guid.entity)) {
                continue;
            }
            state = get_data_reader(
                participant,
                &writer_guid,
                &local->info.endpoint_guid.entity,
                gap_start);
            if (state) {
                nb_rtps_reader_on_gap(&state->state, gap);
            }
            if (lost > 0) {
                local->sample_lost_count += lost;
                queue_status_event(queued,
                                   &queued_count,
                                   NB_RTPS_MAX_DATA_TARGETS,
                                   NB_RTPS_STATUS_SAMPLE_LOST,
                                   &local->info.endpoint_guid,
                                   local->sample_lost_count,
                                   NULL);
            }
        }
    }
    zb_mutex_unlock(participant->net.lock);
    for (i = 0; i < queued_count; i++) {
        emit_queued_status(participant, &queued[i]);
    }
}
/* A partial fragment assembly proves the sample was already arriving after the
 * reader matched. A HEARTBEAT can race the last fragment of a large sample, so
 * the window opened from that HEARTBEAT must not start past it. */
static uint64_t oldest_inflight_sn(nb_rtps_participant_t *participant,
                                   const nb_rtps_guid_t *writer,
                                   uint64_t last_sn)
{
    uint64_t oldest = last_sn + 1u;
    size_t i;

    for (i = 0; i < participant->tables.max_frag_assemblies; i++) {
        const nb_rtps_frag_assembly_t *assembly =
            &participant->tables.frag_assemblies[i];

        if (assembly->used && !assembly->complete && assembly->sn < oldest &&
            nb_rtps_guid_equal(&assembly->writer, writer)) {
            oldest = assembly->sn;
        }
    }
    return oldest;
}
/* Answer a writer HEARTBEAT with the set of samples still missing. */
void handle_user_heartbeat(nb_rtps_participant_t *participant,
                                  const nb_rtps_heartbeat_t *heartbeat,
                                  const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_guid_prefix_t *source_prefix = rx->prefix;
    const char *source_host = rx->host;
    uint16_t source_port = rx->port;
    nb_rtps_guid_t writer_guid;
    nb_rtps_remote_endpoint_t *remote;
    char target_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t target_port = source_port;
    uint64_t first_sn;
    uint64_t last_sn;
    uint64_t now;
    size_t i;

    if (!participant || !heartbeat || !source_prefix) {
        return;
    }
    writer_guid.prefix = *source_prefix;
    writer_guid.entity = heartbeat->writer_id;
    first_sn = nb_rtps_sn_to_u64(&heartbeat->first_sn);
    if (first_sn == 0) {
        first_sn = 1;
    }
    last_sn = nb_rtps_sn_to_u64(&heartbeat->last_sn);
    if (last_sn == 0) {
        return;
    }
    copy_str(target_host, sizeof(target_host), source_host);

    zb_mutex_lock(participant->net.lock);
    remote = find_remote_endpoint(participant, &writer_guid);
    if (remote) {
        if (endpoint_data_target(participant, remote, target_host,
                                 sizeof(target_host), &target_port) != NB_OK) {
            copy_str(target_host, sizeof(target_host), source_host);
            target_port = source_port;
        }
        for (i = 0; i < participant->tables.local_used_count; i++) {
            nb_rtps_local_endpoint_t *local =
                &participant->tables.local_endpoints[participant->tables.local_used[i]];
            nb_rtps_data_reader_state_t *state;
            nb_rtps_acknack_t acknack;

            if (!local->used || !data_endpoint_matches(local, remote)) {
                continue;
            }
            if (local->info.reliability != NB_RTPS_RELIABILITY_RELIABLE) {
                continue;
            }
            if (!nb_rtps_entity_id_equal(&heartbeat->reader_id, &nb_rtps_eid_unknown) &&
                !nb_rtps_entity_id_equal(&heartbeat->reader_id,
                                         &local->info.endpoint_guid.entity)) {
                continue;
            }
            /* A durable reader joins the writer's history at its oldest
             * retained sample; a VOLATILE reader must not pull samples the
             * writer produced before the match, so its window starts just
             * past the heartbeat's last sequence number (RTPS 2.5 8.4.12 and
             * the DDS durability contract). A sample that is already being
             * reassembled is the exception: its fragments only arrived after
             * the match, and the HEARTBEAT may have overtaken the last one. */
            {
                uint64_t start = 1u;

                if (local->info.durability < NB_RTPS_DURABILITY_TRANSIENT_LOCAL &&
                    last_sn != 0 && last_sn != UINT32_MAX) {
                    uint64_t inflight =
                        oldest_inflight_sn(participant, &writer_guid, last_sn);

                    start = inflight < last_sn + 1u ? inflight : last_sn + 1u;
                }
                state = get_data_reader(participant,
                                        &writer_guid,
                                        &local->info.endpoint_guid.entity,
                                        start);
            }
            if (!state) {
                continue;
            }
            /* A heartbeat with the L flag asserts the writer's liveliness, the
             * manual-by-topic equivalent of a ParticipantMessageData sample. */
            if (heartbeat->liveliness) {
                state->last_liveliness_ms = zn_wall_ms();
            }
            if (first_sn > state->state.next_expected) {
                nb_rtps_reader_state_reset(&state->state, first_sn);
            }
            /* Repeated heartbeats with the same count carry no new
             * information; do not answer them again. */
            if (state->heartbeat_seen &&
                state->last_heartbeat_count == heartbeat->count) {
                continue;
            }
            state->heartbeat_seen = 1;
            state->last_heartbeat_count = heartbeat->count;
            now = zn_wall_ms();
            if (state->last_acknack_ms != 0 && now >= state->last_acknack_ms &&
                now - state->last_acknack_ms < NB_RTPS_MIN_ACKNACK_INTERVAL_MS) {
                continue;
            }
            {
                uint32_t missing = nb_rtps_reader_build_acknack(
                    &state->state, last_sn, &acknack);

                /* An ACKNACK is only useful when it asks for something or the
                 * writer explicitly requested an answer (FINAL clear). A
                 * heartbeat that carries no news would otherwise cost one
                 * datagram per message on both sides. */
                if (missing == 0 && !heartbeat->ack_requested) {
                    continue;
                }
            }
            acknack.reader_id = local->info.endpoint_guid.entity;
            acknack.writer_id = heartbeat->writer_id;
            if (send_user_acknack(participant,
                                  &writer_guid,
                                  &local->info.endpoint_guid,
                                  &acknack,
                                  target_host,
                                  target_port) == NB_OK) {
                state->last_acknack_ms = now;
            }
        }
    }
    zb_mutex_unlock(participant->net.lock);
}
/*
 * LIFESPAN: a sample older than the writer's lifespan must not be delivered
 * again, even if it is still in the history. Retained samples therefore expire
 * before history eviction would drop them.
 */
int sample_expired(const nb_rtps_local_endpoint_t *local, uint64_t sn, uint64_t now)
{
    uint64_t written_ms = 0;

    if (!local || !local->history || local->info.lifespan_ms == 0) {
        return 0;
    }
    if (nb_rtps_reliable_sample_ms(local->history, sn, &written_ms) != NB_OK) {
        return 0;
    }
    return now >= written_ms && now - written_ms > local->info.lifespan_ms;
}
/*
 * A reliable reader asks for specific samples. Resume from the writer history
 * when possible; samples that were already evicted, or that outlived their
 * lifespan, are announced with GAP.
 */
void handle_user_acknack(nb_rtps_participant_t *participant,
                                const nb_rtps_acknack_t *acknack,
                                const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_guid_prefix_t *source_prefix = rx->prefix;
    const char *source_host = rx->host;
    uint16_t source_port = rx->port;
    nb_rtps_guid_t writer_guid;
    nb_rtps_guid_t reader_guid;
    nb_rtps_local_endpoint_t *local;
    nb_rtps_remote_endpoint_t *remote;
    char target_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t target_port = source_port;
    uint64_t first_sn = 0;
    uint64_t last_sn = 0;
    size_t sample_count = 0;
    size_t resent = 0;
    size_t i;
    uint64_t now;
    uint64_t gap_start = 0;
    uint64_t gap_last = 0;
    uint64_t ack_base;

    if (!participant || !acknack || !source_prefix ||
        acknack->num_bits > NB_RTPS_ACKNACK_MAX_BITS) {
        return;
    }
    ack_base = nb_rtps_sn_to_u64(&acknack->base);
    if (ack_base == 0) {
        return;
    }
    writer_guid.prefix = participant->local_guid.prefix;
    writer_guid.entity = acknack->writer_id;
    reader_guid.prefix = *source_prefix;
    reader_guid.entity = acknack->reader_id;
    copy_str(target_host, sizeof(target_host), source_host);

    /* ParticipantMessageData keeps no history: an assertion that is lost is
     * replaced by the next one. Answering a retransmit request with GAP stops a
     * reliable peer's reader window from waiting for it. */
    if (nb_rtps_entity_id_equal(&acknack->writer_id,
                                &nb_rtps_eid_participant_message_writer)) {
        uint64_t last;

        zb_mutex_lock(participant->net.lock);
        last = participant->timers.pmd_sn;
        zb_mutex_unlock(participant->net.lock);
        if (last < ack_base) {
            last = ack_base;
        }
        (void)send_user_gap(participant,
                            &writer_guid,
                            &reader_guid,
                            ack_base,
                            last,
                            source_host,
                            source_port);
        return;
    }
    zb_mutex_lock(participant->net.lock);
    local = find_local_endpoint(participant, &writer_guid);
    remote = find_remote_endpoint(participant, &reader_guid);
    if (!local || local->kind != NB_RTPS_ENDPOINT_PUBLICATION || !local->history) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    if (remote &&
        endpoint_data_target(participant, remote, target_host,
                             sizeof(target_host), &target_port) != NB_OK) {
        copy_str(target_host, sizeof(target_host), source_host);
        target_port = source_port;
    }
    /* Bound the work a single peer can trigger. A reader that repeats an
     * ACKNACK, or one that floods them, gets at most one resend burst per
     * interval; the periodic HEARTBEAT brings the remaining samples back. */
    now = zn_wall_ms();
    if (local->resend_seen &&
        nb_rtps_guid_equal(&local->last_resend_reader, &reader_guid) &&
        now >= local->last_resend_ms &&
        now - local->last_resend_ms < NB_RTPS_MIN_RESEND_INTERVAL_MS) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    local->resend_seen = 1;
    local->last_resend_reader = reader_guid;
    local->last_resend_ms = now;
    nb_rtps_reliable_state(local->history, &first_sn, &last_sn, &sample_count);
    for (i = 0; i < acknack->num_bits; i++) {
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        int dispose = 0;
        uint8_t handle[NB_INSTANCE_HANDLE_SIZE];
        int has_handle = 0;
        uint32_t status = 0;
        uint64_t source_ns = 0;
        uint64_t sn;
        int rc;

        if ((acknack->bitmap[i / 32u] & (1u << (31u - (i % 32u)))) == 0) {
            continue;
        }
        sn = ack_base + i;
        rc = nb_rtps_reliable_get_instance(local->history,
                                           sn,
                                           &payload,
                                           &payload_len,
                                           &dispose,
                                           handle,
                                           &has_handle,
                                           &status);
        /* An expired sample is gone as far as the reader is concerned: tell it
         * so with GAP instead of sending data past its lifespan. */
        if (rc == NB_OK && sample_expired(local, sn, now)) {
            if (!gap_start) {
                gap_start = sn;
            }
            gap_last = sn;
        } else if (rc == NB_OK && payload && payload_len > 0) {
            if (resent >= NB_RTPS_MAX_RESEND_PER_ACK) {
                break;
            }
            resent++;
            (void)nb_rtps_reliable_source_ns(local->history, sn, &source_ns);
            (void)send_user_data(participant,
                                 &local->info.endpoint_guid,
                                 &reader_guid,
                                 sn,
                                 source_ns,
                                 payload,
                                 payload_len,
                                 has_handle ? handle : NULL,
                                 status,
                                 target_host,
                                 target_port);
        } else if (sample_count > 0 && sn < first_sn) {
            if (!gap_start) {
                gap_start = sn;
            }
            gap_last = sn;
        }
    }
    if (gap_start != 0 && gap_last >= gap_start) {
        (void)send_user_gap(participant,
                            &local->info.endpoint_guid,
                            &reader_guid,
                            gap_start,
                            gap_last,
                            target_host,
                            target_port);
    }
    zb_mutex_unlock(participant->net.lock);
}
/*
 * Writer side of fragment repair: a reader that lost a DATA_FRAG names the
 * fragments it wants, so only those are resent instead of the whole sample.
 * The bitmap is MSB-first and numbers fragments from bitmap_base.
 */
void handle_user_nack_frag(nb_rtps_participant_t *participant,
                                  const nb_rtps_nack_frag_t *nack,
                                  const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_guid_prefix_t *source_prefix = rx->prefix;
    const char *source_host = rx->host;
    uint16_t source_port = rx->port;
    nb_rtps_guid_t writer_guid;
    nb_rtps_guid_t reader_guid;
    nb_rtps_local_endpoint_t *local;
    nb_rtps_remote_endpoint_t *remote;
    char target_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t target_port = source_port;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t handle[NB_INSTANCE_HANDLE_SIZE];
    int has_handle = 0;
    uint32_t status = 0;
    uint32_t fragment_count;
    uint32_t i;
    uint64_t now;
    uint64_t source_ns = 0;
    int dispose = 0;
    uint64_t writer_sn;

    if (!participant || !nack || !source_prefix ||
        nack->bitmap_base == 0 || nack->num_bits == 0 ||
        nack->num_bits > NB_RTPS_FRAG_NUMBER_SET_MAX_BITS) {
        return;
    }
    writer_sn = nb_rtps_sn_to_u64(&nack->writer_sn);
    if (writer_sn == 0) {
        return;
    }
    writer_guid.prefix = participant->local_guid.prefix;
    writer_guid.entity = nack->writer_id;
    reader_guid.prefix = *source_prefix;
    reader_guid.entity = nack->reader_id;
    copy_str(target_host, sizeof(target_host), source_host);

    zb_mutex_lock(participant->net.lock);
    local = find_local_endpoint(participant, &writer_guid);
    if (!local || local->kind != NB_RTPS_ENDPOINT_PUBLICATION || !local->history) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    remote = find_remote_endpoint(participant, &reader_guid);
    if (remote &&
        endpoint_data_target(
            participant, remote, target_host, sizeof(target_host), &target_port) != NB_OK) {
        copy_str(target_host, sizeof(target_host), source_host);
        target_port = source_port;
    }
    /* Same budget as ACKNACK: one repair per peer per interval, so a reader
     * that repeats NACK_FRAG cannot turn a repair into a storm. */
    now = zn_wall_ms();
    if (local->resend_seen && nb_rtps_guid_equal(&local->last_resend_reader, &reader_guid) &&
        now >= local->last_resend_ms &&
        now - local->last_resend_ms < NB_RTPS_MIN_RESEND_INTERVAL_MS) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    local->resend_seen = 1;
    local->last_resend_reader = reader_guid;
    local->last_resend_ms = now;

    if (nb_rtps_reliable_get_instance(local->history,
                                      writer_sn,
                                      &payload,
                                      &payload_len,
                                      &dispose,
                                      handle,
                                      &has_handle,
                                      &status) != NB_OK ||
        !payload || payload_len == 0) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    (void)nb_rtps_reliable_source_ns(
        local->history, writer_sn, &source_ns);
    if (sample_expired(local, writer_sn, now)) {
        (void)send_user_gap(participant,
                            &local->info.endpoint_guid,
                            &reader_guid,
                            writer_sn,
                            writer_sn,
                            target_host,
                            target_port);
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    fragment_count = nb_rtps_fragment_count(payload_len);
    for (i = 0; i < nack->num_bits; i++) {
        uint32_t fragment_num;

        if ((nack->bitmap[i / 32u] & (1u << (31u - (i % 32u)))) == 0) {
            continue;
        }
        if (nack->bitmap_base > UINT32_MAX - i) {
            break;
        }
        fragment_num = nack->bitmap_base + i;
        if (fragment_num == 0 || fragment_num > fragment_count) {
            continue;
        }
        (void)send_user_fragment(participant,
                                 &local->info.endpoint_guid,
                                 &reader_guid,
                                 writer_sn,
                                 source_ns,
                                 payload,
                                 payload_len,
                                 fragment_num,
                                 has_handle ? handle : NULL,
                                 status,
                                 target_host,
                                 target_port);
    }
    /* Remind the reader what the sample looks like, so a still-incomplete
     * assembly asks again instead of waiting for its timeout. */
    (void)send_user_heartbeat_frag(participant,
                                   &local->info.endpoint_guid,
                                   &reader_guid,
                                   writer_sn,
                                   fragment_count,
                                   target_host,
                                   target_port);
    zb_mutex_unlock(participant->net.lock);
}
/*
 * Route one complete sample. DATA and a reassembled DATA_FRAG sequence carry
 * the same payload, so both paths end here.
 */
void dispatch_data_view(nb_rtps_participant_t *participant,
                               const nb_rtps_data_view_t *data,
                               const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_time_t *source_ts = rx->source_ts;
    const nb_rtps_guid_prefix_t *prefix = rx->prefix;
    const char *source_host = rx->host;
    uint16_t source_port = rx->port;
    nb_rtps_participant_info_t info;
    nb_rtps_data_view_t view = *data;

    /* The source timestamp travels in a separate submessage, so it is attached
     * to the view here rather than by the DATA decoder. */
    view.has_source_ts = source_ts != NULL;
    if (source_ts) {
        view.source_ts = *source_ts;
    }

    if (nb_rtps_entity_id_equal(&view.writer_id, &nb_rtps_eid_spdp_writer)) {
        if (nb_rtps_spdp_parse(view.payload, view.payload_len, &info) == NB_OK &&
            nb_rtps_guid_prefix_equal(&info.guid.prefix, prefix)) {
            update_remote(participant, &info, source_host, source_port);
        }
    } else if (nb_rtps_entity_id_equal(&view.writer_id,
                                       &nb_rtps_eid_sedp_publications_writer)) {
        handle_sedp_data(participant, NB_RTPS_ENDPOINT_PUBLICATION, &view, prefix);
    } else if (nb_rtps_entity_id_equal(&view.writer_id,
                                       &nb_rtps_eid_sedp_subscriptions_writer)) {
        handle_sedp_data(participant, NB_RTPS_ENDPOINT_SUBSCRIPTION, &view, prefix);
    } else if (nb_rtps_entity_id_equal(&view.writer_id,
                                       &nb_rtps_eid_participant_message_writer)) {
        handle_pmd_data(participant, &view, prefix);
    } else {
        handle_user_data(participant, &view, prefix);
    }
}
static void frag_assembly_release(nb_rtps_participant_t *participant,
                                  nb_rtps_frag_assembly_t *assembly)
{
    if (!assembly || !assembly->used) {
        return;
    }
    if (participant->tables.frag_bytes_in_use >= assembly->reserved) {
        participant->tables.frag_bytes_in_use -= assembly->reserved;
    } else {
        participant->tables.frag_bytes_in_use = 0;
    }
    free(assembly->data);
    free(assembly->received);
    memset(assembly, 0, sizeof(*assembly));
}
static nb_rtps_frag_assembly_t *frag_assembly_find(nb_rtps_participant_t *participant,
                                                   const nb_rtps_guid_t *writer,
                                                   uint64_t sn)
{
    size_t i;

    for (i = 0; i < participant->tables.max_frag_assemblies; i++) {
        nb_rtps_frag_assembly_t *assembly = &participant->tables.frag_assemblies[i];

        if (assembly->used && assembly->sn == sn &&
            nb_rtps_guid_equal(&assembly->writer, writer)) {
            return assembly;
        }
    }
    return NULL;
}
static nb_rtps_frag_assembly_t *frag_assembly_free_slot(nb_rtps_participant_t *participant)
{
    nb_rtps_frag_assembly_t *oldest = NULL;
    size_t i;

    for (i = 0; i < participant->tables.max_frag_assemblies; i++) {
        nb_rtps_frag_assembly_t *assembly = &participant->tables.frag_assemblies[i];

        if (!assembly->used) {
            return assembly;
        }
        if (!oldest || assembly->last_update_ms < oldest->last_update_ms) {
            oldest = assembly;
        }
    }
    /* No free slot: drop the sample that has been waiting longest. */
    if (oldest) {
        frag_assembly_release(participant, oldest);
    }
    return oldest;
}
/*
 * Find or create the reassembly state for (writer, sn). Called with the
 * participant lock held. Returns NULL when the sample is too large for the
 * budget or memory is exhausted.
 */
static nb_rtps_frag_assembly_t *frag_assembly_get(nb_rtps_participant_t *participant,
                                                  const nb_rtps_guid_t *writer,
                                                  uint64_t sn,
                                                  uint32_t sample_size,
                                                  uint16_t fragment_size,
                                                  uint64_t now)
{
    nb_rtps_frag_assembly_t *assembly;
    size_t fragment_count;
    size_t reserved;
    size_t i;

    if (participant->tables.frag_assemblies == NULL) {
        participant->tables.max_frag_assemblies = NB_RTPS_FRAG_MAX_ACTIVE;
        participant->tables.frag_assemblies = (nb_rtps_frag_assembly_t *)calloc(
            participant->tables.max_frag_assemblies, sizeof(*participant->tables.frag_assemblies));
        if (!participant->tables.frag_assemblies) {
            participant->tables.max_frag_assemblies = 0;
            return NULL;
        }
    }
    fragment_count = (sample_size + fragment_size - 1u) / fragment_size;
    if (fragment_count == 0) {
        return NULL;
    }
    reserved = (size_t)sample_size + fragment_count;
    if (reserved > NB_RTPS_FRAG_BUDGET_BYTES) {
        return NULL;
    }

    assembly = frag_assembly_find(participant, writer, sn);
    if (assembly) {
        if (assembly->complete) {
            return NULL;
        }
        if (assembly->sample_size != sample_size ||
            assembly->fragment_size != fragment_size) {
            frag_assembly_release(participant, assembly);
        } else {
            return assembly;
        }
    }

    /* Expire samples that stopped arriving, then make room in the budget. */
    for (i = 0; i < participant->tables.max_frag_assemblies; i++) {
        nb_rtps_frag_assembly_t *stale = &participant->tables.frag_assemblies[i];

        if (stale->used && !stale->complete &&
            now - stale->last_update_ms > NB_RTPS_FRAG_TIMEOUT_MS) {
            frag_assembly_release(participant, stale);
        }
    }
    while (participant->tables.frag_bytes_in_use + reserved > NB_RTPS_FRAG_BUDGET_BYTES) {
        nb_rtps_frag_assembly_t *oldest = NULL;

        for (i = 0; i < participant->tables.max_frag_assemblies; i++) {
            nb_rtps_frag_assembly_t *candidate = &participant->tables.frag_assemblies[i];

            if (candidate->used && !candidate->complete &&
                (!oldest || candidate->last_update_ms < oldest->last_update_ms)) {
                oldest = candidate;
            }
        }
        if (!oldest) {
            return NULL;
        }
        frag_assembly_release(participant, oldest);
    }

    assembly = frag_assembly_free_slot(participant);
    if (!assembly) {
        return NULL;
    }
    memset(assembly, 0, sizeof(*assembly));
    assembly->data = (uint8_t *)calloc(1, sample_size);
    assembly->received = (uint8_t *)calloc(1, fragment_count);
    if (!assembly->data || !assembly->received) {
        free(assembly->data);
        free(assembly->received);
        memset(assembly, 0, sizeof(*assembly));
        return NULL;
    }
    assembly->used = 1;
    assembly->writer = *writer;
    assembly->sn = sn;
    assembly->sample_size = sample_size;
    assembly->fragment_size = fragment_size;
    assembly->fragment_count = (uint32_t)fragment_count;
    assembly->reserved = reserved;
    assembly->last_update_ms = now;
    participant->tables.frag_bytes_in_use += reserved;
    return assembly;
}
void handle_data_frag(nb_rtps_participant_t *participant,
                             const nb_rtps_data_frag_t *fragment,
                             const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_time_t *source_ts = rx->source_ts;
    const nb_rtps_guid_prefix_t *prefix = rx->prefix;
    const char *source_host = rx->host;
    uint16_t source_port = rx->port;
    nb_rtps_guid_t writer;
    nb_rtps_frag_assembly_t *assembly;
    uint32_t index;
    size_t offset;
    size_t copy_len;
    uint64_t now;
    uint64_t writer_sn;
    int complete = 0;

    if (!participant || !fragment || !prefix) {
        return;
    }
    writer_sn = nb_rtps_sn_to_u64(&fragment->writer_sn);
    if (writer_sn == 0) {
        return;
    }
    if (fragment->sample_size == 0 || fragment->sample_size > NB_RTPS_SAMPLE_MAX ||
        fragment->fragment_size == 0 || fragment->fragment_starting_num == 0) {
        return;
    }
    writer.prefix = *prefix;
    writer.entity = fragment->writer_id;

    now = zn_wall_ms();
    zb_mutex_lock(participant->net.lock);
    assembly = frag_assembly_get(participant,
                                 &writer,
                                 writer_sn,
                                 fragment->sample_size,
                                 fragment->fragment_size,
                                 now);
    if (assembly) {
        /* Remember who to ask when a fragment goes missing: the address the
         * fragments arrive from is the writer's return path. */
        assembly->reader_id = fragment->reader_id;
        copy_str(assembly->source_host, sizeof(assembly->source_host), source_host);
        assembly->source_port = source_port;
        if (source_ts) {
            assembly->has_source_ts = 1;
            assembly->source_ts = *source_ts;
        }
        if (fragment->key) {
            assembly->key = 1;
        }
        if (fragment->fragment_starting_num == 1 && fragment->inline_qos &&
            fragment->inline_qos_len > 0 &&
            fragment->inline_qos_len <= sizeof(assembly->inline_qos)) {
            memcpy(assembly->inline_qos,
                   fragment->inline_qos,
                   fragment->inline_qos_len);
            assembly->inline_qos_len = fragment->inline_qos_len;
            assembly->has_inline_qos = 1;
        }
        index = fragment->fragment_starting_num - 1u;
        offset = (size_t)index * assembly->fragment_size;
        copy_len = fragment->payload_len;
        if (index < assembly->fragment_count &&
            offset + copy_len <= assembly->sample_size) {
            uint32_t j;

            memcpy(assembly->data + offset, fragment->payload, copy_len);
            /* A submessage may carry several fragments (Cyclone packs them);
             * mark every fragment it covers, not just the first one. */
            for (j = 0; j < fragment->fragments_in_submessage; j++) {
                uint32_t covered = index + j;

                if (covered >= assembly->fragment_count) {
                    break;
                }
                if (!assembly->received[covered]) {
                    assembly->received[covered] = 1;
                    assembly->received_count++;
                }
            }
            assembly->last_update_ms = now;
            if (assembly->received_count == assembly->fragment_count) {
                assembly->complete = 1;
                complete = 1;
            }
        }
    }
    zb_mutex_unlock(participant->net.lock);

    if (complete) {
        nb_rtps_data_view_t view;

        memset(&view, 0, sizeof(view));
        view.reader_id = fragment->reader_id;
        view.writer_id = fragment->writer_id;
        view.writer_sn = fragment->writer_sn;
        view.payload = assembly->data;
        view.payload_len = assembly->sample_size;
        view.key = assembly->key;
        view.dispose = assembly->key;
        if (assembly->has_inline_qos) {
            view.inline_qos = assembly->inline_qos;
            view.inline_qos_len = assembly->inline_qos_len;
        }
        nb_rtps_rx_ctx_t out_rx;

        out_rx.prefix = prefix;
        out_rx.host = source_host;
        out_rx.port = source_port;
        out_rx.source_ts =
            assembly->has_source_ts ? &assembly->source_ts : NULL;
        dispatch_data_view(participant, &view, &out_rx);

        zb_mutex_lock(participant->net.lock);
        if (assembly->used && assembly->complete) {
            frag_assembly_release(participant, assembly);
        }
        zb_mutex_unlock(participant->net.lock);
    }
}
/*
 * Reader side of fragment repair: name the fragments that are still missing so
 * the writer can resend just those. Called with the participant lock held.
 */
int send_frag_nack(nb_rtps_participant_t *participant,
                          nb_rtps_frag_assembly_t *assembly,
                          uint64_t now)
{
    uint8_t message[NB_RTPS_PACKET_MAX];
    nb_rtps_header_t header;
    nb_rtps_time_t timestamp;
    nb_rtps_nack_frag_t nack;
    uint32_t first_missing = 0;
    uint32_t window;
    uint32_t i;
    int found = 0;
    size_t message_len;

    if (!participant || !assembly || !assembly->used || assembly->complete ||
        assembly->fragment_count == 0 || assembly->source_port == 0 ||
        assembly->source_host[0] == '\0') {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < assembly->fragment_count; i++) {
        if (!assembly->received[i]) {
            first_missing = i;
            found = 1;
            break;
        }
    }
    if (!found) {
        return NB_OK;
    }
    window = assembly->fragment_count - first_missing;
    if (window > NB_RTPS_FRAG_NUMBER_SET_MAX_BITS) {
        window = NB_RTPS_FRAG_NUMBER_SET_MAX_BITS;
    }
    memset(&nack, 0, sizeof(nack));
    nack.reader_id = assembly->reader_id;
    nack.writer_id = assembly->writer.entity;
    nb_rtps_sn_from_u64(&nack.writer_sn, assembly->sn);
    /* Fragment numbers are 1-based, so index i of the received bitmap is
     * fragment number i + 1. */
    nack.bitmap_base = first_missing + 1u;
    nack.num_bits = window;
    for (i = 0; i < window; i++) {
        if (!assembly->received[first_missing + i]) {
            nack.bitmap[i / 32u] |= 1u << (31u - (i % 32u));
        }
    }
    nack.count = ++participant->tables.frag_heartbeat_count;

    init_header(participant, &header);
    timestamp.seconds = (int32_t)(now / 1000u);
    timestamp.fraction = (uint32_t)(((now % 1000u) << 32) / 1000u);
    message_len = nb_rtps_wire_nack_frag(message,
                                         sizeof(message),
                                         &header,
                                         &timestamp,
                                         &assembly->writer.prefix,
                                         &nack);
    if (message_len == 0) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (nb_rtps_sendto(participant, participant_data_socket(participant),
                          message,
                          message_len,
                          assembly->source_host,
                          assembly->source_port) < 0) {
        return NB_ERR_IO;
    }
    participant->stats.nack_frags_sent++;

    assembly->last_nack_ms = now;
    assembly->nack_count++;
    return NB_OK;
}
/*
 * A HEARTBEAT_FRAG announces a fragmented sample. If the reader already knows
 * it is incomplete there is no point waiting for the quiet timeout, so ask for
 * the holes right away.
 */
void handle_user_heartbeat_frag(nb_rtps_participant_t *participant,
                                       const nb_rtps_heartbeat_frag_t *heartbeat,
                                       const nb_rtps_rx_ctx_t *rx)
{
    const nb_rtps_guid_prefix_t *source_prefix = rx->prefix;
    nb_rtps_guid_t writer;
    nb_rtps_frag_assembly_t *assembly;
    uint64_t now;

    if (!participant || !heartbeat || !source_prefix) {
        return;
    }
    uint64_t writer_sn = nb_rtps_sn_to_u64(&heartbeat->writer_sn);
    if (writer_sn == 0) {
        return;
    }
    writer.prefix = *source_prefix;
    writer.entity = heartbeat->writer_id;

    now = zn_wall_ms();
    zb_mutex_lock(participant->net.lock);
    assembly = frag_assembly_find(participant, &writer, writer_sn);
    if (assembly && !assembly->complete && assembly->nack_count < NB_RTPS_FRAG_MAX_NACKS) {
        (void)send_frag_nack(participant, assembly, now);
    }
    zb_mutex_unlock(participant->net.lock);
}
