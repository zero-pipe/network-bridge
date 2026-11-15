#include "zb/os/sync.h"
#include <stdlib.h>
#include <windows.h>

/*
 * Windows 后端说明：
 *   - 两种互斥量都基于 CRITICAL_SECTION（可重入）；
 *   - ZB_MUTEX_NORMAL 用 owner/depth 跟踪补齐 POSIX 语义：
 *     trylock 由持有线程再次调用返回 ZB_ERR_AGAIN（CRITICAL_SECTION 本身会成功）；
 *     NORMAL 互斥量"重入 lock"在 POSIX 会死锁、Windows 仍会成功，属已知差异，
 *     调用方不得依赖（见 zb/os/sync.h 注释）。
 */
struct zb_mutex {
    CRITICAL_SECTION cs;
    int recursive;
    DWORD owner;    /**< NORMAL：当前持有线程 id；0 = 未持有 */
    unsigned depth; /**< NORMAL：当前线程持有的深度 */
};

zb_mutex *zb_mutex_create(int flags)
{
    zb_mutex *m = (zb_mutex *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->recursive = (flags & ZB_MUTEX_RECURSIVE) ? 1 : 0;
    InitializeCriticalSection(&m->cs);
    return m;
}

void zb_mutex_destroy(zb_mutex *m)
{
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

zb_err_t zb_mutex_lock(zb_mutex *m)
{
    if (!m)
        return ZB_ERR_INVALID;
    EnterCriticalSection(&m->cs);
    if (!m->recursive) {
        if (m->depth == 0) {
            m->owner = GetCurrentThreadId();
        }
        ++m->depth;
    }
    return ZB_OK;
}

zb_err_t zb_mutex_unlock(zb_mutex *m)
{
    if (!m)
        return ZB_ERR_INVALID;
    if (!m->recursive && m->depth > 0) {
        --m->depth;
        if (m->depth == 0) {
            m->owner = 0;
        }
    }
    LeaveCriticalSection(&m->cs);
    return ZB_OK;
}

zb_err_t zb_mutex_trylock(zb_mutex *m)
{
    if (!m)
        return ZB_ERR_INVALID;
    if (!TryEnterCriticalSection(&m->cs))
        return ZB_ERR_AGAIN;
    /* CRITICAL_SECTION 允许持有线程重入；NORMAL 互斥量需按 POSIX 语义返回 AGAIN */
    if (!m->recursive && m->depth > 0 && m->owner == GetCurrentThreadId()) {
        LeaveCriticalSection(&m->cs);
        return ZB_ERR_AGAIN;
    }
    if (!m->recursive) {
        if (m->depth == 0) {
            m->owner = GetCurrentThreadId();
        }
        ++m->depth;
    }
    return ZB_OK;
}
