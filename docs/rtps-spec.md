# 自研 RTPS 实现规范

本文描述 `src/rtps/` 这套 RTPS/DDS 子集的规范。它不是 OMG 规范的翻译，而是**从代码反推出来的、可逐条核对的实现规范**：下面每个字段宽度、每个定时器、每条匹配规则，都能在对应源文件里找到出处；与 RTPS 2.5 不一致的地方单独标注。`docs/rtps-protocol.html` 是配套的大白话手册（讲“长什么样、怎么排错”），本文是更严格的参考。

本文覆盖的范围：

- IPv4 UDP 上的 RTPS 2.4，以及可选的 RTPS over TCP；
- SPDP/SEDP 发现、可靠与尽力而为的用户数据、分片、实例与 key、第 7 章列出的 QoS、liveliness 与状态事件；
- 明确不做：IPv6、DDS Security、iceoryx/PSMX 共享内存、IDL 编译器和类型化 `dds_*` API。载荷是不透明 CDR 字节串加 topic/type 名。

与 Eclipse Cyclone DDS 11.0.1 的互通范围见第 12 章。

## 1. 分层

整体分层与 Cyclone DDS 对应，`ntk` 承担 `ddsrt` 的角色：

```text
ntk                    线程、socket、锁、时间、原子操作          (ddsrt)
src/rtps               RTPS 引擎与 DDS 数据模型                 (ddsi + dds)
  rtps_types.[ch]        GUID、实体 ID、Locator、序列号
  rtps_cdr.[ch]          CDR 读写
  rtps_message.[ch]      RTPS 消息与子消息编解码
  rtps_param.[ch]        ParameterList 与 PID 编解码
  rtps_spdp.[ch]         SPDP 载荷
  rtps_sedp.[ch]         SEDP 载荷
  rtps_reliable.[ch]     writer 历史与 reader 窗口
  rtps_data.[ch]         dds::Message 编解码
  rtps_participant.[ch]  participant 运行时：发现、匹配、QoS、
                         数据面、可靠性、分片、liveliness
  rtps_tcp.[ch]          RTPS over TCP
  （头文件位于 src/rtps/internal/，只对引擎和测试可见，不安装）
src/transport/rtps      框架传输 provider（类似 ddsc 的对外层）
include/nb/rtps.h       provider 的公开入口
```

`src/rtps` 只依赖 `ntk`、C 标准库和少量 `nb` 工具头（`error`、`log`、`message`、`metrics`、`discovery_view`），不依赖框架的 service、路由或信封代码。

## 2. 身份标识

| 项 | 取值 |
| --- | --- |
| 协议版本 | 2.4（`major = 2`，`minor = 4`）。接收时只校验 major 必须为 2，不强制 minor |
| 厂商标识 | `0x01 0x9a`（`NB_RTPS_VENDOR_ID_0/1`）。这是占位值，正式声明合规前必须换成 OMG 分配的厂商号 |
| Participant 实体 ID | `0x000001c1` |
| SPDP writer / reader | `0x000100c2` / `0x000100c7` |
| SEDP publications writer / reader | `0x000003c2` / `0x000003c7` |
| SEDP subscriptions writer / reader | `0x000004c2` / `0x000004c7` |
| ParticipantMessageData writer / reader | `0x000200c2` / `0x000200c7` |
| 未知实体 ID | `0x00000000` |
| GUID prefix | 12 字节，每个 participant 生成一次：`01 9a` + ASCII `"NB"` + 当前毫秒时间戳低 32 位 + （时间戳高 32 位 XOR participant 指针）。必须保证同进程内不重复 |
| 本地用户端点 | 实体 kind `0x03`（writer）或 `0x04`（reader）；key 来自 participant 内单一计数器，从 1 开始，所以第一个端点（无论 writer 还是 reader）是 `0x01000003` 或 `0x01000004` |
| 序列号 | 线上 64 位（`high:low`），内部统一 `uint64_t`；可跨 2^32 边界 |

RTPS 头的 GUID prefix 等于本 participant 前缀的报文在解析任何子消息之前直接丢弃，因此不存在自发现。

## 3. 报文格式

一个报文 = 20 字节消息头 + 连续的子消息，子消息之间没有分隔符：

```text
0                   4                   8                   12
+-------------------+-------------------+-------------------+-------------------+
| 'R' 'T' 'P' 'S'   | major | minor     | vendor id (2)     | guidPrefix(12)   |
+-------------------+-------------------+-------------------+-------------------+
```

编解码规则：

