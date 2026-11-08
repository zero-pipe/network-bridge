# RTPS 2.5 标准对照与互通评估

本文对照 **OMG DDSI-RTPS 2.5**（formal/22-04-01）检查 `src/rtps/` 的实现程度，
并区分两件事：

1. **规范符合性**：协议要求了什么，我们实现了多少；
2. **工业互通**：在真实 DDS 实现（Cyclone DDS、Fast DDS）上能不能跑通。

结论先说：这是一套**面向 DDS 发布/订阅互通的最小 RTPS 子集**，线格式和
SPDP/SEDP、可靠/尽力而为、分片、实例状态、64 位序列号、INFO_DST 过滤、
标准 participant/端口映射等主路径与规范一致，并已和 Cyclone DDS 11.0.1、
Fast DDS 3.6.2 做了双向验证；但**不是完整 RTPS 2.5 实现**，XTypes/TypeObject、
IPv6、用户数据组播 d2、content filter、group ordered/coherent 等仍是差距。
工业实践上，Cyclone 的对齐程度比“规范逐条符合”更重要。

## 1. 结论

### 1.1 “对齐标准”到底指什么

“符合 RTPS 标准”不等于“实现规范里的每一个可选项”。DDSI-RTPS 2.5 把内容分成：

- **必需行为**（8.4.2）：所有实现必须满足才能互操作，例如消息接收规则、
  SPDP/SEDP、可靠 writer 的 HEARTBEAT、reader 的 ACKNACK；
- **可选行为**（8.4.14）和厂商扩展（8.6）：实现可以不做，只要不破坏扩展机制；
- **独立规范**：DDS QoS、XTypes、DDS Security、内容过滤等很多能力不在 RTPS
  线协议本文里，而在 DDS/XTypes/Security 等规范中。

因此一个实现可以“符合标准”，但只覆盖必需子集加自己需要的可选特性。两个实现
能互通，只能证明它们在**共同覆盖的路径**上一致，不能证明双方逐条实现了规范；
它们甚至可以同时省略同一个可选特性而仍然互通。

Cyclone DDS 11 的源码声明 “This implements DDSI 2.5, accepts 2.1 and later”
（`3rdparty/cyclonedds/src/core/ddsi/include/dds/ddsi/ddsi_protocol_version.h`），
它对齐的是必需互操作行为和它实际支持的可选特性，并不代表它实现了 2.5 的每个
条款。Cyclone 也有自己的取舍和厂商扩展（例如我们代码里兼容的
`PID_CYCLONE_REQUESTS_KEYHASH`），它的 TCP 模式也是自己的传输选择，不是
RTPS 2.5 的 UDP/IP PSM。

所以：

- 和 Cyclone 对齐，是**工业互通目标**，不是“标准逐条符合”的证书；
- 和 Cyclone 互通，不能自动推导出和 Fast DDS 互通——Fast DDS 可能默认启用
  不同的可选特性（XTypes/TypeObject、INFO_DST 多路复用等）；我们已补做
  Fast DDS 3.6.2 的基础矩阵，见第 6 节；
- 我们当前的目标是“必需互通行为 + Cyclone/Fast DDS 实际使用的扩展”，更严格
  的 XTypes、多 participant、IPv6 场景仍需要单独验证。

总体结论如下（✅ 完整/已验证，🟡 部分实现或有偏差，❌ 未实现，➖ 不在本项目范围）。

| 维度 | 状态 | 说明 |
| --- | --- | --- |
| RTPS 线格式（头部、子消息、ParameterList、CDR） | 基本符合 | 已实现主路径子消息，未知子消息/PID 可跳过 |
| SPDP/SEDP 简单发现 | 符合并已验证 | Cyclone 双向发现、QoS 回读、重启、dispose 均通过 |
| 可靠/尽力而为、分片、GAP | 符合并已验证 | HEARTBEAT/ACKNACK/NACK_FRAG/DATA_FRAG 主路径 |
| DDS QoS 映射 | 部分 | 常用策略已映射；content filter、group ordered/coherent 未实现 |
| 实例/key/dispose | 符合并已验证 | PID_KEY_HASH、PID_STATUS_INFO、key-only dispose |
| 类型系统/XTypes | 未实现 | 只按 topic + type 名字匹配，不交换 TypeObject |
| INFO_DST 接收过滤 | ✅ | 按目标前缀过滤 Entity Submessage，全零为广播 |
| IPv6 | 未实现 | 仅 UDPv4/TCPv4 locator |
| 安全 / iceoryx | 不在范围 | 明文 RTPS；安全由外部方案负责 |
| 多 participant / 标准端口映射 | ✅（可选） | `standard_ports=1` 使用 d0/d1/d3 + participant_id；用户组播 d2 未分配 |
| TCP | 非标准扩展 | RTPS 2.5 本文只定义 UDP/IP；本实现 TCP 只与本实现互通 |
| Cyclone DDS 11.0.1 互通 | 已验证 | 矩阵见第 5 节 |
| Fast DDS 3.6.2 / 2.5.0 互通 | 基础场景已验证 | 双向、BE/可靠、1 KB 与 200 KB、TL 晚加入、重启、多 participant；见第 6 节 |

