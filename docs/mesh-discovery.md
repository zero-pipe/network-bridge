# nb_mesh 集群发现

面向大规模编队（数百到 5000 节点）的 RPC 控制面发现：**定长二进制报文 + 组播心跳 +
哈希邻居表 + 服务位图寻址**。目标是在节点数增长时让**单节点出向流量恒定**，并让
调用路径一次哈希就能拿到对端 `ip:port`——不握手、不拉服务清单、不缓存失效逻辑。

代码位置：

| 文件 | 职责 |
| --- | --- |
| `include/nb/mesh_services.h` | 服务位图契约：新增服务必须在这里分配 bit |
| `include/nb/mesh.h` | 公共 API：报文结构、编解码、生命周期、掩码寻址 |
| `src/mesh/nb_mesh.c` | 报文编解码、三个执行体、公共 API |
| `src/mesh/nb_mesh_table.{h,c}` | 邻居表（哈希桶 + 服务位索引 + 淘汰），无 socket，可单测 |
| `src/rpc/discovery/discovery.c` | RPC 适配层：服务名 ↔ 位、邻居 → RPC 端点 |

---

## 1. 报文

**26 字节定长，所有多字节字段统一大端**（含 magic 与 node_id，异构实现不会踩端序）。

| 偏移 | 长度 | 字段 | 编码 | 说明 |
| --- | --- | --- | --- | --- |
| 0 | 2 | `magic` | u16 BE = `0x5A43`（'Z','C'） | 快速丢弃非本协议数据报 |
| 2 | 1 | `msg_type` | u8 | 1=JOIN，2=HEARTBEAT，3=LEAVE，其他一律丢弃 |
| 3 | 1 | `reserved` | u8 | 发送恒 0，接收忽略（预留给扩展位） |
| 4 | 4 | `formation_id` | u32 BE | 编队 ID：不同编队直接丢弃（编队隔离） |
| 8 | 8 | `node_id` | u64 BE | 集群内唯一节点标识；与本机相同时忽略 |
| 16 | 4 | `real_ip` | u32 BE | 通告的 IPv4（`7F 00 00 01` = 127.0.0.1）；0 = 用数据报源地址 |
| 20 | 2 | `rpc_port` | u16 BE | 该节点的 RPC 服务端口 |
| 22 | 4 | `service_mask` | u32 BE | 服务位图（见 `include/nb/mesh_services.h`） |

不用 `#pragma pack`：编码/解码是显式的大小端读写函数
（`nb_mesh_encode()` / `nb_mesh_decode()`），结构体布局变化不会影响线上格式。

接收侧校验顺序（任一条不满足即丢弃，不"尽力解析"）：

1. 数据报长度必须 `== 26`；
2. `magic` 必须符合；
3. `msg_type` 必须是 1/2/3；
4. `formation_id` 必须与本机一致（编队隔离）；
5. `node_id` 不得等于本机（组播回环 / 本机多实例）；
6. `node_id != 0`。

`nb_mesh_packet_accept()` 封装了 4~6，收包路径与单元测试共用同一份判断。

**为什么不做"服务清单拉取"**：清单式发现要么在节点上线时做 O(N²) 握手，要么在首次
调用时引入一次额外往返与缓存版本逻辑。编队场景的服务能力是**编译期固定**的，位图
足够；位图不含参数，接口版本仍走 RPC 元数据。

---

## 2. 服务位图契约

```
bit  0..7   CORE   框架自身（FCS、HEALTH…）
bit  8..15  SENSE  感知（SLAM、VISION…）
bit 16..23  ACT    执行机构（GIMBAL…）
bit 24..31  RELAY  中继/网关（NODE…）
```

规则：

- **新增服务必须先在这里分配 bit**；已写入协议的编号等同于线上 ABI，不可复用。
- 有共享 `nb_service_table` 的部署：`服务名 -> 服务 ID -> bit = 1 << (id % 32)`
  （`id < 32` 时直接一一对应）。
- 没有共享表的部署：`nb_mesh_service_bit(name)` 用 FNV-1a 折叠到 32 位之一。
  两端对同一服务名算出同一位即可互相寻址。
- 两端**必须用同一套映射**，否则"掩码命中"失真。

---

## 3. 邻居表

`nb_mesh_table_t`（`src/mesh/mesh_table.c`）：

