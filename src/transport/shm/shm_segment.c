#if defined(__linux__)

#include "shm_segment.h"

#include "util.h"

#define NB_LOG_TAG "shm"
#include <nb/log.h>

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const uint32_t nb_shm_class_bytes[NB_SHM_CLASS_COUNT] = {
    256u, 1024u, 4096u, 16384u, 65536u, 262144u, 1048576u,
};

/* ---- 段内布局（只在本文件可见） ---- */

typedef struct nb_shm_cell {
    uint64_t seq;    /* Vyukov 序列号 */
    uint64_t offset; /* chunk 段内偏移 */
    uint32_t client;  /* 请求方 id（响应队列不使用） */
    uint32_t reserved;
} nb_shm_cell_t;

typedef struct nb_shm_queue {
    uint64_t enqueue_pos;
    uint64_t dequeue_pos;
    nb_shm_cell_t cells[NB_SHM_QUEUE_DEPTH];
} nb_shm_queue_t;

typedef struct nb_shm_class_hdr {
    uint64_t chunk_bytes; /* 每块 payload 容量 */
    uint64_t stride;      /* 头 + payload，按 64B 对齐 */
    uint64_t count;
    uint64_t arena_off; /* 本档第一块的段内偏移 */
    uint64_t free_head; /* tagged: (tag << 32) | (index + 1)，0 = 空 */
} nb_shm_class_hdr_t;

typedef struct nb_shm_chunk_hdr {
    uint64_t offset;   /* 自身段内偏移（零拷贝校验） */
    uint32_t magic;
    uint32_t class_idx;
    uint32_t capacity; /* payload 容量 */
    uint32_t length;   /* 已写入的消息体字节数 */
    uint32_t next1;    /* 空闲链表：下一个 index + 1 */
    uint32_t refcount; /* 引用计数：1 = 单所有者；publish 后 = 订阅者数 */
    uint8_t reserved[NB_SHM_CHUNK_HDR_BYTES - 8 - 4 - 4 - 4 - 4 - 4 - 4];
} nb_shm_chunk_hdr_t;

typedef char nb_shm_chunk_hdr_size_check[(sizeof(nb_shm_chunk_hdr_t) == NB_SHM_CHUNK_HDR_BYTES)
                                             ? 1
                                             : -1];

/*
 * 每节点一张订阅表：只有该节点写入自己的槽（service 注册串行在单 poller），
 * 发布者跨进程读取时靠 seqlock（version 奇数 = 写入中）拿到一致快照。
 */
typedef struct nb_shm_sub_list {
    uint64_t version;
    uint64_t hashes[NB_SHM_TOPICS_PER_NODE]; /* 主题 FNV-1a 哈希；0 = 空槽 */
} nb_shm_sub_list_t;

typedef struct nb_shm_header {
    uint32_t magic;
    uint32_t layout;
    uint32_t ready; /* 0 = 初始化中，1 = 可用 */
    uint32_t reserved;
    uint64_t segment_bytes;
    uint64_t arena_off;
    uint64_t arena_bytes;
    int64_t owner_pid;
    nb_shm_class_hdr_t classes[NB_SHM_CLASS_COUNT];
    sem_t inbox_sem[NB_SHM_MAX_NODES];
    nb_shm_queue_t inbox[NB_SHM_MAX_NODES];
    uint32_t active_nodes;
    nb_shm_sub_list_t subs[NB_SHM_MAX_NODES];
} nb_shm_header_t;

struct nb_shmseg {
    char name[128];
    char channel[NB_HOST_MAX];
    uint16_t self_id;
    int creator;
    int fd;
    void *map;
    size_t map_size;
    nb_shm_header_t *hdr;
};

/* ---- 小工具 ---- */

static uint64_t nb_shm_align_up(uint64_t v, uint64_t align)
{
    return (v + align - 1u) & ~(align - 1u);
}

static uint64_t nb_shm_pack(uint32_t idx1, uint32_t tag)
{
    return ((uint64_t)tag << 32) | (uint64_t)idx1;
}

static nb_shm_chunk_hdr_t *nb_shm_chunk_at(nb_shmseg_t *seg,
                                           const nb_shm_class_hdr_t *cls,
                                           uint64_t index)
{
    return (nb_shm_chunk_hdr_t *)((uint8_t *)seg->map + cls->arena_off +
                                  index * cls->stride);
}

