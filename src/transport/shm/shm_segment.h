#ifndef NB_SHM_SEGMENT_H
#define NB_SHM_SEGMENT_H

/*
 * iceoryx 风格的共享内存段（segment）。
 *
 * 设计要点（对应 iceoryx 的 MemPool / 无锁队列 / 偏移寻址 / 零拷贝 chunk）：
 *   - 段内一切引用都是“相对段基址的偏移”，不存指针，故可跨进程共享；
 *   - 按 size class 分档的 chunk 池，空闲链表用带 tag 的无锁栈（ABA 安全）；
 *   - 请求/响应队列（有界 MPMC，Vyukov 序列号算法）无锁、无临界区，
 *     消费端用信号量唤醒、一次唤醒排空整条队列；
 *   - chunk 在发送方借出、接收方释放，全程不拷贝消息体；
 *     接收方可以把它包装成 external nb_buf（frame_owner）延长生命周期。
 *
 * 与 iceoryx 的差异（刻意收敛实现规模/依赖）：
 *   - 无 RouDi 守护进程：段由首个打开者创建，由最后一个关闭者 unlink；
 *   - 队列深度/客户端数/分类大小都是编译期常量，段布局由段大小确定性推导，
 *     两个进程只要用同一段名即可得到一致的布局；
 *   - 首个打开者初始化空闲链表与队列；空段（active_nodes=0）可被下一次加入重建；
 *   - 发布/订阅：chunk 带引用计数，一次 publish 可零拷贝扇出给多个订阅者，
 *     每个消费者各减一次、最后一个归还池；订阅表按节点分槽（每节点只写自己的
 *     槽），发布者读取时用 seqlock 校验，无需跨进程锁。
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/buf.h>
#include <nb/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7 档 payload 容量：256B / 1K / 4K / 16K / 64K / 256K / 1M。 */
#define NB_SHM_CLASS_COUNT 7u
/* 1 = server，2..(NB_SHM_MAX_CLIENTS+1) = client id。 */
#define NB_SHM_MAX_CLIENTS 8u
/* Addressable node count (including server), also the number of subscriber
 * slots. Kept in sync with the public shm.h value. */
#ifndef NB_SHM_MAX_NODES
#define NB_SHM_MAX_NODES (NB_SHM_MAX_CLIENTS + 1u)
#endif
/* Max topics (service.method) a single node may subscribe to. */
#define NB_SHM_TOPICS_PER_NODE 16u
#define NB_SHM_QUEUE_DEPTH 256u
#define NB_SHM_CHUNK_HDR_BYTES 64u
#define NB_SHM_CHUNK_ALIGN 64u

#define NB_SHM_SLOT_MAGIC 0x4e425348u  /* "NBSH" */
#define NB_SHM_LAYOUT_VERSION 3u
#define NB_SHM_CHUNK_MAGIC 0x4e424348u /* "NBCH" */

#define NB_SHM_DEFAULT_SEGMENT_BYTES (32u * 1024u * 1024u)
#define NB_SHM_MIN_SEGMENT_BYTES (2u * 1024u * 1024u)

/* 各 size class 的 payload 容量（chunk 头之外的可用字节）。 */
extern const uint32_t nb_shm_class_bytes[NB_SHM_CLASS_COUNT];

typedef struct nb_shmseg nb_shmseg_t;

/* 打开/创建段。self_id 决定角色：1 = server，2.. = client。失败返回 NULL。 */
nb_shmseg_t *nb_shmseg_open(const char *channel, uint16_t self_id);
void nb_shmseg_close(nb_shmseg_t *seg);

/* ---- 通用节点入站队列（返回 0 表示成功，-1 表示满/空/参数错误） ---- */
/* 任意节点 -> 任意节点的统一入站队列；source_id 为发送方节点 id。 */
int nb_shmseg_push(nb_shmseg_t *seg, uint16_t target_id, uint64_t chunk_off);
int nb_shmseg_pop(nb_shmseg_t *seg, uint64_t *chunk_off, uint16_t *source_id);

/* ---- 兼容包装：id=1 为传统 server，id>=2 为 client ---- */
int nb_shmseg_push_request(nb_shmseg_t *seg, uint64_t chunk_off);
int nb_shmseg_pop_request(nb_shmseg_t *seg, uint64_t *chunk_off, uint16_t *client_id);
int nb_shmseg_push_response(nb_shmseg_t *seg, uint16_t client_id, uint64_t chunk_off);
int nb_shmseg_pop_response(nb_shmseg_t *seg, uint64_t *chunk_off, uint16_t *source_id);

/* ---- 发布/订阅（topic = "service.method"） ---- */
/*
 * 订阅表按节点分槽：每个节点只写自己的槽位（service 注册在单 poller 线程执行），
 * 发布者跨进程读取时用 seqlock 校验版本，因此无需任何跨进程锁。
 * 返回 0 = 成功；-1 = 参数错误或订阅槽已满。
 */
int nb_shmseg_subscribe(nb_shmseg_t *seg, const char *topic);
int nb_shmseg_unsubscribe(nb_shmseg_t *seg, const char *topic);

/*
 * 把已编码好的 chunk 零拷贝扇出给该主题的所有订阅者（不含发布者自身）：
 * 引用计数置为订阅者数，每个消费者各减一次、最后一个归还池；无订阅者时直接
 * 归还。返回 0 表示已受理（个别目标队列满时该订阅者被丢弃）；无论成功与否，
 * chunk 所有权都由本函数接管。
 */
int nb_shmseg_publish(nb_shmseg_t *seg, const char *topic, uint64_t chunk_off);

/* 等待本端“收包方向”的队列有数据；返回 0 = 被唤醒，-1 = 超时。 */
int nb_shmseg_wait(nb_shmseg_t *seg, int timeout_ms);
/* 唤醒本端等待者（stop 时用，避免线程卡在 sem_timedwait）。 */
void nb_shmseg_wake(nb_shmseg_t *seg);

/* ---- chunk ---- */
/* 分配一块容量 >= capacity 的 chunk，返回其段内偏移；0 表示无可用块。 */
uint64_t nb_shmseg_alloc(nb_shmseg_t *seg, size_t capacity);
/* 释放一次引用：引用计数减一，归零时才真正归还池。 */
void nb_shmseg_chunk_free(nb_shmseg_t *seg, uint64_t offset);
uint8_t *nb_shmseg_data(nb_shmseg_t *seg, uint64_t offset);
uint32_t nb_shmseg_capacity(nb_shmseg_t *seg, uint64_t offset);
/* 记录消息体长度（发送方写完 envelope 后设置）。 */
void nb_shmseg_set_length(nb_shmseg_t *seg, uint64_t offset, uint32_t length);
uint32_t nb_shmseg_length(nb_shmseg_t *seg, uint64_t offset);
/* 若 data 指向本段某 chunk 的数据区起始，返回其 offset；否则 0。 */
uint64_t nb_shmseg_locate(nb_shmseg_t *seg, const void *data);

/*
 * 把收到的 chunk 包装成 external nb_buf：core 释放该 nb_buf 时归还 chunk，
 * 接收方持有期间负载寿命由 nb_buf 保证。
 */
nb_buf_t *nb_shmseg_wrap_rx(nb_shmseg_t *seg, uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif /* NB_SHM_SEGMENT_H */
