# 架构

Network Bridge 由**三层结构 + 两条并列的语义主干**组成。最底层是 ntk 与 base 共享底座，其上是 RPC 消息编解码层（codec），再上层是两条相互独立的语义主干：RPC 主干与 DDS 主干。两条主干在对象模型、发现、QoS、失败模型和公共头/库目标上都相互独立；它们只共享最底层的工具与 codec 层。

```text
                    应用
           ┌─────────────────────────────────┐
           │  RPC 主干                        │ DDS 主干                        │
           │  node / service / proxy         │ participant / topic / writer / reader│
           │  call / stream / cancel         │ sample / history / wait set / status │
           │  nb_rpc_qos_t (超时、并发)      │ nb_dds_qos_t (DDS 策略全集)        │
           │  tcp / udp / shm 链路           │ 内建 rtps 后端                   │
           │  static routes / nb_mesh        │ SPDP/SEDP（后端自己的发现）      │
           └──────────────┬──────────────────┴──────────────┬─────────────────┘
                          │                                   │
                          ▼                                   ▼
                   ┌─────────────────────────────────────────────────────┐
                   │  RPC 消息编解码层（codec，tcp / udp 共用）           │
                   │  nb_message_t（应用层统一消息：payload / metadata /  │
                   │  status / route / timeout / stream）                │
                   │  nb_envelope_t / nb_frame_t（wire frame v2，固定头 + │
                   │  TLV 扩展 + payload）                               │
                   └──────────────┬─────────────────────────────────────┘
                                  │
                                  ▼
                   ┌─────────────────────────────────────────────────────┐
                   │  共享底座：buf / alloc、错误码、日志、指标、        │
                   │  payload 视图、JSON 读取器、ntk（socket/poller/线程/锁）│
                   └─────────────────────────────────────────────────────┘
```

## 分层说明

### 第 0 层：ntk（network-tool-kit）

socket、poller、timer、thread、mutex、monotonic clock。所有 I/O 与线程原语都通过 ntk 抽象，上层不直接调用平台 socket API。

### 第 1 层：base（共享底座）

`include/nb/base/` + `src/base/`：缓冲区（buf/iobuf）、分配器（alloc/poolset/slotpool）、错误码、日志、指标、payload 视图、sample info、JSON 读取器（cJSON 封装）。

**规则**：`include/nb/base/*` 不得包含 `nb/rpc/*` 或 `nb/dds/*`；`src/base/*` 不得依赖 rpc/dds/rtps。

### 第 2 层：codec（RPC 消息编解码，tcp / udp 共用）

`include/nb/rpc/message.h`、`include/nb/rpc/message_view.h`、`include/nb/rpc/codec.h`；
`src/rpc/message/envelope.c/frame.c`。

这一层把“应用层消息”（`nb_message_t`，字段：kind/flags/status/request_id/timeout_ms/route/metadata/status_msg/stream/payload）与“wire 字节流”（frame v2：固定头 28 字节 + TLV 扩展 + payload）之间进行双向编解码。

- `nb_envelope_encode_prefix()` / `nb_envelope_decode()` / `nb_envelope_to_message()`：核心编解码入口。
- `nb_frame_write_header()` / `nb_frame_decode()`：固定头 + CRC32 校验，扩展按 TLV 迭代解析，未知 tag 跳过（前后兼容）。
- 数据帧（DATA）：REQUEST / RESPONSE / EVENT；控制帧（CONTROL）：CANCEL / GOAWAY / PING。

**规则**：tcp / udp 传输驱动只负责“把编码后的字节流发出去 / 把收到的字节流解码成 envelope”，不再各自实现一套 envelope 拼装。`nb_message_t` 是跨 transport/core 边界的唯一消息上下文。

### 第 3 层：RPC 主干

`include/nb/rpc/` + `src/rpc/`：node / service / proxy / call / stream / cancel；transport provider SPI（tcp/udp/shm）；发现（nb_mesh 组播心跳 / static 静态路由）；路由与注册表。

- **对象**：`nb_node_t`（线程/传输/发现/路由）→ `nb_service_t`（方法）→ `nb_proxy_t`（调用）；流与取消挂在 call/stream 上。
- **传输**：`rpc.listen` 里的 URI scheme 选择 tcp、udp 或 shm；provider SPI 只描述点对点链路（send/reply/loan/subscribe），没有 topic/QoS 概念。
- **发现**：`mode: "mesh"` 用 `nb_mesh` 的定长组播心跳（`include/nb/mesh.h`，编队隔离 + 服务位图 + 哈希邻居表）；`mode: "static"` 只用静态路由。解析顺序是 `routes` → registry → 邻居表掩码寻址，三者可共存。细节见 [`mesh-discovery.md`](mesh-discovery.md)。
- **失败模型**：每次调用一个状态码；超时、取消、重试、流控属于 RPC 面。
- **库依赖**：`nb_rpc` 链接 `nb_base`、`nb_json`、`nb_mesh`；不链接 `rtps`。

### 第 4 层：DDS 主干

`include/nb/dds/` + `src/dds/`：participant / topic / writer / reader；sample / history / wait set / status；QoS 继承（participant → topic → writer/reader）。

