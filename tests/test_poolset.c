#include <nb/poolset.h>

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

int main(void)
{
    static const size_t classes[] = { 64u, 128u };
    static const size_t counts[] = { 2u, 1u };
    nb_poolset_t *set;
    nb_buf_t *a;
    nb_buf_t *b;
    nb_buf_t *c;
    nb_buf_t *d;

    set = nb_poolset_create(classes, counts, 2u);
    CHECK(set != NULL);
    if (!set) {
        return 1;
    }

    a = nb_poolset_acquire(set, 40u); /* smallest class 64 */
    b = nb_poolset_acquire(set, 40u);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(nb_buf_capacity(a) >= 40u && nb_buf_capacity(a) <= 64u);

    c = nb_poolset_acquire(set, 40u); /* 64-class exhausted -> 128-class */
    CHECK(c != NULL);
    CHECK(nb_buf_capacity(c) >= 40u);

    d = nb_poolset_acquire(set, 40u); /* both classes exhausted */
    CHECK(d == NULL);

    CHECK(nb_poolset_acquire(set, 256u) == NULL); /* no class fits */

    if (a) {
        nb_buf_release(a);
    }
    d = nb_poolset_acquire(set, 40u); /* released 64-class slot reused */
    CHECK(d != NULL);

    nb_buf_release(b);
    nb_buf_release(c);
    nb_buf_release(d);
    nb_poolset_destroy(set);

    if (g_fail) {
        fprintf(stderr, "POOLSET FAIL\n");
    } else {
        printf("POOLSET NB_OK\n");
    }
    return g_fail ? 1 : 0;
}
