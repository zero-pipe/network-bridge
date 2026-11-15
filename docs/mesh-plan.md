# nb_mesh 集群发现方案（评审稿）

> 状态：**待评审**。本文件只描述方案与取舍，不含代码改动。
> 目标：把现有的 `nb_mesh`（固定心跳 + 摘要比对）替换为面向 5000+ 节点集群的
> 发现与寻址实现，并接上 RPC 主干。

---

## 1. 对参考方案的理解

你给的 `cluster_discovery` 方案，核心是四件事：

| 机制 | 做法 | 解决的问题 |
| --- | --- | --- |
| 定长二进制报文 | `magic` + `msg_type` + `formation_id` + `node_id` + `real_ip` + `rpc_port` + `service_mask` | 无文本解析开销；组播里直接带地址，调用方无需预知对端 IP |
| 哈希桶邻居表 | 8192 桶（2 的幂）+ 链表，Murmur 收尾混淆 | 5000 节点下 O(1) 更新与清理，内存无碎片 |
| 三线程 + 读写锁 | recv / send（Fast-Join 3 发 + 带抖动心跳）/ sweep（200 ms 扫、1.5 s 租约）；`rwlock` 让读多写少并发 | 心跳抖动打散碰撞；租约漏斗自动剔除下线节点 |
| 掩码寻址 | `service_mask` 位图，`find_service(mask)` 直接返回 ip:port | 调用路径零握手、零缓存失效逻辑 |

我认同这个骨架：它比"上线时互相拉服务列表"省得多，而且把"域隔离"（编队 + 组播组）
和"信道打散"（抖动）这两个大规模必需项都做进去了。

---

## 2. 可行性评估

### 2.1 可直接落地（无风险）

- **协议报文与哈希表**：纯数据结构和位运算，与平台无关，可直接实现。
- **编队隔离 + 忽略自身**：与现有 `nb_mesh` 一致，已验证。
- **抖动 / Fast-Join / 租约**：语义清晰，用任意定时设施都能实现。
- **组播收发**：本仓库 `zn_socket_*` 已支持 `join_multicast` / `set_multicast_ttl`，
  RTPS 的 SPDP 也在用同一套调用。

### 2.2 必须适配（否则在本仓库编译不过）

| 问题 | 说明 | 处理 |
| --- | --- | --- |
| `pthread` / `pthread_rwlock_t` | 本仓库要同时支持 **Windows MSVC**，无 pthread | 线程改用 `zb_thread_create/join`，锁改用 `zb_mutex_*` |
| **没有读写锁** | zbase 只有互斥量（无 `zb_rwlock`，也无 condvar 封装） | 方案 A：用互斥量；方案 B：读侧改用"快照指针 + 原子交换"（见 2.3） |
| 裸 `socket/bind/setsockopt` | 本仓库通过 `zn_socket_*` 抽象，且 Windows 用 Winsock | 全部走 `zn_socket_*`（`bind_udp_ex` / `join_multicast` / `sendto` / `recvfrom`） |
| `clock_gettime(CLOCK_MONOTONIC)` | Windows 无此接口 | 用 `zb_monotonic_ms()` / `nb_now_ms()` |

### 2.3 建议补强（不加会更早撞墙）

1. **`find_service` 是 O(桶数) 全表扫描**（8192 次指针 + 链遍历），而它在**每次 RPC
   调用的热路径**上。5000 节点下虽然能跑，但会把读锁持有时间拉长，和心跳写锁抢。
   → 增加**按服务位的节点索引**：32 个服务位各维护一条节点链表（或不重复的紧凑数组），
   `find_service` 变成 O(1) 取表头 + 轮转游标。
2. **字节序不统一**：参考实现里 `magic`、`node_id` 没做网络序转换，`service_mask`
   转了；同一份代码互通没问题，但一旦有异构实现就是坑。
   → 所有多字节字段统一大端，并在报文里加 `version` 与 `seq`。
