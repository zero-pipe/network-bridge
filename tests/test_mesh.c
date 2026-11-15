/*
 * nb_mesh 单元测试：契约哈希、组播心跳发现、编队隔离、按需内省。
 *
 * 全部走 loopback 单播目标（不依赖环境里是否有可用组播），验证的是协议与
 * 状态机本身：报文编解码、同构节点零握手、异构节点拉取 manifest、编队隔离。
 */
#include <nb/mesh.h>

#include <zn/zn.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

typedef struct observed {
    int peer_added;
    int peer_removed;
    int manifest_events;
    uint64_t last_node_id;
    uint32_t last_service_hash;
    uint32_t last_rpc_port;
    uint32_t last_service_mask;
    size_t manifest_count;
    char manifest[8][NB_MESH_NAME_MAX];
} observed_t;

static void on_peer(void *user, const nb_mesh_peer_info_t *peer, int removed)
{
    observed_t *o = (observed_t *)user;

    if (!o || !peer) {
        return;
    }
    o->last_node_id = peer->node_id;
    o->last_service_hash = peer->service_hash;
    o->last_rpc_port = peer->rpc_port;
    o->last_service_mask = peer->service_mask;
    if (removed) {
        o->peer_removed++;
    } else {
        o->peer_added++;
    }
}

static void on_manifest(void *user,
                        const nb_mesh_peer_info_t *peer,
                        const char *const *services,
                        size_t count)
{
    observed_t *o = (observed_t *)user;
    size_t i;

    (void)peer;
    if (!o) {
        return;
    }
    o->manifest_events++;
    o->manifest_count = count;
    for (i = 0; i < count && i < 8; i++) {
        snprintf(o->manifest[i], sizeof(o->manifest[i]), "%s", services[i]);
    }
}

static void pump(zn_poller *poller, int rounds)
{
    int i;

    for (i = 0; i < rounds; i++) {
        (void)zn_poller_poll(poller, 20);
    }
}

static void test_schema_hash(void)
{
    const char *a[] = {"Order", "Echo", "Vision"};
    const char *b[] = {"Vision", "Echo", "Order"};
    const char *c[] = {"Order", "Echo"};

    printf("  hash step 1\n");
    printf("  h(a)=%08x h(b)=%08x h(c)=%08x\n",
           nb_mesh_service_hash(a, 3),
           nb_mesh_service_hash(b, 3),
           nb_mesh_service_hash(c, 2));

    CHECK(nb_mesh_service_hash(a, 3) == nb_mesh_service_hash(b, 3));
    CHECK(nb_mesh_service_hash(a, 3) != nb_mesh_service_hash(c, 2));
    CHECK(nb_mesh_service_hash(a, 3) != 0);
    CHECK(nb_mesh_service_hash(NULL, 0) == nb_mesh_service_hash(NULL, 0));
}

typedef struct node_ctx {
    nb_mesh_t *mesh;
    observed_t obs;
} node_ctx_t;

static int node_start(zn_poller *poller,
                      node_ctx_t *ctx,
                      uint32_t formation,
                      const char *const *services,
                      size_t service_count,
                      uint32_t rpc_port,
                      uint32_t mask)
{
    nb_mesh_config_t cfg;
    int err = 0;

    memset(ctx, 0, sizeof(*ctx));
    nb_mesh_config_init(&cfg);
    cfg.bind_host = "127.0.0.1";
    cfg.port = 0;                 /* 临时端口，避免与并行测试冲突 */
    cfg.multicast_group = "";     /* 只用单播目标 */
    cfg.formation_id = formation;
    cfg.node_id = 0;              /* 随机 */
    cfg.rpc_port = rpc_port;
    cfg.service_mask = mask;
    cfg.heartbeat_ms = 100;
    cfg.lease_ms = 1500;
    cfg.callbacks.on_peer = on_peer;
    cfg.callbacks.on_manifest = on_manifest;
    cfg.user = &ctx->obs;
    ctx->mesh = nb_mesh_create(poller, &cfg, &err);
    if (!ctx->mesh) {
        printf("mesh create failed: %s\n", nb_mesh_strerror(err));
        return -1;
    }
    (void)nb_mesh_set_services(ctx->mesh, services, service_count);
    return 0;
}

