#include <nb/slotpool.h>
#include <nb/buf.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                    \
        }                                                                  \
    } while (0)

static void test_basic(void)
{
    nb_slotpool_config_t cfg;
    nb_slotpool_t *pool;
    nb_slot_id_t a;
    nb_slot_id_t b;
    nb_slot_id_t c;
    nb_buf_t *buf;

    memset(&cfg, 0, sizeof(cfg));
    cfg.payload_size = 64;
    cfg.slot_count = 2;
    pool = nb_slotpool_create(&cfg);
    CHECK(pool != NULL);
    if (!pool) {
        return;
    }

    a = nb_slotpool_alloc(pool);
    b = nb_slotpool_alloc(pool);
    CHECK(a != NB_SLOT_ID_NONE);
    CHECK(b != NB_SLOT_ID_NONE);
    CHECK(a != b);

    buf = nb_slotpool_get(pool, a);
    CHECK(buf != NULL);
    CHECK(nb_buf_capacity(buf) >= 64u);
    memset(nb_buf_data(buf), 0xAB, 16);
    CHECK(nb_buf_data(nb_slotpool_get(pool, a))[0] == 0xAB);

    c = nb_slotpool_alloc(pool); /* exhausted */
    CHECK(c == NB_SLOT_ID_NONE);

    nb_slotpool_destroy(pool);
}

static void test_stale_and_double_free(void)
{
    nb_slotpool_config_t cfg;
    nb_slotpool_t *pool;
    nb_slot_id_t a;
    nb_slot_id_t b;

    memset(&cfg, 0, sizeof(cfg));
    cfg.payload_size = 32;
    cfg.slot_count = 1;
    pool = nb_slotpool_create(&cfg);
    CHECK(pool != NULL);
    if (!pool) {
        return;
    }

    a = nb_slotpool_alloc(pool);
    CHECK(a != NB_SLOT_ID_NONE);
    nb_slotpool_free(pool, a);

    /* stale ID must not resolve */
    CHECK(nb_slotpool_get(pool, a) == NULL);
    /* double free ignored */
    nb_slotpool_free(pool, a);

    b = nb_slotpool_alloc(pool);
    CHECK(b != NB_SLOT_ID_NONE);
    CHECK(b != a); /* generation advanced */
    CHECK(nb_slotpool_get(pool, b) != NULL);
    CHECK(nb_slotpool_get(pool, a) == NULL);

    nb_slotpool_destroy(pool);
}

static void test_batch_and_invalid(void)
{
    nb_slotpool_config_t cfg;
    nb_slotpool_t *pool;
    nb_slot_id_t ids[3];
    int i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.payload_size = 16;
    cfg.slot_count = 3;
    pool = nb_slotpool_create(&cfg);
    CHECK(pool != NULL);
    if (!pool) {
        return;
    }
    for (i = 0; i < 3; i++) {
        ids[i] = nb_slotpool_alloc(pool);
        CHECK(ids[i] != NB_SLOT_ID_NONE);
    }
    nb_slotpool_free_batch(pool, ids, 3);
    for (i = 0; i < 3; i++) {
        CHECK(nb_slotpool_alloc(pool) != NB_SLOT_ID_NONE);
    }
    CHECK(nb_slotpool_get(pool, NB_SLOT_ID_NONE) == NULL);
    CHECK(nb_slotpool_get(pool, 0xffffffffu) == NULL);
    nb_slotpool_free(pool, NB_SLOT_ID_NONE); /* no-op */
    nb_slotpool_destroy(pool);

    /* invalid configs */
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nb_slotpool_create(&cfg) == NULL);
}

int main(void)
{
    test_basic();
    test_stale_and_double_free();
    test_batch_and_invalid();
    if (g_fail) {
        fprintf(stderr, "SLOTPOOL FAIL\n");
    } else {
        printf("SLOTPOOL NB_OK\n");
    }
    return g_fail ? 1 : 0;
}
