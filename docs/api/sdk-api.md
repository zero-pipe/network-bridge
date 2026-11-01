# Network Bridge SDK API

Network Bridge is an asynchronous RPC and messaging runtime. The public API
exposes only nodes, services, proxies and byte views; payload serialization is
an application decision made through a codec.

## Entry headers

For the complete API:

```c
#include <nb/nb.h>
```

For a minimal include set, include the header that declares what you use:

| Header | Purpose |
| --- | --- |
| `<nb/node.h>` | node, scheduling, discovery, routing, metrics |
| `<nb/service.h>` | service, handler, reply, message view |
| `<nb/proxy.h>` | proxy, async/sync/event/streaming calls |
| `<nb/publish.h>`, `<nb/subscribe.h>` | topics, samples, readers |
| `<nb/endpoint.h>` | endpoint parsing and custom transport providers |
| `<nb/codec.h>` | codec, message view, service table |
| `<nb/config.h>` | JSON configuration loading and node creation |

`<nb/error.h>`, `<nb/types.h>`, `<nb/metrics.h>` and `<nb/log.h>` are included
on demand by those entry points. Public headers live under `<nb/...>` and
nowhere else: `include/nb/` is the only location, so an include path that works
in the build tree works after installation too. There are no aggregator headers
in between - `<nb/service.h>` is the service API, not a bundle of role
headers.

## Object model

```text
node
  ├─ service   registers methods, receives requests and replies
  └─ proxy     discovers/routes and sends calls:
               call_async / call_sync / call_event
```

One node can host both client and server roles. A proxy borrows the node's
pollers and routing table and creates no threads of its own. The transport is
selected per binding; custom transports plug in through
`<nb/transport_provider.h>`.

A node is the runtime/participant handle (poller pool, bindings, discovery),
not a ROS 2 node: it owns threads and sockets, so an application normally
creates one for its lifetime. Multiple nodes per process remain supported for
in-process tests and multi-tenant setups.

Recommended create/destroy order:

```text
nb_node_create
  -> nb_service_create / nb_proxy_create
  -> nb_node_run or an application event loop
  -> nb_service_destroy / nb_proxy_destroy
  -> nb_node_destroy
```

## Threading model

- Callbacks and `NB_SERVICE_EXEC_MODE_INLINE` handlers run on the NTK poller
  thread by default and must not block.
- `NB_SERVICE_EXEC_MODE_WORKER` runs on the node's `runtime.handler_threads`
  pool.
- `NB_SERVICE_EXEC_MODE_EXECUTOR` runs on a caller-provided executor.
- `nb_proxy_call_async` / `nb_proxy_call_event` may be called from any thread.
- `nb_proxy_call_sync` blocks and must not be called inside a poller callback.
- `nb_reply` from a worker/executor handler is posted back to the original
  poller for sending.

## Lifetime and ownership

### Payload

`nb_payload_t` is only a `{data, len, encoding}` view. The framework never
interprets its contents.

On send, `nb_proxy_call_async` and `nb_proxy_call_event` copy the payload
before returning. The caller always owns its buffer and may free it
immediately.

On receive, request and response views are valid only inside the callback. Copy
them if they must outlive it.

### Owned payload API

To avoid a full copy, use `nb_owned_payload_t` with the `*_owned` variants:

```c
nb_buf_t *buf = nb_buf_alloc(12);
memcpy(nb_buf_data(buf), "hello", 5);
nb_owned_payload_t request;
nb_owned_payload_init(&request, buf, 0, 5, 0);

nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.timeout_ms = 1000;
rc = nb_proxy_call_async_owned(proxy, "Svc", "method", &request, &options,
                               on_response, NULL);
/* request.buf is NULL afterwards; the framework owns or released it even on failure. */
```

- `nb_proxy_call_async_owned` / `nb_proxy_call_event_owned`: client requests;
  metadata travels through options.
- `nb_reply_owned`: server replies.

Owned APIs take over `payload->buf` at entry. The borrowed-plus-copy semantics
of the older `nb_payload_t` APIs are unchanged. Metadata on the proxy layer
always uses `nb_call_options_t`. Buffers are managed by `nb_base`'s `nb_buf_*`;
the single-owner path does no atomic reference counting by default.

