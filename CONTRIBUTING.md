# Contributing to Network Bridge

Thanks for taking the time to contribute. This document covers the build, the
test gates, and the naming and layout conventions used in the repository.

## Getting started

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -L unit --output-on-failure
python3 tools/gate.py e2e
```

On Windows use the Visual Studio generator and pass `--config` to `ctest`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug -L unit --output-on-failure
```

Third-party sources live under `3rdparty/`. Do not modify them in place; send
upstream patches to the respective project instead.

## Tests

Every change must keep `tools/gate.py unit` green. Transport or protocol
changes should also run `tools/gate.py e2e --transports tcp,udp,shm` (add
`dds` when Cyclone DDS is available). Performance-sensitive changes should
include the output of `tools/gate.py perf` in the pull request.

New tests:

1. Add `tests/test_<area>.c`.
2. Register it in `tests/CMakeLists.txt` with `nb_add_test(<target>
   <ctest-name> LABELS "unit;<area>" ...)`.
3. Keep unit tests free of sockets and real threads; use the `e2e` label
   otherwise.

## Naming

The library prefix is `nb` for lower-case identifiers and `NB_` for macros and
enum constants.

| Kind | Convention | Example |
| --- | --- | --- |
| Public function | `nb_<object>_<verb>` | `nb_proxy_call_async` |
| Public type | `nb_<name>_t` | `nb_call_options_t` |
| Opaque handle | `struct nb_<name>` + `nb_<name>_t` typedef | `struct nb_node` |
| Enum type | `nb_<name>_t` | `nb_node_mode_t` |
| Enum constant | `NB_<TYPE>_<VALUE>` | `NB_NODE_MODE_STATIC` |
| Macro / limit | `NB_<NAME>` | `NB_SERVICE_NAME_MAX` |
| Internal symbol | `nb_` prefix, declared in `src/**` headers | `nb_node_resolve` |
| Static helper | `nb_` + file-specific verb | `nb_udp_session_open` |

Use `_owned` for functions that take ownership of a payload, `_init` for
in-place initializers, `_release` for explicit cleanup, and `_create` /
`_destroy` for allocating lifecycles.

### Include guards

Every header uses `NB_<PATH>_H`, for example `include/nb/proxy.h` ->
`NB_PROXY_H`, `src/runtime/node_internal.h` -> `NB_NODE_INTERNAL_H`.

### Header layout

Public headers live in `include/nb/`, one per area of the API (`node.h`,
`service.h`, `proxy.h`, `endpoint.h`, `codec.h`, ...). The umbrella header is
`<nb/nb.h>`. There is exactly one copy of each header: no flat
`<runtime.h>`-style forwarding headers and no role-header aggregators, so every
include is `<nb/...>` and names the header that actually declares the API.

Internal headers stay next to their implementation under `src/` and are never
installed.

## Layout

```text
include/nb/     public API
src/base/       dependency-free primitives (nb_base library)
src/runtime/    shared runtime: node, inbound dispatch, binding assembly
src/rpc/        RPC spine: service, proxy, stub, stream, pending, retransmit
src/pubsub/     pub/sub spine: pubsub objects, reader history, wait sets
src/message/    wire framing and envelope
src/transport/  common framing + udp / tcp / shm / dds drivers
src/router/     static routes and mesh resolution
src/registry/   local service registry
src/discovery/   service discovery
src/config/     JSON config model and defaults
src/utils/      logging, errors, metrics, endpoint, JSON codec
```

Place new code in the lowest layer that can support it. `src/base` must not
depend on ntk or on any other framework directory.

## Style

- C99, four-space indentation, 100-column soft limit.
- Run `clang-format` (see `.clang-format`) before committing.
- Public headers and all new user-facing text are written in English.
- Chinese comments in existing internal files are acceptable; translate them
  when you touch the surrounding code.
- Prefer early returns and `goto cleanup` for multi-resource functions.
- Do not add threads to the default path; the framework builds on the ntk
  poller and only creates handler or send-lane threads when configured.

## Commits and pull requests

Use [Conventional Commits](https://www.conventionalcommits.org/):

```text
feat(transport): add udp coalescing
fix(proxy): avoid double release on timeout
docs: translate architecture guide
refactor!: rename rpc target to nb
```

`!` marks a breaking change. Keep commits focused; describe the observable
behaviour change and the tests you ran in the pull request.

By contributing you agree that your contribution is licensed under the
Apache License 2.0 (see `LICENSE`).
