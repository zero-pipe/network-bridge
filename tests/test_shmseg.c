/*
 * iceoryx 风格共享内存段的单元测试（Linux only）。
 *
 * 同一进程里开两个句柄映射同一段：一个当 server（id 1），一个当 client（id 2），
 * 直接验证分档分配、偏移寻址、零拷贝 wrap_rx 生命周期，以及请求/响应队列的
 * 客户端归属与容量上限。
 */
#include "shm_segment.h"

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                    \
        }                                                                  \
    } while (0)

static void test_alloc_and_locate(nb_shmseg_t *srv)
{
    uint64_t off = nb_shmseg_alloc(srv, 100);
    uint8_t *data;

    CHECK(off != 0);
    if (off == 0) {
        return;
    }
    data = nb_shmseg_data(srv, off);
    CHECK(data != NULL);
    CHECK(nb_shmseg_capacity(srv, off) == nb_shm_class_bytes[0]);
    CHECK(nb_shmseg_locate(srv, data) == off);
    CHECK(nb_shmseg_locate(srv, data + 1) == 0); /* 非块首不认 */
    nb_shmseg_set_length(srv, off, 42);
    CHECK(nb_shmseg_length(srv, off) == 42);
    nb_shmseg_chunk_free(srv, off);

    /* 空闲链表按 LIFO 复用：刚归还的块应能被再次分配出来。 */
    {
        uint64_t again = nb_shmseg_alloc(srv, 100);

        CHECK(again == off);
        if (again) {
            nb_shmseg_chunk_free(srv, again);
        }
    }
}

static void test_size_classes(nb_shmseg_t *srv)
{
    unsigned i;

    for (i = 0; i < NB_SHM_CLASS_COUNT; i++) {
        uint64_t off = nb_shmseg_alloc(srv, nb_shm_class_bytes[i]);
        uint32_t cap;

        if (off == 0) {
            continue; /* 段偏小时较大档可能为 0 块 */
        }
        cap = nb_shmseg_capacity(srv, off);
        CHECK(cap == nb_shm_class_bytes[i]);
        CHECK(cap >= nb_shm_class_bytes[i]);
        nb_shmseg_chunk_free(srv, off);
    }
    /* 超出最大档：必须失败。 */
    CHECK(nb_shmseg_alloc(srv, nb_shm_class_bytes[NB_SHM_CLASS_COUNT - 1] + 1u) == 0);
}

static void test_wrap_rx_lifetime(nb_shmseg_t *srv)
{
    uint64_t off = nb_shmseg_alloc(srv, 64);
    nb_buf_t *buf;
    uint8_t *data;

    CHECK(off != 0);
    if (off == 0) {
        return;
    }
    data = nb_shmseg_data(srv, off);
    memcpy(data, "payload", 8);
    nb_shmseg_set_length(srv, off, 8);
    buf = nb_shmseg_wrap_rx(srv, off);
    CHECK(buf != NULL);
    CHECK(nb_buf_size(buf) == 8);
    CHECK(nb_buf_data(buf) == data);
    nb_buf_release(buf); /* 释放即归还 chunk */
    CHECK(nb_shmseg_alloc(srv, 64) == off); /* 归还后同一块可复用 */
    nb_shmseg_chunk_free(srv, off);
}

static void test_queues(nb_shmseg_t *srv, nb_shmseg_t *cli)
{
    uint64_t off = nb_shmseg_alloc(cli, 100);
    uint64_t got = 0;
    uint16_t client = 0;

    CHECK(off != 0);
    if (off == 0) {
        return;
    }
    CHECK(nb_shmseg_push_request(cli, off) == 0);
    CHECK(nb_shmseg_pop_request(srv, &got, &client) == 0);
    CHECK(got == off);
    CHECK(client == 2); /* 请求应记录发起方 client id */

    /* server 用请求带来的 client id 回投响应。 */
    nb_shmseg_set_length(srv, off, 3);
    CHECK(nb_shmseg_push_response(srv, 2, off) == 0);
    got = 0;
    CHECK(nb_shmseg_pop_response(cli, &got, &client) == 0);
    CHECK(got == off);
    CHECK(client == 1); /* 响应的来源节点 = server id */
    nb_shmseg_chunk_free(cli, off);

    /* 空队列返回 -1。 */
    CHECK(nb_shmseg_pop_response(cli, &got, NULL) == -1);
    CHECK(nb_shmseg_pop_request(srv, &got, &client) == -1);
    /* 非法 client id。 */
    CHECK(nb_shmseg_push_response(srv, 0, off) == -1);
    CHECK(nb_shmseg_push_response(srv, NB_SHM_MAX_CLIENTS + 2u, off) == -1);
}