- 只生成小端子消息；`E` 标志为 0 的子消息直接拒绝（`NB_ERR_NOT_SUPPORTED`）。Cyclone 和本实现都发小端，实际不受影响。
- `octetsToNextHeader` = 子消息 body 长度（**含补齐到 4 字节的填充**，不含 4 字节头）。编码器从不写 0（“直到报文末尾”）。
- 子消息长度越过数据报或 TCP 帧边界算解析错误，整包后续子消息不再处理；同一包中在它之前的子消息仍会生效。
- 发出的每个数据报不超过 `NB_RTPS_PACKET_MAX = 1400` 字节；接收上限 `NB_RTPS_RX_MAX = 65536` 字节（对端可能把整个样本塞进一个 DATA/DATA_FRAG）。

### 3.1 子消息

| ID | 名称 | 编码 | 解码 | 说明 |
| --- | --- | --- | --- | --- |
| `0x06` | ACKNACK | 是 | 是 | 仅当 `numbits == 0` 时置 FINAL |
| `0x07` | HEARTBEAT | 是 | 是 | L 位是 `0x04`；`0x02` 是 FINAL，本实现不据此判断 liveliness |
| `0x08` | GAP | 是 | 是 | 连续区间形式，见 5.4 |
| `0x09` | INFO_TS | 是（有效时间戳） | 是 | 不发 invalidate 形式 |
| `0x0c` | INFO_SRC | 是 | 是 | 覆盖后续 Entity Submessage 的源 GUID 前缀 |
| `0x0e` | INFO_DST | 是 | 是 | 按目标 GUID 前缀过滤后续 Entity Submessage；全零视为广播 |
| `0x12` | NACK_FRAG | 是 | 是 | |
| `0x13` | HEARTBEAT_FRAG | 是 | 是 | |
| `0x15` | DATA | 是 | 是 | |
| `0x16` | DATA_FRAG | 是 | 是 | |

INFO_REPLY、PAD、批处理（batching）和厂商私有子消息不解析，按长度跳过。

### 3.2 INFO_TS

`flags = 0x01`，body = `Time_t { int32 seconds; uint32 fraction; }`，fraction 的单位是 2^-32 秒。时间戳对同一报文内其后所有 DATA、DATA_FRAG 生效，直到下一个 INFO_TS。它是样本的源时间戳：会写进 `nb_sample_info_t`、保存在可靠历史里，重传时原样带回。

### 3.3 DATA

```text
flags: E=0x01  I=0x02（内联 QoS）  D=0x04（数据）  K=0x08（key-only）
body:  extraFlags(u16)=0
       octetsToInlineQos(u16)=16
       readerId(4) writerId(4) writerSN(8)
       [inlineQoS ParameterList]        I=1 时
       serializedPayload                 K 或 D
```

`readerId` 可以是具体的本地 reader 实体，也可以是 `UNKNOWN`；接收方在 `UNKNOWN` 时投递给每个匹配的本地 reader，否则只投给指名的那一个。

带 key 的状态变化在內联 QoS 里带 `PID_KEY_HASH`（16 字节），状态变化再加 `PID_STATUS_INFO`：

| STATUS_INFO 值 | 含义 |
| --- | --- |
| `0x00000001` | disposed（实例被删除） |
| `0x00000002` | unregistered（不再有 writer） |

四个状态字节按大端写，和 Cyclone 的 `ddsrt_fromBE4u` 一致。只有 `K` 没有 `STATUS_INFO` 时按 dispose 投递，这是对端发送 key-only 样本时的惯例。

### 3.4 DATA_FRAG

```text
flags: E=0x01  Q=0x02  K=0x04
body:  extraFlags(u16)=0
       octetsToInlineQos(u16)=28
       readerId(4) writerId(4) writerSN(8)
       fragmentStartingNum(u32, 从 1 开始)
       fragmentsInSubmessage(u16, 本实现 =1)
       fragmentSize(u16, =1028)
       sampleSize(u32)
       [inlineQoS，仅第 1 片]
       分片数据
```

`fragmentSize = 1028`（`NB_RTPS_FRAGMENT_SIZE`）：4 的倍数且大于 1024，满足 RTPS 2.5 9.4.5.3.3 的硬性要求；严格的实现会把 1024 判为非法。最后一片更短，其长度由 `sampleSize` 推出，不靠字段。

key hash 与状态只在第一片的內联 QoS 里出现一次；接收方把它们和重组状态一起保存，因此分片的 dispose 仍然带实例句柄。

### 3.5 HEARTBEAT、HEARTBEAT_FRAG、ACKNACK、NACK_FRAG、GAP

