# 配置参考

Network Bridge 的配置按**主干**分成两个互不依赖的块：

| 块 | 内容 | 解析接口 |
| --- | --- | --- |
| `rpc` | node、传输（tcp/udp/shm）、发现、路由、并发上限 | `nb_rpc_config_parse_file` |
| `dds` | participant、topic、writer/reader、domain/partition/prefix/QoS | `nb_dds_config_parse_file` |

一个进程可以只解析自己用的块；同一个 JSON 文件同时包含 `rpc` 和 `dds` 时，每个解析器只读自己的块，另一个块静默忽略。`threads` / `limits` / `metrics` / `log` 是运行期公共项：RPC 解析器读取它们，DDS 解析器只使用 `name` 和 `dds`。

```c
nb_rpc_config_doc_t *rpc_cfg;  nb_config_error_t err;
nb_rpc_config_parse_file("app.json", &rpc_cfg, &err);
nb_node_create_from_config(rpc_cfg, &node);
nb_rpc_config_doc_destroy(rpc_cfg);

nb_dds_config_doc_t *dds_cfg;
nb_dds_config_parse_file("app.json", &dds_cfg, &err);
nb_dds_participant_create_from_config(dds_cfg, &participant);
nb_dds_config_doc_destroy(dds_cfg);
```

未知字段会被忽略并打印 `unknown config key ignored: <path>` 警告。字段类型错误、
数值越界或 URI 非法返回 `NB_ERR_INVALID`，error 输出参数给出精确的 `path`
（例如 `rpc.listen[0]`、`dds.qos.depth`）和 `reason`。**时长**写成数字（毫秒）
或带单位的字符串：`250ms`、`2s`。字符串枚举小写且区分大小写。

完整 JSON Schema 见 [`docs/config.schema.json`](../config.schema.json)，全量示例见
[`examples/04_config/full.json`](../../examples/04_config/full.json)。

## RPC 块

```json
{
  "threads": { "io": 2, "handlers": 4, "send_lanes": 0 },
  "limits": { "max_inflight": 65536, "max_streams": 64 },
  "rpc": {
    "mode": "static",
    "listen": ["tcp://0.0.0.0:42420"],
    "routes": [{ "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }]
  }
}
```

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `mode` | string | `"mesh"` | `mesh` 启用 nb_mesh 组播发现；`static` 关闭发现 |
| `listen` | array | 空 | 启动哪些 RPC 传输；scheme 选 tcp/udp/shm，最多 8 个 |
| `max_message_bytes` | uint | 1 MiB | 单条消息上限（最大 64 MiB） |
| `backpressure_bytes` | uint | 4 MiB | 出站高水位 |
| `routes` | array | 空 | 静态服务路由，优先于发现结果 |
| `discovery` | object | 见下文 | 组播 JOIN/HEARTBEAT/LEAVE + 定向单播目标 |

**`listen` 是“启动哪些传输”，不是服务端专用。** 客户端也要列出至少一个端点
（端口写 `0` 让系统分配），出站调用才能找到对应 scheme 的传输。`listen` 的 URI
scheme 决定传输；同一 scheme 有多个 binding 时出站固定用第一个。

`rpc.udp`：`fragment_bytes`（默认 1440，或 256..8192）、`rtcp_interval`（默认
`"1s"`）、`rtcp`（默认 true）。

`rpc.discovery`：

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `group` | string | 缺省 | `host:port`。缺省时由 `formation` 推导组播组（`239.255.x.y:41953`）；显式给地址则按该地址收发，地址不是组播组时退化为定向单播目标（本机回环测试常用 `127.0.0.1:<port>`） |
| `port` | uint | 41953 | 发现端口（`group` 里写了端口则以其为准） |
| `formation` | uint | 0 | 编队/任务组 ID：形成报文里的 `formation_id`，不同编队互不可见（编队隔离） |
| `service_mask` | uint | 0 | 本节点核心服务位图（32 位）。实际通告的掩码 = 该值 ∪ 已注册服务折算出的位，编译期位定义见 `include/nb/mesh_services.h` |
| `heartbeat_interval` | duration | `"1s"` | 心跳周期；上电先按 50 ms 间隔连发 3 次 JOIN |
| `lease` | duration | `"1500ms"` | 对端租约：超过该时长没有心跳即剔除（必须大于心跳周期，库会自动兜底修正） |
| `peers` | array | 空 | 定向单播发现目标（`host:port`，最多 32 个），组播不可用或跨网段时使用 |

