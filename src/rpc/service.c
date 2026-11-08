/* 服务端：service 生命周期与方法注册（执行见 service_dispatch.c / service_stream.c）。 */
#include "node_internal.h"
#include "service_internal.h"


#include "envelope.h"
#include "metric_macros.h"
#include <nb/internal/poller_alloc.h>
#include "util.h"

#include <nb/log.h>

#include <stdlib.h>
#include <string.h>

/* 同步风格 handler 的桥接：返回值即状态码，把视图回复后再释放。 */
typedef struct nb_sync_bridge {
    nb_sync_handler_fn fn;
    void *user;
} nb_sync_bridge_t;

static void nb_sync_method_bridge(nb_call_t *call, const nb_request_t *req, void *user)
{
    nb_sync_bridge_t *bridge = (nb_sync_bridge_t *)user;
    nb_message_view_t view;
    nb_payload_t pl;
    int status;
    nb_message_view_init(&view);
    status = bridge->fn(req, &view, bridge->user);
    nb_message_view_to_payload(&view, &pl);
    nb_reply(call, status, &pl);
    nb_message_view_release(&view);
}

typedef struct nb_svc_task {
    nb_node_t *node;
    nb_service_t *svc;
    nb_method_t *method;
} nb_svc_task_t;

typedef struct nb_announce_task {
    nb_node_t *node;
} nb_announce_task_t;

static void nb_announce_task_fn(void *user)
{
    nb_announce_task_t *t = (nb_announce_task_t *)user;
    if (t) {
        if (t->node && t->node->registry_backend && t->node->registry_backend->ops->announce) {
            t->node->registry_backend->ops->announce(t->node, 0);
        }
        free(t);
    }
}

static void nb_svc_link_task(void *user)
{
    nb_svc_task_t *t = (nb_svc_task_t *)user;
    ntk_mutex_lock(t->node->services_lock);
    t->svc->next = t->node->services;
    t->node->services = t->svc;
    ntk_mutex_unlock(t->node->services_lock);
    if (t->node->registry_backend && t->node->registry_backend->ops->announce) {
        t->node->registry_backend->ops->announce(t->node, 0);
    }
    free(t);
}

static void nb_svc_method_task(void *user)
{
    nb_svc_task_t *t = (nb_svc_task_t *)user;
    size_t i;
    ntk_mutex_lock(t->node->services_lock);
    t->method->next = t->svc->methods;
    t->svc->methods = t->method;
    ntk_mutex_unlock(t->node->services_lock);
    for (i = 0; i < t->node->transport_count; i++) {
        (void)nb_transport_subscribe(t->node->transports[i], t->svc->name, t->method->name);
    }
    free(t);
}

/* 由客户端 timeout_ms 换算成绝对 deadline（接收时刻起算）。 */
uint64_t nb_service_deadline_from_timeout(uint32_t timeout_ms)
{
    return timeout_ms ? nb_now_ms() + timeout_ms : 0;
}

/* ---- 服务生命周期 ---- */

int nb_service_create(nb_node_t *node, const char *service_name, nb_service_t **out)
{
    nb_service_t *svc;
    nb_svc_task_t *t;
    int rc;

    if (!node || !service_name || !service_name[0] || !out) {
        return NB_ERR_INVALID;
    }
    /*
     * The wire route is "service.method" and is split at the first '.', so a
     * service name containing '.' cannot be addressed. Reject it here instead
     * of registering a service no request can reach.
     */
    if (strchr(service_name, '.') != NULL) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    svc = (nb_service_t *)calloc(1, sizeof(*svc));
    if (!svc) {
        return NB_ERR_NOMEM;
    }
    svc->node = node;
    rc = nb_copy_checked(svc->name, sizeof(svc->name), service_name, NULL);
    if (rc != NB_OK) {
        free(svc);
        return rc;
    }
    t = (nb_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(svc);
        return NB_ERR_NOMEM;
    }
    t->node = node;
    t->svc = svc;
    if (ntk_poller_async(node->poller, nb_svc_link_task, t, 1) != NTK_OK) {
        free(t);
        free(svc);
        return NB_ERR_IO;
    }
    *out = svc;
    return NB_OK;
}

