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
  "threads": { "io": 2 },
  "rpc": {
    "mode": "static",
    "listen": ["tcp://127.0.0.1:42440"],
    "routes": [{ "service": "EchoService", "endpoint": "tcp://127.0.0.1:42440" }]
  }
}
```

配置按用法分组，字段名直接描述含义：

- `threads` / `limits`：线程与并发上限；
- `rpc`：`mode`、`listen`（监听 URI 列表）、`discovery`、`routes`、链路调优；
- `pubsub`：`backends`（发布/订阅后端 URI 或对象）、`default_qos`；
- `metrics` / `log`：运行期开关。

完整字段、取值范围和错误路径见 [`docs/api/config.md`](../../docs/api/config.md)，
编辑器校验使用 [`docs/config.schema.json`](../../docs/config.schema.json)。
