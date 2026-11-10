#ifndef NB_DISCOVERY_H
#define NB_DISCOVERY_H

/*
 * mesh 服务发现（控制面）：UDP 广播 HELLO/BYE + 对端注册表。
 *
 * 协议、分帧、CRC、序号/epoch、哈希表都在独立的 ntk_discovery 库里；
 * 本模块只是 nb 的适配层：把本地服务映射成 key（服务名），把 binding 映射
 * 成 peer 的 endpoint 列表，并把库的回调写进 registry。
 */

#include <nb/rpc/types.h>
#include <nb/rpc/endpoint.h>

#include <stddef.h>
#include <stdint.h>

#include <ntk.h>

#include <ntk_discovery/ntk_discovery.h>

#include "caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_node nb_node_t;

/* 显式发现目标（单播 HELLO）。 */
typedef struct nb_disc_target {
    int used;
    char ip[NB_HOST_MAX];
    uint16_t port;
} nb_disc_target_t;

/* 发现子系统自有状态：由 node 内嵌，模块内聚访问。 */
typedef struct nb_discovery_state {
    ntk_discovery_t *disc;
    nb_disc_target_t targets[NB_DISCOVERY_MAX_TARGETS];
    ntk_mutex *targets_lock; /* 保护 targets（运行期可被任意线程添加） */
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
