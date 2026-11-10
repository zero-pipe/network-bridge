# 示例索引

四个自包含示例，分别对应 RPC 与 DDS 两种最常见的使用习惯。每个目录都包含
源码、`CMakeLists.txt` 和 `README.md`，可以直接运行。

| 目录 | 场景 | 运行方式 |
| --- | --- | --- |
| [01_rpc](../examples/01_rpc/README.md) | 最小 RPC：服务端注册方法，客户端同步 + 异步调用 | 服务端 + 客户端 |
| [02_rpc_streaming](../examples/02_rpc_streaming/README.md) | RPC 流式：分块上传 | 单进程 |
| [03_dds_pubsub](../examples/03_dds_pubsub/README.md) | DDS 发布/订阅：topic、writer、回调/history reader | 单进程 |
| [04_config](../examples/04_config/README.md) | JSON 配置驱动 RPC node 与 DDS participant | 服务端 + 客户端，或单进程 DDS |

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

示例可执行文件生成在构建树的 `examples/` 下。`04_config` 使用的 JSON
文件会自动复制到可执行文件旁边。`03_dds_pubsub` 默认使用内建 RTPS 后端；`cyclone` 后端需要 `NB_ENABLE_DDS=ON`（默认开启）。

## 阅读顺序

先看 `01_rpc` 了解 service/proxy 的基本形状，再看 `02_rpc_streaming` 了解流式
调用；随后看 `03_dds_pubsub` 了解 topic/writer/reader 和 QoS；最后看
`04_config` 了解如何用 JSON 组织生产配置。

更细的能力（metadata、取消、WORKER 线程池、codec、mesh 发现、SHM 零拷贝）
不单独占一个示例，API 说明见 [`api/sdk-api.md`](api/sdk-api.md)。
