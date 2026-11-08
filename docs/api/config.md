# 配置参考

Network Bridge 使用 JSON 配置，可以来自文件，也可以来自内存字符串：

```c
nb_config_doc_t *cfg = NULL;
nb_config_error_t err;

int rc = nb_config_parse_file("server.json", &cfg, &err);
if (rc != NB_OK) {
    printf("path=%s reason=%s\n", err.path, err.reason);
    return rc;
}

rc = nb_node_create_from_config(cfg, &node);
if (rc == NB_OK) {
    const nb_node_config_t *parsed = nb_config_node(cfg);  /* 只读视图 */
    (void)parsed;
}

nb_config_doc_destroy(cfg);  /* node 已经复制所需字段，可以立即释放 */
```

相关接口：

- `nb_config_parse` / `nb_config_parse_file`（第三个 `nb_config_error_t *`
  参数可以为 NULL）
- `nb_node_create_from_config`
- `nb_config_node`（只读 `nb_node_config_t` 视图，用于检查解析结果）
- `nb_config_doc_destroy`（释放文档，必须与 `nb_config_parse*` 配对）

配置按**用法**分组，字段名直接描述含义：

| 分块 | 内容 |
| --- | --- |
| `name` | node 名称 |
| `threads` | 线程数 |
| `limits` | 并发与速率上限 |
| `rpc` | RPC：监听端点、发现、路由、链路调优 |
| `pubsub` | 发布/订阅：后端端点、默认 QoS |
| `metrics` / `log` | 运行期开关 |

未知字段会被忽略，并打印 `unknown config key ignored: <path>` 警告。字段类型
错误、数值越界或 URI 非法返回 `NB_ERR_INVALID`，error 输出参数给出精确的
`path`（例如 `rpc.listen[0]`、`pubsub.default_qos.depth`）和 `reason`。

字符串枚举值（`mode`、`intra_process`、`log.level`、QoS 值）均为小写且区分
大小写。**时长**写成数字（毫秒）或带单位的字符串：`250ms`、`2s`。

完整 JSON Schema 见 [`docs/config.schema.json`](../config.schema.json)。

## 最小服务端

```json
{
  "name": "echo-server",
  "threads": { "io": 2 },
  "rpc": {
    "mode": "static",
    "listen": ["tcp://0.0.0.0:42420"]
  }
}
```

## 最小客户端

```json
{
  "name": "echo-client",
  "threads": { "io": 1 },
  "rpc": {
    "mode": "static",
    "listen": ["tcp://127.0.0.1:0"],
    "routes": [
      { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
    ]
  }
}
```

`listen` 里的 `:0` 表示由系统分配端口；客户端只需要一个出站绑定。

## 最小发布/订阅进程

```json
{
  "name": "ticker",
  "threads": { "io": 1 },
  "pubsub": {
    "backends": [
      { "endpoint": "dds://0", "qos": { "reliability": "reliable" } }
    ],
    "default_qos": { "durability": "transient_local" }
  }
}
```

同一个端点同时出现在 `rpc.listen` 和 `pubsub.backends` 时（例如 RPC 跑在 DDS
上、同时又有发布/订阅），只启动一个传输，两个面共用。

## threads

| 字段 | 类型 | 默认值 | 范围 | 说明 |
| --- | --- | --- | ---: | --- |
| `io` | uint | 硬件并发数 | `0..8` | poller 线程数 |
| `handlers` | uint | `0` | `0..1024` | WORKER handler 线程数；`0` 关闭线程池 |
| `send_lanes` | uint | `0` | `0..64` | 与接收/分发 poller 隔离的专用发送线程 |

`send_lanes > 0` 时，通过 `nb_node_post_send()` 提交发布循环；循环内的
`nb_proxy_call_event()` 走内联快路径。总线程数为 `io + send_lanes`。

没有 `handlers` 时，WORKER 方法会降级为 INLINE 并告警。

## limits

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `max_inflight` | uint | `65536` | 客户端在途调用上限 |
| `max_streams` | uint | `64` | 并发入站流上限（最大 64） |
| `handler_queue` | uint | `0`（不限） | 未完成的 worker/executor 任务上限 |
| `peer_rate_per_second` | uint | `0`（不限） | 每个对端的入站请求速率上限 |

