#include <nb/internal/poller_alloc.h>
#include "node_access.h"
#include "pending.h"

#include <nb/rpc/node.h>

#define NB_LOG_TAG "test"
#include <nb/base/log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;
static zb_sem *g_sem;
static int g_task_ok;
static void *g_remote_obj;

static void pending_probe(void *user)
{
    nb_node_t *node = (nb_node_t *)user;
    nb_pending_t *pending[1024];
    unsigned i;

    memset(pending, 0, sizeof(pending));
    for (i = 0; i < sizeof(pending) / sizeof(pending[0]); i++) {
        pending[i] =
            nb_pending_add(node, (uint64_t)i + 1, NULL, NULL, 60000, nb_node_poller(node), NULL);
        if (!pending[i]) {
            g_task_ok = 0;
            zb_sem_post(g_sem, 1);
            return;
        }
    }
    for (i = 0; i < sizeof(pending) / sizeof(pending[0]); i++) {
        nb_pending_release(pending[i]);
    }
    g_task_ok = 1;
    zb_sem_post(g_sem, 1);
}

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

/* 在 poller 线程内验证：当前 poller 正确，且池释放后可复用同一块。 */
static void pool_probe(void *user)
{
    nb_node_t *node = (nb_node_t *)user;
    zn_poller *cur = nb_node_current_poller(node);

    g_task_ok = 0;
    if (cur != nb_node_poller(node)) {
        goto done;
    }

    /* nb_mem：owner 线程内释放应复用同一块。 */
    {
        void *m1 = nb_poller_alloc(64);
        void *m2;
        if (!m1) {
            goto done;
        }
        nb_poller_free(m1);
        m2 = nb_poller_alloc(64);
        if (m2 != m1) {
            nb_poller_free(m2);
            goto done;
        }
        nb_poller_free(m2);
    }
    /* 交给主线程跨线程释放：不得崩溃。 */
    g_remote_obj = nb_poller_alloc(64);
    if (!g_remote_obj) {
        goto done;
    }
    g_task_ok = 1;
done:
    zb_sem_post(g_sem, 1);
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    unsigned i;
    unsigned n;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "pooltest";
    cfg.rpc.kind = NB_TRANSPORT_KIND_UDP;
    cfg.rpc.bind_host = "127.0.0.1";
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 2;

    if (nb_node_create(&cfg, &node) != NB_OK) {
        nb_log_error("node create failed");
        return 1;
    }

    n = zn_poller_pool_size(nb_node_pool(node));
    CHECK(n >= 1);
    /* 主线程自己不驱动任何 poller：poller 已建好，但当前线程都不拥有。 */
    CHECK(nb_node_current_poller(node) == NULL);
    for (i = 0; i < n; i++) {
        zn_poller *p = zn_poller_pool_at(nb_node_pool(node), i);
        CHECK(p != NULL);
        CHECK(!zn_poller_is_current_thread(p));
    }

    /* 在 owner poller 线程内验证 current + 复用。 */
    g_sem = zb_sem_create(0);
    CHECK(g_sem != NULL);
    if (g_sem) {
        CHECK(zn_poller_async(nb_node_poller(node), pool_probe, node, 0) == ZN_OK);
        CHECK(zb_sem_timedwait(g_sem, 2000) == ZN_OK);
        CHECK(g_task_ok == 1);
        CHECK(zn_poller_async(nb_node_poller(node), pending_probe, node, 0) == ZN_OK);
        CHECK(zb_sem_timedwait(g_sem, 2000) == ZN_OK);
        CHECK(g_task_ok == 1);
        zb_sem_destroy(g_sem);
        g_sem = NULL;
    }

    /* 跨线程释放（非 owner poller 线程）。 */
    if (g_remote_obj) {
        nb_poller_free(g_remote_obj);
        g_remote_obj = NULL;
    }

    nb_node_destroy(node);
    if (g_fail) {
        nb_log_error("POOL FAIL");
    } else {
        nb_log_info("POOL NB_OK");
    }
    return g_fail ? 1 : 0;
}
