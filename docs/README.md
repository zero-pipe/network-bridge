# Network Bridge Documentation

- [SDK API](api/sdk-api.md): object model, lifetime, threading and ownership.
- [Architecture](architecture.md): endpoint / message / QoS / mesh / provider
  layering.
- [Architecture overview](architecture-overview.html): single-page Chinese
  overview - ntk runtime, discovery, UDP/TCP/SHM, RTPS and the third-party
  dependencies, with the layer and data-path diagrams.
- [Discovery review](discovery-review.md): the broadcast key/value discovery
  design, its review findings and the standalone `ntk_discovery` library.
- [RTPS protocol handbook](rtps-protocol.html): plain-language walkthrough of
  the wire format, SPDP/SEDP flows, QoS matching and reliable transfer
  (single-file HTML, opens in a browser).
- [RTPS implementation specification](rtps-spec.md): the normative reference
  derived from `src/rtps/` - message and ParameterList formats, discovery,
  QoS, reliability, fragmentation, limits and the Cyclone DDS interop matrix.
- [RTPS API plan](rtps-api-todo.md): the public `rtps_*` interface layer -
  topic/writer/reader model, QoS inheritance, phases and progress checkboxes.
- [RTPS API reference](rtps-api.md): object model, QoS inheritance, read/take
  semantics, threading rules and the mapping to Cyclone DDS `dds_*`.
- [Configuration](api/config.md): JSON fields, defaults and validation.
- [Error codes](api/error-codes.md): stable values, names, descriptions and
  retry semantics.
- [Examples](examples.md): runnable programs indexed by scenario.
- [Testing](testing.md): the unified unit / e2e / perf / stability / fuzz gates.
- [Low latency and spikes](performance.md): latency methodology, spike
  attribution experiments, CPU pinning and inline dispatch results.

Editor validation for configuration files uses
[`config.schema.json`](config.schema.json).

Design rationale and layering trade-offs are summarised in the repository
[README](../README.md).
