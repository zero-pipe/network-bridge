/*
 * ntk_discovery - broadcast key/value service discovery over UDP.
 *
 * Wire format v5 (no backward compatibility, by design):
 *
 *   header  magic "NDS5" | version 5 | flags | rsvd(2)
 *           instance[16] | epoch(u64) | seq(u32) | crc32(u32)
 *   tlvs    type(u8) length(u16) value
 *            1 NAME      name bytes
 *            2 ENDPOINT  kind(u8) + uri bytes
 *            3 KEY       key_len(u16) key value_len(u16) value ttl(u32)
 *            4 FRAME     index(u16) total(u16), followed by a raw body chunk
 *
 * A HELLO is a full announcement: the TLV body (name, endpoints, keys) is split
 * into frames when it does not fit in one datagram. Frame 0..n-1 carry the same
 * header and are reassembled by (instance, epoch, seq) before being applied.
 * BYE removes the peer and all its keys.
 *
 * The library only depends on ntk. Keys and values are opaque byte strings.
 */

#include "ntk_discovery/ntk_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISC_MAGIC0 'N'
#define DISC_MAGIC1 'D'
#define DISC_MAGIC2 'S'
#define DISC_MAGIC3 '5'
#define DISC_VERSION 5u

#define DISC_FLAG_HELLO 0x01u
#define DISC_FLAG_BYE 0x02u
#define DISC_FLAG_MORE 0x04u

#define DISC_HEADER_SIZE 40u
#define DISC_FRAME_MAX 1400u
/* Space reserved for the FRAME TLV in every fragment budget. */
#define DISC_FRAME_TLV_SIZE 8u

#define DISC_TLV_NAME 1u
#define DISC_TLV_ENDPOINT 2u
#define DISC_TLV_KEY 3u
#define DISC_TLV_FRAME 4u

#define DISC_MAX_FRAGMENTS 64u
#define DISC_FRAG_TIMEOUT_MS 2000u
#define DISC_MAX_TARGETS 32u
#define DISC_RATE_LIMIT_PER_SEC 1024u

#define DISC_TOMB ((void *)(uintptr_t)1)

typedef struct disc_peer {
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    uint64_t epoch;
    uint32_t last_seq;
    char name[NTK_DISC_NAME_MAX];
    ntk_disc_endpoint_t endpoints[NTK_DISC_MAX_ENDPOINTS];
    size_t endpoint_count;
    char host[NTK_DISC_NAME_MAX];
    uint16_t port;
    uint64_t last_seen_ms;
} disc_peer_t;

typedef struct disc_key {
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    char key[NTK_DISC_KEY_MAX];
    uint8_t value[NTK_DISC_VALUE_MAX];
    size_t value_len;
    uint32_t ttl_ms;
    uint64_t expire_at;
    uint64_t seen_seq; /* bookkeeping while applying one HELLO */
} disc_key_t;

typedef struct disc_local_key {
    int used;
    char key[NTK_DISC_KEY_MAX];
    uint8_t *value;
    size_t value_len;
    uint32_t ttl_ms;
} disc_local_key_t;

typedef struct disc_frag {
    int used;
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    uint64_t epoch;
    uint32_t seq;
    uint64_t start_ms;
    char host[NTK_DISC_NAME_MAX];
    uint16_t port;
    uint16_t total;
    uint16_t received;
    uint8_t *chunk[DISC_MAX_FRAGMENTS];
    uint16_t chunk_len[DISC_MAX_FRAGMENTS];
} disc_frag_t;

struct ntk_discovery {
    ntk_socket *sock;
    ntk_poller *poller;
    ntk_mutex *lock;

    char bind_host[NTK_DISC_NAME_MAX];
    char broadcast[NTK_DISC_NAME_MAX];
    uint16_t port;
    uint32_t hello_interval_ms;
    uint32_t jitter_pct;
    uint32_t ttl_ms;
    uint32_t max_peers;
    uint32_t max_keys;
    ntk_discovery_callbacks_t callbacks;
    void *user;

    /* local state */
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    char name[NTK_DISC_NAME_MAX];
    ntk_disc_endpoint_t local_endpoints[NTK_DISC_MAX_ENDPOINTS];
    size_t local_endpoint_count;
    disc_local_key_t *local_keys;
    size_t local_key_cap;
    uint64_t epoch;
    uint32_t seq;
    uint32_t rng;

    /* remote tables: open addressing of pointers, tombstones are DISC_TOMB */
    disc_peer_t **peer_slots;
    size_t peer_cap;
    size_t peer_used;
    size_t peer_tomb;
    disc_key_t **key_slots;
    size_t key_cap;
    size_t key_used;
    size_t key_tomb;

    struct {
        char host[NTK_DISC_NAME_MAX];
        uint16_t port;
    } targets[DISC_MAX_TARGETS];
    size_t target_count;

    disc_frag_t frags[8];
    uint64_t rate_ms;
    uint32_t rate_count;
};

/* ---- small helpers ------------------------------------------------------ */

static void disc_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void disc_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void disc_put_u64(uint8_t *p, uint64_t v)
{
    disc_put_u32(p, (uint32_t)(v & 0xffffffffu));
    disc_put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t disc_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t disc_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t disc_get_u64(const uint8_t *p)
{
    return (uint64_t)disc_get_u32(p) | ((uint64_t)disc_get_u32(p + 4) << 32);
}

static uint32_t disc_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    size_t i;
    int bit;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t disc_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void disc_copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    (void)snprintf(dst, cap, "%s", src);
}

static uint64_t disc_now_ms(void)
{
    return ntk_wall_ms();
}

static uint32_t disc_next_rand(ntk_discovery_t *d)
{
    uint32_t x = d->rng ? d->rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    d->rng = x ? x : 1u;
    return d->rng;
}

/* ---- growable byte buffer ----------------------------------------------- */

typedef struct disc_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} disc_buf_t;

static int disc_buf_reserve(disc_buf_t *b, size_t add)
{
    size_t need;
    size_t cap;
    uint8_t *p;

    if (!b) {
        return NTK_DISC_ERR_INVALID;
    }
    need = b->len + add;
    if (need <= b->cap) {
        return NTK_DISC_OK;
    }
    cap = b->cap ? b->cap : 256u;
    while (cap < need) {
        cap *= 2u;
    }
    p = (uint8_t *)realloc(b->data, cap);
    if (!p) {
        return NTK_DISC_ERR_NOMEM;
    }
    b->data = p;
    b->cap = cap;
    return NTK_DISC_OK;
}

static int disc_buf_put(disc_buf_t *b, const void *data, size_t len)
{
    int rc = disc_buf_reserve(b, len);

    if (rc != NTK_DISC_OK) {
        return rc;
    }
    if (len > 0) {
        memcpy(b->data + b->len, data, len);
        b->len += len;
    }
    return NTK_DISC_OK;
}

