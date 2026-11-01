# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- A public `rtps_*` interface layer over the RTPS engine, so the wire code and
  the API can evolve independently. `include/rtps/` defines opaque
  participant/topic/writer/reader/waitset handles, plain-data GUID/locator/
  sample types, a QoS struct with explicit presence bits, status, discovery and
  metric snapshots. `src/rtps/api/` implements it on top of `src/rtps` and maps
  errors at the boundary, so public headers never mention `nb_*` types.
- Topics are first-class entities: a name, a type name and a QoS template owned
  by a participant. Writers and readers are created from a topic and their
  effective QoS resolves as endpoint > topic > DDS default. Topics are not RTPS
  entities and nothing about them is announced on the wire; SEDP keeps carrying
  the resolved endpoint QoS, so the protocol is unchanged. A duplicate
  (name, type) returns the same handle, a conflicting type is rejected, and
  destroying a topic destroys its endpoints.
- Reader history and read/take: `rtps_reader_enable_history` keeps a bounded
  KEEP_LAST queue of copies, `rtps_reader_read` returns borrowed views and marks
  them READ, `rtps_reader_take` returns owned copies released with
  `rtps_sample_release`, and `rtps_waitset_*` blocks until an attached reader
  matches or the timeout expires. A data callback can be combined with a
  history.
- `rtps_writer_write_instance` publishes keyed samples and dispose/unregister
  state changes; `rtps_participant_discovery_read`, `rtps_participant_metrics`
  and `rtps_participant_tcp_port` expose the observability paths the framework
  provider needs.
- `tests/test_rtps_api.c` exercises the public API only: two participants over
  explicit unicast peers, topic identity and type conflict, QoS inheritance,
  publish/receive, read/take, wait set wake-up, keyed dispose, discovery and
  metrics.
- The `rtps` transport provider now consumes only the public `rtps_*` API.
  Topics, writers and readers are created through the interface layer, reader
  callbacks locate their endpoint by handle instead of by GUID table, and
  status, discovery and metrics are mapped at the boundary. A
  `contract.rtps_provider_api` ctest scans the provider source and fails if it
  reaches into the engine again, so the interface/protocol split is enforced
  rather than documented.
- `rtps_participant_config_t` gained `data_heartbeat_ms` and
  `data_history_depth`, which the provider uses to keep its 250 ms reliable
  heartbeat and the engine's default history depth.
- The RTPS stack is built as separate static libraries: `rtps_core` (the
  engine), `rtps` (the public `rtps_*` API), and `nb_log` (the logger shared
  with the framework). `nb` links `rtps`, so every existing consumer keeps
  working, while a program that only needs DDS can link `rtps` and
  `rtps_core`. Engine headers moved to `src/rtps/internal/`, are not installed,
  and only the `include/rtps/` API is public. `cmake --install` now produces
  `libnb`, `libnb_log`, `librtps`, `librtps_core` and the `rtps/` header tree.
- Publisher and subscriber grouping entities, mirroring DDS: they carry a QoS
  template for their writers/readers and own them, and the effective QoS is
  resolved as endpoint > publisher/subscriber > topic > DDS default.
  `rtps_writer_create_on` / `rtps_reader_create_on` attach an endpoint to a
  group; the direct `rtps_writer_create` / `rtps_reader_create` forms remain.
- `rtps_writer_write_batch` writes several bodies in order, and
  `rtps_reader_loan` / `rtps_reader_return_loan` expose the borrowed history
  views under the DDS loan vocabulary (read() already returned them). A true
  writer-side loan is not offered because the engine copies payloads into its
  history and sockets synchronously.
- Three contract tests guard the interface boundary: the transport provider may
  not use engine symbols, public headers may not mention `nb_*` and every
  `RTPS_API` declaration must be `rtps_*`, and the `rtps` library may only
  export `rtps_*` symbols.

- RTPS: a Cyclone DDS interoperability matrix. `tests/test_rtps_cyclone.c`
  now covers, in addition to the original discovery and DATA checks: best-effort
  in both directions, `TRANSIENT_LOCAL` late joiners in both directions, one nb
  writer feeding two Cyclone readers, a Cyclone participant that restarts, and
  200 KiB samples that need hundreds of fragments. Both multicast and
  unicast-only modes pass against Cyclone DDS 11.0.1.
