#ifndef NB_MESH_TABLE_H
#define NB_MESH_TABLE_H

/*
 * nb_mesh 邻居表（内部实现，供 nb_mesh.c 与单元测试使用）。
 *
 * 结构：
 *   - 主索引：8192 个哈希桶（2 的幂）+ 链式冲突，键 = node_id，
 *     桶号 = murmur64(node_id) & (NB_MESH_BUCKETS - 1)；
 *   - 服务索引：32 条按服务位的双向链，链节点是 (节点, 位) 对；同一节点提供
 *     k 个服务就有 k 个链节点，因此 find_service 是 O(1) 命中而不是扫 8192 桶；
 *   - 每个服务位一个轮转游标，让同服务的多节点被均摊选中。
 *
 * 本模块**不加锁**：调用方（nb_mesh.c）用一把互斥量包住；单测直接单线程用。
 * 所有内存都在 init 时预分配（节点池）或从空闲链复用，运行期不产生 malloc
 * 抖动。
 */

#include <nb/mesh.h>

#include <stddef.h>
#include <stdint.h>

typedef struct nb_mesh_node nb_mesh_node_t;

/* (节点, 服务位) 索引链节点。 */
typedef struct nb_mesh_svc_link {
    nb_mesh_node_t *node;
    uint8_t bit_index;                    /* 0..31，对应 service_mask 的位 */
    struct nb_mesh_svc_link *svc_prev;    /* 服务位链前后 */
    struct nb_mesh_svc_link *svc_next;
    struct nb_mesh_svc_link *node_next;   /* 该节点自己的链节点单链 */
} nb_mesh_svc_link_t;

struct nb_mesh_node {
    uint64_t node_id;
    uint32_t ipv4;         /* 主机序 */
    uint16_t rpc_port;
    uint32_t service_mask;
    uint64_t last_seen_ms; /* 单调时钟 */
    nb_mesh_node_t *hash_next;
    nb_mesh_svc_link_t *links; /* 本节点的全部 (节点,位) 链节点 */
};

typedef struct nb_mesh_table {
    nb_mesh_node_t *buckets[NB_MESH_BUCKETS];
    nb_mesh_svc_link_t *svc_head[NB_MESH_SERVICE_BITS];
    nb_mesh_svc_link_t *svc_cursor[NB_MESH_SERVICE_BITS];
    nb_mesh_node_t *nodes;      /* 节点池，容量 capacity */
    nb_mesh_node_t *free_nodes; /* 空闲节点单链（复用 hash_next） */
    nb_mesh_svc_link_t *free_links;
    size_t capacity;
    size_t count;
    size_t evictions; /* 累计淘汰数（表满时的"最久未见"淘汰） */
} nb_mesh_table_t;

/* murmur3 风格的 64 位收尾混淆：把 node_id 打散到桶号。 */
uint64_t nb_mesh_hash_node_id(uint64_t node_id);
/* node_id 对应的桶下标（0..NB_MESH_BUCKETS-1）。 */
size_t nb_mesh_bucket_of(uint64_t node_id);

/* 分配节点池。capacity = 0 时用 NB_MESH_DEFAULT_MAX_PEERS。 */
int nb_mesh_table_init(nb_mesh_table_t *t, size_t capacity);
void nb_mesh_table_fini(nb_mesh_table_t *t);

/*
 * 插入或更新一个邻居。
 * 返回 1 = 新插入，0 = 已存在（原地更新），<0 = 错误码。
 * 表满时按"最久未见"淘汰一个邻居：若 evicted_id 非 NULL，写回其 node_id
 * （没有淘汰时写 0），调用方据此打告警日志。
 */
int nb_mesh_table_upsert(nb_mesh_table_t *t,
                         uint64_t node_id,
                         uint32_t ipv4,
                         uint16_t rpc_port,
                         uint32_t service_mask,
                         uint64_t now_ms,
                         uint64_t *evicted_id);

/* 删除一个邻居；返回 1 = 删除了，0 = 不存在。 */
int nb_mesh_table_remove(nb_mesh_table_t *t, uint64_t node_id);

/* 按 node_id 查邻居；不存在返回 NULL。 */
nb_mesh_node_t *nb_mesh_table_get(const nb_mesh_table_t *t, uint64_t node_id);

/*
 * 掩码寻址：返回一个 service_mask 含该位的邻居。多节点时按服务位游标轮转。
 * bit 必须是单位掩码；多 bit 时调用方自行取最低位。
 */
nb_mesh_node_t *nb_mesh_table_find_service(nb_mesh_table_t *t, uint32_t bit);

/* 剔除 last_seen_ms 距 now_ms 超过 lease_ms 的邻居；返回剔除个数。 */
size_t nb_mesh_table_sweep(nb_mesh_table_t *t, uint64_t now_ms, uint64_t lease_ms);

/* 拷贝前 max 个邻居（顺序不保证）；返回写入条数。 */
size_t nb_mesh_table_snapshot(nb_mesh_table_t *t, nb_mesh_peer_info_t *out, size_t max);

/* 当前邻居数。 */
size_t nb_mesh_table_count(const nb_mesh_table_t *t);

#endif /* NB_MESH_TABLE_H */
