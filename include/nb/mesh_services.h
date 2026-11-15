#ifndef NB_MESH_SERVICES_H
#define NB_MESH_SERVICES_H

/*
 * 服务位图契约（所有子系统编译期共享）。
 *
 * nb_mesh 心跳里的 service_mask 是 32 位位图，bit 与服务一一对应。位图必须
 * 在**编译期**由所有参与者共同约定：客户端按服务名/ID 算出 bit，服务端把
 * 自己提供的服务填进同一批 bit，两端一致才谈得上"掩码寻址零握手"。
 *
 * 分配规则：
 *   1. 新增服务**必须在这里分配 bit**，不要复用已废弃的 bit（编号一旦写进
 *      协议就等同于线上 ABI）。
 *   2. 按子系统分组，组内低位优先：每组预留 8 个 bit，扩组时先吃空位，再
 *      开新组。当前占用：
 *        CORE   0x00..0x07  框架自身能力（飞控、健康检查…）
 *        SENSE  0x08..0x0F  感知（定位/视觉…）
 *        ACT    0x10..0x17  执行机构（云台…）
 *        RELAY  0x18..0x1F  中继/网关（转发节点…）
 *   3. 没有共享 nb_service_table 的部署用 nb_mesh_service_bit(name) 把服务名
 *      哈希到 32 位之一——它只是"同一个名字算同一个 bit"，与这里的分组无关。
 *      一旦某服务在本头里有了固定 bit，就应改用该 bit（避免哈希碰撞）。
 *
 * 位图只回答"这个对端有没有这项能力"，不携带参数；具体接口版本仍走 RPC
 * 元数据（nb_metadata_t）。
 */

/* ---- CORE：框架自身能力（0x00..0x07） ---- */
/* 飞控核心服务：姿态/控制指令。 */
#define NB_SERVICE_CORE_FCS (1U << 0)
/* 健康检查/心跳应答（存活探针、降级上报）。 */
#define NB_SERVICE_CORE_HEALTH (1U << 1)
/* bit 2..7 预留给 CORE 组后续服务。 */

/* ---- SENSE：感知子系统（0x08..0x0F） ---- */
/* SLAM 建图/定位。 */
#define NB_SERVICE_SENSE_SLAM (1U << 8)
/* 视觉检测/跟踪。 */
#define NB_SERVICE_SENSE_VISION (1U << 9)
/* bit 10..15 预留给 SENSE 组后续服务。 */

/* ---- ACT：执行机构（0x10..0x17） ---- */
/* 云台控制。 */
#define NB_SERVICE_ACT_GIMBAL (1U << 16)
/* bit 17..23 预留给 ACT 组后续服务。 */

/* ---- RELAY：中继/网关（0x18..0x1F） ---- */
/* 跨网段转发节点。 */
#define NB_SERVICE_RELAY_NODE (1U << 24)
/* bit 25..31 预留给 RELAY 组后续服务。 */

/* 位图宽度（bit 数）：心跳 service_mask 为 32 位。 */
#define NB_SERVICE_BITS 32u
/* 合法位图全集。 */
#define NB_SERVICE_MASK_ALL 0xFFFFFFFFu

#endif /* NB_MESH_SERVICES_H */
