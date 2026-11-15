/*
 * nb_mesh 现场验证工具（诊断用，不参与库构建）。
 *
 * 子命令：
 *   serve <formation> <group> <disc_port>
 *       创建一个 mesh 模式节点，注册 EchoService.echo，阻塞运行。
 *   call  <formation> <group> <disc_port>
 *       创建一个 mesh 模式节点，**不配置任何对端地址**，只靠组播发现
 *       EchoService，然后发一次调用并打印结果。
 *   mutual <formation> <group> <disc_port> <secs> <node_index>
 *       只起一个 mesh 节点（无服务），每秒打印一次邻居数，用于多进程互相
 *       发现验证（node_id 由 <node_index> 区分）。判据取最后一行 T（进程
 *       仍在运行时），EXIT 行在退出阶段，会因 LEAVE 偏小。
 *   scale <formation> <group> <disc_port> <peers>
 *       起一个真实 mesh 节点，再从另一个 socket 以 <peers> 个不同 node_id
 *       注入心跳，测量邻居表规模、find 时延与 CPU 占用。
 */
#define NB_LOG_TAG "probe"
#include <nb/nb.h>
#include <nb/mesh.h>

#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zn/zn.h>

static int echo_method(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    return nb_message_view_set_data(resp, req->payload.data, req->payload.len);
}

static void cfg_mesh(nb_node_config_t *cfg,
                     const char *name,
                     unsigned formation,
                     const char *group,
                     unsigned disc_port)
{
    nb_node_config_default(cfg);
    cfg->name = name;
    cfg->rpc.mode = NB_NODE_MODE_MESH;
    cfg->rpc.bind_host = "0.0.0.0";
    cfg->rpc.discovery.multicast_group = group;
    cfg->rpc.discovery.port = (uint16_t)disc_port;
    cfg->rpc.discovery.formation_id = formation;
    cfg->rpc.discovery.heartbeat_interval_ms = 200;
    cfg->rpc.discovery.lease_ms = 3000;
    cfg->runtime.io_threads = 1;
}

static int do_serve(unsigned formation, const char *group, unsigned disc_port)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    int rc;

    cfg_mesh(&cfg, "probe-server", formation, group, disc_port);
    rc = nb_node_create(&cfg, &node);
    if (rc == NB_OK) {
        rc = nb_service_create(node, "EchoService", &svc);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_sync_method(svc, "echo", echo_method, NULL);
    }
    if (rc != NB_OK) {
        nb_log_error("serve setup failed: %d", rc);
        nb_node_destroy(node);
        return 1;
    }
    nb_log_info("serving EchoService.echo (formation=%u group=%s disc=%u)",
                formation, group, disc_port);
    (void)nb_node_run(node);
    nb_node_destroy(node);
    return 0;
}

static int do_call(unsigned formation, const char *group, unsigned disc_port)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_proxy_t *proxy = NULL;
    nb_payload_t req_payload;
    nb_call_options_t opt = NB_CALL_OPTIONS_INIT;
    const char *text = "hello-from-probe";
    int attempt;
    int rc = NB_ERR_NOTFOUND;

    cfg_mesh(&cfg, "probe-client", formation, group, disc_port);
    if (nb_node_create(&cfg, &node) != NB_OK) {
        nb_log_error("client node create failed");
        return 1;
    }
    if (nb_proxy_create(node, NULL, &proxy) != NB_OK) {
        nb_node_destroy(node);
        return 1;
    }
    req_payload.data = text;
    req_payload.len = strlen(text) + 1;
    req_payload.encoding = 0;
    opt.timeout_ms = 1000;

    /* 零预知：只等组播发现出 EchoService，再按名字调用。 */
    for (attempt = 0; attempt < 100; attempt++) {
        nb_response_t resp;
        memset(&resp, 0, sizeof(resp));
        rc = nb_proxy_call_sync(proxy, "EchoService", "echo", &req_payload, &opt, &resp);
        if (rc == NB_OK) {
            int ok = resp.payload.len == req_payload.len &&
                     resp.payload.data &&
                     memcmp(resp.payload.data, text, req_payload.len) == 0;
            printf("CALL_OK=%d status=%d len=%zu\n", ok, resp.status, resp.payload.len);
            nb_response_release(&resp);
            rc = ok ? NB_OK : NB_ERR_INVALID;
            break;
        }
        (void)nb_node_run_once(node, 100);
    }
    if (rc != NB_OK) {
        printf("CALL_FAILED rc=%d after %d attempts\n", rc, attempt);
    }
    nb_proxy_destroy(proxy);
    nb_node_destroy(node);
    return rc == NB_OK ? 0 : 1;
}