## 2. 结构模块（8.2）

| 规范内容 | 状态 | 实现/说明 |
| --- | --- | --- |
| GUID、GuidPrefix、EntityId | ✅ | `rtps_types.h`、`rtps_participant.c` |
| Participant / Endpoint / Writer / Reader | ✅ | 同一套对象模型，writer/reader 有本地/远端表 |
| HistoryCache / CacheChange | ✅ | writer 历史、reader 窗口、按序列号查找 |
| Writer/Reader 与 DDS DataWriter/DataReader 映射 | 🟡 | 映射到本项目的 writer/reader，不暴露完整 DDS 实体 API |
| Group（Entity Group / GroupDigest） | ❌ | 未实现组实体和组摘要 |
| `SequenceNumber` 64 位 | ✅ | 内部统一 `uint64_t`，wire high/low 保留；历史和位图可跨 2^32 |
| `Locator` IPv4 | ✅ | UDPv4、TCPv4；`kind` 8 的 TCPv6 仅定义未打通 |
| `Locator` IPv6 | ❌ | 不解析、不通告 IPv6 locator |

## 3. 消息模块（8.3）

| 规范内容 | 状态 | 说明 |
| --- | --- | --- |
| RTPS Header（20 字节）、ProtocolVersion、VendorId | ✅ | 发送 2.4；接收要求主版本 2 |
| 子消息通用头、长度对齐、未知子消息跳过 | ✅ | `nb_rtps_message_next` 按长度遍历，未知 id 忽略 |
| DATA（E/Q/K 标志、inline QoS、key/status） | ✅ | 支持 PID_KEY_HASH、PID_STATUS_INFO，key-only dispose |
| DATA_FRAG | ✅ | 分片、重组、按 fragment 位图补片 |
| HEARTBEAT / HEARTBEAT_FRAG | ✅ | FINAL 与 L（liveliness）标志区分处理 |
| ACKNACK / NACK_FRAG | ✅ | bitmap、FINAL 语义、按缺口请求 |
| GAP | ✅ | 连续/位图形式；SEDP 与用户数据都处理 |
| INFO_TS | ✅ | 支持正常时间戳；invalidate 的 0 长度形式会保留上一个时间戳（小偏差） |
| INFO_SRC | ✅ | 解析并覆盖后续 Entity Submessage 的源前缀 |
| INFO_DST | ✅ | 接收侧按目标前缀过滤；全零前缀视为广播 |
| INFO_REPLY | ❌ | 未解析（请求/响应不是 RTPS 核心发现路径） |
| PAD | ✅ | 按长度跳过 |
| ParameterList、SequenceNumberSet、FragmentNumberSet | ✅ | SPDP/SEDP 和可靠性路径使用 |
| Checksum（CRC32c/CRC64）、MessageLength、UExtension/WExtension、GroupDigest | ❌ | RTPS 2.5 的扩展元素，未实现 |
| 端序 | 🟡 | 只发送/接受小端（E=1）；x86 上的 Cyclone/Fast DDS 均发小端 |

## 4. 行为、发现与 QoS（8.4–8.7、9、10）

### 4.1 行为（8.4）