发现只回答"谁提供了哪项服务能力"，调用时按 `routes` → registry → 邻居表的顺序
解析；报文格式、状态机与容量测算见 [`../mesh-discovery.md`](../mesh-discovery.md)。

`threads`：`io`（poller 线程，0 = 硬件并发，最大 8）、`handlers`（worker 线程，
0 = 关闭）、`send_lanes`（专用发送线程，最大 64）。

`limits`：`max_inflight`（默认 65536）、`max_streams`（最大 64）、
`handler_queue`（未完成 worker/executor 任务上限）、`peer_rate_per_second`。

## DDS 块

```json
{
  "dds": {
    "domain": 0,
    "partition": "orders",
    "prefix": "app",
    "qos": { "reliability": "reliable", "history": "keep_last", "depth": 8 }
  }
}
```

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `domain` | uint | 0 | DDS domain id |
| `partition` | string | 空 | DDS partition，作为默认 QoS 继承 |
| `prefix` | string | 空 | topic 名前缀，线上名 = `prefix/name` |
| `name` | string | 顶层 `name` | participant 名称 |
| `qos` | object | DDS 默认 | topic/endpoint 的默认 QoS |
| `rtps` | object | 引擎默认 | RTPS 后端调优 |

DDS 面**不依赖** RPC node、binding、路由或 nb_mesh：内建 RTPS 后端自己做
SPDP/SEDP 发现。DDS 默认值为 `best_effort` / `volatile` / `keep_last depth 1`，
topic/writer/reader 的 QoS 逐级继承；线上格式与 Cyclone DDS / Fast DDS 互通。

`dds.rtps` 字段：`bind_host`、`advertise_host`、`multicast_group`（空字符串关闭
组播）、`spdp_port`、`unicast_port`、`announce_interval`、`lease`、
`data_heartbeat`、`data_history_depth`、`tcp`、`tcp_port`、`standard_ports`、
`participant_id`、`peers`（单播 SPDP 目标 `host:port`，最多 32 个）。单播部署时
两边都配置对方的 `peers`。

### QoS

`qos` 与 DDS 策略同名。缺省字段继承 `participant.default_qos`，两者都没给时使用
后端默认值。字段：`reliability`、`durability`、`history`、`depth`、`deadline`、
`lifespan`、`latency_budget`、`min_separation`、`liveliness`、`lease_duration`、
`ownership`、`partition`、`max_samples`、`max_instances`、
`max_samples_per_instance`、`autodispose_unregistered`、`presentation_scope`、
`coherent_access`、`ordered_access`。

## metrics / log

`metrics`（`enabled`、`latency_histogram`、`sample_every`）和 `log`（`enabled`、
`level`）是进程级 RPC 运行期开关；多 node 时最后创建的 node 生效。

## 常见错误

| 错误路径 | 原因 |
| --- | --- |
| `name` | 不是字符串，或过长 |
| `threads.io` / `threads.handlers` / `threads.send_lanes` | 类型错误或超出上限 |
| `limits.max_streams` | 超过 64 |
| `rpc.mode` | 不是 `mesh` / `static` |
| `rpc.listen[i]` | 不是字符串或 URI 非法 |
| `rpc.routes[i].endpoint` | 缺少 service 或 URI 非法 |
| `rpc.discovery.peers[i]` | 不是合法 `host:port` |
| `dds.qos.<field>` | 类型或取值非法 |
| `dds.rtps.peers[i]` | 不是合法 `host:port` |
| `log.level` | 不支持的级别 |