| 子消息 | body |
| --- | --- |
| HEARTBEAT | `readerId(4) writerId(4) firstSN(8) lastSN(8) count(u32)`；空历史写成 `firstSN = lastSN + 1` |
| HEARTBEAT_FRAG | `readerId(4) writerId(4) writerSN(8) lastFragmentNum(u32) count(u32)` |
| ACKNACK | `readerId(4) writerId(4) bitmapBase(8) numbits(u32) bitmap（MSB-first） count(u32)` |
| NACK_FRAG | `readerId(4) writerId(4) writerSN(8) bitmapBase(u32) numbits(u32) bitmap（MSB-first） count(u32)` |
| GAP | `readerId(4) writerId(4) gapStart(8) bitmapBase(8) numbits(u32) bitmap（MSB-first）` |

位图约定：第 0 位是第一个 32 位字的最高位 `0x80000000`，第 *i* 位代表 `bitmapBase + i`。两个方向都与 Cyclone 一致。

`HEARTBEAT.count` 与 `ACKNACK.count` 对每个 writer/reader 单调递增。`count` 未变的重复心跳不再应答，借此限制 ACKNACK 速率。

## 4. CDR 与 ParameterList

- SPDP/SEDP 载荷是 ParameterList，前面带 4 字节 PL_CDR_LE 封装头 `00 03 00 00`。
- 用户数据是 CDR_LE 值，封装头 `00 01 00 00`。本实现不解释内容，只把 CDR body（去掉 4 字节头）和 type 名交给应用。
- 内联 QoS 是**不带封装头**的裸 ParameterList，靠 `octetsToInlineQos` 保证 4 字节对齐。
- 参数的 length 包含补齐字节，因此永远是 4 的倍数；列表以 `PID_SENTINEL (0x0001)` 结束。
- 时长是 `int32 秒 + uint32 分数（2^-32 秒）`；无限是 `0x7fffffff / 0xffffffff`。API 里用 0 表示“策略未设置”，编解码边界负责换算。
- 资源上限的“无限”在线上是 `LENGTH_UNLIMITED = 0xffffffff`；API 里用 0 表示无限，两个方向在编解码时换算。
- 字符串是 CDR 字符串（`uint32 长度，含结尾 '\0'` + 字节 + 补齐）；序列是 `uint32 count` + 元素。

### 4.1 PID 表

| PID | 值 | 编码 | 使用位置 |
| --- | --- | --- | --- |
| PAD | `0x0000` | - | 忽略 |
| SENTINEL | `0x0001` | - | 列表结束 |
| PARTICIPANT_LEASE_DURATION | `0x0002` | Duration | SPDP |
| TIME_BASED_FILTER | `0x0004` | Duration | SEDP 订阅 |
| TOPIC_NAME | `0x0005` | string | SEDP |
| OWNERSHIP_STRENGTH | `0x0006` | u32 | SEDP |
| TYPE_NAME | `0x0007` | string | SEDP |
| PROTOCOL_VERSION | `0x0015` | 2 字节 | SPDP |
| VENDORID | `0x0016` | 2 字节 | SPDP |
| RELIABILITY | `0x001a` | kind(u32) + max_blocking_time(Duration) | SEDP |
| LIVELINESS | `0x001b` | kind(u32) + lease(Duration) | SEDP |
| DURABILITY | `0x001d` | u32 | SEDP |
| OWNERSHIP | `0x001f` | u32 | SEDP |
| PRESENTATION | `0x0021` | scope(u32) + coherent(u8) + ordered(u8) | SEDP |
| DEADLINE | `0x0023` | Duration | SEDP |
| DESTINATION_ORDER | `0x0025` | u32 | SEDP |
| LATENCY_BUDGET | `0x0027` | Duration | SEDP |
| PARTITION | `0x0029` | string 序列 | SEDP |
| LIFESPAN | `0x002b` | Duration | SEDP |
| USER_DATA | `0x002c` | octet 序列 | SPDP、SEDP |
| UNICAST_LOCATOR | `0x002f` | Locator (24) | SEDP |
| MULTICAST_LOCATOR | `0x0030` | Locator (24) | SEDP |
| DEFAULT_UNICAST_LOCATOR | `0x0031` | Locator | SPDP |
| METATRAFFIC_UNICAST_LOCATOR | `0x0032` | Locator | SPDP |
| METATRAFFIC_MULTICAST_LOCATOR | `0x0033` | Locator | SPDP |
| HISTORY | `0x0040` | kind(u32) + depth(u32) | SEDP |
| RESOURCE_LIMITS | `0x0041` | 3 × u32 | SEDP |
| DEFAULT_MULTICAST_LOCATOR | `0x0048` | Locator | SPDP |
| PARTICIPANT_GUID | `0x0050` | GUID (16) | SPDP |
| BUILTIN_ENDPOINT_SET | `0x0058` | u32 | SPDP |
| ENDPOINT_GUID | `0x005a` | GUID (16) | SEDP、key-only dispose |
| ENTITY_NAME | `0x0062` | string | SPDP |
| KEY_HASH | `0x0070` | 16 字节 | DATA/DATA_FRAG 内联 QoS |
| STATUS_INFO | `0x0071` | 4 字节，大端位 | DATA/DATA_FRAG 内联 QoS |
| CYCLONE_REQUESTS_KEYHASH | `0x801c` | 1 字节 bool | SEDP 订阅（Cyclone 厂商扩展） |

