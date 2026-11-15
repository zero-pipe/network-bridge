/* nb_mesh：定长组播心跳 + 哈希邻居表 + 掩码寻址。见 include/nb/mesh.h。
 *
 * 三个执行体共用一条内部 poller 线程：
 *   recv  = zn_socket 可读事件（组播 socket 绑在 poller 上）；
 *   send  = zn_poller_do_delay 自续期回调（Fast-Join 3 连发 -> 带抖动心跳）；
 *   sweep = zn_timer 固定 200 ms（租约过期剔除）。
 * 只依赖 znet + zbase，且不引入阻塞式 recv，因此退出时只需置 stop_flag +
 * wake 即可 join，不存在"recvfrom 卡住无法退出"的问题。
 */
#include <nb/mesh.h>

#include "mesh_table.h"

#include <zn/zn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 地址字符串缓冲区（组播组 / 绑定地址）。 */
#define NB_MESH_STR_MAX NB_MESH_ADDR_MAX
/* 一次可读事件最多处理的报文数：避免持续高压时饿死同一线程上的定时器。 */
#define NB_MESH_RECV_BURST 64
/* 接收缓冲区：略大于线长，超长报文按"长度不符"丢弃。 */
#define NB_MESH_RECV_BUF 64

/* 定向单播目标（组播不可用时的兜底）。 */
typedef struct nb_mesh_unicast_target {
    char ip[NB_MESH_IPV4_STR_MAX];
    uint16_t port;
} nb_mesh_unicast_target_t;

struct nb_mesh {
    /* ---- 配置快照（启动后不再改变） ---- */
    uint32_t formation_id;
    uint64_t node_id;
    uint32_t ipv4; /* 主机序；0 = 用数据报源地址 */
    uint16_t rpc_port;
    uint32_t service_mask;
    uint32_t heartbeat_ms;
    uint32_t lease_ms;
    uint32_t jitter_ms;
    char bind_host[NB_MESH_STR_MAX];
    char group[NB_MESH_STR_MAX];
    uint16_t port;
    int multicast; /* 是否真的加入了组播 */
    int running;

    /* ---- 执行体 ---- */
    zn_socket *sock;
    zn_poller *poller;
    zb_thread *thread;
    volatile int stop_flag;
    zn_poller_timer *send_timer;
    zn_timer *sweep_timer;
    uint32_t fast_join_left;

    /* ---- 共享状态 ---- */
    zb_mutex *lock;      /* 邻居表 + 服务索引 */
    zb_mutex *send_lock; /* 定向目标表 + 发送 */
    nb_mesh_table_t table;

    nb_mesh_unicast_target_t targets[NB_MESH_MAX_TARGETS];
    size_t target_count;

    uint32_t rng;
};

/* ---- 小工具 ---------------------------------------------------------- */

static void mesh_copy_str(char *dst, size_t cap, const char *src)
{
    size_t len;

    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 点分十进制 -> 主机序 u32；非法返回 0（0.0.0.0 也返回 0，语义上等价）。 */
static uint32_t mesh_ipv4_parse(const char *text)
{
    uint32_t value = 0;
    int part;

    if (!text) {
        return 0;
    }
    for (part = 0; part < 4; part++) {
        unsigned octet = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9' && digits < 3) {
            octet = octet * 10u + (unsigned)(*text - '0');
            text++;
            digits++;
        }
        if (digits == 0 || octet > 255u) {
            return 0;
        }
        value = (value << 8) | octet;
        if (part < 3) {
            if (*text != '.') {
                return 0;
            }
            text++;
        }
    }
    if (*text != '\0') {
        return 0;
    }
    return value;
}

/* 主机序 u32 -> 点分十进制。 */
static void mesh_ipv4_format(uint32_t ipv4, char *out, size_t cap)
{
    if (!out || cap < NB_MESH_IPV4_STR_MAX) {
        if (out && cap) {
            out[0] = '\0';
        }
        return;
    }
    (void)snprintf(out,
                   cap,
                   "%u.%u.%u.%u",
                   (unsigned)((ipv4 >> 24) & 0xffu),
                   (unsigned)((ipv4 >> 16) & 0xffu),
                   (unsigned)((ipv4 >> 8) & 0xffu),
                   (unsigned)(ipv4 & 0xffu));
}