static int disc_buf_u16(disc_buf_t *b, uint16_t v)
{
    uint8_t tmp[2];

    disc_put_u16(tmp, v);
    return disc_buf_put(b, tmp, sizeof(tmp));
}

static int disc_buf_u32(disc_buf_t *b, uint32_t v)
{
    uint8_t tmp[4];

    disc_put_u32(tmp, v);
    return disc_buf_put(b, tmp, sizeof(tmp));
}

static int disc_buf_tlv(disc_buf_t *b, uint8_t type, const void *data, size_t len)
{
    int rc;

    if (len > 0xffffu) {
        return NTK_DISC_ERR_TOOBIG;
    }
    rc = disc_buf_reserve(b, 3u + len);
    if (rc != NTK_DISC_OK) {
        return rc;
    }
    b->data[b->len++] = type;
    disc_put_u16(b->data + b->len, (uint16_t)len);
    b->len += 2;
    return disc_buf_put(b, data, len);
}

/* ---- peer table --------------------------------------------------------- */

static size_t disc_table_cap(uint32_t max)
{
    size_t cap = 8;

    while (cap < (size_t)max * 2u) {
        cap *= 2u;
    }
    return cap;
}

static disc_peer_t *disc_peer_find(ntk_discovery_t *d,
                                   const uint8_t instance[NTK_DISC_INSTANCE_SIZE])
{
    size_t mask;
    size_t i;

    if (!d->peer_slots || d->peer_cap == 0) {
        return NULL;
    }
    mask = d->peer_cap - 1u;
    i = disc_hash(instance, NTK_DISC_INSTANCE_SIZE) & mask;
    for (;;) {
        void *slot = d->peer_slots[i];

        if (!slot) {
            return NULL;
        }
        if (slot != DISC_TOMB &&
            memcmp(((disc_peer_t *)slot)->instance, instance,
                   NTK_DISC_INSTANCE_SIZE) == 0) {
            return (disc_peer_t *)slot;
        }
        i = (i + 1u) & mask;
    }
}

static void disc_peer_rebuild(ntk_discovery_t *d)
{
    disc_peer_t **slots;
    size_t i;

    slots = (disc_peer_t **)calloc(d->peer_cap, sizeof(*slots));
    if (!slots) {
        return; /* keep the old table; inserts may then fail with OVERLOAD */
    }
    for (i = 0; i < d->peer_cap; i++) {
        disc_peer_t *peer = d->peer_slots[i];

        if (peer && peer != (disc_peer_t *)DISC_TOMB) {
            size_t mask = d->peer_cap - 1u;
            size_t j = disc_hash(peer->instance, NTK_DISC_INSTANCE_SIZE) & mask;

            while (slots[j]) {
                j = (j + 1u) & mask;
            }
            slots[j] = peer;
        }
    }
    free(d->peer_slots);
    d->peer_slots = slots;
    d->peer_tomb = 0;
}

static disc_peer_t *disc_peer_get_or_create(ntk_discovery_t *d,
                                            const uint8_t instance[NTK_DISC_INSTANCE_SIZE])
{
    disc_peer_t *peer = disc_peer_find(d, instance);
    size_t mask;
    size_t i;
    size_t first_tomb = SIZE_MAX;

    if (peer) {
        return peer;
    }
    if ((d->peer_used + 1u) * 4u >= d->peer_cap * 3u) {
        disc_peer_rebuild(d);
    }
    if ((d->peer_used + 1u) * 4u >= d->peer_cap * 3u) {
        return NULL;
    }
    peer = (disc_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) {
        return NULL;
    }
    memcpy(peer->instance, instance, NTK_DISC_INSTANCE_SIZE);
    mask = d->peer_cap - 1u;
    i = disc_hash(instance, NTK_DISC_INSTANCE_SIZE) & mask;
    for (;;) {
        void *slot = d->peer_slots[i];

        if (!slot) {
            break;
        }
        if (slot == DISC_TOMB) {
            if (first_tomb == SIZE_MAX) {
                first_tomb = i;
            }
        }
        i = (i + 1u) & mask;
    }
    if (first_tomb != SIZE_MAX) {
        i = first_tomb;
        d->peer_tomb--;
    }
    d->peer_slots[i] = peer;
    d->peer_used++;
    return peer;
}

/* ---- key table ---------------------------------------------------------- */

static size_t disc_key_hash(const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                            const char *key)
{
    uint32_t h = disc_hash(instance, NTK_DISC_INSTANCE_SIZE);
    uint32_t k = disc_hash(key, strlen(key));

    return (size_t)(h ^ (k * 2654435761u));
}

static disc_key_t *disc_key_find(ntk_discovery_t *d,
                                 const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                                 const char *key)
{
    size_t mask;
    size_t i;

    if (!d->key_slots || d->key_cap == 0) {
        return NULL;
    }
    mask = d->key_cap - 1u;
    i = disc_key_hash(instance, key) & mask;
    for (;;) {
        void *slot = d->key_slots[i];

        if (!slot) {
            return NULL;
        }
        if (slot != (void *)DISC_TOMB) {
            disc_key_t *entry = (disc_key_t *)slot;

            if (memcmp(entry->instance, instance, NTK_DISC_INSTANCE_SIZE) == 0 &&
                strcmp(entry->key, key) == 0) {
                return entry;
            }
        }
        i = (i + 1u) & mask;
    }
}

static void disc_key_rebuild(ntk_discovery_t *d)
{
    disc_key_t **slots;
    size_t i;

    slots = (disc_key_t **)calloc(d->key_cap, sizeof(*slots));
    if (!slots) {
        return;
    }
    for (i = 0; i < d->key_cap; i++) {
        disc_key_t *entry = d->key_slots[i];

        if (entry && entry != (disc_key_t *)DISC_TOMB) {
            size_t mask = d->key_cap - 1u;
            size_t j = disc_key_hash(entry->instance, entry->key) & mask;

            while (slots[j]) {
                j = (j + 1u) & mask;
            }
            slots[j] = entry;
        }
    }
    free(d->key_slots);
    d->key_slots = slots;
    d->key_tomb = 0;
}

