/*
 * nb_mesh - 定长心跳 + 编队隔离 + 契约/掩码比对 + 按需内省的组播成员发现。
 *
 * 线上格式（全部小端）：
 *
 *   固定头 40 字节，所有报文类型通用
 *     [0..1]   magic 'M','B'
 *     [2]      version (2)
 *     [3]      msg_type
 *     [4..7]   formation_id
 *     [8..15]  node_id
 *     [16..19] rpc_port
 *     [20..23] ipv4          显式通告地址（网络字节序），0 = 用数据报源地址
 *     [24..27] service_hash  该节点服务列表的版本号（只用于缓存失效）
 *     [28..31] mask_lo       服务 ID 0..31 的位图
 *     [32..35] mask_hi       服务 ID 32..63 的位图
 *     [36..39] seq           每发送方单调递增，用于去重/防回放
 *
 *   JOIN / HEARTBEAT / LEAVE   仅固定头，无负载
 *   MANIFEST_REQ               固定头 + target_node_id(u64)
 *   MANIFEST_RSP               固定头 + chunk_index(u16) chunk_total(u16)
 *                              + count(u16) + count × [len(u16) name[]]
 *
 * 收到报文后的判定顺序：长度 → magic/version → 类型 → 编队 ID → 自身 ID。
 * 编队 ID 不同直接丢弃，这是“广播风暴 + 无法隔离”两个问题的核心修法：
 * 报文不会进入任何表，也不会触发任何回调。
 *
 * 为什么没有 CRC：心跳是定长小报文、UDP 自带校验和，且所有字段参与严格
 * 校验；即使个别报文被破坏，最坏结果是多一个租约到期的对端，不会造成状态
 * 错乱。内省响应有长度前缀校验，畸形的会被整包丢弃。
 */

#include <nb/mesh.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESH_MAGIC0 'M'
#define MESH_MAGIC1 'B'

#define MESH_HDR_SIZE NB_MESH_PACKET_SIZE
#define MESH_MAX_DATAGRAM 1200u
#define MESH_MAX_TARGETS 32u
/* 内省响应最多几片：足够装下 128 个 64 字节的服务名。 */
#define MESH_MAX_CHUNKS 16u
#define MESH_MAX_MANIFEST_ATTEMPTS 5u
#define MESH_MANIFEST_BASE_MS 200u
#define MESH_MANIFEST_MAX_BACKOFF_MS 5000u
#define MESH_RATE_LIMIT_PER_SEC 2048u

/* 对端的 manifest 拉取状态使用 nb/mesh.h 里的公开枚举。 */
#define MESH_MANIFEST_NONE NB_MESH_MANIFEST_NONE
#define MESH_MANIFEST_REQUESTED NB_MESH_MANIFEST_REQUESTED
#define MESH_MANIFEST_READY NB_MESH_MANIFEST_READY

typedef struct mesh_peer {
    int used;
    uint64_t node_id;
    char host[NB_MESH_HOST_MAX];
    uint16_t port;
    uint32_t rpc_port;
    uint32_t ipv4;
    uint64_t service_mask;
    uint32_t service_hash;        /* 对端服务列表版本 */
    uint32_t cached_service_hash; /* 我缓存的那份列表的版本；0 = 未缓存 */
    uint32_t last_seq;
    uint64_t last_seen_ms;

    /* 按需内省 */
    int manifest_state;
    uint32_t manifest_attempts;
    uint64_t next_manifest_ms;
    char (*services)[NB_MESH_NAME_MAX];
    size_t service_count;

    /* 分片重组 */
    uint16_t chunk_total;
    uint16_t chunks_got;
    uint8_t *chunk[MESH_MAX_CHUNKS];
    uint16_t chunk_len[MESH_MAX_CHUNKS];
} mesh_peer_t;

struct nb_mesh {
    zn_socket *sock;
    zn_poller *poller;
    zb_mutex *lock;

    char bind_host[NB_MESH_HOST_MAX];
    char group[NB_MESH_HOST_MAX];
    uint16_t port;
    uint32_t formation_id;
    uint64_t node_id;
    uint32_t rpc_port;
    uint64_t service_mask;
    uint32_t service_hash;
    uint32_t ipv4;
    uint32_t heartbeat_ms;
    uint32_t lease_ms;
    uint32_t jitter_pct;
    uint32_t max_peers;
    int multicast_enabled;
    nb_mesh_callbacks_t callbacks;
    void *user;

    /* 本地 manifest */
    char (*local_services)[NB_MESH_NAME_MAX];
    char **local_names; /* 指向 local_services 的指针数组，供回调使用 */
    size_t local_count;
    uint32_t local_hash;
    int hash_from_config;

    mesh_peer_t *peers;

    struct {
        char host[NB_MESH_HOST_MAX];
        uint16_t port;
    } targets[MESH_MAX_TARGETS];
    size_t target_count;

    uint64_t seq;
    uint32_t rng;
    int announced;

    uint64_t rate_ms;
    uint32_t rate_count;
};

/* ---- 小工具 ------------------------------------------------------------- */

