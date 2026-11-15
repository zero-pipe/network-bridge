/* nb_mesh 邻居表：哈希桶 + 服务位双向索引。见 mesh_table.h。 */
#include "mesh_table.h"

#include <stdlib.h>
#include <string.h>

#define NB_MESH_BUCKET_MASK (NB_MESH_BUCKETS - 1u)

/* 2 的幂校验：桶数一旦不是 2 的幂，位掩码取模就会错。 */
typedef char nb_mesh_buckets_pow2_assert[(NB_MESH_BUCKETS & NB_MESH_BUCKET_MASK) == 0 ? 1 : -1];

uint64_t nb_mesh_hash_node_id(uint64_t node_id)
{
    uint64_t x = node_id;

    /* murmur3 fmix64：高/低位都参与，避免 node_id 递增时桶号顺序堆积。 */
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

size_t nb_mesh_bucket_of(uint64_t node_id)
{
    return (size_t)(nb_mesh_hash_node_id(node_id) & (uint64_t)NB_MESH_BUCKET_MASK);
}

/* ---- 内存池 ---------------------------------------------------------- */

static nb_mesh_node_t *table_alloc_node(nb_mesh_table_t *t)
{
    nb_mesh_node_t *node = t->free_nodes;

    if (!node) {
        return NULL;
    }
    t->free_nodes = node->hash_next;
    memset(node, 0, sizeof(*node));
    return node;
}

static void table_free_node(nb_mesh_table_t *t, nb_mesh_node_t *node)
{
    node->hash_next = t->free_nodes;
    t->free_nodes = node;
}

static nb_mesh_svc_link_t *table_alloc_link(nb_mesh_table_t *t)
{
    nb_mesh_svc_link_t *link = t->free_links;

    if (!link) {
        /* 只在"某节点新增了服务位"时发生（节点数变化不会）；失效后仍可回收。 */
        link = (nb_mesh_svc_link_t *)calloc(1, sizeof(*link));
        if (!link) {
            return NULL;
        }
    } else {
        t->free_links = link->svc_next;
        memset(link, 0, sizeof(*link));
    }
    return link;
}

static void table_free_link(nb_mesh_table_t *t, nb_mesh_svc_link_t *link)
{
    link->svc_next = t->free_links;
    t->free_links = link;
}

/* ---- 服务位索引 ------------------------------------------------------ */

static void svc_unlink(nb_mesh_table_t *t, nb_mesh_svc_link_t *link)
{
    uint8_t bit = link->bit_index;

    if (link->svc_prev) {
        link->svc_prev->svc_next = link->svc_next;
    } else if (t->svc_head[bit] == link) {
        t->svc_head[bit] = link->svc_next;
    }
    if (link->svc_next) {
        link->svc_next->svc_prev = link->svc_prev;
    }
    /* 游标不能留下悬垂指针：指向下一个，没有就回到表头。 */
    if (t->svc_cursor[bit] == link) {
        t->svc_cursor[bit] = link->svc_next ? link->svc_next : t->svc_head[bit];
    }
    link->svc_prev = NULL;
    link->svc_next = NULL;
}

/* 把节点的全部服务位挂到对应索引链（头插）。内存不足时已挂的仍然有效。 */
static void node_index_services(nb_mesh_table_t *t, nb_mesh_node_t *node)
{
    uint8_t bit;

    for (bit = 0; bit < NB_MESH_SERVICE_BITS; bit++) {
        nb_mesh_svc_link_t *link;

        if ((node->service_mask & (1u << bit)) == 0) {
            continue;
        }
        link = table_alloc_link(t);
        if (!link) {
            continue; /* 索引缺位只会少一个可寻址节点，不影响邻居表本身 */
        }
        link->node = node;
        link->bit_index = bit;
        link->svc_next = t->svc_head[bit];
        if (link->svc_next) {
            link->svc_next->svc_prev = link;
        }
        t->svc_head[bit] = link;
        if (!t->svc_cursor[bit]) {
            t->svc_cursor[bit] = link;
        }
        link->node_next = node->links;
        node->links = link;
    }
}

/* 摘掉节点的全部服务位索引（掩码变化时先摘再挂）。 */
static void node_unindex_services(nb_mesh_table_t *t, nb_mesh_node_t *node)
{
    nb_mesh_svc_link_t *link = node->links;

    while (link) {
        nb_mesh_svc_link_t *next = link->node_next;

        svc_unlink(t, link);
        link->node = NULL;
        table_free_link(t, link);
        link = next;
    }
    node->links = NULL;
}

/* ---- 表操作 ---------------------------------------------------------- */

int nb_mesh_table_init(nb_mesh_table_t *t, size_t capacity)
{
    size_t i;

    if (!t) {
        return NB_MESH_ERR_INVALID;
    }
    memset(t, 0, sizeof(*t));
    if (capacity == 0) {
        capacity = NB_MESH_DEFAULT_MAX_PEERS;
    }
    t->nodes = (nb_mesh_node_t *)calloc(capacity, sizeof(*t->nodes));
    if (!t->nodes) {
        return NB_MESH_ERR_NOMEM;
    }
    t->capacity = capacity;
    /* 空闲链按倒序串起来，弹出的顺序与下标一致（调试时更直观）。 */
    for (i = capacity; i > 0; i--) {
        t->nodes[i - 1].hash_next = t->free_nodes;
        t->free_nodes = &t->nodes[i - 1];
    }
    return NB_MESH_OK;
}

void nb_mesh_table_fini(nb_mesh_table_t *t)
{
    nb_mesh_svc_link_t *link;

    if (!t) {
        return;
    }
    link = t->free_links;
    while (link) {
        nb_mesh_svc_link_t *next = link->svc_next;

        free(link);
        link = next;
    }
    /* 仍在表里的节点各自持有链节点，它们与 free_links 不重叠，需要单独回收。 */
    if (t->nodes) {
        size_t i;

        for (i = 0; i < t->capacity; i++) {
            nb_mesh_node_t *node = &t->nodes[i];

            while (node->links) {
                nb_mesh_svc_link_t *next = node->links->node_next;

                free(node->links);
                node->links = next;
            }
        }
        free(t->nodes);
    }
    memset(t, 0, sizeof(*t));
}

/* 表满时挑一个"最久未见"的邻居（O(N)，只在真满时发生）。 */
static nb_mesh_node_t *table_pick_victim(nb_mesh_table_t *t)
{
    nb_mesh_node_t *victim = NULL;
    size_t i;

    for (i = 0; i < NB_MESH_BUCKETS; i++) {
        nb_mesh_node_t *node;

        for (node = t->buckets[i]; node; node = node->hash_next) {
            if (!victim || node->last_seen_ms < victim->last_seen_ms) {
                victim = node;
            }
        }
    }
    return victim;
}

int nb_mesh_table_upsert(nb_mesh_table_t *t,
                         uint64_t node_id,
                         uint32_t ipv4,
                         uint16_t rpc_port,
                         uint32_t service_mask,
                         uint64_t now_ms,
                         uint64_t *evicted_id)
{
    size_t bucket;
    nb_mesh_node_t *node;
    nb_mesh_node_t **link;

    if (!t) {
        return NB_MESH_ERR_INVALID;
    }
    if (evicted_id) {
        *evicted_id = 0;
    }
    bucket = nb_mesh_bucket_of(node_id);
    for (node = t->buckets[bucket]; node; node = node->hash_next) {
        if (node->node_id == node_id) {
            /* 已存在：刷新定位信息与租约。掩码变化时先摘再挂，保证索引与掩码一致。 */
            if (node->service_mask != service_mask) {
                node_unindex_services(t, node);
                node->service_mask = service_mask;
                node_index_services(t, node);
            }
            node->ipv4 = ipv4;
            node->rpc_port = rpc_port;
            node->last_seen_ms = now_ms;
            return 0;
        }
    }

    node = table_alloc_node(t);
    if (!node) {
        /* 表满：按"最久未见"淘汰，绝不静默丢弃新节点。 */
        nb_mesh_node_t *victim = table_pick_victim(t);

        if (!victim) {
            return NB_MESH_ERR_NOMEM;
        }
        if (evicted_id) {
            *evicted_id = victim->node_id;
        }
        (void)nb_mesh_table_remove(t, victim->node_id);
        t->evictions++;
        node = table_alloc_node(t);
        if (!node) {
            return NB_MESH_ERR_NOMEM;
        }
    }
    node->node_id = node_id;
    node->ipv4 = ipv4;
    node->rpc_port = rpc_port;
    node->service_mask = service_mask;
    node->last_seen_ms = now_ms;
    link = &t->buckets[bucket];
    node->hash_next = *link;
    *link = node;
    t->count++;
    node_index_services(t, node);
    return 1;
}

int nb_mesh_table_remove(nb_mesh_table_t *t, uint64_t node_id)
{
    size_t bucket;
    nb_mesh_node_t **link;

    if (!t) {
        return 0;
    }
    bucket = nb_mesh_bucket_of(node_id);
    link = &t->buckets[bucket];
    while (*link) {
        nb_mesh_node_t *node = *link;

        if (node->node_id == node_id) {
            *link = node->hash_next;
            node_unindex_services(t, node);
            node->node_id = 0;
            table_free_node(t, node);
            if (t->count > 0) {
                t->count--;
            }
            return 1;
        }
        link = &node->hash_next;
    }
    return 0;
}

nb_mesh_node_t *nb_mesh_table_get(const nb_mesh_table_t *t, uint64_t node_id)
{
    nb_mesh_node_t *node;

    if (!t) {
        return NULL;
    }
    for (node = t->buckets[nb_mesh_bucket_of(node_id)]; node; node = node->hash_next) {
        if (node->node_id == node_id) {
            return node;
        }
    }
    return NULL;
}

nb_mesh_node_t *nb_mesh_table_find_service(nb_mesh_table_t *t, uint32_t bit)
{
    uint8_t index;
    nb_mesh_svc_link_t *cursor;
    nb_mesh_node_t *node;

    if (!t || bit == 0) {
        return NULL;
    }
    index = 0;
    while (index < NB_MESH_SERVICE_BITS && (bit & (1u << index)) == 0) {
        index++;
    }
    if (index >= NB_MESH_SERVICE_BITS) {
        return NULL;
    }
    cursor = t->svc_cursor[index];
    if (!cursor) {
        cursor = t->svc_head[index];
    }
    if (!cursor) {
        return NULL;
    }
    node = cursor->node;
    /* 轮转：下一次从后继开始，天然把同服务的多节点分摊开。 */
    t->svc_cursor[index] = cursor->svc_next ? cursor->svc_next : t->svc_head[index];
    if (node && (node->service_mask & (1u << index)) == 0) {
        return NULL; /* 理论上不会发生（摘挂同步）；防御性拒绝脏索引 */
    }
    return node;
}

size_t nb_mesh_table_sweep(nb_mesh_table_t *t, uint64_t now_ms, uint64_t lease_ms)
{
    size_t removed = 0;
    size_t i;

    if (!t || lease_ms == 0) {
        return 0;
    }
    for (i = 0; i < NB_MESH_BUCKETS; i++) {
        nb_mesh_node_t **link = &t->buckets[i];

        while (*link) {
            nb_mesh_node_t *node = *link;

            if (now_ms > node->last_seen_ms && now_ms - node->last_seen_ms > lease_ms) {
                uint64_t node_id = node->node_id;

                *link = node->hash_next;
                node_unindex_services(t, node);
                node->node_id = 0;
                table_free_node(t, node);
                if (t->count > 0) {
                    t->count--;
                }
                removed++;
                continue;
            }
            link = &node->hash_next;
        }
    }
    return removed;
}

size_t nb_mesh_table_snapshot(nb_mesh_table_t *t, nb_mesh_peer_info_t *out, size_t max)
{
    size_t written = 0;
    size_t i;

    if (!t || !out || max == 0) {
        return 0;
    }
    for (i = 0; i < NB_MESH_BUCKETS && written < max; i++) {
        nb_mesh_node_t *node;

        for (node = t->buckets[i]; node && written < max; node = node->hash_next) {
            nb_mesh_peer_info_t *peer = &out[written++];

            peer->node_id = node->node_id;
            peer->ipv4 = node->ipv4;
            peer->rpc_port = node->rpc_port;
            peer->service_mask = node->service_mask;
            peer->last_seen_ms = node->last_seen_ms;
        }
    }
    return written;
}

size_t nb_mesh_table_count(const nb_mesh_table_t *t)
{
    return t ? t->count : 0;
}