/* ---- 线上编解码（全大端） -------------------------------------------- */

static void mesh_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void mesh_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void mesh_wr_u64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

static uint16_t mesh_rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t mesh_rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t mesh_rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

int nb_mesh_encode(const nb_mesh_packet_t *pkt, uint8_t out[NB_MESH_WIRE_SIZE])
{
    if (!pkt || !out) {
        return NB_MESH_ERR_INVALID;
    }
    if (pkt->msg_type != NB_MESH_MSG_JOIN && pkt->msg_type != NB_MESH_MSG_HEARTBEAT &&
        pkt->msg_type != NB_MESH_MSG_LEAVE) {
        return NB_MESH_ERR_INVALID;
    }
    mesh_wr_u16(out, NB_MESH_MAGIC);
    out[2] = pkt->msg_type;
    out[3] = 0; /* reserved：发送恒为 0，接收忽略 */
    mesh_wr_u32(out + 4, pkt->formation_id);
    mesh_wr_u64(out + 8, pkt->node_id);
    mesh_wr_u32(out + 16, pkt->real_ip);
    mesh_wr_u16(out + 20, pkt->rpc_port);
    mesh_wr_u32(out + 22, pkt->service_mask);
    return NB_MESH_OK;
}

int nb_mesh_decode(const uint8_t *buf, size_t len, nb_mesh_packet_t *out)
{
    if (!buf || !out || len != NB_MESH_WIRE_SIZE) {
        return NB_MESH_ERR_INVALID;
    }
    if (mesh_rd_u16(buf) != NB_MESH_MAGIC) {
        return NB_MESH_ERR_INVALID;
    }
    if (buf[2] != NB_MESH_MSG_JOIN && buf[2] != NB_MESH_MSG_HEARTBEAT &&
        buf[2] != NB_MESH_MSG_LEAVE) {
        return NB_MESH_ERR_INVALID;
    }
    out->msg_type = buf[2];
    out->reserved = buf[3];
    out->formation_id = mesh_rd_u32(buf + 4);
    out->node_id = mesh_rd_u64(buf + 8);
    out->real_ip = mesh_rd_u32(buf + 16);
    out->rpc_port = mesh_rd_u16(buf + 20);
    out->service_mask = mesh_rd_u32(buf + 22);
    return NB_MESH_OK;
}

int nb_mesh_packet_accept(const nb_mesh_packet_t *pkt, uint32_t formation_id, uint64_t node_id)
{
    if (!pkt) {
        return 0;
    }
    if (pkt->formation_id != formation_id) {
        return 0; /* 编队隔离 */
    }
    if (pkt->node_id == node_id || pkt->node_id == 0) {
        return 0; /* 忽略自身（含组播回环） */
    }
    return 1;
}

/* ---- 服务名 -> 位 ---------------------------------------------------- */

uint32_t nb_mesh_service_bit(const char *name)
{
    uint32_t h = 2166136261u; /* FNV-1a 32 */

    if (!name) {
        name = "";
    }
    while (*name) {
        h ^= (uint32_t)(uint8_t)*name++;
        h *= 16777619u;
    }
    /* 折叠高低位：短名字的高位参与少，直接取模会挤在少数 bit 上。 */
    h ^= h >> 16;
    return 1u << (h % NB_MESH_SERVICE_BITS);
}

void nb_mesh_default_group(uint32_t formation_id, char *out, size_t cap)
{
    /* 落在 239.255.0.0/16 的管理范围内，离线部署也能直接用。 */
    uint32_t h = formation_id * 2654435761u + 0x9e3779b9u;
    unsigned a = 1u + (unsigned)((h & 0xffu) % 254u);
    unsigned b = 1u + (unsigned)(((h >> 8) & 0xffu) % 254u);

    if (!out || cap == 0) {
        return;
    }
    (void)snprintf(out, cap, "239.255.%u.%u", a, b);
}

