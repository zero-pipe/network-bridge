#ifndef NB_NODE_ACCESS_H
#define NB_NODE_ACCESS_H

/*
 * node 访问门面。
 *
 * 供 node 作用域后端（如 discovery 控制面）以受控方式访问 node 状态，
 * 避免它们直接穿透 struct nb_node。实现于 nb_node_access.c。
 */

#include <nb/rpc/types.h>

#include <stddef.h>
#include <stdint.h>

#include <zn/zn.h>

#include "discovery.h"
#include "registry.h"

uint32_t nb_node_id(const nb_node_t *node);
const char *nb_node_discovery_host(const nb_node_t *node);
uint32_t nb_node_ttl_ms(const nb_node_t *node);
uint32_t nb_node_discovery_formation(const nb_node_t *node);
uint32_t nb_node_hello_interval_ms(const nb_node_t *node);
uint32_t nb_node_discovery_service_mask(const nb_node_t *node);
zn_poller *nb_node_poller(const nb_node_t *node);
zn_poller_pool *nb_node_pool(const nb_node_t *node);
/*
 * 当前线程正在驱动的 poller（限本 node 的 poller 集合）；线程没驱动任何 poller
 * 时返回 NULL。
 *
 * TODO(znet): znet 尚未提供"当前线程绑定的 poller"查询接口（改名前的实现是在
 * poller bind 时写线程局部变量）。这里改用 zn_poller_is_current_thread() 在 node
 * 自己的 poller 集合里反查，语义一致；znet 补出对应接口后删掉本函数，各调用点
 * 直接换成 znet 的接口即可。
 */
zn_poller *nb_node_current_poller(const nb_node_t *node);
nb_registry_t *nb_node_registry(nb_node_t *node);
nb_discovery_state_t *nb_node_discovery(nb_node_t *node);
void nb_node_set_discovery_port(nb_node_t *node, uint16_t port);

/* 绑定/服务枚举（供发现帧构建）。 */
size_t nb_node_service_count(const nb_node_t *node);
const char *nb_node_service_name(const nb_node_t *node, size_t index);
const char *nb_node_binding_scheme(const nb_node_t *node, size_t index);
uint16_t nb_node_binding_port(const nb_node_t *node, size_t index);
int nb_node_binding_uri(const nb_node_t *node, size_t index, char *out, size_t cap);

#endif /* NB_NODE_ACCESS_H */
