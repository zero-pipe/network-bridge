# 15: Same-Host SHM RPC

The server and client run on the same Linux host and complete RPC over POSIX
shared memory, without the UDP/TCP stack and without service discovery. This
example shows a STATIC direct connection; the same provider also supports MESH
(see below).

```bash
./example_15_shm_server
# in another terminal
./example_15_shm_client
```

Endpoint conventions:

- server: `shm://nb-example:1`
- client: `shm://nb-example:2` (a third client uses `:3`, up to `:9`)
- client proxy target: `shm://nb-example:1`

Implementation highlights (iceoryx-style zero copy):

- one shared-memory segment per channel, with size-classed chunk pools and
  lock-free queues;
- the sender encodes the envelope directly in the segment; the receiver holds
  the chunk through a `frame_owner` (an external `nb_buf`), so large messages
  cross threads without copying the body;
- the server (port 1) owns the segment lifetime, and up to 8 clients share it.

## MESH mode

SHM no longer requires fixed server/client roles. Every node in a channel joins
the segment with its own node id, and mesh broadcasts the full binding URI
(`shm://<channel>:<node-id>`) over UDP HELLO, so either side can be any node:

```c
/* node A: shm://orders:1, node B: shm://orders:2, both in MESH mode */
binding.endpoint = "shm://orders:2";
cfg.mode = NB_NODE_MODE_MESH;
```

As long as the discovery ports of both nodes can reach each other (same
broadcast domain, or mutual `discovery.peers`), a client discovers
`shm://orders:2` by service name and calls it directly without
`nb_proxy_set_target_uri`.

- Node ids within one channel must be unique in the range `1..9`.
- The segment is initialized by the first creator and unlinked by the last
  closer; node 1 no longer has to stay alive.
- Events still fan out by `service.method` to every subscribed node, and the
  publisher does not receive its own events.

Publish/subscribe (same usage as DDS):

- A subscriber is a regular service whose method name is the topic:
  `nb_service_add_method(sub, "chat", ...)`. Registration writes
  `service.method` into the segment's subscription table.
- A publisher sends with
  `nb_proxy_call_event(pub, "service", "chat", &payload, NULL)` and points its
  endpoint at the same channel, for example `shm://nb-example:1`.
- Events fan out to every subscriber node by route with zero copy; the publisher
  does not receive its own events. Chunks carry a reference count and return to
  the pool only after the last consumer releases them.
- Subscribers reuse their inbound queue according to role: a server uses its
  request queue and a client uses its own response queue. Events and responses
  share a queue and the framework dispatches them by message type.

Limitations and tuning:

- Linux only (Windows and other platforms return `NB_ERR_NOT_SUPPORTED`).
- A single encoded envelope is limited to 1 MiB (the largest size class).
- The concurrency limit for large messages is bounded by the chunk count of that
  size class; when no chunk is available, sending returns `NB_ERR_AGAIN`
  (retryable). Set `NB_SHM_BYTES` to grow the segment (default 32 MiB, minimum
  2 MiB); every process on the same channel must use the same value.
- Each node subscribes to at most 16 topics (routes) and a channel holds at most
  9 nodes.
- When one subscriber's queue is full, that event is dropped for that subscriber
  only; other subscribers are unaffected.
- Starting the client first is allowed, but rebuilding the segment on the server
  side discards messages and subscriptions from the previous generation.
  For publish/subscribe scenarios, start the server first so clients register
  against a clean segment.

The implementation lives in `src/transport/shm/`; the public registration API is
in `include/nb/shm.h`.
