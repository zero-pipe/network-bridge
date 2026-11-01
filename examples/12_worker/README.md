# 12: Worker Pool

The server sets `runtime.handler_threads = 2` and registers a blocking handler
with `NB_SERVICE_EXEC_MODE_WORKER`. The handler sleeps on a worker thread while
the poller keeps processing network events, and the reply is posted back to the
original poller.

```powershell
.\example_12_worker.exe
```

Compare with the default INLINE mode: the same handler would stall the whole
poller while sleeping.
