# 04 JSON 配置

生产环境的常见形态：线程、路由、QoS 都写在 JSON 里，代码只注册业务。配置按主干
分成 `rpc` 和 `dds` 两块，各自解析、互不依赖。

```bash
# RPC：终端 1 / 终端 2
./example_04_config_server server.json
./example_04_config_client client.json

# DDS：单进程演示
./example_04_config_dds dds.json

# 全量字段参考（同时含 rpc 与 dds 块）
./example_04_config_dds full.json
```

| 文件 | 内容 |
| --- | --- |
| `server.json` | 最小 RPC 服务端：tcp 监听 42440 |
| `client.json` | 最小 RPC 客户端：静态路由到 42440 |
| `dds.json` | 最小 DDS participant：rtps 后端、domain 70、prefix `app` |
| `full.json` | 全量字段参考；值都是合法值，不代表推荐值 |

## RPC 块

```json
{
  "name": "echo-server",
  "threads": { "io": 2 },
  "rpc": {
    "mode": "static",
    "listen": ["tcp://127.0.0.1:42440"],
    "routes": [{ "service": "EchoService", "endpoint": "tcp://127.0.0.1:42440" }]
  }
}
```

- `listen` 决定启动哪些传输（scheme = tcp/udp/shm），**客户端也要写**，端口用
  `0` 让系统分配；`routes` 优先于发现结果。
- `mode: "static"` 会强制关闭 discovery；`mesh` 下 `routes` 仍然生效。

## DDS 块

```json
{
  "dds": {
    "backend": "rtps",
    "domain": 70,
    "partition": "example",
    "prefix": "app",
    "qos": { "reliability": "reliable", "history": "keep_last", "depth": 4 },
    "rtps": { "bind_host": "127.0.0.1", "multicast_group": "" }
  }
}
```

- `domain` / `partition` / `prefix` / `qos` 是 DDS 的核心；`backend` 选
  `rtps` 或 `cyclone`。
- DDS 面不读 `threads`：后端有自己的线程与发现机制；`rpc` 块在这里被静默忽略。

完整字段、取值范围和错误路径见 [`docs/api/config.md`](../../docs/api/config.md)，
编辑器校验使用 [`docs/config.schema.json`](../../docs/config.schema.json)。
