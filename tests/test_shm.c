/*
 * SHM transport end-to-end test.
 *
 * 一个 server（端口 1）+ 多个 client（端口 2..4）共享同一个 POSIX 共享内存段，
 * 请求/响应队列、provider 线程与 chunk 池完全按真实部署路径运行，覆盖多客户端
 * 对同一 server 的并发回投。跨进程覆盖由 benchmark 工具（--shm）与质量门禁提供。
 */
#include <nb/rpc/proxy.h>
#include <nb/rpc/node.h>
#include <nb/rpc/service.h>
#include <nb/rpc/proxy.h>
#include <nb/rpc/shm.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "test"
#include <nb/base/log.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CLIENT_COUNT 3

typedef struct shm_client_state {
    ntk_sem *sem;
    int status;
    uint8_t buf[8192];
    size_t len;
} shm_client_state_t;

/* 订阅回调状态：EVENT 到达时记录 payload 并唤醒主线程。 */
typedef struct shm_sub_state {
    ntk_sem *sem;
    volatile int count;
    uint8_t buf[256];
    size_t len;
} shm_sub_state_t;

static void echo_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

static void on_event(nb_call_t *call, const nb_request_t *req, void *user)
{
    shm_sub_state_t *st = (shm_sub_state_t *)user;
    (void)call;
    st->len = req->payload.len <= sizeof(st->buf) ? req->payload.len : sizeof(st->buf);
    memcpy(st->buf, req->payload.data, st->len);
    st->count++;
    if (st->sem) {
        ntk_sem_post(st->sem, 1);
    }
}

static void on_response(int status, const nb_response_t *resp, void *user)
{
    shm_client_state_t *st = (shm_client_state_t *)user;
    st->status = status;
    st->len = 0;
    if (status == NB_OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
    }
    ntk_sem_post(st->sem, 1);
}

static int run_call(nb_proxy_t *proxy, shm_client_state_t *st,
                    const void *data, size_t len)
{
    int attempt;

    for (attempt = 0; attempt < 50; attempt++) {
        nb_payload_t payload;
        nb_call_options_t opt = NB_CALL_OPTIONS_INIT;
        int rc;

        payload.data = data;
        payload.len = len;
        payload.encoding = 0;
        opt.timeout_ms = 2000;
        st->status = -999;
        st->len = 0;
        rc = nb_proxy_call_async(
            proxy, "echo", "ping", &payload, &opt, on_response, st);
        if (rc != NB_OK) {
            return rc;
        }
        if (ntk_sem_timedwait(st->sem, 2500) == NTK_OK) {
            if (st->status == NB_ERR_AGAIN) {
                ntk_sleep_ms(10);
                continue;
            }
            return st->status;
        }
        ntk_sleep_ms(10);
    }
    return NB_ERR_TIMEOUT;
}

static int verify_call(nb_proxy_t *proxy, shm_client_state_t *st,
                       const void *data, size_t len)
{
    int rc = run_call(proxy, st, data, len);
    if (rc != NB_OK) {
        nb_log_error("shm call failed: %d", rc);
        return 1;
    }
    if (st->len != len || memcmp(st->buf, data, len) != 0) {
        nb_log_error("shm response mismatch: got=%zu expected=%zu", st->len, len);
        return 1;
    }
    return 0;
}