### Synchronous calls

```c
nb_response_t response = {0};
nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.timeout_ms = 3000;
int rc = nb_proxy_call_sync(proxy, "OrderService", "get", &request, &options,
                            &response);
/* use response.payload */
nb_response_release(&response);
```

`nb_response_release` frees the metadata and payload of a response written by a
synchronous call (when `owns_payload == true`) and zeroes the structure.
Callback-scoped responses have `owns_payload == false`; releasing them never
touches application buffers.

### Message view

`nb_message_view_t` is used by synchronous-style handlers and by applications
assembling payloads:

```c
nb_message_view_t view;
nb_message_view_init(&view);
nb_message_view_set_string(&view, "ok");
nb_message_view_release(&view);
```

`set_borrowed` does not take ownership of external memory; `set_data`,
`set_string` and `set_payload` copy and are owned by the view.

## Node

```c
int nb_node_create(const nb_node_config_t *cfg, nb_node_t **out);
void nb_node_destroy(nb_node_t *node);
```

Node creation assembles transports, pollers, discovery and routing. Query
interfaces:

- `nb_node_name`
- `nb_node_transport`
- `nb_node_data_port`
- `nb_node_discovery_port`
- `nb_node_binding_count` / `nb_node_binding_endpoint`
- `nb_node_metrics` / `nb_node_peer_stats`

Running and routing:

- `nb_node_add_discovery_peer`
- `nb_node_add_static_route` / `nb_node_add_static_route_uri`
- `nb_node_wait_for_service`
- `nb_node_post` / `nb_node_post_delay`
- `nb_node_run_once` (runs one round, installs no signal handlers; suitable for
  embedding in a host application)
- `nb_node_run_until_signal` (installs SIGINT/SIGTERM and runs until one
  arrives; `nb_node_run` is a compatibility alias)

Configuration fields are documented in the [configuration
reference](config.md).

## Service

Asynchronous handler:

```c
typedef void (*nb_handler_fn)(nb_call_t *call, const nb_request_t *req, void *user);
```

Synchronous-style handler:

```c
typedef int (*nb_sync_handler_fn)(const nb_request_t *req, nb_message_view_t *resp, void *user);
```

The return value of a synchronous handler becomes the application status in the
response. An asynchronous handler must call exactly one of `nb_reply`,
`nb_reply_with_metadata` or `nb_reply_stream`.

Every `nb_reply*` returns `int`: `NB_OK` means the reply was accepted for
sending, a duplicate reply returns `NB_ERR_ALREADY`, and a status message above
`NB_STATUS_MSG_MAX` or metadata above `NB_METADATA_WIRE_MAX` returns
`NB_ERR_TOOBIG` instead of being silently truncated. To preserve embedded NUL
bytes in a status message, use
`nb_reply_status(call, &nb_status_t{code, message, message_len}, ...)`.

Registration:

```c
nb_service_t *svc = NULL;
nb_service_create(node, "OrderService", &svc);
nb_service_add_method(svc, "get", on_get, user);
nb_service_add_sync_method(svc, "put", on_put_sync, user);
```

The `*_with_options` variants select INLINE, WORKER or EXECUTOR through
`nb_service_method_options_t`. WORKER requires `runtime.handler_threads > 0`;
without it the method degrades to INLINE with a warning.

Streaming:

- `nb_service_add_stream_method` / `_with_options`
- `nb_reply_stream`
- the final request chunk must carry `NB_STREAM_FLAG_LAST`

Interception and cancellation:

- `nb_service_set_interceptor` runs before the handler; a non-`NB_OK` return
  short-circuits the reply.
- `nb_call_deadline_ms` reads the absolute deadline propagated by the client.
- `nb_call_request_id` reads the request correlation id for logging and
  tracing.
- `nb_call_canceled` supports cooperative handler exit.

### Reader history (read/take) and wait sets

A subscription can either deliver samples to a callback
(`nb_subscriber_subscribe`) or keep them in a reader-side history:

