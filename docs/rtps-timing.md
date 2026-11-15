# RTPS 消息报文与时序

本文说明内建 RTPS 引擎（`src/rtps/`）如何组织报文、以及各个时序（心跳、确认、重传、分片、租约）在运行期如何推进。对象模型与 QoS 语义见 [`rtps-api.md`](rtps-api.md)；分层结构见 [`architecture.md`](architecture.md)。

## 1. 报文结构

一条 RTPS 消息 = **一个 20 字节 MessageHeader** + **零个或多个子消息**，每个子消息按 4 字节对齐：

```text
RTPS MessageHeader (20 B)
  "RTPS" | version(major,minor) | vendorId(2) | GuidPrefix(12)

Submessage × N
  SubmessageHeader (4 B): submessageId | flags | octetsToNextHeader(u16)
  SubmessageBody:         按 submessageId 解释，4 字节对齐
```

实现位置：`src/rtps/rtps_message.c`（`nb_rtps_message_init` / `nb_rtps_submessage_begin` / `nb_rtps_submessage_end` / `nb_rtps_message_next`）。CDR 编解码在 `src/rtps/rtps_cdr.c`，参数（PID）列表在 `src/rtps/rtps_param.c`。

### 1.1 子消息一览

| 子消息 | id | 方向 | 用途 |
| --- | --- | --- | --- |
| `INFO_TS` | 0x09 | 双向 | 后随实体子消息的源时间戳 |
| `INFO_SRC` | 0x0c | 收 | 多播/无源地址时的发送方 GuidPrefix |
| `INFO_DST` | 0x0e | 发/收 | 后随子消息的目标 GuidPrefix |
| `DATA` | 0x15 | 双向 | 单个样本（含内联 QoS：`PID_KEY_HASH` / `PID_STATUS_INFO`） |
| `DATA_FRAG` | 0x16 | 双向 | 大样本的一个分片 |
| `HEARTBEAT` | 0x07 | 双向 | writer 通告 `[firstSN,lastSN]` 可用范围 |
| `ACKNACK` | 0x06 | 双向 | reader 上报缺失序号（bitmap 置位 = 缺） |
| `GAP` | 0x08 | 发/收 | writer 声明一段序号永不发送（已淘汰/过期） |
| `NACK_FRAG` | 0x12 | 双向 | reader 请求单个分片 |
| `HEARTBEAT_FRAG` | 0x13 | 发/收 | writer 通告某样本的总分片数 |

flag 约定：`0x01` 小端；`DATA` 的 `0x04` = Data、`0x08` = Key；`HEARTBEAT` 的 `0x02` = FINAL（不要求回复）、`0x04` = Liveliness 断言；`ACKNACK` 的 `0x02` = FINAL；`DATA_FRAG` 的 Key 位是 `0x04`（与 `DATA` 不同）。

### 1.2 报文构建约定

- 所有出站报文由 `nb_rtps_message_init` 起手，随后依次追加 `INFO_TS` →（可选 `INFO_DST`）→ 实体子消息。`INFO_SRC` 只在需要向多播地址声明来源时添加。
- 单包预算 `NB_RTPS_PACKET_MAX = 1400` 字节；`DATA` 内联载荷上限 `NB_RTPS_DATA_PAYLOAD_MAX = 1200`，超过即走 `DATA_FRAG`。
- 分片大小 `NB_RTPS_FRAGMENT_SIZE = 1028`（4 的倍数、>1024，满足 RTPS 2.5 9.4.5.3.3），每个分片单独一个数据报。

## 2. 参与者线程与节拍

参与者在自己的线程上运行一个 100 ms 的 poller 循环（`thread_main`，`rtps_participant.c`）：

```text
loop:
  poller_poll(100ms)
  ├── drain_socket(rx_sock)        // SPDP 多播
  ├── drain_socket(unicast_sock)   // 元数据流量（SEDP / 心跳 / ACKNACK）
  ├── drain_socket(data_sock)      // 用户数据（启用标准端口映射时）
  ├── tcp_service()                // RTPS-over-TCP 链路
  ├── if now >= next_announce      → send_spdp();        next += announce 抖动
  ├── if now >= next_sweep         → sweep();            next += 200 ms
  ├── if now >= next_sedp_heartbeat→ send_sedp_heartbeats(); next += sedp_heartbeat_ms
  ├── if now >= next_data_heartbeat→ send_data_heartbeats(); next += data_heartbeat_ms
  ├── if now >= next_pmd           → send_pmd(AUTOMATIC); next += pmd_interval_ms
  └── service_frag_assemblies()    // 缺片补请求
```

`sweep()` 做四件事：远端参与者租约过期、deadline miss 检查、liveliness 检查、TCP 链路重连。

