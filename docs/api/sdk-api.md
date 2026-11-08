# Network Bridge SDK API

Network Bridge 是一个异步 RPC 与消息运行时。公共 API 只暴露 node、service、
proxy 和字节视图；载荷序列化由应用通过 codec 决定。

## 入口头文件

完整 API：

```c
#include <nb/nb.h>
```

如果只需要最小集合，就包含实际用到的头文件：

| 头文件 | 用途 |
| --- | --- |
| `<nb/node.h>` | node、调度、发现、路由、指标 |
| `<nb/service.h>` | service、handler、reply、message view |
| `<nb/proxy.h>` | proxy、异步/同步/事件/流式调用 |
| `<nb/publish.h>`、`<nb/subscribe.h>` | topic、sample、reader |
| `<nb/endpoint.h>` | endpoint 解析与自定义传输 provider |
| `<nb/codec.h>` | codec、message view、service table |
| `<nb/config.h>` | JSON 配置加载与 node 创建 |

`<nb/error.h>`、`<nb/types.h>`、`<nb/metrics.h>` 和 `<nb/log.h>` 由上述入口
按需包含。公共头文件只存在于 `<nb/...>` 下，且只存在于 `include/nb/`，因此
构建树里可用的 include 路径在安装后同样可用。中间没有聚合头：
`<nb/service.h>` 就是 service API，不是一组角色头文件的打包。

## 对象模型

```text
node
  ├─ service   注册方法、接收请求并回复
  └─ proxy     发现/路由并发送调用：
               call_async / call_sync / call_event
```

一个 node 可以同时承担客户端和服务端角色。proxy 借用 node 的 poller 和路由表，
自己不创建线程。传输按 binding 选择；自定义传输通过
`<nb/transport_provider.h>` 接入。

node 是运行时/participant 句柄（poller 池、binding、发现），不是 ROS 2 的
node：它拥有线程和 socket，因此应用通常在整个生命周期内只创建一个。进程内
仍支持多个 node，用于进程内测试和多租户场景。

推荐的创建/销毁顺序：

```text
nb_node_create
  -> nb_service_create / nb_proxy_create
  -> nb_node_run 或应用自己的事件循环
  -> nb_service_destroy / nb_proxy_destroy
  -> nb_node_destroy
```

## 线程模型

- 回调和 `NB_SERVICE_EXEC_MODE_INLINE` handler 默认在 NTK poller 线程上执行，
  不能阻塞。
- `NB_SERVICE_EXEC_MODE_WORKER` 在 node 的 `runtime.handler_threads` 线程池上
  执行。
- `NB_SERVICE_EXEC_MODE_EXECUTOR` 在调用方提供的 executor 上执行。
- `nb_proxy_call_async` / `nb_proxy_call_event` 可以在任意线程调用。
- `nb_proxy_call_sync` 会阻塞，不能在 poller 回调里使用。
- worker/executor handler 里的 `nb_reply` 会投递回原 poller 发送。

## 生命周期与所有权

### 载荷（Payload）

`nb_payload_t` 只是 `{data, len, encoding}` 视图，框架从不解释内容。

发送时，`nb_proxy_call_async` 和 `nb_proxy_call_event` 会在返回前复制载荷。
调用方始终拥有自己的缓冲区，可以立即释放。

接收时，请求和响应视图只在回调内有效。需要保留就自行复制。

### 托管载荷 API（owned payload）

要避免一次完整复制，可使用 `nb_owned_payload_t` 和 `*_owned` 变体：

```c
nb_buf_t *buf = nb_buf_alloc(12);
memcpy(nb_buf_data(buf), "hello", 5);
nb_owned_payload_t request;
nb_owned_payload_init(&request, buf, 0, 5, 0);

nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.timeout_ms = 1000;
rc = nb_proxy_call_async_owned(proxy, "Svc", "method", &request, &options,
                               on_response, NULL);
/* 返回后 request.buf 为 NULL；即使失败，框架也已接管或释放。 */
```

- `nb_proxy_call_async_owned` / `nb_proxy_call_event_owned`：客户端请求；
  metadata 通过 options 传递。
- `nb_reply_owned`：服务端回复。

Owned API 在入口处接管 `payload->buf`。旧的 `nb_payload_t` API 仍是
“借用 + 复制”语义，没有变化。proxy 层的 metadata 一律使用
`nb_call_options_t`。缓冲区由 `nb_base` 的 `nb_buf_*` 管理；单所有者路径默认
不做原子引用计数。

