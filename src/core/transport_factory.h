#ifndef NB_TRANSPORT_FACTORY_H
#define NB_TRANSPORT_FACTORY_H

/*
 * 传输装配：为 node 的一个 binding 创建/启动传输（UDP/TCP 或自定义 provider），
 * 并登记到 node->transports[]。从 node.c 拆出，聚焦“装配”职责。
 */

#include <nb/types.h>
#include <nb/config.h>

#include <stdint.h>

/* 成功返回 NB_OK；scheme 未注册/参数非法/启动失败返回相应错误码。 */
int nb_node_transport_add(nb_node_t *node,
                          const char *scheme,
                          const char *host,
                          uint16_t port,
                          const char *path,
                          nb_intra_process_t intra_process,
                          uint32_t poll_us,
                          const nb_qos_t *qos);

/* 按 binding 配置创建并启动传输（推荐入口）。 */
int nb_node_transport_add_binding(nb_node_t *node, const nb_binding_config_t *binding);

#endif /* NB_TRANSPORT_FACTORY_H */
