# RPC 与发布/订阅的分层拆分

本文记录一次结构性重构的决定：把当前的"一层抽象集成所有链路"改成
**两条并列的语义主干 + 一个共享底座**。目的不是加功能，而是**停止应用层接口
继续膨胀**。

## 1. 问题

最初的假设是：tcp / udp / shm / dds 都只搬运字节，所以可以用一个
provider SPI、一套 `service/proxy`、一个 `nb_qos_t` 统一起来。RPC 跑在这些
链路上，发布/订阅用 EVENT 表达。这个假设在 udp/tcp/shm 上成立，在 DDS 上不成立。

DDS 不是"一条能搬字节的链路"，它自己就是一套完整中间件：topic 与 type、
QoS 匹配、实例/键、状态事件、发现、历史与 read/take。把它当传输层塞进 RPC
抽象之后，DDS 真正独有的能力只能以"逃生舱"的形式挂到 RPC API 上，于是接口
按**并集**增长：

| 症状 | 位置 | 后果 |
| --- | --- | --- |
| QoS 是完整 DDS 策略集，却挂在所有 binding 上 | [qos.h](../include/nb/qos.h)、[config.h](../include/nb/config.h) | 用 TCP 的应用也要理解 partition / liveliness / ownership，而这些字段在 TCP 上是死字段 |
| DDS 样本元数据进了核心消息结构 | [message.h](../include/nb/message.h) `nb_sample_info_t sample` | 每条 RPC 消息都背实例句柄 / 视图状态 / generation rank |
| topic 不是一等公民，靠 "method 恰好是 topic 名"表达 | [subscribe.h](../include/nb/subscribe.h)、[publish.h](../include/nb/publish.h) | 同一个字符串在两种链路上含义不同；topic 没有类型、没有 QoS、没有生命周期 |
| DDS 历史/等待集/状态/发现视图抬到通用 service 层 | [sample.h](../include/nb/sample.h)、[waitset.h](../include/nb/waitset.h)、[status.h](../include/nb/status.h)、[discovery_view.h](../include/nb/discovery_view.h)、`nb_service_add_queue_method` | 纯 RPC 应用看到一堆无关 API |
| provider SPI 是一个 17 槽大杂烩 | [transport_provider.h](../include/nb/transport_provider.h) | send / reply / register_method / loan / discovery_read / metrics_read 混在一起，多数 provider 一半是 NULL |
| DDS-RPC 映射写了两份 | [cyclone_dds.c](../src/transport/dds/cyclone_dds.c) 与 [rtps_transport.c](../src/transport/rtps/rtps_transport.c) | 同一个 `<Service>_<method>_req/_res/_evt` 适配器维护两遍 |
| 内部设施出现在公共聚合头里 | [nb.h](../include/nb/nb.h) 导出了 poolset / slotpool / iobuf / poller_alloc | 公共面虚增约 25 个函数 |

`nb_` 当前约 217 个公共函数，`rtps_` 79 个，`ntk_discovery_` 20 个。其中
`include/rtps/` 已经是一个干净的、DDS 形状的公共 API（topic / writer /
reader / waitset）——**拆分所需的素材仓库里已经有了**。

## 2. 结论

要区分两个方向上的统一：

- **横向统一（同一语义、不同链路）是对的，保留。** RPC 底下可选
  tcp / udp / shm；发布/订阅底下可选 dds / rtps / shm。
- **纵向统一（不同语义）是错的，拆开。** RPC 和发布/订阅是两种通信模型，
  各有自己的对象、QoS、发现、状态和错误模型。它们只能共享更下面的底座。

