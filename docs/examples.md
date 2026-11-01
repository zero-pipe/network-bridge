# 示例索引

示例按学习路径排序。每个目录都是自包含的，包含源码、`CMakeLists.txt` 和
`README.md`。

| 目录 | 场景 | 运行方式 |
| --- | --- | --- |
| [01_hello_node](../examples/01_hello_node/README.md) | 创建/销毁 node | 单进程 |
| [02_service](../examples/02_service/README.md) | 带同步 handler 的 service | 服务端 |
| [03_proxy](../examples/03_proxy/README.md) | proxy 生命周期 | 单进程 |
| [04_service_table](../examples/04_service_table/README.md) | 共享方法声明表 | 单进程 |
| [05_sync_call](../examples/05_sync_call/README.md) | 同步调用 02_service | 客户端 |
| [06_static_route](../examples/06_static_route/README.md) | 静态路由 | 服务端 + 客户端 |
| [07_direct_endpoint](../examples/07_direct_endpoint/README.md) | proxy 直接绑定端点 | 服务端 + 客户端 |
| [08_config](../examples/08_config/README.md) | 从 JSON 构建 node | 服务端 + 客户端 |
| [09_service_discovery](../examples/09_service_discovery/README.md) | mesh 服务发现 | 服务端 + 客户端 |
| [10_payload_codec](../examples/10_payload_codec/README.md) | 应用 codec 往返 | 单进程 |
| [11_streaming](../examples/11_streaming/README.md) | 分块上传与流式回复 | 单进程 |
| [12_worker](../examples/12_worker/README.md) | WORKER handler 线程池 | 单进程 |
| [13_metadata_cancel](../examples/13_metadata_cancel/README.md) | metadata/超时/取消 | 单进程 |
| [14_dds_pubsub](../examples/14_dds_pubsub/README.md) | Cyclone DDS 事件发布/订阅 | 单进程 |
| [15_shm](../examples/15_shm/README.md) | SHM mesh RPC（Linux） | 服务端 + 客户端 |

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

示例可执行文件生成在构建树的 `examples/` 下。`08_config` 使用的两个 JSON
文件会自动复制到可执行文件旁边。

## 阅读顺序

先看 01、02、03 了解 node/service/proxy，再看 04/05 了解声明式契约与调用，
然后看 06/07 了解路由。最后看 08/09 和 10–15，覆盖配置、发现、编解码、
流式、并发语义和 DDS 发布/订阅。
