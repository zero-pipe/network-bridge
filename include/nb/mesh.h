#ifndef NB_MESH_H
#define NB_MESH_H

/*
 * nb_mesh - 轻量级 UDP 组播成员发现（RPC 控制面）。
 *
 * 设计目标：节点数增长时单节点流量恒定、默认为零握手。
 *
 *   1. 定长 40 字节心跳，组播发出，默认 500 ms 一次。报文自带定位与摘要：
 *      编队 ID、节点 ID、RPC 端口、**显式 IPv4**、**64 位服务掩码**、
 *      **接口契约哈希**。调用方不需要事先知道对端 IP。
 *   2. 编队隔离：formation_id 不同直接丢弃；默认组播组也由 formation_id
 *      推导，物理上把不同编队分开。
 *   3. service_hash 是**这个节点自己服务列表的版本号**，不是“和我的清单比
 *      是否相同”。对端拿到它之后，只用来判断“我缓存的那份列表过期了吗”：
 *        - 与我缓存的 hash 相同  → 缓存有效，直接用，零流量
 *        - 变了或从没拉过        → 需要时点对点拉一次，然后按这个 hash 缓存
 *      这样 node_a(service_a) 与 node_b(service_b) 之间没有“哈希必须相等”
 *      的要求，异构服务集不会互相触发拉取，避免了 O(N^2) 握手。
 *   4. service_mask（64 位）是可选的快速路径：bit = 服务 ID（或服务名哈希
 *      低位）。命中即可直接路由，连拉取都省了；为 0 表示“没通告”，对端
 *      按需内省。共享 nb_service_table 的部署建议用服务 ID 填位。
 *   5. 按需内省（lazy introspection）：只有真正要调用某个对端上“缓存里没有
 *      或版本已过期”的服务时，才单播拉一次 manifest。
 *
 * 线程模型：与 zn/znet 一致。create/destroy/announce/leave/tick 可在任意线程
 * 调用（内部加锁）；回调在 poller 线程、库锁之内执行，必须非阻塞，且不要在
 * 回调里 destroy。
 *
 * 典型用法：
 *
 *   nb_mesh_config_t cfg;
 *   nb_mesh_config_init(&cfg);
 *   cfg.formation_id = 7;
 *   cfg.rpc_port = 50000;
 *   cfg.service_hash = 0;                    // 0 = 按本地服务清单自动计算
 *   cfg.service_mask = (1ull << 1) | (1ull << 3);   // 可选：本机服务位图
 *   cfg.advertise_host = "192.168.0.1";                        // 多网卡时显式指定
 *   cfg.callbacks.on_peer = on_peer;
 *   nb_mesh_create(poller, &cfg, &mesh);
 *   zn_timer_start(poller, 500, 1, on_tick, mesh);   // tick + announce
 */

#include <zn/zn.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_MESH_VERSION 2u
/* 线上定长心跳包大小（字节）。 */
#define NB_MESH_PACKET_SIZE 40u
#define NB_MESH_DEFAULT_PORT 41953u
#define NB_MESH_DEFAULT_HEARTBEAT_MS 500u
#define NB_MESH_DEFAULT_LEASE_MS 3000u
#define NB_MESH_MAX_PEERS 256u
#define NB_MESH_NAME_MAX 64u
#define NB_MESH_HOST_MAX 64u
/* 单个 manifest 最多携带的服务名个数。 */
#define NB_MESH_MAX_SERVICES 128u
/* 服务掩码位宽。服务 ID（nb_service_table）超出这个范围时，对端只能靠
 * manifest 发现该服务。 */
#define NB_MESH_SERVICE_BITS 64u

/* 返回码。 */
enum {
    NB_MESH_OK = 0,
    NB_MESH_ERR_INVALID = -1,
    NB_MESH_ERR_NOMEM = -2,
    NB_MESH_ERR_IO = -3,
    NB_MESH_ERR_TOOBIG = -4,
    NB_MESH_ERR_STATE = -5,
    NB_MESH_ERR_OVERLOAD = -6,
    NB_MESH_ERR_NOTFOUND = -7
};

const char *nb_mesh_strerror(int error);

/* 报文类型。JOIN/HEARTBEAT/LEAVE 是裸定长包；MANIFEST_* 带负载。 */
typedef enum nb_mesh_msg_type {
    NB_MESH_MSG_JOIN = 1,
    NB_MESH_MSG_HEARTBEAT = 2,
    NB_MESH_MSG_LEAVE = 3,
    NB_MESH_MSG_MANIFEST_REQ = 4,
    NB_MESH_MSG_MANIFEST_RSP = 5
} nb_mesh_msg_type_t;

typedef struct nb_mesh nb_mesh_t;

/* 对端 manifest 状态（nb_mesh_peer_info_t.manifest_state）。 */
typedef enum nb_mesh_manifest_state {
    NB_MESH_MANIFEST_NONE = 0,
    NB_MESH_MANIFEST_REQUESTED = 1,
    NB_MESH_MANIFEST_READY = 2
} nb_mesh_manifest_state_t;