static void test_contract_match_and_isolation(void)
{
    static const char *const services[] = {"Order", "Echo"};
    zn_poller *poller = zn_poller_create();
    node_ctx_t a;
    node_ctx_t b;
    node_ctx_t other_formation;

    CHECK(poller != NULL);
    if (!poller) {
        return;
    }
    CHECK(node_start(poller, &a, 7, services, 2, 41001, 0x3) == 0);
    CHECK(node_start(poller, &b, 7, services, 2, 41002, 0x5) == 0);
    /* 不同编队：即便报文能到达，也必须被丢弃。 */
    CHECK(node_start(poller, &other_formation, 8, services, 2, 41003, 0x0) == 0);
    if (!a.mesh || !b.mesh || !other_formation.mesh) {
        nb_mesh_destroy(a.mesh);
        nb_mesh_destroy(b.mesh);
        nb_mesh_destroy(other_formation.mesh);
        zn_poller_destroy(poller);
        return;
    }

    /* 互相指向对方的发现端口。 */
    (void)nb_mesh_add_peer(a.mesh, "127.0.0.1", nb_mesh_port(b.mesh));
    (void)nb_mesh_add_peer(b.mesh, "127.0.0.1", nb_mesh_port(a.mesh));
    (void)nb_mesh_add_peer(other_formation.mesh, "127.0.0.1", nb_mesh_port(a.mesh));
    (void)nb_mesh_add_peer(a.mesh, "127.0.0.1", nb_mesh_port(other_formation.mesh));

    (void)nb_mesh_announce(a.mesh);
    (void)nb_mesh_announce(b.mesh);
    (void)nb_mesh_announce(other_formation.mesh);
    pump(poller, 20);

    /* 同编队互见；服务版本哈希一致时不需要内省。 */
    CHECK(nb_mesh_peer_count(a.mesh) == 1);
    CHECK(nb_mesh_peer_count(b.mesh) == 1);
    CHECK(a.obs.peer_added == 1);
    CHECK(a.obs.last_service_hash != 0);
    CHECK(a.obs.last_rpc_port == 41002);
    CHECK(a.obs.last_service_mask == 0x5);
    CHECK(a.obs.manifest_events == 0);
    /* 编队 8 的节点看不到编队 7，反之亦然。 */
    CHECK(nb_mesh_peer_count(other_formation.mesh) == 0);
    CHECK(other_formation.obs.peer_added == 0);

    /* 租约过期：停止心跳后对端被移除。 */
    {
        int waited = 0;

        while (nb_mesh_peer_count(b.mesh) > 0 && waited < 40) {
            nb_mesh_tick(b.mesh, zn_wall_ms());
            pump(poller, 1);
            waited++;
        }
        CHECK(nb_mesh_peer_count(b.mesh) == 0);
        CHECK(b.obs.peer_removed >= 1);
    }

    nb_mesh_destroy(a.mesh);
    nb_mesh_destroy(b.mesh);
    nb_mesh_destroy(other_formation.mesh);
    zn_poller_destroy(poller);
}

static void test_lazy_introspection(void)
{
    static const char *const a_services[] = {"Order"};
    static const char *const b_services[] = {"Order", "PluginVision", "PluginLidar"};
    zn_poller *poller = zn_poller_create();
    node_ctx_t a;
    node_ctx_t b;

    CHECK(poller != NULL);
    if (!poller) {
        return;
    }
    CHECK(node_start(poller, &a, 3, a_services, 1, 42001, 0) == 0);
    CHECK(node_start(poller, &b, 3, b_services, 3, 42002, 0) == 0);
    if (!a.mesh || !b.mesh) {
        nb_mesh_destroy(a.mesh);
        nb_mesh_destroy(b.mesh);
        zn_poller_destroy(poller);
        return;
    }
    (void)nb_mesh_add_peer(a.mesh, "127.0.0.1", nb_mesh_port(b.mesh));
    (void)nb_mesh_add_peer(b.mesh, "127.0.0.1", nb_mesh_port(a.mesh));

    (void)nb_mesh_announce(a.mesh);
    (void)nb_mesh_announce(b.mesh);
    pump(poller, 30);

    /* 版本不同：A 能看到 B，按需内省拿到对方的服务清单。 */
    CHECK(nb_mesh_peer_count(a.mesh) == 1);
    CHECK(a.obs.peer_added == 1);
    CHECK(a.obs.last_service_hash != 0);
    /* 按需内省：mesh 自动拉取一次 manifest，A 收到 B 的 3 个服务。 */
    CHECK(a.obs.manifest_events >= 1);
    CHECK(a.obs.manifest_count == 3);

    nb_mesh_destroy(a.mesh);
    nb_mesh_destroy(b.mesh);
    zn_poller_destroy(poller);
}

static void test_leave(void)
{
    static const char *const services[] = {"Svc"};
    zn_poller *poller = zn_poller_create();
    node_ctx_t a;
    node_ctx_t b;

    CHECK(poller != NULL);
    if (!poller) {
        return;
    }
    CHECK(node_start(poller, &a, 11, services, 1, 43001, 0) == 0);
    CHECK(node_start(poller, &b, 11, services, 1, 43002, 0) == 0);
    if (!a.mesh || !b.mesh) {
        nb_mesh_destroy(a.mesh);
        nb_mesh_destroy(b.mesh);
        zn_poller_destroy(poller);
        return;
    }
    (void)nb_mesh_add_peer(b.mesh, "127.0.0.1", nb_mesh_port(a.mesh));
    (void)nb_mesh_add_peer(a.mesh, "127.0.0.1", nb_mesh_port(b.mesh));
    (void)nb_mesh_announce(a.mesh);
    pump(poller, 15);
    CHECK(nb_mesh_peer_count(b.mesh) == 1);

    (void)nb_mesh_leave(a.mesh);
    pump(poller, 10);
    CHECK(nb_mesh_peer_count(b.mesh) == 0);
    CHECK(b.obs.peer_removed >= 1);

    nb_mesh_destroy(a.mesh);
    nb_mesh_destroy(b.mesh);
    zn_poller_destroy(poller);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("mesh test start\n");
    test_schema_hash();
    test_contract_match_and_isolation();
    test_lazy_introspection();
    test_leave();
    if (g_fail) {
        printf("mesh tests failed\n");
        return 1;
    }
    printf("mesh tests passed\n");
    return 0;
}