static disc_key_t *disc_key_upsert(ntk_discovery_t *d,
                                   const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                                   const char *key,
                                   const uint8_t *value,
                                   size_t value_len,
                                   uint32_t ttl_ms,
                                   uint64_t seen_seq,
                                   uint64_t now_ms)
{
    disc_key_t *entry = disc_key_find(d, instance, key);
    size_t mask;
    size_t i;
    size_t first_tomb = SIZE_MAX;

    if (entry) {
        if (value_len <= sizeof(entry->value)) {
            if (value_len > 0) {
                memcpy(entry->value, value, value_len);
            }
            entry->value_len = value_len;
        }
        entry->ttl_ms = ttl_ms;
        entry->expire_at = now_ms + (ttl_ms ? ttl_ms : d->ttl_ms);
        entry->seen_seq = seen_seq;
        return entry;
    }
    if ((d->key_used + 1u) * 4u >= d->key_cap * 3u) {
        disc_key_rebuild(d);
    }
    if ((d->key_used + 1u) * 4u >= d->key_cap * 3u) {
        return NULL;
    }
    entry = (disc_key_t *)calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }
    memcpy(entry->instance, instance, NTK_DISC_INSTANCE_SIZE);
    disc_copy_str(entry->key, sizeof(entry->key), key);
    if (value_len > sizeof(entry->value)) {
        value_len = sizeof(entry->value);
    }
    if (value_len > 0 && value) {
        memcpy(entry->value, value, value_len);
    }
    entry->value_len = value_len;
    entry->ttl_ms = ttl_ms;
    entry->expire_at = now_ms + (ttl_ms ? ttl_ms : d->ttl_ms);
    entry->seen_seq = seen_seq;

    mask = d->key_cap - 1u;
    i = disc_key_hash(instance, key) & mask;
    for (;;) {
        void *slot = d->key_slots[i];

        if (!slot) {
            break;
        }
        if (slot == (void *)DISC_TOMB && first_tomb == SIZE_MAX) {
            first_tomb = i;
        }
        i = (i + 1u) & mask;
    }
    if (first_tomb != SIZE_MAX) {
        i = first_tomb;
        d->key_tomb--;
    }
    d->key_slots[i] = entry;
    d->key_used++;
    return entry;
}

static void disc_key_unlink(ntk_discovery_t *d, disc_key_t *entry)
{
    size_t mask = d->key_cap - 1u;
    size_t i = disc_key_hash(entry->instance, entry->key) & mask;

    for (;;) {
        void *slot = d->key_slots[i];

        if (!slot) {
            break;
        }
        if (slot == entry) {
            d->key_slots[i] = DISC_TOMB;
            d->key_used--;
            d->key_tomb++;
            break;
        }
        i = (i + 1u) & mask;
    }
    free(entry);
}

/* ---- callbacks ---------------------------------------------------------- */

static void disc_emit_peer(ntk_discovery_t *d, const disc_peer_t *peer, int removed)
{
    ntk_disc_peer_info_t info;

    if (!d->callbacks.on_peer) {
        return;
    }
    memset(&info, 0, sizeof(info));
    if (peer) {
        memcpy(info.instance, peer->instance, sizeof(info.instance));
        disc_copy_str(info.name, sizeof(info.name), peer->name);
        disc_copy_str(info.host, sizeof(info.host), peer->host);
        info.port = peer->port;
        info.endpoint_count = peer->endpoint_count;
        memcpy(info.endpoints, peer->endpoints,
               peer->endpoint_count * sizeof(peer->endpoints[0]));
        info.last_seen_ms = peer->last_seen_ms;
    }
    d->callbacks.on_peer(d->user, &info, removed);
}

static void disc_emit_key(ntk_discovery_t *d, const disc_key_t *key, int removed)
{
    ntk_disc_key_info_t info;

    if (!d->callbacks.on_key) {
        return;
    }
    memset(&info, 0, sizeof(info));
    if (key) {
        memcpy(info.instance, key->instance, sizeof(info.instance));
        disc_copy_str(info.key, sizeof(info.key), key->key);
        info.value_len = key->value_len;
        if (key->value_len > 0) {
            memcpy(info.value, key->value, key->value_len);
        }
        info.ttl_ms = key->ttl_ms;
    }
    d->callbacks.on_key(d->user, &info, removed);
}

/* Remove every key of an instance, reporting each. Lock held. */
static void disc_remove_instance_keys(ntk_discovery_t *d,
                                      const uint8_t instance[NTK_DISC_INSTANCE_SIZE])
{
    size_t i;

    if (!d->key_slots) {
        return;
    }
    for (i = 0; i < d->key_cap; i++) {
        disc_key_t *entry = d->key_slots[i];

        if (entry && entry != (disc_key_t *)DISC_TOMB &&
            memcmp(entry->instance, instance, NTK_DISC_INSTANCE_SIZE) == 0) {
            disc_emit_key(d, entry, 1);
            disc_key_unlink(d, entry);
            entry = NULL;
        }
    }
}

/* ---- wire --------------------------------------------------------------- */

static void disc_header(uint8_t *buf,
                        uint8_t flags,
                        const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                        uint64_t epoch,
                        uint32_t seq)
{
    buf[0] = DISC_MAGIC0;
    buf[1] = DISC_MAGIC1;
    buf[2] = DISC_MAGIC2;
    buf[3] = DISC_MAGIC3;
    buf[4] = (uint8_t)DISC_VERSION;
    buf[5] = flags;
    buf[6] = 0;
    buf[7] = 0;
    memcpy(buf + 8, instance, NTK_DISC_INSTANCE_SIZE);
    disc_put_u64(buf + 24, epoch);
    disc_put_u32(buf + 32, seq);
    disc_put_u32(buf + 36, 0);
}

static uint32_t disc_frame_crc(uint8_t *buf, size_t len)
{
    uint8_t saved[4];
    uint32_t crc;

    memcpy(saved, buf + 36, 4);
    memset(buf + 36, 0, 4);
    crc = disc_crc32(0, buf, len);
    memcpy(buf + 36, saved, 4);
    return crc;
}

static void disc_send_buf(ntk_discovery_t *d, const uint8_t *buf, size_t len)
{
    size_t i;

    if (!d->sock) {
        return;
    }
    (void)ntk_socket_sendto(d->sock, buf, len,
                            d->broadcast[0] ? d->broadcast : "255.255.255.255",
                            d->port);
    for (i = 0; i < d->target_count; i++) {
        (void)ntk_socket_sendto(d->sock, buf, len, d->targets[i].host,
                                d->targets[i].port);
    }
}

static int disc_build_body(ntk_discovery_t *d, disc_buf_t *body)
{
    size_t i;
    int rc;

    if (d->name[0]) {
        rc = disc_buf_tlv(body, DISC_TLV_NAME, d->name, strlen(d->name));
        if (rc != NTK_DISC_OK) {
            return rc;
        }
    }
    for (i = 0; i < d->local_endpoint_count; i++) {
        uint8_t tmp[1 + NTK_DISC_URI_MAX];
        size_t ulen = strlen(d->local_endpoints[i].uri);

        if (ulen >= NTK_DISC_URI_MAX) {
            ulen = NTK_DISC_URI_MAX - 1u;
        }
        tmp[0] = d->local_endpoints[i].kind;
        memcpy(tmp + 1, d->local_endpoints[i].uri, ulen);
        rc = disc_buf_tlv(body, DISC_TLV_ENDPOINT, tmp, 1u + ulen);
        if (rc != NTK_DISC_OK) {
            return rc;
        }
    }
    for (i = 0; i < d->local_key_cap; i++) {
        disc_local_key_t *lk = &d->local_keys[i];
        uint8_t tmp[2 + NTK_DISC_KEY_MAX + 2 + NTK_DISC_VALUE_MAX + 4];
        size_t klen;
        size_t off = 0;

        if (!lk->used) {
            continue;
        }
        klen = strlen(lk->key);
        disc_put_u16(tmp + off, (uint16_t)klen);
        off += 2;
        memcpy(tmp + off, lk->key, klen);
        off += klen;
        disc_put_u16(tmp + off, (uint16_t)lk->value_len);
        off += 2;
        if (lk->value_len > 0 && lk->value) {
            memcpy(tmp + off, lk->value, lk->value_len);
            off += lk->value_len;
        }
        disc_put_u32(tmp + off, lk->ttl_ms);
        off += 4;
        rc = disc_buf_tlv(body, DISC_TLV_KEY, tmp, off);
        if (rc != NTK_DISC_OK) {
            return rc;
        }
    }
    return NTK_DISC_OK;
}