```c
#include <nb/subscribe.h>

nb_subscriber_t *sub = NULL;
nb_subscriber_create(node, "TradeService", &sub);
nb_subscriber_subscribe_queue(sub, "ticks", 16); /* KEEP_LAST depth 16 */

nb_sample_t samples[8];
nb_sample_filter_t filter = NB_SAMPLE_FILTER_ANY;
size_t n = nb_subscriber_read(sub, "ticks", &filter, samples, 8);
for (size_t i = 0; i < n; i++) {
    /* samples[i].data is borrowed: valid until the sample is taken */
    use(samples[i].data.data, samples[i].data.len, &samples[i].info);
}

n = nb_subscriber_take(sub, "ticks", &filter, samples, 8);
for (size_t i = 0; i < n; i++) {
    use(samples[i].data.data, samples[i].data.len, &samples[i].info);
    nb_sample_release(&samples[i]); /* take() samples are owned */
}
```

Semantics:

- The history is DDS `KEEP_LAST(depth)`; depth 0 selects the default of 32.
  On overflow the oldest sample is dropped and counted in
  `nb_service_queue_dropped`.
- `read()` marks the returned samples READ but keeps them, and returns
  borrowed views (the loan path). `take()` removes them and returns owned
  copies. Both apply the mask first.
- The mask combines the three DDS dimensions
  (`NB_STATE_SAMPLE_READ`/`_NOT_READ`, `NB_STATE_VIEW_NEW`/`_NOT_NEW`,
  `NB_STATE_INSTANCE_ALIVE`/`_DISPOSED`/`_NO_WRITERS`). A dimension only
  constrains the result when one of its bits is set; `0` means
  `NB_STATE_ANY`, and `NB_STATE_ANY_DATA_READER` is the conventional default.
- `nb_sample_filter_t.instance` selects one DDS instance by its 16-byte
  handle; `NULL` accepts any. Samples that carry no handle never match a
  non-NULL handle.

A wait set blocks on one or more histories:

```c
#include <nb/waitset.h>

nb_waitset_t *ws = NULL;
nb_waitset_create(node, &ws);
nb_waitset_attach(ws, sub, "ticks");

if (nb_waitset_wait(ws, &filter, 500) == 1) {
    n = nb_subscriber_take(sub, "ticks", &filter, samples, 8);
}
```

`nb_waitset_wait` returns 1 when an attached history has a matching sample, 0
on timeout, and a negative error otherwise. Wake-up is event driven: each
enqueue posts a permit, so a blocked waiter returns without polling delay.
`NB_WAITSET_INFINITE` waits forever. The wait set only reports readiness;
drain the specific subscriptions afterwards.

## Proxy

A regular call:

```c
nb_proxy_t *proxy = NULL;
nb_proxy_create(node, NULL, &proxy);

nb_payload_t request = { "hello", 5, 0 };
nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.timeout_ms = 1000;

nb_proxy_call_async(proxy, "EchoService", "echo", &request, &options,
                    on_response, user);
```

Every call form shares `nb_call_options_t`:

```c
typedef struct nb_call_options {
    uint64_t timeout_ms;              /* 0 = default; ignored by events; the stream deadline when opening */
    const nb_metadata_t *metadata;    /* request metadata; NULL = none, copied before return */
    nb_request_id_t *out_request_id;  /* optional; receives the id for nb_proxy_cancel */
} nb_call_options_t;
```

Passing `NULL` for `options` means all defaults. You may also initialize with
`NB_CALL_OPTIONS_INIT` and override individual fields.

Three call forms:

- `nb_proxy_call_async`: waits for a response, invokes the callback once.
- `nb_proxy_call_sync`: blocks and writes an `nb_response_t`.
- `nb_proxy_call_event`: one-way event, no response.

Metadata is passed through `nb_call_options_t.metadata` in a fixed-capacity
container; binary values travel safely.

Streaming calls:

- `nb_proxy_call_server_stream`: regular request with a chunked response.
- `nb_proxy_open_client_stream` / `nb_proxy_open_bidi_stream`: open a client
  request stream.
- `nb_proxy_stream_send` / `nb_proxy_stream_cancel` / `nb_proxy_stream_destroy`.
- Opening a stream uses only `timeout_ms` from options; per-chunk metadata
  travels in `nb_chunk_t.metadata`.

