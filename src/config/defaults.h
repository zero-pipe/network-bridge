#ifndef NB_CONFIG_DEFAULTS_H
#define NB_CONFIG_DEFAULTS_H

/*
 * 配置默认值 / 范围 / 语义总表（单一真源）。
 *
 * config 解析、校验与 node 装配都引用这里，避免同一默认值散落在多处导致漂移。
 * 表中“0”多数表示“使用默认”，由 nb_node_config_normalize 兜底为下表值。
 *
 * 解析（nb_config_parse）对用户输入做严格校验：超出“范围”直接返回
 * NB_ERR_INVALID；normalize 只服务于代码直接构造的配置，负责填默认值并把
 * 越界值夹到范围内。两者使用同一组上下界，docs/api/config.md 与
 * docs/config.schema.json 必须与此表一致。
 *
 * 项                        默认               范围                              语义
 * ------------------------- ------------------ --------------------------------- --------------------
 * io_threads                0(=硬件并发)       0..NB_TRANSPORT_MAX_IO            poller 分片线程数
 * handler_threads           0(关)              0..NB_MAX_HANDLER_THREADS         worker 线程池
 * handler_max_concurrency   0(不限)            0..N                              worker 在途上限
 * max_inflight              65536              0..N                              客户端 pending 上限
 * max_streams               64                 0..NB_STREAM_MAX_RECV_STREAMS     并发接收流上限
 * peer_rate_limit_pps       0(不限)            0..N                              每对端入站限速
 * send_lanes                0(关)              0..NB_MAX_SEND_LANES              专用发送 poller 线程
 * frag_bytes                1440               256..NB_UDP_FRAG_MAX_BYTES        UDP 分片
 * max_msg_bytes             1 MiB              0..NB_GLOBAL_MAX_MSG_BYTES_LIMIT  单条消息上限
 * backpressure_bytes        4 MiB              0..N                              出站高水位
 * rtcp_interval_ms          1000               0..N                              UDP RTCP 周期
 * hello_interval_ms         3000               0..N                              发现 HELLO 周期
 * node_ttl_ms               9000               0..N                              对端过期
 * metrics.sample_every      1                  1..N                              延迟采样
 *
 * 说明：capacity（缓冲/分片/队列容量）仍定义在 caps.h / transport_caps.h；
 * 本头只登记 policy 默认值与校验上界，避免同一默认值散落多处。
 */

#include "caps.h" /* NB_GLOBAL_MAX_MSG_BYTES / NB_STREAM_MAX_RECV_STREAMS / NB_TRANSPORT_MAX_IO */

#define NB_DEFAULT_NODE_NAME "nb"
#define NB_DEFAULT_BIND_HOST "0.0.0.0"
#define NB_DEFAULT_DISCOVERY_HOST "255.255.255.255"
#define NB_DEFAULT_HELLO_INTERVAL_MS 3000u
#define NB_DEFAULT_NODE_TTL_MS 9000u
#define NB_DEFAULT_METRICS_SAMPLE_EVERY 1u

/* capacity 常量经 defaults 单点引用，node/config 不再各自挑来源。 */
#define NB_DEFAULT_MAX_MSG_BYTES NB_GLOBAL_MAX_MSG_BYTES
#define NB_DEFAULT_FRAG_BYTES NB_UDP_FRAG_DEFAULT_BYTES
#define NB_DEFAULT_FRAG_MIN_BYTES NB_UDP_FRAG_MIN_BYTES
#define NB_DEFAULT_FRAG_MAX_BYTES NB_UDP_FRAG_MAX_BYTES
#define NB_DEFAULT_MAX_INFLIGHT NB_NODE_DEFAULT_MAX_INFLIGHT
#define NB_DEFAULT_MAX_STREAMS NB_STREAM_MAX_RECV_STREAMS
#define NB_DEFAULT_BACKPRESSURE_BYTES NB_TRANSPORT_BACKPRESSURE_DEFAULT_BYTES
#define NB_DEFAULT_RTCP_INTERVAL_MS NB_UDP_RTCP_INTERVAL_MS
#define NB_DEFAULT_DISCOVERY_PORT NB_DISCOVERY_PORT

/* 校验上界：解析时超过即判非法，normalize 时夹到上界。 */
/* 配置文件大小上限：正常配置远小于此值，防止误指向大文件时整读进内存。 */
#define NB_CONFIG_MAX_FILE_BYTES (1024u * 1024u)

#endif /* NB_CONFIG_DEFAULTS_H */
