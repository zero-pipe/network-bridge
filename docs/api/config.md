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

`nb_config_doc_t` 是由 `nb_config_parse*` 分配的不透明句柄，拥有解析出的字符串。
不要按值拷贝它，也不要在栈上构造。`nb_node_create_from_config` 会复制 node
需要的字段，因此文档只需活到该调用返回。已知字段类型错误、数值越界或 URI
非法会返回 `NB_ERR_INVALID`；如果传入 error 输出参数，它会报告精确的 `path`
（例如 `runtime.io_threads`、`rpc.routes[0].endpoint`、
`bindings[1].ps_qos.depth`）和 `reason`。

为了向前兼容，未知字段会被忽略，但 `nb_config_parse` 会为每个未知字段打印一条
`unknown config key ignored: <path>` 警告，避免拼写错误悄悄生效。

字符串枚举值（`rpc.mode`、`rpc.kind`、`intra_process`、binding `roles`、
`log.level`、QoS 值）均为小写且区分大小写。

完整 JSON Schema 见 [`docs/config.schema.json`](../config.schema.json)。

## 最小服务端

```json
{
  "name": "echo-server",
  "runtime": {
    "io_threads": 2
  },
  "rpc": {
    "mode": "static"
  },
  "bindings": ["tcp://127.0.0.1:42420"]
}
```

## 最小客户端

```json
{
  "name": "echo-client",
  "rpc": {
    "mode": "static",
    "routes": [
      { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
    ]
  },
  "bindings": ["tcp://127.0.0.1:0"]
}
```

## 最小发布/订阅进程

```json
{
  "name": "ticker",
  "bindings": [
    { "name": "dds", "endpoint": "dds://0", "roles": ["ps"],
      "ps_qos": { "reliability": "reliable", "history": "keep_last", "depth": 8 } }
  ],
  "pubsub": {
    "default_qos": { "durability": "transient_local" }
  }
}
```

## 顶层字段

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `name` | string | `"nb"` | node 名称，最多 `NB_NAME_MAX - 1` 个字符 |
| `runtime` | object | 见下文 | 线程、并发和限速（两条主干共享） |
| `rpc` | object | 见下文 | RPC 面：链路调优、发现、路由 |
| `bindings` | array | 空 | 传输实例，两条主干共享，最多 8 个 |
| `pubsub` | object | 见下文 | 发布/订阅面：默认 QoS |
| `metrics` | object | 见下文 | 指标采集（共享） |
| `log` | object | 见下文 | 日志（共享） |

## runtime

| 字段 | 类型 | 默认值 | 范围 | 说明 |
| --- | --- | --- | ---: | --- |
| `io_threads` | uint | 硬件并发数 | `0..8` | poller 线程数，上限受 `NB_TRANSPORT_MAX_IO` 约束 |
| `handler_threads` | uint | `0` | `0..1024` | WORKER handler 线程数；`0` 关闭线程池 |
| `handler_max_concurrency` | uint | `0` | `0..N` | 未完成的 worker/executor 任务上限；`0` 表示不限制 |
| `max_inflight` | uint | `65536` | `0..N` | 客户端在途调用上限 |
| `max_streams` | uint | `64` | `0..64` | 并发入站流数量 |
| `peer_rate_limit_pps` | uint | `0` | `0..N` | 每个对端每秒请求数；`0` 表示不限制 |
| `send_lanes` | uint | `0` | `0..64` | 与接收/分发 poller 隔离的专用发送线程；`0` 关闭 |

`send_lanes > 0` 时，通过 `nb_node_post_send()` 提交发布循环；循环内的
`nb_proxy_call_event()` 走内联快路径，每个事件省掉一次跨线程入队和唤醒。
总线程数为 `io_threads + send_lanes`，因此可以把核从 `io_threads` 挪到
`send_lanes`。

没有 `handler_threads` 时，WORKER 方法会降级为 INLINE 并告警。超过
`handler_max_concurrency` 时，新请求会按调用路径返回 `NB_ERR_OVERLOAD` 或
`NB_ERR_AGAIN`。