Cancellation:

```c
nb_request_id_t request_id = 0;
nb_call_options_t options = NB_CALL_OPTIONS_INIT;
options.out_request_id = &request_id;
nb_proxy_call_async(proxy, "Svc", "method", &request, &options, on_response, user);
nb_proxy_cancel(proxy, request_id);
```

Cancellation is best-effort: the framework sends `KIND_CANCEL` and completes
the local call with `NB_ERR_NOTFOUND`; a handler that is already running is
never preempted.

Beyond the built-in UDP/TCP/SHM/RTPS bindings, a transport implements
`<nb/transport_provider.h>` and is selected by the node's binding configuration
(or a `host:port` target for a proxy).

### Zero-copy publishing (loan/borrow)

A regular `nb_proxy_call_event` copies the payload into the transport send
buffer. For backends such as shm, where the payload lives directly in the
shared segment, you can borrow a chunk, write into it and publish it, avoiding
that copy:

```c
nb_loan_t *loan = NULL;
uint8_t *data = NULL;
uint32_t cap = 0;

if (nb_publisher_loan(pub, "TopicService", "chat", len, &loan, &data, &cap) == NB_OK) {
    memcpy(data, my_bytes, len);                   /* write directly into the segment */
    nb_publisher_publish_loan(loan, len, 0, NULL); /* add the envelope prefix and fan out */
} else {
    /* NB_ERR_NOT_SUPPORTED: transport does not support loans;
       NB_ERR_AGAIN: no chunk available, retry later */
}
```

- A loan fixes its target transport when it is issued (through a proxy's fixed
  endpoint or discovery), so `publish_loan` does no routing lookup. Only EVENT
  publishing is supported.
- An `encoding` of 0 in `publish_loan` uses the publisher's default
  `payload_encoding`, matching the regular event path.
- `nb_publisher_publish_loan` takes over the loan on both success and failure.
  To abandon an unpublished loan, call `nb_publisher_loan_release`. A loan is
  thread-private; never pass it across threads.
- `cap` may exceed the requested `capacity` (rounded up to a segment size
  class). Never write more than `len` bytes.

## Codec

```c
typedef struct nb_codec {
    const char *name;
    uint8_t encoding;
    int (*encode)(void *ctx, const void *obj, void **out_data, size_t *out_len);
    int (*decode)(void *ctx, const void *data, size_t len, void **out_obj);
    void (*release)(void *ctx, void *obj);
    void *ctx;
} nb_codec_t;
```

Registration:

```c
nb_node_register_codec(node, &my_codec);
const nb_codec_t *codec = nb_node_find_codec(node, response.payload.encoding);
```

Bytes produced by encode may be released as soon as the send call returns,
because the framework has already copied the payload.

## Service table

Share service and method ids through a header used by both sides:

```c
NB_SERVICE_DEFINE(order_table, 7u, "OrderService",
    NB_SERVICE_METHOD(1u, "get"),
    NB_SERVICE_METHOD(2u, "put"));
```

Then put `&order_table` into `nb_node_config_t.service_table`, or call
`nb_service_table_install` before creating the node. The service table is a
declarative contract; routing still uses the `service.method` string.

## Transport provider

The built-in UDP/TCP transports are plaintext and unauthenticated. To add
TLS/DTLS or a custom protocol, implement `nb_transport_provider_ops_t` and
register it before creating a node:

```c
nb_transport_provider_register(&my_provider);
```

The provider descriptor and ops must have static lifetime.

The ops and options structs start with version/size fields for safe
cross-version registration:

- `nb_transport_provider_ops_t.abi_version` must be
  `NB_TRANSPORT_PROVIDER_ABI_VERSION` (0 is treated as a legacy provider and is
  still accepted)
- `nb_transport_provider_ops_t.struct_size` must be
  `sizeof(nb_transport_provider_ops_t)`
- `nb_transport_provider_options_t.struct_size` must be
  `sizeof(nb_transport_provider_options_t)`

Registration returns `NB_ERR_INVALID` on failure. Unregister with
`nb_transport_provider_unregister(scheme)`.