- RTPS: `docs/rtps-spec.md`, the implementation specification derived from
  `src/rtps/`. It records the message and ParameterList formats, discovery
  flow, QoS table and RxO rules, reliability and fragment-repair state
  machines, timers, limits, the verified Cyclone matrix and the known
  deviations. `docs/README.md` links it next to the plain-language handbook.

### Changed

- Cyclone DDS is now built from source. `3rdparty/cyclonedds` keeps a
  source-only tree (Cyclone DDS 11.0.1, upstream `3a552631`) instead of the
  four prebuilt platform SDKs (`windows_amd64`, `linux_amd64`,
  `linux_arm64`, `linux_loongarch64`). The top-level build adds it with
  `add_subdirectory()` and links the static `ddsc` target through the
  `nb_cyclonedds` interface target; security, iceoryx and LTO stay off by
  default, and the DDS provider no longer needs `ddsc.dll` next to the
  executables or an `LD_LIBRARY_PATH` entry.

### Fixed

- RTPS: the send-side counters for DATA, DATA_FRAG, HEARTBEAT, ACKNACK, GAP and
  NACK_FRAG were incremented inside the send-failure branch after an
  unconditional `return NB_ERR_IO`, so they stayed at zero and the metrics for a
  healthy participant reported `data_sent = 0`. The increments now run after a
  successful send.
- RTPS: an ACKNACK was built with the local reader and the remote writer
  swapped when selecting the `INFO_DST` prefix, so every solicited repair was
  addressed to the sender itself and Cyclone ignored it. Discovery samples were
  unaffected, which hid the defect behind retransmissions.
- RTPS: a one-sequence-number GAP was encoded with `bitmapBase = 0`, which
  Cyclone rejects as a malformed submessage. Every ordinary KEEP_LAST eviction
  produced one. GAPs now use a contiguous range (`bitmapBase = gapStart + 1`,
  empty bitmap) and the reader accepts that form too.
- RTPS: SEDP endpoint removal used only the DATA `K` flag. Cyclone routes
  discovery samples by `PID_STATUS_INFO`, so the dispose looked like a live
  re-announcement and remote endpoints never disappeared. A dispose now sends a
  key-only payload (`PID_ENDPOINT_GUID` only) with `PID_STATUS_INFO = 1`.
- RTPS: the HEARTBEAT liveliness flag was `0x02`, which is `FINAL`; the `L` bit
  is `0x04`. Manual-by-topic liveliness therefore never reached a Cyclone peer,
  and every ordinary Cyclone heartbeat looked like a liveliness assertion.
- RTPS: DATA_FRAG key-only samples set the DATA `K` flag `0x08` instead of the
  DATA_FRAG `K` flag `0x04`.
- RTPS: an ACKNACK that requests samples always set `FINAL`, telling the writer
  not to answer. `FINAL` is now set only on a pure acknowledgement.
- RTPS: `PID_RESOURCE_LIMITS` encoded "unlimited" as `0`; DDS uses
  `LENGTH_UNLIMITED (0xffffffff)`, and Cyclone discards the whole ParameterList
  when it sees zero. Both directions convert between the API spelling and the
  wire value.
- RTPS: the SPDP parser required a UDP unicast locator and dropped participants
  that advertise only a TCP locator. It now accepts a TCP-only announcement.
- RTPS: a SEDP sample that failed to parse was never acknowledged, so a peer
  retransmitted it forever. The sequence number is now consumed before the
  payload is interpreted, matching what a reader does with any sample it
  cannot use.
- RTPS: a reliable writer stopped sending HEARTBEATs once its newest retained
  sample had exceeded its lifespan, leaving a reader with a hole no way to learn
  that the sample would never arrive. Heartbeats are now sent for an empty or
  fully expired history too; the writer answers the resulting request with GAP.
- RTPS: a reader window opened at the first DATA sample's sequence number, so a
  reordered older sample was dropped and a durable late joiner never requested
  the history before it. Durable subscriptions now open at sequence number 1;
  VOLATILE subscriptions open at the first sample (by DATA) or at `lastSN + 1`
  (by HEARTBEAT) and therefore no longer pull samples that predate the match.
- RTPS: partition lists. SEDP now sends a comma-separated local partition list
  as a DDS string sequence and joins a received sequence back into the same
  representation, so a peer that announces several names matches the way DDS
  specifies.
- RTPS: optional SPDP/SEDP fields that do not fit the local fixed-size record
  (participant name, USER_DATA, partition element) are truncated or skipped
  instead of invalidating the whole participant or endpoint.
