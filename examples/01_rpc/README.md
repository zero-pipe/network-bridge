# 01 RPC：最小可用服务

和 gRPC 的心智模型一致：服务端注册 `Service.method`，客户端按名字调用，
框架负责寻址、编组、超时和响应关联。

```bash
# 终端 1
./example_01_rpc_server          # tcp://0.0.0.0:42420
# 终端 2
./example_01_rpc_client          # 同步调用 + 异步调用
./example_01_rpc_client 10.0.0.5 42420
```

要点：

- 服务端只需 `nb_service_create` + `nb_service_add_sync_method`。
- 客户端用 `nb_proxy_call_sync` / `nb_proxy_call_async`；响应回调在 poller
  线程上运行。
- 本例用 `nb_node_add_static_route` 指向固定端点。把客户端改成
  `NB_NODE_MODE_MESH` 并调用 `nb_node_wait_for_service(node, "EchoService", 5000)`，
  客户端就不再需要知道服务端地址。
- payload 对框架不透明：这里直接回显字节；业务对象可以用 `<nb/rpc/codec.h>`
  注册 codec，或用 JSON/Protobuf 自己编解码。
