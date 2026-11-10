# Network Bridge

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Network Bridge 是一个面向异构网络的 C99 通信框架，由**两条并列的语义主干 + 一个共享底座**组成：

- **RPC 主干**：`node / service / proxy`，参考 brpc/gRPC 的调用模型；底层传输可选
  **tcp / udp / shm**，控制面用静态路由或广播式 `ntk_discovery`。
- **DDS 主干**：`participant / topic / writer / reader`，参考 ROS 2 的对象模型；
  `domain / partition / prefix / QoS` 是一等公民，后端可选 **内建 RTPS** 或
  **Cyclone DDS**，不需要 RPC node 或广播发现。
- **共享底座**：缓冲区/分配器、日志、指标、错误码、payload 视图和 JSON 读取器。

两条主干在目录、库目标、公共头和配置上都相互独立；应用可以只用其中一条，也可以
同时使用。

## 特性

- **两条独立主干。** RPC（tcp/udp/shm）与 DDS（rtps/cyclone）不共享 node、
  binding、发现或 QoS 模型，各自有独立的目录与静态库（`nb_rpc` / `nb_dds`）。
- **RPC 调用形态。** `nb_proxy_call_async`、`nb_proxy_call_sync`、
  `nb_proxy_call_event`，以及客户端流、服务端流和双向流。
- **DDS 原生语义。** topic 名原样上线（可加 `prefix`），domain/partition/QoS
  逐级继承；样本不再套 RPC 信封。
- **默认不额外起线程。** RPC 回调在 ntk poller 线程上执行；阻塞 handler 可以显式
  选择 `WORKER` 线程池或调用方提供的 `EXECUTOR`。
- **可组合的 RPC 传输。** UDP 带分片、NACK/RTX 和 RTCP 反馈；TCP 使用长度前缀
  分帧；SHM 支持零拷贝 loan/borrow。
- **统一的 endpoint 模型。** RPC 路由、发现和 provider 共享归一化 URI。
- **可观测性。** 指标、延迟分位数和运行期日志开关。
- **稳定的错误码。** 数值、名称、描述、可重试标记，以及保留给应用错误的区间。

## 快速开始

```c
#include <nb/nb.h>

static void on_response(int status, const nb_response_t *resp, void *user)
{
    (void)user;
    if (status == NB_OK) {
        /* resp->payload 只在这个回调内有效 */
    }
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t payload = { "hi", 2, 0 };
    nb_call_options_t options = NB_CALL_OPTIONS_INIT;

    nb_node_config_default(&cfg);
    cfg.name = "demo";
    cfg.mode = NB_NODE_MODE_STATIC;
    if (nb_node_create(&cfg, &node) != NB_OK) {
        return 1;
    }
    if (nb_proxy_create(node, NULL, &proxy) != NB_OK) {
        nb_node_destroy(node);
        return 1;
    }

    options.timeout_ms = 1000;
    (void)nb_proxy_call_async(proxy,
                              "EchoService",
                              "echo",
                              &payload,
                              &options,
                              on_response,
                              NULL);
    (void)nb_node_run_once(node, 1000);

    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return 0;
}
```

完整可运行程序见 [`examples/`](examples/)，索引见[示例文档](docs/examples.md)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [SDK API](docs/api/sdk-api.md) | 对象模型、生命周期、线程与所有权 |
| [架构](docs/architecture.md) | endpoint、message、QoS、mesh 与 provider 分层 |
| [配置](docs/api/config.md) | JSON 字段、默认值与校验 |
| [错误码](docs/api/error-codes.md) | 稳定数值、名称、描述与重试语义 |
| [RTPS 公共接口](docs/rtps-api.md) | 内建 RTPS 引擎的对象模型、QoS 与读写语义 |
| [示例](docs/examples.md) | 按场景索引 |
| [测试](docs/testing.md) | unit / e2e / perf / stability 门禁 |

### 头文件

| 头文件 | 使用场景 |
| --- | --- |
| `<nb/nb.h>` | 完整 API（base + rpc + dds） |
| `<nb/base.h>` | 模型无关的底座：payload、错误、日志、指标、缓冲区 |
| `<nb/rpc.h>` | RPC：node、service、proxy、stream、endpoint |
| `<nb/dds.h>` | DDS：participant、topic、writer、reader、QoS、status |
| `<nb/rpc/config.h>` | RPC JSON 配置 |
| `<nb/dds/config.h>` | DDS JSON 配置 |
| `<nb/rpc/transport_provider.h>` | 自定义 RPC 传输 provider |
| `<nb/rtps/rtps.h>` | 内建 RTPS 引擎的独立公共 API |

