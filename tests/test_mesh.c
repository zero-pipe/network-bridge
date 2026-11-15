/*
 * nb_mesh 单元测试：报文编解码、编队隔离、自包忽略、JOIN/HEARTBEAT/LEAVE、
 * 租约过期、表满淘汰、掩码寻址命中/未命中、下线不可调用，以及 1000~5000
 * 邻居的容量/查找时延冒烟。
 *
 * 全部走 loopback（不依赖环境里是否真的有可用组播）：
 *   - 发现相关用例用两个真实 nb_mesh 实例 + 定向单播目标；
 *   - 表操作与规模用例直接驱动 nb_mesh_table（无 socket），因此不必真发 5000 包。
 */
#include <nb/mesh.h>

#include "nb_mesh_table.h"

#include <zn/zn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        unsigned long long a_ = (unsigned long long)(a);                            \
        unsigned long long b_ = (unsigned long long)(b);                            \
        if (a_ != b_) {                                                             \
            printf("FAIL %s:%d: %s=%llu != %s=%llu\n",                              \
                   __FILE__,                                                       \
                   __LINE__,                                                       \
                   #a,                                                             \
                   a_,                                                             \
                   #b,                                                             \
                   b_);                                                            \
            g_fail = 1;                                                             \
        }                                                                           \
    } while (0)

static void spin(unsigned ms)
{
    zn_sleep_ms(ms);
}

/* 等条件成立，最多 timeout_ms（每 10 ms 试一次）。 */
#define WAIT_UNTIL(expr, timeout_ms)                                              \
    do {                                                                          \
        unsigned waited_ = 0;                                                     \
        while (!(expr) && waited_ < (unsigned)(timeout_ms)) {                      \
            spin(10);                                                             \
            waited_ += 10;                                                        \
        }                                                                         \
    } while (0)

/* ---- 报文编解码 ------------------------------------------------------ */

static void fill_packet(nb_mesh_packet_t *pkt, uint8_t type)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->msg_type = type;
    pkt->formation_id = 0x11223344u;
    pkt->node_id = 0x0102030405060708ull;
    pkt->real_ip = 0xC0A80102u; /* 192.168.1.2 */
    pkt->rpc_port = 0x1F90u;    /* 8080 */
    pkt->service_mask = 0xA5A5F00Fu;
}

