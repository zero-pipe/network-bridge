# Configuration Reference

Network Bridge is configured with JSON, either from a file or from an in-memory
string:

```c
nb_config_doc_t *cfg = NULL;
nb_config_error_t err;

int rc = nb_config_parse_file("server.json", &cfg, &err);
if (rc != NB_OK) {
    printf("path=%s reason=%s\n", err.path, err.reason);
    return rc;
}

rc = nb_node_create_from_config(cfg, &node);
if (rc == NB_OK) {
    const nb_node_config_t *parsed = nb_config_node(cfg);  /* read-only view */
    (void)parsed;
}

nb_config_doc_destroy(cfg);  /* the node copied its fields; free immediately */
```

Related interfaces:

- `nb_config_parse` / `nb_config_parse_file` (the third
  `nb_config_error_t *` argument may be NULL)
- `nb_node_create_from_config`
- `nb_config_node` (read-only `nb_node_config_t` view for inspecting the parse
  result)
- `nb_config_doc_destroy` (frees the document; must pair with
  `nb_config_parse*`)

`nb_config_doc_t` is an opaque handle allocated by `nb_config_parse*` that owns
the parsed strings. Do not copy it by value or build it on the stack.
`nb_node_create_from_config` copies the fields the node needs, so the document
only has to live until that call returns. A known field with a wrong type, an
out-of-range value or an invalid URI returns `NB_ERR_INVALID`; when the error
out-parameter is supplied it reports the exact `path` (for example
`runtime.io_threads`, `routes[0].endpoint`, `transport.bindings[1].qos.depth`)
and the `reason`.

Unknown fields are ignored for forward compatibility, but `nb_config_parse`
logs one `unknown config key ignored: <path>` warning per unknown field so
typos do not take effect silently.

String enum values (`mode`, `transport.kind`, `discovery.mode`,
`intra_process`, `log.level`, QoS values) are lower-case and case-sensitive.

The full JSON Schema is in [`docs/config.schema.json`](../config.schema.json).

## Minimal server

```json
{
  "name": "echo-server",
  "mode": "static",
  "transport": {
    "bindings": ["tcp://127.0.0.1:42420"]
  },
  "runtime": {
    "io_threads": 2
  }
}
```

## Minimal client

```json
{
  "name": "echo-client",
  "mode": "static",
  "routes": [
    { "service": "EchoService", "endpoint": "tcp://127.0.0.1:42420" }
  ]
}
```

## Top-level fields

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `name` | string | `"nb"` | node name, at most `NB_NAME_MAX - 1` characters |
| `mode` | string | `"mesh"` | `mesh` or `static`; `static` disables discovery |
| `runtime` | object | see below | threads, concurrency and rate limits |
| `transport` | object | see below | transports and listening endpoints |
| `discovery` | object | see below | service discovery |
| `metrics` | object | see below | metric collection |
| `log` | object | see below | logging |
| `routes` | array | empty | static routes |

## runtime

| Field | Type | Default | Range | Notes |
| --- | --- | --- | ---: | --- |
| `io_threads` | uint | hardware concurrency | `0..8` | poller threads, capped by `NB_TRANSPORT_MAX_IO` |
| `handler_threads` | uint | `0` | `0..1024` | WORKER handler threads; `0` disables the pool |
| `handler_max_concurrency` | uint | `0` | `0..N` | unfinished worker/executor task limit; `0` means unlimited |
| `max_inflight` | uint | `65536` | `0..N` | client in-flight call limit |
| `max_streams` | uint | `64` | `0..64` | concurrent inbound streams |
| `peer_rate_limit_pps` | uint | `0` | `0..N` | per-peer requests per second; `0` means unlimited |
| `send_lanes` | uint | `0` | `0..64` | dedicated send-lane threads isolated from receive/dispatch pollers; `0` disables |

With `send_lanes > 0`, submit publish loops through `nb_node_post_send()`;
`nb_proxy_call_event()` inside the loop takes the inline fast path and avoids
one cross-thread enqueue and wake-up per event. Total threads =
`io_threads + send_lanes`, so you can move cores from `io_threads` to
`send_lanes`.

Without `handler_threads`, WORKER methods degrade to INLINE with a warning.
When `handler_max_concurrency` is exceeded, new requests receive
`NB_ERR_OVERLOAD` or `NB_ERR_AGAIN` depending on the call path.

## transport

| Field | Type | Default | Range | Notes |
| --- | --- | --- | --- | --- |
| `kind` | string | `"udp"` | `udp` / `tcp` | default transport, used only when no explicit `bindings` exist |
| `bind_host` | string | `"0.0.0.0"` | - | default bind address, used only without explicit `bindings` |
| `data_port` | uint | `0` | `0..65535` | default data port; `0` allocates automatically |
| `bindings` | array | empty | at most 8 | listening endpoints, elements below |
| `frag_bytes` | uint | `1440` | `0` or `256..8192` | UDP fragment size; `0` uses the default |
| `max_msg_bytes` | uint | `1048576` | `0..67108864` | single-message limit; must be at least `frag_bytes` |
| `backpressure_bytes` | uint | `4194304` | `0..N` | outbound high-water mark; `0` uses the default |
| `rtcp_interval_ms` | uint | `1000` | `0..N` | UDP RTCP report interval |
| `disable_rtcp` | bool | `false` | - | `true` disables RTCP |
| `qos` | object | inherited | - | provider QoS for default bindings, fields below |