void nb_service_destroy(nb_service_t *svc)
{
    /*
     * 运行期摘除：置 removed 后不再匹配新请求（查找路径跳过）。方法与服务
     * 内存保留到 nb_node_destroy 统一释放，避免在途 handler 仍持有 method 指针
     * 时被释放。announce 回投 poller，保持发现面一致（best-effort）。
     */
    if (!svc || !svc->node || !svc->node->services_lock) {
        return;
    }
    ntk_mutex_lock(svc->node->services_lock);
    svc->removed = 1;
    ntk_mutex_unlock(svc->node->services_lock);
    if (svc->node->poller && svc->node->registry_backend &&
        svc->node->registry_backend->ops->announce) {
        nb_announce_task_t *t = (nb_announce_task_t *)calloc(1, sizeof(*t));
        if (t) {
            t->node = svc->node;
            if (nb_node_post(svc->node, nb_announce_task_fn, t) != NB_OK) {
                free(t);
            }
        }
    }
}

int nb_service_set_interceptor(nb_service_t *svc, nb_service_interceptor_fn fn, void *user)
{
    if (!svc) {
        return NB_ERR_INVALID;
    }
    if (svc->node && svc->node->services_lock) {
        ntk_mutex_lock(svc->node->services_lock);
    }
    svc->interceptor = fn;
    svc->interceptor_user = user;
    if (svc->node && svc->node->services_lock) {
        ntk_mutex_unlock(svc->node->services_lock);
    }
    return NB_OK;
}

/* ---- 方法注册 ---- */

/*
 * 解析调用者选择的执行模式。allow_worker 为 0 时只允许 INLINE（流式方法）。
 * 输出 run_in_worker / executor 二选一。
 */
static int nb_service_parse_exec(const nb_service_method_options_t *options,
                              int allow_worker,
                              int *out_worker,
                              const nb_service_executor_t **out_executor)
{
    nb_service_exec_mode_t exec = options ? options->exec : NB_SERVICE_EXEC_MODE_INLINE;
    const nb_service_executor_t *executor = options ? options->executor : NULL;

    *out_worker = 0;
    *out_executor = NULL;

    if (exec == NB_SERVICE_EXEC_MODE_INLINE) {
        return executor ? NB_ERR_INVALID : NB_OK;
    }
    if (exec == NB_SERVICE_EXEC_MODE_WORKER && allow_worker) {
        if (executor) {
            return NB_ERR_INVALID;
        }
        *out_worker = 1;
        return NB_OK;
    }
    if (exec == NB_SERVICE_EXEC_MODE_EXECUTOR && allow_worker) {
        if (!executor || !executor->submit) {
            return NB_ERR_INVALID;
        }
        *out_executor = executor;
        return NB_OK;
    }
    return NB_ERR_INVALID;
}

static int nb_service_method_register(nb_service_t *svc,
                                   const char *method,
                                   nb_handler_fn fn,
                                   nb_chunk_handler_fn stream_fn,
                                   void *user,
                                   int owns_user,
                                   int run_in_worker,
                                   const nb_service_executor_t *executor)
{
    nb_method_t *m = NULL;
    nb_svc_task_t *t;
    int rc;

    if (!svc || !method || !method[0] || (!fn && !stream_fn)) {
        return NB_ERR_INVALID;
    }
    m = (nb_method_t *)calloc(1, sizeof(*m));
    if (!m) {
        return NB_ERR_NOMEM;
    }
    rc = nb_copy_checked(m->name, sizeof(m->name), method, NULL);
    if (rc != NB_OK) {
        free(m);
        return rc;
    }
    m->fn = fn;
    m->stream_fn = stream_fn;
    m->user = user;
    m->owns_user = owns_user;
    m->run_in_worker = run_in_worker;
    m->executor = executor;
    t = (nb_svc_task_t *)calloc(1, sizeof(*t));
    if (!t) {
        free(m);
        return NB_ERR_NOMEM;
    }
    t->node = svc->node;
    t->svc = svc;
    t->method = m;
    if (ntk_poller_async(svc->node->poller, nb_svc_method_task, t, 1) != NTK_OK) {
        free(t);
        free(m);
        return NB_ERR_IO;
    }
    return NB_OK;
}

int nb_service_add_method(nb_service_t *svc, const char *method, nb_handler_fn fn, void *user)
{
    return nb_service_method_register(svc, method, fn, NULL, user, 0, 0, NULL);
}

