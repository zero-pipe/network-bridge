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
| RTPS 数据面性能 | 批量收发 + 多子消息聚合 + 自适应大报文 + 可靠槽位复用 + 零拷贝 scatter；best-effort 全面超越 Cyclone，详见下方“性能基线” | 本文件 |

## 已完成（本轮）

| 项 | 结果 | 文档 |
| --- | --- | --- |
| 集群发现重写（`nb_mesh`） | 26 B 组播心跳 + 编队隔离 + 服务位图寻址，`nb_node_resolve()` 增加发现兜底 | [mesh-discovery.md](mesh-discovery.md) |

## 性能基线（RTPS 数据面）

同机 loopback、同方法论（固定时长、时间戳算单程延迟、载荷字节对齐）对比
Cyclone DDS 0.10.5。方法要点（均为已落地的设计事实，见 `dbad0f9`…`cfe58f4`）：

- **批量发布** `nb_dds_writer_write_batch`：多样本聚合为一条 Message 的多个
  DATA 子消息；`sendmmsg`（`zb_sock_sendmsg_batch`，逐消息 iovec）一次刷出。
- **批量接收**：`recvmmsg`（`zb_sock_recvfrom_batch`）+ participant 复用 RX 环形缓冲。
- **大报文自适应**：loopback/大 MTU 上放宽单报文尺寸（loopback 聚合到 60KB），
  非 loopback 仍受单包约束；> 单包样本自动回退分片单发路径。
- **可靠历史槽位缓冲复用**：环形槽位持有可复用 payload，避免每样本 malloc/free
  ——这是可靠吞吐从 ~130k 跃升的关键。
- **零拷贝 scatter**：封装字节（XCDR1 头 + 可选 u32）由引擎生成，应用体作为
  独立 iovec 直接引用，发送路径不再把 payload 拷进报文缓冲；可靠仅保留一次
  历史拷贝（供重传）。

| 载荷 | best-effort（本栈 / Cyclone） | reliable（本栈 / Cyclone） |
| --- | --- | --- |
| 64 B | 2.60 M / 1.21 M（2.15×） | 971 k / 1.13 M（0.86×） |
| 1 KB | 1.21 M / 807 k（1.5×） | 756 k / 830 k（0.91×） |
| 8 KB | 513 k / 173 k（3.0×） | 331 k / 174 k（1.9×） |

**已知缺口**：可靠小包（64B/1KB）约 0.9×——差距来自可靠固有的“保留一次拷贝 +
ACKNACK 协议开销”（Cyclone 可靠≈best-effort 是因为把这层摊得极薄）。要再进一步
需深挖 ACK/去重/历史路径，属低回报高风险，暂不排期。
基准工具：`tools/dds_bench.c`（单写）、`tools/dds_bench_batch.c`（批量/可靠扫描）；
Cyclone 侧对照与互通在实验环境（非仓库）运行。

## 待办

1. **双平台判定**：Linux 为权威（38 项 ctest + perf/stability 门禁）；Windows 保证
   构建与 35 项 ctest。
2. **znet 上游缺口**（影响性能，不影响正确性）：
   - 缺 `zn_poller_current()` → RPC 侧用 `zn_poller_is_current_thread()` 反查兜底；
   - 批量收发：zbase/znet 已补 `zb_sock_sendto_batch` / `zb_sock_recvfrom_batch`
     与 `zb_sock_sendmsg_batch`（Linux `sendmmsg` / `recvmmsg`），RTPS 批量发布与
     接收已接入；RPC 传输仍未改用批量接口，可后续跟进。
3. **DDS 侧类型**：topic 传真实 IDL 类型名时按裸 CDR body 收发，编解码归应用
   （IDL 生成代码）；默认 `dds::Message` 仍是不透明字节、长度精确。默认的按
   type name 匹配下已可与 Cyclone DDS / Fast DDS 互通。
4. **XTypes TypeInformation（已打通，剩余可选增强）**：
   - **已完成**：topic 可用 `nb_dds_topic_create_with_type_info(...)` 携带一份
     idlc 生成的 TypeInformation 不透明 blob（Cyclone `idlc -l c` 输出的
     `TYPE_INFO_CDR_<Type>` 字节），SEDP 以 `PID_TYPE_INFORMATION`(0x75) 原样通告。
     已在 Linux + Cyclone 0.10.5 上验证：严格类型一致性
     （`DISALLOW_TYPE_COERCION` + `force_type_validation`）下对端**能匹配并收到数据**；
     去掉该参数则严格对端拒绝匹配（对照实验通过）。
   - **剩余（可选）**：
     - 运行时从类型结构生成 `MinimalTypeObject` + `EquivalenceHash`，免去用户手动
       用 idlc 生成 blob（跨厂商哈希算法差异也需此项才能互通）；
     - TypeLookup 服务（按 TypeIdentifier 拉取 TypeObject），用于哈希不一致但
       结构可赋值的严格场景。
   - 说明：`rtps_param` 仍按字节透传 0x75，无需实现 XCDR2 结构编解码。

## 约定

- 公共头只放在 `include/nb/`，内部头放 `src/*/internal/` 且不安装。
- `base` 不得引用 `rpc` / `dds`；`rpc` 不得引用 `dds` / `rtps`；`dds` 不得引用 `rpc`。
- 上述边界由 `contract.source_layering` / `contract.nb_headers` 在 CI 中强制。