static void mesh_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void mesh_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void mesh_put_u64(uint8_t *p, uint64_t v)
{
    mesh_put_u32(p, (uint32_t)(v & 0xffffffffu));
    mesh_put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t mesh_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t mesh_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t mesh_get_u64(const uint8_t *p)
{
    return (uint64_t)mesh_get_u32(p) | ((uint64_t)mesh_get_u32(p + 4) << 32);
}

static void mesh_copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    (void)snprintf(dst, cap, "%s", src);
}

/* 把 "a.b.c.d" 解析成网络字节序 u32；失败或 NULL 返回 0（= 用源地址）。 */
static uint32_t mesh_parse_ipv4(const char *host)
{
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;

    if (!host || !host[0]) {
        return 0;
    }
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    if (a > 255u || b > 255u || c > 255u || d > 255u) {
        return 0;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static uint64_t mesh_now_ms(void)
{
    return zn_wall_ms();
}

static uint32_t mesh_next_rand(nb_mesh_t *m)
{
    uint32_t x = m->rng ? m->rng : 0x9e3779b9u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x ? x : 1u;
    return m->rng;
}

/* 由编队 ID 推导组播组，落在 239.255.0.0/16 的管理范围内。 */
static void mesh_default_group(uint32_t formation_id, char *out, size_t cap)
{
    uint32_t h = formation_id * 2654435761u + 0x9e3779b9u;
    unsigned a = 1u + (h & 0xffu) % 254u;
    unsigned b = 1u + ((h >> 8) & 0xffu) % 254u;

    (void)snprintf(out, cap, "239.255.%u.%u", a, b);
}

/* ---- 报文编解码 --------------------------------------------------------- */

typedef struct mesh_header {
    uint8_t msg_type;
    uint32_t formation_id;
    uint64_t node_id;
    uint32_t rpc_port;
    uint32_t ipv4;        /* 网络字节序；0 = 用数据报源地址 */
    uint32_t service_hash;
    uint32_t mask_lo;     /* 服务 ID 0..31 */
    uint32_t mask_hi;     /* 服务 ID 32..63 */
    uint32_t seq;
} mesh_header_t;

static void mesh_encode_header(uint8_t *buf, const mesh_header_t *h)
{
    buf[0] = (uint8_t)MESH_MAGIC0;
    buf[1] = (uint8_t)MESH_MAGIC1;
    buf[2] = (uint8_t)NB_MESH_VERSION;
    buf[3] = h->msg_type;
    mesh_put_u32(buf + 4, h->formation_id);
    mesh_put_u64(buf + 8, h->node_id);
    mesh_put_u32(buf + 16, h->rpc_port);
    mesh_put_u32(buf + 20, h->ipv4);
    mesh_put_u32(buf + 24, h->service_hash);
    mesh_put_u32(buf + 28, h->mask_lo);
    mesh_put_u32(buf + 32, h->mask_hi);
    mesh_put_u32(buf + 36, h->seq);
}

/* 解析固定头。返回 1 成功，0 表示不是本协议的报文。 */
static int mesh_decode_header(const uint8_t *buf, size_t len, mesh_header_t *out)
{
    if (!buf || !out || len < MESH_HDR_SIZE) {
        return 0;
    }
    if (buf[0] != (uint8_t)MESH_MAGIC0 || buf[1] != (uint8_t)MESH_MAGIC1 ||
        buf[2] != (uint8_t)NB_MESH_VERSION) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->msg_type = buf[3];
    out->formation_id = mesh_get_u32(buf + 4);
    out->node_id = mesh_get_u64(buf + 8);
    out->rpc_port = mesh_get_u32(buf + 16);
    out->ipv4 = mesh_get_u32(buf + 20);
    out->service_hash = mesh_get_u32(buf + 24);
    out->mask_lo = mesh_get_u32(buf + 28);
    out->mask_hi = mesh_get_u32(buf + 32);
    out->seq = mesh_get_u32(buf + 36);
    return 1;
}

static void mesh_fill_header(const nb_mesh_t *m, uint8_t type, mesh_header_t *h)
{
    memset(h, 0, sizeof(*h));
    h->msg_type = type;
    h->formation_id = m->formation_id;
    h->node_id = m->node_id;
    h->rpc_port = m->rpc_port;
    h->ipv4 = m->ipv4;
    h->service_hash = m->service_hash;
    h->mask_lo = (uint32_t)(m->service_mask & 0xffffffffu);
    h->mask_hi = (uint32_t)(m->service_mask >> 32);
    h->seq = (uint32_t)m->seq;
}

static void mesh_send_multicast(nb_mesh_t *m, const uint8_t *buf, size_t len)
{
    size_t i;

    if (m->multicast_enabled && m->group[0]) {
        (void)zn_socket_sendto(m->sock, buf, len, m->group, m->port);
    }
    for (i = 0; i < m->target_count; i++) {
        (void)zn_socket_sendto(m->sock, buf, len, m->targets[i].host,
                                m->targets[i].port);
    }
}

static void mesh_send_to(nb_mesh_t *m, const char *host, uint16_t port,
                         const uint8_t *buf, size_t len)
{
    if (!host || !host[0] || port == 0) {
        return;
    }
    (void)zn_socket_sendto(m->sock, buf, len, host, port);
}

/* ---- 对端表 ------------------------------------------------------------- */

static mesh_peer_t *mesh_peer_find(nb_mesh_t *m, uint64_t node_id)
{
    uint32_t i;

    for (i = 0; i < m->max_peers; i++) {
        if (m->peers[i].used && m->peers[i].node_id == node_id) {
            return &m->peers[i];
        }
    }
    return NULL;
}

static void mesh_peer_release_buffers(mesh_peer_t *peer)
{
    uint32_t i;

    free(peer->services);
    peer->services = NULL;
    peer->service_count = 0;
    for (i = 0; i < MESH_MAX_CHUNKS; i++) {
        free(peer->chunk[i]);
        peer->chunk[i] = NULL;
        peer->chunk_len[i] = 0;
    }
    peer->chunk_total = 0;
    peer->chunks_got = 0;
}

static void mesh_peer_fill_info(const nb_mesh_t *m, const mesh_peer_t *peer,
                                nb_mesh_peer_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->node_id = peer->node_id;
    mesh_copy_str(out->host, sizeof(out->host), peer->host);
    out->port = peer->port;
    out->rpc_port = peer->rpc_port;
    out->ipv4 = peer->ipv4;
    out->service_mask = peer->service_mask;
    out->service_hash = peer->service_hash;
    out->cached_service_hash = peer->cached_service_hash;
    out->manifest_state = peer->manifest_state;
    out->service_count = peer->service_count;
    out->last_seen_ms = peer->last_seen_ms;
}

static void mesh_emit_peer(nb_mesh_t *m, const mesh_peer_t *peer, int removed)
{
    nb_mesh_peer_info_t info;

    if (!m->callbacks.on_peer) {
        return;
    }
    mesh_peer_fill_info(m, peer, &info);
    m->callbacks.on_peer(m->user, &info, removed);
}

/* 年龄最小的槽位用于淘汰；表满时不静默丢弃新节点。 */
static mesh_peer_t *mesh_peer_insert(nb_mesh_t *m, uint64_t node_id)
{
    mesh_peer_t *victim = NULL;
    uint32_t i;

    for (i = 0; i < m->max_peers; i++) {
        mesh_peer_t *peer = &m->peers[i];

        if (!peer->used) {
            victim = peer;
            break;
        }
        if (!victim || peer->last_seen_ms < victim->last_seen_ms) {
            victim = peer;
        }
    }
    if (!victim) {
        return NULL;
    }
    if (victim->used) {
        /* 表满：淘汰最久未见的一个，先通知上层。 */
        mesh_emit_peer(m, victim, 1);
        mesh_peer_release_buffers(victim);
    }
    memset(victim, 0, sizeof(*victim));
    victim->used = 1;
    victim->node_id = node_id;
    return victim;
}

/* ---- manifest ----------------------------------------------------------- */

void nb_mesh_config_init(nb_mesh_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->port = NB_MESH_DEFAULT_PORT;
    config->heartbeat_ms = NB_MESH_DEFAULT_HEARTBEAT_MS;
    config->lease_ms = NB_MESH_DEFAULT_LEASE_MS;
    config->jitter_pct = 20;
    config->max_peers = NB_MESH_MAX_PEERS;
}

uint32_t nb_mesh_service_hash(const char *const *names, size_t count)
{
    uint32_t h = 2166136261u;
    size_t i;

    if (!names || count == 0) {
        return h;
    }
    /* 顺序无关：逐轮挑出当前最小的未处理名字（count 很小，选择排序足够）。 */
    {
        const size_t max = 512u;
        size_t n = count < max ? count : max;
        char visited[512];
        size_t round;

        memset(visited, 0, sizeof(visited));
        for (round = 0; round < n; round++) {
            size_t best = SIZE_MAX;

            for (i = 0; i < n; i++) {
                if (visited[i]) {
                    continue;
                }
                if (best == SIZE_MAX ||
                    strcmp(names[i] ? names[i] : "", names[best] ? names[best] : "") < 0) {
                    best = i;
                }
            }
            if (best == SIZE_MAX) {
                break;
            }
            visited[best] = 1;
            if (names[best]) {
                const char *p = names[best];

                while (*p) {
                    h ^= (uint8_t)*p++;
                    h *= 16777619u;
                }
            }
            h ^= 0xffu; /* 名字分隔符，避免 ("ab","c") 与 ("a","bc") 相撞 */
            h *= 16777619u;
        }
    }
    return h;
}

uint64_t nb_mesh_service_bit(const char *name)
{
    uint32_t h = 2166136261u;

    if (!name) {
        return 0;
    }
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return (uint64_t)1 << (h % (uint32_t)NB_MESH_SERVICE_BITS);
}

static void mesh_refresh_hash(nb_mesh_t *m)
{
    if (m->hash_from_config) {
        return;
    }
    m->local_hash = nb_mesh_service_hash((const char *const *)m->local_names,
                                         m->local_count);
    m->service_hash = m->local_hash;
}

int nb_mesh_set_services(nb_mesh_t *m, const char *const *names, size_t count)
{
    char (*copy)[NB_MESH_NAME_MAX] = NULL;
    char **index = NULL;
    size_t i;

    if (!m || (count > 0 && !names) || count > NB_MESH_MAX_SERVICES) {
        return NB_MESH_ERR_INVALID;
    }
    if (count > 0) {
        copy = (char (*)[NB_MESH_NAME_MAX])calloc(count, NB_MESH_NAME_MAX);
        index = (char **)calloc(count, sizeof(*index));
        if (!copy || !index) {
            free(copy);
            free(index);
            return NB_MESH_ERR_NOMEM;
        }
        for (i = 0; i < count; i++) {
            mesh_copy_str(copy[i], NB_MESH_NAME_MAX, names[i]);
            index[i] = copy[i];
        }
    }
    zb_mutex_lock(m->lock);
    free(m->local_services);
    free(m->local_names);
    m->local_services = copy;
    m->local_names = index;
    m->local_count = count;
    mesh_refresh_hash(m);
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

/* 把本地 manifest 分片编码后单播给某个对端。 */
static void mesh_send_manifest(nb_mesh_t *m, const mesh_peer_t *peer)
{
    uint8_t buf[MESH_MAX_DATAGRAM];
    mesh_header_t h;
    size_t entries[MESH_MAX_CHUNKS][NB_MESH_MAX_SERVICES];
    uint16_t counts[MESH_MAX_CHUNKS];
    uint16_t total = 1;
    uint16_t chunk = 0;
    size_t used = 0;
    size_t i;

    /* 先按 datagram 预算分组。 */
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < m->local_count; i++) {
        size_t nlen = strlen(m->local_names[i]);
        size_t need = 2u + nlen;

        if (MESH_HDR_SIZE + 6u + used + need > MESH_MAX_DATAGRAM) {
            if (total + 1u > MESH_MAX_CHUNKS) {
                break; /* 装不下就截断，剩余服务由对端按需再问 */
            }
            total++;
            used = 0;
        }
        entries[total - 1u][counts[total - 1u]++] = i;
        used += need;
    }

    for (chunk = 0; chunk < total; chunk++) {
        size_t off = MESH_HDR_SIZE;

        mesh_fill_header(m, NB_MESH_MSG_MANIFEST_RSP, &h);
        mesh_encode_header(buf, &h);
        mesh_put_u16(buf + off, chunk);
        mesh_put_u16(buf + off + 2, total);
        mesh_put_u16(buf + off + 4, counts[chunk]);
        off += 6u;
        for (i = 0; i < counts[chunk]; i++) {
            const char *name = m->local_names[entries[chunk][i]];
            size_t nlen = strlen(name);

            mesh_put_u16(buf + off, (uint16_t)nlen);
            off += 2;
            memcpy(buf + off, name, nlen);
            off += nlen;
        }
        mesh_send_to(m, peer->host, peer->port, buf, off);
    }
}

static int mesh_peer_manifest_add(mesh_peer_t *peer, const char *name, size_t len)
{
    char (*grown)[NB_MESH_NAME_MAX];
    size_t cap;

    if (len == 0 || len >= NB_MESH_NAME_MAX) {
        return -1;
    }
    if (peer->service_count >= NB_MESH_MAX_SERVICES) {
        return -1;
    }
    if (peer->service_count == 0 || (peer->service_count & (peer->service_count - 1u)) == 0) {
        cap = peer->service_count == 0 ? 8u : peer->service_count * 2u;
        if (cap > NB_MESH_MAX_SERVICES) {
            cap = NB_MESH_MAX_SERVICES;
        }
        grown = (char (*)[NB_MESH_NAME_MAX])realloc(
            peer->services, cap * NB_MESH_NAME_MAX);
        if (!grown) {
            return -1;
        }
        peer->services = grown;
    }
    memcpy(peer->services[peer->service_count], name, len);
    peer->services[peer->service_count][len] = '\0';
    peer->service_count++;
    return 0;
}

static void mesh_emit_manifest(nb_mesh_t *m, mesh_peer_t *peer)
{
    nb_mesh_peer_info_t info;
    char **names;
    size_t i;

    if (!m->callbacks.on_manifest || peer->service_count == 0) {
        return;
    }
    names = (char **)calloc(peer->service_count, sizeof(*names));
    if (!names) {
        return;
    }
    for (i = 0; i < peer->service_count; i++) {
        names[i] = peer->services[i];
    }
    mesh_peer_fill_info(m, peer, &info);
    info.service_count = peer->service_count;
    m->callbacks.on_manifest(m->user, &info, (const char *const *)names,
                             peer->service_count);
    free(names);
}

/* 已收到全部片后解析并应用。调用方持有锁。 */
static void mesh_apply_manifest(nb_mesh_t *m, mesh_peer_t *peer)
{
    uint8_t *body;
    size_t total_len = 0;
    size_t off = 0;
    uint32_t i;

    for (i = 0; i < peer->chunk_total; i++) {
        total_len += peer->chunk_len[i];
    }
    body = (uint8_t *)malloc(total_len ? total_len : 1u);
    if (!body) {
        return;
    }
    total_len = 0;
    for (i = 0; i < peer->chunk_total; i++) {
        memcpy(body + total_len, peer->chunk[i], peer->chunk_len[i]);
        total_len += peer->chunk_len[i];
        free(peer->chunk[i]);
        peer->chunk[i] = NULL;
        peer->chunk_len[i] = 0;
    }
    peer->chunk_total = 0;
    peer->chunks_got = 0;

    free(peer->services);
    peer->services = NULL;
    peer->service_count = 0;

    {
        /* 每片都带 6 字节前缀；拼起来后逐片再走一遍解析。 */
        size_t cursor = 0;
        uint16_t expect_total = 0;

        while (cursor + 6u <= total_len) {
            uint16_t index = mesh_get_u16(body + cursor);
            uint16_t nchunks = mesh_get_u16(body + cursor + 2);
            uint16_t count = mesh_get_u16(body + cursor + 4);

            (void)index;
            if (nchunks == 0 || nchunks > MESH_MAX_CHUNKS) {
                break;
            }
            if (expect_total == 0) {
                expect_total = nchunks;
            } else if (expect_total != nchunks) {
                break;
            }
            cursor += 6u;
            for (i = 0; i < count; i++) {
                uint16_t nlen;

                if (cursor + 2u > total_len) {
                    goto done;
                }
                nlen = mesh_get_u16(body + cursor);
                cursor += 2u;
                if (cursor + nlen > total_len) {
                    goto done;
                }
                (void)mesh_peer_manifest_add(peer, (const char *)body + cursor, nlen);
                cursor += nlen;
            }
        }
    }
done:
    free(body);
    off = 0;
    (void)off;
    if (peer->service_count > 0) {
        peer->manifest_state = MESH_MANIFEST_READY;
        peer->cached_service_hash = peer->service_hash;
        mesh_emit_manifest(m, peer);
    }
}

/* ---- 报文处理 ----------------------------------------------------------- */

static void mesh_request_manifest_locked(nb_mesh_t *m, mesh_peer_t *peer, uint64_t now)
{
    uint8_t buf[MESH_HDR_SIZE + 8u];
    mesh_header_t h;

    if (peer->manifest_state == MESH_MANIFEST_READY) {
        return;
    }
    mesh_fill_header(m, NB_MESH_MSG_MANIFEST_REQ, &h);
    mesh_encode_header(buf, &h);
    mesh_put_u64(buf + MESH_HDR_SIZE, peer->node_id);
    mesh_send_to(m, peer->host, peer->port, buf, sizeof(buf));
    peer->manifest_state = MESH_MANIFEST_REQUESTED;
    peer->manifest_attempts++;
    {
        uint32_t backoff = MESH_MANIFEST_BASE_MS;

        if (peer->manifest_attempts > 1u) {
            backoff <<= (peer->manifest_attempts - 1u);
            if (backoff > MESH_MANIFEST_MAX_BACKOFF_MS) {
                backoff = MESH_MANIFEST_MAX_BACKOFF_MS;
            }
        }
        peer->next_manifest_ms = now + backoff;
    }
}

static void mesh_on_manifest_rsp(nb_mesh_t *m, const mesh_header_t *h,
                                 const uint8_t *payload, size_t len)
{
    mesh_peer_t *peer;
    uint16_t index;
    uint16_t total;
    uint8_t *copy;

    if (len < 6u) {
        return;
    }
    index = mesh_get_u16(payload);
    total = mesh_get_u16(payload + 2);
    if (total == 0 || total > MESH_MAX_CHUNKS || index >= total) {
        return;
    }
    zb_mutex_lock(m->lock);
    peer = mesh_peer_find(m, h->node_id);
    if (!peer) {
        zb_mutex_unlock(m->lock);
        return;
    }
    if (peer->chunk_total != total) {
        uint32_t i;

        for (i = 0; i < MESH_MAX_CHUNKS; i++) {
            free(peer->chunk[i]);
            peer->chunk[i] = NULL;
            peer->chunk_len[i] = 0;
        }
        peer->chunk_total = total;
        peer->chunks_got = 0;
    }
    if (peer->chunk[index]) {
        zb_mutex_unlock(m->lock);
        return;
    }
    copy = (uint8_t *)malloc(len);
    if (!copy) {
        zb_mutex_unlock(m->lock);
        return;
    }
    memcpy(copy, payload, len);
    peer->chunk[index] = copy;
    peer->chunk_len[index] = (uint16_t)len;
    peer->chunks_got++;
    if (peer->chunks_got == peer->chunk_total) {
        mesh_apply_manifest(m, peer);
    }
    zb_mutex_unlock(m->lock);
}

static void mesh_on_member(nb_mesh_t *m, const mesh_header_t *h,
                           const char *host, uint16_t port, int leave)
{
    mesh_peer_t *peer;
    uint64_t now = mesh_now_ms();
    int is_new = 0;
    int hash_changed = 0;

    zb_mutex_lock(m->lock);
    peer = mesh_peer_find(m, h->node_id);
    if (leave) {
        if (peer) {
            mesh_emit_peer(m, peer, 1);
            mesh_peer_release_buffers(peer);
            memset(peer, 0, sizeof(*peer));
        }
        zb_mutex_unlock(m->lock);
        return;
    }
    if (!peer) {
        peer = mesh_peer_insert(m, h->node_id);
        if (!peer) {
            zb_mutex_unlock(m->lock);
            return;
        }
        is_new = 1;
    }
    /* 同一发送方只接受更新的序号；重启（node_id 相同但 seq 归零）也覆盖。 */
    if (!is_new && h->seq != 0 && peer->last_seq != 0 && h->seq <= peer->last_seq) {
        peer->last_seen_ms = now;
        zb_mutex_unlock(m->lock);
        return;
    }
    /* 版本号变了 = 我缓存的列表可能过期；需要时再拉。 */
    hash_changed = !is_new && peer->service_hash != h->service_hash;
    /* 报文显式 IPv4 优先；没填则退回数据报源地址。 */
    if (h->ipv4 != 0) {
        uint32_t ip = h->ipv4;

        peer->ipv4 = ip;
        (void)snprintf(peer->host, sizeof(peer->host), "%u.%u.%u.%u",
                       (unsigned)((ip >> 24) & 0xffu), (unsigned)((ip >> 16) & 0xffu),
                       (unsigned)((ip >> 8) & 0xffu), (unsigned)(ip & 0xffu));
    } else {
        peer->ipv4 = 0;
        mesh_copy_str(peer->host, sizeof(peer->host), host);
    }
    peer->port = port;
    peer->rpc_port = h->rpc_port;
    peer->service_mask = ((uint64_t)h->mask_hi << 32) | (uint64_t)h->mask_lo;
    peer->service_hash = h->service_hash;
    peer->last_seq = h->seq;
    peer->last_seen_ms = now;

    if (is_new || hash_changed) {
        /* 版本变了：缓存的服务列表作废，下次需要时重新内省。 */
        mesh_peer_release_buffers(peer);
        peer->manifest_state = MESH_MANIFEST_NONE;
        peer->manifest_attempts = 0;
        peer->next_manifest_ms = 0;
        peer->cached_service_hash = 0;
        mesh_emit_peer(m, peer, 0);
    }
    zb_mutex_unlock(m->lock);
}

static void mesh_on_datagram(nb_mesh_t *m, const char *host, uint16_t port,
                             const uint8_t *buf, size_t len)
{
    mesh_header_t h;

    if (!mesh_decode_header(buf, len, &h)) {
        return;
    }
    /* 编队隔离：不同编队的报文在这里就被丢弃，不进表、不触发回调。 */
    if (h.formation_id != m->formation_id) {
        return;
    }
    /* 自己的报文（组播回环）。 */
    if (h.node_id == m->node_id) {
        return;
    }
    switch (h.msg_type) {
    case NB_MESH_MSG_JOIN:
    case NB_MESH_MSG_HEARTBEAT:
        mesh_on_member(m, &h, host, port, 0);
        break;
    case NB_MESH_MSG_LEAVE:
        mesh_on_member(m, &h, host, port, 1);
        break;
    case NB_MESH_MSG_MANIFEST_REQ: {
        mesh_peer_t *peer;

        if (len < MESH_HDR_SIZE + 8u) {
            return;
        }
        if (mesh_get_u64(buf + MESH_HDR_SIZE) != m->node_id) {
            return; /* 不是问我的（地址被历史复用） */
        }
        zb_mutex_lock(m->lock);
        peer = mesh_peer_find(m, h.node_id);
        if (peer) {
            mesh_send_manifest(m, peer);
        }
        zb_mutex_unlock(m->lock);
        break;
    }
    case NB_MESH_MSG_MANIFEST_RSP:
        mesh_on_manifest_rsp(m, &h, buf + MESH_HDR_SIZE, len - MESH_HDR_SIZE);
        break;
    default:
        break;
    }
}

static void mesh_on_readable(zn_socket *sock, void *user)
{
    nb_mesh_t *m = (nb_mesh_t *)user;
    uint8_t buf[MESH_MAX_DATAGRAM];

    (void)sock;
    for (;;) {
        char host[NB_MESH_HOST_MAX];
        uint16_t port = 0;
        zn_ssize_t n = zn_socket_recvfrom(m->sock, buf, sizeof(buf), host,
                                            sizeof(host), &port);
        uint64_t now;

        if (n <= 0) {
            break;
        }
        now = mesh_now_ms();
        if (now - m->rate_ms >= 1000u) {
            m->rate_ms = now;
            m->rate_count = 0;
        }
        if (++m->rate_count > MESH_RATE_LIMIT_PER_SEC) {
            continue;
        }
        mesh_on_datagram(m, host, port, buf, (size_t)n);
    }
}

/* ---- 生命周期 ----------------------------------------------------------- */

nb_mesh_t *nb_mesh_create(zn_poller *poller, const nb_mesh_config_t *config,
                          int *error_out)
{
    nb_mesh_t *m;
    nb_mesh_config_t defaults;
    zn_socket_callbacks_t cbs;
    uint16_t actual = 0;
    char group[NB_MESH_HOST_MAX];
    int rc = NB_MESH_OK;

    if (!poller) {
        if (error_out) {
            *error_out = NB_MESH_ERR_INVALID;
        }
        return NULL;
    }
    if (!config || config->struct_size < sizeof(nb_mesh_config_t)) {
        nb_mesh_config_init(&defaults);
        config = &defaults;
    }
    m = (nb_mesh_t *)calloc(1, sizeof(*m));
    if (!m) {
        if (error_out) {
            *error_out = NB_MESH_ERR_NOMEM;
        }
        return NULL;
    }
    m->poller = poller;
    m->port = config->port ? config->port : NB_MESH_DEFAULT_PORT;
    m->formation_id = config->formation_id;
    m->node_id = config->node_id;
    m->rpc_port = config->rpc_port;
    m->service_mask = config->service_mask;
    m->ipv4 = mesh_parse_ipv4(config->advertise_host);
    m->heartbeat_ms = config->heartbeat_ms ? config->heartbeat_ms
                                           : NB_MESH_DEFAULT_HEARTBEAT_MS;
    m->lease_ms = config->lease_ms ? config->lease_ms : NB_MESH_DEFAULT_LEASE_MS;
    m->jitter_pct = config->jitter_pct > 100u ? 100u : config->jitter_pct;
    m->max_peers = config->max_peers ? config->max_peers : NB_MESH_MAX_PEERS;
    if (m->max_peers > NB_MESH_MAX_PEERS) {
        m->max_peers = NB_MESH_MAX_PEERS;
    }
    m->callbacks = config->callbacks;
    m->user = config->user;
    if (config->service_hash != 0) {
        m->service_hash = config->service_hash;
        m->hash_from_config = 1;
    }
    mesh_copy_str(m->bind_host, sizeof(m->bind_host), config->bind_host);
    m->rng = (uint32_t)mesh_now_ms() ^ (uint32_t)(uintptr_t)m;
    if (m->rng == 0) {
        m->rng = 1;
    }
    if (m->node_id == 0) {
        uint64_t hi = mesh_next_rand(m);
        uint64_t lo = mesh_next_rand(m);

        m->node_id = (hi << 32) | lo;
        if (m->node_id == 0) {
            m->node_id = 1;
        }
    }
    if (config->multicast_group == NULL) {
        mesh_default_group(m->formation_id, group, sizeof(group));
        mesh_copy_str(m->group, sizeof(m->group), group);
        m->multicast_enabled = 1;
    } else if (config->multicast_group[0]) {
        mesh_copy_str(m->group, sizeof(m->group), config->multicast_group);
        m->multicast_enabled = 1;
    } else {
        m->group[0] = '\0';
        m->multicast_enabled = 0;
    }

    m->peers = (mesh_peer_t *)calloc(m->max_peers, sizeof(*m->peers));
    m->lock = zb_mutex_create(ZB_MUTEX_RECURSIVE);
    m->sock = zn_socket_create();
    if (!m->peers || !m->lock || !m->sock) {
        rc = NB_MESH_ERR_NOMEM;
        goto fail;
    }
#if defined(_WIN32)
    {
        const int reuse_port = 0;
#else
    {
        const int reuse_port = 1;
#endif
        if (zn_socket_bind_udp_ex(m->sock,
                                   m->bind_host[0] ? m->bind_host : "0.0.0.0",
                                   m->port, 1, reuse_port) != ZN_OK) {
            rc = NB_MESH_ERR_IO;
            goto fail;
        }
    }
    if (m->multicast_enabled) {
        if (zn_socket_join_multicast(m->sock, m->group,
                                      m->bind_host[0] ? m->bind_host : NULL) != ZN_OK) {
            /* 组播不可用不致命：仍可用单播目标。 */
            m->multicast_enabled = 0;
        } else {
            (void)zn_socket_set_multicast_ttl(m->sock, 1);
            (void)zn_socket_set_multicast_loop(m->sock, 1);
        }
    }
    if (zn_socket_get_local(m->sock, NULL, 0, &actual) == ZN_OK && actual != 0) {
        m->port = actual;
    }
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_readable = mesh_on_readable;
    if (zn_socket_attach_poller(m->sock, poller, &cbs, m) != ZN_OK) {
        rc = NB_MESH_ERR_IO;
        goto fail;
    }
    if (error_out) {
        *error_out = NB_MESH_OK;
    }
    return m;

fail:
    if (error_out) {
        *error_out = rc;
    }
    nb_mesh_destroy(m);
    return NULL;
}

void nb_mesh_destroy(nb_mesh_t *m)
{
    uint32_t i;

    if (!m) {
        return;
    }
    if (m->sock) {
        zn_socket_detach_poller(m->sock);
        zn_socket_destroy(m->sock);
        m->sock = NULL;
    }
    if (m->peers) {
        for (i = 0; i < m->max_peers; i++) {
            mesh_peer_release_buffers(&m->peers[i]);
        }
        free(m->peers);
        m->peers = NULL;
    }
    free(m->local_services);
    free(m->local_names);
    if (m->lock) {
        zb_mutex_destroy(m->lock);
        m->lock = NULL;
    }
    free(m);
}

int nb_mesh_announce(nb_mesh_t *m)
{
    uint8_t buf[MESH_HDR_SIZE];
    mesh_header_t h;

    if (!m) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->lock);
    m->seq++;
    mesh_fill_header(m, m->announced ? NB_MESH_MSG_HEARTBEAT : NB_MESH_MSG_JOIN, &h);
    m->announced = 1;
    mesh_encode_header(buf, &h);
    mesh_send_multicast(m, buf, sizeof(buf));
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

int nb_mesh_leave(nb_mesh_t *m)
{
    uint8_t buf[MESH_HDR_SIZE];
    mesh_header_t h;

    if (!m) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->lock);
    m->seq++;
    mesh_fill_header(m, NB_MESH_MSG_LEAVE, &h);
    mesh_encode_header(buf, &h);
    mesh_send_multicast(m, buf, sizeof(buf));
    m->announced = 0;
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

int nb_mesh_add_peer(nb_mesh_t *m, const char *host, uint16_t port)
{
    size_t i;

    if (!m || !host || !host[0] || port == 0 ||
        strlen(host) >= sizeof(m->targets[0].host)) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->lock);
    for (i = 0; i < m->target_count; i++) {
        if (m->targets[i].port == port && strcmp(m->targets[i].host, host) == 0) {
            zb_mutex_unlock(m->lock);
            return NB_MESH_OK;
        }
    }
    if (m->target_count >= sizeof(m->targets) / sizeof(m->targets[0])) {
        zb_mutex_unlock(m->lock);
        return NB_MESH_ERR_OVERLOAD;
    }
    mesh_copy_str(m->targets[m->target_count].host,
                  sizeof(m->targets[m->target_count].host), host);
    m->targets[m->target_count].port = port;
    m->target_count++;
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

int nb_mesh_request_manifest(nb_mesh_t *m, uint64_t node_id)
{
    mesh_peer_t *peer;

    if (!m) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->lock);
    peer = mesh_peer_find(m, node_id);
    if (!peer) {
        zb_mutex_unlock(m->lock);
        return NB_MESH_ERR_NOTFOUND;
    }
    if (peer->manifest_state == MESH_MANIFEST_READY) {
        zb_mutex_unlock(m->lock);
        return NB_MESH_OK;
    }
    if (peer->manifest_attempts >= MESH_MAX_MANIFEST_ATTEMPTS) {
        /* 已经放弃过：给一次新的机会，但仍按退避节流。 */
        peer->manifest_attempts = 0;
    }
    mesh_request_manifest_locked(m, peer, mesh_now_ms());
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

void nb_mesh_tick(nb_mesh_t *m, uint64_t now_ms)
{
    uint32_t i;

    if (!m) {
        return;
    }
    zb_mutex_lock(m->lock);
    for (i = 0; i < m->max_peers; i++) {
        mesh_peer_t *peer = &m->peers[i];

        if (!peer->used) {
            continue;
        }
        if (now_ms >= peer->last_seen_ms && now_ms - peer->last_seen_ms > m->lease_ms) {
            mesh_emit_peer(m, peer, 1);
            mesh_peer_release_buffers(peer);
            memset(peer, 0, sizeof(*peer));
            continue;
        }
        /* 只对“已经请求过但重试超时”的拉取做重试；新对端等上层按需触发。 */
        if (peer->manifest_state == MESH_MANIFEST_REQUESTED &&
            peer->manifest_attempts < MESH_MAX_MANIFEST_ATTEMPTS &&
            peer->next_manifest_ms != 0 && now_ms >= peer->next_manifest_ms) {
            mesh_request_manifest_locked(m, peer, now_ms);
        }
    }
    zb_mutex_unlock(m->lock);
}

uint32_t nb_mesh_next_delay_ms(nb_mesh_t *m)
{
    uint32_t base;
    uint32_t jitter;
    int32_t delta;
    uint32_t r;

    if (!m) {
        return 0;
    }
    base = m->heartbeat_ms ? m->heartbeat_ms : NB_MESH_DEFAULT_HEARTBEAT_MS;
    jitter = base * m->jitter_pct / 100u;
    if (jitter == 0) {
        return base;
    }
    zb_mutex_lock(m->lock);
    r = mesh_next_rand(m) % (jitter * 2u + 1u);
    zb_mutex_unlock(m->lock);
    delta = (int32_t)base + (int32_t)r - (int32_t)jitter;
    if (delta < 50) {
        delta = 50;
    }
    return (uint32_t)delta;
}

uint16_t nb_mesh_port(const nb_mesh_t *m)
{
    return m ? m->port : 0;
}

size_t nb_mesh_peer_count(const nb_mesh_t *m)
{
    uint32_t i;
    size_t n = 0;

    if (!m) {
        return 0;
    }
    for (i = 0; i < m->max_peers; i++) {
        if (m->peers[i].used) {
            n++;
        }
    }
    return n;
}

uint64_t nb_mesh_node_id(const nb_mesh_t *m)
{
    return m ? m->node_id : 0;
}

uint32_t nb_mesh_service_hash_value(const nb_mesh_t *m)
{
    return m ? m->service_hash : 0;
}

int nb_mesh_set_service_mask(nb_mesh_t *m, uint64_t mask)
{
    if (!m) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->lock);
    m->service_mask = mask;
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

size_t nb_mesh_peers(nb_mesh_t *m, nb_mesh_peer_info_t *out, size_t max)
{
    uint32_t i;
    size_t n = 0;

    if (!m || !out || max == 0) {
        return 0;
    }
    zb_mutex_lock(m->lock);
    for (i = 0; i < m->max_peers && n < max; i++) {
        if (m->peers[i].used) {
            mesh_peer_fill_info(m, &m->peers[i], &out[n++]);
        }
    }
    zb_mutex_unlock(m->lock);
    return n;
}

const char *nb_mesh_strerror(int error)
{
    switch (error) {
    case NB_MESH_OK: return "success";
    case NB_MESH_ERR_INVALID: return "invalid argument";
    case NB_MESH_ERR_NOMEM: return "out of memory";
    case NB_MESH_ERR_IO: return "I/O error";
    case NB_MESH_ERR_TOOBIG: return "value too large";
    case NB_MESH_ERR_STATE: return "invalid state";
    case NB_MESH_ERR_OVERLOAD: return "table full";
    case NB_MESH_ERR_NOTFOUND: return "not found";
    default: return "unknown error";
    }
}