`bindings` also accepts `intra_process` (string: `off` / `auto` / `only`) for
the in-process fast path of DDS event pub/sub. `only` creates no DDS event
writer/reader and delivers the message pointer directly to local subscribers of
the same provider; `auto` delivers locally and still uses DDS. Each binding may
carry its own `qos` object; without one it inherits `transport.qos`.

`bindings` also accepts `poll_us` (uint, microseconds) for the `shm` provider
only: after draining the local queue, spin-poll for this long before falling
back to a blocking wait. `0` (default) always blocks on wake-up. This suits
low-tail-latency scenarios at the cost of one core's idle time and usually
pairs with `send_lanes` or one fewer `io_threads`. At runtime the
`NB_SHM_POLL_US` environment variable overrides per-binding values for
benchmarking and experimentation.

`bindings` elements have two forms:

```json
{
  "transport": {
    "bindings": [
      "tcp://127.0.0.1:42420",
      { "name": "udp", "endpoint": "udp://0.0.0.0:42421" }
    ]
  }
}
```

The string form selects UDP/TCP from the URI scheme. In the object form `name`
is the binding name (at most `NB_SCHEME_MAX - 1` characters) and defaults to
the scheme. Port `0` lets the system choose an ephemeral port.

Cyclone DDS uses `dds://<domain>`, where the host is the DDS domain and the port
may be omitted (the provider ignores it; the old `dds://<domain>:1` spelling
still parses). Non-network schemes such as dds and shm may omit the port, while
tcp and udp require one. Shared memory uses
`shm://<channel>:<node-id>`; in mesh mode nodes on the same channel must use
distinct node ids (`1..9`). The shm `node-id` port is mandatory and omitting it
returns `NB_ERR_INVALID`.

### QoS

`qos` mirrors the same-named Cyclone DDS policies. A value of `0` or an absent
field means "inherit the provider default"; only explicitly configured fields
are written. Current mapping to DDS readers/writers:

