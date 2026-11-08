#ifndef NB_CORE_CAPS_H
#define NB_CORE_CAPS_H

/*
 * 跨模块共享的编译期容量/上限常量。
 *
 * 命名约定：
 *   NB_GLOBAL_*    跨模块共享（路由长度、消息上限、tick 等）
 *   NB_<MODULE>_*  模块内常量（MESSAGE/NODE/STREAM/DISCOVERY/CODEC/MEM/TRANSPORT）
 *
 * 传输相关常量（分片/背压/扇出/可靠性/RTCP）见 transport/transport_caps.h；
 * 本头只保留跨模块（core / message / router / discovery）共用的常量。
 */

/* NB_UDP_FRAG_MIN_BYTES / NB_UDP_FRAG_MAX_BYTES 等；NB_TRANSPORT_MAX_FRAGMENTS 依赖前者。 */
#include "transport_caps.h"

#include <nb/types.h> /* NB_SERVICE_NAME_MAX / NB_METHOD_NAME_MAX */
#include <stdint.h>
#include <stdlib.h>

/* ---- GLOBAL / 跨模块 ---- */
/* "service.method" 的最大缓冲区长度（含结尾 '\0'）。 */
#define NB_GLOBAL_MAX_ROUTE_BYTES (NB_SERVICE_NAME_MAX + NB_METHOD_NAME_MAX + 2)
#define NB_GLOBAL_MAX_MSG_BYTES 1048576u /* 1 MiB */
#define NB_GLOBAL_MAX_MSG_BYTES_LIMIT NB_MAX_MSG_BYTES_LIMIT
/* 运行时 tick 默认值；环境变量 TICK_INTERVAL_MS 可覆盖。 */
#define NB_GLOBAL_TICK_INTERVAL_MS 2

/* ---- MESSAGE ---- */
/*
 * frame 固定头 + 扩展相对 payload 的最大额外字节：route/metadata/status 值，
 * 加上 5 个 TLV 头、status code、stream/deadline 定长值与 28B 固定头（80B 余量）。
 */
#define NB_MESSAGE_MAX_ENVELOPE_EXTRA \
    (NB_GLOBAL_MAX_ROUTE_BYTES + NB_METADATA_WIRE_MAX + NB_STATUS_MSG_MAX + 80u)

/* ---- NODE ---- */
#define NB_NODE_DEFAULT_MAX_INFLIGHT 65536u /* 客户端未完成调用上限（防无界增长） */
#define NB_NODE_RATE_LIMIT_SLOTS 256
#define NB_NODE_RATE_WINDOW_MS 1000

/* ---- STREAM ---- */
#define NB_STREAM_MAX_RECV_STREAMS NB_MAX_STREAM_RECV_STREAMS
#define NB_STREAM_IDLE_TIMEOUT_MS 15000

/* ---- DISCOVERY ---- */
#define NB_DISCOVERY_MAX_PEERS 64
#define NB_DISCOVERY_MAX_TARGETS 32
#define NB_DISCOVERY_MAX_SERVICES_PER_PEER 32

/* ---- CODEC ---- */
#define NB_CODEC_MAX 8

/* ---- MEM ---- */
/* per-poller 缓冲池：每档最多缓存的空闲块数，避免池无界增长 */
#define NB_MEM_BUF_POOL_MAX_PER_BUCKET 32

/* ---- TRANSPORT ---- */
/* 传输扇出（跨模块：node 出站分片也用到该上限）。 */
#define NB_TRANSPORT_MAX_IO NB_MAX_IO_THREADS
/* 单条消息分片数硬上限：覆盖 max_msg 上限 + 最小分片的最坏情况，仅拦截异常值。 */
#define NB_TRANSPORT_MAX_FRAGMENTS (NB_GLOBAL_MAX_MSG_BYTES_LIMIT / NB_UDP_FRAG_MIN_BYTES)

/*
 * Runtime-overridable tick period: env TICK_INTERVAL_MS wins, else the
 * compile-time default. A longer tick cuts timer overhead (measured ~10% more
 * throughput at saturation) but coarsens UDP NACK/RTX and TCP backlog flush,
 * hence configurable rather than hardcoded.
 */
static inline uint32_t nb_tick_interval_ms(void)
{
    static int inited = 0;
    static uint32_t value = NB_GLOBAL_TICK_INTERVAL_MS;
    if (!inited) {
        const char *env = getenv("TICK_INTERVAL_MS");
        if (env && env[0]) {
            int v = atoi(env);
            if (v > 0 && v <= 1000) {
                value = (uint32_t)v;
            }
        }
        inited = 1;
    }
    return value;
}

#endif /* NB_CORE_CAPS_H */