/* ---- 配置 ------------------------------------------------------------ */

void nb_mesh_config_init(nb_mesh_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->port = (uint16_t)NB_MESH_DEFAULT_PORT;
    config->heartbeat_ms = NB_MESH_DEFAULT_HEARTBEAT_MS;
    config->lease_ms = NB_MESH_DEFAULT_LEASE_MS;
    config->max_peers = NB_MESH_DEFAULT_MAX_PEERS;
    /* multicast_group 留 NULL：由 formation_id 推导组播组。 */
}

const char *nb_mesh_strerror(int error)
{
    switch (error) {
    case NB_MESH_OK:
        return "ok";
    case NB_MESH_ERR_INVALID:
        return "invalid argument";
    case NB_MESH_ERR_NOMEM:
        return "out of memory";
    case NB_MESH_ERR_IO:
        return "socket io error";
    case NB_MESH_ERR_STATE:
        return "invalid state";
    case NB_MESH_ERR_NOTFOUND:
        return "not found";
    default:
        return "unknown error";
    }
}

/* ---- 随机数与发送 ---------------------------------------------------- */

/* xorshift32：替代 rand()，不依赖进程级全局状态，多个 mesh 实例互不干扰。 */
static uint32_t mesh_rand(nb_mesh_t *m)
{
    uint32_t x = m->rng ? m->rng : 0x9e3779b9u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x ? x : 1u;
    return m->rng;
}

/*
 * 追加一个定向目标。调用方持 send_lock。
 * 返回 1 = 新增，0 = 已存在，<0 = 错误码。
 */
static int mesh_add_target_locked(nb_mesh_t *m, const char *ip, uint16_t port)
{
    size_t i;

    if (!ip || !ip[0] || port == 0) {
        return NB_MESH_ERR_INVALID;
    }
    for (i = 0; i < m->target_count; i++) {
        if (m->targets[i].port == port && strcmp(m->targets[i].ip, ip) == 0) {
            return 0;
        }
    }
    if (m->target_count >= NB_MESH_MAX_TARGETS) {
        return NB_MESH_ERR_NOMEM;
    }
    mesh_copy_str(m->targets[m->target_count].ip,
                  sizeof(m->targets[m->target_count].ip),
                  ip);
    m->targets[m->target_count].port = port;
    m->target_count++;
    return 1;
}

/* 发一帧：组播（若已加入）+ 全部定向目标。 */
static void mesh_send_frame(nb_mesh_t *m, uint8_t msg_type)
{
    nb_mesh_packet_t pkt;
    uint8_t buf[NB_MESH_WIRE_SIZE];
    size_t i;

    if (!m || !m->sock || !m->send_lock) {
        return;
    }
    /* service_mask 可被 nb_mesh_set_service_mask 在运行期更新，故整帧在锁内组装。 */
    zb_mutex_lock(m->send_lock);
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = msg_type;
    pkt.formation_id = m->formation_id;
    pkt.node_id = m->node_id;
    pkt.real_ip = m->ipv4;
    pkt.rpc_port = m->rpc_port;
    pkt.service_mask = m->service_mask;
    if (nb_mesh_encode(&pkt, buf) != NB_MESH_OK) {
        zb_mutex_unlock(m->send_lock);
        return;
    }
    if (m->multicast && m->group[0]) {
        (void)zn_socket_sendto(m->sock, buf, sizeof(buf), m->group, m->port);
    }
    for (i = 0; i < m->target_count; i++) {
        (void)zn_socket_sendto(m->sock,
                               buf,
                               sizeof(buf),
                               m->targets[i].ip,
                               m->targets[i].port);
    }
    zb_mutex_unlock(m->send_lock);
}

/* ---- 三个执行体 ------------------------------------------------------ */