| Field | Values | DDS policy |
| --- | --- | --- |
| `reliability` | `best_effort` / `reliable` | ReliabilityQosPolicy |
| `durability` | `volatile` / `transient_local` | DurabilityQosPolicy |
| `history` | `keep_last` / `keep_all` | HistoryQosPolicy |
| `depth` | uint | history depth |
| `deadline_ms` | uint | DeadlineQosPolicy |
| `lifespan_ms` | uint | LifespanQosPolicy |
| `latency_budget_ms` | uint | LatencyBudgetQosPolicy |
| `min_separation_ms` | uint | TimeBasedFilterQosPolicy (subscriptions drop closer samples) |
| `liveliness` | `automatic` / `manual_by_participant` / `manual_by_topic` | LivelinessQosPolicy |
| `lease_duration_ms` | uint | liveliness lease duration |
| `ownership` | `shared` / `exclusive` | OwnershipQosPolicy |
| `partition` | string (max 127) | PartitionQosPolicy |
| `max_samples` | uint | ResourceLimitsQosPolicy, 0 = unlimited |
| `max_instances` | uint | ResourceLimitsQosPolicy, 0 = built-in cap; a subscription rejects samples beyond it |
| `max_samples_per_instance` | uint | ResourceLimitsQosPolicy, 0 = unlimited |
| `autodispose_unregistered_instances` | bool | WriterDataLifecycleQosPolicy |
| `presentation_scope` | `instance` / `topic` / `group` | PresentationQosPolicy access scope (RxO: the writer must offer at least the reader's scope) |
| `coherent_access` | bool | PresentationQosPolicy coherent_access (announced and checked for compatibility) |
| `ordered_access` | bool | PresentationQosPolicy ordered_access (announced and checked for compatibility) |

Numeric fields must be integers; values such as `1.5` are rejected rather than
truncated.

UDP/TCP/SHM have no one-to-one link policy, so the fields stay in the
configuration for applications and custom providers to read; the built-in
implementations do not change message semantics based on them.

## discovery

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `mode` | string | `"mesh"` | `mesh` enables discovery; `static` / `off` / `none` disable it |
| `broadcast` | string | `"255.255.255.255:41953"` | broadcast/discovery address in `host:port` form |
| `peers` | array | empty | extra unicast HELLO targets as `"host:port"`, at most 32 |
| `hello_interval_ms` | uint | `3000` | HELLO period |
| `node_ttl_ms` | uint | `9000` | peer expiry |

Top-level `mode = "static"` and `discovery.mode = "static"` (or `off` / `none`)
both disable discovery; if both appear, "disabled" wins.
`nb_node_config_normalize` keeps `mode` and `discovery.disable` consistent
(disabling discovery sets `mode` to `STATIC`).

MESH usually needs both sides to use the same discovery port or to configure
each other as unicast `peers`.

## metrics

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `disable` | bool | `false` | disable all metric collection |
| `disable_latency_histogram` | bool | `false` | disable only the latency histogram |
| `sample_every` | uint | `1` | `1` samples everything; `N` samples every Nth event |

With metrics disabled the hot path keeps only one global switch check and does
no atomic accumulation.

`metrics` and `log` are process-wide switches: `nb_node_create` applies them to
the shared metrics/log runtime, so with multiple nodes the last created node
wins and destroying a node does not restore previous values. Logging can be
changed at runtime with `nb_log_configure`; metric switches only take effect
when a node is created.

## log

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `disable` | bool | `false` | disable log output |
| `level` | string | `"info"` | `assert` / `error` / `warn` / `info` / `debug` / `verbose` |

`nb_log_configure(enabled, level)` also changes the switch and level at runtime.

When building configuration in code, `log.level = NB_LOG_LEVEL_DEFAULT` means
"unspecified" and normalizes to `info`; out-of-range values also normalize to
`info` (`nb_node_config_validate` reports an error instead).

## routes

```json
{
  "routes": [
    { "service": "OrderService", "endpoint": "tcp://127.0.0.1:42420" },
    { "service": "EchoService", "endpoint": "udp://127.0.0.1:42421" }
  ]
}
```

| Field | Type | Required | Notes |
| --- | --- | :---: | --- |
| `service` | string | yes | target service name |
| `endpoint` | string | yes | `scheme://host:port` |

Static routes skip discovery. At most `NB_CONFIG_MAX_ROUTES` entries are
accepted; exceeding the limit returns `NB_ERR_INVALID`.

## Defaults and normalization

When building `nb_node_config_t` in code, call `nb_node_config_default` first,
or call `nb_node_config_normalize` before creating the node.
`nb_node_create` normalizes a copy of the configuration.

`nb_node_config_validate` is the strict variant: it neither clamps nor fills
defaults, and returns `NB_ERR_INVALID` / `NB_ERR_TOOBIG` for out-of-range
fields.

`nb_node_config_t` carries `routes`/`route_count` and `peers`/`peer_count`;
`nb_node_create` applies them before returning. If any route or peer fails to
register, the half-built node is destroyed and an error is returned instead of
silently ignoring it. JSON `routes`/`peers` land in the same fields.

Division of responsibility:

- `nb_config_parse` performs **strict validation** of user input and returns
  `NB_ERR_INVALID` for values outside the ranges above.
- `nb_node_config_normalize` serves configurations built in code: it fills
  defaults and **clamps** out-of-range values into the valid range.

Normalization rules:

- `name == NULL` uses `"nb"`.
- `transport.bind_host == NULL` uses `"0.0.0.0"`.
- `discovery.broadcast_host == NULL` uses `"255.255.255.255"`.
- `mode` and `discovery.disable` converge: if either disables discovery, both
  are set to disabled.
- `runtime.io_threads == 0` uses hardware concurrency, clamped to
  `1..NB_TRANSPORT_MAX_IO`.
- `runtime.handler_threads` is clamped to `0..NB_MAX_HANDLER_THREADS`.
- `runtime.send_lanes` is clamped to `0..NB_MAX_SEND_LANES`.
- `frag_bytes == 0` uses `1440`; otherwise clamped to `256..8192`.
- `max_msg_bytes == 0` uses `1 MiB`; clamped to `frag_bytes..64 MiB`.
- `backpressure_bytes == 0` uses `4 MiB`.
- `discovery.port == 0` uses `41953`.
- `metrics.sample_every == 0` uses `1`.

## Common errors

| Error path | Cause |
| --- | --- |
| `name` | not a string, or too long |
| `mode` | not `mesh` / `static` |
| `runtime.io_threads` | exceeds `NB_TRANSPORT_MAX_IO` (8) |
| `runtime.handler_threads` | exceeds `NB_MAX_HANDLER_THREADS` (1024) |
| `runtime.send_lanes` | exceeds `NB_MAX_SEND_LANES` (64) |
| `runtime.max_streams` | exceeds `NB_STREAM_MAX_RECV_STREAMS` (64) |
| `transport.kind` | not `udp` / `tcp` |
| `transport.frag_bytes` | neither `0` nor within `256..8192` |
| `transport.max_msg_bytes` | exceeds 64 MiB or is below `frag_bytes` |
| `transport.bindings[i].endpoint` | element is not a string/object, URI is invalid, or there are too many |
| `transport.bindings[i].intra_process` | not `off` / `auto` / `only` |
| `transport.bindings[i].qos.<field>` | QoS field has an invalid type or value |
| `discovery.mode` | not `mesh` / `static` / `off` / `none` |
| `discovery.broadcast` | not `host:port` |
| `discovery.peers[i]` | not a valid `host:port` |
| `routes[i].service` | missing or too long |
| `routes[i].endpoint` | missing, invalid URI, or too long |
| `log.level` | unsupported level |
| `transport.qos.<field>` | QoS field has an invalid type or value |

`nb_config_parse_file` returns `NB_ERR_INVALID` for files larger than
`NB_CONFIG_MAX_FILE_BYTES` (1 MiB), so a mistaken path cannot read a huge file
into memory.
