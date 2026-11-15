#ifndef NB_MESH_H
#define NB_MESH_H

/*
 * nb_mesh - 面向大规模编队的 UDP 组播成员发现（RPC 控制面）。
 *
 * 设计目标：节点数增长时**单节点出向流量恒定**，成员变更零握手，调用路径
 * 一次哈希即可拿到 ip:port。
 *
 *   1. 定长 26 字节二进制报文（严禁文本协议），所有多字节字段统一大端：
 *        magic(2) | msg_type(1) | reserved(1) | formation_id(4) |
 *        node_id(8) | real_ip(4) | rpc_port(2) | service_mask(4)
 *      报文自带定位信息（显式 IPv4 + RPC 端口 + 服务位图），调用方无需预知
 *      对端地址，也不需要任何"拉服务列表"的二次握手。
 *   2. 编队隔离：formation_id 与本机不同的报文直接丢弃；组播组由 formation_id
 *      推导，物理上再把不同编队分开。发送方自己的报文按 node_id 忽略。
 *   3. 邻居表：8192 个哈希桶（2 的幂，位掩码取模）+ 链式冲突，键 = node_id，
 *      Murmur 风格 64 位收尾混淆。另按服务位建 32 条双向索引链（轮转游标），
 *      让掩码寻址是 O(1) 而不是"扫 8192 桶"。表满时淘汰"最久未见"并告警，
 *      不静默丢弃新节点。
 *   4. 三个执行体（Fast-Join + 抖动 + 租约）：
 *        send   上电连发 3 次 JOIN（间隔 50 ms），此后每
 *               heartbeat_ms + rand()%jitter 发 HEARTBEAT（抖动是防大规模碰撞
 *               的关键：5000 节点若同相位，交换机与接收中断会在同一时刻爆掉）
 *        recv   组播 socket 收包 -> 校验 -> upsert / remove
 *        sweep  每 200 ms 扫一遍，lease_ms 无心跳即剔除；LEAVE 立即移除
 *
 * 线程模型：**poller 事件 + 两个定时器，共 1 条内部线程**（见 docs/mesh-discovery.md）。
 *   - recv 走 zn_socket_attach_poller 的可读事件；
 *   - send / sweep 走 zn_poller_do_delay / zn_timer；
 *   - 只读的公开 API（find/peers）可由任意线程调用，内部用一把互斥量保护，
 *     临界区只有"一次链表操作"，不持锁做 IO。
 * 这样既保住了"行为等价于三条独立线程"，又避免为阻塞式 recvfrom 引入退出
 * 困难（znet 的 socket 没有暴露 SO_RCVTIMEO），也不会给每个进程多加 2 条线程。
 *
 * 典型用法：
 *
 *   nb_mesh_config_t cfg;
 *   nb_mesh_config_init(&cfg);
 *   cfg.formation_id = 7;
 *   cfg.rpc_port = 50000;
 *   cfg.service_mask = NB_SERVICE_CORE_FCS | NB_SERVICE_SENSE_SLAM;
 *   nb_mesh_start(&mesh, &cfg);                     // 立刻开始 Fast-Join
 *   nb_mesh_find(mesh, NB_SERVICE_SENSE_SLAM, &t);  // O(1) 掩码寻址
 *   nb_mesh_stop(mesh);                             // 发 LEAVE 后收摊
 */

#include <nb/mesh_services.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 线上定长报文大小（字节）。 */
#define NB_MESH_WIRE_SIZE 26u
/* 报文 magic（大端 u16）：'Z' 'C'。 */
#define NB_MESH_MAGIC 0x5A43u
/* 邻居表哈希桶数：必须是 2 的幂（用位掩码取模）。 */
#define NB_MESH_BUCKETS 8192u
/* 服务位宽，与 nb/mesh_services.h 的位图宽度一致。 */
#define NB_MESH_SERVICE_BITS NB_SERVICE_BITS
/* 默认发现端口。 */
#define NB_MESH_DEFAULT_PORT 41953u
/* 默认心跳周期与租约。租约 = 1.5 x 心跳，允许偶尔丢一个包。 */
#define NB_MESH_DEFAULT_HEARTBEAT_MS 1000u
#define NB_MESH_DEFAULT_LEASE_MS 1500u
/* 默认邻居表容量。 */
#define NB_MESH_DEFAULT_MAX_PEERS 8192u
/* 默认心跳抖动上限（rand()%jitter）。 */
#define NB_MESH_DEFAULT_JITTER_MS 400u
/* Fast-Join：上电连发次数与间隔。 */
#define NB_MESH_FAST_JOIN_COUNT 3u
#define NB_MESH_FAST_JOIN_INTERVAL_MS 50u
/* 清扫周期。 */
#define NB_MESH_SWEEP_INTERVAL_MS 200u
/* 定向单播目标上限（组播不可用时的兜底）。 */
#define NB_MESH_MAX_TARGETS 32u
/* 字符串字段（地址/URI）长度上限。 */
#define NB_MESH_ADDR_MAX 64u
/* 点分十进制 IPv4 字符串最大长度（"255.255.255.255\0"）。 */
#define NB_MESH_IPV4_STR_MAX 16u