static void test_wire_codec(void)
{
    nb_mesh_packet_t in;
    nb_mesh_packet_t out;
    uint8_t buf[NB_MESH_WIRE_SIZE];
    size_t i;

    printf("  wire codec\n");
    /* 线长必须是 26 字节：定长报文，任何长度变化都是协议破坏。 */
    CHECK_EQ(NB_MESH_WIRE_SIZE, 26u);

    fill_packet(&in, NB_MESH_MSG_JOIN);
    CHECK(nb_mesh_encode(&in, buf) == NB_MESH_OK);

    /* 逐字节核对大端布局（含 magic 与 node_id）。 */
    CHECK_EQ(buf[0], 0x5Au);
    CHECK_EQ(buf[1], 0x43u);
    CHECK_EQ(buf[2], NB_MESH_MSG_JOIN);
    CHECK_EQ(buf[3], 0x00u); /* reserved 恒 0 */
    CHECK_EQ(buf[4], 0x11u);
    CHECK_EQ(buf[5], 0x22u);
    CHECK_EQ(buf[6], 0x33u);
    CHECK_EQ(buf[7], 0x44u);
    for (i = 0; i < 8; i++) {
        CHECK_EQ(buf[8 + i], (uint8_t)(i + 1)); /* node_id 大端：01..08 */
    }
    CHECK_EQ(buf[16], 0xC0u);
    CHECK_EQ(buf[17], 0xA8u);
    CHECK_EQ(buf[18], 0x01u);
    CHECK_EQ(buf[19], 0x02u);
    CHECK_EQ(buf[20], 0x1Fu);
    CHECK_EQ(buf[21], 0x90u);
    CHECK_EQ(buf[22], 0xA5u);
    CHECK_EQ(buf[23], 0xA5u);
    CHECK_EQ(buf[24], 0xF0u);
    CHECK_EQ(buf[25], 0x0Fu);

    memset(&out, 0, sizeof(out));
    CHECK(nb_mesh_decode(buf, sizeof(buf), &out) == NB_MESH_OK);
    CHECK_EQ(out.msg_type, in.msg_type);
    CHECK_EQ(out.formation_id, in.formation_id);
    CHECK_EQ(out.node_id, in.node_id);
    CHECK_EQ(out.real_ip, in.real_ip);
    CHECK_EQ(out.rpc_port, in.rpc_port);
    CHECK_EQ(out.service_mask, in.service_mask);

    /* 三种类型都要能往返。 */
    fill_packet(&in, NB_MESH_MSG_HEARTBEAT);
    CHECK(nb_mesh_encode(&in, buf) == NB_MESH_OK);
    CHECK(nb_mesh_decode(buf, sizeof(buf), &out) == NB_MESH_OK);
    CHECK_EQ(out.msg_type, NB_MESH_MSG_HEARTBEAT);
    fill_packet(&in, NB_MESH_MSG_LEAVE);
    CHECK(nb_mesh_encode(&in, buf) == NB_MESH_OK);
    CHECK(nb_mesh_decode(buf, sizeof(buf), &out) == NB_MESH_OK);
    CHECK_EQ(out.msg_type, NB_MESH_MSG_LEAVE);

    /* 非法输入必须被拒绝，不能"尽力解析"。 */
    fill_packet(&in, 9);
    CHECK(nb_mesh_encode(&in, buf) == NB_MESH_ERR_INVALID);
    fill_packet(&in, NB_MESH_MSG_JOIN);
    CHECK(nb_mesh_encode(&in, buf) == NB_MESH_OK);
    CHECK(nb_mesh_decode(buf, sizeof(buf) - 1, &out) == NB_MESH_ERR_INVALID);
    CHECK(nb_mesh_decode(buf, sizeof(buf) + 1, &out) == NB_MESH_ERR_INVALID);
    buf[0] ^= 0xFFu; /* magic 被破坏 */
    CHECK(nb_mesh_decode(buf, sizeof(buf), &out) == NB_MESH_ERR_INVALID);
    fill_packet(&in, NB_MESH_MSG_JOIN);
    CHECK(nb_mesh_encode(&in, buf) == NB_MESH_OK);
    buf[2] = 7; /* 未知 msg_type */
    CHECK(nb_mesh_decode(buf, sizeof(buf), &out) == NB_MESH_ERR_INVALID);
}

/* 服务名哈希位：同名字同位、非零、且只在 32 位内。 */
static void test_service_bit(void)
{
    uint32_t a = nb_mesh_service_bit("echo");
    uint32_t b = nb_mesh_service_bit("echo");
    uint32_t c = nb_mesh_service_bit("telemetry");

    printf("  service bit\n");
    CHECK_EQ(a, b);
    CHECK(a != 0);
    CHECK_EQ(a & (a - 1u), 0u); /* 单位掩码 */
    CHECK(a != c);
    /* 编译期契约位图：每个已分配的服务位都是单位掩码。 */
    CHECK_EQ(NB_SERVICE_CORE_FCS, 1u << 0);
    CHECK_EQ(NB_SERVICE_CORE_HEALTH, 1u << 1);
    CHECK_EQ(NB_SERVICE_SENSE_SLAM, 1u << 8);
    CHECK_EQ(NB_SERVICE_SENSE_VISION, 1u << 9);
    CHECK_EQ(NB_SERVICE_ACT_GIMBAL, 1u << 16);
    CHECK_EQ(NB_SERVICE_RELAY_NODE, 1u << 24);
}

/* 准入判断：编队隔离 + 忽略自身。 */
static void test_packet_accept(void)
{
    nb_mesh_packet_t pkt;

    printf("  packet accept\n");
    fill_packet(&pkt, NB_MESH_MSG_HEARTBEAT);
    CHECK(nb_mesh_packet_accept(&pkt, pkt.formation_id, 1) == 1);
    /* 编队不同：丢弃。 */
    CHECK(nb_mesh_packet_accept(&pkt, pkt.formation_id + 1u, 1) == 0);
    /* 自己的报文：丢弃（组播回环也要忽略）。 */
    CHECK(nb_mesh_packet_accept(&pkt, pkt.formation_id, pkt.node_id) == 0);
    /* node_id = 0 的畸形报文：丢弃。 */
    pkt.node_id = 0;
    CHECK(nb_mesh_packet_accept(&pkt, pkt.formation_id, 1) == 0);
}