公共头文件只存在于 `<nb/...>` 下，并按 `base/`、`rpc/`、`dds/` 分目录。

## 目录结构

```text
include/nb/
  base.h rpc.h dds.h nb.h   三个聚合头与总入口
  base/                     无模型依赖：alloc/buf/error/log/metrics/types
  rpc/                      node/service/proxy/message/endpoint/config/transport
  dds/                      participant/topic/writer/reader/qos/status/backend
  internal/                 不安装
include/rtps/                RTPS 引擎独立公共 API（不含 nb 头）
src/base/                    共享基础设施 + JSON 读取器
src/rpc/                     RPC 主干：runtime/service/proxy/message/router/
                             registry/discovery/transport(tcp,udp,shm)/config
src/dds/                     DDS 主干：participant/topic/writer/reader/history/
                             waitset/qos/backend(rtps,cyclone)/config
src/rtps/                    RTPS 协议引擎与 rtps_* API
examples/                    01_rpc、02_rpc_streaming、03_dds_pubsub、04_config
docs/                        API、配置、错误和测试文档
tests/                       单元与端到端测试
3rdparty/                    ntk、elog、cJSON、uthash、Cyclone DDS
```

库目标：`nb_base`、`nb_json`、`nb_log`、`ntk_discovery`、`rtps_core`、`rtps`、
`nb_rpc`、`nb_dds`，以及链接两条主干的便利目标 `nb`。`nb_rpc` 不链接
`rtps`/`ddsc`，`nb_dds` 不链接 `ntk_discovery`。

## 构建

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -L unit --output-on-failure
```

Windows：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug -L unit --output-on-failure
```

选项：

```text
NB_BUILD_TESTS       构建测试（默认 ON）
NB_BUILD_TOOLS       构建压测客户端和基准服务端（默认 ON）
NB_BUILD_EXAMPLES    构建示例（默认 ON）
NB_BUILD_GTESTS      构建 GoogleTest 模块测试（默认 OFF）
NB_BUILD_BENCHMARKS  构建 Google Benchmark 微基准（默认 OFF）
NB_ENABLE_DDS        启用 Cyclone DDS provider（默认 ON，从 3rdparty/cyclonedds 构建）
NB_SANITIZE          address | undefined | thread | address,undefined
```

不带前缀的旧名称（`BUILD_TESTS`、`ZERO_RPC_SANITIZE` 等）仍然接受，但会输出
弃用警告。

库目标是 `nb`（别名 `nb::nb`），也可以按主干链接 `nb_rpc` / `nb_dds`。

## 测试与质量门禁

`tools/gate.py` 是唯一入口，其分类与 CTest 标签一致：

```bash
python3 tools/gate.py unit
python3 tools/gate.py e2e --transports tcp,udp,dds,rtps
python3 tools/gate.py perf --perf-duration 10
python3 tools/gate.py stability --transports tcp --stability-duration 60
python3 tools/gate.py all
```

通过标准、产物布局和新增测试的规则见 [docs/testing.md](docs/testing.md)。

## 约定

- 库拥有的函数、类型和变量使用 `nb_` 前缀；宏和枚举常量使用 `NB_`。不透明
  句柄写作 `struct nb_node` 和 `nb_node_t`。
- 枚举常量遵循 `NB_<TYPE>_<VALUE>`（例如 `NB_NODE_MODE_STATIC`）。
- service 和 method 名称是路由键，受 `NB_SERVICE_NAME_MAX` 和
  `NB_METHOD_NAME_MAX` 限制。
- 消息默认上限 1 MiB；UDP 分片默认 1440 字节（IPv4 1500 MTU 减去 60 字节
  头部）。
- 请求和响应视图只在对应回调内有效；需要保留请自行复制。
- 发出的载荷在调用返回前会被复制，调用方可以立即释放自己的缓冲区。
- `nb_proxy_call_sync` 会阻塞，不能在 poller 回调内使用；它返回的响应必须用
  `nb_response_release` 释放。
- 框架状态码和应用状态码位于不同字段，不要混用。

## 性能

吞吐、延迟和尖峰场景由 `tools/gate.py perf` 和 `tools/gate.py stability`
驱动，底层使用 `tools/bench_server` 和 `tools/loadgen`。

## 许可证

Apache License 2.0。见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。