未知 PID 和未知厂商 PID 一律忽略。可选的变长字段如果超出本地定长表示（participant 名过长、USER_DATA 过大），只丢弃该字段，不再让整个样本无效。

## 5. 可靠性模型

### 5.1 writer 历史

可靠 publication，以及 `TRANSIENT_LOCAL` 或更强持久性的 publication，拥有样本历史：KEEP_LAST 最多 `depth` 条，KEEP_ALL 尽可能多，但每个 writer 受 `NB_RTPS_RELIABLE_MAX_BYTES = 256 KiB` 限制。KEEP_LAST 默认深度 `NB_RTPS_DEFAULT_DATA_HISTORY = 8`，上限 1024。

每条记录保存序列化载荷、写入时间（用于 lifespan）、源时间戳，以及可选的实例句柄和状态。样本被淘汰时，writer 向每个匹配的可靠 reader 发 GAP，并在后续心跳中反映新的区间。

Lifespan：超过 publication 的 `lifespan_ms` 的样本不再重传、不再算作可用；对它的请求用 GAP 回答。

### 5.2 reader 窗口

每个（writer, reader）对有一个 256 序列号的窗口，在收到第一个 DATA 或 HEARTBEAT 时建立。`next_expected` 加位图记录已经消费过的样本；重复包（重传、多播地址重复）只投递一次。

- 由 DATA 建立的窗口：VOLATILE 订阅从该样本的序列号开始；`TRANSIENT_LOCAL` 或更强的订阅从 1 开始，由 writer 用 GAP 告诉它哪些号已经没有了。
- 由 HEARTBEAT 建立的窗口：持久订阅从 1 开始；VOLATILE 订阅从 `lastSN + 1` 开始，从而不会拉取匹配之前产生的样本。例外：该 writer 已有未完成的分片重组时，窗口不能越过那个样本的序列号——分片已经开始到达，说明样本产生于匹配之后；大样本的 HEARTBEAT 可能早于最后一片到达。

### 5.3 HEARTBEAT 与 ACKNACK

- 写入或重传可靠样本时立即发心跳；对每个匹配的可靠 reader 每 `data_heartbeat_ms`（默认 1000 ms，最小 100 ms）再发一次。空历史和“全部已过期”的历史也会通告（`firstSN = lastSN + 1` 或实际保留区间），因为心跳是触发 reader 请求修复的唯一信号。
- reader 收到心跳后回 ACKNACK：`bitmapBase = next_expected`，对 `lastSN` 之前仍未收到的每个序列号置位，最多 256 位。
- 只有“纯确认”（`numbits == 0`）才置 FINAL；请求样本时清 FINAL，让 writer 至少回一个心跳。
- 同一个 `count` 的重复心跳最多应答一次，且两次 ACKNACK 至少间隔 `NB_RTPS_MIN_ACKNACK_INTERVAL_MS = 20 ms`。
- writer 对每个对端最多每 `NB_RTPS_MIN_RESEND_INTERVAL_MS = 10 ms` 处理一次 ACKNACK，每次最多重发 `NB_RTPS_MAX_RESEND_PER_ACK = 64` 个样本；剩下的由 reader 下一轮再要。

### 5.4 GAP

GAP 声明 `[gapStart, bitmapBase)` 加上位图置位的序列号不可用。本实现的 writer 只发一种形态：连续区间 `gapStart = first`、`bitmapBase = last + 1`、`numbits = 0`。`bitmapBase = 0` 是非法报文（Cyclone 会拒绝），因此绝不产生。reader 两种形态都接受：`[gapStart, bitmapBase)` 中的每个号都标记为已消费但不投递，再处理位图。

### 5.5 持久性与晚加入者