```text
                    应用
   ┌───────────────────────┬───────────────────────────┐
   │ RPC 面                │ 发布/订阅面                │
   │ node / service        │ participant / topic       │
   │ proxy / call          │ writer / reader           │
   │ stream / cancel       │ sample / waitset          │
   │ nb_rpc_qos_t          │ nb_ps_qos_t（DDS 全集）    │
   └──────────┬────────────┴───────────┬───────────────┘
              │                        │
   ┌──────────▼──────────┐   ┌─────────▼──────────────┐
   │ RPC 链路（点对点）   │   │ PS 后端（topic）        │
   │ tcp / udp / shm     │   │ dds / rtps / shm       │
   └──────────┬──────────┘   └─────────┬──────────────┘
              │      ┌─────────────────┘
              │      │  RPC-over-PS 桥（DDS-RPC 映射，只写一份）
              ▼      ▼
   ┌──────────────────────────────────────────────────┐
   │ 共享底座：endpoint/URI、buf/alloc、poller、        │
   │ envelope/codec、discovery、router、log、metrics    │
   └──────────────────────────────────────────────────┘
```

要点：

1. **DDS 只做发布/订阅后端，不做 RPC 链路。** `dds://` / `rtps://`
   作为 binding 时，它属于 PS 面，暴露原生 topic 语义。
2. **RPC-over-DDS 是一个桥。** 需要在一套 PS 后端上跑 RPC 时，由一个适配器
   按 DDS-RPC 的方式把请求/响应映射成 req/res 主题对。这个适配器只写一份，
   架在任意 PS 后端上，而不是每个 DDS 实现各写一遍。
3. **`nb/nb.h` 降级为便利聚合头。** 新能力必须先决定归属主干，再落到
   `nb/rpc.h` 或 `nb/pubsub.h`；`nb.h` 只做 include 汇总，不作为新 API 的落点。

## 3. 两条主干的分工

| 维度 | RPC 面 | 发布/订阅面 |
| --- | --- | --- |
| 对象 | node → service/method → call | participant → topic → writer/reader |
| 寻址 | `service.method` + 目标 endpoint | topic 名 + type 名 |
| QoS | `nb_rpc_qos_t`：交付保证、超时、优先级、并发上限 | `nb_ps_qos_t`：DDS 全集 |
| 发现 | 服务名 → endpoint（HELLO / 静态路由） | 参与者/topic 匹配（SPDP/SEDP 或等价机制） |
| 状态 | 调用状态、取消、deadline 传播 | 匹配数、QoS 不兼容、deadline/liveliness、样本丢失 |
| 数据形态 | 一次一个请求/响应，字节 | 带实例与时间戳的样本流，可 read/take |
| 零拷贝 | 可选 loan（shm） | 原生 loan（shm）/ 数据共享（DDS） |
| 失败模型 | 每次调用一个状态码 | 匹配失败 / 样本丢失 / 资源拒绝 |

## 4. 硬规则

- **R1** 新的 DDS/发布订阅能力只能进 `nb/pubsub.h` 一族；新的调用能力只能进
  `nb/rpc.h` 一族。`nb.h` 不新增声明。
- **R2** 核心消息结构只放两种模型都需要的东西。DDS 样本元数据只能出现在
  PS 面的样本类型里。已完成：`nb_message_t` 只保留回调期 borrow 的
  `ps_sample` 指针，实例/视图状态不再内嵌。
- **R3** provider SPI 按**角色**拆分：RPC 链路实现 `nb_rpc_transport_ops`，
  PS 后端实现 `nb_ps_backend_ops`，两者只共享生命周期与 metrics。能力通过
  显式查询（loan / history / discovery / status）暴露，不靠 NULL 判断散布。
- **R4** topic 与 method 是两个类型，不是一个字符串的两种解释。
- **R5** DDS 实现之间不允许各写一份 RPC-over-DDS 映射；只允许一份适配器。
- **R6** 公共头不许暴露内部设施（poolset / slotpool / iobuf / poller_alloc）；
  它们移到 `include/nb/internal/`，不随安装导出。

## 5. 里程碑

当前状态：M1–M4、M6、M7 已完成；M5 完成了共用逻辑抽取（主题命名与封包），
完整的 "PS 后端适配器"合并待定。provider options 已按角色拆出 `ps` 子结构，
`nb_message_t` 的 DDS 样本元数据已改成 `ps_sample` 指针。

每个里程碑都要求：**可编译、原有测试全绿**，并尽量不破坏已有源码。

### M1 公共面分段（已完成）