int nb_service_add_method_with_options(nb_service_t *svc,
                          const char *method,
                          nb_handler_fn fn,
                          void *user,
                          const nb_service_method_options_t *options)
{
    int run_in_worker;
    const nb_service_executor_t *executor;
    int rc = nb_service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_service_method_register(svc, method, fn, NULL, user, 0, run_in_worker, executor);
}

int nb_service_add_method_worker(nb_service_t *svc, const char *method, nb_handler_fn fn, void *user)
{
    return nb_service_method_register(svc, method, fn, NULL, user, 0, 1, NULL);
}

static int nb_service_add_sync_ex(nb_service_t *svc,
                               const char *method,
                               nb_sync_handler_fn fn,
                               void *user,
                               const nb_service_method_options_t *options)
{
    nb_sync_bridge_t *bridge;
    int run_in_worker;
    const nb_service_executor_t *executor;
    int rc;
    if (!svc || !method || !method[0] || !fn) {
        return NB_ERR_INVALID;
    }
    rc = nb_service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != NB_OK) {
        return rc;
    }
    bridge = (nb_sync_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) {
        return NB_ERR_NOMEM;
    }
    bridge->fn = fn;
    bridge->user = user;
    rc = nb_service_method_register(
        svc, method, nb_sync_method_bridge, NULL, bridge, 1, run_in_worker, executor);
    if (rc != NB_OK) {
        free(bridge);
    }
    return rc;
}

int nb_service_add_sync_method(nb_service_t *svc, const char *method, nb_sync_handler_fn fn, void *user)
{
    return nb_service_add_sync_ex(svc, method, fn, user, NULL);
}

int nb_service_add_sync_method_with_options(nb_service_t *svc,
                               const char *method,
                               nb_sync_handler_fn fn,
                               void *user,
                               const nb_service_method_options_t *options)
{
    return nb_service_add_sync_ex(svc, method, fn, user, options);
}

int nb_service_add_sync_method_worker(nb_service_t *svc,
                                   const char *method,
                                   nb_sync_handler_fn fn,
                                   void *user)
{
    nb_service_method_options_t options = NB_SERVICE_METHOD_OPTIONS_INIT;
    options.exec = NB_SERVICE_EXEC_MODE_WORKER;
    return nb_service_add_sync_ex(svc, method, fn, user, &options);
}

int nb_service_add_stream_method(nb_service_t *svc, const char *method, nb_chunk_handler_fn fn, void *user)
{
    return nb_service_method_register(svc, method, NULL, fn, user, 0, 0, NULL);
}

int nb_service_add_stream_method_with_options(nb_service_t *svc,
                                 const char *method,
                                 nb_chunk_handler_fn fn,
                                 void *user,
                                 const nb_service_method_options_t *options)
{
    int run_in_worker;
    const nb_service_executor_t *executor;
    int rc = nb_service_parse_exec(options, 1, &run_in_worker, &executor);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_service_method_register(svc, method, NULL, fn, user, 0, run_in_worker, executor);
}

int nb_service_add_methods(nb_service_t *svc,
                           const nb_method_desc_t *methods,
                           size_t count)
{
    size_t i;

    if (!svc || (!methods && count > 0)) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < count; i++) {
        const nb_method_desc_t *m = &methods[i];
        int rc;

        if (!m->name || !m->name[0]) {
            return NB_ERR_INVALID;
        }
        switch (m->kind) {
        case NB_METHOD_KIND_UNARY:
            rc = m->handler.unary ?
                 nb_service_add_method_with_options(
                     svc, m->name, m->handler.unary, m->user, m->options) :
                 NB_ERR_INVALID;
            break;
        case NB_METHOD_KIND_SYNC:
            rc = m->handler.sync ?
                 nb_service_add_sync_method_with_options(
                     svc, m->name, m->handler.sync, m->user, m->options) :
                 NB_ERR_INVALID;
            break;
        case NB_METHOD_KIND_STREAM:
            rc = m->handler.stream ?
                 nb_service_add_stream_method_with_options(
                     svc, m->name, m->handler.stream, m->user, m->options) :
                 NB_ERR_INVALID;
            break;
        default:
            rc = NB_ERR_INVALID;
            break;
        }
        if (rc != NB_OK) {
            return rc;
        }
    }
    return NB_OK;
}