### 同步调用

```c
nb_response_t response = {0};
nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.timeout_ms = 3000;
int rc = nb_proxy_call_sync(proxy, "OrderService", "get", &request, &options,
                            &response);
/* 使用 response.payload */
nb_response_release(&response);
```

`nb_response_release` 释放同步调用写入的响应 metadata 和 payload（当
`owns_payload == true` 时），并把结构清零。回调作用域的响应
`owns_payload == false`；释放它们不会碰应用缓冲区。

### 消息视图（message view）

`nb_message_view_t` 供同步风格 handler 和自行组装载荷的应用使用：

```c
nb_message_view_t view;
nb_message_view_init(&view);
nb_message_view_set_string(&view, "ok");
nb_message_view_release(&view);
```

`set_borrowed` 不接管外部内存；`set_data`、`set_string` 和 `set_payload`
会复制，内存由 view 拥有。

## Node（运行时）

```c
int nb_node_create(const nb_node_config_t *cfg, nb_node_t **out);
void nb_node_destroy(nb_node_t *node);
```

node 创建会装配传输、poller、发现和路由。查询接口：

- `nb_node_name`
- `nb_node_transport`
- `nb_node_data_port`
- `nb_node_discovery_port`
- `nb_node_binding_count` / `nb_node_binding_endpoint`
- `nb_node_metrics` / `nb_node_peer_stats`

运行与路由：

- `nb_node_add_discovery_peer`
- `nb_node_add_static_route` / `nb_node_add_static_route_uri`
- `nb_node_wait_for_service`
- `nb_node_post` / `nb_node_post_delay`
- `nb_node_run_once`（运行一轮，不安装信号处理器；适合嵌入宿主应用）
- `nb_node_run_until_signal`（安装 SIGINT/SIGTERM 并运行到收到信号；
  `nb_node_run` 是兼容别名）

配置字段见[配置参考](config.md)。

## Service（服务端）

异步 handler：

```c
typedef void (*nb_handler_fn)(nb_call_t *call, const nb_request_t *req, void *user);
```

同步风格 handler：

```c
typedef int (*nb_sync_handler_fn)(const nb_request_t *req, nb_message_view_t *resp, void *user);
```

同步 handler 的返回值会成为响应里的应用状态。异步 handler 必须且只能调用一次
`nb_reply`、`nb_reply_with_metadata` 或 `nb_reply_stream`。

每个 `nb_reply*` 都返回 `int`：`NB_OK` 表示回复已被接受准备发送；重复回复返回
`NB_ERR_ALREADY`；状态消息超过 `NB_STATUS_MSG_MAX` 或 metadata 超过
`NB_METADATA_WIRE_MAX` 时返回 `NB_ERR_TOOBIG`，而不是静默截断。状态消息里
需要保留内嵌 NUL 字节时，使用
`nb_reply_status(call, &nb_status_t{code, message, message_len}, ...)`。

注册：

```c
nb_service_t *svc = NULL;
nb_service_create(node, "OrderService", &svc);
nb_service_add_method(svc, "get", on_get, user);
nb_service_add_sync_method(svc, "put", on_put_sync, user);
```

`*_with_options` 变体通过 `nb_service_method_options_t` 选择 INLINE、WORKER 或
EXECUTOR。WORKER 要求 `runtime.handler_threads > 0`；否则方法会降级为 INLINE
并告警。

流式：

- `nb_service_add_stream_method` / `_with_options`
- `nb_reply_stream`
- 请求的最后一个 chunk 必须带 `NB_STREAM_FLAG_LAST`

拦截与取消：

- `nb_service_set_interceptor` 在 handler 前运行；返回非 `NB_OK` 会短路回复。
- `nb_call_deadline_ms` 读取客户端传播过来的绝对 deadline。
- `nb_call_request_id` 读取请求关联 id，用于日志和追踪。
- `nb_call_canceled` 支持 handler 协作式退出。

### Reader history（read/take）与 wait set

订阅可以把样本投递给回调（`nb_subscriber_subscribe`），也可以保存在 reader 侧
history 中：