/* ---- scale：一个真实节点 + N 个合成心跳 ---- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

static int do_mutual(unsigned formation, const char *group, unsigned disc_port,
                     unsigned secs, unsigned index, unsigned mask)
{
    nb_mesh_config_t mcfg;
    nb_mesh_t *mesh = NULL;
    unsigned i;

    nb_mesh_config_init(&mcfg);
    mcfg.formation_id = formation;
    mcfg.multicast_group = group;
    mcfg.port = (uint16_t)disc_port;
    mcfg.node_id = 0xBBBB000000000000ull + index;
    mcfg.rpc_port = (uint16_t)(46000u + index);
    /* mask 0 表示纯成员：不提供任何服务，只用来看邻居。 */
    mcfg.service_mask = mask;
    mcfg.heartbeat_ms = 1000;
    mcfg.lease_ms = 5000;
    mcfg.max_peers = 4096;
    if (nb_mesh_start(&mesh, &mcfg) != 0) {
        printf("MUTUAL index=%u START_FAILED\n", index);
        return 1;
    }
    printf("PORT index=%u want=%u got=%u\n", index, disc_port, nb_mesh_port(mesh));
    fflush(stdout);
    for (i = 0; i < secs * 10u; i++) {
        zn_sleep_ms(100);
        if ((i + 1) % 10u == 0) {
            printf("T index=%u t=%us peers=%zu\n", index, (i + 1) / 10u,
                   nb_mesh_peer_count(mesh));
            fflush(stdout);
        }
    }
    /*
     * 这一行在**各进程开始退出之后**打印，会因 LEAVE 而偏小，不能当作
     * "互相发现是否完成"的判据；判据是上面最后一行 T（进程仍在运行时）。
     */
    printf("EXIT index=%u peers_after_leave=%zu\n", index, nb_mesh_peer_count(mesh));
    nb_mesh_stop(mesh);
    return 0;
}

