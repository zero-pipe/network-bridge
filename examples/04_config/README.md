# 04 JSON 配置

生产环境的常见形态：绑定、线程、路由、QoS 都写在 JSON 里，代码只注册业务。

```bash
# 终端 1
./example_04_config_server        # 使用 server.json
# 终端 2
./example_04_config_client        # 使用 client.json
```

```json
{
  "runtime": { "io_threads": 2 },
  "rpc": {
    "mode": "static",
    "routes": [{ "service": "EchoService", "endpoint": "tcp://127.0.0.1:42440" }]
  },
  "bindings": ["tcp://127.0.0.1:42440"]
}
```

配置按主干分组：

- `rpc`：`mode`、链路调优、`discovery`、`routes`；
- `bindings`：两条主干共享的传输实例，每个可声明 `roles`（`"rpc"` / `"ps"`）
  和发布/订阅用的 `ps_qos`；
- `pubsub`：`default_qos`；
- `runtime` / `metrics` / `log`：两条主干共享。

完整字段、取值范围和错误路径见 [`docs/api/config.md`](../../docs/api/config.md)，
编辑器校验使用 [`docs/config.schema.json`](../../docs/config.schema.json)。