```c
#include <nb/subscribe.h>

nb_subscriber_t *sub = NULL;
nb_subscriber_create(node, "TradeService", &sub);
nb_subscriber_subscribe_queue(sub, "ticks", 16); /* KEEP_LAST 深度 16 */

nb_sample_t samples[8];
nb_sample_filter_t filter = NB_SAMPLE_FILTER_ANY;
size_t n = nb_subscriber_read(sub, "ticks", &filter, samples, 8);
for (size_t i = 0; i < n; i++) {
    /* samples[i].data 是借用视图：有效到样本被 take 为止 */
    use(samples[i].data.data, samples[i].data.len, &samples[i].info);
}

n = nb_subscriber_take(sub, "ticks", &filter, samples, 8);
for (size_t i = 0; i < n; i++) {
    use(samples[i].data.data, samples[i].data.len, &samples[i].info);
    nb_sample_release(&samples[i]); /* take() 返回的样本归调用方所有 */
}
```

语义：

- history 是 DDS 的 `KEEP_LAST(depth)`；depth 为 0 时使用默认值 32。溢出时
  丢弃最旧的样本，并计入 `nb_service_queue_dropped`。
- `read()` 把返回的样本标记为 READ 但保留，返回借用视图（loan 路径）；
  `take()` 移除样本并返回拥有所有权的副本。两者都先应用掩码。
- 掩码组合 DDS 的三个维度
  （`NB_STATE_SAMPLE_READ`/`_NOT_READ`、`NB_STATE_VIEW_NEW`/`_NOT_NEW`、
  `NB_STATE_INSTANCE_ALIVE`/`_DISPOSED`/`_NO_WRITERS`）。某个维度只有设置了
  对应位才约束结果；`0` 表示 `NB_STATE_ANY`，惯例默认值是
  `NB_STATE_ANY_DATA_READER`。
- `nb_sample_filter_t.instance` 用 16 字节句柄选择某个 DDS 实例；`NULL`
  表示任意。没有句柄的样本永远不会匹配非 NULL 句柄。

wait set 可以阻塞等待一个或多个 history：

```c
#include <nb/waitset.h>

nb_waitset_t *ws = NULL;
nb_waitset_create(node, &ws);
nb_waitset_attach(ws, sub, "ticks");

if (nb_waitset_wait(ws, &filter, 500) == 1) {
    n = nb_subscriber_take(sub, "ticks", &filter, samples, 8);
}
```

`nb_waitset_wait` 在挂载的 history 有匹配样本时返回 1，超时返回 0，其它情况
返回负错误码。唤醒是事件驱动的：每次入队都会 post 一个许可，因此阻塞的等待者
无需轮询延迟即可返回。`NB_WAITSET_INFINITE` 表示永久等待。wait set 只报告
就绪；随后仍需从具体订阅中取走数据。

## Proxy（客户端）

普通调用：

```c
nb_proxy_t *proxy = NULL;
nb_proxy_create(node, NULL, &proxy);

nb_payload_t request = { "hello", 5, 0 };
nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.timeout_ms = 1000;

nb_proxy_call_async(proxy, "EchoService", "echo", &request, &options,
                    on_response, user);
```

所有调用形式共享 `nb_call_options_t`：

```c
typedef struct nb_call_options {
    uint64_t timeout_ms;              /* 0 = 默认值；事件忽略；打开流时作为流的 deadline */
    const nb_metadata_t *metadata;    /* 请求 metadata；NULL = 无，返回前复制 */
    nb_request_id_t *out_request_id;  /* 可选；返回 id 供 nb_proxy_cancel 使用 */
} nb_call_options_t;
```

`options` 传 `NULL` 表示全部使用默认值。也可以先用 `NB_CALL_OPTIONS_INIT`
初始化，再覆盖个别字段。

三种调用形式：

- `nb_proxy_call_async`：等待响应，回调调用一次。
- `nb_proxy_call_sync`：阻塞，写入 `nb_response_t`。
- `nb_proxy_call_event`：单向事件，无响应。

Metadata 通过 `nb_call_options_t.metadata` 放在固定容量容器里传递；二进制值
可以安全传输。

流式调用：

- `nb_proxy_call_server_stream`：普通请求，响应分块返回。
- `nb_proxy_open_client_stream` / `nb_proxy_open_bidi_stream`：打开客户端请求流。
- `nb_proxy_stream_send` / `nb_proxy_stream_cancel` / `nb_proxy_stream_destroy`。
- 打开流只使用 options 里的 `timeout_ms`；每个 chunk 的 metadata 放在
  `nb_chunk_t.metadata` 中。

取消：

