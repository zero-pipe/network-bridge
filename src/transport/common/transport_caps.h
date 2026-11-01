#ifndef NB_TRANSPORT_CAPS_H
#define NB_TRANSPORT_CAPS_H

/*
 * 传输层编译期容量/上限常量（UDP/TCP 扇出、分片、可靠性、RTCP）。
 *
 * 命名约定：
 *   NB_TRANSPORT_*  通用传输层策略
 *   NB_UDP_*        UDP 分片、批处理、RTCP
 *   NB_TCP_*        TCP 连接表
 *   NB_RPC_*        RPC 级重传策略（RTO / 最大重传次数）
 */

#include "framing.h"

#include <stdint.h>
#include <stdlib.h>

/* ---- TRANSPORT / 通用 ---- */
#define NB_TRANSPORT_BACKPRESSURE_DEFAULT_BYTES (4u * 1024u * 1024u)

/*
 * 接收路径按尺寸分档的惰性池。小帧走小分档提高命中率，大帧也能池化，
 * 避免热路径回退 malloc；每档 slot_count 是硬上限，池耗尽才回退 nb_buf_alloc。
 * 4K 覆盖普通 RPC，16K/80K 覆盖 8K/64K 帧，2M 覆盖默认 max_msg_bytes 整帧。
 *
 * 可在编译期覆盖（例如 -DNB_TRANSPORT_RX_CLASS_SLOTS=...）按并发/负载调参。
 */
#ifndef NB_TRANSPORT_RX_CLASS_COUNT
#define NB_TRANSPORT_RX_CLASS_COUNT 5u
#endif
#ifndef NB_TRANSPORT_RX_CLASS_BYTES
#define NB_TRANSPORT_RX_CLASS_BYTES { 4096u, 16384u, 81920u, 262144u, 2097152u }
#endif
#ifndef NB_TRANSPORT_RX_CLASS_SLOTS
#define NB_TRANSPORT_RX_CLASS_SLOTS { 128u, 64u, 32u, 8u, 2u }
#endif

/* ---- UDP / 分片与批处理 ---- */
/*
 * IPv4 1500B MTU 下，单个 UDP 数据报的 envelope 分片预算：
 * 1500 - 20(IP) - 8(UDP) - 12(RTP) - 20(分片头) = 1440。
 */
#define NB_UDP_IPV4_MTU_BYTES 1500
#define NB_UDP_IPV4_HDR_BYTES 20
#define NB_UDP_HDR_BYTES 8
#define NB_UDP_WIRE_OVERHEAD_BYTES \
    (NB_UDP_IPV4_HDR_BYTES + NB_UDP_HDR_BYTES + RTP_HDR_LEN + FRAG_HDR_LEN)
#define NB_UDP_FRAG_DEFAULT_BYTES (NB_UDP_IPV4_MTU_BYTES - NB_UDP_WIRE_OVERHEAD_BYTES)
#define NB_UDP_FRAG_MIN_BYTES 256
#define NB_UDP_FRAG_MAX_BYTES 8192
#define NB_UDP_PEERS_PER_IO 64
#define NB_UDP_TX_BATCH_MAX 32
#define NB_UDP_RX_BATCH_MAX 16
/* 拥塞窗口下限：保证 16 KiB（14 个分片）始终能一次批量发出。 */
#define NB_UDP_CC_BURST_MIN 16
#define NB_UDP_RTX_SLOT_MAX_BYTES (RTP_HDR_LEN + FRAG_HDR_LEN + NB_UDP_FRAG_MAX_BYTES)

/* ---- UDP / 可靠性 ---- */
#define NB_UDP_REASM_TIMEOUT_MS 3000
#define NB_UDP_NACK_INTERVAL_MS 10
#define NB_UDP_MAX_NACK_SEQS 64

/* ---- UDP / RTCP ---- */
#define NB_UDP_RTCP_INTERVAL_MS 1000 /* SR/RR 上报周期 */

/* ---- TCP ---- */
#define NB_TCP_LINKS_PER_IO 64

/* ---- RPC / 重传 ---- */
#define NB_RPC_RTO_INIT_MS 150
#define NB_RPC_MAX_RETX 5

#endif /* NB_TRANSPORT_CAPS_H */
