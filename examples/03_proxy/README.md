# 03: Proxy Lifecycle

Create only a node and a proxy to show the client-side create and destroy
order. A proxy is built on top of a stub and owns no thread of its own.

```powershell
.\example_03_proxy.exe
```

The program destroys both objects immediately and sends no request.