/* 对端快照。回调返回后仍然有效（值拷贝）。 */
typedef struct nb_mesh_peer_info {
    uint64_t node_id;
    /* 可路由地址：报文里的显式 IPv4；发送方没填时用数据报源地址。 */
    char host[NB_MESH_HOST_MAX];
    uint32_t ipv4;               /* 网络字节序；0 = 只知道源地址 */
    uint16_t port;               /* 它的发现端口 */
    uint32_t rpc_port;           /* 它的 RPC 服务端口 */
    uint64_t service_mask;       /* 它提供的服务位图；0 = 未通告，需内省 */
    /* 它对端服务列表的版本号（hash）。等于 cached_service_hash 时缓存有效。 */
    uint32_t service_hash;
    /* 我本地缓存的那份列表对应的 hash；0 = 还没缓存。 */
    uint32_t cached_service_hash;
    /* nb_mesh_manifest_state_t */
    int manifest_state;
    size_t service_count; /* manifest_state == READY 时有效 */
    uint64_t last_seen_ms;
} nb_mesh_peer_info_t;

typedef struct nb_mesh_callbacks {
    /* 新对端出现、契约哈希变化时 removed=0；LEAVE 或租约过期时 removed=1。 */
    void (*on_peer)(void *user, const nb_mesh_peer_info_t *peer, int removed);
    /* 按需内省结果：服务名列表（指针数组，回调返回后失效）。 */
    void (*on_manifest)(void *user,
                        const nb_mesh_peer_info_t *peer,
                        const char *const *services,
                        size_t count);
} nb_mesh_callbacks_t;

typedef struct nb_mesh_config {
    uint32_t struct_size;
    /* NULL 或 "0.0.0.0" 绑定所有接口。 */
    const char *bind_host;
    /* 0 = NB_MESH_DEFAULT_PORT。 */
    uint16_t port;
    /* 组播组；NULL = 由 formation_id 推导。"" = 不加入组播，只用单播目标。 */
    const char *multicast_group;
    /* 编队/任务组 ID：不同编队互不可见。 */
    uint32_t formation_id;
    uint64_t node_id;   /* 全局唯一；0 = 由进程随机生成 */
    uint32_t rpc_port;  /* RPC 服务监听端口 */
    /* 在报文里显式通告的 IPv4（"192.168.0.1"）；NULL = 由数据报源地址推断。
     * 多网卡 / NAT 场景必须显式给，否则对端可能拿到不可路由的地址。 */
    const char *advertise_host;
    /* 本机提供的服务位图（bit = 服务 ID 或服务名哈希低位）；0 = 不通告，
     * 对端一律走按需内省。 */
    uint64_t service_mask;
    /* 本机服务列表的版本号；0 = 由本地服务清单计算（推荐）。 */
    uint32_t service_hash;
    uint32_t heartbeat_ms; /* 0 = 500 */
    uint32_t lease_ms;     /* 0 = 3000（6 个心跳） */
    uint32_t jitter_pct;   /* 0 = 20 */
    uint32_t max_peers;    /* 0 = 256 */
    nb_mesh_callbacks_t callbacks;
    void *user;
} nb_mesh_config_t;

void nb_mesh_config_init(nb_mesh_config_t *config);

/*
 * 服务列表版本哈希：对服务名集合做顺序无关的 FNV-1a。
 * 它只是“这份列表的版本号”，不需要在节点之间相等。
 */
uint32_t nb_mesh_service_hash(const char *const *names, size_t count);

/* 服务位图里某服务对应的 bit（服务名哈希低位），供不共享服务表的部署使用。 */
uint64_t nb_mesh_service_bit(const char *name);

/* 绑定 socket、加入组播组。失败返回 NULL，原因写入 error_out。 */
nb_mesh_t *nb_mesh_create(zn_poller *poller,
                          const nb_mesh_config_t *config,
                          int *error_out);
void nb_mesh_destroy(nb_mesh_t *mesh);

/* 本地服务清单（用于内省应答，并刷新 service_hash，除非配置里显式给了）。 */
int nb_mesh_set_services(nb_mesh_t *mesh, const char *const *names, size_t count);
/* 更新本机服务掩码（运行期服务增删后调用）。 */
int nb_mesh_set_service_mask(nb_mesh_t *mesh, uint64_t mask);

/* 立即发送 JOIN（首次 announce 后自动转为 HEARTBEAT）。 */
int nb_mesh_announce(nb_mesh_t *mesh);
/* 发送 LEAVE，通知同编队节点自己离开。 */
int nb_mesh_leave(nb_mesh_t *mesh);

/* 周期维护：租约过期、按需内省重试、分片清理。由定时器调用。 */
void nb_mesh_tick(nb_mesh_t *mesh, uint64_t now_ms);
/* 下次心跳间隔（带抖动），用于调度定时器。 */
uint32_t nb_mesh_next_delay_ms(nb_mesh_t *mesh);

/* 追加一个单播目标（组播不可用时的兜底，或跨网段定向发现）。 */
int nb_mesh_add_peer(nb_mesh_t *mesh, const char *host, uint16_t port);

/*
 * 立刻向某个对端拉取 manifest（跳过退避）。RPC 在解析服务失败时调用它，
 * 让“首次调用未知服务”尽快拿到映射。返回 NB_MESH_OK 表示已发出或已就绪。
 */
int nb_mesh_request_manifest(nb_mesh_t *mesh, uint64_t node_id);

uint16_t nb_mesh_port(const nb_mesh_t *mesh);
size_t nb_mesh_peer_count(const nb_mesh_t *mesh);
uint64_t nb_mesh_node_id(const nb_mesh_t *mesh);
uint32_t nb_mesh_service_hash_value(const nb_mesh_t *mesh);

/* 对端快照；返回写入条数。 */
size_t nb_mesh_peers(nb_mesh_t *mesh, nb_mesh_peer_info_t *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* NB_MESH_H */