- **主索引**：8192 个哈希桶（2 的幂，`hash & (8192-1)` 取模），链式冲突，键 = `node_id`，
  `murmur3 fmix64` 收尾混淆（避免 node_id 递增时桶号顺序堆积）。
- **每项**：`node_id / ipv4（主机序）/ rpc_port / service_mask / last_seen_ms / hash_next`。
- **`last_seen_ms` 用单调时钟**（`zb_monotonic_ms()`）：墙钟跳变不会让整表瞬间过期。
- **服务索引**：32 条按服务位的双向链，链节点是 `(节点, 位)` 对，另有轮转游标，
  因此 `find_service(bit)` 是 **O(1)**，不是"扫 8192 桶"。节点掩码变化时先摘再挂，
  索引与掩码始终一致。
- **表满**：按"最久未见"淘汰一个邻居，并在 WARN 日志里报出被淘汰的 `node_id`；
  绝不静默丢弃新节点（新节点可能才是本编队真正要找的）。
- **内存**：节点池在 `init` 时一次分配（`max_peers` 项，默认 8192），运行期零
  malloc；链节点按需分配并回收进空闲链，节点数变化不会产生分配抖动。

---

## 4. 三个执行体

| 执行体 | 承载 | 行为 |
| --- | --- | --- |
| **recv** | `zn_socket_attach_poller` 的可读事件 | 抽干 socket（单次最多 64 个报文，避免饿死同线程定时器）→ 校验 → upsert / remove |
| **send** | `zn_poller_do_delay` 自续期回调 | 上电连发 **3 次 JOIN（间隔 50 ms）**，之后每 `heartbeat_ms + rand()%jitter`（默认 1000 + 0..400 ms）发 HEARTBEAT |
| **sweep** | `zn_timer` 固定 200 ms | `now - last_seen > lease_ms`（默认 1500 ms）即剔除 |

- **LEAVE 立即移除**，不等租约：主动下线的延迟是 0，不是 1.5 s。
- **抖动是防碰撞的关键**：5000 节点若同相位，交换机与接收中断会在同一毫秒爆掉。
  默认抖动取心跳的 2/5（上限 400 ms），自定义心跳时按比例缩放。
- **租约必须大于心跳周期**，否则每周期都会误剔除；库在启动时会自动兜底修正。

### 线程模型选择：1 条内部 poller 线程

`nb_mesh_start()` 不接收 poller 参数，自己起 **1 条** `zb_thread` 跑
`zn_poller_run()`，三个执行体都跑在这条线程上（recv 走事件、send/sweep 走定时器）。

理由：

1. **等价性**：三个执行体的语义（Fast-Join 3 连发、抖动心跳、200 ms 清扫、1.5 s
   租约）与"三条独立线程"完全一致，只是把阻塞点换成事件与定时器。
2. **可退出**：znet 的 socket 没有暴露 `SO_RCVTIMEO`，独立 recv 线程要么无法唤醒
   （关不掉），要么需要额外的自我唤醒管道。事件驱动版只需 `stop_flag + zn_poller_wake()`
   就能干净 join。
3. **少两条线程**：进程里已经有 RPC 的 io poller 池，发现再各起 3 条线程会显著抬高
   5000 节点部署的线程切换成本；单线程版的心跳/清扫在一个 poller 里天然串行，**写侧
   无需额外的读写锁竞争**。
4. **无平台依赖**：全部走 `zn_socket_*` / `zb_thread_*` / `zb_mutex_*` /
   `zb_monotonic_ms()`，Windows（MSVC，wepoll）与 Linux（epoll）都能编。

并发约定：

- 邻居表由一把 `zb_mutex` 保护，临界区只有一次链表操作，不持锁做 IO；
- 发送路径（帧组装 + 目标表 + `sendto`）由另一把 `zb_mutex` 保护，定时器线程与
  任意线程的 `announce/leave/add_peer` 可以并存；
- 只读 API（`find` / `peers` / `peer_count`）可在任意线程调用；
- **不要在 mesh 自己的回调或持有 mesh 锁时 `nb_mesh_stop()`**。

---

## 5. 与 RPC 的映射

### 5.1 服务名 → 位

`src/rpc/discovery/discovery.c` 的 `disc_service_bit()`：

```
有共享 nb_service_table 且查到服务 ID  -> bit = 1 << (id % 32)
否则                                   -> bit = nb_mesh_service_bit(name)
```