## bindings

一个 binding 就是 node 启动一次的一个传输实例。两条主干从同一份列表里按角色
取用，因此同一个 `dds://` 连接不会因为 RPC 和发布/订阅各写一遍而创建两次。

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `name` | string | URI scheme | binding 名称，最多 `NB_SCHEME_MAX - 1` 个字符 |
| `endpoint` | string | - | `scheme://host[:port][/path]` |
| `roles` | array | provider 默认 | 该 binding 服务的面：`"rpc"` / `"ps"`；缺省或空表示 provider 支持的全部角色 |
| `intra_process` | string | `"off"` | `off` / `auto` / `only`，DDS 事件发布的进程内快路径 |
| `poll_us` | uint | `0` | shm provider 的空闲自旋窗口（微秒） |
| `ps_qos` | object | 继承 | 发布/订阅 QoS，字段见下文 |

元素有两种形式：

```json
{
  "bindings": [
    "tcp://127.0.0.1:42420",
    { "name": "udp", "endpoint": "udp://0.0.0.0:42421", "roles": ["rpc"] },
    { "name": "dds", "endpoint": "dds://0", "roles": ["ps"],
      "ps_qos": { "reliability": "best_effort" } }
  ]
}
```

字符串形式根据 URI scheme 选择 UDP/TCP，角色取 provider 默认值。对象形式中
`name` 默认取 scheme。端口 `0` 表示由系统选择临时端口。

`roles` 会做校验：`udp` / `tcp` 只实现 RPC 角色；只实现发布/订阅的后端不能
声明 `"rpc"`。角色不匹配时 `nb_node_create` 返回 `NB_ERR_NOT_SUPPORTED`。

Cyclone DDS 使用 `dds://<domain>`，host 是 DDS domain，端口可以省略；共享内存
使用 `shm://<channel>:<node-id>`，mesh 模式下同一 channel 上的节点必须使用不同
的 node id（`1..9`）。

`intra_process = "only"` 不创建 DDS 事件 writer/reader，直接把消息指针投递给
同一 provider 的本地订阅者；`auto` 本地投递的同时仍走 DDS。

`poll_us` 仅 `shm` provider 使用：排空本地队列后，先自旋轮询这么久再回退到
阻塞等待。运行期可用环境变量 `NB_SHM_POLL_US` 覆盖每个 binding 的值。

### QoS

`ps_qos`（以及 `pubsub.default_qos`）与 Cyclone DDS 的同名策略对应。值为 `0`
或字段缺省表示“继承 provider 默认值”；只有显式配置的字段才会写入。TCP/UDP 的
RPC 链路不读取这些字段。

| 字段 | 取值 | DDS 策略 |
| --- | --- | --- |
| `reliability` | `best_effort` / `reliable` | ReliabilityQosPolicy |
| `durability` | `volatile` / `transient_local` | DurabilityQosPolicy |
| `history` | `keep_last` / `keep_all` | HistoryQosPolicy |
| `depth` | uint | history depth |
| `deadline_ms` | uint | DeadlineQosPolicy |
| `lifespan_ms` | uint | LifespanQosPolicy |
| `latency_budget_ms` | uint | LatencyBudgetQosPolicy |
| `min_separation_ms` | uint | TimeBasedFilterQosPolicy（订阅端丢弃间隔更近的样本） |
| `liveliness` | `automatic` / `manual_by_participant` / `manual_by_topic` | LivelinessQosPolicy |
| `lease_duration_ms` | uint | liveliness lease duration |
| `ownership` | `shared` / `exclusive` | OwnershipQosPolicy |
| `partition` | string（最多 127） | PartitionQosPolicy |
| `max_samples` | uint | ResourceLimitsQosPolicy，0 = 不限制 |
| `max_instances` | uint | ResourceLimitsQosPolicy，0 = 内建上限；超过上限的样本会被订阅端拒绝 |
| `max_samples_per_instance` | uint | ResourceLimitsQosPolicy，0 = 不限制 |
| `autodispose_unregistered_instances` | bool | WriterDataLifecycleQosPolicy |
| `presentation_scope` | `instance` / `topic` / `group` | PresentationQosPolicy 访问范围（RxO：writer 至少要提供 reader 的范围） |
| `coherent_access` | bool | PresentationQosPolicy coherent_access（通告并检查兼容性） |
| `ordered_access` | bool | PresentationQosPolicy ordered_access（通告并检查兼容性） |

