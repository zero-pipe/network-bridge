# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Refactoring toward two semantic spines over one shared base (see
  `docs/architecture-split.md`). Milestone M1: the public headers are grouped
  as `<nb/base.h>` (model-neutral), `<nb/rpc.h>` and `<nb/pubsub.h>`; `<nb/nb.h>`
  is now only their union and must not gain declarations of its own. Internal
  facilities (`nb/poolset.h`, `nb/slotpool.h`, `nb/iobuf.h`,
  `nb/poller_alloc.h`) moved to `include/nb/internal/` and are no longer
  included by the umbrella header or installed. `contract.nb_headers` enforces
  both rules. Source-compatible for applications that include `<nb/nb.h>` or the
  individual public headers.
- Milestone M2 (naming layer): the DDS policy set is now `nb_ps_qos_t` with
  `nb_ps_qos_init/inherit/is_default/validate/validate_reason`, making it
  explicit that these policies belong to the pub/sub spine and are never read
  by point-to-point RPC links. `nb_qos_t` and the `nb_qos_*` functions remain as
  source-compatible aliases (hidden by `NB_NO_DEPRECATED_API`). The config and
  provider structs now spell the type as `nb_ps_qos_t`; layout is unchanged.
- Milestone M3/M4: a first-class pub/sub spine next to RPC. `include/nb/pubsub.h`
  now declares `nb_participant_t`, `nb_topic_t`, `nb_writer_t` and
  `nb_reader_t`: participants attach to a topic-capable binding, topics carry a
  name + advisory type name + QoS, writers publish samples, and readers are
  either callbacks or a KEEP_LAST history drained with `nb_reader_read` /
  `nb_reader_take`. Native topic names are used verbatim on the wire (no
  `<Service>_<method>_evt` suffix); topic names may not contain '.', so the
  receive path can route topics and RPC events without a per-message flag.
  Providers gain an optional `register_topic(instance, topic, roles)` op with
  `NB_PS_TOPIC_WRITER` / `NB_PS_TOPIC_READER` roles; the `dds`, `rtps` and
  `shm` providers implement it, the provider ABI version is now 2, and a
  provider built against an older header cannot carry the spine. Wait sets can
  now observe reader histories with `nb_waitset_attach_reader` /
  `nb_waitset_detach_reader` (a callback reader has no history and is rejected).
  The legacy `nb_publisher_*` / `nb_subscriber_*` aliases still ride the RPC
  path and keep working; `tests/test_pubsub.c` covers callback, history
  read/take, wait-set attachment, the wire topic name, and both the RTPS and
  Cyclone DDS backends.
- The reader-history calls on a service method (`nb_service_add_queue_method`,
  `nb_service_set_queue`, `nb_service_read`, `nb_service_take`,
  `nb_service_queue_size`, `nb_service_queue_dropped`) moved from
  `<nb/service.h>` to `<nb/service_history.h>`, included by `<nb/pubsub.h>` and
  `<nb/nb.h>`. `<nb/rpc.h>` no longer pulls in `<nb/sample.h>` or declares the
  reader-history APIs; `contract.nb_headers` now enforces that. Sources that
  include `<nb/nb.h>` or `<nb/pubsub.h>` are unaffected.
- The provider SPI is split by role. `nb_transport_provider_ops_t` keeps the
  lifecycle (`create/start/stop/destroy`) and the optional shared
  `metrics_read`, and adds two role interfaces: `rpc` (`send`, `reply`, and the
  optional `loan/send_loan/release_loan`) and `ps` (`register_topic` required,
  `register_method/unregister_method` for the RPC-over-PS bridge, optional
  `discovery_read`). A provider must implement at least one complete role; the
  core asks for the role it needs and returns `NB_ERR_NOT_SUPPORTED` when it is
  absent. The `dds`, `rtps` and `shm` providers implement both roles. This is a
  breaking change to the ops layout: `NB_TRANSPORT_PROVIDER_ABI_VERSION` is now
  3 and a provider must rebuild against the matching header.
- The two DDS-shaped providers now share their route/topic naming and envelope
  wrapping through `src/transport/common/dds_bridge.c` (`nb_bridge_topic_key`,
  `nb_bridge_route_topics`, `nb_bridge_frame_size`, `nb_bridge_frame_encode`)
  instead of each carrying its own copy. No wire change.
- The configuration is split by spine. `nb_node_transport_config_t` is replaced
  by `nb_rpc_config_t` (mode, link tuning, discovery, routes, peers), and
  `nb_node_config_t` is now `runtime + rpc + bindings + pubsub + metrics + log`.
  Bindings moved to the top level because both spines share one transport
  instance; each binding declares `roles` (`"rpc"` / `"ps"`, absent = provider
  default) and carries `ps_qos` instead of `qos`. `pubsub.default_qos` is the
  inherited pub/sub default. JSON moves `mode`, `transport` tuning, `discovery`
  and `routes` under `rpc`, and `transport.bindings` to the top-level
  `bindings`. A role the transport cannot implement fails node creation with
  `NB_ERR_NOT_SUPPORTED`. `docs/config.schema.json` and
  `docs/api/config.md` were rewritten; no legacy keys are accepted.
