# Architecture and Abstractions

Network Bridge separates *call semantics* from *link implementations*. Upper
layers describe only `service.method + payload + metadata`; lower layers move a
single normalized message. Addressing, QoS, fragmentation and reply tokens are
all carried by shared framework abstractions.

## Layers

```text
application
  service / proxy / stream              call forms and lifetime
  codec / message_view                  payload encoding and views
core
  node / dispatch / pending / dedup     correlation, timeouts, retries, scheduling
  router / registry / discovery         service resolution and mesh control plane
transport SPI
  endpoint + message + QoS              transport-neutral contract
providers
  udp / tcp / shm / dds / rtps / custom link implementations
```

Dependencies point downward only. A transport provider never sees
service/proxy concepts and never parses payloads; core never depends on a
scheme-specific private address format.

## Endpoint

`nb_endpoint_t` is the only address representation, with fixed fields:

| Field | Meaning |
| --- | --- |
| `scheme` | provider selector, e.g. `udp` / `tcp` / `shm` / `dds` / `rtps` |
| `host` | provider authority: IP, DDS/RTPS domain or SHM channel |
| `port` | provider service id: UDP/TCP port, SHM node id, RTPS SPDP port, DDS placeholder |
| `path` | optional provider sub-path |
| `transport` | built-in transport kind; custom providers use `NB_TRANSPORT_KIND_CUSTOM` |
| `uri` | normalized URI used by routing, discovery, logs and cross-process transport |

Invariants:

- The route table, registry, discovery frames, proxy target overrides and
  provider send targets all use `nb_endpoint_t` instead of storing separate
  `ip/scheme/port` triples.
- `nb_endpoint_parse` normalizes; `nb_endpoint_refresh` rebuilds the URI after
  fields are modified.
- Only UDP/TCP authorities are network addresses. When discovery receives a
  HELLO, it rewrites the host from the packet source IP only for endpoints where
  `nb_endpoint_is_network()` is true. SHM channels and DDS domains must be
  preserved verbatim.

## Message

`nb_message_t` is both the internal core message and the public provider SPI
message (`include/nb/message.h`). Providers used to have a second struct that
`transport_factory.c` copied field by field, which made it easy to drop newly
added fields. There is now a single definition:

- Inbound: the provider fills `peer`, route, payload, metadata and stream;
  `frame_owner` can extend a zero-copy buffer's lifetime.
- `sample` carries DDS sample metadata: the sample state, the instance handle
  and the writer source timestamp. The RTPS provider fills it; other providers
  leave it zeroed, which reads as ordinary data because `NB_SAMPLE_STATE_VALID`
  is zero.
- Outbound: core fills `target`, route and payload; UDP/TCP use the derived
  `peer_host/peer_port` fast path.
- `reply_token` is a provider-private return path used only for replies.

## QoS

`nb_qos_t` (`include/nb/qos.h`) mirrors DDS policies; fields default to INHERIT
so they do not override provider defaults. Current mapping:

- reliability / durability / history / depth
- deadline / lifespan / latency budget
- liveliness / lease duration / ownership / partition

The DDS provider applies binding QoS when creating readers and writers.
UDP/TCP/SHM keep the configuration readable by applications but do not change
message semantics. A binding without explicit QoS inherits `transport.qos`.

## Status events

`nb_status_event_t` (`include/nb/status.h`) is the application-facing half of the
DDS status contract. A provider that can observe status fills
`nb_transport_provider_options_t.on_status` and reports on its own thread, with
the same `user` as `on_receive`:

- `NB_STATUS_PUBLICATION_MATCHED` / `NB_STATUS_SUBSCRIPTION_MATCHED`: `count` is
  the current number of matches, so a decrease is the DDS "peer went away"
  signal, and `total_count` is the cumulative count.
- `NB_STATUS_QOS_INCOMPATIBLE`: the pair has the same topic and type but cannot
  match; `reason` names the policy.
- `NB_STATUS_OFFERED_DEADLINE_MISSED` / `NB_STATUS_REQUESTED_DEADLINE_MISSED`
  and `NB_STATUS_LIVELINESS_CHANGED`.

`endpoint` is the provider's name for the local endpoint; the RTPS provider uses
the method the endpoint belongs to. Status never travels on the wire as a
framework message: it is emitted by whichever provider owns the data plane, so
UDP/TCP/SHM simply leave the callback unset.

## Mesh and SHM

The mesh control plane still propagates services and bindings over UDP
HELLO/BYE. HELLO v4 carries the full URI of each binding rather than
`scheme + port`, so provider authorities such as SHM channels and DDS domains
survive the round trip.

The SHM segment uses one inbound queue per node:

- Each node joins the segment as `shm://<channel>:<node-id>`, with `node-id`
  in `1..9`.
- Any node may deliver an RPC response or event to any target node; there is no
  fixed server/client role.
- The legacy `push_request/push_response` API remains as compatibility
  wrappers: request means "send to node 1", response means "send to the
  specified client".
- Events fan out to subscribed nodes by route; chunk reference counts preserve
  zero-copy behavior.
- The first creator initializes the segment; the last closer unlinks it.

This removes the need for a dedicated "SHM server": every mesh node is a peer,
but node ids must be unique within one channel.

## Capability matrix

| Capability | UDP | TCP | SHM | DDS |
| --- | :-: | :-: | :-: | :-: |
| unary RPC | yes | yes | yes | yes |
| events / pub-sub | yes | yes | yes | yes |
| streaming | yes | yes | yes | yes |
| mesh discovery | yes | yes | yes | yes |
| zero-copy receive | no | no | yes | while loaned |
| QoS | kept | kept | kept | mapped |

## Boundaries

- TLS/DTLS and authentication remain the responsibility of a custom provider or
  a service interceptor.
- Cross-thread ownership transfer of DDS loans requires extending the loan
  lifetime to asynchronous consumers; the worker path still performs a deep
  copy in core.
- Recovering a SHM segment after an abnormal exit relies on the last closer or
  on rebuilding an empty segment; cross-host deployments should use
  UDP/TCP/DDS.
