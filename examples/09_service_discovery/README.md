# 09: Mesh Service Discovery

The client knows only the server IP and discovery port; it does not configure a
data port, because the server endpoint is learned from mesh HELLO messages. The
defaults are TCP data port `42450`, server discovery port `41953` and client
discovery port `41954`.

```powershell
.\example_09_service_discovery_server.exe
.\example_09_service_discovery_client.exe
```

Server arguments: `[data_port] [discovery_port] [client_ip] [client_discovery_port]`.
Client arguments: `[server_ip] [client_discovery_port] [server_discovery_port]`.
When running across hosts, make sure the peer IP and discovery ports match.
