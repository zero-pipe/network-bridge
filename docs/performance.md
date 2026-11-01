# 低延迟与毫秒级尖峰控制

本文回答三个问题：如何**发现**延迟尖峰、如何**归因**、如何**消除**。同时记录
一些看起来合理但实际无效的做法。下面每个数字都可以用 `tools/loadgen`
（含 `--spike-log`）和本文中的命令复现。

## 结论

1. 尖峰与载荷大小、传输实现无关：64 B 和 1 KB 载荷的 p99/max 处于同一水平。
2. 可消除的部分来自**线程调度**：CPU 绑核加内联分发，可把 mean 降低 40–50%、
   p99 降低 30–50%，吞吐提升 60% 以上。
3. 残余尖峰（1–8 ms）只占样本的 0.003–0.01%。它们随机分布、没有固定周期，
   **即使一个只做忙等的程序在空闲机器上也会出现**。原因是 vCPU 被抢占或宿主
   调度停顿，进程内无法消除，需要 cpuset 隔离和实时调度。
4. 在同步等待路径上加自旋循环**实测会更差**（p50 从 38 us 变成 46–58 us），
   因为投递完成事件的 poller 线程通常与调用方共享同一个核。该实验已回滚，
   代码中没有保留。

## 测量方法

通用设置：回环、TCP、1 KB 请求/响应、`--window 1`（严格只有一个请求在途），
服务端运行 `bench_server`，客户端和服务端绑到不同的物理核，构建类型
`CMAKE_BUILD_TYPE=Debug`（相对差异比绝对值更可信）。

```bash
# 服务端
taskset -c 0 build-linux/tools/bench_server --transport tcp --bind 127.0.0.1 --data-port 9001 \
  --mode static --io-threads 6 --handler-threads 2 --response-bytes 1024

# 客户端（--spike-log 记录超过阈值的样本及其发生时刻）
taskset -c 2 build-linux/tools/loadgen --transport tcp --target 127.0.0.1:9001 \
  --mode static --service OrderService --method get --duration-seconds 30 \
  --payload-bytes 1024 --expect-response-bytes 1024 --window 1 \
  --spike-log spikes.csv --spike-threshold-us 1000 --output result.json
```

`result.json` 现在包含 `slow_samples`（超过 `--spike-threshold-us` 的样本数）和
`slow_threshold_us`，门禁可以直接据此设置尾延迟预算。`spikes.csv` 记录
`t_us,latency_us`，可用来判断尖峰是否具有周期性。

## 测量结果

五秒一轮（同步 API 路径，1 KB，Debug）：

| 配置 | QPS | mean | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| worker 线程池，未绑核 | 10,664 | 93.4us | 80us | 235us | 4.08ms |
| worker 线程池，绑核 | 14,955 | 66.7us | 50us | 172us | 2.30ms |
| 内联分发，绑核 | 18,135 | 54.9us | 44us | 130us | 3.39ms |

十秒一轮（`--window 1` 异步路径，1 KB，Debug）：

| 配置 | QPS | mean | p50 | p99 | p999 | 超过 1 ms |
| --- | --- | --- | --- | --- | --- | --- |
| 内联，绑核 | 21,162 | 47.1us | 41us | 107us | 272us | 11 / 211,625 (0.005%) |
| worker，绑核 | 16,681 | 59.8us | 51us | 152us | 344us | 12 / 166,809 (0.007%) |
| 内联，未绑核 | 18,173 | 54.8us | 51us | 141us | 348us | 7 / 181,736 (0.004%) |

去掉 worker 线程池省下一次跨线程唤醒；绑核消除迁移和缓存抖动。两者收益大致
可以叠加。

三十秒长跑（内联、绑核、1 KB、默认 `TICK_INTERVAL_MS=2`）：

| 轮次 | QPS | mean | p99 | max | 超过 1 ms |
| --- | --- | --- | --- | --- | --- |
| 第 1 轮 | 23,759 | 41.9us | 103us | 2.72ms | 26 / 712,770 (0.0036%) |
| 第 2 轮 | 22,438 | 44.4us | 100us | 7.78ms | 37 / 673,130 (0.0055%) |
| `TICK_INTERVAL_MS=20` | 22,727 | 43.8us | 98us | 3.62ms | 22 / 681,814 (0.0032%) |

## 传输对比（1 KB，单请求在途）

在相同形状下对所有传输与 gRPC 做轮转对比：1 KB 请求和响应、同一时刻只有一个
同步调用在途、Debug 构建、未绑核、每轮五秒。回环使用 `tools/gate.py perf`；
跨机测试在 192.168.54.13 上运行 `bench_server`、在 192.168.54.14 上运行
`loadgen`，走同一 1 GbE 子网。gRPC 基线使用 libgrpc++ 1.16.1（发行版包）的
C++ 同步一元 API，单 channel、单 outstanding call。

回环（服务端 `--io-threads 6 --handler-threads 2 --worker`）：

| 传输 | QPS | mean | p50 | p99 |
| --- | --- | --- | --- | --- |
| TCP | 12,491 | 79.9us | 68us | 298us |
| UDP | 8,438 | 118.3us | 98us | 589us |
| Cyclone DDS | 6,050 | 165.0us | 138us | 673us |
| RTPS | 5,360 | 186.3us | 163us | 661us |
| gRPC 1.16.1 | 10,740 | 91.0us | 72us | 314us |

