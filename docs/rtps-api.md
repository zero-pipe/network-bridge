# RTPS 公共接口（rtps_*）

本文是 `include/rtps/` 的参考文档，说明对象模型、生命周期、QoS 继承、读写语义和线程约束，并给出与 Cyclone DDS `dds_*` API 的对应关系。协议细节见 [rtps-spec.md](rtps-spec.md)，进度见 [rtps-api-todo.md](rtps-api-todo.md)。

## 1. 分层

```text
应用 / 框架 provider
  include/rtps/*.h        rtps_* 公共接口（本文，安装；lib rtps）
    src/rtps/api/         接口层实现（句柄、topic、历史、waitset）
    src/rtps/             协议引擎（SPDP/SEDP、可靠传输、分片、QoS、TCP；
                          头文件在 src/rtps/internal/，不安装；lib rtps_core）
      ntk                 线程、socket、锁、时间
```

接口层和协议引擎是分开的：`include/rtps/` 不包含任何 `nb_*` 类型或引擎内部头；框架的 `rtps` transport provider 现在也只用这层公共接口。`contract.rtps_provider_api` 这个 ctest 会检查 provider 的源码，出现私有引擎符号就让构建失败。

## 2. 对象模型

```text
rtps_participant_t
  ├── rtps_topic_t            名字 + 类型名 + topic 级 QoS
  │     ├── rtps_writer_t     从 topic 创建（可直接建，也可挂在 publisher 下）
  │     └── rtps_reader_t     从 topic 创建（可直接建，也可挂在 subscriber 下）
  ├── rtps_publisher_t        可选分组实体，持有 writer
  ├── rtps_subscriber_t       可选分组实体，持有 reader
  └── rtps_waitset_t          参与者作用域，等待一个或多个 reader
```

- 所有句柄都是不透明的；配置/数据结构的第一个字段是 `uint32_t struct_size`，用 `*_init()` 初始化。库只读它认识的前缀，结构体可以向后扩展。
- **topic 是 DCPS 概念，不是 RTPS 实体**：topic 不上线，SEDP 仍然只通告端点解析后的 QoS。因此增加 topic 不改变协议或互通性。
- 同一个 participant 内，topic 名唯一。用同名同类型再次创建返回同一个句柄；同名不同类型返回 `RTPS_ERR_PRECONDITION`。
- 删除 topic 会级联删除由它创建的 writer/reader（DDS 的父子规则）；删除 participant 删除全部 topic、endpoint 和 waitset。
- writer/reader 一经创建就占一个 RTPS 端点，participant 没启动时先登记，启动后通告。
- publisher/subscriber 是可选的分组实体（DDS 的 Publisher/Subscriber）：它们持有各自的 writer/reader，删除时级联删除；同样不是 RTPS 实体，不上线。`rtps_writer_create()`/`rtps_reader_create()` 直接建端点，不经过分组实体，两种方式可以混用。

## 3. 快速开始

```c
#include <rtps/rtps.h>

static void on_data(void *user, rtps_reader_t *r,
                    const rtps_sample_t *samples, size_t n) {
    (void)user; (void)r;
    for (size_t i = 0; i < n; i++)
        handle(samples[i].data, samples[i].size, &samples[i].info);
}

rtps_participant_config_t pc;
rtps_participant_t *p;
rtps_participant_config_init(&pc);
pc.domain_id = 0;
rtps_participant_create(&pc, &p);
rtps_participant_start(p);

rtps_topic_desc_t td;
rtps_topic_t *topic;
rtps_topic_desc_init(&td);
td.name = "demo/hello";
td.type_name = "demo::Hello";
rtps_qos_set_reliability(&td.qos, RTPS_RELIABILITY_RELIABLE);
rtps_topic_create(p, &td, &topic);

rtps_writer_t *w;
rtps_reader_t *r;
rtps_writer_create(topic, NULL, &w);   /* 未设置的策略继承 topic */
rtps_reader_create(topic, NULL, &r);
rtps_reader_set_data_callback(r, on_data, NULL);

rtps_writer_write(w, cdr_body, cdr_body_len); /* 自动加 4 字节封装头 */

rtps_participant_destroy(p); /* 递归销毁 topic/endpoint/waitset */
```

## 4. QoS 与继承

`rtps_qos_t` 用一个 `present` 位图表示“调用者显式设置了哪些策略”。端点生效的 QoS 按

```text
writer/reader 显式值  >  publisher/subscriber 值  >  topic 值  >  DDS 默认值
```

逐字段解析。`rtps_writer_get_qos()` / `rtps_reader_get_qos()` 返回解析后的结果（present 位是 topic 与端点位的并集）。没有设置、最终也没有默认值的策略不会被写进 SEDP。

