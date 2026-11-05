# Network Bridge

[![CI](https://github.com/zero-pipe/network_bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/zero-pipe/network_bridge/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Network Bridge 是一个面向异构网络的 C99 通信框架。它在可插拔的传输 provider
之上提供统一的 `node / service / proxy / payload` 模型，并在其上叠加 RPC、
消息、路由、服务发现和流式能力。传输、序列化和寻址都是可替换的实现细节。

内建传输：UDP、TCP、POSIX 共享内存（SHM）、Cyclone DDS 和内置 RTPS。

## 特性

- **异构传输。** UDP、TCP、SHM、Cyclone DDS 和 RTPS 共用一套 provider 接口；
  自定义传输无需改动核心代码即可接入。
- **三种调用形式。** `nb_proxy_call_async`、`nb_proxy_call_sync` 和
  `nb_proxy_call_event`，并支持客户端流、服务端流和双向流。
- **默认不额外起线程。** 回调在 ntk poller 线程上执行；阻塞 handler 可以显式
  选择 `WORKER` 线程池或调用方提供的 `EXECUTOR`。
- **可组合的传输能力。** UDP 带分片、NACK/RTX 和 RTCP 反馈；TCP 使用长度前缀
  分帧；SHM 支持零拷贝 loan/borrow。
- **统一的 endpoint 模型。** 路由、发现、registry 和 provider 共享归一化 URI，
  因此 SHM channel 和 DDS domain 可以端到端保留。
- **载荷保持不透明。** 框架只转发字节和 `encoding` 标签；序列化通过
  `nb_codec_t` 扩展。
- **统一路由。** 静态路由和 mesh 发现写入同一张路由表。
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
| [示例](docs/examples.md) | 按场景索引 |
| [测试](docs/testing.md) | unit / e2e / perf / stability / fuzz 门禁 |
| [性能](docs/performance.md) | 延迟测量方法与尖峰分析 |
| [RTPS 标准对照](docs/rtps-standard-alignment.md) | 对照 RTPS 2.5 的实现程度与 Cyclone/Fast DDS 互通评估 |

### 头文件

| 头文件 | 使用场景 |
| --- | --- |
| `<nb/nb.h>` | 需要完整 API |
| `<nb/node.h>` | node、调度、发现、路由、指标 |
| `<nb/service.h>` | service、handler、reply、message view |
| `<nb/proxy.h>` | proxy、异步/同步/事件/流式调用 |
| `<nb/publish.h>`、`<nb/subscribe.h>` | topic、sample 和 reader |
| `<nb/endpoint.h>` | endpoint 与自定义传输 provider（`<nb/transport_provider.h>`） |
| `<nb/codec.h>` | 载荷 codec、message view 和 service table |
| `<nb/config.h>` | JSON 配置 |

公共头文件只存在于 `<nb/...>` 下：`include/nb/` 是公共头文件的唯一位置，
因此构建时的 include 路径和安装后的布局完全一致。

## 目录结构

```text
include/nb/   公共 API（总入口：<nb/nb.h>）
src/base/     无外部依赖的基础设施（nb_base 库）
src/core/     node、dispatch、service、proxy、stream、pending
src/message/  线上组帧与信封
src/transport/ 通用组帧以及 udp / tcp / shm / dds 驱动
src/router/   静态路由与 mesh 解析
src/registry/ 本地服务注册表
src/discovery/ 服务发现
src/config/   JSON 配置模型与默认值
src/utils/    日志、错误、指标、endpoint、JSON 编解码
examples/     按场景分组的可运行程序
docs/         API、配置、错误和测试文档
tests/        单元与端到端测试
tools/        质量门禁、压测客户端和基准服务端
3rdparty/     ntk、elog、cJSON、uthash、Cyclone DDS（源码树）
```

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
NB_BUILD_FUZZERS     构建 libFuzzer 目标（默认 OFF，需要 Clang）
NB_ENABLE_DDS        启用 Cyclone DDS provider（默认 ON，从 3rdparty/cyclonedds 构建）
NB_SANITIZE          address | undefined | thread | address,undefined
```

不带前缀的旧名称（`BUILD_TESTS`、`ZERO_RPC_SANITIZE` 等）仍然接受，但会输出
弃用警告。

库目标是 `nb`（别名 `nb::nb`）。历史目标名 `rpc` 仍保留为别名，供尚未迁移的
下游项目使用。

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

完整约定见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 性能

吞吐、延迟和尖峰场景由 `tools/gate.py perf` 和 `tools/gate.py stability`
驱动，底层使用 `tools/bench_server` 和 `tools/loadgen`。测量方法和已记录的
结果见 [docs/performance.md](docs/performance.md)。

## 许可证

Apache License 2.0。见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。

安全问题报告见 [SECURITY.md](SECURITY.md)。