- RTPS: fragment size raised from 1024 to 1028. RTPS requires a multiple of
  four greater than 1024, and a strict peer discards 1024-byte fragments as
  malformed. The publish limit is now 1 MiB and the receive reassembly budget
  4 MiB, so samples far larger than one datagram work in both directions.
- RTPS: the TCPv6 locator kind is `8`, matching RTPS 2.5 and Cyclone (it was
  `5`); no TCPv6 code path exists, but a peer's locator is classified
  correctly.

### Removed

- The flat forwarding headers in `include/` (`<nb.h>`, `<types.h>`,
  `<runtime.h>`, ...). They existed only to keep the old un-namespaced paths
  compiling, and every one of them was a one-line `#include <nb/...>`. Public
  headers now live in `include/nb/` only, so there is one copy of each header
  and the include path matches the installed layout. Consumers that still use
  the flat form switch to `<nb/...>`, which is a mechanical rename.
- The role-header aggregators `<nb/runtime.h>`, `<nb/server.h>`,
  `<nb/client.h>`, `<nb/transport.h>` and `<nb/extensions.h>`. Each of them
  bundled two to five real headers, so the API had two entry points per area
  and a reader could not tell which file declared a symbol. They are replaced
  by the headers they included: `runtime.h` -> `node.h`; `server.h` ->
  `service.h` (plus `message_view.h`); `client.h` -> `proxy.h` (plus `stub.h`,
  `publish.h`); `transport.h` -> `endpoint.h`, `transport_provider.h`;
  `extensions.h` -> `codec.h`, `service_table.h`. The security note that only
  lived in `transport.h` moved into `transport_provider.h`. Examples, tests and
  the benchmark tools were updated, so nothing in the tree depends on them.

## [0.4.0] - 2026-11-01

RTPS over TCP. Discovery stays on UDP, as the spec requires; everything after
it - SEDP, user data, heartbeats, acknowledgements - follows the peer onto a
stream once both sides have advertised a TCP locator.

### Added

- TCP locators: `nb_rtps_locator_kind_t` gains `TCPV4` (4) and `TCPV6` (5),
  and the SPDP sample carries a second `PID_METATRAFFIC_UNICAST_LOCATOR` with
  the TCP port. Participants that do not enable TCP send exactly what they sent
  before, so a mixed deployment keeps working over UDP.
- `rtps_tcp` transport module: listener, connection table with per-connection
  receive assembly and a transmit queue, `tcp+<n>` peer keys so the existing
  send/reliability paths need no special case, and byte/message/link
  counters. The wire format is the RTPS one - the 20-octet header followed by
  back-to-back submessages with no additional framing - so the stream carries
  exactly what a datagram would.
- Initiator election on GUID prefix order: the lower prefix dials and the other
  side accepts, so a pair never opens two connections and neither side needs a
  configuration knob to break the tie. A failed dial is retried on the sweep
  interval instead of waiting for the next discovery announcement.
- `enable_tcp` and `tcp_port` in `nb_rtps_participant_config_t` (`tcp_port` 0
  asks for an ephemeral port, which is then advertised), plus
  `nb_rtps_participant_tcp_enabled/tcp_port/tcp_links/tcp_stats()` to read back
  the listener and its counters.
- `?tcp` / `?tcp=<port>` on an `rtps://` binding, for example
  `rtps://7:47130/127.0.0.1:47131?tcp=47300`. Existing bindings are unchanged.

### Changed

- `handle_datagram()` was split into a socket reader and
  `handle_submessage()`; both the datagram path and the stream parser now feed
  the same dispatcher, so reliability, fragmentation and dispose behave the
  same on both transports.

## [0.3.0] - 2026-11-01

First release with the DDS alignment work: QoS policies and RxO matching,
instances and keys, read/take with masks and wait sets, discovery views,
and the RTPS operations surface.

### Added

- Apache-2.0 license, NOTICE, CONTRIBUTING, SECURITY and CODE_OF_CONDUCT.
- SEDP now announces the QoS policies that were previously accepted but
  never published: deadline, latency budget, lifespan, liveliness, ownership
  with strength, and destination order. They are decoded into remote endpoint
  info and applied to reader/writer matching (offered must be at least what is
  requested). Defaults are omitted from the wire, so existing endpoints are
  unchanged.
