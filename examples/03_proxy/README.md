# 03: Proxy Lifecycle

Create only a node and a proxy to show the client-side create and destroy
order. A proxy owns no thread of its own; calls run on the node's pollers.

```powershell
.\example_03_proxy.exe
```

The program destroys both objects immediately and sends no request.