- 新增 `include/nb/base.h`（模型中立）、`include/nb/rpc.h`、`include/nb/pubsub.h`。
- `include/nb/nb.h` 改为三者的并集，保持源码兼容。
- 内部设施迁到 `include/nb/internal/`，从 `nb.h` 与安装列表中移除。
- 新增契约测试：`nb/base.h` 不得包含任何主干头；两个主干头不得包含
  `nb/internal/*`。
- **验收**：示例/测试全绿；`nb.h` 的包含集合与拆分前一致（除内部设施）。

### M2 QoS 分裂（命名层完成）

- `nb_qos_t` 更名为 `nb_ps_qos_t`（`nb_qos_t` 保留为兼容 typedef），
  `nb_qos_*` 函数改为 `nb_ps_qos_*`，旧名在 `NB_NO_DEPRECATED_API` 下隐藏。
- 配置与 provider 结构体的字段类型改为 `nb_ps_qos_t`，布局不变。
- 尚未完成：把配置里的 QoS 拆成"RPC binding 的 `nb_rpc_qos_t`"与
  "PS binding 的 `nb_ps_qos_t`"两类。这一步依赖 M6 的配置分裂，因此合并到
  M6 一起做；在此之前 `bindings[].qos` 仍然只有 PS 后端会读取。
- **验收**：TCP binding 上不再出现 DDS 策略字段；DDS/RTPS 映射行为不变。

### M3 topic 一等公民（核心部分完成）

- 已新增 `nb_participant_t` / `nb_topic_t` / `nb_writer_t` / `nb_reader_t`
  （`include/nb/pubsub.h`，实现 `src/pubsub/pubsub.c`）。读者分两种：回调读者
  与 KEEP_LAST 历史读者（复用 `nb_sample_queue`，read/take）。
- 事件主题名已改用**应用给的 topic 名本身**，不再拼接
  `<Service>_<method>_evt`；RPC 桥的 `_req`/`_res` 保持原样。topic 名不允许
  包含 `.`，因此与 `service.method` 路由天然不冲突，入站 EVENT 先在主题登记
  表里查，未命中再回到服务分发。
- RTPS / Cyclone DDS 两个后端已实现，`tests/test_pubsub.c` 覆盖回调、历史
  read/take、线上主题名（"chat" 而非 "chat_evt"）以及参数校验。
- 等待集已能直接观察读者历史：`nb_waitset_attach_reader` /
  `nb_waitset_detach_reader`（回调读者没有历史，返回 `NB_ERR_INVALID`）。
- 服务方法上的读者历史（`nb_service_add_queue_method` / `nb_service_read` 等）
  已从 `include/nb/service.h` 移到 `include/nb/service_history.h`，该头只由
  `pubsub.h` 引入。`<nb/rpc.h>` 因此不再暴露 `nb_sample_t` / 队列类型；契约测试
  新增两条检查：`service.h` 不得包含 `sample.h`，`rpc.h` 不得引入 PS 主干头。
- 未完成：无。`nb_subscriber_*` / `nb_publisher_*` 与 service 上的读者历史已
  在 M7 删除。
- **验收**：发布/订阅不再依赖 `nb_service_create`；topic 名与 DDS 侧一致。

### M4 provider SPI 按角色拆分（完成）

- `nb_transport_provider_ops_t` 现在分成两层：`create/start/stop/destroy` 和
  可选的 `metrics_read` 是生命周期与观测；`rpc` 与 `ps` 是两条主干各自需要的
  角色接口，至少实现一个。`rpc` 角色是 `send/reply`（加可选 loan），`ps` 角色
  是 `register_topic`（必填）、`register_method/unregister_method`
  （RPC-over-PS 桥）和可选 `discovery_read`。ABI 版本升到 3。
- `dds` / `rtps` 实现 `rpc + ps`，`shm` 实现 `rpc + ps`；核心按角色取用，
  缺角色返回 `NB_ERR_NOT_SUPPORTED`。
- **验收**：每个 provider 的 ops 表只在属于自己的角色里出现；核心不再对
  "这个 provider 支不支持 loan / history / discovery" 做散落的 NULL 判断。

### M5 合并 DDS-RPC 适配器（第一步完成）

