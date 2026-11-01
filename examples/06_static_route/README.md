# 06: Static Route

The server listens on a fixed address and the client registers
`OrderService -> 127.0.0.1:42420` with `nb_node_add_static_route`, skipping
service discovery entirely.

```powershell
.\example_06_static_route_server.exe
.\example_06_static_route_client.exe
```

This suits single-host and test deployments, or environments that already have
a configuration service. When changing the port, update both the client and the
server.
