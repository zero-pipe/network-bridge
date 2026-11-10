# 架构

Network Bridge 由**两条并列的语义主干 + 一个共享底座**组成。两条主干在对象模型、
发现、QoS、失败模型和公共头/库目标上都相互独立；它们只共享最底层的工具。

```text
应用
├── RPC 主干                          ├── DDS 主干
│   node / service / proxy            │   participant / topic / writer / reader
│   call / stream / cancel            │   sample / history / wait set / status
│   nb_rpc_qos_t (超时、并发)          │   nb_dds_qos_t (DDS 策略全集)
│   tcp / udp / shm 链路              │   rtps / cyclone 后端
│   static routes / ntk_discovery     │   SPDP/SEDP（后端自己的发现）
└──────────────┬──────────────────────┴──────────────┬──────────────┘
               ▼                                     ▼
        共享底座：buf/alloc、错误码、日志、指标、payload 视图、JSON 读取器
```

## RPC 主干

- **对象**：`nb_node_t`（线程/传输/发现/路由）→ `nb_service_t`（方法）→
  `nb_proxy_t`（调用）；流与取消挂在 call/stream 上。
- **传输**：`rpc.listen` 里的 URI scheme 选择 tcp、udp 或 shm；provider SPI 只
  描述点对点链路（send/reply/loan/subscribe），没有 topic/QoS 概念。
- **发现**：`mode: "mesh"` 使用广播式 `ntk_discovery`；`mode: "static"` 只用
  静态路由。`routes` 优先于发现结果，二者可共存。
- **失败模型**：每次调用一个状态码；超时、取消、重试、流控属于 RPC 面。

## DDS 主干

- **对象**：`nb_dds_participant_t`（domain + backend + 默认 QoS）→
  `nb_dds_topic_t`（name + type + QoS）→ `nb_dds_writer_t` / `nb_dds_reader_t`。
- **核心字段**：`domain`、`partition`、`prefix`（topic 名前缀）、`qos`。QoS 从
  participant 默认值 → topic → writer/reader 逐级继承；`present` 位掩码区分
  “显式 false”和“继承”。
- **后端**：`NB_DDS_BACKEND_RTPS` 直接映射内建 RTPS 引擎；`NB_DDS_BACKEND_CYCLONE`
  映射 Cyclone DDS。两者使用同一套 DDS 默认值与线上类型 `dds::Message`，可互通。
- **history / wait set**：由 DDS 模块统一实现（KEEP_LAST、read/take、状态掩码），
  后端只回调样本，避免两个后端语义分叉。
- **样本与信封**：应用写什么字节，reader 读什么字节；不再套 RPC 信封。
- **发现**：后端自己跑 SPDP/SEDP，不依赖 `ntk_discovery`，也不需要 RPC node。

## 共享底座

缓冲区与分配器、错误码、日志、指标、payload/owned-payload、sample info 类型、
JSON 读取器和配置错误结构。底座头文件不得包含 `nb/rpc/*` 或 `nb/dds/*`；
两个主干的头文件也不得互相包含。`tests/check_nb_headers.cmake` 与
`tests/check_spine_symbols.cmake` 在 CI 中强制这些规则。

## 库目标

| 目标 | 内容 | 依赖 |
| --- | --- | --- |
| `nb_base` | 缓冲区、错误、日志、指标、payload | ntk |
| `nb_json` | 共享 JSON 读取器 + cJSON | `nb_base` |
| `nb_rpc` | RPC 主干 + tcp/udp/shm + 配置 | `nb_base`、`nb_json`、`ntk_discovery` |
| `nb_dds` | DDS 主干 + rtps/cyclone 后端 + 配置 | `nb_base`、`nb_json`、`rtps`、可选 `ddsc` |
| `nb` | 便利目标，链接 `nb_rpc` + `nb_dds` | 两条主干 |

`nb_rpc` 不链接 `rtps`/`ddsc`，`nb_dds` 不链接 `ntk_discovery`；两条主干可以从
链接层独立裁剪。

## 配置

JSON 顶层按主干分块：`threads` / `limits` / `rpc` / `dds` / `metrics` / `log`。
`nb_rpc_config_parse_file` 只读 RPC 相关块，`nb_dds_config_parse_file` 只读
`name` 与 `dds` 块；同一个文件可以同时包含两块。字段表见
[`api/config.md`](api/config.md)。