- Examples were reduced from 15 directories to four that mirror common RPC and
  DDS habits: `01_rpc` (unary sync + async, server/client), `02_rpc_streaming`
  (client streaming in one process), `03_dds_pubsub` (topic/writer/reader with
  callback and history readers over `dds`, `rtps` or `shm`) and `04_config`
  (JSON-configured server/client). The build lists, install rules and
  `docs/examples.md` were updated; the old numbered examples were removed.
- Source layout now mirrors the two spines. The former `src/core/` grab bag is
  split into `src/runtime/` (node, inbound dispatch, binding assembly, shared
  message helpers), `src/rpc/` (service, proxy, stub, pending, dedup, reply,
  stream, sync, loan, service table, cancel table) and `src/pubsub/` (pubsub
  objects, reader history, wait sets, discovery view). `service.c` was split
  into registration (`service.c`), request execution (`service_dispatch.c`),
  server streams (`service_stream.c`) and the cancel table (`call_cancel.c`).
  The remaining pre-0.2 spellings were deleted: the `response_fn` /
  `handler_fn` / `service_interceptor_fn` typedefs, the `NB_MODE_*` /
  `NB_INTRA_*` / `NB_TRANSPORT_*` / `NB_STREAM_*` enum aliases and the
  `NB_METRIC_*` aliases. Two unused discovery helpers were removed.
- Configuration files were redesigned around how they are used: `threads`,
  `limits`, `rpc` (`mode`, `listen`, `udp`, `discovery`, `routes`), `pubsub`
  (`backends`, `default_qos`), `metrics` and `log`. Endpoints are plain URIs
  under `rpc.listen` / `pubsub.backends` instead of the previous abstract
  binding objects, and durations accept `"250ms"` / `"2s"`. The same endpoint
  in both lists is one transport serving both spines. `config.c` was split into
  parsing (`config.c`) and defaults/validation (`config_defaults.c`); the
  unused `codec_json` helper was removed.
- Provider options are split by role: the pub/sub QoS and the DDS status
  callback now live in `nb_transport_provider_options_t.ps`
  (`nb_ps_backend_options_t`), which is NULL for an RPC-only binding. The
  provider ABI version is now 4.
- `nb_message_t.sample` became `nb_message_t.ps_sample`, a callback-scoped
  pointer, so RPC messages no longer embed DDS instance/view state.

### Removed

- The pre-spine pub/sub spelling is gone. Removed headers: `<nb/publish.h>`,
  `<nb/subscribe.h>` and `<nb/service_history.h>`. Removed APIs:
  `nb_publisher_*`, `nb_subscriber_*`,
  `nb_service_add_queue_method` / `nb_service_set_queue` / `nb_service_read` /
  `nb_service_take` / `nb_service_queue_size` / `nb_service_queue_dropped`,
  `nb_waitset_attach` / `nb_waitset_detach` (service/method), and the
  `nb_qos_t` / `nb_qos_*` aliases. Use `nb_participant_t` / `nb_topic_t` /
  `nb_writer_t` / `nb_reader_t`, `nb_reader_read` / `nb_reader_take`,
  `nb_waitset_attach_reader`, `nb_ps_qos_t` and `nb_ps_qos_*` instead.
  Zero-copy publish moved to the RPC spine as `nb_proxy_loan` /
  `nb_proxy_publish_loan` / `nb_proxy_loan_release` in `<nb/proxy.h>`, and
  `nb_reader_dropped` was added for the history drop counter. The deprecated
  `NB_QOS_BEST_EFFORT` / `NB_QOS_RELIABLE` / `NB_QOS_VOLATILE` /
  `NB_QOS_TRANSIENT_LOCAL` / `NB_QOS_KEEP_LAST` / `NB_QOS_KEEP_ALL` enum
  spellings are gone; use the `NB_QOS_RELIABILITY_*`, `NB_QOS_DURABILITY_*` and
  `NB_QOS_HISTORY_*` names.

### Added

- RTPS standard-alignment work: the receive path now parses INFO_SRC and
  INFO_DST and drops Entity Submessages addressed to another participant
  (an all-zero destination prefix is treated as broadcast); sequence numbers
  are 64-bit end to end internally while the wire keeps the standard high/low
  halves; and `rtps_participant_config_t.standard_ports` / `participant_id`
  select the DDSI PB/DG/PG port mapping (d0/d1/d3) with a separate user-data
  unicast socket. The user-data multicast port (d2) is not allocated yet.
  Unit tests cover INFO_DST/SRC round trips, sequence numbers crossing 2^32,
  and the standard port formulas. The framework provider selects the same mode
  with `rtps://<domain>?std&pid=<n>`. The Fast DDS probe also passes with
  `standard_ports=1` (participant_id=10) in both directions.
