/*
 * DDS loan lifetime verification（Phase 3「DDS loan 验证」）。
 *
 * DDS 接收走 dds_take loan：payload 指向 reader 借出的样本内存，整批样本在
 * nb_dds_on_data 返回后由 dds_return_loan 归还。因此 loan 指针只在同步回调期间
 * 有效，跨线程（WORKER/EXECUTOR）消费者必须拿到框架的副本，不能持有 loan。
 *
 * 本用例验证该契约：
 *   - WORKER handler 在回调返回并归还 loan 之后校验 payload，仍应看到原始字节；
 *   - INLINE handler 在回调期间校验 payload；
 *   - 客户端响应校验回显一致。
 * 如果将来把 DDS loan 直接当作 frame_owner 跨线程传递而没有正确的生命周期管理，
 * WORKER 用例会读到被 dds_return_loan 复用/释放的数据而失败。
 */
#include <nb/proxy.h>
#include <nb/node.h>
#include <nb/service.h>
#include <nb/dds.h>
#include <platform.h>
#include <thread/sem.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>

#define NB_LOAN_PAYLOAD_LEN 96u
#define NB_LOAN_DOMAIN 53

static void fill_pattern(uint8_t *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        out[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
    }
}

static int payload_is_pattern(const nb_payload_t *pl, const uint8_t *pattern)
{
    return pl && pl->len == NB_LOAN_PAYLOAD_LEN && pl->data &&
           memcmp(pl->data, pattern, NB_LOAN_PAYLOAD_LEN) == 0;
}

typedef struct loan_state {
    const uint8_t *pattern;
    ntk_sem *sem;
    int worker_seen; /* WORKER 在 loan 归还后仍读到原始字节 */
    int inline_seen; /* INLINE 在回调期间读到原始字节 */
    size_t worker_len;
    size_t inline_len;
} loan_state_t;

static void inline_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    loan_state_t *st = (loan_state_t *)user;
    if (payload_is_pattern(&req->payload, st->pattern)) {
        st->inline_len = req->payload.len;
        st->inline_seen = 1;
    }
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

/*
 * 故意延迟再读 payload：此刻 DDS reader 回调早已返回、loan 已归还。
 * 框架若已为异步 handler 复制，这里应仍读到原始字节。
 */
static void worker_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    loan_state_t *st = (loan_state_t *)user;
    ntk_sleep_ms(150);
    if (memcmp(req->payload.data, st->pattern, NB_LOAN_PAYLOAD_LEN) == 0) {
        st->worker_len = req->payload.len;
        st->worker_seen = 1;
    }
    nb_reply_bytes(call, 0, st->pattern, NB_LOAN_PAYLOAD_LEN);
}

typedef struct client_state {
    ntk_sem *sem;
    const uint8_t *pattern;
    int status;
    int payload_ok;
    size_t len;
} client_state_t;

static void on_response(int status, const nb_response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    if (status == NB_OK && resp) {
        st->len = resp->payload.len;
        st->payload_ok = payload_is_pattern(&resp->payload, st->pattern);
    }
    if (st->sem) {
        ntk_sem_post(st->sem, 1);
    }
}