static size_t disc_build_frame(uint8_t *frame,
                               size_t cap,
                               uint8_t flags,
                               const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                               uint64_t epoch,
                               uint32_t seq,
                               const uint8_t *body,
                               size_t body_len,
                               uint16_t index,
                               uint16_t total)
{
    size_t len = DISC_HEADER_SIZE;

    if (len + (total > 1u ? DISC_FRAME_TLV_SIZE : 0u) + body_len > cap) {
        return 0;
    }
    disc_header(frame, flags, instance, epoch, seq);
    if (total > 1u) {
        frame[len++] = DISC_TLV_FRAME;
        disc_put_u16(frame + len, 4);
        len += 2;
        disc_put_u16(frame + len, index);
        len += 2;
        disc_put_u16(frame + len, total);
        len += 2;
        flags |= DISC_FLAG_MORE;
        frame[5] = flags;
    }
    if (body_len > 0) {
        memcpy(frame + len, body, body_len);
        len += body_len;
    }
    disc_put_u32(frame + 36, disc_frame_crc(frame, len));
    return len;
}

/* Send a HELLO to the broadcast address plus every configured target, or to a
 * single destination when host is non-NULL. Called with the lock held. */
static int disc_send_hello_locked(ntk_discovery_t *d,
                                  const char *host,
                                  uint16_t port)
{
    disc_buf_t body;
    size_t chunk_max = DISC_FRAME_MAX - DISC_HEADER_SIZE - DISC_FRAME_TLV_SIZE;
    size_t total;
    size_t i;
    int rc;

    memset(&body, 0, sizeof(body));
    rc = disc_build_body(d, &body);
    if (rc != NTK_DISC_OK) {
        free(body.data);
        return rc;
    }
    d->seq++;
    total = body.len == 0 ? 1u : (body.len + chunk_max - 1u) / chunk_max;
    if (total > DISC_MAX_FRAGMENTS) {
        free(body.data);
        return NTK_DISC_ERR_TOOBIG;
    }
    for (i = 0; i < total; i++) {
        uint8_t frame[DISC_FRAME_MAX];
        size_t off = i * chunk_max;
        size_t n = body.len - off;
        size_t len;

        if (n > chunk_max) {
            n = chunk_max;
        }
        len = disc_build_frame(frame, sizeof(frame), DISC_FLAG_HELLO,
                               d->instance, d->epoch, d->seq,
                               body.data ? body.data + off : NULL, n,
                               (uint16_t)i, (uint16_t)total);
        if (len == 0) {
            continue;
        }
        if (host && host[0]) {
            (void)ntk_socket_sendto(d->sock, frame, len, host, port);
        } else {
            disc_send_buf(d, frame, len);
        }
    }
    free(body.data);
    return NTK_DISC_OK;
}

static int disc_announce_locked(ntk_discovery_t *d)
{
    return disc_send_hello_locked(d, NULL, 0);
}

/* ---- applying a HELLO --------------------------------------------------- */

typedef struct disc_parsed_key {
    const uint8_t *key;
    size_t key_len;
    const uint8_t *value;
    size_t value_len;
    uint32_t ttl_ms;
} disc_parsed_key_t;

static int disc_parse_key_tlv(const uint8_t *p, size_t len, disc_parsed_key_t *out)
{
    size_t off = 0;
    uint16_t klen;
    uint16_t vlen;

    if (len < 4) {
        return NTK_DISC_ERR_INVALID;
    }
    klen = disc_get_u16(p + off);
    off += 2;
    if (off + klen + 2 > len) {
        return NTK_DISC_ERR_INVALID;
    }
    out->key = p + off;
    out->key_len = klen;
    off += klen;
    vlen = disc_get_u16(p + off);
    off += 2;
    if (off + vlen + 4 > len) {
        return NTK_DISC_ERR_INVALID;
    }
    out->value = p + off;
    out->value_len = vlen;
    off += vlen;
    out->ttl_ms = disc_get_u32(p + off);
    return NTK_DISC_OK;
}

/* Parse a reassembled body and apply it. Lock held; callbacks run under it
 * (the mutex is recursive so snapshot calls from a callback are safe). */
