# 02: Service and Synchronous Handler

Start a TCP server that registers `HealthService.check`. The handler uses the
synchronous style: it writes an `nb_message_view_t` and returns the application
status, and the framework sends the reply asynchronously.

```powershell
.\example_02_service.exe [port]
```

The default listener is `tcp://127.0.0.1:42410`. Keep it running and call it
with [`05_sync_call`](../05_sync_call/README.md).