```c
nb_request_id_t request_id = 0;
nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.out_request_id = &request_id;
nb_proxy_call_async(proxy, "Svc", "method", &request, &options, on_response, user);
nb_proxy_cancel(proxy, request_id);
```

取消是尽力而为的：框架发送 `KIND_CANCEL`，并在本地以 `NB_ERR_NOTFOUND`
结束调用；已经运行的 handler 不会被抢占。

除内建的 UDP/TCP/SHM/RTPS binding 外，传输可以实现
`<nb/transport_provider.h>`，并由 node 的 binding 配置选择（或者由 proxy 的
`host:port` 目标选择）。

### 零拷贝发布（loan/borrow）

普通的 `nb_proxy_call_event` 会把载荷复制到传输发送缓冲区。对于 shm 这类载荷
直接位于共享段的后端，可以借用一个 chunk、写入后发布，省掉这次复制：

```c
nb_loan_t *loan = NULL;
uint8_t *data = NULL;
uint32_t cap = 0;

if (nb_publisher_loan(pub, "TopicService", "chat", len, &loan, &data, &cap) == NB_OK) {
    memcpy(data, my_bytes, len);                   /* 直接写进共享段 */
    nb_publisher_publish_loan(loan, len, 0, NULL); /* 加信封前缀并扇出 */
} else {
    /* NB_ERR_NOT_SUPPORTED：传输不支持 loan；
       NB_ERR_AGAIN：暂时没有可用 chunk，稍后重试 */
}
```

- loan 在发放时就固定了目标传输（通过 proxy 的固定端点或发现），因此
  `publish_loan` 不再做路由查找。只支持 EVENT 发布。
- `publish_loan` 的 `encoding` 为 0 时使用 publisher 的默认
  `payload_encoding`，与普通事件路径一致。
- `nb_publisher_publish_loan` 无论成功失败都会接管 loan。要放弃一个未发布的
  loan，调用 `nb_publisher_loan_release`。loan 是线程私有的，绝不能跨线程传递。
- `cap` 可能大于请求的 `capacity`（按段大小档位向上取整）。写入不要超过
  `len` 字节。

## Codec（编解码器）

```c
typedef struct nb_codec {
    const char *name;
    uint8_t encoding;
    int (*encode)(void *ctx, const void *obj, void **out_data, size_t *out_len);
    int (*decode)(void *ctx, const void *data, size_t len, void **out_obj);
    void (*release)(void *ctx, void *obj);
    void *ctx;
} nb_codec_t;
```

注册：

```c
nb_node_register_codec(node, &my_codec);
const nb_codec_t *codec = nb_node_find_codec(node, response.payload.encoding);
```

encode 产生的字节可以在发送调用返回后立即释放，因为框架已经复制了载荷。

## Service table（服务表）

双方共享同一份头文件里的 service 和 method id：

```c
NB_SERVICE_DEFINE(order_table, 7u, "OrderService",
    NB_SERVICE_METHOD(1u, "get"),
    NB_SERVICE_METHOD(2u, "put"));
```

然后把 `&order_table` 放进 `nb_node_config_t.service_table`，或在创建 node 前
调用 `nb_service_table_install`。service table 是声明式契约；路由仍然使用
`service.method` 字符串。

## Transport provider（传输 provider）

内建的 UDP/TCP 传输是明文且未认证的。要增加 TLS/DTLS 或自定义协议，实现
`nb_transport_provider_ops_t`，并在创建 node 前注册：

```c
nb_transport_provider_register(&my_provider);
```

provider 描述符和 ops 必须有静态生命周期。

ops 和 options 结构体开头都有 version/size 字段，便于跨版本安全注册：

- `nb_transport_provider_ops_t.abi_version` 必须是
  `NB_TRANSPORT_PROVIDER_ABI_VERSION`（0 视为旧版 provider，仍然接受）
- `nb_transport_provider_ops_t.struct_size` 必须是
  `sizeof(nb_transport_provider_ops_t)`
- `nb_transport_provider_options_t.struct_size` 必须是
  `sizeof(nb_transport_provider_options_t)`

注册失败返回 `NB_ERR_INVALID`。用
`nb_transport_provider_unregister(scheme)` 注销。

指针生命周期规则：`options.endpoint` 在实例生命周期内有效；`message` 及其
`payload`/`metadata` 指针只在当前回调内有效；`frame_owner` 归核心所有，由
provider 使用完后释放；`reply_token` 只能原样传回 `reply`；`peer`/`target`
是调用栈上的临时对象。