/* 返回码（与 nb/base/error.h 的通用码保持同一套数值语义）。 */
enum {
    NB_MESH_OK = 0,
    NB_MESH_ERR_INVALID = -1,
    NB_MESH_ERR_NOMEM = -2,
    NB_MESH_ERR_IO = -3,
    NB_MESH_ERR_STATE = -5,
    NB_MESH_ERR_NOTFOUND = -7
};

const char *nb_mesh_strerror(int error);

/* 报文类型。定长报文的全部取值；未知类型一律丢弃。 */
typedef enum nb_mesh_msg_type {
    NB_MESH_MSG_JOIN = 1,
    NB_MESH_MSG_HEARTBEAT = 2,
    NB_MESH_MSG_LEAVE = 3
} nb_mesh_msg_type_t;

typedef struct nb_mesh nb_mesh_t;

/* ---- 线上报文（26 字节，全大端） ------------------------------------- */

/*
 * 报文的内存表示（字段已转成主机序）。real_ip 是 IPv4 主机序：
 *   0 表示"发送方不显式通告地址"，接收方改用数据报的源地址。
 */
typedef struct nb_mesh_packet {
    uint8_t msg_type;        /* nb_mesh_msg_type_t */
    uint8_t reserved;        /* 发送置 0，接收忽略（留给将来的扩展位） */
    uint32_t formation_id;   /* 编队 ID：不同编队互不可见 */
    uint64_t node_id;        /* 集群内唯一节点标识 */
    uint32_t real_ip;        /* 通告的 IPv4（主机序）；0 = 用源地址 */
    uint16_t rpc_port;       /* 该节点的 RPC 服务端口 */
    uint32_t service_mask;   /* 服务位图（见 nb/mesh_services.h） */
} nb_mesh_packet_t;

/* 编码到 26 字节线格式。pkt 非法返回 NB_MESH_ERR_INVALID。 */
int nb_mesh_encode(const nb_mesh_packet_t *pkt, uint8_t out[NB_MESH_WIRE_SIZE]);

/*
 * 解码 26 字节线格式。长度不等于 26、magic 不符、msg_type 未知或保留了
 * 非法字段一律返回 NB_MESH_ERR_INVALID（调用方直接丢弃该数据报）。
 */
int nb_mesh_decode(const uint8_t *buf, size_t len, nb_mesh_packet_t *out);

/*
 * 接收侧准入判断：编队隔离（formation 必须一致）+ 忽略自身（node_id 不能是
 * 本机）。返回 1 表示应当处理，0 表示丢弃。
 */
int nb_mesh_packet_accept(const nb_mesh_packet_t *pkt, uint32_t formation_id, uint64_t node_id);

/* ---- 邻居表视图 ------------------------------------------------------ */

/* 掩码寻址命中结果。 */
typedef struct nb_mesh_target {
    uint64_t node_id;
    uint32_t ipv4; /* 主机序 */
    char host[NB_MESH_IPV4_STR_MAX]; /* 点分十进制，可直接交给 RPC 端点 */
    uint16_t rpc_port;
    uint32_t service_mask;
} nb_mesh_target_t;

/* 邻居快照（值拷贝，调用方负责数组）。 */
typedef struct nb_mesh_peer_info {
    uint64_t node_id;
    uint32_t ipv4;   /* 主机序 */
    uint16_t rpc_port;
    uint32_t service_mask;
    uint64_t last_seen_ms; /* 单调时钟（zb_monotonic_ms） */
} nb_mesh_peer_info_t;

/* 定向单播目标（组播不可用或跨网段时使用）。 */
typedef struct nb_mesh_peer_config {
    const char *ip; /* 点分十进制 IPv4 */
    uint16_t port;
} nb_mesh_peer_config_t;

/* ---- 配置 ------------------------------------------------------------ */