3. **没有 `seq`**：乱序/重复/回放的 JOIN、HEARTBEAT 会被当新状态处理；重启后
   `node_id` 相同但地址变了的旧包可能把新地址覆盖回旧值。
   → 报文加 `uint32 seq`，接收侧按 `node_id` 记最大 `seq`，只接受更新的。
4. **`service_mask` 只有 32 位**：最多 32 个服务，且要求全网对"bit → 服务"有共识。
   本仓库的服务是**任意名字**（`service.method` 字符串路由），32 位掩码本身描述不了。
   → 见 2.4，这是唯一需要你拍板的架构取舍。
5. **`find_service` 只返回第一个匹配**：没有轮转/失败转移，A 节点挂了要等租约过期
   才换。
   → 返回候选集合（或游标 + `next` 语义），RPC 侧失败后重试下一个。

### 2.4 关键取舍：掩码 vs 服务清单（需要你决策）

上一轮我们讨论过"服务 hash + 按需拉取列表"，你这次给的方案是**纯掩码**。两者互补：

| | 纯掩码（你的方案） | 掩码 + 服务清单（上一轮） |
| --- | --- | --- |
| 服务数量上限 | 32（或扩到 64 位 = 64） | 无上限 |
| 服务名任意性 | 需要**编译期约定** bit ↔ 服务 | 任意字符串 |
| 首次调用代价 | 0 | 需要时单播拉一次（之后缓存） |
| 报文大小 | 26 字节 | 40 字节（+hash+64 位掩码） |
| 适用 | 固定接口的无人机编队（FCS/Gimbal/Vision…） | 通用 RPC 集群，服务动态增减 |

**我的建议：两者并存，掩码走快路径，清单做兜底**：
- 有共享服务表（`nb_service_table`，服务 ID 稳定）→ 用 ID 填掩码，调用零握手；
- 服务数 > 64 或没有共享表 → 掩码置 0，该对端走"服务 hash + 按需拉清单"，拉一次缓存；
- 报文统一 40 字节（含 `service_hash`、64 位掩码、显式 IPv4、seq），两种模式共用。

这样你的 5000 节点场景（固定服务掩码）仍是零握手；而通用部署不会因为服务名对不上
而失效。

---

## 3. 推荐设计

### 3.1 报文（定长 40 字节，大端）

```
[0..1]   magic       'M','B' (0x4D42)
[2]      version     1
[3]      msg_type    JOIN(1) / HEARTBEAT(2) / LEAVE(3) / MANIFEST_REQ(4) / MANIFEST_RSP(5)
[4..7]   formation_id
[8..15]  node_id
[16..19] ipv4        通告地址（0 = 用数据报源地址）
[20..21] rpc_port
[22..23] reserved
[24..27] service_mask_lo  服务 0..31
[28..31] service_mask_hi  服务 32..63
[32..35] service_hash     服务清单版本（纯掩码部署可填 0）
[36..39] seq
```

理由：比 26 字节多 14 字节，换来"显式 IP + 64 位掩码 + 清单版本 + 去重序号"；
单节点带宽从 26×2=52 B/s 增至 80 B/s，5000 节点单机入向 400 KB/s（见 §5），可接受。

### 3.2 数据结构

```c
#define NB_MESH_BUCKETS 8192           /* 2 的幂，掩码取模 */
typedef struct mesh_node {
    uint64_t node_id;                  /* 哈希键 */
    uint32_t ipv4;                     /* 主机序，直接可用 */
    uint16_t rpc_port;
    uint64_t service_mask;
    uint32_t service_hash;
    uint32_t seq;                      /* 已接受的最大序号 */
    int64_t  last_seen_ms;             /* 单调时钟 */
    struct mesh_node *hash_next;       /* 哈希链 */
    struct mesh_node *svc_next[?];     /* 可选：按服务位的索引 */
} mesh_node_t;
```

- 邻居表：`buckets[8192]` + 链（按 `node_id`），插入/更新/删除 O(1)；
- **服务索引**：`svc_head[64]` 双向链 + `svc_cursor[64]`，`find_service(bit)` 走 O(1)
  轮转；节点加入/更新/移除时同步维护（更新掩码时先摘再挂）；