/* ---- 邻居表（无 socket） --------------------------------------------- */

static void test_table_basic(void)
{
    nb_mesh_table_t table;
    nb_mesh_peer_info_t peers[8];
    nb_mesh_node_t *node;
    uint64_t now = 1000;
    uint64_t evicted = 0;
    uint64_t removed;
    size_t n;

    printf("  table basics\n");
    CHECK(nb_mesh_table_init(&table, 16) == NB_MESH_OK);
    CHECK_EQ(nb_mesh_table_count(&table), 0u);

    /* 插入 vs 更新。 */
    CHECK_EQ(nb_mesh_table_upsert(&table, 0xAA, 0x0100007Fu, 5001, 0x1u, now, &evicted), 1);
    CHECK_EQ(evicted, 0u);
    CHECK_EQ(nb_mesh_table_upsert(&table, 0xAA, 0x0100007Fu, 5002, 0x1u, now + 10, &evicted), 0);
    CHECK_EQ(nb_mesh_table_count(&table), 1u);
    node = nb_mesh_table_get(&table, 0xAA);
    CHECK(node != NULL);
    if (node) {
        CHECK_EQ(node->rpc_port, 5002u);
        CHECK_EQ(node->last_seen_ms, now + 10u);
    }
    CHECK(nb_mesh_table_get(&table, 0xBB) == NULL);

    /* 掩码寻址：命中 / 未命中。 */
    node = nb_mesh_table_find_service(&table, NB_SERVICE_CORE_FCS);
    CHECK(node != NULL);
    if (node) {
        CHECK_EQ(node->node_id, 0xAAu);
    }
    CHECK(nb_mesh_table_find_service(&table, NB_SERVICE_SENSE_SLAM) == NULL);

    /* 掩码变化：旧位必须失效、新位必须生效（先摘再挂）。 */
    CHECK_EQ(nb_mesh_table_upsert(&table, 0xAA, 0x0100007Fu, 5002,
                                  NB_SERVICE_SENSE_SLAM, now + 20, &evicted), 0);
    CHECK(nb_mesh_table_find_service(&table, NB_SERVICE_CORE_FCS) == NULL);
    node = nb_mesh_table_find_service(&table, NB_SERVICE_SENSE_SLAM);
    CHECK(node != NULL);
    if (node) {
        CHECK_EQ(node->node_id, 0xAAu);
    }

    /* 多节点同服务：轮转游标必须把结果分摊开。 */
    CHECK_EQ(nb_mesh_table_upsert(&table, 0xBB, 0x0100007Fu, 5003,
                                  NB_SERVICE_SENSE_SLAM, now + 30, &evicted), 1);
    {
        uint64_t first = nb_mesh_table_find_service(&table, NB_SERVICE_SENSE_SLAM)->node_id;
        uint64_t second = nb_mesh_table_find_service(&table, NB_SERVICE_SENSE_SLAM)->node_id;

        CHECK(first != second);
    }

    /* 删除。 */
    CHECK_EQ(nb_mesh_table_remove(&table, 0xAA), 1);
    CHECK_EQ(nb_mesh_table_remove(&table, 0xAA), 0);
    CHECK_EQ(nb_mesh_table_count(&table), 1u);
    CHECK(nb_mesh_table_find_service(&table, NB_SERVICE_SENSE_SLAM) != NULL);

    /* 租约过期：超过 lease 的剔除，剩下的保留。 */
    CHECK_EQ(nb_mesh_table_sweep(&table, now + 40, 100), 0u);
    CHECK_EQ(nb_mesh_table_sweep(&table, now + 31 + 101, 100), 1u);
    CHECK_EQ(nb_mesh_table_count(&table), 0u);

    /* 快照。 */
    (void)nb_mesh_table_upsert(&table, 0xCC, 0x7F000001u, 6000, 0x0u, now, NULL);
    n = nb_mesh_table_snapshot(&table, peers, 8);
    CHECK_EQ(n, 1u);
    if (n == 1) {
        CHECK_EQ(peers[0].node_id, 0xCCu);
        CHECK_EQ(peers[0].ipv4, 0x7F000001u);
        CHECK_EQ(nb_mesh_table_snapshot(&table, peers, 0), 0u);
    }
    removed = (uint64_t)nb_mesh_table_count(&table);
    CHECK_EQ(removed, 1u);

    nb_mesh_table_fini(&table);
    CHECK_EQ(nb_mesh_table_count(&table), 0u);
}

