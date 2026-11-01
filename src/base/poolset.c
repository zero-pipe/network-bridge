#include <nb/poolset.h>

#include <string.h>

struct nb_poolset {
    size_t class_count;
    size_t *slot_sizes;
    nb_buf_pool_t **pools;
};

nb_poolset_t *nb_poolset_create(const size_t *slot_sizes,
                                const size_t *slot_counts,
                                size_t class_count)
{
    nb_poolset_t *set;
    size_t i;

    if (!slot_sizes || !slot_counts || class_count == 0) {
        return NULL;
    }
    set = (nb_poolset_t *)nb_malloc(sizeof(*set));
    if (!set) {
        return NULL;
    }
    memset(set, 0, sizeof(*set));
    set->slot_sizes = (size_t *)nb_malloc(class_count * sizeof(size_t));
    set->pools = (nb_buf_pool_t **)nb_malloc(class_count * sizeof(nb_buf_pool_t *));
    if (!set->slot_sizes || !set->pools) {
        nb_poolset_destroy(set);
        return NULL;
    }
    memset(set->pools, 0, class_count * sizeof(nb_buf_pool_t *));
    set->class_count = class_count;
    for (i = 0; i < class_count; i++) {
        nb_buf_pool_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        set->slot_sizes[i] = slot_sizes[i];
        cfg.slot_size = slot_sizes[i];
        cfg.slot_count = slot_counts[i];
        cfg.max_cached = slot_counts[i];
        cfg.flags = NB_BUF_POOL_FLAG_LAZY;
        /* 单个分档创建失败不致命：acquire 会跳过它并回退到更大分档。 */
        set->pools[i] = nb_buf_pool_create(&cfg);
    }
    return set;
}

nb_buf_t *nb_poolset_acquire(nb_poolset_t *set, size_t size)
{
    size_t i;

    if (!set || size == 0) {
        return NULL;
    }
    for (i = 0; i < set->class_count; i++) {
        nb_buf_t *buf;
        if (set->slot_sizes[i] < size || !set->pools[i]) {
            continue;
        }
        buf = nb_buf_acquire(set->pools[i]);
        if (buf) {
            return buf;
        }
        /* 分档耗尽时尝试更大分档，避免回退 malloc。 */
    }
    return NULL;
}

void nb_poolset_get_stats(const nb_poolset_t *set, nb_buf_pool_stats_t *out)
{
    size_t i;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!set) {
        return;
    }
    for (i = 0; i < set->class_count; i++) {
        nb_buf_pool_stats_t s;
        if (!set->pools[i]) {
            continue;
        }
        nb_buf_pool_get_stats(set->pools[i], &s);
        out->hits += s.hits;
        out->misses += s.misses;
        out->failed += s.failed;
        out->outstanding += s.outstanding;
        out->cached += s.cached;
        out->created += s.created;
    }
}

void nb_poolset_destroy(nb_poolset_t *set)
{
    nb_poolset_t *p;
    size_t i;

    if (!set) {
        return;
    }
    p = set;
    for (i = 0; i < p->class_count; i++) {
        if (p->pools && p->pools[i]) {
            nb_buf_pool_destroy(p->pools[i]);
        }
    }
    nb_free(p->pools);
    nb_free(p->slot_sizes);
    nb_free(p);
}
