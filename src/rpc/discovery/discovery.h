#ifndef NB_DISCOVERY_H
#define NB_DISCOVERY_H

/*
 * mesh 服务发现（RPC 控制面）：nb_mesh 的薄适配层。
 *
 * 协议、报文、编队隔离、邻居表、心跳/租约全部在 nb_mesh 库里；这里只做映射：
 *   1. 把本节点的服务名折算成服务位图（有共享 nb_service_table 时用服务 ID，
 *      否则用服务名哈希位），交给 nb_mesh 随心跳通告；
 *   2. 调用时按需掩码寻址：服务名 -> 位 -> nb_mesh_find() -> ip:port，
 *      再拼成本节点主绑定（tcp 优先）的 scheme 作为 RPC 端点；
 *   3. 实现 registry backend 的 start/stop/tick/announce。
 *
 * 关键点：**这里不再往 registry 里塞"服务名 -> 端点"的预填条目**。registry 只
 * 保留程序自己注册的内容；未知服务一律按需查邻居表，因此客户端本地不需要事先
 * 知道远端提供哪些服务名（异构服务集不再互相失明）。
 */

#include <nb/mesh.h>
#include <nb/rpc/config.h>
#include <nb/rpc/endpoint.h>
#include <nb/rpc/types.h>

#include <stddef.h>
#include <stdint.h>

#include <zn/zn.h>

#include "caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_node nb_node_t;

/* 显式发现目标（单播心跳，组播不可用时的兜底）。 */
typedef struct nb_disc_target {
    int used;
    char ip[NB_HOST_MAX];
    uint16_t port;
} nb_disc_target_t;

/* 发现子系统自有状态：由 node 内嵌，模块内聚访问。 */
typedef struct nb_discovery_state {
    nb_mesh_t *mesh;
    nb_disc_target_t targets[NB_DISCOVERY_MAX_TARGETS];
    zb_mutex *targets_lock; /* 保护 targets（运行期可被任意线程添加） */
} nb_discovery_state_t;

int nb_discovery_init(nb_node_t *node);
void nb_discovery_shutdown(nb_node_t *node);
void nb_discovery_send_hello(nb_node_t *node, int bye);
void nb_discovery_tick(nb_node_t *node, uint64_t now_ms);

/*
 * 按服务名做掩码寻址。命中返回 NB_OK 并写出 RPC 端点；否则 NB_ERR_NOTFOUND。
 * 只查邻居表，不改 registry。
 */
int nb_discovery_find(nb_node_t *node, const char *service, nb_endpoint_t *out);

/* 追加一个定向单播发现目标（组播不可用或跨网段时使用）。 */
int nb_discovery_add_peer(nb_node_t *node, const char *ip, uint16_t discovery_port);

#ifdef __cplusplus
}
#endif

#endif /* NB_DISCOVERY_H */
