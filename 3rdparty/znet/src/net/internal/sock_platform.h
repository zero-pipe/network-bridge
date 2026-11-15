#ifndef ZN_SOCK_PLATFORM_H
#define ZN_SOCK_PLATFORM_H

/*
 * znet 兼容层：底层 socket 实现在 zbase。
 * 符号保持 zn_sockplat_* 供 znet 内部使用；新代码可直接用 zb/os/socket.h。
 */
#include "zn/net/socket.h"
#include "zb/os/socket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef zb_sock_iov zn_sock_iov;
#define ZN_SOCK_IOV_MAX ZB_SOCK_IOV_MAX

#define zn_sockplat_init zb_sock_init
#define zn_sockplat_fini zb_sock_fini
#define zn_sockplat_apply_tcp_server_opts zb_sock_apply_tcp_server_opts
#define zn_sockplat_apply_tcp_client_opts zb_sock_apply_tcp_client_opts
#define zn_sockplat_apply_udp_opts zb_sock_apply_udp_opts
#define zn_sockplat_listen zb_sock_listen
#define zn_sockplat_bind_udp zb_sock_bind_udp
#define zn_sockplat_connect zb_sock_connect
#define zn_sockplat_accept zb_sock_accept
#define zn_sockplat_send zb_sock_send
#define zn_sockplat_sendv zb_sock_sendv
#define zn_sockplat_recv zb_sock_recv
#define zn_sockplat_sendto zb_sock_sendto
#define zn_sockplat_recvfrom zb_sock_recvfrom
#define zn_sockplat_get_local zb_sock_get_local
#define zn_sockplat_get_peer zb_sock_get_peer
#define zn_sockplat_check_connect zb_sock_check_connect
#define zn_sockplat_close zb_sock_close
#define zn_sockplat_set_tcp_bbr zb_sock_set_tcp_bbr
#define zn_sockplat_set_broadcast zb_sock_set_broadcast
#define zn_sockplat_set_multicast_ttl zb_sock_set_multicast_ttl
#define zn_sockplat_set_multicast_if zb_sock_set_multicast_if
#define zn_sockplat_set_multicast_loop zb_sock_set_multicast_loop
#define zn_sockplat_join_multicast zb_sock_join_multicast
#define zn_sockplat_leave_multicast zb_sock_leave_multicast
#define zn_sockplat_join_multicast_filter zb_sock_join_multicast_filter
#define zn_sockplat_leave_multicast_filter zb_sock_leave_multicast_filter

#ifdef __cplusplus
}
#endif

#endif /* ZN_SOCK_PLATFORM_H */
