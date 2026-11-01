#include <nb/buf.h>
#include <nb/error.h>

#include <thread/sem.h>
#include <thread/thread.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_COUNT 8
#define CHURN_ITERS 100000

static int g_fail;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                    \
        }                                                                  \
    } while (0)

enum worker_mode {
    WORKER_CHURN = 0,      /* ref + release pairs while owner keeps a ref */
    WORKER_RELEASE_ONCE = 1
};

typedef struct worker_ctx {
    nb_buf_t *buf;
    ntk_sem *start;
    int mode;
    int iters;
    int failed;
} worker_ctx_t;

static int g_freed;

static void counting_free(void *opaque, uint8_t *data)
{
    int *counter = (int *)opaque;

    (*counter)++;
    free(data);
}

static void worker_main(void *user)
{
    worker_ctx_t *ctx = (worker_ctx_t *)user;
    int i;

    ntk_sem_wait(ctx->start);
    if (ctx->mode == WORKER_CHURN) {
        for (i = 0; i < ctx->iters; i++) {
            nb_buf_t *ref = nb_buf_ref(ctx->buf);
            if (!ref) {
                ctx->failed = 1;
                return;
            }
            nb_buf_release(ref);
        }
        return;
    }
    nb_buf_release(ctx->buf);
}

/*
 * Start all workers behind a semaphore gate, then release them together.
 * Returns non-zero when every worker thread ran to completion.
 */
static int run_workers(nb_buf_t *buf, int mode, int iters)
{
    ntk_thread *threads[WORKER_COUNT];
    worker_ctx_t ctx[WORKER_COUNT];
    ntk_sem *start;
    int created = 0;
    int failed = 0;
    int i;

    memset(threads, 0, sizeof(threads));
    memset(ctx, 0, sizeof(ctx));
    start = ntk_sem_create(0);
    if (!start) {
        return 0;
    }

    for (i = 0; i < WORKER_COUNT; i++) {
        ctx[i].buf = buf;
        ctx[i].start = start;
        ctx[i].mode = mode;
        ctx[i].iters = iters;
        threads[i] = ntk_thread_create(worker_main, &ctx[i]);
        if (!threads[i]) {
            failed = 1;
            break;
        }
        created++;
    }

    if (created > 0) {
        ntk_sem_post(start, (unsigned int)created);
    }
    for (i = 0; i < created; i++) {
        ntk_thread_join(threads[i]);
        ntk_thread_destroy(threads[i]);
    }
    ntk_sem_destroy(start);

    if (created != WORKER_COUNT) {
        failed = 1;
    }
    for (i = 0; i < created; i++) {
        if (ctx[i].failed) {
            failed = 1;
        }
    }
    return !failed;
}

/* Owner keeps one base reference while workers churn ref/release pairs. */
static void test_ref_release_churn(void)
{
    uint8_t *mem = (uint8_t *)malloc(64);
    nb_buf_t *buf;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    g_freed = 0;
    buf = nb_buf_create_external(mem, 64, counting_free, &g_freed, 0);
    CHECK(buf != NULL);
    if (!buf) {
        free(mem);
        return;
    }
    CHECK(nb_buf_share(buf) == NB_OK);
    CHECK(nb_buf_is_shared(buf));

    CHECK(run_workers(buf, WORKER_CHURN, CHURN_ITERS));
    CHECK(g_freed == 0); /* owner reference keeps the buffer alive */
    CHECK(nb_buf_data(buf) == mem);

    nb_buf_release(buf);
    CHECK(g_freed == 1);
}

/* ref_n(N) then N concurrent releases; the owner still releases last. */
static void test_ref_n_release_last(void)
{
    uint8_t *mem = (uint8_t *)malloc(64);
    nb_buf_t *buf;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    g_freed = 0;
    buf = nb_buf_create_external(mem, 64, counting_free, &g_freed, 0);
    CHECK(buf != NULL);
    if (!buf) {
        free(mem);
        return;
    }
    CHECK(nb_buf_share(buf) == NB_OK);
    CHECK(nb_buf_ref_n(buf, WORKER_COUNT) == buf);

    /* Total refs = 1 owner + WORKER_COUNT; workers drop their share first. */
    CHECK(run_workers(buf, WORKER_RELEASE_ONCE, 0));
    CHECK(g_freed == 0);

    nb_buf_release(buf);
    CHECK(g_freed == 1);
}

/* Concurrent final release on a pooled buffer must return the slot once. */
static void test_pooled_concurrent_release(void)
{
    nb_buf_pool_config_t config;
    nb_buf_pool_stats_t stats;
    nb_buf_pool_t *pool;
    nb_buf_t *buf;

    memset(&config, 0, sizeof(config));
    config.slot_size = 64;
    config.slot_count = 1;
    pool = nb_buf_pool_create(&config);
    CHECK(pool != NULL);
    if (!pool) {
        return;
    }

    buf = nb_buf_acquire(pool);
    CHECK(buf != NULL);
    if (!buf) {
        nb_buf_pool_destroy(pool);
        return;
    }
    CHECK(nb_buf_share(buf) == NB_OK);
    CHECK(nb_buf_ref_n(buf, WORKER_COUNT) == buf);

    /* Owner drops its ref first, workers race to the last reference. */
    nb_buf_release(buf);
    CHECK(run_workers(buf, WORKER_RELEASE_ONCE, 0));

    memset(&stats, 0, sizeof(stats));
    nb_buf_pool_get_stats(pool, &stats);
    CHECK(stats.outstanding == 0);
    CHECK(stats.cached == 1);
    CHECK(stats.created == 1);

    nb_buf_pool_destroy(pool);
}

int main(void)
{
    test_ref_release_churn();
    test_ref_n_release_last();
    test_pooled_concurrent_release();

    if (g_fail) {
        fprintf(stderr, "BUF CONCURRENCY FAIL\n");
    } else {
        printf("BUF CONCURRENCY NB_OK\n");
    }
    return g_fail ? 1 : 0;
}