/* 表满按"最久未见"淘汰：新节点必须进来，最老的那个被挤掉并报出 id。 */
static void test_table_eviction(void)
{
    nb_mesh_table_t table;
    uint64_t evicted = 0;
    uint64_t now = 1000;
    uint64_t i;

    printf("  table eviction\n");
    CHECK(nb_mesh_table_init(&table, 4) == NB_MESH_OK);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(nb_mesh_table_upsert(&table, i + 1, 0x7F000001u, (uint16_t)(7000 + i),
                                      NB_SERVICE_CORE_HEALTH, now + i, &evicted), 1);
        CHECK_EQ(evicted, 0u); /* 前 4 个不该触发淘汰 */
    }
    CHECK_EQ(nb_mesh_table_count(&table), 4u);

    /* 第 5 个：淘汰"最久未见"的 node 1（last_seen = now）。 */
    CHECK_EQ(nb_mesh_table_upsert(&table, 5, 0x7F000001u, 8000, NB_SERVICE_CORE_HEALTH,
                                  now + 100, &evicted), 1);
    CHECK_EQ(evicted, 1u);
    CHECK_EQ(nb_mesh_table_count(&table), 4u);
    CHECK(nb_mesh_table_get(&table, 1) == NULL);
    CHECK(nb_mesh_table_get(&table, 5) != NULL);
    CHECK_EQ(nb_mesh_table_upsert(&table, 2, 0x7F000001u, 7001, NB_SERVICE_CORE_HEALTH,
                                  now + 101, &evicted), 0);
    CHECK_EQ(nb_mesh_table_count(&table), 4u);

    nb_mesh_table_fini(&table);
}

/*
 * 容量/查找时延冒烟：1000~5000 个邻居（只测表操作，不真发 5000 包）。
 * 断言的是"能装下、查得到、查找是常数级"，不是绝对时延（CI 机器差异大）。
 */
static void test_table_capacity(size_t n)
{
    nb_mesh_table_t table;
    uint64_t start_ms;
    uint64_t elapsed_ms;
    size_t rounds = 20000;
    size_t i;
    int hits = 0;

    printf("  table capacity %lu\n", (unsigned long)n);
    CHECK(nb_mesh_table_init(&table, n) == NB_MESH_OK);
    start_ms = zb_monotonic_ms();
    for (i = 0; i < n; i++) {
        /* 每 3 个节点一个服务位，剩下的挂在 CORE_HEALTH 上。 */
        uint32_t mask = (i % 3u == 0) ? NB_SERVICE_SENSE_SLAM : NB_SERVICE_CORE_HEALTH;

        CHECK_EQ(nb_mesh_table_upsert(&table,
                                      (uint64_t)(i + 1) * 0x9E3779B1ull + 1u,
                                      0x0A000001u + (uint32_t)i,
                                      (uint16_t)(10000u + (i % 50000u)),
                                      mask,
                                      1000,
                                      NULL),
                 1);
    }
    CHECK_EQ(nb_mesh_table_count(&table), n);

    for (i = 0; i < rounds; i++) {
        if (nb_mesh_table_find_service(&table, NB_SERVICE_CORE_HEALTH) != NULL) {
            hits++;
        }
    }
    elapsed_ms = zb_monotonic_ms() - start_ms;
    CHECK_EQ(hits, rounds);
    printf("    %lu peers: %lu finds in %llu ms\n",
           (unsigned long)n,
           (unsigned long)rounds,
           (unsigned long long)elapsed_ms);
    /* 查找必须是 O(1)：5000 邻居下 2 万次查找不该要一秒。 */
    CHECK(elapsed_ms < 1000u);

    /* 全部过期：一次 sweep 清空（模拟 5000 节点同时离线）。 */
    CHECK_EQ(nb_mesh_table_sweep(&table, 1000 + 5000, 1500), n);
    CHECK_EQ(nb_mesh_table_count(&table), 0u);

    nb_mesh_table_fini(&table);
}