### 2.1 节拍参数

| 参数 | 默认 | 下限/上限 | 说明 |
| --- | --- | --- | --- |
| `announce_interval_ms` | 3000 | ≥100 | SPDP 公告；实际间隔为 ±20% 抖动，下限 `max(interval/2, 100)` |
| `lease_ms` | 20000 | 1000..3600000 | 远端租约；超过即从发现表移除并发 REMOVED 事件 |
| sweep 周期 | 200 | 固定 | 租约/截止时间/存活性/TCP 重连 |
| `sedp_heartbeat_ms` | 1000 | ≥100 | SEDP 端点通告的 HEARTBEAT 节拍 |
| `data_heartbeat_ms` | 1000 | ≥100 | 用户数据可靠 writer 的 HEARTBEAT 节拍 |
| PMD 间隔 | 最短租约的 50% | ≥50 | 自动 liveliness 断言 |
| `NB_RTPS_MIN_ACKNACK_INTERVAL_MS` | 20 | 固定 | 同一 writer→reader 在该窗口内只答一次 HEARTBEAT，防 ACKNACK 风暴 |
| `NB_RTPS_MIN_RESEND_INTERVAL_MS` | 10 | 固定 | 同一 (writer, reader) 的重传间隔下限 |
| `NB_RTPS_MAX_RESEND_PER_ACK` | 64 | 固定 | 单个 ACKNACK 最多重传的样本数，余量交给下一轮 |

## 3. 发送时序（writer）

### 3.1 Best-effort

```text
write()
  → 序列化（CDR，加 4 字节封装头）
  → 对每个匹配的远端 reader：DATA（或 DATA_FRAG）×1
  → 结束（不保留历史，不心跳，不重传）
```

### 3.2 Reliable

```text
write()
  → 序列号 sn = ++next_sn
  → 写入可靠历史（bounded：容量 + 256 KiB 字节预算，FIFO 淘汰最老）
  → 若淘汰了样本，立即对每个远端 reader 发 GAP(被淘汰区间)
  → 逐个目标发送 DATA / DATA_FRAG 序列

周期性：
  send_data_heartbeats()
    → 对每个用到的远端 reader 发 HEARTBEAT(firstSN, lastSN, L 位可选)

收到 ACKNACK(bitmap)：
  → 同一 (writer, reader) 10 ms 内只处理一次
  → 对每个置位序号 sn：
       样本仍在历史且未过期  → 重发 DATA / DATA_FRAG
       样本已过期（lifespan）→ 计入 GAP 区间
       样本已被淘汰          → 计入 GAP 区间
  → 若存在 GAP 区间：发 GAP(gapStart, base, bitmap)
  → 单次最多重传 64 个样本

收到 NACK_FRAG(分片 bitmap)：
  → 按分片号重发对应 DATA_FRAG（key/status 只在第 1 片携带）

收到 HEARTBEAT 且带 L 位：
  → 记录该 writer 的存活性（MANUAL_BY_TOPIC 语义）
```

### 3.3 GAP 的语义

`gap_start` 本身一定属于 gap；`[gap_start, base)` 都不再发送；`base + bitmap` 置位的额外序号也不发送。延迟加入的 reader 因此能跳过它永远等不到的样本，窗口继续前进。

## 4. 接收时序（reader）

```text
收到 DATA：
  → 由 INFO_SRC/INFO_DST 确定来源与目标
  → 匹配本地订阅（topic + type + QoS 兼容）
  → 序号窗口去重（next_expected + 256 bit bitmap）
     sn <  next_expected          → 重复，丢弃
     sn 已在窗口内               → 重复，丢弃
     sn 超出窗口 256 位           → 投递但不记录（下次 ACKNACK 再要）
  → 投递回调 / 入历史队列

  → 若 reliability = RELIABLE：不立即 ACK，等 HEARTBEAT 批量确认

收到 HEARTBEAT：
  → 同 count 的重复心跳不再回复
  → 20 ms 内同一 (writer, reader) 只答一次
  → VOLATILE reader：窗口起点默认取 lastSN+1，不拉匹配前的旧样本；
    但若某个样本的分片正在重组，则回退到该样本序号（取两者较小者）
  → TRANSIENT_LOCAL reader：窗口起点取 firstSN，向历史回捞
  → 构造 ACKNACK(base = next_expected, bitmap 置位 = 缺)
     无缺失 → 发空 ACKNACK（FINAL 位置位，表示“无需再答”）

收到 GAP：
  → 把 gap 区间与 bitmap 置位标为已收，窗口前进，不投递

收到 ACKNACK 请求（本端是 writer 时）见 3.2
```

### 4.1 ParticipantMessageData 的特殊处理

