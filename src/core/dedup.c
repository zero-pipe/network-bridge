#include "dedup.h"

#include "envelope.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static nb_dedup_shard_t *nb_shard_for(nb_dedup_t *d, uint32_t request_id)
{
    return &d->shards[request_id % NB_DEDUP_SHARDS];
}

static void nb_slot_free(nb_dedup_shard_t *sh, nb_dedup_slot_t *s)
{
    if (s->payload_owner) {
        sh->bytes -= s->payload_len;
        nb_buf_release(s->payload_owner);
    } else if (s->payload) {
        sh->bytes -= s->payload_len;
        free(s->payload);
    }
    if (s->metadata) {
        sh->bytes -= s->metadata_len;
        free(s->metadata);
    }
    if (s->message) {
        sh->bytes -= s->message_len;
        free(s->message);
    }
    memset(s, 0, sizeof(*s));
}

/* 放弃已接管的 owner：缓存失败时缓冲区仍归调用方，这里只清引用和记账。 */
static void nb_slot_detach_owner(nb_dedup_shard_t *sh, nb_dedup_slot_t *s)
{
    if (!s->payload_owner) {
        return;
    }
    s->payload_owner = NULL;
    s->payload = NULL;
    sh->bytes -= (sh->bytes >= s->payload_len) ? s->payload_len : sh->bytes;
    s->payload_len = 0;
}

static bool nb_slot_matches(const nb_dedup_slot_t *s, const nb_message_t *req)
{
    return s->used && s->request_id == req->request_id && s->peer_port == req->peer_port &&
        strcmp(s->peer_host, req->peer_host ? req->peer_host : "") == 0;
}

static nb_dedup_slot_t *nb_slot_find(nb_dedup_shard_t *sh, const nb_message_t *req)
{
    int i;
    for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
        if (nb_slot_matches(&sh->slots[i], req)) {
            return &sh->slots[i];
        }
    }
    return NULL;
}

/* 复制 len 字节进缓存并计入预算；调用方保证 src 非空且 len > 0。失败返回 NULL。 */
static void *nb_slot_copy(nb_dedup_shard_t *sh, const void *src, size_t len)
{
    void *copy = malloc(len);
    if (copy) {
        memcpy(copy, src, len);
        sh->bytes += len;
    }
    return copy;
}

/* 持锁下腾出 need 字节预算；不足时淘汰本分片最旧的已用槽（keep 除外）。 */
static void nb_shard_make_room(nb_dedup_shard_t *sh, const nb_dedup_slot_t *keep, size_t need)
{
    while (sh->bytes + need > NB_DEDUP_MAX_BYTES) {
        nb_dedup_slot_t *oldest = NULL;
        int i;
        for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
            if (&sh->slots[i] == keep || !sh->slots[i].used) {
                continue;
            }
            if (!oldest || sh->slots[i].last_ms < oldest->last_ms) {
                oldest = &sh->slots[i];
            }
        }
        if (!oldest) {
            break;
        }
        nb_slot_free(sh, oldest);
    }
}

/* 持锁下重放缓存响应。 */
static void nb_dedup_replay_locked(nb_transport_t *transport,
                                   const nb_message_t *req,
                                   const nb_dedup_slot_t *s)
{
    nb_message_t reply;
    nb_payload_t pl;
    reply = *req;
    reply.kind = KIND_RESPONSE;
    reply.flags = 0;
    reply.status = s->status;
    reply.route = NULL;
    reply.metadata = s->metadata;
    reply.metadata_len = s->metadata_len;
    reply.status_msg = s->message;
    reply.status_msg_len = s->message_len;
    reply.has_stream = 0;
    pl.data = s->payload;
    pl.len = s->payload_len;
    pl.encoding = s->encoding;
    reply.payload = pl;
    (void)nb_transport_reply(transport, &reply);
}

void nb_dedup_init(nb_dedup_t *d)
{
    int i;
    if (!d) {
        return;
    }
    memset(d, 0, sizeof(*d));
    for (i = 0; i < NB_DEDUP_SHARDS; i++) {
        d->shards[i].lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    }
}

void nb_dedup_cleanup(nb_dedup_t *d)
{
    int i;
    int j;
    if (!d) {
        return;
    }
    for (i = 0; i < NB_DEDUP_SHARDS; i++) {
        nb_dedup_shard_t *sh = &d->shards[i];
        if (sh->lock) {
            ntk_mutex_lock(sh->lock);
        }
        for (j = 0; j < NB_DEDUP_SLOTS_PER_SHARD; j++) {
            if (sh->slots[j].used) {
                nb_slot_free(sh, &sh->slots[j]);
            }
        }
        if (sh->lock) {
            ntk_mutex_unlock(sh->lock);
            ntk_mutex_destroy(sh->lock);
            sh->lock = NULL;
        }
    }
}

