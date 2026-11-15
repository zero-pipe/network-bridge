# Network Bridge — 层次结构与 wire 格式统一改进计划

## 背景

代码功能已成熟（rtps 与 Cyclone DDS / FastDDS 互联互通，测试优秀），但层次结构不够清晰：

1. **RPC wire 格式**：tcp / udp 两套传输驱动各自实现了一套封装，应用层消息格式（payload / metadata / status / route / timeout）本应统一暴露，但 tcp.c / udp.c 里各自重复了 envelope 编解码与发送逻辑，边界不清。
2. **RTPS 协议**：rtps_participant.c（262 KB）把发现、数据面、可靠传输、时序（heartbeat/acknack/nack/frag 重传）全部揉在一起，消息报文与时序难以追踪。
3. **整体层次**：两条主干（RPC、DDS）+ 共享底座的边界、内部头 vs 公共头的依赖关系需要文档化并用 CI 检查保护。

目标：不动核心功能（保持与 Cyclone/FastDDS 的互通），在不改变对外 API 的前提下，把 wire 格式统一、RTPS 时序清晰化、层次结构分层。

---

## 第一阶段：梳理与文档（不动代码，先把现状和目标固化）

- [x] **任务 1.1**：完成对现有代码的分析，输出《现状与问题》文档（放入 docs/，或在 todo.md 中备注）
  - [x] 1.1-a 确认 tcp.c / udp.c 各自 envelope 编解码重复点
  - [x] 1.1-b 确认 rtps_participant.c 职责过重点
  - [x] 1.1-c 确认 base/rpc/dds/rtps 头文件依赖边界

### 1.1 现状分析（已完成，记录如下）

#### RPC wire 格式现状

**共用部分（已统一）**：
- `src/rpc/message/envelope.h/.c`：`nb_envelope_t` 统一描述符，字段包括 kind/flags/status/request_id/timeout_ms/route/metadata/status_msg/stream/payload
- `src/rpc/message/frame.h/.c`：wire frame v2（固定头 28 字节 + TLV 扩展 + payload），TCP/UDP 共用同一套 frame 编解码
- `include/nb/rpc/message.h`：`nb_message_t` 统一消息上下文，暴露 payload/metadata/status/route/timeout/stream/peer/target
- `include/nb/rpc/transport_provider.h`：provider SPI 直接使用 `nb_message_t`，无额外适配层

**分离部分（tcp vs udp 各自实现）**：
- `src/rpc/transport/tcp/tcp.c`：长度前缀分帧（TCP_HDR_LEN=4 + body），sendv 零拷贝快路径 + 积压队列，在 `nb_tcp_send_common` 里手动拼装 `nb_envelope_t` 并调 `nb_envelope_encode_prefix`
- `src/rpc/transport/udp/udp.c`：RTP + 分片头 + 分片数据（`nb_frag_t`，RTP_HDR_LEN=12 + FRAG_HDR_LEN=20），NACK/RTX/RTCP/FEC 可靠性机制，在 `nb_session_send` 里手动拼装 `nb_envelope_t` 并调 `nb_envelope_encode_prefix`

**问题**：tcp.c / udp.c 里都重复了“把 `nb_message_t` 字段拷贝到 `nb_envelope_t`”的代码（kind/flags/status/request_id/timeout_ms/route/metadata/status_msg/stream 等共 ~15 行），可以抽成一个统一的辅助函数，让两个传输只负责“把编码后的字节流发出去 / 把收到的字节流解码成 envelope”。

#### RTPS 协议现状

`src/rtps/rtps_participant.c`（262 KB，最大单一文件）职责：
- 发现：SPDP 发布/接收（`send_spdp`）、SEDP 发布/接收（`send_sedp_data`/`send_sedp_heartbeat`/`send_sedp_acknack`）、远端参与者租约
- 数据面发送：`send_user_data` / `send_user_fragment` / `send_user_data_fragmented` / `send_user_heartbeat` / `send_user_heartbeat_frag`
- 可靠传输：调用 `rtps_reliable.c` 的 writer 历史 + ACKNACK 处理，但 participant 里直接管理 `data_reader_state_t` 窗口、heartbeat 定时、重传
- 分片重组：`nb_rtps_frag_assembly_t` + `nb_reasm_t`
- PMD：`send_pmd` / `handle_pmd_data`
- TCP 链路管理：`nb_rtps_tcp_t`
- 线程调度：tick / sweep / announce 定时器