| 规范要求 | 状态 | 说明 |
| --- | --- | --- |
| Writer 按顺序发送、保留历史 | ✅ | writer history + 序列号 |
| Reader 请求 inline QoS 时提供 | ✅ | DATA 的 Q 标志与 inline QoS |
| 可靠 writer 周期 HEARTBEAT | ✅ | 可配置 heartbeat 周期；空历史也处理 |
| 收到 ACKNACK 后重发/发 GAP/HEARTBEAT | ✅ | 状态机 + 退避 |
| 可靠 reader 收到非 FINAL HEARTBEAT 后 ACKNACK | ✅ | 支持延迟/合并 |
| 分片样本的 HEARTBEAT_FRAG/NACK_FRAG | ✅ | 重组与补片 |
| Writer/Reader 匹配（可靠性与 QoS RxO） | ✅ | `endpoint_matches` 系列规则 |
| Liveliness（automatic/manual） | 🟡 | 自动租约、manual-by-topic 的 L 标志；没有 `dds_assert_liveliness()` |
| Directed write | ❌ | 无显式 API；接收侧已能按 INFO_DST 过滤 |
| Group ordered access / coherent sets | ❌ | 只在 SEDP 中通告/参与匹配，不提供投递语义 |
| Content-filtered topics | ❌ | 不支持内容过滤表达式 |

### 4.2 发现（8.5）

| 规范内容 | 状态 | 说明 |
| --- | --- | --- |
| SPDP 组播/单播、租约、participant 通告 | ✅ | 默认 `239.255.0.1`，端口 `7400 + 250*domain` |
| SPDP 显式 peer（单播） | ✅ | 本项目扩展，便于固定端口/无组播环境 |
| SEDP publications/subscriptions | ✅ | PID_TOPIC_NAME、PID_TYPE_NAME、QoS、locator、key hash |
| SEDP dispose/unregister | ✅ | key-only + PID_STATUS_INFO；Cyclone 重启用例通过 |
| BuiltinEndpointSet | ✅ | participant/publications/subscriptions + participant message |
| Participant Message（writer liveliness） | ✅ | 内置 writer/reader 与消息处理 |
| TypeObject / TypeInformation / XTypes 类型发现 | ❌ | 未知 PID 忽略；类型一致性仅比较 type 名字 |
| 多 participant 标准端口（d0/d1/d2/d3 + participantId） | ✅（可选） | `standard_ports=1` 计算 d0/d1/d3 端口；d2 未分配 |
| 备选发现协议 | ➖ | 不需要 |

### 4.3 QoS 与 DDS 特性（8.7）

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| Reliability / Durability / History / Depth | ✅ | 映射到 SEDP PID 与本地匹配 |
| Deadline / Lifespan / LatencyBudget | ✅ | 匹配 + 定时检查；lifespan 过滤 |
| Liveliness / LeaseDuration / Ownership / Partition | ✅ | 常用路径；manual liveliness 无显式 assert |
| ResourceLimits | 🟡 | reader 执行 max_instances；writer 主要由历史深度/字节预算间接约束 |
| Presentation（scope/coherent/ordered） | 🟡 | 通告并参与匹配；coherent/ordered 不提供实际投递保证 |
| Instance state / key / dispose / unregister | ✅ | PID_KEY_HASH、PID_STATUS_INFO、view/instance state |
| Content-filtered topics | ❌ | 未实现 |
| Property lists | ❌ | PID 定义但未使用 |
| Original writer info | ❌ | 未实现 |

### 4.4 PSM（9）与序列化（10）

| 规范内容 | 状态 | 说明 |
| --- | --- | --- |
| UDP/IP PSM、RTPS 类型映射 | ✅ | 小端 CDR；IPv4 locator |
| 端口映射公式 | ✅（可选） | `standard_ports=1` 实现 d0/d1/d3 + PG*participant_id；默认仍为临时端口 + locator 通告 |
| 报文映射 | ✅ | 已实现子消息按 PSM 编码 |
| SerializedPayloadHeader（CDR 封装） | ✅ | 4 字节封装头；用户数据 CDR body |
| Built-in endpoint payload（PL_CDR） | ✅ | SPDP/SEDP ParameterList |
| 用户类型 CDR / IDL / XTypes | 🟡 | 载荷是不透明 CDR body + type 名字；应用自行序列化，不生成类型代码 |
| TCP PSM | ➖ | RTPS 2.5 本文只定义 UDP/IP；本实现 TCP 是自有扩展，只保证本实现实例互通 |

## 5. Cyclone DDS 互通（已验证）

