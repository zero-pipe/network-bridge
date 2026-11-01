# 11: Streaming Upload

The client uploads chunks with `nb_proxy_stream_send`; the server sends one
combined response after receiving the chunk marked `NB_STREAM_FLAG_LAST`. Both
nodes run in the same process and connect directly over TCP, with a
system-assigned server port.

```powershell
.\example_11_streaming.exe
```

The example needs no other process and is a good place to observe
`NB_STREAM_FLAG_FIRST` / `NB_STREAM_FLAG_LAST`, chunk offsets and stream
lifetime.