/* send：返回下次间隔（ms），0 结束。Fast-Join 用固定 50 ms，之后是心跳 + 抖动。 */
static uint64_t mesh_send_cb(void *user)
{
    nb_mesh_t *m = (nb_mesh_t *)user;
    uint8_t type;

    if (!m || m->stop_flag) {
        return 0;
    }
    if (m->fast_join_left > 0) {
        m->fast_join_left--;
        type = NB_MESH_MSG_JOIN;
    } else {
        type = NB_MESH_MSG_HEARTBEAT;
    }
    mesh_send_frame(m, type);
    if (m->fast_join_left > 0) {
        return NB_MESH_FAST_JOIN_INTERVAL_MS;
    }
    {
        uint64_t delay = (uint64_t)m->heartbeat_ms;

        if (m->jitter_ms) {
            delay += (uint64_t)(mesh_rand(m) % (m->jitter_ms + 1u));
        }
        return delay;
    }
}

/* sweep：每 200 ms 剔除超过租约没心跳的邻居。 */
static void mesh_sweep_cb(void *user)
{
    nb_mesh_t *m = (nb_mesh_t *)user;
    size_t removed;

    if (!m || m->stop_flag) {
        return;
    }
    zb_mutex_lock(m->lock);
    removed = nb_mesh_table_sweep(&m->table, zb_monotonic_ms(), m->lease_ms);
    zb_mutex_unlock(m->lock);
    if (removed) {
        zn_debug("mesh: lease expired, %lu peer(s) removed", (unsigned long)removed);
    }
}

/* 处理一条通过校验的报文。 */
static void mesh_on_peer_packet(nb_mesh_t *m, const nb_mesh_packet_t *pkt, const char *src_ip)
{
    uint64_t now = zb_monotonic_ms();
    uint64_t evicted = 0;
    uint32_t ipv4;
    size_t capacity;
    int inserted;

    if (pkt->msg_type == NB_MESH_MSG_LEAVE) {
        int removed;

        /* LEAVE 立即删除，不等租约。 */
        zb_mutex_lock(m->lock);
        removed = nb_mesh_table_remove(&m->table, pkt->node_id);
        zb_mutex_unlock(m->lock);
        if (removed) {
            zn_debug("mesh: node %llx left", (unsigned long long)pkt->node_id);
        }
        return;
    }
    /* real_ip 为 0 表示发送方不显式通告：用数据报源地址。 */
    ipv4 = pkt->real_ip ? pkt->real_ip : mesh_ipv4_parse(src_ip);
    if (ipv4 == 0) {
        return;
    }
    zb_mutex_lock(m->lock);
    capacity = m->table.capacity;
    inserted = nb_mesh_table_upsert(
        &m->table, pkt->node_id, ipv4, pkt->rpc_port, pkt->service_mask, now, &evicted);
    zb_mutex_unlock(m->lock);
    if (evicted) {
        /* 表满按"最久未见"淘汰：必须告警，不能静默丢新节点。 */
        zn_warn("mesh: peer table full (%u), evicted stalest node %llx",
                (unsigned)capacity,
                (unsigned long long)evicted);
    }
    if (inserted > 0) {
        zn_debug("mesh: node %llx joined (rpc_port=%u mask=0x%08x)",
                 (unsigned long long)pkt->node_id,
                 (unsigned)pkt->rpc_port,
                 (unsigned)pkt->service_mask);
    }
}

/* recv：poller 可读事件回调，把 socket 缓冲抽干（有上限）。 */
static void mesh_on_readable(zn_socket *sock, void *user)
{
    nb_mesh_t *m = (nb_mesh_t *)user;
    uint8_t buf[NB_MESH_RECV_BUF];
    int round;

    for (round = 0; round < NB_MESH_RECV_BURST; round++) {
        char src_ip[NB_MESH_STR_MAX];
        uint16_t src_port = 0;
        zn_ssize_t n;
        nb_mesh_packet_t pkt;

        src_ip[0] = '\0';
        n = zn_socket_recvfrom(sock, buf, sizeof(buf), src_ip, sizeof(src_ip), &src_port);
        if (n <= 0) {
            break;
        }
        if (nb_mesh_decode(buf, (size_t)n, &pkt) != NB_MESH_OK) {
            continue;
        }
        if (!nb_mesh_packet_accept(&pkt, m->formation_id, m->node_id)) {
            continue;
        }
        mesh_on_peer_packet(m, &pkt, src_ip);
    }
}

