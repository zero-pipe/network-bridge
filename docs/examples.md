# Example Index

Examples are ordered as a learning path. Each directory is self-contained with
its sources, `CMakeLists.txt` and `README.md`.

| Directory | Scenario | Run as |
| --- | --- | --- |
| [01_hello_node](../examples/01_hello_node/README.md) | create/destroy a node | single process |
| [02_service](../examples/02_service/README.md) | service with a synchronous handler | server |
| [03_proxy](../examples/03_proxy/README.md) | proxy lifecycle | single process |
| [04_service_table](../examples/04_service_table/README.md) | shared method declaration table | single process |
| [05_sync_call](../examples/05_sync_call/README.md) | synchronous call to 02_service | client |
| [06_static_route](../examples/06_static_route/README.md) | static routing | server + client |
| [07_direct_endpoint](../examples/07_direct_endpoint/README.md) | proxy bound to an endpoint | server + client |
| [08_config](../examples/08_config/README.md) | build nodes from JSON | server + client |
| [09_service_discovery](../examples/09_service_discovery/README.md) | mesh service discovery | server + client |
| [10_payload_codec](../examples/10_payload_codec/README.md) | application codec round trip | single process |
| [11_streaming](../examples/11_streaming/README.md) | chunked upload and streaming reply | single process |
| [12_worker](../examples/12_worker/README.md) | WORKER handler pool | single process |
| [13_metadata_cancel](../examples/13_metadata_cancel/README.md) | metadata/deadline/cancel | single process |
| [14_dds_pubsub](../examples/14_dds_pubsub/README.md) | Cyclone DDS event pub/sub | single process |
| [15_shm](../examples/15_shm/README.md) | SHM mesh RPC (Linux) | server + client |

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Example binaries are written under `examples/` in the build tree. The two JSON
files used by `08_config` are copied next to the executable automatically.

## Reading order

Start with 01, 02 and 03 to learn node/service/proxy, then 04/05 for
declarative contracts and calls, then 06/07 for routing. Finish with 08/09 and
10-15 for configuration, discovery, codecs, streaming, concurrency semantics
and DDS pub/sub.
