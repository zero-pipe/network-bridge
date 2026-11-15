#ifndef ZN_NET_NET_LIMITS_H
#define ZN_NET_NET_LIMITS_H

/**
 * znet 网络/监听策略常量（非协议魔数）
 */
#include "../zn_export.h"

#define ZN_NET_ADDR_STR_LEN           64
#define ZN_NET_PORT_STR_LEN           16

#define ZN_TCP_DEFAULT_BACKLOG        128
/** TCP 会话发送缓冲初始容量（与 UDP 收包缓冲对齐） */
#define ZN_TCP_SESSION_OUT_INIT_CAP   65536

#define ZN_MS_PER_SEC                 1000

#endif /* ZN_NET_NET_LIMITS_H */