/* ---- 真实实例（loopback 单播） --------------------------------------- */

typedef struct mesh_ctx {
    nb_mesh_t *mesh;
    uint16_t rpc_port;
} mesh_ctx_t;

static int mesh_up(mesh_ctx_t *ctx,
                   uint32_t formation,
                   uint32_t service_mask,
                   uint16_t rpc_port,
                   uint32_t lease_ms)
{
    nb_mesh_config_t cfg;

    memset(ctx, 0, sizeof(*ctx));
    nb_mesh_config_init(&cfg);
    cfg.formation_id = formation;
    cfg.node_id = 0; /* 随机 */
    cfg.rpc_port = rpc_port;
    cfg.service_mask = service_mask;
    cfg.multicast_group = ""; /* 只用定向单播目标：不依赖环境里的组播 */
    cfg.bind_host = "127.0.0.1";
    cfg.port = 0; /* 临时端口，避免并行测试冲突 */
    cfg.heartbeat_ms = 100;
    cfg.lease_ms = lease_ms;
    ctx->rpc_port = rpc_port;
    if (nb_mesh_start(&ctx->mesh, &cfg) != NB_MESH_OK) {
        printf("mesh start failed\n");
        return -1;
    }
    return 0;
}

static void mesh_down(mesh_ctx_t *ctx)
{
    if (ctx->mesh) {
        nb_mesh_stop(ctx->mesh);
        ctx->mesh = NULL;
    }
}

/* 双向互指：把对方的发现端口加为定向目标。 */
static void mesh_link(mesh_ctx_t *a, mesh_ctx_t *b)
{
    CHECK(nb_mesh_add_peer(a->mesh, "127.0.0.1", nb_mesh_port(b->mesh)) == NB_MESH_OK);
    CHECK(nb_mesh_add_peer(b->mesh, "127.0.0.1", nb_mesh_port(a->mesh)) == NB_MESH_OK);
}