- QoS mismatches are reported at debug level with the policy that failed
  (`topic`, `type`, `partition`, `reliability`, `durability`, `deadline`,
  `latency budget`, `ownership`, `liveliness`, `destination order`).
- Fragment repair: writers emit `HEARTBEAT_FRAG` for fragmented samples and
  retransmit only the fragments a reader asks for in `NACK_FRAG`. A lost
  fragment no longer forces a whole-sample retransmit; readers ask after 60 ms
  of quiet, retry up to four times, and fall back to the existing 2 s assembly
  timeout.
- Multi-locator endpoints: a sample is sent to every unicast locator a peer
  announces instead of only the first, matching Cyclone DDS's address-set
  behaviour. Both reliable and best-effort readers now deduplicate by writer
  sequence number, so a sample that arrives through several routes is delivered
  once.
- Lifespan is enforced. A retained sample older than its writer's lifespan is no
  longer retransmitted: `ACKNACK` and `NACK_FRAG` requests for it are answered
  with `GAP`, late joiners do not receive it, and the periodic `HEARTBEAT` is
  suppressed once the newest sample has expired.
- Deadline is monitored, both offered and requested. A publication that does not
  write within `deadline`, and a subscription that receives nothing from a
  matched writer within `deadline`, raise a status event through the new
  `nb_rtps_participant_set_status_callback()`. Events are rate limited to one
  per period and carry a running miss count. Requests are armed when the pair
  matches, so a writer that never writes is still detected.
- Automatic liveliness leases are asserted and monitored with
  ParticipantMessageData on the built-in writer/reader (`0x000200c2` /
  `0x000200c7`, now advertised in `PID_BUILTIN_ENDPOINT_SET`). A publication
  with a finite automatic lease asserts at half that lease, and a subscription
  raises `NB_RTPS_STATUS_LIVELINESS_CHANGED` when a matched writer stops
  asserting within its lease. The event carries the writer, whether it is alive,
  and the `alive_count`/`not_alive_count` pair. Manual liveliness still needs an
  explicit assert operation, so it is announced as automatic.
- Exclusive ownership is applied on the reader side. An `EXCLUSIVE` subscription
  delivers only the matched publication with the highest `ownership_strength`
  (equal strengths are broken by GUID so every participant picks the same
  writer), and hands the topic to the next strongest writer when the owner's
  automatic liveliness lease lapses or the publication is withdrawn.
- `BY_SOURCE_TIMESTAMP` destination order is honoured. The receiver keeps the
  `INFO_TS` of each sample and discards one whose source timestamp is not newer
  than the last sample delivered to that subscription, while
  `BY_RECEPTION_TIMESTAMP` keeps arrival order. Retransmissions and late-joiner
  replays reuse the original timestamp instead of restamping the sample.
- `TimeBasedFilter` is enforced on subscriptions through a new
  `nb_qos_t.min_separation_ms` field, announced as `PID_TIME_BASED_FILTER`
  (the id Cyclone DDS uses). A subscription drops samples stamped less than the
  separation after the last one it delivered, per topic, while still
  acknowledging them on the reliable channel. Publications announce the value
  only, as Cyclone does. Samples without an `INFO_TS` are never filtered.
- DDS sample metadata is part of the public message: `nb_message_t.sample`
  carries the sample state, the 16-octet instance handle from `PID_KEY_HASH`,
  and the writer source timestamp in nanoseconds.
  `nb_sample_info_valid_data()` maps the state onto DDS `SampleInfo::valid_data`.
  `NB_SAMPLE_STATE_VALID` is zero, so providers that do not model samples keep
  reading as ordinary data. The RTPS provider decodes `PID_STATUS_INFO` and
  delivers a dispose or key-only sample as a not-alive sample without a payload
  instead of dropping it.
- DDS status events reach the application: `<nb/status.h>` defines
  `nb_status_event_t` and `nb_transport_provider_options_t.on_status`. The RTPS
  provider reports publication and subscription matches (with the current and
  cumulative match counts, so a drop means a peer went away), QoS
  incompatibility with the policy that failed, and the existing deadline and
  liveliness events. `endpoint` is the method the local endpoint belongs to.
- DDS instances on subscriptions. The RTPS reader keeps a per-subscription
  instance table keyed by the 16-octet handle from `PID_KEY_HASH`, and
  `nb_sample_info_t` now reports `instance_state`
  (`ALIVE`/`NOT_ALIVE_DISPOSED`/`NOT_ALIVE_NO_WRITERS`), `view_state`
  (`NEW` for the first sample of an instance and after a re-registration),
  `generation_rank`, and `reception_timestamp_ns`. A dispose moves the instance
  to not-alive and the next live sample re-registers it. Types whose samples
  carry no key hash are delivered without per-instance state.