**问题**：职责过重，发现 / 数据面 / 可靠传输 三者耦合，时序（heartbeat→acknack→resend→gap）散落在多个函数，难以追踪。

#### 整体层次现状

- `include/nb/base.h`、`include/nb/rpc.h`、`include/nb/dds.h`、`include/nb/nb.h`：三条主干聚合头
- `include/rtps/`：RTPS 独立公共 API（不含 `nb_` 类型）
- `src/base/`、`src/rpc/`、`src/dds/`、`src/rtps/`：对应实现
- 库目标：`nb_base`、`nb_json`、`nb_log`、`ntk_discovery`、`rtps_core`、`rtps`、`nb_rpc`、`nb_dds`、`nb`
- `docs/architecture.md`：已有两条主干 + 共享底座的分层描述，但缺少 codec 层和头依赖边界的正式说明

#### 结论

1. RPC 应用层消息格式（payload/metadata/status/route/timeout）已经通过 `nb_message_t` / `nb_envelope_t` 统一，只需把 tcp/udp 传输里重复的拷贝逻辑抽离。
2. RTPS 需要把 `rtps_participant.c` 按发现 / 数据面 / 可靠传输拆分，并文档化时序。
3. 整体层次需要在 `docs/architecture.md` 里补全 codec 层和头依赖边界，并用 CI 检查保护。

---

- [x] **任务 1.2**：更新 `docs/architecture.md`，补全分层图：
  - base（alloc/buf/error/log/metrics/payload/json）
  - codec 层（envelope/frame，RPC 消息编码/解码，tcp/udp 共用）
  - rpc 主干（node/service/proxy/transport provider）
  - dds 主干（participant/topic/writer/reader + rtps/cyclone 后端）
  - rtps 引擎（消息报文 / 可靠传输 / 发现 SPDP/SEDP / TCP）
  - ntk（socket/poller/thread/lock）
- [x] **任务 1.3**：在 CI 构建中加入头依赖检查（扩展 tests/check_nb_headers.cmake / check_spine_symbols.cmake），禁止 base 包含 rpc/dds，rpc/dds 互不包含
  - 新增 `tests/check_layering_sources.cmake` + ctest `contract.source_layering`：源码级检查
    `src/base` 不引用 rpc/dds、`src/rpc` 不引用 dds/rtps、`src/dds` 不引用 rpc。

---

## 第二阶段：RPC wire 格式统一（tcp / udp 共用一套 envelope）

- [x] **任务 2.1**：抽离统一的 RPC 消息编解码层（src/rpc/message/），已有 envelope.c/frame.c，确认其对外接口：
  - [x] `nb_envelope_encode_prefix` / `nb_envelope_decode` / `nb_envelope_to_message`
  - [x] 应用层字段：kind/flags/status/request_id/timeout_ms/route/metadata/status_msg/stream/payload
  - [x] tcp / udp 传输只负责“把编码后的字节流发出去 / 把收到的字节流解码成 envelope”
  - 新增 `nb_envelope_from_message()`：`nb_message_t → nb_envelope_t` 的唯一映射，消除 tcp/udp 各自手写的 15 行字段拷贝。
- [x] **任务 2.2**：在 tcp.c 中，去掉与 envelope 编解码耦合的重复逻辑，仅保留：
  - [x] 长度前缀分帧（TCP_HDR_LEN + body）
  - [x] sendv 零拷贝快路径 + 积压队列
  - [x] 调用统一 envelope 编解码做前缀/负载拆分
- [x] **任务 2.3**：在 udp.c 中，去掉与 envelope 编解码耦合的重复逻辑，仅保留：
  - [x] RTP + 分片头 + 分片数据（nb_frag_t）
  - [x] NACK/RTX/RTCP/FEC 可靠性机制
  - [x] 调用统一 envelope 编解码做前缀/负载拆分
- [x] **任务 2.4**：确认 `nb_message_t` 作为应用层统一消息上下文，暴露 payload/metadata/status/route/timeout，transport 层只搬运 bytes，不解析业务语义
  - 新增 `src/rpc/transport/common/deliver.c/h`：`nb_transport_deliver_frame()` 成为 tcp/udp/shm 唯一的入站“帧→消息→node”路径。