| 策略 | 设置函数 | 参与匹配 | 说明 |
| --- | --- | --- | --- |
| Reliability | `rtps_qos_set_reliability` | 是 | BE / RELIABLE |
| Durability | `rtps_qos_set_durability` | 是 | VOLATILE / TRANSIENT_LOCAL / … |
| History | `rtps_qos_set_history` | 否 | 只影响 writer 历史与 reader 队列 |
| Deadline | `rtps_qos_set_deadline_ms` | 是 | |
| Lifespan | `rtps_qos_set_lifespan_ms` | 否 | writer 侧过期后不再重传 |
| Liveliness | `rtps_qos_set_liveliness` | 是 | 手动模式只通告，实际按自动处理 |
| Ownership | `rtps_qos_set_ownership` | 是 | EXCLUSIVE 时按 strength 选主 |
| Destination order | `rtps_qos_set_destination_order` | 是 | BY_SOURCE 时丢弃更旧的样本 |
| Presentation | `rtps_qos_set_presentation` | 是 | 只做兼容性检查 |
| Partition | `rtps_qos_set_partition` | 是 | 逗号分隔，支持 `*` `?` |
| Latency budget | `rtps_qos_set_latency_budget_ms` | 是 | |
| Time-based filter | `rtps_qos_set_time_based_filter_ms` | 否 | reader 侧限流 |
| Resource limits | `rtps_qos_set_resource_limits` | 否 | 0 在 API 里表示无限，线上写 0xffffffff |
| Writer data lifecycle | `rtps_qos_set_autodispose` | 否 | writer 删除时 dispose 实例 |

`rtps_qos_init()` 清零；`rtps_qos_is_default()` 判断是否所有策略都未设置。

## 5. 读写语义

### 5.1 载荷

载荷是不透明 CDR。`rtps_writer_write()` 接收 CDR body 并自动加 4 字节 `00 01 00 00`（XCDR1 LE）封装头；`rtps_writer_write_serialized()` 接收已经带封装头的字节。回调与 read/take 都给出去掉封装头后的 body。

RTPS 子消息按 4 字节补齐，而 CDR body 没有长度字段，所以收到的 `rtps_sample_t.size` 可能比写入时多 0–3 个补齐字节；解码器必须忽略尾部的多余字节。这一点和从 Cyclone DDS 收数据时相同。

### 5.2 回调与历史

- `rtps_reader_set_data_callback()` 注册回调；回调在 participant 线程执行，数据是只在本次调用内有效的零拷贝视图。回调里不要调用会阻塞的接口（waitset、read 等）。
- `rtps_reader_enable_history(reader, depth)` 建立有界 KEEP_LAST 队列（depth 0 取 1，最大 4096）。队列是复制存储，与回调可以同时使用。
- 在 topic/reader QoS 里显式设置 `HISTORY` 时，创建 reader 就会自动建立历史。只想用回调、不想复制数据的场景（例如框架 provider）应让 reader 的 QoS 不包含 history。
- `rtps_reader_read()`：返回历史中匹配样本的借用视图，并标记为 READ；不删除，指针在下一次 take/destroy/淘汰前有效。
- `rtps_reader_take()`：移出匹配样本，数据的所有权交给调用者，用 `rtps_sample_release(samples, n)` 归还。
- `rtps_sample_filter_t` 用三个 `*_mask` 维度（sample/view/instance state）和可选实例句柄过滤；`RTPS_STATE_MASK_ANY` 表示该维度不约束。

### 5.2.1 批量写

`rtps_writer_write_batch(writer, rtps_payload_t[], count)` 按顺序写多个 CDR body，
遇到第一个错误就返回。`read`/`take` 本身已经支持一次取多个样本，因此没有单独的
“批量回调”：引擎按样本触发回调，要凑批需要应用侧线程和缓冲，收益不明显。

### 5.2.2 loan（零拷贝）

- 读取侧：`rtps_reader_loan()` 与 `read()` 等价，返回历史中的借用视图，不复制；
  `rtps_reader_return_loan()` 只是让 DDS 风格的代码保持形状，不需要真正归还。
  视图在样本被 take、淘汰或 reader 销毁前有效。
- 写入侧：没有 `rtps_writer_loan`。引擎在 publish 时同步把载荷复制进可靠历史和
  socket 缓冲，真正的零拷贝写入需要引擎持有 loan 缓冲并参与序列化，属于引擎侧
  改造，暂不在接口层伪装。

### 5.3 waitset

`rtps_waitset_create(participant, &ws)` 创建参与者作用域的等待集；`rtps_waitset_attach()` 可以带一个过滤条件，必要时会为该 reader 建立默认深度 1 的历史。`rtps_waitset_wait()` 返回就绪的 reader 数量，0 表示超时，负数是错误；`RTPS_WAITSET_INFINITE` 表示永久等待。唤醒由 reader 历史的入队事件驱动，不依赖轮询周期。

### 5.4 实例与状态

`rtps_writer_write_instance()` 发布带 key 的样本或状态变化：

| state | 线上含义 |
| --- | --- |
| `RTPS_INSTANCE_ALIVE` | 带实例句柄的普通样本（需要 data） |
| `RTPS_INSTANCE_NOT_ALIVE_DISPOSED` | dispose，实例被删除 |
| `RTPS_INSTANCE_NOT_ALIVE_NO_WRITERS` | unregister，writer 不再负责该实例 |