- `ResourceLimitsQosPolicy` (`max_samples`, `max_instances`,
  `max_samples_per_instance`) and `WriterDataLifecycleQosPolicy`
  (`autodispose_unregistered_instances`) are part of `nb_qos_t`, parsed from the
  config file, announced as `PID_RESOURCE_LIMITS`, and mapped onto Cyclone DDS.
  A subscription enforces `max_instances` against its instance table and rejects
  samples beyond it.
- Sample lost and rejected status events: `NB_STATUS_SAMPLE_LOST` counts samples
  a `GAP` withdrew (cumulative, per subscription) and
  `NB_STATUS_SAMPLE_REJECTED` counts samples the reader dropped for resource
  limits, `TimeBasedFilter`, source ordering or exclusive ownership, with the
  policy in `reason`.
- DDS instances on publications. `nb_rtps_participant_publish_instance()` writes
  a keyed sample with the handle in `PID_KEY_HASH`; `PID_STATUS_INFO` (1 disposed,
  2 unregistered) marks a state change, which goes out key-only and is retransmitted
  with the same bits, so a reliable reader or a late joiner never sees a dispose
  as data. `WriterDataLifecycleQosPolicy::autodispose` disposes the instances that
  are still alive when a publication is deleted, and a remote writer that
  unregisters moves the instances of its subscriptions to
  `NOT_ALIVE_NO_WRITERS`. The transport provider forwards an outbound message with
  `sample.state != VALID` and `sample.has_instance_handle` onto the same path.
- `PresentationQosPolicy` (`presentation_scope`, `coherent_access`,
  `ordered_access`) is part of `nb_qos_t`, announced as `PID_PRESENTATION`,
  checked for RxO compatibility (the writer must offer at least the reader's
  access scope and access modes), parsed from the config file and mapped onto
  Cyclone DDS. Grouping samples into coherent/ordered sets needs read/take, so
  the provider only enforces the compatibility rule and announces the policy.
- Manual liveliness. Writing asserts the writer, so a `MANUAL_BY_PARTICIPANT`
  publication sends a manual ParticipantMessageData sample and a
  `MANUAL_BY_TOPIC` one sends a HEARTBEAT with the L flag, which is also how
  Cyclone DDS asserts a writer's liveliness. Readers renew each matched writer
  by the kind it announced (participant messages for automatic and
  manual-by-participant, the writer's own data or L-flagged heartbeat for
  manual-by-topic) and report `LIVELINESS_CHANGED` for every kind with a finite
  lease.
- DDS read/take. A subscription can keep a reader history instead of a
  callback: `nb_subscriber_subscribe_queue()` registers a KEEP_LAST history
  (default depth 32, oldest sample dropped and counted on overflow), and
  `nb_subscriber_read()` / `nb_subscriber_take()` drain it. `<nb/sample.h>`
  defines the DDS state mask (`NB_STATE_*`), an instance-handle filter and
  `nb_sample_t`; `read()` returns borrowed views (the loan path), `take()`
  returns owned copies released with `nb_sample_release()`. The generic
  service surface is `nb_service_add_queue_method()` / `nb_service_read()` /
  `nb_service_take()` / `nb_service_queue_size()` /
  `nb_service_queue_dropped()`. A method that only has a history answers
  REQUESTs with `NB_ERR_NOTFOUND`.
- Wait sets: `<nb/waitset.h>` (`nb_waitset_create/attach/detach/wait/ready`)
  blocks until an attached queued subscription has a sample matching the
  filter or the timeout expires. Wake-up is event driven, so a blocked waiter
  returns on the enqueue rather than after a polling interval.
- Discovery views: `<nb/discovery_view.h>` exposes the DDS built-in topics as
  snapshots. `nb_node_discovery_read()` returns the discovered participants,
  publications, subscriptions or topics (the last derived from the endpoint
  tables, with writer/reader counts). Endpoint entries carry topic, type,
  partition and the announced QoS, so a node can report what it sees without a
  typed built-in reader. Providers opt in through the new optional
  `discovery_read` op in `nb_transport_provider_ops_t`; the RTPS provider
  implements it.