int main(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    nb_binding_config_t binding;
    char endpoint[32];
    nb_service_method_options_t worker_opts;
    loan_state_t loan;
    uint8_t pattern[NB_LOAN_PAYLOAD_LEN];
    client_state_t worker_resp;
    client_state_t inline_resp;
    nb_payload_t pl;
    int failed = 0;

    fill_pattern(pattern, sizeof(pattern));
    memset(&loan, 0, sizeof(loan));
    loan.pattern = pattern;
    memset(&worker_resp, 0, sizeof(worker_resp));
    memset(&inline_resp, 0, sizeof(inline_resp));
    worker_resp.pattern = pattern;
    inline_resp.pattern = pattern;

    if (nb_cyclone_dds_register() != NB_OK) {
        nb_log_error("dds register failed");
        return 1;
    }
    snprintf(endpoint, sizeof(endpoint), "dds://%d", NB_LOAN_DOMAIN);
    memset(&binding, 0, sizeof(binding));
    binding.name = "dds";
    binding.endpoint = endpoint;
    binding.intra_process = NB_INTRA_PROCESS_OFF;

    nb_node_config_default(&cfg);
    cfg.name = "DdsLoan";
    cfg.rpc.mode = NB_NODE_MODE_STATIC;
    cfg.runtime.io_threads = 1;
    cfg.runtime.handler_threads = 1;
    cfg.bindings = &binding;
    cfg.binding_count = 1;

    if (nb_node_create(&cfg, &node) != NB_OK || nb_service_create(node, "LoanService", &svc) != NB_OK) {
        nb_log_error("dds node/service create failed");
        failed = 1;
        goto done;
    }
    worker_opts.exec = NB_SERVICE_EXEC_MODE_WORKER;
    worker_opts.executor = NULL;
    if (nb_service_add_method(svc, "inline", inline_handler, &loan) != NB_OK ||
        nb_service_add_method_with_options(svc, "worker", worker_handler, &loan, &worker_opts) != NB_OK) {
        nb_log_error("dds method registration failed");
        failed = 1;
        goto done;
    }
    if (nb_proxy_create(node, NULL, &proxy) != NB_OK ||
        nb_proxy_set_target_uri(proxy, endpoint) != NB_OK) {
        nb_log_error("dds proxy create failed");
        failed = 1;
        goto done;
    }

    pl.data = pattern;
    pl.len = sizeof(pattern);
    pl.encoding = 0;

    nb_call_options_t opt = NB_CALL_OPTIONS_INIT;
    opt.timeout_ms = 5000;

    /* 预热：DDS 发现可能尚未收敛，重试直到拿到真实响应。 */
    {
        nb_call_options_t warm_opt = NB_CALL_OPTIONS_INIT;

        warm_opt.timeout_ms = 500;
        for (int attempt = 0; attempt < 100 && !worker_resp.status && !inline_resp.status;
             ++attempt) {
            client_state_t warm;
            memset(&warm, 0, sizeof(warm));
            warm.pattern = pattern;
            warm.sem = ntk_sem_create(0);
            if (!warm.sem) {
                failed = 1;
                goto done;
            }
            (void)nb_proxy_call_async(
                proxy, "LoanService", "inline", &pl, &warm_opt, on_response, &warm);
            (void)ntk_sem_timedwait(warm.sem, 600);
            ntk_sem_destroy(warm.sem);
            if (warm.status == NB_OK) {
                break;
            }
            ntk_sleep_ms(50);
        }
    }

    inline_resp.sem = ntk_sem_create(0);
    worker_resp.sem = ntk_sem_create(0);
    if (!inline_resp.sem || !worker_resp.sem) {
        failed = 1;
        goto done;
    }

    if (nb_proxy_call_async(
            proxy, "LoanService", "inline", &pl, &opt, on_response, &inline_resp) != NB_OK) {
        nb_log_error("inline call failed");
        failed = 1;
        goto done;
    }
    if (ntk_sem_timedwait(inline_resp.sem, 5000) != NTK_OK) {
        nb_log_error("inline call timeout");
        failed = 1;
    } else if (inline_resp.status != NB_OK || !inline_resp.payload_ok) {
        nb_log_error("inline response bad status=%d ok=%d len=%zu",
                     inline_resp.status,
                     inline_resp.payload_ok,
                     inline_resp.len);
        failed = 1;
    } else if (!loan.inline_seen) {
        nb_log_error("inline handler did not see original loan payload");
        failed = 1;
    }

    if (nb_proxy_call_async(
            proxy, "LoanService", "worker", &pl, &opt, on_response, &worker_resp) != NB_OK) {
        nb_log_error("worker call failed");
        failed = 1;
        goto done;
    }
    if (ntk_sem_timedwait(worker_resp.sem, 5000) != NTK_OK) {
        nb_log_error("worker call timeout");
        failed = 1;
    } else if (worker_resp.status != NB_OK || !worker_resp.payload_ok) {
        nb_log_error("worker response bad status=%d ok=%d len=%zu",
                     worker_resp.status,
                     worker_resp.payload_ok,
                     worker_resp.len);
        failed = 1;
    } else if (!loan.worker_seen) {
        nb_log_error("worker handler read stale/loaned payload after loan return");
        failed = 1;
    }

done:
    if (inline_resp.sem) {
        ntk_sem_destroy(inline_resp.sem);
    }
    if (worker_resp.sem) {
        ntk_sem_destroy(worker_resp.sem);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (svc) {
        nb_service_destroy(svc);
    }
    if (node) {
        nb_node_destroy(node);
    }
    if (failed) {
        nb_log_error("DDS LOAN FAIL");
    } else {
        nb_log_info("DDS LOAN NB_OK");
    }
    return failed ? 1 : 0;
}
