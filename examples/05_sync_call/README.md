# 05: Synchronous Call

Call `HealthService.check` from [`02_service`](../02_service/README.md). The
core is still asynchronous RPC; `nb_proxy_call_sync` is only a blocking
wrapper and must not be used inside a poller callback.

Start the server first:

```powershell
.\example_02_service.exe
```

Then run the client:

```powershell
.\example_05_sync_call.exe [port]
```

The default port is `42410`. On success the client prints the echoed payload.