static void mesh_thread_main(void *user)
{
    nb_mesh_t *m = (nb_mesh_t *)user;

    zn_poller_run(m->poller, &m->stop_flag);
}

/* ---- 生命周期 -------------------------------------------------------- */

int nb_mesh_start(nb_mesh_t **out, const nb_mesh_config_t *cfg)
{
    nb_mesh_t *m;
    zn_socket_callbacks_t cbs;
    char group[NB_MESH_STR_MAX];
    uint16_t bound = 0;
    size_t i;
    int rc;

    if (!out || !cfg) {
        return NB_MESH_ERR_INVALID;
    }
    if (cfg->peer_count > 0 && !cfg->peers) {
        return NB_MESH_ERR_INVALID;
    }
    *out = NULL;
    zn_platform_init();

    m = (nb_mesh_t *)calloc(1, sizeof(*m));
    if (!m) {
        return NB_MESH_ERR_NOMEM;
    }
    m->formation_id = cfg->formation_id;
    m->node_id = cfg->node_id;
    m->ipv4 = mesh_ipv4_parse(cfg->advertise_ip);
    if (cfg->advertise_ip && cfg->advertise_ip[0] && m->ipv4 == 0) {
        rc = NB_MESH_ERR_INVALID;
        goto fail;
    }
    m->rpc_port = cfg->rpc_port;
    m->service_mask = cfg->service_mask;
    m->heartbeat_ms =
        cfg->heartbeat_ms ? cfg->heartbeat_ms : (uint32_t)NB_MESH_DEFAULT_HEARTBEAT_MS;
    m->lease_ms = cfg->lease_ms ? cfg->lease_ms : (uint32_t)NB_MESH_DEFAULT_LEASE_MS;
    m->jitter_ms = cfg->jitter_ms;
    if (m->jitter_ms == 0) {
        /* 默认抖动 = 心跳的 2/5，上限 400 ms：1000 ms 心跳 -> 0..400 ms。
         * 抖动把 5000 个节点的发包时刻打散，避免同相位碰撞。 */
        m->jitter_ms = m->heartbeat_ms * 2u / 5u;
        if (m->jitter_ms > NB_MESH_DEFAULT_JITTER_MS) {
            m->jitter_ms = NB_MESH_DEFAULT_JITTER_MS;
        }
    }
    if (m->lease_ms <= m->heartbeat_ms) {
        /* 租约必须大于心跳周期，否则每个周期都会误剔除。 */
        m->lease_ms = m->heartbeat_ms + m->heartbeat_ms / 2u;
    }
    mesh_copy_str(m->bind_host, sizeof(m->bind_host), cfg->bind_host ? cfg->bind_host : "");
    /* port = 0 表示由系统分配临时端口（同机多实例/单元测试用）。 */
    m->port = cfg->port;
    m->rng = (uint32_t)zb_monotonic_ms() ^ (uint32_t)(uintptr_t)m;
    if (m->rng == 0) {
        m->rng = 1u;
    }
    if (m->node_id == 0) {
        uint64_t hi = mesh_rand(m);
        uint64_t lo = mesh_rand(m);

        m->node_id = (hi << 32) | lo;
        if (m->node_id == 0) {
            m->node_id = 1;
        }
    }
    m->lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    m->send_lock = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!m->lock || !m->send_lock) {
        rc = NB_MESH_ERR_NOMEM;
        goto fail;
    }
    rc = nb_mesh_table_init(&m->table, cfg->max_peers);
    if (rc != NB_MESH_OK) {
        goto fail;
    }

    /* 组播组：NULL = 由 formation 推导；"" = 不加组播；其他 = 显式给出。 */
    if (cfg->multicast_group == NULL) {
        nb_mesh_default_group(m->formation_id, group, sizeof(group));
        mesh_copy_str(m->group, sizeof(m->group), group);
    } else {
        mesh_copy_str(m->group, sizeof(m->group), cfg->multicast_group);
    }

    m->poller = zn_poller_create();
    m->sock = zn_socket_create();
    if (!m->poller || !m->sock) {
        rc = NB_MESH_ERR_NOMEM;
        goto fail;
    }