跨机（客户端 192.168.54.14 -> 服务端 192.168.54.13）：

| 传输 | QPS | mean | min / max |
| --- | --- | --- | --- |
| TCP | 2,673 | 374us | 185us / 3.9ms |
| UDP | 2,662 | 375us | 241us / 4.2ms |
| Cyclone DDS | 1,829 | 546us | 278us / 6.6ms |
| RTPS | 1,381 | 723us | 290us / 6.7ms |
| gRPC 1.16.1 | 2,630 | 377us | 246us / 5.2ms (p50 339us, p99 768us) |

UDP 的 NACK/FEC 路径在未注入丢包的情况下也跑过，没有出现重传。RTPS 和 DDS
的代价来自发现/保活机制，以及请求和响应分别走不同 topic/实例；在“单请求在途”
这一形状下这是预期结果。回环上我们的 RTPS 比 Cyclone DDS 低约 11%，约为 TCP
的 1/2.3。这些数字来自共享虚拟机，只适合做相对比较，不代表硬件上限。

复现 RTPS 这一行：

```bash
python3 tools/gate.py perf --transports rtps --payload-bytes 1024 --perf-duration 5
```

跨机测试，使用显式单播 SPDP peer（不需要组播）：

```bash
# 服务端（192.168.54.13）
NB_RTPS_ADVERTISE_HOST=192.168.54.13 build/tools/bench_server \
  --rtps 'rtps://7:47130/192.168.54.14:47131' --mode static \
  --io-threads 6 --handler-threads 2 --worker --response-bytes 1024
# 客户端（192.168.54.14）
NB_RTPS_ADVERTISE_HOST=192.168.54.14 build/tools/loadgen \
  --rtps 'rtps://7:47131/192.168.54.13:47130' --rtps-target 'rtps://7:0' \
  --mode static --service OrderService --method get --duration-seconds 5 \
  --payload-bytes 1024 --expect-response-bytes 1024 --window 1 \
  --output result.json
```

## 尖峰归因实验

| 实验 | 结果 | 解释 |
| --- | --- | --- |
| 64 B 对比 1 KB | p99 296us vs 279us，max 2.3ms vs 2.6ms | 载荷大小不是来源 |
| `TICK_INTERVAL_MS=20` 对比默认 2 ms | 0.81/s vs 0.87/s、1.30/s | 定时器 tick 不是来源 |
| 30 秒窗口统计 | 0.0032–0.0055% 的样本超过 1 ms | 量级稳定 |
| 周期性检验（相位集中度 + 蒙特卡洛 p 值） | 最大集中度 0.50–0.53（真正周期会接近 1.0），p=0.37 / 0.63 | 没有稳定周期；间隔接近泊松分布 |
| 纯忙等基线（无 RPC） | 60 秒内 2 次停顿 >= 1 ms（最差 2.76 ms） | 宿主本身存在调度停顿 |
| 限速 2000 QPS（30 秒） | 9 / 60,001 个样本超过 1 ms（0.015%） | 尖峰跟随墙钟时间，而不是吞吐 |

倒数第二行是决定性证据：一个只做 100 us 忙等、完全没有网络和 RPC 的单线程
程序，在空闲机器上 60 秒内仍会出现 1–3 ms 的抢占停顿。进程内热路径再怎么
优化也消不掉这个尾巴。

## 缓解措施与验证

按收益排序：

1. **绑核或隔离 CPU。** 用 `taskset -c` 或 cgroup `cpuset` 给服务端和客户端
   独立核。实测：QPS 提升 40% 以上，p99 下降约 30%。生产环境还可以进一步用
   `isolcpus=`/`nohz_full=` 和 IRQ 亲和性。
2. **延迟敏感方法使用内联分发。** 设置 `handler_threads=0`（或在等价配置中
   关闭 worker 线程池），去掉一次线程交接。实测：mean -18%，p99 -20%。
3. **提高调度优先级。** `SCHED_FIFO` 加 `mlockall` 可以消除 CFS 抢占造成的
   毫秒级停顿；需要 root，验证环境没有该权限。
4. **用窗口深度换尾延迟。** `window>1` 时一次停顿只影响一帧，吞吐可以超过
   80k QPS，代价是每个请求多一段排队延迟。

### 已验证无效

在 `nb_proxy_call_sync` 的 `ntk_sem_timedwait` 前加 50 us 有界自旋，本意是
避免 futex 唤醒：

| 配置 | 无自旋 p50 / mean | 50 us 自旋 p50 / mean |
| --- | --- | --- |
| 内联，绑一个核 | 44us / 54.9us | 58us / 69.5us |
| 内联，绑两个核 | 45us / 53.3us | 53us / 66.3us |

投递完成事件的 poller 线程与自旋线程在同一进程，共享一个核（或 cgroup 配额），
自旋会饿死唤醒方，唤醒反而更晚。该改动已回滚。

## 复现与门禁

```bash
python3 tools/gate.py perf --perf-duration 5 --payload-bytes 1024
```

要排查尖峰，可用
`loadgen --spike-log <csv> --spike-threshold-us 1000` 采集，再按 `t_us` 分析
间隔分布和周期性。门禁可以读取 `result.json` 的 `slow_samples`，设置尾延迟
预算（例如 1 KB / 5 秒场景中不超过 10 个）。
