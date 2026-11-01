#include <nb/buf.h>
#include <nb/alloc.h>
#include <nb/error.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static int g_external_freed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void external_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    g_external_freed++;
    free(data);
}

static void *failing_alloc(size_t size, void *opaque)
{
    (void)size;
    (void)opaque;
    return NULL;
}

static void failing_free(void *ptr, void *opaque)
{
    (void)opaque;
    free(ptr);
}

static void test_unique_and_move(void)
{
    nb_buf_t *buf = nb_buf_alloc(32);
    nb_buf_t *moved;

    CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    CHECK(nb_buf_size(buf) == 32);
    CHECK(nb_buf_capacity(buf) == 32);
    CHECK(nb_buf_data(buf) != NULL);
    memset(nb_buf_data(buf), 0x5a, nb_buf_size(buf));
    CHECK(nb_buf_set_size(buf, 8) == NB_OK);
    CHECK(nb_buf_size(buf) == 8);
    CHECK(nb_buf_set_size(buf, 33) == NB_ERR_INVALID);
    CHECK(!nb_buf_is_shared(buf));
    CHECK(nb_buf_ref(buf) == NULL);

    moved = nb_buf_move(&buf);
    CHECK(moved != NULL);
    CHECK(buf == NULL);
    CHECK(nb_buf_data(moved)[0] == 0x5a);
    nb_buf_release(moved);
}

static void test_share(void)
{
    nb_buf_t *buf = nb_buf_alloc(16);
    nb_buf_t *r1;
    nb_buf_t *r2;

    CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    CHECK(nb_buf_share(buf) == NB_OK);
    CHECK(nb_buf_share(buf) == NB_ERR_ALREADY);
    CHECK(nb_buf_is_shared(buf));
    r1 = nb_buf_ref(buf);
    r2 = nb_buf_ref_n(buf, 2);
    CHECK(r1 == buf);
    CHECK(r2 == buf);
    nb_buf_release(buf);
    nb_buf_release(r1);
    /* ref_n(buf, 2) added two references; both must be released. */
    nb_buf_release(r2);
    nb_buf_release(r2);
}

static void test_share_refcount_lifetime(void)
{
    uint8_t *mem = (uint8_t *)malloc(24);
    nb_buf_t *buf;
    nb_buf_t *ref;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    g_external_freed = 0;
    buf = nb_buf_create_external(mem, 24, external_free, NULL, 0);
    CHECK(buf != NULL);
    if (!buf) {
        free(mem);
        return;
    }
    CHECK(nb_buf_share(buf) == NB_OK);
    ref = nb_buf_ref(buf);
    CHECK(ref == buf);
    nb_buf_release(buf); /* owner gone; ref still keeps the buffer alive */
    CHECK(g_external_freed == 0);
    if (g_external_freed != 0) {
        return; /* premature free: avoid touching released memory */
    }
    CHECK(nb_buf_size(ref) == 24);
    nb_buf_release(ref);
    CHECK(g_external_freed == 1);
}

static void test_external(void)
{
    uint8_t *mem = (uint8_t *)malloc(24);
    nb_buf_t *buf;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    g_external_freed = 0;
    buf = nb_buf_create_external(mem, 24, external_free, NULL, 0);
    CHECK(buf != NULL);
    CHECK(nb_buf_data(buf) == mem);
    CHECK(nb_buf_size(buf) == 24);
    CHECK(nb_buf_flags(buf) & NB_BUF_FLAG_EXTERNAL);
    nb_buf_release(buf);
    CHECK(g_external_freed == 1);
}

static void test_slice(void)
{
    nb_buf_t *buf = nb_buf_alloc(16);
    nb_buf_view_t view;

    CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    memset(nb_buf_data(buf), 0x11, 16);
    CHECK(nb_buf_slice(buf, 4, 8, &view) == NB_OK);
    CHECK(view.owner == buf);
    CHECK(view.data == nb_buf_data(buf) + 4);
    CHECK(view.size == 8);
    CHECK(nb_buf_slice(buf, 12, 8, &view) == NB_ERR_INVALID);
    CHECK(nb_buf_slice(buf, 17, 0, &view) == NB_ERR_INVALID);
    nb_buf_release(buf);
}

static void test_aligned(void)
{
    nb_buf_t *buf = nb_buf_alloc_aligned(64, 64);

    CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    CHECK(((uintptr_t)nb_buf_data(buf) & 63u) == 0);
    nb_buf_release(buf);
}

static void test_pool(void)
{
    nb_buf_pool_config_t config;
    nb_buf_pool_stats_t stats;
    nb_buf_pool_t *pool;
    nb_buf_t *a;
    nb_buf_t *b;
    nb_buf_t *c;

    memset(&config, 0, sizeof(config));
    config.slot_size = 32;
    config.slot_count = 2;
    pool = nb_buf_pool_create(&config);
    CHECK(pool != NULL);
    if (!pool) {
        return;
    }
    a = nb_buf_acquire(pool);
    b = nb_buf_acquire(pool);
    c = nb_buf_acquire(pool);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(c == NULL); /* slot_count 是硬上限 */
    if (a) {
        nb_buf_release(a);
    }
    if (b) {
        nb_buf_release(b);
    }
    nb_buf_pool_get_stats(pool, &stats);
    CHECK(stats.created == 2);
    CHECK(stats.outstanding == 0);
    CHECK(stats.cached == 2);
    CHECK(stats.hits >= 2);
    nb_buf_pool_destroy(pool);
}

static void test_pool_destroy_outstanding(void)
{
    nb_buf_pool_config_t config;
    nb_buf_pool_t *pool;
    nb_buf_t *buf;

    memset(&config, 0, sizeof(config));
    config.slot_size = 16;
    config.slot_count = 1;
    pool = nb_buf_pool_create(&config);
    CHECK(pool != NULL);
    if (!pool) {
        return;
    }
    buf = nb_buf_acquire(pool);
    CHECK(buf != NULL);
    nb_buf_pool_destroy(pool);
    if (buf) {
        nb_buf_release(buf); /* pool 已销毁，buffer 应直接释放 */
    }
}

static void test_oom(void)
{
    nb_allocator_t ops;
    nb_buf_t *buf;

    memset(&ops, 0, sizeof(ops));
    ops.alloc = failing_alloc;
    ops.free = failing_free;
    CHECK(nb_mem_set_allocator(&ops) == NB_OK);
    buf = nb_buf_alloc(16);
    CHECK(buf == NULL);
    CHECK(nb_mem_set_allocator(NULL) == NB_OK);
    if (buf) {
        nb_buf_release(buf);
    }
}

int main(void)
{
    test_unique_and_move();
    test_share();
    test_share_refcount_lifetime();
    test_external();
    test_slice();
    test_aligned();
    test_pool();
    test_pool_destroy_outstanding();
    test_oom();
    if (g_fail) {
        fprintf(stderr, "BUFFER FAIL\n");
    } else {
        printf("BUFFER NB_OK\n");
    }
    return g_fail ? 1 : 0;
}