provider SPI 只使用三个公共对象：

- `nb_endpoint_t`：`send` 的目标和 `reply` 的对端；`scheme/host/port/path`
  由 provider 自己解释，`uri` 是归一化形式。
- `nb_message_t`：与核心内部相同的消息结构；不存在第二份 provider 消息拷贝。
- `nb_ps_qos_t`：发布/订阅 QoS，创建实例时通过
  `nb_transport_provider_options_t.qos` 传入。0.4 的 `nb_qos_t` 是它的兼容
  别名（`NB_NO_DEPRECATED_API` 下隐藏）。点对点 RPC 链路不读取这些策略。

`nb_message_t.sample` 是 DDS 样本元数据：`state`
（`NB_SAMPLE_STATE_VALID` / `DISPOSED` / `NO_WRITERS`，
`nb_sample_info_valid_data()` 对应 DDS 的 `valid_data` 写法）、
`instance_handle` 与 `has_instance_handle`、`source_timestamp_ns`、
`reception_timestamp_ns`，以及 DDS 的实例/视图状态对：
`instance_state`（`ALIVE` / `NOT_ALIVE_DISPOSED` /
`NOT_ALIVE_NO_WRITERS`）和 `view_state`（实例的第一个样本，或重新注册后的
第一个样本为 `NEW`，否则为 `NOT_NEW`），`generation_rank` 统计重新注册次数。
RTPS provider 会填这些字段；其它 provider 保持为零，读起来就是“没有实例的
有效数据”。实例状态需要实例句柄，RTPS reader 从 PID_KEY_HASH 中取得；样本
不带 key hash 的类型投递时不带按实例的状态。

同一套元数据也适用于发送方向。消息设置了 `sample.has_instance_handle`，并且
`sample.state` 为 `NB_SAMPLE_STATE_DISPOSED` 或
`NB_SAMPLE_STATE_NO_WRITERS` 时，会作为 key-only 的 DDS 样本发布，而不是普通
样本：句柄进入 inline QoS，状态位表示实例被 dispose 还是 unregister，payload
被当作序列化 key，而不是编码后的信封。使用 `rtps` provider 时，DDS 层 reader
（包括 Cyclone DDS）能看到状态变化；远端 `nb` 对端目前还看不到，因为 v2 信封
没有可路由这些信息的样本字段。普通样本发送时会忽略 `sample`。

```c
static int my_send(nb_transport_instance_t *inst,
                   const nb_endpoint_t *target,
                   const nb_message_t *msg);
static int my_reply(nb_transport_instance_t *inst,
                    void *reply_token,
                    const nb_endpoint_t *peer,
                    const nb_message_t *msg);
```

入站回调是 `on_receive(receiver, reply_token, message)`；来源端点位于
`message->peer`，因此 provider 不再单独传 `host/port` 参数。

### 状态事件

`nb_transport_provider_options_t.on_status` 是可选的第二个回调：能观察 DDS
状态的 provider 用 `nb_status_event_t`（`<nb/status.h>`）上报，`user` 与
`on_receive` 相同。不上报的 provider 保持 `NULL`。

```c
typedef struct nb_status_event {
    nb_status_kind_t kind;          /* 匹配、deadline、liveliness、QoS */
    char endpoint[NB_STATUS_ENDPOINT_MAX]; /* provider 名称，例如方法名 */
    uint32_t count;                 /* 当前匹配数，或丢失计数 */
    uint32_t total_count;           /* 累计匹配数 */
    int alive;                      /* liveliness 方向 */
    uint32_t alive_count;
    uint32_t not_alive_count;
    bool has_peer;
    uint8_t peer_guid[NB_STATUS_GUID_SIZE];
    char reason[NB_STATUS_REASON_MAX]; /* 匹配失败的 QoS 策略 */
} nb_status_event_t;
```

RTPS provider 在本地端点增加或失去远端匹配时报告
`NB_STATUS_PUBLICATION_MATCHED` 和 `NB_STATUS_SUBSCRIPTION_MATCHED`：
`count` 是当前匹配数（下降表示对端消失），`total_count` 是累计数，对应
DDS 的 `PublicationMatchedStatus`/`SubscriptionMatchedStatus`。topic 和 type
相同但无法匹配的一对端点报告为 `NB_STATUS_QOS_INCOMPATIBLE`，`reason` 指出
具体策略（`deadline`、`reliability` 等）。deadline 和 liveliness 事件使用
同一套接口。