`tests/test_rtps_cyclone.c` 覆盖下列矩阵，两种模式（组播发现、单播固定端口）
在 Linux 上均通过：

| 场景 | 方向 | 结果 |
| --- | --- | --- |
| 基础发现与数据 | 双向 | ✅ SPDP/SEDP、QoS 回读、可靠投递、8 KiB 分片 |
| 尽力而为 | 双向 | ✅ |
| TRANSIENT_LOCAL 晚加入 | Cyclone → nb | ✅ ACKNACK 拉取历史 |
| TRANSIENT_LOCAL 晚加入 | nb → Cyclone | ✅ 新 reader 回放 |
| 多 reader 扇出 | nb → 2 个 Cyclone reader | ✅ |
| Participant 重启 | Cyclone → nb | ✅ dispose/租约、新前缀重新发现 |
| 200 KiB 样本 | 双向 | ✅ 数百分片、NACK_FRAG |
| TCP | — | 未列入：Cyclone 的 TCP 模式会替换发现传输，与 UDP SPDP 不兼容 |

这证明“能跟 Cyclone 这种工业实现完全通信”在**上表范围内**成立；不能外推为
所有 RTPS/DDS 特性都互通。

## 6. Fast DDS 互通（已验证基础矩阵）

### 6.1 验证环境

- Fast DDS：3.6.2（仓库 `3rdparty/Fast-DDS-master`），Fast CDR 2.3.4，
  foonathan_memory 0.7-4，Asio 1.30.2，tinyxml2 10.0.0。
- 构建：Ubuntu 20.04 / GCC 9.4，CMake 3.16（Fast DDS 源码声明 3.22；验证时
  仅把 `cmake_minimum_required` 降为 3.16 并关闭测试/示例/安全/TLS；
  这是测试环境做法，不作为上游构建建议）。
- 运行：`FASTDDS_BUILTIN_TRANSPORTS=UDPv4` 关闭 Fast DDS 的 SHM 传输，确保
  数据真的走 RTPS/UDPv4；domain 0，topic `hello_world_topic`，type
  `HelloWorld`，CDR1（XCDR1）载荷。
- 双方在同一主机 192.168.54.14 上，通过 SPDP 组播和显式单播 peer 发现。

### 6.2 验证矩阵

| 场景 | 方向 | 载荷 | 结果 |
| --- | --- | --- | --- |
| Fast DDS 默认 writer → nb BE reader | FDDS → nb | 16 B × 3 | ✅ 按序收到 |
| Fast DDS 显式 reliable writer → nb reliable reader | FDDS → nb | 16 B × 3 | ✅ 按序收到 |
| 同上 | FDDS → nb | 1 KB | ✅ |
| 同上 | FDDS → nb | 200 KB | ✅ DATA_FRAG 重组 |
| Fast DDS TRANSIENT_LOCAL writer → 晚加入 nb TL reader | FDDS → nb | 16 B / 200 KB | ✅ 通过 HEARTBEAT/ACKNACK 回放 |
| nb BE writer → Fast DDS 默认 reader | nb → FDDS | 16 B × 3 | ✅ |
| nb reliable writer → Fast DDS 显式 reliable reader | nb → FDDS | 16 B × 3 | ✅ 按序收到 |
| 同上 | nb → FDDS | 1 KB | ✅ |
| 同上 | nb → FDDS | 200 KB | ✅ 分片与可靠重传 |
| nb TRANSIENT_LOCAL writer → 晚加入 Fast DDS TL reader | nb → FDDS | 16 B / 200 KB | ✅ 历史回放 |
| Fast DDS publisher 退出后重启 → nb reader | FDDS → nb | 16 B × 2 次启动 | ✅ 重新发现并收到 |
| 两个 Fast DDS participant + 一个 nb participant | FDDS → nb | 16 B × 2 | ✅ publisher `--matched 2` 后 nb reader 与 Fast DDS subscriber 都收到 |
| nb 标准端口映射（participant_id=10）↔ Fast DDS | 双向 | 16 B / 1 KB | ✅ d0/d1/d3 端口与 Fast DDS 共存 |

前 13 行说明 SPDP/SEDP、topic/type 名匹配、CDR1 载荷、可靠/尽力而为、
TRANSIENT_LOCAL 晚加入、DATA_FRAG 重组、Fast DDS 重启后的重新发现，以及
同机多 participant（显式单播 peer）都能工作。

