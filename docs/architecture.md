# 架构与抽象

Network Bridge 把**调用语义**和**链路实现**分开。上层只描述
`service.method + payload + metadata`；下层只搬运一条归一化的消息。寻址、QoS、
分片和回复令牌都由共享的框架抽象承载。

## 分层

```text
应用
  service / proxy / stream              调用形态与生命周期
  codec / message_view                  载荷编解码与视图
核心
  node / dispatch / pending / dedup     关联、超时、重试、调度
  router / registry / discovery         服务解析与 mesh 控制面
传输 SPI
  endpoint + message + QoS              与传输无关的契约
provider
  udp / tcp / shm / dds / rtps / 自定义  具体链路实现
```

依赖只能向下。传输 provider 看不到 service/proxy 概念，也不解析业务载荷；
核心层从不依赖某个 scheme 私有的地址格式。

## Endpoint（端点）

`nb_endpoint_t` 是唯一的地址表示，字段固定：

| 字段 | 含义 |
| --- | --- |
| `scheme` | provider 选择器，例如 `udp` / `tcp` / `shm` / `dds` / `rtps` |
| `host` | provider authority：IP、DDS/RTPS domain 或 SHM channel |
| `port` | provider 内的服务标识：UDP/TCP 端口、SHM node id、RTPS SPDP 端口、DDS 占位 |
| `path` | 可选的 provider 子路径 |
| `transport` | 内建传输类型；自定义 provider 使用 `NB_TRANSPORT_KIND_CUSTOM` |
| `uri` | 归一化 URI，供路由、发现、日志和跨进程传输使用 |

不变量：

- 路由表、registry、发现帧、proxy 目标覆盖和 provider 发送目标都使用
  `nb_endpoint_t`，不再各自保存 `ip/scheme/port` 三元组。
- `nb_endpoint_parse` 负责归一化；字段修改后由 `nb_endpoint_refresh`
  重建 URI。
- 只有 UDP/TCP 的 authority 是网络地址。发现收到 HELLO 时，只对
  `nb_endpoint_is_network()` 为真的 endpoint 用报文源 IP 改写 host；
  SHM channel 和 DDS domain 必须原样保留。

## Message（消息）

`nb_message_t` 既是核心内部消息，也是公共 provider SPI 消息
（`include/nb/message.h`）。过去 provider 还有一份独立结构，
`transport_factory.c` 逐字段拷贝，新增字段很容易漏掉；现在只有一份定义：

- 入站：provider 填 `peer`、route、payload、metadata 和 stream；
  `frame_owner` 可以延长零拷贝缓冲区的生命周期。
- `sample` 携带 DDS 样本元数据：样本状态、实例句柄和 writer 源时间戳。
  RTPS provider 会填；其它 provider 保持为零，读起来就是普通数据，因为
  `NB_SAMPLE_STATE_VALID` 本身就是 0。
- 出站：核心填 `target`、route 和 payload；UDP/TCP 走派生的
  `peer_host/peer_port` 快路径。
- `reply_token` 是 provider 私有的回复通道，只用于回复。

## QoS（服务质量）

`nb_qos_t`（`include/nb/qos.h`）对齐 DDS 策略；字段默认为 INHERIT，因此不会
覆盖 provider 默认值。当前映射：

- reliability / durability / history / depth
- deadline / lifespan / latency budget
- liveliness / lease duration / ownership / partition

DDS provider 在创建 reader/writer 时应用 binding QoS。UDP/TCP/SHM 保留配置
供应用读取，但不改变消息语义。没有显式 QoS 的 binding 继承 `transport.qos`。

## 状态事件

`nb_status_event_t`（`include/nb/status.h`）是 DDS 状态契约中面向应用的那一半。
能观察到状态的 provider 填写
`nb_transport_provider_options_t.on_status`，在自己的线程上报，`user` 与
`on_receive` 相同：

- `NB_STATUS_PUBLICATION_MATCHED` / `NB_STATUS_SUBSCRIPTION_MATCHED`：`count`
  是当前匹配数，下降即 DDS 的“对端消失”信号；`total_count` 是累计计数。
- `NB_STATUS_QOS_INCOMPATIBLE`：topic 和 type 相同但无法匹配；`reason`
  指出具体策略。
- `NB_STATUS_OFFERED_DEADLINE_MISSED` / `NB_STATUS_REQUESTED_DEADLINE_MISSED`
  和 `NB_STATUS_LIVELINESS_CHANGED`。

`endpoint` 是 provider 对本地端点的命名；RTPS provider 使用端点所属的方法名。
状态不会作为框架消息上线：它由拥有数据面的 provider 产生，因此 UDP/TCP/SHM
根本不设置这个回调。

## Mesh 与 SHM

mesh 控制面仍通过 UDP HELLO/BYE 传播服务和 binding。HELLO v4 携带每个
binding 的完整 URI，而不是 `scheme + port`，因此 SHM channel、DDS domain
这类 provider authority 可以完整往返。

SHM 段每个节点一条入站队列：

- 每个节点以 `shm://<channel>:<node-id>` 加入，`node-id` 取 `1..9`。
- 任意节点都可以向任意目标节点投递 RPC 响应或事件；没有固定的
  server/client 角色。
- 旧的 `push_request/push_response` API 保留为兼容包装：request 表示
  “发给节点 1”，response 表示“发给指定客户端”。
- 事件按 route 扇出到订阅节点；chunk 引用计数维持零拷贝。
- 第一个创建者初始化共享段，最后一个关闭者 unlink。

这样就不需要专门的 “SHM server”：每个 mesh 节点都是对等节点，但同一
channel 内 node id 必须唯一。

## 能力矩阵

| 能力 | UDP | TCP | SHM | DDS |
| --- | :-: | :-: | :-: | :-: |
| 一元 RPC | 是 | 是 | 是 | 是 |
| 事件 / 发布订阅 | 是 | 是 | 是 | 是 |
| 流式 | 是 | 是 | 是 | 是 |
| mesh 发现 | 是 | 是 | 是 | 是 |
| 零拷贝接收 | 否 | 否 | 是 | loan 期间 |
| QoS | 保留 | 保留 | 保留 | 映射 |

## 边界

- TLS/DTLS 和认证由自定义 provider 或 service interceptor 负责。
- DDS loan 的跨线程所有权转移，需要把 loan 生命周期延长到异步消费者；
  worker 路径目前仍在核心层做深拷贝。
- 异常退出后的 SHM 段恢复依赖最后一个关闭者，或重建空段；跨主机部署应使用
  UDP/TCP/DDS。
