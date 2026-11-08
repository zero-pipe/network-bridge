# 02 RPC 流式：分块上传

单进程演示 client-streaming：客户端分三块上传，服务端收齐后一次回复。

```bash
./example_02_rpc_streaming
```

三种流形态共用一套接口：

| 形态 | 客户端 | 服务端 |
| --- | --- | --- |
| client streaming | `nb_proxy_open_client_stream` + `nb_proxy_stream_send` | `nb_service_add_stream_method` + `nb_reply*` |
| server streaming | `nb_proxy_call_server_stream` | `nb_reply_stream`（多次） |
| bidirectional | `nb_proxy_open_bidi_stream` + `nb_proxy_stream_send` | `nb_reply_stream`（边收边回） |

每个 chunk 带 `NB_STREAM_FLAG_FIRST` / `LAST`；服务端也可以用
`NB_SERVICE_EXEC_MODE_WORKER` 把慢处理移出 poller 线程（`nb_service_add_stream_method_with_options`）。