static void disc_apply_body(ntk_discovery_t *d,
                            const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                            uint64_t epoch,
                            uint32_t seq,
                            const char *host,
                            uint16_t port,
                            const uint8_t *body,
                            size_t len,
                            uint64_t now_ms)
{
    char name[NTK_DISC_NAME_MAX];
    ntk_disc_endpoint_t eps[NTK_DISC_MAX_ENDPOINTS];
    size_t ep_count = 0;
    size_t off;
    size_t i;
    size_t key_tlvs = 0;
    disc_peer_t *peer;
    int had_state;
    int new_boot;

    memset(name, 0, sizeof(name));
    memset(eps, 0, sizeof(eps));

    /* Pass 1: validate the whole TLV stream and collect the peer fields.
     * Nothing is applied until the stream is known to be well formed. */
    off = 0;
    while (off + 3u <= len) {
        uint8_t type = body[off];
        uint16_t tlen = disc_get_u16(body + off + 1);
        const uint8_t *val = body + off + 3;

        off += 3;
        if (off + tlen > len) {
            return;
        }
        if (type == DISC_TLV_NAME) {
            size_t n = tlen < sizeof(name) - 1u ? tlen : sizeof(name) - 1u;

            memcpy(name, val, n);
            name[n] = 0;
        } else if (type == DISC_TLV_ENDPOINT) {
            if (tlen >= 1 && ep_count < NTK_DISC_MAX_ENDPOINTS) {
                size_t ulen = tlen - 1u;

                if (ulen >= NTK_DISC_URI_MAX) {
                    ulen = NTK_DISC_URI_MAX - 1u;
                }
                eps[ep_count].kind = val[0];
                memcpy(eps[ep_count].uri, val + 1, ulen);
                eps[ep_count].uri[ulen] = 0;
                ep_count++;
            }
        } else if (type == DISC_TLV_KEY) {
            disc_parsed_key_t parsed;

            if (disc_parse_key_tlv(val, tlen, &parsed) != NTK_DISC_OK ||
                parsed.key_len == 0 || parsed.key_len >= NTK_DISC_KEY_MAX ||
                parsed.value_len > NTK_DISC_VALUE_MAX) {
                return;
            }
            key_tlvs++;
        }
        off += tlen;
    }
    if (off != len) {
        return;
    }

    peer = disc_peer_get_or_create(d, instance);
    if (!peer) {
        return;
    }
    had_state = peer->last_seen_ms != 0;
    new_boot = had_state && peer->epoch != epoch;
    if (new_boot) {
        /* Same instance, new process: its old keys are gone. */
        disc_remove_instance_keys(d, instance);
        peer->last_seq = 0;
    }
    if (peer->last_seq != 0 && peer->epoch == epoch && seq <= peer->last_seq) {
        return;
    }
    memcpy(peer->instance, instance, NTK_DISC_INSTANCE_SIZE);
    peer->epoch = epoch;
    peer->last_seq = seq;
    disc_copy_str(peer->name, sizeof(peer->name), name);
    memcpy(peer->endpoints, eps, ep_count * sizeof(eps[0]));
    peer->endpoint_count = ep_count;
    disc_copy_str(peer->host, sizeof(peer->host), host);
    peer->port = port;
    peer->last_seen_ms = now_ms;

    /* Pass 2: apply keys. Upserts mark seen_seq = this announcement; keys of
     * the instance that keep an older mark were withdrawn by the sender. */
    off = 0;
    while (off + 3u <= len) {
        uint8_t type = body[off];
        uint16_t tlen = disc_get_u16(body + off + 1);
        const uint8_t *val = body + off + 3;

        off += 3;
        if (type == DISC_TLV_KEY) {
            disc_parsed_key_t parsed;

            if (disc_parse_key_tlv(val, tlen, &parsed) == NTK_DISC_OK) {
                char key[NTK_DISC_KEY_MAX];
                size_t klen = parsed.key_len < sizeof(key) - 1u
                                  ? parsed.key_len
                                  : sizeof(key) - 1u;
                disc_key_t *entry;

                memcpy(key, parsed.key, klen);
                key[klen] = 0;
                entry = disc_key_upsert(d, instance, key, parsed.value,
                                        parsed.value_len, parsed.ttl_ms, seq,
                                        now_ms);
                if (entry) {
                    disc_emit_key(d, entry, 0);
                }
            }
        }
        off += tlen;
    }
    for (i = 0; i < d->key_cap; i++) {
        disc_key_t *entry = d->key_slots[i];

        if (entry && entry != (disc_key_t *)DISC_TOMB &&
            memcmp(entry->instance, instance, NTK_DISC_INSTANCE_SIZE) == 0 &&
            entry->seen_seq != seq) {
            disc_emit_key(d, entry, 1);
            disc_key_unlink(d, entry);
        }
    }
    disc_emit_peer(d, peer, 0);
    if (!had_state) {
        /* AODV-style hello reply: the new peer does not have to wait for our
         * periodic announcement to learn about us. */
        (void)disc_send_hello_locked(d, host, port);
    }
}

/* ---- fragments ---------------------------------------------------------- */

static disc_frag_t *disc_frag_get(ntk_discovery_t *d,
                                  const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                                  uint64_t epoch,
                                  uint32_t seq,
                                  const char *host,
                                  uint16_t port,
                                  uint16_t total,
                                  uint64_t now_ms)
{
    size_t i;
    disc_frag_t *free_slot = NULL;

    for (i = 0; i < sizeof(d->frags) / sizeof(d->frags[0]); i++) {
        disc_frag_t *f = &d->frags[i];

        if (f->used && f->epoch == epoch && f->seq == seq &&
            memcmp(f->instance, instance, NTK_DISC_INSTANCE_SIZE) == 0) {
            return f;
        }
        if (!f->used && !free_slot) {
            free_slot = f;
        }
    }
    if (!free_slot || total == 0 || total > DISC_MAX_FRAGMENTS) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1;
    memcpy(free_slot->instance, instance, NTK_DISC_INSTANCE_SIZE);
    free_slot->epoch = epoch;
    free_slot->seq = seq;
    free_slot->total = total;
    free_slot->start_ms = now_ms;
    disc_copy_str(free_slot->host, sizeof(free_slot->host), host);
    free_slot->port = port;
    return free_slot;
}

static void disc_frag_release(disc_frag_t *f)
{
    size_t i;

    for (i = 0; i < DISC_MAX_FRAGMENTS; i++) {
        free(f->chunk[i]);
        f->chunk[i] = NULL;
    }
    memset(f, 0, sizeof(*f));
}

static void disc_frag_add(ntk_discovery_t *d,
                          const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                          uint64_t epoch,
                          uint32_t seq,
                          const char *host,
                          uint16_t port,
                          uint16_t index,
                          uint16_t total,
                          const uint8_t *chunk,
                          size_t chunk_len,
                          uint64_t now_ms)
{
    disc_frag_t *f = disc_frag_get(d, instance, epoch, seq, host, port, total, now_ms);
    uint8_t *copy;
    size_t i;
    size_t total_len = 0;
    uint8_t *body;

    if (!f || index >= total || f->chunk[index]) {
        return;
    }
    copy = (uint8_t *)malloc(chunk_len ? chunk_len : 1u);
    if (!copy) {
        return;
    }
    memcpy(copy, chunk, chunk_len);
    f->chunk[index] = copy;
    f->chunk_len[index] = (uint16_t)chunk_len;
    f->received++;
    if (f->received != f->total) {
        return;
    }
    for (i = 0; i < f->total; i++) {
        total_len += f->chunk_len[i];
    }
    body = (uint8_t *)malloc(total_len ? total_len : 1u);
    if (!body) {
        disc_frag_release(f);
        return;
    }
    total_len = 0;
    for (i = 0; i < f->total; i++) {
        memcpy(body + total_len, f->chunk[i], f->chunk_len[i]);
        total_len += f->chunk_len[i];
    }
    disc_apply_body(d, instance, epoch, seq, f->host, f->port, body, total_len, now_ms);
    free(body);
    disc_frag_release(f);
}

/* ---- receive ------------------------------------------------------------ */