typedef struct nb_mesh_config {
    uint32_t formation_id; /* 编队/任务组 ID：不同编队互不可见 */
    uint64_t node_id;      /* 全局唯一；0 = 由时钟随机生成 */
    uint16_t rpc_port;     /* 对外通告的 RPC 服务端口（0 = 不通告） */
    /* 显式通告的 IPv4（"192.168.0.1"）；NULL = 由数据报源地址推断。
     * 多网卡/NAT 场景必须显式给，否则对端可能拿到不可路由的地址。 */
    const char *advertise_ip;
    /* 本机提供的服务位图（见 nb/mesh_services.h）。 */
    uint32_t service_mask;
    /* 组播组：NULL = 由 formation_id 推导；"" = 不加入组播，只用单播目标；
     * 其他值 = 显式组播组（当地址不是组播地址时退化为单播目标）。 */
    const char *multicast_group;
    /* 绑定地址：NULL / "" / "0.0.0.0" = 所有接口。 */
    const char *bind_host;
    uint16_t port; /* 0 = 由系统分配临时端口（同机多实例/单测）；通常用默认 41953 */
    /* 心跳周期与对端租约；0 = 默认（1000 / 1500 ms）。 */
    uint32_t heartbeat_ms;
    uint32_t lease_ms;
    /* 心跳抖动上限（rand()%jitter）；0 = 默认（400 ms，按心跳周期等比缩放）。 */
    uint32_t jitter_ms;
    /* 邻居表容量；0 = 默认 8192。 */
    uint32_t max_peers;
    /* 定向单播目标（可选）。 */
    const nb_mesh_peer_config_t *peers;
    size_t peer_count;
} nb_mesh_config_t;

void nb_mesh_config_init(nb_mesh_config_t *config);

/* ---- 生命周期 -------------------------------------------------------- */

/*
 * 创建并启动发现（绑定端口、加入组播、启动 Fast-Join 与清扫）。
 * 成功返回 NB_MESH_OK 并写出句柄；失败返回错误码且 *out 置 NULL。
 */
int nb_mesh_start(nb_mesh_t **out, const nb_mesh_config_t *cfg);
/* 发送 LEAVE、停止执行体并释放资源。允许传 NULL。 */
void nb_mesh_stop(nb_mesh_t *m);

/* ---- 寻址与公告 ------------------------------------------------------ */

/*
 * 掩码寻址：按服务位返回一个提供该能力的邻居。传单个 bit（如
 * NB_SERVICE_SENSE_SLAM）；传多个 bit 时取最低位。同一 bit 上的多个节点用
 * 轮转游标分摊（等价的最简负载均衡）。命中返回 NB_MESH_OK，否则
 * NB_MESH_ERR_NOTFOUND。
 */
int nb_mesh_find(nb_mesh_t *m, uint32_t service_bit, nb_mesh_target_t *out);

/* 立即发送一次 JOIN（Fast-Join 序列之外的手动公告）。 */
int nb_mesh_announce(nb_mesh_t *m);
/* 立即发送 LEAVE，通知同编队节点自己离开。 */
int nb_mesh_leave(nb_mesh_t *m);

/* 更新本机服务位图（运行期增删服务后调用）；从下一帧心跳起生效。 */
int nb_mesh_set_service_mask(nb_mesh_t *m, uint32_t service_mask);

/* 追加一个定向单播目标，并立刻向全部目标公告一次。 */
int nb_mesh_add_peer(nb_mesh_t *m, const char *ip, uint16_t port);

/* ---- 内省 ------------------------------------------------------------ */

/* 邻居快照；返回写入条数（不超过 max）。 */
size_t nb_mesh_peers(nb_mesh_t *m, nb_mesh_peer_info_t *out, size_t max);
/* 当前邻居数。 */
size_t nb_mesh_peer_count(nb_mesh_t *m);
/* 实际绑定的发现端口（配置 port=0 时由系统分配）。 */
uint16_t nb_mesh_port(nb_mesh_t *m);
/* 本节点最终使用的 node_id（配置给 0 时为随机值）。 */
uint64_t nb_mesh_node_id(nb_mesh_t *m);

/* ---- 服务名 -> 位 ---------------------------------------------------- */

/*
 * 服务名哈希到 32 位之一（FNV-1a 后折叠取模），返回单位掩码。
 * 用于没有共享 nb_service_table 的部署：两端对同一名字算出同一 bit 即可
 * 互相寻址。有服务表时优先用服务 ID（见 docs/mesh-discovery.md）。
 */
uint32_t nb_mesh_service_bit(const char *name);

/* 由编队 ID 推导的默认组播组（239.255.0.0/16 管理范围）。 */
void nb_mesh_default_group(uint32_t formation_id, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NB_MESH_H */