/* 同编队互见 + 编队隔离 + 自包忽略 + 掩码寻址。 */
static void test_discovery_join(void)
{
    mesh_ctx_t a;
    mesh_ctx_t b;
    mesh_ctx_t other;
    nb_mesh_target_t target;
    nb_mesh_peer_info_t peers[8];
    size_t n;

    printf("  discovery join/isolation\n");
    CHECK(mesh_up(&a, 7, NB_SERVICE_CORE_FCS, 44001, 3000) == 0);
    CHECK(mesh_up(&b, 7, NB_SERVICE_SENSE_SLAM, 44002, 3000) == 0);
    /* 不同编队：即便报文能到达，也必须被丢弃。 */
    CHECK(mesh_up(&other, 8, NB_SERVICE_CORE_FCS, 44003, 3000) == 0);
    if (!a.mesh || !b.mesh || !other.mesh) {
        mesh_down(&a);
        mesh_down(&b);
        mesh_down(&other);
        return;
    }
    mesh_link(&a, &b);
    /* 同机多实例必须落在不同端口上，否则报文会全进同一个 socket。 */
    CHECK(nb_mesh_port(a.mesh) != nb_mesh_port(b.mesh));
    CHECK(nb_mesh_port(a.mesh) != nb_mesh_port(other.mesh));
    /* 编队 8 的节点也指向 A：A 不该因为它的 JOIN 认识编队 8 的节点。 */
    CHECK(nb_mesh_add_peer(other.mesh, "127.0.0.1", nb_mesh_port(a.mesh)) == NB_MESH_OK);
    CHECK(nb_mesh_add_peer(a.mesh, "127.0.0.1", nb_mesh_port(other.mesh)) == NB_MESH_OK);

    WAIT_UNTIL(nb_mesh_peer_count(a.mesh) >= 1 && nb_mesh_peer_count(b.mesh) >= 1, 2000);
    CHECK_EQ(nb_mesh_peer_count(a.mesh), 1u);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 1u);
    /* 编队隔离：两个编队互相都看不见对方。 */
    CHECK_EQ(nb_mesh_peer_count(other.mesh), 0u);

    /* 邻居信息正确：node_id 非零、RPC 端口就是对方配置的端口、掩码正确。 */
    n = nb_mesh_peers(a.mesh, peers, 8);
    CHECK_EQ(n, 1u);
    if (n == 1) {
        CHECK(peers[0].node_id != 0);
        CHECK(peers[0].node_id != nb_mesh_node_id(a.mesh));
        CHECK_EQ(peers[0].rpc_port, b.rpc_port);
        CHECK_EQ(peers[0].service_mask, NB_SERVICE_SENSE_SLAM);
    }

    /* 掩码寻址：A 按 B 的服务位能找到 B 的 ip:port。 */
    memset(&target, 0, sizeof(target));
    CHECK(nb_mesh_find(a.mesh, NB_SERVICE_SENSE_SLAM, &target) == NB_MESH_OK);
    CHECK_EQ(target.rpc_port, b.rpc_port);
    CHECK_EQ(target.node_id, nb_mesh_node_id(b.mesh));
    CHECK(strcmp(target.host, "127.0.0.1") == 0);
    /* 未命中：A 没有提供的位、以及谁都没提供的位。 */
    CHECK(nb_mesh_find(a.mesh, NB_SERVICE_ACT_GIMBAL, &target) == NB_MESH_ERR_NOTFOUND);
    CHECK(nb_mesh_find(b.mesh, NB_SERVICE_CORE_FCS, &target) == NB_MESH_OK);
    CHECK(nb_mesh_find(b.mesh, NB_SERVICE_CORE_HEALTH, &target) == NB_MESH_ERR_NOTFOUND);
    CHECK(nb_mesh_find(a.mesh, 0, &target) == NB_MESH_ERR_INVALID);

    /* 名字哈希位同样可用于端到端的"服务名寻址"。 */
    CHECK(nb_mesh_find(a.mesh, nb_mesh_service_bit("whatever"), &target) ==
          NB_MESH_ERR_NOTFOUND);

    /* 服务位图可以运行期更新：下一帧心跳后对端即可按新位找到。 */
    CHECK(nb_mesh_set_service_mask(b.mesh, NB_SERVICE_ACT_GIMBAL) == NB_MESH_OK);
    WAIT_UNTIL(nb_mesh_find(a.mesh, NB_SERVICE_ACT_GIMBAL, &target) == NB_MESH_OK, 2000);
    CHECK(nb_mesh_find(a.mesh, NB_SERVICE_ACT_GIMBAL, &target) == NB_MESH_OK);
    CHECK_EQ(target.rpc_port, b.rpc_port);

    mesh_down(&a);
    mesh_down(&b);
    mesh_down(&other);
}

/* LEAVE 立即移除（不等租约）+ 下线后不可寻址。 */
static void test_discovery_leave(void)
{
    mesh_ctx_t a;
    mesh_ctx_t b;
    nb_mesh_target_t target;

    printf("  discovery leave\n");
    CHECK(mesh_up(&a, 11, NB_SERVICE_RELAY_NODE, 45001, 5000) == 0);
    CHECK(mesh_up(&b, 11, NB_SERVICE_CORE_HEALTH, 45002, 5000) == 0);
    if (!a.mesh || !b.mesh) {
        mesh_down(&a);
        mesh_down(&b);
        return;
    }
    mesh_link(&a, &b);
    WAIT_UNTIL(nb_mesh_peer_count(b.mesh) >= 1, 2000);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 1u);
    CHECK(nb_mesh_find(b.mesh, NB_SERVICE_RELAY_NODE, &target) == NB_MESH_OK);

    /* A 主动离开：B 必须在远小于租约（5 s）的时间内摘掉它。 */
    CHECK(nb_mesh_leave(a.mesh) == NB_MESH_OK);
    WAIT_UNTIL(nb_mesh_peer_count(b.mesh) == 0, 1000);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 0u);
    /* 下线即不可调用。 */
    CHECK(nb_mesh_find(b.mesh, NB_SERVICE_RELAY_NODE, &target) == NB_MESH_ERR_NOTFOUND);

    mesh_down(&a);
    mesh_down(&b);
}