static void disc_handle(ntk_discovery_t *d,
                        const char *host,
                        uint16_t port,
                        uint8_t *buf,
                        size_t len,
                        uint64_t now_ms)
{
    uint8_t flags;
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    uint64_t epoch;
    uint32_t seq;
    uint32_t crc;
    uint32_t saved_crc;
    size_t off;
    disc_peer_t *peer;
    int has_frame = 0;
    uint16_t frame_index = 0;
    uint16_t frame_total = 0;
    const uint8_t *body;
    size_t body_len;

    if (len < DISC_HEADER_SIZE || len > DISC_FRAME_MAX) {
        return;
    }
    if (buf[0] != DISC_MAGIC0 || buf[1] != DISC_MAGIC1 || buf[2] != DISC_MAGIC2 ||
        buf[3] != DISC_MAGIC3 || buf[4] != DISC_VERSION) {
        return;
    }
    crc = disc_get_u32(buf + 36);
    saved_crc = crc;
    memcpy((uint8_t *)(buf + 36), "\0\0\0\0", 4);
    crc = disc_crc32(0, buf, len);
    memcpy((uint8_t *)(buf + 36), &saved_crc, 4);
    if (crc != saved_crc) {
        return;
    }
    flags = buf[5];
    memcpy(instance, buf + 8, NTK_DISC_INSTANCE_SIZE);
    epoch = disc_get_u64(buf + 24);
    seq = disc_get_u32(buf + 32);
    if (memcmp(instance, d->instance, NTK_DISC_INSTANCE_SIZE) == 0) {
        return; /* self */
    }

    ntk_mutex_lock(d->lock);
    if (flags & DISC_FLAG_BYE) {
        peer = disc_peer_find(d, instance);
        if (peer) {
            /* Only the incarnation that is currently registered may say
             * goodbye; a stale BYE from an earlier boot is ignored. */
            if (strcmp(peer->host, host) == 0 && peer->epoch == epoch) {
                disc_remove_instance_keys(d, instance);
                disc_emit_peer(d, peer, 1);
                {
                    size_t mask = d->peer_cap - 1u;
                    size_t i = disc_hash(instance, NTK_DISC_INSTANCE_SIZE) & mask;

                    while (d->peer_slots[i]) {
                        if (d->peer_slots[i] == peer) {
                            d->peer_slots[i] = DISC_TOMB;
                            d->peer_used--;
                            d->peer_tomb++;
                            break;
                        }
                        i = (i + 1u) & mask;
                    }
                }
                free(peer);
            }
        }
        ntk_mutex_unlock(d->lock);
        return;
    }
    if (!(flags & DISC_FLAG_HELLO)) {
        ntk_mutex_unlock(d->lock);
        return;
    }

    /* The FRAME TLV, when present, is first: index/total, then the chunk. */
    off = DISC_HEADER_SIZE;
    if (off + 3u <= len && buf[off] == DISC_TLV_FRAME) {
        uint16_t tlen = disc_get_u16(buf + off + 1);

        if (tlen == 4 && off + 3u + 4u <= len) {
            has_frame = 1;
            frame_index = disc_get_u16(buf + off + 3);
            frame_total = disc_get_u16(buf + off + 5);
            off += 3u + 4u;
        }
    }
    body = buf + off;
    body_len = len - off;
    peer = disc_peer_find(d, instance);
    if (peer && peer->epoch == epoch && peer->last_seq != 0 &&
        seq <= peer->last_seq) {
        ntk_mutex_unlock(d->lock);
        return;
    }
    if (has_frame && frame_total > 1) {
        disc_frag_add(d, instance, epoch, seq, host, port,
                      frame_index, frame_total, body, body_len, now_ms);
    } else {
        disc_apply_body(d, instance, epoch, seq, host, port, body, body_len, now_ms);
    }
    ntk_mutex_unlock(d->lock);
}

static void disc_on_readable(ntk_socket *sock, void *user)
{
    ntk_discovery_t *d = (ntk_discovery_t *)user;
    uint8_t buf[DISC_FRAME_MAX];
    uint64_t now;

    (void)sock;
    for (;;) {
        char host[NTK_DISC_NAME_MAX];
        uint16_t port = 0;
        ntk_ssize_t n = ntk_socket_recvfrom(
            d->sock, buf, sizeof(buf), host, sizeof(host), &port);

        if (n <= 0) {
            break;
        }
        now = disc_now_ms();
        /* Coarse rate limit: a flood cannot monopolise the poller thread. */
        if (now - d->rate_ms >= 1000u) {
            d->rate_ms = now;
            d->rate_count = 0;
        }
        if (++d->rate_count > DISC_RATE_LIMIT_PER_SEC) {
            continue;
        }
        disc_handle(d, host, port, buf, (size_t)n, now);
    }
}

/* ---- public API --------------------------------------------------------- */

void ntk_discovery_config_init(ntk_discovery_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->port = NTK_DISC_DEFAULT_PORT;
    config->hello_interval_ms = 3000;
    config->jitter_pct = 20;
    config->ttl_ms = 9000;
    config->max_peers = 256;
    config->max_keys = 4096;
}

const char *ntk_disc_strerror(int error)
{
    switch (error) {
    case NTK_DISC_OK: return "success";
    case NTK_DISC_ERR_INVALID: return "invalid argument";
    case NTK_DISC_ERR_NOMEM: return "out of memory";
    case NTK_DISC_ERR_IO: return "I/O error";
    case NTK_DISC_ERR_TOOBIG: return "value too large";
    case NTK_DISC_ERR_STATE: return "invalid state";
    case NTK_DISC_ERR_OVERLOAD: return "table full";
    case NTK_DISC_ERR_NOTFOUND: return "not found";
    default: return "unknown error";
    }
}