超过 `handler_queue` 时，新请求按调用路径返回 `NB_ERR_OVERLOAD` 或
`NB_ERR_AGAIN`。

## rpc

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `mode` | string | `"mesh"` | `mesh` 启用服务发现；`static` 关闭 |
| `listen` | array | 空 | 监听端点 URI 列表，元素是字符串 |
| `max_message_bytes` | uint | `1048576` | 单条消息上限（最大 64 MiB） |
| `backpressure_bytes` | uint | `4194304` | 出站高水位 |
| `udp` | object | 见下文 | UDP 专用调优 |
| `discovery` | object | 见下文 | 服务发现 |
| `routes` | array | 空 | 静态路由 |

### rpc.udp

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `fragment_bytes` | uint | `1440` | UDP 分片大小；`0` 用默认值，否则 `256..8192` |
| `rtcp_interval` | duration | `"1s"` | RTCP 报告间隔 |
| `rtcp` | bool | `true` | `false` 关闭 RTCP |

### rpc.discovery

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `broadcast` | string | `"255.255.255.255:41953"` | 广播/发现地址，`host:port` |
| `peers` | array | 空 | 额外的单播 HELLO 目标，`"host:port"`，最多 32 个 |
| `hello_interval` | duration | `"3s"` | HELLO 周期 |
| `node_ttl` | duration | `"9s"` | 对端过期时间 |

`mode = "static"` 关闭发现；`nb_node_config_normalize` 会把 `mode` 与内部的
`discovery.disable` 收敛到一致状态。MESH 通常要求双方使用相同的发现端口，或
互相配置为单播 `peers`。

### rpc.routes

```json
{
  "rpc": {
    "routes": [
      { "service": "OrderService", "endpoint": "tcp://127.0.0.1:42420" },
      { "service": "EchoService", "endpoint": "udp://127.0.0.1:42421" }
    ]
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | :---: | --- |
| `service` | string | 是 | 目标服务名 |
| `endpoint` | string | 是 | `scheme://host:port` |

静态路由跳过发现。最多 `NB_CONFIG_MAX_ROUTES` 条。

## pubsub

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `backends` | array | 空 | 发布/订阅后端，元素是 URI 字符串或对象 |
| `default_qos` | object | 继承 | binding 没写 `qos` 时使用的默认 QoS |

`backends` 对象形式：

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `endpoint` | string | 必填 | `dds://<domain>`、`rtps://<domain>`、`shm://<channel>:<id>` |
| `qos` | object | 继承 | 该后端的 QoS，字段见下文 |
| `intra_process` | string | `"off"` | `off` / `auto` / `only`，DDS 事件进程内快路径 |
| `poll_us` | uint | `0` | shm 排空队列后的自旋窗口（微秒） |

```json
{
  "pubsub": {
    "backends": ["dds://0", { "endpoint": "shm://orders:1", "poll_us": 50 }],
    "default_qos": { "reliability": "reliable", "history": "keep_last", "depth": 8 }
  }
}
```

participant、topic、writer/reader 由 `nb_participant_*` / `nb_topic_*` /
`nb_writer_*` / `nb_reader_*` 在运行期创建；配置只负责把后端启动起来并给出
默认 QoS。

### QoS

`qos` 与 Cyclone DDS 的同名策略对应。值为 `0` 或缺省表示“继承 provider 默认
值”。TCP/UDP 的 RPC 链路不读取这些字段。