/*
 * 租约过期：用一条裸 UDP socket 手工注入 JOIN，之后不再续心跳，验证 sweep
 * 在 lease 到期后剔除邻居。顺便覆盖"忽略自身"与源地址推断（real_ip = 0）。
 */
static void test_discovery_lease(void)
{
    mesh_ctx_t b;
    nb_mesh_target_t target;
    zn_socket *injector;
    nb_mesh_packet_t pkt;
    uint8_t buf[NB_MESH_WIRE_SIZE];
    uint16_t port;

    printf("  discovery lease\n");
    CHECK(mesh_up(&b, 12, 0, 47002, 400) == 0);
    if (!b.mesh) {
        return;
    }
    port = nb_mesh_port(b.mesh);
    injector = zn_socket_create();
    CHECK(injector != NULL);
    if (!injector) {
        mesh_down(&b);
        return;
    }
    CHECK(zn_socket_bind_udp_ex(injector, "127.0.0.1", 0, 1, 0) == ZN_OK);

    /* 先发一帧"自己的"报文：node_id 与本机相同，必须被忽略。 */
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = NB_MESH_MSG_HEARTBEAT;
    pkt.formation_id = 12;
    pkt.node_id = nb_mesh_node_id(b.mesh);
    pkt.real_ip = 0;
    pkt.rpc_port = 1234;
    pkt.service_mask = NB_SERVICE_CORE_FCS;
    CHECK(nb_mesh_encode(&pkt, buf) == NB_MESH_OK);
    CHECK(zn_socket_sendto(injector, buf, sizeof(buf), "127.0.0.1", port) > 0);
    spin(100);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 0u);

    /* 编队不符：同样丢弃。 */
    pkt.formation_id = 13;
    pkt.node_id = 0x1234;
    CHECK(nb_mesh_encode(&pkt, buf) == NB_MESH_OK);
    CHECK(zn_socket_sendto(injector, buf, sizeof(buf), "127.0.0.1", port) > 0);
    spin(100);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 0u);

    /* 合法 JOIN：编队一致、node_id 不同，real_ip = 0 -> 用源地址。 */
    pkt.msg_type = NB_MESH_MSG_JOIN;
    pkt.formation_id = 12;
    pkt.node_id = 0x1234;
    pkt.rpc_port = 9999;
    CHECK(nb_mesh_encode(&pkt, buf) == NB_MESH_OK);
    CHECK(zn_socket_sendto(injector, buf, sizeof(buf), "127.0.0.1", port) > 0);
    WAIT_UNTIL(nb_mesh_peer_count(b.mesh) == 1, 1000);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 1u);
    CHECK(nb_mesh_find(b.mesh, NB_SERVICE_CORE_FCS, &target) == NB_MESH_OK);
    CHECK_EQ(target.rpc_port, 9999u);
    CHECK_EQ(target.node_id, 0x1234u);
    CHECK(strcmp(target.host, "127.0.0.1") == 0);

    /* 不再续心跳：租约 400 ms 后必须被 sweep 掉，且下线后不可寻址。 */
    WAIT_UNTIL(nb_mesh_peer_count(b.mesh) == 0, 3000);
    CHECK_EQ(nb_mesh_peer_count(b.mesh), 0u);
    CHECK(nb_mesh_find(b.mesh, NB_SERVICE_CORE_FCS, &target) == NB_MESH_ERR_NOTFOUND);

    zn_socket_destroy(injector);
    mesh_down(&b);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("mesh test start\n");

    test_wire_codec();
    test_service_bit();
    test_packet_accept();
    test_table_basic();
    test_table_eviction();
    test_table_capacity(1000);
    test_table_capacity(5000);
    test_discovery_join();
    test_discovery_leave();
    test_discovery_lease();

    if (g_fail) {
        printf("mesh tests failed\n");
        return 1;
    }
    printf("mesh tests passed\n");
    return 0;
}