ntk_discovery_t *ntk_discovery_create(ntk_poller *poller,
                                     const ntk_discovery_config_t *config)
{
    ntk_discovery_t *d;
    ntk_discovery_config_t defaults;
    ntk_socket_callbacks_t cbs;
    uint16_t actual = 0;
#if defined(_WIN32)
    const int reuse_port = 0;
#else
    const int reuse_port = 1;
#endif

    if (!poller) {
        return NULL;
    }
    if (!config || config->struct_size < sizeof(ntk_discovery_config_t)) {
        ntk_discovery_config_init(&defaults);
        config = &defaults;
    }
    d = (ntk_discovery_t *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->poller = poller;
    d->port = config->port ? config->port : NTK_DISC_DEFAULT_PORT;
    d->hello_interval_ms = config->hello_interval_ms ? config->hello_interval_ms : 3000;
    d->jitter_pct = config->jitter_pct;
    d->ttl_ms = config->ttl_ms ? config->ttl_ms : 9000;
    d->max_peers = config->max_peers ? config->max_peers : 256;
    d->max_keys = config->max_keys ? config->max_keys : 4096;
    d->callbacks = config->callbacks;
    d->user = config->user;
    disc_copy_str(d->bind_host, sizeof(d->bind_host), config->bind_host);
    disc_copy_str(d->broadcast, sizeof(d->broadcast),
                  config->broadcast ? config->broadcast : "255.255.255.255");
    d->rng = (uint32_t)disc_now_ms() ^ (uint32_t)(uintptr_t)d;
    if (d->rng == 0) {
        d->rng = 1;
    }
    {
        size_t i;
        for (i = 0; i < NTK_DISC_INSTANCE_SIZE; i++) {
            d->instance[i] = (uint8_t)(disc_next_rand(d) & 0xffu);
        }
    }
    d->epoch = ((uint64_t)disc_next_rand(d) << 32) | disc_next_rand(d);

    d->peer_cap = disc_table_cap(d->max_peers);
    d->peer_slots = (disc_peer_t **)calloc(d->peer_cap, sizeof(*d->peer_slots));
    d->key_cap = disc_table_cap(d->max_keys);
    d->key_slots = (disc_key_t **)calloc(d->key_cap, sizeof(*d->key_slots));
    d->lock = ntk_mutex_create(NTK_MUTEX_RECURSIVE);
    if (!d->peer_slots || !d->key_slots || !d->lock) {
        ntk_discovery_destroy(d);
        return NULL;
    }

    d->sock = ntk_socket_create();
    if (!d->sock ||
        ntk_socket_bind_udp_ex(d->sock,
                               d->bind_host[0] ? d->bind_host : "0.0.0.0",
                               d->port, 1, reuse_port) != NTK_OK) {
        ntk_discovery_destroy(d);
        return NULL;
    }
    (void)ntk_socket_set_broadcast(d->sock, 1);
    if (ntk_socket_get_local(d->sock, NULL, 0, &actual) == NTK_OK && actual != 0) {
        d->port = actual;
    }
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_readable = disc_on_readable;
    if (ntk_socket_attach_poller(d->sock, poller, &cbs, d) != NTK_OK) {
        ntk_discovery_destroy(d);
        return NULL;
    }
    return d;
}

void ntk_discovery_destroy(ntk_discovery_t *d)
{
    size_t i;

    if (!d) {
        return;
    }
    if (d->sock) {
        ntk_socket_detach_poller(d->sock);
        ntk_socket_destroy(d->sock);
        d->sock = NULL;
    }
    if (d->peer_slots) {
        for (i = 0; i < d->peer_cap; i++) {
            disc_peer_t *peer = d->peer_slots[i];

            if (peer && peer != (disc_peer_t *)DISC_TOMB) {
                free(peer);
            }
        }
        free(d->peer_slots);
    }
    if (d->key_slots) {
        for (i = 0; i < d->key_cap; i++) {
            disc_key_t *entry = d->key_slots[i];

            if (entry && entry != (disc_key_t *)DISC_TOMB) {
                free(entry);
            }
        }
        free(d->key_slots);
    }
    if (d->local_keys) {
        for (i = 0; i < d->local_key_cap; i++) {
            free(d->local_keys[i].value);
        }
        free(d->local_keys);
    }
    for (i = 0; i < sizeof(d->frags) / sizeof(d->frags[0]); i++) {
        disc_frag_release(&d->frags[i]);
    }
    if (d->lock) {
        ntk_mutex_destroy(d->lock);
    }
    free(d);
}

int ntk_discovery_set_identity(ntk_discovery_t *d,
                               const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                               const char *name)
{
    if (!d || !instance) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    memcpy(d->instance, instance, NTK_DISC_INSTANCE_SIZE);
    disc_copy_str(d->name, sizeof(d->name), name);
    d->epoch = ((uint64_t)disc_next_rand(d) << 32) | disc_next_rand(d);
    ntk_mutex_unlock(d->lock);
    return NTK_DISC_OK;
}

int ntk_discovery_set_endpoints(ntk_discovery_t *d,
                                const ntk_disc_endpoint_t *endpoints,
                                size_t count)
{
    size_t i;

    if (!d || (count > 0 && !endpoints) || count > NTK_DISC_MAX_ENDPOINTS) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    d->local_endpoint_count = 0;
    for (i = 0; i < count; i++) {
        d->local_endpoints[i] = endpoints[i];
        d->local_endpoints[i].uri[NTK_DISC_URI_MAX - 1u] = '\0';
        d->local_endpoint_count++;
    }
    ntk_mutex_unlock(d->lock);
    return NTK_DISC_OK;
}

int ntk_discovery_advertise(ntk_discovery_t *d,
                            const char *key,
                            const void *value,
                            size_t value_len,
                            uint32_t ttl_ms)
{
    size_t i;
    disc_local_key_t *slot = NULL;
    uint8_t *copy = NULL;

    if (!d || !key || key[0] == '\0' || strlen(key) >= NTK_DISC_KEY_MAX ||
        value_len > NTK_DISC_VALUE_MAX || (value_len > 0 && !value)) {
        return NTK_DISC_ERR_INVALID;
    }
    if (value_len > 0) {
        copy = (uint8_t *)malloc(value_len);
        if (!copy) {
            return NTK_DISC_ERR_NOMEM;
        }
        memcpy(copy, value, value_len);
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->local_key_cap; i++) {
        if (d->local_keys[i].used && strcmp(d->local_keys[i].key, key) == 0) {
            slot = &d->local_keys[i];
            break;
        }
        if (!slot && !d->local_keys[i].used) {
            slot = &d->local_keys[i];
        }
    }
    if (!slot) {
        size_t cap = d->local_key_cap ? d->local_key_cap * 2u : 8u;
        disc_local_key_t *grown =
            (disc_local_key_t *)realloc(d->local_keys, cap * sizeof(*grown));

        if (!grown) {
            ntk_mutex_unlock(d->lock);
            free(copy);
            return NTK_DISC_ERR_NOMEM;
        }
        memset(grown + d->local_key_cap, 0,
               (cap - d->local_key_cap) * sizeof(*grown));
        d->local_keys = grown;
        slot = &d->local_keys[d->local_key_cap];
        d->local_key_cap = cap;
    }
    free(slot->value);
    slot->used = 1;
    disc_copy_str(slot->key, sizeof(slot->key), key);
    slot->value = copy;
    slot->value_len = value_len;
    slot->ttl_ms = ttl_ms;
    ntk_mutex_unlock(d->lock);
    return NTK_DISC_OK;
}

int ntk_discovery_withdraw(ntk_discovery_t *d, const char *key)
{
    size_t i;
    int found = 0;

    if (!d || !key) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->local_key_cap; i++) {
        if (d->local_keys[i].used && strcmp(d->local_keys[i].key, key) == 0) {
            free(d->local_keys[i].value);
            memset(&d->local_keys[i], 0, sizeof(d->local_keys[i]));
            found = 1;
            break;
        }
    }
    ntk_mutex_unlock(d->lock);
    return found ? NTK_DISC_OK : NTK_DISC_ERR_NOTFOUND;
}

int ntk_discovery_clear_keys(ntk_discovery_t *d)
{
    size_t i;

    if (!d) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->local_key_cap; i++) {
        if (d->local_keys[i].used) {
            free(d->local_keys[i].value);
            memset(&d->local_keys[i], 0, sizeof(d->local_keys[i]));
        }
    }
    ntk_mutex_unlock(d->lock);
    return NTK_DISC_OK;
}

