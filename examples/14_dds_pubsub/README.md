# 14: DDS Pub/Sub

Publish and subscribe with the built-in `dds` provider backed by Cyclone DDS.
The subscriber is a regular service whose method name is the topic; the
publisher is a regular proxy that sends one-way event calls.

```powershell
.\example_14_dds_pubsub.exe
```

The binding URI is `dds://<domain>`, where the host is the DDS domain; the port
may be omitted (the legacy `dds://<domain>:1` spelling is still accepted and the
provider ignores the port). The example enables `intra_process = "only"`:
events skip DDS serialization and the network, and the publisher hands the
message pointer directly to local subscribers of the same provider.

The topic is derived from the full route as `<Service>_<method>_evt`, with
non-alphanumeric characters replaced by `_`, so equally named methods on
different services never cross streams. Local delivery reuses the same route and
payload pointer.

The program publishes repeatedly until the subscriber receives a message, so the
first event is not lost before DDS discovery completes.

This example is intentionally single-process: `intra_process = "only"` verifies
the local fast path, not DDS network delivery. For a real independent Cyclone
subscriber, see `e2e.dds_remote`, which sets `intra_process = "off"` and runs the
publisher and subscriber in separate processes.