Pointer lifetime rules: `options.endpoint` stays valid for the instance
lifetime; `message` and its `payload`/`metadata` pointers are valid only inside
the current callback; `frame_owner` is owned by core and released by the
provider when done; `reply_token` may only be passed back verbatim to `reply`;
`peer`/`target` are temporary objects on the call stack.

The provider SPI uses only three public objects:

- `nb_endpoint_t`: the target of `send` and the peer of `reply`;
  `scheme/host/port/path` are interpreted by the provider itself, while `uri`
  is the normalized form.
- `nb_message_t`: the same message struct used inside core; there is no second
  provider message copy.
- `nb_qos_t`: passed when creating an instance through
  `nb_transport_provider_options_t.qos`.

`nb_message_t.sample` is the DDS sample metadata: `state`
(`NB_SAMPLE_STATE_VALID` / `DISPOSED` / `NO_WRITERS`, with
`nb_sample_info_valid_data()` as the DDS `valid_data` spelling),
`instance_handle` plus `has_instance_handle`, `source_timestamp_ns`,
`reception_timestamp_ns`, and the DDS instance/view state pair
`instance_state` (`ALIVE` / `NOT_ALIVE_DISPOSED` /
`NOT_ALIVE_NO_WRITERS`) and `view_state` (`NEW` for the first sample of an
instance or the first one after it is re-registered, `NOT_NEW` otherwise),
with `generation_rank` counting those re-registrations. The RTPS provider
fills it; the other providers leave it zeroed, which reads as valid data with
no instance. Instance state needs the instance handle, which the RTPS reader
takes from PID_KEY_HASH; a type whose samples carry no key hash is delivered
without per-instance state.

The same metadata works on the way out. A message whose
`sample.has_instance_handle` is set and whose `sample.state` is
`NB_SAMPLE_STATE_DISPOSED` or `NB_SAMPLE_STATE_NO_WRITERS` is published as a
key-only DDS sample instead of an ordinary one: the handle goes into the inline
QoS, the status bits say whether the instance was disposed or unregistered, and
the payload is taken as the serialized key rather than as an encoded envelope.
With the `rtps` provider a DDS-level reader (including Cyclone DDS) sees the
state change; a remote `nb` peer does not yet, because the v2 envelope has no
sample fields to route it with. Ordinary samples ignore `sample` on send.

```c
static int my_send(nb_transport_instance_t *inst,
                   const nb_endpoint_t *target,
                   const nb_message_t *msg);
static int my_reply(nb_transport_instance_t *inst,
                    void *reply_token,
                    const nb_endpoint_t *peer,
                    const nb_message_t *msg);
```

The inbound callback is `on_receive(receiver, reply_token, message)`; the source
endpoint is `message->peer`, so providers no longer pass separate `host/port`
arguments.

### Status events

`nb_transport_provider_options_t.on_status` is the optional second callback: a
provider that can observe DDS status reports them as `nb_status_event_t`
(`<nb/status.h>`), using the same `user` as `on_receive`. Providers that report
nothing leave it `NULL`.

```c
typedef struct nb_status_event {
    nb_status_kind_t kind;          /* matched, deadline, liveliness, QoS */
    char endpoint[NB_STATUS_ENDPOINT_MAX]; /* provider name, e.g. a method */
    uint32_t count;                 /* current matches, or miss count */
    uint32_t total_count;           /* cumulative matches */
    int alive;                      /* liveliness direction */
    uint32_t alive_count;
    uint32_t not_alive_count;
    bool has_peer;
    uint8_t peer_guid[NB_STATUS_GUID_SIZE];
    char reason[NB_STATUS_REASON_MAX]; /* QoS policy that failed */
} nb_status_event_t;
```

The RTPS provider reports `NB_STATUS_PUBLICATION_MATCHED` and
`NB_STATUS_SUBSCRIPTION_MATCHED` whenever a local endpoint gains or loses a
matched remote endpoint, with `count` as the current number of matches (a drop
means the peer went away) and `total_count` as the cumulative count, mirroring
`PublicationMatchedStatus`/`SubscriptionMatchedStatus`. A same-topic,
same-type pair that cannot match is reported as `NB_STATUS_QOS_INCOMPATIBLE`
with `reason` naming the policy (`deadline`, `reliability`, ...). Deadline and
liveliness events use the same surface.