- 可靠 publication 在 `TRANSIENT_LOCAL` 或更强时，发现新的远端 reader 后会回放保留历史，再发一个覆盖该区间的心跳。尽力而为 reader 也能靠这次回放拿到数据（它自己没有修复通道）。
- 可靠 writer 对仍然保留的样本始终响应 ACKNACK；durability 只决定是否主动回放。
- VOLATILE reader 不请求匹配之前的历史（见 5.2）。
- SEDP 本身是可靠的并保留自己的历史；对端可以在任何时候重新请求某个 SEDP 样本。

## 6. 分片

序列化样本超过 `NB_RTPS_DATA_PAYLOAD_MAX = 1200` 字节时，按 1028 字节切片，每片一个 DATA_FRAG 数据报，全部发完后再发一个 HEARTBEAT_FRAG 告知总片数。

接收侧：

- 最多同时重组 `NB_RTPS_FRAG_MAX_ACTIVE = 4` 个样本；总预留（样本字节 + 每片一个标志）不超过 `NB_RTPS_FRAG_BUDGET_BYTES = 4 MiB`。超过预算的样本直接丢弃。
- 不完整重组在最后一片之后先静默 `NB_RTPS_FRAG_NACK_DELAY_MS = 60 ms`，以免只是乱序；如果 HEARTBEAT_FRAG 到达时已经知道缺片，则跳过等待。
- 然后发 NACK_FRAG，只点缺失的片；每 `NB_RTPS_FRAG_NACK_RETRY_MS = 100 ms` 重试一次，最多 `NB_RTPS_FRAG_MAX_NACKS = 4` 轮。
- 连续 `NB_RTPS_FRAG_TIMEOUT_MS = 2000 ms` 没有进展就释放这次重组。表满或预算不足时，先丢最久没有进展的那一个。
- NACK_FRAG 的补片与 ACKNACK 共用 writer 侧的每对端限流。

单次发布上限 `NB_RTPS_SAMPLE_MAX = 1 MiB`。可靠历史预算 256 KiB 意味着超过该预算的可靠样本会独自占满历史（比它更老的样本全部被淘汰）；尽力而为样本不保留。

## 7. QoS

`nb_rtps_endpoint_info_t` 承载本实现会通告和执行的策略。每条策略都写进 SEDP；处于 DDS 默认值的策略不写。

| 策略 | 编码 | 本地行为 |
| --- | --- | --- |
| Reliability | kind + max_blocking_time，总是写 | reliable：历史、心跳、ACKNACK、修复；best effort：只发一次 |
| Durability | u32，总是写 | `TRANSIENT_LOCAL` 及更强：向晚加入者回放历史；reader 侧不额外过滤 |
| History | kind + depth，总是写 | 限制 writer 历史；reader 历史深度限制框架队列 |
| Partition | string 序列 | 逗号分隔的名字列表；逐名匹配，支持 `*`、`?` 通配 |
| Deadline | Duration | offered/requested deadline-missed 状态事件 |
| Latency budget | Duration | 只参与 RxO 检查 |
| Lifespan | Duration | writer 侧过期；过期样本用 GAP 回答，不再重发 |
| Liveliness | kind + lease | automatic/manual 的续租与过期、状态事件 |
| Ownership | u32 | EXCLUSIVE 时按 strength、再按 GUID 选一个 writer |
| Ownership strength | u32 | 选主依据 |
| Destination order | u32 | BY_SOURCE 时丢弃更旧的样本（按订阅，不按实例） |
| Presentation | scope + 标志 | 通告并参与 RxO；不提供 coherent set 投递保证 |
| Time-based filter | Duration | reader 丢弃间隔小于最小间隔的样本 |
| Resource limits | 3 × u32 | reader 执行 `max_instances`；0 在线上写成 LENGTH_UNLIMITED |
| Writer data lifecycle | 仅本地 | `autodispose`：writer 删除时 dispose 仍存活的实例 |

RxO 匹配（offered >= requested），按代码检查顺序：

1. topic 名相等、type 名相等；
2. partition 列表至少有一对名字匹配（两边都展开通配符）；
3. reader 要求 RELIABLE 时 writer 必须 RELIABLE；
4. offered durability >= requested durability；
5. reader 要求有限 deadline 时，writer 必须有限且 <= 它；
6. offered latency budget <= requested（requested 为 0 不构成约束）；
7. ownership 相等；
8. offered liveliness kind >= requested kind（AUTOMATIC < MANUAL_BY_PARTICIPANT < MANUAL_BY_TOPIC）；reader 要求有限 lease 时 writer 必须有限且 <= 它；
9. offered destination order >= requested；
10. presentation scope、coherent access、ordered access 满足 reader 的要求。