static int nb_shm_chunk_valid(nb_shmseg_t *seg, uint64_t offset)
{
    const nb_shm_header_t *h = seg->hdr;
    const nb_shm_chunk_hdr_t *ch;

    if (offset < h->arena_off || offset >= h->arena_off + h->arena_bytes ||
        offset % NB_SHM_CHUNK_ALIGN != 0) {
        return 0;
    }
    ch = (const nb_shm_chunk_hdr_t *)((const uint8_t *)seg->map + offset);
    if (ch->magic != NB_SHM_CHUNK_MAGIC || ch->offset != offset ||
        ch->class_idx >= NB_SHM_CLASS_COUNT) {
        return 0;
    }
    return offset >= h->classes[ch->class_idx].arena_off;
}

/* ---- 订阅表 ---- */

static uint64_t nb_shm_topic_hash(const char *topic)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a 64 */

    while (topic && *topic) {
        h ^= (uint8_t)*topic++;
        h *= 1099511628211ull;
    }
    return h ? h : 1u; /* 0 保留为空槽 */
}

/* 读取某节点是否订阅了 hash：seqlock 校验，写入中则重试。 */
static int nb_shm_sub_has(nb_shm_header_t *h, unsigned idx, uint64_t hash)
{
    nb_shm_sub_list_t *l = &h->subs[idx];
    unsigned i;

    for (;;) {
        uint64_t v1 = __atomic_load_n(&l->version, __ATOMIC_ACQUIRE);
        uint64_t v2;
        int found = 0;

        if (v1 & 1u) {
            continue; /* 写入中，重试 */
        }
        for (i = 0; i < NB_SHM_TOPICS_PER_NODE; i++) {
            if (__atomic_load_n(&l->hashes[i], __ATOMIC_RELAXED) == hash) {
                found = 1;
                break;
            }
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        v2 = __atomic_load_n(&l->version, __ATOMIC_ACQUIRE);
        if (v1 == v2) {
            return found;
        }
    }
}

/* 更新本节点订阅：add=1 加入（已存在则无操作），add=0 移除。返回 0 成功，-1 槽满。 */
static int nb_shm_sub_update(nb_shmseg_t *seg, uint64_t hash, int add)
{
    nb_shm_sub_list_t *l = &seg->hdr->subs[seg->self_id - 1u];
    uint64_t v = __atomic_load_n(&l->version, __ATOMIC_RELAXED);
    unsigned i;
    int slot = -1;

    for (i = 0; i < NB_SHM_TOPICS_PER_NODE; i++) {
        if (l->hashes[i] == hash) {
            slot = (int)i;
            break;
        }
        if (l->hashes[i] == 0u && slot < 0) {
            slot = (int)i;
        }
    }
    if (add) {
        if (slot >= 0 && l->hashes[slot] == hash) {
            return 0; /* 已订阅 */
        }
        if (slot < 0) {
            return -1; /* 槽满 */
        }
    } else if (slot < 0 || l->hashes[slot] != hash) {
        return 0; /* 未订阅，幂等 */
    }
    __atomic_store_n(&l->version, v + 1u, __ATOMIC_RELEASE); /* 奇数 = 写入中 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    l->hashes[slot] = add ? hash : 0u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&l->version, v + 2u, __ATOMIC_RELEASE); /* 偶数 = 稳定 */
    return 0;
}

int nb_shmseg_subscribe(nb_shmseg_t *seg, const char *topic)
{
    if (!seg || !seg->hdr || !topic || !topic[0]) {
        return -1;
    }
    return nb_shm_sub_update(seg, nb_shm_topic_hash(topic), 1);
}

int nb_shmseg_unsubscribe(nb_shmseg_t *seg, const char *topic)
{
    if (!seg || !seg->hdr || !topic || !topic[0]) {
        return -1;
    }
    return nb_shm_sub_update(seg, nb_shm_topic_hash(topic), 0);
}

/* ---- 空闲链表（带 tag 的无锁栈，ABA 安全） ---- */

static int nb_shm_class_pop(nb_shmseg_t *seg, nb_shm_class_hdr_t *cls, uint64_t *index_out)
{
    uint64_t head = __atomic_load_n(&cls->free_head, __ATOMIC_ACQUIRE);

    for (;;) {
        uint32_t idx1 = (uint32_t)head;
        uint32_t tag = (uint32_t)(head >> 32);
        nb_shm_chunk_hdr_t *ch;
        uint64_t next;

        if (idx1 == 0u || (uint64_t)idx1 > cls->count) {
            return -1; /* 空或链表损坏：当作无可用块。 */
        }
        ch = nb_shm_chunk_at(seg, cls, (uint64_t)idx1 - 1u);
        next = nb_shm_pack(ch->next1, tag + 1u);
        if (__atomic_compare_exchange_n(
                &cls->free_head, &head, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *index_out = (uint64_t)idx1 - 1u;
            return 0;
        }
        /* CAS 失败时 head 已更新为当前值，继续重试。 */
    }
}

static void nb_shm_class_push(nb_shmseg_t *seg, nb_shm_class_hdr_t *cls, uint64_t index)
{
    uint64_t head = __atomic_load_n(&cls->free_head, __ATOMIC_ACQUIRE);

    for (;;) {
        uint32_t tag = (uint32_t)(head >> 32);
        nb_shm_chunk_hdr_t *ch = nb_shm_chunk_at(seg, cls, index);
        uint64_t next;

        ch->next1 = (uint32_t)head;
        next = nb_shm_pack((uint32_t)(index + 1u), tag + 1u);
        if (__atomic_compare_exchange_n(
                &cls->free_head, &head, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

/* ---- 有界 MPMC 队列（Vyukov） ---- */

static void nb_shm_queue_init(nb_shm_queue_t *q)
{
    uint64_t i;

    q->enqueue_pos = 0;
    q->dequeue_pos = 0;
    for (i = 0; i < NB_SHM_QUEUE_DEPTH; i++) {
        q->cells[i].seq = i;
        q->cells[i].offset = 0;
        q->cells[i].client = 0;
        q->cells[i].reserved = 0;
    }
}

/* 返回 1 = 入队成功且入队前队列为空（调用方应唤醒消费端），0 = 成功但队列非空，
 * -1 = 队满。 */
static int nb_shm_queue_push(nb_shm_queue_t *q, uint64_t offset, uint32_t client)
{
    uint64_t pos = __atomic_load_n(&q->enqueue_pos, __ATOMIC_RELAXED);

    for (;;) {
        nb_shm_cell_t *cell = &q->cells[pos % NB_SHM_QUEUE_DEPTH];
        uint64_t seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);
        int64_t dif = (int64_t)seq - (int64_t)pos;

        if (dif == 0) {
            if (__atomic_compare_exchange_n(
                    &q->enqueue_pos, &pos, pos + 1u, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                break;
            }
        } else if (dif < 0) {
            return -1;
        } else {
            pos = __atomic_load_n(&q->enqueue_pos, __ATOMIC_RELAXED);
        }
    }
    {
        int was_empty;
        nb_shm_cell_t *cell = &q->cells[pos % NB_SHM_QUEUE_DEPTH];

        was_empty = pos == __atomic_load_n(&q->dequeue_pos, __ATOMIC_ACQUIRE);
        cell->offset = offset;
        cell->client = client;
        __atomic_store_n(&cell->seq, pos + 1u, __ATOMIC_RELEASE);
        return was_empty ? 1 : 0;
    }
}

static int nb_shm_queue_pop(nb_shm_queue_t *q, uint64_t *offset, uint32_t *client)
{
    uint64_t pos = __atomic_load_n(&q->dequeue_pos, __ATOMIC_RELAXED);

    for (;;) {
        nb_shm_cell_t *cell = &q->cells[pos % NB_SHM_QUEUE_DEPTH];
        uint64_t seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);
        int64_t dif = (int64_t)seq - (int64_t)(pos + 1u);

        if (dif == 0) {
            if (__atomic_compare_exchange_n(
                    &q->dequeue_pos, &pos, pos + 1u, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                break;
            }
        } else if (dif < 0) {
            return -1;
        } else {
            pos = __atomic_load_n(&q->dequeue_pos, __ATOMIC_RELAXED);
        }
    }
    {
        nb_shm_cell_t *cell = &q->cells[pos % NB_SHM_QUEUE_DEPTH];

        *offset = cell->offset;
        if (client) {
            *client = cell->client;
        }
        __atomic_store_n(&cell->seq, pos + NB_SHM_QUEUE_DEPTH, __ATOMIC_RELEASE);
    }
    return 0;
}

/* ---- 段布局 ---- */

/* 按段大小确定性推导分档布局；两进程用同一段名即得一致结果。 */
static void nb_shm_layout(nb_shm_header_t *h, uint64_t segment_bytes)
{
    const uint64_t header_bytes = nb_shm_align_up((uint64_t)sizeof(nb_shm_header_t), 64u);
    /*
     * 各档占用的段预算比例（千分比）。越大档越贵，块数会被腰斩，所以向大档
     * 倾斜：32MB 默认段下 1MB 档约 8 块、256KB 档约 25 块，小档仍够上千块。
     */
    static const uint32_t weight[NB_SHM_CLASS_COUNT] = {30, 60, 110, 150, 190, 210, 250};
    uint64_t remaining = segment_bytes > header_bytes ? segment_bytes - header_bytes : 0u;
    uint64_t cursor = header_bytes;
    unsigned i;

    for (i = 0; i < NB_SHM_CLASS_COUNT; i++) {
        nb_shm_class_hdr_t *cls = &h->classes[i];
        uint64_t space = segment_bytes > cursor ? segment_bytes - cursor : 0u;
        uint64_t stride;
        uint64_t count;

        cls->chunk_bytes = nb_shm_class_bytes[i];
        stride = nb_shm_align_up(NB_SHM_CHUNK_HDR_BYTES + cls->chunk_bytes, NB_SHM_CHUNK_ALIGN);
        cls->stride = stride;
        /*
         * 按预算分档，并夹在剩余空间内：段偏小时较大档可能拿到 0 块
         * （该尺寸发送返回 AGAIN），但小档仍可用，而不是直接打开失败。
         */
        count = remaining * weight[i] / 1000u / stride;
        if (count > space / stride) {
            count = space / stride;
        }
        cls->count = count;
        cls->arena_off = cursor;
        cls->free_head = 0;
        cursor += count * stride;
    }
    h->segment_bytes = segment_bytes;
    h->arena_off = header_bytes;
    h->arena_bytes = cursor - header_bytes;
}

static void nb_shm_class_build(nb_shmseg_t *seg, nb_shm_class_hdr_t *cls, uint64_t ci)
{
    uint64_t i;

    __atomic_store_n(&cls->free_head, 0, __ATOMIC_RELEASE);
    /* 逆序压栈，使 index 0 位于链头（仅影响命中顺序，不影响正确性）。 */
    for (i = cls->count; i > 0; i--) {
        nb_shm_chunk_hdr_t *ch = nb_shm_chunk_at(seg, cls, i - 1u);

        ch->offset = cls->arena_off + (i - 1u) * cls->stride;
        ch->magic = NB_SHM_CHUNK_MAGIC;
        ch->class_idx = (uint32_t)ci;
        ch->capacity = (uint32_t)cls->chunk_bytes;
        ch->length = 0;
        nb_shm_class_push(seg, cls, i - 1u);
    }
}

static void nb_shm_reset(nb_shmseg_t *seg)
{
    nb_shm_header_t *h = seg->hdr;
    unsigned i;

    for (i = 0; i < NB_SHM_MAX_NODES; i++) {
        nb_shm_queue_init(&h->inbox[i]);
        while (sem_trywait(&h->inbox_sem[i]) == 0) {
        }
    }
    for (i = 0; i < NB_SHM_CLASS_COUNT; i++) {
        nb_shm_class_build(seg, &h->classes[i], i);
    }
    /* 清掉上一世代的订阅（订阅方会随 service 注册重新写入）。 */
    memset(h->subs, 0, sizeof(h->subs));
}

static int nb_shm_init_shared(nb_shmseg_t *seg, uint64_t segment_bytes)
{
    nb_shm_header_t *h = seg->hdr;
    unsigned i;

    memset(h, 0, sizeof(*h));
    h->magic = NB_SHM_SLOT_MAGIC;
    h->layout = NB_SHM_LAYOUT_VERSION;
    h->owner_pid = (int64_t)getpid();
    nb_shm_layout(h, segment_bytes);
    for (i = 0; i < NB_SHM_MAX_NODES; i++) {
        if (sem_init(&h->inbox_sem[i], 1, 0) != 0) {
            while (i > 0) {
                sem_destroy(&h->inbox_sem[--i]);
            }
            return NB_ERR_IO;
        }
    }
    nb_shm_reset(seg);
    __atomic_store_n(&h->active_nodes, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&h->ready, 1u, __ATOMIC_RELEASE);
    return NB_OK;
}

/* ---- 打开/关闭 ---- */

static void nb_shm_make_name(const char *channel, char *out, size_t cap)
{
    static const char prefix[] = "/nb-shm-";
    size_t i = 0;
    size_t j = 0;

    while (prefix[i] && j + 1 < cap) {
        out[j++] = prefix[i++];
    }
    for (i = 0; channel && channel[i] && j + 1 < cap; i++) {
        char c = channel[i];

        out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.')
                       ? c
                       : '_';
    }
    if (cap) {
        out[j] = '\0';
    }
}

static uint64_t nb_shm_want_bytes(void)
{
    const char *env = getenv("NB_SHM_BYTES");
    long long v;

    if (!env || !env[0]) {
        return NB_SHM_DEFAULT_SEGMENT_BYTES;
    }
    v = atoll(env);
    if (v < (long long)NB_SHM_MIN_SEGMENT_BYTES) {
        v = NB_SHM_MIN_SEGMENT_BYTES;
    }
    return (uint64_t)v;
}

nb_shmseg_t *nb_shmseg_open(const char *channel, uint16_t self_id)
{
    nb_shmseg_t *seg;
    nb_shm_header_t *h;
    struct stat st;
    size_t map_size;
    int created = 0;
    int fd;
    int i;

    if (!channel || !channel[0] || self_id == 0 || self_id > NB_SHM_MAX_CLIENTS + 1u) {
        return NULL;
    }
    seg = (nb_shmseg_t *)calloc(1, sizeof(*seg));
    if (!seg) {
        return NULL;
    }
    seg->self_id = self_id;
    seg->creator = 0;
    seg->fd = -1;
    nb_copy_str(seg->channel, sizeof(seg->channel), channel, "");
    nb_shm_make_name(channel, seg->name, sizeof(seg->name));

    fd = shm_open(seg->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
        uint64_t want = nb_shm_want_bytes();

        created = 1;
        seg->creator = 1;
        if (ftruncate(fd, (off_t)want) != 0) {
            nb_log_error("shm: ftruncate(%s) failed: %s", seg->name, strerror(errno));
            close(fd);
            shm_unlink(seg->name);
            free(seg);
            return NULL;
        }
        map_size = (size_t)want;
    } else if (errno == EEXIST) {
        fd = shm_open(seg->name, O_RDWR, 0600);
        if (fd < 0) {
            nb_log_error("shm: open(%s) failed: %s", seg->name, strerror(errno));
            free(seg);
            return NULL;
        }
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(nb_shm_header_t)) {
            nb_log_error("shm: region %s is too small", seg->name);
            close(fd);
            free(seg);
            return NULL;
        }
        map_size = (size_t)st.st_size;
    } else {
        nb_log_error("shm: shm_open(%s) failed: %s", seg->name, strerror(errno));
        free(seg);
        return NULL;
    }

    seg->map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (seg->map == MAP_FAILED) {
        nb_log_error("shm: mmap(%s) failed: %s", seg->name, strerror(errno));
        close(fd);
        if (created) {
            shm_unlink(seg->name);
        }
        free(seg);
        return NULL;
    }
    seg->fd = fd;
    seg->map_size = map_size;
    h = (nb_shm_header_t *)seg->map;
    seg->hdr = h;

    if (created) {
        if (nb_shm_init_shared(seg, (uint64_t)map_size) != NB_OK) {
            nb_log_error("shm: init(%s) failed", seg->name);
            munmap(seg->map, map_size);
            close(fd);
            shm_unlink(seg->name);
            free(seg);
            return NULL;
        }
    } else {
        for (i = 0; i < 5000; i++) {
            if (__atomic_load_n(&h->ready, __ATOMIC_ACQUIRE)) {
                break;
            }
            usleep(1000);
        }
        if (!__atomic_load_n(&h->ready, __ATOMIC_ACQUIRE) || h->magic != NB_SHM_SLOT_MAGIC ||
            h->layout != NB_SHM_LAYOUT_VERSION || h->segment_bytes != (uint64_t)map_size) {
            nb_log_error("shm: region %s not ready or layout mismatch", seg->name);
            munmap(seg->map, map_size);
            close(fd);
            free(seg);
            return NULL;
        }
    }

    if (!created) {
        uint32_t active = __atomic_load_n(&h->active_nodes, __ATOMIC_ACQUIRE);
        if (active == 0u) {
            /* Previous owner exited without unlinking; start a clean generation. */
            nb_shm_reset(seg);
            __atomic_store_n(&h->active_nodes, 1u, __ATOMIC_RELEASE);
        } else {
            __atomic_add_fetch(&h->active_nodes, 1u, __ATOMIC_ACQ_REL);
        }
    }
    return seg;
}

void nb_shmseg_close(nb_shmseg_t *seg)
{
    uint32_t active = 0;

    if (!seg) {
        return;
    }
    if (seg->hdr && seg->name[0]) {
        if (seg->self_id >= 1u && seg->self_id <= NB_SHM_MAX_NODES) {
            nb_shm_sub_list_t *l = &seg->hdr->subs[seg->self_id - 1u];
            uint64_t v = __atomic_load_n(&l->version, __ATOMIC_RELAXED);
            __atomic_store_n(&l->version, v + 1u, __ATOMIC_RELEASE);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            memset(l->hashes, 0, sizeof(l->hashes));
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            __atomic_store_n(&l->version, v + 2u, __ATOMIC_RELEASE);
        }
        active = __atomic_sub_fetch(&seg->hdr->active_nodes, 1u, __ATOMIC_ACQ_REL);
    }
    if (seg->hdr) {
        munmap(seg->hdr, seg->map_size);
        seg->hdr = NULL;
    }
    if (seg->fd >= 0) {
        close(seg->fd);
        seg->fd = -1;
    }
    /* 最后一个退出者回收命名生命周期，mesh 模式下不再要求 id=1 存活。 */
    if (seg->name[0] && active == 0u) {
        shm_unlink(seg->name);
    }
    free(seg);
}

/* ---- 队列封装 ---- */

int nb_shmseg_push(nb_shmseg_t *seg, uint16_t target_id, uint64_t chunk_off)
{
    unsigned idx;
    int rc;

    if (!seg || !seg->hdr || chunk_off == 0 || target_id == 0 ||
        target_id > NB_SHM_MAX_NODES) {
        return -1;
    }
    idx = (unsigned)(target_id - 1u);
    rc = nb_shm_queue_push(&seg->hdr->inbox[idx], chunk_off, seg->self_id);
    if (rc == 1) {
        sem_post(&seg->hdr->inbox_sem[idx]);
    }
    return rc < 0 ? -1 : 0;
}

int nb_shmseg_pop(nb_shmseg_t *seg, uint64_t *chunk_off, uint16_t *source_id)
{
    unsigned idx;
    uint32_t source = 0;
    int rc;

    if (!seg || !seg->hdr || !chunk_off || seg->self_id == 0 ||
        seg->self_id > NB_SHM_MAX_NODES) {
        return -1;
    }
    idx = (unsigned)(seg->self_id - 1u);
    rc = nb_shm_queue_pop(&seg->hdr->inbox[idx], chunk_off, &source);
    if (rc == 0 && source_id) {
        *source_id = (uint16_t)source;
    }
    return rc;
}

int nb_shmseg_push_request(nb_shmseg_t *seg, uint64_t chunk_off)
{
    return nb_shmseg_push(seg, 1u, chunk_off);
}

int nb_shmseg_pop_request(nb_shmseg_t *seg, uint64_t *chunk_off, uint16_t *client_id)
{
    return nb_shmseg_pop(seg, chunk_off, client_id);
}

int nb_shmseg_push_response(nb_shmseg_t *seg, uint16_t client_id, uint64_t chunk_off)
{
    if (!seg || !seg->hdr || chunk_off == 0 || client_id < 2u ||
        client_id > NB_SHM_MAX_NODES) {
        return -1;
    }
    return nb_shmseg_push(seg, client_id, chunk_off);
}

int nb_shmseg_pop_response(nb_shmseg_t *seg, uint64_t *chunk_off, uint16_t *source_id)
{
    if (!seg || !seg->hdr || !chunk_off || seg->self_id < 2u) {
        return -1;
    }
    return nb_shmseg_pop(seg, chunk_off, source_id);
}

int nb_shmseg_wait(nb_shmseg_t *seg, int timeout_ms)
{
    sem_t *sem;
    struct timespec ts;
    long add_ms;
    int rc;

    if (!seg || !seg->hdr) {
        return -1;
    }
    if (seg->self_id == 0 || seg->self_id > NB_SHM_MAX_NODES) {
        return -1;
    }
    sem = &seg->hdr->inbox_sem[seg->self_id - 1u];
    add_ms = timeout_ms > 0 ? timeout_ms : 50;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    ts.tv_sec += add_ms / 1000;
    ts.tv_nsec += (add_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    do {
        rc = sem_timedwait(sem, &ts);
    } while (rc == -1 && errno == EINTR);
    return rc == 0 ? 0 : -1;
}

void nb_shmseg_wake(nb_shmseg_t *seg)
{
    if (!seg || !seg->hdr) {
        return;
    }
    if (seg->self_id != 0 && seg->self_id <= NB_SHM_MAX_NODES) {
        sem_post(&seg->hdr->inbox_sem[seg->self_id - 1u]);
    }
}

/* ---- chunk 生命周期 ---- */

uint64_t nb_shmseg_alloc(nb_shmseg_t *seg, size_t capacity)
{
    unsigned i;

    if (!seg || !seg->hdr || capacity == 0) {
        return 0;
    }
    for (i = 0; i < NB_SHM_CLASS_COUNT; i++) {
        nb_shm_class_hdr_t *cls = &seg->hdr->classes[i];
        uint64_t index = 0;
        nb_shm_chunk_hdr_t *ch;

        if (capacity > cls->chunk_bytes || nb_shm_class_pop(seg, cls, &index) != 0) {
            continue;
        }
        ch = nb_shm_chunk_at(seg, cls, index);
        ch->offset = cls->arena_off + index * cls->stride;
        ch->capacity = (uint32_t)cls->chunk_bytes;
        ch->length = 0;
        ch->next1 = 0;
        __atomic_store_n(&ch->refcount, 1u, __ATOMIC_RELAXED);
        return ch->offset;
    }
    return 0;
}

void nb_shmseg_chunk_free(nb_shmseg_t *seg, uint64_t offset)
{
    nb_shm_chunk_hdr_t *ch;
    nb_shm_class_hdr_t *cls;
    uint64_t index;

    if (!seg || !nb_shm_chunk_valid(seg, offset)) {
        return;
    }
    ch = (nb_shm_chunk_hdr_t *)((uint8_t *)seg->map + offset);
    cls = &seg->hdr->classes[ch->class_idx];
    index = (offset - cls->arena_off) / cls->stride;
    if (index >= cls->count) {
        return;
    }
    /* 每个消费者各减一次；只有最后一个引用才真正归还池。 */
    if (__atomic_fetch_sub(&ch->refcount, 1u, __ATOMIC_ACQ_REL) != 1u) {
        return;
    }
    ch->length = 0;
    nb_shm_class_push(seg, cls, index);
}

uint8_t *nb_shmseg_data(nb_shmseg_t *seg, uint64_t offset)
{
    if (!seg || !nb_shm_chunk_valid(seg, offset)) {
        return NULL;
    }
    return (uint8_t *)seg->map + offset + NB_SHM_CHUNK_HDR_BYTES;
}

uint32_t nb_shmseg_capacity(nb_shmseg_t *seg, uint64_t offset)
{
    if (!seg || !nb_shm_chunk_valid(seg, offset)) {
        return 0;
    }
    return ((nb_shm_chunk_hdr_t *)((uint8_t *)seg->map + offset))->capacity;
}

void nb_shmseg_set_length(nb_shmseg_t *seg, uint64_t offset, uint32_t length)
{
    if (!seg || !nb_shm_chunk_valid(seg, offset)) {
        return;
    }
    ((nb_shm_chunk_hdr_t *)((uint8_t *)seg->map + offset))->length = length;
}

uint32_t nb_shmseg_length(nb_shmseg_t *seg, uint64_t offset)
{
    if (!seg || !nb_shm_chunk_valid(seg, offset)) {
        return 0;
    }
    return ((nb_shm_chunk_hdr_t *)((uint8_t *)seg->map + offset))->length;
}

uint64_t nb_shmseg_locate(nb_shmseg_t *seg, const void *data)
{
    uintptr_t base;
    uintptr_t p;
    uint64_t off;
    const nb_shm_header_t *h;
    const nb_shm_chunk_hdr_t *ch;

    if (!seg || !seg->hdr || !data) {
        return 0;
    }
    h = seg->hdr;
    base = (uintptr_t)seg->map;
    p = (uintptr_t)data;
    if (p < base + h->arena_off + NB_SHM_CHUNK_HDR_BYTES ||
        p >= base + h->arena_off + h->arena_bytes || (p & (NB_SHM_CHUNK_ALIGN - 1u)) != 0) {
        return 0;
    }
    off = (uint64_t)(p - base);
    ch = (const nb_shm_chunk_hdr_t *)(p - NB_SHM_CHUNK_HDR_BYTES);
    if (ch->magic != NB_SHM_CHUNK_MAGIC || ch->offset != off - NB_SHM_CHUNK_HDR_BYTES ||
        ch->class_idx >= NB_SHM_CLASS_COUNT) {
        return 0;
    }
    {
        const nb_shm_class_hdr_t *cls = &h->classes[ch->class_idx];

        if ((ch->offset - cls->arena_off) % cls->stride != 0 ||
            (ch->offset - cls->arena_off) / cls->stride >= cls->count) {
            return 0;
        }
    }
    return ch->offset;
}

/* ---- 发布（零拷贝扇出） ---- */

int nb_shmseg_publish(nb_shmseg_t *seg, const char *topic, uint64_t chunk_off)
{
    nb_shm_chunk_hdr_t *ch;
    uint64_t hash;
    uint16_t targets[NB_SHM_MAX_NODES];
    unsigned n = 0;
    unsigned i;

    if (!seg || !seg->hdr || !topic || !topic[0] || !nb_shm_chunk_valid(seg, chunk_off)) {
        if (seg && chunk_off != 0) {
            nb_shmseg_chunk_free(seg, chunk_off);
        }
        return -1;
    }
    hash = nb_shm_topic_hash(topic);
    for (i = 0; i < NB_SHM_MAX_NODES; i++) {
        uint16_t id = (uint16_t)(i + 1u);

        if (id == seg->self_id) {
            continue; /* 不回显给发布者自身 */
        }
        if (nb_shm_sub_has(seg->hdr, i, hash)) {
            targets[n++] = id;
        }
    }
    if (n == 0) {
        nb_shmseg_chunk_free(seg, chunk_off);
        return 0;
    }
    ch = (nb_shm_chunk_hdr_t *)((uint8_t *)seg->map + chunk_off);
    __atomic_store_n(&ch->refcount, (uint32_t)n, __ATOMIC_RELEASE);
    for (i = 0; i < n; i++) {
        int rc = nb_shmseg_push(seg, targets[i], chunk_off);

        if (rc != 0) {
            nb_shmseg_chunk_free(seg, chunk_off); /* 该目标未投递，回收其份额 */
        }
    }
    return 0;
}

/* ---- external nb_buf 桥接 ---- */

/* nb_buf_create_external 的释放回调：数据指针回推 chunk 偏移并归还。 */
static void nb_shm_buf_release(void *opaque, uint8_t *data)
{
    nb_shmseg_t *seg = (nb_shmseg_t *)opaque;
    uint64_t off;

    if (!seg || !data) {
        return;
    }
    off = nb_shmseg_locate(seg, data);
    if (off == 0) {
        nb_log_error("shm: released buffer is not a chunk of this segment");
        return;
    }
    nb_shmseg_chunk_free(seg, off);
}

nb_buf_t *nb_shmseg_wrap_rx(nb_shmseg_t *seg, uint64_t offset)
{
    uint32_t len;
    uint8_t *data;

    if (!seg) {
        return NULL;
    }
    len = nb_shmseg_length(seg, offset);
    data = nb_shmseg_data(seg, offset);
    if (!data) {
        return NULL;
    }
    /* size 至少为 1，空消息也需要一个所有权载体来保证 chunk 归还。 */
    return nb_buf_create_external(data, len ? len : 1u, nb_shm_buf_release, seg, 0);
}

#else /* !__linux__ */

/* 非 Linux 平台没有 POSIX 共享内存，段实现不参与编译。 */
typedef int nb_shm_segment_placeholder_t;

#endif
