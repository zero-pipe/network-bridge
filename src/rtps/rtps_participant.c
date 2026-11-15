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

zn_socket *participant_data_socket(const nb_rtps_participant_t *participant)
{
    return participant && participant->data_sock ? participant->data_sock
                                                  : participant->unicast_sock;
}

#define NB_RTPS_DUMP_BYTES 48

static void nb_rtps_dump_frame(nb_rtps_participant_t *participant,
                               const char *dir,
                               const void *data,
                               size_t len,
                               const char *host,
                               uint16_t port)
{
    const uint8_t *bytes = (const uint8_t *)data;
    char hex[NB_RTPS_DUMP_BYTES * 3 + 1];
    size_t shown = len < NB_RTPS_DUMP_BYTES ? len : NB_RTPS_DUMP_BYTES;
    size_t used = 0;
    size_t i;

    if (!participant || !participant->dump_frames) {
        return;
    }
    hex[0] = '\0';
    for (i = 0; i < shown; i++) {
        (void)snprintf(hex + used, sizeof(hex) - used, "%02x", bytes[i]);
        used += 2;
        if (i + 1 < shown) {
            hex[used++] = ' ';
            hex[used] = '\0';
        }
    }
    nb_log_debug("rtps: %s %s:%u %zu bytes [%s]%s",
                 dir,
                 host ? host : "?",
                 (unsigned)port,
                 len,
                 hex,
                 len > shown ? " ..." : "");
}

/* One datagram on the wire: counters plus the optional frame dump. */
zn_ssize_t nb_rtps_sendto(nb_rtps_participant_t *participant,
                                  zn_socket *socket,
                                  const void *data,
                                  size_t len,
                                  const char *host,
                                  uint16_t port)
{
    zn_ssize_t rc;

    if (!participant || !socket) {
        return -1;
    }
    nb_rtps_dump_frame(participant, "TX", data, len, host, port);
    if (participant->tcp && host &&
        strncmp(host, NB_RTPS_TCP_KEY_PREFIX, 4) == 0) {
        /* A TCP peer is addressed with its key, not with an address. Same
         * counters, same dump, different wire. */
        int index = nb_rtps_tcp_key_index(host);

        if (nb_rtps_tcp_send(participant->tcp, index, data, len) != 0) {
            return -1;
        }
        participant->stats.packets_sent++;
        participant->stats.bytes_sent += len;
        return (zn_ssize_t)len;
    }
    rc = zn_socket_sendto(socket, data, len, host, port);
    if (rc >= 0) {
        participant->stats.packets_sent++;
        participant->stats.bytes_sent += len;
    }
    return rc;
}

void drop_data_readers_for_writer(nb_rtps_participant_t *participant, const nb_rtps_guid_t *writer);

int endpoint_qos_compatible(
    const nb_rtps_endpoint_info_t *publication,
    const nb_rtps_endpoint_info_t *subscription);

void notify_matches(
    nb_rtps_participant_t *participant,
    const nb_rtps_endpoint_info_t *remote_info,
    nb_rtps_endpoint_kind_t remote_kind,
    int added);

void arm_reader_deadlines(
    nb_rtps_participant_t *participant,
    const nb_rtps_remote_endpoint_t *remote);

void handle_pmd_data(
    nb_rtps_participant_t *participant,
    const nb_rtps_data_view_t *data,
    const nb_rtps_guid_prefix_t *prefix);

void send_pmd(nb_rtps_participant_t *participant, uint32_t kind);

void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    (void)snprintf(dst, cap, "%s", src);
}

/*
 * Destination ordering compares source timestamps, so both directions convert
 * between the RTPS Duration and nanoseconds. The fraction is a 32-bit fraction
 * of a second, which gives roughly 233 ps resolution; the stack writes
 * millisecond timestamps, so nothing is lost in practice.
 */
nb_rtps_time_t rtps_time_from_ns(uint64_t ns)
{
    nb_rtps_time_t ts;

    ts.seconds = (int32_t)(ns / 1000000000ull);
    ts.fraction = (uint32_t)(((ns % 1000000000ull) << 32) / 1000000000ull);
    return ts;
}

uint64_t rtps_time_to_ns(const nb_rtps_time_t *ts)
{
    if (!ts || ts->seconds < 0) {
        return 0;
    }
    return (uint64_t)ts->seconds * 1000000000ull +
           (((uint64_t)ts->fraction * 1000000000ull) >> 32);
}

static void make_guid(nb_rtps_participant_t *participant, uint64_t now)
{
    uint32_t low = (uint32_t)(now & 0xffffffffu);
    uint32_t high = (uint32_t)((now >> 32) & 0xffffffffu) ^ (uint32_t)(uintptr_t)participant;
    uint8_t *p = participant->local_guid.prefix.bytes;

    p[0] = NB_RTPS_VENDOR_ID_0;
    p[1] = NB_RTPS_VENDOR_ID_1;
    p[2] = 0x4e; /* 'N' */
    p[3] = 0x42; /* 'B' */
    p[4] = (uint8_t)(low & 0xffu);
    p[5] = (uint8_t)((low >> 8) & 0xffu);
    p[6] = (uint8_t)((low >> 16) & 0xffu);
    p[7] = (uint8_t)((low >> 24) & 0xffu);
    p[8] = (uint8_t)(high & 0xffu);
    p[9] = (uint8_t)((high >> 8) & 0xffu);
    p[10] = (uint8_t)((high >> 16) & 0xffu);
    p[11] = (uint8_t)((high >> 24) & 0xffu);
    participant->local_guid.entity = nb_rtps_eid_participant;
}

void notify(nb_rtps_participant_t *participant,
                   nb_rtps_participant_event_t event,
                   const nb_rtps_participant_info_t *info)
{
    nb_rtps_participant_callback_t callback;
    void *user;

    zb_mutex_lock(participant->lock);
    callback = participant->callback;
    user = participant->callback_user;
    zb_mutex_unlock(participant->lock);
    if (callback) {
        callback(user, event, info);
    }
}

static void allocate_local_entity(nb_rtps_participant_t *participant,
                                  nb_rtps_endpoint_kind_t kind,
                                  nb_rtps_entity_id_t *out)
{
    uint32_t key = ++participant->next_entity_key;

    if (key == 0) {
        key = ++participant->next_entity_key;
    }
    out->bytes[0] = (uint8_t)(key & 0xffu);
    out->bytes[1] = (uint8_t)((key >> 8) & 0xffu);
    out->bytes[2] = (uint8_t)((key >> 16) & 0xffu);
    /* All locally created endpoints are keyless: user-defined writer without
     * key (0x03) and user-defined reader without key (0x04). */
    out->bytes[3] = kind == NB_RTPS_ENDPOINT_PUBLICATION ? 0x03u : 0x04u;
}

int locator_to_host(const nb_rtps_locator_t *locator,
                           char *host,
                           size_t cap,
                           uint16_t *port)
{
    if (!locator || !host || cap == 0 || !port ||
        locator->kind != NB_RTPS_LOCATOR_KIND_UDPV4 || locator->port == 0 ||
        locator->port > 65535u) {
        return NB_ERR_INVALID;
    }
    if (snprintf(host,
                 cap,
                 "%u.%u.%u.%u",
                 (unsigned)locator->address[12],
                 (unsigned)locator->address[13],
                 (unsigned)locator->address[14],
                 (unsigned)locator->address[15]) <= 0) {
        return NB_ERR_INVALID;
    }
    *port = (uint16_t)locator->port;
    return NB_OK;
}

/* Same layout for connection-oriented locators: the address is still a
 * 16-byte IPv4-mapped value, only the kind differs. */
int locator_to_host_any(const nb_rtps_locator_t *locator,
                               char *host,
                               size_t cap,
                               uint16_t *port)
{
    if (!locator || !host || cap == 0 || !port || locator->port == 0 ||
        locator->port > 65535u) {
        return NB_ERR_INVALID;
    }
    if (locator->kind != NB_RTPS_LOCATOR_KIND_UDPV4 &&
        locator->kind != NB_RTPS_LOCATOR_KIND_TCPV4) {
        return NB_ERR_INVALID;
    }
    if (snprintf(host,
                 cap,
                 "%u.%u.%u.%u",
                 (unsigned)locator->address[12],
                 (unsigned)locator->address[13],
                 (unsigned)locator->address[14],
                 (unsigned)locator->address[15]) <= 0) {
        return NB_ERR_INVALID;
    }
    *port = (uint16_t)locator->port;
    return NB_OK;
}

int remote_target(const nb_rtps_remote_participant_t *remote,
                         char *host,
                         size_t cap,
                         uint16_t *port)
{
    if (!remote || !remote->used) {
        return NB_ERR_INVALID;
    }
    if (remote->tcp_key[0] != '\0') {
        copy_str(host, cap, remote->tcp_key);
        *port = (uint16_t)(nb_rtps_tcp_key_index(remote->tcp_key) + 1);
        return NB_OK;
    }
    if (locator_to_host(&remote->info.metatraffic_unicast, host, cap, port) == NB_OK) {
        return NB_OK;
    }
    if (remote->source_port == 0) {
        return NB_ERR_INVALID;
    }
    copy_str(host, cap, remote->source_host);
    *port = remote->source_port;
    return NB_OK;
}

void init_header(const nb_rtps_participant_t *participant, nb_rtps_header_t *header)
{
    memset(header, 0, sizeof(*header));
    header->version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header->version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header->vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header->vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header->guid_prefix = participant->local_guid.prefix;
}

nb_rtps_local_endpoint_t *find_local_endpoint(nb_rtps_participant_t *participant,
                                                     const nb_rtps_guid_t *guid)
{
    size_t i;

    for (i = 0; i < participant->local_used_count; i++) {
        nb_rtps_local_endpoint_t *endpoint =
            &participant->local_endpoints[participant->local_used[i]];

        if (nb_rtps_guid_equal(&endpoint->info.endpoint_guid, guid)) {
            return endpoint;
        }
    }
    return NULL;
}

nb_rtps_remote_endpoint_t *find_remote_endpoint(nb_rtps_participant_t *participant,
                                                       const nb_rtps_guid_t *guid)
{
    size_t i;

    for (i = 0; i < participant->remote_ep_used_count; i++) {
        nb_rtps_remote_endpoint_t *endpoint =
            &participant->remote_endpoints[participant->remote_ep_used[i]];

        if (nb_rtps_guid_equal(&endpoint->info.endpoint_guid, guid)) {
            return endpoint;
        }
    }
    return NULL;
}

/* The fixed-size tables are sized for the worst case but are almost empty in
 * practice. Each table keeps a dense list of the slots in use so a publish or a
 * received sample only visits entries that exist. */
void nb_rtps_used_add(uint32_t *list, size_t *count, size_t cap, size_t index)
{
    if (list && *count < cap) {
        list[(*count)++] = (uint32_t)index;
    }
}

void nb_rtps_used_remove(uint32_t *list, size_t *count, size_t index)
{
    size_t i;

    if (!list) {
        return;
    }
    for (i = 0; i < *count; i++) {
        if (list[i] == index) {
            list[i] = list[--(*count)];
            return;
        }
    }
}

void notify_endpoint(nb_rtps_participant_t *participant,
                            nb_rtps_endpoint_event_t event,
                            nb_rtps_endpoint_kind_t kind,
                            const nb_rtps_endpoint_info_t *info)
{
    nb_rtps_endpoint_callback_t callback;
    void *user;

    zb_mutex_lock(participant->lock);
    callback = participant->endpoint_callback;
    user = participant->endpoint_user;
    zb_mutex_unlock(participant->lock);
    if (callback) {
        callback(user, event, kind, info);
    }
}

nb_rtps_remote_participant_t *find_remote(nb_rtps_participant_t *participant,
                                                 const nb_rtps_guid_prefix_t *prefix)
{
    size_t i;

    for (i = 0; i < participant->remotes_used_count; i++) {
        nb_rtps_remote_participant_t *remote =
            &participant->remotes[participant->remotes_used[i]];

        if (nb_rtps_guid_prefix_equal(&remote->info.guid.prefix, prefix)) {
            return remote;
        }
    }
    return NULL;
}