#if defined(_WIN32)
    {
        /* Windows 没有 SO_REUSEPORT：同端口多实例由定向单播目标区分。 */
        const int reuse_port = 0;
#else
    {
        const int reuse_port = 1;
#endif
        if (zn_socket_bind_udp_ex(m->sock,
                                  m->bind_host[0] ? m->bind_host : "0.0.0.0",
                                  m->port,
                                  1,
                                  reuse_port) != ZN_OK) {
            rc = NB_MESH_ERR_IO;
            goto fail;
        }
    }
    if (m->group[0]) {
        if (zn_socket_join_multicast(m->sock,
                                     m->group,
                                     m->bind_host[0] ? m->bind_host : NULL) == ZN_OK) {
            m->multicast = 1;
            (void)zn_socket_set_multicast_ttl(m->sock, 1);
            /* 同机多进程/多实例也要能看到彼此。 */
            (void)zn_socket_set_multicast_loop(m->sock, 1);
        } else {
            /* 组地址不可 join（例如本机回环地址）：退化为定向单播目标。 */
            zb_mutex_lock(m->send_lock);
            (void)mesh_add_target_locked(m, m->group, m->port);
            zb_mutex_unlock(m->send_lock);
        }
    }
    if (zn_socket_get_local(m->sock, NULL, 0, &bound) == ZN_OK && bound != 0) {
        m->port = bound;
    }
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_readable = mesh_on_readable;
    if (zn_socket_attach_poller(m->sock, m->poller, &cbs, m) != ZN_OK) {
        rc = NB_MESH_ERR_IO;
        goto fail;
    }

    zb_mutex_lock(m->send_lock);
    for (i = 0; i < cfg->peer_count; i++) {
        (void)mesh_add_target_locked(m, cfg->peers[i].ip, cfg->peers[i].port);
    }
    zb_mutex_unlock(m->send_lock);

    m->thread = zb_thread_create(mesh_thread_main, m);
    if (!m->thread) {
        rc = NB_MESH_ERR_NOMEM;
        goto fail;
    }
    /* Fast-Join：立即发第一帧，此后每 50 ms 一帧，共 NB_MESH_FAST_JOIN_COUNT 帧。 */
    m->fast_join_left = NB_MESH_FAST_JOIN_COUNT;
    m->send_timer = zn_poller_do_delay(m->poller, 0, mesh_send_cb, m);
    m->sweep_timer =
        zn_timer_start(m->poller, NB_MESH_SWEEP_INTERVAL_MS, 1, mesh_sweep_cb, m);
    if (!m->send_timer || !m->sweep_timer) {
        rc = NB_MESH_ERR_NOMEM;
        goto fail;
    }
    m->running = 1;
    *out = m;
    return NB_MESH_OK;

fail:
    nb_mesh_stop(m);
    return rc;
}

void nb_mesh_stop(nb_mesh_t *m)
{
    if (!m) {
        return;
    }
    /* 先公告离开：对端无需等租约就能立刻摘掉本节点。 */
    if (m->running) {
        mesh_send_frame(m, NB_MESH_MSG_LEAVE);
        m->running = 0;
    }
    m->stop_flag = 1;
    if (m->poller) {
        (void)zn_poller_wake(m->poller);
    }
    if (m->thread) {
        (void)zb_thread_join(m->thread);
        zb_thread_destroy(m->thread);
        m->thread = NULL;
    }
    /* 线程已停：定时器不会再触发，逐个取消并释放包装对象。 */
    if (m->send_timer) {
        zn_poller_timer_cancel(m->send_timer);
        m->send_timer = NULL;
    }
    if (m->sweep_timer) {
        zn_timer_stop(m->sweep_timer);
        m->sweep_timer = NULL;
    }
    if (m->sock) {
        if (m->multicast && m->group[0]) {
            (void)zn_socket_leave_multicast(m->sock,
                                            m->group,
                                            m->bind_host[0] ? m->bind_host : NULL);
        }
        (void)zn_socket_detach_poller(m->sock);
        zn_socket_destroy(m->sock);
        m->sock = NULL;
    }
    if (m->poller) {
        zn_poller_destroy(m->poller);
        m->poller = NULL;
    }
    nb_mesh_table_fini(&m->table);
    if (m->send_lock) {
        zb_mutex_destroy(m->send_lock);
        m->send_lock = NULL;
    }
    if (m->lock) {
        zb_mutex_destroy(m->lock);
        m->lock = NULL;
    }
    free(m);
}