nb_dedup_action_t nb_dedup_begin(nb_dedup_t *d,
                                 nb_transport_t *transport,
                                 const nb_message_t *req)
{
    nb_dedup_shard_t *sh;
    nb_dedup_slot_t *victim = NULL;
    uint64_t now = nb_now_ms();
    int i;

    if (!d || !transport || !req) {
        return NB_DEDUP_PROCESS;
    }
    sh = nb_shard_for(d, req->request_id);
    if (!sh->lock) {
        return NB_DEDUP_PROCESS;
    }
    ntk_mutex_lock(sh->lock);
    for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
        nb_dedup_slot_t *s = &sh->slots[i];
        bool completed;
        if (s->used && s->expires_ms <= now) {
            nb_slot_free(sh, s);
        }
        if (!nb_slot_matches(s, req)) {
            continue;
        }
        completed = s->completed;
        if (completed) {
            nb_dedup_replay_locked(transport, req, s);
        }
        ntk_mutex_unlock(sh->lock);
        return completed ? NB_DEDUP_REPLAYED : NB_DEDUP_IN_FLIGHT;
    }
    for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
        if (!sh->slots[i].used) {
            victim = &sh->slots[i];
            break;
        }
        if (!victim || sh->slots[i].last_ms < victim->last_ms) {
            victim = &sh->slots[i];
        }
    }
    if (victim) {
        nb_slot_free(sh, victim);
        victim->used = true;
        victim->request_id = req->request_id;
        nb_copy_str(victim->peer_host, sizeof(victim->peer_host), req->peer_host, "");
        victim->peer_port = req->peer_port;
        victim->expires_ms = now + NB_DEDUP_TTL_MS;
        victim->last_ms = now;
    }
    ntk_mutex_unlock(sh->lock);
    return NB_DEDUP_PROCESS;
}

bool nb_dedup_complete(nb_dedup_t *d,
                       const nb_message_t *req,
                       int status,
                       const nb_payload_t *payload,
                       const void *metadata,
                       uint16_t metadata_len,
                       const char *message,
                       uint16_t message_len,
                       nb_buf_t *payload_owner)
{
    nb_dedup_shard_t *sh;
    nb_dedup_slot_t *s;
    size_t plen = (payload && payload->len > 0) ? payload->len : 0;
    size_t need;
    uint64_t now = nb_now_ms();

    if (!d || !req) {
        return false;
    }
    sh = nb_shard_for(d, req->request_id);
    if (!sh->lock) {
        return false;
    }
    need = plen + metadata_len + message_len;
    if (need > NB_DEDUP_MAX_BYTES) {
        return false; /* 单条响应过大，不缓存 */
    }
    ntk_mutex_lock(sh->lock);
    s = nb_slot_find(sh, req);
    if (!s || s->completed) {
        ntk_mutex_unlock(sh->lock);
        return false;
    }
    nb_shard_make_room(sh, s, need);

    if (payload_owner) {
        s->payload_owner = payload_owner;
        s->payload = payload ? (uint8_t *)payload->data : NULL;
        s->payload_len = plen;
        sh->bytes += plen;
    } else if (plen > 0 && payload && payload->data) {
        s->payload = nb_slot_copy(sh, payload->data, plen);
        if (!s->payload) {
            goto fail;
        }
        s->payload_len = plen;
    }
    if (metadata_len > 0 && metadata) {
        s->metadata = nb_slot_copy(sh, metadata, metadata_len);
        if (!s->metadata) {
            goto fail;
        }
        s->metadata_len = metadata_len;
    }
    if (message_len > 0 && message) {
        s->message = malloc((size_t)message_len + 1);
        if (!s->message) {
            goto fail;
        }
        memcpy(s->message, message, message_len);
        s->message[message_len] = '\0';
        s->message_len = message_len;
        sh->bytes += message_len;
    }
    s->status = status;
    s->encoding = payload ? payload->encoding : 0;
    s->completed = true;
    s->last_ms = now;
    s->expires_ms = now + NB_DEDUP_TTL_MS;
    ntk_mutex_unlock(sh->lock);
    return payload_owner != NULL;

fail:
    nb_slot_detach_owner(sh, s);
    nb_slot_free(sh, s);
    ntk_mutex_unlock(sh->lock);
    return false;
}

void nb_dedup_abort(nb_dedup_t *d, const nb_message_t *req)
{
    nb_dedup_shard_t *sh;
    nb_dedup_slot_t *s;
    if (!d || !req) {
        return;
    }
    sh = nb_shard_for(d, req->request_id);
    if (!sh->lock) {
        return;
    }
    ntk_mutex_lock(sh->lock);
    s = nb_slot_find(sh, req);
    if (s) {
        nb_slot_free(sh, s);
    }
    ntk_mutex_unlock(sh->lock);
}

void nb_dedup_expire(nb_dedup_t *d, uint64_t now_ms)
{
    int i;
    int j;
    if (!d) {
        return;
    }
    for (i = 0; i < NB_DEDUP_SHARDS; i++) {
        nb_dedup_shard_t *sh = &d->shards[i];
        if (!sh->lock) {
            continue;
        }
        ntk_mutex_lock(sh->lock);
        for (j = 0; j < NB_DEDUP_SLOTS_PER_SHARD; j++) {
            if (sh->slots[j].used && sh->slots[j].expires_ms <= now_ms) {
                nb_slot_free(sh, &sh->slots[j]);
            }
        }
        ntk_mutex_unlock(sh->lock);
    }
}