启动时把"配置里的 `service_mask` ∪ 本机已注册服务折算出的位"作为本机通告位图。

### 5.2 解析顺序

`nb_node_resolve()`（`src/rpc/runtime/dispatch.c`）：

```
静态路由 routes  ->  registry  ->  nb_discovery_find()（仅 mode == mesh）
```

`nb_discovery_find()`：服务名 → 位 → `nb_mesh_find()` → 邻居表命中 `ip + rpc_port` →
scheme 取**本节点主绑定**（tcp 优先）→ `nb_endpoint_t`。

自定义传输（`shm` 等）的地址不是 `ip:port`：心跳里的 `real_ip` 对它们没有意义，
因此 host 取**本节点该 binding 的 channel 名**，端口仍用对端通告的 id。这样同一条
channel 上的多节点无需在报文里携带 channel 名，换 channel 只改部署配置。

`nb_node_wait_for_service()` 复用同一套解析，因此"等待服务上线"与"真正调用"看到的
是同一个结果。

**为什么不预填 registry**：旧实现按"本地服务名 ∩ 对端掩码"往 registry 里塞条目，
客户端本地没有这个服务名时永远学不到远端有它——异构服务集因此失明。改成按需查询后，
客户端不需要事先知道远端有哪些服务名；registry 只保留程序自己注册的内容。

### 5.3 下线即不可调用

`LEAVE` 或租约到期 → 邻居表移除；下一次 `nb_mesh_find()` 返回 `NB_ERR_NOTFOUND`，
`nb_node_resolve()` 随之返回 `NB_ERR_NOTFOUND`，调用方立即看到明确失败（而不是等到
超时）。

### 5.4 配置

`rpc.discovery.{group, port, formation, service_mask, heartbeat_interval, lease, peers}`，
字段与默认值见 [`api/config.md`](api/config.md)。

- `group` 缺省（NULL）→ 由 `formation` 推导组播组（`239.255.x.y:41953`）；
- `group` 显式给地址 → 用它收发；地址不可 join 时（如 `127.0.0.1`）自动退化成
  定向单播目标，本机多节点测试无需依赖环境组播；
- `group: ""` → 完全不加组播，只发 `peers`。

---

## 6. 容量与带宽测算

单节点出向 = 26 B / 心跳周期；单节点入向 ≈ (N-1) × 26 B / 周期。

| N | 入向（1 s 周期） | 收包率 | 邻居表内存（≈96 B/节点 + 链节点） |
| --- | --- | --- | --- |
| 100 | 2.5 KB/s | 100 pkt/s | ~15 KB |
| 1000 | 25 KB/s | 1000 pkt/s | ~150 KB |
| 5000 | 128 KB/s | 5000 pkt/s | ~750 KB |

- 5000 节点下每包处理预算约 200 µs（哈希一次 + 一次锁），实测查找在 5000 邻居下
  2 万次耗时 ~3 ms（见 `unit.mesh` 的容量冒烟）。
- 真正瓶颈是**交换机组播复制能力**与接收中断，而不是解析逻辑。≥2000 节点时建议：
  接收线程绑核、放大 `SO_RCVBUF`、按编队/网段拆分组播组。
- 心跳降到 2 s（租约 3 s）可让带宽与包率再减半。

---

## 7. 失败模型

| 事件 | 行为 |
| --- | --- |
| 心跳丢失 | `lease_ms` 到期 → sweep 剔除邻居 → 寻址立即 NOTFOUND |
| 主动 LEAVE | 立即移除，不等租约 |
| 同 `node_id` 换地址（重启） | 新报文直接刷新 `ip/rpc_port/mask`（无 seq，靠租约收敛） |
| 组播不可用 | 退化为定向单播目标（`peers` / `nb_node_add_discovery_peer`） |
| 表满 | 淘汰"最久未见"并 WARN，新节点照常入库 |
| 对端只通告能力但未开 RPC 端口 | `rpc_port == 0` 视为不可调用，寻址返回 NOTFOUND |
| 同机多实例同端口 | 只开 `SO_REUSEADDR`：Linux 上多个 socket 绑定同一端口并加入同一组播组时，每个都能收到完整副本（已实测 200 进程）。**绝不能开 `SO_REUSEPORT`**，内核会把报文负载均衡给其中一个 socket，表现为每个实例只看到随机子集 |