/* ---- 寻址与公告 ------------------------------------------------------ */

int nb_mesh_find(nb_mesh_t *m, uint32_t service_bit, nb_mesh_target_t *out)
{
    nb_mesh_node_t *node;

    if (!m || !out || service_bit == 0) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->lock);
    node = nb_mesh_table_find_service(&m->table, service_bit);
    if (!node) {
        zb_mutex_unlock(m->lock);
        return NB_MESH_ERR_NOTFOUND;
    }
    out->node_id = node->node_id;
    out->ipv4 = node->ipv4;
    out->rpc_port = node->rpc_port;
    out->service_mask = node->service_mask;
    mesh_ipv4_format(node->ipv4, out->host, sizeof(out->host));
    zb_mutex_unlock(m->lock);
    return NB_MESH_OK;
}

int nb_mesh_announce(nb_mesh_t *m)
{
    if (!m || !m->running) {
        return NB_MESH_ERR_STATE;
    }
    mesh_send_frame(m, NB_MESH_MSG_JOIN);
    return NB_MESH_OK;
}

int nb_mesh_leave(nb_mesh_t *m)
{
    if (!m || !m->running) {
        return NB_MESH_ERR_STATE;
    }
    mesh_send_frame(m, NB_MESH_MSG_LEAVE);
    return NB_MESH_OK;
}

int nb_mesh_set_service_mask(nb_mesh_t *m, uint32_t service_mask)
{
    if (!m || !m->send_lock) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->send_lock);
    m->service_mask = service_mask;
    zb_mutex_unlock(m->send_lock);
    return NB_MESH_OK;
}

int nb_mesh_add_peer(nb_mesh_t *m, const char *ip, uint16_t port)
{
    int rc;

    if (!m || !ip || !ip[0] || port == 0) {
        return NB_MESH_ERR_INVALID;
    }
    zb_mutex_lock(m->send_lock);
    rc = mesh_add_target_locked(m, ip, port);
    zb_mutex_unlock(m->send_lock);
    if (rc < 0) {
        return rc;
    }
    /* 新目标可能还没见过本机：立刻补一次 JOIN，别等下一个心跳周期。
     * 已存在的目标不重复发，避免 announce 时的放大。 */
    if (rc > 0 && m->running) {
        mesh_send_frame(m, NB_MESH_MSG_JOIN);
    }
    return NB_MESH_OK;
}

/* ---- 内省 ------------------------------------------------------------ */

size_t nb_mesh_peers(nb_mesh_t *m, nb_mesh_peer_info_t *out, size_t max)
{
    size_t written;

    if (!m || !out || max == 0) {
        return 0;
    }
    zb_mutex_lock(m->lock);
    written = nb_mesh_table_snapshot(&m->table, out, max);
    zb_mutex_unlock(m->lock);
    return written;
}

size_t nb_mesh_peer_count(nb_mesh_t *m)
{
    size_t count;

    if (!m) {
        return 0;
    }
    zb_mutex_lock(m->lock);
    count = nb_mesh_table_count(&m->table);
    zb_mutex_unlock(m->lock);
    return count;
}

uint16_t nb_mesh_port(nb_mesh_t *m)
{
    return m ? m->port : 0;
}

uint64_t nb_mesh_node_id(nb_mesh_t *m)
{
    return m ? m->node_id : 0;
}
