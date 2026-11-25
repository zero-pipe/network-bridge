# 路线图与当前状态

本文件只记录**还在推进的事**和**已知缺口**。已完成的历史细节见 git 记录与
各自的设计文档；不要把这里写成工作日志。

## 已完成（近期）

| 项 | 结果 | 文档 |
| --- | --- | --- |
| 分层重构 | `base` / `codec` / `rpc` / `dds` / `rtps` 五层，边界由契约测试守护 | [architecture.md](architecture.md) |
| RPC wire 统一 | tcp / udp / shm 共用 `nb_message_t` + `nb_envelope_t`，入站投递仅一条路径 | [architecture.md](architecture.md) |
| RTPS 拆分 | `rtps_wire` / `rtps_datapath` / `rtps_discovery` 从 6826 行的参与者里独立 | [rtps-timing.md](rtps-timing.md) |
| RTPS 时序说明 | 报文结构、收发/分片/发现时序、节拍参数表 | [rtps-timing.md](rtps-timing.md) |
| Cyclone DDS 后端移除 | 内建 RTPS 成为唯一后端，保留线缆互通能力 | [rtps-api.md](rtps-api.md) |
| 基础依赖迁移 | `ntk` → `znet` + `zbase`，`3rdparty/ntk` 已删除 | 本文件 |

## 已完成（本轮）

| 项 | 结果 | 文档 |
| --- | --- | --- |
| 集群发现重写（`nb_mesh`） | 26 B 组播心跳 + 编队隔离 + 服务位图寻址，`nb_node_resolve()` 增加发现兜底 | [mesh-discovery.md](mesh-discovery.md) |

## 待办

1. **双平台判定**：Linux 为权威（38 项 ctest + perf/stability 门禁）；Windows 保证
   构建与 35 项 ctest。
3. **znet 上游缺口**（影响性能，不影响正确性）：
   - 缺 `zn_poller_current()` → RPC 侧用 `zn_poller_is_current_thread()` 反查兜底；
   - 缺批量收发 → UDP 传输回退到单包循环，失去 `sendmmsg` / `recvmmsg` 优化。
   建议在 znet 补齐后删掉本仓库的兜底代码。
4. **DDS 侧类型**：topic 传真实 IDL 类型名时按裸 CDR body 收发，编解码归应用
   （IDL 生成代码）；默认 `dds::Message` 仍是不透明字节、长度精确。默认的按
   type name 匹配下已可与 Cyclone DDS / Fast DDS 互通。
5. **XTypes TypeObject（暂缓）**：严格类型一致性
   （`EXACT_TYPE_TYPE_CONSISTENCY`）要求 SEDP 通告 `PID_TYPE_INFORMATION`：
   `TypeIdentifier` + `MinimalTypeObject` + 14 字节 `EquivalenceHash`（对序列化
   TypeObject 做 MD5），且必须与对端逐字节一致，否则严格对端拒绝匹配。当前无
   任何 TypeObject 实现，仅有 `PID_TYPE_NAME`。
   - 涉及模块：`rtps_sedp`（参数写入）、`rtps_param`（`PID_TYPE_INFORMATION`
     编解码）、新增 TypeObject 模型与哈希（MD5 已有，`zbase`）。
   - **前置条件**：Linux + Cyclone/FastDDS 参考对端。哈希算法连一个字节都不能
     差，必须在真实对端上验收；本机（Windows、无参考对端）无法验证，故先挂起。
   - 收尾：实现后用严格模式对端跑互通测试，并把本节移到"已完成"。

## 约定

- 公共头只放在 `include/nb/`，内部头放 `src/*/internal/` 且不安装。
- `base` 不得引用 `rpc` / `dds`；`rpc` 不得引用 `dds` / `rtps`；`dds` 不得引用 `rpc`。
- 上述边界由 `contract.source_layering` / `contract.nb_headers` 在 CI 中强制。
