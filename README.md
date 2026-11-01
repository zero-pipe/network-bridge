# Network Bridge

[![CI](https://github.com/zero-pipe/network_bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/zero-pipe/network_bridge/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Network Bridge is a C99 communication framework for heterogeneous networks. It
exposes one `node / service / proxy / payload` model on top of pluggable
transport providers, and adds RPC, messaging, routing, service discovery and
streaming on top. Transports, serialization and addressing are replaceable
implementation details.

Built-in transports: UDP, TCP, POSIX shared memory (SHM) and Cyclone DDS.

## Features

- **Heterogeneous transports.** One provider interface for UDP, TCP, SHM and
  Cyclone DDS; custom transports plug in without touching core code.
- **Three call forms.** `nb_proxy_call_async`, `nb_proxy_call_sync` and
  `nb_proxy_call_event`, plus client, server and bidirectional streaming.
- **No threads by default.** Callbacks run on the ntk poller thread. Blocking
  handlers opt into a `WORKER` pool or a caller-provided `EXECUTOR`.
- **Composable transports.** UDP includes fragmentation, NACK/RTX and RTCP
  feedback; TCP uses length-prefixed framing; SHM offers zero-copy loan/borrow.
- **One endpoint model.** Routing, discovery, registry and providers share a
  normalized URI, so SHM channels and DDS domains are preserved end to end.
- **Payloads stay opaque.** The framework forwards bytes and an `encoding` tag;
  serialization is a `nb_codec_t` extension.
- **Unified routing.** Static routes and mesh discovery feed the same table.
- **Observability.** Metrics, latency quantiles and a runtime log switch.
- **Stable error codes.** Numeric values, names, descriptions, a retryable flag
  and a reserved range for application errors.

## Quick start

```c
#include <nb/nb.h>

static void on_response(int status, const nb_response_t *resp, void *user)
{
    (void)user;
    if (status == NB_OK) {
        /* resp->payload is valid only inside this callback */
    }
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t payload = { "hi", 2, 0 };
    nb_call_options_t options = NB_CALL_OPTIONS_INIT;

    nb_node_config_default(&cfg);
    cfg.name = "demo";
    cfg.mode = NB_NODE_MODE_STATIC;
    if (nb_node_create(&cfg, &node) != NB_OK) {
        return 1;
    }
    if (nb_proxy_create(node, NULL, &proxy) != NB_OK) {
        nb_node_destroy(node);
        return 1;
    }

    options.timeout_ms = 1000;
    (void)nb_proxy_call_async(proxy,
                              "EchoService",
                              "echo",
                              &payload,
                              &options,
                              on_response,
                              NULL);
    (void)nb_node_run_once(node, 1000);

    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return 0;
}
```

Complete, runnable programs live in [`examples/`](examples/); see the
[example index](docs/examples.md).

## Documentation

| Document | Contents |
| --- | --- |
| [SDK API](docs/api/sdk-api.md) | Object model, lifetime, threading and ownership |
| [Architecture](docs/architecture.md) | Endpoint, message, QoS, mesh and provider layers |
| [Configuration](docs/api/config.md) | JSON fields, defaults and validation |
| [Error codes](docs/api/error-codes.md) | Stable values, names, descriptions and retry semantics |
| [Examples](docs/examples.md) | Index by scenario |
| [Testing](docs/testing.md) | unit / e2e / perf / stability / fuzz gates |
| [Performance](docs/performance.md) | Latency methodology and spike analysis |

### Headers

| Header | Use when |
| --- | --- |
| `<nb/nb.h>` | You want the complete API |
| `<nb/node.h>` | Node, scheduling, discovery, routing, metrics |
| `<nb/service.h>` | Services, handlers, replies, message views |
| `<nb/proxy.h>` | Proxies, async/sync/event/streaming calls (`<nb/stub.h>` for the low-level primitive) |
| `<nb/publish.h>`, `<nb/subscribe.h>` | Topics, samples and readers |
| `<nb/endpoint.h>` | Endpoints and custom transport providers (`<nb/transport_provider.h>`) |
| `<nb/codec.h>` | Payload codecs, message views and service tables |
| `<nb/config.h>` | JSON configuration |

Public headers live under `<nb/...>` and nowhere else: `include/nb/` is the
only place a public header exists, so the include path and the installed layout
are the same thing.

## Layout

```text
include/nb/   public API (umbrella: <nb/nb.h>)
src/base/     dependency-free primitives (nb_base library)
src/core/     node, dispatch, service, proxy, stream, pending
src/message/  wire framing and envelope
src/transport/ common framing plus udp / tcp / shm / dds drivers
src/router/   static routes and mesh resolution
src/registry/ local service registry
src/discovery/ service discovery
src/config/   JSON configuration model and defaults
src/utils/    logging, errors, metrics, endpoint, JSON codec
examples/     runnable programs grouped by scenario
docs/         API, configuration, error and testing documentation
tests/        unit and end-to-end tests
tools/        quality gate, load generator and benchmark server
3rdparty/     ntk, elog, cJSON, uthash, Cyclone DDS (source-only tree)
```

## Build

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -L unit --output-on-failure
```

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug -L unit --output-on-failure
```

Options:

```text
NB_BUILD_TESTS       build tests (default ON)
NB_BUILD_TOOLS       build the load generator and benchmark server (default ON)
NB_BUILD_EXAMPLES    build examples (default ON)
NB_BUILD_GTESTS      build GoogleTest module tests (default OFF)
NB_BUILD_BENCHMARKS  build Google Benchmark microbenchmarks (default OFF)
NB_BUILD_FUZZERS     build libFuzzer targets (default OFF, requires Clang)
NB_ENABLE_DDS        enable the Cyclone DDS provider (default ON, built from 3rdparty/cyclonedds)
NB_SANITIZE          address | undefined | thread | address,undefined
```

The unprefixed names (`BUILD_TESTS`, `ZERO_RPC_SANITIZE`, ...) are still
accepted but emit a deprecation warning.

The library target is `nb` (alias `nb::nb`). The historical target name `rpc`
remains as an alias for downstream projects that have not migrated yet.

## Testing and quality gates

`tools/gate.py` is the single entry point; its categories match the CTest
labels:

```bash
python3 tools/gate.py unit
python3 tools/gate.py e2e --transports tcp,udp,dds
python3 tools/gate.py perf --perf-duration 10
python3 tools/gate.py stability --transports tcp --stability-duration 60
python3 tools/gate.py all
```

See [docs/testing.md](docs/testing.md) for pass criteria, artifact layout and
the rules for adding tests.

## Conventions

- Library-owned functions, types and variables use `nb_`; macros and enum
  constants use `NB_`. Opaque handles are `struct nb_node` and `nb_node_t`.
- Enum constants follow `NB_<TYPE>_<VALUE>` (for example
  `NB_NODE_MODE_STATIC`).
- Service and method names are routing keys, limited by `NB_SERVICE_NAME_MAX`
  and `NB_METHOD_NAME_MAX`.
- Messages default to 1 MiB; UDP fragments default to 1440 bytes (IPv4 1500
  MTU minus 60 bytes of headers).
- Request and response views are valid only inside their callback; copy them
  if they must outlive it.
- Outgoing payloads are copied before the call returns; the caller may release
  its buffer immediately.
- `nb_proxy_call_sync` blocks and must not be used inside a poller callback.
  Responses it returns must be released with `nb_response_release`.
- Framework and application status codes travel in different fields.

The full conventions are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Performance

Throughput, latency and spike scenarios are driven by `tools/gate.py perf` and
`tools/gate.py stability`, backed by `tools/bench_server` and `tools/loadgen`.
Methodology and recorded results are in [docs/performance.md](docs/performance.md).

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Security reports: see [SECURITY.md](SECURITY.md).