实例句柄通过 DATA 的内联 QoS `PID_KEY_HASH` 传递；`rtps_sample_info_t` 里的 `instance_state`、`view_state`、`generation_rank`、`has_instance_handle` 与 DDS `SampleInfo` 对应。`valid_data = false` 表示这是一次状态变化而不是新数据。

## 6. 线程与生命周期

- 回调（数据、状态）都在 participant 线程执行，且在库内部锁之外；可以在回调里发数据，但不能在回调里销毁正在使用的句柄。
- `read`/`take`/`match_count`/`size` 是线程安全的，内部按“participant 锁 → reader 历史锁”的顺序加锁。
- `waitset_wait` 必须在应用线程调用，不能从数据回调里调用。
- 句柄的创建/销毁按单线程控制使用；不要在一个线程销毁 reader 的同时用另一个线程对它 read/take。回调期间销毁是安全的（reader 有引用计数，会延迟释放）。
- participant 必须先于其 topic/endpoint 销毁；实际使用中直接 `rtps_participant_destroy()` 即可，它会按从属关系清理。

## 7. 错误码

`rtps_error.h` 定义稳定的负值错误码：`RTPS_ERR_INVALID`、`NOMEM`、`NOTFOUND`、`TIMEOUT`、`IO`、`STATE`、`AGAIN`、`TOOBIG`、`CLOSED`、`ALREADY`、`UNSUPPORTED`、`CANCELED`、`OVERLOAD`、`BAD_MESSAGE`、`BUSY`、`PRECONDITION`、`INTERNAL`。`rtps_error_name()` / `rtps_strerror()` / `rtps_error_is_retryable()` 提供名字、描述和重试提示。

写操作没有匹配的 reader 时返回 `RTPS_OK`——数据可能已经进入 writer 历史，这不是错误。

## 8. 观测接口

- `rtps_participant_set_status_callback()`：匹配、QoS 不兼容、deadline miss、liveliness 变化、样本丢失/拒绝。事件里带本地端点 GUID、当前/累计计数、对端 GUID 和失败策略名。
- `rtps_participant_discovery_read()`：DCPSParticipant / DCPSPublication / DCPSSubscription / DCPSTopic 的快照。
- `rtps_participant_metrics()`：包/字节、SPDP/SEDP/DATA/HEARTBEAT/ACKNACK/GAP/分片计数、解析错误、丢弃、过期租约，以及参与者/端点数量。
- `rtps_participant_tcp_port()`：启用 TCP 时的监听端口。

## 9. 与 Cyclone DDS `dds_*` 的对应

| 本接口 | Cyclone ddsc | 差异 |
| --- | --- | --- |
| `rtps_participant_create` | `dds_create_participant` | 配置结构显式给出端口/租约/peer；不读 XML |
| `rtps_topic_create` / `_find` / `_destroy` | `dds_create_topic` / `dds_find_topic` | 同名同类型复用，无 topic listener |
| `rtps_writer_create` | `dds_create_writer` | 没有 publisher 聚合实体；直接用 topic |
| `rtps_reader_create` | `dds_create_reader` | 同上 |
| `rtps_writer_write` | `dds_write` | 载荷是不透明 CDR body，不是类型化数据 |
| `rtps_writer_write_instance` | `dds_dispose` / `dds_unregister_instance` | 状态用一个枚举参数 |
| `rtps_reader_set_data_callback` | listener `on_data_available` | 回调直接带样本，不是“有数据”通知 |
| `rtps_reader_read` / `_take` | `dds_read` / `dds_take` | 过滤器用状态掩码 + 实例句柄；take 后要 release |
| `rtps_waitset_*` | `dds_waitset_*` | 只支持 data-available 条件，没有 read/query condition 对象 |
| `rtps_status_callback` | `dds_set_listener` / `dds_get_*_status` | 统一回调，不做按实体 listener |
| `rtps_publisher_create` / `rtps_subscriber_create` | `dds_create_publisher` / `dds_create_subscriber` | 只做 QoS 模板与生命周期分组，不做 group_data/entity_factory |
| `rtps_reader_loan` / `rtps_reader_return_loan` | `dds_read` + `dds_return_loan` | 读取侧借用视图；写入侧 loan 不提供 |
| 无对应 | 类型化 API / IDL 编译器 | 载荷保持不透明 |
| 无对应 | DDS Security、IPv6、iceoryx | 明确不在范围内 |

## 10. 版本与兼容

- `RTPS_API_VERSION`（`rtps_version.h`）是接口层版本，与 RTPS 协议版本（2.4）无关。
- 不透明句柄 + `struct_size` + 只追加字段的策略保证源码兼容；破坏性变化才提升 minor/major。
- 目前以静态库形式构建，`RTPS_API` 宏为空；将来做共享库时只需在这一处加导出属性。
- 三个契约测试守护边界：`contract.rtps_provider_api`（provider 不得用引擎符号）、
  `contract.rtps_headers`（公共头不得出现 `nb_`，`RTPS_API` 声明必须是 `rtps_*`）、
  `contract.rtps_symbols`（`librtps` 只能导出 `rtps_*` 符号；没有 nm 时跳过）。
