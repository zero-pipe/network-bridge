/*
 * 取消登记表（RPC 面）。
 *
 * 客户端发 KIND_CANCEL 后按 request_id + 对端登记；worker 任务启动前查表跳过，
 * 已运行的 handler 通过 nb_call_canceled() 观测（无法抢占）。登记项由节点的
 * 定时器按 TTL 过期。
 */
#include "node_internal.h"
#include "service_internal.h"
#include "util.h"

#include "atomic.h"
#include "envelope.h"

#include <nb/log.h>

#include <string.h>

void nb_node_cancel_record(nb_node_t *node,
                            uint32_t request_id,
                            const char *peer_host,
                            uint16_t peer_port)
{
    int i;
    int free_slot = -1;
    uint64_t now;
    if (!node || !node->handler_limit_mutex) {
        return;
    }
    now = nb_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        nb_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms <= now) {
            s->used = 0;
            if (node->cancel_count > 0) {
                nb_atomic_sub_u32(&node->cancel_count, 1u);
            }
        }
        if (!s->used) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        /* 同一调用的重复取消：刷新 TTL 即可。 */
        if (s->request_id == request_id && s->peer_port == peer_port &&
            strcmp(s->peer_host, peer_host ? peer_host : "") == 0) {
            s->expires_ms = now + CANCEL_TTL_MS;
            ntk_mutex_unlock(node->handler_limit_mutex);
            return;
        }
    }
    if (free_slot >= 0) {
        nb_cancel_slot_t *s = &node->cancels[free_slot];
        s->used = 1;
        s->request_id = request_id;
        nb_copy_str(s->peer_host, sizeof(s->peer_host), peer_host, "0.0.0.0");
        s->peer_port = peer_port;
        s->expires_ms = now + CANCEL_TTL_MS;
        nb_atomic_add_u32(&node->cancel_count, 1u);
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}

int nb_node_is_canceled(const nb_node_t *node,
                         uint32_t request_id,
                         const char *peer_host,
                         uint16_t peer_port)
{
    int found = 0;
    int i;
    uint64_t now;
    if (!node || !node->handler_limit_mutex ||
        nb_atomic_load_u32(&node->cancel_count) == 0) {
        return 0;
    }
    now = nb_now_ms();
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        const nb_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms > now && s->request_id == request_id &&
            s->peer_port == peer_port && strcmp(s->peer_host, peer_host ? peer_host : "") == 0) {
            found = 1;
            break;
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
    return found;
}

void nb_node_expire_cancels(nb_node_t *node, uint64_t now_ms)
{
    int i;
    if (!node || !node->handler_limit_mutex) {
        return;
    }
    ntk_mutex_lock(node->handler_limit_mutex);
    for (i = 0; i < CANCEL_SLOTS; i++) {
        nb_cancel_slot_t *s = &node->cancels[i];
        if (s->used && s->expires_ms <= now_ms) {
            s->used = 0;
            if (node->cancel_count > 0) {
                nb_atomic_sub_u32(&node->cancel_count, 1u);
            }
        }
    }
    ntk_mutex_unlock(node->handler_limit_mutex);
}


/*
 * 客户端取消：按 request_id + 对端登记。worker 任务启动前会查表跳过；
 * 已运行的 handler 可通过 nb_call_canceled() 观测（无法抢占）。
 */
void nb_service_handle_cancel(nb_node_t *node, nb_transport_t *transport, const nb_message_t *msg)
{
    (void)transport;
    if (!node || !msg || msg->kind != KIND_CANCEL) {
        return;
    }
    nb_node_cancel_record(node, msg->request_id, msg->peer_host, msg->peer_port);
}


bool nb_call_canceled(const nb_call_t *call)
{
    if (!call) {
        return false;
    }
    return nb_node_is_canceled(call->node, call->msg_id, call->peer_host, call->peer_port);
}