- [x] **任务 2.5**：跑通 RPC 单元测试与 e2e（tcp/udp/shm），确认互通不回退
  - Windows Debug 本地：test_framing / test_loopback 通过
  - Linux Release 远程：27/27 unit + 13/13 e2e 通过（含 e2e.shm、e2e.shm_mesh、e2e.rtps_cyclone）

### 顺带修复的语义不一致（真实缺陷）

`src/rpc/transport/shm/shm.c` 原先手写了一份 envelope→message 映射，与 tcp/udp 不一致：
- `has_stream` 判定为 `stream_flags != 0 || stream_id != 0`，而 tcp/udp 只看 STREAM 标志位；
  `stream_id` 非零但无 stream 扩展的帧会被误判为流式消息。
- `msg.stream.flags` 未屏蔽 `STREAM_FLAG_STREAM` 位，与 tcp/udp 的已屏蔽值不同。

已改为统一调用 `nb_envelope_to_message()`，三种传输语义一致。

---

## 第三阶段：RTPS 消息报文与时序清晰化

- [ ] **任务 3.1**：拆分 `src/rtps/rtps_participant.c`（262 KB），按职责拆分：
  - [x] 第一步（已完成）：抽出**报文工厂** `src/rtps/rtps_wire.c` + `src/rtps/internal/rtps_wire.h`。
        RTPS 每个出站消息都是 `MessageHeader + INFO_TS + (INFO_DST) + 一个实体子消息`，
        这段起手链原先在参与者里重复了 **13 处**；现统一为
        `nb_rtps_wire_{data,data_frag,heartbeat,heartbeat_frag,acknack,gap,nack_frag}`，
        参与者只负责“填参数 + 选目标 + 发送”。净减约 70 行重复代码。
  - [ ] `rtps_discovery.c`：SPDP 发布/接收、SEDP 发布/接收、远端参与者租约
  - [ ] `rtps_datapath.c`：用户 DATA / DATA_FRAG 发送、HEARTBEAT / HEARTBEAT_FRAG 发送
  - [ ] `rtps_reliable.c`：已有，保留可靠 writer 历史 + ACKNACK 处理 + reader 窗口，把与 participant 的耦合降到最低
  - [ ] `rtps_session.c`（或在 participant 内）：远端会话/TCP 链路管理、target 地址解析

### 任务 3.1 过程中发现并修复的真实缺陷

1. **`nb_rtps_participant_publish` 写越界（栈破坏）**
   `nb_rtps_reliable_state()` 的签名是 `(…, uint64_t *first_sn, uint64_t *last_sn, …)`，
   但调用处传的是 `uint32_t old_first / old_last`。函数按 8 字节写入，
   越界踩坏相邻的 `old_count`，导致“历史淘汰 → 发 GAP”的判断读取脏数据。
   已改为 `uint64_t`，`evicted_first/last` 同步改为 64 位。

2. **待确认（未改，涉及线上格式）**
   `handle_sedp_heartbeat()` 里 `base` 是 32 位，而 reader 窗口的 `next_expected`
   是 64 位（MSVC C4244）。SEDP 序号实际不会到 2^32，但 ACKNACK 的 base 在线上是
   完整 64 位序号，应该把 `send_sedp_acknack()` 的 `base/num_bits` 参数改成 64 位。
   需要单独一轮改动 + 互通验证。
- [x] **任务 3.2**：梳理 RTPS 时序，文档化（docs/rtps-api.md 或新增 docs/rtps-timing.md）：
  - [x] 发送时序：write → 进可靠历史 → 周期性 HEARTBEAT → 对端 ACKNACK → 缺失样本重传 → GAP 通告被淘汰样本
  - [x] 接收时序：recv DATA/DATA_FRAG → 序号窗口去重 → ACKNACK 回复 → 组装后分发给本地订阅
  - [x] 分片时序：DATA_FRAG（每片一包）+ HEARTBEAT_FRAG 告知总片数 → NACK_FRAG 请求缺片
  - 产物：`docs/rtps-timing.md`（报文结构、子消息表、线程节拍、发送/接收/分片/发现时序、参数表、代码位置对照）。
- [x] **任务 3.3**：确认与 Cyclone DDS / FastDDS 的互通不受拆分影响（跑 e2e dds 测试）
  - Linux：`e2e.dds_api_rtps`、`e2e.rtps_cyclone`、`e2e.rtps_participant`、`e2e.rtps_api`
    全部通过；全量 40/40 通过。

---

## 第四阶段：整体层次结构清晰化

