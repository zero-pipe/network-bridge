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
2. **znet 上游缺口**（影响性能，不影响正确性）：
   - 缺 `zn_poller_current()` → RPC 侧用 `zn_poller_is_current_thread()` 反查兜底；
   - 批量收发：zbase/znet 已补 `zb_sock_sendto_batch` / `zb_sock_recvfrom_batch`
     （Linux `sendmmsg` / `recvmmsg`），RTPS 批量发布与接收已接入；RPC 传输
     仍未改用批量接口，可后续跟进。
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