Resource limits 是本地限制，不是 RxO 策略，不参与匹配。topic/type 相同但违反上述任一条的端点对会以 `NB_RTPS_STATUS_QOS_INCOMPATIBLE` 上报，`reason` 是失败策略的短名。

## 8. 发现

### 8.1 地址与端口

| 项 | 取值 |
| --- | --- |
| SPDP 组播组 | `239.255.0.1` |
| SPDP 组播端口 | `7400 + 250 * domain_id`（domain > 232 时端口溢出，创建直接失败） |
| SPDP 单播 | participant 自行选端口并通告，也可通过配置固定 |
| 端点 metatraffic 单播 | 默认 participant 自行选端口并通告；启用 `standard_ports` 时用 `7400 + 250*domain + d1 + 2*participant_id` |
| 端点用户数据单播 | 默认复用 metatraffic socket；启用 `standard_ports` 时使用独立的 `d3 + 2*participant_id` 端口 |
| 用户数据组播 | 未分配（RTPS 端口映射里的 `d2`）；端点 unicast locator 已足够互通 |
| Locator kind | `1` = UDPv4，`4` = TCPv4，`8` = TCPv6 |
| 地址编码 | IPv4 放在 16 字节地址字段的最后 4 字节 |

标准端口映射是可选配置：`rtps_participant_config_t.standard_ports = 1`，并用
`participant_id` 区分同一 domain 内的多个 participant。此时：

```text
SPDP 组播     = 7400 + 250*domain + d0
metatraffic   = 7400 + 250*domain + d1 + 2*participant_id
用户单播      = 7400 + 250*domain + d3 + 2*participant_id
d0=0, d1=10, d2=1, d3=11
```

该模式要求组播可用；纯单播部署仍使用显式 `spdp_port`/`unicast_port` 或临时
端口。未启用时可以复用同一个 unicast socket，locator 通过 SPDP/SEDP 通告，
因此互通不依赖接收方按标准端口推算。


### 8.2 SPDP

由内置 SPDP writer（`0x000100c2`）以 DATA 子消息发送 PL_CDR_LE ParameterList：

- `PID_PROTOCOL_VERSION = 2.4`、`PID_VENDORID = 0x019a`；
- `PID_PARTICIPANT_GUID`（前缀 + `0x000001c1`）；
- `PID_PARTICIPANT_LEASE_DURATION`（默认 20 s，可配 1 s .. 1 h）；
- `PID_BUILTIN_ENDPOINT_SET = 0x00000c3f`（participant announcer/detector、publication announcer/detector、subscription announcer/detector、ParticipantMessageData writer 和 reader）；
- `PID_METATRAFFIC_UNICAST_LOCATOR`，启用 TCP 时再带一条 kind=4 的 TCP locator；启用组播时带 `PID_METATRAFFIC_MULTICAST_LOCATOR`；总是带 `PID_DEFAULT_UNICAST_LOCATOR`，有配置时带 `PID_DEFAULT_MULTICAST_LOCATOR`；
- 可选 `PID_ENTITY_NAME` 和 `PID_USER_DATA`。

公告每 `announce_interval_ms`（默认 3000 ms，最小 100 ms，±20% 抖动且不低于间隔一半）重复一次，发往组播组和所有配置的显式单播 peer。远端 participant 超过其通告租约没有音讯即过期：连同它的端点一起删除，并触发相应状态事件。

不发送 `PID_DOMAIN_ID` 和 `PID_DOMAIN_TAG`。Cyclone 对缺失的 domain id 用监听端口推断，空 tag 与配置 tag 比较；默认部署可以互通，但对端配置了非空 domain tag 时会忽略本 participant。

### 8.3 SEDP

两个可靠内置 writer 通告本地端点：

- publications 用 `0x000003c2`（对端 `0x000003c7` 接收）；
- subscriptions 用 `0x000004c2`（对端 `0x000004c7` 接收）。

每个样本是 PL_CDR_LE ParameterList，含 `PID_ENDPOINT_GUID`、topic 名、type 名、reliability、durability、history、locator 以及可选 QoS。订阅端点额外写 Cyclone 厂商参数 `PID_CYCLONE_REQUESTS_KEYHASH = 0x801c`（1 字节，值 1），请求 Cyclone writer 在带 key 的样本里附上 `PID_KEY_HASH`；不认识该 PID 的实现按长度忽略。

删除端点是 dispose：一个 K 标志的 DATA（D 清 0），载荷是**只含 `PID_ENDPOINT_GUID` 的 key-only ParameterList**，内联 QoS 带 `PID_STATUS_INFO = 1`。没有这个状态位，对端会把样本当成一次“还活着”的重复通告，端点永远删不掉。

