# 10: Payload Codec

Register an application-level `nb_codec_t` that encodes `order_t` into the text
form `"id|name"` and decodes it again. The framework only forwards bytes and the
`encoding` tag and never interprets the business format.

```powershell
.\example_10_payload_codec.exe
```

The program prints the encoded payload and the decoded order fields. Production
code can replace the text format with JSON, protobuf or a custom IDL.