`endpoint` is the provider's name for the local endpoint: the RTPS provider uses
the method name the endpoint belongs to, so an event maps back to a service
without exposing GUIDs. The callback runs on the provider's own thread and must
not call back into the provider.

## Cyclone DDS

The built-in `dds` provider carries v2 frames as opaque octet samples over
Cyclone DDS. After registration, use `dds://<domain>` in a binding (the port may
be omitted; the legacy `dds://<domain>:1` spelling is accepted):

```c
#include <nb/dds.h>

nb_cyclone_dds_register();

nb_binding_config_t binding = { "dds", "dds://0" };
nb_node_config_t cfg;
nb_node_config_default(&cfg);
cfg.mode = NB_NODE_MODE_STATIC;
cfg.transport.bindings = &binding;
cfg.transport.binding_count = 1;
```

Binding QoS may be configured; unset fields keep Cyclone DDS defaults:

```c
strcpy(binding.qos.partition, "orders");
binding.qos.reliability = NB_QOS_RELIABILITY_RELIABLE;
binding.qos.history = NB_QOS_HISTORY_KEEP_LAST;
binding.qos.depth = 8;
```

Topics are derived from the full `Service.method` route:

- request: `<Service>_<method>_req`
- response: `<Service>_<method>_res`
- event/pub-sub: `<Service>_<method>_evt`

Publish with `nb_publisher_publish` (equivalent to `nb_proxy_call_event`) and
subscribe with `nb_subscriber_subscribe` (equivalent to
`nb_service_add_method`). Registering a server method creates request/event
readers and a response writer; the first client request lazily creates a
request writer and a response reader.

A DDS binding's `intra_process` supports `off` / `auto` / `only`. `auto`
delivers events in-process first and then publishes to DDS as usual; `only`
skips DDS event writers/readers and `dds_write` entirely and only delivers to
local subscribers of the same provider, with zero-copy payloads during
delivery.

The provider uses the Cyclone DDS data plane itself, so an independent Cyclone
reader on the same domain can receive its samples. The sample type is
`dds::Message`, an opaque octet sequence containing the framework frame, and
event topics are `<Service>_<method>_evt`. Use `intra_process=off` or separate
processes when verifying network delivery; `only` intentionally bypasses DDS.

## Discovery views

DDS publishes discovery data on the built-in topics `DCPSParticipant`,
`DCPSPublication`, `DCPSSubscription` and `DCPSTopic`. This stack has no typed
built-in readers, so the same information is available as a snapshot:

```c
#include <nb/discovery_view.h>

nb_discovery_sample_t views[64];
size_t n = nb_node_discovery_read(node, NB_BUILTIN_TOPIC_PARTICIPANT, views, 64);
for (size_t i = 0; i < n; i++) {
    printf("%s %s:%u%s\n",
           views[i].name,
           views[i].host,
           views[i].port,
           views[i].is_local ? " (local)" : "");
}
```

- `NB_BUILTIN_TOPIC_PARTICIPANT` reports the local participant plus every
  remote participant the node has discovered: GUID in `handle`, name, the
  address it announced, and its lease in `lease_ms`.
- `NB_BUILTIN_TOPIC_PUBLICATION` and `..._SUBSCRIPTION` report local and
  discovered remote endpoints with topic, type, partition, `is_local`,
  `is_writer`, and the announced reliability, durability, history, deadline,
  lifespan and liveliness policies.
- `NB_BUILTIN_TOPIC_TOPIC` is derived from the endpoint tables: one entry per
  `(name, type)` pair, with `writer_count`/`reader_count` and a stable 16-byte
  handle. `DCPSTopic` is not a wire protocol, so nothing is announced for it.

The snapshot is local to the process and reflects discovery as of the call, so
it lags remote changes by the announcement and lease intervals like the
built-in topics do. Discovery is only available from transports that implement
the optional `discovery_read` provider op (currently `rtps`); a node without
such a transport returns 0.

## Metrics and logging

```c
nb_metrics_t metrics;
nb_node_metrics(node, &metrics);

const nb_metric_desc_t *desc = nb_metric_find("requests_sent_total");
```