int ntk_discovery_announce(ntk_discovery_t *d)
{
    int rc;

    if (!d) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    rc = disc_announce_locked(d);
    ntk_mutex_unlock(d->lock);
    return rc;
}

int ntk_discovery_bye(ntk_discovery_t *d)
{
    uint8_t frame[DISC_HEADER_SIZE];

    if (!d) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    d->seq++;
    disc_header(frame, DISC_FLAG_BYE, d->instance, d->epoch, d->seq);
    disc_put_u32(frame + 36, disc_frame_crc(frame, sizeof(frame)));
    disc_send_buf(d, frame, sizeof(frame));
    ntk_mutex_unlock(d->lock);
    return NTK_DISC_OK;
}

uint32_t ntk_discovery_next_delay_ms(ntk_discovery_t *d)
{
    uint32_t base;
    uint32_t jitter;
    int32_t delta;
    uint32_t r;

    if (!d) {
        return 0;
    }
    base = d->hello_interval_ms ? d->hello_interval_ms : 3000;
    jitter = base * (d->jitter_pct > 100 ? 100 : d->jitter_pct) / 100u;
    if (jitter == 0) {
        return base;
    }
    r = disc_next_rand(d) % (jitter * 2u + 1u);
    delta = base + (int32_t)r - (int32_t)jitter;
    if (delta < 100) {
        delta = 100;
    }
    return (uint32_t)delta;
}

int ntk_discovery_add_peer(ntk_discovery_t *d, const char *host, uint16_t port)
{
    size_t i;

    if (!d || !host || !host[0] || port == 0 || strlen(host) >= NTK_DISC_NAME_MAX) {
        return NTK_DISC_ERR_INVALID;
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->target_count; i++) {
        if (d->targets[i].port == port && strcmp(d->targets[i].host, host) == 0) {
            ntk_mutex_unlock(d->lock);
            return NTK_DISC_OK;
        }
    }
    if (d->target_count >= DISC_MAX_TARGETS) {
        ntk_mutex_unlock(d->lock);
        return NTK_DISC_ERR_OVERLOAD;
    }
    disc_copy_str(d->targets[d->target_count].host,
                  sizeof(d->targets[d->target_count].host), host);
    d->targets[d->target_count].port = port;
    d->target_count++;
    ntk_mutex_unlock(d->lock);
    return NTK_DISC_OK;
}

void ntk_discovery_tick(ntk_discovery_t *d, uint64_t now_ms)
{
    size_t i;

    if (!d) {
        return;
    }
    ntk_mutex_lock(d->lock);
    /* Keys first: their TTL can be shorter than the peer's. */
    for (i = 0; i < d->key_cap; i++) {
        disc_key_t *entry = d->key_slots[i];

        if (entry && entry != (disc_key_t *)DISC_TOMB &&
            now_ms >= entry->expire_at) {
            disc_emit_key(d, entry, 1);
            disc_key_unlink(d, entry);
        }
    }
    for (i = 0; i < d->peer_cap; i++) {
        disc_peer_t *peer = d->peer_slots[i];

        if (peer && peer != (disc_peer_t *)DISC_TOMB &&
            now_ms - peer->last_seen_ms > d->ttl_ms) {
            disc_remove_instance_keys(d, peer->instance);
            disc_emit_peer(d, peer, 1);
            d->peer_slots[i] = DISC_TOMB;
            d->peer_used--;
            d->peer_tomb++;
            free(peer);
        }
    }
    for (i = 0; i < sizeof(d->frags) / sizeof(d->frags[0]); i++) {
        if (d->frags[i].used &&
            now_ms - d->frags[i].start_ms > DISC_FRAG_TIMEOUT_MS) {
            disc_frag_release(&d->frags[i]);
        }
    }
    ntk_mutex_unlock(d->lock);
}

uint16_t ntk_discovery_port(const ntk_discovery_t *d)
{
    return d ? d->port : 0;
}

size_t ntk_discovery_peer_count(const ntk_discovery_t *d)
{
    return d ? d->peer_used : 0;
}

static void disc_peer_info(const disc_peer_t *peer, ntk_disc_peer_info_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->instance, peer->instance, sizeof(out->instance));
    disc_copy_str(out->name, sizeof(out->name), peer->name);
    disc_copy_str(out->host, sizeof(out->host), peer->host);
    out->port = peer->port;
    out->endpoint_count = peer->endpoint_count;
    memcpy(out->endpoints, peer->endpoints,
           peer->endpoint_count * sizeof(peer->endpoints[0]));
    out->last_seen_ms = peer->last_seen_ms;
}

static void disc_key_info(const disc_key_t *key, ntk_disc_key_info_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->instance, key->instance, sizeof(out->instance));
    disc_copy_str(out->key, sizeof(out->key), key->key);
    out->value_len = key->value_len;
    if (key->value_len > 0) {
        memcpy(out->value, key->value, key->value_len);
    }
    out->ttl_ms = key->ttl_ms;
}

size_t ntk_discovery_peers(ntk_discovery_t *d,
                           ntk_disc_peer_info_t *out,
                           size_t max)
{
    size_t i;
    size_t n = 0;

    if (!d || !out || max == 0) {
        return 0;
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->peer_cap && n < max; i++) {
        disc_peer_t *peer = d->peer_slots[i];

        if (peer && peer != (disc_peer_t *)DISC_TOMB) {
            disc_peer_info(peer, &out[n++]);
        }
    }
    ntk_mutex_unlock(d->lock);
    return n;
}

size_t ntk_discovery_peer_keys(ntk_discovery_t *d,
                               const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                               ntk_disc_key_info_t *out,
                               size_t max)
{
    size_t i;
    size_t n = 0;

    if (!d || !instance || !out || max == 0) {
        return 0;
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->key_cap && n < max; i++) {
        disc_key_t *entry = d->key_slots[i];

        if (entry && entry != (disc_key_t *)DISC_TOMB &&
            memcmp(entry->instance, instance, NTK_DISC_INSTANCE_SIZE) == 0) {
            disc_key_info(entry, &out[n++]);
        }
    }
    ntk_mutex_unlock(d->lock);
    return n;
}

size_t ntk_discovery_lookup(ntk_discovery_t *d,
                            const char *key,
                            ntk_disc_key_info_t *out,
                            size_t max)
{
    size_t i;
    size_t n = 0;

    if (!d || !key || !out || max == 0) {
        return 0;
    }
    ntk_mutex_lock(d->lock);
    for (i = 0; i < d->key_cap && n < max; i++) {
        disc_key_t *entry = d->key_slots[i];

        if (entry && entry != (disc_key_t *)DISC_TOMB &&
            strcmp(entry->key, key) == 0) {
            disc_key_info(entry, &out[n++]);
        }
    }
    ntk_mutex_unlock(d->lock);
    return n;
}