- 空闲节点回收：`free_list`，避免每 1.5 s 租约到期就 malloc/free 抖动。

### 3.3 线程与锁

推荐 **2 个执行体，0 个额外线程**（贴合本仓库"默认不额外起线程"的约定）：

| 执行体 | 承载 | 行为 |
| --- | --- | --- |
| poller（node 已有） | 收包 | `zn_socket_attach_poller` → `on_readable` → 解析/更新表 |
| poller 定时器 | 发送 | Fast-Join：3 次 × 50 ms；之后 1000 ms + `rand()%400` 抖动 |
| poller 定时器 | 清扫 | 200 ms 一次，`lease = 1500 ms`（心跳 × 1.5）超时剔除 |

收发清扫在同一条 poller 线程，天然无锁竞争；只有**应用线程的寻址**需要与写侧同步。

同步采用**无锁读快照**（避免 rwlock，因为 zbase 没有）：

- 读侧（`find_service`，高频）：双缓冲快照 `atomic{ptr, generation}`，读到即用，
  不加锁；写侧每次变更后发布新快照并延迟回收旧快照（RCU 风格，2 个 generation）。
- 若你更倾向简单：`zb_mutex` 单锁也能跑，5000 节点下临界区只有"更新一个节点"，
  但 `find_service` 会与心跳写入互相排队。**这一点需要你确认**。

> 备选：如果你坚持"3 个独立线程"，可以用 `zb_thread_create` 起 recv / send / sweep，
> 代码结构与你的参考实现一一对应，只是把 `pthread_*` 换成 `zb_*`。代价是每进程多
> 2 条线程 + 需要真正的读写锁（zbase 没有，得自己用 mutex+condvar 实现或退化成单锁）。

### 3.4 对外 API（与现有 RPC 对接）

```c
int  nb_mesh_start(nb_mesh_t **out, const nb_mesh_config_t *cfg);
void nb_mesh_stop(nb_mesh_t *mesh);

/* 掩码寻址（你的方案）：O(1)，返回后可缓存 ip/port */
int  nb_mesh_find(nb_mesh_t *m, uint64_t service_bit, nb_mesh_target_t *out);
/* 按名字寻址（RPC 适配层用）：名字 → bit；未命中则按需拉清单 */
int  nb_mesh_find_name(nb_mesh_t *m, const char *service, nb_mesh_target_t *out);

int  nb_mesh_set_services(nb_mesh_t *m, const char *const *names, size_t count);
uint32_t nb_mesh_service_hash(const char *const *names, size_t count);
uint64_t nb_mesh_service_bit(const char *name);   /* 无共享服务表时的位映射 */
size_t   nb_mesh_peers(nb_mesh_t *m, nb_mesh_peer_info_t *out, size_t max);
```

RPC 侧映射关系：
- `nb_discovery_find(service)` → `nb_mesh_find_name()` → 写回 registry（现有逻辑不变）；
- `on_peer(added)` → 按掩码/清单把该节点的服务批量写进 registry；
- `on_peer(removed)`（LEAVE 或租约过期）→ `nb_registry_remove_node()`，调用立刻
  返回 `NOTFOUND`（你要的"下线即不可调用"）。

### 3.5 失败模型

| 事件 | 行为 |
| --- | --- |
| 心跳丢失 | 1.5 s 租约到期 → 删邻居 + 删 registry 条目 |
| 主动 LEAVE | 立即删除，不等租约 |
| 同一 `node_id` 换地址（重启） | 只有 `seq` 更大才覆盖，避免旧包回灌 |
| 调用中途对端挂掉 | RPC 超时失败；重试时按 `find` 游标选下一个候选 |
| 表满 | 按"最久未见"淘汰一个并在日志告警（不静默丢弃新节点） |

---

## 4. 与现有实现的差异（改造清单）

