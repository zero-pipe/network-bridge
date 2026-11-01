#include <nb/alloc.h>
#include "atomic.h"

#include <nb/error.h>

#include <stdlib.h>
#include <string.h>

#define NB_MEM_MAGIC 0x4e424d48u /* 'NBMH' */

typedef union nb_mem_align {
    void *p;
    long double ld;
    long long ll;
} nb_mem_align_t;

typedef struct nb_mem_hdr {
    nb_mem_align_t align;
    void *base;
    size_t size;
    uint32_t magic;
} nb_mem_hdr_t;

static nb_allocator_t g_allocator;
static int g_allocator_set;

static nb_mem_stats_t g_stats;

static void *nb_default_alloc(size_t size, void *opaque)
{
    (void)opaque;
    return malloc(size);
}

static void nb_default_free(void *ptr, void *opaque)
{
    (void)opaque;
    free(ptr);
}

static void *nb_raw_alloc(size_t size)
{
    if (g_allocator_set) {
        return g_allocator.alloc(size, g_allocator.opaque);
    }
    return nb_default_alloc(size, NULL);
}

static void nb_raw_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    if (g_allocator_set) {
        g_allocator.free(ptr, g_allocator.opaque);
    } else {
        nb_default_free(ptr, NULL);
    }
}

static void nb_mem_note_alloc(size_t size)
{
    uint64_t cur;
    uint64_t peak;

    nb_atomic_add_u64(&g_stats.alloc_count, 1u);
    nb_atomic_add_u64(&g_stats.outstanding_bytes, (uint64_t)size);
    cur = nb_atomic_load_u64(&g_stats.outstanding_bytes);
    for (;;) {
        peak = nb_atomic_load_u64(&g_stats.peak_bytes);
        if (cur <= peak || nb_atomic_cas_u64(&g_stats.peak_bytes, &peak, cur)) {
            break;
        }
    }
}

static void nb_mem_note_free(size_t size)
{
    nb_atomic_add_u64(&g_stats.free_count, 1u);
    nb_atomic_sub_u64(&g_stats.outstanding_bytes, (uint64_t)size);
}

int nb_mem_set_allocator(const nb_allocator_t *allocator)
{
    if (!allocator) {
        memset(&g_allocator, 0, sizeof(g_allocator));
        g_allocator_set = 0;
        return NB_OK;
    }
    if (!allocator->alloc || !allocator->free) {
        return NB_ERR_INVALID;
    }
    g_allocator = *allocator;
    g_allocator_set = 1;
    return NB_OK;
}

void *nb_malloc(size_t size)
{
    nb_mem_hdr_t *hdr;
    void *raw;
    size_t total;

    if (size == 0 || size > (size_t)-1 - sizeof(nb_mem_hdr_t)) {
        return NULL;
    }
    total = sizeof(nb_mem_hdr_t) + size;
    raw = nb_raw_alloc(total);
    if (!raw) {
        nb_atomic_add_u64(&g_stats.failed_alloc_count, 1u);
        return NULL;
    }
    hdr = (nb_mem_hdr_t *)raw;
    hdr->base = raw;
    hdr->size = size;
    hdr->magic = NB_MEM_MAGIC;
    nb_mem_note_alloc(size);
    return (void *)((uint8_t *)hdr + sizeof(*hdr));
}

void *nb_malloc0(size_t size)
{
    void *ptr = nb_malloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *nb_malloc_aligned(size_t size, size_t alignment)
{
    nb_mem_hdr_t *hdr;
    void *raw;
    uintptr_t raw_addr;
    uintptr_t data_addr;
    size_t align = alignment ? alignment : NB_MEM_ALIGN_DEFAULT;
    size_t total;

    if (size == 0 || (align & (align - 1)) != 0) {
        return NULL;
    }
    if (align < NB_MEM_ALIGN_DEFAULT) {
        align = NB_MEM_ALIGN_DEFAULT;
    }
    if (size > (size_t)-1 - sizeof(nb_mem_hdr_t) - (align - 1)) {
        return NULL;
    }
    total = sizeof(nb_mem_hdr_t) + (align - 1) + size;
    raw = nb_raw_alloc(total);
    if (!raw) {
        nb_atomic_add_u64(&g_stats.failed_alloc_count, 1u);
        return NULL;
    }
    raw_addr = (uintptr_t)raw;
    data_addr = (raw_addr + sizeof(nb_mem_hdr_t) + align - 1) & ~(uintptr_t)(align - 1);
    hdr = (nb_mem_hdr_t *)(data_addr - sizeof(nb_mem_hdr_t));
    hdr->base = raw;
    hdr->size = size;
    hdr->magic = NB_MEM_MAGIC;
    nb_mem_note_alloc(size);
    return (void *)data_addr;
}

void *nb_realloc(void *ptr, size_t size)
{
    nb_mem_hdr_t *hdr;
    void *out;
    size_t old_size;
    size_t copy_size;

    if (!ptr) {
        return nb_malloc(size);
    }
    if (size == 0) {
        nb_free(ptr);
        return NULL;
    }
    hdr = (nb_mem_hdr_t *)((uint8_t *)ptr - sizeof(nb_mem_hdr_t));
    old_size = hdr->size;
    out = nb_malloc(size);
    if (!out) {
        return NULL;
    }
    copy_size = old_size < size ? old_size : size;
    memcpy(out, ptr, copy_size);
    nb_free(ptr);
    return out;
}

void nb_free(void *ptr)
{
    nb_mem_hdr_t *hdr;
    void *base;
    size_t size;

    if (!ptr) {
        return;
    }
    hdr = (nb_mem_hdr_t *)((uint8_t *)ptr - sizeof(nb_mem_hdr_t));
    base = hdr->base;
    size = hdr->size;
    hdr->magic = 0;
    nb_mem_note_free(size);
    nb_raw_free(base);
}

void nb_mem_get_stats(nb_mem_stats_t *out)
{
    if (!out) {
        return;
    }
    out->alloc_count = nb_atomic_load_u64(&g_stats.alloc_count);
    out->free_count = nb_atomic_load_u64(&g_stats.free_count);
    out->failed_alloc_count = nb_atomic_load_u64(&g_stats.failed_alloc_count);
    out->outstanding_bytes = nb_atomic_load_u64(&g_stats.outstanding_bytes);
    out->peak_bytes = nb_atomic_load_u64(&g_stats.peak_bytes);
}

void nb_mem_reset_stats(void)
{
    uint64_t outstanding = nb_atomic_load_u64(&g_stats.outstanding_bytes);
    nb_atomic_store_u64(&g_stats.alloc_count, 0);
    nb_atomic_store_u64(&g_stats.free_count, 0);
    nb_atomic_store_u64(&g_stats.failed_alloc_count, 0);
    nb_atomic_store_u64(&g_stats.peak_bytes, outstanding);
}