`endpoint` 是 provider 对本地端点的命名：RTPS provider 使用端点所属的方法名，
因此事件可以映射回 service，而不暴露 GUID。回调在 provider 自己的线程上运行，
不能回调进 provider。

## Cyclone DDS

内建的 `dds` provider 把 v2 信封作为不透明 octet 样本经 Cyclone DDS 传输。
注册后，在 binding 中使用 `dds://<domain>`（端口可以省略；旧的
`dds://<domain>:1` 写法仍接受）：

```c
#include <nb/dds.h>

nb_cyclone_dds_register();

nb_binding_config_t binding = { "dds", "dds://0" };
nb_node_config_t cfg;
nb_node_config_default(&cfg);
cfg.mode = NB_NODE_MODE_STATIC;
cfg.transport.bindings = &binding;
cfg.transport.binding_count = 1;
```

可以配置 binding QoS；未设置的字段保留 Cyclone DDS 默认值：

```c
strcpy(binding.qos.partition, "orders");
binding.qos.reliability = NB_QOS_RELIABILITY_RELIABLE;
binding.qos.history = NB_QOS_HISTORY_KEEP_LAST;
binding.qos.depth = 8;
```

topic 由完整的 `Service.method` 路由派生：

- 请求：`<Service>_<method>_req`
- 响应：`<Service>_<method>_res`
- 事件/发布订阅：`<Service>_<method>_evt`

用 `nb_publisher_publish` 发布（等价于 `nb_proxy_call_event`），用
`nb_subscriber_subscribe` 订阅（等价于 `nb_service_add_method`）。注册服务端
方法会创建请求/事件 reader 和响应 writer；第一次客户端请求会按需创建请求
writer 和响应 reader。

DDS binding 的 `intra_process` 支持 `off` / `auto` / `only`。`auto` 先在进程
内投递事件，再照常发布到 DDS；`only` 完全跳过 DDS 事件 writer/reader 和
`dds_write`，只投递给同一 provider 的本地订阅者，投递过程中载荷零拷贝。

provider 使用 Cyclone DDS 自己的数据面，因此同一 domain 上独立的 Cyclone
reader 可以收到它的样本。样本类型是 `dds::Message`，即包含框架信封的不透明
octet 序列；事件 topic 是 `<Service>_<method>_evt`。验证网络投递时请使用
`intra_process=off` 或独立进程；`only` 会有意绕过 DDS。

## 发现视图

DDS 在内建 topic `DCPSParticipant`、`DCPSPublication`、`DCPSSubscription` 和
`DCPSTopic` 上发布发现数据。本实现没有类型化的内建 reader，因此以快照形式
提供同样的信息：

```c
#include <nb/discovery_view.h>

nb_discovery_sample_t views[64];
size_t n = nb_node_discovery_read(node, NB_BUILTIN_TOPIC_PARTICIPANT, views, 64);
for (size_t i = 0; i < n; i++) {
    printf("%s %s:%u%s\n",
           views[i].name,
           views[i].host,
           views[i].port,
           views[i].is_local ? " (local)" : "");
}
```

- `NB_BUILTIN_TOPIC_PARTICIPANT` 报告本地 participant 以及 node 发现的所有
  远端 participant：`handle` 中的 GUID、名称、通告地址和 `lease_ms` 租约。
- `NB_BUILTIN_TOPIC_PUBLICATION` 和 `..._SUBSCRIPTION` 报告本地及发现的远端
  端点，包括 topic、type、partition、`is_local`、`is_writer`，以及通告的
  reliability、durability、history、deadline、lifespan 和 liveliness 策略。
- `NB_BUILTIN_TOPIC_TOPIC` 从端点表派生：每个 `(name, type)` 对一条记录，带
  `writer_count`/`reader_count` 和稳定的 16 字节句柄。`DCPSTopic` 不是线上
  协议，因此不会为它通告任何内容。

快照是进程本地的，反映调用时刻的发现状态，因此和 DDS 内建 topic 一样，会以
通告和租约间隔滞后于远端变化。只有实现了可选 `discovery_read` provider 操作的
传输才提供发现（当前是 `rtps`）；没有这类传输的 node 返回 0。

## 指标与日志