- RTPS statistics. `nb_metrics_t` gains an RTPS scope
  (`NB_METRIC_SCOPE_RTPS`) covering datagrams and bytes, SPDP/SEDP, DATA,
  HEARTBEAT, ACKNACK, GAP and fragments, parse errors, rejected samples,
  expired leases, and participant/publication/subscription gauges.
  `nb_node_metrics()` collects them through the new optional `metrics_read`
  provider op, and `nb_rtps_participant_metrics()` reads a participant
  directly. Set `NB_RTPS_DUMP_FRAMES=1` (or `dump_frames` in the participant
  config) to log every datagram, first 48 octets in hex, at debug level.
- GitHub Actions CI for Linux (GCC, Release) and Windows (MSVC, Debug).
- `.clang-format`, `.editorconfig` and `.gitattributes` for consistent style and
  line endings.
- Canonical umbrella header `<nb/nb.h>`; `<nb.h>` remains a forwarding header.
- `NB_DEPRECATED` macro and a documented deprecation policy.
- `src/base/` for dependency-free primitives (alloc, buf, poolset, slotpool,
  crc32), matching the `nb_base` library boundary.

### Changed

- **Breaking:** the CMake library target is now `nb` (was `rpc`). The alias
  `rpc` and the imported name `nb::nb` remain available during the transition.
- **Breaking:** CMake options use the `NB_` prefix (`NB_BUILD_TESTS`,
  `NB_BUILD_TOOLS`, `NB_BUILD_EXAMPLES`, `NB_BUILD_GTESTS`,
  `NB_BUILD_BENCHMARKS`, `NB_BUILD_FUZZERS`). Legacy names still work but warn.
- **Breaking:** the sanitizer option is `NB_SANITIZE` (was
  `ZERO_RPC_SANITIZE`); the old name is still accepted with a warning.
- **Breaking:** enum constants now follow `NB_<TYPE>_<VALUE>`:
  - `NB_MODE_*` -> `NB_NODE_MODE_*`
  - `NB_INTRA_*` -> `NB_INTRA_PROCESS_*`
  - `NB_TRANSPORT_*` -> `NB_TRANSPORT_KIND_*`
  - `NB_STREAM_*` -> `NB_STREAM_FLAG_*`
  - `NB_METHOD_*` -> `NB_METHOD_KIND_*`
  - `NB_SERVICE_EXEC_*` -> `NB_SERVICE_EXEC_MODE_*`
  - `NB_STUB_*` -> `NB_STUB_KIND_*`
  - `NB_METRIC_COUNTER/GAUGE/HISTOGRAM` -> `NB_METRIC_TYPE_*`

  Old spellings remain as macros and can be hidden with
  `NB_NO_DEPRECATED_API`.
- Public function signatures use the `nb_`-prefixed callback typedefs
  (`nb_response_fn`, `nb_handler_fn`, ...). The flat typedefs remain for
  compatibility.
- Default node name and log tag changed from `"rpc"` to `"nb"`; default POSIX
  shared-memory names use the `nb-shm-` prefix instead of `zrpc-shm-`.
- Internal include guards are consistently `NB_*_H`.

### Fixed

- README linked a non-existent `docs/BENCH-STATUS.md`; it now points at
  `docs/performance.md`.
- **Breaking:** removed the `include/qos.h` compatibility header. On Windows it
  shadowed the SDK's own `qos.h`, so any translation unit that includes
  `winsock2.h` (or `windows.h`) together with this project's include directory
  failed with an incomplete `FLOWSPEC` definition. Use `<nb/qos.h>`.
- The `rtps` transport provider announces a routable interface address instead
  of `127.0.0.1`, so `rtps://<domain>` works between hosts with no address
  configuration, as `dds://<domain>` does.
- The `rtps` reader dropped a dispose or key-only `DATA` before its receive
  window advanced, so the sequence number was never acknowledged and a reliable
  writer retransmitted the sample forever. Such samples are now consumed like
  any other and skipped just before delivery.

## [0.2.0] - 2026-01-01

Initial public snapshot of the heterogeneous network framework: node/service/
proxy abstractions, UDP/TCP/SHM/Cyclone DDS transports, RPC, pub/sub,
streaming, routing, discovery, QoS, metrics and the unified quality gate.

[Unreleased]: https://github.com/zero-pipe/network_bridge/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/zero-pipe/network_bridge/releases/tag/v0.2.0