- [x] **任务 4.1**：目录层次对齐文档（README.md 中的目录结构表与实际 src/ include/ 一一对应）
  - README 目录结构补充 `message/`（codec 层）、`transport/common/`（传输公共层）、
    `rtps/internal`、`rtps/api`、`docs/rtps-timing.md`、`3rdparty/micro-cdr` 的说明。
- [x] **任务 4.2**：公共头（include/）与内部头（src/*/internal/）边界规范化，CI 强制检查
  - 已有 `contract.nb_headers` / `contract.rtps_headers`；本轮新增 `contract.source_layering`
    覆盖 **源码级** 边界（`src/base` / `src/rpc` / `src/dds` 互不越界）。
- [x] **任务 4.3**：CMake 库目标依赖清理，确保 `nb_rpc` 不链 rtps/ddsc，`nb_dds` 不链 ntk_discovery
  - 核对结果：`nb_rpc = ntk nb_base (+PRIVATE nb_json nb_log ntk_discovery)`，
    `nb_dds = ntk nb_base rtps (+PRIVATE nb_json nb_log)`，`nb = nb_rpc + nb_dds`。
    已满足要求，无需改动。
- [ ] **任务 4.4**：全量构建 + 单元测试 + e2e + perf + stability 门禁通过
  - Linux 已完成：全量 ctest **40/40 通过**（27 unit + 13 e2e，含 shm/shm_mesh/rtps/cyclone）。
  - 待办：`tools/gate.py perf` / `stability` 需要较长的压测窗口，留到下一轮单独跑。

---

## 远程开发环境

- 主机：192.168.1.22
- 用户：youyan / chen
- 目录：/home/youyan/nb
- 用途：远程编译、运行单元测试与 e2e，验证改动在 Linux 上不回退
- 状态：**已连通可用**。远程不是 git 仓库（是代码副本），同步方式：

```bash
# 本地（Git Bash），排除 build/.git/Testing
tar czf - --exclude=.git --exclude=build --exclude=build-dds --exclude=Testing \
    --exclude='*.zip' --exclude=tests/google \
    src include docs examples tools tests CMakeLists.txt README.md todo.md 3rdparty \
  | ssh youyan@192.168.1.22 'cd /home/youyan/nb && tar xzf -'

# 远程构建与测试
ssh youyan@192.168.1.22 'cd /home/youyan/nb && \
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build-linux -j$(nproc) && \
  ctest --test-dir build-linux --output-on-failure'
```

### 环境已知问题（非本项目引入）

Windows Debug 下 `test_rtps_reliable.exe` 返回 exit 3 且无输出。已用 `git stash`
回到未修改的原始代码验证：**同样 exit 3**，因此与本次改动无关，是既有的
Windows 环境问题（同一测试在 Linux 上稳定通过）。后续可单独排查。

优先级：P0 = 构建不回退 & 测试通过；P1 = 层次文档与 CI 检查；P2 = 代码拆分重构。

---

## 本轮完成情况一览

| 阶段 | 状态 |
| --- | --- |
| 一、梳理与文档 | 1.1 / 1.2 / 1.3 全部完成 |
| 二、RPC wire 格式统一 | 2.1–2.5 全部完成，另修复 shm 的 envelope 映射语义不一致 |
| 三、RTPS 报文与时序 | 3.1 第一步（报文工厂）完成；3.2 / 3.3 完成；3.1 剩余模块拆分待续 |
| 四、整体层次清晰化 | 4.1 / 4.2 / 4.3 完成；4.4 单元+e2e 已过，perf/stability 待跑 |

### 下一轮建议起点

1. 继续 3.1：把 `handle_*` 接收分派与 SPDP/SEDP 迁到 `rtps_discovery.c`，
   用户数据面迁到 `rtps_datapath.c`，参与者的 `struct` 移入内部头。
2. 修 `send_sedp_acknack()` 的 32 位序号窄化（见 3.1 缺陷 2），改完跑互通。
3. 跑 `tools/gate.py perf` 与 `stability`，补齐 4.4。

---

## 下一步行动

1. 先在本地完成任务 1.2（更新 docs/architecture.md，补全分层图）
2. 再着手任务 2.1（抽离统一 envelope 编解码辅助函数，供 tcp/udp 复用）
3. 每项代码改动后运行本地构建 + 单元测试，确认不回退
4. 待远程环境可连接时，将代码同步到 /home/youyan/nb 并在 Linux 上跑全量门禁