#include <nb/slotpool.h>

#include "atomic.h"

#include <string.h>

/*
 * slot ID 布局：高 16 位 generation，低 16 位 index+1（保证有效 ID 非 0）。
 * slot 索引上限 65535。
 */
#define NB_SLOT_IDX_MASK 0xffffu

struct nb_slotpool {
    size_t slot_count;
    size_t payload_size;
    nb_buf_t **slots;
    uint32_t *gens;
    uint8_t *in_use;
    uint32_t *free_ids; /* 空闲 slot 索引栈 */
    size_t free_top;
    nb_spinlock_t lock;
};

static nb_slot_id_t nb_make_slot_id(uint32_t gen, uint32_t idx)
{
    return (gen << 16) | (idx + 1u);
}

static uint32_t nb_slot_id_gen(nb_slot_id_t id)
{
    return id >> 16;
}

static uint32_t nb_slot_id_idx(nb_slot_id_t id)
{
    return (id & NB_SLOT_IDX_MASK) - 1u;
}

nb_slotpool_t *nb_slotpool_create(const nb_slotpool_config_t *config)
{
    nb_slotpool_t *pool;
    size_t i;

    if (!config || config->payload_size == 0 || config->slot_count == 0 ||
        config->slot_count > NB_SLOT_IDX_MASK) {
        return NULL;
    }
    pool = (nb_slotpool_t *)nb_malloc(sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    memset(pool, 0, sizeof(*pool));
    pool->slot_count = config->slot_count;
    pool->payload_size = config->payload_size;
    pool->slots = (nb_buf_t **)nb_malloc(pool->slot_count * sizeof(nb_buf_t *));
    pool->gens = (uint32_t *)nb_malloc(pool->slot_count * sizeof(uint32_t));
    pool->in_use = (uint8_t *)nb_malloc(pool->slot_count);
    pool->free_ids = (uint32_t *)nb_malloc(pool->slot_count * sizeof(uint32_t));
    if (!pool->slots || !pool->gens || !pool->in_use || !pool->free_ids) {
        nb_slotpool_destroy(pool);
        return NULL;
    }
    memset(pool->slots, 0, pool->slot_count * sizeof(nb_buf_t *));
    memset(pool->in_use, 0, pool->slot_count);
    for (i = 0; i < pool->slot_count; i++) {
        pool->slots[i] = nb_buf_alloc(pool->payload_size);
        if (!pool->slots[i]) {
            nb_slotpool_destroy(pool);
            return NULL;
        }
        pool->gens[i] = 1;
        pool->free_ids[i] = (uint32_t)i;
    }
    pool->free_top = pool->slot_count;
    nb_spinlock_init(&pool->lock);
    return pool;
}

nb_slot_id_t nb_slotpool_alloc(nb_slotpool_t *pool)
{
    nb_slot_id_t id;
    uint32_t idx;

    if (!pool) {
        return NB_SLOT_ID_NONE;
    }
    nb_spinlock_lock(&pool->lock);
    if (pool->free_top == 0) {
        nb_spinlock_unlock(&pool->lock);
        return NB_SLOT_ID_NONE;
    }
    idx = pool->free_ids[--pool->free_top];
    pool->in_use[idx] = 1;
    id = nb_make_slot_id(pool->gens[idx], idx);
    nb_spinlock_unlock(&pool->lock);
    return id;
}

nb_buf_t *nb_slotpool_get(nb_slotpool_t *pool, nb_slot_id_t id)
{
    nb_buf_t *buf = NULL;
    uint32_t idx;

    if (!pool || id == NB_SLOT_ID_NONE) {
        return NULL;
    }
    idx = nb_slot_id_idx(id);
    nb_spinlock_lock(&pool->lock);
    if (idx < pool->slot_count && pool->in_use[idx] && pool->gens[idx] == nb_slot_id_gen(id)) {
        buf = pool->slots[idx];
    }
    nb_spinlock_unlock(&pool->lock);
    return buf;
}

void nb_slotpool_free(nb_slotpool_t *pool, nb_slot_id_t id)
{
    uint32_t idx;

    if (!pool || id == NB_SLOT_ID_NONE) {
        return;
    }
    idx = nb_slot_id_idx(id);
    nb_spinlock_lock(&pool->lock);
    if (idx < pool->slot_count && pool->in_use[idx] && pool->gens[idx] == nb_slot_id_gen(id)) {
        uint32_t g = (pool->gens[idx] + 1u) & NB_SLOT_IDX_MASK;
        if (g == 0u) {
            g = 1u; /* skip 0 so a fresh ID is never NONE */
        }
        pool->gens[idx] = g;
        pool->in_use[idx] = 0;
        pool->free_ids[pool->free_top++] = idx;
    }
    nb_spinlock_unlock(&pool->lock);
}

void nb_slotpool_free_batch(nb_slotpool_t *pool, const nb_slot_id_t *ids, size_t count)
{
    size_t i;

    if (!pool || !ids) {
        return;
    }
    for (i = 0; i < count; i++) {
        nb_slotpool_free(pool, ids[i]);
    }
}

void nb_slotpool_destroy(nb_slotpool_t *pool)
{
    nb_slotpool_t *p;
    size_t i;

    if (!pool) {
        return;
    }
    p = pool;
    if (p->slots) {
        for (i = 0; i < p->slot_count; i++) {
            if (p->slots[i]) {
                nb_buf_release(p->slots[i]);
            }
        }
    }
    nb_free(p->slots);
    nb_free(p->gens);
    nb_free(p->in_use);
    nb_free(p->free_ids);
    nb_free(p);
}