多 participant 用例需要让 Fast DDS publisher 等到两个 reader 都匹配后再发送
（`--matched 2`）。如果 publisher 只等第一个匹配就发送，另一个 participant
可能还没完成发现，BE 样本会丢，看起来像“互通失败”；这是测试时序问题，不是
协议问题。nb 侧使用显式 peer 端口 7410/7412/7414/7416，并把 unicast 端口
设为 7900，以避免和 Fast DDS 的 7400 组播 socket 抢收包。

### 6.3 发现的问题与兼容性取舍

1. **Fast DDS 默认 reliable writer 省略 `PID_RELIABILITY`。** Fast DDS 把
   DataWriter 的默认可靠性设为 RELIABLE，并在 SEDP 中省略该 PID（其
   `QosPoliciesSerializer` 的 `should_be_sent` 逻辑）。按 DDS/RTPS 默认值，
   PID 缺失应理解为 BEST_EFFORT，因此我们的 **reliable reader** 会报
   `reliability` 不匹配；我们的 BE reader 可以正常接收。
   验证时采用了两种办法：用 BE reader 接收 Fast DDS 默认 writer，或让
   Fast DDS writer 使用非默认 `max_blocking_time`（迫使它发送
   `PID_RELIABILITY`）再与 reliable reader 配对。
   如果要“零配置”兼容 Fast DDS 的默认 writer，可以考虑：对 Fast DDS
   vendor id（0x010f）的 publication，在 `PID_RELIABILITY` 缺失时按
   RELIABLE 处理；这是厂商兼容启发式，不是规范默认值，需要明确开关。
   **当前决策：按标准处理，缺失即 BEST_EFFORT，不加入 Fast DDS 启发式。**
   Fast DDS 应用若要和 reliable reader 互通，应显式设置非默认
   `max_blocking_time`（或显式 reliability QoS）让 SEDP 带上该 PID。
2. **两个 Fast DDS participant + 一个 nb participant 可以通过。** 关键是让
   Fast DDS publisher 用 `--matched 2` 等到两个 reader 都匹配后再发送；否则
   另一个 participant 还没发现完，BE 样本会丢，看起来像失败。nb 侧使用显式
   peer 端口 7410/7412/7414/7416，并把 unicast 端口设为 7900，避免和
   Fast DDS 的 7400 组播 socket 抢收包。这个用例验证了多 participant 场景下
   SEDP/数据路径可用；更强的多 participant 压力/丢包场景仍建议补测。
3. **TRANSIENT_LOCAL 晚加入两个方向都通过**，说明历史回放和
   HEARTBEAT/ACKNACK 驱动对 Fast DDS 有效。
4. **INFO_DST**：接收侧现在解析并按目标前缀过滤 Entity Submessage，全零
   前缀视为广播；单 participant、两 Fast DDS participant 以及 Cyclone
   用例都通过。
5. **XTypes/TypeObject**：Fast DDS 会通告 TypeInformation/TypeObject PID；
   我们忽略未知 PID。基础类型一致性（按 topic + type 名）下匹配成功；如果
   启用严格的 type consistency enforcement，可能因缺少类型对象而不匹配。
6. **协议版本**：Fast DDS 报文头观察到 2.2，我们发送 2.4；主版本相同，
   双方都接受。
7. **IPv6、content filter、security**：本次未覆盖，仍是第 1 节表格中的差距。

### 6.4 仍建议补测的场景

1. 多 participant 压力场景（多个 nb + 多个 Fast DDS，丢包/乱序/重启）；
2. Fast DDS 启用 type consistency enforcement / XTypes；
3. IPv6-only 与双栈；
4. 多 nb participant 共享一个 domain 的压力场景（标准端口已实现，验证多实例/丢包）；
5. 长时间可靠传输（序列号、心跳/ACKNACK 稳定性）。

补充：本次还验证了**同机两个 nb participant**（各自进程、同一 domain、默认
7400 端口、RELIABLE + TRANSIENT_LOCAL、16 B）可以互相发现并完成 TL 回放。
更复杂的多 nb 实例（不同 participant id/端口规划）仍建议按第 5 项补测。

### 6.5 Fast DDS 2.5.0 复验（Ubuntu 22.04 发行版包，2026-11-06）

为了确认结论不依赖 3.6.2 的源码构建，又在一台干净机器上使用发行版包复验：

