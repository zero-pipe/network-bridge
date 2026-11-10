#ifndef NB_DEDUP_H
#define NB_DEDUP_H

/*
 * 服务端请求去重 / 响应重放缓存。
 *
 * UDP 的整包重传（pending RTO）会重复投递同一 request_id；handler 若非幂等，
 * 重复执行就是 bug。这里按 (request_id, peer) 记录在途/已完成请求：
 *   - 已完成：直接重放缓存响应，不再执行 handler；
 *   - 在途  ：丢弃重复，原响应到达后客户端自会拿到；
 *   - 未命中：登记在途，交给上层正常执行。
 *
 * 只对 UDP unary 生效（TCP 可靠无需去重）。分片 + 固定槽 + TTL + 字节预算，
 * 访问用每分片一把锁，热路径无全局争用。
 */

#include <nb/rpc/types.h>

#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

#include <nb/rpc/message.h>
#include "transport_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DEDUP_SHARDS 16
#define NB_DEDUP_SLOTS_PER_SHARD 16
#define NB_DEDUP_TTL_MS 10000u
#define NB_DEDUP_MAX_BYTES (8u * 1024u * 1024u)

typedef struct nb_dedup_slot {
    bool used;
    bool completed;
    uint32_t request_id;
    char peer_host[NB_HOST_MAX];
    uint16_t peer_port;
    uint64_t expires_ms;
    uint64_t last_ms;
    int status;
    uint8_t encoding;
    uint8_t *payload;
    nb_buf_t *payload_owner; /* 非空时 payload 指向 owner 内部，不单独释放 */
    size_t payload_len;
    uint8_t *metadata;
    uint16_t metadata_len;
    char *message;
    uint16_t message_len;
} nb_dedup_slot_t;

typedef struct nb_dedup_shard {
    ntk_mutex *lock;
    size_t bytes;
    nb_dedup_slot_t slots[NB_DEDUP_SLOTS_PER_SHARD];
} nb_dedup_shard_t;

typedef struct nb_dedup {
    nb_dedup_shard_t shards[NB_DEDUP_SHARDS];
} nb_dedup_t;

/* nb_dedup_begin 给出的处置结果。 */
typedef enum nb_dedup_action {
    NB_DEDUP_PROCESS = 0, /* 新请求，交给上层正常执行 */
    NB_DEDUP_REPLAYED,    /* 命中已完成请求，缓存响应已重放 */
    NB_DEDUP_IN_FLIGHT,   /* 命中在途请求，重复已丢弃 */
} nb_dedup_action_t;

void nb_dedup_init(nb_dedup_t *d);
void nb_dedup_cleanup(nb_dedup_t *d);

/* 判定请求如何处置；未启用去重（无锁）时按新请求处理。 */
nb_dedup_action_t nb_dedup_begin(nb_dedup_t *d,
                                 nb_transport_t *transport,
                                 const nb_message_t *req);

/*
 * 记录最终响应供后续重复请求重放。payload_owner 非空时由缓存接管，调用方不得再释放；
 * 返回 true 表示 owner 已被接管（未传 owner 或缓存失败时返回 false，owner 仍归调用方）。
 */
bool nb_dedup_complete(nb_dedup_t *d,
                       const nb_message_t *req,
                       int status,
                       const nb_payload_t *payload,
                       const void *metadata,
                       uint16_t metadata_len,
                       const char *message,
                       uint16_t message_len,
                       nb_buf_t *payload_owner);

/* 处理失败时撤销登记，允许重传重试。 */
void nb_dedup_abort(nb_dedup_t *d, const nb_message_t *req);
void nb_dedup_expire(nb_dedup_t *d, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_DEDUP_H */