- 两个 DDS 形状 provider 共用的部分已抽到 `src/transport/common/dds_bridge.c`：
  路由/主题名规范化（`nb_bridge_topic_key`、`nb_bridge_route_topics`）和
  envelope 封包（`nb_bridge_frame_size` / `nb_bridge_frame_encode`）。
  `cyclone_dds.c` 与 `rtps_transport.c` 不再各写一份。
- 未完成：endpoint 表、writer/reader 生命周期和状态桥接仍在各自 provider 里。
  合并它们需要把"PS 后端"扩展成可创建 writer/reader 句柄的接口（目前只暴露
  `register_topic`），等价于用 `include/rtps/` 作为中立后端 API 再实现一份
  Cyclone 适配层。这一步改动面大、回归风险高，留作独立里程碑。
- **验收**：两个 DDS 后端共用同一份 RPC 映射；`tests/test_rtps_cyclone.c`
  与 `tools/fastdds_interop/` 的互通矩阵不回退。

### M6 配置按主干拆分（完成）

- 配置 JSON 按**用法**重新组织，字段名直接描述含义：`threads` / `limits` /
  `rpc`（mode、listen、udp、discovery、routes）/ `pubsub`（backends、
  default_qos）/ `metrics` / `log`。时长可以写 `"250ms"` / `"2s"`。
- `rpc.listen` 里的 URI 变成 RPC binding，`pubsub.backends` 里的 URI/对象变成
  发布/订阅 binding；同一个 URI 在两处出现时合并为一个传输（角色取并集），
  不再需要手写 `roles`。
- C 结构保持 `runtime + rpc + bindings + pubsub + metrics + log`；
  `nb_node_transport_config_t` 已被 `nb_rpc_config_t` 取代。`rpc.kind` /
  `bind_host` / `data_port` 只作为代码构造配置时的兜底 binding，不属于 JSON。
- `src/config/config.c` 只保留解析与公共 API；默认值/归一化/校验移到
  `config_defaults.c`。原来基于 codec_json 的 model/offset 解析被手写读取器
  取代，`src/utils/json/codec_json.*` 不再需要并已删除。
- **验收**：`config.schema.json`、`docs/api/config.md`、测试与示例同步。

### M7 删除兼容层（完成）

- 删除 `<nb/publish.h>`、`<nb/subscribe.h>`（publisher/subscriber 别名）和
  `<nb/service_history.h>`（service 方法上的 reader history）。
- 删除 `nb_qos_t` 与 `nb_qos_*` 兼容别名，只保留 `nb_ps_qos_t` /
  `nb_ps_qos_*`。
- `nb_waitset_attach` / `nb_waitset_detach`（service/method）删除，wait set
  只能挂载 `nb_reader_t`（`nb_waitset_attach_reader`）。
- 零拷贝发布改为 RPC 面的 `nb_proxy_loan` / `nb_proxy_publish_loan` /
  `nb_proxy_loan_release`（`<nb/proxy.h>`）；新增 `nb_reader_dropped`。
- **验收**：公共函数数明显下降；示例/测试全绿。

## 6. 兼容与版本

仓库已有 `NB_DEPRECATED` / `NB_NO_DEPRECATED_API` 机制（见
[types.h](../include/nb/types.h)），按它推进：

- **0.4**：M1（纯分段，无行为变化）。源码兼容。
- **0.5**：M2–M4，旧符号标记弃用但仍可用。
- **1.0**：M7，删除兼容层。

结构体布局本来就不承诺二进制兼容（见 `nb.h` 头注释），所以 M2/M3 的
结构变化可以接受；关键是**源码**在 0.x 期间保持可用。

## 7. 明确不做的事

- 不引入 C++、模板或代码生成；保持 C99。
- 不追求 rmw 式的"用一个模型套所有通信方式"——那正是本次要拆掉的东西。
- 不把 DDS 降级成纯字节链路；DDS 的 topic/type/QoS/发现必须原样可用。
- 不为了统一而统一 shm：`shm` 在 RPC 面是点对点队列，在 PS 面是 topic 扇出，
  两者共享 `shm_segment.c`，但语义分开实现。
