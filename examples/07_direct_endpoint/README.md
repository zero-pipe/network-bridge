# 07: Direct Endpoint

The client sets its target address with `nb_proxy_set_target_uri`, bypassing
service discovery and the routing table.

```powershell
.\example_07_direct_endpoint_server.exe
.\example_07_direct_endpoint_client.exe
```

The default address is `tcp://127.0.0.1:42430`. Unlike a static route, a direct
endpoint overrides the proxy target instead of registering a service name in the
node.
