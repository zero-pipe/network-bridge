#ifndef NB_BASE_ATOMIC_H
#define NB_BASE_ATOMIC_H

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>

static inline int32_t nb_atomic_ref_inc(int32_t *p)
{
    return (int32_t)_InterlockedIncrement((volatile long *)p);
}

static inline int32_t nb_atomic_ref_dec(int32_t *p)
{
    return (int32_t)_InterlockedDecrement((volatile long *)p);
}

static inline int32_t nb_atomic_ref_add(int32_t *p, int32_t v)
{
    return (int32_t)(_InterlockedExchangeAdd((volatile long *)p, (long)v) + (long)v);
}

static inline void nb_atomic_add_u64(uint64_t *p, uint64_t v)
{
    _InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v);
}

static inline void nb_atomic_sub_u64(uint64_t *p, uint64_t v)
{
    _InterlockedExchangeAdd64((volatile __int64 *)p, -(__int64)v);
}

static inline uint64_t nb_atomic_load_u64(const uint64_t *p)
{
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, 0, 0);
}

static inline void nb_atomic_store_u64(uint64_t *p, uint64_t v)
{
    _InterlockedExchange64((volatile __int64 *)p, (__int64)v);
}

static inline int nb_atomic_cas_u64(uint64_t *p, uint64_t *expected, uint64_t desired)
{
    __int64 cur = _InterlockedCompareExchange64(
        (volatile __int64 *)p, (__int64)desired, (__int64)*expected);
    if ((uint64_t)cur == *expected) {
        return 1;
    }
    *expected = (uint64_t)cur;
    return 0;
}

static inline int nb_atomic_test_and_set(volatile int *p)
{
    return (int)_InterlockedExchange((volatile long *)p, 1);
}

static inline void nb_atomic_clear(volatile int *p)
{
    _InterlockedExchange((volatile long *)p, 0);
}

static inline void nb_atomic_add_u32(uint32_t *p, uint32_t v)
{
    _InterlockedExchangeAdd((volatile long *)p, (long)v);
}

static inline uint32_t nb_atomic_fetch_add_u32(uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v);
}

static inline uint32_t nb_atomic_sub_u32(uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, -(long)v);
}

static inline uint32_t nb_atomic_exchange_u32(uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v);
}

static inline uint32_t nb_atomic_load_u32(const uint32_t *p)
{
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
}

static inline void nb_atomic_store_u32(uint32_t *p, uint32_t v)
{
    _InterlockedExchange((volatile long *)p, (long)v);
}

/* acquire/release 版本用于一次性初始化：release 发布写好的数据，acquire 读取。 */
static inline uint32_t nb_atomic_load_acquire_u32(const uint32_t *p)
{
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
}

static inline void nb_atomic_store_release_u32(uint32_t *p, uint32_t v)
{
    _InterlockedExchange((volatile long *)p, (long)v);
}

static inline int nb_atomic_cas_u32(uint32_t *p, uint32_t *expected, uint32_t desired)
{
    long cur = _InterlockedCompareExchange((volatile long *)p, (long)desired, (long)*expected);
    if ((uint32_t)cur == *expected) {
        return 1;
    }
    *expected = (uint32_t)cur;
    return 0;
}

#else

static inline int32_t nb_atomic_ref_inc(int32_t *p)
{
    return __atomic_add_fetch(p, 1, __ATOMIC_ACQ_REL);
}

static inline int32_t nb_atomic_ref_dec(int32_t *p)
{
    return __atomic_sub_fetch(p, 1, __ATOMIC_ACQ_REL);
}

static inline int32_t nb_atomic_ref_add(int32_t *p, int32_t v)
{
    return __atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);
}

static inline void nb_atomic_add_u64(uint64_t *p, uint64_t v)
{
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline void nb_atomic_sub_u64(uint64_t *p, uint64_t v)
{
    __atomic_fetch_sub(p, v, __ATOMIC_RELAXED);
}

static inline uint64_t nb_atomic_load_u64(const uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void nb_atomic_store_u64(uint64_t *p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

static inline int nb_atomic_cas_u64(uint64_t *p, uint64_t *expected, uint64_t desired)
{
    return __atomic_compare_exchange_n(
        p, expected, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static inline int nb_atomic_test_and_set(volatile int *p)
{
    return __atomic_test_and_set(p, __ATOMIC_ACQUIRE);
}

static inline void nb_atomic_clear(volatile int *p)
{
    __atomic_clear(p, __ATOMIC_RELEASE);
}

static inline void nb_atomic_add_u32(uint32_t *p, uint32_t v)
{
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline uint32_t nb_atomic_fetch_add_u32(uint32_t *p, uint32_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline uint32_t nb_atomic_sub_u32(uint32_t *p, uint32_t v)
{
    return __atomic_fetch_sub(p, v, __ATOMIC_RELAXED);
}

static inline uint32_t nb_atomic_exchange_u32(uint32_t *p, uint32_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_RELAXED);
}

static inline uint32_t nb_atomic_load_u32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void nb_atomic_store_u32(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

/* acquire/release 版本用于一次性初始化：release 发布写好的数据，acquire 读取。 */
static inline uint32_t nb_atomic_load_acquire_u32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void nb_atomic_store_release_u32(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline int nb_atomic_cas_u32(uint32_t *p, uint32_t *expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, expected, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

#endif

/*
 * 三态一次性初始化：0 = 未初始化，1 = 初始化中，2 = 就绪。
 * 返回 1 表示调用方取得初始化权，完成后必须调用 nb_atomic_once_end()；
 * 返回 0 表示初始化已完成（本线程或其它线程完成）。
 */
static inline int nb_atomic_once_begin(uint32_t *state)
{
    uint32_t expected = 0u;

    if (nb_atomic_load_acquire_u32(state) == 2u) {
        return 0;
    }
    if (nb_atomic_cas_u32(state, &expected, 1u)) {
        return 1;
    }
    while (nb_atomic_load_acquire_u32(state) != 2u) {
        /* 另一个线程正在初始化。 */
    }
    return 0;
}

static inline void nb_atomic_once_end(uint32_t *state)
{
    nb_atomic_store_release_u32(state, 2u);
}

typedef struct nb_spinlock {
    volatile int lock;
} nb_spinlock_t;

static inline void nb_spinlock_init(nb_spinlock_t *lock)
{
    lock->lock = 0;
}

static inline void nb_spinlock_lock(nb_spinlock_t *lock)
{
    while (nb_atomic_test_and_set(&lock->lock)) {
        /* spin */
    }
}

static inline void nb_spinlock_unlock(nb_spinlock_t *lock)
{
    nb_atomic_clear(&lock->lock);
}

#endif /* NB_BASE_ATOMIC_H */
