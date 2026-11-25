/* RTPS spine: RTPS-over-TCP link setup and callbacks. */
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
    zb_mutex_lock(participant->net.lock);
    remote = find_remote(participant, prefix);
    if (!remote || remote->tcp_key[0] != '\0' ||
        remote->info.metatraffic_tcp.kind == NB_RTPS_LOCATOR_KIND_INVALID) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    if (locator_to_host_any(&remote->info.metatraffic_tcp,
                            host,
                            sizeof(host),
                            &port) != NB_OK) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    dial = memcmp(participant->local_guid.prefix.bytes,
                  remote->info.guid.prefix.bytes,
                  sizeof(participant->local_guid.prefix.bytes)) < 0;
    now = zn_wall_ms();
    if (!dial || now < remote->next_tcp_connect_ms) {
        zb_mutex_unlock(participant->net.lock);
        return;
    }
    remote->next_tcp_connect_ms = now + 1000u;
    zb_mutex_unlock(participant->net.lock);

    if (nb_rtps_tcp_has_link(participant->tcp, host, port)) {
        return;
    }
    if (nb_rtps_tcp_connect(participant->tcp, host, port) < 0) {
        nb_log_debug("rtps: tcp connect to %s:%u failed",
                     host,
                     (unsigned)port);
    }
}

void retry_tcp_links(nb_rtps_participant_t *participant)
{
    nb_rtps_guid_prefix_t prefixes[NB_RTPS_TCP_MAX_CONNECTIONS];
    size_t count = 0;
    size_t i;

    if (!participant->tcp) {
        return;
    }
    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->max_remotes &&
                count < NB_RTPS_TCP_MAX_CONNECTIONS;
         i++) {
        nb_rtps_remote_participant_t *remote = &participant->tables.remotes[i];

        if (!remote->used || remote->tcp_key[0] != '\0' ||
            remote->info.metatraffic_tcp.kind == NB_RTPS_LOCATOR_KIND_INVALID) {
            continue;
        }
        prefixes[count++] = remote->info.guid.prefix;
    }
    zb_mutex_unlock(participant->net.lock);
    for (i = 0; i < count; i++) {
        maybe_tcp_connect(participant, &prefixes[i]);
    }
}

static void tcp_bind_remote(nb_rtps_participant_t *participant,
                            const nb_rtps_guid_prefix_t *prefix,
                            const char *key)
{
    nb_rtps_remote_participant_t *remote;

    if (!key) {
        return;
    }
    zb_mutex_lock(participant->net.lock);
    remote = find_remote(participant, prefix);
    if (remote && remote->tcp_key[0] == '\0') {
        copy_str(remote->tcp_key, sizeof(remote->tcp_key), key);
    }
    zb_mutex_unlock(participant->net.lock);
}

void tcp_on_message(void *user,
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

void tcp_on_link(void *user, int index)
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
    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->max_remotes; i++) {
        nb_rtps_remote_participant_t *remote = &participant->tables.remotes[i];
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
    zb_mutex_unlock(participant->net.lock);
}

void tcp_on_unlink(void *user, int index)
{
    nb_rtps_participant_t *participant = (nb_rtps_participant_t *)user;
    const char *key = nb_rtps_tcp_key(participant->tcp, index);
    uint32_t i;

    if (!key) {
        return;
    }
    zb_mutex_lock(participant->net.lock);
    for (i = 0; i < participant->max_remotes; i++) {
        nb_rtps_remote_participant_t *remote = &participant->tables.remotes[i];

        if (remote->used && strcmp(remote->tcp_key, key) == 0) {
            remote->tcp_key[0] = '\0';
            remote->next_tcp_connect_ms = 0;
        }
    }
    zb_mutex_unlock(participant->net.lock);
}
