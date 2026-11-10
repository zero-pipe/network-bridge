# Fast DDS 互通验证夹具

下面的源文件用于手工验证本仓库的 RTPS 实现与 Fast DDS 的互通，**不参与常规
CMake 构建**。验证结果见下表。

- `rtps_interop.c`：本仓库侧，使用公共 `rtps_*` API，topic
  `hello_world_topic`、type `HelloWorld`，CDR1 载荷。
- `fdds_qos_pub.cpp` / `fdds_qos_sub.cpp`：Fast DDS 侧，使用 Fast DDS
  hello_world 示例的生成类型，显式设置 RELIABLE QoS 和非默认
  `max_blocking_time`（让 Fast DDS 在 SEDP 中发送 `PID_RELIABILITY`）。

## 前提

- 已按仓库方式构建 `rtps` 目标（`cmake --build build --target rtps`）。
- 已构建并安装 Fast DDS（`3rdparty/Fast-DDS-master`），并安装其依赖
  Fast CDR、foonathan_memory、Asio、tinyxml2；示例头文件位于
  `3rdparty/Fast-DDS-master/examples/cpp/hello_world/`。
- 运行时设置 `FASTDDS_BUILTIN_TRANSPORTS=UDPv4`，避免 Fast DDS 走 SHM，
  确保数据真的经过 RTPS/UDPv4。

## 构建

本仓库侧（路径按实际构建目录调整）：

```bash
cc -O2 -std=gnu99 -Iinclude -I3rdparty/ntk/include \
   -I3rdparty/micro-cdr/include -Ibuild/3rdparty/micro-cdr/include \
   tools/fastdds_interop/rtps_interop.c -o build/rtps_interop \
   build/src/librtps.a build/src/librtps_core.a build/src/libnb_base.a \
   build/3rdparty/micro-cdr/libmicrocdr.a build/src/libnb_log.a \
   build/3rdparty/elog/libelog.a build/3rdparty/ntk/libntk.a \
   -lpthread -lrt -ldl
```

Fast DDS 侧（`$P` 是 Fast DDS 安装前缀，`$H` 是 hello_world 示例目录）：

```bash
g++ -O2 -std=c++14 -pthread -I$P/include -I$H \
    tools/fastdds_interop/fdds_qos_pub.cpp \
    $H/HelloWorldPubSubTypes.cxx $H/HelloWorldTypeObjectSupport.cxx \
    -o build/fdds_qos_pub -L$P/lib -lfastdds -lfastcdr -ltinyxml2 \
    -Wl,-rpath,$P/lib
# fdds_qos_sub.cpp 同理
```

## 运行

```bash
# Fast DDS -> nb（默认 writer 用 BE reader；显式 reliable writer 用 reliable reader）
./build/rtps_interop reader 30 0 3
$H/hello_world publisher --samples 3

# nb -> Fast DDS（默认 reader；显式 reliable reader 用 fdds_qos_sub）
$H/hello_world subscriber --samples 3 &
./build/rtps_interop writer 3 16 0

# 显式 RELIABLE 双向
./build/rtps_interop reader 30 0 3 &
./build/fdds_qos_pub 3 16

./build/fdds_qos_sub 3 30 &
RTPS_TEST_RELIABLE=1 ./build/rtps_interop writer 3 16 0
```

- `RTPS_TEST_RELIABLE=1` 让本仓库侧使用 RELIABLE QoS；默认是 BEST_EFFORT。
- `RTPS_TEST_TRANSIENT_LOCAL=1` 让本仓库侧使用 TRANSIENT_LOCAL durability。
- `RTPS_TEST_NO_WAIT=1` 让 writer 不等待匹配先写入，并保持 15 秒，用于晚加入测试。
- `RTPS_TEST_UNICAST_PORT=<port>` 给本仓库 participant 单独的 unicast 端口。
- `RTPS_TEST_ADVERTISE=<ip>` 覆盖通告地址（多网卡环境）。
- `RTPS_TEST_NO_MULTICAST=1` 禁用本仓库侧的 SPDP 组播，只用显式 peer。
- `RTPS_TEST_NO_PEERS=1` 不添加显式 peer，只依赖组播发现。
- `RTPS_TEST_PEER_HOST=<ip>` 显式 peer 的地址，默认 `127.0.0.1`。同机测试时，
  如果 DDS 对端绑定在真实网卡上，Fast DDS 2.x 会从回环地址回包，握手会卡住；
  此时把该变量设为网卡地址（例如 `192.168.1.22`）。
- `RTPS_TEST_STANDARD_PORTS=1` 启用标准 PB/DG/PG 端口映射；默认
  `RTPS_TEST_PARTICIPANT_ID=10`（避免和 Fast DDS 的 participant 0 抢端口）。
- `FDDS_NO_WAIT=1` 让 Fast DDS writer 不等待匹配先写。
- `FDDS_TL=1` 让 Fast DDS writer 使用 TRANSIENT_LOCAL durability；
  `FDDS_SUB_TL=1` 让 Fast DDS reader 使用 TRANSIENT_LOCAL durability。
- `FDDS_MATCHED=<n>` 让 Fast DDS writer 等到 n 个 reader 匹配后再写，用于
  多 participant 测试（Fast DDS 自带的 `--matched` 等价物）。
- `FDDS_PROFILE=<name>` 用 XML 里的 participant profile 创建 participant，
  XML 文件通过 `FASTRTPS_DEFAULT_PROFILES_FILE` 指定。Fast DDS 2.x 多网卡
  场景通常需要 profile 里的 `interfaceWhiteList` 把传输固定到一个网卡。
