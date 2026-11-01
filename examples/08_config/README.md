# 08: JSON Configuration

Create nodes from JSON files. The server listens on `tcp://127.0.0.1:42440` and
the client calls `EchoService.echo` through a static `routes` entry.

After building, the two `.json` files are copied next to the executables. Start
the server first, then the client:

```powershell
.\example_08_config_server.exe [server.json]
.\example_08_config_client.exe [client.json]
```

Without arguments they read `server.json` and `client.json`. See the
[configuration reference](../../docs/api/config.md) for every field.