```c
nb_metrics_t metrics;
nb_node_metrics(node, &metrics);

const nb_metric_desc_t *desc = nb_metric_find("requests_sent_total");
```

每个注册的指标都有 `scope`（`NB_METRIC_SCOPE_NODE` / `_UDP` / `_TCP` /
`_UDP_RTCP` / `_RTPS`）。`nb_metrics_t` 仍是单个结构体，但当前传输 scope 之外
的字段始终为 0，便于导出器过滤。

### RTPS 统计

使用 `rtps` 传输时，node 还会报告 RTPS scope：

```c
nb_metrics_t m;
nb_node_metrics(node, &m);
printf("rtps: %llu/%llu packets, %llu data in, %llu hb, %llu nacks\n",
       (unsigned long long)m.rtps_packets_sent,
       (unsigned long long)m.rtps_packets_received,
       (unsigned long long)m.rtps_data_received,
       (unsigned long long)m.rtps_heartbeats_received,
       (unsigned long long)m.rtps_acknacks_sent);
```

- 计数器是累计值：报文和字节、SPDP/SEDP 样本、DATA、HEARTBEAT、ACKNACK、
  GAP、分片与 NACK_FRAG、解析错误、被 reader history 策略拒绝的样本，以及
  过期的 participant 租约。
- 仪表是当前值：`rtps_participants`、`rtps_publications` 和
  `rtps_subscriptions`（本地加已发现）。
- 应用直接持有 participant 而不是 node 时，可用
  `nb_rtps_participant_metrics()` 读取单个 participant。

### 基于 TCP 的 RTPS

RTPS 的发现走 UDP，因此 participant 总是先在 UDP 上监听。两个 participant
互相发现后，可以把其余流量——SEDP、用户数据、心跳和确认——迁移到 TCP 连接：

```c
nb_rtps_participant_config_t config = {0};

config.domain_id = 7;
config.enable_tcp = 1;
config.tcp_port = 0;          /* 0 = 临时端口，在 SPDP 中通告 */

nb_rtps_participant_t *participant = nb_rtps_participant_create(&config);
printf("tcp port %u, %zu link(s)\n",
       (unsigned)nb_rtps_participant_tcp_port(participant),
       nb_rtps_participant_tcp_links(participant));

nb_rtps_tcp_stats_t stats;
nb_rtps_participant_tcp_stats(participant, &stats);
printf("%llu bytes in, %llu out over TCP\n",
       (unsigned long long)stats.bytes_received,
       (unsigned long long)stats.bytes_sent);
```

一对 participant 中 GUID 前缀较大的一方接受连接，较小的一方发起连接，因此
每对只存在一条连接，双方无需配置即可达成一致。对端仍已知时，失败的拨号每秒
重试一次。

使用 `rtps` 传输 provider 时，同一开关是查询参数：

```
rtps://7                                   仅 UDP（不变）
rtps://7?tcp                                临时端口上的 TCP
rtps://7/10.0.0.5:47130?tcp=47300           显式 peer，TCP 使用 47300
```

注意：

- SPDP 按规范仍走 UDP；TCP locator 在 SPDP 样本中通告，对端据此拨号。
- 启用 TCP 的 participant 仍接受 UDP 流量，因此混合部署时纯 UDP 一侧无需改动。

要导出线上流量，可在 `nb_rtps_participant_config_t` 中设置
`dump_frames = 1`，或在创建 node 前设置环境变量 `NB_RTPS_DUMP_FRAMES=1`。
每个数据报会以 debug 级别记录方向、对端、长度和开头 48 个八位组的十六进制；
载荷会被有意截断，因此排查发现或可靠性问题时可以放心常开。

指标可以通过 `node.metrics.disable` 关闭；关闭后热路径只保留一次全局标志判断。

日志：

```c
#define NB_LOG_TAG "app"
#include <nb/log.h>

nb_log_info("started node=%s", nb_node_name(node));
```

`nb_log_configure(false, level)` 可在运行期关闭所有日志输出。

## 错误处理

检查每个 `create`、`start` 和 `call` 的返回值。稳定的名称、描述和重试标记见
[错误码](error-codes.md)。

## 版本与兼容性

头文件 API 跨版本保持源码兼容，但不保证二进制兼容：结构布局可能演进。升级时
请用匹配的库版本重新构建调用方。`NB_VERSION_STRING`（`types.h`）报告头文件
API 版本。

## 示例

完整可运行代码见[示例索引](../examples.md)。