int main(void)
{
    char channel[64];
    char server_endpoint[96];
    char client_endpoint[CLIENT_COUNT][96];
    nb_binding_config_t server_binding;
    nb_binding_config_t client_binding[CLIENT_COUNT];
    nb_node_config_t server_cfg;
    nb_node_config_t client_cfg[CLIENT_COUNT];
    nb_node_t *server = NULL;
    nb_node_t *client[CLIENT_COUNT] = {NULL};
    nb_service_t *service = NULL;
    nb_service_t *subscriber = NULL;
    nb_proxy_t *proxy[CLIENT_COUNT] = {NULL};
    shm_client_state_t st;
    shm_sub_state_t sub_st;
    int failed = 0;
    int rc;
    int i;
    int c;

    snprintf(channel, sizeof(channel), "test-shm-%ld", (long)getpid());
    snprintf(server_endpoint, sizeof(server_endpoint), "shm://%s:1", channel);
    for (c = 0; c < CLIENT_COUNT; c++) {
        snprintf(client_endpoint[c], sizeof(client_endpoint[c]), "shm://%s:%d", channel, c + 2);
    }

    if (nb_shm_register() != NB_OK) {
        nb_log_error("shm provider register failed");
        return 1;
    }

    memset(&server_binding, 0, sizeof(server_binding));
    server_binding.name = "shm";
    server_binding.endpoint = server_endpoint;

    nb_node_config_default(&server_cfg);
    server_cfg.name = "shm-server";
    server_cfg.rpc.mode = NB_NODE_MODE_STATIC;
    server_cfg.runtime.io_threads = 1;
    server_cfg.bindings = &server_binding;
    server_cfg.binding_count = 1;

    for (c = 0; c < CLIENT_COUNT; c++) {
        memset(&client_binding[c], 0, sizeof(client_binding[c]));
        client_binding[c].name = "shm";
        client_binding[c].endpoint = client_endpoint[c];

        nb_node_config_default(&client_cfg[c]);
        client_cfg[c].name = "shm-client";
        client_cfg[c].rpc.mode = NB_NODE_MODE_STATIC;
        client_cfg[c].runtime.io_threads = 1;
        client_cfg[c].bindings = &client_binding[c];
        client_cfg[c].binding_count = 1;
    }

    rc = nb_node_create(&server_cfg, &server);
    if (rc == NB_OK) {
        rc = nb_service_create(server, "echo", &service);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(service, "ping", echo_handler, NULL);
    }
    for (c = 0; rc == NB_OK && c < CLIENT_COUNT; c++) {
        rc = nb_node_create(&client_cfg[c], &client[c]);
        if (rc == NB_OK) {
            rc = nb_proxy_create(client[c], NULL, &proxy[c]);
        }
        if (rc == NB_OK) {
            rc = nb_proxy_set_target_uri(proxy[c], server_endpoint);
        }
    }
    /* client[0] 兼任订阅者：方法名即主题名，注册时会把 route 写入段内订阅表。 */
    memset(&sub_st, 0, sizeof(sub_st));
    if (rc == NB_OK) {
        rc = nb_service_create(client[0], "TopicService", &subscriber);
    }
    if (rc == NB_OK) {
        rc = nb_service_add_method(subscriber, "chat", on_event, &sub_st);
    }
    if (rc != NB_OK) {
        nb_log_error("shm setup failed: %d", rc);
        failed = 1;
        goto done;
    }

    memset(&st, 0, sizeof(st));
    st.sem = ntk_sem_create(0);
    if (!st.sem) {
        failed = 1;
        goto done;
    }
    sub_st.sem = ntk_sem_create(0);
    if (!sub_st.sem) {
        failed = 1;
    }

    for (c = 0; c < CLIENT_COUNT && !failed; c++) {
        const char *small = "hello-shm";

        if (verify_call(proxy[c], &st, small, strlen(small) + 1)) {
            failed = 1;
        }
    }
    {
        uint8_t big[5000];

        for (i = 0; i < (int)sizeof(big); i++) {
            big[i] = (uint8_t)('A' + (i % 26));
        }
        /* 轮转各客户端：验证每条的响应都回到发起方自己的队列。 */
        for (c = 0; c < CLIENT_COUNT && !failed; c++) {
            if (verify_call(proxy[c], &st, big, sizeof(big))) {
                failed = 1;
            }
        }
    }
    for (i = 0; i < 100 && !failed; i++) {
        char text[64];
        int ci = i % CLIENT_COUNT;

        snprintf(text, sizeof(text), "shm-request-%d", i);
        if (verify_call(proxy[ci], &st, text, strlen(text) + 1)) {
            failed = 1;
        }
    }

    /* pub/sub：client[1] 发布事件，client[0] 的订阅方法接收（跨节点零拷贝扇出）。 */
    if (!failed) {
        nb_payload_t ev;
        int attempt;

        ev.data = "pubsub-over-shm";
        ev.len = strlen((const char *)ev.data) + 1;
        ev.encoding = 0;
        /* 等 client[0] 的 IO 线程执行方法注册（其中包含 subscribe）。 */
        ntk_sleep_ms(100);
        for (attempt = 0; attempt < 50 && sub_st.count == 0; attempt++) {
            if (nb_proxy_call_event(proxy[1], "TopicService", "chat", &ev, NULL) != NB_OK) {
                failed = 1;
                break;
            }
            (void)ntk_sem_timedwait(sub_st.sem, 200);
        }
        if (!failed && (sub_st.count == 0 || sub_st.len != ev.len ||
                        memcmp(sub_st.buf, ev.data, ev.len) != 0)) {
            nb_log_error(
                "shm pub/sub not delivered: count=%d len=%zu", sub_st.count, sub_st.len);
            failed = 1;
        }
    }

    /* 零拷贝发布：client[2] 借 chunk 直写 payload 再 publish，client[0] 收到。 */
    if (!failed) {
        static const char expect[] = "loan-pubsub-over-shm";
        nb_loan_t *loan = NULL;
        uint8_t *data = NULL;
        uint32_t cap = 0;
        int attempt;

        sub_st.count = 0;
        sub_st.len = 0;
        for (attempt = 0; attempt < 50 && sub_st.count == 0; attempt++) {
            int lrc = nb_proxy_loan(proxy[2],
                                        "TopicService",
                                        "chat",
                                        (uint32_t)sizeof(expect),
                                        &loan,
                                        &data,
                                        &cap);
            if (lrc == NB_ERR_AGAIN) {
                ntk_sleep_ms(10);
                continue;
            }
            if (lrc != NB_OK) {
                nb_log_error("shm loan failed: %d", lrc);
                failed = 1;
                break;
            }
            if (cap < sizeof(expect)) {
                nb_log_error("shm loan capacity too small: %u", cap);
                nb_proxy_loan_release(loan);
                failed = 1;
                break;
            }
            memcpy(data, expect, sizeof(expect));
            if (nb_proxy_publish_loan(loan, (uint32_t)sizeof(expect), 0, NULL) != NB_OK) {
                failed = 1;
                break;
            }
            (void)ntk_sem_timedwait(sub_st.sem, 200);
        }
        if (!failed && (sub_st.count == 0 || sub_st.len != sizeof(expect) ||
                        memcmp(sub_st.buf, expect, sizeof(expect)) != 0)) {
            nb_log_error("shm loan pub/sub not delivered: count=%d len=%zu",
                         sub_st.count,
                         sub_st.len);
            failed = 1;
        }
    }

    /* 归还路径不能泄漏 chunk：连续借还大块后，256K 档仍应有空闲块可用。 */
    if (!failed) {
        int i;

        for (i = 0; i < 64 && !failed; i++) {
            nb_loan_t *loan = NULL;
            uint8_t *data = NULL;
            uint32_t cap = 0;
            int lrc = nb_proxy_loan(
                proxy[2], "TopicService", "chat", 250000, &loan, &data, &cap);

            if (lrc != NB_OK) {
                nb_log_error("shm loan pool leaked (release does not return chunks): %d", lrc);
                failed = 1;
                break;
            }
            nb_proxy_loan_release(loan);
        }
    }

    if (sub_st.sem) {
        ntk_sem_destroy(sub_st.sem);
    }
    ntk_sem_destroy(st.sem);

done:
    if (subscriber) {
        nb_service_destroy(subscriber);
    }
    for (c = 0; c < CLIENT_COUNT; c++) {
        if (proxy[c]) {
            nb_proxy_destroy(proxy[c]);
        }
        if (client[c]) {
            nb_node_destroy(client[c]);
        }
    }
    if (service) {
        nb_service_destroy(service);
    }
    if (server) {
        nb_node_destroy(server);
    }
    if (failed) {
        nb_log_error("nb_shm: FAIL");
        return 1;
    }
    nb_log_info("nb_shm: NB_OK");
    return 0;
}
