#ifndef EXAMPLE_ECHO_SERVICE_TABLE_H
#define EXAMPLE_ECHO_SERVICE_TABLE_H

/*
 * 共享服务表（示例）。
 *
 * 这份头文件是**客户端与服务端共同包含**的"接口契约"：
 *   - 服务名 / 方法名 / 数字 ID 只在编译期确定一次；
 *   - 数字 ID 就是发现层的服务位号：id N -> service_mask 的第 N 位。
 *     所以服务 ID 必须 < 32（nb_mesh 心跳里的位图宽度）。
 *
 * 装表的方式二选一：
 *   cfg.rpc.service_table = &echo_table;        // 建议：随节点配置一起装
 *   nb_service_table_install(&echo_table);      // 或者建节点前手动装
 *
 * 装了表之后，发现层把 "EchoService" 映射到 bit 0，两端一致且零碰撞；
 * 没装表时会退化成"把服务名哈希到 32 个 bit 之一"，那是兜底、可能撞位。
 */

#include <nb/rpc/service_table.h>

/* 服务 ID 0，方法 ID 1（方法编号只在同一服务内需要唯一）。 */
NB_SERVICE_DEFINE(echo_table, 0u, "EchoService",
    NB_SERVICE_METHOD(1u, "echo"));

#endif /* EXAMPLE_ECHO_SERVICE_TABLE_H */