数值字段必须是整数；`1.5` 这类值会被拒绝，而不是截断。

## rpc

RPC 面独占的配置。只做发布/订阅的进程可以省略整个 `rpc` 对象。

| 字段 | 类型 | 默认值 | 范围 | 说明 |
| --- | --- | --- | --- | --- |
| `mode` | string | `"mesh"` | `mesh` / `static` | `static` 关闭服务发现 |
| `kind` | string | `"udp"` | `udp` / `tcp` | 默认传输，仅在未配置 `bindings` 时使用 |
| `bind_host` | string | `"0.0.0.0"` | - | 默认绑定地址 |
| `data_port` | uint | `0` | `0..65535` | 默认数据端口；`0` 自动分配 |
| `frag_bytes` | uint | `1440` | `0` 或 `256..8192` | UDP 分片大小 |
| `max_msg_bytes` | uint | `1048576` | `0..67108864` | 单条消息上限；不得小于 `frag_bytes` |
| `backpressure_bytes` | uint | `4194304` | `0..N` | 出站高水位 |
| `rtcp_interval_ms` | uint | `1000` | `0..N` | UDP RTCP 报告间隔 |
| `disable_rtcp` | bool | `false` | - | `true` 关闭 RTCP |
| `discovery` | object | 见下文 | - | 服务发现 |
| `routes` | array | 空 | 最多 `NB_CONFIG_MAX_ROUTES` | 静态路由 |

### rpc.discovery

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `broadcast` | string | `"255.255.255.255:41953"` | 广播/发现地址，`host:port` 形式 |
| `peers` | array | 空 | 额外的单播 HELLO 目标，`"host:port"`，最多 32 个 |
| `hello_interval_ms` | uint | `3000` | HELLO 周期 |
| `node_ttl_ms` | uint | `9000` | 对端过期时间 |

`rpc.mode = "static"` 会关闭发现；`nb_node_config_normalize` 会把 `mode` 与
内部的 `discovery.disable` 收敛到一致状态。MESH 通常要求双方使用相同的发现
端口，或互相配置为单播 `peers`。

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

静态路由跳过发现。最多接受 `NB_CONFIG_MAX_ROUTES` 条；超出返回
`NB_ERR_INVALID`。

## pubsub

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `default_qos` | object | 继承 | binding 的 `ps_qos` 为 INHERIT 时使用，字段见 QoS |

发布/订阅的 participant、topic、writer/reader 由 `nb_participant_*` /
`nb_topic_*` / `nb_writer_*` / `nb_reader_*` 在运行期创建；配置只负责把底层
传输启动起来并给出默认 QoS。

## metrics

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `disable` | bool | `false` | 关闭所有指标采集 |
| `disable_latency_histogram` | bool | `false` | 只关闭延迟直方图 |
| `sample_every` | uint | `1` | `1` 表示全采样；`N` 表示每第 N 个事件采样 |

关闭指标后，热路径只剩一次全局开关判断，不再做原子累加。

`metrics` 和 `log` 是进程级开关：`nb_node_create` 会把它们应用到共享的
metrics/log 运行时，因此多 node 时最后创建的 node 生效，销毁 node 不会恢复
之前的值。日志可以在运行期用 `nb_log_configure` 修改；指标开关只在创建 node
时生效。

## log

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `disable` | bool | `false` | 关闭日志输出 |
| `level` | string | `"info"` | `assert` / `error` / `warn` / `info` / `debug` / `verbose` |

`nb_log_configure(enabled, level)` 也可以在运行期修改开关和级别。