Every registered metric has a `scope` (`NB_METRIC_SCOPE_NODE` / `_UDP` /
`_TCP` / `_UDP_RTCP` / `_RTPS`). `nb_metrics_t` stays a single struct, but
fields outside the current transport scope are always 0 so exporters can
filter on them.

### RTPS statistics

With the `rtps` transport the node also reports the RTPS scope:

```c
nb_metrics_t m;
nb_node_metrics(node, &m);
printf("rtps: %llu/%llu packets, %llu data in, %llu hb, %llu nacks\n",
       (unsigned long long)m.rtps_packets_sent,
       (unsigned long long)m.rtps_packets_received,
       (unsigned long long)m.rtps_data_received,
       (unsigned long long)m.rtps_heartbeats_received,
       (unsigned long long)m.rtps_acknacks_sent);
```

- Counters are cumulative: packets and bytes, SPDP/SEDP samples, DATA,
  HEARTBEAT, ACKNACK, GAP, fragments and NACK_FRAG, parse errors, samples
  rejected by the reader history policies, and expired participant leases.
- Gauges are current values: `rtps_participants`, `rtps_publications` and
  `rtps_subscriptions` (local plus discovered).
- `nb_rtps_participant_metrics()` reads one participant directly when the
  application owns it rather than a node.

### RTPS over TCP

Discovery is UDP in RTPS, so a participant always listens there first. Once two
participants have discovered each other they can move everything else - SEDP,
user data, heartbeats and acknowledgements - to a TCP connection:

```c
nb_rtps_participant_config_t config = {0};

config.domain_id = 7;
config.enable_tcp = 1;
config.tcp_port = 0;          /* 0 = ephemeral, advertised in SPDP */

nb_rtps_participant_t *participant = nb_rtps_participant_create(&config);
printf("tcp port %u, %zu link(s)\n",
       (unsigned)nb_rtps_participant_tcp_port(participant),
       nb_rtps_participant_tcp_links(participant));

nb_rtps_tcp_stats_t stats;
nb_rtps_participant_tcp_stats(participant, &stats);
printf("%llu bytes in, %llu out over TCP\n",
       (unsigned long long)stats.bytes_received,
       (unsigned long long)stats.bytes_sent);
```

The higher GUID prefix of a pair accepts and the lower one dials, so exactly one
connection exists per pair and both sides agree without configuration. A failed
dial is retried once per second while the peer is still known.

With the `rtps` transport provider the same switch is a query parameter:

```
rtps://7                                    UDP only (unchanged)
rtps://7?tcp                                TCP on an ephemeral port
rtps://7/10.0.0.5:47130?tcp=47300           explicit peers, TCP on 47300
```

Notes:

- SPDP stays on UDP, as the specification requires; the TCP locator is
  advertised inside the SPDP sample so a peer knows where to dial.
- A participant with TCP enabled still accepts UDP traffic, so a mixed
  deployment needs no change on the UDP-only side.

To dump the wire traffic, set `dump_frames = 1` in
`nb_rtps_participant_config_t`, or `NB_RTPS_DUMP_FRAMES=1` in the environment
before creating the node. Each datagram is logged at debug level with
direction, peer, length and the first 48 octets in hex; payloads are truncated
on purpose, so the dump is safe to leave on while diagnosing discovery or
reliability problems.

Metrics can be disabled with `node.metrics.disable`; with metrics disabled the
hot path keeps only one global flag check.

Logging:

```c
#define NB_LOG_TAG "app"
#include <nb/log.h>

nb_log_info("started node=%s", nb_node_name(node));
```

`nb_log_configure(false, level)` disables all log output at runtime.

## Error handling

Check the return value of every `create`, `start` and `call`. Stable names,
descriptions and retry flags are listed in [error codes](error-codes.md).

## Versioning and compatibility

The header API stays source-compatible across releases but is not guaranteed to
be binary-compatible: struct layouts may evolve. Rebuild consumers against the
matching library version when upgrading. `NB_VERSION_STRING` (`types.h`)
reports the header API version.

## Examples

Complete runnable code is in the [example index](../examples.md).