- **核心字段**：`domain`、`partition`、`prefix`（topic 名前缀）、`qos`。QoS 从 participant 默认值 → topic → writer/reader 逐级继承；`present` 位掩码区分“显式 false”和“继承”。
- **引擎**：内建 RTPS 引擎（唯一 DDS 引擎，无后端选择）。它只把 topic/writer/reader 映射到引擎上；默认线上类型是 `dds::Message`（不透明字节），topic 传真实 IDL 类型名时按裸 CDR body 发送，可与 Cyclone DDS / Fast DDS 互通。
- **history / wait set**：由 DDS 模块统一实现（KEEP_LAST、read/take、状态掩码），后端只回调样本，语义与后端解耦。
- **样本与信封**：应用写什么字节，reader 读什么字节；不再套 RPC 信封。
- **发现**：后端自己跑 SPDP/SEDP，不依赖 `nb_mesh`，也不需要 RPC node。
- **库依赖**：`nb_dds` 链接 `nb_base`、`nb_json`、`rtps`；不链接 `nb_mesh`。

### 第 5 层：RTPS 引擎（内建，供 DDS RTPS 后端使用）

`include/rtps/`（公共 API，`rtps_*` 前缀，不含 `nb_*` 类型）+ `src/rtps/`（协议引擎，内部头在 `src/rtps/internal/`，不安装）。

- **消息报文**：RTPS header（20 字节）+ 子消息（DATA / DATA_FRAG / HEARTBEAT / ACKNACK / NACK_FRAG / GAP / INFO_TS / INFO_SRC / INFO_DST / HEARTBEAT_FRAG 等），CDR 小端编码，4 字节对齐。
- **可靠传输**：writer 历史（bounded，FIFO 淘汰最老样本）、reader 接收窗口、HEARTBEAT → ACKNACK → 缺失样本重传、GAP 通告被淘汰样本、DATA_FRAG + HEARTBEAT_FRAG + NACK_FRAG 分片重传。
- **发现**：SPDP（参与者宣布）/ SEDP（端点宣布），远端租约与 PMD（ParticipantMessageData）存活性。
- **TCP**：RTPS-over-TCP 可选，SPDP 通告 TCP locator，数据面随对端走流。

## 库目标

| 目标 | 内容 | 依赖 |
| --- | --- | --- |
| `nb_base` | 缓冲区、错误、日志、指标、payload | ntk |
| `nb_json` | 共享 JSON 读取器 + cJSON | `nb_base` |
| `nb_rpc` | RPC 主干 + tcp/udp/shm + 配置 | `nb_base`、`nb_json`、`nb_mesh` |
| `nb_dds` | DDS 主干 + 内建 rtps 后端 + 配置 | `nb_base`、`nb_json`、`rtps` |
| `nb` | 便利目标，链接 `nb_rpc` + `nb_dds` | 两条主干 |
| `rtps_core` | RTPS 协议引擎（内部，不安装公共头） | ntk、micro-cdr |
| `rtps` | RTPS 公共 API (`rtps_*`) | `rtps_core` |

`nb_rpc` 不链接 `rtps`，`nb_dds` 不链接 `nb_mesh`；两条主干可以从链接层独立裁剪。

## 配置

JSON 顶层按主干分块：`threads` / `limits` / `rpc` / `dds` / `metrics` / `log`。
`nb_rpc_config_parse_file` 只读 RPC 相关块，`nb_dds_config_parse_file` 只读 `name` 与 `dds` 块；同一个文件可以同时包含两块。字段表见 [`api/config.md`](api/config.md)。

## 头依赖边界（CI 强制）

- `include/nb/base/*`：不得包含 `nb/rpc/*` 或 `nb/dds/*`。
- `include/nb/rpc/*`：不得包含 `nb/dds/*`。
- `include/nb/dds/*`：不得包含 `nb/rpc/*`。
- `include/rtps/*`：不得出现 `nb_*` 类型或符号；所有 `RTPS_API` 声明必须以 `rtps_*` 命名。
- `include/nb/internal/*`：不安装，公共头不得包含。
- `tests/check_nb_headers.cmake`、`tests/check_spine_symbols.cmake`、`tests/check_rtps_headers.cmake`、`tests/check_rtps_provider.cmake`、`tests/check_rtps_symbols.cmake` 在 CI 中强制上述规则。

## 契约测试

| 测试 | 检查点 |
| --- | --- |
| `contract.nb_headers` | 公共 nb 头不暴露内部 stub 层；base/rpc/dds 头不跨互 import；`nb.h` 重导出所有 spine |
| `contract.rpc_symbols` | `libnb_rpc` 不导出 `rtps_*` / `dds_*` 符号 |
| `contract.dds_symbols` | `libnb_dds` 不导出 `nb_node_*` / `nb_service_*` / `nb_proxy_*` / `nb_mesh_*` 符号 |
| `contract.rtps_headers` | `include/rtps/*` 无 `nb_*`；`RTPS_API` 声明均以 `rtps_*` 命名 |
| `contract.rtps_provider_api` | RTPS transport provider 仅使用公共 `<rtps/rtps.h>` API，不触碰引擎内部符号 |
| `contract.rtps_symbols` | `librtps` 只导出 `rtps_*` 符号（有 `nm` 时） |