在代码中构造配置时，`log.level = NB_LOG_LEVEL_DEFAULT` 表示“未指定”，会
归一化为 `info`；越界值也会归一化为 `info`（`nb_node_config_validate` 则会
直接报错）。

## 默认值与归一化

在代码中构造 `nb_node_config_t` 时，先调用 `nb_node_config_default`，或者在
创建 node 前调用 `nb_node_config_normalize`。`nb_node_create` 会对配置副本
做归一化。

`nb_node_config_validate` 是严格版本：既不裁剪也不填默认值，字段越界时返回
`NB_ERR_INVALID` / `NB_ERR_TOOBIG`。

`nb_rpc_config_t` 带有 `routes`/`route_count` 和 `peers`/`peer_count`；
`nb_node_create` 会在返回前应用它们。任何 route 或 peer 注册失败，都会销毁
构造到一半的 node 并返回错误，而不是静默忽略。

职责划分：

- `nb_config_parse` 对用户输入做**严格校验**，超出上述范围返回
  `NB_ERR_INVALID`。
- `nb_node_config_normalize` 服务于代码构造的配置：填默认值，并把越界值
  **裁剪**到合法范围。

归一化规则：

- `name == NULL` 使用 `"nb"`。
- `rpc.bind_host == NULL` 使用 `"0.0.0.0"`。
- `rpc.discovery.broadcast_host == NULL` 使用 `"255.255.255.255"`。
- `rpc.mode` 和 `rpc.discovery.disable` 收敛：任一关闭发现，两者都置为关闭。
- `runtime.io_threads == 0` 使用硬件并发数，裁剪到 `1..NB_TRANSPORT_MAX_IO`。
- `runtime.handler_threads` 裁剪到 `0..NB_MAX_HANDLER_THREADS`。
- `runtime.send_lanes` 裁剪到 `0..NB_MAX_SEND_LANES`。
- `rpc.frag_bytes == 0` 使用 `1440`；否则裁剪到 `256..8192`。
- `rpc.max_msg_bytes == 0` 使用 `1 MiB`；裁剪到 `frag_bytes..64 MiB`。
- `rpc.backpressure_bytes == 0` 使用 `4 MiB`。
- `rpc.discovery.port == 0` 使用 `41953`。
- `metrics.sample_every == 0` 使用 `1`。

## 常见错误

| 错误路径 | 原因 |
| --- | --- |
| `name` | 不是字符串，或过长 |
| `rpc.mode` | 不是 `mesh` / `static` |
| `runtime.io_threads` | 超过 `NB_TRANSPORT_MAX_IO`（8） |
| `runtime.handler_threads` | 超过 `NB_MAX_HANDLER_THREADS`（1024） |
| `runtime.send_lanes` | 超过 `NB_MAX_SEND_LANES`（64） |
| `runtime.max_streams` | 超过 `NB_STREAM_MAX_RECV_STREAMS`（64） |
| `rpc.kind` | 不是 `udp` / `tcp` |
| `rpc.frag_bytes` | 既不是 `0` 也不在 `256..8192` 内 |
| `rpc.max_msg_bytes` | 超过 64 MiB 或小于 `frag_bytes` |
| `bindings[i].endpoint` | 元素不是字符串/对象、URI 非法，或数量过多 |
| `bindings[i].roles` | 不是 `"rpc"` / `"ps"` 数组 |
| `bindings[i].intra_process` | 不是 `off` / `auto` / `only` |
| `bindings[i].ps_qos.<field>` | QoS 字段类型或取值非法 |
| `rpc.discovery.broadcast` | 不是 `host:port` |
| `rpc.discovery.peers[i]` | 不是合法的 `host:port` |
| `rpc.routes[i].service` | 缺失或过长 |
| `rpc.routes[i].endpoint` | 缺失、URI 非法或过长 |
| `pubsub.default_qos.<field>` | QoS 字段类型或取值非法 |
| `log.level` | 不支持的级别 |

`nb_config_parse_file` 对超过 `NB_CONFIG_MAX_FILE_BYTES`（1 MiB）的文件返回
`NB_ERR_INVALID`，避免路径写错时把巨大文件读进内存。