static int do_scale(unsigned formation, const char *group, unsigned disc_port, unsigned peers)
{
    nb_mesh_config_t mcfg;
    nb_mesh_t *mesh = NULL;
    nb_mesh_config_t dummy;
    zn_socket *tx = NULL;
    uint8_t pkt[26];
    unsigned i;
    int err = 0;
    uint64_t t0;
    uint64_t elapsed_ms;
    nb_mesh_peer_info_t *snapshot;
    size_t seen;

    nb_mesh_config_init(&mcfg);
    mcfg.formation_id = formation;
    mcfg.multicast_group = group;
    mcfg.port = (uint16_t)disc_port;
    mcfg.node_id = 0xAAAA000000000001ull;
    mcfg.rpc_port = 50000;
    mcfg.service_mask = (1u << 0);
    mcfg.heartbeat_ms = 1000;
    mcfg.lease_ms = 3000;
    mcfg.max_peers = peers + 16;
    if (nb_mesh_start(&mesh, &mcfg) != 0) {
        printf("mesh start failed\n");
        return 1;
    }
    nb_mesh_config_init(&dummy);

    tx = zn_socket_create();
    if (!tx || zn_socket_bind_udp_ex(tx, "127.0.0.1", 0, 1, 1) != ZN_OK) {
        printf("tx socket failed\n");
        nb_mesh_stop(mesh);
        return 1;
    }

    memset(pkt, 0, sizeof(pkt));
    put_u16(pkt, 0x5A43);
    pkt[2] = 2; /* HEARTBEAT */
    put_u32(pkt + 4, formation);
    t0 = zb_monotonic_ms();
    for (i = 0; i < peers; i++) {
        uint64_t node_id = 0x1111000000000000ull + i;
        uint32_t ip = (192u << 24) | (168u << 16) | (1u << 8) | (uint32_t)(i & 0xffu);

        put_u64(pkt + 8, node_id);
        put_u32(pkt + 16, ip);
        put_u16(pkt + 20, (uint16_t)(40000u + (i % 2000u)));
        put_u32(pkt + 22, 1u << (i % 32u));
        (void)zn_socket_sendto(tx, pkt, sizeof(pkt), "127.0.0.1", (uint16_t)disc_port);
        if ((i & 63u) == 63u) {
            zn_sleep_ms(2); /* 让接收侧跟上，模拟真实分批到达 */
        }
    }
    /* 等接收侧把最后一波处理完 */
    for (i = 0; i < 50; i++) {
        if (nb_mesh_peer_count(mesh) >= peers) {
            break;
        }
        zn_sleep_ms(20);
    }
    elapsed_ms = zb_monotonic_ms() - t0;
    seen = nb_mesh_peer_count(mesh);
    printf("SCALE peers=%u seen=%zu inject_ms=%llu\n",
           peers, seen, (unsigned long long)elapsed_ms);

    /* find 时延：对已存在的位做 N 次寻址 */
    {
        uint64_t f0 = zb_monotonic_ms();
        unsigned hits = 0;
        unsigned rounds = 200000;
        for (i = 0; i < rounds; i++) {
            nb_mesh_target_t t;
            if (nb_mesh_find(mesh, 1u << (i % 32u), &t) == NB_MESH_OK) {
                hits++;
            }
        }
        {
            uint64_t ms = zb_monotonic_ms() - f0;
            printf("FIND rounds=%u hits=%u total_ms=%llu per_us=%.3f\n",
                   rounds, hits, (unsigned long long)ms,
                   ms > 0 ? (double)ms * 1000.0 / (double)rounds : 0.0);
        }
    }

    snapshot = (nb_mesh_peer_info_t *)calloc(seen ? seen : 1, sizeof(*snapshot));
    if (snapshot) {
        (void)nb_mesh_peers(mesh, snapshot, seen);
        printf("SNAPSHOT first=0x%016llx last=0x%016llx\n",
               (unsigned long long)(seen ? snapshot[0].node_id : 0),
               (unsigned long long)(seen ? snapshot[seen - 1].node_id : 0));
        free(snapshot);
    }
    zn_socket_destroy(tx);
    nb_mesh_stop(mesh);
    return err;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s serve|call|scale|mutual <formation> <group> <disc_port> "
                "[peers|secs idx]\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "serve") == 0) {
        return do_serve((unsigned)atoi(argv[2]), argv[3], (unsigned)atoi(argv[4]));
    }
    if (strcmp(argv[1], "call") == 0) {
        return do_call((unsigned)atoi(argv[2]), argv[3], (unsigned)atoi(argv[4]));
    }
    if (strcmp(argv[1], "mutual") == 0 && argc >= 7) {
        {
            unsigned mask = argc >= 8 ? (unsigned)strtoul(argv[7], NULL, 0) : 0u;
            return do_mutual((unsigned)atoi(argv[2]), argv[3], (unsigned)atoi(argv[4]),
                             (unsigned)atoi(argv[5]), (unsigned)atoi(argv[6]), mask);
        }
    }
    if (strcmp(argv[1], "scale") == 0 && argc >= 6) {
        return do_scale((unsigned)atoi(argv[2]), argv[3], (unsigned)atoi(argv[4]),
                        (unsigned)atoi(argv[5]));
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
