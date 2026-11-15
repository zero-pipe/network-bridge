#ifndef NB_DISCOVERY_H
#define NB_DISCOVERY_H

/*
 * mesh 服务发现（RPC 控制面）：nb_mesh 的适配层。
 *
 * 协议、报文、编队隔离、按需内省都在 nb_mesh 库里；这里只做三件事：
 *   1. 把本地服务清单交给 mesh（用于 schema_hash 与内省应答）；
 *   2. 把 mesh 的对端/内省回调翻译成 registry 里的服务 -> 端点映射；
 *   3. 实现 registry backend 的 start/stop/tick/announce。
 *
 * 关键点：契约哈希（schema_hash）一致的节点不做任何握手——本地服务清单即
 * 远端服务清单，registry 直接按本地清单填充。只有异构节点（哈希不同、
 * 动态插件）才单播拉取一次 manifest 并缓存。
 */

#include <nb/rpc/types.h>
#include <nb/rpc/endpoint.h>
#include <nb/mesh.h>

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

int nb_discovery_find(nb_node_t *node,
                       const char *service,
                       char *ip_out,
                       size_t ip_cap,
                       uint16_t *port_out,
                       nb_transport_kind_t *transport_out);

#ifdef __cplusplus
}
#endif

#endif /* NB_DISCOVERY_H */
