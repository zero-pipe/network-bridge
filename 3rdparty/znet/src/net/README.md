# net/

## M1：`zn_socket`

| API | 说明 |
|-----|------|
| `zn_socket_listen` | TCP 监听，非阻塞 |
| `zn_socket_connect` | TCP 连接，支持 `EINPROGRESS` |
| `zn_socket_bind_udp` | UDP 绑定 |
| `zn_socket_accept` / send / recv / sendto / recvfrom | 基础 I/O |
| `zn_socket_attach_poller` | 挂到 `zn_poller`，事件回调 |

平台实现：

- 底层 fd/socket 已下沉到 **zbase**：`zb/os/socket.h`
  （Linux `src/os/linux/zb_sock_linux.c`，Win32 `src/os/win32/zb_sock_win32.c`）。
- znet 的 `zn_socket` 负责非阻塞状态机、poller 集成与回调；
  `src/net/internal/sock_platform.h` 保留 `zn_sockplat_*` 兼容宏，实现委托 zbase。

## M6：高层封装与组播

| 模块 | 头文件 |
|------|--------|
| TCP 客户端 | `zn/net/tcp_client.h` |
| UDP 服务 | `zn/net/udp_server.h`（多 poller + `SO_REUSEPORT`） |
| UDP 客户端 | `zn/net/udp_client.h` |
| 组播/广播 | `zn_socket_set_broadcast` / `join_multicast` 等 |

见 [docs/10-net-clients.md](../../docs/10-net-clients.md)。

## M2：`zn_tcp_server`

- 依赖外部 `zn_poller_pool`（I/O 由 pool 线程驱动；TcpServer 不创建 poller 线程）
- 同一 `listen fd` 注册到 pool 内每个 poller（抢占式 accept）
- `zn_tcp_session_ops`：`on_recv` / `on_error` / 可选 `on_manager`
- 可选 `pick_poller` 做负载均衡；`start` 时自动挂载 manager 巡检 timer

## M3–M5

- `zn_poller_async` / `zn_poller_run` — 见 [docs/06-poller-async.md](../../docs/06-poller-async.md)
- `zn_poller_do_delay` / `zn_timer_start`（定时驱动 epoll 超时）
- `zn_poller_pool` + 默认 `pick_poller` — 见 [docs/07-poller-pool.md](../../docs/07-poller-pool.md)
- TcpServer 用法 — [docs/09-tcp-server.md](../../docs/09-tcp-server.md)
- Win32 — [docs/04-win32.md](../../docs/04-win32.md)