PMD（内建 liveliness writer，entity `0x000200c2`）不保留历史。对它的重传请求直接回 `GAP`，避免可靠对端的 reader 窗口一直等待一个永远不会重发的样本。

## 5. 分片时序（DATA_FRAG）

```text
发送侧：
  payload_len > 1200
    → 第 1..N 片各一个 DATA_FRAG（每片一个数据报，fragments_in_submessage = 1）
    → 第 1 片携带 key hash / status 内联 QoS
    → 全部发完后发 HEARTBEAT_FRAG(writerSN, lastFragmentNum)

接收侧：
  维护 nb_rtps_frag_assembly_t（最多 4 个并发样本，4 MiB 预算）
  → 收到分片：按 frag_off 落位，标记 received[]
  → 收齐：拼成完整样本，走 §4 的投递路径
  → 收不齐：
       静默 60 ms（NB_RTPS_FRAG_NACK_DELAY_MS）后
       → 发 NACK_FRAG(只含缺失分片号)
       → 之后每 100 ms 重试一次（NB_RTPS_FRAG_NACK_RETRY_MS）
       → 最多 4 轮（NB_RTPS_FRAG_MAX_NACKS）
       → 该样本 2 s（NB_RTPS_FRAG_TIMEOUT_MS）无进展则丢弃
  → 收到 HEARTBEAT_FRAG：得知总分片数，可提前判断缺片范围
```

设计取舍：先等 60 ms 是因为乱序数据报通常自己会到，立刻 NACK 会造成无谓重传；4 轮之后交给整样本重传，避免单样本把线程钉住。

## 6. 发现时序（SPDP / SEDP）

```text
SPDP（参与者级）
  本端：每 announce_interval_ms 向多播组和配置的 unicast peer 发 DATA(
        writer = SPDP writer，reader = unknown)
  对端：收到后登记 remote（含 locator、租约、内建端点集），
        租约内未见新公告 → sweep() 移除并发 REMOVED

SEDP（端点级）
  本端：端点增删改 → 追加到 publications/subscriptions 可靠历史 →
        立即向已发现远端广播 DATA（dispose 时带 PID_STATUS_INFO）
  周期性：每 sedp_heartbeat_ms 发 HEARTBEAT(firstSN, lastSN)
  对端：ACKNACK 请求缺失的端点通告 → 本端从 SEDP 历史重传
  端点匹配：topic + type 相同且 QoS 兼容 →
        发 PUBLICATION_MATCHED / SUBSCRIPTION_MATCHED（不兼容发 QOS_INCOMPATIBLE）
```

SEDP 复用与用户数据相同的可靠机制，区别只在于历史由 `publications_writer` / `subscriptions_writer` 单独持有。

## 7. 端口与 locator

- 默认端口映射（RTPS 2.5 9.6.1.4，`standard_ports` 打开时）：`PB=7400`、`DG=250`、`PG=2`；
  SPDP 多播 `d0`，元数据单播 `d1 + PG*pid`，用户数据多播 `d2`，用户数据单播 `d3 + PG*pid`。
- 未启用标准映射时：SPDP 端口显式指定或临时分配，locator 通过 SPDP/SEDP 通告，不依赖对端推算。
- 一个远端端点可能有多个单播 locator（多网卡）；发送时镜像 Cyclone DDS 的做法——向全部可用 locator 发送，接收端按序号去重。
- 启用 RTPS-over-TCP 时，SPDP 通告 TCP locator；一旦建立链路，该参与者的所有流量改走 TCP，`remote->tcp_key` 优先于任何 locator。

## 8. 代码位置对照

| 关注点 | 位置 |
| --- | --- |
| 报文头 / 子消息编解码 | `src/rtps/rtps_message.c` |
| CDR / 参数列表 | `src/rtps/rtps_cdr.c`、`src/rtps/rtps_param.c` |
| 可靠历史与 reader 窗口 | `src/rtps/rtps_reliable.c` |
| SPDP 样本 | `src/rtps/rtps_spdp.c` |
| SEDP 样本 | `src/rtps/rtps_sedp.c` |
| 参与者线程、节拍、收发分派 | `src/rtps/rtps_participant.c` |
| RTPS-over-TCP | `src/rtps/rtps_tcp.c` |
| 公共 API（`rtps_*`） | `src/rtps/api/`、`include/rtps/` |

> 拆分现状：报文构建已收敛到 `rtps_wire.c`，用户数据面在 `rtps_datapath.c`，
> 发现面在 `rtps_discovery.c`，`rtps_participant.c` 只剩生命周期、socket、线程
> 节拍与端点匹配。后续里程碑见 [`roadmap.md`](roadmap.md)。