| 现有 | 改为 |
| --- | --- |
| `schema_hash` 与本地比对触发内省 | `service_hash` 只用于**判断本地缓存是否过期**；不比对节点间 |
| 线性数组存 peer（≤256） | 8192 桶哈希 + 服务位索引（支持 5000+） |
| 单 `ntk_mutex` | 读无锁快照（默认）或 `zb_mutex`（简化版） |
| 心跳 500 ms，租约 3 s | 心跳 1000 ms + 0~400 ms 抖动，租约 1500 ms（按你的参数） |
| 无 Fast-Join | 上电连发 3 次 JOIN（50 ms 间隔） |
| 32 位掩码、无 seq、无显式 IP | 40 字节报文：64 位掩码 + seq + 显式 IPv4 |

---

## 5. 规模测算（40 字节报文、1 s + 平均 200 ms 抖动）

| N | 单节点入向 | 单节点出向 | 单节点收包率 | 邻居表内存 |
| --- | --- | --- | --- | --- |
| 100 | 4.0 KB/s | 40 B/s | 100 pkt/s | ~30 KB |
| 1000 | 40 KB/s | 40 B/s | 1000 pkt/s | ~300 KB |
| 5000 | 200 KB/s | 40 B/s | 5000 pkt/s | ~1.5 MB |

- 5000 节点时单机每秒 5000 个报文（约 5 μs/包预算），一次哈希查找 + 一次写锁，
  完全在能力内；组播 + IGMP snooping 下交换机上每周期 5000 个 40 B 帧。
- 真正的瓶颈是**交换机组播复制能力**和**接收中断处理**，不是我们的解析逻辑。
  建议：≥2000 节点时把接收线程绑核、开 `SO_RCVBUF`（8 MB），并考虑按编队拆分。
- 若把心跳降到 2 s（租约 3 s），带宽与包率再减半。

---

## 6. 需要你确认的 4 个决策

1. **线程模型**：`poller + 定时器`（0 额外线程，推荐）还是 `3 个独立线程`（贴合你的
   参考实现，需用 `zb_thread`）？
2. **读的同步方式**：无锁快照（性能最好，代码稍复杂）还是 `zb_mutex` 单锁（简单）？
3. **服务寻址**：只用 32/64 位掩码（要求全网 bit 约定），还是**掩码 + 清单兜底**
   （推荐，兼容任意服务名）？
4. **报文**：采用 40 字节版（含 64 位掩码 / 清单版本 / seq / 显式 IP），还是严格照
   你的 26 字节版（我会补 seq 与统一字节序，但掩码仍 32 位）？

---

## 7. 实施步骤（批准后执行）

1. 重写 `include/nb/mesh.h` + `src/mesh/nb_mesh.c`（报文、哈希表、服务索引、线程/定时器）；
2. 重写 `src/rpc/discovery/discovery.c` 适配新 API（registry 映射、下线清理）；
3. 配置：`rpc.discovery` 增加 `heartbeat_interval` / `lease` / `service_mask` / `contract` 语义，
   JSON 校验同步；
4. 测试：`tests/test_mesh.c` 覆盖 报文编解码/编队隔离/租约过期/LEAVE/表满淘汰/5000 节点
   压测（内存与查找时延）；`tests/test_config.c` 补配置断言；
5. 文档：`docs/architecture.md`、`docs/api/config.md`、新增 `docs/mesh-discovery.md`；
6. 全量构建 + Linux 40 项 ctest + `gate.py perf/stability` 回归。

**验收标准**：单元测试全绿；Linux 全量 ctest 无回退；5000 节点模拟下 `find` 平均
时延 < 1 μs、单节点 CPU 占用可接受；RPC 端到端调用在新发现链路上通过。

---

## 附：当前工作区状态（供参考）

- 并行任务 A（移除 Cyclone DDS）已完成；
- 并行任务 B（ntk → zbase/znet 迁移）仍在运行，完成后需先合入；
- 现有 `nb_mesh` 处于"改名中间态"，本方案落地时会被整体重写。