- Fast DDS 2.5.0（`libfastrtps-dev` + `fastdds-tools`），Fast CDR 1.0.23，
  fastddsgen 2.1.0；Ubuntu 22.04 / GCC 11.4 / CMake 3.22；
- 单机 `192.168.1.22`，`FASTDDS_BUILTIN_TRANSPORTS=UDPv4`，并用带
  `interfaceWhiteList` 的 XML profile 把 Fast DDS 固定在网卡上；
- domain 0，topic `hello_world_topic`，type `HelloWorld`，CDR1 载荷。

第 6.2 节矩阵全部复现通过：双向 BE/可靠、16 B / 1 KB / 200 KB、
TRANSIENT_LOCAL 晚加入、publisher 重启、一个 Fast DDS participant + 一个
nb participant 的多 participant 场景、nb 标准端口映射（participant_id=10）
双向。2.x 与 3.x 的差异（生成文件名、无界 string 的 255 字节上限、
`write()` 重载、回环 peer）都是夹具/构建层面的，记录在
`tools/fastdds_interop/README.md`，不改变第 1 节的协议结论。

复验过程中发现并修复了一个**可靠 reader 大样本首包竞态**：

1. Fast DDS 写完一个 200 KB 样本后立即发 HEARTBEAT；分片的最后一包与
   HEARTBEAT 几乎同时到达。
2. nb 的 VOLATILE reliable reader 在处理 HEARTBEAT 时把窗口起点设为
   `last_sn + 1`（避免向 writer 索取匹配前的历史）。
3. 随后分片重组完成，该样本的序号已落在窗口起点之前，被当作“匹配前的
   样本”丢掉；reader 还会 ACK 掉这个序号，writer 不再重传。
4. 表现为每个大样本的第一包必丢、后续样本正常；小样本因为 DATA 先于
   HEARTBEAT 到达而不受影响。

修复：在 `handle_user_heartbeat()` 里用 `oldest_inflight_sn()` 检查该 writer
尚未完成的分片重组；如果存在，窗口起点不能越过它（`src/rtps/rtps_participant.c`）。
分片已经开始到达本身就证明样本在匹配之后，因此这不违反 VOLATILE 语义。
修复后 3 × 200 KB 连续样本全部按序收到，单元测试与 Cyclone 互通保持通过。
该竞态在 3.6.2 上没有触发，属于版本相关的时序暴露，而不是 2.5 独有的协议
缺陷。

## 7. 建议的优先级

| 优先级 | 事项 | 原因 |
| --- | --- | --- |
| P0（已完成） | Fast DDS 基础互通实测 | 双向、BE/可靠、1 KB/200 KB、TRANSIENT_LOCAL 晚加入、多 participant、重启均已通过 |
| P2 | 多 participant 压力测试 | 多个 nb + 多个 Fast DDS、丢包/乱序/重启 |
| P2 | 用户数据组播 d2 | 依赖标准端口推算/组播用户面的部署 |
| P2 | XTypes/TypeObject 最小支持 | 与启用类型一致性检查的 Fast DDS/Cyclone 互通 |
| P2 | IPv6 locator | 纯 IPv6 或双栈部署 |
| P3 | content filter、group ordered/coherent、security | 特定 DDS 特性，不是基础互通必需 |

**已决策**：`PID_RELIABILITY` 缺失按 DDS/RTPS 默认值当 BEST_EFFORT，不加入
“Fast DDS vendor 缺失即 RELIABLE”的启发式。Fast DDS 默认 writer 与 reliable
reader 互通时，由 Fast DDS 侧显式设置非默认 `max_blocking_time`（或显式
reliability QoS）来发布该 PID。

## 8. 参考

- OMG DDSI-RTPS 2.5，formal/22-04-01（本文对照的规范版本）。
- `docs/rtps-spec.md`：从代码反推的实现规范与已知偏差。
- `docs/rtps-protocol.html`：抓包/排错视角的大白话手册。
- `tests/test_rtps_cyclone.c`、`tools/verify-rtps-interop.sh`：Cyclone 互通入口。
- `tools/fastdds_interop/`：Fast DDS 互通验证夹具（本仓库侧 `rtps_*` 程序 +
  Fast DDS 显式 QoS 的 pub/sub，含构建和运行说明）。