- `docs/rtps-standard-alignment.md` compares the in-tree RTPS implementation
  against OMG DDSI-RTPS 2.5, section by section, and records the verified
  Cyclone DDS matrix. Fast DDS 3.6.2 basic interoperability was verified
  separately in both directions over UDPv4: default/best-effort and explicit
  reliable QoS, 16 B, 1 KiB and 200 KiB payloads, DATA_FRAG reassembly,
  TRANSIENT_LOCAL late join in both directions, publisher restart, and a
  two-Fast-DDS-participant plus one-nb-participant case (the publisher waits
  for `--matched 2`; the nb participant uses explicit unicast peers).
  The remaining gaps (INFO_DST receive filtering, XTypes/TypeObject, IPv6
  locators, multi-participant port mapping, content filters and group ordering)
  are listed there. Fast DDS omits `PID_RELIABILITY` for its default reliable
  writer; the project keeps the standard behavior (missing PID means
  BEST_EFFORT) rather than adding a vendor-specific heuristic, so a Fast DDS
  writer must announce a non-default max_blocking_time to match a reliable
  reader. `tools/fastdds_interop/` keeps the manual probe sources and notes.
- Fast DDS 2.5.0 (Ubuntu 22.04 packages) interoperability was re-verified with
  the same matrix, including 1 KiB and 200 KiB payloads, late join, restart,
  multi-participant and standard ports. The probe gained `RTPS_TEST_PEER_HOST`,
  `RTPS_TEST_NO_PEERS`, `FDDS_TL`, `FDDS_MATCHED` and `FDDS_PROFILE` so the
  loopback, durability and match-count cases can be driven without editing the
  sources. `docs/rtps-standard-alignment.md` section 6.5 records the run and
  the reader-window fix it uncovered.
- A standalone discovery library, `libntk_discovery`, that depends only on ntk.
  It exchanges opaque key/value pairs over UDP broadcast on one hop; the RPC
  framework uses service names as keys and peer endpoints as the value's
  companion data. Wire format v5 adds a CRC32, a per-process instance id, an
  epoch and a sequence number, so stale, reordered and post-restart frames are
  ignored; large announcements are split into frames and reassembled. Peers and
  keys live in open-addressing hash tables sized from configuration, replacing
  the fixed `peers[64]` arrays and the duplicate registry copy.
- `tests/test_ntk_discovery.c` exercises the library without the framework:
  discovery, value update, withdraw, a 40-key announcement that needs
  fragmentation, and BYE removal.
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

- The project README and all documentation under `docs/` are now written in
  Chinese: the SDK API, architecture, configuration reference, error codes,
  examples index, testing and performance guides, plus the JSON Schema
  descriptions. Code blocks, identifiers and generated artifacts are unchanged;
  relative links were verified and the obsolete `rtps-api-todo.md` link was
  removed. The performance guide now also records the same-host cross-process
  SHM comparison (1 KB, one call in flight).
- The quality gate now covers RTPS as a first-class transport:
  `tools/gate.py` accepts `--transports rtps` for e2e/perf/stability,
  `bench_server --rtps <binding-uri>` and
  `loadgen --rtps <binding-uri> [--rtps-target <uri>]` use explicit unicast
  SPDP peers so no multicast is required. The proxy target reuses the RTPS
  binding authority (the domain id) because custom transports are matched by
  their bind host.
- The client stub is now an internal call engine instead of a second public
  client API. `<nb/stub.h>` was removed (use `<nb/proxy.h>`); the request
  descriptor and the proxy-facing call entries moved to
  `src/core/stub_internal.h`, and the unused public wrappers were deleted.
  This is a source-level cleanup only: no wire or behavior change. A new
  `contract.nb_headers` test keeps `nb_stub` out of the installed headers.
- Cyclone DDS is now built from source. `3rdparty/cyclonedds` keeps a
  source-only tree (Cyclone DDS 11.0.1, upstream `3a552631`) instead of the
  four prebuilt platform SDKs (`windows_amd64`, `linux_amd64`,
  `linux_arm64`, `linux_loongarch64`). The top-level build adds it with
  `add_subdirectory()` and links the static `ddsc` target through the
  `nb_cyclonedds` interface target; security, iceoryx and LTO stay off by
  default, and the DDS provider no longer needs `ddsc.dll` next to the
  executables or an `LD_LIBRARY_PATH` entry.

### Fixed

- RTPS: a VOLATILE reliable reader could drop a large fragmented sample when the
  writer's HEARTBEAT overtook the sample's last DATA_FRAG. The heartbeat opened
  the reader window at `lastSN + 1`, so the completed sample fell behind the
  window, was discarded, and was acknowledged as received (so the writer never
  retransmitted it). The window now stops at an already in-flight fragment
  assembly, which proves the sample arrived after the match. Found while
  re-verifying against Fast DDS 2.5.0 with 200 KiB samples; Cyclone DDS and the
  Fast DDS 3.6.2 runs were unaffected.
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