无法解释的 SEDP 样本（必填字段超长、未来扩展）在解析之前就先在 RTPS 层确认序列号，避免对端无休止重传。

发现定时器：SEDP 心跳每 `sedp_heartbeat_ms`（默认 1000 ms）。每个 SEDP writer 在 256 KiB 字节预算内保留至多 `max_local_endpoints` 条样本（默认 1024），ACKNACK 重传与用户数据共用同一套限流。

### 8.4 TCP

启用 `enable_tcp` 时，participant 监听 `tcp_port`（0 表示临时端口），并在 SPDP 里额外通告 kind=4 的 `PID_METATRAFFIC_UNICAST_LOCATOR`。SPDP 本身仍走 UDP。双方发现彼此且都通告了 TCP locator 后，GUID prefix 较小的一方拨号，另一方接受连接。

字节流里就是未经改动的 RTPS 报文（20 字节头 + 子消息），因此 UDP 和 TCP 共用同一个子消息分发器。监听端按连接键（`tcp+<n>`）登记连接，SEDP、用户数据、心跳和 ACKNACK 随后都走这条流。

**与 Cyclone 的 TCP 互通不在支持范围内**：Cyclone 11 的 TCP 是单一传输选择（`General/Transport=tcp`），会把发现也切到 TCP，旧的 `<TCP><Enable>` 元素已被移除；RTPS 规定 SPDP 走 UDP，本实现保持这一点。所以 RTPS over TCP 只在本实现的两个实例之间验证（`test_tcp_transport`），Cyclone 的 TCP-only 模式无法与本实现配对。

## 9. 实例、key 与 SampleInfo

- 实例句柄来自收到的 `PID_KEY_HASH`；本实现不根据载荷计算 key hash。
- 订阅维护一张惰性分配的实例表，受 `max_instances` 限制（默认 64，最大 4096）。每项带实例状态（ALIVE、NOT_ALIVE_DISPOSED、NOT_ALIVE_NO_WRITERS）、view 状态和 generation rank。
- `PID_STATUS_INFO` 是权威来源：dispose 把实例置为 NOT_ALIVE_DISPOSED，unregister 置为 NOT_ALIVE_NO_WRITERS；之后到达的活样本会重新注册并增加 generation rank。
- 远端 writer 被移除（SEDP dispose 或租约过期）时，它注册过的实例变成 NOT_ALIVE_NO_WRITERS。
- `nb_sample_info_t` 带源时间戳、接收时间戳、实例句柄及“是否存在”标志、三个状态维度和 generation rank。dispose/unregister 样本的 `valid_data = false`。

## 10. Liveliness

- AUTOMATIC：participant 用 ParticipantMessageData 样本（writer `0x000200c2`，28 字节载荷：GUID prefix、`uint32 kind = 1`、空 octet 序列）断言其自动 publication，周期为最短有限租约的 50%（下限 50 ms）。
- MANUAL_BY_PARTICIPANT：同样的载荷，`kind = 2`，在每次写入之后发送（当前没有单独的 assert 接口）。
- MANUAL_BY_TOPIC：该 writer 发一个带 `L` 标志（`0x04`）的 HEARTBEAT，`firstSN..lastSN` 覆盖 writer 保留的区间，在写入之后发送。
- reader 在对应信号上续租每个匹配 writer；租约过期或重新断言时上报 `NB_RTPS_STATUS_LIVELINESS_CHANGED`，带 alive/not-alive 计数。

## 11. 状态事件

| 事件 | 来源 |
| --- | --- |
| `PUBLICATION_MATCHED` / `SUBSCRIPTION_MATCHED` | 本地端点增加/失去远端端点；`count` 为当前数，`total_count` 为累计数 |
| `QOS_INCOMPATIBLE` | topic/type 相同但 RxO 不匹配；`reason` 是策略名 |
| `OFFERED_DEADLINE_MISSED` | 写入节奏超过 offered deadline |
| `REQUESTED_DEADLINE_MISSED` | 匹配的 writer 在 requested deadline 内没有样本 |
| `LIVELINESS_CHANGED` | 匹配 writer 的租约过期或重新断言 |
| `SAMPLE_LOST` | GAP 覆盖或 writer 撤回的样本 |
| `SAMPLE_REJECTED` | TimeBasedFilter、源序、独占 ownership 或资源限制导致丢弃 |

回调在 participant 线程执行，且在 participant 锁之外；回调里不得再调用 participant。