---

## 8. 测试

`tests/test_mesh.c`（`unit.mesh`）：

- 报文编解码：逐字节核对 26 B 大端布局、三类报文往返、长度/magic/类型异常全拒绝；
- 编队隔离、自包忽略（`nb_mesh_packet_accept` + 真实实例双向验证）；
- JOIN → HEARTBEAT → LEAVE 全流程、下线即不可寻址；
- 租约过期：手工注入一帧 JOIN 后静默，等待 sweep 剔除；
- 表满淘汰、掩码变化后索引重建、多节点同服务的轮转；
- 掩码寻址命中/未命中、运行期更新服务位；
- 1000 / 5000 邻居的容量与查找时延冒烟（只驱动邻居表，不真发 5000 包）。

端到端由 `e2e.loopback` / `e2e.api` / `e2e.stream` / `e2e.worker` / `e2e.semantics`
覆盖（客户端本地不含远端服务名，靠邻居表按需发现并完成调用）。

---

## 9. 现场验证

工具：`tools/mesh_probe`（诊断用，`serve` / `call` / `mutual` / `scale`）。
环境：Ubuntu 22.04，8 核 / 3.8 GB，单机多进程。

### 9.1 零预知调用（最关键的一条）

```
server: mesh_probe serve 42 239.255.42.1 45001      # 只注册 EchoService.echo
client: mesh_probe call  42 239.255.42.1 45001      # 不配置任何对端地址
        -> CALL_OK=1 status=0 len=17
```

客户端**不知道服务端的 IP、RPC 端口，也没有配置任何发现目标**，完全靠组播心跳学到
`ip:port` 后完成调用并正确回显。这是"程序 A 调用程序 B 的 service_b.method_b" 的
最小可复现证据。

### 9.2 多进程互相发现（每个进程都是真实节点）

判据取**进程仍在运行时**的邻居数（`T` 行）；`EXIT` 行在退出阶段，会因 LEAVE 偏小，
不能当判据。

| 进程数 N | 期望邻居 | 实测 |
| --- | --- | --- |
| 8 | 7 | 8/8 都看到 7 |
| 16 | 15 | 16/16 都看到 15 |
| 32 | 31 | 32/32 都看到 31 |
| 64 | 63 | 64/64 都看到 63 |
| 200 | 199 | 200/200 都看到 199 |
| 500 | 499 | 500/500 都看到 499 |
| 1000 | 999 | 1000/1000 都看到 999（8 核 load 141） |

400/1000 的测试分别在 8 核 3.8 GB 与 8 核 16 GB 的机器上跑；单进程常驻内存约 750 KB，
邻居表容量按 4096 配置。1000 进程时 load 到 141，结果仍然精确收敛。

收敛时间：每秒采样，第 1 秒即达到 N-1 并稳定。

### 9.3 1000 节点集群里的零预知调用

```bash
# 998 个纯成员（mask=0，不提供服务）+ 1 个只注册 EchoService 的服务端
for i in $(seq 0 997); do mesh_probe mutual 21 239.255.21.1 47400 25 $i 0 & done
mesh_probe serve 21 239.255.21.1 47400 &
# 另起一个客户端，配置里没有任何对端地址
mesh_probe call 21 239.255.21.1 47400
-> CALL_OK=1 status=0 len=17
```

成员侧邻居数：998 个成员**全部**看到 998（= 999 个成员减去自己）。也就是说
"1000 节点规模下，客户端只靠组播发现就能找到并调用某个节点的服务"这条链路成立。

### 9.4 大规模接收侧（合成心跳注入）

| 注入 peer 数 | 全部入库耗时 | `find` 平均时延 |
| --- | --- | --- |
| 1000 | 72 ms | 0.435 µs |
| 5000 | 282 ms | 0.460 µs |

5000 邻居时寻址仍是 O(1)，没有随规模退化。

### 9.5 单机多进程的边界

单机多进程（每个进程一条 poller 线程 + 一个 socket，共用同一发现端口）只用来验证
**协议正确性**。1000 进程时 8 核机器 `load average` 到 141，仍能精确收敛；但真实
部署里 1000 个节点分布在多台机器上，每台只处理"本机对端数 × 2 pkt/s"的心跳，
与单机压测不是同一负载模型。容量结论以 §6 的测算与 §9.4 的注入结果为准。