static void test_pubsub(nb_shmseg_t *srv, nb_shmseg_t *cli, nb_shmseg_t *cli2)
{
    const char *topic = "TopicService.chat";
    uint64_t off;
    uint64_t got1 = 0;
    uint64_t got2 = 0;
    uint16_t src = 0;

    CHECK(nb_shmseg_subscribe(cli, topic) == 0);
    CHECK(nb_shmseg_subscribe(cli2, topic) == 0);

    off = nb_shmseg_alloc(srv, 64);
    CHECK(off != 0);
    if (off == 0) {
        return;
    }
    memcpy(nb_shmseg_data(srv, off), "fanout", 7);
    nb_shmseg_set_length(srv, off, 7);
    CHECK(nb_shmseg_publish(srv, topic, off) == 0);

    /* 两个订阅者收到同一块（零拷贝），来源是发布者 id。 */
    CHECK(nb_shmseg_pop_response(cli, &got1, &src) == 0);
    CHECK(got1 == off && src == 1);
    CHECK(nb_shmseg_pop_response(cli2, &got2, &src) == 0);
    CHECK(got2 == off && src == 1);
    CHECK(memcmp(nb_shmseg_data(cli, got1), "fanout", 7) == 0);

    /* 第一个消费者释放后引用计数仍 >0，块不回收；第二个释放后才可复用。 */
    nb_shmseg_chunk_free(cli, got1);
    nb_shmseg_chunk_free(cli2, got2);
    CHECK(nb_shmseg_alloc(srv, 64) == off);
    nb_shmseg_chunk_free(srv, off);

    /* 取消订阅后不再扇出：publish 成功但直接回收。 */
    CHECK(nb_shmseg_unsubscribe(cli, topic) == 0);
    CHECK(nb_shmseg_unsubscribe(cli2, topic) == 0);
    off = nb_shmseg_alloc(srv, 64);
    CHECK(off != 0);
    if (off != 0) {
        CHECK(nb_shmseg_publish(srv, topic, off) == 0);
        CHECK(nb_shmseg_pop_response(cli, &got1, &src) == -1);
        CHECK(nb_shmseg_pop_response(cli2, &got2, &src) == -1);
        CHECK(nb_shmseg_alloc(srv, 64) == off); /* 无订阅者时已归还，可立即复用 */
        nb_shmseg_chunk_free(srv, off);
    }
}

static void test_queue_capacity(nb_shmseg_t *srv, nb_shmseg_t *cli)
{
    uint64_t offs[NB_SHM_QUEUE_DEPTH];
    unsigned i;
    unsigned pushed = 0;

    for (i = 0; i < NB_SHM_QUEUE_DEPTH; i++) {
        offs[i] = nb_shmseg_alloc(cli, 64);
        if (offs[i] == 0) {
            break;
        }
        CHECK(nb_shmseg_push_request(cli, offs[i]) == 0);
        pushed++;
    }
    CHECK(pushed > 0);
    /* 队满：再多一条必须失败。 */
    {
        uint64_t extra = nb_shmseg_alloc(cli, 64);

        if (extra != 0) {
            CHECK(nb_shmseg_push_request(cli, extra) == -1);
            nb_shmseg_chunk_free(cli, extra);
        }
    }
    for (i = 0; i < pushed; i++) {
        uint64_t got = 0;

        CHECK(nb_shmseg_pop_request(srv, &got, NULL) == 0);
        CHECK(got == offs[i]); /* FIFO */
        nb_shmseg_chunk_free(srv, got);
    }
}

static void test_mesh_peer_queues(nb_shmseg_t *cli, nb_shmseg_t *cli2)
{
    uint64_t off = nb_shmseg_alloc(cli, 64);
    uint64_t got = 0;
    uint16_t source = 0;

    CHECK(off != 0);
    if (off == 0) {
        return;
    }
    /* node 2 -> node 3：不经过固定 server 队列。 */
    CHECK(nb_shmseg_push(cli, 3, off) == 0);
    CHECK(nb_shmseg_pop(cli2, &got, &source) == 0);
    CHECK(got == off);
    CHECK(source == 2);

    /* node 3 -> node 2：任意节点都可以作为响应方。 */
    nb_shmseg_set_length(cli2, off, 4);
    CHECK(nb_shmseg_push(cli2, 2, off) == 0);
    got = 0;
    CHECK(nb_shmseg_pop(cli, &got, &source) == 0);
    CHECK(got == off);
    CHECK(source == 3);
    nb_shmseg_chunk_free(cli, off);
}

int main(void)
{
    char channel[64];
    nb_shmseg_t *srv;
    nb_shmseg_t *cli;
    nb_shmseg_t *cli2;

    snprintf(channel, sizeof(channel), "shmseg-test-%ld", (long)getpid());
    srv = nb_shmseg_open(channel, 1);
    if (!srv) {
        fprintf(stderr, "open server segment failed\n");
        return 1;
    }
    cli = nb_shmseg_open(channel, 2);
    if (!cli) {
        fprintf(stderr, "open client segment failed\n");
        nb_shmseg_close(srv);
        return 1;
    }
    cli2 = nb_shmseg_open(channel, 3);
    if (!cli2) {
        fprintf(stderr, "open second client segment failed\n");
        nb_shmseg_close(cli);
        nb_shmseg_close(srv);
        return 1;
    }

    test_alloc_and_locate(srv);
    test_size_classes(srv);
    test_wrap_rx_lifetime(srv);
    test_queues(srv, cli);
    test_pubsub(srv, cli, cli2);
    test_queue_capacity(srv, cli);
    test_mesh_peer_queues(cli, cli2);

    nb_shmseg_close(cli2);
    nb_shmseg_close(cli);
    nb_shmseg_close(srv);

    if (g_fail) {
        fprintf(stderr, "SHMSEG FAIL\n");
        return 1;
    }
    printf("SHMSEG OK\n");
    return 0;
}