- `NB_LOG_LEVEL=debug` 打开调试日志；`NB_RTPS_DUMP_FRAMES=1` 打印每个数据报。
- `reader <seconds> [domain] [expect]` 会在收到 `expect` 个样本或超时后退出。
- `writer <count> [message_size] [domain]` 会等待匹配，然后按序发送。

TRANSIENT_LOCAL 晚加入示例：

```bash
# Fast DDS 先写、后启动 nb reader
FDDS_NO_WAIT=1 ./build/fdds_qos_pub 1 16 20 &
RTPS_TEST_RELIABLE=1 RTPS_TEST_TRANSIENT_LOCAL=1 \
  ./build/rtps_interop reader 25 0 1

# nb 先写、后启动 Fast DDS reader
RTPS_TEST_RELIABLE=1 RTPS_TEST_TRANSIENT_LOCAL=1 RTPS_TEST_NO_WAIT=1 \
  ./build/rtps_interop writer 1 16 0 &
FDDS_SUB_TL=1 ./build/fdds_qos_sub 1 20
```

多 participant（两个 Fast DDS + 一个 nb）：

```bash
RTPS_TEST_ADVERTISE=192.168.54.14 RTPS_TEST_UNICAST_PORT=7900 \
  ./build/rtps_interop reader 40 0 2 &
$H/hello_world subscriber --samples 2 &
sleep 3
$H/hello_world publisher --samples 2 --matched 2
```

publisher 必须等待两个 reader 都匹配（`--matched 2`）后再发送；否则后匹配的
participant 会丢掉 BE 样本。本仓库夹具里对应 `FDDS_MATCHED=2`。

## Fast DDS 2.x（发行版包）适配说明

Ubuntu 22.04 的 `libfastrtps-dev` / `fastdds-tools` / `fastddsgen` 是
Fast DDS 2.5.0 + fastddsgen 2.1，和 3.x 源码树有几处差异，验证时需要处理：

1. **生成的文件名**：fastddsgen 2.1 生成 `HelloWorldPubSubTypes.h`，夹具按
   3.x 习惯 include `.hpp`。建一个同名软链即可。
2. **无界 string 的序列化上限**：2.1 生成的 `getMaxCdrSerializedSize()` 对
   `string` 只按 255 + 1 计算，负载超过约 255 字节时 `write()` 返回
   `RETCODE_ERROR`。把 IDL 改成 `string<262144>`（线格式不变）即可验证
   1 KB / 200 KB。
3. **`write()` 重载**：2.5 的 `bool write(void*)` 与
   `ReturnCode_t write(void*, const InstanceHandle_t&)` 并存，单参数调用会
   选中 bool 重载。统一用 `write(&sample, HANDLE_NIL)`，2.5 和 3.x 都是
   `ReturnCode_t`。
4. **命名空间**：2.5 的 `Duration_t`、`RETCODE_OK` 在
   `eprosima::fastrtps` 而不是 `eprosima::fastdds::dds`，编译时加一个小的
   compat 头即可。
5. **多网卡/回环**：2.5 在收到 `127.0.0.1` 来源的 SPDP 后会把对端固定到回环，
   导致 SEDP 心跳从回环发出、ACKNACK 回到错误的 socket。用
   `RTPS_TEST_PEER_HOST=<网卡地址>`，并给 Fast DDS 一个带
   `interfaceWhiteList` 的 participant profile（`FDDS_PROFILE` +
   `FASTRTPS_DEFAULT_PROFILES_FILE`）。

## 已验证结果

| 场景 | 结果 |
| --- | --- |
| Fast DDS 默认 writer → nb BE reader，3 × 16 B | 通过 |
| Fast DDS 显式 reliable writer → nb reliable reader，3 × 16 B / 1 KB / 200 KB | 通过 |
| nb BE writer → Fast DDS 默认 reader，3 × 16 B | 通过 |
| nb reliable writer → Fast DDS 显式 reliable reader，3 × 16 B / 1 KB / 200 KB | 通过 |
| Fast DDS TRANSIENT_LOCAL writer → 晚加入 nb reader，16 B / 200 KB | 通过 |
| nb TRANSIENT_LOCAL writer → 晚加入 Fast DDS reader，16 B / 200 KB | 通过 |
| Fast DDS publisher 重启后重新发现 | 通过 |
| 两个 Fast DDS participant + 一个 nb participant | 通过（publisher 需要 `--matched 2`） |
| nb 标准端口映射（participant_id=10）→ Fast DDS / Fast DDS → nb | 通过（16 B / 1 KB） |

2026-11-06 在 Ubuntu 22.04 + 发行版 Fast DDS 2.5.0 上按上表复验，结果全部
一致（含 1 KB / 200 KB、TRANSIENT_LOCAL 晚加入、重启、多 participant、
标准端口）；同时发现并修复了本仓库可靠 reader 在大样本上的首包竞态。

注意：Fast DDS 默认 DataWriter 为 RELIABLE，但在 SEDP 中省略
`PID_RELIABILITY`；按 DDS/RTPS 默认值这会被理解为 BEST_EFFORT，因此本仓库的
reliable reader 不能直接匹配 Fast DDS 默认 writer。上面的显式 QoS 程序通过
非默认 `max_blocking_time` 让 Fast DDS 发送该 PID，从而验证 reliable 路径。
当前决策是按标准处理（缺失即 BEST_EFFORT），不加入 Fast DDS 厂商启发式。