| 字段 | 取值 | DDS 策略 |
| --- | --- | --- |
| `reliability` | `best_effort` / `reliable` | ReliabilityQosPolicy |
| `durability` | `volatile` / `transient_local` | DurabilityQosPolicy |
| `history` | `keep_last` / `keep_all` | HistoryQosPolicy |
| `depth` | uint | history depth |
| `deadline` | duration | DeadlineQosPolicy |
| `lifespan` | duration | LifespanQosPolicy |
| `latency_budget` | duration | LatencyBudgetQosPolicy |
| `min_separation` | duration | TimeBasedFilterQosPolicy |
| `liveliness` | `automatic` / `manual_by_participant` / `manual_by_topic` | LivelinessQosPolicy |
| `lease_duration` | duration | liveliness lease duration |
| `ownership` | `shared` / `exclusive` | OwnershipQosPolicy |
| `partition` | string（最多 127） | PartitionQosPolicy |
| `max_samples` | uint | ResourceLimitsQosPolicy，0 = 不限制 |
| `max_instances` | uint | ResourceLimitsQosPolicy，0 = 内建上限 |
| `max_samples_per_instance` | uint | ResourceLimitsQosPolicy，0 = 不限制 |
| `autodispose_unregistered` | bool | WriterDataLifecycleQosPolicy |
| `presentation_scope` | `instance` / `topic` / `group` | PresentationQosPolicy 访问范围 |
| `coherent_access` | bool | PresentationQosPolicy coherent_access |
| `ordered_access` | bool | PresentationQosPolicy ordered_access |

## metrics

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | 关闭所有指标采集 |
| `latency_histogram` | bool | `true` | 是否采集延迟直方图 |
| `sample_every` | uint | `1` | `1` 表示全采样；`N` 表示每第 N 个事件采样 |

## log

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | 关闭日志输出 |
| `level` | string | `"info"` | `assert` / `error` / `warn` / `info` / `debug` / `verbose` |

`metrics` 和 `log` 是进程级开关：`nb_node_create` 会把它们应用到共享的
metrics/log 运行时，多 node 时最后创建的 node 生效。日志可以在运行期用
`nb_log_configure` 修改；指标开关只在创建 node 时生效。

## 默认值与归一化

在代码中构造 `nb_node_config_t` 时，先调用 `nb_node_config_default`，或者在
创建 node 前调用 `nb_node_config_normalize`。`nb_node_create` 会对配置副本
做归一化。

`nb_node_config_validate` 是严格版本：既不裁剪也不填默认值，字段越界时返回
`NB_ERR_INVALID` / `NB_ERR_TOOBIG`。

归一化规则：

- `name` 为空使用 `"nb"`。
- `rpc.bind_host` 为空使用 `"0.0.0.0"`；`rpc.discovery.broadcast_host` 为空
  使用 `"255.255.255.255"`。
- `rpc.mode` 和 `rpc.discovery.disable` 收敛。
- `threads.io == 0` 使用硬件并发数，裁剪到 `1..8`。
- `threads.handlers` 裁剪到 `0..1024`，`threads.send_lanes` 裁剪到 `0..64`。
- `fragment_bytes == 0` 使用 `1440`；`max_message_bytes` 裁剪到
  `fragment_bytes..64 MiB`；`backpressure_bytes == 0` 使用 `4 MiB`。
- discovery 端口/周期、`max_inflight`、`max_streams`、`sample_every` 填默认值。
- `log.level` 的 `NB_LOG_LEVEL_DEFAULT` 归一化为 `info`。

## 常见错误

| 错误路径 | 原因 |
| --- | --- |
| `name` | 不是字符串，或过长 |
| `threads.io` / `threads.handlers` / `threads.send_lanes` | 不是非负整数，或超出上限 |
| `limits.max_streams` | 超过 64 |
| `rpc.mode` | 不是 `mesh` / `static` |
| `rpc.listen[i]` | 不是字符串、URI 非法，或数量过多 |
| `rpc.max_message_bytes` | 超过 64 MiB |
| `rpc.udp.fragment_bytes` | 既不是 `0` 也不在 `256..8192` 内 |
| `rpc.udp.rtcp_interval` | 不是合法时长 |
| `rpc.discovery.broadcast` | 不是 `host:port` |
| `rpc.discovery.peers[i]` | 不是合法 `host:port` |
| `rpc.routes[i].service` / `.endpoint` | 缺失、过长或 URI 非法 |
| `pubsub.backends[i]` | 不是字符串/对象，或 URI 非法 |
| `pubsub.backends[i].qos.<field>` | QoS 字段类型或取值非法 |
| `pubsub.default_qos.<field>` | QoS 字段类型或取值非法 |
| `metrics.enabled` / `log.enabled` | 不是布尔值 |
| `log.level` | 不支持的级别 |

`nb_config_parse_file` 对超过 `NB_CONFIG_MAX_FILE_BYTES`（1 MiB）的文件返回
`NB_ERR_INVALID`。