/*
 * Collect every usable unicast address of a remote endpoint. A 0.0.0.0 locator
 * is not routable and is skipped; when none is usable the participant's default
 * or metatraffic locator, and finally the SEDP source address, stand in.
 */
size_t endpoint_data_targets(nb_rtps_participant_t *participant,
                                    const nb_rtps_remote_endpoint_t *endpoint,
                                    nb_rtps_targets_t *out)
{
    nb_rtps_remote_participant_t *remote;
    size_t i;
    size_t j;

    if (!participant || !endpoint || !out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    /* A participant reachable over TCP has one destination, and it wins over
     * whatever locators the endpoint announced. */
    remote = find_remote(participant, &endpoint->info.endpoint_guid.prefix);
    if (remote && remote->tcp_key[0] != '\0') {
        copy_str(out->host[0], sizeof(out->host[0]), remote->tcp_key);
        out->port[0] = (uint16_t)(nb_rtps_tcp_key_index(remote->tcp_key) + 1);
        out->count = 1;
        return 1;
    }
    for (i = 0; i < endpoint->info.unicast_count; i++) {
        const nb_rtps_locator_t *locator = &endpoint->info.unicast[i];
        char host[NB_RTPS_PEER_HOST_MAX];
        uint16_t port = 0;

        if (locator->address[12] == 0 && locator->address[13] == 0 &&
            locator->address[14] == 0 && locator->address[15] == 0) {
            continue;
        }
        if (locator_to_host(locator, host, sizeof(host), &port) != NB_OK) {
            continue;
        }
        for (j = 0; j < out->count; j++) {
            if (out->port[j] == port && strcmp(out->host[j], host) == 0) {
                break;
            }
        }
        if (j < out->count || out->count == NB_RTPS_DATA_TARGET_MAX) {
            continue;
        }
        copy_str(out->host[out->count], sizeof(out->host[out->count]), host);
        out->port[out->count] = port;
        out->count++;
    }
    if (out->count > 0) {
        return out->count;
    }

    remote = find_remote(participant, &endpoint->info.endpoint_guid.prefix);
    if (!remote) {
        return 0;
    }
    if (locator_to_host(&remote->info.default_unicast,
                        out->host[0],
                        sizeof(out->host[0]),
                        &out->port[0]) == NB_OK ||
        locator_to_host(&remote->info.metatraffic_unicast,
                        out->host[0],
                        sizeof(out->host[0]),
                        &out->port[0]) == NB_OK) {
        out->count = 1;
        return 1;
    }
    if (remote->source_port != 0) {
        copy_str(out->host[0], sizeof(out->host[0]), remote->source_host);
        out->port[0] = remote->source_port;
        out->count = 1;
        return 1;
    }
    return 0;
}

/* First usable target; the single-destination paths (ACKNACK/NACK_FRAG replies)
 * answer where the request came from and do not need the whole list. */
int endpoint_data_target(nb_rtps_participant_t *participant,
                                const nb_rtps_remote_endpoint_t *endpoint,
                                char *host,
                                size_t cap,
                                uint16_t *port)
{
    nb_rtps_targets_t targets;

    if (!host || cap == 0 || !port ||
        endpoint_data_targets(participant, endpoint, &targets) == 0) {
        return NB_ERR_NOTFOUND;
    }
    copy_str(host, cap, targets.host[0]);
    *port = targets.port[0];
    return NB_OK;
}

/*
 * Bring up the RTPS-over-TCP link to a discovered participant.
 *
 * Both sides advertise a TCP locator, so both could dial; DDS leaves the choice
 * to the implementation, and the only requirement is that the two ends agree.
 * The lower GUID prefix dials and the other one accepts, which is stable across
 * restarts because the prefix is unique per participant.
 */
void maybe_tcp_connect(nb_rtps_participant_t *participant,
                              const nb_rtps_guid_prefix_t *prefix)
{
    nb_rtps_remote_participant_t *remote;
    char host[NB_RTPS_PEER_HOST_MAX];
    uint16_t port = 0;
    uint64_t now;
    int dial;

    if (!participant->tcp || !prefix) {
        return;
    }
    zb_mutex_lock(participant->lock);
    remote = find_remote(participant, prefix);
    if (!remote || remote->tcp_key[0] != '\0' ||
        remote->info.metatraffic_tcp.kind == NB_RTPS_LOCATOR_KIND_INVALID) {
        zb_mutex_unlock(participant->lock);
        return;
    }
    if (locator_to_host_any(&remote->info.metatraffic_tcp,
                            host,
                            sizeof(host),
                            &port) != NB_OK) {
        zb_mutex_unlock(participant->lock);
        return;
    }
    dial = memcmp(participant->local_guid.prefix.bytes,
                  remote->info.guid.prefix.bytes,
                  sizeof(participant->local_guid.prefix.bytes)) < 0;
    now = zn_wall_ms();
    if (!dial || now < remote->next_tcp_connect_ms) {
        zb_mutex_unlock(participant->lock);
        return;
    }
    remote->next_tcp_connect_ms = now + 1000u;
    zb_mutex_unlock(participant->lock);

    if (nb_rtps_tcp_has_link(participant->tcp, host, port)) {
        return;
    }
    if (nb_rtps_tcp_connect(participant->tcp, host, port) < 0) {
        nb_log_debug("rtps: tcp connect to %s:%u failed",
                     host,
                     (unsigned)port);
    }
}

/* Periodic retry: discovery is retransmitted every announce interval, so a
 * failed dial must not wait for the next SPDP sample to be retried. */
static void retry_tcp_links(nb_rtps_participant_t *participant)
{
    nb_rtps_guid_prefix_t prefixes[NB_RTPS_TCP_MAX_CONNECTIONS];
    size_t count = 0;
    size_t i;

    if (!participant->tcp) {
        return;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remotes &&
                count < NB_RTPS_TCP_MAX_CONNECTIONS;
         i++) {
        nb_rtps_remote_participant_t *remote = &participant->remotes[i];

        if (!remote->used || remote->tcp_key[0] != '\0' ||
            remote->info.metatraffic_tcp.kind == NB_RTPS_LOCATOR_KIND_INVALID) {
            continue;
        }
        prefixes[count++] = remote->info.guid.prefix;
    }
    zb_mutex_unlock(participant->lock);
    for (i = 0; i < count; i++) {
        maybe_tcp_connect(participant, &prefixes[i]);
    }
}

/*
 * TRANSIENT_LOCAL late joiner support: a reader that matches a publication
 * after samples were written must still receive the retained history. Reliable
 * readers would eventually pull it through ACKNACK, but best-effort readers
 * have no such recovery, so replay the history explicitly for both.
 */
void replay_retained_history(nb_rtps_participant_t *participant,
                                    const nb_rtps_endpoint_info_t *reader_info)
{
    size_t i;

    if (!participant || !reader_info) {
        return;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_local_endpoints; i++) {
        nb_rtps_local_endpoint_t *local = &participant->local_endpoints[i];
        nb_rtps_remote_endpoint_t *remote;
        nb_rtps_targets_t targets;
        uint64_t first_sn = 0;
        uint64_t last_sn = 0;
        uint64_t sn;
        size_t sample_count = 0;

        if (!local->used || local->kind != NB_RTPS_ENDPOINT_PUBLICATION ||
            !local->history ||
            local->info.durability < NB_RTPS_DURABILITY_TRANSIENT_LOCAL ||
            !endpoint_qos_compatible(&local->info, reader_info)) {
            continue;
        }
        remote = find_remote_endpoint(participant, &reader_info->endpoint_guid);
        if (!remote || endpoint_data_targets(participant, remote, &targets) == 0) {
            continue;
        }
        nb_rtps_reliable_state(local->history, &first_sn, &last_sn, &sample_count);
        for (sn = first_sn; sample_count > 0 && sn <= last_sn; sn++) {
            const uint8_t *payload = NULL;
            size_t len = 0;
            int dispose = 0;
            uint8_t handle[NB_INSTANCE_HANDLE_SIZE];
            int has_handle = 0;
            uint32_t status = 0;
            uint64_t source_ns = 0;
            uint64_t now = zn_wall_ms();

            if (nb_rtps_reliable_get_instance(local->history,
                                              sn,
                                              &payload,
                                              &len,
                                              &dispose,
                                              handle,
                                              &has_handle,
                                              &status) != NB_OK) {
                continue;
            }
            (void)nb_rtps_reliable_source_ns(local->history, sn, &source_ns);
            if (sample_expired(local, sn, now)) {
                continue;
            }
            (void)send_user_data_all(participant,
                                     &local->info.endpoint_guid,
                                     &reader_info->endpoint_guid,
                                     sn,
                                     source_ns,
                                     payload,
                                     len,
                                     has_handle ? handle : NULL,
                                     status,
                                     &targets);
        }
        if (sample_count > 0 &&
            local->info.reliability == NB_RTPS_RELIABILITY_RELIABLE) {
            send_user_heartbeat_all(
                participant, local, reader_info, first_sn, last_sn, &targets);
        }
    }
    zb_mutex_unlock(participant->lock);
}

/* Peers may GAP built-in SEDP writers when their endpoint history is trimmed.
 * Mark those samples received so the SEDP reader stops asking for them. */

/* QoS compatibility between an offered publication and a requested
 * subscription: matching topic, type and partition; a reliable reader
 * requires a reliable writer; the writer must offer at least the requested
 * durability. */
/*
 * DDS partition names may contain the '*' wildcard and an endpoint may list
 * several names separated by commas. Two endpoints match when any name on one
 * side matches any name on the other.
 */
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

/*
 * Returns NULL when the pair can be matched, otherwise a short reason for the
 * mismatch. The reason is only used for diagnostics: matching happens in the
 * hot path, so it is logged at debug level and never formatted per packet.
 */
static const char *endpoint_qos_mismatch(const nb_rtps_endpoint_info_t *publication,
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

/*
 * How many remote endpoints a local endpoint is matched with right now.
 * Called with the participant lock held.
 */
static uint32_t matched_remote_count(nb_rtps_participant_t *participant,
                                     const nb_rtps_local_endpoint_t *local)
{
    uint32_t matches = 0;
    size_t i;

    for (i = 0; i < participant->max_remote_endpoints; i++) {
        const nb_rtps_remote_endpoint_t *remote = &participant->remote_endpoints[i];

        if (!remote->used) {
            continue;
        }
        if (local->kind == NB_RTPS_ENDPOINT_PUBLICATION) {
            if (remote->kind == NB_RTPS_ENDPOINT_SUBSCRIPTION &&
                !endpoint_qos_mismatch(&local->info, &remote->info)) {
                matches++;
            }
        } else if (remote->kind == NB_RTPS_ENDPOINT_PUBLICATION &&
                   !endpoint_qos_mismatch(&remote->info, &local->info)) {
            matches++;
        }
    }
    return matches;
}

/*
 * One DDS match notification for one (local, remote) pair. A remote endpoint
 * with the same topic and type either matches the local one or is incompatible
 * with it; anything else is simply a different topic and not interesting. The
 * event carries the local endpoint's current match count, so a lower count
 * means a previously matched peer went away, which is how DDS reports the
 * change in PublicationMatchedStatus/SubscriptionMatchedStatus.
 */
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

/* Notify every local endpoint that could relate to a remote one. */
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

/*
 * The mirror case: an endpoint created after its peers already discovered.
 * SEDP replay tells the peers, but the local side also has to learn that it
 * matched something that was already there.
 */
static void notify_matches_for_local(nb_rtps_participant_t *participant,
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

/* All data_reader helpers are called with participant->lock held. */
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

/*
 * Return the window for a (writer, reader) pair, creating it when needed.
 * first_expected is the sequence number a new window starts at.
 *
 * The table is sized by max_remote_endpoints. When it is full the pair is
 * served without a window: samples are still delivered, but duplicates are not
 * filtered and no ACKNACK is sent for it. Recycling an existing window was
 * rejected because restarting a working window makes the peer replay its whole
 * history, which turns into a duplicate storm for the pairs that were working.
 * Sizing max_remote_endpoints for writers x local readers avoids the state.
 */
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

/*
 * A matched writer starts the subscription's requested-deadline timer even if
 * it never writes: silence is exactly the condition being detected. The same
 * window carries the writer's liveliness state, which only exists for writers
 * that announce a finite lease. Called with the participant lock held.
 */
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

static void drop_data_readers_for_reader(nb_rtps_participant_t *participant,
                                         const nb_rtps_entity_id_t *reader)
{
    size_t i;

    if (!participant->data_readers) {
        return;
    }
    for (i = 0; i < participant->max_data_readers; i++) {
        nb_rtps_data_reader_state_t *state = &participant->data_readers[i];

        if (state->used && nb_rtps_entity_id_equal(&state->reader, reader)) {
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

/*
 * One submessage from one RTPS message. A datagram carries a whole message, so
 * the UDP path decodes the header and walks the submessages; a TCP stream has
 * no message boundaries, so the stream parser in rtps_tcp.c calls this same
 * entry point as it extracts them. Keeping the body in one place is what makes
 * the two transports behave identically.
 *
 * `has_source_ts` and `source_ts` are per-message state: INFO_TS applies to
 * every DATA that follows it until the next INFO_TS or the next message.
 */
static int guid_prefix_is_zero(const nb_rtps_guid_prefix_t *prefix)
{
    size_t i;

    if (!prefix) {
        return 1;
    }
    for (i = 0; i < NB_RTPS_GUID_PREFIX_SIZE; i++) {
        if (prefix->bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void handle_submessage(nb_rtps_participant_t *participant,
                              const nb_rtps_header_t *header,
                              const nb_rtps_submessage_view_t *view,
                              nb_rtps_receiver_state_t *state,
                              const char *source_host,
                              uint16_t source_port)
{
    const nb_rtps_guid_prefix_t *source_prefix;

    /* Interpreter submessages update the per-message state and are never
     * filtered by INFO_DST: a message may carry INFO_TS/INFO_SRC before an
     * INFO_DST that redirects the entity submessages that follow. */
    if (view->id == NB_RTPS_SUBMSG_INFO_DST) {
        nb_rtps_guid_prefix_t prefix;

        if (nb_rtps_info_dst_decode(view->flags, view->body, view->body_len,
                                    &prefix) == NB_OK) {
            state->dst_prefix = prefix;
            state->has_dst_prefix = 1;
        } else {
            state->has_dst_prefix = 0;
            participant->stats.parse_errors++;
        }
        return;
    }
    if (view->id == NB_RTPS_SUBMSG_INFO_SRC) {
        nb_rtps_guid_prefix_t prefix;

        if (nb_rtps_info_src_decode(view->flags, view->body, view->body_len,
                                    &prefix) == NB_OK) {
            state->source_prefix = prefix;
            state->has_source_prefix = 1;
        } else {
            state->has_source_prefix = 0;
            participant->stats.parse_errors++;
        }
        return;
    }

    source_prefix = state->has_source_prefix ? &state->source_prefix
                                             : &header->guid_prefix;
    /* INFO_DST addresses the following Entity Submessages to one participant.
     * An all-zero prefix is treated as "no destination" (broadcast), which is
     * also what a peer that writes INFO_DST without a target produces. */
    if (state->has_dst_prefix && !guid_prefix_is_zero(&state->dst_prefix) &&
        !nb_rtps_guid_prefix_equal(&state->dst_prefix,
                                   &participant->local_guid.prefix)) {
        return;
    }

    if (view->id == NB_RTPS_SUBMSG_DATA) {
        nb_rtps_data_view_t data;

        if (nb_rtps_data_decode(view->flags, view->body, view->body_len, &data) != NB_OK) {
            participant->stats.parse_errors++;
            return;
        }
        if (nb_rtps_entity_id_equal(&data.writer_id, &nb_rtps_eid_spdp_writer)) {
            participant->stats.spdp_received++;
        } else if (nb_rtps_entity_id_equal(&data.writer_id,
                                           &nb_rtps_eid_sedp_publications_writer) ||
                   nb_rtps_entity_id_equal(&data.writer_id,
                                           &nb_rtps_eid_sedp_subscriptions_writer)) {
            participant->stats.sedp_received++;
        } else {
            participant->stats.data_received++;
        }
        dispatch_data_view(participant,
                           &data,
                           state->has_source_ts ? &state->source_ts : NULL,
                           source_prefix,
                           source_host,
                           source_port);
    } else if (view->id == NB_RTPS_SUBMSG_DATA_FRAG) {
        nb_rtps_data_frag_t fragment;

        if (nb_rtps_data_frag_decode(
                view->flags, view->body, view->body_len, &fragment) == NB_OK) {
            participant->stats.fragments_received++;
            handle_data_frag(participant,
                             &fragment,
                             state->has_source_ts ? &state->source_ts : NULL,
                             source_prefix,
                             source_host,
                             source_port);
        } else {
            participant->stats.parse_errors++;
        }
    } else if (view->id == NB_RTPS_SUBMSG_INFO_TS) {
        /* INFO_TS applies to the submessages that follow it until the next
         * INFO_TS, so remember it for DATA and DATA_FRAG. */
        if (nb_rtps_info_ts_decode(view->flags, view->body, view->body_len,
                                   &state->source_ts) == NB_OK) {
            state->has_source_ts = 1;
        } else {
            state->has_source_ts = 0;
        }
    } else if (view->id == NB_RTPS_SUBMSG_HEARTBEAT) {
        nb_rtps_heartbeat_t heartbeat;

        if (nb_rtps_heartbeat_decode(
                view->flags, view->body, view->body_len, &heartbeat) != NB_OK) {
            participant->stats.parse_errors++;
            return;
        }
        participant->stats.heartbeats_received++;
        /* The decoder already set the L (liveliness) and F (final/ack
         * requested) bits from the submessage flags. */
        if (nb_rtps_entity_id_equal(&heartbeat.writer_id,
                                    &nb_rtps_eid_sedp_publications_writer)) {
            handle_sedp_heartbeat(participant,
                                  NB_RTPS_ENDPOINT_PUBLICATION,
                                  &heartbeat,
                                  source_prefix,
                                  source_host,
                                  source_port);
        } else if (nb_rtps_entity_id_equal(&heartbeat.writer_id,
                                           &nb_rtps_eid_sedp_subscriptions_writer)) {
            handle_sedp_heartbeat(participant,
                                  NB_RTPS_ENDPOINT_SUBSCRIPTION,
                                  &heartbeat,
                                  source_prefix,
                                  source_host,
                                  source_port);
        } else {
            handle_user_heartbeat(participant,
                                  &heartbeat,
                                  source_prefix,
                                  source_host,
                                  source_port);
        }
    } else if (view->id == NB_RTPS_SUBMSG_ACKNACK) {
        nb_rtps_acknack_t acknack;

        if (nb_rtps_acknack_decode(view->body, view->body_len, &acknack) != NB_OK) {
            participant->stats.parse_errors++;
            return;
        }
        participant->stats.acknacks_received++;
        if (nb_rtps_entity_id_equal(&acknack.writer_id,
                                    &nb_rtps_eid_sedp_publications_writer)) {
            handle_sedp_acknack(participant,
                                NB_RTPS_ENDPOINT_PUBLICATION,
                                &acknack,
                                source_prefix,
                                source_host,
                                source_port);
        } else if (nb_rtps_entity_id_equal(&acknack.writer_id,
                                           &nb_rtps_eid_sedp_subscriptions_writer)) {
            handle_sedp_acknack(participant,
                                NB_RTPS_ENDPOINT_SUBSCRIPTION,
                                &acknack,
                                source_prefix,
                                source_host,
                                source_port);
        } else {
            handle_user_acknack(participant,
                                &acknack,
                                source_prefix,
                                source_host,
                                source_port);
        }
    } else if (view->id == NB_RTPS_SUBMSG_NACK_FRAG) {
        nb_rtps_nack_frag_t nack;

        if (nb_rtps_nack_frag_decode(view->body, view->body_len, &nack) == NB_OK) {
            participant->stats.nack_frags_received++;
            handle_user_nack_frag(
                participant, &nack, source_prefix, source_host, source_port);
        } else {
            participant->stats.parse_errors++;
        }
    } else if (view->id == NB_RTPS_SUBMSG_HEARTBEAT_FRAG) {
        nb_rtps_heartbeat_frag_t heartbeat;

        if (nb_rtps_heartbeat_frag_decode(view->body, view->body_len, &heartbeat) == NB_OK) {
            participant->stats.heartbeats_received++;
            handle_user_heartbeat_frag(participant, &heartbeat, source_prefix);
        } else {
            participant->stats.parse_errors++;
        }
    } else if (view->id == NB_RTPS_SUBMSG_GAP) {
        nb_rtps_gap_t gap;

        if (nb_rtps_gap_decode(view->body, view->body_len, &gap) == NB_OK) {
            participant->stats.gaps_received++;
            if (nb_rtps_entity_id_equal(&gap.writer_id,
                                        &nb_rtps_eid_sedp_publications_writer)) {
                handle_sedp_gap(participant,
                                NB_RTPS_ENDPOINT_PUBLICATION,
                                &gap,
                                source_prefix);
            } else if (nb_rtps_entity_id_equal(
                           &gap.writer_id,
                           &nb_rtps_eid_sedp_subscriptions_writer)) {
                handle_sedp_gap(participant,
                                NB_RTPS_ENDPOINT_SUBSCRIPTION,
                                &gap,
                                source_prefix);
            } else {
                handle_user_gap(participant, &gap, source_prefix);
            }
        }
    }
}

static void handle_datagram(nb_rtps_participant_t *participant,
                            const uint8_t *buf,
                            size_t len,
                            const char *source_host,
                            uint16_t source_port)
{
    nb_rtps_header_t header;
    nb_rtps_submessage_view_t view;
    nb_rtps_receiver_state_t state;
    size_t offset = 0;
    int rc;

    memset(&state, 0, sizeof(state));
    if (nb_rtps_message_begin(buf, len, &header, &offset) != NB_OK) {
        participant->stats.parse_errors++;
        return;
    }
    if (nb_rtps_guid_prefix_equal(&header.guid_prefix, &participant->local_guid.prefix)) {
        return;
    }
    while ((rc = nb_rtps_message_next(buf, len, &offset, &view)) == 1) {
        handle_submessage(participant,
                          &header,
                          &view,
                          &state,
                          source_host,
                          source_port);
    }
}

/*
 * A TCP peer is identified by its RTPS header, not by an address: the first
 * message on a connection tells us which participant is at the other end, and
 * from then on everything addressed to that participant goes over this link.
 */
static void tcp_bind_remote(nb_rtps_participant_t *participant,
                            const nb_rtps_guid_prefix_t *prefix,
                            const char *key)
{
    nb_rtps_remote_participant_t *remote;

    if (!key) {
        return;
    }
    zb_mutex_lock(participant->lock);
    remote = find_remote(participant, prefix);
    if (remote && remote->tcp_key[0] == '\0') {
        copy_str(remote->tcp_key, sizeof(remote->tcp_key), key);
    }
    zb_mutex_unlock(participant->lock);
}

static void tcp_on_message(void *user,
                           int index,
                           const nb_rtps_header_t *header,
                           const nb_rtps_submessage_view_t *view,
                           nb_rtps_receiver_state_t *state)
{
    nb_rtps_participant_t *participant = (nb_rtps_participant_t *)user;
    const char *key = nb_rtps_tcp_key(participant->tcp, index);

    if (!key) {
        return;
    }
    if (nb_rtps_guid_prefix_equal(&header->guid_prefix,
                                  &participant->local_guid.prefix)) {
        return;
    }
    tcp_bind_remote(participant, &header->guid_prefix, key);
    handle_submessage(participant,
                      header,
                      view,
                      state,
                      key,
                      (uint16_t)(index + 1));
}

/* A dialled connection knows the address it went to, which is enough to bind it
 * to the participant that advertised that locator. */
static void tcp_on_link(void *user, int index)
{
    nb_rtps_participant_t *participant = (nb_rtps_participant_t *)user;
    const char *key = nb_rtps_tcp_key(participant->tcp, index);
    char host[NB_RTPS_PEER_HOST_MAX];
    uint16_t port = 0;
    uint32_t i;

    if (!key) {
        return;
    }
    host[0] = '\0';
    nb_rtps_tcp_link_info(participant->tcp, index, host, sizeof(host), &port);
    if (host[0] == '\0' || port == 0) {
        /* Inbound: the peer identifies itself with its first message. */
        return;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remotes; i++) {
        nb_rtps_remote_participant_t *remote = &participant->remotes[i];
        char remote_host[NB_RTPS_PEER_HOST_MAX];
        uint16_t remote_port = 0;

        if (!remote->used ||
            remote->info.metatraffic_tcp.kind == NB_RTPS_LOCATOR_KIND_INVALID) {
            continue;
        }
        if (locator_to_host_any(&remote->info.metatraffic_tcp,
                                remote_host,
                                sizeof(remote_host),
                                &remote_port) != NB_OK) {
            continue;
        }
        if (remote_port == port && strcmp(remote_host, host) == 0) {
            copy_str(remote->tcp_key, sizeof(remote->tcp_key), key);
            break;
        }
    }
    zb_mutex_unlock(participant->lock);
}

/* The link is gone: fall back to UDP until a new connection is established. */
static void tcp_on_unlink(void *user, int index)
{
    nb_rtps_participant_t *participant = (nb_rtps_participant_t *)user;
    const char *key = nb_rtps_tcp_key(participant->tcp, index);
    uint32_t i;

    if (!key) {
        return;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remotes; i++) {
        nb_rtps_remote_participant_t *remote = &participant->remotes[i];

        if (remote->used && strcmp(remote->tcp_key, key) == 0) {
            remote->tcp_key[0] = '\0';
            remote->next_tcp_connect_ms = 0;
        }
    }
    zb_mutex_unlock(participant->lock);
}

static void drain_socket(nb_rtps_participant_t *participant, zn_socket *socket)
{
    uint8_t buf[NB_RTPS_RX_MAX];
    char host[NB_RTPS_PEER_HOST_MAX];
    uint16_t port;

    for (;;) {
        zn_ssize_t n = zn_socket_recvfrom(
            socket, buf, sizeof(buf), host, sizeof(host), &port);
        if (n <= 0) {
            break;
        }
        participant->stats.packets_received++;
        participant->stats.bytes_received += (uint64_t)n;
        nb_rtps_dump_frame(participant, "RX", buf, (size_t)n, host, port);
        handle_datagram(participant, buf, (size_t)n, host, port);
    }
}

/*
 * Deadline monitoring. DDS reports a missed deadline on the writer that did not
 * write (offered) and on the reader that did not receive (requested) within the
 * configured period. Both are rate limited to one event per period; the count
 * keeps advancing so a caller can tell a continuing stall from a new one. The
 * events are collected under the lock and delivered after it is released,
 * because the callback must not re-enter the participant.
 */
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

/*
 * Shortest lease over the local publications that need automatic assertions:
 * those announce AUTOMATIC liveliness with a finite lease. Asserting at 80% of
 * the shortest lease keeps every one of them alive with one packet.
 */
static uint32_t pmd_interval_from_local(nb_rtps_participant_t *participant)
{
    uint32_t min_lease = 0;
    uint32_t interval;
    size_t i;

    for (i = 0; i < participant->max_local_endpoints; i++) {
        const nb_rtps_local_endpoint_t *local = &participant->local_endpoints[i];

        if (!local->used || local->kind != NB_RTPS_ENDPOINT_PUBLICATION ||
            local->info.liveliness_kind != NB_RTPS_LIVELINESS_AUTOMATIC ||
            local->info.liveliness_lease_ms == 0) {
            continue;
        }
        if (min_lease == 0 || local->info.liveliness_lease_ms < min_lease) {
            min_lease = local->info.liveliness_lease_ms;
        }
    }
    if (min_lease == 0) {
        return 0;
    }
    interval = (uint32_t)(((uint64_t)min_lease * NB_RTPS_PMD_ASSERT_PCT) / 100u);
    if (interval < NB_RTPS_MIN_PMD_INTERVAL_MS) {
        interval = NB_RTPS_MIN_PMD_INTERVAL_MS;
    }
    if (interval >= min_lease) {
        interval = min_lease;
    }
    return interval;
}

/* Called with the participant lock held after a local endpoint changes. */
static void refresh_pmd_interval(nb_rtps_participant_t *participant)
{
    uint32_t interval = pmd_interval_from_local(participant);

    if (interval == participant->pmd_interval_ms) {
        return;
    }
    participant->pmd_interval_ms = interval;
    /* Assert immediately so a peer that just matched sees the lease begin. */
    participant->next_pmd_ms = 0;
}

/*
 * Announce automatic liveliness to every known participant with built-in
 * ParticipantMessageData. The payload travels in a DATA submessage addressed to
 * the built-in reader, exactly as Cyclone DDS does, so a Cyclone reader renews
 * its lease for the local writers it matched.
 */
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

/*
 * One ParticipantMessageData assertion from a peer renews the writer pairs it
 * matched. The kind has to match a liveliness assertion: anything else (an
 * unknown or vendor-specific kind) is ignored.
 */
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

/* LivelinessChangedStatus counts, over the writers matched to one reader.
 * Called with the participant lock held. */
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

/*
 * Automatic liveliness leases. A writer announces a lease and asserts it with
 * ParticipantMessageData; when the last assertion is older than the lease it
 * announced, it is no longer alive and every subscription matched to it reports
 * LivelinessChanged. Manual liveliness is not monitored: its assertions need an
 * application-level "assert" call this stack does not expose yet, so those
 * writers stay in whatever state they started in.
 */
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

/*
 * Repair incomplete fragment assemblies. Falling back to the 2 s timeout and a
 * whole-sample retransmit wastes both latency and bandwidth, so after a short
 * quiet period the reader asks for just the missing fragments.
 */
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

static void thread_main(void *user)
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

static void free_participant(nb_rtps_participant_t *participant)
{
    if (!participant) {
        return;
    }
    if (participant->started) {
        nb_rtps_participant_stop(participant);
    }
    /* The TCP module deregisters its sockets from the poller, so it has to go
     * before the poller itself. */
    nb_rtps_tcp_destroy(participant->tcp);
    if (participant->poller) {
        zn_poller_destroy(participant->poller);
    }
    if (participant->rx_sock) {
        zn_socket_destroy(participant->rx_sock);
    }
    if (participant->unicast_sock) {
        zn_socket_destroy(participant->unicast_sock);
    }
    if (participant->data_sock) {
        zn_socket_destroy(participant->data_sock);
    }
    if (participant->lock) {
        zb_mutex_destroy(participant->lock);
    }
    nb_rtps_reliable_writer_destroy(participant->publications_writer);
    nb_rtps_reliable_writer_destroy(participant->subscriptions_writer);
    if (participant->local_endpoints) {
        uint32_t i;

        for (i = 0; i < participant->max_local_endpoints; i++) {
            nb_rtps_reliable_writer_destroy(participant->local_endpoints[i].history);
            free(participant->local_endpoints[i].instances);
        }
    }
    if (participant->frag_assemblies) {
        size_t i;

        for (i = 0; i < participant->max_frag_assemblies; i++) {
            nb_rtps_frag_assembly_t *assembly = &participant->frag_assemblies[i];

            free(assembly->data);
            free(assembly->received);
        }
        free(participant->frag_assemblies);
    }
    free(participant->data_readers);
    free(participant->data_reader_used);
    free(participant->local_used);
    free(participant->remote_ep_used);
    free(participant->remotes_used);
    free(participant->local_endpoints);
    free(participant->remote_endpoints);
    free(participant->remotes);
    free(participant);
}

nb_rtps_participant_t *nb_rtps_participant_create(const nb_rtps_participant_config_t *config)
{
    nb_rtps_participant_t *participant;
    uint32_t domain;
    uint32_t spdp_port;
    uint32_t unicast_port = 0;
    uint32_t data_port = 0;
    uint64_t now;
    size_t i;
    char local_ip[NB_RTPS_PEER_HOST_MAX];
    uint16_t local_port = 0;

    if (!config) {
        return NULL;
    }
    participant = (nb_rtps_participant_t *)calloc(1, sizeof(*participant));
    if (!participant) {
        return NULL;
    }
    participant->domain_id = config->domain_id;
    participant->participant_id = config->participant_id;
    participant->standard_ports = config->standard_ports != 0;
    copy_str(participant->bind_host,
             sizeof(participant->bind_host),
             config->bind_host && config->bind_host[0] ? config->bind_host : "0.0.0.0");
    copy_str(participant->name, sizeof(participant->name), config->name);
    participant->announce_interval_ms =
        config->announce_interval_ms ? config->announce_interval_ms : NB_RTPS_DEFAULT_ANNOUNCE_MS;
    if (participant->announce_interval_ms < NB_RTPS_MIN_ANNOUNCE_MS) {
        participant->announce_interval_ms = NB_RTPS_MIN_ANNOUNCE_MS;
    }
    participant->lease_ms = config->lease_ms ? config->lease_ms : NB_RTPS_DEFAULT_LEASE_MS;
    if (participant->lease_ms < NB_RTPS_MIN_LEASE_MS) {
        participant->lease_ms = NB_RTPS_MIN_LEASE_MS;
    }
    if (participant->lease_ms > NB_RTPS_MAX_LEASE_MS) {
        participant->lease_ms = NB_RTPS_MAX_LEASE_MS;
    }
    participant->max_remotes =
        config->max_remote_participants ? config->max_remote_participants
                                        : NB_RTPS_DEFAULT_MAX_REMOTES;
    if (participant->max_remotes > 65536u) {
        participant->max_remotes = 65536u;
    }
    participant->max_local_endpoints =
        config->max_local_endpoints ? config->max_local_endpoints
                                    : NB_RTPS_DEFAULT_MAX_LOCAL_ENDPOINTS;
    if (participant->max_local_endpoints > NB_RTPS_MAX_LOCAL_ENDPOINTS) {
        participant->max_local_endpoints = NB_RTPS_MAX_LOCAL_ENDPOINTS;
    }
    participant->max_remote_endpoints =
        config->max_remote_endpoints ? config->max_remote_endpoints
                                     : NB_RTPS_DEFAULT_MAX_REMOTE_ENDPOINTS;
    if (participant->max_remote_endpoints > NB_RTPS_MAX_REMOTE_ENDPOINTS) {
        participant->max_remote_endpoints = NB_RTPS_MAX_REMOTE_ENDPOINTS;
    }
    participant->sedp_heartbeat_ms =
        config->sedp_heartbeat_ms ? config->sedp_heartbeat_ms
                                  : NB_RTPS_DEFAULT_SEDP_HEARTBEAT_MS;
    if (participant->sedp_heartbeat_ms < NB_RTPS_MIN_SEDP_HEARTBEAT_MS) {
        participant->sedp_heartbeat_ms = NB_RTPS_MIN_SEDP_HEARTBEAT_MS;
    }
    participant->data_heartbeat_ms =
        config->data_heartbeat_ms ? config->data_heartbeat_ms
                                  : NB_RTPS_DEFAULT_DATA_HEARTBEAT_MS;
    if (participant->data_heartbeat_ms < NB_RTPS_MIN_DATA_HEARTBEAT_MS) {
        participant->data_heartbeat_ms = NB_RTPS_MIN_DATA_HEARTBEAT_MS;
    }
    participant->data_history_depth =
        config->data_history_depth ? config->data_history_depth
                                   : NB_RTPS_DEFAULT_DATA_HISTORY;
    if (participant->data_history_depth > NB_RTPS_MAX_DATA_HISTORY) {
        participant->data_history_depth = NB_RTPS_MAX_DATA_HISTORY;
    }
    /* Frame dump: explicit config wins, otherwise the environment switch that
     * the rtps transport also honours. */
    participant->dump_frames = config->dump_frames;
    if (!participant->dump_frames) {
        const char *dump = getenv("NB_RTPS_DUMP_FRAMES");
        participant->dump_frames = dump && dump[0] && strcmp(dump, "0") != 0;
    }
    participant->default_unicast = config->default_unicast;
    if (config->peer_count > 0 && !config->peers) {
        free_participant(participant);
        return NULL;
    }
    if (config->peer_count > NB_RTPS_MAX_PEERS) {
        free_participant(participant);
        return NULL;
    }
    for (i = 0; i < config->peer_count; i++) {
        copy_str(participant->peers[i].host,
                 sizeof(participant->peers[i].host),
                 config->peers[i].host);
        participant->peers[i].port = config->peers[i].port;
    }
    participant->peer_count = config->peer_count;

    domain = config->domain_id;
    if (domain > 232u) {
        free_participant(participant);
        return NULL;
    }
    copy_str(participant->multicast_group,
             sizeof(participant->multicast_group),
             config->multicast_group ? config->multicast_group : "239.255.0.1");
    participant->multicast_enabled = participant->multicast_group[0] != '\0';
    if (config->multicast_group && config->multicast_group[0] == '\0') {
        participant->multicast_enabled = 0;
    }
    if (participant->multicast_enabled) {
        if (parse_ipv4(participant->multicast_group, participant->multicast_addr) != NB_OK) {
            free_participant(participant);
            return NULL;
        }
    }
    if (participant->standard_ports) {
        uint64_t base = (uint64_t)NB_RTPS_SPDP_BASE_PORT +
                        (uint64_t)NB_RTPS_SPDP_DOMAIN_GAIN * (uint64_t)domain;
        uint64_t meta = base + NB_RTPS_PORT_OFFSET_D1 +
                        NB_RTPS_PORT_PARTICIPANT_GAIN *
                            (uint64_t)participant->participant_id;
        uint64_t data = base + NB_RTPS_PORT_OFFSET_D3 +
                        NB_RTPS_PORT_PARTICIPANT_GAIN *
                            (uint64_t)participant->participant_id;

        /* The standard mapping needs the multicast SPDP socket; a unicast-only
         * participant must use explicit ports instead. */
        if (!participant->multicast_enabled || meta > 65535u || data > 65535u) {
            free_participant(participant);
            return NULL;
        }
        spdp_port = (uint32_t)(base + NB_RTPS_PORT_OFFSET_D0);
        unicast_port = (uint32_t)meta;
        data_port = (uint32_t)data;
    } else if (config->spdp_port) {
        spdp_port = config->spdp_port;
    } else if (participant->multicast_enabled) {
        spdp_port = NB_RTPS_SPDP_BASE_PORT + NB_RTPS_SPDP_DOMAIN_GAIN * domain;
    } else {
        spdp_port = 0;
    }
    if (config->spdp_port) {
        spdp_port = config->spdp_port;
    }
    if (config->unicast_port) {
        unicast_port = config->unicast_port;
    }
    if (spdp_port > 65535u || unicast_port > 65535u) {
        free_participant(participant);
        return NULL;
    }

    if (!config->advertise_host || !config->advertise_host[0]) {
        if (strcmp(participant->bind_host, "0.0.0.0") != 0) {
            copy_str(participant->advertise_host,
                     sizeof(participant->advertise_host),
                     participant->bind_host);
        } else {
            copy_str(participant->advertise_host,
                     sizeof(participant->advertise_host),
                     "127.0.0.1");
        }
    } else {
        copy_str(participant->advertise_host,
                 sizeof(participant->advertise_host),
                 config->advertise_host);
    }
    if (parse_ipv4(participant->advertise_host, participant->advertise_addr) != NB_OK) {
        free_participant(participant);
        return NULL;
    }

    participant->lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    participant->remotes =
        (nb_rtps_remote_participant_t *)calloc(participant->max_remotes,
                                               sizeof(nb_rtps_remote_participant_t));
    participant->local_endpoints =
        (nb_rtps_local_endpoint_t *)calloc(participant->max_local_endpoints,
                                           sizeof(nb_rtps_local_endpoint_t));
    participant->remote_endpoints =
        (nb_rtps_remote_endpoint_t *)calloc(participant->max_remote_endpoints,
                                            sizeof(nb_rtps_remote_endpoint_t));
    participant->local_used =
        (uint32_t *)calloc(participant->max_local_endpoints, sizeof(uint32_t));
    participant->remote_ep_used =
        (uint32_t *)calloc(participant->max_remote_endpoints, sizeof(uint32_t));
    participant->remotes_used =
        (uint32_t *)calloc(participant->max_remotes, sizeof(uint32_t));
    participant->publications_writer =
        nb_rtps_reliable_writer_create(participant->max_local_endpoints);
    participant->subscriptions_writer =
        nb_rtps_reliable_writer_create(participant->max_local_endpoints);
    if (!participant->lock || !participant->remotes || !participant->local_endpoints ||
        !participant->remote_endpoints || !participant->local_used ||
        !participant->remote_ep_used || !participant->remotes_used ||
        !participant->publications_writer ||
        !participant->subscriptions_writer) {
        free_participant(participant);
        return NULL;
    }

    participant->rx_sock = zn_socket_create();
    if (!participant->rx_sock) {
        free_participant(participant);
        return NULL;
    }
    if (zn_socket_bind_udp(participant->rx_sock,
                            participant->bind_host,
                            (uint16_t)spdp_port,
                            1) != ZN_OK) {
        free_participant(participant);
        return NULL;
    }
    local_ip[0] = '\0';
    if (zn_socket_get_local(participant->rx_sock,
                             local_ip,
                             sizeof(local_ip),
                             &local_port) != ZN_OK ||
        local_port == 0) {
        free_participant(participant);
        return NULL;
    }
    participant->spdp_port = local_port;
    if (participant->multicast_enabled) {
        const char *local_if =
            strcmp(participant->bind_host, "0.0.0.0") == 0 ? NULL : participant->bind_host;
        if (zn_socket_join_multicast(participant->rx_sock,
                                      participant->multicast_group,
                                      local_if) != ZN_OK) {
            free_participant(participant);
            return NULL;
        }
        (void)zn_socket_set_multicast_ttl(participant->rx_sock, 1);
        (void)zn_socket_set_multicast_loop(participant->rx_sock, 1);
    }

    participant->unicast_sock = zn_socket_create();
    if (!participant->unicast_sock) {
        free_participant(participant);
        return NULL;
    }
    if (zn_socket_bind_udp(participant->unicast_sock,
                            participant->bind_host,
                            (uint16_t)unicast_port,
                            0) != ZN_OK) {
        free_participant(participant);
        return NULL;
    }
    local_ip[0] = '\0';
    local_port = 0;
    if (zn_socket_get_local(participant->unicast_sock,
                             local_ip,
                             sizeof(local_ip),
                             &local_port) != ZN_OK ||
        local_port == 0) {
        free_participant(participant);
        return NULL;
    }
    participant->unicast_port = local_port;
    nb_rtps_locator_ipv4(&participant->metatraffic_unicast,
                         participant->advertise_addr[0],
                         participant->advertise_addr[1],
                         participant->advertise_addr[2],
                         participant->advertise_addr[3],
                         participant->unicast_port);
    if (participant->standard_ports) {
        /* User data gets its own unicast socket on d3 + PG*participant_id. The
         * metatraffic socket keeps SEDP and SPDP unicast; peers learn this
         * locator from PID_DEFAULT_UNICAST_LOCATOR. */
        participant->data_sock = zn_socket_create();
        if (!participant->data_sock ||
            zn_socket_bind_udp(participant->data_sock,
                                participant->bind_host,
                                (uint16_t)data_port,
                                0) != ZN_OK) {
            free_participant(participant);
            return NULL;
        }
        local_ip[0] = '\0';
        local_port = 0;
        if (zn_socket_get_local(participant->data_sock,
                                 local_ip,
                                 sizeof(local_ip),
                                 &local_port) != ZN_OK ||
            local_port == 0) {
            free_participant(participant);
            return NULL;
        }
        participant->data_port = local_port;
        nb_rtps_locator_ipv4(&participant->default_unicast,
                             participant->advertise_addr[0],
                             participant->advertise_addr[1],
                             participant->advertise_addr[2],
                             participant->advertise_addr[3],
                             participant->data_port);
    }
    if (participant->default_unicast.kind == NB_RTPS_LOCATOR_KIND_INVALID) {
        /* SPDP peers such as Cyclone DDS require a default unicast locator in
         * addition to the metatraffic locator before they create a proxy
         * participant. With no RTPS data plane yet, reuse the discovery
         * unicast endpoint. */
        participant->default_unicast = participant->metatraffic_unicast;
    }
    if (participant->multicast_enabled) {
        nb_rtps_locator_ipv4(&participant->metatraffic_multicast,
                             participant->multicast_addr[0],
                             participant->multicast_addr[1],
                             participant->multicast_addr[2],
                             participant->multicast_addr[3],
                             participant->spdp_port);
    }
    if (config->enable_tcp) {
        /* The listener owns the port before SPDP is built, so the locator we
         * advertise is the one we actually accept on. */
        participant->tcp = nb_rtps_tcp_create(participant->bind_host,
                                              config->tcp_port,
                                              NULL,
                                              tcp_on_message,
                                              tcp_on_link,
                                              tcp_on_unlink,
                                              participant);
        if (!participant->tcp) {
            free_participant(participant);
            return NULL;
        }
        nb_rtps_locator_ipv4(&participant->metatraffic_tcp,
                             participant->advertise_addr[0],
                             participant->advertise_addr[1],
                             participant->advertise_addr[2],
                             participant->advertise_addr[3],
                             nb_rtps_tcp_port(participant->tcp));
        participant->metatraffic_tcp.kind = NB_RTPS_LOCATOR_KIND_TCPV4;
    }

    now = zn_wall_ms();
    make_guid(participant, now);
    participant->rng = (uint32_t)(now ^ (uint32_t)(uintptr_t)participant);
    if (participant->rng == 0) {
        participant->rng = 1;
    }
    return participant;
}

int nb_rtps_participant_start(nb_rtps_participant_t *participant)
{
    if (!participant || participant->started) {
        return NB_ERR_STATE;
    }
    participant->poller = zn_poller_create();
    if (!participant->poller) {
        return NB_ERR_NOMEM;
    }
    if (zn_poller_add(participant->poller,
                       zn_socket_fd(participant->rx_sock),
                       ZN_POLL_IN,
                       NULL,
                       NULL) != ZN_OK ||
        zn_poller_add(participant->poller,
                       zn_socket_fd(participant->unicast_sock),
                       ZN_POLL_IN,
                       NULL,
                       NULL) != ZN_OK) {
        zn_poller_destroy(participant->poller);
        participant->poller = NULL;
        return NB_ERR_IO;
    }
    if (participant->data_sock &&
        zn_poller_add(participant->poller,
                       zn_socket_fd(participant->data_sock),
                       ZN_POLL_IN,
                       NULL,
                       NULL) != ZN_OK) {
        zn_poller_destroy(participant->poller);
        participant->poller = NULL;
        return NB_ERR_IO;
    }
    if (participant->tcp &&
        nb_rtps_tcp_set_poller(participant->tcp, participant->poller) != 0) {
        zn_poller_destroy(participant->poller);
        participant->poller = NULL;
        return NB_ERR_IO;
    }
    participant->stop = 0;
    participant->next_announce_ms = 0;
    participant->next_sweep_ms = 0;
    participant->next_sedp_heartbeat_ms = 0;
    participant->next_data_heartbeat_ms = 0;
    zb_mutex_lock(participant->lock);
    refresh_pmd_interval(participant);
    zb_mutex_unlock(participant->lock);
    participant->thread = zb_thread_create(thread_main, participant);
    if (!participant->thread) {
        if (participant->tcp) {
            (void)nb_rtps_tcp_set_poller(participant->tcp, NULL);
        }
        zn_poller_destroy(participant->poller);
        participant->poller = NULL;
        return NB_ERR_NOMEM;
    }
    participant->started = 1;
    return NB_OK;
}

void nb_rtps_participant_stop(nb_rtps_participant_t *participant)
{
    uint32_t i;

    if (!participant || !participant->started) {
        return;
    }
    participant->stop = 1;
    if (participant->poller) {
        (void)zn_poller_wake(participant->poller);
    }
    if (participant->thread) {
        (void)zb_thread_join(participant->thread);
        zb_thread_destroy(participant->thread);
        participant->thread = NULL;
    }
    /* The TCP sockets are registered in the poller that is about to go away;
     * detach them first so nothing references it afterwards. */
    if (participant->tcp) {
        (void)nb_rtps_tcp_set_poller(participant->tcp, NULL);
    }
    if (participant->poller) {
        zn_poller_destroy(participant->poller);
        participant->poller = NULL;
    }
    participant->started = 0;
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remotes; i++) {
        participant->remotes[i].used = 0;
    }
    for (i = 0; i < participant->max_remote_endpoints; i++) {
        participant->remote_endpoints[i].used = 0;
    }
    participant->remotes_used_count = 0;
    participant->remote_ep_used_count = 0;
    zb_mutex_unlock(participant->lock);
}

void nb_rtps_participant_destroy(nb_rtps_participant_t *participant)
{
    free_participant(participant);
}

void nb_rtps_participant_set_callback(nb_rtps_participant_t *participant,
                                      nb_rtps_participant_callback_t callback,
                                      void *user)
{
    if (!participant) {
        return;
    }
    zb_mutex_lock(participant->lock);
    participant->callback = callback;
    participant->callback_user = user;
    zb_mutex_unlock(participant->lock);
}

void nb_rtps_participant_set_endpoint_callback(nb_rtps_participant_t *participant,
                                               nb_rtps_endpoint_callback_t callback,
                                               void *user)
{
    if (!participant) {
        return;
    }
    zb_mutex_lock(participant->lock);
    participant->endpoint_callback = callback;
    participant->endpoint_user = user;
    zb_mutex_unlock(participant->lock);
}

void nb_rtps_participant_set_data_callback(nb_rtps_participant_t *participant,
                                           nb_rtps_data_callback_t callback,
                                           void *user)
{
    if (!participant) {
        return;
    }
    zb_mutex_lock(participant->lock);
    participant->data_callback = callback;
    participant->data_user = user;
    zb_mutex_unlock(participant->lock);
}

int nb_rtps_participant_set_status_callback(nb_rtps_participant_t *participant,
                                            nb_rtps_status_callback_t callback,
                                            void *user)
{
    if (!participant) {
        return NB_ERR_INVALID;
    }
    zb_mutex_lock(participant->lock);
    participant->status_callback = callback;
    participant->status_user = user;
    zb_mutex_unlock(participant->lock);
    return NB_OK;
}

int nb_rtps_participant_add_endpoint(nb_rtps_participant_t *participant,
                                     nb_rtps_endpoint_kind_t kind,
                                     nb_rtps_endpoint_info_t *info)
{
    nb_rtps_local_endpoint_t local;
    nb_rtps_reliable_writer_t *history;
    uint8_t payload[NB_RTPS_SEDP_PAYLOAD_MAX];
    size_t payload_len = 0;
    size_t slot = 0;
    size_t i;
    uint64_t sn = 0;
    int rc;

    if (!participant || !info ||
        (kind != NB_RTPS_ENDPOINT_PUBLICATION && kind != NB_RTPS_ENDPOINT_SUBSCRIPTION) ||
        info->topic_name[0] == '\0' || info->type_name[0] == '\0') {
        return NB_ERR_INVALID;
    }
    memset(&local, 0, sizeof(local));
    local.used = 1;
    local.kind = kind;
    local.info = *info;
    local.info.endpoint_guid.prefix = participant->local_guid.prefix;
    if (kind == NB_RTPS_ENDPOINT_SUBSCRIPTION) {
        local.max_instances = info->max_instances;
    }

    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_local_endpoints; i++) {
        if (!participant->local_endpoints[i].used) {
            slot = i;
            break;
        }
        if (i + 1u == participant->max_local_endpoints) {
            zb_mutex_unlock(participant->lock);
            return NB_ERR_TOOBIG;
        }
    }
    allocate_local_entity(participant, kind, &local.info.endpoint_guid.entity);
    if (kind == NB_RTPS_ENDPOINT_SUBSCRIPTION) {
        /* Ask keyed Peers for instance handles; harmless for peers that do not
         * understand the vendor parameter. */
        local.info.requests_keyhash = 1;
    }
    if (local.info.unicast_count == 0) {
        const nb_rtps_locator_t *fallback = NULL;

        if (participant->default_unicast.kind == NB_RTPS_LOCATOR_KIND_UDPV4 &&
            participant->default_unicast.port != 0) {
            fallback = &participant->default_unicast;
        } else if (participant->metatraffic_unicast.kind == NB_RTPS_LOCATOR_KIND_UDPV4 &&
                   participant->metatraffic_unicast.port != 0) {
            fallback = &participant->metatraffic_unicast;
        }
        if (fallback) {
            local.info.unicast[0] = *fallback;
            local.info.unicast_count = 1;
        }
    }
    if (nb_rtps_sedp_build(&local.info, payload, sizeof(payload), &payload_len) != NB_OK) {
        zb_mutex_unlock(participant->lock);
        return NB_ERR_BAD_MESSAGE;
    }
    /* Reliable writers need a history for retransmission. A durable writer
     * needs one even when it is best effort, so late joiners can be served. */
    if (kind == NB_RTPS_ENDPOINT_PUBLICATION &&
        (local.info.reliability == NB_RTPS_RELIABILITY_RELIABLE ||
         local.info.durability >= NB_RTPS_DURABILITY_TRANSIENT_LOCAL)) {
        uint32_t depth = local.info.history_depth
                             ? local.info.history_depth
                             : participant->data_history_depth;

        /* KEEP_ALL means "keep everything the memory bound allows", so it is not
         * tied to the KEEP_LAST depth. The byte budget still caps the total. */
        if (local.info.history_kind == NB_RTPS_HISTORY_KEEP_ALL) {
            depth = NB_RTPS_MAX_DATA_HISTORY;
        }
        local.history = nb_rtps_reliable_writer_create(depth);
        if (!local.history) {
            zb_mutex_unlock(participant->lock);
            return NB_ERR_NOMEM;
        }
    }
    history = kind == NB_RTPS_ENDPOINT_PUBLICATION ? participant->publications_writer
                                                   : participant->subscriptions_writer;
    rc = nb_rtps_reliable_add(history, payload, payload_len, 0, &sn);
    if (rc != NB_OK) {
        nb_rtps_reliable_writer_destroy(local.history);
        zb_mutex_unlock(participant->lock);
        return rc;
    }
    local.sn = sn;
    /* A writer that never writes misses its first deadline one period after it
     * was created; the sweep counts from here. */
    local.last_write_ms = zn_wall_ms();
    local.next_deadline_ms = 0;
    participant->local_endpoints[slot] = local;
    nb_rtps_used_add(participant->local_used,
                     &participant->local_used_count,
                     participant->max_local_endpoints,
                     slot);
    participant->local_endpoint_count++;
    refresh_pmd_interval(participant);
    *info = local.info;
    zb_mutex_unlock(participant->lock);

    /* Manual liveliness needs an explicit assertion this API does not offer, so
     * a peer would declare the writer dead once the lease runs out. Say so
     * rather than announcing a promise that cannot be kept. */
    send_local_endpoint_to_remotes(participant, &local);
    notify_matches_for_local(participant, &local.info.endpoint_guid);
    return NB_OK;
}

int nb_rtps_participant_remove_endpoint(nb_rtps_participant_t *participant,
                                        const nb_rtps_guid_t *endpoint_guid)
{
    nb_rtps_local_endpoint_t local;
    nb_rtps_reliable_writer_t *history;
    nb_rtps_local_endpoint_t *slot;
    uint8_t *dispose_handles = NULL;
    size_t dispose_count = 0;
    uint8_t payload[NB_RTPS_SEDP_PAYLOAD_MAX];
    size_t payload_len = 0;
    uint64_t sn = 0;
    size_t i;
    int rc;

    if (!participant || !endpoint_guid) {
        return NB_ERR_INVALID;
    }
    zb_mutex_lock(participant->lock);
    slot = find_local_endpoint(participant, endpoint_guid);
    if (!slot) {
        zb_mutex_unlock(participant->lock);
        return NB_ERR_NOTFOUND;
    }
    local = *slot;
    /* WriterDataLifecycle: a writer that unregisters disposes the instances it
     * still has alive. Collect them now, publish after the lock is released,
     * then take the endpoint down. */
    if (local.kind == NB_RTPS_ENDPOINT_PUBLICATION && local.info.autodispose &&
        local.instances && local.instance_count > 0) {
        dispose_handles =
            (uint8_t *)malloc((size_t)local.instance_count * NB_INSTANCE_HANDLE_SIZE);
        if (dispose_handles) {
            for (i = 0; i < local.instance_count; i++) {
                if (!local.instances[i].used ||
                    local.instances[i].state != NB_INSTANCE_STATE_ALIVE) {
                    continue;
                }
                memcpy(dispose_handles + dispose_count * NB_INSTANCE_HANDLE_SIZE,
                       local.instances[i].handle,
                       NB_INSTANCE_HANDLE_SIZE);
                dispose_count++;
            }
        }
    }
    zb_mutex_unlock(participant->lock);

    for (i = 0; i < dispose_count; i++) {
        (void)nb_rtps_participant_publish_instance(
            participant,
            endpoint_guid,
            dispose_handles + i * NB_INSTANCE_HANDLE_SIZE,
            1,
            NULL,
            0);
    }
    free(dispose_handles);

    zb_mutex_lock(participant->lock);
    slot = find_local_endpoint(participant, endpoint_guid);
    if (!slot) {
        /* Somebody else removed it while the disposes were in flight. */
        zb_mutex_unlock(participant->lock);
        return NB_ERR_NOTFOUND;
    }
    local = *slot;
    slot->used = 0;
    nb_rtps_used_remove(participant->local_used,
                        &participant->local_used_count,
                        (size_t)(slot - participant->local_endpoints));
    /* Ownership of the history moves to the local copy; clearing the slot
     * keeps participant destruction from freeing it twice. */
    slot->history = NULL;
    slot->instances = NULL;
    if (participant->local_endpoint_count > 0) {
        participant->local_endpoint_count--;
    }
    if (local.kind == NB_RTPS_ENDPOINT_SUBSCRIPTION) {
        drop_data_readers_for_reader(participant, &local.info.endpoint_guid.entity);
    }
    refresh_pmd_interval(participant);
    zb_mutex_unlock(participant->lock);

    if (nb_rtps_sedp_build_key(
            &local.info.endpoint_guid, payload, sizeof(payload), &payload_len) != NB_OK) {
        nb_rtps_reliable_writer_destroy(local.history);
        free(local.instances);
        return NB_ERR_BAD_MESSAGE;
    }
    nb_rtps_reliable_writer_destroy(local.history);
    free(local.instances);
    history = local.kind == NB_RTPS_ENDPOINT_PUBLICATION ? participant->publications_writer
                                                         : participant->subscriptions_writer;
    rc = nb_rtps_reliable_add(history, payload, payload_len, 1, &sn);
    if (rc != NB_OK) {
        return rc;
    }
    broadcast_sedp_sample(
        participant, local.kind, sn, payload, payload_len, 1);
    return NB_OK;
}

/*
 * source_ns == 0 means "stamp with the current wall clock". A non-zero value is
 * the source timestamp the reader uses for BY_SOURCE destination ordering; the
 * reliable history keeps it so retransmissions do not look like new data.
 *
 * The matching readers and their destinations are snapshotted under the
 * participant lock and the datagrams are sent after it is released: a send no
 * longer blocks the receive thread for the duration of a syscall. More matched
 * readers than the snapshot holds are sent inline, so the common case needs no
 * allocation.
 */
#define NB_RTPS_PUBLISH_BATCH 8u

typedef struct nb_rtps_publish_send {
    nb_rtps_guid_t reader;
    nb_rtps_targets_t targets;
} nb_rtps_publish_send_t;

static int publish_internal(nb_rtps_participant_t *participant,
                            const nb_rtps_guid_t *writer_guid,
                            uint64_t source_ns,
                            const uint8_t *handle,
                            uint32_t status,
                            const void *payload,
                            size_t payload_len)
{
    nb_rtps_local_endpoint_t *local;
    nb_rtps_publish_send_t batch[NB_RTPS_PUBLISH_BATCH];
    size_t batch_count = 0;
    uint64_t sn = 0;
    uint64_t evicted_first = 0;
    uint64_t evicted_last = 0;
    uint64_t first_sn = 0;
    size_t i;
    int target_count = 0;
    int sent = 0;
    int last_rc = NB_ERR_IO;
    int assert_participant_liveliness = 0;

    if (!participant || !writer_guid || !payload || payload_len == 0 ||
        payload_len > NB_RTPS_SAMPLE_MAX) {
        return NB_ERR_INVALID;
    }

    zb_mutex_lock(participant->lock);
    local = find_local_endpoint(participant, writer_guid);
    if (!local || local->kind != NB_RTPS_ENDPOINT_PUBLICATION) {
        zb_mutex_unlock(participant->lock);
        return NB_ERR_NOTFOUND;
    }

    if (local->history) {
        /* nb_rtps_reliable_state() writes full uint64_t sequence numbers; a
         * narrower type here would write past the variable and corrupt the
         * neighbouring `old_count`, which is what decides whether a GAP has to
         * announce evicted samples. */
        uint64_t old_first = 0;
        uint64_t old_last = 0;
        size_t old_count = 0;
        int rc;

        nb_rtps_reliable_state(local->history, &old_first, &old_last, &old_count);
        rc = nb_rtps_reliable_add_instance(local->history,
                                           payload,
                                           payload_len,
                                           status != 0,
                                           source_ns,
                                           handle,
                                           status,
                                           &sn);
        if (rc != NB_OK) {
            zb_mutex_unlock(participant->lock);
            return rc;
        }
        nb_rtps_reliable_state(local->history, &first_sn, NULL, NULL);
        /* nb_rtps_reliable_add drops the oldest sample when the history is
         * full; the reader must learn that it will never arrive. */
        if (old_count > 0 && first_sn > old_first) {
            evicted_first = old_first;
            evicted_last = first_sn - 1u;
        }
    } else {
        sn = local->data_sn + 1u;
        if (sn == 0) {
            sn = 1;
        }
        local->data_sn = sn;
        first_sn = sn;
    }
    /* Offered deadline is measured from the last write, not from the last
     * delivery, so it is independent of how many readers are matched. */
    local->last_write_ms = zn_wall_ms();

    /* Remember the instances this writer registered, so that deleting the
     * writer can dispose them when WriterDataLifecycle asks for it. */
    if (handle) {
        nb_rtps_instance_t *instance =
            get_instance(local, handle, &local->info.endpoint_guid);

        if (instance) {
            if (status == 1) {
                instance->state = NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED;
            } else if (status != 0) {
                instance->state = NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS;
            } else {
                instance->state = NB_INSTANCE_STATE_ALIVE;
                instance->seen = 1;
            }
        }
    }

    for (i = 0; i < participant->remote_ep_used_count; i++) {
        nb_rtps_remote_endpoint_t *remote =
            &participant->remote_endpoints[participant->remote_ep_used[i]];
        nb_rtps_targets_t targets;

        if (remote->kind != NB_RTPS_ENDPOINT_SUBSCRIPTION) {
            continue;
        }
        if (!endpoint_qos_compatible(&local->info, &remote->info)) {
            continue;
        }
        if (endpoint_data_targets(participant, remote, &targets) == 0) {
            continue;
        }
        target_count++;
        /* A write asserts the writer's liveliness. Manual-by-topic asserts with
         * a per-writer heartbeat; manual-by-participant is one participant
         * message for all its manual writers, sent once after the lock. Both
         * are opt-in, so they stay on the synchronous path. */
        if (local->info.liveliness_kind == NB_RTPS_LIVELINESS_MANUAL_BY_TOPIC &&
            local->info.liveliness_lease_ms > 0) {
            size_t t;

            for (t = 0; t < targets.count; t++) {
                send_user_liveliness_heartbeat(participant,
                                               local,
                                               &remote->info,
                                               targets.host[t],
                                               targets.port[t]);
            }
        } else if (local->info.liveliness_kind ==
                       NB_RTPS_LIVELINESS_MANUAL_BY_PARTICIPANT &&
                   local->info.liveliness_lease_ms > 0) {
            assert_participant_liveliness = 1;
        }
        if (batch_count < NB_RTPS_PUBLISH_BATCH) {
            batch[batch_count].reader = remote->info.endpoint_guid;
            batch[batch_count].targets = targets;
            batch_count++;
        } else {
            /* More matched readers than the snapshot holds: send this one here
             * rather than allocate. */
            if (evicted_first) {
                send_user_gap_all(participant,
                                  &local->info.endpoint_guid,
                                  &remote->info.endpoint_guid,
                                  evicted_first,
                                  evicted_last,
                                  &targets);
            }
            last_rc = send_user_data_all(participant,
                                         &local->info.endpoint_guid,
                                         &remote->info.endpoint_guid,
                                         sn,
                                         source_ns,
                                         payload,
                                         payload_len,
                                         handle,
                                         status,
                                         &targets);
            if (last_rc == NB_OK) {
                sent++;
            }
        }
    }
    zb_mutex_unlock(participant->lock);

    for (i = 0; i < batch_count; i++) {
        if (evicted_first) {
            send_user_gap_all(participant,
                              &local->info.endpoint_guid,
                              &batch[i].reader,
                              evicted_first,
                              evicted_last,
                              &batch[i].targets);
        }
        last_rc = send_user_data_all(participant,
                                     &local->info.endpoint_guid,
                                     &batch[i].reader,
                                     sn,
                                     source_ns,
                                     payload,
                                     payload_len,
                                     handle,
                                     status,
                                     &batch[i].targets);
        if (last_rc == NB_OK) {
            sent++;
        }
    }

    if (assert_participant_liveliness) {
        send_pmd(participant, NB_RTPS_PMD_KIND_MANUAL);
    }

    if (target_count == 0) {
        return NB_ERR_NOTFOUND;
    }
    return sent > 0 ? NB_OK : last_rc;
}

int nb_rtps_participant_publish(nb_rtps_participant_t *participant,
                                const nb_rtps_guid_t *writer_guid,
                                const void *payload,
                                size_t payload_len)
{
    return publish_internal(
        participant, writer_guid, 0, NULL, 0, payload, payload_len);
}

int nb_rtps_participant_publish_at(nb_rtps_participant_t *participant,
                                   const nb_rtps_guid_t *writer_guid,
                                   const void *payload,
                                   size_t payload_len,
                                   const nb_rtps_time_t *source_ts)
{
    return publish_internal(participant,
                            writer_guid,
                            rtps_time_to_ns(source_ts),
                            NULL,
                            0,
                            payload,
                            payload_len);
}

/*
 * Publish a keyed sample. `status` is the DDS PID_STATUS_INFO value: 0 for a
 * live sample, 1 to dispose the instance and 2 to unregister it. For a state
 * change the payload should be the serialized key (the usual four-byte
 * encapsulation header followed by the key fields); an empty payload is sent as
 * an empty CDR value, which a peer that only needs the instance handle can
 * still use.
 */
int nb_rtps_participant_publish_instance(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer_guid,
    const uint8_t handle[NB_INSTANCE_HANDLE_SIZE],
    uint32_t status,
    const void *payload,
    size_t payload_len)
{
    uint8_t empty[4] = {0x00, 0x01, 0x00, 0x00};

    if (!handle) {
        return NB_ERR_INVALID;
    }
    if ((!payload || payload_len == 0) && status != 0) {
        payload = empty;
        payload_len = sizeof(empty);
    }
    return publish_internal(
        participant, writer_guid, 0, handle, status, payload, payload_len);
}

int nb_rtps_participant_reader_consumed(nb_rtps_participant_t *participant,
                                        const nb_rtps_guid_t *writer_guid,
                                        const nb_rtps_guid_t *reader_guid,
                                        uint64_t sn)
{
    nb_rtps_data_reader_state_t *state;
    int consumed;

    if (!participant || !writer_guid || !reader_guid || sn == 0) {
        return 0;
    }
    zb_mutex_lock(participant->lock);
    state = find_data_reader(participant, writer_guid, &reader_guid->entity);
    consumed = state ? nb_rtps_reader_has_sample(&state->state, sn) : 0;
    zb_mutex_unlock(participant->lock);
    return consumed;
}

int nb_rtps_participant_publish_dds_message(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer_guid,
    const nb_rtps_dds_message_t *message)
{
    uint8_t payload[NB_RTPS_DATA_PAYLOAD_MAX];
    size_t payload_len = 0;
    int rc;

    if (!message) {
        return NB_ERR_INVALID;
    }
    rc = nb_rtps_dds_message_serialize(
        message, payload, sizeof(payload), &payload_len);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_rtps_participant_publish(
        participant, writer_guid, payload, payload_len);
}

int nb_rtps_participant_add_peer(nb_rtps_participant_t *participant,
                                 const char *host,
                                 uint16_t port)
{
    if (!participant || !host || host[0] == '\0' || port == 0) {
        return NB_ERR_INVALID;
    }
    if (participant->started) {
        return NB_ERR_STATE;
    }
    if (participant->peer_count >= NB_RTPS_MAX_PEERS) {
        return NB_ERR_TOOBIG;
    }
    zb_mutex_lock(participant->lock);
    copy_str(participant->peers[participant->peer_count].host,
             sizeof(participant->peers[participant->peer_count].host),
             host);
    participant->peers[participant->peer_count].port = port;
    participant->peer_count++;
    zb_mutex_unlock(participant->lock);
    return NB_OK;
}

uint16_t nb_rtps_participant_spdp_port(const nb_rtps_participant_t *participant)
{
    return participant ? participant->spdp_port : 0;
}

size_t nb_rtps_participant_remote_count(const nb_rtps_participant_t *participant)
{
    size_t count = 0;
    uint32_t i;

    if (!participant) {
        return 0;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remotes; i++) {
        if (participant->remotes[i].used) {
            count++;
        }
    }
    zb_mutex_unlock(participant->lock);
    return count;
}

size_t nb_rtps_participant_local_endpoint_count(const nb_rtps_participant_t *participant)
{
    size_t count;

    if (!participant) {
        return 0;
    }
    zb_mutex_lock(participant->lock);
    count = participant->local_endpoint_count;
    zb_mutex_unlock(participant->lock);
    return count;
}

size_t nb_rtps_participant_remote_endpoint_count(const nb_rtps_participant_t *participant)
{
    size_t count = 0;
    size_t i;

    if (!participant) {
        return 0;
    }
    zb_mutex_lock(participant->lock);
    for (i = 0; i < participant->max_remote_endpoints; i++) {
        if (participant->remote_endpoints[i].used) {
            count++;
        }
    }
    zb_mutex_unlock(participant->lock);
    return count;
}

int nb_rtps_participant_announce(nb_rtps_participant_t *participant)
{
    if (!participant || !participant->started) {
        return NB_ERR_STATE;
    }
    return send_spdp(participant);
}

size_t nb_rtps_participant_matched_count(const nb_rtps_participant_t *participant,
                                         const nb_rtps_guid_t *endpoint_guid)
{
    nb_rtps_participant_t *mutable_participant =
        (nb_rtps_participant_t *)participant;
    size_t count = 0;

    if (!participant || !endpoint_guid) {
        return 0;
    }
    zb_mutex_lock(mutable_participant->lock);
    {
        const nb_rtps_local_endpoint_t *local =
            find_local_endpoint(mutable_participant, endpoint_guid);

        if (local) {
            count = matched_remote_count(mutable_participant, local);
        }
    }
    zb_mutex_unlock(mutable_participant->lock);
    return count;
}

int nb_rtps_participant_local_info(const nb_rtps_participant_t *participant,
                                   nb_rtps_participant_info_t *out)
{
    if (!participant || !out) {
        return NB_ERR_INVALID;
    }
    fill_local_info(participant, out);
    return NB_OK;
}

/* ---- Discovery views (the DCPS built-in topics) ---- */

static void nb_rtps_discovery_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, cap, "%s", src);
}

static void nb_rtps_discovery_endpoint(nb_dds_discovery_sample_t *sample,
                                       int topic,
                                       const nb_rtps_endpoint_info_t *info,
                                       int is_local)
{
    memset(sample, 0, sizeof(*sample));
    sample->topic = (nb_dds_builtin_topic_t)topic;
    memcpy(sample->handle, &info->endpoint_guid, NB_DDS_DISCOVERY_HANDLE_SIZE);
    nb_rtps_discovery_copy(sample->name, sizeof(sample->name), info->topic_name);
    nb_rtps_discovery_copy(sample->type_name, sizeof(sample->type_name), info->type_name);
    nb_rtps_discovery_copy(sample->partition, sizeof(sample->partition), info->partition);
    sample->is_local = is_local;
    sample->is_writer = topic == NB_DDS_BUILTIN_TOPIC_PUBLICATION;
    sample->reliability = info->reliability;
    sample->durability = info->durability;
    sample->history_kind = info->history_kind;
    sample->history_depth = info->history_depth;
    sample->deadline_ms = info->deadline_ms;
    sample->lifespan_ms = info->lifespan_ms;
    sample->liveliness_kind = info->liveliness_kind;
    sample->liveliness_lease_ms = info->liveliness_lease_ms;
    sample->ownership_strength = (int32_t)info->ownership_strength;
}

/* Fold one endpoint into the derived DCPSTopic table. */
static void nb_rtps_discovery_topic_add(nb_dds_discovery_sample_t *out,
                                        size_t *count,
                                        size_t max,
                                        const nb_rtps_endpoint_info_t *info,
                                        int is_local,
                                        int is_writer)
{
    size_t i;
    uint32_t hash = 2166136261u;
    const char *text;

    if (!info->topic_name[0]) {
        return;
    }
    for (i = 0; i < *count; i++) {
        if (strcmp(out[i].name, info->topic_name) == 0 &&
            strcmp(out[i].type_name, info->type_name) == 0) {
            if (is_writer) {
                out[i].writer_count++;
            } else {
                out[i].reader_count++;
            }
            if (is_local) {
                out[i].is_local = 1;
            }
            return;
        }
    }
    if (*count >= max) {
        return;
    }
    memset(&out[*count], 0, sizeof(out[*count]));
    out[*count].topic = NB_DDS_BUILTIN_TOPIC_TOPIC;
    nb_rtps_discovery_copy(out[*count].name, sizeof(out[*count].name), info->topic_name);
    nb_rtps_discovery_copy(out[*count].type_name, sizeof(out[*count].type_name), info->type_name);
    out[*count].is_local = is_local;
    if (is_writer) {
        out[*count].writer_count = 1;
    } else {
        out[*count].reader_count = 1;
    }
    /* DDS derives the topic handle from name + type; a stable FNV-1a hash
     * over both is enough for equality tests. */
    for (text = info->topic_name; *text; text++) {
        hash = (hash ^ (uint8_t)*text) * 16777619u;
    }
    for (text = info->type_name; *text; text++) {
        hash = (hash ^ (uint8_t)*text) * 16777619u;
    }
    memcpy(out[*count].handle, &hash, sizeof(hash));
    (*count)++;
}

size_t nb_rtps_participant_discovery_read(nb_rtps_participant_t *participant,
                                          int topic,
                                          nb_dds_discovery_sample_t *out,
                                          size_t max)
{
    size_t count = 0;
    size_t i;

    if (!participant || !out || max == 0 || topic < NB_DDS_BUILTIN_TOPIC_PARTICIPANT ||
        topic > NB_DDS_BUILTIN_TOPIC_TOPIC) {
        return 0;
    }
    zb_mutex_lock(participant->lock);
    if (topic == NB_DDS_BUILTIN_TOPIC_PARTICIPANT) {
        if (count < max) {
            nb_dds_discovery_sample_t *sample = &out[count++];
            memset(sample, 0, sizeof(*sample));
            sample->topic = NB_DDS_BUILTIN_TOPIC_PARTICIPANT;
            memcpy(sample->handle, &participant->local_guid, NB_DDS_DISCOVERY_HANDLE_SIZE);
            nb_rtps_discovery_copy(sample->name, sizeof(sample->name), participant->name);
            nb_rtps_discovery_copy(
                sample->host, sizeof(sample->host), participant->advertise_host);
            sample->port = participant->spdp_port;
            sample->is_local = 1;
            sample->lease_ms = participant->lease_ms;
        }
        for (i = 0; i < participant->max_remotes && count < max; i++) {
            nb_rtps_remote_participant_t *remote = &participant->remotes[i];
            nb_dds_discovery_sample_t *sample;
            if (!remote->used) {
                continue;
            }
            sample = &out[count++];
            memset(sample, 0, sizeof(*sample));
            sample->topic = NB_DDS_BUILTIN_TOPIC_PARTICIPANT;
            memcpy(sample->handle, &remote->info.guid, NB_DDS_DISCOVERY_HANDLE_SIZE);
            nb_rtps_discovery_copy(sample->name, sizeof(sample->name), remote->info.name);
            nb_rtps_discovery_copy(sample->host, sizeof(sample->host), remote->source_host);
            sample->port = remote->source_port;
            sample->lease_ms = remote->lease_ms;
        }
    } else if (topic == NB_DDS_BUILTIN_TOPIC_TOPIC) {
        for (i = 0; i < participant->max_local_endpoints; i++) {
            nb_rtps_local_endpoint_t *endpoint = &participant->local_endpoints[i];
            if (endpoint->used) {
                nb_rtps_discovery_topic_add(out,
                                            &count,
                                            max,
                                            &endpoint->info,
                                            1,
                                            endpoint->kind == NB_RTPS_ENDPOINT_PUBLICATION);
            }
        }
        for (i = 0; i < participant->max_remote_endpoints; i++) {
            nb_rtps_remote_endpoint_t *endpoint = &participant->remote_endpoints[i];
            if (endpoint->used) {
                nb_rtps_discovery_topic_add(out,
                                            &count,
                                            max,
                                            &endpoint->info,
                                            0,
                                            endpoint->kind == NB_RTPS_ENDPOINT_PUBLICATION);
            }
        }
    } else {
        nb_rtps_endpoint_kind_t kind = topic == NB_DDS_BUILTIN_TOPIC_PUBLICATION
                                           ? NB_RTPS_ENDPOINT_PUBLICATION
                                           : NB_RTPS_ENDPOINT_SUBSCRIPTION;
        for (i = 0; i < participant->max_local_endpoints && count < max; i++) {
            nb_rtps_local_endpoint_t *endpoint = &participant->local_endpoints[i];
            if (endpoint->used && endpoint->kind == kind) {
                nb_rtps_discovery_endpoint(&out[count++], topic, &endpoint->info, 1);
            }
        }
        for (i = 0; i < participant->max_remote_endpoints && count < max; i++) {
            nb_rtps_remote_endpoint_t *endpoint = &participant->remote_endpoints[i];
            if (endpoint->used && endpoint->kind == kind) {
                nb_rtps_discovery_endpoint(&out[count++], topic, &endpoint->info, 0);
            }
        }
    }
    zb_mutex_unlock(participant->lock);
    return count;
}

int nb_rtps_participant_tcp_enabled(const nb_rtps_participant_t *participant)
{
    return participant && participant->tcp != NULL;
}

uint16_t nb_rtps_participant_tcp_port(const nb_rtps_participant_t *participant)
{
    return participant && participant->tcp ? nb_rtps_tcp_port(participant->tcp) : 0;
}

size_t nb_rtps_participant_tcp_links(const nb_rtps_participant_t *participant)
{
    return participant && participant->tcp ? nb_rtps_tcp_links(participant->tcp) : 0;
}

void nb_rtps_participant_tcp_stats(const nb_rtps_participant_t *participant,
                                   nb_rtps_tcp_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (participant && participant->tcp) {
        nb_rtps_tcp_stats(participant->tcp, out);
    }
}

void nb_rtps_participant_metrics(const nb_rtps_participant_t *participant, nb_metrics_t *out)
{
    size_t i;

    if (!participant || !out) {
        return;
    }
    zb_mutex_lock((zb_mutex *)participant->lock);
    out->rtps_packets_sent = participant->stats.packets_sent;
    out->rtps_packets_received = participant->stats.packets_received;
    out->rtps_bytes_sent = participant->stats.bytes_sent;
    out->rtps_bytes_received = participant->stats.bytes_received;
    out->rtps_spdp_sent = participant->stats.spdp_sent;
    out->rtps_spdp_received = participant->stats.spdp_received;
    out->rtps_sedp_sent = participant->stats.sedp_sent;
    out->rtps_sedp_received = participant->stats.sedp_received;
    out->rtps_data_sent = participant->stats.data_sent;
    out->rtps_data_received = participant->stats.data_received;
    out->rtps_heartbeats_sent = participant->stats.heartbeats_sent;
    out->rtps_heartbeats_received = participant->stats.heartbeats_received;
    out->rtps_acknacks_sent = participant->stats.acknacks_sent;
    out->rtps_acknacks_received = participant->stats.acknacks_received;
    out->rtps_gaps_sent = participant->stats.gaps_sent;
    out->rtps_gaps_received = participant->stats.gaps_received;
    out->rtps_fragments_sent = participant->stats.fragments_sent;
    out->rtps_fragments_received = participant->stats.fragments_received;
    out->rtps_nack_frags_sent = participant->stats.nack_frags_sent;
    out->rtps_nack_frags_received = participant->stats.nack_frags_received;
    out->rtps_parse_errors = participant->stats.parse_errors;
    out->rtps_samples_dropped = participant->stats.samples_dropped;
    out->rtps_lease_expired = participant->stats.lease_expired;

    /* Gauges: this participant plus everything it has discovered. */
    out->rtps_participants = 1;
    for (i = 0; i < participant->max_remotes; i++) {
        if (participant->remotes[i].used) {
            out->rtps_participants++;
        }
    }
    out->rtps_publications = 0;
    out->rtps_subscriptions = 0;
    for (i = 0; i < participant->max_local_endpoints; i++) {
        const nb_rtps_local_endpoint_t *endpoint = &participant->local_endpoints[i];
        if (!endpoint->used) {
            continue;
        }
        if (endpoint->kind == NB_RTPS_ENDPOINT_PUBLICATION) {
            out->rtps_publications++;
        } else {
            out->rtps_subscriptions++;
        }
    }
    for (i = 0; i < participant->max_remote_endpoints; i++) {
        const nb_rtps_remote_endpoint_t *endpoint = &participant->remote_endpoints[i];
        if (!endpoint->used) {
            continue;
        }
        if (endpoint->kind == NB_RTPS_ENDPOINT_PUBLICATION) {
            out->rtps_publications++;
        } else {
            out->rtps_subscriptions++;
        }
    }
    zb_mutex_unlock((zb_mutex *)participant->lock);
}