## 12. 与 Cyclone DDS 11.0.1 的互通

下表由 `tests/test_rtps_cyclone.c` 覆盖；`NB_TEST_RTPS_CYCLONE=1` 启用，`NB_TEST_RTPS_UNICAST_ONLY=1` 让双方都用固定端口和显式 peer。两种模式在 Linux 上对仓库内 Cyclone 11.0.1 均通过。

| 场景 | 方向 | 验证内容 |
| --- | --- | --- |
| 基础发现与数据 | 双向 | SPDP/SEDP、QoS 从 Cyclone 的 DCPSPublication 回读、可靠投递、8 KiB 分片 |
| 尽力而为 | 双向 | BE writer/reader 配对与投递 |
| TRANSIENT_LOCAL 晚加入 | Cyclone writer → nb reader | 匹配后通过 ACKNACK 拉取保留历史 |
| TRANSIENT_LOCAL 晚加入 | nb writer → Cyclone reader | 发现新 reader 后回放历史 |
| 多读者扇出 | nb writer → 两个 Cyclone reader | 按 reader 实体寻址，两个 reader 都确认 |
| Participant 重启 | Cyclone writer → nb reader | SEDP dispose/租约清除、新前缀重新发现 |
| 200 KiB 样本 | 双向 | 数百个分片、NACK_FRAG 补片、1 MiB 接收预算 |

用例还检查 Cyclone 能把每个通告的 QoS 原值读回、liveliness 租约正确，以及单序列号 GAP 不再被记录为 malformed。

TCP 不在上表中：Cyclone 11 的 TCP 是单一传输模式（发现也走 TCP），而本实现的 SPDP 按 RTPS 规定固定走 UDP；因此 TCP 只验证了本实现内部的两个实例（`test_tcp_transport`）。

## 13. 已知限制与偏差

- **没有 IPv6**：只使用 UDPv4/TCPv4 locator；TCPv6 kind 已定义但没有 socket 路径。
- **没有安全与 iceoryx**：发现和数据都是明文。
- **没有类型系统**：载荷是不透明 CDR body 加 type 名；没有 IDL 编译器、XTypes 类型信息和类型对象交换，type 一致性就是名字相等。
- **INFO_SRC / INFO_DST**：接收侧已解析并按目标前缀过滤 Entity Submessage；
  全零前缀视为广播。TCP 连接按每条 RTPS 消息重置该状态。
- **64 位序列号**：内部统一用 `uint64_t`，wire 上的 high/low 原样保留；writer
  历史和 reader 位图都按 64 位推进，可跨 2^32 边界。
- **Resource limits** 的 writer 侧 `max_samples`/`max_samples_per_instance` 只受历史深度和字节预算间接约束；reader 执行 `max_instances`。
- **Presentation** 只通告并参与匹配，coherent/ordered access 不提供实际投递保证。
- **没有 `dds_assert_liveliness()`**；manual liveliness 靠一次写入触发。
- **Participant 名字**本地记录上限 64 字节、USER_DATA 上限 256 字节；线上更长时截断而不是拒绝。
- **TCP 不与 Cyclone 互通**：本实现的 RTPS over TCP 只覆盖两个本实现实例；Cyclone 的 TCP 选择会替换发现传输，与 RTPS 的 UDP SPDP 不兼容，见 8.4。
- **不支持 PID_DOMAIN_TAG**：配置了 domain tag 的对端会忽略本 participant。

## 14. 测试与验证入口

- `tests/test_rtps_wire.c`：子消息编解码、ParameterList 单元测试。
- `tests/test_rtps_cdr.c`：CDR 读写。
- `tests/test_rtps_sedp.c`：SEDP 构造/解析、key-only dispose。
- `tests/test_rtps_reliable.c`：writer 历史、reader 窗口、GAP。
- `tests/test_rtps_participant.c`：participant 端到端：发现、QoS、可靠性、分片与补片、lifespan、deadline、liveliness、ownership、顺序、过滤、实例、TCP。
- `tests/test_rtps_transport.c`：框架 provider 走 RTPS。
- `tests/test_rtps_cyclone.c`：第 12 章的 Cyclone 矩阵。
- `tools/verify-rtps-interop.sh`：构建，跑 unit/rtps 标签，重复 participant 用例，再跑两种 Cyclone 模式。

`NB_RTPS_DUMP_FRAMES=1` 会在 debug 级别打印每个数据报（方向、对端、长度、前 48 字节十六进制）；`NB_RTPS_ADVERTISE_HOST` 选择通告的 IPv4 地址；`nb_rtps_stats_t` 提供累计计数器。
