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
4. **DDS 侧**：`dds::Message` 的序列化仍是"长度前缀 + 原始字节"，与 Cyclone
   互通时按需扩展类型化支持。

## 约定

- 公共头只放在 `include/nb/`，内部头放 `src/*/internal/` 且不安装。
- `base` 不得引用 `rpc` / `dds`；`rpc` 不得引用 `dds` / `rtps`；`dds` 不得引用 `rpc`。
- 上述边界由 `contract.source_layering` / `contract.nb_headers` 在 CI 中强制。
