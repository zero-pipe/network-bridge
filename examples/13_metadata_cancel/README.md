# 13: Metadata, Deadline and Cancellation

The example demonstrates three things:

1. The client sends request metadata through a `_with_metadata` API and the
   server returns response metadata with `nb_reply_with_metadata`.
2. The handler reads the absolute deadline derived from the client timeout with
   `nb_call_deadline_ms`.
3. The client cancels an in-flight slow call with `nb_proxy_cancel`; the local
   callback receives `NB_ERR_NOTFOUND`.

```powershell
.\example_13_metadata_cancel.exe
```

Cancellation is best-effort: a handler that is already running is not
preempted and must exit cooperatively through `nb_call_canceled`.
